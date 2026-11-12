// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"



class UModKitSettings;
struct FAngelscriptModuleDesc;

/**
 * Allowlist policy consumed by the ModuleAdmission validator. Leaf (mod/source-only) modules may only reference the system API
 * surface declared by UModKitSettings; everything else is denied by default
 * (fail-closed). Non-leaf modules, built-in FeatureMods and verified official
 * FeatureMods (unless configured otherwise) are trusted and exempt. Candidate
 * source paths must match the native registry before any exemption applies.
 */
struct FModKitApiAllowlist
{
	struct FModApiUsage
	{
		const FAngelscriptModuleDesc* Module = nullptr;
		TArray<FString> SystemCallDeclarations;
		TArray<FString> BaseTypeDeclarations;
		TArray<FString> OverrideDeclarations;
		FString CollectionError;
	};
	// The candidate descriptors carry the preprocessor's import identity, before swap.
	struct FBlueprintAuditScope
	{
		explicit FBlueprintAuditScope(const TArray<TSharedRef<FAngelscriptModuleDesc>>& Modules);
		~FBlueprintAuditScope();
	private:
		const TArray<TSharedRef<FAngelscriptModuleDesc>>* Previous;
	};
	static FString StableBlueprintDeclaration(const FString& Declaration, const TMap<FString, FString>& Views);

	enum class EApiFace : uint8 { SystemCall, BaseType, Override };
	/**
	 * Matches a declaration string against a pattern. Leading/trailing '*'
	 * select suffix/prefix/contains matching; no wildcard means exact match.
	 */
	static bool MatchDeclaration(const FString& Declaration, const FString& Pattern);
	static bool IsAllowed(const FString& Declaration, EApiFace Face, const UModKitSettings& Settings, FString* OutReason = nullptr);

	/**
	 * Candidate audit used by ModuleAdmission. Emits the
	 * MODKIT_API_AUDIT heartbeat and returns true with OutError set to the
	 * MODKIT-API-ALLOWLIST diagnostic when a leaf module references an API
	 * outside configured tiers, groups and patterns.
	 */
	static bool CheckApiAllowlist(const FString& ModuleName, const FModApiUsage& Usage, FString& OutError);
};
