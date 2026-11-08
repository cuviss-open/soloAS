                                                                                                       
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
namespace
{
struct FCacheClientOwner
{
    FName FeatureId;
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
uint64 NextGenerationLease = 0;
bool bClosingCacheClients = false;
bool bInCacheClosingCallback = false;
}
FDelegateHandle RegisterModuleGenerationLeaseClient(FName FeatureId, TFunction<void()> OnClosing)
{
    if (!IsInGameThread() || FeatureId.IsNone() || bClosingCacheClients || bInCacheClosingCallback
        || IsAngelscriptExecutionShutdownStarted()) return {};
    for (const auto& Entry : CacheClients())
        if (Entry.Value.FeatureId == FeatureId) return {};
    FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
    auto& Owner = CacheClients().Add(Handle);
    Owner.FeatureId = FeatureId;
    Owner.OnClosing = MoveTemp(OnClosing);
    return Handle;
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
    if (!Owner->GenerationLeases.IsEmpty())
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
    if (!IsInGameThread() || bInCacheClosingCallback) return ERemoveResult::Busy;
    auto* Owner = CacheClients().Find(Client);
    if (!Owner) return ERemoveResult::Unknown;
    if (!Owner->GenerationLeases.IsEmpty()) return ERemoveResult::Busy;
    CacheClients().Remove(Client);
    return ERemoveResult::Removed;
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
bool Private::HasModuleGenerationLeases()
{
    for (const auto& Entry : CacheClients()) if (!Entry.Value.GenerationLeases.IsEmpty()) return true;
    return false;
}
void Private::CloseModuleCacheClients()
{
    if (bClosingCacheClients) return;
    bClosingCacheClients = true;
    TArray<FDelegateHandle> Handles;
    CacheClients().GetKeys(Handles);
    for (auto Handle : Handles) CloseModuleCacheClient(Handle);
}
}
                                                                                                     
