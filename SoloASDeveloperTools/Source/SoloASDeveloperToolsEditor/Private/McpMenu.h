// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UToolMenu;

namespace SoloASDeveloperTools
{
void PopulateMcpMenu(UToolMenu* Menu);
bool BuildMcpConfiguration(const FString& Host, const FString& Engine, const FString& Project, int32 Port, FString& Out);
}
