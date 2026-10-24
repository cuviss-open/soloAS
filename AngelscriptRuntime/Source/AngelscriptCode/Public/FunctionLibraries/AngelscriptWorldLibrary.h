#pragma once
#include "Engine/World.h"
#include "Engine/LevelStreaming.h"
#include "AngelscriptWorldLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "UWorld"))
class UAngelscriptWorldLibrary : public UObject
{
	GENERATED_BODY()

public:

	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static TArray<ULevelStreaming*> GetStreamingLevels(const UWorld* World)
	{
		return World->GetStreamingLevels();
	}
};
