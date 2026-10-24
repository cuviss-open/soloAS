#pragma once

#include "Runtime/SoloASBridgeObjectLifecycle.h"

#include "HAL/CriticalSection.h"
#include "UObject/ObjectKey.h"

namespace SoloASBridge::Private
{
enum class ETrackedState : uint8
{
    Pending,
    Deferred,
    Completing,
    Dispatching,
    Destroyed,
};

struct FTrackedEntry
{
    FObjectKey Key;
    const UObjectBase* ObjectIdentity = nullptr;
    void* TypeCookie = nullptr;
    uint64 Generation = 0;
    ETrackedState State = ETrackedState::Pending;
    uint64 ClaimSequence = 0;
    FRuntimeDeferredDestroyToken DeferredToken;
    FRuntimeDestroyControlCallback DeferredControl = nullptr;
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    FRuntimeTerminalDestroyTokenForTest TerminalToken;
    FRuntimeTerminalDestroyValidatorForTest TerminalValidator = nullptr;
#endif
};

extern FCriticalSection LifecycleMutex;
extern TMap<FObjectKey, FTrackedEntry> Entries;
extern FRuntimeDestroyCallback DestroyCallback;
extern FRuntimeOwnerReferenceCallback OwnerReferenceCallback;
extern FRuntimeDestroyPrepare DestroyPrepare;
extern FRuntimeDestroyControlCallback DestroyControl;
extern uint64 NextClaimSequence;
extern uint64 CallbackGeneration;
extern FDelegateHandle PostReachabilityHandle;
extern FDelegateHandle PreBeginDestroyHandle;
extern int32 DispatchingCount;
extern bool bDispatchInProgress;
extern thread_local bool bExitPurgeRuntimeDestroyCallback;
extern bool bDeleteListenerRegistered;
extern bool bDeleteListenerUnregistering;

void DispatchUnreachable();
void DispatchPreBeginDestroyUnreachable();
void FailIfOffGameThread(const TCHAR* Operation);
void InitializeObjectLifecycle();
void ShutdownObjectLifecycle();
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
bool CancelPendingTerminalRecipe(UObject* Object, void* ExpectedTypeCookie = nullptr);
#endif
}
