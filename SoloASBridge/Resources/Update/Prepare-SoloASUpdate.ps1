# SPDX-License-Identifier: MPL-2.0
# Copyright Cuviss Studio. All Rights Reserved.
# Protocol 1 trusts the official GitHub Release over HTTPS; SHA256 detects corruption.
[CmdletBinding()]
param([string]$RequestFile, [switch]$FunctionsOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if ($PSVersionTable.PSEdition -eq 'Desktop') {
    $env:PSModulePath = [string]::Join(';', @(
        [IO.Path]::Combine([Environment]::GetFolderPath('MyDocuments'), 'WindowsPowerShell', 'Modules'),
        [IO.Path]::Combine([Environment]::GetFolderPath('ProgramFiles'), 'WindowsPowerShell', 'Modules'),
        [IO.Path]::Combine([Environment]::GetFolderPath('Windows'), 'System32', 'WindowsPowerShell', 'v1.0', 'Modules')
    ))
}
foreach ($SoloModule in @('Microsoft.PowerShell.Management', 'Microsoft.PowerShell.Utility', 'Microsoft.PowerShell.Security', 'Microsoft.PowerShell.Host')) {
    Import-Module ([IO.Path]::Combine($PSHOME, 'Modules', $SoloModule, ($SoloModule+'.psd1'))) -ErrorAction Stop
}
$ProgressPreference='SilentlyContinue'
$script:KeepVerifiedArchive=$false
$ManualUrl='https://soloas.cuviss.com/docs/en/requirements.html#updating-soloas'

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

function Get-SoloPowerShell {
    return Join-Path ([Environment]::GetFolderPath('Windows')) 'System32/WindowsPowerShell/v1.0/powershell.exe'
}
function Get-SoloInstalledRoot {
    return (Get-SoloFullPath (Join-Path $PSScriptRoot '../../..')).TrimEnd('\','/')
}
function Get-SoloChecksum([string]$Content,[string]$Name) {
    $Lines=@($Content -split "`r?`n" | Where-Object {$_ -match ('^[a-fA-F0-9]{64} [ *]'+[regex]::Escape($Name)+'\z')})
    if($Lines.Count -ne 1){throw 'Release checksum is missing or ambiguous.'}
    return $Lines[0].Substring(0,64)
}
function Test-SoloChecksum([string]$Archive,[string]$Checksums,[string]$Name) {
    $Expected=Get-SoloChecksum ([IO.File]::ReadAllText((Get-SoloExtendedPath ($Checksums)))) $Name
    if((Get-SoloFileHash $Archive) -ine $Expected){throw 'SHA256 verification failed. Nothing was installed.'}
}
function Assert-SoloDownloadUri([uri]$Uri) {
    if($Uri.Scheme -cne 'https' -or $Uri.Port -ne 443 -or $Uri.UserInfo -or $Uri.Host -notin @('github.com','objects.githubusercontent.com','release-assets.githubusercontent.com')){throw 'The release redirected to an untrusted download host.'}
}
function Download-SoloAsset([string]$Url,[string]$OutFile,[string]$CancelFile,[switch]$Capture) {
    $Uri=[uri]$Url
    for($Redirect=0;$Redirect -lt 6;$Redirect++) {
        Assert-SoloDownloadUri $Uri
        if(Test-Path -LiteralPath (Get-SoloExtendedPath ($CancelFile))){throw [OperationCanceledException]::new('Update download cancelled.')}
        $Web=[Net.HttpWebRequest]::Create($Uri);$Web.AllowAutoRedirect=$false;$Web.UserAgent='soloAS-updater';$Web.Timeout=15000;$Web.ReadWriteTimeout=15000
        $Response=$null;$Stream=$null;$File=$null;$Memory=[IO.MemoryStream]::new();$Hasher=[Security.Cryptography.SHA256]::Create()
        try {
            try {$Response=$Web.GetResponse()} catch [Net.WebException] {
                if($_.Exception.Response -and [int]$_.Exception.Response.StatusCode -in @(403,429)){throw 'GitHub has rate limited this download. Please try again later.'}
                throw
            }
            $Code=[int]$Response.StatusCode
            if($Code -in @(301,302,303,307,308)){$Uri=[uri]::new($Uri,[string]$Response.Headers['Location']);continue}
            if($Code -ne 200){throw "Download failed (HTTP $Code)."}
            $Stream=$Response.GetResponseStream();$File=[IO.File]::Create((Get-SoloExtendedPath ($OutFile)));$Buffer=New-Object byte[] 65536
            while(($Count=$Stream.Read($Buffer,0,$Buffer.Length)) -gt 0) {
                if(Test-Path -LiteralPath (Get-SoloExtendedPath ($CancelFile))){$Web.Abort();throw [OperationCanceledException]::new('Update download cancelled.')}
                $File.Write($Buffer,0,$Count)
                $null=$Hasher.TransformBlock($Buffer,0,$Count,$Buffer,0)
                if($Capture){if($Memory.Length+$Count -gt 1048576){throw 'Release checksum list is too large.'};$Memory.Write($Buffer,0,$Count)}
            }
            $null=$Hasher.TransformFinalBlock([byte[]]@(),0,0)
            return @{sha256=([BitConverter]::ToString($Hasher.Hash).Replace('-','').ToLowerInvariant());content=[Text.Encoding]::UTF8.GetString($Memory.ToArray())}
        } finally {$Hasher.Dispose();$Memory.Dispose();if($File){$File.Dispose()};if($Stream){$Stream.Dispose()};if($Response){$Response.Dispose()}}
    }
    throw 'The download exceeded the redirect limit.'
}
function Expand-SoloArchive([string]$Archive,[string]$Extract) {
    New-Item -ItemType Directory -Path (Get-SoloExtendedPath ($Extract)) | Out-Null
    $Tar=Join-Path ([Environment]::GetFolderPath('Windows')) 'System32/tar.exe'
    $Entries=@(& $Tar -tf (Get-SoloExtendedPath $Archive))
    if($LASTEXITCODE -ne 0){throw 'Windows tar could not read this package. Install the verified package manually.'}
    foreach($Entry in $Entries){if($Entry -match '(^[/\\]|:|\\|(^|/)\.\.(/|\z))'){throw 'Unsafe archive path.'}}
    $Details=@(& $Tar -tvf (Get-SoloExtendedPath $Archive))
    if($LASTEXITCODE -ne 0 -or @($Details|Where-Object {$_ -match '^[lh]'}).Count){throw 'Linked archive entries are not supported.'}
    & $Tar -xf (Get-SoloExtendedPath $Archive) -C (Get-SoloExtendedPath $Extract)
    if($LASTEXITCODE -ne 0){throw 'Package extraction failed.'}
    $Manifests=@(Get-ChildItem -LiteralPath (Get-SoloExtendedPath ($Extract)) -Filter 'soloAS-manifest.json' -Recurse -File)
    if($Manifests.Count -ne 1){throw 'Package must contain exactly one soloAS-manifest.json.'}
    return Get-SoloFullPath $Manifests[0].DirectoryName
}
function Stage-SoloVerifiedPackage([string]$Archive,[string]$Root,[string]$Expected,[string]$ApplyHash,[string]$ManifestHash) {
    $Sealed=Join-Path $Root 'package.archive';Copy-Item -LiteralPath (Get-SoloExtendedPath ($Archive)) -Destination (Get-SoloExtendedPath ($Sealed))
    if((Get-SoloFileHash $Sealed) -ine $Expected){throw 'Copied package checksum mismatch.'}
    $Package=Expand-SoloArchive $Sealed (Join-Path $Root 'extracted')
    $Script=Join-Path $Package 'SoloASBridge/Resources/Update/Apply-SoloASUpdate.ps1'
    if((Get-SoloFileHash $Script) -ine $ApplyHash -or
        (Get-SoloFileHash (Join-Path $Package 'soloAS-manifest.json')) -ine $ManifestHash){throw 'Sealed updater or manifest checksum mismatch.'}
    return $Package
}
function Confirm-SoloRestart {
    Add-Type -AssemblyName PresentationFramework
    return [Windows.MessageBox]::Show('The download is verified. Save your work, then close the editor to complete the update. The editor will reopen automatically. Continue?','soloAS update','YesNo') -eq 'Yes'
}
function Show-SoloManualUpdate([string]$Archive) {
    Add-Type -AssemblyName PresentationFramework
    [Windows.MessageBox]::Show(('Permission was not granted. Install the verified package manually: '+$Archive+"`nInstructions: "+$ManualUrl),'soloAS update')|Out-Null
}
function Start-SoloApply([string]$Script,[string]$RequestFile,[bool]$Elevate=$false,[string]$SecureRoot='') {
    if($Elevate){throw 'Privileged code must use the sealed bootstrap, not a script from Saved.'}
    $Parameters='-NoProfile -ExecutionPolicy Bypass -File "'+(Get-SoloExtendedPath $Script)+'" -RequestFile "'+(Get-SoloExtendedPath $RequestFile)+'"'
    if($SecureRoot){$Parameters+=' -SecureRoot "'+$SecureRoot+'"'}
    Start-Process -FilePath (Get-SoloPowerShell) -ArgumentList $Parameters -WindowStyle Hidden | Out-Null
}

# This fixed entry comes from the installed bootstrap, never from the downloaded
# package or Saved/request.json. Only literal command-line arguments are appended.
$SoloSecureBootstrap={
    param([string]$InstallRoot,[string]$Archive,[string]$ArchiveHash,[string]$ApplyHash,[string]$ManifestHash,[string]$ReadSid,[string]$Id,[int]$EditorPid,[long]$EditorTicks,[string]$Editor,[string]$Project)
    $ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
    if ($PSVersionTable.PSEdition -eq 'Desktop') {
        $env:PSModulePath = [string]::Join(';', @(
            [IO.Path]::Combine([Environment]::GetFolderPath('MyDocuments'), 'WindowsPowerShell', 'Modules'),
            [IO.Path]::Combine([Environment]::GetFolderPath('ProgramFiles'), 'WindowsPowerShell', 'Modules'),
            [IO.Path]::Combine([Environment]::GetFolderPath('Windows'), 'System32', 'WindowsPowerShell', 'v1.0', 'Modules')
        ))
    }
    foreach ($SoloModule in @('Microsoft.PowerShell.Management', 'Microsoft.PowerShell.Utility', 'Microsoft.PowerShell.Security', 'Microsoft.PowerShell.Host')) {
        Import-Module ([IO.Path]::Combine($PSHOME, 'Modules', $SoloModule, ($SoloModule+'.psd1'))) -ErrorAction Stop
    }
    if($Id -notmatch '^[a-f0-9]{32}\z' -or $ReadSid -notmatch '^S-1-[0-9-]+\z' -or @($ArchiveHash,$ApplyHash,$ManifestHash|Where-Object {$_ -notmatch '^[a-fA-F0-9]{64}\z'}).Count){throw 'Invalid privileged bootstrap arguments.'}
    $Engine=Split-Path (Split-Path (Split-Path $Editor -Parent) -Parent) -Parent
    $InstallRoot=(Get-SoloFullPath $InstallRoot).TrimEnd('\','/')
    if(-not $InstallRoot.StartsWith($Engine+'\Plugins\',[StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath (Get-SoloExtendedPath ((Join-Path $InstallRoot 'SoloASBridge/SoloASBridge.uplugin'))))){throw 'Privileged install root is not the installed engine SoloASBridge parent.'}
    $Admin=[Security.Principal.SecurityIdentifier]::new('S-1-5-32-544');$System=[Security.Principal.SecurityIdentifier]::new('S-1-5-18');$User=[Security.Principal.SecurityIdentifier]::new($ReadSid)
    $Acl=[Security.AccessControl.DirectorySecurity]::new();$Acl.SetAccessRuleProtection($true,$false);$Acl.SetOwner($Admin)
    foreach($Sid in @($Admin,$System)){$Acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($Sid,'FullControl','ContainerInherit,ObjectInherit','None','Allow'))}
    $Acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($User,'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow'))
    # OWNER RIGHTS suppresses implicit WRITE_DAC even if a created child happens
    # to inherit the administrator's user SID as its owner.
    $Acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-3-4'),'ReadPermissions','ContainerInherit,ObjectInherit','None','Allow'))
    $Base=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'soloAS'
    foreach($Path in @($Base,(Join-Path $Base 'update'),(Join-Path $Base ('update/'+$Id)))) {
        if(Test-Path -LiteralPath (Get-SoloExtendedPath ($Path))){
            $Item=Get-Item -LiteralPath (Get-SoloExtendedPath ($Path))
            if($Item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Linked protected directory.'}
            $Owner=(Get-Acl -LiteralPath (Get-SoloExtendedPath ($Path))).GetOwner([Security.Principal.SecurityIdentifier]).Value
            if($Owner -notin @('S-1-5-32-544','S-1-5-18')){throw 'Protected update directory has an untrusted owner.'}
            Set-Acl -LiteralPath (Get-SoloExtendedPath ($Path)) -AclObject $Acl
        } elseif ($PSVersionTable.PSEdition -eq 'Desktop') {
            $null=[IO.Directory]::CreateDirectory((Get-SoloExtendedPath ($Path)),$Acl)
        } else {
            $null=[IO.FileSystemAclExtensions]::CreateDirectory($Acl,(Get-SoloExtendedPath ($Path)))
        }
    }
    $Root=Join-Path $Base ('update/'+$Id)
    Start-Transcript -LiteralPath (Get-SoloExtendedPath ((Join-Path $Root 'worker.log')))|Out-Null
    try {
        $Package=Stage-SoloVerifiedPackage $Archive $Root $ArchiveHash $ApplyHash $ManifestHash
        $Script=Join-Path $Package 'SoloASBridge/Resources/Update/Apply-SoloASUpdate.ps1';$ManifestFile=Join-Path $Package 'soloAS-manifest.json'
        $Manifest=Get-Content -LiteralPath (Get-SoloExtendedPath ($ManifestFile)) -Raw|ConvertFrom-Json
        $Build=Get-Content -LiteralPath (Get-SoloExtendedPath ((Join-Path $Engine 'Build/Build.version'))) -Raw|ConvertFrom-Json
        $UE=('{0}.{1}.{2}' -f $Build.MajorVersion,$Build.MinorVersion,$Build.PatchVersion)
        $State=@{installRoot=$InstallRoot;installedManifestRoot=$InstallRoot;installedManifestFile=(Join-Path $InstallRoot 'soloAS-manifest.json');packageRoot=$Package;workRoot=$Root;editorPid=$EditorPid;editorStartTicks=$EditorTicks;editor=$Editor;ueVersion=$UE;version=$Manifest.version;token=$Id;deadlineTicks=[DateTime]::UtcNow.AddMinutes(90).Ticks}
        if ($Project) { $State.project=$Project }
        $State|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Get-SoloExtendedPath ((Join-Path $Root 'state.json'))) -Encoding UTF8
        $Decision=Join-Path $Root 'decision.txt';[IO.File]::WriteAllText((Get-SoloExtendedPath ($Decision)),'')
        $FileAcl=Get-Acl -LiteralPath (Get-SoloExtendedPath ($Decision))
        $FileAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($User,'Write','Allow'));Set-Acl -LiteralPath (Get-SoloExtendedPath ($Decision)) -AclObject $FileAcl
        & (Get-SoloExtendedPath $Script) -Worker -SecureRoot $Root
        exit $LASTEXITCODE
    } finally {Stop-Transcript|Out-Null}
}

function Start-SoloElevatedStage($Request,[string]$Archive) {
    $Id=[Guid]::NewGuid().ToString('N')
    $Root=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) ('soloAS/update/'+$Id)
    $Values=@($Request.installRoot,$Archive,$Request.archiveHash,
        (Get-SoloFileHash (Join-Path $Request.packageRoot 'SoloASBridge/Resources/Update/Apply-SoloASUpdate.ps1')),
        (Get-SoloFileHash (Join-Path $Request.packageRoot 'soloAS-manifest.json')),
        [Security.Principal.WindowsIdentity]::GetCurrent().User.Value,$Id,[string]$Request.editorPid,[string]$Request.editorStartTicks,$Request.editor,
        $(if($Request.PSObject.Properties['project']){[string]$Request.project}else{''}))
    $Arguments=@($Values|ForEach-Object {"'"+([string]$_).Replace("'","''")+"'"}) -join ' '
    $Command='function Get-SoloFullPath {'+${function:Get-SoloFullPath}.ToString()+"}`nfunction Get-SoloExtendedPath {"+${function:Get-SoloExtendedPath}.ToString()+"}`nfunction Get-SoloFileHash {"+${function:Get-SoloFileHash}.ToString()+"}`nfunction Expand-SoloArchive {"+${function:Expand-SoloArchive}.ToString()+"}`nfunction Stage-SoloVerifiedPackage {"+${function:Stage-SoloVerifiedPackage}.ToString()+"}`n& {"+$SoloSecureBootstrap.ToString()+"} "+$Arguments
    $Encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Command))
    try {$Process=Start-Process -FilePath (Get-SoloPowerShell) -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -EncodedCommand '+$Encoded) -Verb RunAs -PassThru}
    catch {Show-SoloManualUpdate $Archive;return $null}
    $Deadline=[DateTime]::UtcNow.AddMinutes(10)
    while(-not (Test-Path -LiteralPath (Get-SoloExtendedPath ((Join-Path $Root 'status.json'))))) {
        if($Process.HasExited){
            if(Test-Path -LiteralPath (Get-SoloExtendedPath (Join-Path $Root 'status.json'))){break}
            throw ('The privileged updater failed. See '+(Join-Path $Root 'worker.log'))
        }
        if([DateTime]::UtcNow -gt $Deadline){throw 'The protected update did not become ready. See worker.log.'}
        Start-Sleep -Milliseconds 250
    }
    $Status=Get-Content -LiteralPath (Get-SoloExtendedPath (Join-Path $Root 'status.json')) -Raw|ConvertFrom-Json
    if($Status.status -in @('failed','rollback-failed')){throw $Status.error}
    return $Root
}

function Invoke-SoloPrepare([string]$RequestFile) {
    $Request=Get-Content -LiteralPath (Get-SoloExtendedPath ($RequestFile)) -Raw|ConvertFrom-Json
    $Request.workRoot=(Get-SoloFullPath (Split-Path $RequestFile -Parent))
    # Privileged destinations are derived from the installed script, not JSON.
    $Request.installRoot=Get-SoloInstalledRoot
    $Request.installedManifestRoot=$Request.installRoot
    $Request.installedManifestFile=Join-Path $Request.installRoot 'soloAS-manifest.json'
    $Original=Get-Process -Id $Request.editorPid
    $Request.editorStartTicks=$Original.StartTime.ToUniversalTime().Ticks
    $Request.editor=$Original.Path
    foreach($Url in @($Request.downloadUrl,$Request.checksumsUrl)){if($Url -notmatch '^https://github\.com/cuviss-open/soloAS/releases/download/[^/]+/[^/?#]+\z'){throw 'Invalid release asset URL.'}}
    if($Request.assetName -notmatch '^soloAS-[0-9.]+(-ue[0-9.]+)?-(win64\.(7z|zip)|source\.zip)\z'){throw 'Invalid release package name.'}
    [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
    $Archive=Join-Path $Request.workRoot $Request.assetName;$Cancel=Join-Path $Request.workRoot 'cancel.request'
    Write-Host 'Downloading soloAS. Use Cancel download in the editor to cancel.'
    $Sums=Download-SoloAsset $Request.checksumsUrl (Join-Path $Request.workRoot 'SHA256SUMS') $Cancel -Capture
    $Expected=Get-SoloChecksum $Sums.content $Request.assetName
    $Downloaded=Download-SoloAsset $Request.downloadUrl $Archive $Cancel
    if($Downloaded.sha256 -ine $Expected){throw 'SHA256 verification failed. Nothing was installed.'}
    # Keep the network checksum in memory; never re-trust a mutable Saved file.
    $Request|Add-Member -NotePropertyName archiveHash -NotePropertyValue $Expected -Force
    $Request.packageRoot=Expand-SoloArchive $Archive (Join-Path $Request.workRoot 'extracted')
    $Manifest=Get-Content -LiteralPath (Get-SoloExtendedPath ((Join-Path $Request.packageRoot 'soloAS-manifest.json'))) -Raw|ConvertFrom-Json
    if($Manifest.updaterProtocol -ne 1 -or $Manifest.version -cne $Request.version -or $Request.ueVersion -cnotin $Manifest.ueVersions){throw 'Unsupported updater protocol or mismatched release identity.'}
    $Script=Join-Path $Request.packageRoot 'SoloASBridge/Resources/Update/Apply-SoloASUpdate.ps1'
    if(-not (Test-Path -LiteralPath (Get-SoloExtendedPath ($Script)) -PathType Leaf)){throw 'New package has no updater script.'}
    Copy-Item -LiteralPath (Get-SoloExtendedPath ($Request.installedManifestFile)) -Destination (Get-SoloExtendedPath ((Join-Path $Request.workRoot 'installed-manifest.json')))
    $Request|Add-Member -NotePropertyName archivePath -NotePropertyValue $Archive -Force
    if((Test-Path -LiteralPath (Get-SoloExtendedPath ($Cancel))) -or -not (Confirm-SoloRestart)){throw [OperationCanceledException]::new('Update cancelled. Nothing was installed.')}
    $Secure='';$Probe=Join-Path $Request.installRoot ('.soloas-write-'+[Guid]::NewGuid().ToString('N'))
    try {[IO.File]::WriteAllText((Get-SoloExtendedPath ($Probe)),'');Remove-Item -LiteralPath (Get-SoloExtendedPath ($Probe))} catch [UnauthorizedAccessException] {$Secure=Start-SoloElevatedStage $Request $Archive;if(-not $Secure){$script:KeepVerifiedArchive=$true;return 2}}
    $Request|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Get-SoloExtendedPath ($RequestFile)) -Encoding UTF8
    if($Secure){$Script=Join-Path (Get-Content -LiteralPath (Get-SoloExtendedPath ((Join-Path $Secure 'state.json'))) -Raw|ConvertFrom-Json).packageRoot 'SoloASBridge/Resources/Update/Apply-SoloASUpdate.ps1'}
    Start-SoloApply $Script $RequestFile $false $Secure
    return 0
}
if(-not $FunctionsOnly) {
    $Work=Split-Path $RequestFile -Parent
    Start-Transcript -LiteralPath (Get-SoloExtendedPath ((Join-Path $Work 'download.log')))|Out-Null
    $Prepared=$false
    try {$Code=Invoke-SoloPrepare $RequestFile;$Prepared=($Code -eq 0);exit $Code}
    catch [OperationCanceledException] {Write-Host $_.Exception.Message;exit 2}
    catch {$_|Out-String|Set-Content -LiteralPath (Get-SoloExtendedPath ((Join-Path $Work 'download-error.txt')));Write-Host $_.Exception.Message;exit 1}
    finally {
        if(-not $Prepared){
            try {Remove-Item -LiteralPath (Get-SoloExtendedPath ((Join-Path $Work 'extracted'))) -Recurse -Force -ErrorAction Stop}catch{}
            if(-not $script:KeepVerifiedArchive){
                foreach($File in Get-ChildItem -LiteralPath (Get-SoloExtendedPath ($Work)) -File){if($File.Name -match '^soloAS-[0-9.]+(-ue[0-9.]+)?-(win64\.(7z|zip)|source\.zip)\z'){try {Remove-Item -LiteralPath (Get-SoloExtendedPath ($File.FullName)) -Force}catch{}}}
            }
        }
        Stop-Transcript|Out-Null
    }
}
