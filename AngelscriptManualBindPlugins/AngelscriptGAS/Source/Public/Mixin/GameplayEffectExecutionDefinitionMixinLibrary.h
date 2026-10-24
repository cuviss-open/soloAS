#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"

#include "GameplayEffectExecutionDefinitionMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FGameplayEffectExecutionDefinition"))
class ANGELSCRIPTGAS_API UGameplayEffectExecutionDefinitionMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static void SetCalculationClass(FGameplayEffectExecutionDefinition& Def, const TSubclassOf<UGameplayEffectExecutionCalculation>& CalculationClass)
	{
		Def.CalculationClass = CalculationClass;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static TSubclassOf<UGameplayEffectExecutionCalculation> GetCalculationClass(const FGameplayEffectExecutionDefinition& Def)
	{
		return Def.CalculationClass;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static void SetPassedInTags(FGameplayEffectExecutionDefinition& Def, const FGameplayTagContainer& PassedInTags)
	{
		Def.PassedInTags = PassedInTags;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static FGameplayTagContainer& GetPassedInTags(FGameplayEffectExecutionDefinition& Def)
	{
		return Def.PassedInTags;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static void SetCalculationModifiers(FGameplayEffectExecutionDefinition& Def, const TArray<FGameplayEffectExecutionScopedModifierInfo>& CalculationModifiers)
	{
		Def.CalculationModifiers = CalculationModifiers;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static void AddCalculationModifier(FGameplayEffectExecutionDefinition& Def, const FGameplayEffectExecutionScopedModifierInfo& CalculationModifier)
	{
		Def.CalculationModifiers.Add(CalculationModifier);
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static TArray<FGameplayEffectExecutionScopedModifierInfo>& GetCalculationModifiers(FGameplayEffectExecutionDefinition& Def)
	{
		return Def.CalculationModifiers;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static void SetConditionalGameplayEffects(FGameplayEffectExecutionDefinition& Def, const TArray<FConditionalGameplayEffect>& ConditionalGameplayEffects)
	{
		Def.ConditionalGameplayEffects = ConditionalGameplayEffects;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static void AddConditionalGameplayEffect(FGameplayEffectExecutionDefinition& Def, const FConditionalGameplayEffect& ConditionalGameplayEffect)
	{
		Def.ConditionalGameplayEffects.Add(ConditionalGameplayEffect);
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static TArray<FConditionalGameplayEffect>& GetConditionalGameplayEffects(FGameplayEffectExecutionDefinition& Def)
	{
		return Def.ConditionalGameplayEffects;
	}
};
