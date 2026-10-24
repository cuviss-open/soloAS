// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreTypes.h"
#include "HAL/CriticalSection.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Containers/UnrealString.h"
#include "Templates/Function.h"
#include "UObject/ObjectKey.h"
#include "UObject/NameTypes.h"
#include "AngelscriptRuntimeGcMirrorAPI.h"

class FProperty;
class FReferenceCollector;
class FStructProperty;
class UObjectBase;
class FFieldClass;
class UObject;
class UClass;
class UASClass;
class asITypeInfo;
class asIScriptModule;

enum class EAngelscriptPropertyTrait : uint16;

DECLARE_LOG_CATEGORY_EXTERN(LogAngelscriptRuntimeGcLifecycle, Log, All);

namespace SoloASBridge
{
struct FRuntimeDestroyPreclaim;
struct FRuntimeDeferredDestroyToken;
}

namespace AngelscriptRuntimeGcMirror
{
enum class ERuntimeMirrorState : uint8
{
	Pending,
	TerminalPrepared,
	TerminalDeferred,
	Dispatching,
	Reconstructing,
	Untracking,
};

struct FRuntimeTrackedObject
{
	FObjectKey Key;
	const UObjectBase* ObjectIdentity = nullptr;
	void* SavedTypeCookie = nullptr;
	ERuntimeMirrorState State = ERuntimeMirrorState::Pending;
	uint64 TrackSequence = 0;
	FObjectKey SavedClassKey;
	bool bReplacementCaptureExcluded = false;
	asIScriptModule* RetainedModule = nullptr;
	uint64 TerminalPlanId = 0;
	bool bOrdinaryRemovedAdmissionRequired = false;
	uint64 OrdinaryRemovedAdmissionCollectionEpoch = 0;
	uint64 ClaimRevision = 0;
	uint64 ClaimCount = 0;
	uint64 CommittedRevision = 0;
	uint64 ClaimEpoch = 0;
	const void* ClaimOldCookie = nullptr;
	const void* ClaimExpectedCookie = nullptr;
	EScriptObjectReloadPlan ClaimKind = EScriptObjectReloadPlan::RequiresReplacement;
};

extern FCriticalSection RuntimeTrackedObjectsMutex;
extern TMap<FObjectKey, FRuntimeTrackedObject> RuntimeTrackedObjects;
extern bool bRuntimeLifecycleShuttingDown;
// Weak serial identities outlive untracking; only real script construction resets a claim.
extern TSet<FObjectKey> ExplicitlyDestroyedGarbageObjects;
bool ClaimRuntimeScriptDestructor(UObject* Object);
void ResetRuntimeScriptDestructor(UObject* Object);
void CollectReleasedScriptDestructorClaims();
bool UpdateRetainedRuntimeCookie(UObject* Object, asITypeInfo* ExpectedCookie, asITypeInfo* NewCookie,
    ERuntimeMirrorState ExpectedState, ERuntimeMirrorState NewState);
bool IsExactRuntimeReconstructionTypeCookie(UObject* Object, asITypeInfo* NewTypeCookie);
bool IsRuntimeReconstructionFaultEnabled(const TCHAR* FaultName);
bool IsRuntimeReconstructionFaultTarget(const UObject* Object);
bool ClaimRuntimeTrackedMirrorForSoftReload(UObject* Object, asITypeInfo* ExpectedCookie);
bool CommitRuntimeReconstructionRevision(UObject* Object, asITypeInfo* PreviousCookie, asITypeInfo* CurrentCookie);

bool IsRuntimeGcTraceEnabled();
// Terminal lookup uses the original tracked key only inside the private callback certificate.
FObjectKey ResolveRuntimeDestructorKey(UObject* Object);
bool MatchesRuntimeDestructorTracking(UObject* Object, const UObjectBase* Identity,
    void* SavedCookie, uint64 Sequence, asITypeInfo* ExpectedCookie, bool bAllowAnyCookie = false);
bool IsRuntimeTerminalDestructorActive();
bool PrepareRuntimeTerminalReconstruction(UObject* Object, asITypeInfo* ExpectedCookie);
bool IsRuntimeDestructorContextValid(UObject* Object, void* Cookie);
bool ObserveBridgeRuntimeCookieForMirror(UObject* Object, void*& OutCookie);
bool ReadRuntimeDestroyTracking(const SoloASBridge::FRuntimeDestroyPreclaim& Source, uint64& OutSequence,
    asIScriptModule** OutRetainedModule = nullptr);
bool IsRegisteredRuntimeTypeIdentity(const asITypeInfo* Type);
bool IsRetiredRuntimeTypeIdentity(const asITypeInfo* Type, const asIScriptModule* RetainedModule, const UASClass* Class);
void CancelRuntimeTerminalOrdinaryClaim(UObject* Object, asITypeInfo* ExpectedCookie = nullptr);
enum class ERuntimeTerminalMirrorTransition : uint8 { Prepare, Commit, Abort };
bool TransitionRuntimeTerminalTracking(const SoloASBridge::FRuntimeDeferredDestroyToken& Token,
    ERuntimeTerminalMirrorTransition Transition);
void CompleteRuntimeTerminalFieldDestroy(const FStructProperty& Property, void* ValueAddress);
#if WITH_DEV_AUTOMATION_TESTS
using FCrossActorMissingMapObserver = TFunction<void(const TMap<UObject*, UObject*>&, bool)>;
ANGELSCRIPTCODE_API bool ArmCrossActorMissingMapForTest(
    const void* const Identities[6], FCrossActorMissingMapObserver&& Observer);
ANGELSCRIPTCODE_API void ClearCrossActorMissingMapForTest();
#endif
/**
 * Owns only replacement-stable scalar data captured from an old script
 * object. In particular, this type must never retain an old UObject or
 * FProperty pointer: the callback runs after those objects may be retired.
 */
enum class EReinstancedScriptScalarKind : uint8
{
	Numeric,
	Bool,
	Enum,
};

enum class EReinstancedScriptClassReferenceKind : uint8
{
	Null,
	StableNativeExact,
	ReloadedScriptOldToNew,
	AlreadyExpectedNew,
};

/**
 * Replacement-stable schema and value identity for one admitted direct
 * runtime-generated FClassProperty. It never retains an old property or
 * replacement object pointer.
 */
struct FReinstancedScriptClassReferenceSnapshot
{
	FName PropertyName;
	const void* ExpectedNewOwnerIdentity = nullptr;
	const FFieldClass* PropertyClass = nullptr;
	EAngelscriptPropertyTrait PropertyTraits =
		static_cast<EAngelscriptPropertyTrait>(0);
	int32 Offset = INDEX_NONE;
	int32 Size = 0;
	int32 ElementSize = 0;
	int32 ArrayDim = 0;
	uint64 PropertyFlags = 0;
	FObjectKey MetaClassKey;
	const void* MetaClassIdentity = nullptr;
	EReinstancedScriptClassReferenceKind Kind =
		EReinstancedScriptClassReferenceKind::Null;
	FObjectKey ExpectedValueKey;
	const void* ExpectedValueIdentity = nullptr;
};

struct FReinstancedScriptScalarSnapshot
{
	FName PropertyName;
	const void* ExpectedNewOwnerIdentity = nullptr;
	const FFieldClass* PropertyClass = nullptr;
	EReinstancedScriptScalarKind Kind = EReinstancedScriptScalarKind::Numeric;
	EAngelscriptPropertyTrait PropertyTraits =
		static_cast<EAngelscriptPropertyTrait>(0);
	const FFieldClass* UnderlyingPropertyClass = nullptr;
	int32 Offset = INDEX_NONE;
	int32 Size = 0;
	int32 ElementSize = 0;
	int32 ArrayDim = 0;
	uint64 PropertyFlags = 0;
	TArray<uint8> PrimitiveBytes;
	bool bBoolValue = false;
	bool bBoolIsNativeBool = false;
	uint32 BoolFieldMask = 0;
	uint32 BoolByteOffset = 0;
	uint32 BoolFieldSize = 0;
	uint32 BoolByteMask = 0;
	FString EnumIdentity;
	FString EnumSignature;
};

/**
 * Explicit context for the pair-only finalization boundary. Ordinary
 * same-object reconstruction passes no context and therefore cannot consume
 * or fail a replacement-pair ledger record, including during re-entry.
 */
struct FReinstancedScriptPairFinalizationContext
{
	FObjectKey OldKey;
	FObjectKey NewKey;
	const void* OldObjectIdentity = nullptr;
	const void* NewObjectIdentity = nullptr;
	asITypeInfo* OldTypeCookie = nullptr;
	const void* OldTypeCookieUserData = nullptr;
	const void* OldClassIdentity = nullptr;
	const void* NewClassIdentity = nullptr;
	uint64 CaptureGeneration = 0;
	uint64 OldTrackSequence = 0;
	uint64 NewTrackSequence = 0;
	TArray<FReinstancedScriptScalarSnapshot> ScalarSnapshots;
	TArray<FReinstancedScriptClassReferenceSnapshot> ClassReferenceSnapshots;
};

bool TrackRuntimeObjectWithMirror(UObject* Object, asITypeInfo* TypeCookie);
void ReleasePublicDestroyReloadTracking(UObject* Object);
bool HasRuntimeObjectsPendingFinishDestroy();
void CollectRuntimeOwnerReferences(UObject* Object, void* TypeCookie, FReferenceCollector& Collector);
bool HasRuntimeObjectsPendingFinishDestroyForClass(const UASClass* Class);
bool TrackRuntimeObjectWithMirrorForLifecycle(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie
);
bool UntrackRuntimeObjectWithMirrorForLifecycle(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* ExpectedTypeCookie
);
bool UpdateRuntimeTrackedMirrorCookie(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	asITypeInfo* NewTypeCookie
);
/**
 * Finalizes state on an engine-constructed replacement after the map consumer
 * validates the completed initializer, exact type, pair, and pending mirror.
 * Does not end or restart the replacement object's script storage lifetime.
 * This is deliberately separate from UASClass's same-object cookie protocol.
 */
ANGELSCRIPTCODE_API void ReconstructReinstancedScriptClassPair(
	UObject* NewObject,
	UASClass* NewClass,
	asITypeInfo* NewTypeCookie,
	const FReinstancedScriptPairFinalizationContext& PairContext
);
ANGELSCRIPTCODE_API bool RestoreScriptRuntimeClassReferenceState(
	UObject* NewObject,
	UASClass* NewClass,
	asITypeInfo* NewTypeCookie,
	const TArray<FReinstancedScriptClassReferenceSnapshot>& Snapshots
);
void DestroyTrackedRuntimeObjectExplicitly(UObject* Object);
/**
 * Caller-owned lifecycle mirror evidence with an exact tracked cookie.
 * This is not transparent UClass, REINST, CDO, or archetype parity.
 */
bool DestroyTrackedRuntimeObjectExplicitlyForLifecycle(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* ExpectedTypeCookie
);
void DestroyRuntimeTrackedObjectsForClass(UClass* RemovedClass, bool bPreserveLiveObjects = false);
bool RuntimeDestroyObjectAdapter(
	UObject* Object,
	void* TypeCookie,
	uint64 Generation
);
void VerifyRuntimeGcLifecycleStartup();
void BeginRuntimeLifecycleShutdown();
void AbandonRuntimeTrackedObjects();
void VerifyRuntimeTrackedObjectsAbandoned();
void FailRuntimeMirror(const TCHAR* Sentinel, const UObject* Object);
}
