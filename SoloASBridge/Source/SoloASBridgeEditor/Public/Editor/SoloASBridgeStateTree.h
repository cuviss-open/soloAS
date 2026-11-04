// category=A; basis=UE-AS fork StateTreeNodeClassCache::CacheClasses script-node discovery; StateTree remains optional.
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/TopLevelAssetPath.h"
struct FStateTreeNodeClassCache;
class FStateTreeEditorModule;

namespace SoloASBridge::Editor
{
SOLOASBRIDGEEDITOR_API FStateTreeEditorModule& GetStateTreeEditorModule();
// Updates only plugin-provided script entries, preserving the native cache and asset data.
SOLOASBRIDGEEDITOR_API void SynchronizeStateTreeScriptClasses(
    FStateTreeNodeClassCache& Cache, const TArray<UClass*>& Classes,
    const TSet<FTopLevelAssetPath>& PreviousScriptPaths);
}
