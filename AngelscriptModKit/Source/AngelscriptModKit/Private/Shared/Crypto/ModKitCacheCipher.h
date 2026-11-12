// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned cache cipher primitives for the AngelscriptModKit module.

#pragma once

#include "CoreMinimal.h"
#include "ScriptCache/ModKitCacheKeystore.h"

/**
 * Self-contained SHA-256 (FIPS 180-4) with the FSHA256-style streaming
 * surface. The locked UE-AS fork 5.7 Core has no FSHA256 (only FSHA1 and an
 * unimplemented FGenericPlatformMisc::GetSHA256Signature fallback), so the
 * digest primitive lives here.
 */
struct FModKitSha256
{
	FModKitSha256();
	void Update(const uint8* Data, int32 NumBytes);
	void Final(uint8* OutDigest32); // also resets the state
	static void HashBuffer(const uint8* Data, int32 NumBytes, uint8* OutDigest32);
	static FString HashHex(const uint8* Data, int32 NumBytes);

private:
	uint32 State[8];
	uint64 TotalBytes;
	uint8 Pending[64];
	int32 PendingBytes;
};

/**
 
 *
 * The cipher builds an AES-CTR keystream on the engine Core FAES block
 * primitive (AES-256, one block per counter value) and authenticates the
 * ciphertext with HMAC-SHA256 over FModKitSha256.
 */
struct FModKitCacheCipher
{
	static constexpr uint32 KeySizeBytes = 32;
	static constexpr uint32 SaltSizeBytes = 16;
	static constexpr uint32 HmacSizeBytes = 32;
	static constexpr uint32 BlockSizeBytes = 16;

	/** KDF: SHA-256(BinaryFragments.Data ++ HeaderFragment ++ Salt ++ RuntimeHash ++ KeyId). */
	static TArray<uint8> DeriveKey(
		const TArray<FModKitKeystoreBinaryFragment>& BinaryFragments,
		const TArray<uint8>& HeaderFragment,
		const TArray<uint8>& Salt,
		const TArray<uint8>& RuntimeHash,
		const FString& KeyId);

	/** RFC 2104 HMAC-SHA256 over the payload (Key is 32 bytes). */
	static TArray<uint8> HmacSha256(const TArray<uint8>& Key, const TArray<uint8>& Data);

	/** AES-256-CTR encrypt/decrypt in place (symmetric; Salt is the nonce). */
	static void EncryptCtr(TArray<uint8>& InOutData, const TArray<uint8>& Key, const TArray<uint8>& Salt);

	/** Alias of EncryptCtr (CTR decryption is identical). */
	static void DecryptCtr(TArray<uint8>& InOutData, const TArray<uint8>& Key, const TArray<uint8>& Salt);

	/** Fresh 16-byte nonce from the OS CSPRNG; empty on failure. */
	static TArray<uint8> GenerateSalt();

	/** OS CSPRNG bytes (keystore fragments, keygen); empty on failure. */
	static TArray<uint8> GenerateRandomBytes(int32 NumBytes);

	static TArray<uint8> BytesFromHex(const FString& Hex);
	static FString BytesToHex(const TArray<uint8>& Bytes);
};