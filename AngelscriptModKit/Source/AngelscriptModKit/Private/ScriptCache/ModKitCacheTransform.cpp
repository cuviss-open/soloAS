// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Authenticated stock-cache envelopes and immutable synchronous file views.

#include "ScriptCache/ModKitCacheTransform.h"
#include "FeatureMods/ModKitCompilationManager.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Math/RandomStream.h"
#include "Misc/Crc.h"
#include "Modules/ModuleManager.h"
#include "Misc/CString.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Containers/StringConv.h"
#include <filesystem>
#include "Shared/Crypto/ModKitCacheCipher.h"
#include "ScriptCache/ModKitCacheKeystore.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformFile.h"
#include "Async/AsyncFileHandle.h"
#include "Async/MappedFileHandle.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "Misc/EngineVersionComparison.h"
#include "RuntimeServices/ModKitMemoryIntegrity.h"
#include "Core/ModKitSettings.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/MinWindows.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	using FPolicy = FModKitCacheEnvelopePolicy;
	using EDirection = EModKitCacheDirection;

	// MKCC header (design 2.3), little-endian fields; the magic is stored so
	// the file's first four bytes are literally 'M','K','C','C':
	//   uint32 Magic 'MKCC' | uint16 Version | uint16 Flags |
	//   uint8 KeyIdLen | uint8 KeyId[KeyIdLen] | uint8 FragmentLen |
	//   uint8 HeaderFragment[FragmentLen] | uint8 Salt[16] |
	//   uint8 PayloadHmac[32] | uint32 PayloadLen | Payload
	constexpr uint32 MkccMagic = 0x43434B4Du;
	constexpr uint16 MkccVersion = 2;
	constexpr uint16 FlagAes = 0x0001u;
	constexpr int32 MaxKeyIdBytes = 63;

	FModKitRuntimeHashProvider GRuntimeHashProvider;
	bool bRuntimeHashComputed = false;
	bool bRuntimeHashValid = false;
	TArray<uint8> CachedRuntimeHash;

	IConsoleCommand* GenerateKeyCommand = nullptr;
    bool GCacheViewInstalled = false;

	uint32 ReadU32LE(const TArray<uint8>& Bytes, int32 Offset)
	{
		uint32 Value = 0;
		for (int32 i = 0; i < 4; ++i)
		{
			Value |= ((uint32)Bytes[Offset + i]) << (8 * i);
		}
		return Value;
	}

	uint16 ReadU16LE(const TArray<uint8>& Bytes, int32 Offset)
	{
		return (uint16)((uint16)Bytes[Offset] | ((uint16)Bytes[Offset + 1] << 8));
	}

	uint8 ReadU8(const TArray<uint8>& Bytes, int32 Offset)
	{
		return Bytes[Offset];
	}

	void AppendU8(TArray<uint8>& Out, uint8 Value)
	{
		Out.Add(Value);
	}

	void AppendU16LE(TArray<uint8>& Out, uint16 Value)
	{
		Out.Add((uint8)(Value & 0xFFu));
		Out.Add((uint8)((Value >> 8) & 0xFFu));
	}

	void AppendU32LE(TArray<uint8>& Out, uint32 Value)
	{
		for (int32 i = 0; i < 4; ++i)
		{
			Out.Add((uint8)((Value >> (8 * i)) & 0xFFu));
		}
	}

	bool ResolveKeystoreEntry(const FPolicy& Policy, FModKitKeystoreEntry& OutEntry)
	{
		return FModKitCacheKeystore::FindEntry(FModKitCacheKeystore::GetDefaultPath(), Policy.KeyId, OutEntry);
	}

#if WITH_DEV_AUTOMATION_TESTS
	/** Salt-free stable key so the opcode map is reproducible across Save/Load. */
	TArray<uint8> DerivePermutationSeed(const FModKitKeystoreEntry& Entry)
	{
		FModKitSha256 Sha;
		for (const FModKitKeystoreBinaryFragment& Fragment : Entry.BinaryFragments)
		{
			if (Fragment.Data.Num() > 0)
			{
				Sha.Update(Fragment.Data.GetData(), Fragment.Data.Num());
			}
		}
		if (Entry.HeaderFragment.Num() > 0)
		{
			Sha.Update(Entry.HeaderFragment.GetData(), Entry.HeaderFragment.Num());
		}
		if (Entry.ExpectedRuntimeHash.Num() > 0)
		{
			Sha.Update(Entry.ExpectedRuntimeHash.GetData(), Entry.ExpectedRuntimeHash.Num());
		}
		FTCHARToUTF8 KeyIdBytes(*Entry.KeyId);
		if (KeyIdBytes.Length() > 0)
		{
			Sha.Update((const uint8*)KeyIdBytes.Get(), KeyIdBytes.Length());
		}
		TArray<uint8> Out;
		Out.SetNum(32);
		Sha.Final(Out.GetData());
		return Out;
	}

	/** 0-219 bijection + 220-255 identity (S4), Fisher-Yates over 220 elements. */
	TArray<uint8> DeriveOpcodePermutation(const FModKitKeystoreEntry& Entry)
	{
		const uint8 Label[] = { 'o', 'p', 'c', 'o', 'd', 'e', '-', 'm', 'a', 'p', '-', 'v', '1' };
		const TArray<uint8> StableKey = DerivePermutationSeed(Entry);
		const TArray<uint8> Seed = FModKitCacheCipher::HmacSha256(StableKey, TArray<uint8>(Label, (int32)sizeof(Label)));

	// FRandomStream has no byte-buffer Initialize overload; fold the 32-byte
	// seed into an int32 with a CRC (deterministic across Save/Load).
	const int32 SeedValue = (int32)FCrc::MemCrc32(Seed.GetData(), Seed.Num());
	FRandomStream Stream(SeedValue);
		TArray<uint8> Perm;
		Perm.SetNum(256);
		for (int32 i = 0; i < 256; ++i)
		{
			Perm[i] = (uint8)i;
		}
		for (int32 i = 219; i >= 1; --i)
		{
			const int32 J = Stream.RandHelper(i + 1);
			Swap(Perm[i], Perm[J]);
		}
		return Perm;
	}

#endif

	struct FModKitCacheHeader
	{
		uint16 Flags = 0;
		TArray<uint8> KeyIdBytes;
		TArray<uint8> HeaderFragment;
		TArray<uint8> Salt;
		TArray<uint8> PayloadHmac;
		uint32 PayloadLen = 0;
		int32 HeaderSize = 0;
	};

	TArray<uint8> BuildHeaderBytes(const FPolicy& Policy, const TArray<uint8>& Salt, const TArray<uint8>& Hmac, uint32 PayloadLen, uint16 Flags)
	{
		TArray<uint8> Out;
		AppendU32LE(Out, MkccMagic);
		AppendU16LE(Out, MkccVersion);
		AppendU16LE(Out, Flags);
		FTCHARToUTF8 KeyIdBytes(*Policy.KeyId);
		const int32 KeyIdLen = FMath::Min(KeyIdBytes.Length(), MaxKeyIdBytes);
		AppendU8(Out, (uint8)KeyIdLen);
		for (int32 i = 0; i < KeyIdLen; ++i)
		{
			Out.Add((uint8)KeyIdBytes.Get()[i]);
		}
		AppendU8(Out, (uint8)FMath::Min(Policy.HeaderFragment.Num(), 32));
		for (int32 i = 0; i < FMath::Min(Policy.HeaderFragment.Num(), 32); ++i)
		{
			Out.Add(Policy.HeaderFragment[i]);
		}
		for (const uint8 Byte : Salt)
		{
			Out.Add(Byte);
		}
		for (const uint8 Byte : Hmac)
		{
			Out.Add(Byte);
		}
		AppendU32LE(Out, PayloadLen);
		return Out;
	}

	bool ParseHeader(const TArray<uint8>& Bytes, FModKitCacheHeader& OutHeader)
	{
		if (Bytes.Num() < 4)
		{
			return false;
		}
		if (ReadU32LE(Bytes, 0) != MkccMagic)
		{
			return false;
		}
		int32 Offset = 4;
		if (Bytes.Num() < Offset + 2)
		{
			return false;
		}
		const uint16 Version = ReadU16LE(Bytes, Offset);
		Offset += 2;
		if (Version != MkccVersion)
		{
			return false;
		}
		if (Bytes.Num() < Offset + 2)
		{
			return false;
		}
		const uint16 Flags = ReadU16LE(Bytes, Offset);
		Offset += 2;
		if (Bytes.Num() < Offset + 1)
		{
			return false;
		}
		const uint8 KeyIdLen = ReadU8(Bytes, Offset);
		Offset += 1;
		if (KeyIdLen > MaxKeyIdBytes || Bytes.Num() < Offset + KeyIdLen)
		{
			return false;
		}
		OutHeader.KeyIdBytes = TArray<uint8>(Bytes.GetData() + Offset, KeyIdLen);
		Offset += KeyIdLen;
		if (Bytes.Num() < Offset + 1)
		{
			return false;
		}
		const uint8 FragmentLen = ReadU8(Bytes, Offset);
		Offset += 1;
		if (FragmentLen > 32 || Bytes.Num() < Offset + FragmentLen)
		{
			return false;
		}
		OutHeader.HeaderFragment = TArray<uint8>(Bytes.GetData() + Offset, FragmentLen);
		Offset += FragmentLen;
		if (Bytes.Num() < Offset + 16 + 32 + 4)
		{
			return false;
		}
		OutHeader.Salt = TArray<uint8>(Bytes.GetData() + Offset, 16);
		Offset += 16;
		OutHeader.PayloadHmac = TArray<uint8>(Bytes.GetData() + Offset, 32);
		Offset += 32;
		OutHeader.PayloadLen = ReadU32LE(Bytes, Offset);
		Offset += 4;
		if (uint64(Bytes.Num()) != uint64(Offset) + OutHeader.PayloadLen)
		{
			return false;
		}
		OutHeader.Flags = Flags;
		OutHeader.HeaderSize = Offset;
		return true;
	}

	bool KeysMatch(const FPolicy& Policy, const TArray<uint8>& KeyIdBytes)
	{
		FTCHARToUTF8 PolicyKeyId(*Policy.KeyId);
		return PolicyKeyId.Length() == KeyIdBytes.Num()
			&& (KeyIdBytes.Num() == 0 || FMemory::Memcmp(PolicyKeyId.Get(), KeyIdBytes.GetData(), KeyIdBytes.Num()) == 0);
	}

	bool ShouldRejectPlaintext(const UModKitSettings& Settings)
	{
		return Settings.bEncryptScriptCache;
	}

	bool ComputeDiskTextHash(const TArray<uint8>& FileBytes, TArray<uint8>& OutHash)
	{
		// Minimal PE reader: DOS e_lfanew -> NT headers -> section table -> ".text"
		// raw bytes. Only the backing file bytes are hashed (MK-Q02: disk .text,
		// immune to ASLR/relocations).
		if (FileBytes.Num() < 0x40)
		{
			return false;
		}
		const uint32 PeOffset = ReadU32LE(FileBytes, 0x3C);
		if (PeOffset + 24 > (uint32)FileBytes.Num())
		{
			return false;
		}
		if (FileBytes[PeOffset] != 'P' || FileBytes[PeOffset + 1] != 'E'
			|| FileBytes[PeOffset + 2] != 0 || FileBytes[PeOffset + 3] != 0)
		{
			return false;
		}
		const uint16 NumSections = ReadU16LE(FileBytes, PeOffset + 6);
		const uint16 OptSize = ReadU16LE(FileBytes, PeOffset + 20);
		const uint32 FirstSection = PeOffset + 24 + OptSize;
		for (uint16 i = 0; i < NumSections; ++i)
		{
			const uint32 SectionOffset = FirstSection + (uint32)i * 40;
			if (SectionOffset + 40 > (uint32)FileBytes.Num())
			{
				return false;
			}
			char Name[9] = { 0 };
			FMemory::Memcpy(Name, FileBytes.GetData() + SectionOffset, 8);
			if (FCStringAnsi::Strcmp(Name, ".text") != 0)
			{
				continue;
			}
			const uint32 RawDataSize = ReadU32LE(FileBytes, SectionOffset + 16);
			const uint32 RawDataOffset = ReadU32LE(FileBytes, SectionOffset + 20);
			if (RawDataSize == 0 || RawDataOffset == 0)
			{
				return false;
			}
			if ((uint64)RawDataOffset + RawDataSize > (uint64)FileBytes.Num())
			{
				return false;
			}
			uint8 Digest[32];
			FModKitSha256::HashBuffer(FileBytes.GetData() + RawDataOffset, RawDataSize, Digest);
			OutHash.SetNum(32);
			FMemory::Memcpy(OutHash.GetData(), Digest, 32);
			return true;
		}
		return false;
	}

	bool ComputeRuntimeModuleHash(const FString& RuntimeModuleName, TArray<uint8>& OutHash)
	{
#if PLATFORM_WINDOWS
		HMODULE Module = nullptr;
		// Resolve the loaded module by config-suffixed name first, then the bare
		// module name, then the main executable (monolithic degradation).
		const FString Platform = FString(FPlatformMisc::GetUBTPlatform());
		const FString Config = FString(FModuleManager::GetUBTConfiguration());
		TArray<FString> Candidates;
		if (!Platform.IsEmpty() && !Config.IsEmpty())
		{
			Candidates.Add(RuntimeModuleName + TEXT("-") + Platform + TEXT("-") + Config);
		}
		Candidates.Add(RuntimeModuleName);
		for (const FString& Candidate : Candidates)
		{
			Module = GetModuleHandleW(*Candidate);
			if (Module != nullptr)
			{
				break;
			}
		}
		if (Module == nullptr)
		{
			Module = GetModuleHandleW(nullptr);
		}
		if (Module == nullptr)
		{
			return false;
		}

		TCHAR ModulePath[MAX_PATH] = { 0 };
		const DWORD PathLen = GetModuleFileNameW(Module, ModulePath, MAX_PATH);
		if (PathLen == 0)
		{
			return false;
		}

		TArray<uint8> FileBytes;
		if (!FFileHelper::LoadFileToArray(FileBytes, ModulePath))
		{
			return false;
		}
		return ComputeDiskTextHash(FileBytes, OutHash);
#else
		return false;
#endif
	}

	bool DefaultRuntimeHashProvider(TArray<uint8>& OutHash)
	{
		const UModKitSettings* Settings = UModKitSettings::Get();
		return ComputeRuntimeModuleHash(Settings->RuntimeModuleName, OutHash);
	}

	bool GetRuntimeHash(TArray<uint8>& OutHash)
	{
		// Register-early, compute-lazily (S1): the runtime module DLL is not mapped at ModKit
		// startup, so the first consumer-side KDF computes and caches the hash.
		if (!bRuntimeHashComputed)
		{
			bRuntimeHashComputed = true;
			bRuntimeHashValid = GRuntimeHashProvider != nullptr && GRuntimeHashProvider(CachedRuntimeHash);
		}
		if (!bRuntimeHashValid)
		{
			return false;
		}
		OutHash = CachedRuntimeHash;
		return true;
	}

	bool TransformSave(FPolicy& Policy, TArray<uint8>& Bytes)
	{
		if (!Policy.bTransform)
		{
			return true;
		}
		FModKitKeystoreEntry Entry;
		if (!ResolveKeystoreEntry(Policy, Entry))
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_SAVE_FAILED reason=keystore-unavailable keyId=%s"), *Policy.KeyId);
			return false;
		}

        FTCHARToUTF8 KeyIdBytes(*Policy.KeyId);
        if (KeyIdBytes.Length() < 1 || KeyIdBytes.Length() > MaxKeyIdBytes || Policy.HeaderFragment.Num() > 32)
            return false;
		const TArray<uint8> Salt = FModKitCacheCipher::GenerateSalt();
		if (Salt.Num() != FModKitCacheCipher::SaltSizeBytes)
		{
			Bytes.Reset();
			return false;
		}
		TArray<uint8> Key = FModKitCacheCipher::DeriveKey(
			Entry.BinaryFragments, Entry.HeaderFragment, Salt, Entry.ExpectedRuntimeHash, Entry.KeyId);
		ON_SCOPE_EXIT { FMemory::Memzero(Key.GetData(), Key.Num()); };
		FModKitCacheCipher::EncryptCtr(Bytes, Key, Salt);


		const uint16 Flags = FlagAes;

		TArray<uint8> ZeroMac;
		ZeroMac.SetNumZeroed(FModKitCacheCipher::HmacSizeBytes);
		TArray<uint8> Header = BuildHeaderBytes(Policy, Salt, ZeroMac, Bytes.Num(), Flags);
		const int32 MacOffset = Header.Num() - 4 - FModKitCacheCipher::HmacSizeBytes;
		Header.Append(Bytes);
		const TArray<uint8> Hmac = FModKitCacheCipher::HmacSha256(Key, Header);
		FMemory::Memcpy(Header.GetData() + MacOffset, Hmac.GetData(), Hmac.Num());
		Bytes = MoveTemp(Header);

		UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_CACHE_SAVE_ENCRYPTED keyId=%s bytes=%d flags=%d"), *Entry.KeyId, Bytes.Num(), (int32)Flags);
		return true;
	}

	bool TransformLoad(FPolicy& Policy, TArray<uint8>& Bytes, const UModKitSettings* Settings)
	{

		if (Bytes.Num() < 4 || ReadU32LE(Bytes, 0) != MkccMagic)
		{
			// Development cache generation can start from plaintext; encrypted
			// delivery consumers reject it according to their load policy.
			if (ShouldRejectPlaintext(*Settings))
			{
				UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_PLAINTEXT_REJECTED keyId=%s"), *Policy.KeyId);
				Bytes.Reset();
				return false;
			}
			return true;
		}

		if (!Policy.bTransform)
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_MISMATCH reason=no-keystore"));
			Bytes.Reset();
			return false;
		}

		FModKitCacheHeader Header;
		if (!ParseHeader(Bytes, Header))
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_HEADER_MALFORMED"));
			Bytes.Reset();
			return false;
		}

        if (Header.Flags != FlagAes)
        {
            UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_UNSUPPORTED_FLAGS flags=%u"), Header.Flags);
            Bytes.Reset();
            return false;
        }

		if (!KeysMatch(Policy, Header.KeyIdBytes))
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_MISMATCH reason=key-id expected=%s"), *Policy.KeyId);
			Bytes.Reset();
			return false;
		}

		FModKitKeystoreEntry Entry;
		if (!ResolveKeystoreEntry(Policy, Entry))
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_MISMATCH reason=no-keystore keyId=%s"), *Policy.KeyId);
			Bytes.Reset();
			return false;
		}

		if (Settings->bBindCacheToRuntimeHash)
		{
			// MK-Q02 explicit comparison (primary judgment): swap the runtime binary and the
			// disk .text hash diverges from the registered fingerprint.
			TArray<uint8> ActualHash;
			if (!GetRuntimeHash(ActualHash) || ActualHash.Num() != 32 || ActualHash != Entry.ExpectedRuntimeHash)
			{
				UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_MISMATCH reason=runtime-hash keyId=%s"), *Entry.KeyId);
				Bytes.Reset();
				return false;
			}
		}

		TArray<uint8> Key = FModKitCacheCipher::DeriveKey(
			Entry.BinaryFragments, Entry.HeaderFragment, Header.Salt, Entry.ExpectedRuntimeHash, Entry.KeyId);
		ON_SCOPE_EXIT { FMemory::Memzero(Key.GetData(), Key.Num()); };

		TArray<uint8> Ciphertext(Bytes.GetData() + Header.HeaderSize, (int32)Header.PayloadLen);

		TArray<uint8> AuthenticatedBytes = Bytes;
		FMemory::Memzero(AuthenticatedBytes.GetData() + Header.HeaderSize - 4 - FModKitCacheCipher::HmacSizeBytes,
			FModKitCacheCipher::HmacSizeBytes);
		const TArray<uint8> ExpectedHmac = FModKitCacheCipher::HmacSha256(Key, AuthenticatedBytes);
		uint8 MacDifference = 0;
		for (int32 Index = 0; Index < ExpectedHmac.Num(); ++Index)
		{
			MacDifference |= ExpectedHmac[Index] ^ Header.PayloadHmac[Index];
		}
		if (MacDifference != 0)
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_MISMATCH reason=payload keyId=%s"), *Entry.KeyId);
			Bytes.Reset();
			return false;
		}

		FModKitCacheCipher::DecryptCtr(Ciphertext, Key, Header.Salt);
		Bytes = MoveTemp(Ciphertext);

		UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_CACHE_LOAD_OK keyId=%s bytes=%d"), *Entry.KeyId, Bytes.Num());
		return true;
	}
    struct FCacheSnapshot
    {
        TArray<uint8> Bytes;
        ~FCacheSnapshot() { FMemory::Memzero(Bytes.GetData(), Bytes.Num()); }
    };

    class FCacheMemoryHandle final : public IFileHandle
    {
        TSharedRef<const FCacheSnapshot, ESPMode::ThreadSafe> Snapshot;
        int64 Position = 0;
    public:
        explicit FCacheMemoryHandle(TSharedRef<const FCacheSnapshot, ESPMode::ThreadSafe> InSnapshot) : Snapshot(InSnapshot) {}
        virtual int64 Tell() override { return Position; }
        virtual int64 Size() override { return Snapshot->Bytes.Num(); }
        virtual bool Seek(int64 Offset) override
        {
            if (Offset < 0 || Offset > Size()) return false;
            Position = Offset;
            return true;
        }
        virtual bool SeekFromEnd(int64 Offset = 0) override { return Offset <= 0 && Offset >= -Size() && Seek(Size() + Offset); }
        // ReadAt is pure virtual in 5.8 and an ordinary method on older profiles.
        virtual bool ReadAt(uint8* Destination, int64 Count, int64 Offset)
        {
            if (Offset < 0 || Count < 0 || Offset > Size() || Count > Size() - Offset || (Count && !Destination)) return false;
            if (Count) FMemory::Memcpy(Destination, Snapshot->Bytes.GetData() + Offset, Count);
            return true;
        }
        virtual bool Read(uint8* Destination, int64 Count) override
        {
            if (!ReadAt(Destination, Count, Position)) return false;
            Position += Count;
            return true;
        }
        virtual bool Write(const uint8*, int64) override { return false; }
        virtual bool Flush(bool = false) override { return false; }
        virtual bool Truncate(int64) override { return false; }
    };

    FString CachePathIdentity(const TCHAR* Filename)
    {
        FString Path = FPaths::ConvertRelativePathToFull(Filename);
        FPaths::NormalizeFilename(Path);
        FPaths::CollapseRelativeDirectories(Path);
#if PLATFORM_WINDOWS
        Path.ToLowerInline();
#endif
        return Path;
    }

    // Retained for process lifetime. All reads use the chain captured at PreDefault,
    // including Pak/IoStore wrappers, rather than the physical platform file.
    class FCachePlatformFile final : public IPlatformFile
    {
        IPlatformFile* Lower = nullptr;
        FCriticalSection Mutex;
        TMap<FString, TSharedPtr<const FCacheSnapshot, ESPMode::ThreadSafe>> Snapshots;
        TSharedPtr<const FCacheSnapshot, ESPMode::ThreadSafe> Snapshot(const TCHAR* Filename)
        {
            FScopeLock Lock(&Mutex);
            auto* Found = Snapshots.Find(CachePathIdentity(Filename));
            if (!Found) return nullptr;
            if (FModKitMemoryIntegrity::IsBreached())
                UE_LOG(LogAngelscriptModKit, Fatal, TEXT("MODKIT_CACHE_TRANSFORM_BLOCKED reason=integrity-breach"));
            return *Found;
        }
        bool Target(const TCHAR* Filename) const { return Snapshots.Contains(CachePathIdentity(Filename)); }
    public:
        virtual const TCHAR* GetName() const override { return TEXT("ModKitCacheEnvelope"); }
        virtual bool Initialize(IPlatformFile* Inner, const TCHAR*) override { Lower = Inner; return Inner != nullptr; }
        virtual IPlatformFile* GetLowerLevel() override { return Lower; }
        virtual void SetLowerLevel(IPlatformFile* Inner) override { Lower = Inner; }
        void Freeze(const FString& Filename)
        {
            const FString Path = CachePathIdentity(*Filename);
            TSharedPtr<const FCacheSnapshot, ESPMode::ThreadSafe> Result;
            // Authenticate every byte before publishing existence or a handle.
            TUniquePtr<IFileHandle> Source(Lower->OpenRead(*Filename));
            const int64 SourceSize = Source ? Source->Size() : -1;
            if (SourceSize >= 0 && SourceSize <= MAX_int32)
            {
                auto Candidate = MakeShared<FCacheSnapshot, ESPMode::ThreadSafe>();
                Candidate->Bytes.SetNumUninitialized(int32(SourceSize));
                FPolicy Policy;
                if (Source->Read(Candidate->Bytes.GetData(), Candidate->Bytes.Num())
                    && FModKitCacheTransform::GetTransformPolicy(EDirection::Load, Policy)
                    && FModKitCacheTransform::TransformCacheBytes(EDirection::Load, Policy, Candidate->Bytes))
                    Result = Candidate;
                if (!Result)
                    UE_LOG(LogAngelscriptModKit, Fatal, TEXT("MODKIT_CACHE_VIEW_REJECTED: cache authentication failed; no source fallback"));
            }
            if (!Result && Lower->FileExists(*Filename))
                UE_LOG(LogAngelscriptModKit, Fatal, TEXT("MODKIT_CACHE_VIEW_REJECTED: unreadable cache; no source fallback"));
            // Missing files are also frozen; never expose ciphertext.
            Snapshots.Add(Path, Result);
            // Runtime canonicalizes its first Script root. Resolve aliases once
            // at install, never probe the filesystem on non-target read paths.
            std::error_code Error;
#if PLATFORM_WINDOWS
            const auto Canonical = std::filesystem::weakly_canonical(std::filesystem::path(*Filename), Error);
            if (!Error) Snapshots.Add(CachePathIdentity(Canonical.native().c_str()), Result);
#else
            const auto Canonical = std::filesystem::weakly_canonical(std::filesystem::path(TCHAR_TO_UTF8(*Filename)), Error);
            if (!Error) Snapshots.Add(CachePathIdentity(UTF8_TO_TCHAR(Canonical.native().c_str())), Result);
#endif
        }
        virtual bool FileExists(const TCHAR* P) override { return Target(P) ? Snapshot(P).IsValid() : Lower->FileExists(P); }
        virtual int64 FileSize(const TCHAR* P) override
        {
            if (!Target(P)) return Lower->FileSize(P);
            auto Data = Snapshot(P); return Data ? Data->Bytes.Num() : -1;
        }
        virtual IFileHandle* OpenRead(const TCHAR* P, bool AllowWrite = false) override
        {
            if (!Target(P)) return Lower->OpenRead(P, AllowWrite);
            auto Data = Snapshot(P); return Data ? new FCacheMemoryHandle(Data.ToSharedRef()) : nullptr;
        }
        virtual IFileHandle* OpenReadNoBuffering(const TCHAR* P, bool AllowWrite = false) override
        { return Target(P) ? OpenRead(P, AllowWrite) : Lower->OpenReadNoBuffering(P, AllowWrite); }
// SOLOAS-COMPAT-BEGIN(<5.6) Older platform files use the one-argument async read [test: ModKit cache view] [id: CS.Compatibility.ModKit.ModKitCacheTransform.AsyncRead]
#if UE_VERSION_OLDER_THAN(5, 6, 0)
        virtual IAsyncReadFileHandle* OpenAsyncRead(const TCHAR* P) override
        { return Target(P) ? nullptr : Lower->OpenAsyncRead(P); }
#else
        virtual IAsyncReadFileHandle* OpenAsyncRead(const TCHAR* P, bool AllowWrite = false) override
        { return Target(P) ? nullptr : Lower->OpenAsyncRead(P, AllowWrite); }
#endif
// SOLOAS-COMPAT-END
// SOLOAS-COMPAT-BEGIN(<5.8) Older platform files expose the legacy mapped read [test: ModKit cache view] [id: CS.Compatibility.ModKit.ModKitCacheTransform.MappedRead]
#if UE_VERSION_OLDER_THAN(5, 8, 0)
        virtual IMappedFileHandle* OpenMapped(const TCHAR* P) override
        { return Target(P) ? nullptr : Lower->OpenMapped(P); }
#endif
// SOLOAS-COMPAT-END
// SOLOAS-COMPAT-BEGIN(<5.5) Older platform files do not expose flags-based reads [test: ModKit cache view] [id: CS.Compatibility.ModKit.ModKitCacheTransform.ReadFlags]
#if UE_VERSION_NEWER_THAN(5, 4, 99)
        virtual FFileOpenResult OpenRead(const TCHAR* P, EOpenReadFlags Flags) override
        {
            if (!Target(P)) return Lower->OpenRead(P, Flags);
            TUniquePtr<IFileHandle> Handle(OpenRead(P, false));
            if (Handle) return MakeValue(MoveTemp(Handle));
            return MakeError(FFileSystemError(FString(TEXT("ModKit cache authentication failed"))));
        }
#endif
// SOLOAS-COMPAT-END
// SOLOAS-COMPAT-BEGIN(<5.7) Older platform files do not expose flags-based unbuffered reads and writes [test: ModKit cache view] [id: CS.Compatibility.ModKit.ModKitCacheTransform.UnbufferedWriteFlags]
#if UE_VERSION_NEWER_THAN(5, 6, 99)
        virtual FFileOpenResult OpenReadNoBuffering(const TCHAR* P, EOpenReadFlags Flags) override
        { return Target(P) ? OpenRead(P, Flags) : Lower->OpenReadNoBuffering(P, Flags); }
        virtual FFileOpenResult OpenWrite(const TCHAR* P, EOpenWriteFlags Flags) override
        { return Target(P) ? FFileOpenResult(MakeError(FFileSystemError(FString(TEXT("ModKit cache view is immutable"))))) : Lower->OpenWrite(P, Flags); }
#endif
// SOLOAS-COMPAT-END
// SOLOAS-COMPAT-BEGIN(<5.6) Older platform files do not expose flags-based async and mapped reads [test: ModKit cache view] [id: CS.Compatibility.ModKit.ModKitCacheTransform.AsyncMappedFlags]
#if UE_VERSION_NEWER_THAN(5, 5, 99)
        virtual FFileOpenAsyncResult OpenAsyncRead(const TCHAR* P, EOpenReadFlags Flags) override
        { return Target(P) ? FFileOpenAsyncResult(MakeError(FFileSystemError(FString(TEXT("ModKit cache async view unsupported"))))) : Lower->OpenAsyncRead(P, Flags); }
        virtual FOpenMappedResult OpenMappedEx(const TCHAR* P, EOpenReadFlags Flags = EOpenReadFlags::None, int64 Size = 0) override
        { return Target(P) ? FOpenMappedResult(MakeError(FFileSystemError(FString(TEXT("ModKit cache mapped view unsupported"))))) : Lower->OpenMappedEx(P, Flags, Size); }
#endif
// SOLOAS-COMPAT-END
// SOLOAS-COMPAT-BEGIN(<5.8) UE 5.8 replaces the mapped read flags overload [test: ModKit cache view] [id: CS.Compatibility.ModKit.ModKitCacheTransform.MappedReadFlags]
#if UE_VERSION_NEWER_THAN(5, 7, 99)
        virtual FOpenMappedResult OpenMappedEx2(const TCHAR* P, EOpenMappedFlags Flags = EOpenMappedFlags::None, int64 Size = 0) override
        { return Target(P) ? FOpenMappedResult(MakeError(FFileSystemError(FString(TEXT("ModKit cache mapped view unsupported"))))) : Lower->OpenMappedEx2(P, Flags, Size); }
#endif
// SOLOAS-COMPAT-END
        virtual IFileHandle* OpenWrite(const TCHAR* P, bool Append = false, bool AllowRead = false) override
        { return Target(P) ? nullptr : Lower->OpenWrite(P, Append, AllowRead); }
        virtual bool DeleteFile(const TCHAR* P) override { return !Target(P) && Lower->DeleteFile(P); }
        virtual bool MoveFile(const TCHAR* To, const TCHAR* From) override { return !Target(To) && !Target(From) && Lower->MoveFile(To, From); }
        virtual bool IsReadOnly(const TCHAR* P) override { return Target(P) || Lower->IsReadOnly(P); }
        virtual bool SetReadOnly(const TCHAR* P, bool Value) override { return !Target(P) && Lower->SetReadOnly(P, Value); }
        virtual FDateTime GetTimeStamp(const TCHAR* P) override { return Lower->GetTimeStamp(P); }
        virtual void SetTimeStamp(const TCHAR* P, FDateTime Time) override { if (!Target(P)) Lower->SetTimeStamp(P, Time); }
        virtual FDateTime GetAccessTimeStamp(const TCHAR* P) override { return Lower->GetAccessTimeStamp(P); }
        virtual FString GetFilenameOnDisk(const TCHAR* P) override { return Lower->GetFilenameOnDisk(P); }
        virtual bool DirectoryExists(const TCHAR* P) override { return Lower->DirectoryExists(P); }
        virtual bool CreateDirectory(const TCHAR* P) override { return Lower->CreateDirectory(P); }
        virtual bool DeleteDirectory(const TCHAR* P) override { return Lower->DeleteDirectory(P); }
        virtual FFileStatData GetStatData(const TCHAR* P) override
        {
            auto Stat = Lower->GetStatData(P);
            if (Target(P)) { Stat.FileSize = FileSize(P); Stat.bIsValid = Stat.FileSize >= 0; Stat.bIsReadOnly = true; }
            return Stat;
        }
        virtual bool IterateDirectory(const TCHAR* P, FDirectoryVisitor& V) override { return Lower->IterateDirectory(P, V); }
        virtual bool IterateDirectoryStat(const TCHAR* P, FDirectoryStatVisitor& V) override { return Lower->IterateDirectoryStat(P, V); }
    };

    void InstallCacheFileView()
    {
        const auto* Settings = UModKitSettings::Get();
        if (!Settings->bEncryptScriptCache || FParse::Param(FCommandLine::Get(), TEXT("as-generate-precompiled-data"))) return;
        if (FModuleManager::Get().IsModuleLoaded(TEXT("AngelscriptLoader")))
            UE_LOG(LogAngelscriptModKit, Fatal, TEXT("ModKit encrypted cache view must precede AngelscriptLoader"));
        FString Commandlet;
        if (FParse::Value(FCommandLine::Get(), TEXT("run="), Commandlet) && Commandlet == TEXT("ModKitSealScriptCache")) return;
        FPolicy Policy;
        if (!FModKitCacheTransform::GetTransformPolicy(EDirection::Load, Policy) || Policy.bBlocked)
            UE_LOG(LogAngelscriptModKit, Fatal, TEXT("MODKIT_CACHE_VIEW_REJECTED: unsupported or unavailable envelope policy"));
        auto* View = new FCachePlatformFile();
        auto& Manager = FPlatformFileManager::Get();
        View->Initialize(&Manager.GetPlatformFile(), FCommandLine::Get());
        for (const TCHAR* Name : {TEXT("PrecompiledScript.Cache"), TEXT("PrecompiledScript_Development.Cache"),
            TEXT("PrecompiledScript_Test.Cache"), TEXT("PrecompiledScript_Shipping.Cache")})
            View->Freeze(FPaths::ProjectDir() / TEXT("Script") / Name);
        Manager.SetPlatformFile(*View);
        GCacheViewInstalled = true;
        if (Manager.FindPlatformFile(View->GetName()) != View)
            UE_LOG(LogAngelscriptModKit, Fatal, TEXT("ModKit encrypted cache file chain installation failed"));
    }

}

void FModKitCacheTransform::Startup()
{
	GRuntimeHashProvider = &DefaultRuntimeHashProvider;

    InstallCacheFileView();

	GenerateKeyCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("ModKit.GenerateScriptCacheKey"),
		TEXT("Computes the disk .text SHA-256 of a runtime module DLL and writes a Build/ScriptCacheKey.json entry (game-build -> keygen -> cache-generate)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FModKitCacheTransform::ExecGenerateScriptCacheKey),
		ECVF_Default);
}

bool FModKitCacheTransform::RetainsFileView()
{
    return GCacheViewInstalled;
}

void FModKitCacheTransform::Shutdown()
{
	if (GenerateKeyCommand != nullptr)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GenerateKeyCommand);
		GenerateKeyCommand = nullptr;
	}
    // The file layer and any live handles retain module code until process exit.
    // Do not edit the platform-file chain during shutdown.

	GRuntimeHashProvider = nullptr;
	bRuntimeHashComputed = false;
	bRuntimeHashValid = false;
	CachedRuntimeHash.Reset();
}

bool FModKitCacheTransform::GetTransformPolicy(EDirection Direction, FPolicy& OutPolicy)
{
	const UModKitSettings* Settings = UModKitSettings::Get();

    // A breached process cannot create or open another cache envelope.
	if (FModKitMemoryIntegrity::IsBreached())
	{
		OutPolicy = FPolicy();
		OutPolicy.bTransform = true;
		OutPolicy.bBlocked = true;
		OutPolicy.KeyId = Settings->ScriptCacheKeyId;
		return true;
	}

	FModKitKeystoreEntry Entry;
	const bool bHasKeystore = !Settings->ScriptCacheKeyId.IsEmpty()
		&& FModKitCacheKeystore::FindEntry(FModKitCacheKeystore::GetDefaultPath(), Settings->ScriptCacheKeyId, Entry);

	OutPolicy = FPolicy();
    if (Settings->bObfuscateOpcode || Settings->bStripLineNumbersOnCache || Settings->bStripBytecodeForJitModules)
    {
        OutPolicy.bBlocked = true;
        UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_POLICY_UNSUPPORTED opcode/debug/bytecode stripping requires a serializer contract"));
        return false;
    }

	if (Direction == EDirection::Save && !Settings->bEncryptScriptCache)
	{
		return true;
	}
	if (!bHasKeystore)
	{
		if (Settings->bEncryptScriptCache)
		{
			OutPolicy.bTransform = true;
			OutPolicy.bBlocked = true;
			OutPolicy.KeyId = Settings->ScriptCacheKeyId;
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_MISMATCH reason=no-keystore"));
		}
		return true;
	}

	OutPolicy.bTransform = OutPolicy.bTransform
		|| ((Direction == EDirection::Save) ? Settings->bEncryptScriptCache : true);
	OutPolicy.KeyId = Entry.KeyId;
	OutPolicy.RuntimeModule = Entry.RuntimeModule;
	OutPolicy.HeaderFragment = Entry.HeaderFragment;
	OutPolicy.ExpectedRuntimeHash = Entry.ExpectedRuntimeHash;

	return true;
}

bool FModKitCacheTransform::TransformCacheBytes(EDirection Direction, FPolicy& Policy, TArray<uint8>& Bytes)
{
    // Offline sealing preserves the original file when transformation fails.
	if (Policy.bBlocked)
	{
		UE_LOG(LogAngelscriptModKit, Error,
			TEXT("MODKIT_CACHE_TRANSFORM_BLOCKED reason=integrity-breach keyId=%s direction=%s"),
			*Policy.KeyId, Direction == EDirection::Save ? TEXT("save") : TEXT("load"));
		if (Direction == EDirection::Save)
		{
			Bytes.Reset();
		}
		return false;
	}
	if (Direction == EDirection::Save)
	{
		const bool bSaved = TransformSave(Policy, Bytes);
		if (!bSaved) Bytes.Reset();
		return bSaved;
	}
	return TransformLoad(Policy, Bytes, UModKitSettings::Get());
}

void FModKitCacheTransform::SetRuntimeHashProvider(FModKitRuntimeHashProvider Provider)
{
	GRuntimeHashProvider = MoveTemp(Provider);
	bRuntimeHashComputed = false;
	bRuntimeHashValid = false;
	CachedRuntimeHash.Reset();
}

void FModKitRuntimeHash::SetProvider(FModKitRuntimeHashProvider Provider)
{
	FModKitCacheTransform::SetRuntimeHashProvider(MoveTemp(Provider));
}

bool FModKitRuntimeHash::ComputeDefault(TArray<uint8>& OutHash)
{
	return DefaultRuntimeHashProvider(OutHash);
}

void FModKitCacheTransform::ExecGenerateScriptCacheKey(const TArray<FString>& Args)
{
	FString KeyId;
	FString ModulePath;
	const UModKitSettings* Settings = UModKitSettings::Get();
	FString RuntimeModule = Settings->RuntimeModuleName;
	for (const FString& Arg : Args)
	{
		if (Arg.StartsWith(TEXT("-KeyId="), ESearchCase::CaseSensitive))
		{
			KeyId = Arg.RightChop(7);
		}
		else if (Arg.StartsWith(TEXT("-ModulePath="), ESearchCase::CaseSensitive))
		{
			ModulePath = Arg.RightChop(12);
		}
		else if (Arg.StartsWith(TEXT("-RuntimeModule="), ESearchCase::CaseSensitive))
		{
			RuntimeModule = Arg.RightChop(15);
		}
	}

	if (KeyId.IsEmpty() || ModulePath.IsEmpty())
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_GENERATION_FAILED reason=missing-args usage=-KeyId=<id> -ModulePath=<runtime-dll>"));
		return;
	}

	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *ModulePath))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_GENERATION_FAILED reason=dll-read path=%s"), *ModulePath);
		return;
	}
	TArray<uint8> TextHash;
	if (!ComputeDiskTextHash(FileBytes, TextHash))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_GENERATION_FAILED reason=pe-text-missing path=%s"), *ModulePath);
		return;
	}

	FModKitKeystoreEntry Entry;
	Entry.KeyId = KeyId;
	FModKitKeystoreBinaryFragment Fragment;
	Fragment.Id = TEXT("b1");
	Fragment.Data = FModKitCacheCipher::GenerateRandomBytes(32);
	Entry.BinaryFragments.Add(MoveTemp(Fragment));
	Entry.HeaderFragment = FModKitCacheCipher::GenerateSalt();
	if (Entry.BinaryFragments[0].Data.Num() != 32 || Entry.HeaderFragment.Num() != FModKitCacheCipher::SaltSizeBytes)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_GENERATION_FAILED reason=csprng"));
		return;
	}
	Entry.ExpectedRuntimeHash = TextHash;
	Entry.RuntimeModule = RuntimeModule;
	Entry.CreatedAt = FDateTime::UtcNow().ToString();
	Entry.Comment = TEXT("gitignore. Lose it = this release's cache can never be regenerated; rotate keyId instead.");

	if (!FModKitCacheKeystore::WriteEntry(FModKitCacheKeystore::GetDefaultPath(), Entry))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_KEY_GENERATION_FAILED reason=write path=%s"), *FModKitCacheKeystore::GetDefaultPath());
		return;
	}

	UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_CACHE_KEY_GENERATED keyId=%s runtimeModule=%s runtimeHash=%s"),
		*Entry.KeyId, *Entry.RuntimeModule, *FModKitCacheCipher::BytesToHex(Entry.ExpectedRuntimeHash));
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitPlaintextPolicyTest,
	"AngelscriptModKit.Security.PlaintextPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitPlaintextPolicyTest::RunTest(const FString&)
{
	UModKitSettings* Settings = NewObject<UModKitSettings>();
	Settings->bEncryptScriptCache = true;
	Settings->bForceRejectPlaintextInDev = false;
	AddExpectedError(TEXT("MODKIT_CACHE_PLAINTEXT_REJECTED"), EAutomationExpectedErrorFlags::Contains, 3);
	for (const TArray<uint8>& Input : { TArray<uint8>(), TArray<uint8>{ 1, 2, 3 }, TArray<uint8>{ 1, 2, 3, 4, 5 } })
	{
		TArray<uint8> Bytes = Input;
		FPolicy Policy;
		TestFalse(TEXT("Encrypted delivery rejects plaintext/corruption without a development switch"), TransformLoad(Policy, Bytes, Settings));
		TestTrue(TEXT("Rejected bytes never reach deserialization"), Bytes.IsEmpty());
	}
	Settings->bEncryptScriptCache = false;
	TArray<uint8> Bytes{ 1, 2, 3, 4, 5 };
	const TArray<uint8> Original = Bytes;
	FPolicy Policy;
	TestTrue(TEXT("Source delivery accepts an unencrypted cache"), TransformLoad(Policy, Bytes, Settings));
	TestTrue(TEXT("Plaintext passthrough preserves bytes"), Bytes == Original);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitCacheFileViewTest,
    "AngelscriptModKit.Security.CacheFileView",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext | EAutomationTestFlags::EngineFilter)
bool FModKitCacheFileViewTest::RunTest(const FString&)
{
    auto Snapshot = MakeShared<FCacheSnapshot, ESPMode::ThreadSafe>();
    Snapshot->Bytes = {10, 20, 30};
    FCacheMemoryHandle First(Snapshot), Second(Snapshot);
    TestEqual(TEXT("View exposes plaintext size"), First.Size(), int64(3));
    TestFalse(TEXT("Negative seek fails"), First.Seek(-1));
    TestFalse(TEXT("Seek past end fails"), First.Seek(4));
    uint8 Byte = 0;
    TestTrue(TEXT("Read plaintext"), First.Read(&Byte, 1));
    TestEqual(TEXT("First plaintext byte"), Byte, uint8(10));
    TestEqual(TEXT("Handles own independent cursors"), Second.Tell(), int64(0));
    TestTrue(TEXT("Seek from end"), First.SeekFromEnd(-1));
    TestTrue(TEXT("Read last byte"), First.Read(&Byte, 1));
    TestEqual(TEXT("Last plaintext byte"), Byte, uint8(30));
    TestFalse(TEXT("Read beyond end fails"), First.Read(&Byte, 1));
    TestEqual(TEXT("Failed read preserves cursor"), First.Tell(), int64(3));
    TestFalse(TEXT("View never writes plaintext"), First.Write(&Byte, 1));
    TestFalse(TEXT("View never truncates"), First.Truncate(0));
    TestTrue(TEXT("ReadAt uses snapshot"), Second.ReadAt(&Byte, 1, 1));
    TestEqual(TEXT("ReadAt leaves cursor"), Second.Tell(), int64(0));
    FCachePlatformFile View;
    IPlatformFile& Lower = FPlatformFileManager::Get().GetPlatformFile();
    View.Initialize(&Lower, TEXT(""));
    TestTrue(TEXT("Non-target chain is retained"), View.GetLowerLevel() == &Lower);
    const FString Project = FPaths::GetProjectFilePath();
    TestEqual(TEXT("Non-target existence passes through"), View.FileExists(*Project), Lower.FileExists(*Project));
    TestEqual(TEXT("Non-target size passes through"), View.FileSize(*Project), Lower.FileSize(*Project));
    UModKitSettings* Settings = NewObject<UModKitSettings>();
    Settings->bEncryptScriptCache = true;
    FPolicy Policy;
    Policy.bTransform = true;
    TArray<uint8> Salt, Mac;
    Salt.Init(0, 16); Mac.Init(0, 32);
    AddExpectedError(TEXT("MODKIT_CACHE_UNSUPPORTED_FLAGS"), EAutomationExpectedErrorFlags::Contains, 3);
    for (uint16 Flag : {uint16(3), uint16(5), uint16(0x8001)})
    {
        auto Bytes = BuildHeaderBytes(Policy, Salt, Mac, 0, Flag);
        TestFalse(TEXT("Legacy or unknown flags are never treated as stock plaintext"), TransformLoad(Policy, Bytes, Settings));
        TestTrue(TEXT("Rejected envelope exposes no bytes"), Bytes.IsEmpty());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitOpcodePermutationTest,
	"AngelscriptModKit.Security.OpcodePermutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitOpcodePermutationTest::RunTest(const FString&)
{
	FModKitKeystoreEntry Entry;
	Entry.KeyId = TEXT("opcode-test");
	Entry.HeaderFragment.Init(17, 32);
	Entry.ExpectedRuntimeHash.Init(23, 32);
	const TArray<uint8> Permutation = DeriveOpcodePermutation(Entry);
	TestTrue(TEXT("A key always derives the same opcode map"), Permutation == DeriveOpcodePermutation(Entry));
	TSet<uint8> Seen;
	TArray<uint8> Inverse;
	Inverse.SetNum(256);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		Seen.Add(Permutation[Index]);
		Inverse[Permutation[Index]] = uint8(Index);
		if (Index >= 220) TestEqual(TEXT("Reserved opcodes remain unchanged"), Permutation[Index], uint8(Index));
	}
	TestEqual(TEXT("Opcode map is bijective"), Seen.Num(), 256);
	for (int32 Index = 0; Index < 256; ++Index)
		TestEqual(TEXT("Inverse restores the original opcode"), Inverse[Permutation[Index]], uint8(Index));
	Entry.KeyId += TEXT("-rotated");
	TestTrue(TEXT("Key rotation changes the opcode map"), Permutation != DeriveOpcodePermutation(Entry));
	return true;
}
#endif
