// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UClass;
class UFunction;

enum class EAngelscriptGeneratedClassFunctionStateResult : uint8
{
	Accepted,
	NullClass,
	InvalidClass,
	InvalidGeneration,
	DuplicateClass,
	StaleClass,
	RetiredClass,
	MissingClass,
	NullFunction,
	InvalidFunction,
	DuplicateFunction,
	MissingFunction,
	FunctionIdentityMismatch,
};

struct FAngelscriptGeneratedClassStateRequest
{
	UClass* GeneratedClass = nullptr;
	uint64 Generation = 0;
	uint64 TypeCookie = 0;
};

struct FAngelscriptGeneratedFunctionStateRequest
{
	FAngelscriptGeneratedClassStateRequest Class;
	UFunction* Function = nullptr;
	FName FunctionName;
};

/**
 * Caller-owned sidecar for generated class/function identity and retirement state.
 *
 * This registry does not inject UClass fields, register engine objects automatically,
 * construct script CDOs, migrate REINST classes, or define engine-global ordering.
 * A class pointer has one active registration; callers must retire and unregister it
 * before reusing the pointer for another generation or before UObject destruction.
 * This is an editor-session-scoped index, not UObject lifetime authority; generation
 * and TypeCookie are caller-provided identity tokens. This Private-module adapter has
 * no public ABI promise.
 */
class ANGELSCRIPTCODE_API FAngelscriptGeneratedClassFunctionStateRegistry final
{
public:
	EAngelscriptGeneratedClassFunctionStateResult RegisterClass(
		const FAngelscriptGeneratedClassStateRequest& Request);
	EAngelscriptGeneratedClassFunctionStateResult RegisterFunction(
		const FAngelscriptGeneratedFunctionStateRequest& Request);
	EAngelscriptGeneratedClassFunctionStateResult LookupFunction(
		const FAngelscriptGeneratedClassStateRequest& ClassRequest,
		const FName FunctionName,
		UFunction*& OutFunction) const;
	EAngelscriptGeneratedClassFunctionStateResult RetireClass(
		const FAngelscriptGeneratedClassStateRequest& Request);
	EAngelscriptGeneratedClassFunctionStateResult UnregisterClass(
		const FAngelscriptGeneratedClassStateRequest& Request);

private:
	struct FClassState
	{
		uint64 Generation = 0;
		uint64 TypeCookie = 0;
		bool bRetired = false;
		TMap<FName, UFunction*> Functions;
	};

	TMap<UClass*, FClassState> Classes;
};
