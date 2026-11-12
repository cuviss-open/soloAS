// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "RuntimeServices/ModKitScriptExecutionTimeout.h"

#include "Core/ModKitSettings.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "FeatureMods/ModKitRuntimeOps.h"
#include "AngelscriptExecutionBudget.h"
#include "Async/Async.h"
#include <memory>
#include <atomic>
#include "Misc/CoreDelegates.h"
#include "UObject/Package.h"

namespace
{
using namespace AngelscriptCoreExtensionPoints;
struct FGenerationBudget
{
    FString ModuleName;
    FString Owner;
    uint64 BudgetNs = 0;
};
struct FBudgetPolicy
{
    using FGenerations = TMap<uint64, FGenerationBudget>;
    std::shared_ptr<const FGenerations> Generations = std::make_shared<const FGenerations>();
    // Only the GT completion tasks access counts; executing threads read snapshots.
    TMap<FString, int32> ConsecutiveTimeouts;
};
thread_local TSet<uint64> TimedOutGenerations;
FDelegateHandle BudgetHandle, PreExitHandle;
TSharedPtr<FBudgetPolicy, ESPMode::ThreadSafe> ExecutionBudgetPolicy;
}

void FModKitScriptExecutionTimeout::Startup()
{
    check(IsInGameThread());
    if (BudgetHandle.IsValid()) return;
    const auto* Settings = UModKitSettings::Get();
    const bool bDisableOnTimeout = Settings->bDisableModOnTimeout;
    const int32 Threshold = Settings->MaxConsecutiveTimeoutsBeforeDisable;
    if (Threshold < 1)
        UE_LOG(LogAngelscriptModKit, Fatal, TEXT("Invalid MaxConsecutiveTimeoutsBeforeDisable"));
    const double Seconds = Settings->ModScriptExecutionTimeoutSec;
    if (!FMath::IsFinite(Seconds) || Seconds < 0.1 || Seconds >= double(MAX_uint64) / 1000000000.0)
        UE_LOG(LogAngelscriptModKit, Fatal, TEXT("Invalid ModScriptExecutionTimeoutSec execution budget"));
    uint64 BudgetNs = 0;
#if UE_BUILD_TEST || UE_BUILD_SHIPPING
    BudgetNs = uint64(Seconds * 1000000000.0);
#elif !WITH_EDITOR
    if (Settings->bForceModKitTimeoutInDev)
    {
        BudgetNs = uint64(Seconds * 1000000000.0);
        UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_TIMEOUT_FORCE_ACTIVE"));
    }
#endif
    auto Snapshot = MakeShared<FBudgetPolicy, ESPMode::ThreadSafe>();
    FExecutionBudgetProvider Provider;
    Provider.BindGeneration = [Snapshot, BudgetNs](const FModuleGenerationId& Generation,
        const FAngelscriptModuleDesc& Module, FString& Error)
    {
        FModKitSourceOwner Owner;
        FString OwnerError;
        if (!FModKitCompilationManager::GetGenerationOwner(Module, Owner, OwnerError)) return true;
        if (Owner.Layer != EModKitSourceLayer::Leaf || BudgetNs == 0) return true;
        if (!Generation.Generation || Module.CoreGenerationId != Generation.Generation
            || Module.ModuleName != Generation.ModuleName)
        { Error = TEXT("modkit-budget-generation-mismatch"); return false; }
        auto Next = std::make_shared<FBudgetPolicy::FGenerations>(*std::atomic_load(&Snapshot->Generations));
        Next->Add(Generation.Generation, {Generation.ModuleName, Owner.Id, BudgetNs});
        std::atomic_store(&Snapshot->Generations, std::shared_ptr<const FBudgetPolicy::FGenerations>(MoveTemp(Next)));
        return true;
    };
    Provider.ReleaseGeneration = [Snapshot](const FModuleGenerationId& Generation)
    {
        const auto Current = std::atomic_load(&Snapshot->Generations);
        if (!Current->Contains(Generation.Generation)) return;
        auto Next = std::make_shared<FBudgetPolicy::FGenerations>(*Current);
        Next->Remove(Generation.Generation);
        std::atomic_store(&Snapshot->Generations, std::shared_ptr<const FBudgetPolicy::FGenerations>(MoveTemp(Next)));
    };
    Provider.QueryBudgetNs = [Snapshot](const FModuleGenerationId& Generation) -> uint64
    {
        const auto Generations = std::atomic_load(&Snapshot->Generations);
        const auto* Budget = Generations->Find(Generation.Generation);
        return Budget && Budget->ModuleName == Generation.ModuleName ? Budget->BudgetNs : 0;
    };
    Provider.OnExceeded = [Snapshot](const FExecutionBudgetEvent& Event)
    {
        FString Owner;
        {
            const auto Generations = std::atomic_load(&Snapshot->Generations);
            const auto* Budget = Generations->Find(Event.Module.Generation);
            if (!Budget || Budget->ModuleName != Event.Module.ModuleName) return;
            Owner = Budget->Owner;
        }
        UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_EXECUTION_BUDGET_EXCEEDED owner=%s generation=%llu"),
            *Owner, static_cast<unsigned long long>(Event.Module.Generation));
        TimedOutGenerations.Add(Event.Module.Generation);
    };
    Provider.OnCompleted = [Snapshot, bDisableOnTimeout, Threshold](const FModuleGenerationId& Generation, bool bSucceeded)
    {
        // A plain script exception is not a timeout. Core orders the matching
        // OnExceeded before this interval's completion on this executing thread.
        const bool bTimedOut = TimedOutGenerations.Remove(Generation.Generation) != 0;
        if (!bSucceeded && !bTimedOut) return;
        AsyncTask(ENamedThreads::GameThread, [Snapshot, Generation, bSucceeded, bTimedOut, bDisableOnTimeout, Threshold]()
        {
            const auto Generations = std::atomic_load(&Snapshot->Generations);
            const auto* Budget = Generations->Find(Generation.Generation);
            if (!Budget || Budget->ModuleName != Generation.ModuleName
                || FModKitModRegistry::IsTimeoutDisabled(Budget->Owner)) return;
            int32& Count = Snapshot->ConsecutiveTimeouts.FindOrAdd(Budget->Owner);
            if (bSucceeded) { Count = 0; return; }
            if (!bTimedOut || Count >= Threshold) return;
            ++Count;
            UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_TIMEOUT_COUNTED mod=%s count=%d"), *Budget->Owner, Count);
            if (bDisableOnTimeout && Count == Threshold)
                FModKitRuntimeOps::RequestTimeoutDisable(Budget->Owner);
        });
    };
    BudgetHandle = RegisterExecutionBudget(MoveTemp(Provider));
    if (!BudgetHandle.IsValid())
        UE_LOG(LogAngelscriptModKit, Fatal, TEXT("ModKit execution budget provider unavailable"));
    ExecutionBudgetPolicy = Snapshot;
    PreExitHandle = FCoreDelegates::OnPreExit.AddStatic(&FModKitScriptExecutionTimeout::Shutdown);
}

void FModKitScriptExecutionTimeout::Shutdown()
{
    check(IsInGameThread());
    if (BudgetHandle.IsValid())
    {
        if (AngelscriptCoreExtensionPoints::UnregisterExecutionBudget(BudgetHandle)
            != AngelscriptCoreExtensionPoints::ERemoveResult::Removed) return;
        BudgetHandle.Reset();
    }
    if (PreExitHandle.IsValid()) FCoreDelegates::OnPreExit.Remove(PreExitHandle);
    PreExitHandle.Reset();
    ExecutionBudgetPolicy.Reset();
}

bool FModKitScriptExecutionTimeout::IsRetained()
{
    return BudgetHandle.IsValid();
}

#if WITH_DEV_AUTOMATION_TESTS && !WITH_EDITOR
#include "ClassGenerator/ASClass.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformTime.h"
#include "AngelscriptExecutionQuiescence.h"
#include "angelscript.h"
#include "as_module.h"

class FModKitH5BudgetCompletionCommand final : public IAutomationLatentCommand
{
public:
    explicit FModKitH5BudgetCompletionCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        if (!bStarted)
        {
            Snapshot = ExecutionBudgetPolicy;
            if (!Test->TestTrue(TEXT("Default ModKit budget provider is installed"), Snapshot.IsValid())) return true;
            const auto Generations = std::atomic_load(&Snapshot->Generations);
            int32 Targets = 0;
            auto& Manager = FAngelscriptManager::Get();
            for (const auto& Entry : *Generations)
            {
                if (Entry.Value.Owner != TEXT("H5Collection")) continue;
                ++Targets;
                const auto Module = Manager.GetModuleByModuleName(Entry.Value.ModuleName);
                Test->TestTrue(TEXT("Early budget binding names the actual cache constructor generation"), Module.IsValid()
                    && Module->CoreGenerationId == Entry.Key && Module->CoreCacheSetGuid.IsValid());
                Test->TestTrue(TEXT("The real Game budget is positive before execution"), Entry.Value.BudgetNs > 0);
            }
            if (!Test->TestEqual(TEXT("Both H5 collection members have early budget bindings"), Targets, 2)) return true;
            {
                FAngelscriptExecutionReadScope Reader;
                asIScriptFunction* Marker = nullptr;
                for (const auto& Module : Manager.GetActiveModules())
                    if (FModKitCompilationManager::OwnsModule(Module, TEXT("H5Collection")))
                        if (auto* Function = Module->ScriptModule->GetFunctionByDecl("int H5Collection::ModKitH5CollectionMarker()")) Marker = Function;
                if (!Test->TestNotNull(TEXT("The budget target has its real cached marker"), Marker)) return true;
                auto* Context = Manager.GetScriptEngine()->CreateContext();
                ON_SCOPE_EXIT { Context->Release(); };
                Test->TestTrue(TEXT("Prepare the real budget target"), Context->Prepare(Marker) >= 0);
                Test->TestEqual(TEXT("The generation-bound target executes"), Context->Execute(), int(asEXECUTION_FINISHED));
                Test->TestEqual(TEXT("Budgeted cache preserves its result"), Context->GetReturnDWord(), asDWORD(31));
            }
            bStarted = true;
            Deadline = FPlatformTime::Seconds() + 5.0;
            return false;
        }
        if (const auto* Count = Snapshot->ConsecutiveTimeouts.Find(TEXT("H5Collection")))
        {
            Test->TestEqual(TEXT("Core calls completion for the H5 budget generation"), *Count, 0);
            return true;
        }
        if (FPlatformTime::Seconds() >= Deadline)
        {
            Test->AddError(TEXT("The real H5 execution did not reach ModKit OnCompleted"));
            return true;
        }
        return false;
    }
private:
    FAutomationTestBase* Test;
    TSharedPtr<FBudgetPolicy, ESPMode::ThreadSafe> Snapshot;
    bool bStarted = false;
    double Deadline = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitH5BudgetGenerationTest,
    "AngelscriptModKit.ExecutionBudget.H5Generation",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitH5BudgetGenerationTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FModKitH5BudgetCompletionCommand(this));
    return true;
}

class FModKitCacheTimeoutCleanupCommand final : public IAutomationLatentCommand
{
public:
    explicit FModKitCacheTimeoutCleanupCommand(FAutomationTestBase* InTest, bool bInSource = false)
        : Test(InTest), bSource(bInSource) {}
    ~FModKitCacheTimeoutCleanupCommand() override
    {
        FModKitRuntimeOps::OnBeforeUnload.Remove(BeforeUnload);
        if (ExternalHold) { ExternalHold->Unprepare(); ExternalHold->Release(); }
    }
    bool Update() override
    {
        const FString Id = bSource ? TEXT("H5TimeoutSource") : TEXT("H5Collection");
        auto& Manager = FAngelscriptManager::Get();
        if (!bStarted)
        {
            if (bSource)
            {
                if (!Test->TestTrue(TEXT("Source representative runs beside a real authenticated cache"),
                    FModKitCompilationManager::HasCacheSet(TEXT("H5Collection")))) return true;
                Test->TestFalse(TEXT("Source owner has no invented H5 set handle"), FModKitCompilationManager::HasCacheSet(Id));
                int32 Sources = 0;
                for (const auto& Module : Manager.GetActiveModules())
                    if (FModKitCompilationManager::OwnsModule(Module, Id))
                    {
                        ++Sources;
                        Test->TestFalse(TEXT("The target is a source generation"), Module->CoreCacheSetGuid.IsValid());
                        ExternalHold = Manager.GetScriptEngine()->CreateContext();
                        if (!Test->TestTrue(TEXT("External consumer prepares its real source function"),
                            ExternalHold->Prepare(Module->ScriptModule->GetFunctionByDecl("int H5TimeoutSource::ModKitSourceTimeoutValue()")) >= 0)) return true;
                    }
                if (!Test->TestEqual(TEXT("Exactly one authenticated source module is active"), Sources, 1)) return true;
                const auto InstanceClass = Manager.GetClass(TEXT("H5TimeoutSource::UModKitSourceTimeoutObject"));
                if (!Test->TestTrue(TEXT("Source unit publishes its reflected instance class"), InstanceClass.IsValid() && InstanceClass->Class)) return true;
                OwnedInstance = NewObject<UObject>(GetTransientPackage(), InstanceClass->Class);
                if (!Test->TestTrue(TEXT("Source instance exists before timeout"), OwnedInstance.IsValid())) return true;
                BaselineModules = Manager.GetActiveModules().Num() - Sources;
                BeforeUnload = FModKitRuntimeOps::OnBeforeUnload.AddLambda([this, Id](const FString& Name)
                    { if (Name == Id) ++TeardownCount; });
            }
            else
            {
                if (!Test->TestTrue(TEXT("The signed cache starts normally"), FModKitCompilationManager::HasCacheSet(Id))) return true;
                BaselineModules = Manager.GetActiveModules().Num() - 2;
            }
            Deadline = FPlatformTime::Seconds() + 30.0;
            bStarted = true;
        }
        if (FPlatformTime::Seconds() >= Deadline)
        { Test->AddError(TEXT("Timed-out cache cleanup did not finish")); return true; }
        if (FModKitModRegistry::IsTimeoutDisabled(Id))
        {
            if (bSource && ExternalHold)
            {
                if (!TeardownCount) return false;
                const double Now = FPlatformTime::Seconds();
                const auto* Queued = FModKitRuntimeOps::PendingOps.FindByPredicate([&](const auto& Op)
                    { return Op.bIsTimeoutDisable && Op.ModName == Id; });
                if (!Test->TestNotNull(TEXT("Busy source keeps its own retry entry"), Queued)) return true;
                if (RetryDeadline == 0)
                {
                    RetryDeadline = Queued->RetryAfterSeconds;
                    if (!Test->TestTrue(TEXT("Busy source schedules a delayed retry"), RetryDeadline > Now)) return true;
                    FModKitRuntimeOps::RequestUnload(TEXT("H5Collection"));
                    OtherPhase = 1;
                    return false;
                }
                if (Now < RetryDeadline)
                {
                    Test->TestEqual(TEXT("Backoff does not repeat safety scans every frame"), Queued->RetryAfterSeconds, RetryDeadline);
                    bObservedBackoff = true;
                }
                else RetryDeadline = Queued->RetryAfterSeconds;
                if (FModKitRuntimeOps::IsOperationPending(TEXT("H5Collection"))) return false;
                if (OtherPhase == 1)
                {
                    Test->TestFalse(TEXT("Unrelated mod unload completes while source is held"), FModKitModRegistry::IsModActivated(TEXT("H5Collection")));
                    FModKitRuntimeOps::RequestLoad(TEXT("H5Collection"));
                    OtherPhase = 2;
                    return false;
                }
                if (!Test->TestTrue(TEXT("Unrelated mod load completes while source is held"), FModKitModRegistry::IsModActivated(TEXT("H5Collection")))) return true;
                Test->TestTrue(TEXT("Retry backoff was observed on a later frame"), bObservedBackoff);
                FModKitSourceOwner Admitted;
                FString Error;
                for (const auto& Module : Manager.GetActiveModules())
                    if (FModKitCompilationManager::OwnsModule(Module, Id))
                    {
                        Test->TestTrue(TEXT("Failed state preserves immutable admitted ownership"),
                            FModKitCompilationManager::GetGenerationOwner(*Module, Admitted, Error) && Admitted.Id == Id);
                        Test->TestFalse(TEXT("Failed owner cannot admit fresh source"), FModKitModRegistry::IsSourceOwnerCurrent(Admitted));
                    }
                Test->TestFalse(TEXT("Timeout teardown destroys its owned instance before core removal"), OwnedInstance.IsValid());
                Test->TestTrue(TEXT("Real external hold makes source cleanup Busy"), FModKitCompilationManager::RequestModuleRemoval(Id).bBusy);
                Test->TestTrue(TEXT("Busy keeps timeout cleanup queued"), FModKitRuntimeOps::IsOperationPending(Id));
                Test->TestEqual(TEXT("Busy keeps source intact"), Manager.GetActiveModules().Num(), BaselineModules + 1);
                Test->TestTrue(TEXT("Native epoch can change before source cleanup"), Manager.GetScriptEngine()->RegisterEnum("ModKitTimeoutCleanupEpoch") >= 0);
                ExternalHold->Unprepare(); ExternalHold->Release(); ExternalHold = nullptr;
                return false;
            }
            if (FModKitRuntimeOps::IsOperationPending(Id)) return false;
            if (bCheckingReload)
            {
                Test->TestFalse(TEXT("Reload cannot restore a timeout-disabled collection"), FModKitCompilationManager::HasCacheSet(Id));
                Test->TestEqual(TEXT("Rejected reload keeps modules at baseline"), Manager.GetActiveModules().Num(), BaselineModules);
                return true;
            }
            if (bSource)
            {
                Test->TestEqual(TEXT("Busy retry does not repeat entry/instance teardown"), TeardownCount, 1);
                CollectGarbage(RF_NoFlags);
                Test->TestFalse(TEXT("Removed source class is no longer discoverable"), Manager.GetClass(TEXT("H5TimeoutSource::UModKitSourceTimeoutObject")).IsValid());
                for (const auto& Module : Manager.GetActiveModules())
                    Test->TestFalse(TEXT("No active source entry remains after exact cleanup"), FModKitCompilationManager::OwnsModule(Module, Id));
            }
            Test->TestFalse(TEXT("Timeout cleanup releases the cache set"), FModKitCompilationManager::HasCacheSet(Id));
            Test->TestEqual(TEXT("Executable modules return to baseline after three timeouts"), Manager.GetActiveModules().Num(), BaselineModules);
            const auto Generations = std::atomic_load(&ExecutionBudgetPolicy->Generations);
            for (const auto& Entry : *Generations)
                Test->TestTrue(TEXT("Timeout cleanup releases the generation budget registrations"), Entry.Value.Owner != Id);
            FModKitRuntimeOps::RequestLoad(Id);
            bCheckingReload = true;
            return false;
        }
        if (bCheckingReload)
        { Test->AddError(TEXT("Timeout-disabled state was lost")); return true; }
        if (CompletedCalls >= 3) return false;
        const auto* Count = ExecutionBudgetPolicy->ConsecutiveTimeouts.Find(Id);
        if (CompletedCalls && (!Count || *Count != CompletedCalls)) return false;
        FAngelscriptExecutionReadScope Reader;
        asIScriptFunction* Loop = nullptr;
        for (const auto& Module : Manager.GetActiveModules())
            if (FModKitCompilationManager::OwnsModule(Module, Id))
                if (auto* Function = Module->ScriptModule->GetFunctionByDecl(bSource ? "void H5TimeoutSource::ModKitSourceTimeout()" : "void H5Collection::ModKitH5Timeout()")) Loop = Function;
        if (!Test->TestNotNull(TEXT("The authenticated package supplies its timeout function"), Loop)) return true;
        auto* Context = Manager.GetScriptEngine()->CreateContext();
        ON_SCOPE_EXIT { Context->Release(); };
        if (!Test->TestTrue(TEXT("Prepare real timeout function"), Context->Prepare(Loop) >= 0)) return true;
        Test->TestEqual(TEXT("The real execution budget interrupts the loop"), Context->Execute(), int(asEXECUTION_EXCEPTION));
        ++CompletedCalls;
        return false;
    }
private:
    FAutomationTestBase* Test;
    bool bStarted = false, bCheckingReload = false, bSource = false;
    asIScriptContext* ExternalHold = nullptr;
    TWeakObjectPtr<UObject> OwnedInstance;
    FDelegateHandle BeforeUnload;
    int32 TeardownCount = 0, OtherPhase = 0;
    double RetryDeadline = 0;
    bool bObservedBackoff = false;
    int32 BaselineModules = 0, CompletedCalls = 0;
    double Deadline = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitCacheTimeoutCleanupTest,
    "AngelscriptModKit.ExecutionBudget.CacheTimeoutCleanup",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitCacheTimeoutCleanupTest::RunTest(const FString&)
{
    AddExpectedError(TEXT("MODKIT_MOD_DISABLED name=H5Collection reason=script-timeout"), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedError(TEXT("MODKIT_RUNTIME_OP_REJECTED op=load name=H5Collection reason=timeout-disabled"), EAutomationExpectedErrorFlags::Contains, 1);
    ADD_LATENT_AUTOMATION_COMMAND(FModKitCacheTimeoutCleanupCommand(this));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitSourceTimeoutCleanupTest,
    "AngelscriptModKit.ExecutionBudget.SourceTimeoutCleanup",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitSourceTimeoutCleanupTest::RunTest(const FString&)
{
    AddExpectedError(TEXT("MODKIT_MOD_DISABLED name=H5TimeoutSource reason=script-timeout"), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedError(TEXT("MODKIT_RUNTIME_OP_REJECTED op=load name=H5TimeoutSource reason=timeout-disabled"), EAutomationExpectedErrorFlags::Contains, 1);
    ADD_LATENT_AUTOMATION_COMMAND(FModKitCacheTimeoutCleanupCommand(this, true));
    return true;
}
#endif
