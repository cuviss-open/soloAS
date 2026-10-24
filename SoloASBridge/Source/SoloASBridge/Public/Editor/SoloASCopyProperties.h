// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "Engine/Engine.h"

namespace SoloAS
{
// Prevents implicit template/CDO archetype remapping. Caller mappings remain
// authoritative; a temporary self pair is removed after synchronous copying.
SOLOASBRIDGE_API void CopyPropertiesForUnrelatedObjectsWithArchetypePolicy(
    UObject* OldObject, UObject* NewObject,
    UEngine::FCopyPropertiesForUnrelatedObjectsParams Params = {});
}
