// Copyright Cuviss Studio. All Rights Reserved.
#include "SoloASToolchainInstaller.h"
#include "SoloASToolchain.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"

namespace SoloASToolchain
{
namespace
{
FString PS(const FString& Value) { return TEXT("'") + Value.Replace(TEXT("'"), TEXT("''")) + TEXT("'"); }
const TCHAR* VsLicense = TEXT("https://visualstudio.microsoft.com/license-terms/vs2022-ga-diagnosticbuildtools/");
const TCHAR* SdkLicense = TEXT("https://github.com/microsoft/win32metadata/blob/main/licenses/sdk_license.txt");

FString Signature(const FString& Path)
{
    return TEXT("$s=Get-AuthenticodeSignature -LiteralPath ") + Path + TEXT("; if($s.Status -ne 'Valid' -or "
        "$s.SignerCertificate.GetNameInfo([Security.Cryptography.X509Certificates.X509NameType]::SimpleName,$false) -cne 'Microsoft Corporation')"
        "{throw 'A valid Microsoft Corporation Authenticode signature is required'};\n");
}

FString Preamble(const FString& Root)
{
    // No caller-controlled root: even the printable plan refuses any other location at execution time.
    return TEXT("$ErrorActionPreference='Stop'; Set-StrictMode -Version Latest;\n$root=") + PS(Root)
        + TEXT("; $expected=Join-Path $env:LOCALAPPDATA 'SoloAS/Toolchains/")
        + FEngineVersion::Current().ToString(EVersionComponent::Patch) + TEXT("';\n"
        "if([IO.Path]::GetFullPath($root).TrimEnd('\\') -ine [IO.Path]::GetFullPath($expected).TrimEnd('\\')){throw 'Unexpected managed root'};\n"
        "$p=$root; while($p){if((Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Managed root ancestor is a reparse point'}; $p=Split-Path -Parent $p};\n"
        "foreach($child in @('Installer','BuildTools','HostWin64','HostWin64/Win64','HostWin64/Win64/Windows Kits','HostWin64/Win64/Windows Kits/10','HostWin64/Win64/VS2022')){"
        "$c=Join-Path $root $child; if((Test-Path -LiteralPath $c) -and ((Get-Item -LiteralPath $c -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Managed directory is a reparse point'}};\n"
        "New-Item -ItemType Directory -Path (Join-Path $root 'Installer') -Force | Out-Null;\n"
        "$marker=Join-Path $root 'Installer/owner.txt'; if(Test-Path -LiteralPath $marker){if((Get-Content -LiteralPath $marker -Raw) -cne 'SoloAS.Toolchain.1'){throw 'Managed owner mismatch'}}"
        "else{if(@(Get-ChildItem -LiteralPath $root -Force | Where-Object Name -ne 'Installer').Count){throw 'Refusing an unowned nonempty root'}; [IO.File]::WriteAllText($marker,'SoloAS.Toolchain.1')};\n"
        "Start-Transcript -LiteralPath (Join-Path $root 'Installer/install.log') -Append | Out-Null;\n");
}

bool ExecuteScript(const FInstallStep& Step, FScopedSlowTask& Progress, FString& Output)
{
    const FString Script = TEXT("try {\n") + Step.Script
        + TEXT("\nStop-Transcript -ErrorAction SilentlyContinue | Out-Null; exit 0 } catch { Write-Output $_; try {Stop-Transcript | Out-Null} catch {}; exit 1 }\n");
    const FString PowerShell = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")) / TEXT("System32/WindowsPowerShell/v1.0/powershell.exe");
    void* Read = nullptr; void* Write = nullptr;
    if (!FPlatformProcess::CreatePipe(Read, Write)) return false;
    const FString Arguments = TEXT("-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand ")
        + FBase64::Encode(reinterpret_cast<const uint8*>(*Script), Script.Len() * sizeof(TCHAR));
    FProcHandle Process = FPlatformProcess::CreateProc(*PowerShell, *Arguments, false, true, true, nullptr, 0, nullptr, Write);
    if (!Process.IsValid()) { FPlatformProcess::ClosePipe(Read, Write); return false; }
    // Never kill an active MSI/VS transaction. Cancel is observed before the next stage.
    while (FPlatformProcess::IsProcRunning(Process))
    {
        Output += FPlatformProcess::ReadPipe(Read);
        Progress.EnterProgressFrame(0);
        FPlatformProcess::Sleep(0.05f);
    }
    int32 Code = -1;
    FPlatformProcess::GetProcReturnCode(Process, &Code);
    Output += FPlatformProcess::ReadPipe(Read);
    FPlatformProcess::CloseProc(Process);
    FPlatformProcess::ClosePipe(Read, Write);
    return Code == 0;
}
}

TArray<FInstallStep> InstallPlan(const FString& Root)
{
    const FString Begin = Preamble(Root);
    const FString Vs = PS(Root / TEXT("Installer/vs_BuildTools.exe"));
    const FString Sdk = PS(Root / TEXT("Installer/winsdksetup.exe"));
    const FString Instance = Root / TEXT("BuildTools");
    const FString Kits = Root / TEXT("HostWin64/Win64/Windows Kits/10");
    TArray<FInstallStep> Plan;
    Plan.Add({TEXT("Check private Windows SDK ownership before installing"),
        Begin
        + TEXT("foreach($key in @('Registry::HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots',"
            "'Registry::HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows Kits\\Installed Roots')){"
            "if(Test-Path -LiteralPath $key){$r=Get-ItemProperty -LiteralPath $key; if(($r.PSObject.Properties.Name -contains 'KitsRoot10') -and $r.KitsRoot10 -and "
            "[IO.Path]::GetFullPath($r.KitsRoot10).TrimEnd('\\') -ine [IO.Path]::GetFullPath(") + PS(Kits)
        + TEXT(").TrimEnd('\\')){throw 'A user-shared Windows SDK is registered. Managed installation stopped; existing SDK is untouched.'}}};\n")});
    auto Download = [&](const TCHAR* Name, const FString& File, const TCHAR* Url)
    {
        Plan.Add({Name, Begin + TEXT("$file=") + File + TEXT("; if(Test-Path -LiteralPath $file){\n")
            + Signature(TEXT("$file")) + TEXT("} else {\n[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;\n"
            "Invoke-WebRequest -UseBasicParsing -TimeoutSec 600 -Uri ") + PS(Url) + TEXT(" -OutFile ($file+'.partial');\n")
            + Signature(TEXT("($file+'.partial')")) + TEXT("Move-Item -LiteralPath ($file+'.partial') -Destination $file;\n}\n")});
    };
    Download(TEXT("Download and verify Microsoft Build Tools"), Vs,
        TEXT("https://download.visualstudio.microsoft.com/download/pr/5967a899-96aa-47e2-a7c5-1b7192f292ee/c0333eeb7a1a3dbcb0aff2a94d6331d01848eee02cdfb68ca9f622b121afc4db/vs_BuildTools.exe"));
    Download(TEXT("Download and verify Microsoft Windows SDK 22621.5040"), Sdk,
        TEXT("https://go.microsoft.com/fwlink/?linkid=2311806"));
    Plan.Add({TEXT("Install MSVC 14.44 x64 (cancel waits for this transaction)"), Begin + Signature(Vs)
        + TEXT("$cl=Get-ChildItem -Path ") + PS(Instance / TEXT("VC/Tools/MSVC/14.44.*/bin/Hostx64/x64/cl.exe"))
        + TEXT(" -ErrorAction SilentlyContinue; if(!$cl){\n$p=Start-Process -FilePath ") + Vs
        + TEXT(" -Verb RunAs -Wait -PassThru -ArgumentList ")
        + PS(TEXT("--quiet --wait --norestart --nocache --installPath \"") + Instance
            + TEXT("\" --add Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64"))
        + TEXT("; if($p.ExitCode -notin @(0,3010)){throw ('Build Tools exit '+$p.ExitCode)}; if($p.ExitCode -eq 3010){throw 'Restart Windows, then rerun setup'};\n}\n")});
    Plan.Add({TEXT("Install Windows SDK x64 (cancel waits for this transaction)"), Begin + Signature(Sdk)
        + TEXT("if(!(Test-Path -LiteralPath ") + PS(Kits / TEXT("Lib/10.0.22621.0/um/x64/kernel32.lib"))
        + TEXT(")){ $p=Start-Process -FilePath ") + Sdk + TEXT(" -Verb RunAs -Wait -PassThru -ArgumentList ")
        + PS(TEXT("/quiet /norestart /ceip off /features OptionId.DesktopCPPx64 /installpath \"") + Kits + TEXT("\""))
        + TEXT("; if($p.ExitCode -notin @(0,3010)){throw ('Windows SDK exit '+$p.ExitCode)}; if($p.ExitCode -eq 3010){throw 'Restart Windows, then rerun setup'} };\n")});
    Plan.Add({TEXT("Verify private files and publish AutoSDK view"), Begin
        + TEXT("$families=@(Get-ChildItem -Path ") + PS(Instance / TEXT("VC/Tools/MSVC/14.44.*"))
        + TEXT(" -Directory); if($families.Count -ne 1){throw 'Expected exactly one MSVC 14.44 family'};\n"
        "$msvc=$families[0].FullName; foreach($file in @('bin/Hostx64/x64/cl.exe','bin/Hostx64/x64/link.exe','bin/Hostx64/x64/lib.exe',"
        "'bin/Hostx64/x64/cvtres.exe','include/vector','lib/x64/libcmt.lib')){if(!(Test-Path -LiteralPath (Join-Path $msvc $file) -PathType Leaf)){throw ('Missing MSVC file '+$file)}};\n"
        "if((Get-Item -LiteralPath (Join-Path $msvc 'bin/Hostx64/x64/cl.exe')).VersionInfo.FileVersionRaw.Build -lt 35223){throw 'MSVC 14.44.35223 or newer servicing required'};\n"
        "$sdk=") + PS(Kits) + TEXT("; foreach($file in @('bin/10.0.22621.0/x64/rc.exe','bin/10.0.22621.0/x64/mt.exe',"
        "'Include/10.0.22621.0/um/windows.h','Include/10.0.22621.0/ucrt/corecrt.h','Lib/10.0.22621.0/um/x64/kernel32.lib',"
        "'Lib/10.0.22621.0/ucrt/x64/ucrt.lib')){if(!(Test-Path -LiteralPath (Join-Path $sdk $file) -PathType Leaf)){throw ('SDK did not install privately: '+$file)}};\n"
        "$view=Join-Path $root ('HostWin64/Win64/VS2022/'+$families[0].Name);\n"
        "New-Item -ItemType Directory -Path (Split-Path -Parent $view) -Force | Out-Null;\n"
        "if(Test-Path -LiteralPath $view){$item=Get-Item -LiteralPath $view; if($item.LinkType -ne 'Junction' -or $item.Target -ine $msvc){throw 'Existing AutoSDK view has an unexpected target'}}"
        "else{New-Item -ItemType Junction -Path $view -Target $msvc | Out-Null};\n")});
    return Plan;
}

FInstallResult RunInstallPlan(bool bAccepted, bool bDryRun, const TArray<FInstallStep>& Plan,
    TFunctionRef<bool()> Cancelled, TFunctionRef<bool(const FInstallStep&)> Execute, TFunctionRef<void(const FString&)> Log)
{
    FInstallResult Result;
    if (!bDryRun && !bAccepted) { Result.Error = TEXT("Microsoft licenses must be explicitly accepted."); return Result; }
    for (const auto& Step : Plan)
    {
        Log(Step.Name + TEXT("\n") + Step.Script);
        if (bDryRun) continue;
        if (Cancelled()) { Result.bCancelled = true; Result.Error = TEXT("Cancelled between transactions. Rerun setup to resume."); return Result; }
        if (!Execute(Step)) { Result.Error = Step.Name + TEXT(" failed. See Installer/install.log; rerun setup after correcting the error."); return Result; }
    }
    if (!bDryRun && Cancelled()) { Result.bCancelled = true; Result.Error = TEXT("Cancelled. Completed installer transactions are retained."); return Result; }
    Result.bSuccess = true;
    return Result;
}

void ShowInstaller(bool bDryRun)
{
    if (FApp::IsUnattended() && !bDryRun) return;
    if (!bDryRun)
    {
        // Opening each license precedes the explicit acceptance; there is no persisted or inferred consent.
        FPlatformProcess::LaunchURL(VsLicense, nullptr, nullptr);
        FPlatformProcess::LaunchURL(SdkLicense, nullptr, nullptr);
        const FString Prompt = TEXT("Review the Microsoft Build Tools and Windows SDK licenses opened in your browser.\n\n")
            + FString(VsLicense) + TEXT("\n") + SdkLicense
            + TEXT("\n\nInstall MSVC 14.44 (VS 2022 17.14) and SDK 10.0.22621.5040 in:\n") + ManagedRoot()
            + TEXT("\n\nMicrosoft's installers request administrator approval and register this instance with Windows. "
                "SoloAS never modifies an existing VS/SDK or global environment; registry registration is performed only by Microsoft installers. Setup stops if a shared SDK is registered. "
                "Cancel waits for the current official installer transaction to finish; rerun to resume.\n\n"
                "By choosing Yes, I personally accept BOTH Microsoft licenses and authorize this installation.");
        if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(Prompt)) != EAppReturnType::Yes) return;
    }
    const auto Plan = InstallPlan(ManagedRoot());
    FScopedSlowTask Progress(Plan.Num(), FText::FromString(TEXT("Microsoft C++ toolchain setup")));
    if (!bDryRun) Progress.MakeDialog(true);
    FString Transcript, Output;
    const auto Result = RunInstallPlan(!bDryRun, bDryRun, Plan, [&]() { return Progress.ShouldCancel(); },
        [&](const FInstallStep& Step) { Progress.EnterProgressFrame(1, FText::FromString(Step.Name)); return ExecuteScript(Step, Progress, Output); },
        [&](const FString& Line) { Transcript += Line + TEXT("\n"); });
    const FString LogFile = FPaths::ProjectLogDir() / TEXT("SoloASToolchain-plan.log");
    FFileHelper::SaveStringToFile(Transcript + Output, *LogFile);
    if (bDryRun) { UE_LOG(LogTemp, Display, TEXT("%s"), *Transcript); return; }
    if (Result.bSuccess) OnInstallRequested().Broadcast(ManagedRoot());
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Result.bSuccess
        ? TEXT("Private tools installed. Refreshing discovery with this engine's UBT. Restart the Editor before changing toolchain selection.")
        : Result.Error + TEXT("\n") + Output.Right(1500)));
}

TArray<FInstallStep> UninstallPlan(const FString& Root)
{
    const FString Begin = Preamble(Root);
    const FString Vs = PS(Root / TEXT("Installer/vs_BuildTools.exe"));
    const FString Sdk = PS(Root / TEXT("Installer/winsdksetup.exe"));
    const FString Kits = Root / TEXT("HostWin64/Win64/Windows Kits/10");
    return {
        { TEXT("Verify private ownership before uninstall"), Begin
            + TEXT("foreach($key in @('Registry::HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots',"
                "'Registry::HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows Kits\\Installed Roots')){"
                "if(Test-Path -LiteralPath $key){$r=Get-ItemProperty -LiteralPath $key; if(($r.PSObject.Properties.Name -contains 'KitsRoot10') -and $r.KitsRoot10 -and "
                "[IO.Path]::GetFullPath($r.KitsRoot10).TrimEnd('\\') -ine [IO.Path]::GetFullPath(") + PS(Kits)
            + TEXT(").TrimEnd('\\')){throw 'Shared Windows SDK detected. Refusing SDK uninstall or root deletion.'}}};\n") },
        { TEXT("Uninstall only the private SDK with Microsoft setup"), Begin
            + TEXT("if(Test-Path -LiteralPath ") + PS(Kits / TEXT("Lib/10.0.22621.0")) + TEXT("){\n")
            + Signature(Sdk) + TEXT("$p=Start-Process -FilePath ") + Sdk
            + TEXT(" -Verb RunAs -Wait -PassThru -ArgumentList '/uninstall /quiet /norestart'; "
                "if($p.ExitCode -ne 0){throw ('Private SDK uninstall exit '+$p.ExitCode)}; };\n") },
        { TEXT("Uninstall only the private Build Tools instance"), Begin
            + TEXT("if(Test-Path -LiteralPath ") + PS(Root / TEXT("BuildTools/VC")) + TEXT("){\n")
            + Signature(Vs) + TEXT("$p=Start-Process -FilePath ") + Vs
            + TEXT(" -Verb RunAs -Wait -PassThru -ArgumentList ")
            + PS(TEXT("uninstall --quiet --wait --norestart --installPath \"") + (Root / TEXT("BuildTools")) + TEXT("\""))
            + TEXT("; if($p.ExitCode -ne 0){throw ('Private Build Tools uninstall exit '+$p.ExitCode)}; };\n") },
        { TEXT("Remove only the owned private root"), Begin
            + TEXT("$dirs=[Collections.Generic.Stack[string]]::new(); $dirs.Push($root); $links=@(); "
                "while($dirs.Count){foreach($item in Get-ChildItem -LiteralPath $dirs.Pop() -Force){"
                "if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){"
                "if($item.LinkType -ne 'Junction' -or $item.Parent.FullName -ine [IO.Path]::GetFullPath((Join-Path $root 'HostWin64/Win64/VS2022')) "
                "-or $item.Target -ine [IO.Path]::GetFullPath((Join-Path $root ('BuildTools/VC/Tools/MSVC/'+$item.Name)))){throw 'Unexpected link; root retained'}; $links+= $item.FullName"
                "}elseif($item.PSIsContainer){$dirs.Push($item.FullName)}}}; "
                "foreach($link in $links){[IO.Directory]::Delete($link)}; Stop-Transcript | Out-Null; Remove-Item -LiteralPath $root -Recurse -Force;\n") }
    };
}

void UninstallManaged()
{
    if (FApp::IsUnattended() || !IFileManager::Get().FileExists(*(ManagedRoot() / TEXT("Installer/owner.txt")))) return;
    if (FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(TEXT("Uninstall the private Build Tools/SDK using Microsoft setup, then remove only this SoloAS root? "
        "User-shared SDKs will never be uninstalled. Cancel waits for the active transaction.\n") + ManagedRoot())) != EAppReturnType::Yes) return;
    const auto Plan = UninstallPlan(ManagedRoot());
    FScopedSlowTask Progress(Plan.Num(), FText::FromString(TEXT("Uninstall private C++ tools")));
    Progress.MakeDialog(true);
    FString Transcript, Output;
    const auto Result = RunInstallPlan(true, false, Plan, [&]() { return Progress.ShouldCancel(); },
        [&](const FInstallStep& Step) { Progress.EnterProgressFrame(1, FText::FromString(Step.Name)); return ExecuteScript(Step, Progress, Output); },
        [&](const FString& Line) { Transcript += Line + TEXT("\n"); });
    FFileHelper::SaveStringToFile(Transcript + Output, *(FPaths::ProjectLogDir() / TEXT("SoloASToolchain-uninstall.log")));
    if (Result.bSuccess) OnInstallRequested().Broadcast(ManagedRoot());
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Result.bSuccess ? TEXT("Private root removed. Restart the Editor before compiling.") : Result.Error + TEXT("\n") + Output.Right(1500)));
}
}
