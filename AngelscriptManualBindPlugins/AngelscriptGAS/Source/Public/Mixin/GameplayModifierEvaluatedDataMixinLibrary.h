#pragma once

#include "CoreMinimal.h"

#include "GameplayEffectTypes.h"

#include "GameplayModifierEvaluatedDataMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FGameplayModifierEvaluatedData"))
class ANGELSCRIPTGAS_API UGameplayModifierEvaluatedDataMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(ScriptCallable)
	static const FGameplayAttribute& GetAttribute(const FGameplayModifierEvaluatedData& Data) 
	{
		return Data.Attribute; 
	}

	UFUNCTION(ScriptCallable)
	static void SetAttribute(FGameplayModifierEvaluatedData& Data, const FGameplayAttribute& NewValue) 
	{
		Data.Attribute = NewValue; 
	}

	UFUNCTION(ScriptCallable)
	static TEnumAsByte<EGameplayModOp::Type> GetModifierOp(const FGameplayModifierEvaluatedData& Data) 
	{
		return Data.ModifierOp; 
	}

	UFUNCTION(ScriptCallable)
	static void SetModifierOp(FGameplayModifierEvaluatedData& Data, TEnumAsByte<EGameplayModOp::Type> NewValue) 
	{
		Data.ModifierOp = NewValue; 
	}

	UFUNCTION(ScriptCallable)
	static float GetMagnitude(const FGameplayModifierEvaluatedData& Data) 
	{
		return Data.Magnitude; 
	}

	UFUNCTION(ScriptCallable)
	static void SetMagnitude(FGameplayModifierEvaluatedData& Data, float NewValue) 
	{
		Data.Magnitude = NewValue; 
	}

	UFUNCTION(ScriptCallable)
	static FActiveGameplayEffectHandle GetHandle(const FGameplayModifierEvaluatedData& Data) 
	{
		return Data.Handle; 
	}

	UFUNCTION(ScriptCallable)
	static void SetHandle(FGameplayModifierEvaluatedData& Data, FActiveGameplayEffectHandle NewValue) 
	{
		Data.Handle = NewValue; 
	}

	UFUNCTION(ScriptCallable)
	static bool GetIsValid(const FGameplayModifierEvaluatedData& Data) 
	{
		return Data.IsValid; 
	}
	UFUNCTION(ScriptCallable)
	static void SetIsValid(FGameplayModifierEvaluatedData& Data, bool NewValue) 
	{
		Data.IsValid = NewValue; 
	}

	UFUNCTION(ScriptCallable)
	static FString ToSimpleString(const FGameplayModifierEvaluatedData& Data) 
	{
		return Data.ToSimpleString(); 
	}
};
