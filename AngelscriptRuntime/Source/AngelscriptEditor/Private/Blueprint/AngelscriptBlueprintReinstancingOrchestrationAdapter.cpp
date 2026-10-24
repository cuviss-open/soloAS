// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Blueprint/AngelscriptBlueprintReinstancingOrchestrationAdapter.h"

#include "BlueprintCompilationManager.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetReinstanceUtilities.h"
#include "UObject/Class.h"

namespace
{
bool GBlueprintReinstancingSessionActive = false;
bool GBlueprintReinstancingOperationActive = false;
}

FAngelscriptBlueprintReinstancingOrchestrationAdapter::
	FAngelscriptBlueprintReinstancingOrchestrationAdapter(
		const FAngelscriptBlueprintReinstancingOrchestrationRequest& InRequest)
	: Request(InRequest)
	, bSessionRegistered(true)
{
}

FAngelscriptBlueprintReinstancingOrchestrationAdapter::~FAngelscriptBlueprintReinstancingOrchestrationAdapter()
{
	if (bSessionRegistered)
	{
		GBlueprintReinstancingSessionActive = false;
	}
}

EAngelscriptBlueprintReinstancingOrchestrationResult
FAngelscriptBlueprintReinstancingOrchestrationAdapter::ValidateReplacementMap(
	const TMap<UClass*, UClass*>& ClassReplacements)
{
	if (ClassReplacements.IsEmpty())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::EmptyReplacementMap;
	}

	TSet<UClass*> ReplacementTargets;
	for (const TPair<UClass*, UClass*>& Replacement : ClassReplacements)
	{
		UClass* OldClass = Replacement.Key;
		UClass* NewClass = Replacement.Value;
		if (OldClass == nullptr || NewClass == nullptr)
		{
			return EAngelscriptBlueprintReinstancingOrchestrationResult::NullReplacementClass;
		}
		if (!IsValid(OldClass) || !IsValid(NewClass))
		{
			return EAngelscriptBlueprintReinstancingOrchestrationResult::InvalidReplacementClass;
		}
		if (OldClass == NewClass)
		{
			return EAngelscriptBlueprintReinstancingOrchestrationResult::SameReplacementClass;
		}
		if (ReplacementTargets.Contains(NewClass))
		{
			return EAngelscriptBlueprintReinstancingOrchestrationResult::DuplicateReplacementTarget;
		}
		ReplacementTargets.Add(NewClass);
		if (ClassReplacements.Contains(NewClass))
		{
			return EAngelscriptBlueprintReinstancingOrchestrationResult::CyclicReplacementMap;
		}
	}

	return EAngelscriptBlueprintReinstancingOrchestrationResult::Completed;
}

EAngelscriptBlueprintReinstancingOrchestrationResult
FAngelscriptBlueprintReinstancingOrchestrationAdapter::ValidateRequest(
	const FAngelscriptBlueprintReinstancingOrchestrationRequest& InRequest)
{
	if (InRequest.Blueprints.IsEmpty())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::NullBlueprint;
	}

	TSet<UBlueprint*> SeenBlueprints;
	for (UBlueprint* Blueprint : InRequest.Blueprints)
	{
		if (Blueprint == nullptr)
		{
			return EAngelscriptBlueprintReinstancingOrchestrationResult::NullBlueprint;
		}
		if (!IsValid(Blueprint))
		{
			return EAngelscriptBlueprintReinstancingOrchestrationResult::InvalidBlueprint;
		}
		if (Blueprint->GeneratedClass == nullptr)
		{
			return EAngelscriptBlueprintReinstancingOrchestrationResult::MissingGeneratedClass;
		}
		if (SeenBlueprints.Contains(Blueprint))
		{
			return EAngelscriptBlueprintReinstancingOrchestrationResult::InvalidBlueprint;
		}
		SeenBlueprints.Add(Blueprint);
	}

	const EAngelscriptBlueprintReinstancingOrchestrationResult MapResult =
		ValidateReplacementMap(InRequest.ClassReplacements);
	if (MapResult != EAngelscriptBlueprintReinstancingOrchestrationResult::Completed)
	{
		return MapResult;
	}
	return EAngelscriptBlueprintReinstancingOrchestrationResult::Completed;
}

EAngelscriptBlueprintReinstancingOrchestrationResult
FAngelscriptBlueprintReinstancingOrchestrationAdapter::Begin(
	const FAngelscriptBlueprintReinstancingOrchestrationRequest& InRequest,
	TUniquePtr<FAngelscriptBlueprintReinstancingOrchestrationAdapter>& OutSession)
{
	if (!IsInGameThread())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::WrongThread;
	}
	if (OutSession.IsValid())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::OutputSessionNotEmpty;
	}
	if (GBlueprintReinstancingSessionActive || GBlueprintReinstancingOperationActive)
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::Reentrant;
	}

	const EAngelscriptBlueprintReinstancingOrchestrationResult ValidationResult =
		ValidateRequest(InRequest);
	if (ValidationResult != EAngelscriptBlueprintReinstancingOrchestrationResult::Completed)
	{
		return ValidationResult;
	}

	GBlueprintReinstancingSessionActive = true;
	OutSession = TUniquePtr<FAngelscriptBlueprintReinstancingOrchestrationAdapter>(
		new FAngelscriptBlueprintReinstancingOrchestrationAdapter(InRequest));
	OutSession->Trace.Add(EAngelscriptBlueprintReinstancingTracePhase::SessionValidated);
	OutSession->Trace.Add(
		EAngelscriptBlueprintReinstancingTracePhase::PrivateDefaultCopyOrderingUnavailable);
	OutSession->Trace.Add(
		EAngelscriptBlueprintReinstancingTracePhase::PrivateNotificationOrderingUnavailable);
	OutSession->Trace.Add(
		EAngelscriptBlueprintReinstancingTracePhase::PrivateAttachmentOrderingUnavailable);
	return EAngelscriptBlueprintReinstancingOrchestrationResult::Completed;
}

EAngelscriptBlueprintReinstancingOrchestrationResult
FAngelscriptBlueprintReinstancingOrchestrationAdapter::ValidateLiveRequest() const
{
	return ValidateRequest(Request);
}

bool FAngelscriptBlueprintReinstancingOrchestrationAdapter::BeginOperation()
{
	if (!bSessionRegistered || GBlueprintReinstancingSessionActive == false)
	{
		return false;
	}
	if (bOperationActive || GBlueprintReinstancingOperationActive)
	{
		return false;
	}
	bOperationActive = true;
	GBlueprintReinstancingOperationActive = true;
	return true;
}

void FAngelscriptBlueprintReinstancingOrchestrationAdapter::EndOperation()
{
	bOperationActive = false;
	GBlueprintReinstancingOperationActive = false;
}

bool FAngelscriptBlueprintReinstancingOrchestrationAdapter::HasCapability(
	EAngelscriptBlueprintReinstancingCapability Capability)
{
	switch (Capability)
	{
	case EAngelscriptBlueprintReinstancingCapability::PublicCompilation:
	case EAngelscriptBlueprintReinstancingCapability::PublicHierarchyReparenting:
	case EAngelscriptBlueprintReinstancingCapability::PublicReferenceReplacement:
	case EAngelscriptBlueprintReinstancingCapability::PublicInstanceReplacement:
		return true;
	case EAngelscriptBlueprintReinstancingCapability::PrivateDefaultCopyOrdering:
	case EAngelscriptBlueprintReinstancingCapability::PrivateNotificationOrdering:
	case EAngelscriptBlueprintReinstancingCapability::PrivateAttachmentOrdering:
		return false;
	default:
		return false;
	}
}

EAngelscriptBlueprintReinstancingOrchestrationResult
FAngelscriptBlueprintReinstancingOrchestrationAdapter::QueueCompilation()
{
	if (!IsInGameThread())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::WrongThread;
	}
	if (Phase != EAngelscriptBlueprintReinstancingOrchestrationPhase::Validated)
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::InvalidPhase;
	}
	const EAngelscriptBlueprintReinstancingOrchestrationResult ValidationResult =
		ValidateLiveRequest();
	if (ValidationResult != EAngelscriptBlueprintReinstancingOrchestrationResult::Completed)
	{
		return ValidationResult;
	}
	if (!BeginOperation())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::Reentrant;
	}
	for (UBlueprint* Blueprint : Request.Blueprints)
	{
		FBlueprintCompilationManager::QueueForCompilation(Blueprint);
	}
	EndOperation();
	Phase = EAngelscriptBlueprintReinstancingOrchestrationPhase::CompilationQueued;
	Trace.Add(EAngelscriptBlueprintReinstancingTracePhase::CompilationCallReturned);
	return EAngelscriptBlueprintReinstancingOrchestrationResult::Completed;
}

EAngelscriptBlueprintReinstancingOrchestrationResult
FAngelscriptBlueprintReinstancingOrchestrationAdapter::ReparentClassHierarchies()
{
	if (!IsInGameThread())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::WrongThread;
	}
	if (Phase != EAngelscriptBlueprintReinstancingOrchestrationPhase::CompilationQueued)
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::InvalidPhase;
	}
	if (ValidateReplacementMap(Request.ClassReplacements) !=
		EAngelscriptBlueprintReinstancingOrchestrationResult::Completed)
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::InvalidReplacementMap;
	}
	if (!BeginOperation())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::Reentrant;
	}
	FBlueprintCompilationManager::ReparentHierarchies(Request.ClassReplacements);
	EndOperation();
	Phase = EAngelscriptBlueprintReinstancingOrchestrationPhase::HierarchiesReparented;
	Trace.Add(EAngelscriptBlueprintReinstancingTracePhase::HierarchyReparentCallReturned);
	return EAngelscriptBlueprintReinstancingOrchestrationResult::Completed;
}

EAngelscriptBlueprintReinstancingOrchestrationResult
FAngelscriptBlueprintReinstancingOrchestrationAdapter::ReplaceInstances()
{
	if (!IsInGameThread())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::WrongThread;
	}
	if (Phase != EAngelscriptBlueprintReinstancingOrchestrationPhase::HierarchiesReparented)
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::InvalidPhase;
	}
	if (ValidateReplacementMap(Request.ClassReplacements) !=
		EAngelscriptBlueprintReinstancingOrchestrationResult::Completed)
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::InvalidReplacementMap;
	}
	if (!BeginOperation())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::Reentrant;
	}
	FReplaceInstancesOfClassParameters Parameters;
	Parameters.bReplaceReferencesToOldClasses = Request.bReplaceReferencesToOldClasses;
	Parameters.bReplaceReferencesToOldCDOs = Request.bReplaceReferencesToOldCDOs;
	FBlueprintCompileReinstancer::BatchReplaceInstancesOfClass(
		Request.ClassReplacements, Parameters);
	EndOperation();
	if (Request.bReplaceReferencesToOldClasses)
	{
		Trace.Add(EAngelscriptBlueprintReinstancingTracePhase::PublicReferenceReplacementCallReturned);
	}
	Phase = EAngelscriptBlueprintReinstancingOrchestrationPhase::InstancesReplaced;
	Trace.Add(EAngelscriptBlueprintReinstancingTracePhase::InstanceReplacementCallReturned);
	return EAngelscriptBlueprintReinstancingOrchestrationResult::Completed;
}

EAngelscriptBlueprintReinstancingOrchestrationResult
FAngelscriptBlueprintReinstancingOrchestrationAdapter::FlushCompilation()
{
	if (!IsInGameThread())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::WrongThread;
	}
	if (Phase != EAngelscriptBlueprintReinstancingOrchestrationPhase::InstancesReplaced)
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::InvalidPhase;
	}
	if (!BeginOperation())
	{
		return EAngelscriptBlueprintReinstancingOrchestrationResult::Reentrant;
	}
	FBlueprintCompilationManager::FlushCompilationQueueAndReinstance(
		Request.bFindAndReplaceCDOReferences);
	EndOperation();
	Trace.Add(EAngelscriptBlueprintReinstancingTracePhase::PublicFinalizeCallReturned);
	Phase = EAngelscriptBlueprintReinstancingOrchestrationPhase::Completed;
	Trace.Add(EAngelscriptBlueprintReinstancingTracePhase::SessionCompleted);
	return EAngelscriptBlueprintReinstancingOrchestrationResult::Completed;
}
