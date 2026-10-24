// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FProperty;
class UFunction;

enum class EAngelscriptPropertyTrait : uint16
{
	None = 0,
	CppConst = 1,
	CppRef = 2,
	CppEnumAsByte = 4,
	RuntimeGenerated = 8,
	WorldContext = 16,
	ConstTemplateArg = 32,
};
ENUM_CLASS_FLAGS(EAngelscriptPropertyTrait)

/**
 * Returns only reflection traits established by an exact Runtime owner,
 * Bridge registration, native metadata, or immutable UHT sidecar row.
 */
ANGELSCRIPTCODE_API EAngelscriptPropertyTrait GetAngelscriptPropertyTraits(
	const FProperty* Property
);

ANGELSCRIPTCODE_API bool HasAngelscriptPropertyTrait(
	const FProperty* Property,
	EAngelscriptPropertyTrait Trait
);

/**
 * Registers the exact generated-delegate identity in the Bridge registry.
 *
 * Executable UASFunction instances remain type-derived and are rejected here.
 */
ANGELSCRIPTCODE_API bool RegisterAngelscriptRuntimeGeneratedFunction(
	UFunction* Function
);

/** Clears every generated-delegate trait owned by the Runtime module. */
ANGELSCRIPTCODE_API void
ClearAngelscriptRuntimeGeneratedFunctionTraits();

/**
 * Returns whether Function is owned by the AngelScript Runtime reflection
 * hierarchy or has an exact RuntimeGenerated function-trait registration.
 */
ANGELSCRIPTCODE_API bool IsAngelscriptRuntimeGeneratedFunction(
	const UFunction* Function
);
