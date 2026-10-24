#pragma once

#include "GameplayCueNotify_Static.h"

#include "GameplayCueNotifyMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "UGameplayCueNotify_Static"))
class ANGELSCRIPTGAS_API UGameplayCueNotify_StaticMixinLibrary : public UObject
{
	GENERATED_BODY()
	
	UFUNCTION(ScriptCallable, Category = "Gameplay Cue Notify")
	static FGameplayTag GetGameplayCueTag(const UGameplayCueNotify_Static* GameplayCueNotifyStatic)
	{
		if (GameplayCueNotifyStatic == nullptr)
		{
			return FGameplayTag();
		}
		return GameplayCueNotifyStatic->GameplayCueTag;
	}
};