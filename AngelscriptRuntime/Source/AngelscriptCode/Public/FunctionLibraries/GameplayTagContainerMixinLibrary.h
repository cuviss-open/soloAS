#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"

#include "GameplayTagContainerMixinLibrary.generated.h"

/**
 * ScriptMixin library to bind functions on FGameplayTagContainer
 * that are not BlueprintCallable by default.
 */
UCLASS(Meta = (ScriptMixin = "FGameplayTagContainer"))
class UGameplayTagContainerMixinLibrary : public UObject
{
	GENERATED_BODY()
public:
	UFUNCTION(ScriptCallable)
	static void AppendTags(FGameplayTagContainer& GameplayTagContainer, const FGameplayTagContainer& TagsToAdd)
	{
		GameplayTagContainer.AppendTags(TagsToAdd);
	}

	UFUNCTION(ScriptCallable)
	static void AddTag(FGameplayTagContainer& GameplayTagContainer, const FGameplayTag& TagToAdd)
	{
		GameplayTagContainer.AddTag(TagToAdd);
	}

	UFUNCTION(ScriptCallable)
	static void AddTagFast(FGameplayTagContainer& GameplayTagContainer, const FGameplayTag& TagToAdd)
	{
		GameplayTagContainer.AddTagFast(TagToAdd);
	}

	UFUNCTION(ScriptCallable)
	static void AddLeafTag(FGameplayTagContainer& GameplayTagContainer, const FGameplayTag& TagToAdd)
	{
		GameplayTagContainer.AddLeafTag(TagToAdd);
	}

	UFUNCTION(ScriptCallable)
	static bool RemoveTag(FGameplayTagContainer& GameplayTagContainer, const FGameplayTag& TagToRemove)
	{
		return GameplayTagContainer.RemoveTag(TagToRemove);
	}

	UFUNCTION(ScriptCallable)
	static void RemoveTags(FGameplayTagContainer& GameplayTagContainer, const FGameplayTagContainer& TagsToRemove)
	{
		GameplayTagContainer.RemoveTags(TagsToRemove);
	}

	UFUNCTION(ScriptCallable)
	static bool HasTag(const FGameplayTagContainer& GameplayTagContainer, const FGameplayTag& TagToCheck)
	{
		return GameplayTagContainer.HasTag(TagToCheck);
	}

	UFUNCTION(ScriptCallable)
	static bool HasTagExact(const FGameplayTagContainer& GameplayTagContainer, const FGameplayTag& TagToCheck)
	{
		return GameplayTagContainer.HasTagExact(TagToCheck);
	}

	UFUNCTION(ScriptCallable)
	static bool HasAny(const FGameplayTagContainer& GameplayTagContainer, const FGameplayTagContainer& ContainerToCheck)
	{
		return GameplayTagContainer.HasAny(ContainerToCheck);
	}

	UFUNCTION(ScriptCallable)
	static bool HasAnyExact(const FGameplayTagContainer& GameplayTagContainer, const FGameplayTagContainer& ContainerToCheck)
	{
		return GameplayTagContainer.HasAnyExact(ContainerToCheck);
	}

	UFUNCTION(ScriptCallable)
	static bool HasAll(const FGameplayTagContainer& GameplayTagContainer, const FGameplayTagContainer& ContainerToCheck)
	{
		return GameplayTagContainer.HasAll(ContainerToCheck);
	}

	UFUNCTION(ScriptCallable)
	static bool HasAllExact(const FGameplayTagContainer& GameplayTagContainer, const FGameplayTagContainer& ContainerToCheck)
	{
		return GameplayTagContainer.HasAllExact(ContainerToCheck);
	}

	UFUNCTION(ScriptCallable)
	static int32 Num(const FGameplayTagContainer& GameplayTagContainer)
	{
		return GameplayTagContainer.Num();
	}

	UFUNCTION(ScriptCallable)
	static bool IsValid(const FGameplayTagContainer& GameplayTagContainer)
	{
		return GameplayTagContainer.IsValid();
	}

	UFUNCTION(ScriptCallable)
	static bool IsEmpty(const FGameplayTagContainer& GameplayTagContainer)
	{
		return GameplayTagContainer.IsEmpty();
	}

	UFUNCTION(ScriptCallable)
	static FGameplayTagContainer GetGameplayTagParents(const FGameplayTagContainer& GameplayTagContainer)
	{
		return GameplayTagContainer.GetGameplayTagParents();
	}

	UFUNCTION(ScriptCallable)
	static FGameplayTagContainer Filter(const FGameplayTagContainer& GameplayTagContainer, const FGameplayTagContainer& OtherContainer)
	{
		return GameplayTagContainer.Filter(OtherContainer);
	}

	UFUNCTION(ScriptCallable)
	static FGameplayTagContainer FilterExact(const FGameplayTagContainer& GameplayTagContainer, const FGameplayTagContainer& OtherContainer)
	{
		return GameplayTagContainer.FilterExact(OtherContainer);
	}

	UFUNCTION(ScriptCallable)
	static bool MatchesQuery(const FGameplayTagContainer& GameplayTagContainer, const FGameplayTagQuery& Query)
	{
		return GameplayTagContainer.MatchesQuery(Query);
	}

	UFUNCTION(ScriptCallable)
	static FGameplayTag Last(const FGameplayTagContainer& GameplayTagContainer)
	{
		return GameplayTagContainer.Last();
	}

	UFUNCTION(ScriptCallable)
	static FGameplayTag First(const FGameplayTagContainer& GameplayTagContainer)
	{
		return GameplayTagContainer.First();
	}
};
