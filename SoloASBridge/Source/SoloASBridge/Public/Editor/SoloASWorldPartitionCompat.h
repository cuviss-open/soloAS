// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#if WITH_EDITOR && !IS_MONOLITHIC
namespace SoloASBridge::Editor
{
struct FWorldPartitionHookTarget
{
    const TCHAR* WindowsSymbol;
    const ANSICHAR* MacSymbol;
    const TCHAR* Match;
    bool bApplicable;
};
SOLOASBRIDGE_API const FWorldPartitionHookTarget& GetWorldPartitionHookTarget();
}
#endif
