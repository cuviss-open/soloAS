#include "Runtime/SoloASBridgeCallableDispatchSupport.h"

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

bool IsUnsupportedWritableRawObjectSlot(const FProperty& Property)
{
    return IsRawObjectSlot(Property)
        && !Property.HasAnyPropertyFlags(CPF_ReturnParm)
        && Property.HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm);
}

bool IsRawObjectSlotValueCompatible(const FProperty& Property, const void* Slot)
{
    UObject* Object = static_cast<UObject*>(const_cast<void*>(Slot));
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
            Property->GetElementSize() <= 0
            || Property->GetOffset_ForInternal() < 0
            || Property->GetOffset_ForInternal() + Property->GetSize()
                > Function.ParmsSize
            || (
                Property->IsA<FObjectPropertyBase>()
                && !IsRawObjectSlot(*Property)
                && !IsReadOnlyClassWrapperInput(*Property)
            )
            || IsUnsupportedWritableRawObjectSlot(*Property)
        )
        {
            return false;
        }

        if (
            Property->HasAnyPropertyFlags(CPF_ReturnParm)
            && Property->HasAnyPropertyFlags(CPF_ReferenceParm)
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
            && !IsRawObjectSlot(*Property)
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
                && !IsRawObjectSlot(*Property)
                && !IsRawObjectSlot(*EarlierProperty)
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
    void* ParameterData
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

        if (IsRawObjectSlot(*Property))
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
        else if (
            !Property->HasAnyPropertyFlags(CPF_OutParm)
            || Property->HasAnyPropertyFlags(CPF_ReferenceParm)
        )
        {
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
    bool bBorrowedConstReferences
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
            && !(bBorrowedConstReferences && Property->HasAllPropertyFlags(CPF_ConstParm | CPF_ReferenceParm)))
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
