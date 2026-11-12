// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch manifest schema for the
// AngelscriptModKit module. Zero new Runtime/Bridge registration points: the
// manifest is a pure ModKit-side data model consumed by the offline verify
// commandlet and later by the fetch/land pipeline (MK-T703).

#pragma once

#include "CoreMinimal.h"

/**
 * One patch file entry of a ModKitHotPatchManifest/v1. The path is relative to
 * the manifest directory, forward slashes only, and never escapes the patch
 * root (the same fail-closed predicate as ModKitPatchPrecheck's
 * invalid-module-name). Sha256 is the lowercase hex digest of the file bytes.
 */
struct FModKitPatchFileEntry
{
	FString Path;
	FString Sha256;
};

/**
 precheck.verdict is the
 * offline verdict (PASS/REJECT) and is the operation-side gate that the
 * runtime re-checks before landing or applying a patch.
 */
struct FModKitPatchPrecheckRecord
{
	FString Verdict;
	FString Reason;
	FString EngineVersion;
	FString BaselineSha256;
};

/**
 * Hot-patch manifest (v1) with a signed canonical payload.
 *
 * Schema: a JSON object with format/patchId/version/engineVersion, a non-empty
 * files array (path + sha256), an optional precheck record, and a signature
 * object (algorithm/payloadSha256/value). The signature covers the canonical
 * serialization of the package fields and signature identity; the payloadSha256
 * anchor re-locks that canonical so the digest can be checked independently of
 * the Ed25519 path.
 */
struct ANGELSCRIPTMODKIT_API FModKitPatchManifest
{
	static const TCHAR* GetFormatV1();
	static const TCHAR* GetSignatureAlgorithmEd25519();

	FString PatchId;
	FString Kind = TEXT("hotpatch");
	FString Version;
	FString EngineVersion;
	TArray<FModKitPatchFileEntry> Files;
	FModKitPatchPrecheckRecord Precheck;
	FString SignatureAlgorithm;
	FString SignatureSigner;
	FString PublicKeySha256;
	FString PayloadSha256;         // signature.payloadSha256 (hex)
	TArray<uint8> SignatureBytes;  // decoded signature.value

	static constexpr int64 MaxManifestBytes = 1024 * 1024;
	static constexpr int64 MaxFileBytes = 16 * 1024 * 1024;
	static constexpr int64 MaxPackageBytes = 64 * 1024 * 1024;
	static bool ReadBytes(const FString& Path, int64 MaxBytes, TArray<uint8>& Out);
	static bool IsJsonWithinBudget(const FString& JsonText);
	static bool ReadText(const FString& Path, FString& Out);
	bool MatchesPublicKey(const uint8* PublicKey) const;

	/** Parses and schema-validates JsonText; OutReason carries a detail string. */
	static bool Parse(const FString& JsonText, FModKitPatchManifest& Out, FString& OutReason);

	/**
	 * Deterministic canonical payload (UTF-8, LF line endings, no trailing LF;
	 * files sorted by path). Byte-identical across the C++ verifier, the
	 * PowerShell offline signer, and the Python fixture generator.
	 */
	void ToCanonicalBytes(TArray<uint8>& OutBytes) const;

	/** Lowercase hex of SHA-256(canonical). */
	FString ComputePayloadSha256() const;

	/** Engine-version match predicate against the running engine version. */
	bool MatchesEngineVersion() const;

	bool IsVersionAtLeast(const FString& Other) const;

	/** Trust policy is supplied by the consumer's startup-frozen settings. */
	bool VerifySignature(const FString& ExpectedKind, const FString& PublicKeyBase64,
		const TArray<FString>& TrustedKeyIds, FString& OutReason) const;

	/** Keys are manifest-relative POSIX paths; bytes are the exact immutable input snapshot. */
	bool VerifyFiles(const TMap<FString, TArray<uint8>>& FileBytes, FString& OutReason) const;

	/**
	 * Verifies a signed package and its full byte snapshot before atomically establishing
	 * or advancing its floor. Existing invalid/unreadable state is never treated as absent.
	 * Expected identity comes from the consumer's descriptor, not the manifest itself.
	 */
	static bool VerifyAndCommitPackageFloor(const FString& ManifestJson,
		const TMap<FString, TArray<uint8>>& FileBytes, const FString& ExpectedId,
		const FString& ExpectedVersion, const FString& PublicKeyBase64,
		const TArray<FString>& TrustedKeyIds, const FString& FloorPath,
		bool bAllowDowngrade, FString& OutReason);
};

/**
 * Running engine version in "{Major}.{Minor}.{Patch}-{Changelist} {Branch}" form,
 * matching ModKitExportApiSurface and the offline fixture generator. UE-AS fork's
 * FApp::GetBuildVersion() is not stamped ("UE5-CL-0"), so FEngineVersion::Current()
 * is the authoritative source.
 */
FString ModKitPatchEngineVersionString();
