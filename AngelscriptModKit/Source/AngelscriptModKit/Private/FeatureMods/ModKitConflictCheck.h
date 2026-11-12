// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "AngelscriptManager.h"

/**
 * Pre-load conflict detection for runtime load .
 *
 * Before a runtime-loaded mod is compiled, its module names and script class
 * names are checked against the active modules and resolvable script classes.
 * A collision disables the new mod (MODKIT_MOD_DISABLED) instead of shadowing
 * an existing module or class. Empty placeholder modules (a previously
 * unloaded mod) are treated as absent and are not a conflict.
 */
struct FModKitConflictCheck
{
	/**
	 * Returns true with OutReason set when any module name in the preprocessed
	 * set collides with a content-bearing active module, or any class name
	 * resolves to an already-active script class (AS type names are globally
	 * unique).
	 */
	static bool CheckModuleSet(const TArray<TSharedRef<FAngelscriptModuleDesc>>& Modules, FString& OutReason);
};