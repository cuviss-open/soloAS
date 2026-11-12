// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned validation commandlets for the AngelscriptModKit module.

#include "DeveloperTools/ModKitCommandlets.h"

#include "FeatureMods/ModKitCompilationManager.h"
#include "ScriptCache/ModKitCacheTransform.h"

#if WITH_EDITOR
#include "AngelscriptManager.h"
#include "AngelscriptCoreExtensionPoints.h"
#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "GameFeatureData.h"
#include "Interfaces/IPluginManager.h"
#include "Shared/Delivery/ModKitPatchManifest.h"
#include "Shared/Crypto/ModKitCacheCipher.h"
#include "Shared/Crypto/ModKitPatchSignature.h"
#include "Core/ModKitSettings.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Base64.h"
#include "Misc/Parse.h"
#include "HAL/PlatformFileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#endif

#if WITH_EDITOR
namespace
{
	/**
	 * Collects all UClasses belonging to source-only leaf modules, resolved
	 * module-side: leaf module names come from the ModKit scan and classes
	 * are read from the module descriptors (no global UClass iteration).
	 */
	TArray<UClass*> CollectLeafClasses()
	{
		TArray<UClass*> Classes;
		TArray<FString> LeafModuleNames;
		FModKitCompilationManager::GetLeafModuleNames(LeafModuleNames);
		for (const FString& ModuleName : LeafModuleNames)
		{
			TSharedPtr<FAngelscriptModuleDesc> Module = FAngelscriptManager::Get().GetModuleByModuleName(ModuleName);
			if (!Module.IsValid())
			{
				continue;
			}
			for (const TSharedRef<FAngelscriptClassDesc>& ClassDesc : Module->Classes)
			{
				if (ClassDesc->Class != nullptr)
				{
					Classes.Add(ClassDesc->Class);
				}
			}
		}
		Classes.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
		return Classes;
	}

	/** True when the soft reference path points at a leaf module class. */
	bool IsLeafReference(const FString& PathString, const TSet<FString>& LeafClassNames, FString& OutLeafClass)
	{
		for (const FString& LeafName : LeafClassNames)
		{
			if (PathString == LeafName || PathString.EndsWith(TEXT(".") + LeafName))
			{
				OutLeafClass = LeafName;
				return true;
			}
		}
		return false;
	}
}
#endif

int32 UModKitAssetAuditCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	TArray<UClass*> LeafClasses = CollectLeafClasses();
	TSet<FString> LeafClassNames;
	for (const UClass* LeafClass : LeafClasses)
	{
		LeafClassNames.Add(LeafClass->GetName());
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.ScanPathsSynchronous({ TEXT("/Game") }, true);

	TArray<FAssetData> Assets;
	Registry.GetAssetsByPath(FName(TEXT("/Game")), Assets, true);

	int32 Violations = 0;
	for (const FAssetData& Asset : Assets)
	{
		UObject* Object = Asset.GetAsset();
		if (Object == nullptr)
		{
			continue;
		}
		for (TFieldIterator<FSoftObjectProperty> PropertyIt(Object->GetClass()); PropertyIt; ++PropertyIt)
		{
			const FString PathString = PropertyIt->GetPropertyValue_InContainer(Object).ToString();
			if (PathString.IsEmpty())
			{
				continue;
			}
			FString LeafClass;
			if (IsLeafReference(PathString, LeafClassNames, LeafClass))
			{
				UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_AUDIT_VIOLATION asset=%s ref=%s leafClass=%s"),
					*Asset.GetObjectPathString(), *PathString, *LeafClass);
				++Violations;
			}
		}
	}

	if (Violations == 0)
	{
		UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_AUDIT_CLEAN assets=%d leafClasses=%d"), Assets.Num(), LeafClassNames.Num());
		return 0;
	}
	return 1;
#else
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_AUDIT_SKIPPED reason=non-editor-target"));
	return 0;
#endif
}

int32 UModKitMakeAuditViolationFixtureCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	UClass* ProbeClass = FindObject<UClass>(nullptr, TEXT("/Script/MixedCompileV1.MixedCompileAuditProbeAsset"));
	if (ProbeClass == nullptr)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_AUDIT_FIXTURE_FAILED reason=probe-class-missing"));
		return 1;
	}

	TArray<UClass*> LeafClasses = CollectLeafClasses();
	if (LeafClasses.Num() == 0)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_AUDIT_FIXTURE_FAILED reason=no-leaf-classes"));
		return 1;
	}
	UClass* LeafClass = LeafClasses[0];

	FSoftObjectProperty* ProbeProperty = FindFProperty<FSoftObjectProperty>(ProbeClass, TEXT("ProbeReference"));
	if (ProbeProperty == nullptr)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_AUDIT_FIXTURE_FAILED reason=probe-property-missing"));
		return 1;
	}

	UPackage* Package = CreatePackage(TEXT("/Game/ModKitAuditViolationProbe"));
	UObject* Asset = NewObject<UObject>(Package, ProbeClass, FName(TEXT("ModKitAuditViolationProbe")), RF_Public | RF_Standalone);
	ProbeProperty->SetPropertyValue_InContainer(Asset, FSoftObjectPtr(LeafClass));

	const FString Filename = FPaths::ProjectContentDir() / TEXT("ModKitAuditViolationProbe.uasset");
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaved = UPackage::SavePackage(Package, Asset, *Filename, SaveArgs);
	if (!bSaved)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_AUDIT_FIXTURE_FAILED reason=save-failed"));
		return 1;
	}

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_AUDIT_FIXTURE_WRITTEN path=%s ref=%s"), *Filename, *LeafClass->GetPathName());
	return 0;
#else
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_AUDIT_FIXTURE_SKIPPED reason=non-editor-target"));
	return 0;
#endif
}

int32 UModKitAssembleCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString Source, Output, Signer, PublicKey;
	FParse::Value(*Params, TEXT("Source="), Source);
	FParse::Value(*Params, TEXT("Output="), Output);
	FParse::Value(*Params, TEXT("Signer="), Signer);
	FParse::Value(*Params, TEXT("PublicKey="), PublicKey);
	Source.TrimQuotesInline(); Output.TrimQuotesInline();
	auto Fail = [](const FString& Reason)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_ASSEMBLE_FAILED reason=%s"), *Reason);
		return 1;
	};
	if (Source.IsEmpty() || Output.IsEmpty()) return Fail(TEXT("source-output-required"));
	Source = FPaths::ConvertRelativePathToFull(Source);
	Output = FPaths::ConvertRelativePathToFull(Output);
	FPaths::NormalizeDirectoryName(Source); FPaths::NormalizeDirectoryName(Output);
	FPaths::CollapseRelativeDirectories(Source); FPaths::CollapseRelativeDirectories(Output);
	if (FPaths::IsSamePath(Source, Output) || FPaths::IsUnderDirectory(Output, Source)
		|| IFileManager::Get().DirectoryExists(*Output)) return Fail(TEXT("output-must-be-new-and-outside-source"));
	const FString Id = FPaths::GetCleanFilename(Source);
	const bool bCacheOnly = FParse::Param(*Params, TEXT("CacheOnly"));
	FString ConfigText, DescriptorText;
	TSharedPtr<FJsonObject> Config, Descriptor;
	if (!FModKitPatchManifest::ReadText(Source / TEXT("FeatureMod.json"), ConfigText)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ConfigText), Config)
		|| !FModKitPatchManifest::ReadText(Source / (Id + TEXT(".uplugin")), DescriptorText)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DescriptorText), Descriptor))
		return Fail(TEXT("featuremod-metadata"));
	bool bFeatureMod = false;
	FString Version;
	if (!Descriptor->TryGetBoolField(TEXT("FeatureMod"), bFeatureMod) || !bFeatureMod
		|| !Config->TryGetStringField(TEXT("version"), Version)) return Fail(TEXT("featuremod-metadata"));
	FModKitPatchManifest Manifest;
	Manifest.Kind = TEXT("package"); Manifest.PatchId = Id; Manifest.Version = Version;
	Manifest.EngineVersion = ModKitPatchEngineVersionString();
	Manifest.SignatureAlgorithm = FModKitPatchManifest::GetSignatureAlgorithmEd25519();
	Manifest.SignatureSigner = Signer;
	const bool bSigning = !Signer.IsEmpty() || !PublicKey.IsEmpty();
	if (bSigning)
	{
		FModKitPatchSignature::FPublicKey Key;
		if (Signer.IsEmpty() || !FModKitPatchSignature::DecodePublicKey(PublicKey, Key))
			return Fail(TEXT("signer-public-key-required"));
		Manifest.PublicKeySha256 = FModKitSha256::HashHex(Key.Bytes, 32);
	}
	TMap<FString, TArray<uint8>> Snapshot;
	if (bCacheOnly)
	{
		using namespace AngelscriptCoreExtensionPoints;
		TArray<TSharedRef<FAngelscriptModuleDesc>> Modules;
		for (const auto& Module : FAngelscriptManager::Get().GetActiveModules())
		{
			FModKitSourceOwner Owner;
			FString Error;
			if (!FModKitCompilationManager::GetSourceOwner(Module, Owner, Error)
				|| Owner.Layer != EModKitSourceLayer::Leaf || Owner.Id != Id) continue;
			if (!FModKitModRegistry::IsSourceOwnerCurrent(Owner)
				|| !FPaths::IsSamePath(FPaths::GetPath(Owner.ScriptRoot), Source))
				return Fail(TEXT("producer-source-owner-mismatch"));
			Modules.Add(Module);
		}
		if (Modules.IsEmpty()) return Fail(TEXT("producer-requires-loaded-featuremod-source"));
		TSharedPtr<const FProducedModuleCacheSet> Produced;
		FString Error;
		if (ProduceModuleCacheSet(FAngelscriptManager::Get().GetScriptEngine(), Modules, Produced, Error)
			!= ECacheAdmission::Completed || !Produced.IsValid()) return Fail(TEXT("cache-producer: ") + Error);
		// Produce seals nativeTypeLayouts in this same unchanged VM. Preserve its
		// manifest verbatim; the package's file hashes/signature authenticate it.
		Snapshot.Add(TEXT("Script/ModuleCaches/ModuleCaches.json"), Produced->Manifest);
		FModKitCacheEnvelopePolicy Policy;
		if (!FModKitCacheTransform::GetTransformPolicy(EModKitCacheDirection::Save, Policy))
			return Fail(TEXT("cache-envelope-policy"));
		// VM serialization/sealing and envelope policy use the GT producer context.
		for (const auto& File : Produced->Files)
		{
			TArray<uint8> Bytes = File.Bytes;
			if (!FModKitCacheTransform::TransformCacheBytes(EModKitCacheDirection::Save, Policy, Bytes))
				return Fail(TEXT("cache-envelope-seal"));
			Snapshot.Add(TEXT("Script/ModuleCaches/") + File.RelativePath, MoveTemp(Bytes));
		}
	}
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Source, TEXT("*"), true, false);
	if (Files.Num() > 4096) return Fail(TEXT("file-limit"));
	int64 TotalBytes = 0;
	for (const FString& File : Files)
	{
		FString Relative = File;
		if (!FPaths::MakePathRelativeTo(Relative, *(Source + TEXT("/")))) return Fail(TEXT("path"));
		if (Relative.StartsWith(TEXT("Binaries/")) || Relative.StartsWith(TEXT("Intermediate/"))
			|| Relative.StartsWith(TEXT("Saved/")) || Relative.StartsWith(TEXT(".git/"))
			|| Relative == TEXT(".git")) continue;
		if (bCacheOnly && (Relative.EndsWith(TEXT(".as"), ESearchCase::IgnoreCase)
			|| Relative.StartsWith(TEXT("Script/ModuleCaches/")))) continue;
		TArray<uint8> Bytes;
		if (!FModKitPatchManifest::ReadBytes(File, FModKitPatchManifest::MaxFileBytes, Bytes)) return Fail(TEXT("file-read"));
		Snapshot.Add(Relative, MoveTemp(Bytes));
	}
	if (Snapshot.Num() > 4096) return Fail(TEXT("file-limit"));
	TArray<FString> Paths;
	Snapshot.GetKeys(Paths);
	Paths.Sort();
	for (const FString& Path : Paths)
	{
		const auto& Bytes = Snapshot.FindChecked(Path);
		TotalBytes += Bytes.Num();
		if (Bytes.Num() > FModKitPatchManifest::MaxFileBytes || TotalBytes > FModKitPatchManifest::MaxPackageBytes)
			return Fail(TEXT("package-byte-limit"));
	}
	TArray<FString> Hashes;
	Hashes.SetNum(Paths.Num());
	ParallelFor(Paths.Num(), [&](int32 Index)
	{
		const auto& Bytes = Snapshot.FindChecked(Paths[Index]);
		Hashes[Index] = FModKitSha256::HashHex(Bytes.GetData(), Bytes.Num());
	});
	for (int32 Index = 0; Index < Paths.Num(); ++Index) Manifest.Files.Add({Paths[Index], Hashes[Index]});
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("format"), FModKitPatchManifest::GetFormatV1());
	Json->SetStringField(TEXT("kind"), Manifest.Kind);
	Json->SetStringField(TEXT("patchId"), Id);
	Json->SetStringField(TEXT("version"), Version);
	Json->SetStringField(TEXT("engineVersion"), Manifest.EngineVersion);
	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const FModKitPatchFileEntry& File : Manifest.Files)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), File.Path); Entry->SetStringField(TEXT("sha256"), File.Sha256);
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Json->SetArrayField(TEXT("files"), Entries);
	if (bSigning)
	{
		TSharedRef<FJsonObject> Signature = MakeShared<FJsonObject>();
		Signature->SetStringField(TEXT("algorithm"), Manifest.SignatureAlgorithm);
		Signature->SetStringField(TEXT("signer"), Signer);
		Signature->SetStringField(TEXT("publicKeySha256"), Manifest.PublicKeySha256);
		Signature->SetStringField(TEXT("payloadSha256"), Manifest.ComputePayloadSha256());
		TArray<uint8> Placeholder; Placeholder.SetNumZeroed(64);
		Signature->SetStringField(TEXT("value"), FBase64::Encode(Placeholder));
		Json->SetObjectField(TEXT("signature"), Signature);
		FString Text, Reason;
		FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
		FModKitPatchManifest Parsed;
		if (!FModKitPatchManifest::Parse(Text, Parsed, Reason)) return Fail(Reason);
	}
	for (const auto& File : Snapshot)
	{
		const FString Path = Output / Id / File.Key;
		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true)
			|| !FFileHelper::SaveArrayToFile(File.Value, *Path)) return Fail(TEXT("output-write"));
	}
	if (bSigning)
	{
		FString Text; FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
		TArray<uint8> Canonical; Manifest.ToCanonicalBytes(Canonical);
		if (!FFileHelper::SaveStringToFile(Text, *(Output / (Id + TEXT(".manifest.json"))), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			|| !FFileHelper::SaveArrayToFile(Canonical, *(Output / (Id + TEXT(".canonical"))))) return Fail(TEXT("output-write"));
	}
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_ASSEMBLE_OK mod=%s files=%d signing=%d output=%s"), *Id, Snapshot.Num(), bSigning, *Output);
	return 0;
#else
	return 1;
#endif
}

int32 UModKitCreateGameFeatureDataCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString PluginName;
	FParse::Value(*Params, TEXT("Plugin="), PluginName);
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (!Plugin || !Plugin->CanContainContent()) return 1;
	const FString Filename = Plugin->GetContentDir() / (PluginName + TEXT(".uasset"));
	if (IFileManager::Get().FileExists(*Filename)) return 1;
	UPackage* Package = CreatePackage(*(TEXT("/") + PluginName + TEXT("/") + PluginName));
	UGameFeatureData* Asset = NewObject<UGameFeatureData>(Package, *PluginName, RF_Public | RF_Standalone);
	FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
	if (!IFileManager::Get().MakeDirectory(*Plugin->GetContentDir(), true)
		|| !UPackage::SavePackage(Package, Asset, *Filename, Args)) return 1;
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_GAME_FEATURE_DATA_CREATED plugin=%s asset=%s"), *PluginName, *Asset->GetPathName());
	return 0;
#else
	return 1;
#endif
}

int32 UModKitSealScriptCacheCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
    FString Source, Output;
    if (!FParse::Value(*Params, TEXT("Source="), Source) || !FParse::Value(*Params, TEXT("Output="), Output)
        || FPaths::IsSamePath(Source, Output) || IFileManager::Get().FileExists(*Output)
        || !UModKitSettings::Get()->bEncryptScriptCache)
    {
        UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_SEAL_FAILED: require distinct source and new output with encryption configured"));
        return 1;
    }
    TArray<uint8> Bytes;
    FModKitCacheEnvelopePolicy Policy;
    if (!FFileHelper::LoadFileToArray(Bytes, *Source) || Bytes.Num() < 20
        || (Bytes[0] == 'M' && Bytes[1] == 'K' && Bytes[2] == 'C' && Bytes[3] == 'C')
        || !FModKitCacheTransform::GetTransformPolicy(EModKitCacheDirection::Save, Policy)
        || !Policy.bTransform || Policy.bBlocked
        || !FModKitCacheTransform::TransformCacheBytes(EModKitCacheDirection::Save, Policy, Bytes))
    {
        UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_SEAL_FAILED: invalid input or envelope policy"));
        return 1;
    }
    const FString Temporary = Output + TEXT(".") + FGuid::NewGuid().ToString() + TEXT(".tmp");
    const bool Saved = FFileHelper::SaveArrayToFile(Bytes, *Temporary, &IFileManager::Get(), FILEWRITE_NoReplaceExisting);
    FMemory::Memzero(Bytes.GetData(), Bytes.Num());
    if (!Saved || !IFileManager::Get().Move(*Output, *Temporary, false))
    {
        IFileManager::Get().Delete(*Temporary);
        UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_SEAL_FAILED: output was not published"));
        return 1;
    }
    UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_CACHE_SEALED"));
    return 0;
#else
    return 1;
#endif
}
