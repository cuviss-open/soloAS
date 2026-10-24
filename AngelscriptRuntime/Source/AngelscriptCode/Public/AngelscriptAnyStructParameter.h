#pragma once
#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "AngelscriptAnyStructParameter.generated.h"

/**
 * This is a simple struct that has an implicit constructor bound that can take any struct type.
 *
 * It can be used to create function binds that take an FInstancedStruct on the C++ side, but
 * that AS scripts can conveniently pass any struct to, without having to manually call `FInstancedStruct::Make` each time.
 */
USTRUCT(BlueprintType)
struct FAngelscriptAnyStructParameter
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Struct Data")
	FInstancedStruct InstancedStruct;
};
