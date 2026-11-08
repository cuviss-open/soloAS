// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Modules/ModuleManager.h"

namespace SoloASBridge::Maintenance
{
enum class EUpdateState { Failed, RateLimited, Current, Available, Ignored, NoCompatiblePackage };
struct FUpdate
{
    EUpdateState State = EUpdateState::Failed;
    FString Version;
    FString ReleaseUrl;
    FString DownloadUrl;
    FString AssetName;
    FString ChecksumsUrl;
};
bool ReadDescriptorVersion(const FString& Json, FString& Version);
bool SupportsEngine(const TSharedPtr<FJsonObject>& Profile, const FString& Engine);
FUpdate ParseRelease(bool Connected, int32 Status, const FString& Json, const FString& Installed, const FString& Engine, bool SourcePackage = false);
void RegisterMenus();
void Initialize();
void Shutdown();
}
