// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once

#include "Extensions/AngelscriptExtensionHooks.h"

#include "Containers/Array.h"
#include "Containers/UnrealString.h"

namespace SoloASCoreExtensions
{
namespace ExtensionRegistration
{
using FExtensionFactory = FAngelscriptExtensionCallbackRegistration (*)();

struct FExtensionFactoryEntry
{
	const ANSICHAR* FeatureId;
	FExtensionFactory Factory;
};

struct FExtensionFactorySnapshot
{
	TArray<FExtensionFactoryEntry> Entries;
	FString CompatibilityTag;
};

class FExtensionRegistrar final
{
public:
	FExtensionRegistrar(
		const ANSICHAR* FeatureId,
		FExtensionFactory Factory
	);
};

struct FFeaturePolicy
{
    bool bBlueprintStructs = true;
    bool bNativeEventSuper = true;
    bool bBlueprintInheritance = true;
    bool bBlueprintQuickCalls = true;
};

// Frozen on first use, like the existing interface and lambda policies.
const FFeaturePolicy& GetFeaturePolicy();

FString MakeCompatibilityTag(const TArray<FExtensionFactoryEntry>& Entries,
    bool bScriptInterfacesEnabled, bool bLambdaEnabled, int32 SlotsPerSite, const FFeaturePolicy& Policy = FFeaturePolicy());

// Factory addresses and VM bindings live until process exit. A second freeze
// is rejected even after callback withdrawal; configuration changes need restart.
bool SnapshotAndFreezeExtensionFactories(
	FExtensionFactorySnapshot& OutSnapshot,
	FString& OutError
);
}
}

namespace SoloASCoreExtensions::ExtensionRegistration
{
// Blueprint structs run before interfaces. Shared transactions and disabled-feature
// diagnostics still enter the integration block and apply their own feature policy.
EAngelscriptExtensionResult DispatchCoreTypeGeneration(
    FAngelscriptTypeGenerationContext& Context,
    FAngelscriptTypeGenerationExtensionCallback Interfaces,
    FAngelscriptTypeGenerationExtensionCallback BlueprintStructs,
    bool bInterfacesEnabled, bool bBlueprintStructsEnabled = true);

bool RegisterExtensionCallbacks(const FExtensionFactorySnapshot& Snapshot,
    TArray<FAngelscriptExtensionCallbackRegistration>& Tokens, FString& OutError);
bool UnregisterExtensionCallbacksReverse(
    TArray<FAngelscriptExtensionCallbackRegistration>& Tokens, FString& OutError);
}

class FSoloASCoreExtensionsRegistration
{
public:
    FAngelscriptExtensionProviderRegistration ProviderRegistration;
    SoloASCoreExtensions::ExtensionRegistration::FExtensionFactorySnapshot Factories;
    TArray<FAngelscriptExtensionCallbackRegistration> Tokens;
};
