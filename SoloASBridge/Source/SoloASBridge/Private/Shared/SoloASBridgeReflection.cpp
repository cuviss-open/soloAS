#include "Shared/SoloASBridgeReflection.h"
#include "HAL/CriticalSection.h"
#include "Shared/SoloASBridgeReflectionSupport.h"
namespace SoloASBridge
{
void InitializeReflectionTraits()
{
    if (!IsInGameThread())
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("SoloASBridge reflection traits must initialize on the game thread.")
        );
        return;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge reflection traits must initialize on the game thread.")
    );
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    if (
        Registry.Lifecycle != FReflectionTraitRegistry::ELifecycle::Uninitialized
    )
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("SoloASBridge reflection traits cannot be initialized twice.")
        );
        return;
    }
    checkf(
        Registry.Lifecycle == FReflectionTraitRegistry::ELifecycle::Uninitialized,
        TEXT("SoloASBridge reflection traits cannot be initialized twice.")
    );
    Registry.Lifecycle = FReflectionTraitRegistry::ELifecycle::Accepting;
}

void ShutdownReflectionTraits()
{
    if (!IsInGameThread())
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("SoloASBridge reflection traits must shut down on the game thread.")
        );
        return;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge reflection traits must shut down on the game thread.")
    );
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    if (Registry.Lifecycle != FReflectionTraitRegistry::ELifecycle::Accepting)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("SoloASBridge reflection traits were not accepting registrations.")
        );
        return;
    }
    checkf(
        Registry.Lifecycle == FReflectionTraitRegistry::ELifecycle::Accepting,
        TEXT("SoloASBridge reflection traits were not accepting registrations.")
    );
    Registry.Lifecycle = FReflectionTraitRegistry::ELifecycle::Stopped;
}

bool SetPropertyTraits(
    const FProperty* Property,
    FName RegistrationOwner,
    ESoloASBridgePropertyTrait Traits
)
{
    if (!IsInGameThread())
    {
        UE_LOG(LogTemp, Error, TEXT("SoloASBridge property traits require the game thread."));
        return false;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge property traits require the game thread.")
    );
    const UStruct* OwningStruct = GetPropertyOwner(Property);
    if (
        RegistrationOwner.IsNone()
        || OwningStruct == nullptr
        || !HasOnlyPropertyTraitBits(Traits)
    )
    {
        return false;
    }
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    if (Registry.Lifecycle != FReflectionTraitRegistry::ELifecycle::Accepting)
    {
        return false;
    }
    FReflectionOwnerTraits& OwnerTraits = Registry.Owners.FindOrAdd(
        FObjectKey(OwningStruct)
    );
    FPropertyTraitRecord* Record =
        OwnerTraits.Properties.Find(Property);
    if (Record != nullptr)
    {
        if (!HasCurrentPropertyIdentity(*Record, Property))
        {
            OwnerTraits.Properties.Remove(Property);
            Record = nullptr;
        }
    }
    if (Record != nullptr)
    {
        if (Record->RegistrationOwner != RegistrationOwner)
        {
            return false;
        }
        Record->Traits = Traits;
        return true;
    }
    OwnerTraits.Properties.Add(
        Property,
        MakePropertyTraitRecord(Property, RegistrationOwner, Traits)
    );
    return true;
}
bool GetPropertyTraits(
    const FProperty* Property,
    ESoloASBridgePropertyTrait& OutTraits
)
{
    OutTraits = ESoloASBridgePropertyTrait::None;
    if (!IsInGameThread())
    {
        UE_LOG(LogTemp, Error, TEXT("SoloASBridge property traits require the game thread."));
        return false;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge property traits require the game thread.")
    );
    const UStruct* OwningStruct = GetPropertyOwner(Property);
    if (OwningStruct == nullptr)
    {
        return false;
    }
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FReadScopeLock ScopeLock(Registry.Lock);
    const FReflectionOwnerTraits* OwnerTraits = Registry.Owners.Find(
        FObjectKey(OwningStruct)
    );
    if (OwnerTraits == nullptr)
    {
        return false;
    }
    const FPropertyTraitRecord* Record =
        OwnerTraits->Properties.Find(Property);
    if (
        Record == nullptr
        || !HasCurrentPropertyIdentity(*Record, Property)
    )
    {
        return false;
    }
    OutTraits = Record->Traits;
    return true;
}
bool ClearPropertyTraits(
    const FProperty* Property,
    FName RegistrationOwner
)
{
    if (!IsInGameThread())
    {
        UE_LOG(LogTemp, Error, TEXT("SoloASBridge property traits require the game thread."));
        return false;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge property traits require the game thread.")
    );
    const UStruct* OwningStruct = GetPropertyOwner(Property);
    if (
        RegistrationOwner.IsNone()
        || OwningStruct == nullptr
    )
    {
        return false;
    }
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    const FObjectKey StructKey(OwningStruct);
    FReflectionOwnerTraits* OwnerTraits = Registry.Owners.Find(StructKey);
    if (OwnerTraits == nullptr)
    {
        return false;
    }
    const FPropertyTraitRecord* Record =
        OwnerTraits->Properties.Find(Property);
    if (
        Record == nullptr
        || !HasCurrentPropertyIdentity(*Record, Property)
        || Record->RegistrationOwner != RegistrationOwner
    )
    {
        return false;
    }
    OwnerTraits->Properties.Remove(Property);
    if (OwnerTraits->Properties.IsEmpty() && OwnerTraits->Functions.IsEmpty())
    {
        Registry.Owners.Remove(StructKey);
    }
    return true;
}
bool SetFunctionTraits(
    const UFunction* Function,
    FName RegistrationOwner,
    ESoloASBridgeFunctionTrait Traits
)
{
    if (!IsInGameThread())
    {
        UE_LOG(LogTemp, Error, TEXT("SoloASBridge function traits require the game thread."));
        return false;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge function traits require the game thread.")
    );
    const UObject* OwningObject = GetFunctionOwner(Function);
    if (
        RegistrationOwner.IsNone()
        || OwningObject == nullptr
        || !HasOnlyFunctionTraitBits(Traits)
    )
    {
        return false;
    }
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    if (Registry.Lifecycle != FReflectionTraitRegistry::ELifecycle::Accepting)
    {
        return false;
    }
    FReflectionOwnerTraits& OwnerTraits = Registry.Owners.FindOrAdd(
        FObjectKey(OwningObject)
    );
    const FObjectKey FunctionKey(Function);
    TReflectionTraitRecord<ESoloASBridgeFunctionTrait>* Record =
        OwnerTraits.Functions.Find(FunctionKey);
    if (Record != nullptr)
    {
        if (Record->RegistrationOwner != RegistrationOwner)
        {
            return false;
        }
        Record->Traits = Traits;
        return true;
    }
    OwnerTraits.Functions.Add(
        FunctionKey,
        { RegistrationOwner, Traits }
    );
    return true;
}
bool GetFunctionTraits(
    const UFunction* Function,
    ESoloASBridgeFunctionTrait& OutTraits
)
{
    OutTraits = ESoloASBridgeFunctionTrait::None;
    if (!IsInGameThread())
    {
        UE_LOG(LogTemp, Error, TEXT("SoloASBridge function traits require the game thread."));
        return false;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge function traits require the game thread.")
    );
    const UObject* OwningObject = GetFunctionOwner(Function);
    if (OwningObject == nullptr)
    {
        return false;
    }
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FReadScopeLock ScopeLock(Registry.Lock);
    const FReflectionOwnerTraits* OwnerTraits = Registry.Owners.Find(
        FObjectKey(OwningObject)
    );
    if (OwnerTraits == nullptr)
    {
        return false;
    }
    const TReflectionTraitRecord<ESoloASBridgeFunctionTrait>* Record =
        OwnerTraits->Functions.Find(FObjectKey(Function));
    if (Record == nullptr)
    {
        return false;
    }
    OutTraits = Record->Traits;
    return true;
}
bool ClearFunctionTraits(
    const UFunction* Function,
    FName RegistrationOwner
)
{
    if (!IsInGameThread())
    {
        UE_LOG(LogTemp, Error, TEXT("SoloASBridge function traits require the game thread."));
        return false;
    }
    checkf(
        IsInGameThread(),
        TEXT("SoloASBridge function traits require the game thread.")
    );
    const UObject* OwningObject = GetFunctionOwner(Function);
    if (
        RegistrationOwner.IsNone()
        || OwningObject == nullptr
    )
    {
        return false;
    }
    FReflectionTraitRegistry& Registry = GetReflectionTraitRegistry();
    FWriteScopeLock ScopeLock(Registry.Lock);
    const FObjectKey OwnerKey(OwningObject);
    const FObjectKey FunctionKey(Function);
    FReflectionOwnerTraits* OwnerTraits = Registry.Owners.Find(OwnerKey);
    if (OwnerTraits == nullptr)
    {
        return false;
    }
    const TReflectionTraitRecord<ESoloASBridgeFunctionTrait>* Record =
        OwnerTraits->Functions.Find(FunctionKey);
    if (
        Record == nullptr
        || Record->RegistrationOwner != RegistrationOwner
    )
    {
        return false;
    }
    OwnerTraits->Functions.Remove(FunctionKey);
    if (OwnerTraits->Properties.IsEmpty() && OwnerTraits->Functions.IsEmpty())
    {
        Registry.Owners.Remove(OwnerKey);
    }
    return true;
}
}
