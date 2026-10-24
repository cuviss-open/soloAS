// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "AngelscriptRuntimeGcMirrorAPI.h"

class UScriptStruct;
class UClass;
struct FAngelscriptClassDesc;
struct FAngelscriptModuleDesc;
struct FAngelscriptEnumDesc;
class UEnum;
class asITypeInfo;

struct ANGELSCRIPTCODE_API FAngelscriptPayloadReloadType
{
	UScriptStruct* Previous = nullptr;
	UClass* PreviousObjectClass = nullptr;
	TSharedPtr<FAngelscriptClassDesc> Candidate;
	bool bWillReplace = false;
	bool bWillReplaceObjectClass = false;
};

DECLARE_DELEGATE_RetVal_TwoParams(
	bool,
	FAngelscriptPayloadReloadAdmission,
	const TArray<FAngelscriptPayloadReloadType>&,
	FString&
);

namespace AngelscriptReloadSafety
{
    // Call on the game thread before reload side effects; retain requests until loading finishes.
    ANGELSCRIPTCODE_API bool ShouldDeferForPackageLoading();
    class IPreparedPayloadReloadLease;
    // Synchronous admission input; the Editor lease owns any retained copy and pins.
    struct FPreparedScriptEnumReloadPlan
    {
        TSharedPtr<FAngelscriptModuleDesc> PreviousModule;
        TSharedPtr<FAngelscriptModuleDesc> CandidateModule;
        TSharedPtr<FAngelscriptEnumDesc> Previous;
        TSharedPtr<FAngelscriptEnumDesc> Candidate;
        asITypeInfo* PreviousCookie = nullptr;
        asITypeInfo* CandidateCookie = nullptr;
        bool bOwnerRecompiled = false;
        bool bEligibleFixedDefinition = false;
    };

    enum class EPreparedScriptEnumReloadPhase : uint8 { Begin, Commit };
    struct FPreparedScriptEnumReloadTransition
    {
        uint64 Epoch = 0;
        const FAngelscriptModuleDesc* PreviousModule = nullptr;
        const FAngelscriptModuleDesc* CandidateModule = nullptr;
        const FAngelscriptEnumDesc* Previous = nullptr;
        const FAngelscriptEnumDesc* Candidate = nullptr;
        UEnum* ExistingEnum = nullptr;
        bool bDefinitionSetter = false;
        EPreparedScriptEnumReloadPhase Phase = EPreparedScriptEnumReloadPhase::Begin;
    };
    DECLARE_DELEGATE_RetVal_TwoParams(bool, FPreparedScriptEnumReloadTransitionHandler,
        const FPreparedScriptEnumReloadTransition&, FString&);

    enum class EPreparedScriptEnumWitnessPhase : uint8 { Unchanged, Prepared, Claimed, Committed };
    struct FPreparedScriptEnumReloadWitness
    {
        uint64 Epoch = 0;
        FObjectKey EnumIdentity;
        const void* PreviousCookieKey = nullptr;
        const void* CandidateCookieKey = nullptr;
        const void* PreviousModuleKey = nullptr;
        const void* CandidateModuleKey = nullptr;
        bool bOwnerRecompiled = false;
        uint64 ClaimRevision = 0;
        uint64 CommitRevision = 0;
        EPreparedScriptEnumWitnessPhase Phase = EPreparedScriptEnumWitnessPhase::Prepared;
    };
    DECLARE_DELEGATE_RetVal_TwoParams(bool, FPreparedScriptEnumReloadWitnessReader,
        const UEnum*, FPreparedScriptEnumReloadWitness&);

    ANGELSCRIPTCODE_API bool ExecutePayloadAdmissionWithScriptEnums(const TArray<FAngelscriptPayloadReloadType>& Types,
        const TArray<FPreparedScriptEnumReloadPlan>& EnumPlans, FString& Error);
    ANGELSCRIPTCODE_API const TArray<FPreparedScriptEnumReloadPlan>* GetPreparedScriptEnumReloadPlans();
    ANGELSCRIPTCODE_API FPreparedScriptEnumReloadTransitionHandler& GetPreparedScriptEnumReloadTransition();
    ANGELSCRIPTCODE_API FPreparedScriptEnumReloadWitnessReader& GetPreparedScriptEnumReloadWitnessReader();
    ANGELSCRIPTCODE_API bool GetPreparedScriptEnumReloadWitness(const UEnum* Enum, FPreparedScriptEnumReloadWitness& Out);
    // One-shot registration inside the original synchronous admission, after it installs its lease.
    ANGELSCRIPTCODE_API bool SealPreparedScriptEnumReloadWitnessProvider(
        IPreparedPayloadReloadLease* Lease, const void* Owner, FString& Error);
    ANGELSCRIPTCODE_API void NotifyPreparedScriptEnumReload(const FPreparedScriptEnumReloadTransition& Transition);

	class ANGELSCRIPTCODE_API IPreparedPayloadReloadLease
	{
	public:
		virtual ~IPreparedPayloadReloadLease() = default;
		virtual bool ValidateBeforeSwap(FString& Error) = 0;
		virtual bool Attach(const TMap<UScriptStruct*, UScriptStruct*>& Structs,
			const TMap<UClass*, UClass*>& Classes,
			const TArray<AngelscriptRuntimeGcMirror::FRuntimeScriptReplacementWitness>& Witnesses, FString& Error) = 0;
		virtual bool Publish(FString& Error) = 0;
		virtual bool ValidateCompletion(FString& Error) = 0;
	};

	class ANGELSCRIPTCODE_API FPreparedPayloadReloadScope
	{
	public:
		FPreparedPayloadReloadScope();
		~FPreparedPayloadReloadScope();
		FPreparedPayloadReloadScope(const FPreparedPayloadReloadScope&) = delete;
		FPreparedPayloadReloadScope& operator=(const FPreparedPayloadReloadScope&) = delete;
		uint64 Epoch = 0;
		bool bCommitted = false;
		TArray<AngelscriptRuntimeGcMirror::FScriptObjectReloadPlan> Plans;
		TSharedPtr<IPreparedPayloadReloadLease> Lease;
	};
	ANGELSCRIPTCODE_API FPreparedPayloadReloadScope* GetPreparedPayloadReloadScope();
	ANGELSCRIPTCODE_API const AngelscriptRuntimeGcMirror::FScriptObjectReloadPlan* FindScriptObjectReloadPlan(UClass* Class);

	ANGELSCRIPTCODE_API FAngelscriptPayloadReloadAdmission&
	GetPayloadReloadAdmission();

	class ANGELSCRIPTCODE_API FCommittedReloadScope
	{
	public:
		FCommittedReloadScope();
		~FCommittedReloadScope() noexcept;

		FCommittedReloadScope(const FCommittedReloadScope&) = delete;
		FCommittedReloadScope& operator=(const FCommittedReloadScope&) = delete;
		FCommittedReloadScope(FCommittedReloadScope&&) = delete;
		FCommittedReloadScope& operator=(FCommittedReloadScope&&) = delete;
	};

	ANGELSCRIPTCODE_API bool IsCommittedReloadActive();

	[[noreturn]] ANGELSCRIPTCODE_API void StopCommittedReload(
		const TCHAR* Phase,
		const TCHAR* Reason
	);

	ANGELSCRIPTCODE_API void CheckScriptExecution(
		int32 Result,
		const TCHAR* Operation
	);
}
