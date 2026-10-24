// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#include "AngelscriptType.h"

class asIScriptEngine;
class asIScriptFunction;

/**
 * Engine-lifetime metadata for one Runtime-owned native event super alias.
 *
 * The record retains no reflection object or native function pointer. The
 * Bridge resolves every native identity again for each synchronous call.
 */
struct FAngelscriptNativeEventSuperAlias
{
	const asIScriptFunction* FunctionIdentity = nullptr;
	FString CanonicalScriptName;
	FString NativeFunctionName;
	FString NativeDeclaringClassPath;
	FAngelscriptTypeUsage ReturnType;
	TArray<FAngelscriptTypeUsage> ArgumentTypes;
	bool bInitializeReturn = false;
	bool bZeroReturnPointer = false;
};

/** Transfers one validated alias to the registry owned by its script engine. */
bool RegisterAngelscriptNativeEventSuperAlias(
	asIScriptEngine* Engine,
	TUniquePtr<FAngelscriptNativeEventSuperAlias> Alias
);

/** Returns metadata only for an internal alias owned by this registry. */
const FAngelscriptNativeEventSuperAlias*
ResolveAngelscriptNativeEventSuperAlias(
	const asIScriptFunction* Function
);

/** Narrow identity check used by the AngelScript builder and compiler. */
bool IsAngelscriptNativeEventSuperAlias(
	const asIScriptFunction* Function
);

/** Validates that a snapshot fits the fixed event-call parameter buffer. */
bool ValidateNativeEventSuperArgumentShape(
	const TArray<FAngelscriptTypeUsage>& ArgumentTypes,
	const FAngelscriptTypeUsage& ReturnType,
	int32 EffectiveArgumentCount
);

/** Validates the complete argument shape stored by an alias. */
bool ValidateNativeEventSuperArgumentShape(
	const FAngelscriptNativeEventSuperAlias& Alias
);

/** Resolves and validates the script override that is making this call. */
bool ResolveNativeEventSuperCaller(
	const FAngelscriptNativeEventSuperAlias& Alias,
	FString& OutCallerClassPath
);
