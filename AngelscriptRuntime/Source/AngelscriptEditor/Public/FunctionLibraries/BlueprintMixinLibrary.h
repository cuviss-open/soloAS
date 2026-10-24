#pragma once

#include "CoreMinimal.h"
#include "Engine/BlueprintCore.h"
#include "BlueprintMixinLibrary.generated.h"

UCLASS(MinimalAPI, Meta = (ScriptMixin = "UBlueprintCore UBlueprint"))
class UBlueprintMixinLibrary : public UObject
{
	GENERATED_BODY()

public:

	UFUNCTION(ScriptCallable)
	static UClass* GetGeneratedClass(UBlueprintCore* Blueprint)
	{
		return Blueprint != nullptr ? Cast<UClass>(Blueprint->GeneratedClass) : nullptr;
	}
};
