// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once
#include "CoreMinimal.h"
#include "AngelscriptType.h"
#include "ComponentInstanceDataCache.h"
#include "Dom/JsonValue.h"
#include "Misc/SecureHash.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/SoftObjectPath.h"
#include "ScriptBlueprintInstancing.h"

class AActor;
class APawn;
class UActorComponent;
class USceneComponent;
class UWorld;
class ULevel;
class USCS_Node;
class USimpleConstructionScript;
class UBlueprintGeneratedClass;
struct FAngelscriptClassDesc;

namespace SoloASCoreExtensions::ScriptInterfaces
{
// Match reflected members by schema name; the VM owner checks compiled storage
// accesses separately before a replacement generation is published.
struct FBlueprintGenerationSchema
{
    TMap<FProperty*, FProperty*> Properties;
    TArray<FProperty*> AddedProperties;
};

struct FBlueprintReceiverValue
{
    FAngelscriptTypeUsage Type;
    int32 PreviousOffset = 0, CandidateOffset = 0;
};
struct FBlueprintReceiverGeneration
{
    TSharedPtr<FAngelscriptClassDesc> PreviousDesc, CandidateDesc;
    TStrongObjectPtr<UClass> Previous;
    UClass* Candidate = nullptr;
    TArray<FString> Schema;
    TArray<FBlueprintReceiverValue> Values;
    TArray<TPair<FObjectPropertyBase*, UClass*>> TypedFields;
};
struct FBlueprintCurrentReference
{
    TWeakObjectPtr<UObject> Owner;
    FProperty* Property = nullptr;
};
struct FBlueprintSubclassGeneration
{
    TStrongObjectPtr<UClass> Previous;
    TStrongObjectPtr<UClass> Candidate;
    TStrongObjectPtr<UPackage> Package;
    FString CanonicalPackage;
    FString ClassPath;
    TArray<FString> Schema;
};
struct FBlueprintNativeComponentGeneration
{
    TStrongObjectPtr<UActorComponent> Previous;
    TStructOnScope<FActorComponentInstanceData> Data;
    TArray<FProperty*> Properties;
};
struct FBlueprintActorGeneration
{
    TStrongObjectPtr<AActor> Previous;
    TStrongObjectPtr<UWorld> World;
    TStrongObjectPtr<ULevel> Level;
    FTransform Transform;
    TWeakObjectPtr<AActor> Owner;
    TWeakObjectPtr<APawn> Instigator;
    TUniquePtr<FComponentInstanceDataCache> Components;
    TArray<TStrongObjectPtr<UObject>> CacheReferences;
    TStrongObjectPtr<USceneComponent> NativeRoot;
    TArray<FBlueprintNativeComponentGeneration> NativeGraph;
};
struct FBlueprintActorCacheReferences : FReferenceCollector
{
    TArray<TStrongObjectPtr<UObject>>& Pins;
    const TMap<UObject*, UObject*>* Replacements;
    FBlueprintActorCacheReferences(TArray<TStrongObjectPtr<UObject>>& InPins,
        const TMap<UObject*, UObject*>* InReplacements = nullptr) : Pins(InPins), Replacements(InReplacements) {}
    virtual bool IsIgnoringArchetypeRef() const override { return false; }
    virtual bool IsIgnoringTransient() const override { return false; }
    virtual void HandleObjectReference(UObject*& Object, const UObject*, const FProperty*) override
    {
        if (Replacements)
            if (UObject* const* Replacement = Replacements->Find(Object)) Object = *Replacement;
        if (Object) Pins.Emplace(Object);
    }
};
struct FBlueprintInPlaceUpdate;
struct FBlueprintReceiverMigration;
struct FPendingBlueprintGeneration
{
    TStrongObjectPtr<UPackage> Package;
    TStrongObjectPtr<UClass> Previous;
    TStrongObjectPtr<UClass> Candidate;
    FString ClassPath, Filename, UnitId, View;
    FMD5Hash Hash;
    TSet<FString> RequiredModules;
    TMap<FString, TMap<FString, TSharedPtr<FJsonValue>>> ImportSchemas;
    TArray<FBlueprintReceiverGeneration> Receivers;
    TArray<FBlueprintSubclassGeneration> Subclasses;
    TMap<UObject*, UObject*> BlueprintMetadata;
    TMap<FField*, FField*> BlueprintFields;
    TSet<UObject*> OwnedBlueprintMetadata;
    TArray<TStrongObjectPtr<UObject>> BlueprintMetadataPins;
    TArray<FBlueprintActorGeneration> Actors;
    TArray<FBlueprintChildActorOwner> ChildActors;
    TUniquePtr<FScriptBlueprintInstancing> Instancing;
    TMap<UObject*, UObject*> ActorTemplates;
    TSet<UObject*> ActorComponents;
    TSet<UObject*> NativeActorComponents;
    TArray<TStrongObjectPtr<UObject>> PreviousInstances;
    TArray<TStrongObjectPtr<UObject>> PublishedInstances;
    TArray<FBlueprintCurrentReference> CurrentReferences;
    TMap<FSoftObjectPath, UObject*> PreviousSoftTargets;
    TSet<UObject*> RejectedSoftTargets;
    FString DelegateError;
    TMap<UObject*, UObject*> ReinstancedObjects;
#if WITH_EDITOR && !IS_MONOLITHIC
    TArray<TStrongObjectPtr<UObject>> ReinstancedInstances;
#endif
    TUniquePtr<FBlueprintInPlaceUpdate> InPlaceUpdate;
    TUniquePtr<FBlueprintReceiverMigration> Migration;
    FDelegateHandle ClassReloadHandle, ObjectsReinstancedHandle;
    bool bScriptParentOnly = false;
    bool bPrepared = false;
    bool bCommitAttempted = false;
    void RemoveMigrationDelegates();
    FPendingBlueprintGeneration();
    ~FPendingBlueprintGeneration();
};
struct FBlueprintOwnedGraphUpdate
{
    struct FNode { USCS_Node* Candidate; USCS_Node* Live; USCS_Node* Previous; USCS_Node* Parent; };
    struct FDefault { UObject* Candidate; UObject* Live; TArray<FProperty*> Changed; };
    struct FInstance
    {
        UObject* Object;
        int32 Default;
        TMap<UObject*, UObject*> Objects;
        TArray<FProperty*> Inherited;
        TUniquePtr<FStructOnScope> Values;
    };
    struct FMove { UObject* Object; UObject* Outer; FName Name; };
    struct FActor
    {
        AActor* Object;
        FTransform Transform;
        TMap<UObject*, UObject*> Objects;
        TArray<UActorComponent*> Retired;
        TArray<USceneComponent*> Reattach;
    };
    UBlueprintGeneratedClass* Class = nullptr;
    USimpleConstructionScript* Script = nullptr;
    USCS_Node* Root = nullptr;
    TArray<FNode> Nodes;
    TArray<USCS_Node*> PreviousNodes;
    TArray<FDefault> Defaults;
    TArray<FInstance> Instances;
    TArray<FMove> Moves;
    TArray<TStrongObjectPtr<UObject>> Prepared;
    TArray<FActor> Actors;
    TMap<UObject*, UObject*> Replacements;
};

// ABI-stable changes use the same compile transaction as new generations, but
// keep the current class/view. The package owns candidate bytecode and defaults;
// strong targets keep the prepared Actor ownership graph alive until commit.
struct FBlueprintInPlaceUpdate
{
    TMap<UObject*, UObject*> Objects;
    TArray<TPair<UClass*, UClass*>> Classes;
    TArray<TPair<UFunction*, UFunction*>> Functions;
    TArray<FBlueprintOwnedGraphUpdate> Graphs;
    TArray<TStrongObjectPtr<UObject>> Targets;
    FMD5Hash PreviousHash;
};


TArray<FString> BlueprintImportAbi(UClass* Class,
    const TMap<UObject*, UObject*>* ReboundTypes = nullptr, bool bIncludeStorage = true);
bool BuildBlueprintGenerationSchema(UClass* Previous, UClass* Candidate,
    const TMap<UObject*, UObject*>& CandidateTypes, FBlueprintGenerationSchema& Schema, FString& Error);
bool GatherBlueprintSubclassGenerations(FPendingBlueprintGeneration& Pending, TSet<UClass*>& Affected, FString& Error);
bool PrepareBlueprintSubclassGenerations(FPendingBlueprintGeneration& Pending,
    TMap<UObject*, UObject*>& Types, TMap<UObject*, UObject*>& PreviousTypes, FString& Error);
bool PublishBlueprintSubclassGenerations(FPendingBlueprintGeneration& Pending, FString& Error);
void RebindBlueprintGenerationMetadata(UObject* Object, const TMap<UObject*, UObject*>& Objects,
    const TMap<FField*, FField*>& Fields, const TSet<UObject*>* Excluded = nullptr,
    const TMap<FSoftObjectPath, UObject*>* SoftTargets = nullptr);
void AppendPreparedBlueprintInstances(const FPendingBlueprintGeneration& Pending, TMap<UObject*, UObject*>& Objects);
bool IsBlueprintActorStateField(FProperty* Property);
USCS_Node* BlueprintActorComponentSlot(UClass* Class, FProperty* Property);
bool VisitBlueprintGenerationReferences(FPendingBlueprintGeneration& Pending, FProperty* Property, void* Value,
    TFunctionRef<bool(UObject*&, UClass*, bool)> Visit, bool bReplace = false);
bool IsBlueprintGenerationInstance(UObject* Object);
USceneComponent* BlueprintNativeSceneRoot(AActor* Actor);
bool BlueprintNativeGraphMatches(AActor* Actor, AActor* Template);
bool PrepareBlueprintReceiverCommit(FPendingBlueprintGeneration& Pending, FString& Error);
void LockBlueprintReceiverMigration(FPendingBlueprintGeneration& Pending);
bool CommitBlueprintReceiverMigration(FPendingBlueprintGeneration& Pending, FString& Error);
}
