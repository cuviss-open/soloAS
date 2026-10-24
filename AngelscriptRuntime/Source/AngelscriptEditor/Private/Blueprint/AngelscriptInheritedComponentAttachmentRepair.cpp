// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Blueprint/AngelscriptInheritedComponentAttachmentRepair.h"
#include "Blueprint/AngelscriptInheritedComponentAttachmentRepairSupport.h"

#include "AngelscriptReflectionAdapter.h"
#include "ClassGenerator/ASClass.h"
#include "Engine/SCS_Node.h"
#include "ComponentInstanceDataCache.h"
#include "UObject/GCObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Misc/ScopeExit.h"

DEFINE_LOG_CATEGORY_STATIC(LogAngelscriptInheritedAttachmentRepair, Log, All);

namespace
{
using namespace AngelscriptInheritedAttachmentRepairPrivate;

bool BuildActorRepairPlan(
	AActor* Actor,
	const TSet<UClass*>& ReloadedScriptClasses,
	const TMap<UClass*, UClass*>& ReloadedScriptClassReplacements,
	bool bRepairAttachments,
	const FScriptInstanceAttachmentOverrides* InstanceOverrides,
	FActorRepairPlan& OutPlan,
	FString& OutFailure)
{
	OutPlan.Actor = Actor;
	OutPlan.OriginalRoot = Actor->GetRootComponent();
	OutPlan.DesiredRoot = OutPlan.OriginalRoot;

	for (TFieldIterator<FProperty> It(Actor->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAllPropertyFlags(CPF_InstancedReference)
			|| !HasAngelscriptPropertyTrait(
				Property, EAngelscriptPropertyTrait::RuntimeGenerated))
		{
			continue;
		}
		FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
		if (ObjectProperty == nullptr
			|| ObjectProperty->PropertyClass == nullptr
			|| !ObjectProperty->PropertyClass->IsChildOf(UActorComponent::StaticClass())
			|| !IsDeclaredDefaultComponentProperty(ObjectProperty))
		{
			continue;
		}

		UObject* OriginalValue =
			ObjectProperty->GetObjectPropertyValue_InContainer(Actor);
		if (IsValid(OriginalValue))
		{
			continue;
		}

		UActorComponent* Candidate = nullptr;
		const EComponentPropertyCandidateResolution Resolution =
			ResolveOwnedDefaultComponentPropertyCandidate(
				Actor, ObjectProperty, Candidate, OutFailure);
		if (Resolution == EComponentPropertyCandidateResolution::Invalid)
		{
			return false;
		}
		if (Resolution == EComponentPropertyCandidateResolution::Resolved)
		{
			FPropertyRepair& Repair = OutPlan.Properties.AddDefaulted_GetRef();
			Repair.Property = ObjectProperty;
			Repair.OriginalValue = OriginalValue;
			Repair.DesiredValue = Candidate;
		}
	}
	if (!bRepairAttachments)
	{
		return true;
	}
	if (OutPlan.OriginalRoot != nullptr
		&& (!IsUsableObject(OutPlan.OriginalRoot)
			|| OutPlan.OriginalRoot->GetOwner() != Actor
			|| OutPlan.OriginalRoot->GetOuter() != Actor))
	{
		OutFailure = TEXT("replacement actor contains an invalid original root component");
		return false;
	}

	TArray<USceneComponent*> Components;
	TMap<FName, USceneComponent*> ComponentsByName;
	for (UActorComponent* ActorComponent : Actor->GetComponents())
	{
		USceneComponent* Component = Cast<USceneComponent>(ActorComponent);
		if (Component == nullptr)
		{
			continue;
		}
		if (!IsUsableObject(Component) || Component->GetOwner() != Actor)
		{
			OutFailure = TEXT("replacement actor contains an unusable scene component");
			return false;
		}
		if (ComponentsByName.Contains(Component->GetFName()))
		{
			OutFailure = TEXT("replacement actor contains duplicate scene component names");
			return false;
		}
		Components.Add(Component);
		ComponentsByName.Add(Component->GetFName(), Component);
	}
	Components.Sort([](const USceneComponent& Left, const USceneComponent& Right)
	{
		return Left.GetFName().LexicalLess(Right.GetFName());
	});

	if (!Actor->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		const AActor* ActorArchetype = Cast<AActor>(Actor->GetArchetype());
		const USceneComponent* ArchetypeRoot = ActorArchetype != nullptr
			? ActorArchetype->GetRootComponent()
			: nullptr;
		if (ArchetypeRoot != nullptr)
		{
			USceneComponent* const* LocalRoot = ComponentsByName.Find(ArchetypeRoot->GetFName());
			if (LocalRoot == nullptr
				|| !IsUsableObject(*LocalRoot)
				|| (*LocalRoot)->GetOwner() != Actor
				|| (*LocalRoot)->GetOuter() != Actor
				|| !(*LocalRoot)->IsDefaultSubobject())
			{
				OutFailure = TEXT("replacement actor is missing its archetype root component");
				return false;
			}
			OutPlan.DesiredRoot = *LocalRoot;
		}
	}

	TMap<USceneComponent*, USceneComponent*> DesiredParents;
	for (USceneComponent* Component : Components)
	{
		if (!Component->IsDefaultSubobject() || Component->GetOuter() != Actor)
		{
			continue;
		}
		FName DesiredParentName;
		FName DesiredSocketName;
		const EInheritedAttachmentResolution Resolution = ResolveInheritedAttachment(
			Component,
			ReloadedScriptClasses,
			ReloadedScriptClassReplacements,
			DesiredParentName,
			DesiredSocketName,
			OutFailure);
		if (Resolution == EInheritedAttachmentResolution::Invalid)
		{
			return false;
		}
		if (Resolution == EInheritedAttachmentResolution::NotApplicable)
		{
			continue;
		}
		bool bHasInstanceOverride = false;
		const FScriptInstanceAttachmentOverride* AttachmentOverride = nullptr;
		if (InstanceOverrides != nullptr)
		{
			if (const auto* Override = InstanceOverrides->Find(Component->GetFName()))
			{
				DesiredParentName = Override->ParentName;
				DesiredSocketName = Override->SocketName;
				AttachmentOverride = Override;
				bHasInstanceOverride = true;
			}
		}

		USceneComponent* CurrentParent = Component->GetAttachParent();
		// A root's cross-Actor attachment is instance state restored by the reinstancer,
		// not an inherited intra-Actor default attachment.
		if (Component == OutPlan.DesiredRoot && DesiredParentName.IsNone() && !bHasInstanceOverride
			&& CurrentParent && CurrentParent->GetOwner() != OutPlan.Actor)
		{
			if (!IsUsableObject(CurrentParent) || !IsUsableObject(CurrentParent->GetOwner())
				|| CurrentParent->GetWorld() != Actor->GetWorld() || CountInParent(Component, CurrentParent) != 1)
			{
				OutFailure = TEXT("replacement root contains an invalid cross-Actor attachment");
				return false;
			}
			continue;
		}
		USceneComponent* DesiredParent = nullptr;
		if (!DesiredParentName.IsNone())
		{
			USceneComponent* const* FoundParent = ComponentsByName.Find(DesiredParentName);
			if (FoundParent == nullptr
				|| !IsUsableObject(*FoundParent)
				|| *FoundParent == Component
				|| (*FoundParent)->GetOwner() != Actor
				|| (*FoundParent)->GetOuter() != Actor
				|| (*FoundParent)->GetWorld() != Actor->GetWorld()
				|| (AttachmentOverride && AttachmentOverride->bDynamicParent && (*FoundParent)->IsDefaultSubobject())
				|| (!(*FoundParent)->IsDefaultSubobject()
					&& !(bHasInstanceOverride
						&& (*FoundParent)->CreationMethod == EComponentCreationMethod::Instance
						&& Actor->GetInstanceComponents().Contains(*FoundParent)
						&& AttachmentOverride->bDynamicParent
						&& AttachmentOverride->DynamicParent == FObjectKey(*FoundParent))))
			{
				OutFailure = TEXT("replacement actor is missing an inherited attachment parent");
				return false;
			}
			DesiredParent = *FoundParent;
		}
		DesiredParents.Add(Component, DesiredParent);
		if (CurrentParent != nullptr
			&& (!IsUsableObject(CurrentParent)
				|| CurrentParent->GetOwner() != Actor
				|| CurrentParent->GetOuter() != Actor))
		{
			OutFailure = TEXT("replacement actor contains an invalid original attachment parent");
			return false;
		}
		const int32 CurrentParentMembershipCount = CountInParent(
			Component, CurrentParent);
		const int32 ExistingDesiredParentMembershipCount = CountInParent(
			Component, DesiredParent);
		if (CurrentParentMembershipCount > 1
			|| ExistingDesiredParentMembershipCount > 1
			|| (CurrentParent != DesiredParent
				&& ExistingDesiredParentMembershipCount != 0)
			|| (Component->IsRegistered()
				&& CurrentParent != nullptr
				&& CurrentParentMembershipCount != 1))
		{
			OutFailure = TEXT("replacement actor contains stale or duplicate attachment membership");
			return false;
		}
		const bool bShouldBeListedByDesiredParent = DesiredParent != nullptr
			&& (Component->IsRegistered()
				|| (CurrentParent == DesiredParent
					&& ExistingDesiredParentMembershipCount == 1)
				|| (CurrentParent != nullptr
					&& CurrentParent != DesiredParent)
				|| !DesiredParent->GetAttachChildren().IsEmpty());
		if (Component->IsRegistered()
			&& DesiredParent != nullptr
			&& !DesiredParent->IsRegistered())
		{
			OutFailure = TEXT("registered component has an unregistered desired parent");
			return false;
		}

		if (CurrentParent != DesiredParent
			|| (DesiredParent != nullptr
				&& Component->GetAttachSocketName() != DesiredSocketName))
		{
			FComponentRepair& Repair = OutPlan.Components.AddDefaulted_GetRef();
			Repair.Component = Component;
			Repair.DesiredParent = DesiredParent;
			Repair.OriginalParent = CurrentParent;
			Repair.DesiredSocket = DesiredSocketName;
			Repair.OriginalSocket = Component->GetAttachSocketName();
			Repair.OriginalRelativeTransform = Component->GetRelativeTransform();
			Repair.bWasRegistered = Component->IsRegistered();
			Repair.OriginalParentMembershipCount = CurrentParentMembershipCount;
			Repair.DesiredParentMembershipCount =
				bShouldBeListedByDesiredParent ? 1 : 0;
		}
	}

	for (FComponentRepair& Repair : OutPlan.Components)
	{
		TSet<USceneComponent*> Visited;
		for (USceneComponent* Cursor = Repair.DesiredParent; Cursor != nullptr; )
		{
			if (Visited.Contains(Cursor))
			{
				OutFailure = TEXT("desired inherited attachment graph contains a cycle");
				return false;
			}
			Visited.Add(Cursor);
			++Repair.DesiredDepth;
			USceneComponent* const* Next = DesiredParents.Find(Cursor);
			Cursor = Next != nullptr ? *Next : Cursor->GetAttachParent();
		}
	}
	if (!ValidateNoDesiredAttachmentCycles(OutPlan))
	{
		OutFailure = TEXT("desired inherited attachment graph contains a cycle");
		return false;
	}
	OutPlan.Components.Sort([](const FComponentRepair& Left, const FComponentRepair& Right)
	{
		if (Left.DesiredDepth != Right.DesiredDepth)
		{
			return Left.DesiredDepth < Right.DesiredDepth;
		}
		return Left.Component->GetFName().LexicalLess(Right.Component->GetFName());
	});
	return true;
}

bool BuildRepairPlans(
	const TArray<AActor*>& Actors,
	const TSet<AActor*>& AttachmentActors,
	const TSet<UClass*>& ReloadedScriptClasses,
	const TMap<UClass*, UClass*>& ReloadedScriptClassReplacements,
	const TMap<FObjectKey, FScriptInstanceAttachmentOverrides>& InstanceAttachments,
	TArray<FActorRepairPlan>& OutPlans,
	FString& OutFailure)
{
	for (AActor* Actor : Actors)
	{
		FActorRepairPlan Plan;
		if (!BuildActorRepairPlan(
			Actor,
			ReloadedScriptClasses,
			ReloadedScriptClassReplacements,
			AttachmentActors.Contains(Actor),
			InstanceAttachments.Find(FObjectKey(Actor)),
			Plan,
			OutFailure))
		{
			return false;
		}
		if (Plan.DesiredRoot != Plan.OriginalRoot
			|| !Plan.Properties.IsEmpty()
			|| !Plan.Components.IsEmpty())
		{
			OutPlans.Add(MoveTemp(Plan));
		}
	}
	return true;
}


}

class FAngelscriptInheritedComponentAttachmentRepairScope::FInstanceState final : public FGCObject
{
    struct FComponent
    {
        UActorComponent* Source = nullptr;
        UClass* Class = nullptr;
        FString Path;
        EComponentCreationMethod Method = EComponentCreationMethod::Native;
        TStructOnScope<FActorComponentInstanceData> Data;
        USceneComponent* Parent = nullptr;
        FName Socket;
        FTransform Relative;
        FObjectKey Applied;
    };
    struct FActor
    {
        AActor* Source = nullptr;
        AActor* Replacement = nullptr;
        TUniquePtr<FComponentInstanceDataCache> Cache;
        TArray<FComponent> Components;
        bool bConstructed = false;
    };
    class FRemap final : public FReferenceCollector
    {
    public:
        explicit FRemap(const TMap<UObject*, UObject*>& InMap) : Map(InMap) {}
        virtual bool IsIgnoringArchetypeRef() const override { return false; }
        virtual bool IsIgnoringTransient() const override { return false; }
        virtual void HandleObjectReference(UObject*& Object, const UObject*, const FProperty*) override
        {
            if (UObject* const* Replacement = Map.Find(Object)) Object = *Replacement;
        }
    private:
        const TMap<UObject*, UObject*>& Map;
    };
    TArray<FActor> Actors;
    TMap<UObject*, UObject*> Map;

public:
    explicit FInstanceState(const TMap<UClass*, UClass*>& Classes)
    {
        for (const auto& Pair : Classes) if (Pair.Key && Pair.Value) Map.Add(Pair.Key, Pair.Value);
        for (TObjectIterator<AActor> It; It; ++It)
        {
            AActor* Actor = *It;
            if (!IsValid(Actor) || Actor->IsTemplate() || !Actor->GetWorld() || Actor->GetWorld()->IsGameWorld()) continue;
            bool bAffected = false;
            for (UClass* Class = Actor->GetClass(); Class; Class = Class->GetSuperClass()) bAffected |= Classes.Contains(Class);
            for (UActorComponent* Component : Actor->GetComponents())
                if (Component) bAffected |= Classes.Contains(Component->GetClass());
            if (!bAffected) continue;
            FActor& Snapshot = Actors.AddDefaulted_GetRef();
            Snapshot.Source = Actor;
            Snapshot.Cache = MakeUnique<FComponentInstanceDataCache>(Actor);
            for (UActorComponent* Component : Actor->GetComponents())
            {
                if (!IsValid(Component) || !Component->IsCreatedByConstructionScript()) continue;
                FComponent& Entry = Snapshot.Components.AddDefaulted_GetRef();
                Entry.Source = Component;
                Entry.Class = Component->GetClass();
                Entry.Path = Component->GetPathName(Actor);
                Entry.Method = Component->CreationMethod;
                Entry.Data = Component->GetComponentInstanceData();
                if (USceneComponent* Scene = Cast<USceneComponent>(Component))
                {
                    Entry.Parent = Scene->GetAttachParent();
                    Entry.Socket = Scene->GetAttachSocketName();
                    Entry.Relative = Scene->GetRelativeTransform();
                }
            }
        }
    }

    virtual FString GetReferencerName() const override { return TEXT("AngelscriptReloadComponentState"); }
    virtual void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        for (auto& Actor : Actors)
        {
            Collector.AddReferencedObject(Actor.Source);
            Collector.AddReferencedObject(Actor.Replacement);
            Actor.Cache->AddReferencedObjects(Collector);
            for (auto& Component : Actor.Components)
            {
                Collector.AddReferencedObject(Component.Source);
                Collector.AddReferencedObject(Component.Class);
                Collector.AddReferencedObject(Component.Parent);
                if (Component.Data.IsValid()) Component.Data->AddReferencedObjects(Collector);
            }
        }
        Collector.AddReferencedObjects(Map);
    }

    void Consume(const TMap<UObject*, UObject*>& Replacements, bool bFinal)
    {
        for (const auto& Pair : Replacements) if (Pair.Key && Pair.Value && Pair.Key != Pair.Value) Map.Add(Pair.Key, Pair.Value);
        // Collapse chains so cached object references never stop at a retired intermediate.
        for (auto& Pair : Map)
        {
            TSet<UObject*> Seen;
            UObject* Target = Pair.Value;
            while (UObject* const* Next = Map.Find(Target))
            {
                if (*Next == Target) break;
                checkf(!Seen.Contains(Target), TEXT("Cyclic component-state replacement map"));
                Seen.Add(Target);
                Target = *Next;
            }
            Pair.Value = Target;
        }
        bool bReady = true;
        for (auto& Actor : Actors)
        {
            Actor.Replacement = Cast<AActor>(Map.FindRef(Actor.Source));
            if (!Actor.Replacement && IsValid(Actor.Source)) Actor.Replacement = Actor.Source;
            if (!IsValid(Actor.Replacement)) { bReady = false; continue; }
            const bool bActorCopy = Replacements.Contains(Actor.Source);
            if (bFinal) Actor.bConstructed = true;
            else if (!bActorCopy)
                for (const auto& Component : Actor.Components)
                    if (UActorComponent* New = Cast<UActorComponent>(Replacements.FindRef(Component.Source)))
                        Actor.bConstructed |= New->GetOwner() == Actor.Replacement && New->CreationMethod == Component.Method;
            if (!Actor.Components.IsEmpty() && !Actor.bConstructed) bReady = false;
            for (const auto& Component : Actor.Components)
            {
                UActorComponent* New = nullptr;
                for (UActorComponent* Candidate : Actor.Replacement->GetComponents())
                    if (Candidate && Candidate->GetPathName(Actor.Replacement) == Component.Path && Candidate->CreationMethod == Component.Method)
                    { check(!New); New = Candidate; }
                UClass* Expected = Cast<UClass>(Map.FindRef(Component.Class));
                if (!Expected) Expected = Component.Class;
                if (!IsValid(New) || New->GetClass() != Expected) { bReady = false; continue; }
                Map.Add(Component.Source, New);
            }
        }
        if (!bReady) return;
        FRemap Remap(Map);
        for (auto& Actor : Actors)
        {
            bool bChanged = false;
            for (const auto& Component : Actor.Components)
                bChanged |= Component.Applied != FObjectKey(Map.FindChecked(Component.Source));
            if (!bChanged) continue;
            Actor.Cache->FindAndReplaceInstances(Map);
            Actor.Cache->AddReferencedObjects(Remap);
            Actor.Cache->ApplyToActor(Actor.Replacement, ECacheApplyPhase::NonConstructionScript);
            for (auto& Component : Actor.Components)
            {
                UActorComponent* New = CastChecked<UActorComponent>(Map.FindChecked(Component.Source));
                if (Component.Data.IsValid())
                {
                    Component.Data->FindAndReplaceInstances(Map);
                    Component.Data->AddReferencedObjects(Remap);
                    // Name/path and creation method establish the pair independently of
                    // the engine cache's exact template-class equality requirement.
                    Component.Data->ApplyToComponent(New, ECacheApplyPhase::NonConstructionScript);
                }
                Component.Applied = FObjectKey(New);
            }
        }
        // Restore scene instance overrides only after every component reference is mapped.
        for (auto& Actor : Actors)
            for (const auto& Component : Actor.Components)
                if (USceneComponent* Scene = Cast<USceneComponent>(Map.FindChecked(Component.Source)))
                {
                    USceneComponent* Parent = Component.Parent ? Cast<USceneComponent>(Map.FindRef(Component.Parent)) : nullptr;
                    if (!Parent && IsValid(Component.Parent)) Parent = Component.Parent;
                    if (Parent && Scene->GetAttachParent() != Parent)
                    {
                        check(Scene->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform, Component.Socket));
                    }
                    else if (!Parent && Scene->GetAttachParent()) Scene->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
                    Scene->SetRelativeTransform(Component.Relative);
                }
    }
};

FAngelscriptInheritedComponentAttachmentRepairScope::
	FAngelscriptInheritedComponentAttachmentRepairScope(
		const TMap<UClass*, UClass*>& ReloadClasses)
{
	for (const TPair<UClass*, UClass*>& Pair : ReloadClasses)
	{
		UASClass* NewScriptClass = Cast<UASClass>(Pair.Value);
		if (Cast<UASClass>(Pair.Key) != nullptr
			&& NewScriptClass != nullptr
			&& NewScriptClass->HasValidScriptClassCarrier())
		{
			ReloadedScriptClasses.Add(NewScriptClass);
			ReloadedScriptClassReplacements.Add(Pair.Key, NewScriptClass);
		}
	}
	if (!ReloadedScriptClasses.IsEmpty())
	{
		InstanceState = MakeUnique<FInstanceState>(ReloadClasses);
		CopyDelegateHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddRaw(
			this, &FAngelscriptInheritedComponentAttachmentRepairScope::CaptureInstanceAttachments);
		DelegateHandle = FCoreUObjectDelegates::OnObjectsReinstanced.AddRaw(
			this,
			&FAngelscriptInheritedComponentAttachmentRepairScope::HandleObjectsReinstanced);
	}
}

FAngelscriptInheritedComponentAttachmentRepairScope::
	~FAngelscriptInheritedComponentAttachmentRepairScope()
{
	if (DelegateHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectsReinstanced.Remove(DelegateHandle);
	}
	FCoreUObjectDelegates::OnObjectsReplaced.Remove(CopyDelegateHandle);
}

void FAngelscriptInheritedComponentAttachmentRepairScope::CaptureInstanceAttachments(
	const TMap<UObject*, UObject*>& ReplacedObjects)
{
	auto FailCapture = [this](const TCHAR* Reason)
	{
		if (!bOverrideCaptureFailed) OverrideCaptureFailureReason = Reason;
		bOverrideCaptureFailed = true;
	};
	// Stock constructs SCS children before its final reference-replacement pass.
	// Consume genuine template pairs while archetype copying is still in progress.
	for (TObjectIterator<USCS_Node> Node; Node; ++Node)
	{
		UActorComponent* OldTemplate = Node->ComponentTemplate;
		if (!OldTemplate) continue;
		UObject* const* Replacement = ReplacedObjects.Find(OldTemplate);
		if (!Replacement) continue;
		UActorComponent* NewTemplate = Cast<UActorComponent>(*Replacement);
		UClass* ExpectedClass = ReloadedScriptClassReplacements.FindRef(OldTemplate->GetClass());
		if (NewTemplate && ExpectedClass && NewTemplate->GetClass() == ExpectedClass && NewTemplate->IsTemplate())
		{
			Node->ComponentTemplate = NewTemplate;
			Node->ComponentClass = ExpectedClass;
			// ExecuteNodeOnActor checks the generated property's class before assigning.
			// Object-reference slot size is unchanged; stock's later class-reference pass
			// would update this constraint after construction, which is too late here.
			UBlueprintGeneratedClass* Owner = Node->GetTypedOuter<UBlueprintGeneratedClass>();
			UBlueprint* Blueprint = Owner ? Cast<UBlueprint>(Owner->ClassGeneratedBy) : nullptr;
			UClass* PropertyOwners[] = {Owner, Blueprint ? Blueprint->GeneratedClass : nullptr,
				Blueprint ? Blueprint->SkeletonGeneratedClass : nullptr};
			for (UClass* PropertyOwner : PropertyOwners)
			{
				if (!PropertyOwner) continue;
				FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(PropertyOwner, Node->GetVariableName());
				if (Property && Property->GetOwnerStruct() == PropertyOwner
					&& ReloadedScriptClassReplacements.FindRef(Property->PropertyClass) == ExpectedClass)
					Property->PropertyClass = ExpectedClass;
			}
		}
	}
	for (const auto& Pair : ReplacedObjects)
	{
		USceneComponent* OldComponent = Cast<USceneComponent>(Pair.Key);
		USceneComponent* NewComponent = Cast<USceneComponent>(Pair.Value);
		if (!OldComponent || !NewComponent || OldComponent == NewComponent
			|| OldComponent->IsTemplate() || NewComponent->IsTemplate()) continue;
		const FObjectKey OldKey(Pair.Key), NewKey(Pair.Value);
		if (const auto* Prior = InstanceComponentReplacements.Find(OldKey))
			if (*Prior != NewKey)
			{
				FailCapture(TEXT("scene_component_replacement_conflict"));
				return;
			}
		InstanceComponentReplacements.Add(OldKey, NewKey);
	}
	auto ResolveParents = [&](TMap<FObjectKey, FScriptInstanceAttachmentOverrides>& Attachments)
	{
		for (auto& Actor : Attachments)
			for (auto& Entry : Actor.Value)
			{
				auto& Override = Entry.Value;
				if (!Override.bDynamicParent) continue;
				TSet<FObjectKey> Seen;
				while (const FObjectKey* Next = InstanceComponentReplacements.Find(Override.DynamicParent))
				{
					if (Seen.Contains(Override.DynamicParent)) { FailCapture(TEXT("dynamic_attachment_parent_cycle")); return; }
					Seen.Add(Override.DynamicParent); Override.DynamicParent = *Next;
				}
			}
	};
	TMap<FObjectKey, FScriptInstanceAttachmentOverrides> Captured = InstanceAttachments;
	for (const auto& Pair : ReplacedObjects)
	{
		AActor* OldActor = Cast<AActor>(Pair.Key);
		AActor* NewActor = Cast<AActor>(Pair.Value);
		if (!IsReloadedScriptBlueprintActor(OldActor) || !IsReloadedScriptBlueprintActor(NewActor)
			|| OldActor == NewActor || OldActor->IsTemplate() || NewActor->IsTemplate()
			|| OldActor->GetWorld() != NewActor->GetWorld()) continue;
		FScriptInstanceAttachmentOverrides Overrides;
		if (const auto* Prior = Captured.Find(FObjectKey(OldActor))) Overrides = *Prior;
		for (UActorComponent* Entry : OldActor->GetComponents())
		{
			USceneComponent* Component = Cast<USceneComponent>(Entry);
			if (!IsUsableObject(Component) || Component->GetOuter() != OldActor || !Component->IsDefaultSubobject()) continue;
			USceneComponent* Template = Cast<USceneComponent>(Component->GetArchetype());
			USceneComponent* Parent = Component->GetAttachParent();
			if (!IsUsableObject(Template) || !Template->IsTemplate() || !Template->IsDefaultSubobject()
				|| (Parent && (!IsUsableObject(Parent) || Parent->GetOuter() != OldActor
					|| Parent->GetOwner() != OldActor
					|| (!Parent->IsDefaultSubobject()
						&& !(Parent->CreationMethod == EComponentCreationMethod::Instance
							&& OldActor->GetInstanceComponents().Contains(Parent)))
					|| Parent->GetWorld() != OldActor->GetWorld() || CountInParent(Component, Parent) != 1))) continue;
			USceneComponent* TemplateParentObject = Template->GetAttachParent();
			if (TemplateParentObject && (!IsUsableObject(TemplateParentObject) || !TemplateParentObject->IsTemplate()
				|| !TemplateParentObject->IsDefaultSubobject() || TemplateParentObject->GetOuter() != Template->GetOuter()
				|| TemplateParentObject->GetOwner() != Template->GetOwner())) continue;
			const FName ParentName = Parent ? Parent->GetFName() : NAME_None;
			const FName TemplateParent = TemplateParentObject ? TemplateParentObject->GetFName() : NAME_None;
			if (ParentName != TemplateParent || Component->GetAttachSocketName() != Template->GetAttachSocketName())
			{
				FScriptInstanceAttachmentOverride Override;
				Override.ParentName = ParentName;
				Override.SocketName = Component->GetAttachSocketName();
				Override.bDynamicParent = Parent && !Parent->IsDefaultSubobject();
				if (Override.bDynamicParent) Override.DynamicParent = FObjectKey(Parent);
				Overrides.Add(Component->GetFName(), Override);
			}
		}
		if (!Overrides.IsEmpty()) Captured.Add(FObjectKey(OldActor), MoveTemp(Overrides));
	}
	// Resolve complete callback chains before publishing any propagated witness.
	TMap<FObjectKey, FScriptInstanceAttachmentOverrides> Propagated = InstanceAttachments;
	ResolveParents(Captured);
	ResolveParents(Propagated);
	if (bOverrideCaptureFailed) return;
	for (const auto& Pair : ReplacedObjects)
	{
		if (!Pair.Key) continue;
		const auto* Overrides = Captured.Find(FObjectKey(Pair.Key));
		if (!Overrides) continue;
		UObject* Terminal = Pair.Key;
		TSet<UObject*> Seen;
		while (UObject* const* Next = ReplacedObjects.Find(Terminal))
		{
			if (!*Next || *Next == Terminal) break;
			if (Seen.Contains(Terminal)) { FailCapture(TEXT("replacement_actor_cycle")); return; }
			Seen.Add(Terminal); Terminal = *Next;
		}
		AActor* Actor = Cast<AActor>(Terminal);
		AActor* SourceActor = Cast<AActor>(Pair.Key);
		if (!SourceActor || !IsReloadedScriptBlueprintActor(Actor) || Actor->IsTemplate()
			|| SourceActor->GetWorld() != Actor->GetWorld()) continue;
		auto& Target = Propagated.FindOrAdd(FObjectKey(Actor));
		for (const auto& Override : *Overrides)
		{
			if (const auto* Prior = Target.Find(Override.Key))
				if (*Prior != Override.Value) { FailCapture(TEXT("inherited_attachment_override_conflict")); return; }
			Target.Add(Override.Key, Override.Value);
		}
	}
	InstanceAttachments = MoveTemp(Propagated);
	if (InstanceState) InstanceState->Consume(ReplacedObjects, false);
}

bool FAngelscriptInheritedComponentAttachmentRepairScope::
	IsReloadedScriptBlueprintActor(const AActor* Actor) const
{
	if (!IsUsableObject(Actor))
	{
		return false;
	}
	UClass* ActorClass = Actor->GetClass();
	if (ActorClass == nullptr
		|| Cast<UASClass>(ActorClass) != nullptr
		|| !ActorClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
	{
		return false;
	}
	TSet<const UClass*> Visited;
	for (UClass* Cursor = ActorClass; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
	{
		if (Visited.Contains(Cursor))
		{
			return false;
		}
		Visited.Add(Cursor);
		if (ReloadedScriptClasses.Contains(Cursor)
			|| ReloadedScriptClassReplacements.Contains(Cursor))
		{
			return true;
		}
	}
	return false;
}

bool FAngelscriptInheritedComponentAttachmentRepairScope::
	IsReloadedScriptActor(const AActor* Actor) const
{
	return IsUsableObject(Actor)
		&& Cast<UASClass>(Actor->GetClass()) != nullptr
		&& ReloadedScriptClasses.Contains(Actor->GetClass());
}

void FAngelscriptInheritedComponentAttachmentRepairScope::HandleObjectsReinstanced(
	const TMap<UObject*, UObject*>& ReinstancedObjects)
{
	CaptureInstanceAttachments(ReinstancedObjects);
	if (InstanceState) InstanceState->Consume(ReinstancedObjects, true);
	if (bOverrideCaptureFailed)
	{
		UE_LOG(LogAngelscriptInheritedAttachmentRepair, Error,
			TEXT("INHERITED_ATTACHMENT_OVERRIDE_CAPTURE_FAILED reason=%s"),
			OverrideCaptureFailureReason ? OverrideCaptureFailureReason : TEXT("unknown"));
		return;
	}
	TSet<AActor*> SeenActors;
	TSet<AActor*> AttachmentActors;
	TArray<AActor*> Actors;
	for (const TPair<UObject*, UObject*>& Pair : ReinstancedObjects)
	{
		AActor* Actor = Cast<AActor>(Pair.Value);
		const bool bRepairAttachments = IsReloadedScriptBlueprintActor(Actor);
		if ((bRepairAttachments || IsReloadedScriptActor(Actor))
			&& !SeenActors.Contains(Actor))
		{
			SeenActors.Add(Actor);
			Actors.Add(Actor);
			if (bRepairAttachments)
			{
				AttachmentActors.Add(Actor);
			}
		}
	}
	Actors.Sort([](const AActor& Left, const AActor& Right)
	{
		if (Left.HasAnyFlags(RF_ClassDefaultObject)
			!= Right.HasAnyFlags(RF_ClassDefaultObject))
		{
			return Left.HasAnyFlags(RF_ClassDefaultObject);
		}
		return Left.GetPathName().Compare(Right.GetPathName()) < 0;
	});

	// Admit only explicit null pairs for defaults actually removed from the new schema.
	TMap<AActor*, TSet<FName>> RemovedDefaults;
	TMap<USceneComponent*, int32> RemovedChildBudgets;
	for (const auto& Pair : ReinstancedObjects)
	{
		USceneComponent* Old = Cast<USceneComponent>(Pair.Key);
		if (Pair.Value || !Old || !Old->IsDefaultSubobject()) continue;
		AActor* OldActor = Cast<AActor>(Old->GetOuter());
		AActor* Actor = OldActor ? Cast<AActor>(ReinstancedObjects.FindRef(OldActor)) : nullptr;
		if (!Actors.Contains(Actor) || FindFProperty<FProperty>(Actor->GetClass(), Old->GetFName())
			|| Actor->GetClass()->GetDefaultObject()->GetDefaultSubobjectByName(Old->GetFName())) continue;
		RemovedDefaults.FindOrAdd(Actor).Add(Old->GetFName());
		USceneComponent* OldParent = Old->GetAttachParent();
		if (!OldParent)
		{
			USceneComponent* Template = Cast<USceneComponent>(Old->GetArchetype());
			OldParent = Template ? Template->GetAttachParent() : nullptr;
		}
		USceneComponent* Parent = OldParent ? Cast<USceneComponent>(ReinstancedObjects.FindRef(OldParent)) : nullptr;
		if (!Parent && OldParent) Parent = Cast<USceneComponent>(Actor->GetDefaultSubobjectByName(OldParent->GetFName()));
		if (IsUsableObject(Parent) && Parent->GetOuter() == Actor && Parent->IsDefaultSubobject())
			++RemovedChildBudgets.FindOrAdd(Parent);
	}
	for (const auto& Removed : RemovedDefaults)
		if (auto* Overrides = InstanceAttachments.Find(FObjectKey(Removed.Key)))
			for (auto It = Overrides->CreateIterator(); It; ++It)
				if (!It.Value().bDynamicParent && Removed.Value.Contains(It.Value().ParentName))
					It.RemoveCurrent(); // The new authoritative template supplies the surviving child's parent.

	TArray<FActorRepairPlan> Plans;
	FString Failure;
	// A copied default child list can still name an instance whose parent was
	// overridden. Restrict normalization to overrides witnessed in this reload.
	FArrayProperty* ChildrenProperty = FindFProperty<FArrayProperty>(USceneComponent::StaticClass(), TEXT("AttachChildren"));
	FObjectPropertyBase* ChildProperty = ChildrenProperty ? CastField<FObjectPropertyBase>(ChildrenProperty->Inner) : nullptr;
	TMap<USceneComponent*, TArray<UObject*>> OriginalChildLists;
	bool bNormalizeValid = true;
	for (AActor* Actor : Actors)
	{
		const auto* Overrides = InstanceAttachments.Find(FObjectKey(Actor));
		if ((!Overrides && !RemovedDefaults.Contains(Actor)) || !bNormalizeValid) continue;
		for (UActorComponent* Entry : Actor->GetComponents())
		{
			USceneComponent* Parent = Cast<USceneComponent>(Entry);
			if (!IsUsableObject(Parent) || Parent->GetOuter() != Actor || !Parent->IsDefaultSubobject()) continue;
			TArray<UObject*> Children;
			bool bNeedsRepair = false;
			int32 RemovedChildBudget = RemovedChildBudgets.FindRef(Parent);
			for (const auto& EntryChild : Parent->GetAttachChildren())
			{
				USceneComponent* Child = EntryChild.Get();
				Children.Add(Child);
				if (!Child && RemovedChildBudget > 0)
				{
					--RemovedChildBudget;
					bNeedsRepair = true;
					continue;
				}
				if (!IsUsableObject(Child) || Child->GetOuter() != Actor || Child->GetOwner() != Actor
					|| Child->GetWorld() != Parent->GetWorld()) { bNormalizeValid = false; break; }
				if (Child->GetAttachParent() == Parent) continue;
				const auto* Override = Overrides ? Overrides->Find(Child->GetFName()) : nullptr;
				USceneComponent* Actual = Child->GetAttachParent();
				const bool bParentMatches = Override && (Override->ParentName.IsNone() ? Actual == nullptr
					: IsUsableObject(Actual) && Actual->GetOuter() == Actor && Actual->GetOwner() == Actor
						&& Actual->GetWorld() == Parent->GetWorld() && Actual->GetFName() == Override->ParentName
						&& (!Override->bDynamicParent || Override->DynamicParent == FObjectKey(Actual))
						&& CountInParent(Child, Actual) == 1);
				if (!bParentMatches || Child->GetAttachSocketName() != Override->SocketName || CountInParent(Child, Parent) != 1)
				{ bNormalizeValid = false; break; }
				bNeedsRepair = true;
			}
			if (bNeedsRepair)
			{
				if (!ChildrenProperty || !ChildProperty || ChildProperty->PropertyClass != USceneComponent::StaticClass())
				{ bNormalizeValid = false; break; }
				OriginalChildLists.Add(Parent, MoveTemp(Children));
			}
		}
	}
	if (!bNormalizeValid)
	{
		UE_LOG(LogAngelscriptInheritedAttachmentRepair, Error, TEXT("INHERITED_ATTACHMENT_OVERRIDE_PREFLIGHT_FAILED"));
		return;
	}
	bool bKeepNormalizedLists = false;
	ON_SCOPE_EXIT
	{
		if (!bKeepNormalizedLists)
			for (const auto& Entry : OriginalChildLists)
			{
				FScriptArrayHelper Array(ChildrenProperty, ChildrenProperty->ContainerPtrToValuePtr<void>(Entry.Key));
				Array.EmptyAndAddValues(Entry.Value.Num());
				for (int32 Index = 0; Index < Entry.Value.Num(); ++Index)
					ChildProperty->SetObjectPropertyValue(Array.GetRawPtr(Index), Entry.Value[Index]);
			}
	};
	for (const auto& Entry : OriginalChildLists)
	{
		FScriptArrayHelper Array(ChildrenProperty, ChildrenProperty->ContainerPtrToValuePtr<void>(Entry.Key));
		for (int32 Index = Array.Num() - 1; Index >= 0; --Index)
		{
			USceneComponent* Child = Cast<USceneComponent>(ChildProperty->GetObjectPropertyValue(Array.GetRawPtr(Index)));
			if (!Child || Child->GetAttachParent() != Entry.Key) Array.RemoveValues(Index);
		}
	}
	if (!BuildRepairPlans(
		Actors,
		AttachmentActors,
		ReloadedScriptClasses,
		ReloadedScriptClassReplacements,
		InstanceAttachments,
		Plans,
		Failure))
	{
		UE_LOG(LogAngelscriptInheritedAttachmentRepair, Error,
			TEXT("INHERITED_ATTACHMENT_REPAIR_PREFLIGHT_FAILED reason=%s"), *Failure);
		return;
	}
	if (Plans.IsEmpty())
	{
		bKeepNormalizedLists = true;
		return;
	}
	if (!ApplyRepairPlans(Plans, Failure))
	{
		const bool bRollbackSucceeded = RollbackRepairPlans(Plans);
		UE_LOG(LogAngelscriptInheritedAttachmentRepair, Error,
			TEXT("INHERITED_ATTACHMENT_REPAIR_APPLY_FAILED reason=%s rollback=%d"),
			*Failure, bRollbackSucceeded ? 1 : 0);
		if (!bRollbackSucceeded)
		{
			bKeepNormalizedLists = true;
			UE_LOG(LogAngelscriptInheritedAttachmentRepair, Fatal,
				TEXT("INHERITED_ATTACHMENT_REPAIR_ROLLBACK_FAILED"));
		}
		return;
	}

	bKeepNormalizedLists = true;
	int32 ComponentCount = 0;
	int32 PropertyCount = 0;
	int32 CdoCount = 0;
	for (const FActorRepairPlan& Plan : Plans)
	{
		ComponentCount += Plan.Components.Num();
		PropertyCount += Plan.Properties.Num();
		CdoCount += Plan.Actor->HasAnyFlags(RF_ClassDefaultObject) ? 1 : 0;
	}
	if (PropertyCount == 0)
	{
		UE_LOG(LogAngelscriptInheritedAttachmentRepair, Display,
			TEXT("SCRIPT_BP_INHERITED_ATTACHMENT_REPAIRED actors=%d components=%d cdo=%d live=%d"),
			Plans.Num(), ComponentCount, CdoCount, Plans.Num() - CdoCount);
	}
	else
	{
		UE_LOG(LogAngelscriptInheritedAttachmentRepair, Display,
			TEXT("SCRIPT_BP_INHERITED_COMPONENT_STATE_REPAIRED actors=%d attachments=%d properties=%d cdo=%d live=%d"),
			Plans.Num(), ComponentCount, PropertyCount, CdoCount,
			Plans.Num() - CdoCount);
	}
}
