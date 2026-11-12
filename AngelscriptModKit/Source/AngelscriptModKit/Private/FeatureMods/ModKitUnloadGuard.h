// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "AngelscriptManager.h"

/**
 * Live-instance guard for runtime unload .
 *
 * Refuses to unload a mod while any UObject instance of its script classes or
 * any external script struct value is live, or its count is unknown. Class default objects
 * and templates/prototypes are excluded from the UObject scan. Suspended
 * script contexts are not supported: the fork's Suspend()/Abort() are asERROR
 * stubs, so a suspended context is unreachable in practice (v2 §2.4).
 */
struct FModKitUnloadGuard
{
	static bool HasUnknownOwnership(FString& OutModuleName);
	/**
	 * Scans every active module owned by the mod for live UObject instances
 * of its script classes, and queries external script struct values. Unknown
 * struct lifetime coverage refuses unload. Returns true when unload is unsafe;
	 * OutLiveCount is -1 for unknown struct lifetimes, -2 for unknown module ownership,
	 * otherwise the live count.
	 * OutLiveClassName receives the first offending class or struct name.
	 */
	static bool HasLiveInstances(const FString& ModName, int32& OutLiveCount, FString& OutLiveClassName);

	/** Destroys live UObject instances owned by a mod after a script timeout. */
	static void DestroyLiveInstances(const FString& ModName);
};
