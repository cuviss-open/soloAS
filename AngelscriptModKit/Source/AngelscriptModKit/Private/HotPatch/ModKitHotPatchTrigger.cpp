// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch explicit trigger implementation .

#include "HotPatch/ModKitHotPatchTrigger.h"

#include "AngelscriptCodeModule.h"
#include "AngelscriptManager.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "HotPatch/ModKitHotPatch.h"
#include "HotPatch/ModKitHotPatchFetch.h"
#include "HotPatch/ModKitHotPatchLand.h"
#include "Shared/Delivery/ModKitPatchManifest.h"
#include "Core/ModKitSettings.h"
#include "Preprocessor/AngelscriptPreprocessor.h"

#include "HAL/FileManager.h"
#include "HAL/CriticalSection.h"
#include "Misc/Crc.h"
#include "HAL/PlatformFileManager.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#else
#include <cstdio>
#endif
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
#if !WITH_EDITOR
	bool PersistAcceptedVersion(const FString& VersionPath, const FString& Version)
	{
		// Resolve through the sandbox wrapper before using the native atomic
		// replace: IFileManager::Move may delete the destination before moving.
		const FString Destination = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*VersionPath);
		const FString Temporary = Destination + TEXT(".tmp");
		IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformPhysical();
		TUniquePtr<IFileHandle> File(Files.OpenWrite(*Temporary));
		if (!File) return false;
		FTCHARToUTF8 Bytes(*Version);
		const bool bWritten = File->Write(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length())
			&& File->Flush(true);
		File.Reset();
		bool bPublished = false;
		if (bWritten)
		{
#if PLATFORM_WINDOWS
			bPublished = MoveFileExW(*Temporary, *Destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
			bPublished = ::rename(TCHAR_TO_UTF8(*Temporary), TCHAR_TO_UTF8(*Destination)) == 0;
#endif
		}
		if (!bPublished) Files.DeleteFile(*Temporary);
		return bPublished;
	}
#endif

	const TCHAR* PatchTriggerCompileResultReason(ECompileResult Result)
	{
		switch (Result)
		{
		case ECompileResult::ErrorNeedFullReload: return TEXT("structural-required");
		case ECompileResult::Error: return TEXT("compile-error");
		default: return TEXT("compile-error");
		}
	}

	void PatchTriggerReject(const FString& PatchId, const FString& Reason)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_REJECTED op=apply patch=%s reason=%s"),
			*PatchId, *Reason);
	}

	bool PatchTriggerFail(const FString& PatchId, const FString& Reason)
	{
		FModKitHotPatch::Ledger.Add(PatchId, EModKitPatchState::Failed);
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_FAILED patch=%s reason=%s"), *PatchId, *Reason);
		UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_STATE patch=%s state=failed"), *PatchId);
		if (!UModKitSettings::Get()->bKeepFailedPatchOnDisk && !FModKitHotPatchLand::ResolveStagingRoot().IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(
				*(FModKitHotPatchLand::ResolveStagingRoot() / PatchId), false, true);
		}
		return false;
	}

	FString PendingPatchId;
	TSharedPtr<FAngelscriptPreprocessor> PendingPreprocessor;

	bool CompilePreparedPatch(const FString& PatchId, bool& bDeferred)
	{
		bDeferred = false;
		const auto Modules = PendingPreprocessor->GetModulesToCompile();
		FAngelscriptManager& Manager = FAngelscriptManager::Get();
		// Cache descriptors borrow buffers that a source-only compile never re-enters.
		TArray<TPair<TSharedRef<FAngelscriptModuleDesc>, const FAngelscriptPrecompiledModule*>> PreviousPrecompiledData;
		for (const auto& ActiveModule : Manager.GetActiveModules())
		{
			PreviousPrecompiledData.Emplace(ActiveModule, ActiveModule->PrecompiledData);
			ActiveModule->PrecompiledData = nullptr;
		}
		auto RestorePrecompiledData = [&PreviousPrecompiledData]()
		{
			for (const auto& Previous : PreviousPrecompiledData)
				Previous.Key->PrecompiledData = Previous.Value;
		};

		TArray<TSharedRef<FAngelscriptModuleDesc>> CompiledModules;
		const ECompileResult Result = FModKitCompilationManager::CompileOwnedSourceModules(
			ECompileType::SoftReloadOnly, Modules, CompiledModules);
		if (Result == ECompileResult::Deferred)
		{
			RestorePrecompiledData();
			bDeferred = true;
			if (FModKitHotPatch::Ledger.FindRef(PatchId) != EModKitPatchState::Pending)
				UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_STATE patch=%s state=pending"), *PatchId);
			FModKitHotPatch::Ledger.Add(PatchId, EModKitPatchState::Pending);
			return false;
		}
		PendingPreprocessor.Reset();
		PendingPatchId.Reset();
		if (Result == ECompileResult::FullyHandled)
		{
			FModKitHotPatch::Ledger.Add(PatchId, EModKitPatchState::Applied);
			UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_APPLIED patch=%s modules=%d result=FullyHandled"),
				*PatchId, CompiledModules.Num());
			UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_STATE patch=%s state=applied"), *PatchId);
			return true;
		}
		if (Result == ECompileResult::PartiallyHandled)
		{
			FModKitHotPatch::Ledger.Add(PatchId, EModKitPatchState::AppliedPartial);
			UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_PATCH_APPLIED_PARTIAL patch=%s modules=%d result=PartiallyHandled reason=unexpected-suggested"),
				*PatchId, CompiledModules.Num());
			UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_STATE patch=%s state=applied-partial"), *PatchId);
			return true;
		}
		RestorePrecompiledData();
		return PatchTriggerFail(PatchId, PatchTriggerCompileResultReason(Result));
	}
}

void FModKitHotPatchTrigger::Shutdown()
{
	PendingPreprocessor.Reset();
	PendingPatchId.Reset();
}

bool FModKitHotPatchTrigger::Apply(const FString& PatchId, bool& bDeferred)
{
	bDeferred = false;
	// H3 is a game-thread runtime operation; editor/commandlet contexts never
	// apply (red gate apply-editor-reject, statically asserted).
#if WITH_EDITOR
	PatchTriggerReject(PatchId, TEXT("editor"));
	return false;
#else
	const UModKitSettings* Settings = UModKitSettings::Get();
	if (!Settings->bEnableHotPatch)
	{
		PatchTriggerReject(PatchId, TEXT("disabled"));
		return false;
	}
	if (PatchId.IsEmpty() || PatchId.Contains(TEXT("/")) || PatchId.Contains(TEXT("\\")) || PatchId.Contains(TEXT("..")))
	{
		PatchTriggerReject(PatchId, TEXT("unknown-patch"));
		return false;
	}

	// Session-ledger terminal states: never replay (v1 session semantics).
	if (const EModKitPatchState* State = FModKitHotPatch::Ledger.Find(PatchId))
	{
		if (*State == EModKitPatchState::Applied || *State == EModKitPatchState::AppliedPartial)
		{
			PatchTriggerReject(PatchId, TEXT("already-applied"));
			return false;
		}
		if (*State == EModKitPatchState::Failed)
		{
			PatchTriggerReject(PatchId, TEXT("already-failed"));
			return false;
		}
	}

	if (PendingPreprocessor)
	{
		if (PendingPatchId != PatchId)
		{
			PatchTriggerReject(PatchId, TEXT("busy"));
			return false;
		}
		return CompilePreparedPatch(PatchId, bDeferred);
	}

	const FString StagingRoot = FModKitHotPatchLand::ResolveStagingRoot();
	if (StagingRoot.IsEmpty())
	{
		PatchTriggerReject(PatchId, TEXT("staging-root"));
		return false;
	}
	const FString StagedDir = StagingRoot / PatchId;
	if (!IFileManager::Get().DirectoryExists(*StagedDir))
	{
		PatchTriggerReject(PatchId, TEXT("not-staged"));
		return false;
	}

	// H3 re-verification: read the staged tree back into memory and re-run the
	// full MK-T702 chain against the on-disk bytes (post-landing tamper is a
	// rejection, never a compile attempt).
	FModKitFetchedPatch Patch;
	FString Reason;
	FString Detail;
	if (!FModKitHotPatchFetch::Fetch(StagedDir, Settings->PatchMaxBytes, Settings->PatchFetchTimeoutSec,
		Patch, Reason, Detail))
	{
		PatchTriggerReject(PatchId, TEXT("not-staged"));
		return false;
	}
	FModKitPatchManifest Manifest;
	if (!FModKitHotPatchLand::ValidatePatch(Patch, UModKitSettings::Get()->PatchPublicKeyBase64, Manifest, Reason, Detail))
	{
		PatchTriggerReject(PatchId, Reason);
		return false;
	}
	for (const FModKitPatchFileEntry& Entry : Manifest.Files)
		if (FModKitCompilationManager::IsLeafPatchPath(Entry.Path))
		{
			PatchTriggerReject(PatchId, TEXT("leaf-hotpatch-owner-unavailable"));
			return false;
		}

	if (FAngelscriptManager::bStaticJITTranspiledCodeLoaded)
	{
		for (const FModKitPatchFileEntry& Entry : Manifest.Files)
		{
			FString ModuleName = Entry.Path.LeftChop(3).Replace(TEXT("/"), TEXT("."));
			const auto Active = FAngelscriptManager::Get().GetModule(ModuleName);
			if (Active.IsValid() && Active->bLoadedPrecompiledCode)
			{
				PatchTriggerReject(PatchId, TEXT("static-target"));
				return false;
			}
		}
	}

	const FString VersionPath = FPaths::ProjectSavedDir() / TEXT("ModKitPatchVersion.txt");
	// Serialize the version check and atomic publication across game processes.
	const FString WatermarkFile = IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(*VersionPath);
	FSystemWideCriticalSection VersionLock(FString::Printf(TEXT("ModKitPatchVersion-%08x"), FCrc::StrCrc32(*WatermarkFile.ToLower())));
	if (!VersionLock.IsValid())
	{
		PatchTriggerReject(PatchId, TEXT("busy"));
		return false;
	}
	FString PreviousVersion;
	if (Manifest.PatchId != PatchId
		|| (IFileManager::Get().FileExists(*VersionPath)
			&& (!FFileHelper::LoadFileToString(PreviousVersion, *VersionPath)
				|| !Manifest.IsVersionAtLeast(PreviousVersion))))
	{
		PatchTriggerReject(PatchId, TEXT("version"));
		return false;
	}

	// The watermark is the highest accepted attempt, including failed compiles.
	// Publish durably before any preprocessing, compilation or module swap.
	if (!PersistAcceptedVersion(VersionPath, Manifest.Version))
	{
		return PatchTriggerFail(PatchId, TEXT("version-write"));
	}

	VersionLock.Release();

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_COMPILE_BEGIN patch=%s files=%d"),
		*PatchId, Manifest.Files.Num());

	// Build module descriptors from the staged bytes (patch-relative path is
	// the script-relative path; same mapping as the precheck collector).
	TSharedPtr<FAngelscriptPreprocessor> Preprocessor = MakeShared<FAngelscriptPreprocessor>();
	TArray<FModKitPatchFileEntry> SortedFiles = Manifest.Files;
	SortedFiles.Sort([](const FModKitPatchFileEntry& A, const FModKitPatchFileEntry& B)
	{
		return A.Path.Compare(B.Path, ESearchCase::CaseSensitive) < 0;
	});
	for (const FModKitPatchFileEntry& Entry : SortedFiles)
	{
		Preprocessor->AddFile(Entry.Path, StagedDir / Entry.Path, false);
		// Compile the exact fetched and verified bytes, independent of later disk changes.
		const TArray<uint8>& Bytes = Patch.Files.FindChecked(Entry.Path);
		FFileHelper::BufferToString(Preprocessor->Files.Last().RawCode, Bytes.GetData(), Bytes.Num());
	}
	if (!Preprocessor->Preprocess())
	{
		return PatchTriggerFail(PatchId, TEXT("preprocess"));
	}

	TArray<TSharedRef<FAngelscriptModuleDesc>> Modules = Preprocessor->GetModulesToCompile();
	if (Modules.Num() == 0)
	{
		return PatchTriggerFail(PatchId, TEXT("preprocess"));
	}

	PendingPatchId = PatchId;
	PendingPreprocessor = Preprocessor;
	return CompilePreparedPatch(PatchId, bDeferred);
#endif
}

#if WITH_DEV_AUTOMATION_TESTS
#include "AngelscriptExecutionQuiescence.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "angelscript.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitDeferredCallerTest, "AngelscriptModKit.HotPatch.DeferredCaller",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

static bool RunDeferredCaller(FAutomationTestBase& Test)
{
	auto& Manager = FAngelscriptManager::Get();
	if (!Test.TestFalse(TEXT("No unrelated patch transaction is active"), PendingPreprocessor.IsValid())) return false;
	const FString PatchId(TEXT("deferred-caller-probe"));
	const FString Filename = FPaths::ProjectSavedDir() / TEXT("ModKitDeferredCallerProbe.as");
	auto Prepare = [&](int32 Value)
	{
		auto Preprocessor = MakeShared<FAngelscriptPreprocessor>();
		Preprocessor->AddFile(TEXT("ModKitDeferredCallerProbe.as"), Filename, false);
		Preprocessor->Files.Last().RawCode = FString::Printf(TEXT("int ModKitDeferredCallerValue() { return %d; }\n"), Value);
		if (!Preprocessor->Preprocess()) Test.AddError(TEXT("Prepare the focused source fixture"));
		return Preprocessor;
	};
	auto Initial = Prepare(17);
	TArray<TSharedRef<FAngelscriptModuleDesc>> Compiled;
	if (!Test.TestEqual(TEXT("Install a real source baseline"),
		uint8(FModKitCompilationManager::CompileOwnedSourceModules(ECompileType::FullReload,
			Initial->GetModulesToCompile(), Compiled)), uint8(ECompileResult::FullyHandled))) return false;
	ON_SCOPE_EXIT
	{
		FModKitHotPatchTrigger::Shutdown();
		FModKitHotPatch::Ledger.Remove(PatchId);
		FAngelscriptPreprocessor Removal;
		Removal.AddFile(TEXT("ModKitDeferredCallerProbe.as"), Filename, false, true);
		if (Removal.Preprocess())
			FModKitCompilationManager::CompileOwnedSourceModules(ECompileType::FullReload, Removal.GetModulesToCompile(), Compiled);
	};
	auto Read = [&]() -> int32
	{
		auto* Script = Manager.GetScriptEngine()->GetModule("ModKitDeferredCallerProbe");
		auto* Function = Script ? Script->GetFunctionByDecl("int ModKitDeferredCallerValue()") : nullptr;
		if (!Test.TestNotNull(TEXT("The active source owns the callable function"), Function)) return -1;
		auto* Context = Manager.GetScriptEngine()->CreateContext();
		Context->Prepare(Function);
		Test.TestEqual(TEXT("Execute the actual source method"), Context->Execute(), int(asEXECUTION_FINISHED));
		const int32 Value = int32(Context->GetReturnDWord());
		Context->Release();
		return Value;
	};
	Test.TestEqual(TEXT("Original method is active"), Read(), 17);
	const auto Previous = Manager.GetModule(TEXT("ModKitDeferredCallerProbe"));
	const int32 QueuedBefore = Manager.DeferredSourceCompilations.Num();
	PendingPatchId = PatchId;
	PendingPreprocessor = Prepare(27);
	const auto Frozen = PendingPreprocessor;
	FModKitHotPatch::Ledger.Add(PatchId, EModKitPatchState::Staged);
	bool bDeferred = false;
	{
		FAngelscriptExecutionReadScope Reader;
		Test.TestFalse(TEXT("A live reader prevents patch completion"), CompilePreparedPatch(PatchId, bDeferred));
		Test.TestTrue(TEXT("The caller is explicitly pending"), bDeferred);
		Test.TestEqual(TEXT("The ledger has no Failed or Applied terminal state"),
			uint8(FModKitHotPatch::Ledger.FindChecked(PatchId)), uint8(EModKitPatchState::Pending));
		Test.TestTrue(TEXT("Pending retains the exact prepared source"), PendingPreprocessor == Frozen);
		Test.TestTrue(TEXT("The old module stays published during deferral"), Manager.GetModule(TEXT("ModKitDeferredCallerProbe")) == Previous);
		Test.TestEqual(TEXT("Old behavior remains callable during deferral"), Read(), 17);
		Test.TestEqual(TEXT("There is no orphan Runtime queue entry"), Manager.DeferredSourceCompilations.Num(), QueuedBefore);
	}
	Test.TestTrue(TEXT("The owning caller commits at the next safe boundary"), CompilePreparedPatch(PatchId, bDeferred));
	Test.TestFalse(TEXT("Completion is no longer pending"), bDeferred);
	Test.TestEqual(TEXT("Only completed publication becomes Applied"),
		uint8(FModKitHotPatch::Ledger.FindChecked(PatchId)), uint8(EModKitPatchState::Applied));
	Test.TestFalse(TEXT("Completion releases the prepared transaction"), PendingPreprocessor.IsValid());
	Test.TestEqual(TEXT("New method body is actually published"), Read(), 27);
	const auto Published = Manager.GetModule(TEXT("ModKitDeferredCallerProbe"));
	Manager.DrainDeferredSourceCompilations();
	Test.TestTrue(TEXT("A later Runtime tick cannot replay the caller's transaction"), Manager.GetModule(TEXT("ModKitDeferredCallerProbe")) == Published);
	Test.AddInfo(TEXT("MODKIT_DEFERRED_CALLER_PASS pending=1 old=17 applied=27 orphanQueue=0 replay=0"));
	return true;
}

bool FModKitDeferredCallerTest::RunTest(const FString&)
{
	return RunDeferredCaller(*this);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitDeferredCallerWithCacheTest,
	"AngelscriptModKit.HotPatch.DeferredCallerWithCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitDeferredCallerWithCacheTest::RunTest(const FString&)
{
	if (!TestTrue(TEXT("The independent cache is loaded"),
		FModKitCompilationManager::HasCacheSet(TEXT("H5Collection")))) return false;
	return RunDeferredCaller(*this);
}

#endif
