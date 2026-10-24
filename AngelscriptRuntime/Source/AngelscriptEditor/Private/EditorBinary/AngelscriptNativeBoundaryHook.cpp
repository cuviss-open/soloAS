// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "EditorBinary/AngelscriptNativeBoundaryHook.h"
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "Editor/SoloASEditorEntryHook.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "EngineGlobals.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "UObject/Class.h"

namespace
{
    FSoloASEditorEntryHook NativeBoundaryEntry;
    TAutoConsoleVariable<int32> CVarEditorInternalNativeBoundary(
        TEXT("soloas.EditorScriptNativeBoundaryInternal"), 1,
        TEXT("Use AS boundaries for UnrealEd internal editor workflows. 0 calls the original trampoline."));
    using FFindNativeBoundary = UClass* (*)(UClass*);
    UClass* FindNativeBoundary(UClass* Class)
    {
        const FFindNativeBoundary Original = reinterpret_cast<FFindNativeBoundary>(NativeBoundaryEntry.Original());
        if (!NativeBoundaryEntry.IsCallerInTargetModule(SoloASBridge::Editor::GetEditorHookCallerAddress()))
            return Original(Class);
        static IConsoleVariable* ExternalBoundary = IConsoleManager::Get().FindConsoleVariable(TEXT("soloas.EditorScriptNativeBoundary"));
        if (CVarEditorInternalNativeBoundary.GetValueOnAnyThread() != 0
            && ExternalBoundary && ExternalBoundary->GetInt() != 0)
        {
            for (UClass* Cursor = Class; Cursor; Cursor = Cursor->GetSuperClass())
            {
                if (Cursor->HasAnyClassFlags(CLASS_Native)) break;
                if (SoloASBridge::IsRegisteredScriptClass(Cursor)) return Cursor;
            }
        }
        return Original(Class);
    }
    template<typename T> constexpr bool HasNativeScriptClassMarker()
    {
        return requires(const T& Class) { Class.bIsScriptClass; };
    }
}
namespace AngelscriptNativeBoundaryHook
{
    void Initialize()
    {
        if (!GIsEditor || IsRunningCommandlet() || HasNativeScriptClassMarker<UClass>()) return;
        if (!NativeBoundaryEntry.Install(
            TEXT("?FindFirstNativeClass@FBlueprintEditorUtils@@SAPEAVUClass@@PEAV2@@Z"),
            "_ZN21FBlueprintEditorUtils20FindFirstNativeClassEP6UClass", reinterpret_cast<void*>(&FindNativeBoundary)))
            UE_LOG(LogTemp, Warning, TEXT("[SoloAS] UnrealEd internal native boundary hook unavailable; internal workflows keep Stock behavior."));
    }
    void Shutdown() { NativeBoundaryEntry.Remove(); }
}
#endif
