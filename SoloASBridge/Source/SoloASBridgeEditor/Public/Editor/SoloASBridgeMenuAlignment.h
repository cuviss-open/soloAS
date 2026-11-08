// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
class UToolMenu;
namespace SoloASBridge::Editor
{
// Values match EToolMenuSectionAlign on UE 5.5 and later.
SOLOASBRIDGEEDITOR_API void SetMenuSectionAlignment(FName Menu, FName Section, uint8 Alignment);
SOLOASBRIDGEEDITOR_API uint8 GetMenuSectionAlignment(FName Menu, FName Section);
void ShutdownMenuSectionAlignment();
}
