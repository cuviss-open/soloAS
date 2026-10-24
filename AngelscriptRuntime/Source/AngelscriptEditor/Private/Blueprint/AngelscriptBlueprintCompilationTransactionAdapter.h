// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UBlueprint;

enum class EAngelscriptBlueprintCompilationTransactionResult : uint8
{
	Completed,
	NullBlueprint,
	InvalidBlueprint,
	InvalidState,
	ReentrantExecution,
	MissingCompletion,
	WrongThread,
};

enum class EAngelscriptBlueprintCompilationTransactionState : uint8
{
	NotQueued,
	Queued,
	Compiled,
	Finalized,
};

enum class EAngelscriptBlueprintCompilationTransactionTracePhase : uint8
{
	QueueRequested,
	QueueCallReturned,
	CompileRequested,
	CompilePostconditionPassed,
	FinalizeRequested,
	FinalizePostconditionPassed,
};

enum class EAngelscriptBlueprintCompilationTransactionCapability : uint8
{
	PublicLifecycle,
	PrivateTransactionOrdering,
};

/**
 * Caller-owned state for an explicit stock Blueprint compilation lifecycle.
 *
 * The adapter only calls public BlueprintCompilationManager entry points. The
 * caller owns this state, the transaction boundary, and any object replacement
 * or post-compile work. It does not claim Haze's private reinstancing phases,
 * transparent replacement timing, or automatic transaction ownership.
 * All operations must be called on the game thread; off-thread calls fail with
 * WrongThread before touching Blueprint state.
 */
struct FAngelscriptBlueprintCompilationTransactionRequest
{
	UBlueprint* Blueprint = nullptr;
	EAngelscriptBlueprintCompilationTransactionState State =
		EAngelscriptBlueprintCompilationTransactionState::NotQueued;
	bool bInProgress = false;
	TArray<EAngelscriptBlueprintCompilationTransactionTracePhase> Trace;
};

/** Explicit queue, compile, and finalize operations for one Blueprint. */
class FAngelscriptBlueprintCompilationTransactionAdapter
{
public:
	static EAngelscriptBlueprintCompilationTransactionResult Queue(
		FAngelscriptBlueprintCompilationTransactionRequest& Request);

	static EAngelscriptBlueprintCompilationTransactionResult Compile(
		FAngelscriptBlueprintCompilationTransactionRequest& Request);

	static EAngelscriptBlueprintCompilationTransactionResult Finalize(
		FAngelscriptBlueprintCompilationTransactionRequest& Request);

	static bool HasCapability(
		EAngelscriptBlueprintCompilationTransactionCapability Capability);
};
