// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "SoloASToolchainSettings.h"

class UToolMenu;
struct FToolMenuEntry;

namespace SoloASToolchain
{
enum class ESelection : uint8 { Missing, Own, Managed };
struct FCandidate
{
    bool bUsable = false;
    FString Compiler;
    FString CompilerVersion;
    FString SdkRoot;
    FString SdkVersion;
    FString Error;
};

bool MigrateLegacySettings(const FString& Filename);
ESelection Select(ESoloASToolchainPreference Preference, bool bOwn, bool bManaged);
FString RootForSelection(ESelection InSelection, const FString& InheritedRoot, const FString& ManagedRootPath);
FString DetectionKey(const FString& EngineIdentity, const FString& Xml, const FString& Arguments, const FString& SdkRoot);
FString ManagedRoot();
// Version acceptance comes from this engine's UBT query, never a duplicated version table.
FCandidate ReadQuery(const FString& Json, int32 ExitCode, const FString& RequiredRoot,
    const FString& ExcludedRoot, TFunctionRef<bool(const FString&)> FileExists);
bool CheckFiles(const FCandidate& Candidate, TFunctionRef<bool(const FString&)> FileExists, FString& Error);
bool WriteManagedConfig(const FString& Filename, const FCandidate& Candidate, FString& Error);
bool RestoreProjectConfig(const FString& Filename, FString& Error);
bool WrapPackageMenu(FToolMenuEntry& Entry, TFunction<bool()> PreCheck);
bool IsDetectionComplete();
ESelection ActiveSelection();
FCandidate DetectedOwn();
FCandidate DetectedManaged();
// Commandlet-only, bounded discovery. Does not apply managed XML or install tools.
bool RunPrecheck();
DECLARE_MULTICAST_DELEGATE_OneParam(FOnInstallRequested, const FString&);
SOLOASDEVELOPERTOOLSTOOLCHAINEDITOR_API FOnInstallRequested& OnInstallRequested();
}
