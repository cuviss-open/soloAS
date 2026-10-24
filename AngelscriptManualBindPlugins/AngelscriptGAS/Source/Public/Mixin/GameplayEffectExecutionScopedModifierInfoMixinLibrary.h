#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"

#include "GameplayEffectExecutionScopedModifierInfoMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FGameplayEffectExecutionScopedModifierInfo"))
class ANGELSCRIPTGAS_API UGameplayEffectExecutionScopedModifierInfoMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static FGameplayEffectAttributeCaptureDefinition& GetCapturedAttribute(FGameplayEffectExecutionScopedModifierInfo& Handle)
	{
		return Handle.CapturedAttribute;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static FGameplayTag& GetTransientAggregatorIdentifier(FGameplayEffectExecutionScopedModifierInfo& Handle)
	{
		return Handle.TransientAggregatorIdentifier;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Execution")
	static EGameplayEffectScopedModifierAggregatorType& GetAggregatorType(FGameplayEffectExecutionScopedModifierInfo& Handle)
	{
		return Handle.AggregatorType;
	}
};
