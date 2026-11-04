// Copyright Cuviss Studio. All Rights Reserved.
#include "Editor/SoloASBridgeMenuAlignment.h"
#include "SoloASEngineCompat.h"
// SOLOAS-COMPAT-BEGIN(<5.5) Supply section alignment for script extensions on UE 5.4 [test: SoloAS.EditorMenuSectionAlignment] [id: CS.Compatibility.Bridge.MenuAlignment]
#if SOLOAS_UE_BEFORE(5, 5) && WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "Editor/SoloASBridgeEditorInlineHook.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ToolMenus.h"
#include "Widgets/Layout/SSpacer.h"
#if PLATFORM_MAC
#include <dlfcn.h>
#endif
namespace SoloASBridge::Editor
{
namespace
{
TMap<FName, TMap<FName, uint8>> Alignments;
FEditorInlineHookHandle MenuAlignmentHook;
void* ToolMenusHandle = nullptr;
TAutoConsoleVariable<int32> CVarMenuAlignment(TEXT("soloas.EditorMenuSectionAlignment"), 1,
    TEXT("Apply section alignment to script toolbars on UE 5.4. Zero keeps native layout."));
using FPopulate = void (*)(UToolMenus*, FToolBarBuilder&, UToolMenu*);

void Populate(UToolMenus* Self, FToolBarBuilder& Builder, UToolMenu* Menu)
{
    const auto Original = reinterpret_cast<FPopulate>(MenuAlignmentHook.Original);
    // Entry replacement also sees ToolMenus' internal calls. Only the named script-extension menus opt in.
    const auto* Sections = Alignments.Find(Menu->GetMenuName());
    if (!Sections || CVarMenuAlignment.GetValueOnGameThread() == 0)
    {
        Original(Self, Builder, Menu);
        return;
    }
    TArray<FToolMenuSection> Saved = MoveTemp(Menu->Sections);
    TArray<FToolMenuSection> Sorted = Saved;
    const auto Alignment = [Sections](const FToolMenuSection& Section)
    {
        const uint8* Value = Sections->Find(Section.Name);
        return Value ? *Value : uint8(0);
    };
    // First precedes Default; both occupy the leading group, matching newer ToolMenus.
    const auto Rank = [](uint8 Value) { return Value == 1 ? 0 : Value == 0 ? 1 : int(Value); };
    Sorted.StableSort([&](const FToolMenuSection& A, const FToolMenuSection& B)
    {
        return Rank(Alignment(A)) < Rank(Alignment(B));
    });
    uint8 Previous = 1;
    for (int32 Index = 0; Index < Sorted.Num(); ++Index)
    {
        const uint8 Current = Alignment(Sorted[Index]);
        if (Index > 0 && Current != Previous && Current >= 2)
        {
            FToolMenuSection Spacer;
            Spacer.Construct.NewToolBarDelegateLegacy = FNewToolBarDelegateLegacy::CreateLambda(
                [](FToolBarBuilder& Bar, UToolMenu*)
                {
                    // UE 5.4's widget block uses a fill slot for non-Fill alignment.
                    Bar.AddWidget(SNew(SSpacer), NAME_None, false, HAlign_Right);
                });
            Menu->Sections.Add(MoveTemp(Spacer));
        }
        Menu->Sections.Add(Sorted[Index]);
        Previous = Current;
    }
    Original(Self, Builder, Menu);
    Menu->Sections = MoveTemp(Saved);
}

bool Install()
{
    if (MenuAlignmentHook.Original) return true;
#if PLATFORM_WINDOWS
    ToolMenusHandle = FPlatformProcess::GetDllHandle(TEXT("UnrealEditor-ToolMenus.dll"));
    void* Target = ToolMenusHandle ? FPlatformProcess::GetDllExport(ToolMenusHandle,
        TEXT("?PopulateToolBarBuilder@UToolMenus@@AEAAXAEAVFToolBarBuilder@@PEAVUToolMenu@@@Z")) : nullptr;
#else
    void* Target = dlsym(RTLD_DEFAULT, "_ZN10UToolMenus22PopulateToolBarBuilderER15FToolBarBuilderP9UToolMenu");
#endif
    if (Target && InstallEditorInlineHook(MenuAlignmentHook, Target, reinterpret_cast<void*>(&Populate),
        TEXT("UToolMenus::PopulateToolBarBuilder"))) return true;
    UE_LOG(LogTemp, Warning, TEXT("[SoloAS] UE 5.4 menu alignment hook unavailable; retaining native layout."));
    return false;
}
}
void SetMenuSectionAlignment(FName Menu, FName Section, uint8 Alignment)
{
    if (Alignment == 0)
    {
        if (auto* Sections = Alignments.Find(Menu))
        {
            Sections->Remove(Section);
            if (Sections->IsEmpty()) Alignments.Remove(Menu);
        }
        return;
    }
    if (Install()) Alignments.FindOrAdd(Menu).Add(Section, Alignment);
}
uint8 GetMenuSectionAlignment(FName Menu, FName Section)
{
    const auto* Sections = Alignments.Find(Menu);
    const uint8* Value = Sections ? Sections->Find(Section) : nullptr;
    return Value ? *Value : 0;
}
void ShutdownMenuSectionAlignment()
{
    Alignments.Empty();
    if (MenuAlignmentHook.Original) RemoveEditorInlineHook(MenuAlignmentHook, reinterpret_cast<void*>(&Populate),
        TEXT("UToolMenus::PopulateToolBarBuilder"));
    if (!MenuAlignmentHook.Original && ToolMenusHandle)
    {
        FPlatformProcess::FreeDllHandle(ToolMenusHandle);
        ToolMenusHandle = nullptr;
    }
}
}
#else
namespace SoloASBridge::Editor
{
void SetMenuSectionAlignment(FName, FName, uint8) {}
uint8 GetMenuSectionAlignment(FName, FName) { return 0; }
void ShutdownMenuSectionAlignment() {}
}
#endif
// SOLOAS-COMPAT-END
