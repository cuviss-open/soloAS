#pragma once

#include "CoreMinimal.h"

#include "FunctionLibraries/AngelscriptGameplayEffectUtils.h"

#include "GameplayEffectCustomExecutionParametersMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FGameplayEffectCustomExecutionParameters"))
class ANGELSCRIPTGAS_API UGameplayEffectCustomExecutionParametersMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	/** Simple accessor to owning gameplay spec */
	UFUNCTION(ScriptCallable)
	static const FGameplayEffectSpec& GetOwningSpec(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters)
	{
		return CustomExecutionParameters.GetOwningSpec();
	}

	/** Non const access. Be careful with this, especially when modifying a spec after attribute capture. */
	UFUNCTION(ScriptCallable)
	static FGameplayEffectSpec& GetOwningSpecForPreExecuteMod(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters)
	{
		return *CustomExecutionParameters.GetOwningSpecForPreExecuteMod();
	}

	/** Simple accessor to target ability system component */
	UFUNCTION(ScriptCallable)
	static UAbilitySystemComponent* GetTargetAbilitySystemComponent(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters)
	{
		return CustomExecutionParameters.GetTargetAbilitySystemComponent();
	}

	/** Simple accessor to source ability system component (could be null!) */
	UFUNCTION(ScriptCallable)
	static UAbilitySystemComponent* GetSourceAbilitySystemComponent(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters)
	{
		return CustomExecutionParameters.GetSourceAbilitySystemComponent();
	}

	/** Simple accessor to the Passed In Tags to this execution */
	UFUNCTION(ScriptCallable)
	static const FGameplayTagContainer& GetPassedInTags(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters)
	{
		return CustomExecutionParameters.GetPassedInTags();
	}

	UFUNCTION(ScriptCallable)
	static TArray<FActiveGameplayEffectHandle> GetIgnoreHandles(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters)
	{
		return CustomExecutionParameters.GetIgnoreHandles();
	}

	UFUNCTION(ScriptCallable)
	static FPredictionKey GetPredictionKey(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters)
	{
		return CustomExecutionParameters.GetPredictionKey();
	}

	/**
	 * Attempts to calculate the magnitude of a captured attribute given the specified parameters. Can fail if the gameplay spec doesn't have
	 * a valid capture for the attribute.
	 *
	 * @param InCaptureDef	Attribute definition to attempt to calculate the magnitude of
	 * @param InEvalParams	Parameters to evaluate the attribute under
	 * @param OutMagnitude	[OUT] Computed magnitude
	 *
	 * @return True if the magnitude was successfully calculated, false if it was not
	 */
	UFUNCTION(ScriptCallable)
	static bool AttemptCalculateCapturedAttributeMagnitude(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters, const FGameplayEffectAttributeCaptureDefinition& InCaptureDef, const FGameplayEffectExecutionParameters& InParams, float& OutMagnitude)
	{
		return CustomExecutionParameters.AttemptCalculateCapturedAttributeMagnitude(InCaptureDef, InParams.WrappedParams, OutMagnitude);
	}

	/**
	 * Attempts to calculate the magnitude of a captured attribute given the specified parameters, including a starting base value.
	 * Can fail if the gameplay spec doesn't have a valid capture for the attribute.
	 *
	 * @param InCaptureDef	Attribute definition to attempt to calculate the magnitude of
	 * @param InEvalParams	Parameters to evaluate the attribute under
	 * @param InBaseValue	Base value to evaluate the attribute under
	 * @param OutMagnitude	[OUT] Computed magnitude
	 *
	 * @return True if the magnitude was successfully calculated, false if it was not
	 */
	UFUNCTION(ScriptCallable)
	static bool AttemptCalculateCapturedAttributeMagnitudeWithBase(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters, const FGameplayEffectAttributeCaptureDefinition& InCaptureDef, const FGameplayEffectExecutionParameters& InParams, float InBaseValue, float& OutMagnitude)
	{
		return CustomExecutionParameters.AttemptCalculateCapturedAttributeMagnitudeWithBase(InCaptureDef, InParams.WrappedParams, InBaseValue, OutMagnitude);
	}

	/**
	 * Attempts to calculate the base value of a captured attribute given the specified parameters. Can fail if the gameplay spec doesn't have
	 * a valid capture for the attribute.
	 *
	 * @param InCaptureDef	Attribute definition to attempt to calculate the base value of
	 * @param OutBaseValue	[OUT] Computed base value
	 *
	 * @return True if the base value was successfully calculated, false if it was not
	 */
	UFUNCTION(ScriptCallable)
	static bool AttemptCalculateCapturedAttributeBaseValue(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters, const FGameplayEffectAttributeCaptureDefinition& InCaptureDef, float& OutBaseValue)
	{
		return CustomExecutionParameters.AttemptCalculateCapturedAttributeBaseValue(InCaptureDef, OutBaseValue);
	}

	/**
	 * Attempts to calculate the bonus magnitude of a captured attribute given the specified parameters. Can fail if the gameplay spec doesn't have
	 * a valid capture for the attribute.
	 *
	 * @param InCaptureDef		Attribute definition to attempt to calculate the bonus magnitude of
	 * @param InEvalParams		Parameters to evaluate the attribute under
	 * @param OutBonusMagnitude	[OUT] Computed bonus magnitude
	 *
	 * @return True if the bonus magnitude was successfully calculated, false if it was not
	 */
	UFUNCTION(ScriptCallable)
	static bool AttemptCalculateCapturedAttributeBonusMagnitude(const FGameplayEffectCustomExecutionParameters& CustomExecutionParameters, const FGameplayEffectAttributeCaptureDefinition& InCaptureDef, const FGameplayEffectExecutionParameters& InParams, float& OutBonusMagnitude)
	{
		return CustomExecutionParameters.AttemptCalculateCapturedAttributeBonusMagnitude(InCaptureDef, InParams.WrappedParams, OutBonusMagnitude);
	}

	/**
	* Attempts to calculate the magnitude of a transient aggregator given the specified parameters.
	*
	* @param InAggregatorIdentifier	Tag identifying the transient aggregator to attempt to calculate the magnitude of
	* @param InParams				Parameters to evaluate the aggregator under
	* @param OutMagnitude			[OUT] Computed magnitude
	*/
	UFUNCTION(ScriptCallable)
	static void AttemptCalculateTransientAggregatorMagnitude(const FGameplayEffectCustomExecutionParameters& ExecutionParameters, const FGameplayTag& InAggregatorIdentifier, const FGameplayEffectExecutionParameters& InParams, float& OutMagnitude)
	{
		ExecutionParameters.AttemptCalculateTransientAggregatorMagnitude(InAggregatorIdentifier, InParams.WrappedParams, OutMagnitude);
	}
};
