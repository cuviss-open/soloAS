// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "StateTreeNodeClassCache.h"

class UClass;

/** Supplies current script node classes to the normal StateTree cache and explicit consumers. */
class ANGELSCRIPTEDITOR_API FAngelscriptStateTreeClassProvider
{
public:
	static void Initialize();
	static void Shutdown();
	static const TCHAR* GetMarker();
	static void GetScriptNodeClasses(TArray<UClass*>& OutClasses);

	/**
	 * Builds explicit StateTree class data for valid script classes derived from
	 * BaseClass. A null or invalid base, a class with a newer version, a class
	 * outside BaseClass, and duplicate class paths are rejected. Results retain
	 * the provider's stable path order.
	 */
	static void GetScriptNodeClassDataForBaseClass(
		const UClass* BaseClass,
		TArray<TSharedPtr<FStateTreeNodeClassData>>& OutClassData);
};
