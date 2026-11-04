                                                                                                  
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "SoloASEditorManagement.h"
#include "AngelscriptManager.h"
#include "AngelscriptRuntimeGcLifecycle.h"
#include "Editor.h"
#include "Containers/Ticker.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopedSlowTask.h"
#include "PackageTools.h"
#include "ToolMenus.h"
#include "UObject/LinkerLoad.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/SavePackage.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace SoloASEditorManagement
{
namespace
{
const FName MenuOwner(TEXT("SoloAS.EditorManagement"));
struct FRebindingPackage
{
    TWeakObjectPtr<UPackage> Package;
    FString Filename;
};
TMap<FName, FRebindingPackage> RebindingPackages;
FTSTicker::FDelegateHandle TickHandle;
TArray<TWeakPtr<SNotificationItem>> Notifications;
bool bRebindingToastShown = false;
bool bActionActive = false;
FDelegateHandle EndLoadHandle;

bool IsProjectFile(const FString& Filename)
{
    return FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Filename),
        FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
}

void Notify(const FText& Message, FSimpleDelegate Fix = FSimpleDelegate())
{
    UE_LOG(Angelscript, Display, TEXT("SoloAS management: %s"), *Message.ToString());
    if (IsRunningCommandlet() || !FSlateApplication::IsInitialized()) return;
    FNotificationInfo Info(Message);
    Info.ExpireDuration = 12.0f;
    if (Fix.IsBound())
        Info.ButtonDetails.Add(FNotificationButtonInfo(
            NSLOCTEXT("SoloAS", "FixOnce", "Fix in one click"),
            NSLOCTEXT("SoloAS", "FixOnceHelp", "Run the corresponding SoloAS repair action."), Fix));
    Notifications.Add(FSlateNotificationManager::Get().AddNotification(Info));
}

void Report(const TCHAR* Action, const FActionResult& Result)
{
    Notify(FText::FromString(FString::Printf(TEXT("%s: %d succeeded, %d failed, %d skipped%s."),
        Action, Result.Succeeded, Result.Failed, Result.Skipped,
        Result.bCancelled ? TEXT(" (cancelled)") : TEXT(""))));
}

void ResaveAction() { ResaveEmptyEngineVersionAssets(); }
void RebindingAction() { FixLoadTimeRebindingAssets(); }

bool CanRun()
{
    return GEditor && !GEditor->PlayWorld && !bActionActive && !IsLoading() && !IsAsyncLoading()
        && !FAngelscriptManager::bIsHotReloading;
}

void OnEndLoadPackage(const FEndLoadPackageContext& Context)
{
    if (!IsInGameThread() || !HasAngelscriptRuntimeGcCompilationRetry()) return;
    for (UPackage* Package : Context.LoadedPackages)
    {
        if (!Package || Package->HasAnyFlags(RF_Transient) || Package->GetName().StartsWith(TEXT("/Script/"))) continue;
        FString Filename;
        if (FPackageName::DoesPackageExist(Package->GetName(), &Filename) && IsProjectFile(Filename))
            RebindingPackages.Add(Package->GetFName(), {Package, FPaths::ConvertRelativePathToFull(Filename)});
    }
}
}

namespace
{
TArray<FString> FindEmptyEngineVersionAssets()
{
    TArray<FString> Files;
    FPackageName::FindPackagesInDirectory(Files, FPaths::ProjectContentDir());
    for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
        if (Plugin->CanContainContent() && IsProjectFile(Plugin->GetContentDir()))
            FPackageName::FindPackagesInDirectory(Files, Plugin->GetContentDir());
    TArray<FString> Names;
    for (const FString& Filename : Files)
    {
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
        if (!Reader) continue;
        FPackageFileSummary Summary;
        *Reader << Summary;
        FString Name;
        if (!Reader->IsError() && Summary.Tag == PACKAGE_FILE_TAG
            && !(Summary.GetPackageFlags() & PKG_Cooked) && !Summary.SavedByEngineVersion.HasChangelist()
            && FPackageName::TryConvertFilenameToLongPackageName(Filename, Name)) Names.AddUnique(Name);
    }
    Names.Sort();
    return Names;
}
}

TArray<FName> GetLoadTimeRebindingPackages()
{
    if (IsInGameThread() && !IsLoading() && !IsAsyncLoading() && !IsGarbageCollecting()
        && !FAngelscriptManager::bIsHotReloading)
    {
        for (auto It = RebindingPackages.CreateIterator(); It; ++It)
        {
            // Keep live/unsaved packages and files whose mount point is temporarily unavailable.
            if (!It.Value().Package.IsValid() && !FindPackage(nullptr, *It.Key().ToString())
                && !IFileManager::Get().FileExists(*It.Value().Filename)
                && !FPackageName::DoesPackageExist(It.Key().ToString()))
                It.RemoveCurrent();
        }
    }
    TArray<FName> Names;
    RebindingPackages.GenerateKeyArray(Names);
    return Names;
}

FActionResult ResaveEmptyEngineVersionAssets()
{
    FActionResult Result;
    if (!CanRun()) { Result.Failed = 1; Report(TEXT("Resave empty engine version assets"), Result); return Result; }
    TGuardValue<bool> ActionGuard(bActionActive, true);
    const TArray<FString> Names = FindEmptyEngineVersionAssets();
    FScopedSlowTask Progress(Names.Num(), NSLOCTEXT("SoloAS", "ResaveProgress", "Resaving assets with empty engine version"));
    Progress.MakeDialog(true);
    for (const FString& Name : Names)
    {
        if (Progress.ShouldCancel()) { Result.bCancelled = true; break; }
        Progress.EnterProgressFrame(1, FText::FromString(Name));
        FString Filename;
        UPackage* Existing = FindPackage(nullptr, *Name);
        if ((Existing && Existing->IsDirty()) || !FPackageName::DoesPackageExist(Name, &Filename)
            || !IsProjectFile(Filename) || IFileManager::Get().IsReadOnly(*Filename))
        { ++Result.Skipped; continue; }
        UPackage* Package = LoadPackage(nullptr, *Name, LOAD_None);
        FlushAsyncLoading();
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        if (Package && UPackage::SavePackage(Package, nullptr, *Filename, Args))
        { Package->SetDirtyFlag(false); ++Result.Succeeded; }
        else ++Result.Failed;
    }
    Report(TEXT("Resave empty engine version assets"), Result);
    return Result;
}

FActionResult FixLoadTimeRebindingAssets()
{
    FActionResult Result;
    if (!CanRun()) { Result.Failed = 1; Report(TEXT("Reload affected assets"), Result); return Result; }
    TGuardValue<bool> ActionGuard(bActionActive, true);
    const TArray<FName> Names = GetLoadTimeRebindingPackages();
    FScopedSlowTask Progress(Names.Num(), NSLOCTEXT("SoloAS", "RebindProgress", "Reloading assets affected by load-time rebinding"));
    Progress.MakeDialog(true);
    for (FName Name : Names)
    {
        if (Progress.ShouldCancel()) { Result.bCancelled = true; break; }
        Progress.EnterProgressFrame(1, FText::FromName(Name));
        FString Filename;
        UPackage* Package = FindPackage(nullptr, *Name.ToString());
        if ((Package && Package->IsDirty()) || !FPackageName::DoesPackageExist(Name.ToString(), &Filename)
            || !IsProjectFile(Filename)) { ++Result.Skipped; continue; }
        FText Error;
        const bool bReloaded = Package
            ? UPackageTools::ReloadPackages({Package}, Error, EReloadPackagesInteractionMode::AssumeNegative)
            : LoadPackage(nullptr, *Name.ToString(), LOAD_None) != nullptr;
        FlushAsyncLoading();
        if (bReloaded) { RebindingPackages.Remove(Name); ++Result.Succeeded; }
        else { ++Result.Failed; UE_LOG(Angelscript, Warning, TEXT("SoloAS reload %s failed: %s"), *Name.ToString(), *Error.ToString()); }
    }
    Report(TEXT("Reload affected assets"), Result);
    return Result;
}

void RegisterMenu()
{
    FToolMenuOwnerScoped Owner(MenuOwner);
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu"));
    if (!Menu) return;
    FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("SoloAS"), FText::GetEmpty(),
        FToolMenuInsert(TEXT("Tools"), EToolMenuInsertType::After));
    Section.AddSubMenu(TEXT("SoloAS"),
        NSLOCTEXT("SoloAS", "Menu", "SoloAS"), NSLOCTEXT("SoloAS", "MenuHelp", "Manage SoloAS scripts and repair project assets."),
        FNewToolMenuDelegate::CreateLambda([](UToolMenu* Submenu)
        {
            auto& Section = Submenu->FindOrAddSection(TEXT("Management"));
            auto Add = [&Section](const FName Name, const FText& Label, FExecuteAction Action)
            {
                Section.AddMenuEntry(Name, Label, FText(), FSlateIcon(),
                    FUIAction(Action, FCanExecuteAction::CreateStatic(&CanRun)));
            };
            Add(
                TEXT("ResaveEmptyVersion"),
                NSLOCTEXT("SoloAS", "Resave", "Resave assets with empty engine version"),
                FExecuteAction::CreateStatic(&ResaveAction));
            Add(
                TEXT("FixLoadRebinding"),
                NSLOCTEXT("SoloAS", "FixRebind", "Fix assets affected by load-time rebinding"),
                FExecuteAction::CreateStatic(&RebindingAction));
        }));
}

void Initialize()
{
    EndLoadHandle = FCoreUObjectDelegates::OnEndLoadPackage.AddStatic(&OnEndLoadPackage);
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
    {
        if (!CanRun() || !FAngelscriptManager::bIsInitialCompileFinished) return true;
        if (!bRebindingToastShown && !GetLoadTimeRebindingPackages().IsEmpty())
        {
            bRebindingToastShown = true;
            Notify(
                NSLOCTEXT("SoloAS", "RebindNotice", "SoloAS deferred a script reload while assets were loading. Reload the affected assets."),
                FSimpleDelegate::CreateStatic(&RebindingAction));
        }
        return true;
    }));
}

void Shutdown()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    FCoreUObjectDelegates::OnEndLoadPackage.Remove(EndLoadHandle);
    EndLoadHandle.Reset();
    for (auto& Weak : Notifications)
        if (auto Item = Weak.Pin()) Item->ExpireAndFadeout();
    Notifications.Reset();
    UToolMenus::UnregisterOwner(MenuOwner);
    RebindingPackages.Reset();
}
}

                                                                                                
