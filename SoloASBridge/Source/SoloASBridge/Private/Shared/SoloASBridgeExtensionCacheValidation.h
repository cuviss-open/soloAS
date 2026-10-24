#pragma once

#include "Shared/SoloASBridgeExtensionCache.h"

namespace SoloASBridge::Extensions::CacheInternal
{
struct FCacheArtifactMember
{
    FString Role;
    FString Suffix;
    FString Path;
    TArray<uint8> Bytes;
};

bool ValidateOperationInputs(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath,
    FString& OutError);

bool ValidateFingerprint(
    const FString& ExpectedFingerprint,
    FString& OutError);

FString GetCompanionPath(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath);

TArray<FCacheArtifactMember> MakeMembers(
    const FExtensionCacheContract& Contract,
    const FString& ArtifactPath);
}
