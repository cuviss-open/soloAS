// Copyright Cuviss Studio. All Rights Reserved.
#include "Runtime/SoloASInterfaceDispatch.h"
#include "UObject/Class.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"

bool SoloASBridge::InvokeInterfaceDefault(UObject* Object, UFunction* Function, void* Parameters, SIZE_T Capacity)
{
    if (!Object || !Function || !Parameters || Capacity < Function->ParmsSize) return false;
    UClass* Interface = Function->GetOuterUClass();
    if (!Interface->HasAnyClassFlags(CLASS_Interface) || !Object->GetClass()->ImplementsInterface(Interface)) return false;
    FProperty* Return = Function->GetReturnProperty();
    if (!Function->HasAnyFunctionFlags(FUNC_Native) || !Object->GetInterfaceAddress(Interface))
    {
        if (Return) Return->ClearValue_InContainer(Parameters);
        return true;
    }
    if (!Function->GetNativeFunc()) return false;
    TArray<FOutParmRec, TInlineAllocator<8>> OutParameters;
    for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
    {
        if (!It->HasAnyPropertyFlags(CPF_OutParm)) continue;
        FOutParmRec& Out = OutParameters.AddDefaulted_GetRef();
        Out.Property = *It;
        Out.PropAddr = It->ContainerPtrToValuePtr<uint8>(Parameters);
        Out.NextOutParm = nullptr;
    }
    for (int32 Index = 1; Index < OutParameters.Num(); ++Index)
        OutParameters[Index - 1].NextOutParm = &OutParameters[Index];
    FFrame Frame(Object, Function, Parameters, nullptr, Function->ChildProperties);
    Frame.OutParms = OutParameters.IsEmpty() ? nullptr : &OutParameters[0];
    Function->Invoke(Object, Frame, Return ? Return->ContainerPtrToValuePtr<uint8>(Parameters) : nullptr);
    return true;
}
