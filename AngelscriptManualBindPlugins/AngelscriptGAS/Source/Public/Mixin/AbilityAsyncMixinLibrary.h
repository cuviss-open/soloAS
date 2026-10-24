#pragma once

#include "CoreMinimal.h"

#include "Abilities/Async/AbilityAsync.h"

#include "AbilityAsyncMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "UAbilityAsync"))
class UAbilityAsyncMixinLibrary : public UObject
{
	GENERATED_BODY()

	UFUNCTION(ScriptCallable, Category = "Gameplay Tasks")
	static void Activate(UAbilityAsync* Async)
	{
		Async->Activate();
	}
};
