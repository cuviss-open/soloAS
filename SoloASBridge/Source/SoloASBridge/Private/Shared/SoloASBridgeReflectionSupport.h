#pragma once

#include "Shared/SoloASBridgeReflection.h"
#include "Misc/ScopeRWLock.h"
#include "UObject/Class.h"
#include "UObject/FieldPath.h"
#include "UObject/ObjectKey.h"
#include "UObject/UnrealType.h"

namespace SoloASBridge
{
template <typename TraitType>
struct TReflectionTraitRecord
{
    FName RegistrationOwner = NAME_None;
    TraitType Traits = TraitType::None;
};

struct FPropertyTraitRecord
    : TReflectionTraitRecord<ESoloASBridgePropertyTrait>
{
    TFieldPath<FProperty> Identity;
};

struct FReflectionOwnerTraits
{
    TMap<const FProperty*, FPropertyTraitRecord> Properties;
    TMap<
        FObjectKey,
        TReflectionTraitRecord<ESoloASBridgeFunctionTrait>
    > Functions;
};

struct FReflectionTraitRegistry
{
    FRWLock Lock;
    TMap<FObjectKey, FReflectionOwnerTraits> Owners;
    enum class ELifecycle : uint8
    {
        Uninitialized,
        Accepting,
        Stopped,
    } Lifecycle = ELifecycle::Uninitialized;
};

FReflectionTraitRegistry& GetReflectionTraitRegistry();
bool HasOnlyPropertyTraitBits(ESoloASBridgePropertyTrait Traits);
bool HasOnlyFunctionTraitBits(ESoloASBridgeFunctionTrait Traits);
const UStruct* GetPropertyOwner(const FProperty* Property);
const UObject* GetFunctionOwner(const UFunction* Function);
bool HasCurrentPropertyIdentity(
    const FPropertyTraitRecord& Record,
    const FProperty* Property
);
FPropertyTraitRecord MakePropertyTraitRecord(
    const FProperty* Property,
    FName RegistrationOwner,
    ESoloASBridgePropertyTrait Traits
);
}
