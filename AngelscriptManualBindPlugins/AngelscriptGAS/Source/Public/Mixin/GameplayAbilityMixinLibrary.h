#pragma once

#include "CoreMinimal.h"

#include "Abilities/GameplayAbility.h"

#include "GameplayAbilityMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "UGameplayAbility"))
class ANGELSCRIPTGAS_API UGameplayAbilityMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static UObject* GetSourceObject(const UGameplayAbility* Ability, FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo& ActorInfo)
	{
		return Ability ? Ability->GetSourceObject(Handle, &ActorInfo) : nullptr;
	}
};
