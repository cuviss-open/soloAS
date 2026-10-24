#pragma once
#include "Engine/LevelStreaming.h"
#include "AngelscriptLevelStreamingLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "ULevelStreaming"))
class UAngelscriptLevelStreamingLibrary : public UObject
{
	GENERATED_BODY()

public:
	
#if WITH_EDITOR
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static bool GetShouldBeVisibleInEditor(const ULevelStreaming* LevelStreaming)
	{
		return LevelStreaming->GetShouldBeVisibleInEditor();
	}
#endif
};
