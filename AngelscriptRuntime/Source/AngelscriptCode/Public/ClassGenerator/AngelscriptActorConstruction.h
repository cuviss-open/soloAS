// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
class AActor;
struct FRotationConversionCache;
namespace AngelscriptActorConstruction
{
ANGELSCRIPTCODE_API bool IsPureScriptActor(const AActor* Actor);
ANGELSCRIPTCODE_API void Prepare(AActor* Actor, const FTransform* Transform = nullptr,
    const FRotationConversionCache* RotationCache = nullptr, bool bDefaultTransform = true);
}
