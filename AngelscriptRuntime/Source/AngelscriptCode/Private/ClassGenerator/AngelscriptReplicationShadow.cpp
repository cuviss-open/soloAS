// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptReplicationShadow.h"
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "Engine/PackageMapClient.h"
#include "ClassGenerator/ASClass.h"
#include "Components/ActorComponent.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Net/RepLayout.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#if UE_WITH_IRIS
#include "Iris/ReplicationState/ReplicationStateDescriptorBuilder.h"
#include "Iris/ReplicationState/ReplicationStateDescriptorRegistry.h"
#include "Iris/ReplicationSystem/ObjectReplicationBridge.h"
#include "Iris/ReplicationSystem/ReplicationSystem.h"
#endif

namespace
{
bool bContractFailure = false;
bool Fail(const TCHAR* Reason, const UClass* Class)
{
    bContractFailure = true;
    UE_LOG(Angelscript, Error, TEXT("ASR-REPLICATION-SHADOW-CONTRACT: %s class=%s"), Reason, *GetPathNameSafe(Class));
    return false;
}

// Standard explicit instantiation: the compiler computes member pointers, without binary patches or offsets.
template<class Tag, typename Tag::Type Member>
struct TMemberAccess { friend typename Tag::Type Resolve(Tag) { return Member; } };
struct FLayoutOwnerTag
{
    using Type = UStruct* FRepLayout::*;
    friend Type Resolve(FLayoutOwnerTag);
};
template struct TMemberAccess<FLayoutOwnerTag, &FRepLayout::Owner>;

#if UE_WITH_IRIS
using FRegistry = UE::Net::Private::FReplicationStateDescriptorRegistry;
using FDescriptors = UE::Net::FReplicationStateDescriptorBuilder::FResult;
struct FRegistryTag
{
    using Type = FRegistry* (UReplicationBridge::*)() const;
    friend Type Resolve(FRegistryTag);
};
template struct TMemberAccess<FRegistryTag, &UReplicationBridge::GetReplicationStateDescriptorRegistry>;
struct FDescriptorsTag
{
    using Type = bool (*)(FRegistry&, UASClass*, const UObject*, FDescriptors&, bool);
    friend Type Resolve(FDescriptorsTag);
    template<auto Member>
    static bool Invoke(FRegistry& Registry, UASClass* Source, const UObject* Defaults,
        FDescriptors& Descriptors, bool bInsert)
    {
        auto& Map = Registry.*Member;
        const FFieldVariant Key(const_cast<UObject*>(Defaults));
        if (bInsert)
        {
            if (Map.Contains(Key)) return false;
            auto& Entry = Map.FindOrAdd(Key);
            Entry.WeakPtrForPruning = Source;
            Entry.OwnerKey = FObjectKey(Defaults);
            Entry.Descriptors = Descriptors;
            return true;
        }
        const auto* Entry = Map.Find(Key);
        if (!Entry || Entry->WeakPtrForPruning.Get() != Source
            || Entry->OwnerKey != FObjectKey(Defaults) || !Entry->OwnerKey.ResolveObjectPtr()) return false;
        Descriptors = Entry->Descriptors;
        return true;
    }
};
template<class Tag, auto Member>
struct TBoundOperation { friend typename Tag::Type Resolve(Tag) { return &Tag::template Invoke<Member>; } };
template struct TBoundOperation<FDescriptorsTag, &FRegistry::RegisteredDescriptorsMap>;
bool SameDescriptors(const FDescriptors& Left, const FDescriptors& Right)
{
    if (Left.Num() != Right.Num()) return false;
    for (int32 Index = 0; Index < Left.Num(); ++Index)
        if (Left[Index].GetReference() != Right[Index].GetReference()) return false;
    return true;
}
#endif

bool IsCurrentNetworkClass(UASClass* Class)
{
    return IsValid(Class) && Class->ScriptTypePtr != nullptr && !Class->HasAnyFlags(RF_ClassDefaultObject)
        && !Class->HasAnyClassFlags(CLASS_NewerVersionExists)
        && Class->HasValidScriptClassCarrier() && Class->GetDefaultObject(false)
        && (Class->IsChildOf(AActor::StaticClass()) || Class->IsChildOf(UActorComponent::StaticClass()));
}

class FShadowClasses final : public FGCObject
{
public:
    TMap<TWeakObjectPtr<UASClass>, TObjectPtr<USoloASReplicationLayoutClass>> Classes;
    TMap<TWeakObjectPtr<UNetDriver>, TMap<TWeakObjectPtr<UASClass>, TWeakPtr<FRepLayout>>> LegacyLayouts;
    FDelegateHandle WorldTick, PostReload;
#if UE_WITH_IRIS
    struct FIrisEntry { TWeakObjectPtr<UASClass> Source; FDescriptors Descriptors; };
    TMap<TWeakObjectPtr<UReplicationSystem>, TMap<FObjectKey, FIrisEntry>> IrisDescriptors;
    FDelegateHandle SystemCreated;
#endif
    virtual FString GetReferencerName() const override { return TEXT("AngelscriptReplicationShadow"); }
    virtual void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        for (auto& Pair : Classes)
            if (IsCurrentNetworkClass(Pair.Key.Get()))
                Collector.AddReferencedObject(Pair.Value);
    }

    USoloASReplicationLayoutClass* Get(UASClass* Source)
    {
        if (!IsCurrentNetworkClass(Source)) return nullptr;
        if (auto* Existing = Classes.Find(Source)) return Existing->Get();
        Source->SetUpRuntimeReplicationData();
        // UClass metaclasses require a Package outer. Reuse a canonical name across reload generations.
        const FName Name(*(TEXT("SoloASReplication_") + Source->GetName()));
        if (auto* Retired = FindObject<USoloASReplicationLayoutClass>(GetTransientPackage(), *Name.ToString()))
        {
            if (Retired->SourceClass == Source) return Retired;
            if (Retired->SourceClass && !Retired->SourceClass->HasAnyClassFlags(CLASS_NewerVersionExists))
            {
                Fail(TEXT("shadow name belongs to a live different class"), Source);
                return nullptr;
            }
            const FName RetiredName = MakeUniqueObjectName(GetTransientPackage(), Retired->GetClass(),
                FName(*(Name.ToString() + TEXT("_Retired"))));
            if (!Retired->Rename(*RetiredName.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty))
            {
                Fail(TEXT("shadow retirement rename failed"), Source);
                return nullptr;
            }
        }
        auto* Shadow = NewObject<USoloASReplicationLayoutClass>(GetTransientPackage(), Name, RF_Transient);
        Shadow->SourceClass = Source;
        Shadow->ClassFlags = (Source->ClassFlags & ~CLASS_ReplicationDataIsSetUp)
            | CLASS_Hidden | CLASS_NotPlaceable | CLASS_Transient;
        Shadow->ClassCastFlags = Source->ClassCastFlags;
        Shadow->ClassWithin = Source->ClassWithin;
        Shadow->ClassConfigName = Source->ClassConfigName;
        Shadow->ClassConstructor = Source->ClassConstructor;
        Shadow->ClassVTableHelperCtorCaller = Source->ClassVTableHelperCtorCaller;
        Shadow->CppClassStaticFunctions = Source->CppClassStaticFunctions;
        Shadow->bCooked = true;
        // This implementation class is deliberately absent from the class-child hash.
        // Serialize and BeginDestroy below preserve the matching unhashed-super invariant.
        Shadow->UStruct::SetSuperStruct(Source);
        Shadow->Bind();
        Shadow->StaticLink(true);
        Shadow->SetUpRuntimeReplicationData();
        if (Shadow->GetPropertiesSize() != Source->GetPropertiesSize()
            || Shadow->GetMinAlignment() != Source->GetMinAlignment()
            || Shadow->ClassReps.Num() != Source->ClassReps.Num())
        {
            Fail(*FString::Printf(TEXT("shadow layout differs: size=%d/%d alignment=%d/%d replication=%d/%d"),
                Shadow->GetPropertiesSize(), Source->GetPropertiesSize(), Shadow->GetMinAlignment(), Source->GetMinAlignment(),
                Shadow->ClassReps.Num(), Source->ClassReps.Num()), Source);
            Shadow->ConditionalBeginDestroy();
            return nullptr;
        }
        for (int32 Index = 0; Index < Source->ClassReps.Num(); ++Index)
            if (Source->ClassReps[Index].Property != Shadow->ClassReps[Index].Property)
            {
                Fail(TEXT("shadow replication index differs"), Source);
                Shadow->ConditionalBeginDestroy();
                return nullptr;
            }
        TArray<UClass*> Derived;
        GetDerivedClasses(Source, Derived, false);
        if (Derived.Contains(Shadow))
        {
            Fail(TEXT("shadow leaked into derived classes"), Source);
            Shadow->ConditionalBeginDestroy();
            return nullptr;
        }
        Classes.Add(Source, Shadow);
        UObject* ShadowDefault = Shadow->GetDefaultObject();
        UObject* SourceDefault = Source->GetDefaultObject();
        for (TFieldIterator<FProperty> It(Source); It; ++It)
            It->CopyCompleteValue_InContainer(ShadowDefault, SourceDefault);
        return Shadow;
    }

    bool InstallLegacy(UNetDriver* Driver, UASClass* Source)
    {
        auto& Installed = LegacyLayouts.FindOrAdd(Driver);
        if (auto* Existing = Driver->RepLayoutMap.Find(Source))
        {
            const auto* Recorded = Installed.Find(Source);
            if (!Recorded || Recorded->Pin() != *Existing || !Existing->IsValid() || (*Existing)->GetOwner() != Source)
                return Fail(TEXT("legacy layout was created before injection or changed ownership"), Source);
            return true;
        }
        auto* Shadow = Get(Source);
        if (!Shadow) return false;
        auto Layout = FRepLayout::CreateFromClass(Shadow, Driver->ServerConnection,
            Driver->MaySendProperties() ? ECreateRepLayoutFlags::MaySendProperties : ECreateRepLayoutFlags::None);
        if (!Layout || Layout->GetOwner() != Shadow) return Fail(TEXT("unexpected legacy layout owner"), Source);
        Layout.Get()->*Resolve(FLayoutOwnerTag{}) = Source;
        if (Layout->GetOwner() != Source) return Fail(TEXT("legacy owner retarget failed"), Source);
        Installed.Add(Source, Layout);
        Driver->RepLayoutMap.Add(Source, MoveTemp(Layout));
        return true;
    }

#if UE_WITH_IRIS
    bool InstallIris(UReplicationSystem* System, UASClass* Source, const UObject* Defaults)
    {
        auto* Bridge = System ? System->GetReplicationBridge() : nullptr;
        if (!Bridge || !Defaults) return false;
        auto* Registry = (Bridge->*Resolve(FRegistryTag{}))();
        if (!Registry) return Fail(TEXT("Iris registry missing"), Source);
        auto& Installed = IrisDescriptors.FindOrAdd(System);
        if (const auto* Recorded = Installed.Find(FObjectKey(Defaults)))
        {
            FDescriptors Actual;
            if (Recorded->Source != Source || !Resolve(FDescriptorsTag{})(*Registry, Source, Defaults, Actual, false)
                || !SameDescriptors(Recorded->Descriptors, Actual))
                return Fail(TEXT("Iris injected descriptor ownership changed"), Source);
            return true;
        }
        auto* Shadow = Get(Source);
        if (!Shadow) return false;
        UE::Net::FReplicationStateDescriptorBuilder::FParameters Parameters;
        Parameters.DescriptorRegistry = nullptr;
        Parameters.ReplicationSystem = System;
        Parameters.DefaultStateSource = Defaults;
        FDescriptors Descriptors;
        UE::Net::FReplicationStateDescriptorBuilder::CreateDescriptorsForClass(Descriptors, Shadow, Parameters);
        if (!Resolve(FDescriptorsTag{})(*Registry, Source, Defaults, Descriptors, true))
            return Fail(TEXT("Iris descriptor registration happened before injection"), Source);
        // Exercise the exported builder's real registry lookup, including pruning and OwnerKey validation.
        Parameters.DescriptorRegistry = Registry;
        FDescriptors Checked;
        UE::Net::FReplicationStateDescriptorBuilder::CreateDescriptorsForClass(Checked, Source, Parameters);
        if (!SameDescriptors(Checked, Descriptors)) return Fail(TEXT("Iris registry semantics changed"), Source);
        FIrisEntry Entry;
        Entry.Source = Source;
        Entry.Descriptors = MoveTemp(Descriptors);
        Installed.Add(FObjectKey(Defaults), MoveTemp(Entry));
        return true;
    }
    void InstallSystem(UReplicationSystem* System)
    {
        for (TObjectIterator<UASClass> It; It; ++It)
            if (IsCurrentNetworkClass(*It)) InstallIris(System, *It, It->GetDefaultObject(false));
    }
#endif
    void RepairRenamedClassGuids()
    {
        for (TObjectIterator<UNetDriver> Driver; Driver; ++Driver)
        {
            const TSharedPtr<FNetGUIDCache>& Cache = Driver->GetNetGuidCache();
            if (!Cache) continue;
            for (auto& Pair : Cache->ObjectLookup)
            {
                const UASClass* Class = Cast<UASClass>(Pair.Value.Object.Get());
                if (Class && Class->HasAnyClassFlags(CLASS_NewerVersionExists)
                    && Pair.Value.PathName != Class->GetFName())
                {
                    // FullReload renames the class we own; preserve the cached object identity.
                    Pair.Value.PathName = Class->GetFName();
                    Pair.Value.bDirtyForReplay = true;
                }
            }
        }
    }
    void Update()
    {
        for (auto It = Classes.CreateIterator(); It; ++It)
            if (!IsCurrentNetworkClass(It.Key().Get())) It.RemoveCurrent();
        for (auto It = LegacyLayouts.CreateIterator(); It; ++It)
            if (!It.Key().IsValid()) It.RemoveCurrent();
        for (TObjectIterator<UNetDriver> Driver; Driver; ++Driver)
        {
            if (Driver->HasAnyFlags(RF_ClassDefaultObject) || !Driver->GetWorld()) continue;
            for (TObjectIterator<UASClass> Class; Class; ++Class)
                if (IsCurrentNetworkClass(*Class)) InstallLegacy(*Driver, *Class);
        }
#if UE_WITH_IRIS
        for (auto It = IrisDescriptors.CreateIterator(); It; ++It)
            if (!It.Key().IsValid()) It.RemoveCurrent();
        for (UReplicationSystem* System : UE::Net::FReplicationSystemFactory::GetAllReplicationSystems())
            if (System) InstallSystem(System);
#endif
    }
};
TUniquePtr<FShadowClasses> Shadows;
}

void USoloASReplicationLayoutClass::Bind() { UClass::Bind(); }
EStructPropertyLinkFlags USoloASReplicationLayoutClass::GetPropertyLinkFlags(UStruct* Container, FProperty* Property) const
{
    const UASClass* Source = SourceClass.Get();
    return Source ? Source->GetPropertyLinkFlags(Container, Property) : UClass::GetPropertyLinkFlags(Container, Property);
}
bool USoloASReplicationLayoutClass::IsFunctionImplementedInScript(FName Name) const
{
    const UASClass* Source = SourceClass.Get();
    return Source && Source->IsFunctionImplementedInScript(Name);
}
UObject* USoloASReplicationLayoutClass::CreateDefaultObject() { return UClass::CreateDefaultObject(); }
void USoloASReplicationLayoutClass::InitPropertiesFromCustomList(uint8* Data, const uint8* Defaults)
{ UClass::InitPropertiesFromCustomList(Data, Defaults); }
void USoloASReplicationLayoutClass::GetLifetimeBlueprintReplicationList(TArray<FLifetimeProperty>& Out) const
{
    if (!UASClass::TryGetLifetimeScriptReplicationList(SourceClass.Get(), Out)) Fail(TEXT("shadow lifetime list invalid"), SourceClass.Get());
}
void USoloASReplicationLayoutClass::DestroyPersistentUberGraphFrame(UObject* Object, bool bSkipSuperClass) const
{
    if (UASClass* Source = SourceClass.Get()) Source->DestroyPersistentUberGraphFrame(Object, bSkipSuperClass);
}
void USoloASReplicationLayoutClass::Serialize(FArchive& Ar)
{
    UClass* Parent = GetSuperClass();
    const bool bRehash = Ar.IsLoading() || (Ar.IsModifyingWeakAndStrongReferences() && !Ar.IsSaving());
    if (bRehash) UStruct::SetSuperStruct(nullptr);
    Super::Serialize(Ar);
    if (bRehash) UStruct::SetSuperStruct(Parent);
}
bool USoloASReplicationLayoutClass::Rename(const TCHAR* Name, UObject* Outer, ERenameFlags Flags)
{
    UClass* Parent = GetSuperClass();
    UStruct::SetSuperStruct(nullptr);
    const bool Result = Super::Rename(Name, Outer, Flags);
    UStruct::SetSuperStruct(Parent);
    return Result;
}
void USoloASReplicationLayoutClass::BeginDestroy()
{
    UClass* Parent = GetSuperClass();
    UStruct::SetSuperStruct(nullptr);
    Super::BeginDestroy();
    // UObject unhashed this class once. Preserve ancestry until its CDO/receivers finish destruction.
    UStruct::SetSuperStruct(Parent);
}

namespace AngelscriptReplicationShadow
{
void Startup()
{
    Shadows = MakeUnique<FShadowClasses>();
    Shadows->PostReload = FAngelscriptClassGenerator::OnPostReload.AddLambda([](bool) { Shadows->RepairRenamedClassGuids(); });
    Shadows->WorldTick = FWorldDelegates::OnWorldTickStart.AddLambda([](UWorld*, ELevelTick, float) { Shadows->Update(); });
#if UE_WITH_IRIS
    Shadows->SystemCreated = UE::Net::FReplicationSystemFactory::GetReplicationSystemCreatedDelegate().AddLambda(
        [](UReplicationSystem* System) { Shadows->InstallSystem(System); });
#endif
}
void Shutdown()
{
    if (!Shadows) return;
    FWorldDelegates::OnWorldTickStart.Remove(Shadows->WorldTick);
    FAngelscriptClassGenerator::OnPostReload.Remove(Shadows->PostReload);
#if UE_WITH_IRIS
    UE::Net::FReplicationSystemFactory::GetReplicationSystemCreatedDelegate().Remove(Shadows->SystemCreated);
#endif
    Shadows.Reset();
}
void PrepareObject(UObject* Object)
{
    if (!Shadows || !Object || Object->IsTemplate()
        || (!Object->IsA<AActor>() && !Object->IsA<UActorComponent>())) return;
    UASClass* Source = UASClass::GetFirstASClass(Object);
    if (!Source || Object->GetClass() != Source) return;
    Shadows->Update();
#if UE_WITH_IRIS
    for (UReplicationSystem* System : UE::Net::FReplicationSystemFactory::GetAllReplicationSystems())
        if (System) Shadows->InstallIris(System, Source, Object->GetArchetype());
#endif
}
bool HasContractFailure() { return bContractFailure; }
UClass* GetLayoutClass(UASClass* Class) { return Shadows ? Shadows->Get(Class) : nullptr; }
}
