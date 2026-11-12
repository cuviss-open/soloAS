// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned cache cipher primitives for the AngelscriptModKit module .

#include "Shared/Crypto/ModKitCacheCipher.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif
#if PLATFORM_MAC
#include <stdlib.h>
#endif
#include "Misc/AES.h"
#include "Containers/StringConv.h"

namespace
{
	constexpr uint32 HmacBlockSizeBytes = 64;
	constexpr uint32 Sha256DigestBytes = 32;

	// Self-contained SHA-256 (FIPS 180-4): round constants and message schedule.
	constexpr uint32 Sha256K[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};

	uint32 RotR(uint32 Value, int32 Bits)
	{
		return (Value >> Bits) | (Value << (32 - Bits));
	}

	void Sha256Compress(uint32 State[8], const uint8* Block)
	{
		uint32 W[64];
		for (int32 i = 0; i < 16; ++i)
		{
			W[i] = ((uint32)Block[i * 4] << 24) | ((uint32)Block[i * 4 + 1] << 16)
				| ((uint32)Block[i * 4 + 2] << 8) | (uint32)Block[i * 4 + 3];
		}
		for (int32 i = 16; i < 64; ++i)
		{
			const uint32 S0 = RotR(W[i - 15], 7) ^ RotR(W[i - 15], 18) ^ (W[i - 15] >> 3);
			const uint32 S1 = RotR(W[i - 2], 17) ^ RotR(W[i - 2], 19) ^ (W[i - 2] >> 10);
			W[i] = W[i - 16] + S0 + W[i - 7] + S1;
		}
		uint32 A = State[0], B = State[1], C = State[2], D = State[3];
		uint32 E = State[4], F = State[5], G = State[6], H = State[7];
		for (int32 i = 0; i < 64; ++i)
		{
			const uint32 S1 = RotR(E, 6) ^ RotR(E, 11) ^ RotR(E, 25);
			const uint32 Ch = (E & F) ^ (~E & G);
			const uint32 T1 = H + S1 + Ch + Sha256K[i] + W[i];
			const uint32 S0 = RotR(A, 2) ^ RotR(A, 13) ^ RotR(A, 22);
			const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
			const uint32 T2 = S0 + Maj;
			H = G; G = F; F = E; E = D + T1;
			D = C; C = B; B = A; A = T1 + T2;
		}
		State[0] += A; State[1] += B; State[2] += C; State[3] += D;
		State[4] += E; State[5] += F; State[6] += G; State[7] += H;
	}

	uint8 HexValue(TCHAR C)
	{
		if (C >= TCHAR('0') && C <= TCHAR('9'))
		{
			return (uint8)(C - TCHAR('0'));
		}
		if (C >= TCHAR('a') && C <= TCHAR('f'))
		{
			return (uint8)(C - TCHAR('a') + 10);
		}
		if (C >= TCHAR('A') && C <= TCHAR('F'))
		{
			return (uint8)(C - TCHAR('A') + 10);
		}
		return 0xFF;
	}

}

FModKitSha256::FModKitSha256()
	: TotalBytes(0)
	, PendingBytes(0)
{
	State[0] = 0x6a09e667u; State[1] = 0xbb67ae85u; State[2] = 0x3c6ef372u; State[3] = 0xa54ff53au;
	State[4] = 0x510e527fu; State[5] = 0x9b05688cu; State[6] = 0x1f83d9abu; State[7] = 0x5be0cd19u;
	FMemory::Memzero(Pending, sizeof(Pending));
}

void FModKitSha256::Update(const uint8* Data, int32 NumBytes)
{
	if (Data == nullptr || NumBytes <= 0)
	{
		return;
	}
	TotalBytes += (uint64)NumBytes;
	if (PendingBytes > 0)
	{
		const int32 Take = FMath::Min(64 - PendingBytes, NumBytes);
		FMemory::Memcpy(Pending + PendingBytes, Data, Take);
		PendingBytes += Take;
		Data += Take;
		NumBytes -= Take;
		if (PendingBytes == 64)
		{
			Sha256Compress(State, Pending);
			PendingBytes = 0;
		}
	}
	while (NumBytes >= 64)
	{
		Sha256Compress(State, Data);
		Data += 64;
		NumBytes -= 64;
	}
	if (NumBytes > 0)
	{
		FMemory::Memcpy(Pending, Data, NumBytes);
		PendingBytes = NumBytes;
	}
}

void FModKitSha256::Final(uint8* OutDigest32)
{
	// Snapshot the bit length before padding mutates the byte counter.
	const uint64 TotalBits = TotalBytes * 8;
	uint8 PadBlock[72];
	FMemory::Memzero(PadBlock, sizeof(PadBlock));
	PadBlock[0] = 0x80;
	// After the 0x80 byte, pad with zeros until the pending block holds 56
	// bytes, then append the 8-byte big-endian bit length.
	const int32 PadLen = (PendingBytes < 56) ? (56 - PendingBytes) : (120 - PendingBytes);
	Update(PadBlock, PadLen);
	uint8 LengthBytes[8];
	for (int32 i = 0; i < 8; ++i)
	{
		LengthBytes[i] = (uint8)((TotalBits >> ((7 - i) * 8)) & 0xFF);
	}
	Update(LengthBytes, 8);
	check(PendingBytes == 0);
	for (int32 i = 0; i < 8; ++i)
	{
		OutDigest32[i * 4] = (uint8)((State[i] >> 24) & 0xFF);
		OutDigest32[i * 4 + 1] = (uint8)((State[i] >> 16) & 0xFF);
		OutDigest32[i * 4 + 2] = (uint8)((State[i] >> 8) & 0xFF);
		OutDigest32[i * 4 + 3] = (uint8)(State[i] & 0xFF);
	}
	*this = FModKitSha256();
}

void FModKitSha256::HashBuffer(const uint8* Data, int32 NumBytes, uint8* OutDigest32)
{
	FModKitSha256 Sha;
	Sha.Update(Data, NumBytes);
	Sha.Final(OutDigest32);
}

TArray<uint8> FModKitCacheCipher::DeriveKey(
	const TArray<FModKitKeystoreBinaryFragment>& BinaryFragments,
	const TArray<uint8>& HeaderFragment,
	const TArray<uint8>& Salt,
	const TArray<uint8>& RuntimeHash,
	const FString& KeyId)
{
	// KDF (design 1.3): Key = SHA-256(BinaryFragments.Data ++ HeaderFragment ++
	// Salt ++ RuntimeHash ++ KeyId), 32-byte truncation. The Salt is the
	// per-cache nonce embedded in the MKCC header, so Save and Load derive the
	// same key only when they share the cache file's salt.
	FModKitSha256 Sha;
	for (const FModKitKeystoreBinaryFragment& Fragment : BinaryFragments)
	{
		if (Fragment.Data.Num() > 0)
		{
			Sha.Update(Fragment.Data.GetData(), Fragment.Data.Num());
		}
	}
	if (HeaderFragment.Num() > 0)
	{
		Sha.Update(HeaderFragment.GetData(), HeaderFragment.Num());
	}
	if (Salt.Num() > 0)
	{
		Sha.Update(Salt.GetData(), Salt.Num());
	}
	if (RuntimeHash.Num() > 0)
	{
		Sha.Update(RuntimeHash.GetData(), RuntimeHash.Num());
	}
	FTCHARToUTF8 KeyIdBytes(*KeyId);
	if (KeyIdBytes.Length() > 0)
	{
		Sha.Update((const uint8*)KeyIdBytes.Get(), KeyIdBytes.Length());
	}

	TArray<uint8> OutKey;
	OutKey.SetNum(Sha256DigestBytes);
	Sha.Final(OutKey.GetData());
	return OutKey;
}

TArray<uint8> FModKitCacheCipher::HmacSha256(const TArray<uint8>& Key, const TArray<uint8>& Data)
{
	// RFC 2104 HMAC-SHA256 built on FModKitSha256 (UE Core has no HMAC helper):
	// inner = SHA256(keyPad^ipad ++ data), result = SHA256(keyPad^opad ++ inner).
	uint8 KeyBlock[HmacBlockSizeBytes] = { 0 };
	const uint32 KeyLen = FMath::Min<uint32>(Key.Num(), HmacBlockSizeBytes);
	if (KeyLen > 0)
	{
		FMemory::Memcpy(KeyBlock, Key.GetData(), KeyLen);
	}

	uint8 Ipad[HmacBlockSizeBytes];
	uint8 Opad[HmacBlockSizeBytes];
	for (uint32 i = 0; i < HmacBlockSizeBytes; ++i)
	{
		Ipad[i] = KeyBlock[i] ^ 0x36;
		Opad[i] = KeyBlock[i] ^ 0x5c;
	}

	FModKitSha256 Inner;
	Inner.Update(Ipad, HmacBlockSizeBytes);
	if (Data.Num() > 0)
	{
		Inner.Update(Data.GetData(), Data.Num());
	}
	uint8 InnerHash[Sha256DigestBytes];
	Inner.Final(InnerHash);

	FModKitSha256 Outer;
	Outer.Update(Opad, HmacBlockSizeBytes);
	Outer.Update(InnerHash, Sha256DigestBytes);

	TArray<uint8> OutHmac;
	OutHmac.SetNum(Sha256DigestBytes);
	Outer.Final(OutHmac.GetData());
	return OutHmac;
}

void FModKitCacheCipher::EncryptCtr(TArray<uint8>& InOutData, const TArray<uint8>& Key, const TArray<uint8>& Salt)
{
	const uint32 NumBytes = (uint32)InOutData.Num();
	if (NumBytes == 0 || Key.Num() != KeySizeBytes || Salt.Num() != SaltSizeBytes)
	{
		return;
	}

	// AES-CTR over the FAES block primitive: build a keystream of encrypted
	// counter blocks (the 16-byte salt is the initial counter, the trailing
	// four bytes increment per block) and XOR it with the payload. FAES
	// encrypts in place in ECB mode, so a whole keystream buffer is encrypted
	// in one call. NOTE (design 1.3 verification TODO): the UE-AS fork 5.7 FAES
	// surface is confirmed at build time; if the byte-count EncryptData
	// overload shape differs, this is the only call site to adjust.
	const uint32 RoundedBytes = Align(NumBytes, BlockSizeBytes);
	TArray<uint8> Keystream;
	Keystream.SetNumUninitialized(RoundedBytes);

	uint8 Counter[BlockSizeBytes];
	FMemory::Memcpy(Counter, Salt.GetData(), BlockSizeBytes);
	uint8* Out = Keystream.GetData();
	for (uint32 Offset = 0; Offset < RoundedBytes; Offset += BlockSizeBytes)
	{
		FMemory::Memcpy(Out + Offset, Counter, BlockSizeBytes);
		for (int32 i = BlockSizeBytes - 1; i >= BlockSizeBytes - 4; --i)
		{
			if (++Counter[i] != 0)
			{
				break;
			}
		}
	}

	FAES::EncryptData(Keystream.GetData(), RoundedBytes, Key.GetData(), KeySizeBytes);

	uint8* Payload = InOutData.GetData();
	for (uint32 i = 0; i < NumBytes; ++i)
	{
		Payload[i] ^= Keystream[i];
	}
}

void FModKitCacheCipher::DecryptCtr(TArray<uint8>& InOutData, const TArray<uint8>& Key, const TArray<uint8>& Salt)
{
	EncryptCtr(InOutData, Key, Salt);
}

TArray<uint8> FModKitCacheCipher::GenerateSalt()
{
	return GenerateRandomBytes(SaltSizeBytes);
}

TArray<uint8> FModKitCacheCipher::GenerateRandomBytes(int32 NumBytes)
{
	TArray<uint8> Out;
	Out.SetNumUninitialized(FMath::Max(0, NumBytes));
#if PLATFORM_WINDOWS
	if (Out.Num() > 0 && BCryptGenRandom(nullptr, Out.GetData(), Out.Num(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
	{
		Out.Reset();
	}
#elif PLATFORM_MAC
	if (Out.Num() > 0) arc4random_buf(Out.GetData(), Out.Num());
#else
	Out.Reset();
#endif
	return Out;
}

TArray<uint8> FModKitCacheCipher::BytesFromHex(const FString& Hex)
{
	TArray<uint8> Out;
	const int32 Len = Hex.Len();
	if (Len % 2 != 0)
	{
		return Out;
	}
	Out.Reserve(Len / 2);
	for (int32 i = 0; i + 1 < Len; i += 2)
	{
		const uint8 Hi = HexValue(Hex[i]);
		const uint8 Lo = HexValue(Hex[i + 1]);
		if (Hi == 0xFF || Lo == 0xFF)
		{
			Out.Reset();
			return Out;
		}
		Out.Add((Hi << 4) | Lo);
	}
	return Out;
}

FString FModKitCacheCipher::BytesToHex(const TArray<uint8>& Bytes)
{
	FString Out;
	Out.Reserve(Bytes.Num() * 2);
	for (const uint8 Byte : Bytes)
	{
		Out += FString::Printf(TEXT("%02x"), Byte);
	}
	return Out;
}
FString FModKitSha256::HashHex(const uint8* Data, int32 NumBytes)
{
	TArray<uint8> Digest;
	Digest.SetNumUninitialized(32);
	HashBuffer(Data, NumBytes, Digest.GetData());
	return FModKitCacheCipher::BytesToHex(Digest);
}
