// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UASClass;
class UObject;

enum class EAngelscriptObjectDestructionResult : uint8
{
	Destroyed,
	Accepted,
	NullObject,
	NullScriptClass,
	InvalidLifecycleGeneration,
	NotGameThread,
	ExecutionShutdownStarted,
	MissingExecutionWriter,
	InvalidObject,
	ObjectIsUnreachable,
	ObjectIsBeingDestroyed,
	UnsupportedObjectKind,
	MismatchedScriptClass,
	MissingScriptType,
	MismatchedScriptType,
};

struct FAngelscriptObjectDestructionRequest
{
	UObject* Object = nullptr;
	UASClass* ScriptClass = nullptr;
	uint64 LifecycleGeneration = 0;
};

/**
 * Caller-owned explicit script-state destruction only.
 *
 * The caller owns the UObject lifetime and the execution write scope. This
 * adapter never performs transparent UObject destruction, parent walks,
 * automatic GC work, or the private DestroyNonNativeProperties path. It only
 * delegates an accepted request to the existing tracked Runtime lifecycle API.
 * The caller must provide a previously tracked pending mirror entry; this
 * adapter does not prove mirror registration or recover missing mirror state.
 * Missing or retired entries remain an engine lifecycle invariant.
 */
class FAngelscriptObjectDestructionAdapter
{
public:
	static EAngelscriptObjectDestructionResult Validate(
		const FAngelscriptObjectDestructionRequest& Request);

	static EAngelscriptObjectDestructionResult Destroy(
		const FAngelscriptObjectDestructionRequest& Request);
};
