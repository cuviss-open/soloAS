#include "AngelscriptModMagnitudeCalculation.h"

void UAngelscriptModMagnitudeCalculation::AddRelevantAttributeToCapture(const FGameplayEffectAttributeCaptureDefinition &CaptureDefinition)
{
	RelevantAttributesToCapture.Add(CaptureDefinition);
}