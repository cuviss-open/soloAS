// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned external FeatureMod package verification.

#include "FeatureMods/ModKitSignature.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "FeatureMods/ModKitModRegistry.h"
#include "Shared/Delivery/ModKitPatchManifest.h"
#include "Core/ModKitSettings.h"
#include "AngelscriptCoreAdmission.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

FSimpleMulticastDelegate FModKitSignature::OnUnsignedModWarning;

namespace
{
	TMap<FString, FString> VerifiedSources;
	FDelegateHandle LoadedSourceHandle;
	TMap<FString, FString> HighestAcceptedVersions;
	TMap<FString, TMap<FString, TArray<uint8>>> VerifiedPackages;
	struct FReloadVerification
	{
		bool bHadPackage = false;
		TMap<FString, TArray<uint8>> Package;
		TMap<FString, FString> Sources;
	};
	TMap<FString, FReloadVerification> ReloadVerifications;

	FString SourceKey(const FString& Path)
	{
		FString Full = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Full);
		FPaths::CollapseRelativeDirectories(Full);
		#if PLATFORM_WINDOWS
		Full.ToLowerInline();
#endif
		return Full;
	}

	bool IsTrustedSigner(const FString& Signer)
	{
#if UE_BUILD_SHIPPING
		return !Signer.IsEmpty() && Signer == TEXT(MODKIT_DELIVERY_KEY_ID);
#else
		const UModKitSettings* Settings = UModKitSettings::Get();
		return !Signer.IsEmpty() && (Signer == Settings->PatchVerifyKeyId
			|| Settings->PatchAcceptedKeyIds.Contains(Signer));
#endif
	}

	TArray<FString> TrustedKeyIds()
	{
#if UE_BUILD_SHIPPING
		return { TEXT(MODKIT_DELIVERY_KEY_ID) };
#else
		TArray<FString> Ids = UModKitSettings::Get()->PatchAcceptedKeyIds;
		Ids.AddUnique(UModKitSettings::Get()->PatchVerifyKeyId);
		return Ids;
#endif
	}
}

bool FModKitSignature::RequiresSignature()
{
	return UModKitSettings::Get()->bOnlyOfficialExternalMods;
}

FString FModKitSignature::PublicKeyBase64()
{
#if UE_BUILD_SHIPPING
	return TEXT(MODKIT_DELIVERY_PUBLIC_KEY);
#else
	return UModKitSettings::Get()->PatchPublicKeyBase64;
#endif
}

bool FModKitSignature::ValidateTrust(const FModKitModInfo& Mod, EModKitModTrust& OutTrust, FString& OutReason)
{
	if (FModKitCompilationManager::IsSourceAdmissionPending()) { OutReason = TEXT("busy-source-admission"); return false; }
	OutTrust = EModKitModTrust::Player;
	OutReason.Reset();
	const FString ManifestPath = FPaths::GetPath(Mod.DirectoryPath) / (Mod.DirectoryName + TEXT(".manifest.json"));
	IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformPhysical();
	if (Disk.FileExists(*ManifestPath))
	{
		FString Text;
		FModKitPatchManifest Manifest;
		if (!FModKitPatchManifest::ReadText(ManifestPath, Text)
			|| !FModKitPatchManifest::Parse(Text, Manifest, OutReason))
		{ OutReason = TEXT("manifest-invalid"); return false; }
		// An unknown signer grants no official permissions. A known signer
		// must pass every package check, even if player packages are allowed.
		if (IsTrustedSigner(Manifest.SignatureSigner))
		{
			if (!ValidateExternal(Mod, OutReason)) return false;
			OutTrust = EModKitModTrust::Official;
			return true;
		}
	}
	if (RequiresSignature()) { OutReason = TEXT("official-only"); return false; }
	// A replaced official package must not keep its verified source snapshot.
	const FString Root = SourceKey(Mod.DirectoryPath);
	VerifiedPackages.Remove(Root);
	for (auto It = VerifiedSources.CreateIterator(); It; ++It)
		if (It.Key().StartsWith(Root + TEXT("/"), ESearchCase::CaseSensitive)) It.RemoveCurrent();
	return true;
}

bool FModKitSignature::ValidateExternal(const FModKitModInfo& Mod, FString& OutReason)
{
	if (FModKitCompilationManager::IsSourceAdmissionPending()) { OutReason = TEXT("busy-source-admission"); return false; }
	IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformPhysical();
	const FString ManifestPath = FPaths::ConvertRelativePathToFull(FPaths::GetPath(Mod.DirectoryPath) / (Mod.DirectoryName + TEXT(".manifest.json")));
	FString JsonText;
	if (!Disk.FileExists(*ManifestPath) || !FModKitPatchManifest::ReadText(ManifestPath, JsonText))
	{
		OutReason = Disk.FileExists(*ManifestPath) ? TEXT("manifest-limits") : TEXT("manifest-missing");
		return false;
	}
	FModKitPatchManifest Manifest;
	if (!FModKitPatchManifest::Parse(JsonText, Manifest, OutReason) || Manifest.Kind != TEXT("package"))
	{ OutReason = TEXT("manifest-invalid"); return false; }
	if (Manifest.PatchId != Mod.DirectoryName || Manifest.Version != Mod.VersionName)
	{ OutReason = TEXT("identity-mismatch"); return false; }
	if (!Manifest.MatchesEngineVersion()) { OutReason = TEXT("engine-mismatch"); return false; }
	const bool bAllowDowngrade = UModKitSettings::Get()->bAllowFeatureModDowngrade;
	const FString RootKey = SourceKey(Mod.DirectoryPath);
	const FString* Highest = HighestAcceptedVersions.Find(RootKey);
	if (!bAllowDowngrade && Highest && !Manifest.IsVersionAtLeast(*Highest))
	{ OutReason = TEXT("version-rollback"); return false; }

	TSet<FString> Expected;
	TMap<FString, FString> Sources;
	TMap<FString, TArray<uint8>> PackageFiles;
	int64 TotalBytes = 0;
	for (const FModKitPatchFileEntry& File : Manifest.Files)
	{
		Expected.Add(File.Path.ToLower());
		const FString Path = FPaths::ConvertRelativePathToFull(Mod.DirectoryPath / File.Path);
		if (!Disk.FileExists(*Path)) { OutReason = TEXT("package-file-missing"); return false; }
		TArray<uint8> Bytes;
		if (!FModKitPatchManifest::ReadBytes(Path, FMath::Min(FModKitPatchManifest::MaxFileBytes,
			FModKitPatchManifest::MaxPackageBytes - TotalBytes), Bytes))
		{ OutReason = TEXT("package-byte-limit"); return false; }
		TotalBytes += Bytes.Num();

		if (File.Path.EndsWith(TEXT(".as"), ESearchCase::CaseSensitive))
		{
			FString Source;
			FFileHelper::BufferToString(Source, Bytes.GetData(), Bytes.Num());
			Sources.Add(SourceKey(Path), MoveTemp(Source));
		}
		PackageFiles.Add(File.Path, MoveTemp(Bytes));
	}
	struct FFileSetVisitor : IPlatformFile::FDirectoryVisitor
	{
		const TSet<FString>& Expected;
		FString Root;
		int32 Files = 0, Entries = 0;
		FFileSetVisitor(const TSet<FString>& InExpected, const FString& InRoot) : Expected(InExpected), Root(FPaths::ConvertRelativePathToFull(InRoot) / TEXT("")) {}
		virtual bool Visit(const TCHAR* Path, bool bDirectory) override
		{
			if (++Entries > 8192) return false;
			if (bDirectory) return true;
			FString Relative(Path);
			FPaths::MakePathRelativeTo(Relative, *Root);
			FPaths::NormalizeFilename(Relative);
			++Files;
			return Expected.Contains(Relative.ToLower());
		}
	} Visitor(Expected, Mod.DirectoryPath);
	if (!Disk.IterateDirectoryRecursively(*FPaths::ConvertRelativePathToFull(Mod.DirectoryPath), Visitor))
	{ OutReason = TEXT("package-file-unlisted"); return false; }
	if (Visitor.Files != Expected.Num()) { OutReason = TEXT("package-file-set-mismatch"); return false; }
	const FString VersionStatePath = FPaths::ProjectSavedDir() / TEXT("FeatureModVersions") / (Mod.DirectoryName + TEXT(".json"));
	if (!FModKitPatchManifest::VerifyAndCommitPackageFloor(JsonText, PackageFiles,
		Mod.DirectoryName, Mod.VersionName, PublicKeyBase64(), TrustedKeyIds(),
		VersionStatePath, bAllowDowngrade, OutReason)) return false;
	if (!Highest || Manifest.IsVersionAtLeast(*Highest)) HighestAcceptedVersions.Add(RootKey, Manifest.Version);
	for (auto It = VerifiedSources.CreateIterator(); It; ++It) if (It.Key().StartsWith(RootKey + TEXT("/"), ESearchCase::CaseSensitive)) It.RemoveCurrent();
	TMap<FString, TArray<uint8>> AbsoluteFiles;
	for (auto& File : PackageFiles) AbsoluteFiles.Add(SourceKey(Mod.DirectoryPath / File.Key), MoveTemp(File.Value));
	VerifiedPackages.Add(RootKey, MoveTemp(AbsoluteFiles));
	for (TPair<FString, FString>& Source : Sources) VerifiedSources.Add(Source.Key, MoveTemp(Source.Value));
	OutReason.Reset();
	return true;
}

void FModKitSignature::Startup()
{
	LoadedSourceHandle = AngelscriptCoreExtensionPoints::RegisterLoadedSourceValidator(&FModKitSignature::CheckLoadedSource);
	if (!LoadedSourceHandle.IsValid())
	{
		UE_LOG(LogAngelscriptModKit, Fatal, TEXT("ModKit could not register loaded source validation"));
	}
}

void FModKitSignature::Shutdown()
{
	if (!TryRevokeSourceValidator())
		UE_LOG(LogAngelscriptModKit, Warning, TEXT("ModKit retains busy loaded source validation"));
}

bool FModKitSignature::TryRevokeSourceValidator()
{
	if (!LoadedSourceHandle.IsValid()) return true;
	if (AngelscriptCoreExtensionPoints::UnregisterLoadedSourceValidator(LoadedSourceHandle)
		!= AngelscriptCoreExtensionPoints::ERemoveResult::Removed) return false;
	LoadedSourceHandle.Reset();
	return true;
}

bool FModKitSignature::HasSourceValidator()
{
	return LoadedSourceHandle.IsValid();
}

bool FModKitSignature::CheckLoadedSource(FStringView AbsolutePath, FStringView RelativePath,
	FStringView LoadedText, bool bDeleted, FString& OutError)
{
	if (bDeleted) return FModKitCompilationManager::AdmitSource(AbsolutePath, RelativePath, true, OutError);
	const FString Root = SourceKey(FPaths::ProjectDir() / TEXT("FeatureMods")) + TEXT("/");
	const FString Path = SourceKey(FString(AbsolutePath));
	bool bOfficial = false;
	for (const auto& Package : VerifiedPackages)
		if (Path.StartsWith(Package.Key + TEXT("/"), ESearchCase::CaseSensitive)) { bOfficial = true; break; }
	if (!bOfficial && (!Path.StartsWith(Root, ESearchCase::CaseSensitive) || !RequiresSignature()))
		return FModKitCompilationManager::AdmitSource(AbsolutePath, RelativePath, false, OutError);
	const FString* Verified = VerifiedSources.Find(Path);
	if (Verified)
	{
		const FString Wrapped = FModKitCompilationManager::WrapLeafSource(FString(AbsolutePath), *Verified, false);
		if (FStringView(*Verified) == LoadedText || FStringView(Wrapped) == LoadedText)
			return FModKitCompilationManager::AdmitSource(AbsolutePath, RelativePath, false, OutError);
	}
	OutError = FString::Printf(TEXT("MODKIT_PACKAGE_SOURCE_REJECTED reason=unverified-loaded-source file=%s"), *FString(RelativePath));
	return false;
}

bool FModKitSignature::IsModSigned(const FModKitModInfo& Mod)
{
	return VerifiedPackages.Contains(SourceKey(Mod.DirectoryPath));
}

void FModKitSignature::Reset()
{
	if (!ensureMsgf(!FModKitCompilationManager::IsSourceAdmissionPending(), TEXT("ModKit source verification is busy"))) return;
	VerifiedSources.Reset();
	VerifiedPackages.Reset();
	HighestAcceptedVersions.Reset();
	ReloadVerifications.Reset();
}

void FModKitSignature::BeginReloadVerification(const FString& DirectoryPath)
{
	if (!ensureMsgf(!FModKitCompilationManager::IsSourceAdmissionPending(), TEXT("ModKit source verification is busy"))) return;
	const FString Root = SourceKey(DirectoryPath);
	check(!ReloadVerifications.Contains(Root));
	FReloadVerification& Saved = ReloadVerifications.Add(Root);
	Saved.bHadPackage = VerifiedPackages.RemoveAndCopyValue(Root, Saved.Package);
	for (auto It = VerifiedSources.CreateIterator(); It; ++It)
		if (It.Key().StartsWith(Root + TEXT("/"), ESearchCase::CaseSensitive))
		{
			Saved.Sources.Add(It.Key(), MoveTemp(It.Value()));
			It.RemoveCurrent();
		}
}

void FModKitSignature::CompleteReloadVerification(const FString& DirectoryPath, bool bSuccess)
{
	if (!ensureMsgf(!FModKitCompilationManager::IsSourceAdmissionPending(), TEXT("ModKit source verification is busy"))) return;
	const FString Root = SourceKey(DirectoryPath);
	FReloadVerification Saved;
	const bool bFound = ReloadVerifications.RemoveAndCopyValue(Root, Saved);
	check(bFound);
	if (bSuccess) return;
	VerifiedPackages.Remove(Root);
	for (auto It = VerifiedSources.CreateIterator(); It; ++It)
		if (It.Key().StartsWith(Root + TEXT("/"), ESearchCase::CaseSensitive)) It.RemoveCurrent();
	if (Saved.bHadPackage) VerifiedPackages.Add(Root, MoveTemp(Saved.Package));
	for (auto& Source : Saved.Sources) VerifiedSources.Add(Source.Key, MoveTemp(Source.Value));
}

void FModKitSignature::NotifyUnsigned(const FModKitModInfo& Mod, const FString& Reason)
{
	UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_MOD_UNSIGNED name=%s reason=%s"), *Mod.DirectoryName, *Reason);
	OnUnsignedModWarning.Broadcast();
}

const TMap<FString, TArray<uint8>>* FModKitSignature::GetVerifiedPackageFiles(const FString& ScriptDirectory)
{
	const FString Root = FPaths::GetPath(SourceKey(ScriptDirectory));
	return VerifiedPackages.Find(Root);
}

const FString* FModKitSignature::GetVerifiedSource(const FString& Path)
{
	return VerifiedSources.Find(SourceKey(Path));
}

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Preprocessor/AngelscriptPreprocessor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitLoadedSourceAdmissionTest,
	"AngelscriptModKit.FeatureMods.LoadedSourceAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FModKitLoadedSourceAdmissionTest::RunTest(const FString&)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Script/Automation") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString File = Root / TEXT("SignedSource.as");
	const FString Relative = TEXT("SignedSource.as");
	const FString Original = TEXT("int ModKitSignedSource() { return 7; }");
	const FString Changed = TEXT("int ModKitSignedSource() { return 9; }");
	const FString RootKey = SourceKey(Root);
	const FString FileKey = SourceKey(File);
	bool bStopped = false;
	ON_SCOPE_EXIT
	{
		if (bStopped) FModKitSignature::Startup();
		VerifiedSources.Remove(FileKey);
		VerifiedPackages.Remove(RootKey);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	};
	if (!TestTrue(TEXT("Create signed source fixture directory"), IFileManager::Get().MakeDirectory(*Root, true))) return false;
	VerifiedPackages.Add(RootKey, TMap<FString, TArray<uint8>>());
	VerifiedSources.Add(FileKey, Original);
	if (!TestTrue(TEXT("Write signed source fixture"), FFileHelper::SaveStringToFile(Original, *File))) return false;
	auto Preprocess = [&](bool bAsync)
	{
		FAngelscriptPreprocessor Processor;
		Processor.AddFile(Relative, File, bAsync);
		return Processor.Preprocess();
	};
	TestTrue(TEXT("Verified loaded source is accepted"), Preprocess(false));
	if (!TestTrue(TEXT("Change signed source fixture"), FFileHelper::SaveStringToFile(Changed, *File))) return false;
	AddExpectedError(TEXT("MODKIT_PACKAGE_SOURCE_REJECTED"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Changed loaded source is rejected"), Preprocess(true));
	FModKitSignature::Shutdown();
	bStopped = true;
	TestTrue(TEXT("Removing ModKit validator restores preprocessing"), Preprocess(false));
	FModKitSignature::Startup();
	bStopped = false;
	AddExpectedError(TEXT("MODKIT_PACKAGE_SOURCE_REJECTED"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Restored ModKit validator rejects changed source"), Preprocess(false));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitFeatureModTrustTest,
	"AngelscriptModKit.FeatureMods.TrustPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitFeatureModTrustTest::RunTest(const FString&)
{
	FModKitModInfo Mod;
	Mod.DirectoryName = TEXT("Unsigned-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Mod.DirectoryPath = FPaths::ProjectSavedDir() / TEXT("Automation") / Mod.DirectoryName;
	FString Reason;
	EModKitModTrust Trust = EModKitModTrust::Official;
	TestEqual(TEXT("Unsigned policy follows the startup snapshot"),
		FModKitSignature::ValidateTrust(Mod, Trust, Reason), !FModKitSignature::RequiresSignature());
	TestTrue(TEXT("Unsigned classification is player"), Trust == EModKitModTrust::Player);
	TestFalse(TEXT("Query does not promote a player"), FModKitSignature::IsModSigned(Mod));
	if (FModKitSignature::RequiresSignature())
		TestEqual(TEXT("Policy refusal is explicit"), Reason, FString(TEXT("official-only")));
	TestFalse(TEXT("Unknown mod names cannot escape the roots"), FModKitModRegistry::RescanOrAdd(TEXT("../Outside")));
	TestTrue(TEXT("Unknown identity has no trusted permissions"), FModKitModRegistry::GetModTrust(Mod.DirectoryName) == EModKitModTrust::Player);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitFeatureModFirstFloorTest,
	"AngelscriptModKit.FeatureMods.SignedFirstFloor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitFeatureModFirstFloorTest::RunTest(const FString&)
{
	// Reuse the existing signed-package fixtures and their public verification key.
	const FString Fixtures = FPaths::ProjectDir() / TEXT("FeatureModPackages");
	const FString Root = FPaths::ProjectSavedDir() / TEXT("Automation/FeatureModTrust")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformPhysical();
	FModKitModInfo Mod;
	Mod.DirectoryName = TEXT("GfpModA");
	Mod.DirectoryPath = Root / Mod.DirectoryName;
	Mod.VersionName = TEXT("1.0");
	const FString FloorPath = FPaths::ProjectSavedDir() / TEXT("FeatureModVersions/GfpModA.json");
	if (!TestFalse(TEXT("First-install fixture must not replace an active GfpModA"), FModKitModRegistry::IsModActivated(Mod.DirectoryName))) return false;
	TArray<uint8> OriginalFloor;
	const bool bHadFloor = Disk.FileExists(*FloorPath);
	if (bHadFloor && !TestTrue(TEXT("Preserve provisioned fixture floor"), FModKitPatchManifest::ReadBytes(FloorPath, FModKitPatchManifest::MaxManifestBytes, OriginalFloor))) return false;
	if (!TestTrue(TEXT("Copy signed positive fixture"), Disk.CopyDirectoryTree(*Root, *(Fixtures / TEXT("positive")), true))) return false;
	const auto SavedSources = VerifiedSources;
	const auto SavedPackages = VerifiedPackages;
	const auto SavedHighest = HighestAcceptedVersions;
	ON_SCOPE_EXIT
	{
		Disk.DeleteDirectoryRecursively(*Root);
		if (bHadFloor) FFileHelper::SaveArrayToFile(OriginalFloor, *FloorPath);
		else Disk.DeleteFile(*FloorPath);
		VerifiedSources = SavedSources;
		VerifiedPackages = SavedPackages;
		HighestAcceptedVersions = SavedHighest;
	};
	if (bHadFloor && !TestTrue(TEXT("Begin isolated first-install state"), Disk.DeleteFile(*FloorPath))) return false;
	FString PublicKey;
	if (!TestTrue(TEXT("Read fixture public key"), FFileHelper::LoadFileToString(PublicKey, *(Fixtures / TEXT("keys/verify.pub.b64"))))) return false;
	PublicKey.TrimStartAndEndInline();
	if (!TestEqual(TEXT("Host uses the fixture public trust root"), FModKitSignature::PublicKeyBase64(), PublicKey)) return false;
	FString ManifestText;
	const FString ManifestPath = Root / TEXT("GfpModA.manifest.json");
	FModKitPatchManifest Manifest;
	FString Reason;
	if (!TestTrue(TEXT("Read signed manifest"), FModKitPatchManifest::ReadText(ManifestPath, ManifestText))
		|| !TestTrue(TEXT("Parse signed manifest"), FModKitPatchManifest::Parse(ManifestText, Manifest, Reason))) return false;
	EModKitModTrust Trust;
	if (!TestTrue(TEXT("First verified package installs without a provisioned floor"), FModKitSignature::ValidateTrust(Mod, Trust, Reason)))
	{ AddError(Reason); return false; }
	TestTrue(TEXT("Valid signature grants official classification"), Trust == EModKitModTrust::Official);
	FString FloorText;
	TestTrue(TEXT("First install persists its floor"), FModKitPatchManifest::ReadText(FloorPath, FloorText));
	TestEqual(TEXT("Floor preserves the authenticated manifest"), FloorText, ManifestText);
	TestTrue(TEXT("A second verification accepts the same version"), FModKitSignature::ValidateTrust(Mod, Trust, Reason));
	TestTrue(TEXT("Signature queries do not re-read disk"), FModKitSignature::IsModSigned(Mod));
	const FString PayloadPath = Mod.DirectoryPath / Manifest.Files[0].Path;
	TArray<uint8> Original;
	TestTrue(TEXT("Read fixture payload"), FModKitPatchManifest::ReadBytes(PayloadPath, FModKitPatchManifest::MaxFileBytes, Original));
	FModKitSignature::BeginReloadVerification(Mod.DirectoryPath);
	TestTrue(TEXT("Mutate official payload"), FFileHelper::SaveStringToFile(TEXT("tampered"), *PayloadPath));
	TestFalse(TEXT("Invalid official signature never downgrades to player"), FModKitSignature::ValidateTrust(Mod, Trust, Reason));
	TestEqual(TEXT("Payload failure is preserved"), Reason, FString(TEXT("package-file-hash")));
	FModKitSignature::CompleteReloadVerification(Mod.DirectoryPath, false);
	TestTrue(TEXT("Failed candidate preserves the verified active package"), FModKitSignature::IsModSigned(Mod));
	TestTrue(TEXT("Restore fixture payload"), FFileHelper::SaveArrayToFile(Original, *PayloadPath));
	const FString FloorBeforeRejection = FloorText;
	FModKitPatchManifest BadSignature = Manifest;
	BadSignature.SignatureBytes[0] ^= 1;
	TestFalse(TEXT("A changed signature is rejected"), BadSignature.VerifySignature(TEXT("package"), PublicKey,
		{Manifest.SignatureSigner}, Reason));
	TestEqual(TEXT("Changed signature has a readable reason"), Reason, FString(TEXT("signature-invalid")));
	FModKitPatchManifest BadKey = Manifest;
	BadKey.PublicKeySha256 = FString::ChrN(64, TCHAR('0'));
	TestFalse(TEXT("A mismatched public key is rejected"), BadKey.VerifySignature(TEXT("package"), PublicKey,
		{Manifest.SignatureSigner}, Reason));
	TestEqual(TEXT("Key mismatch has a readable reason"), Reason, FString(TEXT("signature-anchor")));
	TestFalse(TEXT("An untrusted key ID is rejected at the authenticated boundary"), Manifest.VerifySignature(
		TEXT("package"), PublicKey, {TEXT("another-publisher")}, Reason));
	TestEqual(TEXT("Key ID rejection has a readable reason"), Reason, FString(TEXT("key-id-untrusted")));
	TestTrue(TEXT("Read floor after rejected signature and keys"), FModKitPatchManifest::ReadText(FloorPath, FloorText));
	TestEqual(TEXT("Rejected authentication cannot advance the floor"), FloorText, FloorBeforeRejection);
	TestTrue(TEXT("Corrupt floor"), FFileHelper::SaveStringToFile(TEXT("{}"), *FloorPath));
	TestFalse(TEXT("Corrupt floor is not treated as first install"), FModKitSignature::ValidateExternal(Mod, Reason));
	TestEqual(TEXT("Corrupt floor failure is explicit"), Reason, FString(TEXT("version-state-invalid")));
	FString HigherFloor;
	TestTrue(TEXT("Read signed newer floor"), FModKitPatchManifest::ReadText(Fixtures / TEXT("security/FloorV2.json"), HigherFloor));
	TestTrue(TEXT("Install signed newer floor"), FFileHelper::SaveStringToFile(HigherFloor, *FloorPath));
	TestFalse(TEXT("Older signed package cannot roll back"), FModKitSignature::ValidateExternal(Mod, Reason));
	TestEqual(TEXT("Rollback failure is explicit"), Reason, FString(TEXT("version-rollback")));
	FString UnknownSigner = ManifestText.Replace(*Manifest.SignatureSigner, TEXT("unrecognized-test-publisher"));
	TestTrue(TEXT("Write unknown-signer player manifest"), FFileHelper::SaveStringToFile(UnknownSigner, *ManifestPath));
	FModKitSignature::BeginReloadVerification(Mod.DirectoryPath);
	TestEqual(TEXT("Unknown signer follows player policy"), FModKitSignature::ValidateTrust(Mod, Trust, Reason),
		!FModKitSignature::RequiresSignature());
	TestTrue(TEXT("Unknown signer gains no official permissions"), Trust == EModKitModTrust::Player);
	if (!FModKitSignature::RequiresSignature())
		TestFalse(TEXT("Player replacement clears official source snapshot"), FModKitSignature::IsModSigned(Mod));
	FModKitSignature::CompleteReloadVerification(Mod.DirectoryPath, false);
	TestTrue(TEXT("Compile failure restores official source identity"), FModKitSignature::IsModSigned(Mod));
	FString FloorAfterPlayer;
	TestTrue(TEXT("Read floor after player classification"), FModKitPatchManifest::ReadText(FloorPath, FloorAfterPlayer));
	TestEqual(TEXT("Player classification does not change the signed floor"), FloorAfterPlayer, HigherFloor);
	return true;
}
#endif
