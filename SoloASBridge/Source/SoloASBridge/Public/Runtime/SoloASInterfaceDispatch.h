// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
class UFunction;
namespace SoloASBridge
{
// The caller owns an initialized reflected parameter buffer for the synchronous call.
SOLOASBRIDGE_API bool InvokeInterfaceDefault(UObject* Object, UFunction* Function, void* Parameters, SIZE_T Capacity);
}
