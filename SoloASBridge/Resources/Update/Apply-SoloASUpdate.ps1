# Copyright Cuviss Studio. All Rights Reserved.
# Protocol 1. This script must be executed from the verified NEW package.
[CmdletBinding()]
param([string]$RequestFile, [switch]$FunctionsOnly, [switch]$Worker, [string]$SecureRoot)
$Request=$null
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -eq 'Desktop') {
    $env:PSModulePath = [string]::Join(';', @(
        [IO.Path]::Combine([Environment]::GetFolderPath('MyDocuments'), 'WindowsPowerShell', 'Modules'),
        [IO.Path]::Combine([Environment]::GetFolderPath('ProgramFiles'), 'WindowsPowerShell', 'Modules'),
        [IO.Path]::Combine([Environment]::GetFolderPath('Windows'), 'System32', 'WindowsPowerShell', 'v1.0', 'Modules')
    ))
}
foreach ($SoloModule in @('Microsoft.PowerShell.Management', 'Microsoft.PowerShell.Utility', 'Microsoft.PowerShell.Security', 'Microsoft.PowerShell.Host', 'CimCmdlets')) {
    Import-Module ([IO.Path]::Combine($PSHOME, 'Modules', $SoloModule, ($SoloModule+'.psd1'))) -ErrorAction Stop
}

function Get-SoloFullPath([string]$Path) {
    if($Path.StartsWith('\\?\UNC\',[StringComparison]::OrdinalIgnoreCase)){$Path='\\'+$Path.Substring(8)}
    elseif($Path.StartsWith('\\?\')){$Path=$Path.Substring(4)}
    return [IO.Path]::GetFullPath($Path)
}
function Get-SoloExtendedPath([string]$Path) {
    $Full=Get-SoloFullPath $Path
    if($Full.StartsWith('\\')){return '\\?\UNC\'+$Full.Substring(2)}
    return '\\?\'+$Full
}
function Get-SoloFileHash([string]$Path) {
    $Stream=[IO.File]::OpenRead((Get-SoloExtendedPath $Path))
    $Hasher=[Security.Cryptography.SHA256]::Create()
    try {return [BitConverter]::ToString($Hasher.ComputeHash($Stream)).Replace('-','').ToLowerInvariant()}
    finally {$Hasher.Dispose();$Stream.Dispose()}
}

function Test-SoloRelativePath([string]$Path) {
    return $Path -cmatch '^[A-Za-z0-9_. /+@()-]+\z' -and -not $Path.StartsWith('/') -and
        -not @($Path.Split('/') | Where-Object { $_ -in @('', '.', '..') -or $_.EndsWith('.') -or $_.EndsWith(' ') }).Count
}

function Read-SoloManifest([string]$Root) {
    $ManifestFile=Join-Path $Root 'soloAS-manifest.json'
    if((Get-Item -LiteralPath (Get-SoloExtendedPath ($ManifestFile))).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Linked installation manifest is not supported.'}
    $Manifest = Get-Content -LiteralPath (Get-SoloExtendedPath ($ManifestFile)) -Raw | ConvertFrom-Json
    if ($Manifest.updaterProtocol -ne 1 -or $Manifest.packageKind -notin @('win64','source') -or
        $Manifest.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\z' -or -not @($Manifest.ueVersions).Count -or @($Manifest.ueVersions | Where-Object { $_ -notmatch '^5\.[0-9]+\.[0-9]+\z' }).Count) { throw 'Unsupported soloAS manifest or updater protocol.' }
    $Names = @{}; $Sources = @{}
    foreach ($Plugin in $Manifest.plugins) {
        if (-not (Test-SoloRelativePath $Plugin.path) -or -not (Test-SoloRelativePath $Plugin.installPath) -or
            $Plugin.descriptor -notmatch '^[A-Za-z][A-Za-z0-9_]*\.uplugin\z' -or
            $Plugin.path -cne $Plugin.installPath -or $Names.ContainsKey($Plugin.installPath) -or $Sources.ContainsKey($Plugin.path)) { throw 'Invalid or duplicate plugin directory.' }
        $Names[$Plugin.installPath] = $true; $Sources[$Plugin.path] = $true
    }
    foreach ($Paths in @($Names, $Sources)) {
        foreach ($A in $Paths.Keys) { foreach ($B in $Paths.Keys) {
            if ($A -ine $B -and $A.StartsWith($B + '/', [StringComparison]::OrdinalIgnoreCase)) { throw 'Overlapping plugin directories.' }
        } }
    }
    if (-not $Names.ContainsKey('SoloASBridge') -or -not $Sources.ContainsKey('SoloASBridge')) { throw 'Package has no SoloASBridge.' }
    $Files = @{}
    foreach ($File in $Manifest.files) {
        if (-not (Test-SoloRelativePath $File.path) -or $File.sha256 -notmatch '^[a-fA-F0-9]{64}\z' -or $Files.ContainsKey($File.path)) { throw "Unsafe manifest entry: $($File.path)" }
        $Files[$File.path] = [string]$File.sha256
    }
    foreach ($Plugin in $Manifest.plugins) {
        if (-not $Files.ContainsKey($Plugin.path + '/' + $Plugin.descriptor)) { throw "Missing descriptor: $($Plugin.path)" }
    }
    return $Manifest
}

function Read-SoloSymbols($Request, $Manifest) {
    $File=Join-Path $Request.installRoot 'soloAS-symbols-manifest.json'
    if(-not (Test-Path -LiteralPath (Get-SoloExtendedPath ($File)))){return @{present=$false;paths=@()}}
    if((Get-Item -LiteralPath (Get-SoloExtendedPath ($File))).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Linked symbols manifest is not supported.'}
    $Symbols=Get-Content -LiteralPath (Get-SoloExtendedPath ($File)) -Raw|ConvertFrom-Json
    if($Symbols.updaterProtocol -ne 1 -or $Symbols.packageKind -cne 'win64-symbols'){throw 'Invalid symbols manifest.'}
    $Paths=[Collections.Generic.List[string]]::new()
    foreach($Entry in $Symbols.files){
        if(-not (Test-SoloRelativePath $Entry.path) -or $Entry.sha256 -notmatch '^[a-fA-F0-9]{64}\z'){throw 'Invalid symbols manifest file path or hash.'}
        $Installed=Get-SoloInstalledPath $Entry.path $Manifest
        if(-not $Installed){throw 'Symbols manifest references a different plugin.'}
        if($Entry.path.EndsWith('.uplugin',[StringComparison]::OrdinalIgnoreCase)){
            $Descriptor=@($Manifest.plugins|Where-Object {($_.path+'/'+$_.descriptor) -ceq $Entry.path})
            $Baseline=@($Manifest.files|Where-Object {$_.path -ceq $Entry.path})
            if($Descriptor.Count -ne 1 -or $Baseline.Count -ne 1 -or $Entry.sha256 -ine $Baseline[0].sha256 -or
                (Get-SoloFileHash (Join-Path $Request.installRoot $Installed)) -ine $Entry.sha256){throw 'Symbols descriptor differs from the installed binary package.'}
            continue
        }
        if(-not $Entry.path.EndsWith('.pdb',[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid symbols manifest file path.'}
        $Paths.Add($Installed)
    }
    return @{present=$true;paths=@($Paths)}
}

function Get-SoloInstalledPath([string]$Path, $Manifest) {
    foreach ($Plugin in $Manifest.plugins) {
        if ($Path.StartsWith($Plugin.path + '/', [StringComparison]::Ordinal)) { return $Plugin.installPath + $Path.Substring($Plugin.path.Length) }
    }
    return $null
}

function Assert-SoloTree([string]$Root) {
    if ((Get-Item -LiteralPath (Get-SoloExtendedPath ($Root))).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked directory: $Root" }
    foreach ($Item in Get-ChildItem -LiteralPath (Get-SoloExtendedPath ($Root)) -Recurse -Force) {
        if ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Linked file or directory: $($Item.FullName)" }
    }
}

function Assert-SoloPackage([string]$Root, $Manifest) {
    Assert-SoloTree $Root
    $Declared = @{}
    foreach ($File in $Manifest.files) {
        $Full = Join-Path $Root $File.path
        if (-not (Test-Path -LiteralPath (Get-SoloExtendedPath ($Full)) -PathType Leaf) -or
            (Get-SoloFileHash $Full) -ine $File.sha256) { throw "Package hash mismatch: $($File.path)" }
        $Declared[$File.path] = $true
    }
    foreach ($Entry in $Manifest.plugins) {
        $Plugin = Join-Path $Root $Entry.path
        foreach ($File in Get-ChildItem -LiteralPath (Get-SoloExtendedPath ($Plugin)) -Recurse -Force -File) {
            $Relative = $Entry.path + '/' + (Get-SoloFullPath $File.FullName).Substring($Plugin.Length + 1).Replace('\','/')
            if (-not $Declared.ContainsKey($Relative)) { throw "Unlisted package file: $Relative" }
        }
    }
    if (-not $Declared.ContainsKey('SoloASBridge/Resources/Update/Apply-SoloASUpdate.ps1')) { throw 'Package has no updater.' }
}

function Invoke-SoloRetry([scriptblock]$Action) {
    for ($Attempt=0; $Attempt -lt 10; $Attempt++) {
        try { & $Action; return } catch { if ($Attempt -eq 9) { throw }; Start-Sleep -Seconds 1 }
    }
}

function Copy-SoloDirectory([string]$From, [string]$To) {
    New-Item -ItemType Directory -Path (Get-SoloExtendedPath ($To)) -Force | Out-Null
    foreach ($Item in Get-ChildItem -LiteralPath (Get-SoloExtendedPath ($From)) -Force) {
        Invoke-SoloRetry { Copy-Item -LiteralPath (Get-SoloExtendedPath ($Item.FullName)) -Destination (Get-SoloExtendedPath ($To)) -Recurse -Force }
    }
}

function Get-SoloPluginSearchRoots($Request) {
    $Roots = @()
    $Directory = Get-SoloFullPath $Request.installRoot
    while ($Directory) {
        if ((Split-Path $Directory -Leaf) -ieq 'Plugins') { $Roots += $Directory; break }
        $Directory = Split-Path $Directory -Parent
    }
    if (-not $Roots.Count) { throw 'The installation is not inside a Plugins directory.' }
    if ($Request.PSObject.Properties['project'] -and $Request.project) {
        $Roots += Join-Path (Split-Path $Request.project -Parent) 'Plugins'
    }
    $Roots += Join-Path (Split-Path (Split-Path (Split-Path $Request.editor -Parent) -Parent) -Parent) 'Plugins'
    return @($Roots | Select-Object -Unique)
}

function Get-SoloBackupRoot($Request) {
    $SearchRoot = @(Get-SoloPluginSearchRoots $Request)[0]
    $Root = Join-Path (Split-Path $SearchRoot -Parent) ('Saved/SoloASUpdates/Backups/' + $Request.token)
    # Linked ancestors could place backups on another volume or inside a search tree.
    foreach ($Path in @($Request.installRoot, $Root)) {
        $Directory = Get-SoloFullPath $Path
        while ($Directory) {
            if ((Test-Path -LiteralPath (Get-SoloExtendedPath $Directory)) -and
                ((Get-Item -LiteralPath (Get-SoloExtendedPath $Directory)).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Linked update directory: $Directory" }
            $Directory = Split-Path $Directory -Parent
        }
    }
    return $Root
}

function Get-SoloOldPath($Request, [string]$Name) {
    return Join-Path (Get-SoloBackupRoot $Request) ('old/' + $Name)
}

function Assert-SoloPluginDiscovery($Request, $Manifest) {
    $Descriptors = @{}
    foreach ($Root in Get-SoloPluginSearchRoots $Request) {
        if (-not (Test-Path -LiteralPath (Get-SoloExtendedPath $Root))) { continue }
        foreach ($File in Get-ChildItem -LiteralPath (Get-SoloExtendedPath $Root) -Recurse -Force -File -Filter '*.uplugin') {
            $Full = Get-SoloFullPath $File.FullName
            $Descriptors[$Full] = $File.Name
        }
    }
    foreach ($Plugin in $Manifest.plugins) {
        $Expected = Get-SoloFullPath (Join-Path $Request.installRoot ($Plugin.installPath + '/' + $Plugin.descriptor))
        $Found = @($Descriptors.Keys | Where-Object { $Descriptors[$_] -ieq $Plugin.descriptor })
        if ($Found.Count -ne 1 -or $Found[0] -ine $Expected) {
            throw ('Plugin discovery must resolve exactly one ' + $Plugin.descriptor + ': ' + ($Found -join ', '))
        }
    }
}

function Restore-SoloBackup($Request, $Names) {
    foreach ($Name in $Names) {
        $Target = Join-Path $Request.installRoot $Name
        $Old = Get-SoloOldPath $Request $Name
        $Failed = Join-Path (Get-SoloBackupRoot $Request) ('failed/' + $Name)
        New-Item -ItemType Directory -Path (Get-SoloExtendedPath (Split-Path $Failed -Parent)) -Force | Out-Null
        if (Test-Path -LiteralPath (Get-SoloExtendedPath ($Target))) { Invoke-SoloRetry { Move-Item -LiteralPath (Get-SoloExtendedPath ($Target)) -Destination (Get-SoloExtendedPath ($Failed)) } }
        if (Test-Path -LiteralPath (Get-SoloExtendedPath ($Old))) { Invoke-SoloRetry { Move-Item -LiteralPath (Get-SoloExtendedPath ($Old)) -Destination (Get-SoloExtendedPath ($Target)) } }
        elseif (Test-Path -LiteralPath (Get-SoloExtendedPath ((Join-Path (Get-SoloBackupRoot $Request) "copy/$Name")))) { Copy-SoloDirectory (Join-Path (Get-SoloBackupRoot $Request) "copy/$Name") $Target }
        if (Test-Path -LiteralPath (Get-SoloExtendedPath ($Failed))) { try { Invoke-SoloRetry { Remove-Item -LiteralPath (Get-SoloExtendedPath ($Failed)) -Recurse -Force } } catch { Write-Warning "Old version restored; cleanup required: $Failed" } }
    }
    Copy-Item -LiteralPath (Get-SoloExtendedPath ((Join-Path $Request.workRoot 'installed-manifest.json'))) -Destination (Get-SoloExtendedPath ($Request.installedManifestFile)) -Force
    $Symbols=Join-Path $Request.workRoot 'installed-symbols-manifest.json'
    if(Test-Path -LiteralPath (Get-SoloExtendedPath ($Symbols))){Copy-Item -LiteralPath (Get-SoloExtendedPath ($Symbols)) -Destination (Get-SoloExtendedPath ((Join-Path $Request.installRoot 'soloAS-symbols-manifest.json'))) -Force}
}

function Complete-SoloUpdate($Request, $Names) {
    # A cleanup failure must NEVER roll back a successfully loaded version.
    foreach ($Name in $Names) {
        $Old = Get-SoloOldPath $Request $Name
        if (Test-Path -LiteralPath (Get-SoloExtendedPath ($Old))) { try { Invoke-SoloRetry { Remove-Item -LiteralPath (Get-SoloExtendedPath ($Old)) -Recurse -Force } } catch { Write-Warning "Update succeeded; cleanup required: $Old" } }
    }
    $Backup = Get-SoloBackupRoot $Request
    try { Invoke-SoloRetry { Remove-Item -LiteralPath (Get-SoloExtendedPath ($Backup)) -Recurse -Force } } catch { Write-Warning "Update succeeded; backup cleanup required: $Backup" }
}

function Assert-SoloFilesAvailable([string]$Root) {
    foreach($File in Get-ChildItem -LiteralPath (Get-SoloExtendedPath ($Root)) -Recurse -Force -File) {
        Invoke-SoloRetry {
            $Handle=[IO.File]::Open((Get-SoloExtendedPath ($File.FullName)),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
            $Handle.Dispose()
        }
    }
}

function Install-SoloPackage($Request, $Manifest) {
    $Old = Read-SoloManifest $Request.installedManifestRoot
    $Symbols = Read-SoloSymbols $Request $Old
    if ([version]$Old.version -ge [version]$Manifest.version) { throw 'This release is already installed or older than the installed version.' }
    if ($Request.ueVersion -cnotin $Old.ueVersions -or $Request.ueVersion -cnotin $Manifest.ueVersions -or $Old.packageKind -cne $Manifest.packageKind) { throw 'UE version or package type changed.' }
    $OldHashes = @{}; foreach ($File in $Old.files) { $Key = Get-SoloInstalledPath $File.path $Old; if ($Key) { $OldHashes[$Key] = [string]$File.sha256 } }
    $NewHashes = @{}; foreach ($File in $Manifest.files) { $Key = Get-SoloInstalledPath $File.path $Manifest; if ($Key) { $NewHashes[$Key] = [string]$File.sha256 } }
    $Names = @(@($Old.plugins.installPath) + @($Manifest.plugins.installPath) | Select-Object -Unique)
    $Conflicts = [Collections.Generic.List[string]]::new()
    $Modified = [Collections.Generic.List[object]]::new()
    $BackupRoot = Join-Path (Get-SoloBackupRoot $Request) 'copy'
    if (Test-Path -LiteralPath (Get-SoloExtendedPath ($BackupRoot))) { throw 'An update backup already exists; resolve it before retrying.' }
    New-Item -ItemType Directory -Path (Get-SoloExtendedPath ($BackupRoot)) | Out-Null
    Copy-Item -LiteralPath (Get-SoloExtendedPath ($Request.installedManifestFile)) -Destination (Get-SoloExtendedPath ((Join-Path $Request.workRoot 'installed-manifest.json'))) -Force
    if($Symbols.present){Copy-Item -LiteralPath (Get-SoloExtendedPath ((Join-Path $Request.installRoot 'soloAS-symbols-manifest.json'))) -Destination (Get-SoloExtendedPath ((Join-Path $Request.workRoot 'installed-symbols-manifest.json'))) -Force}
    foreach ($Name in $Names) {
        $Target = Join-Path $Request.installRoot $Name
        if (-not (Test-Path -LiteralPath (Get-SoloExtendedPath ($Target)))) { continue }
        if (Test-Path -LiteralPath (Get-SoloExtendedPath ((Get-SoloOldPath $Request $Name)))) { throw 'An earlier update backup is still present.' }
        Assert-SoloTree $Target
        Assert-SoloFilesAvailable $Target
        Copy-SoloDirectory $Target (Join-Path $BackupRoot $Name)
        foreach ($File in Get-ChildItem -LiteralPath (Get-SoloExtendedPath ($Target)) -Recurse -Force -File) {
            $Local = (Get-SoloFullPath $File.FullName).Substring($Target.Length + 1).Replace('\','/')
            if ($Local.Split('/')[0] -in @('Binaries','Intermediate','.vs','DerivedDataCache','Saved')) { continue }
            $Relative = "$Name/" + $Local
            if($Relative -in $Symbols.paths){continue}
            $Hash = (Get-SoloFileHash $File.FullName)
            if (-not $OldHashes.ContainsKey($Relative) -or $Hash -ine $OldHashes[$Relative]) {
                $Modified.Add(@{path=$Relative; source=(Get-SoloFullPath $File.FullName).Substring($Request.installRoot.Length + 1)})
                if ($NewHashes.ContainsKey($Relative) -and (-not $OldHashes.ContainsKey($Relative) -or $NewHashes[$Relative] -ine $OldHashes[$Relative])) { $Conflicts.Add($Relative) }
            }
        }
    }
    $Touched = [Collections.Generic.List[string]]::new()
    try {
        foreach ($Name in $Names) {
            $Target = Join-Path $Request.installRoot $Name
            if (Test-Path -LiteralPath (Get-SoloExtendedPath ($Target))) {
                $OldPath = Get-SoloOldPath $Request $Name
                New-Item -ItemType Directory -Path (Get-SoloExtendedPath (Split-Path $OldPath -Parent)) -Force | Out-Null
                Invoke-SoloRetry { Move-Item -LiteralPath (Get-SoloExtendedPath ($Target)) -Destination (Get-SoloExtendedPath ($OldPath)) }
            }
            $Touched.Add($Name)
            $Entry = @($Manifest.plugins | Where-Object { $_.installPath -ceq $Name })
            if ($Entry.Count) { Copy-SoloDirectory (Join-Path $Request.packageRoot $Entry[0].path) $Target }
        }
        foreach ($File in $Modified) {
            $Target = Join-Path $Request.installRoot $File.source
            New-Item -ItemType Directory -Path (Get-SoloExtendedPath ((Split-Path $Target -Parent))) -Force | Out-Null
            Invoke-SoloRetry { Copy-Item -LiteralPath (Get-SoloExtendedPath ((Join-Path $BackupRoot $File.source))) -Destination (Get-SoloExtendedPath ($Target)) -Force }
        }
        Copy-Item -LiteralPath (Get-SoloExtendedPath ((Join-Path $Request.packageRoot 'soloAS-manifest.json'))) -Destination (Get-SoloExtendedPath ($Request.installedManifestFile)) -Force
        [IO.File]::WriteAllLines((Get-SoloExtendedPath ((Join-Path $Request.workRoot 'conflicts.txt'))), $Conflicts)
        if($Symbols.present){
            foreach($Path in $Symbols.paths){
                $Target=Join-Path $Request.installRoot $Path
                if(-not $NewHashes.ContainsKey($Path) -and (Test-Path -LiteralPath (Get-SoloExtendedPath ($Target)))){Invoke-SoloRetry {Remove-Item -LiteralPath (Get-SoloExtendedPath ($Target)) -Force}}
            }
            Remove-Item -LiteralPath (Get-SoloExtendedPath ((Join-Path $Request.installRoot 'soloAS-symbols-manifest.json'))) -Force
            [IO.File]::WriteAllText((Get-SoloExtendedPath ((Join-Path $Request.workRoot 'symbols-removed.txt'))),'symbols removed')
        }
        Assert-SoloPluginDiscovery $Request $Manifest
        return @{names=$Names; conflicts=@($Conflicts);symbolsRemoved=$Symbols.present}
    } catch {
        $Failure=$_
        try { Restore-SoloBackup $Request $Touched } catch { $Failure.Exception.Data['RollbackFailed']=$true }
        throw $Failure
    }
}

function Start-SoloEditor($Request, [switch]$Updated) {
    if(-not $Updated){
        $Original=Get-Process -Id $Request.editorPid -ErrorAction SilentlyContinue
        if($Original -and $Original.StartTime.ToUniversalTime().Ticks -eq $Request.editorStartTicks){return $Original}
    }
    $Arguments = '"' + $Request.project.Replace('"','') + '"'
    if ($Updated) {
        Assert-SoloPluginDiscovery $Request (Read-SoloManifest $Request.installedManifestRoot)
        foreach ($Name in @('loaded.txt','loaded-version.json')) {
            $Receipt = Join-Path $Request.workRoot $Name
            if (Test-Path -LiteralPath (Get-SoloExtendedPath $Receipt)) { Remove-Item -LiteralPath (Get-SoloExtendedPath $Receipt) -Force }
        }
        $Arguments += ' -SoloASUpdateReceipt="' + (Join-Path $Request.workRoot 'loaded.txt') + '" -SoloASUpdateToken=' + $Request.token }
    return Start-Process -FilePath $Request.editor -ArgumentList $Arguments -PassThru
}

function Stop-SoloEditorTree($Child) {
    if ($Child -and -not $Child.HasExited) {
        & (Join-Path ([Environment]::GetFolderPath('Windows')) 'System32/taskkill.exe') /PID $Child.Id /T /F | Out-Null
        if ($LASTEXITCODE -ne 0 -and -not $Child.HasExited) { throw 'Could not stop the new editor process tree. Backups have been retained.' }
        $Child.WaitForExit()
    }
}

function Wait-SoloEditorExit($Request) {
    $Editor = Get-Process -Id $Request.editorPid -ErrorAction SilentlyContinue
    if ($Editor -and $Editor.StartTime.ToUniversalTime().Ticks -eq $Request.editorStartTicks) {
        $Timeout=3600000
        if($Request.PSObject.Properties['deadlineTicks']){$Timeout=[int][Math]::Max(0,[Math]::Min($Timeout,([DateTime]::new([long]$Request.deadlineTicks)-[DateTime]::UtcNow).TotalMilliseconds))}
        if (-not $Editor.WaitForExit($Timeout)) { throw 'The original editor is still open. Nothing was replaced.' }
    }
}

function Assert-SoloOtherEditorsClosed($Request) {
    $EngineRoot = Split-Path (Split-Path (Split-Path $Request.editor -Parent) -Parent) -Parent
    $Deadline = (Get-SoloUtcNow).AddSeconds(30)
    while ($true) {
        $Others = @(Get-CimInstance Win32_Process | Where-Object {
            $_.ExecutablePath -and $_.ExecutablePath.StartsWith($EngineRoot + '\', [StringComparison]::OrdinalIgnoreCase) -and
            ($_.Name -match '^(UnrealEditor(-Cmd)?|ShaderCompileWorker|CrashReportClient(Editor)?)\.exe\z' -or $_.ParentProcessId -eq $Request.editorPid)
        })
        if (-not $Others.Count) { return }
        $Blocking = @($Others | Where-Object {
            $_.Name -match '^UnrealEditor(-Cmd)?\.exe\z' -or
            ($_.Name -notmatch '^ShaderCompileWorker\.exe\z' -and $_.ParentProcessId -ne $Request.editorPid -and
             -not ($_.Name -match '^CrashReportClient(Editor)?\.exe\z' -and $_.CommandLine -match ('(?i)(?:^|\s)["'']?-?MONITOR=["'']?' + [regex]::Escape([string]$Request.editorPid) + '["'']?(?=\s|$)')))
        })
        if ($Blocking.Count -or (Get-SoloUtcNow) -ge $Deadline) {
            $Names = @($Others | ForEach-Object { '{0} (PID {1})' -f $_.Name, $_.ProcessId }) -join ', '
            throw ('Close processes using this engine, then retry the update: ' + $Names)
        }
        Start-Sleep -Milliseconds 500
    }
}

function Get-SoloUtcNow {return [DateTime]::UtcNow}

function Confirm-SoloLoadedVersion($Request, $Child, $Manifest) {
    $Bridge = @($Manifest.plugins | Where-Object { $_.descriptor -ceq 'SoloASBridge.uplugin' })[0]
    $Expected = Get-SoloFullPath (Join-Path $Request.installRoot ($Bridge.installPath + '/Binaries/Win64/UnrealEditor-SoloASBridge.dll'))
    $Modules = @($Child.Modules | Where-Object { $_.ModuleName -ieq 'UnrealEditor-SoloASBridge.dll' })
    if ($Modules.Count -ne 1 -or (Get-SoloFullPath $Modules[0].FileName) -ine $Expected) {
        throw 'The initialization receipt came from an editor that did not load the updated SoloASBridge location.'
    }
    $Hash = Get-SoloFileHash $Expected
    $Declared = @($Manifest.files | Where-Object { $_.path -ceq ($Bridge.path + '/Binaries/Win64/UnrealEditor-SoloASBridge.dll') })
    if (($Manifest.packageKind -ceq 'win64' -and $Declared.Count -ne 1) -or
        ($Declared.Count -and $Hash -ine $Declared[0].sha256)) { throw 'The loaded SoloASBridge binary differs from the new release.' }
    $Receipt = @{version=$Manifest.version;manifestSha256=(Get-SoloFileHash (Join-Path $Request.packageRoot 'soloAS-manifest.json'));moduleSha256=$Hash;modulePath=$Expected;editorPid=$Child.Id;token=$Request.token}
    [IO.File]::WriteAllText((Get-SoloExtendedPath (Join-Path $Request.workRoot 'loaded-version.json')), ($Receipt | ConvertTo-Json))
}

function Wait-SoloLoaded($Request, $Child, [string]$ProtectedRoot='') {
    $Receipt = Join-Path $Request.workRoot 'loaded.txt'
    $Manifest = Read-SoloManifest $Request.packageRoot
    $Minutes = if ($Manifest.packageKind -eq 'source') {45} else {15}
    $Started=Get-SoloUtcNow
    $Deadline=$Started.AddMinutes(90)
    $PromptAt=$Started.AddMinutes($Minutes)
    while ($true) {
        if($ProtectedRoot -and (Read-SoloWorkerStatus $ProtectedRoot).status -in @('failed','rollback-failed','timeout')){throw 'The protected update did not complete; rollback has been requested.'}
        if ($Child.HasExited) { throw 'The updated editor exited before successful initialization.' }
        if ((Test-Path -LiteralPath (Get-SoloExtendedPath ($Receipt))) -and [IO.File]::ReadAllText((Get-SoloExtendedPath ($Receipt))) -ceq $Request.token) {
            Confirm-SoloLoadedVersion $Request $Child $Manifest
            return
        }
        $Now=Get-SoloUtcNow
        if($Now -ge $Deadline){throw 'The updated editor did not finish initialization within 90 minutes.'}
        if ($Now -gt $PromptAt) {
            Write-Warning 'The editor is still running. Waiting for compilation, dialogs and the initialization receipt.'
            $PromptAt = $Now.AddMinutes(10)
        }
        Start-Sleep -Milliseconds 500
    }
}

function Open-SoloUpdateLock([string]$InstallRoot) {
    try {
        return [IO.File]::Open((Get-SoloExtendedPath (Join-Path $InstallRoot '.soloas-update.lock')), [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    } catch [IO.IOException] {
        if (($_.Exception.HResult -band 0xffff) -in @(32,33)) {
            throw 'Another soloAS update is in progress. Wait for it to complete or roll back before retrying.'
        }
        throw
    }
}

function Invoke-SoloUpdate($Request) {
    $Manifest = Read-SoloManifest $Request.packageRoot
    Assert-SoloPackage $Request.packageRoot $Manifest
    if ($Manifest.version -cne $Request.version -or $Request.ueVersion -cnotin $Manifest.ueVersions) { throw 'Downloaded release identity differs.' }
    $Lock = Open-SoloUpdateLock $Request.installRoot
    $Child = $null; $Installed = $null
    try {
        Wait-SoloEditorExit $Request
        Assert-SoloOtherEditorsClosed $Request
        $Installed = Install-SoloPackage $Request $Manifest
        $Child = Start-SoloEditor $Request -Updated
        Wait-SoloLoaded $Request $Child
    } catch {
        Stop-SoloEditorTree $Child
        if ($Installed) { Restore-SoloBackup $Request $Installed.names }
        if (-not $_.Exception.Data.Contains('RollbackFailed')) { $null = Start-SoloEditor $Request }
        throw
    } finally { $Lock.Dispose() }
    Complete-SoloUpdate $Request $Installed.names
    return $Installed
}

function Read-SoloSecureState([string]$Root) {
    $Base = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'soloAS/update'
    $Full = (Get-SoloFullPath $Root)
    if ((Split-Path $Full -Parent) -ine (Get-SoloFullPath $Base) -or (Split-Path $Full -Leaf) -notmatch '^[a-f0-9]{32}\z') { throw 'Invalid protected update directory.' }
    Assert-SoloTree $Full
    $Write = [Security.AccessControl.FileSystemRights]::WriteData -bor [Security.AccessControl.FileSystemRights]::AppendData -bor [Security.AccessControl.FileSystemRights]::WriteAttributes -bor [Security.AccessControl.FileSystemRights]::WriteExtendedAttributes -bor [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor [Security.AccessControl.FileSystemRights]::Delete -bor [Security.AccessControl.FileSystemRights]::ChangePermissions -bor [Security.AccessControl.FileSystemRights]::TakeOwnership
    $Acl = Get-Acl -LiteralPath (Get-SoloExtendedPath ($Full))
    if ($Acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin @('S-1-5-32-544','S-1-5-18')) { throw 'Protected update directory has an untrusted owner.' }
    foreach ($Rule in $Acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])) {
        if ($Rule.AccessControlType -eq 'Allow' -and ($Rule.FileSystemRights -band $Write) -and $Rule.IdentityReference.Value -notin @('S-1-5-32-544','S-1-5-18')) { throw 'Protected update directory is writable by a non-administrator.' }
    }
    $State = Get-Content -LiteralPath (Get-SoloExtendedPath ((Join-Path $Full 'state.json'))) -Raw | ConvertFrom-Json
    if (-not (Get-SoloFullPath $State.packageRoot).StartsWith($Full + '\',[StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath (Get-SoloExtendedPath ((Join-Path $State.installRoot 'SoloASBridge/SoloASBridge.uplugin')))) -or
        (Get-SoloFullPath $State.workRoot) -ine $Full) { throw 'Protected update identity is invalid.' }
    if (-not (Get-SoloFullPath $PSCommandPath).StartsWith($Full + '\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Privileged updater must execute from the protected package.' }
    return $State
}

function Read-SoloWorkerStatus([string]$Root) {
    try {return Get-Content -LiteralPath (Get-SoloExtendedPath ((Join-Path $Root 'status.json'))) -Raw | ConvertFrom-Json}
    catch {return @{status='pending';error=''}}
}
function Write-SoloWorkerStatus([string]$Root, [string]$Status, [string]$ErrorText = '') {
    $File=Join-Path $Root 'status.json';$Temp=Join-Path $Root 'status.tmp'
    [IO.File]::WriteAllText((Get-SoloExtendedPath ($Temp)),(@{status=$Status;error=$ErrorText}|ConvertTo-Json))
    Invoke-SoloRetry {if(Test-Path -LiteralPath (Get-SoloExtendedPath ($File))){[IO.File]::Replace((Get-SoloExtendedPath ($Temp)),(Get-SoloExtendedPath ($File)),[NullString]::Value)}else{[IO.File]::Move((Get-SoloExtendedPath ($Temp)),(Get-SoloExtendedPath ($File)))}}
}

function Read-SoloDecision([string]$Root) {
    $File=Join-Path $Root 'decision.txt'
    $Item=Get-Item -LiteralPath (Get-SoloExtendedPath ($File))
    if(($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $Item.Length -gt 64){throw 'Invalid update completion receipt.'}
    $Value=[IO.File]::ReadAllText((Get-SoloExtendedPath ($File)))
    if($Value -cnotin @('','loaded','failed') -and $Value -cnotmatch '^running:([1-9][0-9]{0,9}):([1-9][0-9]{0,18})\z'){throw 'Invalid update completion receipt.'}
    return $Value
}

function Invoke-SoloFileWorker([string]$Root) {
    $State = Read-SoloSecureState $Root
    $Manifest = Read-SoloManifest $State.packageRoot
    Assert-SoloPackage $State.packageRoot $Manifest
    try { $Lock = Open-SoloUpdateLock $State.installRoot }
    catch { Write-SoloWorkerStatus $Root 'failed' $_.Exception.Message; throw }
    $Installed=$null;$UpdatedEditor=$null
    try {
        Write-SoloWorkerStatus $Root 'staged'
        Wait-SoloEditorExit $State
        Assert-SoloOtherEditorsClosed $State
        $Installed = Install-SoloPackage $State $Manifest
        Write-SoloWorkerStatus $Root 'installed'
        $Deadline = [DateTime]::new([long]$State.deadlineTicks)
        $EditorIdentity=''
        while ([DateTime]::UtcNow -lt $Deadline) {
            $Decision = Read-SoloDecision $Root
            if ($Decision -ceq 'loaded') { Complete-SoloUpdate $State $Installed.names; Write-SoloWorkerStatus $Root 'complete'; return }
            if ($Decision -ceq 'failed') { throw 'The new editor failed to initialize.' }
            if ($Decision -clike 'running:*') {
                if (-not $EditorIdentity) {
                    $IdentityParts=$Decision.Split(':')
                    $UpdatedEditor=Get-Process -Id ([int]$IdentityParts[1]) -ErrorAction SilentlyContinue
                    if (-not $UpdatedEditor -or $UpdatedEditor.HasExited -or
                        $UpdatedEditor.StartTime.ToUniversalTime().Ticks -ne [long]$IdentityParts[2]) {
                        throw 'The updated editor exited before successful initialization. Rolling back the update.'
                    }
                    $EditorIdentity=$Decision
                } elseif ($Decision -cne $EditorIdentity) { throw 'The updated editor identity changed.' }
                if ($UpdatedEditor.HasExited) { throw 'The updated editor exited before successful initialization. Rolling back the update.' }
            } elseif ($Decision -cne '') { throw 'Invalid update completion receipt.' }
            Start-Sleep -Milliseconds 500
        }
        Write-SoloWorkerStatus $Root 'timeout'
        $StopDeadline=[DateTime]::UtcNow.AddSeconds(60)
        while([DateTime]::UtcNow -lt $StopDeadline -and (Read-SoloDecision $Root) -ne 'failed'){Start-Sleep -Milliseconds 250}
        if((Read-SoloDecision $Root) -ne 'failed'){
            $Failure=[InvalidOperationException]::new('The monitor did not stop the editor. Backups were retained for manual recovery.')
            $Failure.Data['RollbackFailed']=$true;throw $Failure
        }
        throw 'No completion receipt was received within the update deadline.'
    } catch {
        $Failure=$_
        if($Failure.Exception.Data.Contains('RollbackFailed')){Write-SoloWorkerStatus $Root 'rollback-failed' $Failure.Exception.Message;throw $Failure}
        if ($Installed) {
            try {Restore-SoloBackup $State $Installed.names}
            catch {Write-SoloWorkerStatus $Root 'rollback-failed' ('Recovery requires the retained backup: '+$_.Exception.Message);throw}
        }
        Write-SoloWorkerStatus $Root 'failed' $Failure.Exception.Message
        throw $Failure
    } finally {
        $Lock.Dispose()
        if($UpdatedEditor){$UpdatedEditor.Dispose()}
        try { Remove-Item -LiteralPath (Get-SoloExtendedPath ((Join-Path $Root 'extracted'))) -Recurse -Force;Remove-Item -LiteralPath (Get-SoloExtendedPath ((Join-Path $Root 'package.archive'))) -Force } catch { Write-Warning 'Protected extraction cleanup failed.' }
    }
}

function Invoke-SoloProtectedMonitor($Request, [string]$Root) {
    $StatusFile=Join-Path $Root 'status.json';$Child=$null;$Installed=$false
    $State=Get-Content -LiteralPath (Get-SoloExtendedPath ((Join-Path $Root 'state.json'))) -Raw|ConvertFrom-Json
    $Deadline=[DateTime]::new([long]$State.deadlineTicks).AddMinutes(2)
    try {
        while ([DateTime]::UtcNow -lt $Deadline) {
            $Status=Read-SoloWorkerStatus $Root
            if ($Status.status -in @('failed','rollback-failed')) {throw $Status.error}
            if ($Status.status -eq 'installed') {break}
            Start-Sleep -Milliseconds 500
        }
        if($Status.status -ne 'installed'){throw 'The protected update did not become ready within its deadline.'}
        $Installed=$true
        $Child=Start-SoloEditor $Request -Updated
        [IO.File]::WriteAllText((Get-SoloExtendedPath (Join-Path $Root 'decision.txt')), ('running:{0}:{1}' -f $Child.Id,$Child.StartTime.ToUniversalTime().Ticks))
        Wait-SoloLoaded $Request $Child $Root
        [IO.File]::WriteAllText((Get-SoloExtendedPath ((Join-Path $Root 'decision.txt'))),'loaded')
        while ((Read-SoloWorkerStatus $Root).status -ne 'complete' -and [DateTime]::UtcNow -lt $Deadline) {
            if((Read-SoloWorkerStatus $Root).status -in @('failed','rollback-failed')){throw 'The privileged updater could not finalize.'}
            Start-Sleep -Milliseconds 500
        }
        if((Read-SoloWorkerStatus $Root).status -ne 'complete'){throw 'The protected update did not finalize within its deadline.'}
        return @{conflicts=@(Get-Content -LiteralPath (Get-SoloExtendedPath ((Join-Path $Root 'conflicts.txt'))));symbolsRemoved=(Test-Path -LiteralPath (Get-SoloExtendedPath ((Join-Path $Root 'symbols-removed.txt'))))}
    } catch {
        Stop-SoloEditorTree $Child
        [IO.File]::WriteAllText((Get-SoloExtendedPath ((Join-Path $Root 'decision.txt'))),'failed')
        if ($Installed) {while ((Read-SoloWorkerStatus $Root).status -notin @('failed','rollback-failed') -and [DateTime]::UtcNow -lt $Deadline) {Start-Sleep -Milliseconds 500}}
        if((Read-SoloWorkerStatus $Root).status -eq 'failed'){$null=Start-SoloEditor $Request}
        throw
    }
}

if (-not $FunctionsOnly) {
    $Transcribing=$false
    if(-not $Worker -and $RequestFile){Start-Transcript -LiteralPath (Get-SoloExtendedPath ((Join-Path (Split-Path $RequestFile -Parent) 'update.log')))|Out-Null;$Transcribing=$true}
    try {
        if ($Worker) { Invoke-SoloFileWorker $SecureRoot; exit 0 }
        $Request = Get-Content -LiteralPath (Get-SoloExtendedPath ($RequestFile)) -Raw | ConvertFrom-Json
        if ($SecureRoot) {$Result=Invoke-SoloProtectedMonitor $Request $SecureRoot}
        else {$Result=Invoke-SoloUpdate $Request}
        Add-Type -AssemblyName PresentationFramework
        $Message='soloAS updated. User file conflicts (your versions were preserved): ' + ($Result.conflicts -join ', ')
        if($Result.symbolsRemoved){$Message+=' Download matching debug symbols again for this version.'}
        [Windows.MessageBox]::Show($Message, 'soloAS update') | Out-Null
    } catch {
        if ($RequestFile) { $_ | Out-String | Set-Content -LiteralPath (Get-SoloExtendedPath ((Join-Path (Split-Path $RequestFile -Parent) 'update-error.txt'))) }
        if (-not $Worker) {
            Add-Type -AssemblyName PresentationFramework
            [Windows.MessageBox]::Show(('soloAS update failed. See the retained backup and update log if manual recovery is needed. ' + $_.Exception.Message), 'soloAS update') | Out-Null
        }
        exit 1
    } finally {
        if($Transcribing){Stop-Transcript|Out-Null}
        if(-not $Worker -and $RequestFile){
            $DownloadRoot=Split-Path $RequestFile -Parent
            try {Remove-Item -LiteralPath (Get-SoloExtendedPath ((Join-Path $DownloadRoot 'extracted'))) -Recurse -Force -ErrorAction Stop}catch{}
            if($Request -and $Request.PSObject.Properties['assetName']){
                $Name=[IO.Path]::GetFileName([string]$Request.assetName)
                if($Name -match '^soloAS-[0-9.]+(-ue[0-9.]+)?-(win64\.(7z|zip)|source\.zip)\z'){try {Remove-Item -LiteralPath (Get-SoloExtendedPath ((Join-Path $DownloadRoot $Name))) -Force -ErrorAction Stop}catch{}}
            }
        }
    }
}
