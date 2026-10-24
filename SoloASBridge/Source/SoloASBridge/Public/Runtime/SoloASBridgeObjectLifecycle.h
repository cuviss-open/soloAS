#pragma once

#ifndef SOLOAS_LEGACY_TERMINAL_LIFECYCLE
#define SOLOAS_LEGACY_TERMINAL_LIFECYCLE 0
#endif

#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"

class UObject;
class UClass;
class FReferenceCollector;

namespace SoloASBridge
{
using FRuntimeDestroyCallback = bool (*)(UObject*, void*, uint64);
using FRuntimeOwnerReferenceCallback = void (*)(UObject*, void*, FReferenceCollector&);

struct FRuntimeDestroyPreclaim
{
    FObjectKey Key;
    UObject* Object = nullptr;
    void* TypeCookie = nullptr;
    uint64 Generation = 0;
    uint64 ClaimSequence = 0;
};

struct FRuntimeDeferredDestroyToken
{
    FObjectKey Key;
    UObject* Object = nullptr;
    void* TypeCookie = nullptr;
    uint64 TrackSequence = 0, ArmEpoch = 0, Generation = 0;
    uint64 ClaimSequence = 0, PlanId = 0;
};

enum class ERuntimeDestroyPrepareResult : uint8
{
    NotApplicable,
    Prepared,
    InvalidIdentity,
};

enum class ERuntimeDestroyControl : uint8
{
    CommitPrepared,
    AbortPrepared,
    ValidateForCallback,
};

using FRuntimeDestroyPrepare = ERuntimeDestroyPrepareResult (*)(
    const FRuntimeDestroyPreclaim&, FRuntimeDeferredDestroyToken&);
using FRuntimeDestroyControlCallback = bool (*)(
    const FRuntimeDeferredDestroyToken&, ERuntimeDestroyControl);

SOLOASBRIDGE_API bool CompleteRuntimeDeferredDestroy(const FRuntimeDeferredDestroyToken& Token);
// Immutable certificate owned by the registered callback's stack frame in ordinary GC or ExitPurge.
SOLOASBRIDGE_API const FRuntimeDeferredDestroyToken* GetActiveRuntimeDeferredDestroyToken();

SOLOASBRIDGE_API bool IsExitPurgeRuntimeDestroyCallback();
SOLOASBRIDGE_API bool HasRuntimeReceiversPendingFinishDestroy(const UClass* Class);
SOLOASBRIDGE_API void CollectRuntimeOwnerReferences(UObject* Object, FReferenceCollector& Collector);

SOLOASBRIDGE_API bool RegisterRuntimeDestroyCallback(
    FRuntimeDestroyCallback Callback,
    uint64 Generation,
    FRuntimeDestroyPrepare Prepare = nullptr,
    FRuntimeDestroyControlCallback Control = nullptr,
    FRuntimeOwnerReferenceCallback OwnerReferences = nullptr
);

SOLOASBRIDGE_API bool TrackRuntimeObject(
    UObject* Object,
    void* TypeCookie
);

SOLOASBRIDGE_API bool UpdateRuntimeObjectTypeCookie(
    UObject* Object,
    void* ExpectedTypeCookie,
    void* NewTypeCookie
);

#if WITH_DEV_AUTOMATION_TESTS
SOLOASBRIDGE_API bool ObserveRuntimeObjectTypeCookie(
    UObject* Object,
    void*& OutTypeCookie
);
#endif

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
using FRuntimeTerminalDestroyTokenForTest = FRuntimeDeferredDestroyToken;
enum class ERuntimeTerminalDestroyPhaseForTest : uint8
{
    BeforeClaim, AfterOriginalFieldDestroy, CancelPending,
};
using FRuntimeTerminalDestroyValidatorForTest = bool (*)(
    const FRuntimeTerminalDestroyTokenForTest&, ERuntimeTerminalDestroyPhaseForTest Phase);
SOLOASBRIDGE_API bool ArmRuntimeTerminalDestroyForTest(UObject* Object, void* TypeCookie,
    uint64 TrackSequence, uint64 ArmEpoch, FRuntimeTerminalDestroyValidatorForTest Validator,
    FRuntimeTerminalDestroyTokenForTest& OutToken);
SOLOASBRIDGE_API void DisarmRuntimeTerminalDestroyForTest(const FRuntimeTerminalDestroyTokenForTest& Token);
SOLOASBRIDGE_API bool CompleteRuntimeTerminalDestroyForTest(const FRuntimeTerminalDestroyTokenForTest& Token);
SOLOASBRIDGE_API bool IsTerminalRuntimeDestroyCallbackForTest();
SOLOASBRIDGE_API bool IsTerminalRuntimeDestroyReceiverForTest(const UObject* Object);
#endif

SOLOASBRIDGE_API bool UntrackRuntimeObject(UObject* Object);

SOLOASBRIDGE_API bool QuiesceAndUnregisterRuntimeDestroyCallback(
    uint64 Generation
);
}
