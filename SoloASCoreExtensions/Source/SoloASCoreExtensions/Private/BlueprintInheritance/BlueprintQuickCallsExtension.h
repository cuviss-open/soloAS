// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once
#include "CoreMinimal.h"
struct FAngelscriptPreprocessor;
namespace SoloASCoreExtensions::BlueprintInheritance
{
bool AreQuickCallsEnabled();
void RewriteQuickCalls(FAngelscriptPreprocessor& Preprocessor);
void StartBlueprintImports();
void StopBlueprintImports();
void StartQuickCalls();
void StopQuickCalls();
}
