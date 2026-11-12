// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
                                                                                     

#pragma once

#include "CoreMinimal.h"

/**
 * One binary fragment stored in the keystore JSON (keystore schema v1, design
 * 2.1): a 32-byte hex "data" blob plus an id used only for bookkeeping. The
 * fragments are the split key material consumed by the KDF.
 */
struct FModKitKeystoreBinaryFragment
{
	FString Id;
	TArray<uint8> Data;
};

/**
 * One keystore entry. expectedRuntimeHash is the keystore-registered SHA-256
 * of the target runtime module disk .text (MK-Q02: expected-fingerprint
 * registration); runtimeModule is the module name the consumer resolves
 * (default "AngelscriptCode").
 */
struct FModKitKeystoreEntry
{
	FString KeyId;
	TArray<FModKitKeystoreBinaryFragment> BinaryFragments;
	TArray<uint8> HeaderFragment;
	TArray<uint8> ExpectedRuntimeHash; // 32B
	FString RuntimeModule;
	FString CreatedAt;
	FString Comment;
};

/**
 * Build/ScriptCacheKey.json read/write. The file is the single human-maintained
 * secret artifact (gitignored; loss means the release's cache can no longer be
 * regenerated; rotate the KeyId instead). Absence is not an error: callers
 * fall back to plaintext passthrough.
 */
struct FModKitCacheKeystore
{
	/** ProjectDir/Build/ScriptCacheKey.json, overridable via MODKIT_CACHE_KEY_PATH. */
	static FString GetDefaultPath();

	/** True when Path parses as a valid keystore JSON file. */
	static bool LoadEntries(const FString& Path, TArray<FModKitKeystoreEntry>& OutEntries);

	/** True when Path has an entry with the requested KeyId. */
	static bool FindEntry(const FString& Path, const FString& KeyId, FModKitKeystoreEntry& OutEntry);

	/** Adds or replaces the entry with the same KeyId and writes the file. */
	static bool WriteEntry(const FString& Path, const FModKitKeystoreEntry& NewEntry);
};