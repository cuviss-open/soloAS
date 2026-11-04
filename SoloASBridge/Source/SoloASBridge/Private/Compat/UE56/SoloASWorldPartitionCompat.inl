// Copyright Cuviss Studio. All Rights Reserved.
#include "Editor/SoloASWorldPartitionCompat.h"
#if WITH_EDITOR && !IS_MONOLITHIC
// 5.7.4 ISM.cpp:146-195 is identical in Stock and UE-AS fork; the 5.8 logic gate is absent.
// Test: SoloAS.EditorHookInstallation explicitly reports this target as not applicable.
const SoloASBridge::Editor::FWorldPartitionHookTarget& SoloASBridge::Editor::GetWorldPartitionHookTarget()
{
    static const FWorldPartitionHookTarget Target{
        TEXT(""), "", TEXT("IsBlueprintActorWithLogic"), false};
    return Target;
}
#endif
