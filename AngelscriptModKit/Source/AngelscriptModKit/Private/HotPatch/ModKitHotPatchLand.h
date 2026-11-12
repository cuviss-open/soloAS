// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch atomic landing for the
// AngelscriptModKit module. Zero new Runtime/Bridge registration points:
// landing re-runs the full MK-T702 verification chain on the in-memory patch
// (signature + per-file SHA-256 + precheck + engine version) before any byte
// reaches the staging root, and lands via a .tmp-incoming directory rename so
// a failed patch never leaves a partial tree behind.

#pragma once

#include "CoreMinimal.h"

struct FModKitFetchedPatch;
struct FModKitPatchManifest;

struct ANGELSCRIPTMODKIT_API FModKitHotPatchLand
{
	/** Default staging root: <ProjectSaved>/ModKitPatches (never a script
	 * root, never under FeatureMods/, never inside any plugin content tree). */
	static FString DefaultStagingRoot();

	/** Settings override or default; empty when it overlaps a script/mod root. */
	static FString ResolveStagingRoot();

	/**
	 * The full MK-T702 verification chain on an in-memory patch: schema, key,
	 * payloadSha256 anchor + Ed25519 over the canonical, fetched-set equality
	 * + per-file SHA-256, precheck PASS, engine-version match. Shared by Land
	 * (MK-T703) and the H3 explicit trigger (MK-T704), which re-verifies the
	 * staged bytes before every apply. OutReason in {schema, kind, key-missing, key-id-untrusted,
	 * bad-signature, sha-mismatch, precheck-required, engine-mismatch}.
	 */
	static bool ValidatePatch(const FModKitFetchedPatch& Patch, const FString& PublicKeyBase64,
		FModKitPatchManifest& OutManifest, FString& OutReason, FString& OutDetail);

	/**
	 * Validates then atomically lands the fetched patch under
	 * <StagingRoot>/<PatchId>. An existing directory of the same PatchId is
	 * replaced wholesale (re-land is idempotent). The old tree is retained
	 * until the new tree is renamed into place. A failed restore preserves
	 * the old tree in .previous for recovery instead of deleting it.
	 * OutReason additionally in {disabled, io, config, version}.
	 */
	static bool Land(const FModKitFetchedPatch& Patch, const FString& PublicKeyBase64,
		FString& OutPatchId, int32& OutFileCount, FString& OutReason, FString& OutDetail);
};
