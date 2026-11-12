// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch atomic landing implementation .

#include "HotPatch/ModKitHotPatchLand.h"

#include "HotPatch/ModKitHotPatchFetch.h"
#include "Shared/Delivery/ModKitPatchManifest.h"
#include "Core/ModKitSettings.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "AngelscriptManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
	bool ReplaceStagedTree(IFileManager& FM, const FString& FinalDir, const FString& TmpDir,
		const FString& BackupDir, bool bHadPrevious, FString& OutReason, FString& OutDetail)
	{
		if (bHadPrevious)
		{
			if (!FM.Move(*BackupDir, *FinalDir, false))
			{
				FM.DeleteDirectory(*TmpDir, false, true);
				OutReason = TEXT("io");
				OutDetail = TEXT("backup:") + FinalDir;
				return false;
			}
		}
		if (!FM.Move(*FinalDir, *TmpDir, false))
		{
			const bool bRestored = !bHadPrevious || FM.Move(*FinalDir, *BackupDir, false);
			FM.DeleteDirectory(*TmpDir, false, true);
			OutReason = TEXT("io");
			OutDetail = (bRestored ? TEXT("rename:") : TEXT("restore-required:")) + FinalDir;
			return false;
		}
		if (bHadPrevious) FM.DeleteDirectory(*BackupDir, false, true);
		return true;
	}
}

FString FModKitHotPatchLand::DefaultStagingRoot()
{
	return FPaths::ProjectSavedDir() / TEXT("ModKitPatches");
}

FString FModKitHotPatchLand::ResolveStagingRoot()
{
	const UModKitSettings* Settings = UModKitSettings::Get();
	FString Root = FPaths::ConvertRelativePathToFull(Settings->PatchStagingRoot.IsEmpty()
		? DefaultStagingRoot() : Settings->PatchStagingRoot);
	FPaths::NormalizeDirectoryName(Root);
	if (!FPaths::CollapseRelativeDirectories(Root)) return FString();
	// Staging is writable only by the application/installer. These checks reject
	// existing aliases; they do not defend against a local writer replacing a
	// checked directory later. Never grant untrusted users staging write access.
	for (FString Part = Root; !Part.IsEmpty(); )
	{
		if (FPlatformFileManager::Get().GetPlatformFile().IsSymlink(*Part) == ESymlinkResult::Symlink)
			return FString();
#if PLATFORM_WINDOWS
		const DWORD Attributes = GetFileAttributesW(*Part);
		if (Attributes != INVALID_FILE_ATTRIBUTES)
		{
			if ((Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return FString();
			// Expand each existing ancestor, including when the final directory
			// does not exist yet. A short alias must not bypass root containment.
			const DWORD Required = GetLongPathNameW(*Part, nullptr, 0);
			if (Required == 0) return FString();
			TArray<TCHAR> LongPath;
			LongPath.SetNumUninitialized(Required);
			const DWORD Written = GetLongPathNameW(*Part, LongPath.GetData(), Required);
			if (Written == 0 || Written >= Required) return FString();
			FString Expanded(LongPath.GetData());
			FPaths::NormalizeDirectoryName(Expanded);
			if (!Expanded.Equals(Part, ESearchCase::IgnoreCase)) return FString();
		}
#endif
		const FString Parent = FPaths::GetPath(Part);
		if (Parent == Part) break;
		Part = Parent;
	}
	TArray<FString> ScriptRoots = FAngelscriptManager::MakeAllScriptRoots();
	ScriptRoots.Add(FPaths::ProjectDir() / TEXT("FeatureMods"));
	ScriptRoots.Add(FPaths::ProjectPluginsDir());
	for (FString ScriptRoot : ScriptRoots)
	{
		ScriptRoot = FPaths::ConvertRelativePathToFull(ScriptRoot);
		FPaths::NormalizeDirectoryName(ScriptRoot);
		FPaths::CollapseRelativeDirectories(ScriptRoot);
		if (Root.Equals(ScriptRoot, ESearchCase::IgnoreCase)
			|| FPaths::IsUnderDirectory(Root.ToLower(), ScriptRoot.ToLower())
			|| FPaths::IsUnderDirectory(ScriptRoot.ToLower(), Root.ToLower())) return FString();
	}
	return Root;
}

bool FModKitHotPatchLand::ValidatePatch(const FModKitFetchedPatch& Patch, const FString& PublicKeyBase64,
	FModKitPatchManifest& OutManifest, FString& OutReason, FString& OutDetail)
{
	OutReason.Reset();
	OutDetail.Reset();
	// --- 1. schema -----------------------------------------------------------
	if (!FModKitPatchManifest::Parse(Patch.ManifestText, OutManifest, OutDetail))
	{
		OutReason = TEXT("schema");
		return false;
	}

	if (OutManifest.Kind != TEXT("hotpatch")) { OutReason = TEXT("kind"); return false; }

	// Shared owns signed-delivery verification; HotPatch retains its apply policy.
#if UE_BUILD_SHIPPING
	const FString TrustRoot = TEXT(MODKIT_DELIVERY_PUBLIC_KEY);
	const TArray<FString> TrustedKeyIds = { TEXT(MODKIT_DELIVERY_KEY_ID) };
#else
	const FString& TrustRoot = PublicKeyBase64;
	const UModKitSettings* Settings = UModKitSettings::Get();
	TArray<FString> TrustedKeyIds = Settings->PatchAcceptedKeyIds;
	TrustedKeyIds.AddUnique(Settings->PatchVerifyKeyId);
#endif
	if (!OutManifest.VerifySignature(TEXT("hotpatch"), TrustRoot, TrustedKeyIds, OutDetail))
	{
		OutReason = OutDetail == TEXT("key-missing") || OutDetail == TEXT("key-id-untrusted")
			? OutDetail : TEXT("bad-signature");
		return false;
	}
	if (!OutManifest.VerifyFiles(Patch.Files, OutDetail))
	{
		OutReason = TEXT("sha-mismatch");
		return false;
	}

	// --- 5. precheck + engine-version gates ----------------------------------
	if (OutManifest.Precheck.Verdict != TEXT("PASS"))
	{
		OutReason = TEXT("precheck-required");
		OutDetail = FString::Printf(TEXT("verdict:%s"), *OutManifest.Precheck.Verdict);
		return false;
	}
	if (!OutManifest.MatchesEngineVersion())
	{
		OutReason = TEXT("engine-mismatch");
		OutDetail = FString::Printf(TEXT("manifest:%s runtime:%s"),
			*OutManifest.EngineVersion, *ModKitPatchEngineVersionString());
		return false;
	}
	return true;
}

bool FModKitHotPatchLand::Land(const FModKitFetchedPatch& Patch, const FString& PublicKeyBase64,
	FString& OutPatchId, int32& OutFileCount, FString& OutReason, FString& OutDetail)
{
	const UModKitSettings* Settings = UModKitSettings::Get();
	if (!Settings->bEnableHotPatch)
	{
		OutReason = TEXT("disabled");
		return false;
	}

	// --- 1-5. full MK-T702 verification chain on the in-memory patch ---------
	FModKitPatchManifest Manifest;
	if (!ValidatePatch(Patch, PublicKeyBase64, Manifest, OutReason, OutDetail))
	{
		return false;
	}

	// --- 6. atomic landing: write <PatchId>.tmp-incoming, then replace -------
	const FString StagingRoot = ResolveStagingRoot();
	if (StagingRoot.IsEmpty())
	{
		OutReason = TEXT("config");
		OutDetail = TEXT("staging-root-overlaps-script-root");
		return false;
	}
	// PatchId is schema-validated ([A-Za-z0-9-]); refuse anything path-like anyway.
	if (Manifest.PatchId.Contains(TEXT("/")) || Manifest.PatchId.Contains(TEXT("\\")) || Manifest.PatchId.Contains(TEXT("..")))
	{
		OutReason = TEXT("schema");
		OutDetail = TEXT("patchId-path-like");
		return false;
	}
	const FString FinalDir = StagingRoot / Manifest.PatchId;
	const FString TmpDir = StagingRoot / (Manifest.PatchId + TEXT(".tmp-incoming"));
	const FString BackupDir = StagingRoot / (Manifest.PatchId + TEXT(".previous"));

	IFileManager& FM = IFileManager::Get();
	FM.DeleteDirectory(*TmpDir, false, true);
	for (const FModKitPatchFileEntry& Entry : Manifest.Files)
	{
		const FString Dest = TmpDir / Entry.Path;
		if (!FM.MakeDirectory(*FPaths::GetPath(Dest), true)
			|| !FFileHelper::SaveArrayToFile(Patch.Files[Entry.Path], *Dest))
		{
			FM.DeleteDirectory(*TmpDir, false, true);
			OutReason = TEXT("io");
			OutDetail = Dest;
			return false;
		}
	}
	// Manifest.json itself lands alongside the payload so a staged patch is
	// self-describing (T704 apply re-reads it).
	if (!FFileHelper::SaveStringToFile(Patch.ManifestText, *(TmpDir / TEXT("Manifest.json"))))
	{
		FM.DeleteDirectory(*TmpDir, false, true);
		OutReason = TEXT("io");
		OutDetail = TmpDir / TEXT("Manifest.json");
		return false;
	}
	const bool bHadPrevious = FM.DirectoryExists(*FinalDir);
	if (FM.DirectoryExists(*BackupDir))
	{
		FM.DeleteDirectory(*TmpDir, false, true);
		OutReason = TEXT("io");
		OutDetail = TEXT("previous-recovery-required");
		return false;
	}
	if (bHadPrevious)
	{
		FString OldText;
		FModKitPatchManifest OldManifest;
		FString ParseReason;
		if (!FFileHelper::LoadFileToString(OldText, *(FinalDir / TEXT("Manifest.json")))
			|| !FModKitPatchManifest::Parse(OldText, OldManifest, ParseReason)
			|| !Manifest.IsVersionAtLeast(OldManifest.Version))
		{
			FM.DeleteDirectory(*TmpDir, false, true);
			OutReason = TEXT("version");
			return false;
		}
	}
	// Cooked sandbox reads can fall back to an installer/editor tree outside
	// the write view. Keep its version check above, but never try to move that
	// read-only fallback: only a previous writable tree needs backup/restore.
	const FString WritableFinalDir = FM.ConvertToAbsolutePathForExternalAppForWrite(*FinalDir);
	const bool bHadWritablePrevious = FPlatformFileManager::Get().GetPlatformPhysical().DirectoryExists(*WritableFinalDir);
	if (!ReplaceStagedTree(FM, FinalDir, TmpDir, BackupDir, bHadWritablePrevious, OutReason, OutDetail))
	{
		return false;
	}

	OutPatchId = Manifest.PatchId;
	OutFileCount = Manifest.Files.Num();
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
#include "HAL/FileManagerGeneric.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitLandingRestoreTest,
	"AngelscriptModKit.Security.LandingRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitLandingRestoreTest::RunTest(const FString&)
{
	class FFailIncomingMove final : public FFileManagerGeneric
	{
	public:
		virtual bool Move(const TCHAR* Dest, const TCHAR* Src, bool Replace = true,
			bool EvenIfReadOnly = false, bool Attributes = false, bool bDoNotRetryOrError = false) override
		{
			if (FString(Src).EndsWith(TEXT(".tmp-incoming"))) return false;
			return FFileManagerGeneric::Move(Dest, Src, Replace, EvenIfReadOnly, Attributes, bDoNotRetryOrError);
		}
	} FM;
	const FString Root = FPaths::ProjectSavedDir() / TEXT("Automation/LandingRestore") / FGuid::NewGuid().ToString();
	const FString Final = Root / TEXT("patch");
	const FString Incoming = Final + TEXT(".tmp-incoming");
	const FString Backup = Final + TEXT(".previous");
	FM.MakeDirectory(*Final, true);
	FM.MakeDirectory(*Incoming, true);
	TestTrue(TEXT("Writes old payload"), FFileHelper::SaveStringToFile(TEXT("old payload"), *(Final / TEXT("old.as"))));
	TestTrue(TEXT("Writes old manifest"), FFileHelper::SaveStringToFile(TEXT("old manifest"), *(Final / TEXT("Manifest.json"))));
	TestTrue(TEXT("Writes incoming payload"), FFileHelper::SaveStringToFile(TEXT("new payload"), *(Incoming / TEXT("new.as"))));
	FString Reason, Detail, Text;
	TestFalse(TEXT("Second rename fails"), ReplaceStagedTree(FM, Final, Incoming, Backup, true, Reason, Detail));
	TestEqual(TEXT("Failure is I/O"), Reason, FString(TEXT("io")));
	TestTrue(TEXT("Restored rather than restore-required"), Detail.StartsWith(TEXT("rename:")));
	TestTrue(TEXT("Old payload is readable"), FFileHelper::LoadFileToString(Text, *(Final / TEXT("old.as"))));
	TestEqual(TEXT("Old payload preserved"), Text, FString(TEXT("old payload")));
	TestTrue(TEXT("Old manifest is readable"), FFileHelper::LoadFileToString(Text, *(Final / TEXT("Manifest.json"))));
	TestEqual(TEXT("Old manifest preserved"), Text, FString(TEXT("old manifest")));
	TestFalse(TEXT("No backup poisons next landing"), FM.DirectoryExists(*Backup));
	TestFalse(TEXT("No incoming residue"), FM.DirectoryExists(*Incoming));
	FM.MakeDirectory(*Incoming, true);
	FFileHelper::SaveStringToFile(TEXT("retry"), *(Incoming / TEXT("new.as")));
	TestTrue(TEXT("A real retry succeeds"), ReplaceStagedTree(IFileManager::Get(), Final, Incoming, Backup, true, Reason, Detail));
	TestTrue(TEXT("Retry installed new payload"), FFileHelper::LoadFileToString(Text, *(Final / TEXT("new.as"))));
	TestEqual(TEXT("Retry bytes"), Text, FString(TEXT("retry")));
	FM.DeleteDirectory(*Root, false, true);
	return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitHotPatchSharedDeliveryTest, "AngelscriptModKit.HotPatch.SharedDelivery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitHotPatchSharedDeliveryTest::RunTest(const FString& Parameters)
{
	const FString Fixture = FPaths::ProjectDir() / TEXT("HotPatch/positive-valid");
	FModKitFetchedPatch Patch;
	FModKitPatchManifest Manifest;
	FString Reason, Detail;
	if (!TestTrue(TEXT("Read signed fixture"), FModKitPatchManifest::ReadText(Fixture / TEXT("Manifest.json"), Patch.ManifestText))
		|| !TestTrue(TEXT("Parse fixture"), FModKitPatchManifest::Parse(Patch.ManifestText, Manifest, Reason))) return false;
	for (const FModKitPatchFileEntry& Entry : Manifest.Files)
	{
		TArray<uint8> Bytes;
		if (!TestTrue(TEXT("Read signed payload"), FModKitPatchManifest::ReadBytes(Fixture / Entry.Path, FModKitPatchManifest::MaxFileBytes, Bytes))) return false;
		Patch.Files.Add(Entry.Path, MoveTemp(Bytes));
	}
	const FString PublicKey = TEXT("bqQ94LWkdv0bTbv7O0yt+TM6JH1qkRZ5fokdmWRoL8s=");
	TestTrue(TEXT("Shared signature and exact snapshot accepted"), FModKitHotPatchLand::ValidatePatch(Patch, PublicKey, Manifest, Reason, Detail));
	TestTrue(TEXT("Success clears reason"), Reason.IsEmpty() && Detail.IsEmpty());
	FModKitFetchedPatch Invalid = Patch;
	Invalid.Files.FindChecked(Manifest.Files[0].Path).Add(1);
	TestFalse(TEXT("Tampered snapshot rejected"), FModKitHotPatchLand::ValidatePatch(Invalid, PublicKey, Manifest, Reason, Detail));
	TestEqual(TEXT("Hash failure"), Reason, FString(TEXT("sha-mismatch")));
	Invalid = Patch;
	Invalid.Files.Add(TEXT("Unlisted.as"), {});
	TestFalse(TEXT("Extra payload rejected"), FModKitHotPatchLand::ValidatePatch(Invalid, PublicKey, Manifest, Reason, Detail));
	TestEqual(TEXT("File-set failure"), Detail, FString(TEXT("package-file-set-mismatch")));
	Invalid = Patch;
	Invalid.ManifestText.ReplaceInline(TEXT("modkit-g10-sign"), TEXT("untrusted-signer"));
	TestFalse(TEXT("Untrusted signer rejected before landing"), FModKitHotPatchLand::ValidatePatch(Invalid, PublicKey, Manifest, Reason, Detail));
	TestEqual(TEXT("Signer failure"), Reason, FString(TEXT("key-id-untrusted")));
	Invalid = Patch;
	Invalid.ManifestText.ReplaceInline(TEXT("\"format\":"), TEXT("\"kind\":\"package\",\"format\":"));
	TestFalse(TEXT("Package cannot enter HotPatch"), FModKitHotPatchLand::ValidatePatch(Invalid, PublicKey, Manifest, Reason, Detail));
	TestEqual(TEXT("Kind failure"), Reason, FString(TEXT("kind")));
	return true;
}
#endif
