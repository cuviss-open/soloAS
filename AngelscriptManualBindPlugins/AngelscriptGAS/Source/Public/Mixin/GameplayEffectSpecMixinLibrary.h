#pragma once

#include "CoreMinimal.h"

#include "GameplayEffect.h"
#include "GameplayEffectExecutionCalculation.h"

#include "GameplayEffectSpecMixinLibrary.generated.h"

// Wrapper type for FTagContainerAggregator
// @TODO: We should make a binding instead.
USTRUCT(BlueprintType)
struct FAngelscriptTagContainerAggregator
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tags")
	FGameplayTagContainer CapturedActorTags;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tags")
	FGameplayTagContainer CapturedSpecTags;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Tags")
	FGameplayTagContainer AggregatedTags;

public:
	FAngelscriptTagContainerAggregator()
		: CapturedActorTags()
		, CapturedSpecTags()
		, AggregatedTags()
	{
	}

	FAngelscriptTagContainerAggregator(const FTagContainerAggregator& SourceContainer)
		: CapturedActorTags(SourceContainer.GetActorTags())
		, CapturedSpecTags(SourceContainer.GetSpecTags())
		, AggregatedTags(*SourceContainer.GetAggregatedTags())
	{
	}
};

UCLASS(Meta = (ScriptMixin = "FGameplayEffectSpec"))
class ANGELSCRIPTGAS_API UGameplayEffectSpecMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec")
	static void SetContext(UPARAM(ref) FGameplayEffectSpec& Spec, FGameplayEffectContextHandle NewEffectContextHandle)
	{
		Spec.SetContext(NewEffectContextHandle);
	}
	
	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec")
	static void SetByCallerMagnitude(UPARAM(ref) FGameplayEffectSpec& Spec, FGameplayTag DataTag, float Magnitude)
	{
		Spec.SetSetByCallerMagnitude(DataTag, Magnitude);
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec")
	static void SetLevel(UPARAM(ref) FGameplayEffectSpec& Spec, float InLevel)
	{
		Spec.SetLevel(InLevel);
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec")
	static void CaptureAttributeDataFromTarget(UPARAM(ref) FGameplayEffectSpec& Spec, UAbilitySystemComponent* TargetAbilitySystemComponent)
	{
		Spec.CaptureAttributeDataFromTarget(TargetAbilitySystemComponent);
	}

	/** Fills out the modifier magnitudes inside the Modifier Specs */
	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec")
	static void CalculateModifierMagnitudes(UPARAM(ref) FGameplayEffectSpec& Spec)
	{
		Spec.CalculateModifierMagnitudes();
	}

	/** Recapture attributes from source and target for cloning */
	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec")
	static void RecaptureAttributeDataForClone(UPARAM(ref) FGameplayEffectSpec& Spec, UAbilitySystemComponent* OriginalASC, UAbilitySystemComponent* NewASC)
	{
		Spec.RecaptureAttributeDataForClone(OriginalASC, NewASC);
	}

	/** Recaptures source actor tags of this spec without modifying anything else */
	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec")
	static void RecaptureSourceActorTags(UPARAM(ref) FGameplayEffectSpec& Spec)
	{
		Spec.RecaptureSourceActorTags();
	}

	/** Helper function to initialize all of the capture definitions required by the spec */
	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec")
	static void SetupAttributeCaptureDefinitions(UPARAM(ref) FGameplayEffectSpec& Spec)
	{
		Spec.SetupAttributeCaptureDefinitions();
	}

	/** Helper function that returns the duration after applying relevant modifiers from the source and target ability system components */
	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static float CalculateModifiedDuration(const FGameplayEffectSpec& Spec)
	{
		return Spec.CalculateModifiedDuration();
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static float GetModifiedAttributeMagnitude(const FGameplayEffectSpec& Spec, const FGameplayAttribute& Attribute)
	{
		const FGameplayEffectModifiedAttribute* ModAttributePointer = Spec.GetModifiedAttribute(Attribute); 
		return ModAttributePointer ? ModAttributePointer->TotalMagnitude : 0.0f;
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static float GetDuration(const FGameplayEffectSpec& Spec)
	{
		return Spec.GetDuration();
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static float GetPeriod(const FGameplayEffectSpec& Spec)
	{
		return Spec.GetPeriod();
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static FGameplayEffectContextHandle GetContext(const FGameplayEffectSpec& Spec)
	{
		return Spec.GetContext();
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static float GetSetByCallerMagnitude(const FGameplayEffectSpec& Spec, FGameplayTag DataTag, bool bWarnIfNotFound = true, float DefaultIfNotFound = 0.f)
	{
		return Spec.GetSetByCallerMagnitude(DataTag, bWarnIfNotFound, DefaultIfNotFound);
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static FGameplayTagContainer GetAllGrantedTags(const FGameplayEffectSpec& Spec)
	{
		FGameplayTagContainer OutContainer; Spec.GetAllGrantedTags(OutContainer); return OutContainer;
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static FGameplayTagContainer GetAllAssetTags(const FGameplayEffectSpec& Spec)
	{
		FGameplayTagContainer OutContainer; Spec.GetAllAssetTags(OutContainer); return OutContainer;
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static float GetLevel(const FGameplayEffectSpec& Spec)
	{
		return Spec.GetLevel();
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec")
	static void PrintAll(const FGameplayEffectSpec& Spec)
	{
		Spec.PrintAll();
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static FString ToSimpleString(const FGameplayEffectSpec& Spec)
	{
		return Spec.ToSimpleString();
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec")
	static float GetModifierMagnitude(const FGameplayEffectSpec& Spec, int32 ModifierIdx)
	{
		return Spec.GetModifierMagnitude(ModifierIdx);
	}
	
	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec Properties")
	static FAngelscriptTagContainerAggregator GetCapturedSourceTags(const FGameplayEffectSpec& Spec) 
	{
		return FAngelscriptTagContainerAggregator(Spec.CapturedSourceTags); 
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec Properties")
	static FAngelscriptTagContainerAggregator GetCapturedTargetTags(const FGameplayEffectSpec& Spec) 
	{
		return FAngelscriptTagContainerAggregator(Spec.CapturedTargetTags); 
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec Properties")
	static const FGameplayTagContainer GetDynamicAssetTags(const FGameplayEffectSpec& Spec)
	{
		return Spec.GetDynamicAssetTags(); 
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec Properties")
	static void AddDynamicAssetTag(UPARAM(ref) FGameplayEffectSpec& Spec, const FGameplayTag& TagToAdd)
	{
		Spec.AddDynamicAssetTag(TagToAdd); 
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec Properties")
	static void AppendDynamicAssetTags(UPARAM(ref) FGameplayEffectSpec& Spec, const FGameplayTagContainer& ContainerToAdd)
	{
		Spec.AppendDynamicAssetTags(ContainerToAdd);
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec Properties")
	static int32 GetStackCount(const FGameplayEffectSpec& Spec) 
	{
		return Spec.GetStackCount(); 
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec Properties")
	static int32 GetCompletedSourceAttributeCapture(const FGameplayEffectSpec& Spec) 
	{
		return Spec.bCompletedSourceAttributeCapture; 
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec Properties")
	static int32 GetCompletedTargetAttributeCapture(const FGameplayEffectSpec& Spec) 
	{
		return Spec.bCompletedTargetAttributeCapture; 
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Effect Spec Properties")
	static int32 GetDurationLocked(const FGameplayEffectSpec& Spec) 
	{
		return Spec.bDurationLocked; 
	}

	UFUNCTION(meta=(DeprecatedFunction, DeprecationMessage="This variable will be removed in favor of (immutable) GASpecs that live on GameplayEffectComponents (e.g. AbilitiesGameplayEffectComponent)."), BlueprintPure, Category = "Gameplay Effect Spec Properties")
	static TArray<FGameplayAbilitySpecDef> GetGrantedAbilitySpecs(const FGameplayEffectSpec& Spec) 
	{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
		return Spec.GrantedAbilitySpecs;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Effect Spec")
	static void ApplyExecutorModifiersForDefinition(FGameplayEffectSpec& Spec, UClass* CalculationClass, const FGameplayEffectAttributeCaptureDefinition& InCaptureDef, float& ValueToModify)
	{
		if (Spec.Def)
		{
			// There is a big caveat here. If some gameplay effect has multiple executors of the same type, this will not work as intended. There is no way we can know which executor this is called from since it all goes via CDO.
			// Since even adding an executor is rare, and we still haven't even seen an effect with two, having more than one executor of the same type should pretty much never happen. I have an ensure here to guard a bit more.
			int32 NrCalcsOfWantedType = 0;
			for (const FGameplayEffectExecutionDefinition& Execution : Spec.Def->Executions)
			{
				if (Execution.CalculationClass != CalculationClass)
				{
					continue;
				}

				NrCalcsOfWantedType++;
				for (const FGameplayEffectExecutionScopedModifierInfo& Modifier : Execution.CalculationModifiers)
				{
					if (Modifier.CapturedAttribute == InCaptureDef)
					{
						float OutCalcMagnitude = 0.0f;
						Modifier.ModifierMagnitude.AttemptCalculateMagnitude(Spec, OutCalcMagnitude);

						switch (Modifier.ModifierOp)
						{
							case EGameplayModOp::Override:
							{
								ValueToModify = OutCalcMagnitude;
								break;
							}
							case EGameplayModOp::Additive:
							{
								ValueToModify += OutCalcMagnitude;
								break;
							}
							case EGameplayModOp::Division:
							{
								if (OutCalcMagnitude != 0.0f)
								{
									ValueToModify /= OutCalcMagnitude;
								}
								break;
							}
							case EGameplayModOp::Multiplicitive:
							{
								ValueToModify *= OutCalcMagnitude;
								break;
							}
							default:
							{
								break;
							}
						}
					}
				}
			}

			ensureMsgf(NrCalcsOfWantedType <= 1, TEXT("More than one executor of wanted type detected! This is not allowed! Split them into multiple effects instead."));
		}
	}
};
