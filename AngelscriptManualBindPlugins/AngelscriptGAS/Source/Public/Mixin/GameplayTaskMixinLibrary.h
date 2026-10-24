#pragma once

#include "CoreMinimal.h"

#include "GameplayTask.h"

#include "GameplayTaskMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "UGameplayTask"))
class UGameplayTaskMixinLibrary : public UObject
{
	GENERATED_BODY()

	UFUNCTION(ScriptCallable, Category = "Gameplay Tasks")
	static void ReadyForActivation(UGameplayTask* Task)
	{
		Task->ReadyForActivation();
	}
};
