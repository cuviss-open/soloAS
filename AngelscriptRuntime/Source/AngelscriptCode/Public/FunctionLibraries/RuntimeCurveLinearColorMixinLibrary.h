
#pragma once

#include "CoreMinimal.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "RuntimeCurveLinearColorMixinLibrary.generated.h"

UCLASS(meta = (ScriptMixin = "FRuntimeCurveLinearColor"))
class ANGELSCRIPTCODE_API URuntimeCurveLinearColorMixinLibrary  : public UObject
{
	GENERATED_BODY()
	
public:

	UFUNCTION(ScriptCallable, Category = "Math|Curves")
	static void AddDefaultKey(FRuntimeCurveLinearColor& Target, float InTime, FLinearColor InColor)
	{
		Target.ColorCurves[0].AddKey(InTime, InColor.R);
		Target.ColorCurves[1].AddKey(InTime, InColor.G);
		Target.ColorCurves[2].AddKey(InTime, InColor.B);
		Target.ColorCurves[3].AddKey(InTime, InColor.A);
	}
	
};
