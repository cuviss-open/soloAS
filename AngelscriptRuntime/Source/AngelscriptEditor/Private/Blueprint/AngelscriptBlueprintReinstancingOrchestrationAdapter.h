// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"

class UBlueprint;
class UClass;

enum class EAngelscriptBlueprintReinstancingOrchestrationResult : uint8
{
	Completed,
	NullSession,
	NullBlueprint,
	InvalidBlueprint,
	MissingGeneratedClass,
	EmptyReplacementMap,
	NullReplacementClass,
	InvalidReplacementClass,
	SameReplacementClass,
	DuplicateReplacementTarget,
	CyclicReplacementMap,
	InvalidReplacementMap,
	Reentrant,
	OutputSessionNotEmpty,
	WrongThread,
	InvalidPhase,
};

enum class EAngelscriptBlueprintReinstancingOrchestrationPhase : uint8
{
	Validated,
	CompilationQueued,
	HierarchiesReparented,
	InstancesReplaced,
	Completed,
};

enum class EAngelscriptBlueprintReinstancingTracePhase : uint8
{
	SessionValidated,
	PrivateDefaultCopyOrderingUnavailable,
	PrivateNotificationOrderingUnavailable,
	PrivateAttachmentOrderingUnavailable,
	CompilationCallReturned,
	HierarchyReparentCallReturned,
	PublicReferenceReplacementCallReturned,
	InstanceReplacementCallReturned,
	PublicFinalizeCallReturned,
	SessionCompleted,
};

enum class EAngelscriptBlueprintReinstancingCapability : uint8
{
	PublicCompilation,
	PublicHierarchyReparenting,
	PublicReferenceReplacement,
	PublicInstanceReplacement,
	PrivateDefaultCopyOrdering,
	PrivateNotificationOrdering,
	PrivateAttachmentOrdering,
};

struct FAngelscriptBlueprintReinstancingOrchestrationRequest
{
	TArray<UBlueprint*> Blueprints;
	TMap<UClass*, UClass*> ClassReplacements;
	bool bReplaceReferencesToOldClasses = true;
	bool bReplaceReferencesToOldCDOs = false;
	bool bFindAndReplaceCDOReferences = false;
};

/**
 * Caller-owned sequencing over stock/public Blueprint compilation and reinstancing APIs.
 *
 * The caller must retain the returned session and invoke the phases in order:
 * QueueCompilation, ReparentClassHierarchies, ReplaceInstances, then FlushCompilation.
 * Destroying an incomplete session releases adapter ownership only; it does not roll back
 * engine compilation or reinstancing work and does not invoke a hidden or automatic phase.
 * All operations must be called on the game thread; off-thread calls fail with WrongThread.
 *
 * This contract does not claim engine-wide reentrancy protection, transparent Haze private transaction phases, or an
 * timing, CDO/template/schema/bytecode/attachment ordering, or automatic stock reinstancing.
 */
class ANGELSCRIPTEDITOR_API FAngelscriptBlueprintReinstancingOrchestrationAdapter final
{
public:
	static EAngelscriptBlueprintReinstancingOrchestrationResult Begin(
		const FAngelscriptBlueprintReinstancingOrchestrationRequest& Request,
		TUniquePtr<FAngelscriptBlueprintReinstancingOrchestrationAdapter>& OutSession);

	~FAngelscriptBlueprintReinstancingOrchestrationAdapter();

	FAngelscriptBlueprintReinstancingOrchestrationAdapter(
		const FAngelscriptBlueprintReinstancingOrchestrationAdapter&) = delete;
	FAngelscriptBlueprintReinstancingOrchestrationAdapter& operator=(
		const FAngelscriptBlueprintReinstancingOrchestrationAdapter&) = delete;

	EAngelscriptBlueprintReinstancingOrchestrationResult QueueCompilation();
	EAngelscriptBlueprintReinstancingOrchestrationResult ReparentClassHierarchies();
	EAngelscriptBlueprintReinstancingOrchestrationResult ReplaceInstances();
	EAngelscriptBlueprintReinstancingOrchestrationResult FlushCompilation();

	EAngelscriptBlueprintReinstancingOrchestrationPhase GetPhase() const
	{
		return Phase;
	}

	const TArray<EAngelscriptBlueprintReinstancingTracePhase>& GetTrace() const
	{
		return Trace;
	}

	static bool HasCapability(EAngelscriptBlueprintReinstancingCapability Capability);

private:
	explicit FAngelscriptBlueprintReinstancingOrchestrationAdapter(
		const FAngelscriptBlueprintReinstancingOrchestrationRequest& InRequest);

	static EAngelscriptBlueprintReinstancingOrchestrationResult ValidateRequest(
		const FAngelscriptBlueprintReinstancingOrchestrationRequest& Request);
	static EAngelscriptBlueprintReinstancingOrchestrationResult ValidateReplacementMap(
		const TMap<UClass*, UClass*>& ClassReplacements);
	EAngelscriptBlueprintReinstancingOrchestrationResult ValidateLiveRequest() const;
	bool BeginOperation();
	void EndOperation();

	FAngelscriptBlueprintReinstancingOrchestrationRequest Request;
	EAngelscriptBlueprintReinstancingOrchestrationPhase Phase =
		EAngelscriptBlueprintReinstancingOrchestrationPhase::Validated;
	TArray<EAngelscriptBlueprintReinstancingTracePhase> Trace;
	bool bSessionRegistered = false;
	bool bOperationActive = false;
};
