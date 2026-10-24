// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreTypes.h"
#include "Containers/Map.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "UObject/ObjectKey.h"

class UClass;
class UObject;
class asITypeInfo;

namespace AngelscriptRuntimeGcMirror
{
enum class EScriptObjectReloadPlan : uint8 { Unchanged, SoftReconstruction, RequiresReplacement };

struct FScriptObjectReloadPlan
{
	UClass* Class = nullptr;
	FObjectKey ClassKey;
	const void* PreviousCookie = nullptr;
	asITypeInfo* ExpectedCookie = nullptr;
	EScriptObjectReloadPlan Kind = EScriptObjectReloadPlan::Unchanged;
};

struct FLiveScriptReferenceWitness
{
	FObjectKey ObjectKey;
	FObjectKey ClassKey;
	const void* ObjectIdentity = nullptr;
	TArray<FObjectKey> OuterKeys;
	const void* PreviousCookie = nullptr;
	const void* ExpectedCookie = nullptr;
	uint64 TrackSequence = 0;
	uint64 Revision = 0;
	uint64 CommittedRevision = 0;
	uint64 ClaimCount = 0;
	uint64 Epoch = 0;
	EScriptObjectReloadPlan Kind = EScriptObjectReloadPlan::Unchanged;
};

ANGELSCRIPTCODE_API bool CaptureLiveScriptReferenceWitness(UObject* Object,
	const FScriptObjectReloadPlan& Plan, uint64 Epoch, FLiveScriptReferenceWitness& Out, FString& Error);
ANGELSCRIPTCODE_API bool ValidatePreparedScriptReferenceWitness(UObject* Object,
	const FLiveScriptReferenceWitness& Witness, FString& Error);
ANGELSCRIPTCODE_API bool ValidateLiveScriptReferenceWitness(UObject* Object,
	const FLiveScriptReferenceWitness& Witness, FString& Error, FLiveScriptReferenceWitness* Current = nullptr);

struct FReinstancedScriptPairFinalizationContext;

/** Immutable admission identity copied from the active replacement ledger. */
struct FRuntimeScriptReplacementWitness
{
	FObjectKey OldKey;
	const void* OldIdentity = nullptr;
	const void* ExpectedNewClassIdentity = nullptr;
	uint64 CaptureGeneration = 0;
};

/**
 * Callback-local replay token. It is captured while the engine replacement
 * objects are live and is intentionally independent of UObject pointers.
 */
struct FReinstancedScriptPairReplayToken
{
	FObjectKey OldKey;
	FObjectKey NewKey;
	uint64 CaptureGeneration = 0;
	uint64 NewTrackSequence = 0;
};

enum class EReinstancedScriptPairReplayResult : uint8
{
	Missing,
	Conflict,
	Reserved,
	Consumed,
	Failed,
};

ANGELSCRIPTCODE_API bool ClaimRuntimeTrackedMirrorForReconstruction(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie
);
ANGELSCRIPTCODE_API bool CommitRuntimeTrackedObjectReconstruction(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	asITypeInfo* NewTypeCookie,
	const FReinstancedScriptPairFinalizationContext* PairContext = nullptr
);
/**
 * Consumes the engine-owned old/new replacement map only while the editor
 * reload adapter has an active event window. This validates a paired
 * UASClass/script-object tuple; it does not invoke same-object reconstruction.
 */
ANGELSCRIPTCODE_API void ConsumeEngineReinstancedScriptClassPairs(
	const TMap<UObject*, UObject*>& ReinstancedObjects
);
ANGELSCRIPTCODE_API bool BeginRuntimeScriptReplacementCapture(
	const TMap<UClass*, UClass*>& ReloadClasses
);
ANGELSCRIPTCODE_API bool BeginRuntimeScriptReplacementCaptureWithWitnesses(
	const TMap<UClass*, UClass*>& ReloadClasses,
	TArray<FRuntimeScriptReplacementWitness>* OutWitnesses
);
ANGELSCRIPTCODE_API void EndRuntimeScriptReplacementCapture();
/** Copies the token captured for a currently live replacement object. */
ANGELSCRIPTCODE_API bool GetReinstancedScriptPairReplayToken(
	UObject* LiveNewObject,
	FReinstancedScriptPairReplayToken& OutToken
);
/**
 * Replays only the generation-safe ledger decision. This function never
 * resolves, dereferences, validates, or constructs a key from a UObject.
 */
ANGELSCRIPTCODE_API EReinstancedScriptPairReplayResult ReplayReinstancedScriptPairToken(
	const FReinstancedScriptPairReplayToken& Token
);
#if WITH_DEV_AUTOMATION_TESTS
ANGELSCRIPTCODE_API bool ObserveRuntimeTrackedMirrorCookie(
	UObject* Object,
	asITypeInfo*& OutTypeCookie
);
#endif
}
