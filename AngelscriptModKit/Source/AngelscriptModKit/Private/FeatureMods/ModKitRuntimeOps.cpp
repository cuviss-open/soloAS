// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "FeatureMods/ModKitRuntimeOps.h"

#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptManager.h"
#include "AngelscriptReloadSafety.h"
#include "Engine/Engine.h"
#include "GameFeaturesSubsystem.h"
// SOLOAS-COMPAT-BEGIN(5.7) UE 5.7 only forward-declares GameFeatures results [test: 5.7 Editor compile] [id: CS.Compatibility.ModKit.ModKitRuntimeOps.ResultDefinition]
#include "GameFeaturePluginOperationResult.h"
// SOLOAS-COMPAT-END
#include "UObject/UObjectGlobals.h"
#include "HAL/IConsoleManager.h"
#include "FeatureMods/ModKitAssetReplacements.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "FeatureMods/ModKitModRegistry.h"
#include "Core/ModKitSettings.h"
#include "FeatureMods/ModKitUnloadGuard.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "HAL/PlatformTime.h"

TArray<FModKitRuntimeOps::FModKitPendingOp> FModKitRuntimeOps::PendingOps;
FString FModKitRuntimeOps::PendingLoadMod;
FString FModKitRuntimeOps::PendingUnloadMod;
TMap<FString, FString> FModKitRuntimeOps::OperationStatus;
TSharedPtr<FAngelscriptPreprocessor> FModKitRuntimeOps::PendingLoadPreprocessor;
double FModKitRuntimeOps::PendingCompileDeadlineSeconds = 0.0;
bool FModKitRuntimeOps::bOpInProgress = false;
bool FModKitRuntimeOps::bReloadInProgress = false;
FTSTicker::FDelegateHandle FModKitRuntimeOps::TickerHandle;
IConsoleCommand* FModKitRuntimeOps::LoadCommand = nullptr;
IConsoleCommand* FModKitRuntimeOps::UnloadCommand = nullptr;
FModKitBeforeUnloadDelegate FModKitRuntimeOps::OnBeforeUnload;
FModKitBeforeUnloadDelegate FModKitRuntimeOps::OnUnloadRefused;
namespace { TSet<FString> TimeoutTeardownStarted; }

void FModKitRuntimeOps::Startup()
{
	// Handlers only enqueue (S-1): an op executes at the next top-level
	// game-thread tick, never inside an Angelscript dispatch stack.
	LoadCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("ModKit.Load"),
		TEXT("Loads a mod at runtime (frame-driven, top-level tick)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FModKitRuntimeOps::ExecLoad),
		ECVF_Default);

	UnloadCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("ModKit.Unload"),
		TEXT("Unloads a mod at runtime (frame-driven, top-level tick)."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&FModKitRuntimeOps::ExecUnload),
		ECVF_Default);
}

void FModKitRuntimeOps::Shutdown()
{
	FModKitAssetReplacements::Shutdown();
	if (LoadCommand != nullptr)
	{
		IConsoleManager::Get().UnregisterConsoleObject(LoadCommand);
		LoadCommand = nullptr;
	}
	if (UnloadCommand != nullptr)
	{
		IConsoleManager::Get().UnregisterConsoleObject(UnloadCommand);
		UnloadCommand = nullptr;
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	for (const auto& Op : PendingOps) FModKitCompilationManager::CancelModuleRemoval(Op.ModName);
	FModKitCompilationManager::CancelModuleRemoval(PendingUnloadMod);
	PendingOps.Reset();
	TimeoutTeardownStarted.Reset();
	OperationStatus.Reset();
	PendingUnloadMod.Reset();
	PendingLoadMod.Reset();
	PendingLoadPreprocessor.Reset();
	PendingCompileDeadlineSeconds = 0.0;
	bReloadInProgress = false;
	bOpInProgress = false;
}

FString FModKitRuntimeOps::GetOperationStatus(const FString& ModName)
{
	return OperationStatus.FindRef(ModName.ToLower());
}

bool FModKitRuntimeOps::IsOperationPending(const FString& ModName)
{
	return PendingLoadMod.Equals(ModName, ESearchCase::IgnoreCase)
		|| PendingUnloadMod.Equals(ModName, ESearchCase::IgnoreCase)
		|| PendingOps.ContainsByPredicate([&ModName](const FModKitPendingOp& Op)
		{
			return Op.ModName.Equals(ModName, ESearchCase::IgnoreCase);
		});
}

void FModKitRuntimeOps::RequestLoad(const FString& ModName)
{
	check(IsInGameThread());
	Enqueue(true, ModName);
}

bool FModKitRuntimeOps::CanReload(const FString& ModName)
{
	return FAngelscriptManager::IsInitialized() && !bOpInProgress
		&& !FAngelscriptManager::Get().bIsCompilingModules && FModKitModRegistry::IsModActivated(ModName);
}

void FModKitRuntimeOps::RequestReload(const FString& ModName)
{
	check(IsInGameThread());
	Enqueue(false, ModName, false, true);
}

void FModKitRuntimeOps::RequestUnload(const FString& ModName)
{
	check(IsInGameThread());
	Enqueue(false, ModName);
}

void FModKitRuntimeOps::RequestTimeoutDisable(const FString& ModName)
{
	check(IsInGameThread());
	FModKitModRegistry::MarkModFailed(ModName, TEXT("script-timeout"));
	Enqueue(false, ModName, true);
}

void FModKitRuntimeOps::ExecLoad(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		return;
	}
	RequestLoad(Args[0]);
}

void FModKitRuntimeOps::ExecUnload(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		return;
	}
	RequestUnload(Args[0]);
}

void FModKitRuntimeOps::Enqueue(bool bIsLoad, const FString& ModName, bool bIsTimeoutDisable, bool bIsReload)
{
	check(IsInGameThread());
	if (ModName.IsEmpty())
	{
		return;
	}

	FModKitPendingOp Op;
	Op.ModName = ModName;
	Op.bIsLoad = bIsLoad;
	Op.bIsReload = bIsReload;
	Op.bIsTimeoutDisable = bIsTimeoutDisable;

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_RUNTIME_OP_ENQUEUED op=%s name=%s"),
		bIsTimeoutDisable ? TEXT("timeout-disable") : (bIsReload ? TEXT("reload") : (bIsLoad ? TEXT("load") : TEXT("unload"))), *ModName);

	OperationStatus.Add(ModName.ToLower(), TEXT("Queued"));
	PendingOps.Add(MoveTemp(Op));

	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateStatic(&FModKitRuntimeOps::OnTick), 0.0f);
	}
}

bool FModKitRuntimeOps::OnTick(float DeltaTime)
{
	check(IsInGameThread());
	(void)DeltaTime;

	if (!FAngelscriptManager::IsInitialized())
	{
		return true; // keep ticking until the runtime is up
	}

	if (bOpInProgress)
	{
		ContinueOp();
	}

	if (!bOpInProgress && PendingOps.Num() > 0)
	{
		// A held timeout cleanup yields to other owners. Same-owner commands
		// retain FIFO order, and the safety scan runs at most every 250 ms.
		TSet<FString> WaitingOwners;
		for (int32 Index = 0; Index < PendingOps.Num();)
		{
			const FModKitPendingOp Op = PendingOps[Index];
			const FString Owner = Op.ModName.ToLower();
			if (WaitingOwners.Contains(Owner) || Op.RetryAfterSeconds > FPlatformTime::Seconds())
			{
				WaitingOwners.Add(Owner);
				++Index;
				continue;
			}
			if (!ExecuteOp(Op))
			{
				if (!Op.bIsTimeoutDisable) break;
				PendingOps[Index].RetryAfterSeconds = FPlatformTime::Seconds() + 0.25;
				OperationStatus.Add(Owner, TEXT("Disabled; cleanup waiting for active consumers."));
				WaitingOwners.Add(Owner);
				++Index;
				continue;
			}
			FModKitCompilationManager::CancelModuleRemoval(Op.ModName);
			TimeoutTeardownStarted.Remove(Op.ModName);
			PendingOps.RemoveAt(Index);
			if (bOpInProgress) break;
		}
	}

	const bool bHasWork = bOpInProgress || PendingOps.Num() > 0;
	if (!bHasWork)
	{
		TickerHandle.Reset();
	}
	return bHasWork;
}

bool FModKitRuntimeOps::ExecuteOp(const FModKitPendingOp& Op)
{
	OperationStatus.Add(Op.ModName.ToLower(), TEXT("Operation failed; see Output Log (AngelscriptModKit) for the validation reason."));
	if (Op.bIsTimeoutDisable)
	{
		return ExecuteTimeoutDisable(Op.ModName);
	}
	else if (Op.bIsReload)
	{
		ExecuteReload(Op.ModName);
	}
	else if (Op.bIsLoad)
	{
		ExecuteLoad(Op.ModName);
	}
	else
	{
		return ExecuteUnload(Op.ModName);
	}
	return true;
}

bool FModKitRuntimeOps::RejectIfNotExecutable(const TCHAR* OpName, const FString& ModName)
{
	if (IsAngelscriptExecutionWriterHeldByCurrentThread())
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=%s name=%s reason=writer-held"), OpName, *ModName);
		return true;
	}
	if (bOpInProgress || FAngelscriptManager::Get().bIsCompilingModules)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=%s name=%s reason=busy"), OpName, *ModName);
		return true;
	}
	return false;
}

void FModKitRuntimeOps::ExecuteLoad(const FString& ModName)
{
	if (RejectIfNotExecutable(TEXT("load"), ModName))
	{
		return;
	}

	bOpInProgress = true;
	PendingLoadMod.Reset();
	PendingLoadPreprocessor.Reset();
	PendingCompileDeadlineSeconds = 0.0;

	// Expected, not a defect: a startup-roster or already-loaded mod is never
	// reloaded in place (v2 §1.1).
	if (FModKitModRegistry::IsModActivated(ModName))
	{
		UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_RUNTIME_OP_REJECTED op=load name=%s reason=already-loaded"), *ModName);
		bOpInProgress = false;
		return;
	}

	if (FModKitModRegistry::IsTimeoutDisabled(ModName))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=load name=%s reason=timeout-disabled"), *ModName);
		bOpInProgress = false;
		return;
	}

	if (!FModKitModRegistry::RescanOrAdd(ModName))
	{
		if (FModKitModRegistry::IsPlayerModApprovalPending(ModName))
		{
			OperationStatus.Add(ModName.ToLower(), TEXT("Awaiting player approval"));
			UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_RUNTIME_LOAD_PENDING name=%s reason=player-approval-required"), *ModName);
		}
		else
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=load name=%s reason=unknown-mod"), *ModName);
		}
		bOpInProgress = false;
		return;
	}

	if (FModKitModRegistry::IsUPluginMod(ModName))
	{
		if (!FModKitModRegistry::StartRuntimeActivation(ModName))
		{
			// The registry already emitted MODKIT_MOD_DISABLED (UX-D2).
			bOpInProgress = false;
			return;
		}
		// Async: the op continues on a later tick once activation settles.
		PendingLoadMod = ModName;
		OperationStatus.Add(ModName.ToLower(), TEXT("Loading"));
		return;
	}

	// Legacy mod (no uplugin): RescanOrAdd already marked it Activated.
	PendingLoadMod = ModName;
	OperationStatus.Add(ModName.ToLower(), TEXT("Loading"));
	ContinueOp();
}

void FModKitRuntimeOps::ExecuteReload(const FString& ModName)
{
	if (RejectIfNotExecutable(TEXT("reload"), ModName)) return;
	FString Reason;
	if (!FModKitModRegistry::PrepareReload(ModName, Reason))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=reload name=%s reason=%s"), *ModName, *Reason);
		return;
	}
	bOpInProgress = bReloadInProgress = true;
	PendingLoadMod = ModName;
	PendingLoadPreprocessor.Reset();
	PendingCompileDeadlineSeconds = 0.0;
	ContinueOp();
}

void FModKitRuntimeOps::ContinueOp()
{
	if (!PendingUnloadMod.IsEmpty()) return;
	if (PendingLoadMod.IsEmpty())
	{
		bOpInProgress = false;
		return;
	}

	const FString ModName = PendingLoadMod;
	if (!FModKitModRegistry::IsModTerminal(ModName))
	{
		return; // GFP activation still in flight
	}

	if (PendingCompileDeadlineSeconds > 0.0 && FPlatformTime::Seconds() >= PendingCompileDeadlineSeconds)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_LOAD_FAILED name=%s reason=package-loading-timeout"), *ModName);
		if (bReloadInProgress) FModKitModRegistry::CompleteReload(ModName, false, false);
		else FModKitModRegistry::MarkModFailed(ModName, TEXT("package-loading-timeout"));
	}
	else if (FModKitModRegistry::IsModActivated(ModName) && !CompileLoadedMod(ModName))
	{
		return;
	}
	PendingLoadMod.Reset();
	PendingLoadPreprocessor.Reset();
	PendingCompileDeadlineSeconds = 0.0;
	bReloadInProgress = false;
	if (GetOperationStatus(ModName) == TEXT("Loading"))
		OperationStatus.Add(ModName.ToLower(), TEXT("Load failed; see Output Log (AngelscriptModKit) for the validation reason."));
	// Activation and compile failures have already emitted their disable reason.
	bOpInProgress = false;
}

bool FModKitRuntimeOps::CompileLoadedMod(const FString& ModName)
{
	FModKitCompileResult Result = bReloadInProgress
		? FModKitCompilationManager::RequestReplaceMod(ModName, &PendingLoadPreprocessor)
		: FModKitCompilationManager::RequestCompileModSet({ ModName }, &PendingLoadPreprocessor);
	if (Result.bDeferred)
	{
		if (PendingCompileDeadlineSeconds == 0.0)
		{
			PendingCompileDeadlineSeconds = FPlatformTime::Seconds()
				+ UModKitSettings::Get()->ModStartupActivationTimeoutSec;
			UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_RUNTIME_LOAD_DEFERRED name=%s reason=package-loading"), *ModName);
		}
		return false;
	}
	if (Result.bBusy && PendingLoadPreprocessor.IsValid())
	{
		return false;
	}
	if (bReloadInProgress)
	{
		FModKitModRegistry::CompleteReload(ModName, Result.bSuccess, Result.Result == ECompileResult::NativeReloadFailed);
		OperationStatus.Add(ModName.ToLower(), Result.bSuccess ? TEXT("Reloaded") : TEXT("Reload failed; see Output Log (AngelscriptModKit)."));
		UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_RUNTIME_RELOAD_RESULT name=%s success=%d nativeFailed=%d modules=%d reason=%s"),
			*ModName, Result.bSuccess, Result.Result == ECompileResult::NativeReloadFailed,
			Result.CompiledModuleNames.Num(), *Result.ConflictReason);
		return true;
	}
	if (Result.bBusy)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=load name=%s reason=busy"), *ModName);
		return true;
	}
	if (Result.bNoSource)
	{
		// A mod with no .as sources is not loadable at runtime (cache-shaped
		// mods never participate in runtime load, three explicit constraints).
		UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_RUNTIME_OP_REJECTED op=load name=%s reason=no-source-script"), *ModName);
		FModKitModRegistry::MarkModFailed(ModName, TEXT("no-source-script"));
		return true;
	}
	if (Result.bConflict)
	{
		// UX-D4: collision with an activated mod or the game module surface.
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=load name=%s reason=conflict %s"), *ModName, *Result.ConflictReason);
		FModKitModRegistry::MarkModFailed(ModName, TEXT("module-or-class-conflict"));
		return true;
	}
	if (Result.bSuccess)
	{
		OperationStatus.Add(ModName.ToLower(), TEXT("Loaded"));
		UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_RUNTIME_LOADED name=%s modules=%d"), *ModName, Result.CompiledModuleNames.Num());
		return true;
	}

	// UX-D2: a failed load disables the mod while the game keeps running.
	UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_LOAD_FAILED name=%s"), *ModName);
	FModKitModRegistry::MarkModFailed(ModName, TEXT("compile-failed"));
	return true;
}

bool FModKitRuntimeOps::ExecuteUnload(const FString& ModName)
{
	if (RejectIfNotExecutable(TEXT("unload"), ModName))
	{
		return true;
	}

	bOpInProgress = true;

	if (!FModKitModRegistry::IsModActivated(ModName))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=unload name=%s reason=not-loaded"), *ModName);
		bOpInProgress = false;
		return true;
	}

	if (UModKitSettings::Get()->bForceDestroyInstancesOnUnload)
	{
		// v1 reserve only (v2 §2.3): the destructive force-destroy path is not
		// implemented; fail closed instead of pretending to destroy.
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR reason=force-destroy-not-implemented"));
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=unload name=%s reason=force-destroy-not-implemented"), *ModName);
		bOpInProgress = false;
		return true;
	}

	// Live-instance guard: refuse to unload while script instances survive.
	int32 LiveCount = 0;
	FString LiveClassName;
	if (FModKitUnloadGuard::HasLiveInstances(ModName, LiveCount, LiveClassName))
	{
		if (LiveCount == -2)
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=unload name=%s reason=owner-unknown module=%s"), *ModName, *LiveClassName);
			OperationStatus.Add(ModName.ToLower(), FString::Printf(TEXT("Unload refused: module ownership is unknown for %s."), *LiveClassName));
		}
		else if (LiveCount < 0)
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=unload name=%s reason=struct-lifetime-unknown struct=%s; restart or fully reload the project"),
				*ModName, *LiveClassName);
			OperationStatus.Add(ModName.ToLower(), FString::Printf(TEXT("Unload refused: live value count for script struct %s is unknown. Restart or fully reload the project."), *LiveClassName));
		}
		else
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=unload name=%s reason=live-instances count=%d class=%s"),
				*ModName, LiveCount, *LiveClassName);
			OperationStatus.Add(ModName.ToLower(), FString::Printf(TEXT("Unload refused: %d live instances of %s. Release them before unloading."), LiveCount, *LiveClassName));
		}
		OnUnloadRefused.Broadcast(ModName);
		bOpInProgress = false;
		return true;
	}

	// Retain the queued op before owner teardown when source compilation cannot start.
	FAngelscriptExecutionWriteScope Admission(FAngelscriptExecutionWriteScope::EAcquire::Try);
	if (!Admission.IsAcquired() || !FAngelscriptManager::Get().DeferredSourceCompilations.IsEmpty()
		|| AngelscriptReloadSafety::ShouldDeferForPackageLoading())
	{
		bOpInProgress = false;
		return false;
	}
	FString RemovalError;
	if (!FModKitCompilationManager::CanBeginRemoval(ModName, RemovalError))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=unload name=%s reason=%s"), *ModName, *RemovalError);
		OperationStatus.Add(ModName.ToLower(), TEXT("Unload refused: ") + RemovalError);
		bOpInProgress = false;
		return true;
	}

	// Cache removal can reject or be busy; do not tear down the owner first.
	FModKitCompileResult Result;
	const bool bCacheSet = FModKitCompilationManager::HasCacheSet(ModName);
	if (bCacheSet)
	{
		Result = FModKitCompilationManager::RequestModuleRemoval(ModName);
		if (Result.bBusy || Result.bDeferred)
		{
			bOpInProgress = false;
			return false;
		}
		if (!Result.bSuccess)
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=unload name=%s reason=cache-removal-failed"), *ModName);
			bOpInProgress = false;
			return true;
		}
	}
	// Game-side pre-unload hook for static/ownership teardown (v2 §2.4).
	OnBeforeUnload.Broadcast(ModName);

	// GFP mods deactivate before the removal compile (after the guard, before
	// the compile, v2 §2.4).
	if (FModKitModRegistry::IsUPluginMod(ModName))
	{
		FModKitModRegistry::DeactivateMod(ModName);
	}

	if (!bCacheSet) Result = FModKitCompilationManager::RequestModuleRemoval(ModName);
	if (Result.bDeferred)
	{
		bOpInProgress = false;
		return false;
	}
	if (Result.bBusy)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_OP_REJECTED op=unload name=%s reason=busy"), *ModName);
		bOpInProgress = false;
		return true;
	}
	// bNoSource: the mod has no script modules to remove (data-only GFP mod);
	// deactivation already happened, so the unload is a no-op success.
	if (Result.bSuccess || Result.bNoSource)
	{
		if (FModKitModRegistry::IsUPluginMod(ModName))
		{
			auto* Features = GEngine ? GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() : nullptr;
			FString URL;
			if (!Features || !Features->GetPluginURLByName(ModName, URL))
			{
				OperationStatus.Add(ModName.ToLower(), TEXT("Unload failed: GameFeatures plugin URL unavailable."));
				bOpInProgress = false;
				return true;
			}
			PendingUnloadMod = ModName;
			OperationStatus.Add(ModName.ToLower(), TEXT("Unregistering and unmounting FeatureMod"));
			Features->UnloadGameFeaturePlugin(URL, FGameFeaturePluginUnloadComplete::CreateLambda(
				[ModName](const UE::GameFeatures::FResult& UnloadResult)
				{
					if (UnloadResult.HasValue())
					{
						FModKitModRegistry::MarkModUnloaded(ModName);
						OperationStatus.Add(ModName.ToLower(), TEXT("Unloaded"));
						if (GEngine) GEngine->ForceGarbageCollection(true);
						UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_RUNTIME_UNLOADED name=%s"), *ModName);
					}
					else OperationStatus.Add(ModName.ToLower(), TEXT("Unload failed: ") + UnloadResult.GetError());
					PendingUnloadMod.Reset();
					bOpInProgress = false;
				}), false);
			return true;
		}
		FModKitModRegistry::MarkModUnloaded(ModName);
		OperationStatus.Add(ModName.ToLower(), TEXT("Unloaded"));
		UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_RUNTIME_UNLOADED name=%s"), *ModName);

		// FullReload already ran one GC pass; force a second convergence
		// (same pattern as the runtime removal path in AngelscriptManager).
		if (GEngine != nullptr)
		{
			GEngine->ForceGarbageCollection(true);
		}
	}
	else
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_RUNTIME_UNLOAD_FAILED name=%s"), *ModName);
	}
	bOpInProgress = false;
	return true;
}

bool FModKitRuntimeOps::ExecuteTimeoutDisable(const FString& ModName)
{
	if (RejectIfNotExecutable(TEXT("timeout-disable"), ModName))
	{
		return true;
	}
	if (!FModKitModRegistry::IsTimeoutDisabled(ModName))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_TIMEOUT_CLEANUP name=%s result=failed reason=not-timeout-disabled"), *ModName);
		return true;
	}
	FString UnknownModule;
	if (FModKitUnloadGuard::HasUnknownOwnership(UnknownModule))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_TIMEOUT_CLEANUP name=%s result=failed reason=owner-unknown module=%s"), *ModName, *UnknownModule);
		return true;
	}

	FAngelscriptExecutionWriteScope Admission(FAngelscriptExecutionWriteScope::EAcquire::Try);
	if (!Admission.IsAcquired() || !FAngelscriptManager::Get().DeferredSourceCompilations.IsEmpty()
		|| AngelscriptReloadSafety::ShouldDeferForPackageLoading()) return false;
	FString RemovalError;
	if (!FModKitCompilationManager::CanBeginRemoval(ModName, RemovalError))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_TIMEOUT_CLEANUP name=%s result=failed reason=%s"), *ModName, *RemovalError);
		return true;
	}
	bOpInProgress = true;
	if (!TimeoutTeardownStarted.Contains(ModName))
	{
		TimeoutTeardownStarted.Add(ModName);
		OnBeforeUnload.Broadcast(ModName);
		if (FModKitModRegistry::IsUPluginMod(ModName)) FModKitModRegistry::DeactivateMod(ModName);
		FModKitUnloadGuard::DestroyLiveInstances(ModName);
		CollectGarbage(RF_NoFlags);
	}

	const FModKitCompileResult Result = FModKitCompilationManager::RequestModuleRemoval(ModName);
	if (Result.bBusy || Result.bDeferred)
	{
		bOpInProgress = false;
		return false;
	}
	if (Result.bSuccess || Result.bNoSource)
	{
		OperationStatus.Add(ModName.ToLower(), TEXT("Disabled; cleanup complete."));
		UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_TIMEOUT_CLEANUP name=%s result=complete modules=%d"),
			*ModName, Result.CompiledModuleNames.Num());
	}
	else
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_TIMEOUT_CLEANUP name=%s result=failed busy=%d"), *ModName, Result.bBusy);
	}
	bOpInProgress = false;
	return true;
}
