#pragma once

#include "CoreMinimal.h"

class UToolMenu;

namespace SoloASDeveloperTools
{
void PopulateMcpMenu(UToolMenu* Menu);
bool BuildMcpConfiguration(const FString& Host, const FString& Engine, const FString& Project, int32 Port, FString& Out);
}
