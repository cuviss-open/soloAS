// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Blueprint/AngelscriptActorReplacementTransaction.h"
#include "Editor/SoloASCopyProperties.h"
#include "Editor/SoloASRetiredLoadingExportRepair.h"

#include "AngelscriptReloadSafety.h"
#include "ClassGenerator/ASClass.h"
#include "ComponentInstanceDataCache.h"
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/ScopedMovementUpdate.h"
#include "Engine/World.h"
#include "Layers/LayersSubsystem.h"
#include "Misc/ScopeExit.h"
#include "Serialization/ArchiveReplaceObjectAndStructPropertyRef.h"
#include "UObject/GarbageCollection.h"
#include "UObject/PropertyBagRepository.h"
#include "UObject/ReferencerFinder.h"
#include "UObject/Script.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectThreadContext.h"
#include "Templates/UnrealTemplate.h"

namespace AngelscriptActorReplacementTransaction
{
namespace
{
bool bRunning = false;
constexpr EObjectFlags ReinstanceFlags = RF_Public | RF_ArchetypeObject | RF_Transactional
	| RF_Transient | RF_TextExportTransient | RF_InheritableComponentTemplate | RF_Standalone | RF_HasDynamicImports;

void Require(bool bCondition, const TCHAR* Detail)
{
	if (!bCondition)
		AngelscriptReloadSafety::StopCommittedReload(TEXT("owned-actor-transaction"), Detail);
}

bool Reject(FString& OutError, const TCHAR* Detail)
{
	OutError = Detail;
	UE_LOG(LogTemp, Verbose, TEXT("SOLOAS_OWNED_ACTOR_TRANSACTION phase=admission accepted=0 mutation_started=0 reason=%s"), Detail);
	return false;
}

class FReplaceReferences final : public FArchiveReplaceObjectAndStructPropertyRef<UObject>
{
public:
	FReplaceReferences(UObject* Object, const TMap<UObject*, UObject*>& Map,
		const TMap<FSoftObjectPath, UObject*>& InSoftMap)
		: FArchiveReplaceObjectAndStructPropertyRef<UObject>(Object, Map, EArchiveReplaceObjectFlags::DelayStart)
		, SoftMap(InSoftMap)
	{
		SerializeSearchObject();
	}

	FArchive& operator<<(FSoftObjectPath& Path) override
	{
		if (UObject* const* Replacement = SoftMap.Find(Path)) Path = *Replacement;
		return *this;
	}

	FArchive& operator<<(FSoftObjectPtr& Pointer) override
	{
		return operator<<(Pointer.GetUniqueID());
	}

private:
	const TMap<FSoftObjectPath, UObject*>& SoftMap;
};

struct FActorState
{
	AActor* Old = nullptr;
	AActor* New = nullptr;
	USceneComponent* OldRoot = nullptr;
	USceneComponent* NewRoot = nullptr;
	FName Name;
	FSoftObjectPath Path;
	FTransform Transform;
	FGuid Guid;
	bool bRegistered = false;
	bool bHiddenEdLevel = false;
	USceneComponent* Parent = nullptr;
	FName Socket;
	TMap<FName, UActorComponent*> ComponentNames;
	TArray<TPair<AActor*, FName>> Children;
	int32 NativeNotifications = 0;
	int32 DestroyCalls = 0;
	int32 ConstructionCalls = 0;
	int32 AuxiliaryNotifications = 0;
};

bool HasUnsupportedObjectState(const UObject* Object)
{
	return !IsValid(Object) || Object->IsUnreachable() || Object->GetExternalPackage() || Object->GetLinker()
		|| Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed | RF_NeedLoad | RF_NeedPostLoad
			| RF_NeedPostLoadSubobjects | RF_NeedInitialization)
		|| Object->HasAnyInternalFlags(EInternalObjectFlags::Async | EInternalObjectFlags_AsyncLoading
			| EInternalObjectFlags::PendingConstruction);
}

bool IncludeDefaults(UObject* OldOuter, UObject* NewOuter, const TMap<UClass*, UClass*>& Classes,
	TMap<UObject*, UObject*>& Map, FString& OutError)
{
	TArray<UObject*> OldChildren;
	GetObjectsWithOuter(OldOuter, OldChildren, EGetObjectsFlags::None);
	for (UObject* OldChild : OldChildren)
	{
		UObject* NewChild = NewOuter->GetDefaultSubobjectByName(OldChild->GetFName());
		UClass* ExpectedClass = Classes.FindRef(OldChild->GetClass());
		if (!ExpectedClass) ExpectedClass = OldChild->GetClass();
		if (!OldChild->HasAnyFlags(RF_DefaultSubObject) || HasUnsupportedObjectState(OldChild)
			|| !NewChild || NewChild->GetOuter() != NewOuter || NewChild->GetClass() != ExpectedClass
			|| !NewChild->HasAnyFlags(RF_DefaultSubObject) || HasUnsupportedObjectState(NewChild))
			return Reject(OutError, TEXT("Unsupported or unmatched class default subobject"));
		Map.Add(OldChild, NewChild);
		if (!IncludeDefaults(OldChild, NewChild, Classes, Map, OutError)) return false;
	}
	return true;
}

bool Admit(const TMap<UClass*, UClass*>& Classes,
	TArray<FActorState>& Actors, TMap<UObject*, UObject*>& Defaults, FString& OutError)
{
	if (!IsInGameThread() || GIsReinstancing || GIsReconstructingBlueprintInstances || IsGarbageCollecting()
		|| FUObjectThreadContext::Get().IsRoutingPostLoad || !GEditor || GEngine != GEditor)
		return Reject(OutError, TEXT("Actor replacement requires a quiet editor game-thread reload"));
	for (const auto& Pair : Classes)
	{
		const UASClass* Old = Cast<UASClass>(Pair.Key);
		const UASClass* New = Cast<UASClass>(Pair.Value);
		if (!Old || !New || Old == New || !Old->HasValidScriptClassCarrier() || !New->HasValidScriptClassCarrier()
			|| Old->ClassGeneratedBy || New->ClassGeneratedBy || Old->GetSuperClass() != New->GetSuperClass()
			|| New->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			return Reject(OutError, TEXT("Blueprints, changed parents and unspawnable classes are unsupported"));
		TArray<UClass*> Derived;
		GetDerivedClasses(Old, Derived, true);
		for (UClass* Child : Derived)
		{
			if (Child->ClassGeneratedBy && !Child->HasAnyClassFlags(CLASS_NewerVersionExists))
				return Reject(
					OutError,
					TEXT("Blueprint descendants require stock generated-class relinking and CDO/SCS archetype migration before Actor replacement"));
		}
		if (Derived.Num()) return Reject(OutError, TEXT("Derived classes require stock hierarchy preparation"));
		GetDerivedClasses(New, Derived, true);
		for (UClass* Child : Derived)
		{
			if (Child->ClassGeneratedBy && !Child->HasAnyClassFlags(CLASS_NewerVersionExists))
				return Reject(
					OutError,
					TEXT("Blueprint descendants require stock generated-class relinking and CDO/SCS archetype migration before Actor replacement"));
		}
		if (Derived.Num()) return Reject(OutError, TEXT("Replacement classes with derived classes are unsupported"));
		UObject* OldDefault = Old->GetDefaultObject(false);
		UObject* NewDefault = New->GetDefaultObject(false);
		if (!OldDefault || !NewDefault || OldDefault->GetClass() != Old || NewDefault->GetClass() != New
			|| !OldDefault->HasAnyFlags(RF_ClassDefaultObject) || !NewDefault->HasAnyFlags(RF_ClassDefaultObject)
			|| HasUnsupportedObjectState(OldDefault) || HasUnsupportedObjectState(NewDefault))
			return Reject(OutError, TEXT("Both generated class defaults must already exist"));
		Defaults.Add(Pair.Key, Pair.Value);
		Defaults.Add(OldDefault, NewDefault);
		if (!IncludeDefaults(OldDefault, NewDefault, Classes, Defaults, OutError)) return false;
	}

	TArray<UObject*> Instances;
	for (const auto& Pair : Classes)
	{
		TArray<UObject*> ClassInstances;
		GetObjectsOfClass(Pair.Key, ClassInstances, false, RF_NoFlags, EInternalObjectFlags::Garbage);
		for (UObject* Object : ClassInstances)
		{
			if (Defaults.Contains(Object)) continue;
			if (Object->IsA<AActor>()) Instances.Add(Object);
			else if (!Object->IsA<UActorComponent>())
				return Reject(OutError, TEXT("Non-Actor instances require stock replacement"));
		}
	}
	for (UObject* Object : Instances)
	{
		AActor* Actor = Cast<AActor>(Object);
		UWorld* World = Actor ? Actor->GetWorld() : nullptr;
		UClass* ActorClass = Object->GetClass();
		if (!Actor || HasUnsupportedObjectState(Actor) || Actor->IsTemplate()
			|| !World || !World->IsInitialized() || World->IsCleanedUp() || World->IsGameWorld()
			|| World->GetNetDriver() || !Actor->GetLevel()
			|| Actor->GetOuter() != Actor->GetLevel() || Actor->IsSelectedInEditor()
			|| Actor->CurrentTransactionAnnotation.IsValid() || Actor->IsActorInitialized() || Actor->HasActorBegunPlay()
			|| Actor->GetIsReplicated() || Actor->GetTearOff() || Actor->GetOwner() || Actor->GetInstigator()
			|| Actor->GetActorInstanceGuid() != Actor->GetActorGuid() || !Actor->GetActorGuid().IsValid()
			|| Actor->GetExternalDataLayerAsset() || Actor->Layers.Num())
			return Reject(OutError, TEXT("Unsupported Actor archetype, identity, editor, package or gameplay state"));
		USceneComponent* Root = Actor->GetRootComponent();
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		TArray<UObject*> InnerObjects;
		GetObjectsWithOuter(Actor, InnerObjects, EGetObjectsFlags::IncludeNestedObjects);
		if (!Root || Components.IsEmpty() || HasUnsupportedObjectState(Root) || Root->GetOuter() != Actor
			|| Root->GetOwner() != Actor || Root->GetWorld() != World || Root->IsTemplate()
			|| !Root->HasAnyFlags(RF_DefaultSubObject) || Root->CreationMethod != EComponentCreationMethod::Native
			|| Actor->GetArchetype() != ActorClass->GetDefaultObject(false))
			return Reject(OutError, TEXT("A genuine native root component is required"));
		FActorState& State = Actors.AddDefaulted_GetRef();
		State.Old = Actor;
		State.OldRoot = Root;
		State.Name = Actor->GetFName();
		State.Path = FSoftObjectPath(Actor);
		State.Guid = Actor->GetActorGuid();
		State.Transform = Root->GetComponentTransform();
		State.bRegistered = Actor->HasActorRegisteredAllComponents();
		State.bHiddenEdLevel = Actor->bHiddenEdLevel;
		State.Parent = Root->GetAttachParent();
		State.Socket = Root->GetAttachSocketName();
		for (UObject* Inner : InnerObjects)
		{
			UActorComponent* Component = Cast<UActorComponent>(Inner);
			if (!Component || !Components.Contains(Component) || Component->GetOuter() != Actor
				|| HasUnsupportedObjectState(Component) || Component->CreationMethod != EComponentCreationMethod::Native
				|| !Component->HasAnyFlags(RF_DefaultSubObject))
				return Reject(OutError, TEXT("Non-native or nested instance components require stock replacement"));
			State.ComponentNames.Add(Component->GetFName(), Component);
		}
		TArray<AActor*> Children;
		Actor->GetAttachedActors(Children);
		for (AActor* Child : Children)
		{
			USceneComponent* ChildRoot = Child ? Child->GetRootComponent() : nullptr;
			if (!ChildRoot || HasUnsupportedObjectState(Child) || Child->GetWorld() != World
				|| ChildRoot->GetAttachParent() != Root)
				return Reject(OutError, TEXT("Unsupported attached child Actor"));
			State.Children.Emplace(Child, ChildRoot->GetAttachSocketName());
		}
	}
	if (Actors.Num() == 0) return Reject(OutError, TEXT("No native-root Actor instance requires replacement"));
	for (const auto& Pair : Classes)
	{
		if (!Pair.Key->IsChildOf(UActorComponent::StaticClass())) continue;
		Instances.Reset();
		GetObjectsOfClass(Pair.Key, Instances, false, RF_NoFlags, EInternalObjectFlags::Garbage);
		for (UObject* Object : Instances)
		{
			if (Defaults.Contains(Object)) continue;
			bool bOwned = false;
			for (const FActorState& Actor : Actors)
				bOwned |= Actor.ComponentNames.FindKey(Cast<UActorComponent>(Object)) != nullptr;
			if (!bOwned) return Reject(OutError, TEXT("Independent components require stock replacement"));
		}
	}
	for (const FActorState& Actor : Actors)
	{
		if (Actor.Parent)
		{
			bool bOwnedParent = false;
			for (const FActorState& Parent : Actors) bOwnedParent |= Actor.Parent == Parent.OldRoot;
			if (!bOwnedParent) return Reject(OutError, TEXT("Attachment parents must belong to this transaction"));
		}
		for (const auto& Child : Actor.Children)
		{
			bool bOwnedChild = false;
			for (const FActorState& Candidate : Actors) bOwnedChild |= Child.Key == Candidate.Old;
			if (!bOwnedChild) return Reject(OutError, TEXT("Attachment children must belong to this transaction"));
		}
	}
	return true;
}
}

bool Run(const TMap<UClass*, UClass*>& Classes, FString& OutError)
{
	if (bRunning) return Reject(OutError, TEXT("Nested Actor replacement transaction"));
	TArray<FActorState> Actors;
	TMap<UObject*, UObject*> Map;
	if (!Admit(Classes, Actors, Map, OutError)) return false;
	TGuardValue<bool> RunningGuard(bRunning, true);
	TGuardValue<std::atomic<bool>, bool> ReinstancingGuard(GIsReinstancing, true);
	TGuardValue<bool> ReconstructingGuard(GIsReconstructingBlueprintInstances, true);
	TArray<TStrongObjectPtr<UObject>> Retained;
	TSet<UObject*> Excluded;
	TMap<FSoftObjectPath, UObject*> SoftMap;
	TMap<UObject*, FSoftObjectPath> OriginalPaths;
	for (const auto& Pair : Map)
	{
		Retained.Emplace(Pair.Key);
		Retained.Emplace(Pair.Value);
		Excluded.Add(Pair.Key);
		OriginalPaths.Add(Pair.Key, FSoftObjectPath(Pair.Key));
		SoftMap.Add(FSoftObjectPath(Pair.Key), Pair.Value);
	}
	for (const auto& Pair : Classes)
	{
		TArray<UObject*> OldMembers;
		GetObjectsWithOuter(Pair.Key, OldMembers, EGetObjectsFlags::IncludeNestedObjects);
		Excluded.Append(OldMembers);
	}
	for (const FActorState& Actor : Actors)
	{
		Retained.Emplace(Actor.Old);
		Retained.Emplace(Actor.OldRoot);
		Excluded.Add(Actor.Old);
		for (const auto& Component : Actor.ComponentNames)
		{
			Retained.Emplace(Component.Value);
			Excluded.Add(Component.Value);
			OriginalPaths.Add(Component.Value, FSoftObjectPath(Component.Value));
		}
		OriginalPaths.Add(Actor.Old, Actor.Path);
		OriginalPaths.Add(Actor.OldRoot, FSoftObjectPath(Actor.OldRoot));
	}
	const FDelegateHandle NoGcHandle = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddLambda([]()
	{
		Require(!bRunning, TEXT("Garbage collection attempted during the owned Actor transaction"));
	});
	ON_SCOPE_EXIT { FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(NoGcHandle); };
	UE_LOG(LogTemp, Display, TEXT("SOLOAS_OWNED_ACTOR_TRANSACTION phase=admission accepted=1 actors=%d classes=%d defaults_map_entries=%d mutation_started=0"),
		Actors.Num(), Classes.Num(), Map.Num());
	for (FActorState& Actor : Actors)
	{
		UClass* NewActorClass = Classes.FindChecked(Actor.Old->GetClass());
		UClass* NewComponentClass = Classes.FindRef(Actor.OldRoot->GetClass());
		if (!NewComponentClass) NewComponentClass = Actor.OldRoot->GetClass();
		{
			FScopedMovementUpdate SilenceMovement(Actor.OldRoot);
			Actor.OldRoot->ConditionalUpdateComponentToWorld();
			Actor.Transform = Actor.OldRoot->GetComponentTransform();
			SilenceMovement.RevertMove();
			Actor.OldRoot->SetComponentToWorld(Actor.Transform);
		}
		for (const auto& Child : Actor.Children)
			Child.Key->GetRootComponent()->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		if (Actor.OldRoot->GetAttachParent())
			Actor.OldRoot->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
		Actor.Transform = Actor.OldRoot->GetComponentTransform();
		Require(Actor.Old->UObject::Rename(nullptr, Actor.Old->GetOuter(),
			REN_DoNotDirty | REN_DontCreateRedirectors | REN_AllowPackageLinkerMismatch), TEXT("Old Actor rename failed"));
		Actor.Old->UnregisterAllComponents();
		FActorSpawnParameters Spawn;
		Spawn.Name = Actor.Name;
		Spawn.OverrideLevel = Actor.Old->GetLevel();
		Spawn.OverrideActorGuid = Actor.Guid;
		Spawn.ObjectFlags |= Actor.Old->GetFlags() & ReinstanceFlags;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Spawn.bDeferConstruction = true;
		Spawn.bCreateActorPackage = false;
		Spawn.bHideFromSceneOutliner = !Actor.Old->IsListedInSceneOutliner();
		const FVector Location = Actor.Transform.GetLocation();
		const FRotator Rotation = Actor.Transform.Rotator();
		Actor.New = Actor.Old->GetWorld()->SpawnActor(NewActorClass, &Location, &Rotation, Spawn);
		Require(IsValid(Actor.New), TEXT("Deferred replacement Actor spawn failed"));
		Retained.Emplace(Actor.New);
		Actor.NewRoot = Actor.New->GetRootComponent();
		Require(IsValid(Actor.NewRoot) && Actor.NewRoot->GetClass() == NewComponentClass
			&& Actor.NewRoot->GetOuter() == Actor.New && Actor.New->GetActorGuid() == Actor.Guid
			&& Actor.New->GetActorInstanceGuid() == Actor.Guid, TEXT("Spawned Actor/root identity is outside the admitted contract"));
		Retained.Emplace(Actor.NewRoot);
		Actor.New->UnregisterAllComponents();
		TMap<UObject*, UObject*> NativeNotificationMap;
		const FDelegateHandle CopyHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddLambda(
			[&](const TMap<UObject*, UObject*>& NativeMap)
			{
				if (!NativeMap.Contains(Actor.Old)) return;
				++Actor.NativeNotifications;
				NativeNotificationMap = NativeMap;
			});
		{
			ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectsReplaced.Remove(CopyHandle); };
			UEngine::FCopyPropertiesForUnrelatedObjectsParams Params;
			Params.bPreserveRootComponent = true;
			Params.bNotifyObjectReplacement = true;
			Params.bReplaceInternalReferenceUponRead = true;
			Params.OptionalReplacementMappings = &Map;
			Params.OptionalOldToNewClassMappings = &Classes;
			FSoloASRetiredLoadingExportRepair LoadingExports(Actor.New);
			SoloAS::CopyPropertiesForUnrelatedObjectsWithArchetypePolicy(Actor.Old, Actor.New, Params);
		}
		Require(
			Actor.NativeNotifications == 1 && NativeNotificationMap.FindRef(Actor.Old) == Actor.New
				&& NativeNotificationMap.FindRef(Actor.OldRoot) == Actor.NewRoot
				&& Map.FindRef(Actor.OldRoot) == Actor.NewRoot && IsValid(Actor.OldRoot)
				&& Actor.Old->GetRootComponent() == Actor.OldRoot
				&& Actor.Old->GetComponents().Num() == Actor.ComponentNames.Num()
				&& Actor.Old->GetComponents().Contains(Actor.OldRoot),
			TEXT("Native CPFUO did not preserve the original root collection and produce both genuine pairs"));
		for (const auto& Pair : NativeNotificationMap)
		{
			Require(Pair.Key && Pair.Value && Pair.Key != Pair.Value, TEXT("Native CPFUO emitted an incomplete replacement pair"));
			UObject* Existing = Map.FindRef(Pair.Key);
			Require(Existing || Pair.Key == Actor.Old, TEXT("Native CPFUO emitted a pair outside the admitted object graph"));
			Require(!Existing || Existing == Pair.Value, TEXT("Native CPFUO replacement conflicts with the admitted map"));
			Map.Add(Pair.Key, Pair.Value);
		}
		UE_LOG(LogTemp, Display,
			TEXT("SOLOAS_OWNED_ACTOR_TRANSACTION phase=native-cpfuo actor=%p new_actor=%p old_root=%p new_root=%p native_notifications=%d native_map_entries=%d transaction_map_entries=%d root_pair_origin=native_cpfuo auxiliary_map_used=0 old_root_owned=%d"),
			Actor.Old, Actor.New, Actor.OldRoot, Actor.NewRoot, Actor.NativeNotifications,
			NativeNotificationMap.Num(), Map.Num(), Actor.Old->GetComponents().Contains(Actor.OldRoot));
		Actor.New->ResetPropertiesForConstruction();
		if (Actor.bRegistered) Actor.New->RegisterAllComponents();
	}
	for (FActorState& Actor : Actors)
	{
		++Actor.DestroyCalls;
		Require(Actor.Old->GetWorld()->EditorDestroyActor(Actor.Old, true), TEXT("Old Actor editor destruction failed"));
	}
	for (const auto& Pair : Map)
	{
		const FSoftObjectPath* Path = OriginalPaths.Find(Pair.Key);
		if (Path) SoftMap.Add(*Path, Pair.Value);
	}
	// Construction scripts may read other replacement instances. Complete their references
	// from the genuine batch map before running any script, regardless of Actor copy order.
	for (const FActorState& Actor : Actors)
	{
		FReplaceReferences ReplaceActor(Actor.New, Map, SoftMap);
		for (UActorComponent* Component : Actor.New->GetComponents())
		{
			FReplaceReferences ReplaceComponent(Component, Map, SoftMap);
		}
	}
	for (FActorState& Actor : Actors)
	{
		if (Actor.Parent)
		{
			USceneComponent* Parent = Cast<USceneComponent>(Map.FindRef(Actor.Parent));
			Require(IsValid(Parent) && Actor.NewRoot->AttachToComponent(Parent,
				FAttachmentTransformRules::KeepWorldTransform, Actor.Socket), TEXT("Replacement parent attachment failed"));
		}
		for (const auto& Child : Actor.Children)
		{
			AActor* NewChild = Cast<AActor>(Map.FindRef(Child.Key));
			Require(IsValid(NewChild) && NewChild->GetRootComponent(), TEXT("Replacement child Actor mapping is missing"));
			USceneComponent* ChildRoot = NewChild->GetRootComponent();
			if (!ChildRoot->GetAttachParent())
				Require(ChildRoot->AttachToComponent(Actor.NewRoot, FAttachmentTransformRules::KeepWorldTransform,
					Child.Value), TEXT("Replacement child attachment failed"));
			ChildRoot->UpdateComponentToWorld();
		}
	}
	for (FActorState& Actor : Actors)
	{
		FEditorScriptExecutionGuard ScriptGuard;
		FComponentInstanceDataCache EmptyComponentData;
		++Actor.ConstructionCalls;
		Require(Actor.New->ExecuteConstruction(Actor.Transform, nullptr, &EmptyComponentData), TEXT("Replacement Actor construction failed"));
		Require(IsValid(Actor.New) && IsValid(Actor.NewRoot) && Actor.New->GetRootComponent() == Actor.NewRoot
			&& Actor.NewRoot->GetOwner() == Actor.New && Actor.NewRoot->GetWorld() == Actor.New->GetWorld(),
			TEXT("Construction changed the admitted native-root identity"));
		if (Actor.bHiddenEdLevel)
		{
			Actor.New->bHiddenEdLevel = true;
			Actor.New->MarkComponentsRenderStateDirty();
		}
		TMap<UObject*, UObject*> AuxiliaryComponentMap;
		for (UActorComponent* Component : Actor.New->GetComponents())
		{
			if (UActorComponent** Old = Actor.ComponentNames.Find(Component->GetFName()))
				AuxiliaryComponentMap.Add(*Old, Component);
		}
		Require(AuxiliaryComponentMap.Num() == Actor.ComponentNames.Num(), TEXT("Construction auxiliary component notification is incomplete"));
		++Actor.AuxiliaryNotifications;
		GEditor->NotifyToolsOfObjectReplacement(AuxiliaryComponentMap);
		UE_LOG(
			LogTemp,
			Display,
			TEXT("SOLOAS_OWNED_ACTOR_TRANSACTION phase=construction actor=%p construction_calls=%d "
				"auxiliary_notifications=%d auxiliary_map_entries=%d native_map_replaced=0"),
			Actor.New, Actor.ConstructionCalls, Actor.AuxiliaryNotifications, AuxiliaryComponentMap.Num());
		Actor.New->Modify();
		if (ULayersSubsystem* Layers = GEditor->GetEditorSubsystem<ULayersSubsystem>())
			Layers->InitializeNewActorLayers(Actor.New);
	}
	TArray<UObject*> Sources;
	Map.GenerateKeyArray(Sources);
	const TArray<UObject*> Referencers = FReferencerFinder::GetAllReferencers(Sources, &Excluded);
	for (UObject* Referencer : Referencers)
	{
		if (IsValid(Referencer) && !Excluded.Contains(Referencer))
		{
			FReplaceReferences Replace(Referencer, Map, SoftMap);
		}
	}
	UE::FPropertyBagRepository::Get().ReassociateObjects(Map);
	for (const FActorState& Actor : Actors)
	{
		Require(
			Map.FindRef(Actor.Old) == Actor.New && Map.FindRef(Actor.OldRoot) == Actor.NewRoot
				&& Actor.DestroyCalls == 1 && Actor.ConstructionCalls == 1 && Actor.NativeNotifications == 1
				&& Actor.AuxiliaryNotifications == 1
				&& Actor.New->GetClass() == Classes.FindChecked(Actor.Old->GetClass())
				&& Actor.NewRoot->IsRegistered() == Actor.bRegistered,
			TEXT("Final Actor transaction invariants failed"));
	}
	UE_LOG(
		LogTemp,
		Display,
		TEXT("SOLOAS_OWNED_ACTOR_TRANSACTION phase=final-notification actors=%d map_entries=%d referencers=%d final_notifications=1 g_is_reinstancing=%d g_is_reconstructing=%d gc_during_transaction=0 whole093_credit=0"),
		Actors.Num(), Map.Num(), Referencers.Num(), GIsReinstancing.load(), GIsReconstructingBlueprintInstances);
	FCoreUObjectDelegates::OnObjectsReinstanced.Broadcast(Map);
	return true;
}
}
