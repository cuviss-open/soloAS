#pragma once

#include "CoreMinimal.h"

#include "GameplayEffectExecutionCalculation.h"

#include "GameplayEffectCustomExecutionOutputMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FGameplayEffectCustomExecutionOutput"))
class ANGELSCRIPTGAS_API UGameplayEffectCustomExecutionOutputMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	/** Mark that the execution has manually handled the stack count and the GE system should not attempt to automatically act upon it for emitted modifiers */
	UFUNCTION(ScriptCallable)
	static void MarkStackCountHandledManually(FGameplayEffectCustomExecutionOutput& CustomExecutionOutput) 
	{
		CustomExecutionOutput.MarkStackCountHandledManually(); 
	}

	/** Simple accessor for determining whether the execution has manually handled the stack count or not */
	UFUNCTION(ScriptCallable)
	static bool IsStackCountHandledManually(FGameplayEffectCustomExecutionOutput& CustomExecutionOutput) 
	{ 
		return CustomExecutionOutput.IsStackCountHandledManually(); 
	}

	/** Accessor for determining if GameplayCue events have already been handled */
	UFUNCTION(ScriptCallable)
	static bool AreGameplayCuesHandledManually(FGameplayEffectCustomExecutionOutput& CustomExecutionOutput) 
	{ 
		return CustomExecutionOutput.AreGameplayCuesHandledManually(); 
	}

	/** Mark that the execution wants conditional gameplay effects to trigger */
	UFUNCTION(ScriptCallable)
	static void MarkConditionalGameplayEffectsToTrigger(FGameplayEffectCustomExecutionOutput& CustomExecutionOutput) 
	{ 
		CustomExecutionOutput.MarkConditionalGameplayEffectsToTrigger(); 
	}

	/** Mark that the execution wants conditional gameplay effects to trigger */
	UFUNCTION(ScriptCallable)
	static void MarkGameplayCuesHandledManually(FGameplayEffectCustomExecutionOutput& CustomExecutionOutput) 
	{
		CustomExecutionOutput.MarkGameplayCuesHandledManually(); 
	}

	/** Simple accessor for determining whether the execution wants conditional gameplay effects to trigger or not */
	UFUNCTION(ScriptCallable)
	static bool ShouldTriggerConditionalGameplayEffects(FGameplayEffectCustomExecutionOutput& CustomExecutionOutput) 
	{ 
		return CustomExecutionOutput.ShouldTriggerConditionalGameplayEffects(); 
	}

	/** Add the specified evaluated data to the execution's output modifiers */
	UFUNCTION(ScriptCallable)
	static void AddOutputModifier(FGameplayEffectCustomExecutionOutput& CustomExecutionOutput, const FGameplayModifierEvaluatedData& InOutputMod) 
	{ 
		CustomExecutionOutput.AddOutputModifier(InOutputMod); 
	}

	/** Simple accessor to output modifiers of the execution */
	UFUNCTION(ScriptCallable)
	static const TArray<FGameplayModifierEvaluatedData>& GetOutputModifiers(FGameplayEffectCustomExecutionOutput& CustomExecutionOutput) 
	{
		return CustomExecutionOutput.GetOutputModifiers(); 
	}

	/** Returns direct access to output modifiers of the execution (avoid copy) */
	UFUNCTION(ScriptCallable)
	static TArray<FGameplayModifierEvaluatedData>& GetOutputModifiersRef(FGameplayEffectCustomExecutionOutput& CustomExecutionOutput) 
	{
		return CustomExecutionOutput.GetOutputModifiersRef(); 
	}
};
