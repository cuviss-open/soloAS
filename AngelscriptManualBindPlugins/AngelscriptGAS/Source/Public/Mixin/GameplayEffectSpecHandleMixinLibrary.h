#pragma once

#include "CoreMinimal.h"

#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"

#include "GameplayEffectSpecHandleMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FGameplayEffectSpecHandle"))
class ANGELSCRIPTGAS_API UGameplayEffectSpecHandleMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(ScriptCallable, Category = "Gameplay Effect Spec")
	static bool IsValid(const FGameplayEffectSpecHandle& Handle)
	{
		return Handle.IsValid();
	}

	UFUNCTION(ScriptCallable, Category = "Gameplay Effect Spec")
	static FGameplayEffectSpec& GetSpec(FGameplayEffectSpecHandle& Handle)
	{
		ensureMsgf(Handle.Data.IsValid(), TEXT("FGameplayEffectSpecHandle contained invalid spec! Should never happen!"));
		return *Handle.Data.Get();
	}

	UFUNCTION(ScriptCallable, Category = "Gameplay Effect Spec", meta=(ScriptName="GetSpec"))
	static const FGameplayEffectSpec& GetSpec_Const(const FGameplayEffectSpecHandle& Handle)
	{
		ensureMsgf(Handle.Data.IsValid(), TEXT("FGameplayEffectSpecHandle contained invalid spec! Should never happen!"));
		return *Handle.Data.Get();
	}

	UFUNCTION(ScriptCallable, Category = "Gameplay Effect Spec")
	static void AddDynamicAssetTag(UPARAM(ref) FGameplayEffectSpecHandle& Handle, FGameplayTag TagToAdd)
	{
		ensureMsgf(Handle.Data.IsValid(), TEXT("FGameplayEffectSpecHandle contained invalid spec! Should never happen!"));
		Handle.Data->AddDynamicAssetTag(TagToAdd);
	}

	UFUNCTION(ScriptCallable, Category = "Gameplay Effect Spec")
	static void AddDynamicAssetTags(UPARAM(ref) FGameplayEffectSpecHandle& Handle, FGameplayTagContainer TagsToAdd)
	{
		ensureMsgf(Handle.Data.IsValid(), TEXT("FGameplayEffectSpecHandle contained invalid spec! Should never happen!"));
		Handle.Data->AppendDynamicAssetTags(TagsToAdd);
	}
};
