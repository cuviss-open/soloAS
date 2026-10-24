#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"

#include "InheritedTagContainerMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FInheritedTagContainer"))
class ANGELSCRIPTGAS_API UInheritedTagContainerMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Inherited Tag Container")
	static void AddTag(FInheritedTagContainer& Container, const FGameplayTag& TagToAdd)
	{
		Container.AddTag(TagToAdd);
	}

	UFUNCTION(BlueprintCallable, Category = "Inherited Tag Container")
	static void RemoveTag(FInheritedTagContainer& Container, const FGameplayTag& TagToRemove)
	{
		Container.RemoveTag(TagToRemove);
	}
};
