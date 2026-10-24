#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "SoloASLambdaCaptureStore.generated.h"

USTRUCT(meta=(ForceAngelscriptBind))
struct ANGELSCRIPTCODE_API FScriptLambdaHandle
{
    GENERATED_BODY()
    UPROPERTY()
    uint64 Lease = 0;
    UPROPERTY()
    uint64 Item = 0;
};

USTRUCT()
struct FSoloASLambdaCaptureList
{
    GENERATED_BODY()
    UPROPERTY()
    TArray<FInstancedStruct> Values;
    UPROPERTY()
    TArray<uint64> Items;
    UPROPERTY()
    int32 Generation = 0;
};

// Only this stable wrapper is an owner property. Adding/removing source sites
// changes map entries and capture schemas, never the owner's storage layout.
USTRUCT(meta=(ForceAngelscriptBind))
struct ANGELSCRIPTCODE_API FSoloASLambdaCaptureStore
{
    GENERATED_BODY()
    UPROPERTY()
    TMap<FName, FSoloASLambdaCaptureList> Sites;
};
