// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Blueprint/AngelscriptBlueprintActorComponentOrderingAdapter.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

namespace
{
using EActorComponentOrderingResult = EAngelscriptBlueprintActorComponentOrderingResult;

EActorComponentOrderingResult ValidateComponentList(
	AActor* Actor,
	const TArray<UActorComponent*>& Components)
{
	TSet<UActorComponent*> Seen;
	for (UActorComponent* Component : Components)
	{
		if (Component == nullptr)
			return EActorComponentOrderingResult::NullComponent;
		if (!IsValid(Component))
			return EActorComponentOrderingResult::InvalidComponent;
		if (Seen.Contains(Component))
			return EActorComponentOrderingResult::DuplicateComponent;
		Seen.Add(Component);
		if (Component->GetOwner() != Actor)
			return EActorComponentOrderingResult::WrongOwner;
		if (Component->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			return EActorComponentOrderingResult::DestroyedComponent;
	}
	return EActorComponentOrderingResult::Accepted;
}
}

EAngelscriptBlueprintActorComponentOrderingResult
FAngelscriptBlueprintActorComponentOrderingAdapter::Validate(
	const FAngelscriptBlueprintActorComponentOrderingRequest& Request)
{
	if (Request.Actor == nullptr)
		return EActorComponentOrderingResult::NullActor;
	if (!IsValid(Request.Actor))
		return EActorComponentOrderingResult::InvalidActor;
	if (Request.Before.IsEmpty() || Request.After.IsEmpty())
		return EActorComponentOrderingResult::EmptySnapshot;

	const EActorComponentOrderingResult BeforeResult = ValidateComponentList(Request.Actor, Request.Before);
	if (BeforeResult != EActorComponentOrderingResult::Accepted)
		return BeforeResult;
	const EActorComponentOrderingResult AfterResult = ValidateComponentList(Request.Actor, Request.After);
	if (AfterResult != EActorComponentOrderingResult::Accepted)
		return AfterResult;
	if (Request.Before.Num() != Request.After.Num())
		return EActorComponentOrderingResult::SnapshotSizeMismatch;

	TSet<UActorComponent*> SeenOld;
	TSet<UActorComponent*> SeenNew;
	for (const TPair<UActorComponent*, UActorComponent*>& Pair : Request.Replacements)
	{
		if (Pair.Key == nullptr || Pair.Value == nullptr)
			return EActorComponentOrderingResult::ReplacementMissing;
		if (SeenOld.Contains(Pair.Key) || SeenNew.Contains(Pair.Value))
			return EActorComponentOrderingResult::ReplacementUnexpected;
		SeenOld.Add(Pair.Key);
		SeenNew.Add(Pair.Value);
		if (!IsValid(Pair.Key) || !IsValid(Pair.Value))
			return EActorComponentOrderingResult::InvalidComponent;
		if (Pair.Key == Pair.Value)
			return EActorComponentOrderingResult::ReplacementIdentityMismatch;
		if (!Request.Before.Contains(Pair.Key))
			return EActorComponentOrderingResult::ReplacementUnexpected;
		if (!Request.After.Contains(Pair.Value))
			return EActorComponentOrderingResult::ReplacementUnexpected;
	}

	for (int32 Index = 0; Index < Request.Before.Num(); ++Index)
	{
		UActorComponent* const* Replacement = Request.Replacements.Find(Request.Before[Index]);
		if (Replacement == nullptr)
			return EActorComponentOrderingResult::ReplacementMissing;
		if (*Replacement != Request.After[Index])
			return EActorComponentOrderingResult::OrderMismatch;
	}
	if (Request.Replacements.Num() != Request.Before.Num())
		return EActorComponentOrderingResult::ReplacementUnexpected;
	return EActorComponentOrderingResult::Accepted;
}

EAngelscriptBlueprintActorComponentOrderingResult
FAngelscriptBlueprintActorComponentOrderingAdapter::BuildOrderedReplacementList(
	const FAngelscriptBlueprintActorComponentOrderingRequest& Request,
	TArray<UActorComponent*>& OutOrderedComponents)
{
	OutOrderedComponents.Reset();
	const EActorComponentOrderingResult ValidationResult = Validate(Request);
	if (ValidationResult != EActorComponentOrderingResult::Accepted)
		return ValidationResult;

	OutOrderedComponents.Reserve(Request.Before.Num());
	for (UActorComponent* Component : Request.Before)
	{
		UActorComponent* const* Replacement = Request.Replacements.Find(Component);
		if (Replacement == nullptr)
		{
			OutOrderedComponents.Reset();
			return EActorComponentOrderingResult::ReplacementMissing;
		}
		OutOrderedComponents.Add(*Replacement);
	}
	return EActorComponentOrderingResult::Accepted;
}
