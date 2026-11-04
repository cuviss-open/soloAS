#include "Runtime/SoloASBridgeCallableDispatchSupport.h"
#include "Compat/SoloASRuntimeCompat.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace SoloASBridge::Private
{
namespace
{
bool IsRawObjectSlot(const FProperty& Property)
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

bool IsReadOnlyClassWrapperInput(const FProperty& Property)
{
    return Property.GetClass() == FClassProperty::StaticClass()
        && Property.HasAnyPropertyFlags(CPF_UObjectWrapper)
        && !Property.HasAnyPropertyFlags(CPF_ReturnParm | CPF_TObjectPtr)
        && (!Property.HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm)
            || Property.HasAllPropertyFlags(CPF_ConstParm | CPF_ReferenceParm));
}

// A: these AS value types retain the native wrapper's FProperty storage layout.
bool IsSupportedObjectWrapperSlot(const FProperty& Property)
{
    return Property.IsA<FSoftObjectProperty>()
        || (Property.GetClass() == FClassProperty::StaticClass()
            && Property.HasAnyPropertyFlags(CPF_UObjectWrapper)
            && !Property.HasAnyPropertyFlags(CPF_TObjectPtr));
}

// A: UE-AS fork passes UObject*& as a pointer slot, while UObject* is the object itself.
bool IsRawObjectReferenceSlot(const FProperty& Property)
{
    return IsRawObjectSlot(Property)
        && !Property.HasAnyPropertyFlags(CPF_ReturnParm)
        && Property.HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm);
}

bool IsRawObjectSlotValueCompatible(const FProperty& Property, const void* Slot)
{
    UObject* Object = IsRawObjectReferenceSlot(Property)
        ? *static_cast<UObject* const*>(Slot)
        : static_cast<UObject*>(const_cast<void*>(Slot));
    if (Object == nullptr)
    {
        return true;
    }

    if (const FClassProperty* ClassProperty = CastField<FClassProperty>(&Property))
    {
        const UClass* Class = Cast<UClass>(Object);
        return Class != nullptr
            && (
                ClassProperty->MetaClass == nullptr
                || Class->IsChildOf(ClassProperty->MetaClass)
            );
    }

    if (const FObjectPropertyBase* ObjectProperty =
            CastField<FObjectPropertyBase>(&Property))
    {
        return ObjectProperty->PropertyClass == nullptr
            || Object->IsA(ObjectProperty->PropertyClass);
    }

    return false;
}

bool IsClassSlotValueCompatible(const FProperty& Property, const void* Slot)
{
    if (Slot == nullptr)
    {
        return false;
    }

    const FClassProperty* ClassProperty =
        CastFieldChecked<FClassProperty>(&Property);
    UObject* Object = ClassProperty->GetObjectPropertyValue(Slot);
    if (Object == nullptr)
    {
        return true;
    }

    const UClass* Class = Cast<UClass>(Object);
    return Class != nullptr
        && (
            ClassProperty->MetaClass == nullptr
            || Class->IsChildOf(ClassProperty->MetaClass)
        );
}

bool SlotsOverlap(
    const void* Left,
    SIZE_T LeftSize,
    const void* Right,
    SIZE_T RightSize
)
{
    const UPTRINT LeftStart = reinterpret_cast<UPTRINT>(Left);
    const UPTRINT RightStart = reinterpret_cast<UPTRINT>(Right);
    return LeftStart < RightStart + RightSize
        && RightStart < LeftStart + LeftSize;
}
}

bool HasSupportedParameterLayout(const UFunction& Function)
{
    for (
        TFieldIterator<FProperty> It(
            &Function,
            EFieldIteratorFlags::ExcludeSuper
        );
        It;
        ++It
    )
    {
        const FProperty* Property = *It;
        if (!Property->HasAnyPropertyFlags(CPF_Parm))
        {
            continue;
        }

        if (
            SoloASBridge::Compat::GetPropertyElementSize(Property) <= 0
            || Property->GetOffset_ForInternal() < 0
            || Property->GetOffset_ForInternal() + Property->GetSize()
                > Function.ParmsSize
            || (
                Property->IsA<FObjectPropertyBase>()
                && !IsRawObjectSlot(*Property)
                && !IsReadOnlyClassWrapperInput(*Property)
                && !IsSupportedObjectWrapperSlot(*Property)
            )
        )
        {
            return false;
        }

        if (
            Property->HasAnyPropertyFlags(CPF_ReturnParm)
            && Property->HasAnyPropertyFlags(CPF_ReferenceParm)
            && SoloASBridge::GetNativeReferenceCaller(&Function) == nullptr
        )
        {
            return false;
        }
    }
    return true;
}

bool HasSupportedSignature(const UFunction& Function)
{
    return Function.HasAnyFunctionFlags(FUNC_Native)
        && HasSupportedParameterLayout(Function);
}

bool HaveCompatibleSignatures(
    const UFunction& DeclaredFunction,
    const UFunction& ActualFunction
)
{
    TFieldIterator<FProperty> DeclaredIt(
        &DeclaredFunction,
        EFieldIteratorFlags::ExcludeSuper
    );
    TFieldIterator<FProperty> ActualIt(
        &ActualFunction,
        EFieldIteratorFlags::ExcludeSuper
    );
    for (;;)
    {
        while (
            DeclaredIt
            && !DeclaredIt->HasAnyPropertyFlags(CPF_Parm)
        )
        {
            ++DeclaredIt;
        }
        while (ActualIt && !ActualIt->HasAnyPropertyFlags(CPF_Parm))
        {
            ++ActualIt;
        }

        if (!DeclaredIt || !ActualIt)
        {
            return !DeclaredIt && !ActualIt;
        }

        const FProperty* DeclaredProperty = *DeclaredIt;
        const FProperty* ActualProperty = *ActualIt;
        constexpr EPropertyFlags RelevantFlags =
            CPF_ReturnParm | CPF_OutParm | CPF_ReferenceParm | CPF_ConstParm;
        if (
            !DeclaredProperty->SameType(ActualProperty)
            || DeclaredProperty->GetSize() != ActualProperty->GetSize()
            || (DeclaredProperty->GetPropertyFlags() & RelevantFlags)
                != (ActualProperty->GetPropertyFlags() & RelevantFlags)
        )
        {
            return false;
        }

        ++DeclaredIt;
        ++ActualIt;
    }
}

UFunction* ResolveDeclaredFunction(
    const FSoloASBridgeCallableDispatchRequest& Request,
    UClass*& OutOwner,
    ESoloASBridgeCallableDispatchResult& OutFailure
)
{
    OutOwner = nullptr;
    if (Request.DeclaredFunction != nullptr)
    {
        UFunction* Function = const_cast<UFunction*>(Request.DeclaredFunction);
        OutOwner = Function->GetOuterUClass();
        if (OutOwner == nullptr)
        {
            OutFailure = ESoloASBridgeCallableDispatchResult::OwnerNotFound;
            return nullptr;
        }
        if (
            !Request.FunctionName.IsNone()
            && Request.FunctionName != Function->GetFName()
        )
        {
            OutFailure =
                ESoloASBridgeCallableDispatchResult::FunctionNotFound;
            return nullptr;
        }
// SOLOAS-COMPAT-BEGIN(<5.8) BeginDestroy clears outer package names before script finalization on UE 5.4-5.7 [test: SoloAS.RealProjectLifecycleBenchmark] [id: CS.Bridge.CallableDispatch.ExitNativeOwnerIdentity]
#if SOLOAS_UE_BEFORE(5, 8)
        // A: CS.Bridge.CallableDispatch.ExitNativeOwnerIdentity replaces UE-AS fork native-pointer dispatch.
        // Use the bound native identities before touching the retired outer path.
        if (GetScriptFinishDestroyReceiver() && GExitPurge
            && Function->HasAnyFunctionFlags(FUNC_Native)
            && OutOwner->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic)
            && !Function->HasAnyFlags(RF_FinishDestroyed) && !OutOwner->HasAnyFlags(RF_FinishDestroyed)
            && Function == OutOwner->FindFunctionByName(Function->GetFName()))
        {
            return Function;
        }
#endif
// SOLOAS-COMPAT-END
        if (
            Request.OwnerPath != nullptr
            && OutOwner->GetPathName() != Request.OwnerPath
        )
        {
            OutFailure = ESoloASBridgeCallableDispatchResult::OwnerNotFound;
            return nullptr;
        }
        return Function;
    }

    if (Request.OwnerPath == nullptr || Request.FunctionName.IsNone())
    {
        OutFailure = ESoloASBridgeCallableDispatchResult::InvalidRequest;
        return nullptr;
    }

    OutOwner = FindObject<UClass>(nullptr, Request.OwnerPath);
    if (OutOwner == nullptr)
    {
        OutFailure = ESoloASBridgeCallableDispatchResult::OwnerNotFound;
        return nullptr;
    }

    UFunction* Function = OutOwner->FindFunctionByName(Request.FunctionName);
    if (
        Function == nullptr
        || Function->GetOuterUClass() != OutOwner
    )
    {
        OutFailure = ESoloASBridgeCallableDispatchResult::FunctionNotFound;
        return nullptr;
    }
    return Function;
}

bool IsTargetCompatible(const UObject& Target, const UClass& Owner)
{
    return Owner.HasAnyClassFlags(CLASS_Interface)
        ? Target.GetClass()->ImplementsInterface(&Owner)
        : Target.IsA(&Owner);
}

ESoloASBridgeCallableDispatchResult ValidateSlots(
    const UFunction& Function,
    TConstArrayView<void*> Arguments,
    void* ReturnValue
)
{
    int32 ArgumentIndex = 0;
    for (
        TFieldIterator<FProperty> It(
            &Function,
            EFieldIteratorFlags::ExcludeSuper
        );
        It;
        ++It
    )
    {
        const FProperty* Property = *It;
        if (!Property->HasAnyPropertyFlags(CPF_Parm))
        {
            continue;
        }
        if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            if (ReturnValue == nullptr)
            {
                return ESoloASBridgeCallableDispatchResult::
                    MissingReturnStorage;
            }
            continue;
        }
        if (!Arguments.IsValidIndex(ArgumentIndex))
        {
            return ESoloASBridgeCallableDispatchResult::ArgumentCountMismatch;
        }
        if (
            Arguments[ArgumentIndex] == nullptr
            && (!IsRawObjectSlot(*Property) || IsRawObjectReferenceSlot(*Property))
        )
        {
            return ESoloASBridgeCallableDispatchResult::
                MissingArgumentStorage;
        }
        if (
            IsRawObjectSlot(*Property)
            && !IsRawObjectSlotValueCompatible(
                *Property,
                Arguments[ArgumentIndex]
            )
        )
        {
            return ESoloASBridgeCallableDispatchResult::
                ArgumentTypeMismatch;
        }
        if (
            IsReadOnlyClassWrapperInput(*Property)
            && !IsClassSlotValueCompatible(
                *Property,
                Arguments[ArgumentIndex]
            )
        )
        {
            return ESoloASBridgeCallableDispatchResult::
                ArgumentTypeMismatch;
        }

        for (int32 EarlierIndex = 0; EarlierIndex < ArgumentIndex;
             ++EarlierIndex)
        {
            const FProperty* EarlierProperty = nullptr;
            int32 CandidateIndex = 0;
            for (
                TFieldIterator<FProperty> EarlierIt(
                    &Function,
                    EFieldIteratorFlags::ExcludeSuper
                );
                 EarlierIt;
                 ++EarlierIt)
            {
                const FProperty* Candidate = *EarlierIt;
                if (
                    !Candidate->HasAnyPropertyFlags(CPF_Parm)
                    || Candidate->HasAnyPropertyFlags(CPF_ReturnParm)
                )
                {
                    continue;
                }
                if (CandidateIndex++ == EarlierIndex)
                {
                    EarlierProperty = Candidate;
                    break;
                }
            }
            const bool bEitherSlotRequiresIdentity =
                Property->HasAnyPropertyFlags(CPF_ReferenceParm | CPF_OutParm)
                || (
                    EarlierProperty != nullptr
                    && EarlierProperty->HasAnyPropertyFlags(
                        CPF_ReferenceParm | CPF_OutParm
                    )
                );
            if (
                bEitherSlotRequiresIdentity
                && EarlierProperty != nullptr
                && (!IsRawObjectSlot(*Property) || IsRawObjectReferenceSlot(*Property))
                && (!IsRawObjectSlot(*EarlierProperty) || IsRawObjectReferenceSlot(*EarlierProperty))
                && SlotsOverlap(
                    Arguments[ArgumentIndex],
                    Property->GetSize(),
                    Arguments[EarlierIndex],
                    EarlierProperty->GetSize()
                )
            )
            {
                return ESoloASBridgeCallableDispatchResult::
                    UnsupportedSignature;
            }
        }
        ++ArgumentIndex;
    }

    return ArgumentIndex == Arguments.Num()
        ? ESoloASBridgeCallableDispatchResult::Succeeded
        : ESoloASBridgeCallableDispatchResult::ArgumentCountMismatch;
}

void CopyInputs(
    const UFunction& Function,
    TConstArrayView<void*> Arguments,
    void* ParameterData,
    bool bBorrowedReferences
)
{
    int32 ArgumentIndex = 0;
    for (
        TFieldIterator<FProperty> It(
            &Function,
            EFieldIteratorFlags::ExcludeSuper
        );
        It;
        ++It
    )
    {
        FProperty* Property = *It;
        if (
            !Property->HasAnyPropertyFlags(CPF_Parm)
            || Property->HasAnyPropertyFlags(CPF_ReturnParm)
        )
        {
            continue;
        }

        if (bBorrowedReferences && Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm))
        {
            ++ArgumentIndex;
            continue;
        }

        if (IsRawObjectSlot(*Property) && !IsRawObjectReferenceSlot(*Property))
        {
            FObjectPropertyBase* ObjectProperty =
                CastFieldChecked<FObjectPropertyBase>(Property);
            ObjectProperty->SetObjectPropertyValue_InContainer(
                ParameterData,
                static_cast<UObject*>(Arguments[ArgumentIndex])
            );
        }
        else if (IsReadOnlyClassWrapperInput(*Property))
        {
            Property->CopyCompleteValue(
                Property->ContainerPtrToValuePtr<void>(ParameterData),
                Arguments[ArgumentIndex]
            );
        }
        else
        {
            // Event dispatch uses an owned value frame, matching fork event calls.
            Property->CopyCompleteValue(
                Property->ContainerPtrToValuePtr<void>(ParameterData),
                Arguments[ArgumentIndex]
            );
        }
        ++ArgumentIndex;
    }
}

void CopyOutputs(
    const UFunction& Function,
    TConstArrayView<void*> Arguments,
    void* ReturnValue,
    const void* ParameterData,
    bool bBorrowedReferences
)
{
    int32 ArgumentIndex = 0;
    for (
        TFieldIterator<FProperty> It(
            &Function,
            EFieldIteratorFlags::ExcludeSuper
        );
        It;
        ++It
    )
    {
        FProperty* Property = *It;
        if (!Property->HasAnyPropertyFlags(CPF_Parm))
        {
            continue;
        }
        if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            Property->InitializeValue(ReturnValue);
            Property->CopyCompleteValue(
                ReturnValue,
                Property->ContainerPtrToValuePtr<void>(ParameterData)
            );
            continue;
        }
        if (Property->HasAnyPropertyFlags(CPF_OutParm)
            && !bBorrowedReferences)
        {
            Property->CopyCompleteValue(
                Arguments[ArgumentIndex],
                Property->ContainerPtrToValuePtr<void>(ParameterData)
            );
        }
        ++ArgumentIndex;
    }
}
}
