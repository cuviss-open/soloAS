// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch signature verification for the
// AngelscriptModKit module. Zero new Runtime/Bridge registration points and
// zero private-key material: only the public half of the Ed25519 key is ever
// configured, and the verifier only ever performs a public-key operation.

#pragma once

#include "CoreMinimal.h"

/**
 * Ed25519 verification (verify-only) of the patch-manifest canonical payload.
 *
 * Red gate 8 (MK-T702) fallback path: the UE-AS fork 5.7 fork has no Crypto module /
 * FEncryptionContext, and its Runtime/RSA module FRSA::DecryptPublic is not
 * implemented (returns -1 for every input across the 2x2 key/signature
 * endianness matrix, run pf-modkit-mixed-compile-v1-20260904a-UE-AS fork). The
 * signature primitive is therefore a self-contained Ed25519 verify-only
 * (TweetNaCl-derived, public domain; SHA-512 included). The signature is the
 * 64-byte (R || S) encoding over the canonical payload; the public key is the
 * 32-byte packed Ed25519 point.
 */
struct FModKitPatchSignature
{
	/**
	 * Public key: the 32-byte packed Ed25519 point encoding (RFC 8032) that
	 * PatchPublicKeyBase64 stores (also emitted by gen-fixtures.py in the
	 * fixture chain).
	 */
	struct FPublicKey
	{
		uint8 Bytes[32];
	};

	/** Decodes PatchPublicKeyBase64 into the public key; false on malformed input. */
	static bool DecodePublicKey(const FString& Base64, FPublicKey& OutKey);

	/**
	 * Verifies SignatureBytes (64 bytes, R || S) over CanonicalPayload with
	 * Key. Returns true only when the Ed25519 equation R == [S]B - [H]A holds
	 * with H = SHA-512(R || A || M) reduced mod L.
	 */
	static bool Verify(const TArray<uint8>& CanonicalPayload, const TArray<uint8>& SignatureBytes,
		const FPublicKey& Key);
};