#pragma once

#include "CoreMinimal.h"

#include "AbilitySystemComponent.h"

#include "AbilitySystemComponentMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "UAbilitySystemComponent"))
class ANGELSCRIPTGAS_API UAbilitySystemComponentMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(ScriptCallable)
	static const TArray<UAttributeSet*>& GetSpawnedAttributes(const UAbilitySystemComponent* AbilitySystemComponent)
	{
		check(AbilitySystemComponent != nullptr);
		return AbilitySystemComponent->GetSpawnedAttributes();
	}

	UFUNCTION(ScriptCallable)
	static void AddSpawnedAttribute(UAbilitySystemComponent* AbilitySystemComponent, UAttributeSet* Attribute)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->AddSpawnedAttribute(Attribute);
		}
	}

	UFUNCTION(ScriptCallable)
	static void RemoveSpawnedAttribute(UAbilitySystemComponent* AbilitySystemComponent, UAttributeSet* Attribute)
	{
		if (AbilitySystemComponent)
		{
			return AbilitySystemComponent->RemoveSpawnedAttribute(Attribute);
		}
	}

	UFUNCTION(ScriptCallable)
	static void RemoveAllSpawnedAttributes(UAbilitySystemComponent* AbilitySystemComponent)
	{
		if (AbilitySystemComponent)
		{
			return AbilitySystemComponent->RemoveAllSpawnedAttributes();
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Cues")
	static void ExecuteGameplayCueWithEffectContext(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTag GameplayCueTag, const FGameplayEffectContextHandle& EffectContext)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->ExecuteGameplayCue(GameplayCueTag, EffectContext);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Cues")
	static void ExecuteGameplayCueWithParameters(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTag GameplayCueTag, const FGameplayCueParameters& GameplayCueParameters)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->ExecuteGameplayCue(GameplayCueTag, GameplayCueParameters);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Cues")
	static void AddGameplayCueWithEffectContext(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTag GameplayCueTag, const FGameplayEffectContextHandle& EffectContext)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->AddGameplayCue(GameplayCueTag, EffectContext);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Cues")
	static void AddGameplayCueWithParameters(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTag GameplayCueTag, const FGameplayCueParameters& GameplayCueParameters)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->AddGameplayCue(GameplayCueTag, GameplayCueParameters);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Cues")
	static void RemoveGameplayCue(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTag GameplayCueTag)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->RemoveGameplayCue(GameplayCueTag);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Cues")
	static void RemoveAllGameplayCues(UAbilitySystemComponent* AbilitySystemComponent)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->RemoveAllGameplayCues();
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Cues")
	static void InitDefaultGameplayCueParameters(UAbilitySystemComponent* AbilitySystemComponent, FGameplayCueParameters& Parameters)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->InitDefaultGameplayCueParameters(Parameters);
		}
	}

	UFUNCTION(ScriptCallable)
	static FGameplayAbilitySpecHandle GiveAbility(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayAbilitySpec& Spec)
	{
		if (AbilitySystemComponent)
		{
			return AbilitySystemComponent->GiveAbility(Spec);
		}
		return FGameplayAbilitySpecHandle();
	}

	UFUNCTION(ScriptCallable)
	static FGameplayAbilitySpecHandle GiveAbilityAndActivateOnce(UAbilitySystemComponent* AbilitySystemComponent, FGameplayAbilitySpec& Spec)
	{
		if (AbilitySystemComponent)
		{
			return AbilitySystemComponent->GiveAbilityAndActivateOnce(Spec);
		}
		return FGameplayAbilitySpecHandle();
	}

	UFUNCTION(ScriptCallable)
	static FGameplayAbilitySpecHandle GiveAbilityAndActivateOnceWithEventData(UAbilitySystemComponent* AbilitySystemComponent, FGameplayAbilitySpec& Spec, const FGameplayEventData& EventData)
	{
		if (AbilitySystemComponent)
		{
			return AbilitySystemComponent->GiveAbilityAndActivateOnce(Spec, &EventData);
		}
		return FGameplayAbilitySpecHandle();
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Tags")
	static void AddLooseGameplayTag(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTag& GameplayTag, const int32 Count = 1, EGameplayTagReplicationState TagRepState = EGameplayTagReplicationState::None)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->AddLooseGameplayTag(GameplayTag, Count, TagRepState);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Tags")
	static void AddLooseGameplayTags(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTagContainer& GameplayTags, const int32 Count = 1, EGameplayTagReplicationState TagRepState = EGameplayTagReplicationState::None)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->AddLooseGameplayTags(GameplayTags, Count, TagRepState);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Tags")
	static void RemoveLooseGameplayTag(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTag& GameplayTag, const int32 Count = 1, EGameplayTagReplicationState TagRepState = EGameplayTagReplicationState::None)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->RemoveLooseGameplayTag(GameplayTag, Count, TagRepState);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Tags")
	static void RemoveLooseGameplayTags(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTagContainer& GameplayTags, const int32 Count = 1, EGameplayTagReplicationState TagRepState = EGameplayTagReplicationState::None)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->RemoveLooseGameplayTags(GameplayTags, Count, TagRepState);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Tags")
	static void SetLooseGameplayTagCount(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTag& GameplayTag, const int32 NewCount, EGameplayTagReplicationState TagRepState = EGameplayTagReplicationState::None)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->SetLooseGameplayTagCount(GameplayTag, NewCount, TagRepState);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Gameplay Tags")
	static void GetOwnedGameplayTags(const UAbilitySystemComponent* AbilitySystemComponent, FGameplayTagContainer& TagContainer)
	{
		if (AbilitySystemComponent)
		{
			AbilitySystemComponent->GetOwnedGameplayTags(TagContainer);
		}
	}

	UFUNCTION(ScriptCallable)
	static bool FindAbilitySpecFromHandle(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayAbilitySpecHandle Handle, FGameplayAbilitySpec& OutSpec)
	{
		if (AbilitySystemComponent)
		{
			if (const FGameplayAbilitySpec* Result = AbilitySystemComponent->FindAbilitySpecFromHandle(Handle))
			{
				OutSpec = *Result;
				return true;
			}
		}
		return false;
	}

	UFUNCTION(ScriptCallable)
	static bool FindAbilitySpecFromClass(UAbilitySystemComponent* AbilitySystemComponent, const TSubclassOf<UGameplayAbility> InAbilityClass, FGameplayAbilitySpec& OutSpec)
	{
		if (AbilitySystemComponent)
		{
			if (const FGameplayAbilitySpec* Result = AbilitySystemComponent->FindAbilitySpecFromClass(InAbilityClass))
			{
				OutSpec = *Result;
				return true;
			}
		}
		return false;
	}

	UFUNCTION(ScriptCallable)
	static bool FindAbilitySpecFromInputID(UAbilitySystemComponent* AbilitySystemComponent, const int32 InputID, FGameplayAbilitySpec& OutSpec)
	{
		if (AbilitySystemComponent)
		{
			if (const FGameplayAbilitySpec* Result = AbilitySystemComponent->FindAbilitySpecFromInputID(InputID))
			{
				OutSpec = *Result;
				return true;
			}
		}
		return false;
	}

	UFUNCTION(ScriptCallable)
	static void MarkAbilitySpecDirty(UAbilitySystemComponent* AbilitySystemComponent, FGameplayAbilitySpec& Spec, const bool bWasAddOrRemove = false)
	{
		if (AbilitySystemComponent)
		{
			if (FGameplayAbilitySpec* ExistingSpec = AbilitySystemComponent->FindAbilitySpecFromHandle(Spec.Handle))
			{
				const int32 ReplicationID = ExistingSpec->ReplicationID;
				const int32 ReplicationKey = ExistingSpec->ReplicationKey;
				const int32 MostRecentArrayReplicationKey = ExistingSpec->MostRecentArrayReplicationKey;

				*ExistingSpec = Spec;
				ExistingSpec->ReplicationID = ReplicationID;
				ExistingSpec->ReplicationKey = ReplicationKey;
				ExistingSpec->MostRecentArrayReplicationKey = MostRecentArrayReplicationKey;

				AbilitySystemComponent->MarkAbilitySpecDirty(*ExistingSpec, bWasAddOrRemove);
			}
		}
	}

	UFUNCTION(ScriptCallable)
	static void SendGameplayEvent(UAbilitySystemComponent* AbilitySystemComponent, const FGameplayTag EventTag, const FGameplayEventData& Payload)
	{
		if (AbilitySystemComponent)
		{
			FScopedPredictionWindow NewScopedWindow(AbilitySystemComponent, true);
			AbilitySystemComponent->HandleGameplayEvent(EventTag, &Payload);
		}
	}
};
