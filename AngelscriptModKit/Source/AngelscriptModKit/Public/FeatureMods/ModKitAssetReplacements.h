// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

/** Explicit asset lookup replacements. This does not install engine redirects. */
struct ANGELSCRIPTMODKIT_API FModKitAssetReplacements
{
	/**
	 * Register one /Game asset replacement owned by an active FeatureMod. The
	 * ModName is its stable plugin ID, and the replacement must be under that
	 * plugin's mounted content root. A second owner
	 * cannot replace the same source path.
	 */
	static bool Register(const FString& ModName, const FSoftObjectPath& SourceAsset,
		const FSoftObjectPath& ReplacementAsset, FString& OutReason);

	/** Returns the active replacement path, or the original path on any miss. */
	static FSoftObjectPath Resolve(const FSoftObjectPath& SourceAsset);

	/** Tries the resolved asset synchronously, then falls back to the source asset. */
	static UObject* LoadSynchronous(const FSoftObjectPath& SourceAsset);

	/** Remove every replacement owned by a mod. Called automatically on unload/failure. */
	static void RemoveForMod(const FString& ModName);

	/** Clears registrations and unload delegate binding when ModKit shuts down. */
	static void Shutdown();
};
