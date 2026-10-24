// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UFunction;

/**
 * Returns whether Function is a supported reflected ScriptCallable function.
 *
 * The exact native-function sidecar is the only ScriptCallable authority.
 * Unsupported reflected signatures fail closed.
 */
ANGELSCRIPTCODE_API bool IsAngelscriptScriptCallableFunction(
	const UFunction* Function
);

