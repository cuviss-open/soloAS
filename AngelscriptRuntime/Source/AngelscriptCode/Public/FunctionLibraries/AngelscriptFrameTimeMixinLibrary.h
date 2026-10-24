
#pragma once

#include "CoreMinimal.h"
#include "Misc/QualifiedFrameTime.h"
#include "AngelscriptFrameTimeMixinLibrary.generated.h"

UCLASS(meta = (ScriptMixin = "FQualifiedFrameTime"))
class ANGELSCRIPTCODE_API UAngelscriptFrameTimeMixinLibrary  : public UObject
{
	GENERATED_BODY()
	
public:

	UFUNCTION(ScriptCallable, Category = "FrameTime")
    static double AsSeconds(const FQualifiedFrameTime& Target)
	{
		return Target.AsSeconds();
	}
};
