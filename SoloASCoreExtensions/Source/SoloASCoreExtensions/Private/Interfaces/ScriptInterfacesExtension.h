// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once
#include "CoreMinimal.h"
#if __has_include("Extensions/AngelscriptExtensionHooks.h")
#include "Extensions/AngelscriptExtensionHooks.h"
#endif

namespace SoloASCoreExtensions::ScriptInterfaces
{
// Frozen at provider startup; all consumers must use the same build input.
bool IsEnabled(const FString& SourceFilename = FString());
#if __has_include("Extensions/AngelscriptExtensionHooks.h")
EAngelscriptExtensionResult GenerateScriptInterfaceTypes(FAngelscriptTypeGenerationContext& Context);
EAngelscriptExtensionResult PrepareBlueprintImports(FAngelscriptTypeGenerationContext& Context);
EAngelscriptExtensionResult CompleteBlueprintImports(FAngelscriptTypeGenerationContext& Context);
UClass* FindPendingBlueprintClass(const FString& ClassPath);
UClass* ResolveBlueprintMapping(const FString& SourceFilename, const FString& Name, FString& Error, const TMap<FName, FString>* ProjectBindings = nullptr);
bool RefreshFeatureModBlueprintPackage(const FString& ClassPath, FString& Error);
bool IsFeatureModBlueprintDependency(const FString& SourceFilename, const FString& Name, FString& Error);
EAngelscriptExtensionResult RewriteDeclarations(FAngelscriptPreParseContext& Context);
EAngelscriptExtensionResult RestoreCachedInterfaces(FAngelscriptTypeGenerationContext& Context);
EAngelscriptExtensionResult ConfigureInterfaces(FAngelscriptTypeGenerationContext& Context);
#endif
void Start();
void Stop();
bool IsProviderRegistered();
void StartInterfaceReloadCallbacks();
void StopInterfaceReloadCallbacks();
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
void StartBlueprintRelink();
bool CanReloadBlueprintSubclass(UClass* Class);
void StartBlueprintInterfaceDefaults();
void StopBlueprintInterfaceDefaults();
void StopBlueprintRelink();
#endif
}
