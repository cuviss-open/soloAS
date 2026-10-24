// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptRetiredModules.h"

#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectArray.h"

#include "StartAngelscriptHeaders.h"
#include "as_module.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

namespace AngelscriptRetiredModules
{
namespace
{
TMap<asCModule*, int32> LiveTypesByModule;
TArray<TSharedRef<FAngelscriptModuleDesc>> DeferredModules;
FDelegateHandle PostGcHandle;
bool bCollecting = false;

TSet<asCModule*> RequiredModules()
{
    TSet<asCModule*> Required;
    TArray<asCModule*> Pending;
    for (const auto& Pair : LiveTypesByModule) Pending.Add(Pair.Key);
    for (const auto& Module : FAngelscriptManager::Get().GetActiveModules())
        if (Module->ScriptModule) Pending.Add(Module->ScriptModule);
    while (!Pending.IsEmpty())
    {
        asCModule* Module = Pending.Pop(EAllowShrinking::No);
        if (Required.Contains(Module)) continue;
        Required.Add(Module);
        for (const auto& Dependency : Module->moduleDependencies) Pending.Add(Dependency.Key);
    }
    return Required;
}

void CollectReleasedModules()
{
    check(IsInGameThread());
    if (DeferredModules.IsEmpty() || bCollecting || IsGarbageCollecting()) return;
    // Full reload may collect after swapping modules but before retiring old
    // classes. Their dependency closure remains in use until the compile ends.
    if (FAngelscriptManager::Get().bIsCompilingModules) return;
    // A GC requested from a script frame must not upgrade its read scope or
    // wait for a reader which may itself be waiting for the game thread.
    FAngelscriptExecutionWriteScope Writer(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Writer.IsAcquired()) return;
    TGuardValue<bool> CollectingGuard(bCollecting, true);
    const TSet<asCModule*> Required = RequiredModules();
    TArray<TSharedRef<FAngelscriptModuleDesc>> Released;
    for (int32 Index = 0; Index < DeferredModules.Num();)
    {
        if (Required.Contains(static_cast<asCModule*>(DeferredModules[Index]->ScriptModule))) { ++Index; continue; }
        Released.Add(DeferredModules[Index]);
        DeferredModules.RemoveAt(Index);
    }
    // Preserve the manager's original discard order and keep all modules
    // intact until their global exit callbacks have completed.
    for (const auto& Module : Released)
    {
        FAngelscriptManager::Get().Engine->DiscardModule(Module->ScriptModule->GetName());
        Module->ScriptModule = nullptr;
    }
    if (!Released.IsEmpty()) FAngelscriptManager::Get().Engine->DeleteDiscardedModules();
}
}

asIScriptModule* RetainType(asITypeInfo* Type)
{
    check(IsInGameThread() && IsAngelscriptExecutionWriterHeldByCurrentThread());
    asCModule* Module = Type ? static_cast<asCModule*>(Type->GetModule()) : nullptr;
    if (!Module) return nullptr;
    ++LiveTypesByModule.FindOrAdd(Module);
    return Module;
}

void ReleaseModule(asIScriptModule* ScriptModule)
{
    check(IsInGameThread() && IsAngelscriptExecutionWriterHeldByCurrentThread());
    asCModule* Module = static_cast<asCModule*>(ScriptModule);
    int32* Count = LiveTypesByModule.Find(Module);
    if (!Count || *Count <= 0) UE_LOG(LogTemp, Fatal, TEXT("ASR-RETIRED-MODULE-LEASE-MISMATCH"));
    if (--*Count == 0) LiveTypesByModule.Remove(Module);
}

bool DeferDiscard(const TSharedRef<FAngelscriptModuleDesc>& Module)
{
    check(IsInGameThread() && IsAngelscriptExecutionWriterHeldByCurrentThread());
    if (!RequiredModules().Contains(static_cast<asCModule*>(Module->ScriptModule))) return false;
    DeferredModules.AddUnique(Module);
    return true;
}

void Startup()
{
    check(IsInGameThread() && !PostGcHandle.IsValid() && LiveTypesByModule.IsEmpty() && DeferredModules.IsEmpty());
    PostGcHandle = FCoreUObjectDelegates::GetPostGarbageCollect().AddStatic(&CollectReleasedModules);
}

void Shutdown()
{
    check(IsInGameThread() && LiveTypesByModule.IsEmpty());
    FCoreUObjectDelegates::GetPostGarbageCollect().Remove(PostGcHandle);
    PostGcHandle.Reset();
    // The engine still owns loaded modules. Do not execute their script
    // global destructors after execution has entered shutdown quiescence.
    DeferredModules.Empty();
}
}
