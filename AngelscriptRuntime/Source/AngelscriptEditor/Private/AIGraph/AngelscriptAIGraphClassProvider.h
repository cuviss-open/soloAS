// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "AIGraphTypes.h"

class UClass;

/**
 * Explicit, side-effect-free provider for script-backed AIGraph node consumers.
 *
 * The provider exposes a caller-owned class-data bridge. It does not mutate
 * UClass identity flags or inject entries into the private class helper.
 */
class ANGELSCRIPTEDITOR_API FAngelscriptAIGraphClassProvider
{
public:
	static const TCHAR* GetMarker();
	static void GetScriptNodeClasses(TArray<UClass*>& OutClasses);

	/**
	 * Builds public AIGraph class data for valid script classes derived from
	 * BaseClass. Invalid bases and classes outside BaseClass are rejected. The
	 * result retains the provider's stable class-path order.
	 */
	static void GetScriptNodeClassDataForBaseClass(
		const UClass* BaseClass,
		TArray<FGraphNodeClassData>& OutClassData);
};
