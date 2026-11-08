// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

namespace AngelscriptCoreExtensionPoints
{
// Process-local VM generation. Names and cache DataGuid do not identify a generation.
struct FModuleGenerationId
{
    FString ModuleName;
    uint64 Generation = 0;
};
}

class asCModule;
namespace AngelscriptCoreExtensionPoints::Private
{
// Internal identity lookup; never creates or changes a generation.
ANGELSCRIPTCODE_API uint64 GetModuleGeneration(const asCModule* Module);
}
