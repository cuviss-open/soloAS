// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once

#include "CoreMinimal.h"

class UUserDefinedStruct;

namespace SoloASCoreExtensions::BlueprintStructs
{
SOLOASCOREEXTENSIONS_API void StartBlueprintStructs();
SOLOASCOREEXTENSIONS_API void StopBlueprintStructs();
SOLOASCOREEXTENSIONS_API bool ValidateBlueprintStructLayouts(FString& Error);
SOLOASCOREEXTENSIONS_API bool ValidateBlueprintStructSchema(const FString& Schema, FString& Error);
bool PrepareConfiguredBlueprintStructs(FString& Error);

/** Configured UDS asset paths, independent of the interfaces/lambda switches. */
SOLOASCOREEXTENSIONS_API TArray<FString> GetBlueprintStructAssetPaths();
SOLOASCOREEXTENSIONS_API FString GetBlueprintStructSchema();

/** Core module owns these symmetric editor cook delegate registrations. */
SOLOASCOREEXTENSIONS_API void RegisterBlueprintStructCookDependencies();
SOLOASCOREEXTENSIONS_API void UnregisterBlueprintStructCookDependencies();

/** Resolve and validate before any VM type registration. */
bool PrepareBlueprintStructBindings(const TMap<FName, FString>& Bindings,
    const TArray<FString>& Dependencies, FString& Error);
bool BindBlueprintStructs(bool bDeclare, FString& Error);
}
