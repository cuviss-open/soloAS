// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned offline patch manifest verification commandlet .

#include "HotPatch/ModKitPatchVerify.h"

#include "HotPatch/ModKitHotPatchFetch.h"
#include "HotPatch/ModKitHotPatchLand.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "Shared/Delivery/ModKitPatchManifest.h"
#include "Core/ModKitSettings.h"

#if WITH_EDITOR
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"
#endif

#if WITH_EDITOR
namespace
{
                                                                                    
	int32 PatchVerifyEmitFailed(const FString& Reason, const FString& Detail = FString())
	{
		if (Detail.IsEmpty())
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_VERIFY_FAILED reason=%s"), *Reason);
		}
		else
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_VERIFY_FAILED reason=%s detail=%s"), *Reason, *Detail);
		}
		return 1;
	}

	FString PatchVerifyResolvePublicKey(const FString& Params)
	{
		FString PublicKeyBase64;
		if (FParse::Value(*Params, TEXT("PublicKey="), PublicKeyBase64))
		{
			PublicKeyBase64.TrimQuotesInline();
			return PublicKeyBase64;
		}
		const UModKitSettings* Settings = UModKitSettings::Get();
		return Settings->PatchPublicKeyBase64;
	}
}
#endif

int32 UModKitPatchVerifyCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString ManifestPath;
	FParse::Value(*Params, TEXT("Manifest="), ManifestPath);
	ManifestPath.TrimQuotesInline();
	// BEGIN is emitted before any validation so every lane (including early
	// config/schema failures) carries the marker the runners assert on.
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_VERIFY_BEGIN manifest=%s"), *ManifestPath);
	if (ManifestPath.IsEmpty() || !IFileManager::Get().FileExists(*ManifestPath))
	{
		return PatchVerifyEmitFailed(TEXT("config"));
	}
	ManifestPath = FPaths::ConvertRelativePathToFull(ManifestPath);

	FString ManifestText;
	if (!FModKitPatchManifest::ReadText(ManifestPath, ManifestText))
	{
		return PatchVerifyEmitFailed(TEXT("config"));
	}

	// --- 1. schema ----------------------------------------------------------
	FModKitPatchManifest Manifest;
	FString SchemaReason;
	if (!FModKitPatchManifest::Parse(ManifestText, Manifest, SchemaReason))
	{
		return PatchVerifyEmitFailed(TEXT("schema"), SchemaReason);
	}

	FString ExpectedKind = TEXT("hotpatch");
	FParse::Value(*Params, TEXT("Kind="), ExpectedKind);
	if ((ExpectedKind != TEXT("hotpatch") && ExpectedKind != TEXT("package")) || Manifest.Kind != ExpectedKind)
		return PatchVerifyEmitFailed(TEXT("kind"));

	FModKitFetchedPatch Patch;
	Patch.ManifestText = ManifestText;
	const FString ManifestDir = FPaths::GetPath(ManifestPath);
	TArray<FString> Files;
	const FString PayloadDir = ExpectedKind == TEXT("package") ? ManifestDir / Manifest.PatchId : ManifestDir;
	IFileManager::Get().FindFilesRecursive(Files, *PayloadDir, TEXT("*"), true, false);
	int64 TotalBytes = 0;
	for (const FString& File : Files)
	{
		if (FPaths::IsSamePath(File, ManifestPath)) continue;
		FString Relative = File;
		if (!FPaths::MakePathRelativeTo(Relative, *(PayloadDir + TEXT("/"))))
			return PatchVerifyEmitFailed(TEXT("sha-mismatch"));
		const int64 Size = IFileManager::Get().FileSize(*File);
		if (Size < 0 || Size > FModKitPatchManifest::MaxFileBytes
			|| Size > FModKitPatchManifest::MaxPackageBytes - TotalBytes)
			return PatchVerifyEmitFailed(TEXT("sha-mismatch"), TEXT("package-byte-limit"));
		TotalBytes += Size;
		TArray<uint8> Bytes;
		if (!FModKitPatchManifest::ReadBytes(File, FModKitPatchManifest::MaxFileBytes, Bytes))
			return PatchVerifyEmitFailed(TEXT("sha-mismatch"), TEXT("file-missing:") + Relative);
		Patch.Files.Add(Relative, MoveTemp(Bytes));
	}
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_VERIFY_PARSED patch=%s files=%d"),
		*Manifest.PatchId, Manifest.Files.Num());
	FString Reason, Detail;
	if (ExpectedKind == TEXT("package"))
	{
		const UModKitSettings* Settings = UModKitSettings::Get();
		TArray<FString> TrustedIds = Settings->PatchAcceptedKeyIds;
		TrustedIds.AddUnique(Settings->PatchVerifyKeyId);
		if (!Manifest.VerifySignature(TEXT("package"), PatchVerifyResolvePublicKey(Params), TrustedIds, Reason)
			|| !Manifest.VerifyFiles(Patch.Files, Reason))
			return PatchVerifyEmitFailed(Reason);
        auto ReadMetadata = [&Patch](const FString& Path, TSharedPtr<FJsonObject>& Out)
        {
            const TArray<uint8>* Bytes = Patch.Files.Find(Path);
            if (!Bytes) return false;
            FString Text;
            FFileHelper::BufferToString(Text, Bytes->GetData(), Bytes->Num());
            return FModKitPatchManifest::IsJsonWithinBudget(Text)
                && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out.IsValid();
        };
        TSharedPtr<FJsonObject> Config, Descriptor;
        FString Version;
        bool bFeatureMod = false;
        if (!ReadMetadata(TEXT("FeatureMod.json"), Config)
            || !ReadMetadata(Manifest.PatchId + TEXT(".uplugin"), Descriptor)
            || !Config->TryGetStringField(TEXT("version"), Version) || Version != Manifest.Version
            || !Descriptor->TryGetBoolField(TEXT("FeatureMod"), bFeatureMod) || !bFeatureMod)
            return PatchVerifyEmitFailed(TEXT("identity-mismatch"));
		if (!Manifest.MatchesEngineVersion()) return PatchVerifyEmitFailed(TEXT("engine-mismatch"));
		FString MinimumVersion;
		if (FParse::Value(*Params, TEXT("MinimumVersion="), MinimumVersion) && !Manifest.IsVersionAtLeast(MinimumVersion))
			return PatchVerifyEmitFailed(TEXT("version"));
	}
	else if (!FModKitHotPatchLand::ValidatePatch(Patch, PatchVerifyResolvePublicKey(Params), Manifest, Reason, Detail))
		return PatchVerifyEmitFailed(Reason == TEXT("bad-signature") ? TEXT("signature") : Reason, Detail);

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_VERIFY_OK patch=%s files=%d payloadSha256=%s"),
		*Manifest.PatchId, Manifest.Files.Num(), *Manifest.PayloadSha256);
	return 0;
#else
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_VERIFY_SKIPPED reason=non-editor-target"));
	return 0;
#endif
}
