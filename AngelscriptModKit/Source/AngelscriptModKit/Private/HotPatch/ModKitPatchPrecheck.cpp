// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned offline patch precheck commandlet (-run=ModKitPatchPrecheck).

#include "HotPatch/ModKitPatchPrecheck.h"

#include "FeatureMods/ModKitCompilationManager.h"

#if WITH_EDITOR
#include "AngelscriptManager.h"
#include "Preprocessor/AngelscriptPreprocessor.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/LinkerLoad.h"
#endif

#if WITH_EDITOR
namespace
{
	// Same module-name derivation as FAngelscriptPreprocessor::FilenameToModuleName.
	FString ToModuleName(const FString& RelativePath)
	{
		return RelativePath.Replace(TEXT(".as"), TEXT("")).Replace(TEXT("/"), TEXT("."));
	}

	void PrecheckCollectScriptFilesRecursive(
		IFileManager& FileManager,
		const FString& RelativeRoot,
		const FString& SearchDirectory,
		TArray<FAngelscriptManager::FFilenamePair>& OutFilenames)
	{
		TArray<FString> Files;
		FileManager.FindFiles(Files, *(SearchDirectory / TEXT("*.as")), true, false);
		for (const FString& File : Files)
		{
			OutFilenames.Add(FAngelscriptManager::FFilenamePair{ SearchDirectory / File, RelativeRoot / File });
		}

		TArray<FString> Directories;
		FileManager.FindFiles(Directories, *(SearchDirectory / TEXT("*")), false, true);
		for (const FString& Directory : TSet<FString>(Directories))
		{
			PrecheckCollectScriptFilesRecursive(FileManager, RelativeRoot / Directory, SearchDirectory / Directory, OutFilenames);
		}
	}

	/**
	 * Collects the patch .as files with script-relative paths: the patch dir
	 * maps onto the game Script root, so <PatchDir>/MixedCompileCacheActor.as
	 * has the script-relative path MixedCompileCacheActor.as and the module
	 * name MixedCompileCacheActor. Paths that escape the patch root are
	 * dropped (fail-closed; an empty result becomes reason=empty-patch).
	 */
	void CollectPatchFiles(const FString& PatchDir, TArray<FAngelscriptManager::FFilenamePair>& OutFilenames)
	{
		IFileManager& FileManager = IFileManager::Get();
		TArray<FAngelscriptManager::FFilenamePair> Raw;
		PrecheckCollectScriptFilesRecursive(FileManager, TEXT(""), PatchDir, Raw);

		for (const FAngelscriptManager::FFilenamePair& Pair : Raw)
		{
			FString Relative = Pair.RelativePath;
			Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
			OutFilenames.Add(FAngelscriptManager::FFilenamePair{ Pair.AbsolutePath, Relative });
		}
	}

	/**
	 * Known game module set: every module the game Script tree and FeatureMods
	 * tree can produce (same scan shape as the startup compile and the ModKit
	 * append). A patch module outside this set is unexpected-module; a known
	 * module that is missing from the active modules is a broken baseline.
	 */
	void CollectGameModuleNames(TSet<FString>& OutModuleNames)
	{
		IFileManager& FileManager = IFileManager::Get();
		TArray<FAngelscriptManager::FFilenamePair> GameFiles;

		const FString ScriptRoot = FPaths::ProjectDir() / TEXT("Script");
		PrecheckCollectScriptFilesRecursive(FileManager, TEXT(""), ScriptRoot, GameFiles);

		const TPair<FString, FString> Roots[] = {
			{ TEXT("FeatureMods"), FPaths::ProjectDir() / TEXT("FeatureMods") },
			{ TEXT("Plugins/FeatureMods"), FPaths::ProjectPluginsDir() / TEXT("FeatureMods") }
		};
		for (const TPair<FString, FString>& Root : Roots)
		{
			TArray<FString> ModDirectories;
			FileManager.FindFiles(ModDirectories, *(Root.Value / TEXT("*")), false, true);
			ModDirectories.Sort();
			for (const FString& ModDirectory : ModDirectories)
			{
				const FString ScriptDirectory = Root.Value / ModDirectory / TEXT("Script");
				if (FileManager.DirectoryExists(*ScriptDirectory))
				{
					PrecheckCollectScriptFilesRecursive(FileManager,
						Root.Key / ModDirectory / TEXT("Script"), ScriptDirectory, GameFiles);
				}
			}
		}

		for (const FAngelscriptManager::FFilenamePair& Pair : GameFiles)
		{
			OutModuleNames.Add(ToModuleName(Pair.RelativePath));
		}
	}

                                                                                  
	int32 EmitConfigError(const FString& Reason)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PRECHECK_CONFIG reason=%s"), *Reason);
		return 2;
	}

	int32 EmitVerdict(bool bPass, const FString& Reason, int32 Modules)
	{
		UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PRECHECK_VERDICT verdict=%s reason=%s modules=%d"),
			bPass ? TEXT("PASS") : TEXT("REJECT"), *Reason, Modules);
		return bPass ? 0 : 1;
	}

	/** Per-line detail (B4): reads the public Diagnostics map FormatDiagnostics uses. */
	void EmitViolations(const TArray<FAngelscriptManager::FFilenamePair>& PatchFiles)
	{
		FAngelscriptManager& Mgr = FAngelscriptManager::Get();
		TMap<FString, FString> ModuleByAbsolute;
		for (const FAngelscriptManager::FFilenamePair& Pair : PatchFiles)
		{
			ModuleByAbsolute.Add(Pair.AbsolutePath, ToModuleName(Pair.RelativePath));
		}

		for (const auto& Elem : Mgr.Diagnostics)
		{
			const FString& ModuleName = ModuleByAbsolute.FindRef(Elem.Key);
			if (ModuleName.IsEmpty())
			{
				continue;
			}
			for (const FAngelscriptManager::FDiagnostic& Diagnostic : Elem.Value.Diagnostics)
			{
				UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_PRECHECK_VIOLATION module=%s line=%d detail=%s"),
					*ModuleName, Diagnostic.Row, *Diagnostic.Message);
			}
		}
	}
}
#endif

int32 UModKitPatchPrecheckCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FAngelscriptManager& Mgr = FAngelscriptManager::Get();

	// --- CLI contract (B2/B3): one patch per process, complete file set -----
	FString PatchDir;
	FParse::Value(*Params, TEXT("PatchDir="), PatchDir);
	PatchDir.TrimQuotesInline();
	if (PatchDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*PatchDir))
	{
		return EmitConfigError(TEXT("missing-patch"));
	}
	PatchDir = FPaths::ConvertRelativePathToFull(PatchDir);

	FString ManifestPath;
	if (FParse::Value(*Params, TEXT("Manifest="), ManifestPath))
	{
		ManifestPath.TrimQuotesInline();
		FString ManifestText;
		if (!FFileHelper::LoadFileToString(ManifestText, *FPaths::ConvertRelativePathToFull(ManifestPath)))
		{
			return EmitConfigError(TEXT("config-error"));
		}
		TSharedPtr<FJsonObject> Manifest;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestText);
		if (!FJsonSerializer::Deserialize(Reader, Manifest) || !Manifest.IsValid())
		{
			return EmitConfigError(TEXT("config-error"));
		}
		                                                                 
		                                                      
		FString Version, Sha;
		if (!Manifest->TryGetStringField(TEXT("version"), Version) || !Manifest->TryGetStringField(TEXT("sha"), Sha))
			return EmitConfigError(TEXT("config-error"));
		UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PRECHECK_MANIFEST version=%s sha=%s"), *Version, *Sha);
	}

	TArray<FAngelscriptManager::FFilenamePair> PatchFiles;
	CollectPatchFiles(PatchDir, PatchFiles);

	if (PatchFiles.Num() == 0)
	{
		return EmitConfigError(TEXT("empty-patch"));
	}

	for (const FAngelscriptManager::FFilenamePair& Pair : PatchFiles)
	{
		if (FModKitCompilationManager::IsLeafPatchPath(Pair.RelativePath))
			return EmitVerdict(false, TEXT("leaf-hotpatch-owner-unavailable"), 0);
		if (Pair.RelativePath.IsEmpty() || Pair.RelativePath.StartsWith(TEXT("../"))
			|| ToModuleName(Pair.RelativePath).IsEmpty())
		{
			return EmitConfigError(TEXT("invalid-module-name"));
		}
	}

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PRECHECK_BEGIN patchDir=%s files=%d"), *PatchDir, PatchFiles.Num());

	// --- Baseline guard (B1): pass 1 is the process-start Initial compile; ---
	// pass 2 is the patch SoftReloadOnly compile below.
	if (!Mgr.bDidInitialCompileSucceed)
	{
		return EmitConfigError(TEXT("baseline-error"));
	}

	TSet<FString> GameModules;
	CollectGameModuleNames(GameModules);

	for (const FAngelscriptManager::FFilenamePair& Pair : PatchFiles)
	{
		const FString ModuleName = ToModuleName(Pair.RelativePath);
		if (!GameModules.Contains(ModuleName))
		{
			return EmitVerdict(false, TEXT("unexpected-module"), 0);
		}

		TSharedPtr<FAngelscriptModuleDesc> Active = Mgr.GetModule(ModuleName);
		if (!Active.IsValid() || FModKitCompilationManager::IsEmptyPlaceholderModule(ModuleName))
		{
			return EmitConfigError(TEXT("baseline-error"));
		}
	}

	// --- Patch compile: ResetDiagnostics (B4), then SoftReloadOnly (B1) ------
	Mgr.ResetDiagnostics();

	FAngelscriptPreprocessor Preprocessor;
	for (const FAngelscriptManager::FFilenamePair& Pair : PatchFiles)
	{
		Preprocessor.AddFile(Pair.RelativePath, Pair.AbsolutePath, false);
	}

	if (!Preprocessor.Preprocess())
	{
		EmitViolations(PatchFiles);
		return EmitVerdict(false, TEXT("compile-error"), 0);
	}

	TArray<TSharedRef<FAngelscriptModuleDesc>> Modules = Preprocessor.GetModulesToCompile();
	if (Modules.Num() == 0)
	{
		EmitViolations(PatchFiles);
		return EmitVerdict(false, TEXT("compile-error"), 0);
	}

	TArray<TSharedRef<FAngelscriptModuleDesc>> CompiledModules;
	FlushAsyncLoading();
	const ECompileResult Result = FModKitCompilationManager::CompileOwnedSourceModules(
		ECompileType::SoftReloadOnly, Modules, CompiledModules);
	if (Result == ECompileResult::Deferred)
	{
		UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PRECHECK_PENDING reason=execution-busy modules=0"));
		return 3;
	}

	FString Reason;
	bool bPass = false;
	switch (Result)
	{
	case ECompileResult::FullyHandled:        Reason = TEXT("soft"); bPass = true; break;
	case ECompileResult::PartiallyHandled:    Reason = TEXT("structural-suggested"); break;
	case ECompileResult::ErrorNeedFullReload: Reason = TEXT("structural-required"); break;
	case ECompileResult::Error:               Reason = TEXT("compile-error"); break;
	}

	EmitViolations(PatchFiles);
	return EmitVerdict(bPass, Reason, CompiledModules.Num());
#else
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PRECHECK_SKIPPED reason=non-editor-target"));
	return 0;
#endif
}
