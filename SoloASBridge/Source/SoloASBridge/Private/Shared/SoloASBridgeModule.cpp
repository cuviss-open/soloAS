#include "Shared/SoloASBridgeModule.h"
#include "Runtime/SoloASBridgeScriptMetadata.h"

#include "Shared/SoloASBridgeReflection.h"

#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"

namespace SoloASBridge::Private
{
void PruneClassStates();
void ClearAllClassStates();
void PruneScriptClasses();
void ClearScriptClasses();
void PruneReflectionTraits();
void ClearAllReflectionTraits();
void InitializeObjectLifecycle();
void ShutdownObjectLifecycle();
void InitializeRetiredLoadingExportRepair();
void ShutdownRetiredLoadingExportRepair();
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
void InitializeEditorCopyPropertiesImports();
void ShutdownEditorCopyPropertiesImports();
#endif
}

namespace
{
FDelegateHandle PostGarbageCollectHandle;

void PruneBridgeRegistries()
{
    if (!IsInGameThread())
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("SoloASBridge registries must prune on the game thread.")
        );
        return;
    }
    SoloASBridge::Private::PruneClassStates();
    SoloASBridge::Private::PruneScriptClasses();
    SoloASBridge::Private::PruneReflectionTraits();
}


}

void FSoloASBridgeModule::StartupModule()
{
    SoloASBridge::InitializeScriptMetadata();
    SoloASBridge::InitializeReflectionTraits();
    SoloASBridge::Private::InitializeObjectLifecycle();
    SoloASBridge::Private::InitializeRetiredLoadingExportRepair();
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    SoloASBridge::Private::InitializeEditorCopyPropertiesImports();
#endif
    PostGarbageCollectHandle =
        FCoreUObjectDelegates::GetPostGarbageCollect().AddStatic(
            &PruneBridgeRegistries
        );
}

void FSoloASBridgeModule::ShutdownModule()
{
    SoloASBridge::ShutdownScriptMetadata();
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    SoloASBridge::Private::ShutdownEditorCopyPropertiesImports();
#endif
    SoloASBridge::Private::ShutdownRetiredLoadingExportRepair();
    SoloASBridge::ShutdownReflectionTraits();
    FCoreUObjectDelegates::GetPostGarbageCollect().Remove(
        PostGarbageCollectHandle
    );
    PostGarbageCollectHandle.Reset();
    SoloASBridge::Private::ShutdownObjectLifecycle();
    SoloASBridge::Private::ClearAllClassStates();
    SoloASBridge::Private::ClearScriptClasses();
    SoloASBridge::Private::ClearAllReflectionTraits();
}

IMPLEMENT_MODULE(FSoloASBridgeModule, SoloASBridge)
