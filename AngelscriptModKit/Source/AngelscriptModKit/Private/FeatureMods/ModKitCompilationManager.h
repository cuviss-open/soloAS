// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "AngelscriptManager.h"
#include "ModKitModRegistry.h"

struct FAngelscriptPreprocessor;

DECLARE_LOG_CATEGORY_EXTERN(LogAngelscriptModKit, Log, All);

/**
 * Outcome of a runtime compile/removal request . bBusy, bConflict
 * and bNoSource are op-level rejections that never touch the compile pipeline;
 * otherwise bSuccess mirrors the ECompileResult handling of the batch.
 */
struct FModKitCompileResult
{
	ECompileResult Result = ECompileResult::Error;
	TArray<FString> CompiledModuleNames;
	bool bSuccess = false;
	bool bBusy = false;
	bool bDeferred = false;
	bool bConflict = false;
	bool bNoSource = false;
	FString ConflictReason;
};

enum class EModKitSourceLayer : uint8 { Trunk, Branch, Leaf };

// Copied at source admission; never borrow mutable registry or package maps.
struct FModKitSourceOwner
{
	FString Id;
	FString ScriptRoot;
	FString Revision;
	FString VerifiedConfig;
	TArray<FString> LeafDependencies;
	EModKitSourceLayer Layer = EModKitSourceLayer::Trunk;
	EModKitModTrust Trust = EModKitModTrust::BuiltIn;
};

class FModKitCompilationManager
{
public:
	static void Startup();
	static ANGELSCRIPTMODKIT_API bool TryRevokeProviders();
	static bool HasProviders();
	static bool IsSourceAdmissionPending();
	static bool AdmitSource(FStringView AbsolutePath, FStringView RelativePath, bool bDeleted, FString& Error);
	static FString WrapLeafSource(const FString& AbsolutePath, const FString& Source, bool bWarn);
	static bool IsLeafPatchPath(const FString& RelativePath);
	static ANGELSCRIPTMODKIT_API bool GetSourceOwner(const TSharedRef<FAngelscriptModuleDesc>& Module, FModKitSourceOwner& Owner, FString& Error);
	static bool RequiresModAdmission(const FAngelscriptModuleDesc& Module);
	static bool GetGenerationOwner(const FAngelscriptModuleDesc& Module, FModKitSourceOwner& Owner, FString& Error);
	static ANGELSCRIPTMODKIT_API bool GetModuleOwner(const TSharedRef<FAngelscriptModuleDesc>& Module, FModKitSourceOwner& Owner, FString& Error);
	static ANGELSCRIPTMODKIT_API bool CanReferenceOwner(const FModKitSourceOwner& Source, const FModKitSourceOwner& Target);
	static ANGELSCRIPTMODKIT_API bool OwnsModule(const TSharedRef<FAngelscriptModuleDesc>& Module, const FString& Id);
	static bool ValidateModuleAdmission(TConstArrayView<TSharedRef<FAngelscriptModuleDesc>> Modules, FString& Error);
	static void Shutdown();

	/** The caller owns retries and its prepared bytes; Deferred never enters Runtime's source queue. */
	static ECompileResult CompileOwnedSourceModules(ECompileType Type,
		const TArray<TSharedRef<FAngelscriptModuleDesc>>& Modules,
		TArray<TSharedRef<FAngelscriptModuleDesc>>& Compiled);

	/**
	 * Normalized source-only prefixes: the built-in FeatureMods roots plus the
	 * configured UModKitSettings::SourceOnlyScriptPaths entries.
	 */
	static TArray<FString> GetSourceOnlyPrefixes();

	/**
	 * Cache-generation filter handler: true when any of the module's
	 * relative filenames falls under a source-only prefix.
	 */
	static bool IsSourceOnlyModule(const FString& ModuleName, const TArray<FString>& RelativeFilenames);

	/**
	 * Lists the module names of all source-only leaf modules found by the
	 * mod discovery scan and the configured source-only prefixes.
	 */
	static void GetLeafModuleNames(TArray<FString>& OutModuleNames);

	/**
	 * Classifies a module name as a source-only leaf; OutRoot (optional)
	 * receives the leaf root ("FeatureMods.<ModName>", "Plugins.FeatureMods.<ModName>", or the configured prefix).
	 */
	static bool IsLeafModuleName(const FString& ModuleName, FString* OutRoot = nullptr);

	/**
	 * Leaf-direction audit handler: true when the module violates the leaf
	 * direction rule (non-leaf depending on a leaf module, or a leaf module
	 * depending on a different-root leaf module). OutError receives the
	 * diagnostic message with the MODKIT-LEAF-DIRECTION marker.
	 */
	static bool CheckLeafDirection(const FString& ModuleName, const TArray<FString>& DependencyModuleNames, FString& OutError);

	/**
	 * Dual-event rendezvous for the startup append: runs the append at most
	 * once after the initial compile finished, engine init arrived, and the
	 * mod activation roster settled. Public so the mod registry can re-trigger
	 * it from the activation settle point.
	 */
	static void MaybeRunAppend();

	/**
 * Public re-entrant append entry : compiles the source-only
 * modules of the named mods, reusing the append chain (roster filter,
 * empty-placeholder duplicate predicate, FullReload swap-in). Rejected
 * while another runtime op or compile is in progress (reason=busy).
 * A module/class name collision with active content is reported through
 * bConflict (UX-D4) without starting a compile.
 */
	static FModKitCompileResult RequestCompileModSet(const TArray<FString>& ModNames,
		TSharedPtr<FAngelscriptPreprocessor>* PendingPreprocessor = nullptr);

	/** FullReload replacement of the complete source set of one active leaf owner. */
	static FModKitCompileResult RequestReplaceMod(const FString& ModName,
		TSharedPtr<FAngelscriptPreprocessor>* PendingPreprocessor = nullptr);

	/**
 * Runtime removal of a mod : every script file of the mod is
 * added with bTreatAsDeleted=true, so the class generator marks the old
 * classes as removed and a FullReload destroys the tracked instances and
 * deactivates the classes. The empty placeholder modules stay in
 * ActiveModules (C0+1 constant module-count semantics).
 */
	static FModKitCompileResult RequestModuleRemoval(const FString& ModName);
	/** End or cancel a removal request; Busy callers retain it only while retrying. */
	static void CancelModuleRemoval(const FString& ModName);
	static bool CanBeginRemoval(const FString& ModName, FString& Error);
	static bool HasCacheSet(const FString& ModName);

	/**
	 * True when every active module owned by the mod is an empty placeholder
	 * (no classes/enums/delegates/functions/globals). Stress assertions use
	 * this to prove a successful unload left only the empty shell behind.
	 */
	static bool HasEmptyPlaceholderModule(const FString& ModName);

	/**
	 * Empty-placeholder predicate for a single module: an active module with
	 * no classes/enums/delegates and no functions/globals on its asCModule is
	 * treated as absent by the duplicate predicate, so a reload may recompile
	 * it (v2 §2.5).
	 */
	static bool IsEmptyPlaceholderModule(const FString& ModuleName);

private:
	static void OnInitialCompileFinished();
	static void OnPostEngineInit();
	static void OnReloadHadErrors();
	static bool OnAppendRetryTick(float DeltaTime);

	static FDelegateHandle InitialCompileFinishedHandle;
	static FDelegateHandle PostEngineInitHandle;
	static FDelegateHandle ReloadHadErrorsHandle;
	static FTSTicker::FDelegateHandle AppendRetryHandle;
	static double AppendRetryDeadlineSeconds;
	static bool bIsAppendingModules;
	static bool bInitialCompileFinished;
	static bool bPostEngineInit;
	static bool bDidRunAppend;
	static bool bIsRuntimeOpInProgress;
};
