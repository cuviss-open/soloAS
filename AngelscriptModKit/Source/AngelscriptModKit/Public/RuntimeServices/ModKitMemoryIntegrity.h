// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
                                                                                 

#pragma once

#include "CoreMinimal.h"
#include "RuntimeServices/ModKitRuntimeHash.h"

/**
 * Delayed-degradation runtime integrity check : shares the disk .text
 * hash domain with the v11 cache binding (two entry points, one domain).
 *
 * The process-lifetime baseline is computed once from the unwrapped default
 * provider (FModKitRuntimeHash::ComputeDefault); periodic checks run through
 * the integrity provider slot, which a game/fixture may override. A mismatch
 * records a breach (MODKIT_MEMORY_INTEGRITY_VIOLATION) and arms the cache
 * transform: every later Save/Load consults FModKitCacheTransform, which then
 * fails closed (MODKIT_CACHE_TRANSFORM_BLOCKED reason=integrity-breach). The
 * check is registered only when bMemoryIntegrityCheck is enabled; the negative
 * path uses bForceIntegrityCheckInDev to sample every ticker call (hash
 * computation itself is capped at once per second).
 */
struct ANGELSCRIPTMODKIT_API FModKitMemoryIntegrity
{
	/** Registers the plugin ticker when bMemoryIntegrityCheck is enabled. */
	static void Startup();
	static void Shutdown();

	/** True after a violation has been recorded (delayed degradation). */
	static bool IsBreached();

	/** Game/fixture override for the check-side hash provider. */
	static void SetIntegrityProvider(FModKitRuntimeHashProvider Provider);
};
