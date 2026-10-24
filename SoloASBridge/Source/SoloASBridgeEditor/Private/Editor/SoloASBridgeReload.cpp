#include "Editor/SoloASBridgeReload.h"
#include "Editor/SoloASRetiredLoadingExportRepair.h"

#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"
#include "Misc/ScopeExit.h"
#include "UObject/FieldPathProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"

#include "HAL/IConsoleManager.h"
#include "Kismet2/ReloadUtilities.h"
#include "Misc/OutputDevice.h"
#include "Templates/UnrealTemplate.h"

namespace
{
bool GBridgeReloadInProgress = false; // Accessed only after the game-thread check.

bool PropertyTreeContainsRetainedTypePath(const FProperty* Property,
	TSet<const FProperty*>& VisitedProperties)
{
	if (CastField<FFieldPathProperty>(Property) || CastField<FSoftObjectProperty>(Property))
		return true;
	// Leaf properties have no recursive edges; only compound properties need cycle tracking.
	constexpr uint64 CompoundPropertyFlags = CASTCLASS_FStructProperty | CASTCLASS_FArrayProperty
		| CASTCLASS_FSetProperty | CASTCLASS_FMapProperty | CASTCLASS_FOptionalProperty;
	if ((Property->GetCastFlags() & CompoundPropertyFlags) == 0) return false;
	if (VisitedProperties.Contains(Property))
		return false;
	VisitedProperties.Add(Property);
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		for (TFieldIterator<FProperty> It(StructProperty->Struct, EFieldIteratorFlags::IncludeSuper,
			EFieldIteratorFlags::IncludeDeprecated, EFieldIteratorFlags::ExcludeInterfaces); It; ++It)
			if (PropertyTreeContainsRetainedTypePath(*It, VisitedProperties)) return true;
	}
	else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		return PropertyTreeContainsRetainedTypePath(ArrayProperty->Inner, VisitedProperties);
	else if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		return PropertyTreeContainsRetainedTypePath(SetProperty->ElementProp, VisitedProperties);
	else if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		return PropertyTreeContainsRetainedTypePath(MapProperty->KeyProp, VisitedProperties)
			|| PropertyTreeContainsRetainedTypePath(MapProperty->ValueProp, VisitedProperties);
	else if (const FOptionalProperty* OptionalProperty = CastField<FOptionalProperty>(Property))
		return PropertyTreeContainsRetainedTypePath(OptionalProperty->GetValueProperty(), VisitedProperties);
	return false;
}

}

ESoloASBridgeReloadResult SoloASBridge::ValidateStockReloadInputs(
    const TMap<UDelegateFunction*, UDelegateFunction*>& Delegates,
    const TMap<UObject*, UObject*>& Assets
)
{
    if (!Delegates.IsEmpty())
    {
        return ESoloASBridgeReloadResult::UnsupportedDelegates;
    }
    if (!IsInGameThread()) return ESoloASBridgeReloadResult::WrongThread;
    if (GBridgeReloadInProgress || IsReloadActive()) return ESoloASBridgeReloadResult::ReloadAlreadyActive;

    for (const TPair<UObject*, UObject*>& Pair : Assets)
    {
        const UObject* OldAsset = Pair.Key;
        const UObject* NewAsset = Pair.Value;
        const UClass* OldClass = OldAsset != nullptr ? OldAsset->GetClass() : nullptr;
        const UClass* NewClass = NewAsset != nullptr ? NewAsset->GetClass() : nullptr;
        if (OldAsset == nullptr || NewAsset == nullptr || OldAsset == NewAsset
            || OldClass == nullptr || NewClass == nullptr || OldClass == NewClass
            || !OldClass->HasAnyClassFlags(CLASS_NewerVersionExists)
            || NewClass->HasAnyClassFlags(CLASS_NewerVersionExists))
        {
            return ESoloASBridgeReloadResult::UnsupportedAssets;
        }
    }
    return ESoloASBridgeReloadResult::Completed;
}

const TCHAR* SoloASBridge::DescribeStockReloadResult(ESoloASBridgeReloadResult Result)
{
    switch (Result)
    {
    case ESoloASBridgeReloadResult::Completed: return TEXT("completed");
    case ESoloASBridgeReloadResult::UnsupportedDelegates: return TEXT("unsupported-delegates");
    case ESoloASBridgeReloadResult::UnsupportedAssets: return TEXT("unsupported-assets");
    case ESoloASBridgeReloadResult::WrongThread: return TEXT("wrong-thread");
    case ESoloASBridgeReloadResult::ReloadAlreadyActive: return TEXT("reload-already-active");
    }
    return TEXT("unknown-reload-result");
}

ESoloASBridgeReloadResult SoloASBridge::ReinstanceWithStockReload(
    const TMap<UClass*, UClass*>& Classes,
    const TMap<UScriptStruct*, UScriptStruct*>& Structs,
    const TSet<UEnum*>& Enums,
    const TMap<UDelegateFunction*, UDelegateFunction*>& Delegates,
    const TMap<UObject*, UObject*>& Assets
)
{
    return ReinstanceWithStockReload(Classes, Structs, Enums, Delegates, Assets, nullptr);
}

ESoloASBridgeReloadResult SoloASBridge::ReinstanceWithStockReload(
    const TMap<UClass*, UClass*>& Classes,
    const TMap<UScriptStruct*, UScriptStruct*>& Structs,
    const TSet<UEnum*>& Enums,
    const TMap<UDelegateFunction*, UDelegateFunction*>& Delegates,
    const TMap<UObject*, UObject*>& Assets,
    int32* OutFailureDetail,
    bool bRunGC
)
{
    if (OutFailureDetail) *OutFailureDetail = 0;
    const ESoloASBridgeReloadResult StockReloadResult =
        ValidateStockReloadInputs(Delegates, Assets);
    if (StockReloadResult != ESoloASBridgeReloadResult::Completed)
    {
        return StockReloadResult;
    }

    // Script classes and their actions already migrated through the script
    // hierarchy pass. With no remaining stock domain or GC, there is no native
    // reload to complete; native completion would rebuild the entire action list.
    if (!bRunGC && Classes.IsEmpty() && Structs.IsEmpty() && Enums.IsEmpty() && Assets.IsEmpty())
    {
        return ESoloASBridgeReloadResult::Completed;
    }

    return RunWithStockCopyPropertiesScope([&]()
    {
        FSoloASRetiredLoadingExportRepair LoadingExports;
        FReload Reload(EActiveReloadType::Reinstancing, TEXT("Angelscript"), *GLog);
        for (const TPair<UScriptStruct*, UScriptStruct*>& Change : Structs)
        {
            Reload.NotifyChange(Change.Value, Change.Key);
        }
        for (const TPair<UClass*, UClass*>& Change : Classes)
        {
            Reload.NotifyChange(Change.Value, Change.Key);
        }
        for (UEnum* Enum : Enums)
        {
            Reload.NotifyChange(Enum, Enum);
        }
        Reload.Reinstance();
        Reload.Finalize(bRunGC);
    }, OutFailureDetail);
}

ESoloASBridgeReloadResult SoloASBridge::RunWithStockCopyPropertiesScope(
    TFunctionRef<void()> Body, int32* OutFailureDetail)
{
    if (OutFailureDetail) *OutFailureDetail = 0;
    if (!IsInGameThread()) return ESoloASBridgeReloadResult::WrongThread;
    if (GBridgeReloadInProgress || IsReloadActive()) return ESoloASBridgeReloadResult::ReloadAlreadyActive;

    if (const IConsoleVariable* AggressiveReplacement = IConsoleManager::Get().FindConsoleVariable(
        TEXT("cpfuo.UseAggressiveReferenceReplacment"));
        AggressiveReplacement != nullptr && AggressiveReplacement->GetInt() == 1)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SOLOAS_RELOAD_WARNING cpfuo.UseAggressiveReferenceReplacment=1 enables aggressive CPFUO notification behavior; ")
            TEXT("disable it for script reloads."));
    }

    TGuardValue<bool> ReloadInProgress(GBridgeReloadInProgress, true);
    Body();
    return ESoloASBridgeReloadResult::Completed;
}

void SoloASBridge::RefreshRetainedScriptTypeReferences(const TMap<UClass*, UClass*>& Classes,
    const TMap<UScriptStruct*, UScriptStruct*>& Structs,
    const TMap<UObject*, UObject*>& Objects,
    const TSet<TWeakObjectPtr<UObject>>* CandidateObjects)
{
    TMap<UObject*, UObject*> Replacements;
    for (const auto& Pair : Classes) Replacements.Add(Pair.Key, Pair.Value);
    for (const auto& Pair : Structs) Replacements.Add(Pair.Key, Pair.Value);
    for (const auto& Pair : Objects) Replacements.Add(Pair.Key, Pair.Value);
    if (Replacements.IsEmpty()) return;
    TArray<FString> CurrentOwnerPrefixes;
    for (const auto& Pair : Classes)
        if (Pair.Value) CurrentOwnerPrefixes.Add(Pair.Value->GetPathName() + TEXT(":"));
    for (const auto& Pair : Structs)
        if (Pair.Value) CurrentOwnerPrefixes.Add(Pair.Value->GetPathName() + TEXT(":"));

	TMap<const FProperty*, bool> PropertyHasRetainedTypePathCache;
	auto HasRetainedTypePath = [&](const FProperty* Property)
	{
		if (const bool* Cached = PropertyHasRetainedTypePathCache.Find(Property)) return *Cached;
		TSet<const FProperty*> VisitedProperties;
		const bool bHasPath = PropertyTreeContainsRetainedTypePath(Property, VisitedProperties);
		PropertyHasRetainedTypePathCache.Add(Property, bHasPath);
		return bHasPath;
	};
    struct FClassPaths
    {
        TArray<const FProperty*> Direct;
        TArray<const FProperty*> Roots;
        bool bRecursive = false;
        bool bRetiredClass = false;
    };
    class FPathReferenceArchive final : public FArchiveReplaceObjectRef<UObject>
    {
    public:
        using FArchiveReplaceObjectRef<UObject>::FArchiveReplaceObjectRef;
        void SetObject(UObject* Object)
        {
            SearchObject = Object;
            SerializingObject = nullptr;
            Count = 0;
            SerializedObjects.Reset();
            PendingSerializationObjects.Reset();
            ReplacedReferences.Reset();
            ClearError();
        }
    };
    FPathReferenceArchive References(nullptr, Replacements,
        EArchiveReplaceObjectFlags::DelayStart | EArchiveReplaceObjectFlags::IgnoreOuterRef
        | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
    // Only fixed engine path serializers use this archive; their registry versions
    // stay current for this synchronous pass while per-owner search state is reset.
    References.SetShouldSkipUpdateCustomVersion(true);
    TMap<const UClass*, FClassPaths> ClassPathsCache;
    TMap<const UStruct*, TArray<const FProperty*>> StructPathsCache;
    auto GetStructPaths = [&](const UStruct* Struct) -> const TArray<const FProperty*>&
    {
        if (const auto* Cached = StructPathsCache.Find(Struct)) return *Cached;
        TArray<const FProperty*> Fields;
        for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper,
            EFieldIteratorFlags::IncludeDeprecated, EFieldIteratorFlags::ExcludeInterfaces); It; ++It)
            if (HasRetainedTypePath(*It)) Fields.Add(*It);
        return StructPathsCache.Add(Struct, MoveTemp(Fields));
    };
	auto RefreshObject = [&](UObject* Object)
	{
		if (Object == nullptr) return;
        if (!IsValid(Object) || Object->HasAnyFlags(RF_NeedInitialization | RF_NeedLoad | RF_NeedPostLoad)
            || Replacements.Contains(Object->GetClass())
            || Object->GetClass()->HasAnyClassFlags(CLASS_NewerVersionExists)) return;
		UClass* ObjectClass = Object->GetClass();
        FClassPaths* Paths = ClassPathsCache.Find(ObjectClass);
        if (Paths == nullptr)
        {
            FClassPaths Found;
            for (UClass* Cursor = ObjectClass; Cursor; Cursor = Cursor->GetSuperClass())
                Found.bRetiredClass |= Replacements.Contains(Cursor);
            for (TFieldIterator<FProperty> It(ObjectClass, EFieldIteratorFlags::IncludeSuper,
                EFieldIteratorFlags::IncludeDeprecated, EFieldIteratorFlags::ExcludeInterfaces); It; ++It)
            {
                if (!HasRetainedTypePath(*It)) continue;
                Found.Roots.Add(*It);
                if (CastField<FFieldPathProperty>(*It) || CastField<FSoftObjectProperty>(*It))
                    Found.Direct.Add(*It);
                else
                    Found.bRecursive = true;
            }
            Paths = &ClassPathsCache.Add(ObjectClass, MoveTemp(Found));
        }
        if (!Paths->bRecursive && Paths->Direct.IsEmpty()) return;
        bool bRetiredOwner = Paths->bRetiredClass;
        for (UObject* Outer = Object; Outer; Outer = Outer->GetOuter())
            bRetiredOwner |= Replacements.Contains(Outer);
        if (bRetiredOwner) return;

        // Only touch retained path values. Old instance layouts, object classes,
        // outers and archetypes remain owned by the actual reinstancer.
        References.SetObject(Object);
        auto RefreshPath = [&](const FProperty* Property, void* Address)
        {
            const int32 BeforeValue = References.GetCount();
            bool bPathRefreshed = false;
            if (const auto* PathProperty = CastField<FFieldPathProperty>(Property))
            {
                FFieldPath& Path = *PathProperty->GetPropertyValuePtr(Address);
                const int32 Before = References.GetCount();
                static_cast<FArchive&>(References) << Path;
                bool bRefresh = false;
                if (References.GetCount() != Before)
                {
                    struct FFieldPathCacheAccess : FFieldPath
                    {
                        static auto ClearMember() { return &FFieldPathCacheAccess::ClearCachedField; }
                    };
                    (Path.*FFieldPathCacheAccess::ClearMember())();
                    bPathRefreshed = true;
                }
                else if (Path.IsStale())
                {
                    // Inspect a copy: resolving a mismatched concrete type can clear its owner.
                    FFieldPath Copy(Path);
                    const FString Name = Copy.ToString();
                    for (const FString& Prefix : CurrentOwnerPrefixes) bRefresh |= Name.StartsWith(Prefix);
                }
                if (bRefresh)
                {
                    // A normal public serialization round trip clears the cached
                    // concrete field type without weakening TFieldPath<T>'s type check.
                    TArray<uint8> Bytes;
                    FMemoryWriter MemoryWriter(Bytes);
                    FObjectAndNameAsStringProxyArchive Writer(MemoryWriter, false);
                    Writer << Path;
                    FMemoryReader MemoryReader(Bytes);
                    FObjectAndNameAsStringProxyArchive Reader(MemoryReader, false);
                    Reader.SetCustomVersions(Writer.GetCustomVersions());
                    Reader << Path;
                    bPathRefreshed = true;
                }
            }
            else if (const auto* SoftProperty = CastField<FSoftObjectProperty>(Property))
            {
                static_cast<FArchive&>(References) << *SoftProperty->GetPropertyValuePtr(Address);
            }
            return bPathRefreshed || References.GetCount() != BeforeValue;
        };
        // Direct path fields need no recursive value iterator or per-object property list.
        // Recursive classes visit only the cached roots and fields that can hold a path.
        if (!Paths->bRecursive)
        {
            for (const FProperty* Property : Paths->Direct)
                for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
                    RefreshPath(Property, Property->ContainerPtrToValuePtr<void>(Object, Index));
            return;
        }
        struct FContainer { const FProperty* Property; void* Address; };
        TArray<FContainer> Containers;
        TArray<int32> RehashContainers;
        auto RefreshValue = [&](auto&& Self, const FProperty* Property, void* Address) -> bool
        {
            if (const auto* Struct = CastField<FStructProperty>(Property))
            {
                if (Replacements.Contains(Struct->Struct)) return false;
                bool bChanged = false;
                for (const FProperty* Field : MakeArrayView(GetStructPaths(Struct->Struct)))
                    for (int32 Index = 0; Index < Field->ArrayDim; ++Index)
                        bChanged |= Self(Self, Field, Field->ContainerPtrToValuePtr<void>(Address, Index));
                return bChanged;
            }
            if (!HasRetainedTypePath(Property)) return false;
            if (const auto* Array = CastField<FArrayProperty>(Property))
            {
                FScriptArrayHelper Helper(Array, Address);
                bool bChanged = false;
                for (int32 Index = 0; Index < Helper.Num(); ++Index)
                    bChanged |= Self(Self, Array->Inner, Helper.GetRawPtr(Index));
                return bChanged;
            }
            if (const auto* Map = CastField<FMapProperty>(Property))
            {
                const int32 ContainerIndex = Containers.Add({Property, Address});
                FScriptMapHelper Helper(Map, Address);
                const bool bVisitKeys = HasRetainedTypePath(Map->KeyProp);
                const bool bVisitValues = HasRetainedTypePath(Map->ValueProp);
                bool bKeysChanged = false;
                bool bValuesChanged = false;
                for (FScriptMapHelper::FIterator It(Helper); It; ++It)
                {
                    if (bVisitKeys) bKeysChanged |= Self(Self, Map->KeyProp, Helper.GetKeyPtr(It));
                    if (bVisitValues) bValuesChanged |= Self(Self, Map->ValueProp, Helper.GetValuePtr(It));
                }
                if (bKeysChanged) RehashContainers.Add(ContainerIndex);
                return bKeysChanged || bValuesChanged;
            }
            if (const auto* Set = CastField<FSetProperty>(Property))
            {
                const int32 ContainerIndex = Containers.Add({Property, Address});
                FScriptSetHelper Helper(Set, Address);
                bool bChanged = false;
                for (FScriptSetHelper::FIterator It(Helper); It; ++It)
                    bChanged |= Self(Self, Set->ElementProp, Helper.GetElementPtr(It));
                if (bChanged) RehashContainers.Add(ContainerIndex);
                return bChanged;
            }
            if (const auto* Optional = CastField<FOptionalProperty>(Property))
            {
                void* Value = Optional->GetValuePointerForReplaceIfSet(Address);
                return Value && Self(Self, Optional->GetValueProperty(), Value);
            }
            return RefreshPath(Property, Address);
        };
        for (const FProperty* Property : Paths->Roots)
            for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
                RefreshValue(RefreshValue, Property, Property->ContainerPtrToValuePtr<void>(Object, Index));
        // Only changed keys require hashing; unrelated user-defined hash callbacks stay untouched.
        RehashContainers.Sort();
        for (int32 Position = RehashContainers.Num() - 1; Position >= 0; --Position)
        {
            const FContainer& Container = Containers[RehashContainers[Position]];
            if (const auto* Map = CastField<FMapProperty>(Container.Property))
                FScriptMapHelper(Map, Container.Address).Rehash();
            else if (const auto* Set = CastField<FSetProperty>(Container.Property))
                FScriptSetHelper(Set, Container.Address).Rehash();
        }
    };
	if (CandidateObjects)
	{
		for (const TWeakObjectPtr<UObject>& Object : *CandidateObjects) RefreshObject(Object.Get());
	}
	else
	{
		for (TObjectIterator<UObject> ObjectIt; ObjectIt; ++ObjectIt)
			RefreshObject(*ObjectIt);
	}
}

namespace
{
const void* ResolveSerializerImport(const void* Function)
{
#if PLATFORM_WINDOWS && PLATFORM_CPU_X86_FAMILY
    const uint8* Code = static_cast<const uint8*>(Function);
    if (Code[0] == 0xff && Code[1] == 0x25)
    {
        int32 Displacement;
        FMemory::Memcpy(&Displacement, Code + 2, sizeof(Displacement));
        return *reinterpret_cast<const void* const*>(Code + 6 + Displacement);
    }
#endif
    return Function;
}
}

// Read-only serializer identity check; unsupported layouts retain the full probe.
bool SoloASBridge::HasOnlyReflectedReferences(const UObject* Object)
{
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    struct FSerializerSlots
    {
        int32 Archive = INDEX_NONE;
        int32 Structured = INDEX_NONE;
        const void* ArchiveFunction = nullptr;
        const void* StructuredFunction = nullptr;

        FSerializerSlots()
        {
            void* Module = FPlatformProcess::GetDllHandle(*FModuleManager::Get().GetModuleFilename(TEXT("CoreUObject")));
            if (Module == nullptr) return;
            ON_SCOPE_EXIT { FPlatformProcess::FreeDllHandle(Module); };
#if PLATFORM_WINDOWS
            ArchiveFunction = FPlatformProcess::GetDllExport(Module, TEXT("?Serialize@UObject@@UEAAXAEAVFArchive@@@Z"));
            StructuredFunction = FPlatformProcess::GetDllExport(Module, TEXT("?Serialize@UObject@@UEAAXVFStructuredArchiveRecord@@@Z"));
#else
            ArchiveFunction = FPlatformProcess::GetDllExport(Module, TEXT("_ZN7UObject9SerializeER8FArchive"));
            StructuredFunction = FPlatformProcess::GetDllExport(Module, TEXT("_ZN7UObject9SerializeE24FStructuredArchiveRecord"));
#endif
            if (ArchiveFunction == nullptr || StructuredFunction == nullptr) return;
            const UObject* Base = UObject::StaticClass()->GetDefaultObject(false);
            if (Base == nullptr) return;
            const void* const* VTable = *reinterpret_cast<const void* const* const*>(Base);
            // Both supported editor ABIs place these methods within UObject's first 64 virtual slots.
            for (int32 Index = 0; Index < 64; ++Index)
            {
                if (ResolveSerializerImport(VTable[Index]) == ArchiveFunction) Archive = Index;
                if (ResolveSerializerImport(VTable[Index]) == StructuredFunction) Structured = Index;
            }
        }
    };
    static const FSerializerSlots Slots;
    if (Slots.Archive == INDEX_NONE || Slots.Structured == INDEX_NONE) return false;
    const void* const* VTable = *reinterpret_cast<const void* const* const*>(Object);
    // UEnum adds names, values and version bytes; UField::Next is not serialized at the current version.
    // Exact class identity excludes UUserDefinedEnum and any serializer added by a derived class.
    if (Object->GetClass() == UEnum::StaticClass()) return true;
    return ResolveSerializerImport(VTable[Slots.Archive]) == Slots.ArchiveFunction
        && ResolveSerializerImport(VTable[Slots.Structured]) == Slots.StructuredFunction;
#else
    return false;
#endif
}
