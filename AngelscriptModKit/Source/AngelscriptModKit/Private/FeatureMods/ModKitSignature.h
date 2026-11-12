// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned mod signature posture for the AngelscriptModKit module.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"

struct FModKitModInfo;
enum class EModKitModTrust : uint8;

/**
 * Official FeatureMods are checked against their signed package manifest before
 * registration. Player FeatureMods follow the project's consent and API policy.
 */
struct ANGELSCRIPTMODKIT_API FModKitSignature
{
	/** Verifies an external package before its descriptor is registered. */
	static bool ValidateExternal(const FModKitModInfo& Mod, FString& OutReason);

	static const TMap<FString, TArray<uint8>>* GetVerifiedPackageFiles(const FString& ScriptDirectory);
	static const FString* GetVerifiedSource(const FString& Path);
	static bool RequiresSignature();
	/** Classifies unsigned/unknown-key packages as players, rejecting invalid official packages. */
	static bool ValidateTrust(const FModKitModInfo& Mod, EModKitModTrust& OutTrust, FString& OutReason);
	static FString PublicKeyBase64();
	static void Startup();
	static void Shutdown();
	static bool TryRevokeSourceValidator();
	static bool HasSourceValidator();
	static bool CheckLoadedSource(FStringView AbsolutePath, FStringView RelativePath,
		FStringView LoadedText, bool bDeleted, FString& OutError);
	static bool IsModSigned(const FModKitModInfo& Mod);
	/** Stage verified source identity with the reload candidate; failed compilation restores the active snapshot. */
	static void Reset();
	static void BeginReloadVerification(const FString& DirectoryPath);
	static void CompleteReloadVerification(const FString& DirectoryPath, bool bSuccess);

	/** Optional on-screen presentation hook; default behaviour is logging only. */
	static FSimpleMulticastDelegate OnUnsignedModWarning;

	/** Emits MODKIT_MOD_UNSIGNED and broadcasts when policy allows a player package. */
	static void NotifyUnsigned(const FModKitModInfo& Mod, const FString& Reason);
};
