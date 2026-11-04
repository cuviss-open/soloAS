#include "Editor/SoloASMaintenance.h"
#include "Editor/SoloASMaintenanceInternal.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Containers/Ticker.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolMenus.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Text/STextBlock.h"

namespace SoloASBridge::Maintenance
{
                                                                                               
namespace
{
const FName MenuOwner(TEXT("SoloASBridge.Maintenance"));
TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
FDelegateHandle ModuleHandle;
FDelegateHandle MenuHandle;
FDelegateHandle LoadedHandle;
FTSTicker::FDelegateHandle UpdateTicker;
FProcHandle UpdateProcess;
FString UpdateWork;
bool UpdatePending = false;

TSharedPtr<FJsonObject> ReadJson(const FString& Json)
{
    TSharedPtr<FJsonObject> Object;
    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
    return Object;
}

bool VersionParts(FString Version, TArray<int32>& Parts)
{
    Version.RemoveFromStart(TEXT("v"));
    Version = Version.Left(Version.Find(TEXT("+")) == INDEX_NONE ? Version.Len() : Version.Find(TEXT("+")));
    TArray<FString> Strings;
    Version.ParseIntoArray(Strings, TEXT("."), false);
    if (Strings.Num() != 3) return false;
    for (const FString& Part : Strings)
    {
        if (Part.IsEmpty()) return false;
        int32 Value = 0;
        for (TCHAR Digit : Part)
        {
            if (Digit < '0' || Digit > '9' || Value > (MAX_int32 - (Digit - '0')) / 10) return false;
            Value = Value * 10 + Digit - '0';
        }
        Parts.Add(Value);
    }
    return true;
}

bool IsReleaseUrl(const FString& Url)
{
    return Url.StartsWith(TEXT("https://github.com/cuviss-open/soloAS/releases/"));
}

FString EngineVersion()
{
    const auto& Version = FEngineVersion::Current();
    return FString::Printf(TEXT("%d.%d.%d"), Version.GetMajor(), Version.GetMinor(), Version.GetPatch());
}

void Show(const FText& Message)
{
    FMessageDialog::Open(EAppMsgType::Ok, Message);
}

void About()
{
    FString Text = TEXT("soloAS\n\n");
    TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetDiscoveredPlugins();
    Plugins.Sort([](const TSharedRef<IPlugin>& A, const TSharedRef<IPlugin>& B) { return A->GetName() < B->GetName(); });
    TSet<FString> Family = {TEXT("SoloASBridge")};
    bool Added;
    do
    {
        Added = false;
        for (const auto& Plugin : Plugins)
            if (!Family.Contains(Plugin->GetName()))
                for (const auto& Dependency : Plugin->GetDescriptor().Plugins)
                    if (Family.Contains(Dependency.Name))
                    {
                        Family.Add(Plugin->GetName());
                        Added = true;
                        break;
                    }
    } while (Added);
    for (const auto& Plugin : Plugins)
    {
        if (!Family.Contains(Plugin->GetName())) continue;
        FString Json, Version;
        if (FFileHelper::LoadFileToString(Json, *Plugin->GetDescriptorFileName()) && ReadDescriptorVersion(Json, Version))
            Text += Plugin->GetName() + TEXT(": ") + Version + TEXT("\n");
    }
    const auto Bridge = IPluginManager::Get().FindPlugin(TEXT("SoloASBridge"));
    FString Profile;
    const bool Loaded = Bridge.IsValid() && FFileHelper::LoadFileToString(Profile,
        *FPaths::Combine(Bridge->GetBaseDir(), TEXT("Config/EngineCompatibility.json")));
    const auto Object = Loaded ? ReadJson(Profile) : nullptr;
    Text += TEXT("\nUnreal Engine: ") + EngineVersion() + TEXT("\n");
    Text += !Object.IsValid() ? TEXT("Support list unavailable.") :
        SupportsEngine(Object, EngineVersion()) ? TEXT("This UE version is supported.") : TEXT("This UE version is not in the support list.");
    Show(FText::FromString(Text));
}

FString InstalledManifestPath()
{
    const auto Bridge = IPluginManager::Get().FindPlugin(TEXT("SoloASBridge"));
    return Bridge.IsValid() ? FPaths::Combine(FPaths::GetPath(Bridge->GetBaseDir()), TEXT("soloAS-manifest.json")) : FString();
}

bool IsSourceInstallation()
{
    FString Json;
    FString Kind;
    if (FFileHelper::LoadFileToString(Json, *InstalledManifestPath()))
    {
        const auto Manifest = ReadJson(Json);
        if (Manifest.IsValid() && Manifest->TryGetStringField(TEXT("packageKind"), Kind)) return Kind == TEXT("source");
    }
    return false;
}

void InstallUpdate(const FUpdate& Update)
{
#if PLATFORM_WINDOWS
    if (UpdatePending) return;
    const auto Bridge = IPluginManager::Get().FindPlugin(TEXT("SoloASBridge"));
    if (!Bridge.IsValid() || Update.ChecksumsUrl.IsEmpty() || !FPaths::FileExists(InstalledManifestPath()) || FPaths::GetProjectFilePath().IsEmpty())
    {
        Show(NSLOCTEXT("SoloAS", "UpdateManifestMissing", "Automatic update requires a release installation with soloAS-manifest.json beside the plugin directories and an open project. Download and install this release manually first."));
        return;
    }
    const FString Work = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SoloASUpdates"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    if (!IFileManager::Get().MakeDirectory(*Work, true)) return;
    const auto Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("downloadUrl"), Update.DownloadUrl);
    Object->SetStringField(TEXT("checksumsUrl"), Update.ChecksumsUrl);
    Object->SetStringField(TEXT("assetName"), Update.AssetName);
    Object->SetStringField(TEXT("version"), Update.Version.StartsWith(TEXT("v")) ? Update.Version.Mid(1) : Update.Version);
    Object->SetStringField(TEXT("ueVersion"), EngineVersion());
    Object->SetStringField(TEXT("workRoot"), Work);
    Object->SetStringField(TEXT("packageRoot"), FString());
    Object->SetStringField(TEXT("installRoot"), FPaths::ConvertRelativePathToFull(FPaths::GetPath(Bridge->GetBaseDir())));
    Object->SetStringField(TEXT("installedManifestRoot"), FPaths::ConvertRelativePathToFull(FPaths::GetPath(Bridge->GetBaseDir())));
    Object->SetStringField(TEXT("installedManifestFile"), FPaths::ConvertRelativePathToFull(InstalledManifestPath()));
    Object->SetStringField(TEXT("project"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
    Object->SetStringField(TEXT("editor"), FPlatformProcess::ExecutablePath());
    Object->SetNumberField(TEXT("editorPid"), FPlatformProcess::GetCurrentProcessId());
    Object->SetNumberField(TEXT("editorStartTicks"), 0);
    Object->SetStringField(TEXT("token"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FString Json;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json));
    const FString RequestFile = FPaths::Combine(Work, TEXT("request.json"));
    if (!FFileHelper::SaveStringToFile(Json, *RequestFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return;
    const FString Script = FPaths::ConvertRelativePathToFull(FPaths::Combine(Bridge->GetBaseDir(), TEXT("Resources/Update/Prepare-SoloASUpdate.ps1")));
    const FString Arguments = FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -RequestFile \"%s\""), *Script, *RequestFile);
    const FString PowerShell = FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")), TEXT("System32/WindowsPowerShell/v1.0/powershell.exe"));
    UpdateProcess = FPlatformProcess::CreateProc(*PowerShell, *Arguments, true, true, true, nullptr, 0, nullptr, nullptr);
    if (!UpdateProcess.IsValid()) Show(NSLOCTEXT("SoloAS", "UpdaterStartFailed", "Could not start the soloAS updater. Download and install the package manually."));
    else { UpdateWork = Work; UpdatePending = true; }
#else
    Show(NSLOCTEXT("SoloAS", "ManualUpdate", "On macOS, download and install the matching source package manually. Save your work and close the editor before replacing plugins."));
    FPlatformProcess::LaunchURL(*Update.DownloadUrl, nullptr, nullptr);
#endif
}

bool PollUpdate(float)
{
    if (!UpdateProcess.IsValid() || FPlatformProcess::IsProcRunning(UpdateProcess)) return true;
    int32 Code = -1;
    FPlatformProcess::GetProcReturnCode(UpdateProcess, &Code);
    FPlatformProcess::CloseProc(UpdateProcess);
    UpdateProcess = FProcHandle();
    if (Code != 0)
    {
        UpdatePending = false;
        Show(FText::Format(NSLOCTEXT("SoloAS", "UpdatePreparationFailed", "The update was cancelled or could not be prepared. Nothing was installed. See {0} for details."),
            FText::FromString(FPaths::Combine(UpdateWork, TEXT("download.log")))));
    }
    return true;
}

void ConfirmLoaded()
{
    FString Receipt, Token;
    if (!FParse::Value(FCommandLine::Get(), TEXT("SoloASUpdateReceipt="), Receipt) ||
        !FParse::Value(FCommandLine::Get(), TEXT("SoloASUpdateToken="), Token) || Token.Len() != 32) return;
    FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SoloASUpdates"))) + TEXT("/");
    Receipt = FPaths::ConvertRelativePathToFull(Receipt);
    FPaths::NormalizeFilename(Receipt);
    if (Receipt.StartsWith(Root) && FPaths::GetCleanFilename(Receipt) == TEXT("loaded.txt"))
        FFileHelper::SaveStringToFile(Token, *Receipt, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void CheckUpdates()
{
    if (Request.IsValid()) return;
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("SoloASBridge"));
    const FString Installed = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString();
    Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(TEXT("https://api.github.com/repos/cuviss-open/soloAS/releases/latest"));
    Request->SetVerb(TEXT("GET"));
    Request->SetHeader(TEXT("Accept"), TEXT("application/vnd.github+json"));
    Request->SetHeader(TEXT("User-Agent"), TEXT("soloAS-update-check"));
    Request->SetTimeout(15.0f);
    Request->OnProcessRequestComplete().BindLambda([Installed](FHttpRequestPtr, FHttpResponsePtr Response, bool Connected)
    {
        Request.Reset();
        const FUpdate Update = ParseRelease(Connected && Response.IsValid(), Response.IsValid() ? Response->GetResponseCode() : 0,
            Response.IsValid() ? Response->GetContentAsString() : FString(), Installed, EngineVersion(), IsSourceInstallation());
        switch (Update.State)
        {
        case EUpdateState::Available:
        {
            const TSharedRef<SWindow> Window = SNew(SWindow)
                .Title(NSLOCTEXT("SoloAS", "UpdateTitle", "soloAS update"))
                .ClientSize(FVector2D(460, 210)).SupportsMaximize(false).SupportsMinimize(false)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(16)
                    [SNew(STextBlock).Text(FText::Format(NSLOCTEXT("SoloAS", "NewVersion", "soloAS {0} is available."), FText::FromString(Update.Version)))]
                    + SVerticalBox::Slot().AutoHeight().Padding(16, 0)
                    [SNew(SHyperlink).Text(NSLOCTEXT("SoloAS", "ReleaseNotes", "Read release notes"))
                        .OnNavigate_Lambda([Url = Update.ReleaseUrl] { FPlatformProcess::LaunchURL(*Url, nullptr, nullptr); })]
                    + SVerticalBox::Slot().AutoHeight().Padding(16)
                    [SNew(SButton).Text(NSLOCTEXT("SoloAS", "InstallUpdate", "Update (restart editor to finish)"))
                        .IsEnabled_Lambda([] { return !UpdatePending; })
                        .OnClicked_Lambda([Update]
                        {
                            InstallUpdate(Update);
                            return FReply::Handled();
                        })]
                    + SVerticalBox::Slot().AutoHeight().Padding(16, 0)
                    [SNew(SButton).Text(NSLOCTEXT("SoloAS", "CancelDownload", "Cancel download"))
                        .IsEnabled_Lambda([] { return UpdateProcess.IsValid(); })
                        .OnClicked_Lambda([]
                        {
                            FFileHelper::SaveStringToFile(TEXT("cancel"), *FPaths::Combine(UpdateWork, TEXT("cancel.request")));
                            return FReply::Handled();
                        })]
                ];
            FSlateApplication::Get().AddModalWindow(Window, FSlateApplication::Get().GetActiveTopLevelWindow());
            break;
        }
        case EUpdateState::NoCompatiblePackage:
            if (FMessageDialog::Open(EAppMsgType::YesNo, FText::Format(NSLOCTEXT("SoloAS", "NoPackage", "soloAS {0} is available, but has no package for UE {1}. Open release notes?"),
                FText::FromString(Update.Version), FText::FromString(EngineVersion()))) == EAppReturnType::Yes)
                FPlatformProcess::LaunchURL(*Update.ReleaseUrl, nullptr, nullptr);
            break;
        case EUpdateState::Current: Show(NSLOCTEXT("SoloAS", "UpToDate", "soloAS is up to date.")); break;
        case EUpdateState::Ignored: Show(NSLOCTEXT("SoloAS", "NoStableRelease", "No newer stable soloAS release is available.")); break;
        case EUpdateState::RateLimited: Show(NSLOCTEXT("SoloAS", "RateLimited", "GitHub has temporarily limited update checks. Please try again later.")); break;
        default: Show(NSLOCTEXT("SoloAS", "UpdateFailed", "Could not check for soloAS updates. Check your connection and try again later.")); break;
        }
    });
    if (!Request->ProcessRequest())
    {
        Request->OnProcessRequestComplete().Unbind();
        Request.Reset();
        Show(NSLOCTEXT("SoloAS", "UpdateStartFailed", "Could not start the update check. Please try again later."));
    }
}
}

bool ReadDescriptorVersion(const FString& Json, FString& Version)
{
    const auto Object = ReadJson(Json);
    return Object.IsValid() && Object->TryGetStringField(TEXT("VersionName"), Version) && !Version.IsEmpty();
}

bool SupportsEngine(const TSharedPtr<FJsonObject>& Profile, const FString& Engine)
{
    TArray<int32> Parts;
    if (!Profile.IsValid() || !VersionParts(Engine, Parts)) return false;
    const TSharedPtr<FJsonObject>* Support = nullptr;
    const TSharedPtr<FJsonObject>* Line = nullptr;
    if (!Profile->TryGetObjectField(TEXT("support"), Support) ||
        !(*Support)->TryGetObjectField(FString::Printf(TEXT("%d.%d"), Parts[0], Parts[1]), Line)) return false;
    bool Supported = true;
    (*Line)->TryGetBoolField(TEXT("supported"), Supported);
    if (!Supported) return false;
    double Minimum = 0;
    if (!(*Line)->TryGetNumberField(TEXT("minimumPatch"), Minimum) || Parts[2] < Minimum) return false;
    FString Policy;
    (*Line)->TryGetStringField(TEXT("policy"), Policy);
    if (Policy == TEXT("track-all-patches")) return true;
    const TSharedPtr<FJsonObject>* Patches = nullptr;
    if (Policy != TEXT("last-patch-only") || !(*Line)->TryGetObjectField(TEXT("patches"), Patches)) return false;
    int32 Latest = -1;
    for (const auto& Patch : (*Patches)->Values) Latest = FMath::Max(Latest, FCString::Atoi(*Patch.Key));
    return Parts[2] == Latest;
}

FUpdate ParseRelease(bool Connected, int32 Status, const FString& Json, const FString& Installed, const FString& Engine, bool SourcePackage)
{
    FUpdate Result;
    if (!Connected) return Result;
    if (Status == 403 || Status == 429) { Result.State = EUpdateState::RateLimited; return Result; }
    if (Status != 200) return Result;
    const auto Object = ReadJson(Json);
    bool Draft = false, Prerelease = false;
    if (!Object.IsValid() || !Object->TryGetBoolField(TEXT("draft"), Draft) || !Object->TryGetBoolField(TEXT("prerelease"), Prerelease)) return Result;
    if (Draft || Prerelease) { Result.State = EUpdateState::Ignored; return Result; }
    if (!Object->TryGetStringField(TEXT("tag_name"), Result.Version) ||
        !Object->TryGetStringField(TEXT("html_url"), Result.ReleaseUrl) || !IsReleaseUrl(Result.ReleaseUrl)) return Result;
    TArray<int32> Current, Latest;
    if (!VersionParts(Installed, Current) || !VersionParts(Result.Version, Latest)) return Result;
    bool Newer = false;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        if (Latest[Index] != Current[Index]) { Newer = Latest[Index] > Current[Index]; break; }
    }
    if (!Newer) { Result.State = EUpdateState::Current; return Result; }
    Result.State = EUpdateState::NoCompatiblePackage;
    const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
    if (!Object->TryGetArrayField(TEXT("assets"), Assets)) return Result;
    for (const auto& Asset : *Assets)
    {
        const auto Entry = Asset->AsObject();
        FString Name, Url;
        if (Entry.IsValid() && Entry->TryGetStringField(TEXT("name"), Name) && Name == TEXT("SHA256SUMS") &&
            Entry->TryGetStringField(TEXT("browser_download_url"), Url) && IsReleaseUrl(Url)) Result.ChecksumsUrl = Url;
    }
    for (const auto& Asset : *Assets)
    {
        const auto Entry = Asset->AsObject();
        FString Name, Url;
        if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("name"), Name) ||
            !Entry->TryGetStringField(TEXT("browser_download_url"), Url) || !IsReleaseUrl(Url)) continue;
        // Exact UE patch token; never offer symbols or an unrelated platform binary.
        const FString Prefix = TEXT("soloAS-") + Result.Version.RightChop(Result.Version.StartsWith(TEXT("v")) ? 1 : 0) + TEXT("-ue") + Engine;
#if PLATFORM_WINDOWS
        const bool Binary = !SourcePackage && (Name == Prefix + TEXT("-win64.7z") || Name == Prefix + TEXT("-win64.zip"));
#else
        const bool Binary = false;
#endif
        const FString ReleasePrefix = TEXT("soloAS-") + Result.Version.RightChop(Result.Version.StartsWith(TEXT("v")) ? 1 : 0);
        const bool Source = Name == ReleasePrefix + TEXT("-source.zip") ||
            (Name.StartsWith(ReleasePrefix + TEXT("-ue")) && Name.EndsWith(TEXT("-source.zip")));
        if (Binary || (Result.DownloadUrl.IsEmpty() && Source) || Name == Prefix + TEXT("-source.zip"))
        {
            Result.DownloadUrl = Url;
            Result.AssetName = Name;
            Result.State = EUpdateState::Available;
            if (Binary) break;
        }
    }
    return Result;
}
                                                                                             

                                                                                            
namespace
{
struct FMenuItem { FName Owner; FText Label; void (*Action)(); uint64 Generation; };
uint64 NextGeneration = 0;
TMap<FName, FMenuItem> MenuExtensionItems;
void AddExtensions(UToolMenu* Menu)
{
    if (MenuExtensionItems.IsEmpty()) return;
    auto& Section = Menu->FindOrAddSection(TEXT("SoloASMaintenanceExtensions"));
    TArray<FName> Ids;
    MenuExtensionItems.GetKeys(Ids);
    Ids.Sort(FNameLexicalLess());
    for (FName Id : Ids)
    {
        const auto& Item = MenuExtensionItems.FindChecked(Id);
        Section.AddMenuEntry(Id, Item.Label, FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([Id, Generation = Item.Generation]
        {
            if (const auto* Current = MenuExtensionItems.Find(Id); Current && Current->Generation == Generation)
            {
                if (FModuleManager::Get().IsModuleLoaded(Current->Owner)) Current->Action();
            }
        })));
    }
}
}
bool RegisterMenuItem(FName Id, FName OwnerModule, const FText& Label, void (*Action)())
{
    check(IsInGameThread());
    if (Id.IsNone() || OwnerModule.IsNone() || MenuExtensionItems.Contains(Id) || !Action ||
        !FModuleManager::Get().IsModuleLoaded(OwnerModule)) return false;
    MenuExtensionItems.Add(Id, {OwnerModule, Label, Action, ++NextGeneration});
    UToolMenus::Get()->RefreshAllWidgets();
    return true;
}
void UnregisterMenuItem(FName Id, FName OwnerModule)
{
    check(IsInGameThread());
    const auto* Item = MenuExtensionItems.Find(Id);
    if (Item && Item->Owner == OwnerModule)
    {
        MenuExtensionItems.Remove(Id);
        UToolMenus::Get()->RefreshAllWidgets();
    }
}
void OnModuleChanged(FName Name, EModuleChangeReason Reason)
{
    if (Reason != EModuleChangeReason::ModuleUnloaded) return;
    bool Removed = false;
    for (auto It = MenuExtensionItems.CreateIterator(); It; ++It)
        if (It.Value().Owner == Name) { It.RemoveCurrent(); Removed = true; }
    if (Removed) UToolMenus::Get()->RefreshAllWidgets();
}
                                                                                          

                                                                                                   
void Initialize()
{
    LoadedHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddStatic(&ConfirmLoaded);
    UpdateTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&PollUpdate), 0.5f);
    if (!GIsEditor || IsRunningCommandlet()) return;
    ModuleHandle = FModuleManager::Get().OnModulesChanged().AddStatic(&OnModuleChanged);
    MenuHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterMenus));
}
void RegisterMenus()
{
    FToolMenuOwnerScoped Owner(MenuOwner);
    auto* Main = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu"));
    auto& MainSection = Main->FindOrAddSection(TEXT("SoloAS"), FText(), FToolMenuInsert(TEXT("Tools"), EToolMenuInsertType::After));
    if (!MainSection.FindEntry(TEXT("SoloAS")))
        MainSection.AddSubMenu(TEXT("SoloAS"), NSLOCTEXT("SoloAS", "Menu", "SoloAS"), FText(), FNewToolMenuDelegate());
    auto* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.SoloAS"));
    auto& Section = Menu->FindOrAddSection(TEXT("SoloASMaintenance"), NSLOCTEXT("SoloAS", "Maintenance", "soloAS maintenance"));
    Section.AddMenuEntry(TEXT("AboutSoloAS"), NSLOCTEXT("SoloAS", "About", "About soloAS"), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&About)));
    Section.AddMenuEntry(TEXT("CheckSoloASUpdates"), NSLOCTEXT("SoloAS", "CheckUpdates", "Check for updates"), FText(), FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&CheckUpdates)));
    Menu->AddDynamicSection(TEXT("SoloASMaintenanceProviders"), FNewToolMenuDelegate::CreateStatic(&AddExtensions));
}

void Shutdown()
{
    FCoreDelegates::OnFEngineLoopInitComplete.Remove(LoadedHandle);
    FTSTicker::GetCoreTicker().RemoveTicker(UpdateTicker);
    if (UpdateProcess.IsValid()) { FPlatformProcess::CloseProc(UpdateProcess); UpdateProcess = FProcHandle(); }
    FModuleManager::Get().OnModulesChanged().Remove(ModuleHandle);
    UToolMenus::UnRegisterStartupCallback(MenuHandle);
    UToolMenus::UnregisterOwner(MenuOwner);
    MenuExtensionItems.Empty();
    if (Request.IsValid())
    {
        Request->OnProcessRequestComplete().Unbind();
        Request->CancelRequest();
        Request.Reset();
    }
}
                                                                                                 
}
