#include "Runtime/SoloASBridgeObjectLifecycle.h"
#include "Runtime/SoloASBridgeObjectLifecycleSupport.h"

#include "Misc/ScopeLock.h"
#include "UObject/Object.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace SoloASBridge
{
bool IsExitPurgeRuntimeDestroyCallback()
{
    return IsInGameThread() && GExitPurge && Private::bExitPurgeRuntimeDestroyCallback;
}

bool RegisterRuntimeDestroyCallback(
    FRuntimeDestroyCallback Callback,
    uint64 Generation,
    FRuntimeDestroyPrepare Prepare,
    FRuntimeDestroyControlCallback Control,
    FRuntimeOwnerReferenceCallback OwnerReferences
)
{
    checkf(IsInGameThread(), TEXT("registration requires game thread"));
    Private::FailIfOffGameThread(TEXT("Runtime callback registration"));
    checkf(Callback != nullptr, TEXT("callback missing"));
    checkf(Generation != 0, TEXT("callback generation must be non-zero"));
    if (Callback == nullptr || Generation == 0 || (Prepare == nullptr) != (Control == nullptr))
    {
        return false;
    }

    FScopeLock Lock(&Private::LifecycleMutex);
    checkf(
        Private::bDeleteListenerRegistered,
        TEXT("Runtime lifecycle is not initialized")
    );
    checkf(
        Private::DestroyCallback == nullptr
            && !Private::PostReachabilityHandle.IsValid()
            && !Private::PreBeginDestroyHandle.IsValid(),
        TEXT("Runtime callback is already registered")
    );
    checkf(
        Private::Entries.Num() == 0
            && Private::DispatchingCount == 0
            && !Private::bDispatchInProgress,
        TEXT("Runtime callback registration state is not empty")
    );
    if (
        !Private::bDeleteListenerRegistered
        || Private::DestroyCallback != nullptr
        || Private::PostReachabilityHandle.IsValid()
        || Private::PreBeginDestroyHandle.IsValid()
        || Private::Entries.Num() != 0
        || Private::DispatchingCount != 0
        || Private::bDispatchInProgress
    )
    {
        return false;
    }

    Private::DestroyCallback = Callback;
    Private::OwnerReferenceCallback = OwnerReferences;
    Private::DestroyPrepare = Prepare;
    Private::DestroyControl = Control;
    Private::CallbackGeneration = Generation;
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    Private::PostReachabilityHandle =
        FCoreUObjectDelegates::PostReachabilityAnalysis.AddStatic(
            &Private::DispatchUnreachable
        );
    Private::PreBeginDestroyHandle =
        FCoreUObjectDelegates::PreGarbageCollectConditionalBeginDestroy.AddStatic(
            &Private::DispatchPreBeginDestroyUnreachable
        );
    checkf(
        Private::PostReachabilityHandle.IsValid() && Private::PreBeginDestroyHandle.IsValid(),
        TEXT("Runtime lifecycle delegate registration failed")
    );
#endif
    return true;
}

bool TrackRuntimeObject(UObject* Object, void* TypeCookie)
{
    const bool bSupportedTrackingThread = IsInGameThread()
        || (IsInAsyncLoadingThread()
            && Object != nullptr
            && Object->HasAnyInternalFlags(
                EInternalObjectFlags_AsyncLoading
            ));
    checkf(bSupportedTrackingThread, TEXT("unsupported tracking thread"));
    if (!bSupportedTrackingThread)
    {
        return false;
    }
    checkf(Object != nullptr, TEXT("tracked object missing"));
    checkf(TypeCookie != nullptr, TEXT("tracked type cookie missing"));
    if (Object == nullptr || TypeCookie == nullptr)
    {
        return false;
    }

    FScopeLock Lock(&Private::LifecycleMutex);
    checkf(
        Private::DestroyCallback != nullptr
            && Private::CallbackGeneration != 0,
        TEXT("Runtime callback is not registered")
    );
    checkf(
        !Private::bDispatchInProgress,
        TEXT("DISPATCHING: object tracked during lifecycle dispatch")
    );
    checkf(
        !Object->IsUnreachable()
            && !Object->HasAnyFlags(
                RF_BeginDestroyed | RF_FinishDestroyed
            ),
        TEXT("tracked object is already entering destruction")
    );
    if (
        Private::DestroyCallback == nullptr
        || Private::CallbackGeneration == 0
        || Private::bDispatchInProgress
        || Object->IsUnreachable()
        || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
    )
    {
        return false;
    }

    const FObjectKey Key(Object);
    if (Private::Entries.Contains(Key))
    {
        return false;
    }
    Private::Entries.Add(
        Key,
        Private::FTrackedEntry{
            Key,
            Object,
            TypeCookie,
            Private::CallbackGeneration,
            Private::ETrackedState::Pending,
        }
    );
    return true;
}

bool UpdateRuntimeObjectTypeCookie(
    UObject* Object,
    void* ExpectedTypeCookie,
    void* NewTypeCookie
)
{
    checkf(IsInGameThread(), TEXT("cookie update requires game thread"));
    Private::FailIfOffGameThread(TEXT("Runtime type cookie update"));
    checkf(Object != nullptr, TEXT("tracked object missing"));
    checkf(ExpectedTypeCookie != nullptr, TEXT("expected type cookie missing"));
    checkf(NewTypeCookie != nullptr, TEXT("new type cookie missing"));
    if (
        Object == nullptr
        || ExpectedTypeCookie == nullptr
        || NewTypeCookie == nullptr
    )
    {
        return false;
    }

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    if (ExpectedTypeCookie != NewTypeCookie && !Private::CancelPendingTerminalRecipe(Object, ExpectedTypeCookie)) return false;
#endif
    FScopeLock Lock(&Private::LifecycleMutex);
    checkf(
        !Private::bDispatchInProgress,
        TEXT("DISPATCHING: cookie updated during lifecycle dispatch")
    );
    if (Private::bDispatchInProgress)
    {
        return false;
    }
    Private::FTrackedEntry* Entry =
        Private::Entries.Find(FObjectKey(Object));
    if (
        Entry == nullptr
        || Entry->State != Private::ETrackedState::Pending
        || Entry->TypeCookie != ExpectedTypeCookie
        || Entry->Generation != Private::CallbackGeneration
    )
    {
        return false;
    }
    Entry->TypeCookie = NewTypeCookie;
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS
bool ObserveRuntimeObjectTypeCookie(
    UObject* Object,
    void*& OutTypeCookie
)
{
    OutTypeCookie = nullptr;
    if (!IsInGameThread() || Object == nullptr)
    {
        return false;
    }

    FScopeLock Lock(&Private::LifecycleMutex);
    const Private::FTrackedEntry* Entry =
        Private::Entries.Find(FObjectKey(Object));
    if (
        Entry == nullptr
        || Entry->State != Private::ETrackedState::Pending
        || Entry->ObjectIdentity != static_cast<const UObjectBase*>(Object)
        || Entry->TypeCookie == nullptr
        || Entry->Generation == 0
        || Entry->Generation != Private::CallbackGeneration
        || Private::DestroyCallback == nullptr
        || Object->IsUnreachable()
        || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
    )
    {
        return false;
    }

    OutTypeCookie = Entry->TypeCookie;
    return true;
}
#endif

bool UntrackRuntimeObject(UObject* Object)
{
    checkf(IsInGameThread(), TEXT("untracking requires game thread"));
    Private::FailIfOffGameThread(TEXT("Runtime object untracking"));
    checkf(Object != nullptr, TEXT("tracked object missing"));
    if (Object == nullptr)
    {
        return false;
    }

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    if (!Private::CancelPendingTerminalRecipe(Object)) return false;
#endif
    FScopeLock Lock(&Private::LifecycleMutex);
    checkf(
        !Private::bDispatchInProgress,
        TEXT("DISPATCHING: object untracked during lifecycle dispatch")
    );
    if (Private::bDispatchInProgress)
    {
        return false;
    }
    const FObjectKey Key(Object);
    const Private::FTrackedEntry* Entry = Private::Entries.Find(Key);
    if (Entry == nullptr)
    {
        return false;
    }
    checkf(
        Entry->State == Private::ETrackedState::Pending,
        TEXT("DISPATCHING: object callback has not completed")
    );
    return Entry->State == Private::ETrackedState::Pending && Private::Entries.Remove(Key) == 1;
}

bool HasRuntimeReceiversPendingFinishDestroy(const UClass* Class)
{
    check(IsInGameThread());
    FScopeLock Lock(&Private::LifecycleMutex);
    for (const auto& Pair : Private::Entries)
    {
        const UObject* Receiver = Pair.Key.ResolveObjectPtrEvenIfUnreachable();
        if (Receiver && Receiver->GetClass()->IsChildOf(Class)) return true;
    }
    return false;
}

void CollectRuntimeOwnerReferences(UObject* Object, FReferenceCollector& Collector)
{
    FRuntimeOwnerReferenceCallback Callback = nullptr;
    void* TypeCookie = nullptr;
    {
        FScopeLock Lock(&Private::LifecycleMutex);
        const Private::FTrackedEntry* Entry = Object ? Private::Entries.Find(FObjectKey(Object)) : nullptr;
        if (Entry && Entry->ObjectIdentity == Object && Entry->Generation == Private::CallbackGeneration)
        {
            Callback = Private::OwnerReferenceCallback;
            TypeCookie = Entry->TypeCookie;
        }
    }
    if (Callback) Callback(Object, TypeCookie, Collector);
}

bool QuiesceAndUnregisterRuntimeDestroyCallback(uint64 Generation)
{
    checkf(IsInGameThread(), TEXT("quiesce requires game thread"));
    Private::FailIfOffGameThread(TEXT("Runtime callback quiesce"));

    FScopeLock Lock(&Private::LifecycleMutex);
    TMap<FObjectKey, Private::FTrackedEntry>& Entries = Private::Entries;
    int32& DispatchingCount = Private::DispatchingCount;
    uint64& CallbackGeneration = Private::CallbackGeneration;
    checkf(
        Entries.Num() == 0,
        TEXT("LIVE_ENTRIES: tracked objects remain during quiesce")
    );
    checkf(
        DispatchingCount == 0,
        TEXT("DISPATCHING: callbacks remain during quiesce")
    );
    checkf(
        !Private::bDispatchInProgress,
        TEXT("DISPATCHING: lifecycle dispatch remains active")
    );
    checkf(
        Generation == CallbackGeneration,
        TEXT("GENERATION_MISMATCH: stale callback generation")
    );
    checkf(
        Private::DestroyCallback != nullptr
            && (!SOLOAS_LEGACY_TERMINAL_LIFECYCLE || (Private::PostReachabilityHandle.IsValid()
            && Private::PreBeginDestroyHandle.IsValid())),
        TEXT("Runtime callback is not registered")
    );
    if (
        Entries.Num() != 0
        || DispatchingCount != 0
        || Private::bDispatchInProgress
        || Generation != CallbackGeneration
        || Private::DestroyCallback == nullptr
        || (SOLOAS_LEGACY_TERMINAL_LIFECYCLE && (!Private::PostReachabilityHandle.IsValid()
        || !Private::PreBeginDestroyHandle.IsValid()))
    )
    {
        return false;
    }

    FCoreUObjectDelegates::PostReachabilityAnalysis.Remove(
        Private::PostReachabilityHandle
    );
    Private::PostReachabilityHandle.Reset();
    FCoreUObjectDelegates::PreGarbageCollectConditionalBeginDestroy.Remove(
        Private::PreBeginDestroyHandle
    );
    Private::PreBeginDestroyHandle.Reset();
    Private::DestroyCallback = nullptr;
    Private::OwnerReferenceCallback = nullptr;
    Private::DestroyPrepare = nullptr;
    Private::DestroyControl = nullptr;
    Private::CallbackGeneration = 0;
    return true;
}
}

// WEAK_REFERENCES_MAY_BE_CLEARED before PostReachabilityAnalysis.
