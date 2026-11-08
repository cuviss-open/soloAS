// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "SoloASToolchain.h"
#include "SoloASToolchainInstaller.h"

#include "Containers/Ticker.h"
#include "DesktopPlatformModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IDesktopPlatform.h"
#include "ISettingsModule.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "Internationalization/PolyglotTextData.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoloASToolchain, Log, All);

#define LOCTEXT_NAMESPACE "SoloASToolchain"

namespace SoloASToolchain
{
namespace
{
FCandidate Own, Managed;
ESelection Selection = ESelection::Missing;
bool bComplete = false;
const FName Owner(TEXT("SoloAS.CppToolchain"));

FString CompilerVersion(const FString& Compiler)
{
    // MSVC cl.exe 19.xx.yyyyy corresponds to the UBT toolchain version 14.xx.yyyyy.
    const uint64 Version = FPlatformMisc::GetFileVersion(Compiler);
    return Version ? FString::Printf(TEXT("%u.%u.%u"), uint32(Version >> 48) - 5,
        uint32((Version >> 32) & 0xffff), uint32((Version >> 16) & 0xffff)) : FString();
}

void OpenSettings()
{
    FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings"))
        .ShowViewer(TEXT("Project"), TEXT("SoloAS"), TEXT("CppToolchain"));
}

void OpenOfficialSetup()
{
    FPlatformProcess::LaunchURL(TEXT("https://learn.microsoft.com/en-us/visualstudio/gamedev/unreal/get-started/vs-tools-unreal-install"), nullptr, nullptr);
}

void Install()
{
    ShowInstaller();
}

enum class EPrompt { Cancel, Install, Official, Settings, Continue };

bool PromptMissing(bool bPackaging = true)
{
    if (!FSlateApplication::IsInitialized() || FApp::IsUnattended())
    {
        UE_LOG(LogSoloASToolchain, Error, TEXT("No selected usable C++ toolchain; configure it in SoloAS > C++ Toolchain."));
        return false;
    }
    EPrompt Choice = EPrompt::Cancel;
    const TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(FText::FromString(TEXT("SoloAS C++ toolchain required")))
        .SizingRule(ESizingRule::Autosized).SupportsMinimize(false).SupportsMaximize(false);
    const TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
    const TWeakPtr<SWindow> WeakWindow = Window;
    auto Add = [&](const TCHAR* Label, EPrompt Value)
    {
        Buttons->AddSlot().AutoWidth().Padding(4)[SNew(SButton).Text(FText::FromString(Label))
            .OnClicked_Lambda([&Choice, WeakWindow, Value]()
            {
                Choice = Value;
                if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin()) Pinned->RequestDestroyWindow();
                return FReply::Handled();
            })];
    };
    Add(TEXT("Install managed..."), EPrompt::Install);
    Add(TEXT("Official setup..."), EPrompt::Official);
    Add(TEXT("Toolchain settings..."), EPrompt::Settings);
    if (bPackaging) Add(TEXT("Continue packaging"), EPrompt::Continue);
    Add(bPackaging ? TEXT("Cancel") : TEXT("Close"), EPrompt::Cancel);
    Window->SetContent(SNew(SBox).WidthOverride(760).Padding(16)[SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)[SNew(STextBlock).AutoWrapText(true)
            .Text(FText::FromString(TEXT("No usable C++ toolchain is selected. C++ packaging, generated project bindings and StaticJIT compilation require MSVC and a Windows SDK accepted by this engine. "
                "Configure Microsoft's official tools, request managed installation, or change the SoloAS selection. Restart this Editor after setup or selection changes. "
                "Continuing may fail when compilation is required.")))]
        + SVerticalBox::Slot().AutoHeight()[Buttons]]);
    FSlateApplication::Get().AddModalWindow(Window, nullptr);
    if (Choice == EPrompt::Install) Install();
    if (Choice == EPrompt::Official) OpenOfficialSetup();
    if (Choice == EPrompt::Settings) OpenSettings();
    return Choice == EPrompt::Continue;
}
}

bool IsDetectionComplete() { return bComplete; }
ESelection ActiveSelection() { return Selection; }
FCandidate DetectedOwn() { return Own; }
FCandidate DetectedManaged() { return Managed; }
}

class FSoloASDeveloperToolsToolchainEditorModule : public IModuleInterface
{
    using FCandidate = SoloASToolchain::FCandidate;
    using ESelection = SoloASToolchain::ESelection;
    FProcHandle Query;
    void* ReadPipe = nullptr;
    void* WritePipe = nullptr;
    double QueryStarted = 0;
    bool bManagedQuery = false;
    FString QueryOutput, QueryJson, QueryKey, OriginalRoot, XmlPath;
    bool bAppliedManaged = false;
    bool bChangedRoot = false;
    bool bPrecheckOnly = false;
    FString AppliedRoot;
    ESoloASToolchainPreference StartupPreference = ESoloASToolchainPreference::Auto;
    FTSTicker::FDelegateHandle Tick;
    FDelegateHandle MenuHandle, InstallHandle;
    FNewToolMenuDelegate OriginalPackageMenu;

    bool PrepareProbe(FString& Project, FString& Xml, FString& Error)
    {
        const FString Probe = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("SoloAS/Toolchain/Probe"));
        Project = Probe / TEXT("SoloASToolchainProbe.uproject");
        const TPair<FString, FString> Files[] = {
            { Project, TEXT("{\"FileVersion\":3,\"DisableEnginePluginsByDefault\":true,\"Modules\":[{\"Name\":\"SoloASToolchainProbe\",\"Type\":\"Runtime\",\"LoadingPhase\":\"Default\"}]}\n") },
            { Probe / TEXT("Source/SoloASToolchainProbeEditor.Target.cs"), TEXT("using UnrealBuildTool; public class SoloASToolchainProbeEditorTarget : TargetRules { public SoloASToolchainProbeEditorTarget(TargetInfo Target) : base(Target) { Type = TargetType.Editor; DefaultBuildSettings = BuildSettingsVersion.Latest; ExtraModuleNames.Add(\"SoloASToolchainProbe\"); } }\n") },
            { Probe / TEXT("Source/SoloASToolchainProbe/SoloASToolchainProbe.Build.cs"), TEXT("using UnrealBuildTool; public class SoloASToolchainProbe : ModuleRules { public SoloASToolchainProbe(ReadOnlyTargetRules Target) : base(Target) { PublicDependencyModuleNames.Add(\"Core\"); } }\n") },
            { Probe / TEXT("Source/SoloASToolchainProbe/SoloASToolchainProbe.cpp"), TEXT("#include \"Modules/ModuleManager.h\"\nIMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, SoloASToolchainProbe, \"SoloASToolchainProbe\");\n") }
        };
        auto WriteChanged = [&Error](const FString& Filename, const FString& Content)
        {
            FString Before;
            if (FFileHelper::LoadFileToString(Before, *Filename) && Before == Content) return true;
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
            if (FFileHelper::SaveStringToFile(Content, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return true;
            Error = TEXT("Cannot prepare the disposable UBT toolchain probe: ") + Filename;
            return false;
        };
        for (const auto& File : Files) if (!WriteChanged(File.Key, File.Value)) return false;
        Xml = TEXT("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Configuration xmlns=\"https://www.unrealengine.com/BuildConfiguration\"></Configuration>\n");
        if (IFileManager::Get().FileExists(*XmlPath) && !FFileHelper::LoadFileToString(Xml, *XmlPath))
        {
            Error = TEXT("Cannot read project UBT configuration for toolchain discovery.");
            return false;
        }
        return WriteChanged(Probe / TEXT("Saved/UnrealBuildTool/BuildConfiguration.xml"), Xml);
    }

    bool StartQuery(bool bManaged)
    {
        using namespace SoloASToolchain;
        bManagedQuery = bManaged;
        FString Arguments = TEXT("-Mode=Query -Query=TargetDetails -Platform=Win64 -Configuration=Development -NoUBA -WaitMutex");
        IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
        FString Project, Xml;
        if (!PrepareProbe(Project, Xml, (bManaged ? Managed : Own).Error)) return false;
        QueryJson = FPaths::ProjectIntermediateDir() / TEXT("SoloAS/Toolchain") / (bManaged ? TEXT("managed.json") : TEXT("own.json"));
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(QueryJson), true);
        Arguments += FString::Printf(TEXT(" -Target=SoloASToolchainProbeEditor -Project=\"%s\" -OutputPath=\"%s\""),
            *Project, *FPaths::ConvertRelativePathToFull(QueryJson));
        if (!bManaged)
        {
            for (const TCHAR* Setting : { TEXT("Compiler"), TEXT("CompilerVersion"), TEXT("WindowsSDKVersion") })
            {
                FString Value;
                if (!GConfig->GetString(TEXT("/Script/WindowsTargetPlatform.WindowsTargetSettings"), Setting, Value, GEngineIni) || Value.IsEmpty()) continue;
                for (const TCHAR C : Value)
                    if (!FChar::IsAlnum(C) && C != TEXT('.'))
                    {
                        Own.Error = TEXT("Invalid Windows compiler/SDK version setting.");
                        return false;
                    }
                Arguments += FString::Printf(TEXT(" -%s=%s"), Setting, *Value);
            }
        }
        if (bManaged)
        {
            // Managed installs have one versioned VS2022 family and the standard Windows Kits/10 view.
            const FString Base = ManagedRoot() / TEXT("HostWin64/Win64");
            TArray<FString> Versions, Sdks;
            IFileManager::Get().FindFiles(Versions, *(Base / TEXT("VS2022/*")), false, true);
            IFileManager::Get().FindFiles(Sdks, *(Base / TEXT("Windows Kits/10/Include/*")), false, true);
            Versions.Sort(); Sdks.Sort();
            FString Version, Sdk;
            for (int32 I = Versions.Num() - 1; I >= 0 && Version.IsEmpty(); --I)
                Version = CompilerVersion(Base / TEXT("VS2022") / Versions[I] / TEXT("bin/Hostx64/x64/cl.exe"));
            for (int32 I = Sdks.Num() - 1; I >= 0 && Sdk.IsEmpty(); --I)
                if (IFileManager::Get().FileExists(*(Base / TEXT("Windows Kits/10/Lib") / Sdks[I] / TEXT("um/x64/kernel32.lib")))) Sdk = Sdks[I];
            if (Version.IsEmpty() || Sdk.IsEmpty())
            {
                Managed.Error = TEXT("Managed MSVC / Windows SDK layout is absent or incomplete.");
                return false;
            }
            Arguments += FString::Printf(TEXT(" -Compiler=VisualStudio2022 -CompilerVersion=%s -WindowsSDKVersion=%s"), *Version, *Sdk);
        }
        // InvokeUnrealBuildToolAsync creates the child before returning. Only that child inherits this probe's root.
        const FString Previous = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_SDKS_ROOT"));
        const FString OwnRoot = RootForSelection(ESelection::Own, OriginalRoot, ManagedRoot());
        const FString ProbeRoot = bManaged ? ManagedRoot() : OwnRoot;
        QueryKey = DetectionKey(FEngineVersion::Current().ToString() + FPaths::EngineDir(), Xml, Arguments, ProbeRoot);
        FString CachedKey, CachedJson;
        if (FFileHelper::LoadFileToString(CachedKey, *FPaths::ChangeExtension(QueryJson, TEXT("cache-id")))
            && FFileHelper::LoadFileToString(CachedJson, *QueryJson))
        {
            FCandidate Cached = ReadQuery(CachedJson, 0, bManaged ? ManagedRoot() : FString(), bManaged ? FString() : ManagedRoot(),
                [](const FString& File) { return IFileManager::Get().FileExists(*File); });
            if (Cached.bUsable)
            {
                Cached.CompilerVersion = CompilerVersion(Cached.Compiler);
                if (CachedKey == QueryKey + TEXT("\n") + Cached.CompilerVersion)
                {
                    (bManaged ? Managed : Own) = MoveTemp(Cached);
                    UE_LOG(LogSoloASToolchain, Display, TEXT("Reused validated %s toolchain discovery cache."), bManaged ? TEXT("managed") : TEXT("own"));
                    return false;
                }
            }
        }
        IFileManager::Get().Delete(*QueryJson);
        if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
        {
            (bManaged ? Managed : Own).Error = TEXT("Unable to create the UnrealBuildTool output pipe.");
            return false;
        }
        FPlatformMisc::SetEnvironmentVar(TEXT("UE_SDKS_ROOT"), *ProbeRoot);
        Query = Desktop->InvokeUnrealBuildToolAsync(Arguments, *GLog, ReadPipe, WritePipe, true);
        FPlatformMisc::SetEnvironmentVar(TEXT("UE_SDKS_ROOT"), *Previous);
        QueryOutput.Reset();
        QueryStarted = FPlatformTime::Seconds();
        if (!Query.IsValid())
        {
            (bManaged ? Managed : Own).Error = TEXT("Unable to start this engine's UnrealBuildTool query.");
            FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
            ReadPipe = WritePipe = nullptr;
        }
        return Query.IsValid();
    }

    void FinishDetection()
    {
        using namespace SoloASToolchain;
        Selection = Select(StartupPreference, Own.bUsable, Managed.bUsable);
        if (Selection == ESelection::Managed && !bPrecheckOnly)
        {
            XmlPath = FPaths::ProjectSavedDir() / TEXT("UnrealBuildTool/BuildConfiguration.xml");
            if (WriteManagedConfig(XmlPath, Managed, Managed.Error))
            {
                bAppliedManaged = true;
            }
            if (!Managed.Error.IsEmpty()) Selection = ESelection::Missing;
        }
        AppliedRoot = RootForSelection(Selection, OriginalRoot, ManagedRoot());
        if (!bPrecheckOnly && AppliedRoot != OriginalRoot)
        {
            FPlatformMisc::SetEnvironmentVar(TEXT("UE_SDKS_ROOT"), *AppliedRoot);
            bChangedRoot = true;
        }
        bComplete = true;
        UE_LOG(LogSoloASToolchain, Display, TEXT("Detection complete: own=%d managed=%d selected=%d. %s %s"),
            Own.bUsable, Managed.bUsable, int32(Selection), *Own.Error, *Managed.Error);
        if (Selection == ESelection::Missing && !IsRunningCommandlet() && !FApp::IsUnattended()) PromptMissing(false);
    }

    bool Poll(float)
    {
        using namespace SoloASToolchain;
        QueryOutput += FPlatformProcess::ReadPipe(ReadPipe);
        if (FPlatformProcess::IsProcRunning(Query) && FPlatformTime::Seconds() - QueryStarted < 180) return true;
        int32 Code = -1;
        if (FPlatformProcess::IsProcRunning(Query))
        {
            FPlatformProcess::TerminateProc(Query, true);
            FPlatformProcess::WaitForProc(Query);
        }
        FPlatformProcess::GetProcReturnCode(Query, &Code);
        QueryOutput += FPlatformProcess::ReadPipe(ReadPipe);
        FPlatformProcess::CloseProc(Query);
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
        ReadPipe = WritePipe = nullptr;
        FString Json;
        FFileHelper::LoadFileToString(Json, *QueryJson);
        FFileHelper::SaveStringToFile(FString::Printf(TEXT("ExitCode: %d\n"), Code) + QueryOutput,
            *FPaths::ChangeExtension(QueryJson, TEXT("log")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        FCandidate& Candidate = bManagedQuery ? Managed : Own;
        Candidate = ReadQuery(Json, Code, bManagedQuery ? ManagedRoot() : FString(),
            bManagedQuery ? FString() : ManagedRoot(), [](const FString& File) { return IFileManager::Get().FileExists(*File); });
        if (Candidate.bUsable)
        {
            Candidate.CompilerVersion = CompilerVersion(Candidate.Compiler);
            FFileHelper::SaveStringToFile(QueryKey + TEXT("\n") + Candidate.CompilerVersion,
                *FPaths::ChangeExtension(QueryJson, TEXT("cache-id")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }
        if (!Candidate.bUsable) UE_LOG(LogSoloASToolchain, Warning, TEXT("UBT toolchain query failed (exit %d, JSON chars %d): %s\n%s"), Code, Json.Len(), *Candidate.Error, *QueryOutput.Right(4000));
        if (!bManagedQuery && StartQuery(true)) return true;
        FinishDetection();
        return false;
    }

    bool PrePackage()
    {
        using namespace SoloASToolchain;
        if (!bComplete)
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("SoloAS is checking the C++ toolchain with UnrealBuildTool. Try packaging again when the SoloAS toolchain status is ready.")));
            return false;
        }
        if (Selection != ESelection::Missing)
        {
            FString Error;
            const FCandidate& Candidate = Selection == ESelection::Own ? Own : Managed;
            bool bValid = CheckFiles(Candidate, [](const FString& File) { return IFileManager::Get().FileExists(*File); }, Error);
            if (bAppliedManaged)
            {
                TArray<uint8> Current, Applied;
                bValid &= FPlatformMisc::GetEnvironmentVariable(TEXT("UE_SDKS_ROOT")) == ManagedRoot()
                    && FFileHelper::LoadFileToArray(Current, *XmlPath)
                    && FFileHelper::LoadFileToArray(Applied, *(XmlPath + TEXT(".SoloAS-applied.xml"))) && Current == Applied;
            }
            if (bValid) return true;
            UE_LOG(LogSoloASToolchain, Warning, TEXT("Selected C++ toolchain changed after detection. %s"), *Error);
        }
        return PromptMissing();
    }

    void RefreshAfterInstall(const FString&)
    {
        using namespace SoloASToolchain;
        if (Query.IsValid()) return; // An in-flight probe completes before the user retries refresh.
        FString Error;
        if (bAppliedManaged && !RestoreProjectConfig(XmlPath, Error))
        {
            UE_LOG(LogSoloASToolchain, Error, TEXT("%s"), *Error);
            return;
        }
        bAppliedManaged = false;
        Own = FCandidate(); Managed = FCandidate(); bComplete = false;
        for (const TCHAR* Cache : {TEXT("own.cache-id"), TEXT("managed.cache-id")})
            IFileManager::Get().Delete(*(FPaths::ProjectIntermediateDir() / TEXT("SoloAS/Toolchain") / Cache));
        if (StartQuery(false) || StartQuery(true))
            Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FSoloASDeveloperToolsToolchainEditorModule::Poll), 0.1f);
        else FinishDetection();
    }

    void BeforeMenu(FName Name, const FToolMenuContext&)
    {
        if (Name != TEXT("LevelEditor.MainMenu.Platforms") || OriginalPackageMenu.IsBound()) return;
        UToolMenu* Platforms = UToolMenus::Get()->FindMenu(Name);
        FToolMenuSection* Section = Platforms ? Platforms->FindSection(TEXT("ContentManagementSection")) : nullptr;
        FToolMenuEntry* Entry = Section ? Section->FindEntry(TEXT("PackageSubMenu")) : nullptr;
        if (!Entry || !Entry->SubMenuData.ConstructMenu.NewToolMenu.IsBound()) return;
        OriginalPackageMenu = Entry->SubMenuData.ConstructMenu.NewToolMenu;
        SoloASToolchain::WrapPackageMenu(*Entry, [this]() { return PrePackage(); });
    }

    FText Status() const
    {
        using namespace SoloASToolchain;
        if (!bComplete) return FText::FromString(TEXT("C++ toolchain: checking with UBT..."));
        const FCandidate& Candidate = Selection == ESelection::Managed ? Managed : Own;
        return FText::FromString(Selection == ESelection::Missing ? TEXT("C++ toolchain: missing / unavailable")
            : FString::Printf(TEXT("C++ toolchain: %s (MSVC %s, SDK %s)"),
                Selection == ESelection::Own ? TEXT("own") : TEXT("managed"), *Candidate.CompilerVersion, *Candidate.SdkVersion));
    }

    void RegisterMenus()
    {
        static bool bMenuTextsRegistered = false;
        if (!bMenuTextsRegistered)
        {
            const struct { const TCHAR* Key; const TCHAR* English; const TCHAR* Chinese; } Texts[] = {
                {TEXT("Section"), TEXT("C++ Toolchain"), TEXT("C++ 工具链")},
                {TEXT("Menu"), TEXT("C++ Toolchain"), TEXT("C++ 工具链")},
                {TEXT("MenuTooltip"), TEXT("Tools for project bindings, packaging and StaticJIT compilation."), TEXT("工程绑定、打包和 StaticJIT 编译的工具。")},
            };
            TArray<FPolyglotTextData> Translations;
            for (const auto& Text : Texts)
            {
                FPolyglotTextData Data(ELocalizedTextSourceCategory::Editor, TEXT("SoloASToolchain"), Text.Key, Text.English, TEXT("en"));
                Data.AddLocalizedString(TEXT("en"), Text.English);
                Data.AddLocalizedString(TEXT("zh"), Text.Chinese);
                Translations.Add(MoveTemp(Data));
            }
            FTextLocalizationManager::Get().RegisterPolyglotTextData(Translations);
            bMenuTextsRegistered = true;
        }
        using namespace SoloASToolchain;
        FToolMenuOwnerScoped Scoped(Owner);
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.SoloAS"));
        Menu->AddSection(TEXT("CppToolchain"), LOCTEXT("Section", "C++ Toolchain"), FToolMenuInsert(TEXT("Packaging"), EToolMenuInsertType::After)).AddSubMenu(TEXT("CppToolchain"),
            LOCTEXT("Menu", "C++ Toolchain"), LOCTEXT("MenuTooltip", "Tools for project bindings, packaging and StaticJIT compilation."),
            FNewToolMenuDelegate::CreateLambda([this](UToolMenu* Sub)
            {
                FToolMenuSection& Section = Sub->AddSection(TEXT("CppToolchain"));
                Section.AddMenuEntry(TEXT("Status"), TAttribute<FText>::CreateLambda([this]() { return Status(); }),
                    FText::FromString(ManagedRoot()), FSlateIcon(), FUIAction(FExecuteAction(), FCanExecuteAction::CreateLambda([]() { return false; })));
                Section.AddMenuEntry(TEXT("Install"), FText::FromString(TEXT("Install managed C++ toolchain...")), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&Install)));
                Section.AddMenuEntry(TEXT("InstallDryRun"), FText::FromString(TEXT("Preview installation commands (no changes)...")), FText(), FSlateIcon(),
                    FUIAction(FExecuteAction::CreateLambda([]() { ShowInstaller(true); })));
                Section.AddMenuEntry(TEXT("Uninstall"), FText::FromString(TEXT("Uninstall private C++ toolchain...")), FText(), FSlateIcon(),
                    FUIAction(FExecuteAction::CreateStatic(&UninstallManaged)));
                Section.AddMenuEntry(TEXT("Refresh"), FText::FromString(TEXT("Refresh toolchain detection")), FText(), FSlateIcon(),
                    FUIAction(FExecuteAction::CreateLambda([this]() { RefreshAfterInstall(ManagedRoot()); })));
                Section.AddMenuEntry(TEXT("Official"), FText::FromString(TEXT("Official setup instructions...")), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&OpenOfficialSetup)));
                Section.AddMenuEntry(TEXT("Settings"), FText::FromString(TEXT("Toolchain settings...")), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&OpenSettings)));
                const TCHAR* Labels[] = { TEXT("Auto (prefer my own)"), TEXT("Force managed"), TEXT("Force my own") };
                const ESoloASToolchainPreference Values[] = { ESoloASToolchainPreference::Auto, ESoloASToolchainPreference::ForceManaged, ESoloASToolchainPreference::ForceOwn };
                for (int32 I = 0; I < 3; ++I)
                {
                    const auto Value = Values[I];
                    Section.AddMenuEntry(FName(Labels[I]), FText::FromString(Labels[I]),
                        FText::FromString(TEXT("Saved for this user and project. Restart the Editor to apply.")), FSlateIcon(),
                        FUIAction(FExecuteAction::CreateLambda([Value]()
                        {
                            GetMutableDefault<USoloASToolchainSettings>()->Preference = Value;
                            GetMutableDefault<USoloASToolchainSettings>()->SaveConfig();
                            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Toolchain selection saved. Restart the Editor to apply it.")));
                        }), FCanExecuteAction(), FIsActionChecked::CreateLambda([Value]() { return GetDefault<USoloASToolchainSettings>()->Preference == Value; })), EUserInterfaceActionType::RadioButton);
                }
            }));
    }

public:
    bool RunPrecheck()
    {
        using namespace SoloASToolchain;
        check(IsRunningCommandlet());
        bPrecheckOnly = true;
        if (StartQuery(false) || StartQuery(true))
        {
            while (Poll(0.0f)) FPlatformProcess::Sleep(0.05f);
        }
        else FinishDetection();
        return Selection != ESelection::Missing;
    }

    virtual void StartupModule() override
    {
        using namespace SoloASToolchain;
        InstallHandle = OnInstallRequested().AddRaw(this, &FSoloASDeveloperToolsToolchainEditorModule::RefreshAfterInstall);
        if (!IsRunningCommandlet() && MigrateLegacySettings(GEditorPerProjectIni))
            GetMutableDefault<USoloASToolchainSettings>()->ReloadConfig();
        StartupPreference = GetDefault<USoloASToolchainSettings>()->Preference;
        OriginalRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_SDKS_ROOT"));
        XmlPath = FPaths::ProjectSavedDir() / TEXT("UnrealBuildTool/BuildConfiguration.xml");
        FString RecoveryError;
        const bool bRecovered = IsRunningCommandlet() || RestoreProjectConfig(XmlPath, RecoveryError);
        // Commandlets neither package from the menu nor need startup discovery.
        if (!IsRunningCommandlet() && bRecovered)
        {
            if (StartQuery(false)) Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FSoloASDeveloperToolsToolchainEditorModule::Poll), 0.1f);
            else if (StartQuery(true)) Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FSoloASDeveloperToolsToolchainEditorModule::Poll), 0.1f);
            else FinishDetection();
        }
        else if (!bRecovered)
        {
            Own.Error = RecoveryError;
            bComplete = true;
            UE_LOG(LogSoloASToolchain, Error, TEXT("%s"), *RecoveryError);
        }
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSoloASDeveloperToolsToolchainEditorModule::RegisterMenus));
        MenuHandle = UToolMenus::Get()->OnPreGenerateWidget.AddRaw(this, &FSoloASDeveloperToolsToolchainEditorModule::BeforeMenu);
    }

    virtual void ShutdownModule() override
    {
        using namespace SoloASToolchain;
        OnInstallRequested().Remove(InstallHandle);
        FTSTicker::GetCoreTicker().RemoveTicker(Tick);
        if (Query.IsValid()) { FPlatformProcess::TerminateProc(Query, true); FPlatformProcess::CloseProc(Query); }
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
        if (UToolMenus* Menus = UToolMenus::TryGet())
        {
            Menus->OnPreGenerateWidget.Remove(MenuHandle);
            if (OriginalPackageMenu.IsBound())
                if (UToolMenu* Menu = Menus->FindMenu(TEXT("LevelEditor.MainMenu.Platforms")))
                    if (FToolMenuSection* Section = Menu->FindSection(TEXT("ContentManagementSection")))
                        if (FToolMenuEntry* Entry = Section->FindEntry(TEXT("PackageSubMenu"))) Entry->SubMenuData.ConstructMenu.NewToolMenu = OriginalPackageMenu;
        }
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(Owner);
        if (bAppliedManaged)
        {
            FString Error;
            if (!RestoreProjectConfig(XmlPath, Error)) UE_LOG(LogSoloASToolchain, Warning, TEXT("%s"), *Error);
        }
        if (bChangedRoot && FPlatformMisc::GetEnvironmentVariable(TEXT("UE_SDKS_ROOT")) == AppliedRoot)
            FPlatformMisc::SetEnvironmentVar(TEXT("UE_SDKS_ROOT"), *OriginalRoot);
    }
};

IMPLEMENT_MODULE(FSoloASDeveloperToolsToolchainEditorModule, SoloASDeveloperToolsToolchainEditor)

bool SoloASToolchain::RunPrecheck()
{
    return FModuleManager::GetModuleChecked<FSoloASDeveloperToolsToolchainEditorModule>(TEXT("SoloASDeveloperToolsToolchainEditor")).RunPrecheck();
}

#undef LOCTEXT_NAMESPACE
