#pragma once

#include "Runtime/SoloASBridgeCallableDispatch.h"

class UClass;
class UFunction;
class UObject;

namespace SoloASBridge::Private
{
bool HasSupportedParameterLayout(const UFunction& Function);
bool HasSupportedSignature(const UFunction& Function);
bool HaveCompatibleSignatures(
    const UFunction& DeclaredFunction,
    const UFunction& ActualFunction
);
UFunction* ResolveDeclaredFunction(
    const FSoloASBridgeCallableDispatchRequest& Request,
    UClass*& OutOwner,
    ESoloASBridgeCallableDispatchResult& OutFailure
);
bool IsTargetCompatible(const UObject& Target, const UClass& Owner);
ESoloASBridgeCallableDispatchResult ValidateSlots(
    const UFunction& Function,
    TConstArrayView<void*> Arguments,
    void* ReturnValue
);
void CopyInputs(
    const UFunction& Function,
    TConstArrayView<void*> Arguments,
    void* ParameterData
);
void CopyOutputs(
    const UFunction& Function,
    TConstArrayView<void*> Arguments,
    void* ReturnValue,
    const void* ParameterData,
    bool bBorrowedConstReferences = false
);
}
