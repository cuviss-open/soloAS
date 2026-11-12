// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UModKitSettings;

enum class EFeatureModLocation : uint8
{
	Internal,
	External
};

struct FFeatureModManifest
{
	int32 SchemaVersion = 1;
	TArray<FString> TrunkModules;
	TArray<FString> Branches;
	TArray<FString> FeatureMods;
	TMap<FString, FString> BlueprintBindings;
	TArray<FString> BlueprintDependencies;
	FString Version = TEXT("1.0.0");
	FString SignatureAlgorithm;
	FString SignatureKeyId;
	FString SignaturePayloadSha256;
	FString SignatureValue;
	TArray<FString> RequiredWhitelistCapabilities;
	bool bHasSignature = false;
};

struct FFeatureModPluginInfo
{
	FString Id;
	FString DescriptorPath;
	TArray<FString> PluginDependencies;
	TArray<FString> Modules;
	bool bFeatureMod = false;
	bool bGameFeature = false;
	bool bEnabled = false;
	bool bExplicitlyLoaded = false;
	FString InitialFeatureState;
};

struct FFeatureModInfo
{
	FString Id;
	FString DisplayName;
	FString Directory;
	FString DescriptorPath;
	FString ManifestPath;
	FString ManifestText;
	FString Version;
	FString LoadState;
	EFeatureModLocation Location = EFeatureModLocation::External;
	bool bEnabled = false;
	bool bSigned = false;
	FFeatureModManifest Manifest;
	TArray<FString> Diagnostics;
};

class FFeatureModCatalog
{
public:
	static void Discover(const FString& ProjectDirectory, const TArray<FString>& AllowedRoots,
		TArray<FFeatureModInfo>& OutFeatureMods, TArray<FFeatureModPluginInfo>& OutPlugins);
	static bool ParseManifest(const FString& Json, FFeatureModManifest& OutManifest, TArray<FString>& OutErrors);
	static FString SerializeManifest(const FFeatureModManifest& Manifest);
	static bool SetDependency(const FString& Json, const FString& Kind, const FString& Id, bool bEnabled,
		const FString& FeatureModId, TArray<FFeatureModInfo> FeatureMods,
		const TArray<FFeatureModPluginInfo>& Plugins, FString& OutJson, TArray<FString>& OutErrors);
	static bool IsSupportedRoot(const FString& Root);
	static void CheckWhitelistCapabilities(FFeatureModInfo& FeatureMod, const UModKitSettings& Settings);
	static void Validate(TArray<FFeatureModInfo>& FeatureMods, const TArray<FFeatureModPluginInfo>& Plugins);
	static FString BuildDependencyView(const TArray<FFeatureModInfo>& FeatureMods, const TArray<FFeatureModPluginInfo>& Plugins);
	static bool SaveManifest(const FString& Filename, const FString& Json, const FString& FeatureModId,
		TArray<FFeatureModInfo> FeatureMods, const TArray<FFeatureModPluginInfo>& Plugins, TArray<FString>& OutErrors);
	static bool CreateFromTemplate(const FString& ProjectDirectory, const FString& RelativeRoot,
		const FString& FeatureModId, FString& OutDirectory, FString& OutError);
};
