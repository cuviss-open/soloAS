// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "AngelscriptReflectionAdapter.h"
#include "ClassGenerator/ASClass.h"
#include "UObject/UnrealType.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"

class FObjectPropertyBase;

namespace AngelscriptInheritedAttachmentRepairPrivate
{
struct FPropertyRepair
{
	FObjectPropertyBase* Property = nullptr;
	UObject* OriginalValue = nullptr;
	UActorComponent* DesiredValue = nullptr;
	bool bPropertyApplied = false;
};

struct FComponentRepair
{
	USceneComponent* Component = nullptr;
	USceneComponent* DesiredParent = nullptr;
	USceneComponent* OriginalParent = nullptr;
	FName DesiredSocket;
	FName OriginalSocket;
	FTransform OriginalRelativeTransform;
	int32 DesiredDepth = 0;
	bool bWasRegistered = false;
	int32 OriginalParentMembershipCount = 0;
	int32 DesiredParentMembershipCount = 0;
	bool bApplied = false;
};

struct FActorRepairPlan
{
	AActor* Actor = nullptr;
	USceneComponent* DesiredRoot = nullptr;
	USceneComponent* OriginalRoot = nullptr;
	TArray<FPropertyRepair> Properties;
	TArray<FComponentRepair> Components;
	bool bRootApplied = false;
};

enum class EInheritedAttachmentResolution : uint8
{
	NotApplicable,
	Resolved,
	Invalid,
};

inline bool IsUsableObject(const UObject* Object)
{
	return Object != nullptr
		&& IsValid(Object)
		&& !Object->IsUnreachable()
		&& !Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
}

inline int32 CountInParent(const USceneComponent* Component, const USceneComponent* Parent)
{
	if (Component == nullptr || Parent == nullptr)
	{
		return 0;
	}
	int32 Count = 0;
	for (const TObjectPtr<USceneComponent>& Child : Parent->GetAttachChildren())
	{
		if (Child.Get() == Component)
		{
			++Count;
		}
	}
	return Count;
}

inline USceneComponent* FindOwnedDefaultSceneComponent(AActor* Actor, FName Name)
{
	USceneComponent* Match = nullptr;
	for (UActorComponent* ActorComponent : Actor->GetComponents())
	{
		USceneComponent* Component = Cast<USceneComponent>(ActorComponent);
		if (Component == nullptr || Component->GetFName() != Name)
		{
			continue;
		}
		if (Match != nullptr || !IsUsableObject(Component)
			|| Component->GetOwner() != Actor
			|| Component->GetOuter() != Actor
			|| !Component->IsDefaultSubobject())
		{
			return nullptr;
		}
		Match = Component;
	}
	return Match;
}

inline bool ValidateNoDesiredAttachmentCycles(const FActorRepairPlan& Plan)
{
	TMap<USceneComponent*, USceneComponent*> DesiredParents;
	for (const FComponentRepair& Repair : Plan.Components)
	{
		DesiredParents.Add(Repair.Component, Repair.DesiredParent);
	}
	for (const TPair<USceneComponent*, USceneComponent*>& Entry : DesiredParents)
	{
		TSet<USceneComponent*> Visited;
		for (USceneComponent* Cursor = Entry.Key; Cursor != nullptr; )
		{
			if (Visited.Contains(Cursor))
			{
				return false;
			}
			Visited.Add(Cursor);
			USceneComponent* const* Next = DesiredParents.Find(Cursor);
			Cursor = Next != nullptr ? *Next : nullptr;
		}
	}
	return true;
}

inline bool ApplyAttachmentState(
	USceneComponent* Component,
	USceneComponent* Parent,
	FName SocketName,
	const FTransform& RelativeTransform,
	bool bShouldBeRegistered,
	int32 ExpectedParentMembershipCount)
{
	if (!IsUsableObject(Component)
		|| Parent == Component
		|| Component->IsRegistered() != bShouldBeRegistered
		|| (ExpectedParentMembershipCount != 0
			&& ExpectedParentMembershipCount != 1)
		|| (Parent == nullptr && ExpectedParentMembershipCount != 0)
		|| (Parent != nullptr
			&& (!IsUsableObject(Parent)
				|| Parent->GetOwner() != Component->GetOwner()
				|| (bShouldBeRegistered && !Parent->IsRegistered()))))
	{
		return false;
	}
	if (Component->GetAttachParent() != nullptr)
	{
		Component->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
	}
	if (Parent != nullptr)
	{
		if (bShouldBeRegistered || ExpectedParentMembershipCount == 1)
		{
			if (!Component->AttachToComponent(
				Parent,
				FAttachmentTransformRules::KeepRelativeTransform,
				SocketName))
			{
				return false;
			}
		}
		else
		{
			Component->SetupAttachment(Parent, SocketName);
		}
	}
	else if (!bShouldBeRegistered)
	{
		Component->SetupAttachment(nullptr, NAME_None);
	}
	Component->SetRelativeTransform(RelativeTransform);
	return Component->IsRegistered() == bShouldBeRegistered
		&& Component->GetAttachParent() == Parent
		&& (Parent == nullptr || Component->GetAttachSocketName() == SocketName)
		&& CountInParent(Component, Parent) == ExpectedParentMembershipCount
		&& Component->GetRelativeTransform().Equals(RelativeTransform);
}

enum class EComponentPropertyCandidateResolution : uint8
{
	NotFound,
	Resolved,
	Invalid,
};

inline bool IsDeclaredDefaultComponentProperty(const FObjectPropertyBase* Property)
{
	const UASClass* OwnerClass = Property != nullptr
		? Cast<UASClass>(Property->GetOwnerClass())
		: nullptr;
	if (OwnerClass == nullptr)
	{
		return false;
	}
	for (const UASClass::FDefaultComponent& DefaultComponent :
		OwnerClass->DefaultComponents)
	{
		if (DefaultComponent.ComponentName == Property->GetFName()
			&& DefaultComponent.ComponentClass == Property->PropertyClass)
		{
			return true;
		}
	}
	return false;
}

inline EComponentPropertyCandidateResolution
ResolveOwnedDefaultComponentPropertyCandidate(
	AActor* Actor,
	const FObjectPropertyBase* Property,
	UActorComponent*& OutCandidate,
	FString& OutFailure)
{
	OutCandidate = nullptr;
	for (UActorComponent* Candidate : Actor->GetComponents())
	{
		if (Candidate == nullptr || Candidate->GetFName() != Property->GetFName())
		{
			continue;
		}
		if (OutCandidate != nullptr)
		{
			OutFailure = TEXT("replacement actor contains duplicate component property candidates");
			return EComponentPropertyCandidateResolution::Invalid;
		}
		OutCandidate = Candidate;
	}

	UActorComponent* Candidate = OutCandidate;
	if (Candidate == nullptr)
	{
		return EComponentPropertyCandidateResolution::NotFound;
	}
	if (!IsUsableObject(Candidate)
		|| Candidate->GetOwner() != Actor
		|| Candidate->GetOuter() != Actor
		|| !Candidate->IsDefaultSubobject()
		|| !Candidate->IsA(Property->PropertyClass))
	{
		OutFailure = TEXT("replacement actor contains an invalid component property candidate");
		return EComponentPropertyCandidateResolution::Invalid;
	}
	return EComponentPropertyCandidateResolution::Resolved;
}

inline EInheritedAttachmentResolution ResolveInheritedAttachment(
	USceneComponent* Component,
	const TSet<UClass*>& ReloadedScriptClasses,
	const TMap<UClass*, UClass*>& ReloadedScriptClassReplacements,
	FName& OutParentName,
	FName& OutSocketName,
	FString& OutFailure)
{
	USceneComponent* Template = Cast<USceneComponent>(Component->GetArchetype());
	TSet<const USceneComponent*> Visited;
	while (Template != nullptr && !Template->HasAnyFlags(RF_ClassDefaultObject))
	{
		if (!IsUsableObject(Template) || Visited.Contains(Template))
		{
			OutFailure = TEXT("invalid or cyclic component archetype chain");
			return EInheritedAttachmentResolution::Invalid;
		}
		Visited.Add(Template);

		AActor* TemplateActor = Cast<AActor>(Template->GetOuter());
		if (TemplateActor == nullptr || Template->GetOwner() != TemplateActor)
		{
			return EInheritedAttachmentResolution::NotApplicable;
		}
		if (!Component->GetOwner()->GetClass()->IsChildOf(TemplateActor->GetClass()))
		{
			return EInheritedAttachmentResolution::NotApplicable;
		}

		USceneComponent* AuthoritativeTemplate = nullptr;
		if (ReloadedScriptClasses.Contains(TemplateActor->GetClass()))
		{
			AuthoritativeTemplate = Template;
		}
		else if (UClass* const* ReplacementClass =
			ReloadedScriptClassReplacements.Find(TemplateActor->GetClass()))
		{
			AActor* ReplacementDefault = Cast<AActor>(
				(*ReplacementClass)->GetDefaultObject(false));
			AuthoritativeTemplate = ReplacementDefault != nullptr
				? FindOwnedDefaultSceneComponent(ReplacementDefault, Template->GetFName())
				: nullptr;
			if (AuthoritativeTemplate == nullptr)
			{
				OutFailure = TEXT("replacement script template could not be normalized");
				return EInheritedAttachmentResolution::Invalid;
			}
		}

		if (AuthoritativeTemplate != nullptr)
		{
			AActor* AuthoritativeActor = CastChecked<AActor>(AuthoritativeTemplate->GetOuter());
			USceneComponent* TemplateParent = AuthoritativeTemplate->GetAttachParent();
			if (TemplateParent != nullptr
				&& (!IsUsableObject(TemplateParent)
					|| TemplateParent == AuthoritativeTemplate
					|| !TemplateParent->IsTemplate()
					|| !TemplateParent->IsDefaultSubobject()
					|| TemplateParent->GetOuter() != AuthoritativeActor
					|| TemplateParent->GetOwner() != AuthoritativeActor))
			{
				OutFailure = TEXT("invalid authoritative script attachment template");
				return EInheritedAttachmentResolution::Invalid;
			}
			OutParentName = TemplateParent != nullptr
				? TemplateParent->GetFName()
				: NAME_None;
			OutSocketName = AuthoritativeTemplate->GetAttachSocketName();
			return EInheritedAttachmentResolution::Resolved;
		}

		USceneComponent* NextTemplate = Cast<USceneComponent>(Template->GetArchetype());
		if (NextTemplate == nullptr || NextTemplate->GetOuter() == TemplateActor)
		{
			return EInheritedAttachmentResolution::NotApplicable;
		}
		Component = Template;
		Template = NextTemplate;
	}
	return EInheritedAttachmentResolution::NotApplicable;
}

inline bool ApplyRepairPlans(TArray<FActorRepairPlan>& Plans, FString& OutFailure)
{
	for (FActorRepairPlan& Plan : Plans)
	{
		for (FPropertyRepair& Repair : Plan.Properties)
		{
			if (Repair.Property->GetObjectPropertyValue_InContainer(Plan.Actor)
				!= Repair.OriginalValue)
			{
				OutFailure = TEXT("component property changed after repair preflight");
				return false;
			}
			Repair.bPropertyApplied = true;
			Repair.Property->SetObjectPropertyValue_InContainer(Plan.Actor, Repair.DesiredValue);
			if (Repair.Property->GetObjectPropertyValue_InContainer(Plan.Actor) != Repair.DesiredValue)
			{
				OutFailure = TEXT("failed to restore component property");
				return false;
			}
		}
		if (Plan.DesiredRoot != Plan.OriginalRoot)
		{
			Plan.bRootApplied = true;
			if (!Plan.Actor->SetRootComponent(Plan.DesiredRoot))
			{
				OutFailure = TEXT("failed to restore replacement actor root component");
				return false;
			}
		}
		for (FComponentRepair& Repair : Plan.Components)
		{
			Repair.bApplied = true;
			if (!ApplyAttachmentState(
				Repair.Component,
				Repair.DesiredParent,
				Repair.DesiredSocket,
				Repair.OriginalRelativeTransform,
				Repair.bWasRegistered,
				Repair.DesiredParentMembershipCount))
			{
				OutFailure = TEXT("failed to apply inherited component attachment repair");
				return false;
			}
		}
	}
	return true;
}

inline bool RollbackRepairPlans(TArray<FActorRepairPlan>& Plans)
{
	bool bSucceeded = true;
	for (int32 PlanIndex = Plans.Num() - 1; PlanIndex >= 0; --PlanIndex)
	{
		FActorRepairPlan& Plan = Plans[PlanIndex];
		for (int32 ComponentIndex = Plan.Components.Num() - 1;
			ComponentIndex >= 0;
			--ComponentIndex)
		{
			FComponentRepair& Repair = Plan.Components[ComponentIndex];
			if (Repair.bApplied)
			{
				bSucceeded &= ApplyAttachmentState(
					Repair.Component,
					Repair.OriginalParent,
					Repair.OriginalSocket,
					Repair.OriginalRelativeTransform,
					Repair.bWasRegistered,
					Repair.OriginalParentMembershipCount);
			}
		}
		if (Plan.bRootApplied)
		{
			bSucceeded &= Plan.Actor->SetRootComponent(Plan.OriginalRoot);
		}
		for (int32 PropertyIndex = Plan.Properties.Num() - 1;
			PropertyIndex >= 0;
			--PropertyIndex)
		{
			FPropertyRepair& Repair = Plan.Properties[PropertyIndex];
			if (Repair.bPropertyApplied)
			{
				Repair.Property->SetObjectPropertyValue_InContainer(Plan.Actor, Repair.OriginalValue);
				bSucceeded &=
					Repair.Property->GetObjectPropertyValue_InContainer(Plan.Actor)
					== Repair.OriginalValue;
			}
		}
	}
	return bSucceeded;
}
}
