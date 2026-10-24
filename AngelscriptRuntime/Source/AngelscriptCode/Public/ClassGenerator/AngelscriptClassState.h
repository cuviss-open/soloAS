// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

class UClass;
class asITypeInfo;

/**
 * Returns the borrowed AngelScript type associated with Class.
 *
 * UASClass instances store the pointer locally. Other UClass instances use
 * the SoloASBridge class-state registry.
 */
ANGELSCRIPTCODE_API asITypeInfo* GetAngelscriptClassScriptType(
	const UClass* Class
);

/**
 * Returns only the type directly associated with Class.
 *
 * Reload state machines use this exact-class view so inherited Blueprint
 * identity cannot hide a missing or replaced direct entry.
 */
ANGELSCRIPTCODE_API asITypeInfo* GetDirectAngelscriptClassScriptType(
	const UClass* Class
);

/**
 * Associates a non-null borrowed AngelScript type with Class.
 */
ANGELSCRIPTCODE_API bool SetAngelscriptClassScriptType(
	UClass* Class,
	asITypeInfo* ScriptType
);

/**
 * Clears the association only when ExpectedScriptType still matches.
 */
ANGELSCRIPTCODE_API bool ClearAngelscriptClassScriptType(
	UClass* Class,
	const asITypeInfo* ExpectedScriptType
);

/**
 * Clears every association owned by this runtime module.
 *
 * Callers must quiesce users of the borrowed pointers before shutdown.
 */
ANGELSCRIPTCODE_API void ClearAngelscriptClassScriptTypes();
