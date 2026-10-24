#pragma once
#include "CoreMinimal.h"

#include "InputActionValue.h"

#include "InputActionValueMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FInputActionValue"))
class UInputActionValueMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
                                                                                                                                                
#if 0
                                                                                                                                              
	UFUNCTION(ScriptCallable)
	static void Reset(FInputActionValue& Value)
	{
		Value.Reset();
	}
                                                                                                                                          
#endif
	UFUNCTION(ScriptCallable)
	static void Reset(UPARAM(ref) FInputActionValue& Value)
	{
		Value.Reset();
	}
                                                                                                                                        

	UFUNCTION(ScriptCallable)
	static EInputActionValueType GetValueType(const FInputActionValue& Value)
	{
		return Value.GetValueType();
	}

	UFUNCTION(ScriptCallable)
	static float GetMagnitudeSq(const FInputActionValue& Value)
	{
		return Value.GetMagnitudeSq();
	}

	UFUNCTION(ScriptCallable)
	static float GetMagnitude(const FInputActionValue& Value)
	{
		return Value.GetMagnitude();
	}

	UFUNCTION(ScriptCallable)
	static FString ToString(const FInputActionValue& Value)
	{
		return Value.ToString();
	}
};
