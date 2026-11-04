#include "Shared/SoloASBridgeReflectionSupport.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"

namespace SoloASBridge
{
FReflectionTraitRegistry& GetReflectionTraitRegistry()
{
    static FReflectionTraitRegistry Registry;
    return Registry;
}

bool HasOnlyPropertyTraitBits(ESoloASBridgePropertyTrait Traits)
{
    const uint16 Bits = static_cast<uint16>(Traits);
    return Bits != 0 && (Bits & ~SoloASBridgePropertyTraitMask) == 0;
}

bool HasOnlyFunctionTraitBits(ESoloASBridgeFunctionTrait Traits)
{
    const uint16 Bits = static_cast<uint16>(Traits);
    return Bits != 0 && (Bits & ~SoloASBridgeFunctionTraitMask) == 0;
}

const UStruct* GetPropertyOwner(const FProperty* Property)
{
    return Property != nullptr ? Property->GetOwnerStruct() : nullptr;
}

const UObject* GetFunctionOwner(const UFunction* Function)
{
    return Function != nullptr ? Function->GetOuter() : nullptr;
}

bool HasCurrentPropertyIdentity(
    const FPropertyTraitRecord& Record,
    const FProperty* Property
)
{
    return !Record.Identity.IsStale()
        && Record.Identity.Get() == Property;
}

FPropertyTraitRecord MakePropertyTraitRecord(
    const FProperty* Property,
    FName RegistrationOwner,
    ESoloASBridgePropertyTrait Traits
)
{
    FPropertyTraitRecord Record;
    Record.RegistrationOwner = RegistrationOwner;
    Record.Traits = Traits;
    Record.Identity = const_cast<FProperty*>(Property);
    return Record;
}

void ClearPropertyTraits(FName RegistrationOwner)
{
    if (!IsInGameThread())
    {
        UE_LOG(LogTemp, Error, TEXT("SoloASBridge property traits require the game thread."));
        return;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge property traits require the game thread.")
    );
    if (RegistrationOwner.IsNone())
    {
        return;
    }
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    for (
        auto OwnerIterator = Registry.Owners.CreateIterator();
        OwnerIterator;
        ++OwnerIterator
    )
    {
        FReflectionOwnerTraits& OwnerTraits = OwnerIterator.Value();
        for (
            auto PropertyIterator = OwnerTraits.Properties.CreateIterator();
            PropertyIterator;
            ++PropertyIterator
        )
        {
            if (
                PropertyIterator.Value().RegistrationOwner
                == RegistrationOwner
            )
            {
                PropertyIterator.RemoveCurrent();
            }
        }
        if (
            OwnerTraits.Properties.IsEmpty()
            && OwnerTraits.Functions.IsEmpty()
        )
        {
            OwnerIterator.RemoveCurrent();
        }
    }
}

void ClearFunctionTraits(FName RegistrationOwner)
{
    if (!IsInGameThread())
    {
        UE_LOG(LogTemp, Error, TEXT("SoloASBridge function traits require the game thread."));
        return;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge function traits require the game thread.")
    );
    if (RegistrationOwner.IsNone())
    {
        return;
    }
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    for (
        auto OwnerIterator = Registry.Owners.CreateIterator();
        OwnerIterator;
        ++OwnerIterator
    )
    {
        FReflectionOwnerTraits& OwnerTraits = OwnerIterator.Value();
        for (
            auto FunctionIterator = OwnerTraits.Functions.CreateIterator();
            FunctionIterator;
            ++FunctionIterator
        )
        {
            if (
                FunctionIterator.Value().RegistrationOwner
                == RegistrationOwner
            )
            {
                FunctionIterator.RemoveCurrent();
            }
        }
        if (
            OwnerTraits.Properties.IsEmpty()
            && OwnerTraits.Functions.IsEmpty()
        )
        {
            OwnerIterator.RemoveCurrent();
        }
    }
}
}

namespace SoloASBridge::Private
{
void PruneReflectionTraits()
{
    if (!IsInGameThread())
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("SoloASBridge reflection trait pruning requires the game thread.")
        );
        return;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge reflection trait pruning requires the game thread.")
    );
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    for (auto Iterator = Registry.Owners.CreateIterator(); Iterator; ++Iterator)
    {
        UObject* OwningObject = Iterator.Key().ResolveObjectPtr();
        if (OwningObject == nullptr)
        {
            Iterator.RemoveCurrent();
            continue;
        }
        const UStruct* OwningStruct = Cast<UStruct>(OwningObject);
        FReflectionOwnerTraits& OwnerTraits = Iterator.Value();
        for (
            auto PropertyIterator = OwnerTraits.Properties.CreateIterator();
            PropertyIterator;
            ++PropertyIterator
        )
        {
            if (
                OwningStruct == nullptr
                || !HasCurrentPropertyIdentity(
                    PropertyIterator.Value(),
                    PropertyIterator.Key()
                )
                || PropertyIterator.Key()->GetOwnerStruct() != OwningStruct
            )
            {
                PropertyIterator.RemoveCurrent();
            }
        }
        for (
            auto FunctionIterator = OwnerTraits.Functions.CreateIterator();
            FunctionIterator;
            ++FunctionIterator
        )
        {
            const UFunction* Function = Cast<UFunction>(
                FunctionIterator.Key().ResolveObjectPtr()
            );
            if (
                Function == nullptr
                || Function->GetOuter() != OwningObject
            )
            {
                FunctionIterator.RemoveCurrent();
            }
        }
        if (
            OwnerTraits.Properties.IsEmpty()
            && OwnerTraits.Functions.IsEmpty()
        )
        {
            Iterator.RemoveCurrent();
        }
    }
}

void ClearAllReflectionTraits()
{
    if (!IsInGameThread())
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("SoloASBridge reflection trait clearing requires the game thread.")
        );
        return;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge reflection trait clearing requires the game thread.")
    );
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    Registry.Owners.Reset();
}
}

namespace SoloASBridge
{
}
