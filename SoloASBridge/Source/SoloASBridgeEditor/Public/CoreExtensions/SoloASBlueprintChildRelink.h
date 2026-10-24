// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
class UBlueprint;
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
namespace SoloASBridge
{
// A null Blueprint only queries class ownership; it must not schedule a reload.
using FBlueprintChildRelinkCallback = bool (*)(UClass*, const UBlueprint*);
SOLOASBRIDGEEDITOR_API bool InstallBlueprintChildRelink(FBlueprintChildRelinkCallback Callback);
SOLOASBRIDGEEDITOR_API void ResetBlueprintChildRelinkBatch();
SOLOASBRIDGEEDITOR_API void RemoveBlueprintChildRelink();
}
#endif
