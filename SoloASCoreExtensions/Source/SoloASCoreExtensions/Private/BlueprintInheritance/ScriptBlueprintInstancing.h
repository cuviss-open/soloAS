// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class AActor;
class UActorComponent;
class UChildActorComponent;
class USceneComponent;
struct FFrame;
enum class ECacheApplyPhase;

namespace SoloASCoreExtensions::ScriptInterfaces
{
using FBlueprintObjectReference = TFunctionRef<bool(UObject*&, UClass*, bool)>;
using FBlueprintPropertyReferences = TFunctionRef<bool(FProperty*, void*, FBlueprintObjectReference, bool)>;

struct FBlueprintChildActorOwner
{
    TStrongObjectPtr<AActor> Previous;
    TStrongObjectPtr<UChildActorComponent> Parent;
    FTransform Relative;
};
struct FDeferredBlueprintChildActor
{
    TStrongObjectPtr<UChildActorComponent> Component;
    TStrongObjectPtr<UClass> Class;
    TStrongObjectPtr<AActor> Template;
};
bool GatherBlueprintChildActors(TArray<UObject*>& Roots, const TSet<UClass*>& GeneratedClasses,
    TArray<FBlueprintChildActorOwner>& Children, FString& Error);
bool DeferBlueprintChildActorComponents(AActor* Previous, AActor* Candidate,
    const TArray<FBlueprintChildActorOwner>& Children, const TMap<UObject*, UObject*>& Types,
    TArray<FDeferredBlueprintChildActor>& Deferred, FString& Error);
bool CreateDeferredBlueprintChildActor(FDeferredBlueprintChildActor& State,
    TFunction<void(AActor*)> Customize, FString& Error);

// A transaction-local snapshot. Allocation follows Outer order; references are
// copied only after the complete instance map exists, preserving aliases/cycles.
class FScriptBlueprintInstancing
{
public:
    FScriptBlueprintInstancing();
    ~FScriptBlueprintInstancing();
    bool Prepare(const TArray<UObject*>& Roots, const TArray<FBlueprintChildActorOwner>& Children,
        FBlueprintPropertyReferences References, FString& Error);
    bool Contains(UObject* Object) const;
    bool RequiresMigration(UObject* Root) const;
    bool HasMigration() const;
    void RequireMigration(UObject* Object);
    bool Allocate(TMap<UObject*, UObject*>& Objects, const TMap<UObject*, UObject*>& Types,
        bool bConstructionReady, FString& Error);
    bool Copy(TMap<UObject*, UObject*>& Objects, FBlueprintPropertyReferences References, FString& Error);
    bool Rebind(const TMap<UObject*, UObject*>& Objects, FBlueprintPropertyReferences References, FString& Error);
    void ApplyComponentData(const TMap<UObject*, UObject*>& Objects,
        const TMap<UObject*, UObject*>& Templates, ECacheApplyPhase Phase, AActor* Actor = nullptr);
    void RestoreTransforms(const TMap<UObject*, UObject*>& Objects);
    bool Validate(const TMap<UObject*, UObject*>& Objects, FString& Error) const;
    void RegisterComponents(const TMap<UObject*, UObject*>& Objects);

private:
    struct FNode;
    TArray<TUniquePtr<FNode>> Nodes;
    TMap<UObject*, UObject*> Owners;
    TSet<UObject*> RequiredRoots;
    TSet<UObject*> ChildComponents;
    TArray<TStrongObjectPtr<UObject>> References;
};

// The engine calls UserConstructionScript after SCS and before native
// OnConstruction/BeginPlay. Only candidate class maps and listed instances are
// intercepted; the original function is restored when the batch leaves scope.
class FScriptBlueprintConstruction
{
public:
    FScriptBlueprintConstruction();
    ~FScriptBlueprintConstruction();
    bool Prepare(const TArray<AActor*>& Actors, TFunction<bool(AActor*)> BeforeUserScript,
        TFunction<bool(AActor*)> AfterUserScript, FString& Error);
    bool AddActor(AActor* Actor, FString& Error);
    bool Entered(AActor* Actor) const;
    bool Succeeded() const;

private:
    struct FClass;
    TArray<TUniquePtr<FClass>> Classes;
    TSet<AActor*> Receivers;
    TSet<AActor*> EnteredReceivers;
    TFunction<bool(AActor*)> Before;
    TFunction<bool(AActor*)> After;
    bool bSucceeded = true;
    static thread_local FScriptBlueprintConstruction* Active;
    static void Invoke(UObject* Context, FFrame& Stack, void* Result);
};
}
