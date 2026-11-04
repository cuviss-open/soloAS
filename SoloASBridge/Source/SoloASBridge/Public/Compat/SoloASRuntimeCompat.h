// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Tickable.h"
#include "SoloASEngineCompat.h"
#include "Algo/IntroSort.h"
#include "Algo/Sort.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/Class.h"
#include "UObject/UObjectArray.h"
#include "UObject/UnrealType.h"
struct FKey;
class FProperty;
class FBoolProperty;
class FNumericProperty;
class FStructProperty;
class UEnum;
struct FCollisionObjectQueryParams;
struct FActorSpawnParameters;
class AActor;
class UPackage;
class UNetDriver;
class FNetGUIDCache;
class UWorld;
// SOLOAS-COMPAT-BEGIN(<5.5) Legacy retarget adapters accept skeletal mesh objects [test: SoloAS.ApiFixb.AccessorSurface] [id: CS.Compatibility.Bridge.LegacyRetargetSource.Forward]
#if WITH_EDITOR && SOLOAS_UE_BEFORE(5, 5)
class USkeletalMesh;
#endif
// SOLOAS-COMPAT-END
// SOLOAS-COMPAT-BEGIN(<5.7) Old tickable registration has no tick-type constructor; keep UHT consumers unconditional [test: SoloAS.EngineCompat.ReflectionSurface] [id: CS.Compatibility.Bridge.SoloASRuntimeCompat.TickableRegistration]
#if SOLOAS_UE_BEFORE(5, 7)
class FSoloASTickableGameObject : public FTickableGameObject
{
public:
    FSoloASTickableGameObject() = default;
    explicit FSoloASTickableGameObject(ETickableTickType TickType)
    {
        SetTickableTickType(TickType);
    }
#if SOLOAS_UE_BEFORE(5, 5)
    // 5.4 queues registration and queries the most-derived tick type at the first tick.
    // Its scheduler checks IsAllowedToTick even for Always entries.
    void SetTickableTickType(ETickableTickType TickType) { bAllowsTick = TickType != ETickableTickType::Never; }
    bool IsAllowedToTick() const override { return bAllowsTick; }
private:
    bool bAllowsTick = true;
#endif
};
#else
using FSoloASTickableGameObject = FTickableGameObject;
#endif
#if SOLOAS_UE_BEFORE(5, 5)
inline constexpr ETickableTickType SoloASNewObjectTickType = ETickableTickType::Conditional;
#else
inline constexpr ETickableTickType SoloASNewObjectTickType = ETickableTickType::NewObject;
#endif
// SOLOAS-COMPAT-END
class UPrimitiveComponent;
enum class ELightmapType : uint8;
namespace SoloASBridge::Compat
{
// SOLOAS-COMPAT-BEGIN(<5.5) Retarget source accessors were not public in UE 5.4 [test: SoloAS.ApiFixb.AccessorSurface] [id: CS.Compatibility.Bridge.LegacyRetargetSource.Declarations]
#if WITH_EDITOR && SOLOAS_UE_BEFORE(5, 5)
SOLOASBRIDGE_API void SetLegacyRetargetSourceAsset(UObject* Asset, USkeletalMesh* Mesh);
SOLOASBRIDGE_API const TSoftObjectPtr<USkeletalMesh>& GetLegacyRetargetSourceAsset(const UObject* Asset);
#endif
// SOLOAS-COMPAT-END
SOLOASBRIDGE_API void RequestScriptGenerationExit();
// SOLOAS-COMPAT-BEGIN(<5.6) UE 5.5 exposes the object slot directly [test: SoloAS.RuntimePayloadNativeEnumQuietWitness] [id: CS.Compatibility.Bridge.SoloASRuntimeCompat.ObjectItemAccess]
inline UObjectBase* ObjectFromItem(const FUObjectItem* Item)
{
#if SOLOAS_UE_BEFORE(5, 6)
    return Item->Object;
#else
    return Item->GetObject();
#endif
}
// SOLOAS-COMPAT-END
#if WITH_METADATA
SOLOASBRIDGE_API const TMap<FName, FString>* GetObjectMetadata(const UObject* Object);
SOLOASBRIDGE_API void SetPackageMetadata(UPackage* Package, UObject* Object, const TCHAR* Key, const TCHAR* Value);
#endif
// A: UE-AS fork removes const from InputModifier::ModifyRaw and exposes only the FName timer adapter.
// The script int32 view preserves native high bits on UE 5.8.
SOLOASBRIDGE_API SIZE_T GetCollisionObjectMaskScriptOffset();
SOLOASBRIDGE_API bool IsScriptMethodConst(const UFunction* Function);
SOLOASBRIDGE_API bool IsReplacedScriptTimer(const UFunction* Function);
#if WITH_EDITOR
SOLOASBRIDGE_API void PrepareScriptBindingModules();
#endif
// A: CS.Bridge.PluginProjectSearchPaths.PublicReference preserves the engine-owned array view.
SOLOASBRIDGE_API const TArray<FString>& GetAdditionalProjectPluginSearchPaths();
SOLOASBRIDGE_API void SetClassDefaultObject(UClass* Class, UObject* Object);
SOLOASBRIDGE_API void ClearUnversionedSchemas(UStruct* Struct);
SOLOASBRIDGE_API void InheritObjectOverrides(UObject* Object, const UObject* DefaultData);
SOLOASBRIDGE_API const TSharedPtr<FNetGUIDCache>& GetNetGuidCache(const UNetDriver& Driver);
SOLOASBRIDGE_API bool IsWorldRetired(const UWorld& World);
                                                                                                                                                                                      
#if SOLOAS_UE_BEFORE(5, 6)
inline constexpr EPropertyFlags VirtualPropertyFlag = CPF_None;
inline constexpr EObjectFlags ImmutableDefaultObjectFlag = RF_NoFlags;
#else
inline constexpr EPropertyFlags VirtualPropertyFlag = CPF_Virtual;
inline constexpr EObjectFlags ImmutableDefaultObjectFlag = RF_ImmutableDefaultObject;
#endif
// SOLOAS-COMPAT-END
                                                                                                                                                                                              
inline bool PropertyContainsFinishDestroy(const FProperty* Property, TArray<const FStructProperty*>& Encountered)
{
#if SOLOAS_UE_BEFORE(5, 5)
    return false;
#else
    return Property->ContainsFinishDestroy(Encountered);
#endif
}
inline bool StructOpsHasClearOnFinishDestroy(const UScriptStruct::ICppStructOps* Ops)
{
#if SOLOAS_UE_BEFORE(5, 5)
    return false;
#else
    return Ops && Ops->HasClearOnFinishDestroy();
#endif
}
inline bool IsPropertyInContainer(const FProperty* Property, const UStruct* Container)
{
#if SOLOAS_UE_BEFORE(5, 5)
    return Property->IsInContainer(const_cast<UStruct*>(Container));
#else
    return Property->IsInContainer(Container);
#endif
}
inline int32 GetPropertyElementSize(const FProperty* Property)
{
#if SOLOAS_UE_BEFORE(5, 5)
    return Property->ElementSize;
#else
    return Property->GetElementSize();
#endif
}
inline void SetPropertyElementSize(FProperty* Property, int32 Value)
{
#if SOLOAS_UE_BEFORE(5, 5)
    Property->ElementSize = Value;
#else
    Property->SetElementSize(Value);
#endif
}
#if SOLOAS_UE_BEFORE(5, 5)
inline constexpr EInternalObjectFlags AsyncLoadingObjectFlags = EInternalObjectFlags::AsyncLoading;
inline constexpr ERenameFlags AllowPackageLinkerMismatchRenameFlag = REN_None;
#else
inline constexpr EInternalObjectFlags AsyncLoadingObjectFlags = EInternalObjectFlags_AsyncLoading;
inline constexpr ERenameFlags AllowPackageLinkerMismatchRenameFlag = REN_AllowPackageLinkerMismatch;
#endif
// SOLOAS-COMPAT-END

// SOLOAS-COMPAT-BEGIN(<5.8) UE 5.7 keeps enum storage on FProperty, not UEnum [test: SoloAS.RuntimePayloadNativeEnumQuietWitness] [id: CS.Compatibility.Bridge.SoloASRuntimeCompat.EnumStorage]
enum class EEnumStorage { int8, uint8, Other };
SOLOASBRIDGE_API void ObserveEnumPropertyStorage(const UEnum* Enum, const FNumericProperty* Storage);
SOLOASBRIDGE_API EEnumStorage EnumStorageForBinding(const UEnum* Enum, EEnumStorage DeclaredStorage);
SOLOASBRIDGE_API bool SetEnumValues(UEnum* Enum, TArray<TPair<FName, int64>>& Values,
    EEnumStorage Storage, EEnumFlags Flags, bool AddMax);
// SOLOAS-COMPAT-END
                                                                                                                                                      
#if SOLOAS_UE_BEFORE(5, 8)
inline constexpr EObjectFlags DynamicImportObjectFlag = RF_NoFlags;
inline constexpr EPropertyObjectReferenceType AnyObjectReferences = EPropertyObjectReferenceType::Any;
#else
inline constexpr EObjectFlags DynamicImportObjectFlag = RF_HasDynamicImports;
inline constexpr EPropertyObjectReferenceType AnyObjectReferences = EPropertyObjectReferenceType_Any;
#endif
// SOLOAS-COMPAT-END
SOLOASBRIDGE_API void NotifyClassDefaultObjectRegistered(UClass* Class, UObject* DefaultObject);
SOLOASBRIDGE_API int32 ScriptArrayCapacity(const FScriptArray& Array);
SOLOASBRIDGE_API int32 ScriptArrayNumUnchecked(const FScriptArray& Array);
SOLOASBRIDGE_API void SetPrimitiveLightmapType(UPrimitiveComponent& Component, ELightmapType Type);
SOLOASBRIDGE_API FKey ResolveVirtualKey(const FKey& Key);
SOLOASBRIDGE_API const FKey& VirtualGamepadAcceptKey();
SOLOASBRIDGE_API const FKey& VirtualGamepadBackKey();
SOLOASBRIDGE_API uint8 GetBoolFieldSize(const FBoolProperty* Property);
SOLOASBRIDGE_API uint8 GetBoolByteMask(const FBoolProperty* Property);
SOLOASBRIDGE_API void GetDirectObjects(const UObject* Outer, TArray<UObject*>& Objects);
SOLOASBRIDGE_API void GetPackageObjects(const UPackage* Package, TArray<UObject*>& Objects);
SOLOASBRIDGE_API void SetPreSpawnInitialization(FActorSpawnParameters& Params, TFunction<void(AActor*)> Callback);
SOLOASBRIDGE_API bool UseDedicatedInstancingGraph(const UObject* Object, TOptional<bool> UsesOverrides = {});
SOLOASBRIDGE_API bool UseDedicatedInstancingGraphForNewObject(const UClass* Class, EObjectFlags Flags,
    const UObject* Template, bool UsesOverrides);

SOLOASBRIDGE_API bool AddActorPostInitCallback(FObjectInitializer& Initializer, UObject* Object, UClass* ScriptAnchor, TFunction<void()> Callback);
SOLOASBRIDGE_API bool HasActorPostInitCallback(const FObjectInitializer& Initializer);
SOLOASBRIDGE_API FSimpleMulticastDelegate& OnPostEngineInit();
SOLOASBRIDGE_API int32 GetClassPropertiesStartOffset(const UClass* Class);
SOLOASBRIDGE_API bool SetScriptEnumValues(UEnum* Enum, TArray<TPair<FName, int64>>& Values);
SOLOASBRIDGE_API bool ContainsAnyObjectReference(const FProperty* Property, TArray<const FStructProperty*>& Encountered);
SOLOASBRIDGE_API void ProcessMulticastDelegate(FMulticastScriptDelegate& Delegate, void* Parameters);
SOLOASBRIDGE_API int64 GetCollisionObjectTypes(const FCollisionObjectQueryParams& Params);
SOLOASBRIDGE_API bool SetCollisionObjectTypes(FCollisionObjectQueryParams& Params, int64 Value);
SOLOASBRIDGE_API int64 GetCollisionQueryBitfield(const FCollisionObjectQueryParams& Params);
SOLOASBRIDGE_API void GetNestedObjects(const UObject* Outer, TArray<UObject*>& Objects, EObjectFlags ExclusionFlags = RF_NoFlags);

// UE 5.7 IntroSort accepts pointers only. Sort indices first, then apply the
// permutation with the iterator's ADL Swap so script values retain their storage.
// SOLOAS-COMPAT-BEGIN(<5.8) UE 5.7 IntroSort accepts pointers only [test: ASRegression] [id: CS.Compatibility.Bridge.SoloASRuntimeCompat.IteratorIntroSort]
template <typename Iterator, typename Projection, typename Predicate>
void IntroSort(Iterator First, int32 Num, Projection Proj, Predicate Pred)
{
#if SOLOAS_UE_BEFORE(5, 8)
    TArray<int32> Order;
    Order.Reserve(Num);
    for (int32 Index = 0; Index < Num; ++Index) Order.Add(Index);
    Algo::Sort(Order, [&](int32 A, int32 B) { return Pred(Invoke(Proj, First + A), Invoke(Proj, First + B)); });
    TArray<int32> Destinations;
    Destinations.SetNumUninitialized(Num);
    for (int32 Index = 0; Index < Num; ++Index) Destinations[Order[Index]] = Index;
    for (int32 Index = 0; Index < Num; ++Index)
    {
        while (Destinations[Index] != Index)
        {
            const int32 Other = Destinations[Index];
            Swap(First + Index, First + Other);
            Swap(Destinations[Index], Destinations[Other]);
        }
    }
#else
    AlgoImpl::IntroSortInternal(First, Num, Proj, Pred);
#endif
}
// SOLOAS-COMPAT-END
}
