// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

namespace SoloASEditorManagement
{
struct FActionResult
{
    int32 Succeeded = 0;
    int32 Failed = 0;
    int32 Skipped = 0;
    bool bCancelled = false;
};
ANGELSCRIPTEDITOR_API void Initialize();
ANGELSCRIPTEDITOR_API void RegisterMenu();
ANGELSCRIPTEDITOR_API void Shutdown();
ANGELSCRIPTEDITOR_API TArray<FString> FindEmptyEngineVersionAssets();
ANGELSCRIPTEDITOR_API TArray<FName> GetLoadTimeRebindingPackages();
ANGELSCRIPTEDITOR_API FActionResult ResaveEmptyEngineVersionAssets();
ANGELSCRIPTEDITOR_API FActionResult FixLoadTimeRebindingAssets();
ANGELSCRIPTEDITOR_API void FullReloadScripts();
ANGELSCRIPTEDITOR_API void OpenLog();
}
