// Copyright Cuviss Studio. All Rights Reserved.
#include "Editor/SoloASBridgeListViewEntryClassHook.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)

#include "Components/ListViewBase.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"
#include "Editor/SoloASEditorEntryHook.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "UObject/UObjectGlobals.h"

#if PLATFORM_MAC
#include <dlfcn.h>
#endif

namespace SoloASBridge::Editor
{
namespace
{
FSoloASEditorEntryHook GListViewRebuildHook;
FDelegateHandle GListViewModulesChanged;
void* GListViewUMGHandle = nullptr;
UClass* GListViewUserWidgetClass = nullptr;
TAutoConsoleVariable<int32> CVarEditorListViewScriptEntryClass(
    TEXT("soloas.EditorListViewScriptEntryClass"), 1,
    TEXT("Allow registered AngelScript default list entry classes in the Stock editor."),
    ECVF_Default);

// The private table owner must be retained exactly as in UListViewBase::RebuildWidget.
struct FListViewTableTag
{
    using Type = TSharedPtr<STableViewBase> UListViewBase::*;
    friend Type Resolve(FListViewTableTag);
};
template<typename FListViewTableTag::Type Member>
struct TListViewTableAccess
{
    friend FListViewTableTag::Type Resolve(FListViewTableTag) { return Member; }
};
template struct TListViewTableAccess<&UListViewBase::MyTableViewBase>;

struct FListViewBuildAccess : UListViewBase
{
    static TSharedRef<SWidget> Build(UListViewBase& List)
    {
        auto& Table = List.*Resolve(FListViewTableTag{});
        Table = (List.*&FListViewBuildAccess::RebuildListWidget)();
        Table->SetIsScrollAnimationEnabled(List.*&FListViewBuildAccess::bEnableScrollAnimation);
        Table->SetScrollingAnimationInterpolationSpeed(List.*&FListViewBuildAccess::ScrollingAnimationInterpolationSpeed);
        Table->SetEnableTouchAnimatedScrolling(List.*&FListViewBuildAccess::bInEnableTouchAnimatedScrolling);
        Table->SetIsRightClickScrollingEnabled(List.*&FListViewBuildAccess::bEnableRightClickScrolling);
        Table->SetIsTouchScrollingEnabled(List.*&FListViewBuildAccess::bEnableTouchScrolling);
        Table->SetIsPointerScrollingEnabled(List.*&FListViewBuildAccess::bIsPointerScrollingEnabled);
        Table->SetFixedLineScrollOffset(List.*&FListViewBuildAccess::bEnableFixedLineOffset
            ? TOptional<double>(List.*&FListViewBuildAccess::FixedLineScrollOffset) : TOptional<double>());
        Table->SetWheelScrollMultiplier(GetGlobalScrollAmount() * (List.*&FListViewBuildAccess::WheelScrollMultiplier));
        return Table.ToSharedRef();
    }
};

// Keep the native member-function ABI, including the non-trivial TSharedRef return.
// This member has no data, virtual dispatch or this adjustment; it is never instantiated.
struct FListViewRebuildThunk
{
    TSharedRef<SWidget> Rebuild();
};
using FListViewRebuildMember = TSharedRef<SWidget> (UListViewBase::*)();
using FListViewThunkMember = TSharedRef<SWidget> (FListViewRebuildThunk::*)();
#if PLATFORM_WINDOWS
// UWidget also inherits INotifyFieldValueChanged: MSVC retains the zero this-adjust slot.
static_assert(sizeof(FListViewRebuildMember) == 2 * sizeof(void*));
static_assert(sizeof(FListViewThunkMember) == sizeof(void*));
#elif PLATFORM_MAC
static_assert(sizeof(FListViewRebuildMember) == 2 * sizeof(void*));
static_assert(sizeof(FListViewThunkMember) == 2 * sizeof(void*));
#endif

void* ListViewReplacementAddress()
{
    const FListViewThunkMember Member = &FListViewRebuildThunk::Rebuild;
    UPTRINT Parts[2]{};
    FMemory::Memcpy(Parts, &Member, sizeof(Member));
    // Itanium nonvirtual PMFs hold an even code pointer and a zero this adjustment.
#if PLATFORM_MAC
    if ((Parts[0] & 1) != 0 || Parts[1] != 0)
        return nullptr;
#endif
    return reinterpret_cast<void*>(Parts[0]);
}

TSharedRef<SWidget> FListViewRebuildThunk::Rebuild()
{
    UListViewBase& List = *reinterpret_cast<UListViewBase*>(this);
    FListViewRebuildMember Original{};
    void* OriginalAddress = GListViewRebuildHook.Original();
    FMemory::Memcpy(&Original, &OriginalAddress, sizeof(void*));
    const void* Caller = GetEditorHookCallerAddress();
    if (GListViewRebuildHook.IsCallerInTargetModule(Caller)
        && GIsEditor && IsInGameThread() && !IsRunningCommandlet()
        && CVarEditorListViewScriptEntryClass.GetValueOnGameThread() != 0)
    {
        // Avoid an import of UUserWidget::StaticClass: UMG is not loaded by this adapter.
        auto EntrySubclass = List.GetEntryWidgetClass();
        UClass* EntryClass = EntrySubclass.GetGCPtr().Get();
        if (IsValid(EntryClass) && GListViewUserWidgetClass != nullptr
            && EntryClass->IsChildOf(GListViewUserWidgetClass)
            && !EntryClass->bCooked && EntryClass->ClassGeneratedBy == nullptr
            && !EntryClass->HasAnyClassFlags(CLASS_Native | CLASS_Abstract
                | CLASS_Deprecated | CLASS_NewerVersionExists)
            && SoloASBridge::IsRegisteredScriptClass(EntryClass))
        {
            return FListViewBuildAccess::Build(List);
        }
    }
    return (List.*Original)();
}

void TryInstallListViewEntryClassHook()
{
    if (GListViewRebuildHook.Original() != nullptr || !FModuleManager::Get().IsModuleLoaded(TEXT("UMG")))
        return;
    GListViewUserWidgetClass = FindObject<UClass>(nullptr, TEXT("/Script/UMG.UserWidget"));
    void* Target = nullptr;
#if PLATFORM_WINDOWS
    GListViewUMGHandle = FPlatformProcess::GetDllHandle(TEXT("UnrealEditor-UMG.dll"));
    if (GListViewUMGHandle != nullptr)
    {
        Target = FPlatformProcess::GetDllExport(GListViewUMGHandle,
            TEXT("?RebuildWidget@UListViewBase@@MEAA?AV?$TSharedRef@VSWidget@@$00@@XZ"));
    }
#elif PLATFORM_MAC
    Target = dlsym(RTLD_DEFAULT, "_ZN13UListViewBase13RebuildWidgetEv");
    Dl_info Info{};
    if (Target != nullptr && (dladdr(Target, &Info) == 0 || Info.dli_fname == nullptr
        || FCStringAnsi::Strstr(Info.dli_fname, "UnrealEditor-UMG") == nullptr))
        Target = nullptr;
    if (Target != nullptr)
    {
        GListViewUMGHandle = FPlatformProcess::GetDllHandle(UTF8_TO_TCHAR(Info.dli_fname));
        if (GListViewUMGHandle == nullptr)
            Target = nullptr;
    }
#endif
    void* Replacement = ListViewReplacementAddress();
    if (GListViewUserWidgetClass == nullptr || Target == nullptr || Replacement == nullptr
        || !GListViewRebuildHook.Install(
            TEXT("?RebuildWidget@UListViewBase@@MEAA?AV?$TSharedRef@VSWidget@@$00@@XZ"),
            "_ZN13UListViewBase13RebuildWidgetEv", Replacement,
            TEXT("UnrealEditor-UMG.dll"), "UnrealEditor-UMG"))
    {
        UE_LOG(LogTemp, Warning, TEXT("[SoloAS] ListView script entry-class hook is unavailable; Stock validation is preserved."));
        if (GListViewUMGHandle != nullptr)
            FPlatformProcess::FreeDllHandle(GListViewUMGHandle);
        GListViewUMGHandle = nullptr;
        GListViewUserWidgetClass = nullptr;
    }
}

void OnListViewModuleChanged(FName ModuleName, EModuleChangeReason Reason)
{
    if (ModuleName == TEXT("UMG") && Reason == EModuleChangeReason::ModuleLoaded)
        TryInstallListViewEntryClassHook();
}
}

void InitializeListViewEntryClassHook()
{
    if (!GIsEditor || IsRunningCommandlet() || GListViewModulesChanged.IsValid())
        return;
    GListViewModulesChanged = FModuleManager::Get().OnModulesChanged().AddStatic(&OnListViewModuleChanged);
    TryInstallListViewEntryClassHook();
}

void ShutdownListViewEntryClassHook()
{
    FModuleManager::Get().OnModulesChanged().Remove(GListViewModulesChanged);
    GListViewModulesChanged.Reset();
    GListViewRebuildHook.Remove();
    if (GListViewRebuildHook.Original() == nullptr)
    {
        if (GListViewUMGHandle != nullptr)
            FPlatformProcess::FreeDllHandle(GListViewUMGHandle);
        GListViewUMGHandle = nullptr;
        GListViewUserWidgetClass = nullptr;
    }
}
}
#endif
