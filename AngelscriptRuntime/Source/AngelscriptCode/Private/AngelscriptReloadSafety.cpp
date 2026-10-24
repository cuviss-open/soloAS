// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptReloadSafety.h"

#include "AngelscriptInclude.h"
#include "StartAngelscriptHeaders.h"
#include "as_module.h"
#include "EndAngelscriptHeaders.h"
#include "AngelscriptManager.h"
#include "CoreGlobals.h"
#include "Misc/OutputDevice.h"
#include "Misc/ScopeExit.h"
#include "UObject/UObjectGlobals.h"

#include <cstdlib>
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <intrin.h>
#endif

namespace
{
thread_local int32 CommittedReloadDepth = 0;
thread_local AngelscriptReloadSafety::FPreparedPayloadReloadScope* PreparedPayloadScope = nullptr;
uint64 NextPayloadReloadEpoch = 1;
thread_local const TArray<AngelscriptReloadSafety::FPreparedScriptEnumReloadPlan>* PendingScriptEnumPlans = nullptr;
thread_local uint64 PendingScriptEnumEpoch = 0;
struct FScriptEnumWitnessProviderState
{
    AngelscriptReloadSafety::FPreparedPayloadReloadScope* Scope = nullptr;
    AngelscriptReloadSafety::IPreparedPayloadReloadLease* Lease = nullptr;
    const void* Owner = nullptr;
    uint64 Epoch = 0;
    FDelegateHandle Reader;
    FDelegateHandle Transition;
    bool bSealAttempted = false;
    bool bSealed = false;
    bool bQueryActive = false;
};
thread_local FScriptEnumWitnessProviderState ScriptEnumWitnessProvider;

bool MatchesScriptEnumWitnessProvider(const FScriptEnumWitnessProviderState& Expected)
{
    using namespace AngelscriptReloadSafety;
    const auto& Current = ScriptEnumWitnessProvider;
    if (!Expected.bSealed || !PreparedPayloadScope || PreparedPayloadScope != Expected.Scope
        || PreparedPayloadScope->Epoch != Expected.Epoch || PreparedPayloadScope->Lease.Get() != Expected.Lease
        || !Current.bSealed || Current.Scope != Expected.Scope || Current.Epoch != Expected.Epoch
        || Current.Lease != Expected.Lease || Current.Owner != Expected.Owner
        || Current.Reader != Expected.Reader || Current.Transition != Expected.Transition) return false;
    const auto& Reader = GetPreparedScriptEnumReloadWitnessReader();
    const auto& Transition = GetPreparedScriptEnumReloadTransition();
    return Reader.IsBoundToObject(Expected.Owner) && Transition.IsBoundToObject(Expected.Owner)
        && Reader.GetHandle() == Expected.Reader && Transition.GetHandle() == Expected.Transition;
}
}

namespace AngelscriptReloadSafety
{
bool ShouldDeferForPackageLoading()
{
    check(IsInGameThread());
    static bool bReported = false;
    const bool bLoading = IsLoading() || IsAsyncLoading();
    if (bLoading && !bReported)
    {
        if (IsLoading())
        {
            UE_LOG(Angelscript, Warning, TEXT("ASR-LINKER-LOAD-DEFERRED: Script reload postponed while packages are loading. ")
                TEXT("Detaching an export before serialization/PostLoad completes can leave an incomplete object or package. ")
                TEXT("Old script code remains active; queued reloads retry after loading. If loading does not finish, ")
                TEXT("finish/cancel the load and retry reload; restart the editor/game before saving affected assets after a linker loading error."));
        }
        else
        {
            UE_LOG(Angelscript, Verbose, TEXT("ASR-LINKER-LOAD-DEFERRED: Script reload postponed while packages are loading. ")
                TEXT("Detaching an export before serialization/PostLoad completes can leave an incomplete object or package. ")
                TEXT("Old script code remains active; queued reloads retry after loading."));
        }
    }
    bReported = bLoading;
    return bLoading;
}

FPreparedPayloadReloadScope::FPreparedPayloadReloadScope()
{
	if (!IsInGameThread() || PreparedPayloadScope || NextPayloadReloadEpoch == MAX_uint64)
		StopCommittedReload(TEXT("payload-scope"), TEXT("Invalid or nested payload preparation scope"));
	Epoch = NextPayloadReloadEpoch++;
	PreparedPayloadScope = this;
}

FPreparedPayloadReloadScope::~FPreparedPayloadReloadScope()
{
	if (PreparedPayloadScope != this)
		StopCommittedReload(TEXT("payload-scope"), TEXT("Payload preparation scope ownership changed"));
	if (bCommitted && Lease)
	{
		FString Error;
		if (!Lease->ValidateCompletion(Error)) StopCommittedReload(TEXT("payload-completion"), *Error);
	}
	Lease.Reset();
	ScriptEnumWitnessProvider = {};
	PreparedPayloadScope = nullptr;
}

FPreparedPayloadReloadScope* GetPreparedPayloadReloadScope() { return PreparedPayloadScope; }

const AngelscriptRuntimeGcMirror::FScriptObjectReloadPlan* FindScriptObjectReloadPlan(UClass* Class)
{
	return PreparedPayloadScope ? PreparedPayloadScope->Plans.FindByPredicate(
		[Class](const auto& Plan) { return Plan.Class == Class; }) : nullptr;
}

FAngelscriptPayloadReloadAdmission& GetPayloadReloadAdmission()
{
	static FAngelscriptPayloadReloadAdmission Admission;
	return Admission;
}

bool ExecutePayloadAdmissionWithScriptEnums(const TArray<FAngelscriptPayloadReloadType>& Types,
    const TArray<FPreparedScriptEnumReloadPlan>& EnumPlans, FString& Error)
{
    if (!IsInGameThread() || !PreparedPayloadScope || PreparedPayloadScope->Lease || PendingScriptEnumPlans || PendingScriptEnumEpoch
        || ScriptEnumWitnessProvider.Scope)
    {
        Error = TEXT("Invalid or nested script enum admission view");
        return false;
    }
    ScriptEnumWitnessProvider.Scope = PreparedPayloadScope;
    ScriptEnumWitnessProvider.Epoch = PreparedPayloadScope->Epoch;
    TArray<asITypeInfo*> Pins;
    const auto Pin = [&](const TSharedPtr<FAngelscriptModuleDesc>& Module, asITypeInfo* Cookie)
    {
        if (!Module.IsValid() || !Module->ScriptModule || !Cookie) return;
        auto* Engine = FAngelscriptManager::Get().GetScriptEngine();
        if (!Engine) return;
        for (asUINT I = 0; I < Engine->GetModuleCount(); ++I)
            if (Engine->GetModuleByIndex(I) == Module->ScriptModule)
            {
                const char* ActualName = Module->ScriptModule->GetName();
                if (!ActualName || Engine->GetModule(ActualName, asGM_ONLY_IF_EXISTS) != Module->ScriptModule) return;
                for (asUINT E = 0; E < Module->ScriptModule->GetEnumCount(); ++E)
                    if (Module->ScriptModule->GetEnumByIndex(E) == Cookie)
                    {
                        Cookie->AddRef();
                        Pins.Add(Cookie);
                        return;
                    }
            }
    };
    for (const auto& Plan : EnumPlans)
    {
        Pin(Plan.PreviousModule, Plan.PreviousCookie);
        Pin(Plan.CandidateModule, Plan.CandidateCookie);
    }
    ON_SCOPE_EXIT { for (asITypeInfo* Cookie : Pins) Cookie->Release(); };
    TGuardValue<const TArray<FPreparedScriptEnumReloadPlan>*> View(PendingScriptEnumPlans, &EnumPlans);
    TGuardValue<uint64> Epoch(PendingScriptEnumEpoch, PreparedPayloadScope->Epoch);
    const bool bAdmitted = GetPayloadReloadAdmission().Execute(Types, Error);
    for (asITypeInfo* Cookie : Pins) Cookie->Release();
    Pins.Reset();
    if (!bAdmitted) return false;
    if (!PreparedPayloadScope || PreparedPayloadScope != ScriptEnumWitnessProvider.Scope || !PreparedPayloadScope->Lease
        || ((ScriptEnumWitnessProvider.bSealed || GetPreparedScriptEnumReloadWitnessReader().IsBound()
            || GetPreparedScriptEnumReloadTransition().IsBound()) && !MatchesScriptEnumWitnessProvider(ScriptEnumWitnessProvider)))
    {
        Error = TEXT("Script enum admission provider or lease ownership changed");
        return false;
    }
    return PreparedPayloadScope->Lease->ValidateBeforeSwap(Error);
}

const TArray<FPreparedScriptEnumReloadPlan>* GetPreparedScriptEnumReloadPlans()
{
    return IsInGameThread() && PreparedPayloadScope && PendingScriptEnumEpoch == PreparedPayloadScope->Epoch
        ? PendingScriptEnumPlans : nullptr;
}

FPreparedScriptEnumReloadTransitionHandler& GetPreparedScriptEnumReloadTransition()
{
    static FPreparedScriptEnumReloadTransitionHandler Handler;
    return Handler;
}

FPreparedScriptEnumReloadWitnessReader& GetPreparedScriptEnumReloadWitnessReader()
{
    static FPreparedScriptEnumReloadWitnessReader Reader;
    return Reader;
}

bool SealPreparedScriptEnumReloadWitnessProvider(IPreparedPayloadReloadLease* Lease, const void* Owner, FString& Error)
{
    auto& State = ScriptEnumWitnessProvider;
    if (!IsInGameThread() || !PreparedPayloadScope || !PendingScriptEnumPlans
        || PendingScriptEnumEpoch != PreparedPayloadScope->Epoch || State.Scope != PreparedPayloadScope
        || State.Epoch != PreparedPayloadScope->Epoch || State.bSealAttempted)
    {
        Error = TEXT("Script enum witness provider cannot be registered outside its one admission");
        return false;
    }
    State.bSealAttempted = true;
    auto& Reader = GetPreparedScriptEnumReloadWitnessReader();
    auto& Transition = GetPreparedScriptEnumReloadTransition();
    if (!Lease || PreparedPayloadScope->Lease.Get() != Lease || !Owner
        || !Reader.IsBoundToObject(Owner) || !Transition.IsBoundToObject(Owner)
        || !Reader.GetHandle().IsValid() || !Transition.GetHandle().IsValid())
    {
        Error = TEXT("Script enum witness provider does not belong to the admitted lease");
        return false;
    }
    State.Lease = Lease;
    State.Owner = Owner;
    State.Reader = Reader.GetHandle();
    State.Transition = Transition.GetHandle();
    State.bSealed = true;
    return true;
}

bool GetPreparedScriptEnumReloadWitness(const UEnum* Enum, FPreparedScriptEnumReloadWitness& Out)
{
    Out = {};
    if (!IsInGameThread() || ScriptEnumWitnessProvider.bQueryActive) return false;
    const auto Expected = ScriptEnumWitnessProvider;
    if (!MatchesScriptEnumWitnessProvider(Expected)) return false;
    // Keep the actual lease and delegate instance alive across an execution-time rebind or release.
    const auto Lease = PreparedPayloadScope->Lease;
    const auto Reader = GetPreparedScriptEnumReloadWitnessReader();
    TGuardValue<bool> Busy(ScriptEnumWitnessProvider.bQueryActive, true);
    FPreparedScriptEnumReloadWitness Candidate;
    if (!Reader.Execute(Enum, Candidate) || Lease.Get() != Expected.Lease || !MatchesScriptEnumWitnessProvider(Expected)
        || Candidate.Epoch != Expected.Epoch) return false;
    Out = Candidate;
    return true;
}

void NotifyPreparedScriptEnumReload(const FPreparedScriptEnumReloadTransition& Transition)
{
    if (!IsInGameThread() || !PreparedPayloadScope || !PreparedPayloadScope->Lease) return;
    const auto Handler = GetPreparedScriptEnumReloadTransition();
    if (!Handler.IsBound() && !ScriptEnumWitnessProvider.bSealed) return; // No referenced script enum needs no listener.
    FString Error;
    if (!MatchesScriptEnumWitnessProvider(ScriptEnumWitnessProvider))
        StopCommittedReload(TEXT("payload-enum"), TEXT("Script enum transition provider ownership changed"));
    if (!PreparedPayloadScope->bCommitted || Transition.Epoch != PreparedPayloadScope->Epoch)
        StopCommittedReload(TEXT("payload-enum"), TEXT("Script enum transition has no matching committed epoch"));
    if (!Handler.Execute(Transition, Error)) StopCommittedReload(TEXT("payload-enum"), *Error);
    if (!MatchesScriptEnumWitnessProvider(ScriptEnumWitnessProvider))
        StopCommittedReload(TEXT("payload-enum"), TEXT("Script enum transition provider changed during execution"));
}

FCommittedReloadScope::FCommittedReloadScope()
{
	checkf(
		IsInGameThread(),
		TEXT("Committed reload scope construction requires the game thread.")
	);
	++CommittedReloadDepth;
	if (PreparedPayloadScope) PreparedPayloadScope->bCommitted = true;
}

FCommittedReloadScope::~FCommittedReloadScope() noexcept
{
	checkf(
		IsInGameThread(),
		TEXT("Committed reload scope destruction requires the game thread.")
	);
	checkf(
		CommittedReloadDepth > 0,
		TEXT("Committed reload scope depth underflow.")
	);
	--CommittedReloadDepth;
}

bool IsCommittedReloadActive()
{
	return IsInGameThread() && CommittedReloadDepth > 0;
}

[[noreturn]] void StopCommittedReload(const TCHAR* Phase, const TCHAR* Reason)
{
	UE_LOG(
		Angelscript,
		Error,
		TEXT("SCRIPT_RELOAD_COMMITTED_FAILURE phase=%s reason=%s exit_code=86 restart_required=1"),
		Phase != nullptr ? Phase : TEXT("<unknown>"),
		Reason != nullptr ? Reason : TEXT("<unknown>")
	);
	if (GLog != nullptr)
	{
		GLog->Flush();
	}

	// A committed failure cannot run engine delegates or DLL detach callbacks.
#if PLATFORM_WINDOWS
	::TerminateProcess(::GetCurrentProcess(), 86);
	__fastfail(FAST_FAIL_FATAL_APP_EXIT);
#else
	std::_Exit(86);
#endif
}

void CheckScriptExecution(int32 Result, const TCHAR* Operation)
{
	if (IsCommittedReloadActive() && Result != asEXECUTION_FINISHED)
	{
		const FString Reason = FString::Printf(TEXT("Script operation did not finish result=%d"), Result);
		StopCommittedReload(Operation, *Reason);
	}
}
}
