                                                                                                       
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptCoreExtensionPoints.h"
#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptManager.h"
#include "ClassGenerator/AngelscriptRetiredModules.h"
#include "ClassGenerator/ASStruct.h"
#include "UObject/GarbageCollection.h"
#include "StartAngelscriptHeaders.h"
#include "as_scriptengine.h"
#include "as_module.h"
#include "EndAngelscriptHeaders.h"

                                                                                                     
                                                                                                          
#include "StaticJIT/PrecompiledData.h"
#include "AngelscriptBinds.h"
#include "AngelscriptStructLiveValues.h"
#include "ClassGenerator/ASClass.h"
#include "UObject/UObjectIterator.h"
#include "HAL/PlatformMemory.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeExit.h"
                                                                                                        
                                                                                                       
#if WITH_AS_DEBUGSERVER
#include "Debugging/AngelscriptDebugServer.h"
#endif
                                                                                                     
                                                                                                         
namespace AngelscriptCoreExtensionPoints
{
bool GHasPreprocessDependencyProvider = false;
namespace
{
FDelegateHandle PreprocessDependencyHandle;
FPreprocessDependencyProvider PreprocessDependencyProvider;
bool bCollectingPreprocessDependencies = false;
}
FDelegateHandle RegisterPreprocessDependencyProvider(FName FeatureId, FPreprocessDependencyProvider Provider)
{
    if (!IsInGameThread() || FeatureId.IsNone() || !Provider || PreprocessDependencyHandle.IsValid()
        || bCollectingPreprocessDependencies || IsAngelscriptExecutionShutdownStarted()) return {};
    PreprocessDependencyHandle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
    PreprocessDependencyProvider = MoveTemp(Provider);
    GHasPreprocessDependencyProvider = true;
    return PreprocessDependencyHandle;
}
ERemoveResult UnregisterPreprocessDependencyProvider(FDelegateHandle Handle)
{
    if (!IsInGameThread() || bCollectingPreprocessDependencies) return ERemoveResult::Busy;
    if (!Handle.IsValid() || Handle != PreprocessDependencyHandle) return ERemoveResult::Unknown;
    GHasPreprocessDependencyProvider = false;
    PreprocessDependencyProvider = {};
    PreprocessDependencyHandle.Reset();
    return ERemoveResult::Removed;
}
void CollectPreprocessDependencies(TConstArrayView<FPreprocessSource> Sources, TArray<FPreprocessDependency>& Out)
{
    if (!GHasPreprocessDependencyProvider) return;
    check(IsInGameThread());
    TGuardValue<bool> Guard(bCollectingPreprocessDependencies, true);
    PreprocessDependencyProvider(Sources, Out);
}
}
                                                                                                       
                                                                                                   
namespace AngelscriptCoreExtensionPoints
{
namespace
{
struct FDebugAliasProvider { FName FeatureId; TFunction<void(TArray<FDebugTypeAlias>&)> Collect; };
TMap<FDelegateHandle, FDebugAliasProvider> DebugAliasProviders;
bool bCollectingDebugAliases = false;
}
FDelegateHandle RegisterDebugTypeAliasProvider(FName FeatureId, TFunction<void(TArray<FDebugTypeAlias>&)> Collect)
{
    if (!IsInGameThread() || FeatureId.IsNone() || !Collect || bCollectingDebugAliases
        || IsAngelscriptExecutionShutdownStarted()) return {};
    for (const auto& Entry : DebugAliasProviders)
        if (Entry.Value.FeatureId == FeatureId) return {};
    FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
    DebugAliasProviders.Add(Handle, {FeatureId, MoveTemp(Collect)});
    return Handle;
}
ERemoveResult UnregisterDebugTypeAliasProvider(FDelegateHandle Handle)
{
    if (!IsInGameThread() || bCollectingDebugAliases) return ERemoveResult::Busy;
    return DebugAliasProviders.Remove(Handle) ? ERemoveResult::Removed : ERemoveResult::Unknown;
}
void CollectDebugTypeAliases(TArray<FDebugTypeAlias>& Out)
{
    if (DebugAliasProviders.IsEmpty()) return;
    check(IsInGameThread());
    TGuardValue<bool> Guard(bCollectingDebugAliases, true);
    for (const auto& Entry : DebugAliasProviders) Entry.Value.Collect(Out);
}
}
                                                                                                 
                                                                                                       
namespace AngelscriptCoreExtensionPoints
{
bool GHasReloadObservers = false;
bool GCollectingReloadDiagnostics = false;
namespace
{
struct FReloadDeferral
{
    FName FeatureId;
    TFunction<bool()> Predicate;
    TFunction<void(uint64)> OnBegin;
    TFunction<void(uint64, bool, const TArray<FReloadDiagnostic>&)> OnComplete;
};
TMap<FDelegateHandle, FReloadDeferral> ReloadDeferrals;
bool bEvaluatingReloadDeferrals = false;
uint64 ReloadGeneration = 0;
TArray<FReloadDiagnostic> ReloadDiagnostics;
}
FDelegateHandle RegisterAutomaticReloadObserver(FName FeatureId, TFunction<bool()> ShouldDefer,
    TFunction<void(uint64)> OnBegin, TFunction<void(uint64, bool, const TArray<FReloadDiagnostic>&)> OnComplete)
{
    if (!IsInGameThread() || FeatureId.IsNone() || !ShouldDefer || !OnBegin || !OnComplete || bEvaluatingReloadDeferrals
        || IsAngelscriptExecutionShutdownStarted()) return {};
    for (const auto& Entry : ReloadDeferrals)
        if (Entry.Value.FeatureId == FeatureId) return {};
    FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
    ReloadDeferrals.Add(Handle, {FeatureId, MoveTemp(ShouldDefer), MoveTemp(OnBegin), MoveTemp(OnComplete)});
    GHasReloadObservers = true;
    return Handle;
}
ERemoveResult UnregisterAutomaticReloadObserver(FDelegateHandle Handle)
{
    if (!IsInGameThread() || bEvaluatingReloadDeferrals) return ERemoveResult::Busy;
    if (!ReloadDeferrals.Remove(Handle)) return ERemoveResult::Unknown;
    GHasReloadObservers = !ReloadDeferrals.IsEmpty();
    return ERemoveResult::Removed;
}
bool ShouldDeferAutomaticReload()
{
    if (ReloadDeferrals.IsEmpty()) return false;
    check(IsInGameThread());
    TGuardValue<bool> Guard(bEvaluatingReloadDeferrals, true);
    for (const auto& Entry : ReloadDeferrals)
        if (Entry.Value.Predicate()) return true;
    return false;
}
void NotifyAutomaticReloadBegin()
{
    if (ReloadDeferrals.IsEmpty()) return;
    ReloadDiagnostics.Reset();
    GCollectingReloadDiagnostics = true;
    ++ReloadGeneration;
    TGuardValue<bool> Guard(bEvaluatingReloadDeferrals, true);
    for (const auto& Entry : ReloadDeferrals) Entry.Value.OnBegin(ReloadGeneration);
}
void NotifyAutomaticReloadComplete(bool bSuccess)
{
    if (!GCollectingReloadDiagnostics) return;
    GCollectingReloadDiagnostics = false;
    TGuardValue<bool> Guard(bEvaluatingReloadDeferrals, true);
    for (const auto& Entry : ReloadDeferrals) Entry.Value.OnComplete(ReloadGeneration, bSuccess, ReloadDiagnostics);
    ReloadDiagnostics.Reset();
}
void CaptureReloadDiagnostic(const FString& Filename, const FString& Message,
    int32 Line, int32 Column, bool bIsError, bool bIsInfo)
{
    if (GCollectingReloadDiagnostics)
        ReloadDiagnostics.Add({Filename, Message, Line, Column, bIsError, bIsInfo});
}
bool HasDebugClientEndpoint(const FString& Endpoint)
{
    if (!IsInGameThread() || Endpoint.IsEmpty() || !FAngelscriptManager::IsInitialized()) return false;
#if WITH_AS_DEBUGSERVER
    const auto* Server = FAngelscriptManager::Get().DebugServer;
    return Server != nullptr && Server->HasClientEndpoint(Endpoint);
#else
    return false;
#endif
}
}
                                                                                                     
                                                                                                        
namespace AngelscriptCoreExtensionPoints
{
namespace
{
struct FReloadClient { FName FeatureId; TSet<uint64> Requests; };
TMap<FDelegateHandle, FReloadClient> ReloadClients;
bool bInExplicitCall = false;
}
FDelegateHandle RegisterExplicitReloadClient(FName FeatureId)
{
    if (!IsInGameThread() || FeatureId.IsNone() || bInExplicitCall || IsAngelscriptExecutionShutdownStarted()) return {};
    for (const auto& Entry : ReloadClients)
        if (Entry.Value.FeatureId == FeatureId) return {};
    FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
    ReloadClients.Add(Handle, {FeatureId, {}});
    return Handle;
}
ERemoveResult UnregisterExplicitReloadClient(FDelegateHandle Client)
{
    if (!IsInGameThread() || bInExplicitCall) return ERemoveResult::Busy;
    if (!ReloadClients.Remove(Client)) return ERemoveResult::Unknown;
    if (ReloadClients.IsEmpty() && FAngelscriptManager::IsInitialized())
        FAngelscriptManager::Get().ConsumedExplicitSourceIdentities.Empty();
    return ERemoveResult::Removed;
}
EReloadAdmission TryExplicitReload(FDelegateHandle Client, const FExplicitReloadRequest& Request,
    TFunctionRef<void(const FReloadResultView&)> Complete, FString& Error)
{
    if (!IsInGameThread()) { Error = TEXT("Explicit reload requires the game thread."); return EReloadAdmission::Invalid; }
    if (bInExplicitCall) return EReloadAdmission::Busy;
    auto* Entry = ReloadClients.Find(Client);
    if (!Entry || Request.RequestId == 0 || Entry->Requests.Contains(Request.RequestId))
    { Error = TEXT("Invalid client or repeated reload request."); return EReloadAdmission::Invalid; }
    if (!FAngelscriptManager::IsInitialized()) return EReloadAdmission::Busy;
    TGuardValue<bool> Guard(bInExplicitCall, true);
    const auto Admission = FAngelscriptManager::Get().TryExplicitReloadRequest(Request, Complete, Error);
    if (Admission == EReloadAdmission::Accepted) Entry->Requests.Add(Request.RequestId);
    return Admission;
}
}
                                                                                                      

                                                                                                       
namespace AngelscriptCoreExtensionPoints
{
std::atomic<unsigned int> Private::ModuleCacheClientCount{0};
namespace
{
struct FCacheSetOwner
{
    TArray<TUniquePtr<FPreparedModuleCacheSet>> Members;
    TArray<TSharedRef<FAngelscriptModuleDesc>> Modules;
    uint64 NativeEpoch = 0;
};
struct FCacheClientOwner
{
    FName FeatureId;
    bool bCacheClient = false;
    bool bTransient = false;
    bool bRetainCompileSurface = false;
    uint64 StructLiveToken = 0;
    bool bCacheApplying = false;
    TMap<uint64, TUniquePtr<FCacheSetOwner>> Sets;
    TUniquePtr<FAngelscriptExecutionWriteScope> Writer;
    TUniquePtr<FCompileSurfaceLease> Lease;
    uint64 CompileUsedPhysicalBefore = 0;
    uint64 Action = 0;
    // IDs and weak candidates do not add descriptor/VM lifetime holds.
    TMap<uint64, FString> SourceGenerations;
    TArray<TWeakPtr<FAngelscriptModuleDesc>> PendingSourceModules;
    TMap<uint64, asIScriptModule*> GenerationLeases;
    TFunction<void()> OnClosing;
    bool bClosing = false;
    bool bNotifiedClosing = false;
    bool bWarnedClosing = false;
};
TMap<FDelegateHandle, FCacheClientOwner>& CacheClients()
{
    static TMap<FDelegateHandle, FCacheClientOwner> Clients;
    return Clients;
}
TUniquePtr<FCompileSurfaceRecipe> StartupCompileRecipe;
asIScriptEngine* StartupCompileEngine = nullptr;
FString StartupCompileError;
bool bCompileCaptureStarted = false;
uint64 CompileRegistrationEpoch = 0;
uint64 NextCompileAction = 0;
uint64 NextGenerationLease = 0;
uint64 NextCacheSet = 0;
bool bInCacheAction = false;
FDelegateHandle CompilePreExit;
bool bClosingCacheClients = false;
bool bInCacheClosingCallback = false;
}
static FDelegateHandle RegisterCacheClient(FName FeatureId, TFunction<void()> OnClosing, bool bRetainDuringStartup)
{
    if (!IsInGameThread() || FeatureId.IsNone() || bClosingCacheClients || bInCacheClosingCallback
        || bInCacheAction || Private::IsNativeCompileActionActive() || IsAngelscriptExecutionShutdownStarted()) return {};
    for (const auto& Entry : CacheClients())
        if (Entry.Value.FeatureId == FeatureId) return {};
    FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
    CacheClients().Add(Handle).FeatureId = FeatureId;
    auto& Owner = CacheClients().FindChecked(Handle);
    Owner.OnClosing = MoveTemp(OnClosing);
    Owner.bCacheClient = bRetainDuringStartup;
    if (bRetainDuringStartup && !bCompileCaptureStarted && !FAngelscriptManager::IsInitialized())
    {
        uint32 Count = 1;
        for (const auto& Entry : CacheClients()) if (Entry.Value.bRetainCompileSurface) ++Count;
        if (!Private::SetTransientNativeClientCount(Count)) { CacheClients().Remove(Handle); return {}; }
        Owner.bRetainCompileSurface = true;
        Owner.StructLiveToken = EnableAngelscriptStructLiveValues();
        ++CompileRegistrationEpoch;
    }
    if (Owner.bCacheClient) Private::ModuleCacheClientCount.fetch_add(1, std::memory_order_release);
    return Handle;
}
FDelegateHandle RegisterModuleCacheClient(FName FeatureId, TFunction<void()> OnClosing)
{
    return RegisterCacheClient(FeatureId, MoveTemp(OnClosing), true);
}
FDelegateHandle RegisterModuleGenerationLeaseClient(FName FeatureId, TFunction<void()> OnClosing)
{
    return RegisterCacheClient(FeatureId, MoveTemp(OnClosing), false);
}
bool Private::IsModuleCacheOwned(asIScriptModule* Module)
{
    if (!Module) return false;
    for (const auto& Client : CacheClients())
        for (const auto& Set : Client.Value.Sets)
            for (const auto& Owned : Set.Value->Modules)
                if (Owned->ScriptModule == Module) return true;
    return false;
}
void Private::ForgetSourceGeneration(uint64 Generation)
{
    if (!Generation) return;
    for (auto& Entry : CacheClients()) Entry.Value.SourceGenerations.Remove(Generation);
}
#if WITH_DEV_AUTOMATION_TESTS
int32 Private::GetOwnedSourceGenerationCountForTests(FDelegateHandle Client)
{
    const auto* Owner = CacheClients().Find(Client);
    return Owner ? Owner->SourceGenerations.Num() : 0;
}
#endif
static bool HasActiveSourceGenerations(FCacheClientOwner& Owner)
{
    if (!FAngelscriptManager::IsInitialized()) return !Owner.SourceGenerations.IsEmpty();
    for (auto It = Owner.SourceGenerations.CreateIterator(); It; ++It)
    {
        const auto Module = FAngelscriptManager::Get().GetModuleByModuleName(It.Value());
        if (!Module || !Module->ScriptModule || Module->CoreGenerationId != It.Key()) It.RemoveCurrent();
    }
    return !Owner.SourceGenerations.IsEmpty();
}
ERemoveResult CloseModuleCacheClient(FDelegateHandle Client)
{
    if (!IsInGameThread() || bInCacheClosingCallback) return ERemoveResult::Busy;
    auto* Owner = CacheClients().Find(Client);
    if (!Owner) return ERemoveResult::Unknown;
    Owner->bClosing = true;
    if (!Owner->bNotifiedClosing)
    {
        Owner->bNotifiedClosing = true;
        TGuardValue<bool> Guard(bInCacheClosingCallback, true);
        if (Owner->OnClosing) Owner->OnClosing();
    }
    if (Owner->Lease || !Owner->GenerationLeases.IsEmpty() || !Owner->Sets.IsEmpty() || HasActiveSourceGenerations(*Owner))
    {
        if (!Owner->bWarnedClosing)
        {
            Owner->bWarnedClosing = true;
            UE_LOG(Angelscript, Warning, TEXT("ASR-CACHE-CLIENT-CLOSING-BUSY: %s retains active module holds; process exit will preserve them."), *Owner->FeatureId.ToString());
        }
        return ERemoveResult::Busy;
    }
    return ERemoveResult::Removed;
}
ERemoveResult UnregisterModuleCacheClient(FDelegateHandle Client)
{
    if (!IsInGameThread() || bInCacheClosingCallback || bInCacheAction) return ERemoveResult::Busy;
    auto* Owner = CacheClients().Find(Client);
    if (!Owner) return ERemoveResult::Unknown;
    if (Owner->Lease || !Owner->GenerationLeases.IsEmpty() || !Owner->Sets.IsEmpty() || HasActiveSourceGenerations(*Owner) || Private::IsNativeCompileActionActive()
        || (StartupCompileRecipe && StartupCompileRecipe->IsCapturingOrStopping())) return ERemoveResult::Busy;
    // A drained capture-only owner has no VM resources to mutate. This also
    // permits its final callback teardown after the execution gate has closed.
    if (!Owner->bRetainCompileSurface && !Owner->StructLiveToken)
    {
        if (Owner->bCacheClient) Private::ModuleCacheClientCount.fetch_sub(1, std::memory_order_release);
        CacheClients().Remove(Client);
        return ERemoveResult::Removed;
    }
    FAngelscriptExecutionWriteScope RevokeWriter(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!RevokeWriter.IsAcquired()) return ERemoveResult::Busy;
    const bool bTransient = Owner->bRetainCompileSurface;
    if (bTransient)
    {
        uint32 Remaining = 0;
        for (const auto& Entry : CacheClients()) if (Entry.Value.bRetainCompileSurface && Entry.Key != Client) ++Remaining;
        if (!Remaining)
        {
            StartupCompileRecipe.Reset();
            FCoreDelegates::OnPreExit.Remove(CompilePreExit);
            CompilePreExit.Reset();
        }
        if (!Private::SetTransientNativeClientCount(Remaining)) return ERemoveResult::Busy;
    }
    if (Owner->StructLiveToken) DisableAngelscriptStructLiveValues(Owner->StructLiveToken);
    if (Owner->bCacheClient) Private::ModuleCacheClientCount.fetch_sub(1, std::memory_order_release);
    CacheClients().Remove(Client);
    return ERemoveResult::Removed;
}
ECacheAdmission ProduceModuleCacheSet(asIScriptEngine* ProducerEngine,
    TConstArrayView<TSharedRef<FAngelscriptModuleDesc>> Modules,
    TSharedPtr<const FProducedModuleCacheSet>& Output, FString& Error)
{
    Output.Reset();
    Error.Reset();
    if (!IsInGameThread() || IsGarbageCollecting() || IsAngelscriptExecutionShutdownStarted()
        || bInCacheAction || Private::IsNativeCompileActionActive()) return ECacheAdmission::Busy;
    if (!ProducerEngine || Modules.IsEmpty()) return ECacheAdmission::Invalid;
    auto* Engine = static_cast<asCScriptEngine*>(ProducerEngine);
    if (Engine->isBuilding || Engine->shuttingDown) return ECacheAdmission::Busy;
    FAngelscriptExecutionWriteScope Writer(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Writer.IsAcquired()) return ECacheAdmission::Busy;
    for (const auto& Module : Modules)
        if (Private::IsModuleCacheOwned(Module->ScriptModule))
        {
            Error = TEXT("Cache producer requires source modules; cache-owned modules cannot be serialized again");
            return ECacheAdmission::Invalid;
        }
    return FPreparedModuleCacheSet::Produce(ProducerEngine, Modules, Output, Error)
        ? ECacheAdmission::Completed : ECacheAdmission::Invalid;
}
#if WITH_DEV_AUTOMATION_TESTS
ECacheAdmission SealModuleCacheNativeTypeLayouts(asIScriptEngine* ProducerEngine,
    TConstArrayView<FVerifiedCacheFileView> Files, FString& NativeTypeLayoutsJson, FString& Error)
{
    if (!IsInGameThread() || IsGarbageCollecting() || IsAngelscriptExecutionShutdownStarted()) return ECacheAdmission::Busy;
    if (!ProducerEngine || Files.IsEmpty()) return ECacheAdmission::Invalid;
    auto* Engine = static_cast<asCScriptEngine*>(ProducerEngine);
    if (Engine->isBuilding || Engine->shuttingDown) return ECacheAdmission::Busy;
    FAngelscriptExecutionWriteScope Writer(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Writer.IsAcquired()) return ECacheAdmission::Busy;
    // VM lookup and private arenas are owner-thread-only; no worker callbacks.
    return FPreparedModuleCacheSet::SealNativeLayouts(ProducerEngine, Files, NativeTypeLayoutsJson, Error)
        ? ECacheAdmission::Completed : ECacheAdmission::Invalid;
}
#endif
ECacheAdmission TryLoadModuleCacheSet(FDelegateHandle Client, const FVerifiedCacheSetView& Set, FCacheLoadResult& Result)
{
    if (!IsInGameThread()) return ECacheAdmission::Busy;
    auto* Owner = CacheClients().Find(Client);
    if (!Owner) return ECacheAdmission::Invalid;
    if (!FAngelscriptManager::IsInitialized()) return ECacheAdmission::Busy;
    auto& Manager = FAngelscriptManager::Get();
    if (Owner->bClosing || Owner->Lease || bInCacheAction || Private::IsNativeCompileActionActive()
        || Manager.bIsCompilingModules || IsGarbageCollecting() || IsAngelscriptExecutionShutdownStarted()) return ECacheAdmission::Busy;
    auto Writer = MakeUnique<FAngelscriptExecutionWriteScope>(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Writer->IsAcquired()) return ECacheAdmission::Busy;
    Result = {};
    if (!StartupCompileRecipe || !Owner->bRetainCompileSurface)
    {
        Result.Error = TEXT("Cache loading requires a client registered before Runtime initialization");
        return ECacheAdmission::Completed;
    }
    auto Lease = FCompileSurfaceLease::Acquire(*StartupCompileRecipe, Result.Error);
    if (!Lease) return ECacheAdmission::Completed;
    uint64 InputBytes = uint64(Set.Manifest.Num());
    for (const auto& File : Set.Files) InputBytes += uint64(File.Bytes.Num());
    if (!Lease->ReserveInput(InputBytes, Result.Error, false)) return ECacheAdmission::Completed;
    Owner->Lease = MoveTemp(Lease);
    Owner->Writer = MoveTemp(Writer);
    Owner->bCacheApplying = true;
    TGuardValue<bool> Action(bInCacheAction, true);
    ON_SCOPE_EXIT
    {
        Owner->bCacheApplying = false;
        Owner->Lease.Reset();
        Owner->Writer.Reset();
    };
    auto Candidate = MakeUnique<FCacheSetOwner>();
    auto* Engine = static_cast<asCScriptEngine*>(Manager.GetScriptEngine());
    Candidate->NativeEpoch = Private::GetNativeRegistryEpoch(Engine);
    if (!FPreparedModuleCacheSet::DecodeVerified(Engine, Set, Candidate->Members, Result.Error)
        || !FPreparedModuleCacheSet::ValidateCollection(Candidate->Members, Result.Error)) return ECacheAdmission::Completed;
    const int32 OriginalNames = Manager.StaticNames.Num();
    bool bCommitted = false;
    ON_SCOPE_EXIT { if (!bCommitted) Manager.StaticNames.SetNum(OriginalNames); };
    for (const auto& Member : Candidate->Members)
    {
        auto& Data = *Member->Reader;
        for (int32 I = Manager.StaticNames.Num(); I < Data.StaticNames.Num(); ++I)
            Manager.StaticNames.Add(FName(*Data.StaticNames[I].UnrealString()));
        auto Modules = Data.GetModulesToCompile();
        auto Module = Modules[0];
        Module->ModuleCacheContext = &Data;
        Module->CoreCacheSetGuid = Set.DataGuid;
        Module->CoreCacheFileIdentity = Member->VerifiedContentIdentity;
        Module->CoreCacheRelativePath = Member->VerifiedRelativePath;
        Candidate->Modules.Add(Module);
    }
    FPreparedModuleCacheSet::BeginApplication(Candidate->Members);
    ON_SCOPE_EXIT { FPreparedModuleCacheSet::EndApplication(); };
    TArray<TSharedRef<FAngelscriptModuleDesc>> Compiled;
    const auto Status = Manager.CompileModules(ECompileType::FullReload, Candidate->Modules, Compiled);
    bool bAllPublished = true;
    for (const auto& Module : Candidate->Modules)
        bAllPublished &= Manager.GetModuleByModuleName(Module->ModuleName) == Module && Module->ScriptModule;
    FString EpochError;
    const bool bEpochValid = Candidate->NativeEpoch == Private::GetNativeRegistryEpoch(Engine)
        && StartupCompileRecipe && StartupCompileRecipe->IsReady(EpochError) && Private::IsNativeCompileActionUsable(Engine);
    if (Status != ECompileResult::FullyHandled || !bAllPublished || !bEpochValid)
    {
        Result.Error = TEXT("Cache compilation or publication failed");
        if (bAllPublished)
        {
            FString RollbackError;
            if (!Manager.RemoveModuleCacheModules(Candidate->Modules, RollbackError))
            {
                // Do not erase evidence of a real publication or free its backing.
                Result.Error += TEXT("; publication rollback refused: ") + RollbackError;
                Result.bVmCommitted = true;
                for (const auto& Module : Candidate->Modules) Result.CommittedModules.Add({Module->ModuleName, Module->CoreGenerationId});
                Result.Set.Id = ++NextCacheSet;
                Owner->Sets.Add(Result.Set.Id, MoveTemp(Candidate));
                bCommitted = true;
            }
        }
        return ECacheAdmission::Completed;
    }
    Result.Set.Id = ++NextCacheSet;
    for (const auto& Module : Candidate->Modules)
    {
        Result.CommittedModules.Add({Module->ModuleName, Module->CoreGenerationId});
        // Runtime functions/types own their remapped bytecode, strings and
        // declarations. No published descriptor borrows a reader's recipe.
        Module->ModuleCacheContext = nullptr;
        Module->PrecompiledData = nullptr;
    }
    bCommitted = true;
    Result.bSucceeded = Result.bVmCommitted = true;
    Owner->Sets.Add(Result.Set.Id, MoveTemp(Candidate));
    return ECacheAdmission::Completed;
}
static ECacheAdmission PreflightModuleRemoval(FAngelscriptManager& Manager,
    const TArray<TSharedRef<FAngelscriptModuleDesc>>& Modules)
{
    Manager.ReleaseModuleCacheIdleContexts(Modules);
    TSet<asCModule*> Scripts;
    for (const auto& Module : Modules)
    {
        if (Manager.GetModuleByModuleName(Module->ModuleName) != Module || !Module->ScriptModule
            || Module->CoreGenerationId != Private::GetModuleGeneration(Module->ScriptModule)) return ECacheAdmission::Invalid;
        if (AngelscriptRetiredModules::HasModuleHolds(Module->ScriptModule)
            || Module->ScriptModule->HasExternalReferences(false)) return ECacheAdmission::Busy;
        Scripts.Add(Module->ScriptModule);
        for (asUINT I = 0; I < Module->ScriptModule->classTypes.GetLength(); ++I)
        {
            auto* Type = Module->ScriptModule->classTypes[I];
            if (Type->flags & asOBJ_VALUE)
            {
                int32 Values = -1;
                if (!QueryAngelscriptStructLiveValues(Type, Values) || Values) return ECacheAdmission::Busy;
            }
        }
        for (const auto& Class : Module->Classes)
            if (Class->Class) for (TObjectIterator<UObject> Object; Object; ++Object)
                if ((*Object)->GetClass()->IsChildOf(Class->Class) && !(*Object)->HasAnyFlags(RF_ClassDefaultObject)) return ECacheAdmission::Busy;
    }
    auto* Engine = static_cast<asCScriptEngine*>(Manager.GetScriptEngine());
    for (asUINT I = 0; I < Engine->scriptModules.GetLength(); ++I)
    {
        auto* Other = Engine->scriptModules[I];
        if (!Other || Scripts.Contains(Other)) continue;
        for (const auto& Dependency : Other->moduleDependencies)
            if (Scripts.Contains(Dependency.Key)) return ECacheAdmission::Busy;
    }
    return ECacheAdmission::Completed;
}
ECacheAdmission TryRemoveModuleCacheSet(FDelegateHandle Client, FModuleCacheSetHandle Set, FCacheRemoveResult& Result)
{
    if (!IsInGameThread()) return ECacheAdmission::Busy;
    auto* Owner = CacheClients().Find(Client);
    auto* Found = Owner ? Owner->Sets.Find(Set.Id) : nullptr;
    if (!Found || !Set.Id) return ECacheAdmission::Invalid;
    auto& Manager = FAngelscriptManager::Get();
    if (Owner->Lease || bInCacheAction || Private::IsNativeCompileActionActive()
        || Manager.bIsCompilingModules || IsGarbageCollecting() || IsAngelscriptExecutionShutdownStarted()) return ECacheAdmission::Busy;
    FAngelscriptExecutionWriteScope Writer(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Writer.IsAcquired()) return ECacheAdmission::Busy;
    TGuardValue<bool> Action(bInCacheAction, true);
    FString ActionError;
    auto NativeAction = StartupCompileRecipe ? FCompileSurfaceLease::Acquire(*StartupCompileRecipe, ActionError) : nullptr;
    if (!NativeAction)
    {
        Result = {};
        Result.Error = ActionError.IsEmpty() ? TEXT("Cache removal declaration surface is unavailable") : ActionError;
        return ECacheAdmission::Completed;
    }
    auto& Loaded = **Found;
    const auto Preflight = PreflightModuleRemoval(Manager, Loaded.Modules);
    if (Preflight != ECacheAdmission::Completed) return Preflight;
    Result = {};
    if (!Manager.RemoveModuleCacheModules(Loaded.Modules, Result.Error)) return ECacheAdmission::Completed;
    for (const auto& Module : Loaded.Modules) Result.RemovedModules.Add({Module->ModuleName, Module->CoreGenerationId});
    Owner->Sets.Remove(Set.Id);
    Result.bSucceeded = true;
    return ECacheAdmission::Completed;
}
                                                                                                     
                                                                                                          
ECacheAdmission TryRemoveSourceGenerations(FDelegateHandle Client,
    TConstArrayView<FModuleGenerationId> ExactGenerations, FSourceRemoveResult& Result)
{
    Result = {};
    if (!IsInGameThread()) return ECacheAdmission::Busy;
    auto* Owner = CacheClients().Find(Client);
    if (!Owner || !Owner->bTransient || ExactGenerations.IsEmpty()) return ECacheAdmission::Invalid;
    if (!FAngelscriptManager::IsInitialized()) return ECacheAdmission::Busy;
    auto& Manager = FAngelscriptManager::Get();
    TArray<TSharedRef<FAngelscriptModuleDesc>> Modules;
    TSet<uint64> Seen;
    for (const auto& Generation : ExactGenerations)
    {
        const auto* Name = Owner->SourceGenerations.Find(Generation.Generation);
        const auto Module = Manager.GetModuleByModuleName(Generation.ModuleName);
        if (!Name || *Name != Generation.ModuleName || Seen.Contains(Generation.Generation)
            || !Module || !Module->ScriptModule || Module->CoreGenerationId != Generation.Generation
            || Private::GetModuleGeneration(Module->ScriptModule) != Generation.Generation)
            return ECacheAdmission::Invalid;
        Seen.Add(Generation.Generation);
        Modules.Add(Module.ToSharedRef());
    }
    if (Owner->Lease || bInCacheAction || Private::IsNativeCompileActionActive()
        || Manager.bIsCompilingModules || IsGarbageCollecting() || IsAngelscriptExecutionShutdownStarted())
        return ECacheAdmission::Busy;
    FAngelscriptExecutionWriteScope Writer(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Writer.IsAcquired()) return ECacheAdmission::Busy;
    TGuardValue<bool> Action(bInCacheAction, true);
    auto NativeAction = FCompileSurfaceLease::AcquireForRemoval(Manager.GetScriptEngine(), Result.Error);
    if (!NativeAction) return ECacheAdmission::Invalid;
    const auto Preflight = PreflightModuleRemoval(Manager, Modules);
    if (Preflight != ECacheAdmission::Completed) return Preflight;
    bool bBusy = false;
    if (!Manager.RemoveModuleCacheModules(Modules, Result.Error, bBusy))
        return bBusy ? ECacheAdmission::Busy : ECacheAdmission::Completed;
    for (const auto& Generation : ExactGenerations)
    {
        Result.RemovedGenerations.Add(Generation);
        Owner->SourceGenerations.Remove(Generation.Generation);
    }
    Result.bSucceeded = Result.bVmCommitted = true;
    return ECacheAdmission::Completed;
}
                                                                                                        
                                                                                                       
ECacheAdmission AcquireModuleGenerationLease(FDelegateHandle Client, const UScriptStruct* CaptureType,
    FModuleGenerationLease& Lease)
{
    Lease = {};
    if (!IsInGameThread()) return ECacheAdmission::Busy;
    auto* Owner = CacheClients().Find(Client);
    const auto* Struct = Cast<UASStruct>(CaptureType);
    auto* Type = Struct ? Struct->ScriptType : nullptr;
    if (!Owner || !Type) return ECacheAdmission::Invalid;
    if (Owner->bClosing || IsAngelscriptExecutionShutdownStarted() || IsGarbageCollecting()) return ECacheAdmission::Busy;
    if (!FAngelscriptManager::IsInitialized()) return ECacheAdmission::Busy;
    auto* Engine = static_cast<asCScriptEngine*>(FAngelscriptManager::Get().GetScriptEngine());
    auto* Module = static_cast<asCModule*>(Type->GetModule());
    if (Type->GetEngine() != Engine || !Module || Module->discarded
        || !Engine->scriptModules.Exists(Module)) return ECacheAdmission::Invalid;
    if (!AngelscriptRetiredModules::RetainCaptureModule(Module)) return ECacheAdmission::Busy;
    Lease.Id = ++NextGenerationLease;
    Lease.Module = {ANSI_TO_TCHAR(Module->baseModuleName.GetLength() ? Module->baseModuleName.AddressOf() : Module->GetName()),
        Private::GetModuleGeneration(Module)};
    Owner->GenerationLeases.Add(Lease.Id, Module);
    return ECacheAdmission::Completed;
}
ECacheAdmission CloneModuleGenerationLease(FDelegateHandle Client,
    const FModuleGenerationLease& Existing, FModuleGenerationLease& Lease)
{
    // Preserve an input handle when callers accidentally alias the output.
    const auto Source = Existing;
    Lease = {};
    if (!IsInGameThread()) return ECacheAdmission::Busy;
    auto* Owner = CacheClients().Find(Client);
    auto* Module = Owner ? Owner->GenerationLeases.Find(Source.Id) : nullptr;
    if (!Module || Private::GetModuleGeneration(static_cast<asCModule*>(*Module)) != Source.Module.Generation)
        return ECacheAdmission::Invalid;
    auto* Script = static_cast<asCModule*>(*Module);
    if (Source.Module.ModuleName != ANSI_TO_TCHAR(Script->baseModuleName.GetLength() ? Script->baseModuleName.AddressOf() : Script->GetName()))
        return ECacheAdmission::Invalid;
    if (Owner->bClosing || IsAngelscriptExecutionShutdownStarted() || IsGarbageCollecting()) return ECacheAdmission::Busy;
    if (!AngelscriptRetiredModules::RetainCaptureModule(*Module)) return ECacheAdmission::Busy;
    Lease = Source;
    Lease.Id = ++NextGenerationLease;
    auto* HeldModule = *Module;
    Owner->GenerationLeases.Add(Lease.Id, HeldModule);
    return ECacheAdmission::Completed;
}
ERemoveResult ReleaseModuleGenerationLease(FDelegateHandle Client, const FModuleGenerationLease& Lease)
{
    if (!IsInGameThread()) return ERemoveResult::Busy;
    auto* Owner = CacheClients().Find(Client);
    auto* Module = Owner ? Owner->GenerationLeases.Find(Lease.Id) : nullptr;
    if (!Module || Private::GetModuleGeneration(static_cast<asCModule*>(*Module)) != Lease.Module.Generation)
        return ERemoveResult::Unknown;
    auto* Script = static_cast<asCModule*>(*Module);
    if (Lease.Module.ModuleName != ANSI_TO_TCHAR(Script->baseModuleName.GetLength() ? Script->baseModuleName.AddressOf() : Script->GetName()))
        return ERemoveResult::Unknown;
    if (IsAngelscriptExecutionShutdownStarted()) return ERemoveResult::Busy;
    AngelscriptRetiredModules::ReleaseCaptureModule(*Module);
    Owner->GenerationLeases.Remove(Lease.Id);
    return ERemoveResult::Removed;
}
}
                                                                                                     

                                                                                                          
namespace AngelscriptCoreExtensionPoints
{
FDelegateHandle RegisterTransientCompileClient(FName FeatureId, TFunction<void()> OnClosing)
{
    if (!IsInGameThread() || bCompileCaptureStarted || FAngelscriptManager::IsInitialized()) return {};
    auto Handle = RegisterModuleCacheClient(FeatureId, MoveTemp(OnClosing));
    if (!Handle.IsValid()) return {};
    CacheClients().FindChecked(Handle).bTransient = true;
    return Handle;
}
ERemoveResult UnregisterTransientCompileClient(FDelegateHandle Client)
{
    if (!IsInGameThread()) return ERemoveResult::Busy;
    const auto* Owner = CacheClients().Find(Client);
    if (!Owner || !Owner->bTransient) return ERemoveResult::Unknown;
    return UnregisterModuleCacheClient(Client);
}
ECompileSurfaceAdmission AcquireTransientCompileSurface(FDelegateHandle Client,
    FCompileSurfaceHandle& Surface, FString& Error)
{
    Surface = {}; Error.Reset();
    if (!IsInGameThread()) return ECompileSurfaceAdmission::Busy;
    auto* Owner = CacheClients().Find(Client);
    if (!Owner || !Owner->bTransient) return ECompileSurfaceAdmission::Invalid;
    if (Owner->bClosing || bInCacheAction || Owner->Lease || Private::IsNativeCompileActionActive() || IsGarbageCollecting()
        || IsAngelscriptExecutionShutdownStarted()) return ECompileSurfaceAdmission::Busy;
    if (!StartupCompileRecipe)
    { Error = StartupCompileError.IsEmpty() ? TEXT("Compile surface was not captured during initialization") : StartupCompileError; return ECompileSurfaceAdmission::Unavailable; }
    auto Writer = MakeUnique<FAngelscriptExecutionWriteScope>(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Writer->IsAcquired()) return ECompileSurfaceAdmission::Busy;
    auto Lease = FCompileSurfaceLease::Acquire(*StartupCompileRecipe, Error);
    if (!Lease) return ECompileSurfaceAdmission::Unavailable;
    Owner->Writer = MoveTemp(Writer);
    Owner->Lease = MoveTemp(Lease);
    Owner->Action = ++NextCompileAction;
    Surface.Id = Owner->Action;
    return ECompileSurfaceAdmission::Ready;
}
ERemoveResult ReleaseTransientCompileSurface(FDelegateHandle Client, FCompileSurfaceHandle Surface)
{
    if (!IsInGameThread()) return ERemoveResult::Busy;
    auto* Owner = CacheClients().Find(Client);
    if (!Owner || !Surface.Id || Owner->Action != Surface.Id) return ERemoveResult::Unknown;
    if (Owner->bCacheApplying || static_cast<asCScriptEngine*>(StartupCompileEngine)->isBuilding
        || FAngelscriptManager::Get().bIsCompilingModules) return ERemoveResult::Busy;
    for (const auto& Weak : Owner->PendingSourceModules)
        if (const auto Candidate = Weak.Pin())
            if (FAngelscriptManager::Get().GetModuleByModuleName(Candidate->ModuleName) == Candidate
                && Candidate->ScriptModule && Candidate->CoreGenerationId)
                Owner->SourceGenerations.Add(Candidate->CoreGenerationId, Candidate->ModuleName);
    Owner->PendingSourceModules.Reset();
    Owner->Lease.Reset();
    Owner->Writer.Reset();
    Owner->Action = 0;
    return ERemoveResult::Removed;
}
#if WITH_DEV_AUTOMATION_TESTS
ECompileSurfaceAdmission CompileTransientModule(FDelegateHandle Client, FCompileSurfaceHandle Surface,
    FStringView ModuleName, TConstArrayView<FTransientScriptSection> Sections, FModuleGenerationId& Module, FString& Error)
{
    Module = {}; Error.Reset();
    if (!IsInGameThread()) return ECompileSurfaceAdmission::Busy;
    auto* Owner = CacheClients().Find(Client);
    if (!Owner || !Owner->Lease || Owner->bCacheApplying || !Surface.Id || Owner->Action != Surface.Id
        || ModuleName.IsEmpty() || Sections.IsEmpty()) return ECompileSurfaceAdmission::Invalid;
    auto* Engine = static_cast<asCScriptEngine*>(StartupCompileEngine);
    if (Owner->bClosing || Engine->isBuilding || FAngelscriptManager::Get().bIsCompilingModules
        || IsGarbageCollecting() || IsAngelscriptExecutionShutdownStarted()) return ECompileSurfaceAdmission::Busy;
    if (!StartupCompileRecipe || !StartupCompileRecipe->IsReady(Error)) return ECompileSurfaceAdmission::Unavailable;
    const FString Name(ModuleName);
    FTCHARToUTF8 Utf8Name(*Name);
    if (Engine->GetModule(Utf8Name.Get(), asGM_ONLY_IF_EXISTS))
    { Error = TEXT("Transient compile cannot replace an existing module"); return ECompileSurfaceAdmission::Invalid; }
    // Use the same staged compiler as ordinary Runtime source compilation.
    // Low-level asCModule::Build additionally gates on the VM's legacy configFailed
    // latch, which the production Manager pipeline does not use.
    uint64 InputBytes = sizeof(FAngelscriptModuleDesc) + uint64(Name.Len() + 1) * sizeof(TCHAR)
        + uint64(Sections.Num()) * sizeof(FAngelscriptModuleDesc::FCodeSection);
    for (const auto& Section : Sections)
        InputBytes += uint64(Section.Code.Len() + 2 * Section.Name.Len() + 3) * sizeof(TCHAR);
    if (!Owner->Lease->ReserveInput(InputBytes, Error, false)) return ECompileSurfaceAdmission::Unavailable;
    auto Candidate = MakeShared<FAngelscriptModuleDesc>();
    Candidate->ModuleName = Name;
    for (const auto& Section : Sections)
    {
        auto& Code = Candidate->Code.AddDefaulted_GetRef();
        Code.RelativeFilename = Section.Name;
        Code.AbsoluteFilename = Section.Name;
        Code.Code = Section.Code;
    }
    TArray<TSharedRef<FAngelscriptModuleDesc>> Input {Candidate}, Compiled;
    if (FAngelscriptManager::Get().CompileModules(ECompileType::FullReload, Input, Compiled) != ECompileResult::FullyHandled
        || !Candidate->ScriptModule || !Private::IsNativeCompileActionUsable(Engine)
        || !StartupCompileRecipe->IsReady(Error))
    { Error = TEXT("Transient script compilation failed"); return ECompileSurfaceAdmission::Invalid; }
    Module.ModuleName = Name;
    Module.Generation = Private::GetModuleGeneration(Candidate->ScriptModule);
    return ECompileSurfaceAdmission::Ready;
}
#endif
namespace Private
{
void CloseModuleCacheClients()
{
    if (bClosingCacheClients) return;
    bClosingCacheClients = true;
    TArray<FDelegateHandle> Handles;
    CacheClients().GetKeys(Handles);
    for (auto Handle : Handles) CloseModuleCacheClient(Handle);
}
bool HasModuleGenerationLeases()
{
    for (const auto& Entry : CacheClients()) if (!Entry.Value.GenerationLeases.IsEmpty()) return true;
    return false;
}
bool ValidateTransientManagerModules(const TArray<TSharedRef<FAngelscriptModuleDesc>>& Modules, bool bRequireOwnedExisting)
{
    FCacheClientOwner* Active = nullptr;
    for (auto& Entry : CacheClients()) if (Entry.Value.Lease) Active = &Entry.Value;
    if (!Active || Active->bClosing || Modules.IsEmpty() || !StartupCompileRecipe
        || !IsNativeCompileActionUsable(static_cast<asCScriptEngine*>(StartupCompileEngine))) return false;
    FString Error;
    if (!StartupCompileRecipe->IsReady(Error)) return false;
    auto& Manager = FAngelscriptManager::Get();
    if (Manager.bIsCompilingModules || static_cast<asCScriptEngine*>(StartupCompileEngine)->isBuilding
        || IsGarbageCollecting() || IsAngelscriptExecutionShutdownStarted()) return false;
    TSet<FString> Names;
    for (const auto& Module : Modules)
    {
        if (Active->bCacheApplying && Module->ModuleCacheContext && FPreparedModuleCacheSet::IsApplying(Module->ModuleCacheContext))
        {
            if (bRequireOwnedExisting || Module->ScriptModule || Manager.GetModuleByModuleName(Module->ModuleName)) return false;
            continue;
        }
        if (Module->ModuleName.IsEmpty() || Module->ScriptModule || Module->PrecompiledData
            || Module->ModuleCacheContext || Names.Contains(Module->ModuleName)) return false;
        const auto Existing = Manager.GetModuleByModuleName(Module->ModuleName);
        auto* ExistingScript = StartupCompileEngine->GetModule(TCHAR_TO_UTF8(*Module->ModuleName), asGM_ONLY_IF_EXISTS);
        if (Existing)
        {
            // A scoped source owner may reload its own live generation while a
            // different cache set remains loaded. Cache-owned modules stay immutable.
            const FString* OwnedName = Active->SourceGenerations.Find(Existing->CoreGenerationId);
            const bool bSameOwnedGeneration = OwnedName && *OwnedName == Module->ModuleName;
            const bool bSameScope = Existing->CoreTypeScope && Module->CoreTypeScope
                && Existing->CoreTypeScope->Namespace == Module->CoreTypeScope->Namespace;
            if (!Existing->ScriptModule || Existing->ScriptModule != ExistingScript
                || IsModuleCacheOwned(Existing->ScriptModule) || (!bSameScope && !bSameOwnedGeneration)
                || (Existing->CoreTypeScope && Module->CoreTypeScope && !bSameScope)
                || (bRequireOwnedExisting && !bSameOwnedGeneration)) return false;
        }
        else if (ExistingScript || bRequireOwnedExisting) return false;
        Names.Add(Module->ModuleName);
        for (const auto& Class : Module->Classes)
        {
            TSharedPtr<FAngelscriptModuleDesc> ClassOwner;
            if (Manager.GetClass(Class->GetTypeIdentity(), &ClassOwner) && ClassOwner != Existing) return false;
        }
    }
    uint64 InputBytes = 0;
    for (const auto& Module : Modules)
    {
        InputBytes += sizeof(FAngelscriptModuleDesc) + Module->ModuleName.GetAllocatedSize() + Module->Code.GetAllocatedSize();
        for (const auto& Section : Module->Code)
            InputBytes += Section.Code.GetAllocatedSize() + Section.RelativeFilename.GetAllocatedSize() + Section.AbsoluteFilename.GetAllocatedSize();
    }
    if (!Active->Lease->ReserveInput(InputBytes, Error)) return false;
    if (!Active->bCacheApplying)
        for (const auto& Module : Modules) Active->PendingSourceModules.Add(Module);
    Active->CompileUsedPhysicalBefore = FPlatformMemory::GetStats().UsedPhysical;
    return true;
}
bool FinishTransientManagerCompile()
{
    for (auto& Entry : CacheClients()) if (Entry.Value.Lease)
    {
        auto& Active = Entry.Value;
        const uint64 After = FPlatformMemory::GetStats().UsedPhysical;
        FString Error;
        const bool bWithinBudget = StartupCompileRecipe && StartupCompileRecipe->IsReady(Error)
            && Active.Lease->ReserveInput(0, Error, false);
        UE_LOG(Angelscript, Log, TEXT("H6 compile memory: actionBytes=%llu retainedCompileSurfaceBytes=%llu usedPhysicalDelta=%lld processWideApproximation=1 limit=%llu"),
            Active.Lease->GetReservedBytes(), static_cast<asCScriptEngine*>(StartupCompileEngine)->RetainedCompileSurfaceBytes,
            int64(After) - int64(Active.CompileUsedPhysicalBefore), 128ull * 1024 * 1024);
        return bWithinBudget;
    }
    return true;
}
TUniquePtr<FNativeCompileCoreCallGuard> EnterTransientManagerCompileCall(asCScriptEngine* Engine)
{
    if (Engine != StartupCompileEngine || !IsNativeCompileActionActive()) return nullptr;
    for (auto& Entry : CacheClients()) if (Entry.Value.Lease) return Entry.Value.Lease->EnterManagerCall();
    return nullptr;
}
#if WITH_DEV_AUTOMATION_TESTS
void GetTransientCompileCapacity(uint64& RecipeBytes, uint64& ActionBytes)
{
    RecipeBytes = StartupCompileRecipe ? StartupCompileRecipe->GetAllocatedBytes() : 0;
    ActionBytes = 0;
    for (auto& Entry : CacheClients()) if (Entry.Value.Lease) ActionBytes = Entry.Value.Lease->GetReservedBytes();
}
#endif
void BeginTransientCompileCapture(asIScriptEngine* Engine)
{
    bCompileCaptureStarted = true;
    StartupCompileEngine = Engine;
    bool bHasClient = false;
    for (const auto& Entry : CacheClients()) bHasClient |= Entry.Value.bRetainCompileSurface;
    if (!bHasClient) return;
    // Subscribe before the recipe so active actions release before owner roots.
    CompilePreExit = FCoreDelegates::OnPreExit.AddLambda([]
    {
        for (auto& Entry : CacheClients())
        {
            Entry.Value.Lease.Reset();
            Entry.Value.Writer.Reset();
            Entry.Value.Action = 0;
        }
        StartupCompileRecipe.Reset();
        Private::SetTransientNativeClientCount(0);
    });
    StartupCompileRecipe = FCompileSurfaceRecipe::Begin(Engine, CompileRegistrationEpoch, StartupCompileError);
}
void SealTransientCompileBindings()
{
    if (StartupCompileRecipe && !StartupCompileRecipe->CaptureBindings(StartupCompileError)) StartupCompileRecipe.Reset();
}
void SealTransientCompileRuntime(FAngelscriptManager* Manager)
{
    if (StartupCompileRecipe && !StartupCompileRecipe->CaptureRuntime(Manager, StartupCompileError)) StartupCompileRecipe.Reset();
}
uint64 BeginGameplayTagRegistration(asIScriptEngine* Engine)
{
    if (!IsInGameThread() || !StartupCompileRecipe || Engine != StartupCompileEngine) return 0;
    return GetNativeRegistryEpoch(static_cast<asCScriptEngine*>(Engine));
}
void FinishGameplayTagRegistration(asIScriptEngine* Engine, uint64 PreviousEpoch, int32 PropertyId)
{
    if (StartupCompileRecipe && Engine == StartupCompileEngine)
        StartupCompileRecipe->AcceptGameplayTagRegistration(Engine, PreviousEpoch, PropertyId);
}
void FinishTransientCompileCapture()
{
    if (StartupCompileRecipe) StartupCompileRecipe->FinishCapture();
}
}
}
                                                                                                        

                                                                                            
namespace AngelscriptCoreExtensionPoints
{
namespace
{
FDelegateHandle TypeScopeHandle;
FTypeScopeProvider TypeScopeProvider;
TArray<TWeakPtr<const FTypeScope>> TypeScopeSnapshots;
bool bInTypeScopeProvider = false;
}
FDelegateHandle RegisterTypeScope(FTypeScopeProvider Provider)
{
    if (!IsInGameThread() || !Provider || TypeScopeProvider || bInTypeScopeProvider
        || (FAngelscriptManager::IsInitialized() && FAngelscriptManager::Get().bIsCompilingModules)) return {};
    TypeScopeHandle = FDelegateHandle(FDelegateHandle::GenerateNewHandle);
    TypeScopeProvider = MoveTemp(Provider);
    return TypeScopeHandle;
}
ERemoveResult UnregisterTypeScope(FDelegateHandle Handle)
{
    if (!IsInGameThread() || bInTypeScopeProvider
        || (FAngelscriptManager::IsInitialized() && FAngelscriptManager::Get().bIsCompilingModules))
        return ERemoveResult::Busy;
    if (!Handle.IsValid() || Handle != TypeScopeHandle) return ERemoveResult::Unknown;
    TypeScopeSnapshots.RemoveAll([](const auto& Entry) { return !Entry.IsValid(); });
    if (!TypeScopeSnapshots.IsEmpty()) return ERemoveResult::Busy;
    TypeScopeProvider = {};
    TypeScopeHandle.Reset();
    return ERemoveResult::Removed;
}
TSharedPtr<const FTypeScope> Private::ResolveTypeScope(FStringView AbsolutePath)
{
    if (!TypeScopeProvider) return {};
    check(IsInGameThread());
    if (bInTypeScopeProvider) return {};
    TGuardValue<bool> Guard(bInTypeScopeProvider, true);
    FTypeScope Value;
    if (!TypeScopeProvider(AbsolutePath, Value) || Value.Namespace.IsEmpty()) return {};
    auto Snapshot = MakeShared<const FTypeScope>(MoveTemp(Value));
    TypeScopeSnapshots.RemoveAll([](const auto& Entry) { return !Entry.IsValid(); });
    TypeScopeSnapshots.Add(Snapshot);
    return Snapshot;
}
void Private::PrepareTypeScope(FAngelscriptModuleDesc& Module)
{
    if (!Module.CoreTypeScope && Module.ModuleCacheContext)
        Module.CoreTypeScope = ResolveTypeScope(FStringView());
    if (!Module.CoreTypeScope) return;
    const FString& Owner = Module.CoreTypeScope->Namespace;
    for (const auto& Class : Module.Classes)
    {
        if (Class->bIsStaticsClass) continue;
        Class->CoreTypeOwner = Owner;
        Class->CoreTypeIdentity = Class->Namespace.Get(Owner) + TEXT("::") + Class->ClassName;
    }
    for (const auto& Delegate : Module.Delegates)
    {
        Delegate->CoreTypeOwner = Owner;
        if (Delegate->CoreTypeIdentity.IsEmpty())
            Delegate->CoreTypeIdentity = Owner + TEXT("::") + Delegate->DelegateName;
    }
}
}
                                                                                          

                                                                                                   
#include "AngelscriptDebugStartPolicyInternal.h"
namespace AngelscriptCoreExtensionPoints
{
namespace Private
{
FDebugStartPolicyState& DebugStartPolicy()
{
    static FDebugStartPolicyState State;
    return State;
}
}
EDebugPolicyRegistration RegisterDebugStartPolicy(TFunction<bool()> DenyStart, FDelegateHandle& OutHandle)
{
    OutHandle.Reset();
    if (!IsInGameThread()) return EDebugPolicyRegistration::Conflict;
    return Private::DebugStartPolicy().Register(MoveTemp(DenyStart), OutHandle);
}
ERemoveResult UnregisterDebugStartPolicy(FDelegateHandle Handle)
{
    if (!IsInGameThread()) return ERemoveResult::Busy;
    return Private::DebugStartPolicy().Remove(Handle);
}
}
                                                                                                 

                                                                                              
#include "AngelscriptCoreAdmissionInternal.h"
#include "AngelscriptType.h"
#include "ClassGenerator/AngelscriptClassState.h"
#include "ClassGenerator/ASStruct.h"
#include "UObject/Class.h"
#include "StartAngelscriptHeaders.h"
#include "as_module.h"
#include "as_typeinfo.h"
#include "as_objecttype.h"
#include "EndAngelscriptHeaders.h"
namespace AngelscriptCoreExtensionPoints
{
FDelegateHandle RegisterDebugAccess(TFunction<bool(const FDebugAccessView&)> Allow)
{
    if (!IsInGameThread()) return {};
    return Private::DebugAccess().Register(MoveTemp(Allow));
}
ERemoveResult UnregisterDebugAccess(FDelegateHandle Handle)
{
    if (!IsInGameThread()) return ERemoveResult::Busy;
    return Private::DebugAccess().Remove(Handle);
}
namespace Private
{
FDebugAccessState& DebugAccess()
{
    static FDebugAccessState State;
    return State;
}
static thread_local FDebugReadScope* DebugRead = nullptr;
FDebugReadScope* CurrentDebugRead() { return DebugRead; }
FDebugReadScope::FDebugReadScope(const FDebugAccessView& InView) : View(InView)
{
    if (DebugAccess().Enabled.load(std::memory_order_acquire))
    {
        { FScopeLock Lock(&DebugAccess().Mutex); ++DebugAccess().Readers; }
        Previous = DebugRead;
        DebugRead = this;
    }
}
FDebugReadScope::~FDebugReadScope()
{
    if (DebugRead == this)
    {
        DebugRead = Previous;
        FScopeLock Lock(&DebugAccess().Mutex);
        --DebugAccess().Readers;
    }
}
bool AllowDebugRead(const FDebugAccessView& View)
{
    if (!DebugAccess().Allow(View)) return false;
    if (DebugRead && !DebugRead->Reads.ContainsByPredicate([&](const FDebugAccessView& Existing)
        { return Existing.Operation == View.Operation && Existing.Phase == View.Phase && Existing.SessionId == View.SessionId
            && Existing.Scope == View.Scope && Existing.ModuleGenerationId.Generation == View.ModuleGenerationId.Generation
            && Existing.ModuleGenerationId.ModuleName == View.ModuleGenerationId.ModuleName && Existing.SourceIdentity == View.SourceIdentity; }))
        DebugRead->Reads.Add(View);
    return true;
}
FDebugAccessView DebugFunctionView(asIScriptFunction* Function, const FDebugAccessView& Request)
{
    FDebugAccessView View = Request;
    View.ModuleGenerationId = {};
    View.SourceIdentity.Reset();
    View.Scope = EDebugAccessScope::Unknown;
    if (!Function) return View;
    auto* Module = static_cast<asCModule*>(Function->GetModule());
    if (!Module)
    {
        if (Function->GetFuncType() == asFUNC_SYSTEM) View.Scope = EDebugAccessScope::Native;
        return View;
    }
    if (Module->discarded) return View;
    View.Scope = EDebugAccessScope::Script;
    View.ModuleGenerationId = { ANSI_TO_TCHAR(Module->GetName()), GetModuleGeneration(Module) };
    const char* Section = Function->GetScriptSectionName();
    if (Section) View.SourceIdentity = ANSI_TO_TCHAR(Section);
    return View;
}
FDebugAccessView DebugTypeView(asITypeInfo* Type, const FDebugAccessView& Request)
{
    FDebugAccessView View = Request;
    View.ModuleGenerationId = {};
    View.SourceIdentity.Reset();
    View.Scope = EDebugAccessScope::Unknown;
    if (!Type) return View;
    auto* Info = static_cast<asCTypeInfo*>(Type);
    auto* Module = static_cast<asCModule*>(Type->GetModule());
    if (!Module)
    {
        View.Scope = EDebugAccessScope::Native;
        return View;
    }
    if (Module->discarded) return View;
    View.Scope = EDebugAccessScope::Script;
    View.ModuleGenerationId = { ANSI_TO_TCHAR(Module->GetName()), GetModuleGeneration(Module) };
    const int Index = Info->scriptSectionIdx;
    if (Index >= 0 && Index < int(Info->engine->scriptSectionNames.GetLength()))
        View.SourceIdentity = ANSI_TO_TCHAR(Info->engine->scriptSectionNames[Index]->AddressOf());
    else if (IsInGameThread())
    {
        auto Desc = FAngelscriptManager::Get().GetModule(Module);
        if (Desc.IsValid() && Desc->ScriptModule == Module && Desc->Code.Num() == 1)
            View.SourceIdentity = Desc->Code[0].AbsoluteFilename;
    }
    return View;
}
bool AllowDebugFunction(asIScriptFunction* Function, bool bExecute)
{
    if (!DebugRead) return true;
    FDebugAccessView View = DebugFunctionView(Function, DebugRead->View);
    if (bExecute) View.Operation = EDebugAccessOperation::Getter;
    if (bExecute) View.Phase = EDebugAccessPhase::Read;
    return AllowDebugRead(View) && (!Function || !Function->GetObjectType() || AllowDebugType(Function->GetObjectType()));
}
bool AllowDebugType(asITypeInfo* Type)
{
    if (!DebugRead) return true;
    if (!Type || !AllowDebugRead(DebugTypeView(Type, DebugRead->View))) return false;
    for (asUINT Index = 0; Index < Type->GetSubTypeCount(); ++Index)
        if (auto* Child = Type->GetSubType(Index))
            if (!AllowDebugType(Child)) return false;
    return true;
}
bool AllowDebugOwner(UStruct* Owner)
{
    if (!DebugRead) return true;
    if (auto* Class = Cast<UClass>(Owner))
        if (auto* Type = GetAngelscriptClassScriptType(Class)) return AllowDebugType(Type);
    if (auto* Struct = Cast<UASStruct>(Owner)) return AllowDebugType(Struct->ScriptType);
    FDebugAccessView View = DebugRead->View;
    View.Scope = Owner && ((Cast<UClass>(Owner) && Cast<UClass>(Owner)->HasAnyClassFlags(CLASS_Native))
        || (Cast<UScriptStruct>(Owner) && (Cast<UScriptStruct>(Owner)->StructFlags & STRUCT_Native))) ? EDebugAccessScope::Native : EDebugAccessScope::Unknown;
    View.ModuleGenerationId = {};
    View.SourceIdentity.Reset();
    return AllowDebugRead(View);
}
bool AllowDebugProperty(asITypeInfo* Type, int PropertyIndex)
{
    if (!DebugRead) return true;
    auto* Owner = CastToObjectType(static_cast<asCTypeInfo*>(Type));
    if (!Owner || PropertyIndex < 0 || PropertyIndex >= int(Owner->properties.GetLength())) return false;
    auto* Property = Owner->properties[PropertyIndex];
    while (Owner->IsPropertyInherited(PropertyIndex))
    {
        Owner = Owner->derivedFrom ? Owner->derivedFrom : CastToObjectType(static_cast<asCTypeInfo*>(Owner->shadowType));
        if (!Owner) return false;
        PropertyIndex = -1;
        for (asUINT Index = 0; Index < Owner->properties.GetLength(); ++Index)
            if (Owner->properties[Index]->byteOffset == Property->byteOffset && Owner->properties[Index]->name == Property->name)
            { PropertyIndex = int(Index); break; }
        if (PropertyIndex < 0) return false;
    }
    return AllowDebugType(Owner);
}
bool AllowDebugModule(asIScriptModule* Module)
{
    if (!DebugRead) return true;
    auto View = DebugRead->View;
    View.ModuleGenerationId = {};
    View.SourceIdentity.Reset();
    if (!Module)
    {
        View.Scope = EDebugAccessScope::Native;
        return AllowDebugRead(View);
    }
    if (!IsInGameThread() || static_cast<asCModule*>(Module)->discarded) return false;
    auto Desc = FAngelscriptManager::Get().GetModule(Module);
    if (!Desc.IsValid() || Desc->Code.IsEmpty()) return false;
    View.Scope = EDebugAccessScope::Script;
    View.ModuleGenerationId = { Desc->ModuleName, GetModuleGeneration(static_cast<asCModule*>(Module)) };
    for (const auto& Section : Desc->Code)
    {
        View.SourceIdentity = Section.AbsoluteFilename;
        if (!AllowDebugRead(View)) return false;
    }
    return true;
}
bool AllowDebugUsage(const FAngelscriptTypeUsage& Usage)
{
    if (!DebugRead) return true;
    if (Usage.IsValid())
    {
        if (Usage.Type == FAngelscriptType::GetScriptObject() || Usage.Type == FAngelscriptType::GetScriptEnum()
            || Usage.Type == FAngelscriptType::GetScriptStruct() || Usage.Type == FAngelscriptType::GetScriptDelegate()
            || Usage.Type == FAngelscriptType::GetScriptMulticastDelegate()) return AllowDebugType(Usage.ScriptClass);
        if (auto* Info = Usage.Type->GetAngelscriptTypeInfo(Usage)) return AllowDebugType(Info);
        const FString Declaration = Usage.GetAngelscriptDeclaration();
        if (auto* Info = FAngelscriptManager::Get().GetScriptEngine()->GetTypeInfoByDecl(TCHAR_TO_ANSI(*Declaration)))
            return AllowDebugType(Info);
    }
    if (Usage.IsValid() && (Usage.Type->GetClass(Usage) || Usage.Type->GetUnrealStruct(Usage)))
    {
        FDebugAccessView View = DebugRead->View;
        View.Scope = EDebugAccessScope::Native;
        View.ModuleGenerationId = {};
        View.SourceIdentity.Reset();
        return AllowDebugRead(View);
    }
    return true;
}
}
}
                                                                                            
