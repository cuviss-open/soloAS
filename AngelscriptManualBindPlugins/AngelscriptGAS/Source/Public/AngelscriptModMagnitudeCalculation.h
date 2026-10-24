#pragma once

#include "CoreMinimal.h"

#include "GameplayModMagnitudeCalculation.h"

#include "AngelscriptModMagnitudeCalculation.generated.h"

UCLASS(abstract, meta = (ChildCanTick))
class ANGELSCRIPTGAS_API UAngelscriptModMagnitudeCalculation : public UGameplayModMagnitudeCalculation
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = Attributes)
	void AddRelevantAttributeToCapture(const FGameplayEffectAttributeCaptureDefinition& CaptureDefinition);
};
