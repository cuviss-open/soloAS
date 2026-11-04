// Copyright Cuviss Studio. All Rights Reserved.
#include "Editor/SoloASBridgeSceneDestroyHook.h"
#include "SoloASEngineCompat.h"
// SOLOAS-COMPAT-BEGIN(<5.8) CS.Compatibility.Bridge.SceneDestroy [test: SoloAS.SceneGcReentrantAttachment] [id: CS.Compatibility.Bridge.SceneDestroy]
#if WITH_EDITOR && !IS_MONOLITHIC && SOLOAS_UE_BEFORE(5, 8) && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "Editor/SoloASBridgeEditorInlineHook.h"
#include "Runtime/SoloASSceneComponentAttachment.h"
#include "Components/SceneComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#if PLATFORM_MAC
#include <dlfcn.h>
#endif
namespace SoloASBridge::Editor
{
namespace
{
FEditorInlineHookHandle SceneDestroyHook;
void* EngineHandle = nullptr;
TAutoConsoleVariable<int32> CVarSceneDestroy(TEXT("soloas.EditorSceneDestroy"), 1,
    TEXT("Remove stale attachment edges during retired component destruction on UE 5.4-5.7."));
using FDestroy = void (*)(USceneComponent*, bool);
void Destroy(USceneComponent* Self, bool bDestroyingHierarchy)
{
    if (!bDestroyingHierarchy && IsInGameThread() && CVarSceneDestroy.GetValueOnGameThread() != 0)
        RemoveRetiredSceneComponentStaleChildren(Self);
    reinterpret_cast<FDestroy>(SceneDestroyHook.Original)(Self, bDestroyingHierarchy);
}
}
void InitializeSceneDestroyHook()
{
    if (SceneDestroyHook.Original) return;
#if PLATFORM_WINDOWS
    EngineHandle = FPlatformProcess::GetDllHandle(TEXT("UnrealEditor-Engine.dll"));
    void* Target = EngineHandle ? FPlatformProcess::GetDllExport(EngineHandle,
        TEXT("?OnComponentDestroyed@USceneComponent@@UEAAX_N@Z")) : nullptr;
#else
    void* Target = dlsym(RTLD_DEFAULT, "_ZN15USceneComponent20OnComponentDestroyedEb");
#endif
    if (!Target || !InstallEditorInlineHook(SceneDestroyHook, Target, reinterpret_cast<void*>(&Destroy),
        TEXT("USceneComponent::OnComponentDestroyed")))
        UE_LOG(LogTemp, Error, TEXT("[SoloAS] CS.Compatibility.Bridge.SceneDestroy unavailable"));
}
void ShutdownSceneDestroyHook()
{
    if (SceneDestroyHook.Original) RemoveEditorInlineHook(SceneDestroyHook, reinterpret_cast<void*>(&Destroy),
        TEXT("USceneComponent::OnComponentDestroyed"));
    if (!SceneDestroyHook.Original && EngineHandle)
    {
        FPlatformProcess::FreeDllHandle(EngineHandle);
        EngineHandle = nullptr;
    }
}
}
#else
namespace SoloASBridge::Editor
{
void InitializeSceneDestroyHook() {}
void ShutdownSceneDestroyHook() {}
}
#endif
// SOLOAS-COMPAT-END
