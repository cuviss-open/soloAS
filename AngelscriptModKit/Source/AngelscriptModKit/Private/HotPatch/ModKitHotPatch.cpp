// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch orchestration + offline land commandlet .

#include "HotPatch/ModKitHotPatch.h"

#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptManager.h"
#include "HAL/IConsoleManager.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "HotPatch/ModKitHotPatchFetch.h"
#include "HotPatch/ModKitHotPatchLand.h"
#include "HotPatch/ModKitHotPatchTrigger.h"
#include "Core/ModKitSettings.h"

#if WITH_EDITOR
#include "Misc/Parse.h"
#endif

TMap<FString, EModKitPatchState> FModKitHotPatch::Ledger;
TArray<FString> FModKitHotPatch::PendingFetches;
TArray<FString> FModKitHotPatch::PendingApplies;
bool FModKitHotPatch::bOpInProgress = false;
FTSTicker::FDelegateHandle FModKitHotPatch::TickerHandle;
IConsoleCommand* FModKitHotPatch::FetchApplyCommand = nullptr;
IConsoleCommand* FModKitHotPatch::ApplyCommand = nullptr;

void FModKitHotPatch::Startup()
{
	// Handlers only enqueue (S-1): the op executes at the next top-level
	// game-thread tick, never inside an Angelscript dispatch stack.
	FetchApplyCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("ModKit.Patch.FetchApply"),
		TEXT("Fetches and stages a hot patch (frame-driven, top-level tick)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FModKitHotPatch::ExecFetchApply),
		ECVF_Default);

	// Explicit trigger (H3, MK-T704): apply a staged patch by id.
	ApplyCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("ModKit.Patch.Apply"),
		TEXT("Applies a staged hot patch (frame-driven, top-level tick)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FModKitHotPatch::ExecApply),
		ECVF_Default);
}

void FModKitHotPatch::Shutdown()
{
	if (FetchApplyCommand != nullptr)
	{
		IConsoleManager::Get().UnregisterConsoleObject(FetchApplyCommand);
		FetchApplyCommand = nullptr;
	}
	if (ApplyCommand != nullptr)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ApplyCommand);
		ApplyCommand = nullptr;
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	PendingFetches.Reset();
	PendingApplies.Reset();
	FModKitHotPatchTrigger::Shutdown();
	Ledger.Reset();
	bOpInProgress = false;
}

void FModKitHotPatch::RequestFetchApply(const FString& FeedUrl)
{
	check(IsInGameThread());
	Enqueue(FeedUrl.IsEmpty() ? UModKitSettings::Get()->PatchFeedUrl : FeedUrl);
}

void FModKitHotPatch::RequestApply(const FString& PatchId)
{
	check(IsInGameThread());
	EnqueueApply(PatchId);
}

void FModKitHotPatch::ExecFetchApply(const TArray<FString>& Args)
{
	RequestFetchApply(Args.IsEmpty() ? FString() : Args[0]);
}

void FModKitHotPatch::ExecApply(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		return;
	}
	RequestApply(Args[0]);
}

void FModKitHotPatch::Enqueue(const FString& FeedUrl)
{
	check(IsInGameThread());
	if (FeedUrl.IsEmpty())
	{
		return;
	}
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_ENQUEUED op=fetch-apply feed=%s"), *FeedUrl);
	PendingFetches.Add(FeedUrl);
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&FModKitHotPatch::OnTick), 0.0f);
	}
}

void FModKitHotPatch::EnqueueApply(const FString& PatchId)
{
	check(IsInGameThread());
	if (PatchId.IsEmpty())
	{
		return;
	}
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_ENQUEUED op=apply patch=%s"), *PatchId);
	PendingApplies.Add(PatchId);
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&FModKitHotPatch::OnTick), 0.0f);
	}
}

bool FModKitHotPatch::OnTick(float DeltaTime)
{
	check(IsInGameThread());
	(void)DeltaTime;
	if (!FAngelscriptManager::IsInitialized())
	{
		return true; // keep ticking until the runtime is up
	}
	if (bOpInProgress)
	{
		bool bDeferred = false;
		FModKitHotPatchTrigger::Apply(PendingApplies[0], bDeferred);
		bOpInProgress = bDeferred;
		if (!bDeferred) PendingApplies.RemoveAt(0);
	}
	while (!bOpInProgress && PendingFetches.Num() > 0)
	{
		const FString FeedUrl = PendingFetches[0];
		PendingFetches.RemoveAt(0);
		ExecuteFetchApply(FeedUrl);
	}
	while (!bOpInProgress && PendingApplies.Num() > 0)
	{
		const FString PatchId = PendingApplies[0];
		ExecuteApply(PatchId);
		if (!bOpInProgress) PendingApplies.RemoveAt(0);
	}
	const bool bHasWork = bOpInProgress || PendingFetches.Num() > 0 || PendingApplies.Num() > 0;
	if (!bHasWork)
	{
		TickerHandle.Reset();
	}
	return bHasWork;
}

void FModKitHotPatch::ExecuteFetchApply(const FString& FeedUrl)
{
	if (IsAngelscriptExecutionWriterHeldByCurrentThread())
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_REJECTED op=fetch-apply reason=writer-held"));
		return;
	}
	if (bOpInProgress || FAngelscriptManager::Get().bIsCompilingModules)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_REJECTED op=fetch-apply reason=busy"));
		return;
	}
	bOpInProgress = true;
	FetchAndLand(FeedUrl, UModKitSettings::Get()->PatchPublicKeyBase64);
	bOpInProgress = false;
}

void FModKitHotPatch::ExecuteApply(const FString& PatchId)
{
	if (IsAngelscriptExecutionWriterHeldByCurrentThread())
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_REJECTED op=apply patch=%s reason=writer-held"), *PatchId);
		return;
	}
	if (bOpInProgress || FAngelscriptManager::Get().bIsCompilingModules)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_REJECTED op=apply patch=%s reason=busy"), *PatchId);
		return;
	}
	bOpInProgress = true;
	bool bDeferred = false;
	FModKitHotPatchTrigger::Apply(PatchId, bDeferred);
	bOpInProgress = bDeferred;
}

bool FModKitHotPatch::FetchAndLand(const FString& FeedUrl, const FString& PublicKeyBase64)
{
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_LAND_BEGIN feed=%s"), *FeedUrl);
	const UModKitSettings* Settings = UModKitSettings::Get();

	FModKitFetchedPatch Patch;
	FString Reason;
	FString Detail;
	if (!FModKitHotPatchFetch::Fetch(FeedUrl, Settings->PatchMaxBytes, Settings->PatchFetchTimeoutSec,
		Patch, Reason, Detail))
	{
		if (Detail.IsEmpty())
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_FETCH_FAILED reason=%s"), *Reason);
		}
		else
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_FETCH_FAILED reason=%s detail=%s"), *Reason, *Detail);
		}
		return false;
	}

	FString PatchId;
	int32 FileCount = 0;
	if (!FModKitHotPatchLand::Land(Patch, PublicKeyBase64, PatchId, FileCount, Reason, Detail))
	{
		if (Detail.IsEmpty())
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_LAND_FAILED reason=%s"), *Reason);
		}
		else
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_PATCH_LAND_FAILED reason=%s detail=%s"), *Reason, *Detail);
		}
		return false;
	}

	Ledger.Add(PatchId, EModKitPatchState::Staged);
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_STAGED patch=%s files=%d dir=%s"),
		*PatchId, FileCount, *FModKitHotPatchLand::ResolveStagingRoot());
	return true;
}

int32 UModKitPatchLandCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	FString FeedUrl;
	FParse::Value(*Params, TEXT("Feed="), FeedUrl);
	FeedUrl.TrimQuotesInline();
	FString PublicKeyBase64;
	if (!FParse::Value(*Params, TEXT("PublicKey="), PublicKeyBase64))
	{
		PublicKeyBase64 = UModKitSettings::Get()->PatchPublicKeyBase64;
	}
	PublicKeyBase64.TrimQuotesInline();
	return FModKitHotPatch::FetchAndLand(FeedUrl, PublicKeyBase64) ? 0 : 1;
#else
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_PATCH_LAND_SKIPPED reason=non-editor-target"));
	return 0;
#endif
}
