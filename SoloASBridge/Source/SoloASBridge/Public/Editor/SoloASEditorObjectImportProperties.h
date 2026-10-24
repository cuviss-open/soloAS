// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

struct FImportObjectParams;

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
namespace SoloASBridge
{
using FEditorObjectImportPredicate = bool (*)(const FImportObjectParams&);
using FEditorObjectImportAdapter = const TCHAR* (*)(FImportObjectParams&);

SOLOASBRIDGE_API void SetEditorObjectImportPropertiesAdapter(
	FEditorObjectImportPredicate Predicate,
	FEditorObjectImportAdapter Adapter);

#if WITH_DEV_AUTOMATION_TESTS
namespace Editor::Tests
{
enum class ECallerRange : uint8
{
	CopyProperties,
	ScriptFunctionSourceNavigation,
	OutlinerScriptSourceLink
};

SOLOASBRIDGE_API void RegisterCallerModule(ECallerRange Range, FName ModuleName);
SOLOASBRIDGE_API void UnregisterCallerModule(ECallerRange Range, FName ModuleName);
SOLOASBRIDGE_API bool IsCallerInRegisteredModule(ECallerRange Range, const void* Address);

class FScopedCallerModuleRegistration
{
public:
	FScopedCallerModuleRegistration(ECallerRange InRange, FName InModuleName)
		: Range(InRange), ModuleName(InModuleName)
	{
		RegisterCallerModule(Range, ModuleName);
	}

	~FScopedCallerModuleRegistration()
	{
		UnregisterCallerModule(Range, ModuleName);
	}

private:
	ECallerRange Range;
	FName ModuleName;
};
}
#endif
}
#endif
