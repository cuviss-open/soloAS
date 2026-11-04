// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
namespace SoloASBridge::Compat
{
void NotifyClassDefaultObjectRegistered(UClass* Class, UObject* DefaultObject)
{
    NotifyRegistrationEvent(*Class->GetOutermost()->GetName(), *Class->GetDefaultObjectName().ToString(),
        ENotifyRegistrationType::NRT_ClassCDO, ENotifyRegistrationPhase::NRP_Finished,
        nullptr, false, DefaultObject);
}
}
namespace SoloASBridge::Compat
{
// UE 5.5 already resolves Virtual_Accept/Back to the platform gamepad keys.
FKey ResolveVirtualKey(const FKey& Key) { return EKeys::GetKeyDetails(Key).IsValid() ? Key : FKey(); }
const FKey& VirtualGamepadAcceptKey() { return EKeys::Virtual_Accept; }
const FKey& VirtualGamepadBackKey() { return EKeys::Virtual_Back; }
}
namespace SoloASBridge::Compat
{
template<class Tag, auto Member> struct TBoolMemberAccess { friend auto Resolve(Tag) { return Member; } };
struct FBoolFieldSize { friend auto Resolve(FBoolFieldSize); };
struct FBoolByteMask { friend auto Resolve(FBoolByteMask); };
template struct TBoolMemberAccess<FBoolFieldSize, &FBoolProperty::FieldSize>;
template struct TBoolMemberAccess<FBoolByteMask, &FBoolProperty::ByteMask>;
uint8 GetBoolFieldSize(const FBoolProperty* P) { return P->*Resolve(FBoolFieldSize{}); }
uint8 GetBoolByteMask(const FBoolProperty* P) { return P->*Resolve(FBoolByteMask{}); }
struct FArrayCapacity { friend auto Resolve(FArrayCapacity); };
template struct TBoolMemberAccess<FArrayCapacity, &FScriptArray::ArrayMax>;
// Quiet-storage admission must read capacity without Num() assertions or slack arithmetic.
int32 ScriptArrayCapacity(const FScriptArray& Array) { return Array.*Resolve(FArrayCapacity{}); }

}
namespace SoloASBridge::Compat
{
namespace
{
// UE55 stores the representation on each FEnumProperty. Only plugin-owned
// factories may publish that observed representation; queries never use the
// caller's expected representation as evidence. Public object-slot serials prevent address reuse without reading handles.
TMap<const UEnum*, TPair<int32, EEnumStorage>> EnumPropertyStorage;
void RecordEnumStorage(const UEnum* Enum, EEnumStorage Storage)
{
    const int32 Index = GUObjectArray.ObjectToIndex(Enum);
    EnumPropertyStorage.Add(Enum, {GUObjectArray.AllocateSerialNumber(Index), Storage});
}
}
void ObserveEnumPropertyStorage(const UEnum* Enum, const FNumericProperty* Storage)
{
    if (!Enum || !Storage) return;
    const EEnumStorage Kind = Storage->GetClass() == FInt8Property::StaticClass() ? EEnumStorage::int8
        : Storage->GetClass() == FByteProperty::StaticClass() ? EEnumStorage::uint8 : EEnumStorage::Other;
    const auto* Existing = EnumPropertyStorage.Find(Enum);
    RecordEnumStorage(Enum, Existing && Existing->Key == GUObjectArray.IndexToObject(GUObjectArray.ObjectToIndex(Enum))->GetSerialNumber() && Existing->Value != Kind ? EEnumStorage::Other : Kind);
}
int32 GetClassPropertiesStartOffset(const UClass* Class) { return 0; } // UE 5.5 has no class partials; property offsets are relative to the UObject.
FSimpleMulticastDelegate& OnPostEngineInit() { return FCoreDelegates::OnPostEngineInit; }
bool SetScriptEnumValues(UEnum* Enum, TArray<TPair<FName, int64>>& Values)
{
    const bool Result = Enum->SetEnums(Values, UEnum::ECppForm::EnumClass, EEnumFlags::None, true);
    if (Result) RecordEnumStorage(Enum, EEnumStorage::int8);
    return Result;
}
bool ContainsAnyObjectReference(const FProperty* Property, TArray<const FStructProperty*>& Encountered)
{
    return Property->ContainsObjectReference(Encountered, EPropertyObjectReferenceType::Any);
}
void ProcessMulticastDelegate(FMulticastScriptDelegate& Delegate, void* Parameters)
{
    Delegate.ProcessMulticastDelegate<UObject>(Parameters);
}
int64 GetCollisionObjectTypes(const FCollisionObjectQueryParams& Params) { return static_cast<uint32>(Params.ObjectTypesToQuery); }
bool SetCollisionObjectTypes(FCollisionObjectQueryParams& Params, int64 Value)
{
    if (Value < MIN_int32 || Value > static_cast<int64>(MAX_uint32)) return false;
    Params.ObjectTypesToQuery = static_cast<int32>(static_cast<uint32>(Value));
    return true;
}
int64 GetCollisionQueryBitfield(const FCollisionObjectQueryParams& Params) { return static_cast<uint32>(Params.GetQueryBitfield()); }
void GetNestedObjects(const UObject* Outer, TArray<UObject*>& Objects, EObjectFlags ExclusionFlags)
{
    GetObjectsWithOuter(Outer, Objects, true, ExclusionFlags);
}
EEnumStorage EnumStorageForBinding(const UEnum* Enum, EEnumStorage DeclaredStorage)
{
    const auto* Observed = EnumPropertyStorage.Find(Enum);
    const FUObjectItem* Slot = Observed ? GUObjectArray.IndexToObject(GUObjectArray.ObjectToIndex(Enum)) : nullptr;
    return Slot && Slot->GetSerialNumber() == Observed->Key ? Observed->Value : EEnumStorage::Other;
}
bool SetEnumValues(UEnum* Enum, TArray<TPair<FName, int64>>& Values, EEnumStorage Storage, EEnumFlags Flags, bool AddMax)
{
    if (Storage == EEnumStorage::Other) return false;
    const bool Result = Enum->UEnum::SetEnums(Values, UEnum::ECppForm::EnumClass, Flags, AddMax);
    if (Result) RecordEnumStorage(Enum, Storage);
    return Result;
}
void GetDirectObjects(const UObject* Outer, TArray<UObject*>& Objects) { GetObjectsWithOuter(Outer, Objects, false); }
void GetPackageObjects(const UPackage* Package, TArray<UObject*>& Objects) { GetObjectsWithPackage(Package, Objects, true); }
void SetPreSpawnInitialization(FActorSpawnParameters& Params, TFunction<void(AActor*)> Callback)
{
    Params.CustomPreSpawnInitalization = MoveTemp(Callback);
}
bool UseDedicatedInstancingGraph(const UObject* Object, TOptional<bool> UsesOverrides)
{
    // UE 5.5 EditorObject.cpp:922 uses the override-enabled predicate.
    return UsesOverrides.IsSet() ? UsesOverrides.GetValue() : (Object && FOverridableManager::Get().IsEnabled(*Object));
}
bool UseDedicatedInstancingGraphForNewObject(const UClass* Class, EObjectFlags Flags, const UObject* Template, bool UsesOverrides)
{
    return UsesOverrides;
}

}

#if WITH_METADATA
const TMap<FName, FString>* SoloASBridge::Compat::GetObjectMetadata(const UObject* Object)
{
    return UMetaData::GetMapForObject(Object);
}
#endif

const TSharedPtr<FNetGUIDCache>& SoloASBridge::Compat::GetNetGuidCache(const UNetDriver& Driver)
{
    return Driver.GuidCache;
}

bool SoloASBridge::Compat::IsWorldRetired(const UWorld& World)
{
    // UE 5.5 IsInitializedAndNeedsCleanup and IsInitialized both read bIsWorldInitialized;
    // IsInitialized is also available in Game targets.
    return World.bIsTearingDown || !World.IsInitialized();
}

#if WITH_METADATA
void SoloASBridge::Compat::SetPackageMetadata(UPackage* Package, UObject* Object, const TCHAR* Key, const TCHAR* Value)
{ Package->GetMetaData()->SetValue(Object, Key, Value); }
#endif

void SoloASBridge::Compat::ClearUnversionedSchemas(UStruct* Struct)
{
    const auto* Game = Struct->UnversionedGameSchema;
    Struct->UnversionedGameSchema = nullptr;
    FMemory::Free(const_cast<FUnversionedStructSchema*>(Game));
#if WITH_EDITORONLY_DATA
    const auto* Editor = Struct->UnversionedEditorSchema;
    Struct->UnversionedEditorSchema = nullptr;
    FMemory::Free(const_cast<FUnversionedStructSchema*>(Editor));
#endif
}
void SoloASBridge::Compat::InheritObjectOverrides(UObject* Object, const UObject* DefaultData)
{
#if WITH_EDITORONLY_DATA
    FOverridableManager* Manager = &FOverridableManager::Get();
    if (!Manager->IsEnabled(*Object))
    {
        const UObject* Outer = Object->GetOuter();
        if ((Outer && Manager->IsEnabled(*Outer)) || (DefaultData && Manager->IsEnabled(*DefaultData)))
            Manager->Enable(*Object);
    }
#endif
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
