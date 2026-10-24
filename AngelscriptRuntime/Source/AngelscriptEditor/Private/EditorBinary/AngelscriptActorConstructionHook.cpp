// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "EditorBinary/AngelscriptActorConstructionHook.h"
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "Editor/SoloASEditorEntryHook.h"
#include "ClassGenerator/AngelscriptActorConstruction.h"
#include "ComponentInstanceDataCache.h"
#include "Components/ActorComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "EngineGlobals.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"

namespace
{
FSoloASEditorEntryHook ExecuteEntry, UserScriptEntry, DelegateEntry;
TAutoConsoleVariable<int32> ActorConstructionEnabled(TEXT("soloas.EditorScriptActorConstruction"), 1,
    TEXT("Restore pure AS editor construction cache phases. 0 uses the original engine flow."));
using FExecute = bool (*)(AActor*, const FTransform&, const FRotationConversionCache*,
    const FComponentInstanceDataCache*, bool, ESpawnActorScaleMethod);
using FUserScript = void (*)(AActor*);
using FBind = void (*)(const UClass*, UObject*);
struct FConstruction
{
    AActor* Actor;
    const FTransform& Transform;
    const FRotationConversionCache* Rotation;
    const FComponentInstanceDataCache* Cache;
    bool bDefaultTransform;
    bool bUserScriptFinished = false;
    bool bCacheFinished = false;
};
thread_local FConstruction* ActorConstructionCurrent = nullptr;
bool Eligible(AActor* Actor)
{
    return GIsEditor && ActorConstructionEnabled.GetValueOnGameThread() != 0 && Actor && Actor->GetWorld()
        && !Actor->GetWorld()->IsGameWorld() && AngelscriptActorConstruction::IsPureScriptActor(Actor);
}
bool Execute(AActor* Actor, const FTransform& Transform, const FRotationConversionCache* Rotation,
    const FComponentInstanceDataCache* Cache, bool bDefaultTransform, ESpawnActorScaleMethod Scale)
{
    const auto Original = reinterpret_cast<FExecute>(ExecuteEntry.Original());
    if (!Eligible(Actor)) return Original(Actor, Transform, Rotation, Cache, bDefaultTransform, Scale);
    FConstruction Construction{Actor, Transform, Rotation, Cache, bDefaultTransform};
    TGuardValue<FConstruction*> Scope(ActorConstructionCurrent, &Construction);
    const bool Result = Original(Actor, Transform, Rotation, Cache, bDefaultTransform, Scale);
    if (Cache && !Construction.bCacheFinished)
        UE_LOG(LogTemp, Error, TEXT("ASR-EDITOR-CONSTRUCTION-CACHE-PHASE-MISSING: %s"), *Actor->GetPathName());
    return Result;
}
void ProcessUserScript(AActor* Actor)
{
    const auto Original = reinterpret_cast<FUserScript>(UserScriptEntry.Original());
    if (!ActorConstructionCurrent || ActorConstructionCurrent->Actor != Actor || !Eligible(Actor)) { Original(Actor); return; }
    AngelscriptActorConstruction::Prepare(
        Actor, &ActorConstructionCurrent->Transform, ActorConstructionCurrent->Rotation,
        ActorConstructionCurrent->bDefaultTransform);
    if (ActorConstructionCurrent->Cache)
        ActorConstructionCurrent->Cache->ApplyToActor(Actor, ECacheApplyPhase::PostSimpleConstructionScript);
    // Preserve the engine's UCS running flag, cook random scope and mobility validation.
    FGuardValue_Bitfield(Actor->GetWorld()->bIsRunningConstructionScript, true);
    Original(Actor);
    for (UActorComponent* Component : Actor->GetComponents())
        if (Component) Component->DetermineUCSModifiedProperties();
    ActorConstructionCurrent->bUserScriptFinished = true;
}
void Bind(const UClass* Class, UObject* Object)
{
    // BindDynamicDelegates recurses through parents. Only bracket the top-level call.
    const bool bTop = ActorConstructionCurrent
        && ActorConstructionCurrent->Actor == Object
        && Class == Object->GetClass()
        && Eligible(ActorConstructionCurrent->Actor);
    if (bTop && !ActorConstructionCurrent->bUserScriptFinished)
    {
        // The editor may explicitly disable UCS. Cached SCS values still precede delegate binding.
        AngelscriptActorConstruction::Prepare(
            ActorConstructionCurrent->Actor, &ActorConstructionCurrent->Transform,
            ActorConstructionCurrent->Rotation, ActorConstructionCurrent->bDefaultTransform);
        if (ActorConstructionCurrent->Cache)
            ActorConstructionCurrent->Cache->ApplyToActor(
                ActorConstructionCurrent->Actor, ECacheApplyPhase::PostSimpleConstructionScript);
        for (UActorComponent* Component : ActorConstructionCurrent->Actor->GetComponents())
            if (Component) Component->DetermineUCSModifiedProperties();
    }
    reinterpret_cast<FBind>(DelegateEntry.Original())(Class, Object);
    if (!bTop) return;
    if (ActorConstructionCurrent->Cache)
        ActorConstructionCurrent->Cache->ApplyToActor(
            ActorConstructionCurrent->Actor, ECacheApplyPhase::PostUserConstructionScript);
    ActorConstructionCurrent->bCacheFinished = true;
}
}
namespace AngelscriptActorConstructionHook
{
void Initialize()
{
    if (!GIsEditor || IsRunningCommandlet()) return;
    const bool UserInstalled = UserScriptEntry.Install(
        TEXT("?ProcessUserConstructionScript@AActor@@IEAAXXZ"),
        "_ZN6AActor29ProcessUserConstructionScriptEv", reinterpret_cast<void*>(&ProcessUserScript),
        TEXT("UnrealEditor-Engine.dll"), "UnrealEditor-Engine");
    const bool BindInstalled = DelegateEntry.Install(
        TEXT("?BindDynamicDelegates@UBlueprintGeneratedClass@@SAXPEBVUClass@@PEAVUObject@@@Z"),
        "_ZN24UBlueprintGeneratedClass20BindDynamicDelegatesEPK6UClassP7UObject", reinterpret_cast<void*>(&Bind),
        TEXT("UnrealEditor-Engine.dll"), "UnrealEditor-Engine");
    const bool ExecuteInstalled = UserInstalled && BindInstalled && ExecuteEntry.Install(
        TEXT("?ExecuteConstruction@AActor@@QEAA_NAEBU?$TTransform@N@Math@UE@@")
        TEXT("PEBUFRotationConversionCache@@PEBVFComponentInstanceDataCache@@_NW4ESpawnActorScaleMethod@@@Z"),
        "_ZN6AActor19ExecuteConstructionERKN2UE4Math10TTransformIdEEPK24FRotationConversionCachePK27FComponentInstanceDataCacheb22ESpawnActorScaleMethod",
        reinterpret_cast<void*>(&Execute), TEXT("UnrealEditor-Engine.dll"), "UnrealEditor-Engine");
    if (!ExecuteInstalled)
    {
        ExecuteEntry.Remove(); DelegateEntry.Remove(); UserScriptEntry.Remove();
        UE_LOG(LogTemp, Error, TEXT("ASR-EDITOR-CONSTRUCTION-HOOK-UNAVAILABLE: pure AS editor cache phases cannot be restored."));
    }
}
void Shutdown() { ExecuteEntry.Remove(); DelegateEntry.Remove(); UserScriptEntry.Remove(); }
}
#endif
