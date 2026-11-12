// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch fetch for the AngelscriptModKit
// module. Zero new Runtime/Bridge registration points: the fetcher is pure
// ModKit-side code that reads a patch feed (local directory / file:// for the
// ops fixture chain, http(s) for production) fully into memory before any
// validation or landing happens.

#pragma once

#include "CoreMinimal.h"

/** One fetched patch payload held entirely in memory: manifest text plus
 * every payload file keyed by its forward-slash relative path. */
struct FModKitFetchedPatch
{
	FString ManifestText;
	TMap<FString, TArray<uint8>> Files;
	int64 TotalBytes = 0;
};

struct FModKitHotPatchFetch
{
	/**
	 * Synchronously fetches the patch feed into memory. A bare path or a
	 * file:// URL names a local directory containing Manifest.json plus the
	 * payload files; an http(s):// URL is fetched with the HTTP module
	 * (manifest first, then every file the manifest lists). Fails closed with
	 * OutReason in {config, not-found, size, http, timeout}; nothing is ever
	 * written to disk here. TotalBytes above MaxBytes rejects with
	 * reason=size before any file body is buffered.
	 */
	static bool Fetch(const FString& FeedUrl, int64 MaxBytes, int32 TimeoutSec,
		FModKitFetchedPatch& Out, FString& OutReason, FString& OutDetail);
};
