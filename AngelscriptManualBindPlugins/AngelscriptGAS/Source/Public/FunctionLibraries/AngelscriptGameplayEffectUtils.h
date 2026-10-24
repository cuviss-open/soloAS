#pragma once

#include "CoreMinimal.h"

#include "Kismet/BlueprintFunctionLibrary.h"
#include "GameplayEffectAggregator.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GameplayEffectTypes.h"

#include "AngelscriptGameplayEffectUtils.generated.h"

USTRUCT(BlueprintType)
struct ANGELSCRIPTGAS_API FGameplayEffectExecutionParameters
{
	GENERATED_BODY()

public:
	FAggregatorEvaluateParameters WrappedParams;
};

UCLASS(Meta = (ScriptMixin = "FGameplayEffectExecutionParameters"))
class ANGELSCRIPTGAS_API UGameplayEffectExecutionParametersMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(ScriptCallable)
	static TArray<FActiveGameplayEffectHandle>& GetIgnoreHandles(FGameplayEffectExecutionParameters& Data)
	{
		return Data.WrappedParams.IgnoreHandles;
	}

	UFUNCTION(ScriptCallable)
	static FGameplayTagContainer& GetAppliedSourceTagFilter(FGameplayEffectExecutionParameters& Data)
	{
		return Data.WrappedParams.AppliedSourceTagFilter;
	}

	UFUNCTION(ScriptCallable)
	static FGameplayTagContainer& GetAppliedTargetTagFilter(FGameplayEffectExecutionParameters& Data)
	{
		return Data.WrappedParams.AppliedTargetTagFilter;
	}

	UFUNCTION(ScriptCallable)
	static bool GetIncludePredictiveMods(const FGameplayEffectExecutionParameters& Data)
	{
		return Data.WrappedParams.IncludePredictiveMods;
	}

	UFUNCTION(ScriptCallable)
	static void SetIncludePredictiveMods(FGameplayEffectExecutionParameters& Data, bool bShouldIncludePredictiveMods)
	{
		Data.WrappedParams.IncludePredictiveMods = bShouldIncludePredictiveMods;
	}

	UFUNCTION(ScriptCallable)
	static void SetCapturedSourceTagsFromSpec(FGameplayEffectExecutionParameters& Data, FGameplayEffectSpec& Spec)
	{
		Data.WrappedParams.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
		Data.WrappedParams.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();
	}
};

UCLASS()
class ANGELSCRIPTGAS_API UAngelscriptGameplayEffectUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(ScriptCallable, Category = "Angelscript|Gameplay Effect Helpers")
	static FGameplayEffectAttributeCaptureDefinition CaptureGameplayAttribute(TSubclassOf<UAttributeSet> AttributeSetType, FName AttributeName, EGameplayEffectAttributeCaptureSource InSource, bool bIsSnapshot)
	{
		if (ensureMsgf(AttributeSetType.Get(), TEXT("Struct from Angelscript should never be null!")))
		{
			FProperty* AttributeProperty = FindFProperty<FProperty>(AttributeSetType, AttributeName);
			if (ensureAlwaysMsgf(AttributeProperty, TEXT("Cannot find property %s on struct %s"), *AttributeName.ToString(), *AttributeSetType->GetName()))
			{
				return FGameplayEffectAttributeCaptureDefinition(FGameplayAttribute(AttributeProperty), InSource, bIsSnapshot);
			}
		}

		return FGameplayEffectAttributeCaptureDefinition();
	}

	UFUNCTION(ScriptCallable, Category = "Angelscript|Gameplay Effect Helpers")
	static FGameplayModifierEvaluatedData MakeGameplayModifierEvaluationData(const FGameplayAttribute& InAttribute, TEnumAsByte<EGameplayModOp::Type> InModOp, float InMagnitude)
	{
		return FGameplayModifierEvaluatedData(InAttribute, InModOp, InMagnitude, FActiveGameplayEffectHandle());
	}

	UFUNCTION(ScriptCallable, Category = "Angelscript|Gameplay Effect Helpers")
	static FGameplayEffectExecutionScopedModifierInfo MakeGameplayEffectExecutionScopedModifierInfo(const FGameplayEffectAttributeCaptureDefinition& InCaptureDef)
	{
		return FGameplayEffectExecutionScopedModifierInfo(InCaptureDef);
	}
};
