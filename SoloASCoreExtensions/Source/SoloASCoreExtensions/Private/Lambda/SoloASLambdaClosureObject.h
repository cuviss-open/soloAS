// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Lambda/SoloASLambdaCaptureStore.h"
#include "SoloASLambdaClosureObject.generated.h"

USTRUCT()
struct FSoloASLambdaFallbackSlot
{
    GENERATED_BODY()
    UPROPERTY()
    TWeakObjectPtr<UObject> Owner;
    UPROPERTY()
    FName Body;
    UPROPERTY()
    FSoloASLambdaCaptureList Captures;
};

// A single class/CDO serves every source site and signature. Each instance
// holds several independent (object, function-name) binding identities.
UCLASS(Transient, NotBlueprintable)
class USoloASLambdaClosureObject final : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(Transient)
    TMap<FName, FSoloASLambdaFallbackSlot> Slots;
    int32 ActiveInvocations = 0;
    bool bNeverPool = false;
    void ProcessEvent(UFunction* Function, void* Parameters) override;
};
