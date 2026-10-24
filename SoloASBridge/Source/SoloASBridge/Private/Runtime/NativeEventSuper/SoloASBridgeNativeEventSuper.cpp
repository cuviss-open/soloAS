#include "Runtime/NativeEventSuper/SoloASBridgeNativeEventSuper.h"

#include "HAL/ThreadingBase.h"
#include "UObject/Class.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr EFunctionFlags DisallowedFunctionFlags =
    FUNC_Static | FUNC_Net | FUNC_NetReliable | FUNC_NetRequest
    | FUNC_NetResponse | FUNC_NetMulticast | FUNC_NetServer | FUNC_NetClient
    | FUNC_NetValidate;
bool HasRejectedFunctionFlags(const UFunction& Function)
{
    return Function.HasAnyFunctionFlags(DisallowedFunctionFlags);
}
bool IsCanonicalPath(const UClass& Class, const TCHAR* RequestedPath)
{
    return RequestedPath != nullptr && RequestedPath[0] != TCHAR('\0')
        && Class.GetPathName() == RequestedPath;
}
bool IsRawObjectPointer(const FProperty& Property)
{
    const FFieldClass* PropertyClass = Property.GetClass();
    return (
               PropertyClass == FObjectProperty::StaticClass()
               || PropertyClass == FClassProperty::StaticClass()
           )
        && !Property.HasAnyPropertyFlags(
            CPF_TObjectPtr | CPF_UObjectWrapper
        );
}
bool RangesOverlap(const FProperty& Left, const FProperty& Right)
{
    const int64 LeftStart = Left.GetOffset_ForInternal();
    const int64 RightStart = Right.GetOffset_ForInternal();
    const int64 LeftSize = Left.GetSize();
    const int64 RightSize = Right.GetSize();
    if (
        LeftStart < 0 || RightStart < 0 || LeftSize <= 0 || RightSize <= 0
    )
    {
        return true;
    }
    return LeftStart < RightStart
        ? RightStart - LeftStart < LeftSize
        : LeftStart - RightStart < RightSize;
}
ESoloASBridgeNativeEventSuperResult ValidateParameterLayout(
    const UFunction& Function,
    int64 ParameterBufferSize,
    const FProperty** OutReturnProperty
)
{
    if (OutReturnProperty != nullptr)
    {
        *OutReturnProperty = nullptr;
    }
    bool bFoundReturn = false;
    for (
        TFieldIterator<FProperty> It(
            &Function,
            EFieldIteratorFlags::ExcludeSuper
        );
        It;
        ++It
    )
    {
        const FProperty& Property = **It;
        const bool bIsReturn = Property.HasAnyPropertyFlags(CPF_ReturnParm);
        if (
            bIsReturn && !Property.HasAnyPropertyFlags(CPF_Parm)
        )
        {
            return ESoloASBridgeNativeEventSuperResult::ParameterLayoutMismatch;
        }
        if (!Property.HasAnyPropertyFlags(CPF_Parm))
        {
            continue;
        }
        const int64 Offset = Property.GetOffset_ForInternal();
        const int64 Size = Property.GetSize();
        if (
            Property.GetElementSize() <= 0
            || Property.ArrayDim <= 0
            || Offset < 0
            || Size <= 0
            || Offset > ParameterBufferSize
            || Size > ParameterBufferSize - Offset
        )
        {
            return ESoloASBridgeNativeEventSuperResult::ParameterLayoutMismatch;
        }
        if (bIsReturn)
        {
            if (bFoundReturn)
            {
                return ESoloASBridgeNativeEventSuperResult::ParameterLayoutMismatch;
            }
            bFoundReturn = true;
            if (Property.HasAnyPropertyFlags(CPF_ReferenceParm))
            {
                return ESoloASBridgeNativeEventSuperResult::
                    UnsupportedReferenceReturn;
            }
            if (OutReturnProperty != nullptr)
            {
                *OutReturnProperty = &Property;
            }
        }
        if (
            IsRawObjectPointer(Property)
            && Property.HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm)
        )
        {
            return ESoloASBridgeNativeEventSuperResult::
                UnsupportedWritableObjectReference;
        }
        for (
            TFieldIterator<FProperty> EarlierIt(
                &Function,
                EFieldIteratorFlags::ExcludeSuper
            );
            EarlierIt;
            ++EarlierIt
        )
        {
            const FProperty& EarlierProperty = **EarlierIt;
            if (&EarlierProperty == &Property)
            {
                break;
            }
            if (!EarlierProperty.HasAnyPropertyFlags(CPF_Parm))
            {
                continue;
            }
            if (RangesOverlap(Property, EarlierProperty))
            {
                const bool bAliasSensitive =
                    Property.HasAnyPropertyFlags(
                        CPF_OutParm | CPF_ReferenceParm
                    )
                    || EarlierProperty.HasAnyPropertyFlags(
                        CPF_OutParm | CPF_ReferenceParm
                    );
                return bAliasSensitive
                    ? ESoloASBridgeNativeEventSuperResult::
                          UnsupportedAliasLayout
                    : ESoloASBridgeNativeEventSuperResult::
                          ParameterLayoutMismatch;
            }
        }
    }
    return ESoloASBridgeNativeEventSuperResult::Succeeded;
}
bool HaveExactParameterLayouts(
    const UFunction& CallerFunction,
    const UFunction& CandidateFunction
)
{
    TFieldIterator<FProperty> CallerIt(
        &CallerFunction,
        EFieldIteratorFlags::ExcludeSuper
    );
    TFieldIterator<FProperty> CandidateIt(
        &CandidateFunction,
        EFieldIteratorFlags::ExcludeSuper
    );
    for (;;)
    {
        while (CallerIt && !CallerIt->HasAnyPropertyFlags(CPF_Parm))
        {
            ++CallerIt;
        }
        while (CandidateIt && !CandidateIt->HasAnyPropertyFlags(CPF_Parm))
        {
            ++CandidateIt;
        }
        if (!CallerIt || !CandidateIt)
        {
            return !CallerIt && !CandidateIt;
        }
        const FProperty& CallerProperty = **CallerIt;
        const FProperty& CandidateProperty = **CandidateIt;
        constexpr EPropertyFlags ParameterContractFlags =
            CPF_ReturnParm | CPF_OutParm | CPF_ReferenceParm | CPF_ConstParm;
        if (
            CallerProperty.GetFName() != CandidateProperty.GetFName()
            || !CallerProperty.SameType(&CandidateProperty)
            || (CallerProperty.GetPropertyFlags() & ParameterContractFlags)
                != (CandidateProperty.GetPropertyFlags()
                    & ParameterContractFlags)
            || CallerProperty.GetOffset_ForInternal()
                != CandidateProperty.GetOffset_ForInternal()
            || CallerProperty.ArrayDim != CandidateProperty.ArrayDim
            || CallerProperty.GetElementSize()
                != CandidateProperty.GetElementSize()
            || CallerProperty.GetSize() != CandidateProperty.GetSize()
        )
        {
            return false;
        }
        ++CallerIt;
        ++CandidateIt;
    }
}
bool IsValidParameterBuffer(
    const UFunction& Function,
    const FSoloASBridgeNativeEventSuperRequest& Request
)
{
    if (
        Request.InitializedParameterBuffer == nullptr
        || Request.ParameterBufferCapacity
            < static_cast<SIZE_T>(Function.ParmsSize)
        || Request.ParameterBufferAlignment == 0
        || (Request.ParameterBufferAlignment
            & (Request.ParameterBufferAlignment - 1U)) != 0
    )
    {
        return false;
    }
    const uint32 RequiredAlignment = Function.GetMinAlignment();
    if (RequiredAlignment == 0)
    {
        return false;
    }
    const UPTRINT Address = reinterpret_cast<UPTRINT>(
        Request.InitializedParameterBuffer
    );
    return Request.ParameterBufferAlignment >= RequiredAlignment
        && Address % Request.ParameterBufferAlignment == 0
        && Address % RequiredAlignment == 0;
}
UClass* ResolveClass(const TCHAR* Path)
{
    UClass* Class = FindObject<UClass>(nullptr, Path);
    return Class != nullptr && IsCanonicalPath(*Class, Path)
        ? Class
        : nullptr;
}
UFunction* ResolveExactFunction(UClass& Owner, FName FunctionName)
{
    UFunction* Function = Owner.FindFunctionByName(FunctionName);
    return Function != nullptr && Function->GetOuterUClass() == &Owner
        && Function->GetFName() == FunctionName
        ? Function
        : nullptr;
}
UFunction* ResolveNativeAncestor(
    UFunction& CallerFunction,
    UClass& NativeClass,
    UFunction& ExpectedNativeFunction,
    FName FunctionName,
    ESoloASBridgeNativeEventSuperResult& OutResult
)
{
    UFunction* Current = CallerFunction.GetSuperFunction();
    if (Current == nullptr)
    {
        OutResult = ESoloASBridgeNativeEventSuperResult::NativeAncestorNotFound;
        return nullptr;
    }

    UClass* Owner = Current->GetOuterUClass();
    if (Owner == nullptr)
    {
        OutResult = ESoloASBridgeNativeEventSuperResult::NativeAncestorNotFound;
        return nullptr;
    }
    if (HasRejectedFunctionFlags(*Current))
    {
        OutResult = ESoloASBridgeNativeEventSuperResult::
            UnsupportedFunctionFlags;
        return nullptr;
    }
    if (
        !Owner->HasAnyClassFlags(CLASS_Native)
        || Owner != &NativeClass
        || Current != &ExpectedNativeFunction
        || Current->GetFName() != FunctionName
    )
    {
        OutResult = ESoloASBridgeNativeEventSuperResult::NativeAncestorMismatch;
        return nullptr;
    }
    return Current;
}
}
ESoloASBridgeNativeEventSuperResult
SoloASBridge::InvokeNativeEventSuper(
    const FSoloASBridgeNativeEventSuperRequest& Request
)
{
    if (!IsInGameThread())
    {
        return ESoloASBridgeNativeEventSuperResult::OffGameThread;
    }
    if (
        Request.CallerScriptClassPath == nullptr
        || Request.NativeDeclaringClassPath == nullptr
        || Request.CallerScriptClassPath[0] == TCHAR('\0')
        || Request.NativeDeclaringClassPath[0] == TCHAR('\0')
        || Request.FunctionName.IsNone()
    )
    {
        return ESoloASBridgeNativeEventSuperResult::InvalidRequest;
    }
    if (Request.Target == nullptr)
    {
        return ESoloASBridgeNativeEventSuperResult::TargetRequired;
    }
    UClass* CallerClass = ResolveClass(Request.CallerScriptClassPath);
    if (CallerClass == nullptr)
    {
        return ESoloASBridgeNativeEventSuperResult::CallerClassNotFound;
    }
    if (CallerClass->HasAnyClassFlags(CLASS_Interface))
    {
        return ESoloASBridgeNativeEventSuperResult::InterfaceRejected;
    }
    if (CallerClass->HasAnyClassFlags(CLASS_Native))
    {
        return ESoloASBridgeNativeEventSuperResult::CallerOwnerInvalid;
    }
    UClass* NativeClass = ResolveClass(Request.NativeDeclaringClassPath);
    if (NativeClass == nullptr)
    {
        return ESoloASBridgeNativeEventSuperResult::NativeClassNotFound;
    }
    const bool bNativeInterface =
        NativeClass->HasAnyClassFlags(CLASS_Interface);
    if (!NativeClass->HasAnyClassFlags(CLASS_Native))
    {
        return ESoloASBridgeNativeEventSuperResult::NativeClassInvalid;
    }
    UFunction* CallerFunction =
        ResolveExactFunction(*CallerClass, Request.FunctionName);
    if (CallerFunction == nullptr)
    {
        return ESoloASBridgeNativeEventSuperResult::CallerFunctionNotFound;
    }
    // Runtime-generated Blueprint overrides may carry FUNC_Native for the
    // standard Angelscript thunk.
    if (
        !CallerFunction->HasAnyFunctionFlags(FUNC_BlueprintEvent)
        || CallerFunction->GetSuperFunction() == nullptr
        || HasRejectedFunctionFlags(*CallerFunction)
    )
    {
        return ESoloASBridgeNativeEventSuperResult::
            CallerFunctionShapeInvalid;
    }
    UFunction* ExpectedNativeFunction =
        ResolveExactFunction(*NativeClass, Request.FunctionName);
    if (ExpectedNativeFunction == nullptr)
    {
        return ESoloASBridgeNativeEventSuperResult::NativeFunctionNotFound;
    }
    ESoloASBridgeNativeEventSuperResult AncestorResult =
        ESoloASBridgeNativeEventSuperResult::NativeAncestorNotFound;
    UFunction* CandidateFunction = ResolveNativeAncestor(
        *CallerFunction,
        *NativeClass,
        *ExpectedNativeFunction,
        Request.FunctionName,
        AncestorResult
    );
    if (CandidateFunction == nullptr)
    {
        return AncestorResult;
    }
    if (
        !CandidateFunction->HasAnyFunctionFlags(FUNC_BlueprintEvent)
        || !CandidateFunction->HasAnyFunctionFlags(FUNC_Native)
        || HasRejectedFunctionFlags(*CandidateFunction)
    )
    {
        return ESoloASBridgeNativeEventSuperResult::UnsupportedFunctionFlags;
    }
    if (!Request.Target->IsA(CallerClass))
    {
        return ESoloASBridgeNativeEventSuperResult::TargetTypeMismatch;
    }
    if (bNativeInterface)
    {
        if (!Request.Target->GetClass()->ImplementsInterface(NativeClass))
        {
            return ESoloASBridgeNativeEventSuperResult::TargetTypeMismatch;
        }
        if (Request.Target->GetInterfaceAddress(NativeClass) == nullptr)
        {
            return ESoloASBridgeNativeEventSuperResult::
                NativeImplementationMissing;
        }
    }
    else if (!Request.Target->IsA(NativeClass))
    {
        return ESoloASBridgeNativeEventSuperResult::TargetTypeMismatch;
    }
    const FProperty* CandidateReturnProperty = nullptr;
    ESoloASBridgeNativeEventSuperResult LayoutResult = ValidateParameterLayout(
        *CandidateFunction,
        static_cast<int64>(CandidateFunction->ParmsSize),
        &CandidateReturnProperty
    );
    if (LayoutResult != ESoloASBridgeNativeEventSuperResult::Succeeded)
    {
        return LayoutResult;
    }
    if (!HaveExactParameterLayouts(*CallerFunction, *CandidateFunction))
    {
        return ESoloASBridgeNativeEventSuperResult::ParameterLayoutMismatch;
    }
    // Runtime-generated overrides may report an input-only ParmsSize while
    // still exposing an exact trailing return property. The native function
    // owns the invocation buffer extent; every caller property remains
    // subject to the exact per-property comparison above.
    LayoutResult = ValidateParameterLayout(
        *CallerFunction,
        static_cast<int64>(CandidateFunction->ParmsSize),
        nullptr
    );
    if (LayoutResult != ESoloASBridgeNativeEventSuperResult::Succeeded)
    {
        return LayoutResult;
    }
    if (!IsValidParameterBuffer(*CandidateFunction, Request))
    {
        return ESoloASBridgeNativeEventSuperResult::ParameterBufferInvalid;
    }
    if (CandidateFunction->GetNativeFunc() == nullptr)
    {
        return ESoloASBridgeNativeEventSuperResult::NativeImplementationMissing;
    }
    void* ReturnAddress = nullptr;
    if (CandidateReturnProperty != nullptr)
    {
        const int64 ReturnOffset =
            CandidateReturnProperty->GetOffset_ForInternal();
        const int64 ReturnSize = CandidateReturnProperty->GetSize();
        if (ReturnOffset < 0 || ReturnSize <= 0)
        {
            return ESoloASBridgeNativeEventSuperResult::
                ParameterBufferInvalid;
        }
        const uint64 Offset = static_cast<uint64>(ReturnOffset);
        const uint64 Size = static_cast<uint64>(ReturnSize);
        const uint64 Capacity =
            static_cast<uint64>(Request.ParameterBufferCapacity);
        if (Offset > Capacity || Size > Capacity - Offset)
        {
            return ESoloASBridgeNativeEventSuperResult::
                ParameterBufferInvalid;
        }
        ReturnAddress = static_cast<uint8*>(
            Request.InitializedParameterBuffer
        ) + static_cast<SIZE_T>(Offset);
    }
    TArray<FOutParmRec, TInlineAllocator<8>> OutParameters;
    OutParameters.Reserve(CandidateFunction->NumParms);
    for (
        TFieldIterator<FProperty> It(
            CandidateFunction,
            EFieldIteratorFlags::ExcludeSuper
        );
        It;
        ++It
    )
    {
        FProperty* Property = *It;
        if (!Property->HasAllPropertyFlags(CPF_Parm | CPF_OutParm))
        {
            continue;
        }
        FOutParmRec& OutParameter = OutParameters.AddDefaulted_GetRef();
        OutParameter.Property = Property;
        OutParameter.PropAddr = Property->ContainerPtrToValuePtr<uint8>(
            Request.InitializedParameterBuffer
        );
        OutParameter.NextOutParm = nullptr;
    }
    for (int32 Index = 1; Index < OutParameters.Num(); ++Index)
    {
        OutParameters[Index - 1].NextOutParm = &OutParameters[Index];
    }
    FFrame Frame(
        Request.Target,
        CandidateFunction,
        Request.InitializedParameterBuffer,
        nullptr,
        CandidateFunction->ChildProperties
    );
    Frame.OutParms = OutParameters.IsEmpty()
        ? nullptr
        : &OutParameters[0];
    CandidateFunction->Invoke(Request.Target, Frame, ReturnAddress);
    return ESoloASBridgeNativeEventSuperResult::Succeeded;
}
