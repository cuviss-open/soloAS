// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch manifest schema implementation .

#include "Shared/Delivery/ModKitPatchManifest.h"

#include "Shared/Crypto/ModKitCacheCipher.h"
#include "Shared/Crypto/ModKitPatchSignature.h"
#include "HAL/CriticalSection.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#else
#include <cstdio>
#endif

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TSharedPtr<FJsonValue> PatchManifestTryGetField(const TSharedPtr<FJsonObject>& Object, const FString& Key)
	{
		return Object.IsValid() ? Object->TryGetField(FStringView(Key)) : nullptr;
	}

	bool PatchManifestTryGetStringField(const TSharedPtr<FJsonObject>& Object, const FString& Key, FString& Out)
	{
		const TSharedPtr<FJsonValue> Value = PatchManifestTryGetField(Object, Key);
		if (!Value.IsValid() || Value->Type != EJson::String)
		{
			return false;
		}
		Out = Value->AsString();
		return true;
	}

	bool PatchManifestIsHex64(const FString& Value)
	{
		if (Value.Len() != 64)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!((Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f')))
			{
				return false;
			}
		}
		return true;
	}

	bool PatchManifestCanonicalField(const FString& Value)
	{
		for (TCHAR C : Value)
		{
			if (C < 32 || C == 127 || C == TCHAR('='))
			{
				return false;
			}
		}
		return true;
	}

	bool PatchManifestToken(const FString& Value)
	{
		if (Value.IsEmpty()) return false;
		for (TCHAR C : Value)
		{
			if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z')
				|| (C >= '0' && C <= '9') || C == '-' || C == '_')) return false;
		}
		return true;
	}

	bool PatchManifestIsValidPatchPath(const FString& Path)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("/"), false);
		if (Parts.Num() == 0) return false;
		for (const FString& Part : Parts)
		{
			if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part.EndsWith(TEXT("."))) return false;
			for (TCHAR C : Part)
			{
				if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z')
					|| (C >= '0' && C <= '9') || C == '-' || C == '_' || C == '.')) return false;
			}
			const FString Stem = Part.Left(Part.Find(TEXT(".")) < 0 ? Part.Len() : Part.Find(TEXT("."))).ToUpper();
			if (Stem == TEXT("CON") || Stem == TEXT("PRN") || Stem == TEXT("AUX") || Stem == TEXT("NUL")
				|| (Stem.Len() == 4 && (Stem.StartsWith(TEXT("COM")) || Stem.StartsWith(TEXT("LPT")))
					&& Stem[3] >= '1' && Stem[3] <= '9')) return false;
		}
		return true;
	}

	bool PatchManifestKnownFields(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> Names)
	{
		for (const auto& Field : Object->Values)
		{
			bool bKnown = false;
			for (const TCHAR* Name : Names) bKnown |= Field.Key == Name;
			if (!bKnown) return false;
		}
		return true;
	}

	bool PatchManifestParseFileEntries(const TSharedPtr<FJsonObject>& Root, FModKitPatchManifest& Out, FString& OutReason)
	{
		const TSharedPtr<FJsonValue> FilesValue = PatchManifestTryGetField(Root, TEXT("files"));
		if (!FilesValue.IsValid() || FilesValue->Type != EJson::Array)
		{
			OutReason = TEXT("files must be a JSON array");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Files = FilesValue->AsArray();
		if (Files.Num() == 0 || Files.Num() > 4096)
		{
			OutReason = TEXT("files must not be empty");
			return false;
		}
		TSet<FString> Paths;
		for (const TSharedPtr<FJsonValue>& FileValue : Files)
		{
			const TSharedPtr<FJsonObject> FileObject = FileValue.IsValid() && FileValue->Type == EJson::Object ? FileValue->AsObject() : nullptr;
			if (!FileObject
				|| !PatchManifestKnownFields(FileObject, { TEXT("path"), TEXT("sha256") }))
			{
				OutReason = TEXT("files entry must be an object");
				return false;
			}
			FModKitPatchFileEntry Entry;
			if (!PatchManifestTryGetStringField(FileObject, TEXT("path"), Entry.Path) || !PatchManifestIsValidPatchPath(Entry.Path))
			{
				OutReason = FString::Printf(TEXT("invalid file path '%s'"), *Entry.Path);
				return false;
			}
			if (!PatchManifestTryGetStringField(FileObject, TEXT("sha256"), Entry.Sha256) || !PatchManifestIsHex64(Entry.Sha256))
			{
				OutReason = FString::Printf(TEXT("invalid file sha256 for '%s'"), *Entry.Path);
				return false;
			}
			if (Paths.Contains(Entry.Path.ToLower()))
			{
				OutReason = TEXT("duplicate file path");
				return false;
			}
			Paths.Add(Entry.Path.ToLower());
			Out.Files.Add(MoveTemp(Entry));
		}
		return true;
	}

	bool PatchManifestParsePrecheck(const TSharedPtr<FJsonObject>& Root, FModKitPatchManifest& Out, FString& OutReason)
	{
		const TSharedPtr<FJsonValue> PrecheckValue = PatchManifestTryGetField(Root, TEXT("precheck"));
		if (!PrecheckValue.IsValid() || PrecheckValue->IsNull())
		{
			// precheck is a gate field, not a schema field: a missing record is
			// rejected by the precheck gate (reason=precheck-required), never by
			// the schema validation. The canonical emits empty precheck lines.
			return true;
		}
		if (PrecheckValue->Type != EJson::Object)
		{
			OutReason = TEXT("precheck must be a JSON object");
			return false;
		}
		const TSharedPtr<FJsonObject> Precheck = PrecheckValue->AsObject();
		if (!PatchManifestKnownFields(Precheck, { TEXT("verdict"), TEXT("reason"), TEXT("engineVersion"), TEXT("baselineSha256") }))
		{
			OutReason = TEXT("unknown precheck field");
			return false;
		}
		FString Verdict;
		if (!PatchManifestTryGetStringField(Precheck, TEXT("verdict"), Verdict)
			|| (Verdict != TEXT("PASS") && Verdict != TEXT("REJECT")))
		{
			OutReason = TEXT("precheck.verdict must be PASS or REJECT");
			return false;
		}
		Out.Precheck.Verdict = Verdict;
		if (!PatchManifestTryGetStringField(Precheck, TEXT("reason"), Out.Precheck.Reason))
		{
			OutReason = TEXT("precheck.reason must be a string");
			return false;
		}
		if (!PatchManifestTryGetStringField(Precheck, TEXT("engineVersion"), Out.Precheck.EngineVersion))
		{
			OutReason = TEXT("precheck.engineVersion must be a string");
			return false;
		}
		if (!PatchManifestTryGetStringField(Precheck, TEXT("baselineSha256"), Out.Precheck.BaselineSha256)
			|| (Out.Precheck.BaselineSha256.Len() > 0 && !PatchManifestIsHex64(Out.Precheck.BaselineSha256)))
		{
			OutReason = TEXT("precheck.baselineSha256 must be a 64-hex string");
			return false;
		}
		return true;
	}

	bool PatchManifestParseSignature(const TSharedPtr<FJsonObject>& Root, FModKitPatchManifest& Out, FString& OutReason)
	{
		const TSharedPtr<FJsonValue> SignatureValue = PatchManifestTryGetField(Root, TEXT("signature"));
		if (!SignatureValue.IsValid() || SignatureValue->Type != EJson::Object)
		{
			OutReason = TEXT("signature must be a JSON object");
			return false;
		}
		const TSharedPtr<FJsonObject> Signature = SignatureValue->AsObject();
		if (!PatchManifestKnownFields(Signature, { TEXT("algorithm"), TEXT("payloadSha256"), TEXT("value"), TEXT("signer"), TEXT("keyId"), TEXT("publicKeySha256") }))
		{
			OutReason = TEXT("unknown signature field");
			return false;
		}
		if (!PatchManifestTryGetStringField(Signature, TEXT("algorithm"), Out.SignatureAlgorithm)
			|| Out.SignatureAlgorithm != FModKitPatchManifest::GetSignatureAlgorithmEd25519())
		{
			OutReason = TEXT("signature.algorithm must be ed25519");
			return false;
		}
		if (!PatchManifestTryGetStringField(Signature, TEXT("payloadSha256"), Out.PayloadSha256) || !PatchManifestIsHex64(Out.PayloadSha256))
		{
			OutReason = TEXT("signature.payloadSha256 must be a 64-hex string");
			return false;
		}
		if (!PatchManifestTryGetStringField(Signature, TEXT("signer"), Out.SignatureSigner)
			|| Out.SignatureSigner.IsEmpty() || !PatchManifestCanonicalField(Out.SignatureSigner)
			|| !PatchManifestTryGetStringField(Signature, TEXT("publicKeySha256"), Out.PublicKeySha256)
			|| !PatchManifestIsHex64(Out.PublicKeySha256))
		{
			OutReason = TEXT("signature signer and public key fingerprint are required");
			return false;
		}
		FString KeyId;
		if (Signature->HasField(TEXT("keyId"))
			&& (!PatchManifestTryGetStringField(Signature, TEXT("keyId"), KeyId) || KeyId != Out.SignatureSigner))
		{
			OutReason = TEXT("signature keyId must equal signer");
			return false;
		}
		FString ValueBase64;
		if (!PatchManifestTryGetStringField(Signature, TEXT("value"), ValueBase64)
			|| !FBase64::Decode(ValueBase64, Out.SignatureBytes) || Out.SignatureBytes.Num() != 64)
		{
			OutReason = TEXT("signature.value must be a decodable base64 blob");
			return false;
		}
		return true;
	}
}

const TCHAR* FModKitPatchManifest::GetFormatV1()
{
	return TEXT("ModKitHotPatchManifest/v1");
}

const TCHAR* FModKitPatchManifest::GetSignatureAlgorithmEd25519()
{
	return TEXT("ed25519");
}

bool FModKitPatchManifest::Parse(const FString& JsonText, FModKitPatchManifest& Out, FString& OutReason)
{
	Out = FModKitPatchManifest();
	OutReason.Reset();
	if (!IsJsonWithinBudget(JsonText)) { OutReason = TEXT("manifest-limits"); return false; }

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutReason = TEXT("manifest is not valid JSON");
		return false;
	}

	if (!PatchManifestKnownFields(Root, { TEXT("format"), TEXT("kind"), TEXT("patchId"), TEXT("version"), TEXT("engineVersion"), TEXT("files"), TEXT("precheck"), TEXT("signature") }))
	{
		OutReason = TEXT("unknown manifest field");
		return false;
	}
	FString Format;
	if (!PatchManifestTryGetStringField(Root, TEXT("format"), Format) || Format != GetFormatV1())
	{
		OutReason = TEXT("format must be ModKitHotPatchManifest/v1");
		return false;
	}
	if (Root->HasField(TEXT("kind"))
		&& (!PatchManifestTryGetStringField(Root, TEXT("kind"), Out.Kind)
			|| (Out.Kind != TEXT("hotpatch") && Out.Kind != TEXT("package"))))
	{
		OutReason = TEXT("kind must be hotpatch or package");
		return false;
	}
	if (!PatchManifestTryGetStringField(Root, TEXT("patchId"), Out.PatchId) || !PatchManifestToken(Out.PatchId)
		|| !PatchManifestIsValidPatchPath(Out.PatchId + TEXT(".as")))
	{
		OutReason = TEXT("patchId must be a non-empty string");
		return false;
	}
	if (!PatchManifestTryGetStringField(Root, TEXT("version"), Out.Version) || Out.Version.IsEmpty())
	{
		OutReason = TEXT("version must be a non-empty string");
		return false;
	}
	if (!PatchManifestTryGetStringField(Root, TEXT("engineVersion"), Out.EngineVersion) || Out.EngineVersion.IsEmpty())
	{
		OutReason = TEXT("engineVersion must be a non-empty string");
		return false;
	}

	if (!PatchManifestParseFileEntries(Root, Out, OutReason))
	{
		return false;
	}
	if (Out.Kind != TEXT("package"))
	{
		for (const FModKitPatchFileEntry& Entry : Out.Files)
		{
			if (!Entry.Path.EndsWith(TEXT(".as"), ESearchCase::CaseSensitive))
			{
				OutReason = TEXT("hotpatch files must be .as files");
				return false;
			}
		}
	}
	if (!PatchManifestParsePrecheck(Root, Out, OutReason))
	{
		return false;
	}
	if (!PatchManifestParseSignature(Root, Out, OutReason))
	{
		return false;
	}
	if (!PatchManifestCanonicalField(Out.Version) || !PatchManifestCanonicalField(Out.EngineVersion)
		|| !PatchManifestCanonicalField(Out.Precheck.Reason) || !PatchManifestCanonicalField(Out.Precheck.EngineVersion))
	{
		OutReason = TEXT("non-canonical field");
		return false;
	}
	TArray<FString> VersionParts;
	Out.Version.ParseIntoArray(VersionParts, TEXT("."), false);
	for (const FString& Part : VersionParts)
	{
		if (Part.IsEmpty() || Part.Len() > 9 || (Part.Len() > 1 && Part[0] == '0'))
		{
			OutReason = TEXT("version must be dot-separated canonical integers");
			return false;
		}
		for (TCHAR C : Part) if (C < '0' || C > '9') { OutReason = TEXT("invalid version"); return false; }
	}
	return true;
}

void FModKitPatchManifest::ToCanonicalBytes(TArray<uint8>& OutBytes) const
{
	// Deterministic line format shared with the offline signer and the Python
	// fixture generator: format/patchId/version/engineVersion, then one
	// file.<path>=<sha256> per entry sorted by path, then the precheck lines.
	// Signature identity is bound; signature bytes and payload anchor are excluded.
	FString Canonical;
	Canonical.Append(FString::Printf(TEXT("format=%s\n"), GetFormatV1()));
	if (Kind == TEXT("package")) Canonical.Append(TEXT("kind=package\n"));
	Canonical.Append(FString::Printf(TEXT("patchId=%s\n"), *PatchId));
	Canonical.Append(FString::Printf(TEXT("version=%s\n"), *Version));
	Canonical.Append(FString::Printf(TEXT("engineVersion=%s\n"), *EngineVersion));

	Canonical.Append(FString::Printf(TEXT("signature.algorithm=%s\n"), *SignatureAlgorithm));
	Canonical.Append(FString::Printf(TEXT("signature.signer=%s\n"), *SignatureSigner));
	Canonical.Append(FString::Printf(TEXT("signature.publicKeySha256=%s\n"), *PublicKeySha256));
	TArray<FModKitPatchFileEntry> SortedFiles = Files;
	SortedFiles.Sort([](const FModKitPatchFileEntry& A, const FModKitPatchFileEntry& B)
	{
		return A.Path.Compare(B.Path, ESearchCase::CaseSensitive) < 0;
	});
	for (const FModKitPatchFileEntry& File : SortedFiles)
	{
		Canonical.Append(FString::Printf(TEXT("file.%s=%s\n"), *File.Path, *File.Sha256));
	}

	Canonical.Append(FString::Printf(TEXT("precheck.verdict=%s\n"), *Precheck.Verdict));
	Canonical.Append(FString::Printf(TEXT("precheck.reason=%s\n"), *Precheck.Reason));
	Canonical.Append(FString::Printf(TEXT("precheck.engineVersion=%s\n"), *Precheck.EngineVersion));
	Canonical.Append(FString::Printf(TEXT("precheck.baselineSha256=%s\n"), *Precheck.BaselineSha256));
	if (Canonical.EndsWith(TEXT("\n")))
	{
		Canonical = Canonical.LeftChop(1);
	}

	FTCHARToUTF8 Converted(*Canonical);
	OutBytes.Reset();
	OutBytes.SetNum(Converted.Length());
	if (Converted.Length() > 0)
	{
		FMemory::Memcpy(OutBytes.GetData(), (const uint8*)Converted.Get(), Converted.Length());
	}
}

FString FModKitPatchManifest::ComputePayloadSha256() const
{
	TArray<uint8> Canonical;
	ToCanonicalBytes(Canonical);

	uint8 Digest[32];
	FModKitSha256::HashBuffer(Canonical.Num() > 0 ? Canonical.GetData() : nullptr, Canonical.Num(), Digest);

	TArray<uint8> DigestBytes;
	DigestBytes.SetNum(32);
	FMemory::Memcpy(DigestBytes.GetData(), Digest, 32);
	return FModKitCacheCipher::BytesToHex(DigestBytes);
}

FString ModKitPatchEngineVersionString()
{
	const FEngineVersion& Version = FEngineVersion::Current();
	return FString::Printf(TEXT("%d.%d.%d-%d %s"), Version.GetMajor(), Version.GetMinor(),
		Version.GetPatch(), Version.GetChangelist(), *Version.GetBranch());
}

bool FModKitPatchManifest::MatchesEngineVersion() const
{
	return EngineVersion == ModKitPatchEngineVersionString();
}

bool FModKitPatchManifest::IsVersionAtLeast(const FString& Other) const
{
	TArray<FString> CurrentParts, OtherParts;
	Version.ParseIntoArray(CurrentParts, TEXT("."), false);
	Other.ParseIntoArray(OtherParts, TEXT("."), false);
	if (OtherParts.Num() == 0) return false;
	for (const FString& Part : OtherParts)
	{
		if (Part.IsEmpty() || Part.Len() > 9 || (Part.Len() > 1 && Part[0] == '0')) return false;
		for (TCHAR C : Part) if (C < '0' || C > '9') return false;
	}
	for (int32 Index = 0; Index < FMath::Max(CurrentParts.Num(), OtherParts.Num()); ++Index)
	{
		const int64 A = CurrentParts.IsValidIndex(Index) ? FCString::Atoi64(*CurrentParts[Index]) : 0;
		const int64 B = OtherParts.IsValidIndex(Index) ? FCString::Atoi64(*OtherParts[Index]) : 0;
		if (A != B) return A > B;
	}
	return true;
}


bool FModKitPatchManifest::ReadBytes(const FString& Path, int64 MaxBytes, TArray<uint8>& Out)
{
	TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformPhysical().OpenRead(*FPaths::ConvertRelativePathToFull(Path)));
	if (!Handle) return false;
	const int64 Size = Handle->Size();
	if (Size < 0 || Size > MaxBytes || Size > MAX_int32) return false;
	Out.SetNumUninitialized(static_cast<int32>(Size));
	return Size == 0 || Handle->Read(Out.GetData(), Size);
}

bool FModKitPatchManifest::ReadText(const FString& Path, FString& Out)
{
	TArray<uint8> Bytes;
	if (!ReadBytes(Path, MaxManifestBytes, Bytes)) return false;
	FFileHelper::BufferToString(Out, Bytes.GetData(), Bytes.Num());
	return IsJsonWithinBudget(Out);
}

bool FModKitPatchManifest::MatchesPublicKey(const uint8* PublicKey) const
{
	uint8 Digest[32];
	FModKitSha256::HashBuffer(PublicKey, 32, Digest);
	TArray<uint8> Hash;
	Hash.Append(Digest, 32);
	return PublicKeySha256 == FModKitCacheCipher::BytesToHex(Hash);
}

bool FModKitPatchManifest::IsJsonWithinBudget(const FString& JsonText)
{
	if (JsonText.Len() > MaxManifestBytes || FTCHARToUTF8(*JsonText).Length() > MaxManifestBytes) return false;
	int32 Depth = 0;
	bool bString = false, bEscape = false;
	for (TCHAR C : JsonText)
	{
		if (bString)
		{
			if (bEscape) bEscape = false;
			else if (C == TCHAR('\\')) bEscape = true;
			else if (C == TCHAR('"')) bString = false;
		}
		else if (C == TCHAR('"')) bString = true;
		else if (C == TCHAR('{') || C == TCHAR('['))
		{
			if (++Depth > 16) return false;
		}
		else if (C == TCHAR('}') || C == TCHAR(']')) --Depth;
	}

	return true;
}


bool FModKitPatchManifest::VerifySignature(const FString& ExpectedKind, const FString& PublicKeyBase64,
	const TArray<FString>& TrustedKeyIds, FString& OutReason) const
{
	OutReason.Reset();
	if ((ExpectedKind != TEXT("package") && ExpectedKind != TEXT("hotpatch")) || Kind != ExpectedKind)
	{ OutReason = TEXT("kind"); return false; }
	if (SignatureSigner.IsEmpty() || !TrustedKeyIds.Contains(SignatureSigner))
	{ OutReason = TEXT("key-id-untrusted"); return false; }
	FModKitPatchSignature::FPublicKey Key;
	if (!FModKitPatchSignature::DecodePublicKey(PublicKeyBase64, Key))
	{ OutReason = TEXT("key-missing"); return false; }
	if (!MatchesPublicKey(Key.Bytes) || PayloadSha256 != ComputePayloadSha256())
	{ OutReason = TEXT("signature-anchor"); return false; }
	TArray<uint8> Canonical;
	ToCanonicalBytes(Canonical);
	if (!FModKitPatchSignature::Verify(Canonical, SignatureBytes, Key))
	{ OutReason = TEXT("signature-invalid"); return false; }
	return true;
}

bool FModKitPatchManifest::VerifyFiles(const TMap<FString, TArray<uint8>>& FileBytes, FString& OutReason) const
{
	OutReason.Reset();
	if (FileBytes.Num() != Files.Num())
	{ OutReason = TEXT("package-file-set-mismatch"); return false; }
	int64 TotalBytes = 0;
	for (const FModKitPatchFileEntry& File : Files)
	{
		const TArray<uint8>* Bytes = FileBytes.Find(File.Path);
		if (!Bytes) { OutReason = TEXT("package-file-missing"); return false; }
		TotalBytes += Bytes->Num();
		if (Bytes->Num() > MaxFileBytes || TotalBytes > MaxPackageBytes)
		{ OutReason = TEXT("package-byte-limit"); return false; }
		if (FModKitSha256::HashHex(Bytes->GetData(), Bytes->Num()) != File.Sha256)
		{ OutReason = TEXT("package-file-hash"); return false; }
	}
	return true;
}

bool FModKitPatchManifest::VerifyAndCommitPackageFloor(const FString& ManifestJson,
	const TMap<FString, TArray<uint8>>& FileBytes, const FString& ExpectedId,
	const FString& ExpectedVersion, const FString& PublicKeyBase64,
	const TArray<FString>& TrustedKeyIds, const FString& FloorPath,
	bool bAllowDowngrade, FString& OutReason)
{
	FModKitPatchManifest Manifest;
	if (!Parse(ManifestJson, Manifest, OutReason))
	{ OutReason = TEXT("manifest-invalid"); return false; }
	if (Manifest.PatchId != ExpectedId || Manifest.Version != ExpectedVersion)
	{ OutReason = TEXT("identity-mismatch"); return false; }
	if (!Manifest.MatchesEngineVersion()) { OutReason = TEXT("engine-mismatch"); return false; }
	if (!Manifest.VerifySignature(TEXT("package"), PublicKeyBase64, TrustedKeyIds, OutReason)
		|| !Manifest.VerifyFiles(FileBytes, OutReason)) return false;

	FString StatePath = FPaths::ConvertRelativePathToFull(FloorPath);
	FPaths::NormalizeFilename(StatePath);
	FPaths::CollapseRelativeDirectories(StatePath);
	FSystemWideCriticalSection Lock(FString::Printf(TEXT("ModKitFloor-%08x"), FCrc::StrCrc32(*StatePath.ToLower())), FTimespan::FromSeconds(5));
	if (!Lock.IsValid()) { OutReason = TEXT("version-state-lock"); return false; }
	IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformPhysical();
	FString StateText;
	FModKitPatchManifest Floor;
	const bool bHasFloor = Disk.FileExists(*StatePath);
	if (Disk.DirectoryExists(*StatePath) || (bHasFloor && (!ReadText(StatePath, StateText)
		|| !Parse(StateText, Floor, OutReason) || Floor.PatchId != ExpectedId
		|| !Floor.VerifySignature(TEXT("package"), PublicKeyBase64, TrustedKeyIds, OutReason))))
	{ OutReason = TEXT("version-state-invalid"); return false; }
	if (bHasFloor && !Manifest.IsVersionAtLeast(Floor.Version))
	{
		if (!bAllowDowngrade) { OutReason = TEXT("version-rollback"); return false; }
		OutReason.Reset();
		return true;
	}
	if (bHasFloor && StateText == ManifestJson) { OutReason.Reset(); return true; }
	if (!Disk.CreateDirectoryTree(*FPaths::GetPath(StatePath)))
	{ OutReason = TEXT("version-state-write"); return false; }
	const FString TempPath = StatePath + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	const FTCHARToUTF8 Encoded(*ManifestJson);
	TUniquePtr<IFileHandle> Writer(Disk.OpenWrite(*TempPath));
	if (!Writer || !Writer->Write(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length()) || !Writer->Flush(true))
	{ Writer.Reset(); Disk.DeleteFile(*TempPath); OutReason = TEXT("version-state-write"); return false; }
	Writer.Reset();
	if (bHasFloor && Disk.IsReadOnly(*StatePath) && !Disk.SetReadOnly(*StatePath, false))
	{ Disk.DeleteFile(*TempPath); OutReason = TEXT("version-state-write"); return false; }
#if PLATFORM_WINDOWS
	const bool bReplaced = !!MoveFileExW(*TempPath, *StatePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
	const bool bReplaced = ::rename(TCHAR_TO_UTF8(*TempPath), TCHAR_TO_UTF8(*StatePath)) == 0;
#endif
	if (!bReplaced) { Disk.DeleteFile(*TempPath); OutReason = TEXT("version-state-write"); return false; }
	OutReason.Reset();
	return true;
}


#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitSharedManifestTest, "AngelscriptModKit.Shared.ManifestContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitSharedManifestTest::RunTest(const FString& Parameters)
{
	const FString Fixture = FPaths::ProjectDir() / TEXT("HotPatch/positive-valid");
	const FString PublicKey = TEXT("bqQ94LWkdv0bTbv7O0yt+TM6JH1qkRZ5fokdmWRoL8s=");
	const TArray<FString> TrustedIds = { TEXT("modkit-g10-sign") };
	FString Json, Reason;
	FModKitPatchManifest Manifest;
	if (!TestTrue(TEXT("Read existing signed fixture"), FModKitPatchManifest::ReadText(Fixture / TEXT("Manifest.json"), Json))
		|| !TestTrue(TEXT("Parse existing hotpatch"), FModKitPatchManifest::Parse(Json, Manifest, Reason))) return false;
	TestTrue(TEXT("Trusted signature"), Manifest.VerifySignature(TEXT("hotpatch"), PublicKey, TrustedIds, Reason));
	TestFalse(TEXT("Package cannot consume hotpatch"), Manifest.VerifySignature(TEXT("package"), PublicKey, TrustedIds, Reason));
	TestEqual(TEXT("Wrong kind reason"), Reason, FString(TEXT("kind")));
	TestFalse(TEXT("Unknown signer"), Manifest.VerifySignature(TEXT("hotpatch"), PublicKey, {}, Reason));
	TestFalse(TEXT("Missing key"), Manifest.VerifySignature(TEXT("hotpatch"), TEXT(""), TrustedIds, Reason));
	TMap<FString, TArray<uint8>> Bytes;
	for (const FModKitPatchFileEntry& File : Manifest.Files)
	{
		TArray<uint8> Data;
		if (!TestTrue(TEXT("Read signed file"), FModKitPatchManifest::ReadBytes(Fixture / File.Path, FModKitPatchManifest::MaxFileBytes, Data))) return false;
		Bytes.Add(File.Path, MoveTemp(Data));
	}
	TestTrue(TEXT("Exact file snapshot"), Manifest.VerifyFiles(Bytes, Reason));
	Bytes.FindChecked(Manifest.Files[0].Path).Add(1);
	TestFalse(TEXT("Tampered bytes"), Manifest.VerifyFiles(Bytes, Reason));
	TestEqual(TEXT("Tampered bytes reason"), Reason, FString(TEXT("package-file-hash")));
	Bytes.Add(TEXT("Unlisted.as"), {});
	TestFalse(TEXT("Unlisted file"), Manifest.VerifyFiles(Bytes, Reason));
	FString Invalid = Json;
	Invalid.ReplaceInline(TEXT("\"format\":"), TEXT("\"kind\":42,\"format\":"));
	TestFalse(TEXT("Non-string kind rejected"), FModKitPatchManifest::Parse(Invalid, Manifest, Reason));
	Invalid = Json;
	Invalid.ReplaceInline(TEXT("\"algorithm\":"), TEXT("\"keyId\":42,\"algorithm\":"));
	TestFalse(TEXT("Non-string keyId rejected"), FModKitPatchManifest::Parse(Invalid, Manifest, Reason));
	TestEqual(TEXT("SHA256 empty input"), FModKitSha256::HashHex(nullptr, 0),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitSharedPackageFloorTest, "AngelscriptModKit.Shared.PackageFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitSharedPackageFloorTest::RunTest(const FString& Parameters)
{
	const FString Fixture = FPaths::ProjectDir() / TEXT("FeatureModPackages");
	const FString PublicKey = TEXT("bqQ94LWkdv0bTbv7O0yt+TM6JH1qkRZ5fokdmWRoL8s=");
	const TArray<FString> TrustedIds = { TEXT("patch-verify-v1") };
	FString V1, V2, Reason;
	FModKitPatchManifest Manifest;
	if (!TestTrue(TEXT("Read signed package"), FModKitPatchManifest::ReadText(Fixture / TEXT("positive/GfpModA.manifest.json"), V1))
		|| !TestTrue(TEXT("Read signed upgrade"), FModKitPatchManifest::ReadText(Fixture / TEXT("security/PackageV2.json"), V2))
		|| !TestTrue(TEXT("Parse package"), FModKitPatchManifest::Parse(V1, Manifest, Reason))) return false;
	TestTrue(TEXT("Package engine fixture matches"), Manifest.MatchesEngineVersion());
	TestFalse(TEXT("Hotpatch cannot consume package"), Manifest.VerifySignature(TEXT("hotpatch"), PublicKey, TrustedIds, Reason));
	TMap<FString, TArray<uint8>> Bytes;
	for (const FModKitPatchFileEntry& File : Manifest.Files)
	{
		TArray<uint8> Data;
		if (!TestTrue(TEXT("Read package snapshot"), FModKitPatchManifest::ReadBytes(Fixture / TEXT("positive/GfpModA") / File.Path, FModKitPatchManifest::MaxFileBytes, Data))) return false;
		Bytes.Add(File.Path, MoveTemp(Data));
	}
	const FString FloorDir = FPaths::ProjectSavedDir() / TEXT("Automation/ModKitShared") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString FloorPath = FloorDir / TEXT("GfpModA.json");
	auto Accept = [&](const FString& Json, const FString& Version, bool bAllowDowngrade = false)
	{
		return FModKitPatchManifest::VerifyAndCommitPackageFloor(Json, Bytes, TEXT("GfpModA"), Version,
			PublicKey, TrustedIds, FloorPath, bAllowDowngrade, Reason);
	};
	const TArray<uint8> OriginalConfig = Bytes.FindChecked(TEXT("FeatureMod.json"));
	Bytes.FindChecked(TEXT("FeatureMod.json")).Add(1);
	TestFalse(TEXT("Invalid first install rejected"), Accept(V1, TEXT("1.0")));
	TestFalse(TEXT("Invalid install leaves no floor"), IFileManager::Get().FileExists(*FloorPath));
	Bytes.FindChecked(TEXT("FeatureMod.json")) = OriginalConfig;
	TestTrue(TEXT("First install automatically creates floor"), Accept(V1, TEXT("1.0")));
	FString State;
	TestTrue(TEXT("Floor is persisted"), FModKitPatchManifest::ReadText(FloorPath, State));
	TestEqual(TEXT("Floor stores signed original"), State, V1);
	TestTrue(TEXT("Same version reaccepted"), Accept(V1, TEXT("1.0")));
	FString Config;
	FFileHelper::BufferToString(Config, OriginalConfig.GetData(), OriginalConfig.Num());
	Config.ReplaceInline(TEXT("\"1.0\""), TEXT("\"2.0\""));
	const FTCHARToUTF8 Encoded(*Config);
	TArray<uint8>& ConfigBytes = Bytes.FindChecked(TEXT("FeatureMod.json"));
	ConfigBytes.Reset(); ConfigBytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	TestTrue(TEXT("Signed second version advances floor"), Accept(V2, TEXT("2.0")));
	Bytes.FindChecked(TEXT("FeatureMod.json")) = OriginalConfig;
	TestFalse(TEXT("Old signed version rollback rejected"), Accept(V1, TEXT("1.0")));
	TestEqual(TEXT("Rollback reason"), Reason, FString(TEXT("version-rollback")));
	TestTrue(TEXT("Explicit development downgrade accepted"), Accept(V1, TEXT("1.0"), true));
	TestTrue(TEXT("Read retained floor"), FModKitPatchManifest::ReadText(FloorPath, State));
	TestEqual(TEXT("Downgrade never lowers floor"), State, V2);
	TestTrue(TEXT("Write corrupt state fixture"), FFileHelper::SaveStringToFile(TEXT("{}"), *FloorPath));
	TestFalse(TEXT("Corrupt floor never becomes first install"), Accept(V1, TEXT("1.0")));
	TestEqual(TEXT("Corrupt floor reason"), Reason, FString(TEXT("version-state-invalid")));
	IFileManager::Get().DeleteDirectory(*FloorDir, false, true);
	return true;
}
#endif
