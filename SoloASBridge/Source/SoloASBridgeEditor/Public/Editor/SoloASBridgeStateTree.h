// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/TopLevelAssetPath.h"
struct FStateTreeNodeClassCache;

namespace SoloASBridge::Editor
{
// Updates only plugin-provided script entries, preserving the native cache and asset data.
SOLOASBRIDGEEDITOR_API void SynchronizeStateTreeScriptClasses(
    FStateTreeNodeClassCache& Cache, const TArray<UClass*>& Classes,
    const TSet<FTopLevelAssetPath>& PreviousScriptPaths);
}
