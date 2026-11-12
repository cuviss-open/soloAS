// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
                                                                                  

#pragma once

#include "CoreMinimal.h"

/**
 * Runtime-hash provider: fills OutHash with the 32-byte SHA-256 of the bound
 * binary's disk .text. The default (ModKit) resolves the configured
 * RuntimeModuleName in the current process and hashes the backing file bytes,
 * degrading to the main executable for monolithic targets; a game module may
 * override it to bind a different binary or to add simulation vectors.
 */
using FModKitRuntimeHashProvider = TFunction<bool(TArray<uint8>& OutHash)>;

/**
 * Public entry points for the runtime-hash binding used by the cache
 * transform. Game-side code registers its provider early (StartupModule);
 * the hash itself is computed lazily at the first cache load, after every
 * module is mapped.
 */
struct ANGELSCRIPTMODKIT_API FModKitRuntimeHash
{
	/** Replaces the default runtime-hash provider (game/fixture override). */
	static void SetProvider(FModKitRuntimeHashProvider Provider);

	/**
	 * Computes the default binding hash: the configured RuntimeModuleName's
	 * backing file disk .text, or the main executable for monolithic targets.
	 */
	static bool ComputeDefault(TArray<uint8>& OutHash);
};
