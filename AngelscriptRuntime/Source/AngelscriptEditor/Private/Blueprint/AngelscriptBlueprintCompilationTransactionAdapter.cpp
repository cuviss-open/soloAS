// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Blueprint/AngelscriptBlueprintCompilationTransactionAdapter.h"

#include "BlueprintCompilationManager.h"
#include "Engine/Blueprint.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/Object.h"

namespace
{
thread_local bool GAngelscriptBlueprintCompilationTransactionInProgress = false;

using ECompilationTransactionResult = EAngelscriptBlueprintCompilationTransactionResult;
using EState = EAngelscriptBlueprintCompilationTransactionState;

ECompilationTransactionResult ValidateRequest(
	const FAngelscriptBlueprintCompilationTransactionRequest& Request,
	EState ExpectedState)
{
	if (!IsInGameThread())
		return ECompilationTransactionResult::WrongThread;
	if (Request.Blueprint == nullptr)
		return ECompilationTransactionResult::NullBlueprint;
	if (Request.State != ExpectedState)
		return ECompilationTransactionResult::InvalidState;
	if (GAngelscriptBlueprintCompilationTransactionInProgress
		|| Request.bInProgress)
		return ECompilationTransactionResult::ReentrantExecution;
	if (!IsValid(Request.Blueprint))
		return ECompilationTransactionResult::InvalidBlueprint;
	if (Request.Blueprint->Status == BS_BeingCreated
		|| Request.Blueprint->bBeingCompiled
		|| Request.Blueprint->HasAnyFlags(
			RF_BeginDestroyed | RF_FinishDestroyed))
		return ECompilationTransactionResult::InvalidState;
	return ECompilationTransactionResult::Completed;
}

bool HasCompilationCompleted(UBlueprint* Blueprint)
{
	return Blueprint != nullptr
		&& IsValid(Blueprint)
		&& !Blueprint->bBeingCompiled
		&& Blueprint->IsUpToDate();
}

void RecordTrace(
	FAngelscriptBlueprintCompilationTransactionRequest& Request,
	EAngelscriptBlueprintCompilationTransactionTracePhase Phase)
{
	Request.Trace.Add(Phase);
}
}

EAngelscriptBlueprintCompilationTransactionResult
FAngelscriptBlueprintCompilationTransactionAdapter::Queue(
	FAngelscriptBlueprintCompilationTransactionRequest& Request)
{
	const ECompilationTransactionResult ValidationResult = ValidateRequest(Request, EState::NotQueued);
	if (ValidationResult != ECompilationTransactionResult::Completed)
		return ValidationResult;

	RecordTrace(Request,
		EAngelscriptBlueprintCompilationTransactionTracePhase::QueueRequested);
	TGuardValue<bool> ExecutionGuard(
		GAngelscriptBlueprintCompilationTransactionInProgress, true);
	TGuardValue<bool> RequestGuard(Request.bInProgress, true);
	FBlueprintCompilationManager::QueueForCompilation(Request.Blueprint);
	Request.State = EState::Queued;
	RecordTrace(Request,
		EAngelscriptBlueprintCompilationTransactionTracePhase::QueueCallReturned);
	return ECompilationTransactionResult::Completed;
}

EAngelscriptBlueprintCompilationTransactionResult
FAngelscriptBlueprintCompilationTransactionAdapter::Compile(
	FAngelscriptBlueprintCompilationTransactionRequest& Request)
{
	const ECompilationTransactionResult ValidationResult = ValidateRequest(Request, EState::Queued);
	if (ValidationResult != ECompilationTransactionResult::Completed)
		return ValidationResult;

	RecordTrace(Request,
		EAngelscriptBlueprintCompilationTransactionTracePhase::CompileRequested);
	TGuardValue<bool> ExecutionGuard(
		GAngelscriptBlueprintCompilationTransactionInProgress, true);
	TGuardValue<bool> RequestGuard(Request.bInProgress, true);
	FBlueprintCompilationManager::FlushCompilationQueue(nullptr);
	if (!HasCompilationCompleted(Request.Blueprint))
		return ECompilationTransactionResult::MissingCompletion;

	Request.State = EState::Compiled;
	RecordTrace(Request,
		EAngelscriptBlueprintCompilationTransactionTracePhase::CompilePostconditionPassed);
	return ECompilationTransactionResult::Completed;
}

EAngelscriptBlueprintCompilationTransactionResult
FAngelscriptBlueprintCompilationTransactionAdapter::Finalize(
	FAngelscriptBlueprintCompilationTransactionRequest& Request)
{
	const ECompilationTransactionResult ValidationResult = ValidateRequest(Request, EState::Compiled);
	if (ValidationResult != ECompilationTransactionResult::Completed)
		return ValidationResult;

	RecordTrace(Request,
		EAngelscriptBlueprintCompilationTransactionTracePhase::FinalizeRequested);
	TGuardValue<bool> ExecutionGuard(
		GAngelscriptBlueprintCompilationTransactionInProgress, true);
	TGuardValue<bool> RequestGuard(Request.bInProgress, true);
	FBlueprintCompilationManager::FlushCompilationQueueAndReinstance();
	if (!HasCompilationCompleted(Request.Blueprint))
		return ECompilationTransactionResult::MissingCompletion;

	Request.State = EState::Finalized;
	RecordTrace(Request,
		EAngelscriptBlueprintCompilationTransactionTracePhase::FinalizePostconditionPassed);
	return ECompilationTransactionResult::Completed;
}

bool FAngelscriptBlueprintCompilationTransactionAdapter::HasCapability(
	EAngelscriptBlueprintCompilationTransactionCapability Capability)
{
	return Capability == EAngelscriptBlueprintCompilationTransactionCapability::PublicLifecycle;
}
