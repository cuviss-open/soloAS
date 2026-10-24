// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UClass;

enum class EAngelscriptScriptClassLifetimeTransitionResult : uint8
{
	Accepted,
	NullClass,
	InvalidClass,
	InvalidGeneration,
	DuplicateClass,
	MissingClass,
	StaleIdentity,
	InvalidTransition,
	AlreadyRetired,
	NotRetired,
};

enum class EAngelscriptScriptClassLifetimeState : uint8
{
	Registered,
	Active,
	Retiring,
	Retired,
};

struct FAngelscriptScriptClassLifetimeRequest
{
	UClass* ScriptClass = nullptr;
	uint64 Generation = 0;
	uint64 TypeCookie = 0;
};

/**
 * Caller-owned, state-machine-only class lifetime transition registry.
 *
 * The registry externalizes identity-checked transitions only. It does not replace
 * UObjectGlobals construction/destruction, create script CDOs/default subobjects,
 * trigger GC, map REINST classes, verify class provenance, or install an engine-global hook.
 * Only an Active class may enter retirement; callers must guarantee teardown ordering and
 * unregister before UObject destruction. This Private-module adapter has no public ABI promise.
 */
class ANGELSCRIPTCODE_API FAngelscriptScriptClassLifetimeTransitionRegistry final
{
public:
	EAngelscriptScriptClassLifetimeTransitionResult Register(
		const FAngelscriptScriptClassLifetimeRequest& Request);
	EAngelscriptScriptClassLifetimeTransitionResult Activate(
		const FAngelscriptScriptClassLifetimeRequest& Request);
	EAngelscriptScriptClassLifetimeTransitionResult BeginRetirement(
		const FAngelscriptScriptClassLifetimeRequest& Request);
	EAngelscriptScriptClassLifetimeTransitionResult CompleteRetirement(
		const FAngelscriptScriptClassLifetimeRequest& Request);
	EAngelscriptScriptClassLifetimeTransitionResult Unregister(
		const FAngelscriptScriptClassLifetimeRequest& Request);
	EAngelscriptScriptClassLifetimeTransitionResult GetState(
		const FAngelscriptScriptClassLifetimeRequest& Request,
		EAngelscriptScriptClassLifetimeState& OutState) const;

private:
	struct FClassState
	{
		uint64 Generation = 0;
		uint64 TypeCookie = 0;
		EAngelscriptScriptClassLifetimeState State =
			EAngelscriptScriptClassLifetimeState::Registered;
	};

	static EAngelscriptScriptClassLifetimeTransitionResult ValidateIdentity(
		const FAngelscriptScriptClassLifetimeRequest& Request,
		const TMap<UClass*, FClassState>& Classes,
		const FClassState*& OutState);

	TMap<UClass*, FClassState> Classes;
};
