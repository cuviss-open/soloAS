// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

struct IConsoleCommand;
struct FAngelscriptPreprocessor;

DECLARE_MULTICAST_DELEGATE_OneParam(FModKitBeforeUnloadDelegate, const FString&);

/**
 * Runtime load/unload operations .
 *
 * Console commands ModKit.Load/ModKit.Unload and the public
 * RequestLoad/RequestUnload only enqueue (S-1 structural avoidance): a frame
 * ticker dequeues and executes the op at the next top-level game-thread tick,
 * where the call stack never passes through Angelscript dispatch and
 * ReaderDepth==0 is guaranteed structurally. Before any compile work an op is
 * gated on IsAngelscriptExecutionWriterHeldByCurrentThread() (reason=writer-held)
 * and on the operation lock / compile-in-progress (reason=busy).
 */
struct FModKitRuntimeOps
{
	static void Startup();
	static ANGELSCRIPTMODKIT_API void Shutdown();

	/** Public game-side entry points; they share the console-command enqueue path. */
	static ANGELSCRIPTMODKIT_API void RequestLoad(const FString& ModName);
	static ANGELSCRIPTMODKIT_API void RequestUnload(const FString& ModName);
	/** Replaces the complete source set of one active FeatureMod on the top-level tick. */
	static ANGELSCRIPTMODKIT_API void RequestReload(const FString& ModName);
	static ANGELSCRIPTMODKIT_API bool CanReload(const FString& ModName);
	/** Disables a timed-out mod now and removes its scripts on the next top-level tick. */
	static ANGELSCRIPTMODKIT_API FString GetOperationStatus(const FString& ModName);
	static ANGELSCRIPTMODKIT_API bool IsOperationPending(const FString& ModName);
	static void RequestTimeoutDisable(const FString& ModName);

	/** Game-side callbacks: pre-unload teardown hook and unload-refused notice. */
	static FModKitBeforeUnloadDelegate OnBeforeUnload;
	static FModKitBeforeUnloadDelegate OnUnloadRefused;

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FModKitCacheTimeoutCleanupCommand;
#endif
	struct FModKitPendingOp
	{
		FString ModName;
		bool bIsLoad = false;
		bool bIsReload = false;
		bool bIsTimeoutDisable = false;
		double RetryAfterSeconds = 0.0;
	};

	static void ExecLoad(const TArray<FString>& Args);
	static void ExecUnload(const TArray<FString>& Args);
	static void Enqueue(bool bIsLoad, const FString& ModName, bool bIsTimeoutDisable = false, bool bIsReload = false);
	static bool OnTick(float DeltaTime);
	static bool ExecuteOp(const FModKitPendingOp& Op);
	static void ExecuteLoad(const FString& ModName);
	static void ExecuteReload(const FString& ModName);
	static bool ExecuteUnload(const FString& ModName);
	static bool ExecuteTimeoutDisable(const FString& ModName);
	static void ContinueOp();
	static bool CompileLoadedMod(const FString& ModName);
	static bool RejectIfNotExecutable(const TCHAR* OpName, const FString& ModName);

	static TArray<FModKitPendingOp> PendingOps;
	static TMap<FString, FString> OperationStatus;
	static FString PendingUnloadMod;
	static FString PendingLoadMod;
	static TSharedPtr<FAngelscriptPreprocessor> PendingLoadPreprocessor;
	static double PendingCompileDeadlineSeconds;
	static bool bOpInProgress;
	static bool bReloadInProgress;
	static FTSTicker::FDelegateHandle TickerHandle;
	static IConsoleCommand* LoadCommand;
	static IConsoleCommand* UnloadCommand;
};
