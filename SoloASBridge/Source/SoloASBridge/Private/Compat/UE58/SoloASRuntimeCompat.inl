// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
namespace SoloASBridge::Compat
{
void NotifyClassDefaultObjectRegistered(UClass* Class, UObject* DefaultObject)
{
    NotifyRegistrationEvent(Class->GetOutermost()->GetFName(), Class->GetDefaultObjectName(),
        ENotifyRegistrationType::NRT_ClassCDO, ENotifyRegistrationPhase::NRP_Finished,
        nullptr, false, DefaultObject);
}
int32 ScriptArrayCapacity(const FScriptArray& Array) { return Array.Max(); }
}
namespace SoloASBridge::Compat
{
FKey ResolveVirtualKey(const FKey& Key) { return Key.GetVirtualKey(); }
const FKey& VirtualGamepadAcceptKey() { return EKeys::Virtual_Gamepad_Accept; }
const FKey& VirtualGamepadBackKey() { return EKeys::Virtual_Gamepad_Back; }
}
namespace SoloASBridge::Compat
{
uint8 GetBoolFieldSize(const FBoolProperty* P) { return P->GetBoolFieldSize(); }
uint8 GetBoolByteMask(const FBoolProperty* P) { return P->GetByteMask(); }
}
namespace SoloASBridge::Compat
{
int32 GetClassPropertiesStartOffset(const UClass* Class) { return Class->GetPropertiesStartOffset(); }
FSimpleMulticastDelegate& OnPostEngineInit() { return FCoreDelegates::GetOnPostEngineInit(); }
bool SetScriptEnumValues(UEnum* Enum, TArray<TPair<FName, int64>>& Values)
{
    return Enum->SetEnums(Values, UEnum::ECppForm::EnumClass, UEnum::EUnderlyingType::int8, EEnumFlags::None, UEnum::EAddMaxKeyIfMissing::Yes);
}
bool ContainsAnyObjectReference(const FProperty* Property, TArray<const FStructProperty*>& Encountered)
{
    return Property->ContainsObjectReference(Encountered, EPropertyObjectReferenceType_Any);
}
void ProcessMulticastDelegate(FMulticastScriptDelegate& Delegate, void* Parameters)
{
    Delegate.ProcessDelegate<UObject>(Parameters);
}
int64 GetCollisionObjectTypes(const FCollisionObjectQueryParams& Params) { return Params.GetObjectTypesToQuery(); }
bool SetCollisionObjectTypes(FCollisionObjectQueryParams& Params, int64 Value)
{
    Params.SetObjectTypesToQuery(Value);
    return true;
}
int64 GetCollisionQueryBitfield(const FCollisionObjectQueryParams& Params) { return Params.GetQueryBitfield64(); }
void GetNestedObjects(const UObject* Outer, TArray<UObject*>& Objects, EObjectFlags ExclusionFlags)
{
    GetObjectsWithOuter(Outer, Objects, EGetObjectsFlags::IncludeNestedObjects, ExclusionFlags);
}
void ObserveEnumPropertyStorage(const UEnum* Enum, const FNumericProperty* Storage) {}
EEnumStorage EnumStorageForBinding(const UEnum* Enum, EEnumStorage DeclaredStorage)
{
    const auto Type = Enum->GetUnderlyingType();
    return Type == UEnum::EUnderlyingType::int8 ? EEnumStorage::int8
        : Type == UEnum::EUnderlyingType::uint8 ? EEnumStorage::uint8 : EEnumStorage::Other;
}
bool SetEnumValues(UEnum* Enum, TArray<TPair<FName, int64>>& Values, EEnumStorage Storage, EEnumFlags Flags, bool AddMax)
{
    if (Storage == EEnumStorage::Other) return false;
    return Enum->UEnum::SetEnums(Values, UEnum::ECppForm::EnumClass,
        Storage == EEnumStorage::int8 ? UEnum::EUnderlyingType::int8 : UEnum::EUnderlyingType::uint8,
        Flags, AddMax ? UEnum::EAddMaxKeyIfMissing::Yes : UEnum::EAddMaxKeyIfMissing::No);
}
void GetDirectObjects(const UObject* Outer, TArray<UObject*>& Objects) { GetObjectsWithOuter(Outer, Objects, EGetObjectsFlags::None); }
void GetPackageObjects(const UPackage* Package, TArray<UObject*>& Objects) { GetObjectsWithPackage(Package, Objects, EGetObjectsFlags::IncludeNestedObjects); }
void SetPreSpawnInitialization(FActorSpawnParameters& Params, TFunction<void(AActor*)> Callback)
{
    Params.CustomPreSpawnInitialization = MoveTemp(Callback);
}
bool UseDedicatedInstancingGraph(const UObject* Object, TOptional<bool> UsesOverrides)
{
    return FOverridableSerializationLogic::ShouldUseDedicatedInstancingGraph(Object, UsesOverrides);
}
bool UseDedicatedInstancingGraphForNewObject(const UClass* Class, EObjectFlags Flags, const UObject* Template, bool UsesOverrides)
{
    return FOverridableSerializationLogic::ShouldUseDedicatedInstancingGraphForNewObject(Class, Flags, Template, UsesOverrides);
}

}

#if WITH_METADATA
const TMap<FName, FString>* SoloASBridge::Compat::GetObjectMetadata(const UObject* Object)
{
    return FMetaData::GetMapForObject(Object);
}
#endif

const TSharedPtr<FNetGUIDCache>& SoloASBridge::Compat::GetNetGuidCache(const UNetDriver& Driver)
{
    return Driver.GetNetGuidCache();
}

bool SoloASBridge::Compat::IsWorldRetired(const UWorld& World)
{
    return World.IsCleanedUp();
}

#if WITH_METADATA
void SoloASBridge::Compat::SetPackageMetadata(UPackage* Package, UObject* Object, const TCHAR* Key, const TCHAR* Value)
{ Package->GetMetaData().SetValue(Object, Key, Value); }
#endif

void SoloASBridge::Compat::ClearUnversionedSchemas(UStruct* Struct)
{
    const auto* Game = Struct->UnversionedGameSchema.exchange(nullptr, std::memory_order_acq_rel);
    FMemory::Free(const_cast<FUnversionedStructSchema*>(Game));
#if WITH_EDITORONLY_DATA
    const auto* Editor = Struct->UnversionedEditorSchema.exchange(nullptr, std::memory_order_acq_rel);
    FMemory::Free(const_cast<FUnversionedStructSchema*>(Editor));
#endif
}
void SoloASBridge::Compat::InheritObjectOverrides(UObject* Object, const UObject* DefaultData)
{
    if (FOverridableManager* Manager = FOverridableManager::TryGet()) Manager->InheritEnabledFrom(Object, DefaultData);
}

namespace SoloASBridge::Compat
{
int32 ScriptArrayNumUnchecked(const FScriptArray& Array) { return Array.NumUnchecked(); }
void SetPrimitiveLightmapType(UPrimitiveComponent& Component, ELightmapType Type) { Component.SetLightmapType(Type); }
}

namespace SoloASBridge::Compat
{
bool AddActorPostInitCallback(FObjectInitializer& Initializer, UObject*, UClass*, TFunction<void()> Callback)
{
    Initializer.AddPropertyPostInitCallback(MoveTemp(Callback));
    return true;
}
bool HasActorPostInitCallback(const FObjectInitializer&) { return false; }
}
