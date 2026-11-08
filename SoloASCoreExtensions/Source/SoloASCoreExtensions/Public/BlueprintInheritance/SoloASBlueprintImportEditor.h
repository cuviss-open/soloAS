// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once
#include "CoreMinimal.h"
#if WITH_EDITOR && !IS_MONOLITHIC
namespace SoloASCoreExtensions::BlueprintInheritance
{
using FImportEditorAdapter = bool (*)(bool bEnabled);
SOLOASCOREEXTENSIONS_API bool SetImportEditorAdapter(FImportEditorAdapter Adapter);
SOLOASCOREEXTENSIONS_API bool IsImportedBlueprintParent(const UClass* Class);
}
#endif
