// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
                                                                        
// MK-T703) for the AngelscriptModKit module. Zero new Runtime/Bridge
// registration points: the runtime entry point is an S-1 enqueue/ticker pump
// isomorphic to FModKitRuntimeOps, and the offline tool is a commandlet.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Containers/Ticker.h"
#include "ModKitHotPatch.generated.h"

struct IConsoleCommand;

/** Session ledger state of a hot patch (staged by T703; applied/applied-partial/failed
 * transitions arrive with the T704 trigger). */
enum class EModKitPatchState : uint8
{
	Staged,
	Pending,
	Applied,
	AppliedPartial,
	Failed,
};

/**
 * Hot-patch orchestration .
 *
 * ModKit.Patch.FetchApply and RequestFetchApply only enqueue (S-1 structural
 * avoidance, isomorphic to FModKitRuntimeOps): a frame ticker dequeues and
 * executes the fetch+land at the next top-level game-thread tick, gated on
 * IsAngelscriptExecutionWriterHeldByCurrentThread() (reason=writer-held), the
 * op lock / compile-in-progress (reason=busy), and UModKitSettings
 * bEnableHotPatch (reason=disabled). Landing itself re-verifies the full
 * MK-T702 chain and is atomic (ModKitHotPatchLand).
 */
struct FModKitHotPatch
{
	static void Startup();
	static void Shutdown();

	/** Game-side entry point; an empty feed uses the startup-frozen PatchFeedUrl. */
	static void RequestFetchApply(const FString& FeedUrl);

	/**
	 * Explicit trigger entry point (H3, MK-T704): ModKit.Patch.Apply <PatchId>
	 * enqueues; the ticker executes the apply at the next top-level tick with
	 * the writer-held/busy gates, then FModKitHotPatchTrigger::Apply runs the
	 * re-verify + compile + verdict mapping.
	 */
	static void RequestApply(const FString& PatchId);

	/** Session ledger, PatchId -> state. */
	static TMap<FString, EModKitPatchState> Ledger;

	/**
	 * Synchronous fetch + verify + land core shared by the ticker op and the
	 * offline commandlet. Emits MODKIT_PATCH_LAND_BEGIN and exactly one of
	 * MODKIT_PATCH_FETCH_FAILED / MODKIT_PATCH_LAND_FAILED /
	 * MODKIT_PATCH_STAGED. Returns true on staged.
	 */
	static bool FetchAndLand(const FString& FeedUrl, const FString& PublicKeyBase64);

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FModKitHttpStubTest;
#endif
	static void ExecFetchApply(const TArray<FString>& Args);
	static void ExecApply(const TArray<FString>& Args);
	static void Enqueue(const FString& FeedUrl);
	static void EnqueueApply(const FString& PatchId);
	static bool OnTick(float DeltaTime);
	static void ExecuteFetchApply(const FString& FeedUrl);
	static void ExecuteApply(const FString& PatchId);

	static TArray<FString> PendingFetches;
	static TArray<FString> PendingApplies;
	static bool bOpInProgress;
	static FTSTicker::FDelegateHandle TickerHandle;
	static IConsoleCommand* FetchApplyCommand;
	static IConsoleCommand* ApplyCommand;
};

/**
 * Offline patch fetch + land tool (-run=ModKitPatchLand -Feed=<url|dir>
 * [-PublicKey=<base64>]).
 *
 * Fetches the feed fully into memory, re-runs the MK-T702 verification chain
 * (schema/key/Ed25519/per-file SHA-256/precheck PASS/engine match) and lands
 * the patch atomically under the staging root (default
 * <ProjectSaved>/ModKitPatches). Every failure emits
 * MODKIT_PATCH_FETCH_FAILED reason=<config|not-found|size|http|timeout> or
 * MODKIT_PATCH_LAND_FAILED reason=<disabled|schema|key-missing|bad-signature|
 * sha-mismatch|precheck-required|engine-mismatch|io> and exits 1; success
 * emits MODKIT_PATCH_STAGED patch=<id> files=N and exits 0. 
 * (modkit-g10-land.ps1) drives one editor-cmd process per fixture lane.
 */
UCLASS()
class UModKitPatchLandCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
