// Copyright Cuviss Studio. All Rights Reserved.
#include "Editor/SoloASWorldPartitionCompat.h"
#if WITH_EDITOR && !IS_MONOLITHIC
// 5.8.4 WorldPartitionRuntimeCellTransformerISM.cpp:236 rejects actors with script logic.
// Test: SoloAS.EditorHookInstallation; SoloAS.ScriptActorCellTransformer.
const SoloASBridge::Editor::FWorldPartitionHookTarget& SoloASBridge::Editor::GetWorldPartitionHookTarget()
{
    static const FWorldPartitionHookTarget Target{
        TEXT("?IsBlueprintActorWithLogic@UWorldPartitionRuntimeCellTransformer@@MEBA_NPEAVAActor@@@Z"),
        "_ZNK37UWorldPartitionRuntimeCellTransformer25IsBlueprintActorWithLogicEP6AActor",
        TEXT("IsBlueprintActorWithLogic"), true};
    return Target;
}
#endif
