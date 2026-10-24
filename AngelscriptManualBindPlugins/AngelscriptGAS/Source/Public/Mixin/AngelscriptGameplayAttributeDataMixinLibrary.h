#pragma once

#include "CoreMinimal.h"

#include "AngelscriptAttributeSet.h"

#include "AngelscriptGameplayAttributeDataMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FAngelscriptGameplayAttributeData"))
class ANGELSCRIPTGAS_API UAngelscriptGameplayAttributeDataMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Ability System")
	static void Initialize(FAngelscriptGameplayAttributeData& Data, float InitialData)
	{
		Data.SetBaseValue(InitialData);
		Data.SetCurrentValue(InitialData);
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System")
	static void SetCurrentValue(FAngelscriptGameplayAttributeData& Data, float NewValue)
	{
		Data.SetCurrentValue(NewValue);
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System")
	static void SetBaseValue(FAngelscriptGameplayAttributeData& Data, float NewValue)
	{
		Data.SetBaseValue(NewValue);
	}

	UFUNCTION(BlueprintPure, Category = "Ability System")
	static float GetBaseValue(const FAngelscriptGameplayAttributeData& Data)
	{
		return Data.GetBaseValue();
	}

	UFUNCTION(BlueprintPure, Category = "Ability System")
	static float GetCurrentValue(const FAngelscriptGameplayAttributeData& Data)
	{
		return Data.GetCurrentValue();
	}
};