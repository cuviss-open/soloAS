// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "Lambda/SoloASBridgeLambdaClosure.h"
#include "SoloASLambdaClosureObject.h"
#include "AngelscriptManager.h"
#include "AngelscriptCoreExtensionPoints.h"
#include "ClassGenerator/ASClass.h"
#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "EndAngelscriptHeaders.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/SparseDelegate.h"

namespace SoloASBridge::Private
{
void ShutdownLambdaClosures();
namespace
{
struct FStorage
{
    FObjectKey Object;
    FName Property;
    bool bKnown = false;
};
struct FBindingKey
{
    FObjectKey Target;
    FName Entry;
    bool operator==(const FBindingKey& Other) const { return Target == Other.Target && Entry == Other.Entry; }
    friend uint32 GetTypeHash(const FBindingKey& Key) { return HashCombine(GetTypeHash(Key.Target), GetTypeHash(Key.Entry)); }
};
struct FRegisteredTimer
{
    TWeakObjectPtr<UWorld> World;
    FTimerHandle Handle;
};
struct FSlot
{
    FObjectKey Owner;
    FBindingKey Key;
    FName Site;
    FName Store;
    FName Body;
    FString ModuleName;
    uint64 ModuleGeneration = 0;
    uint64 Lease = 0;
    bool bDirect = false;
    TMap<uint64, int32> ItemIndices;
    AngelscriptCoreExtensionPoints::FModuleGenerationLease CaptureLease;
    FSoloASLambdaCaptureList Direct;
    FStorage Storage;
    bool bPending = true;
    bool bTimerPending = false;
    bool bNativeRetained = false;
    bool bUntracked = false;
    bool bRetired = false;
    bool bRetirePending = false;
    TArray<FRegisteredTimer> Timers;
};
struct FIdle
{
    USoloASLambdaClosureObject* Object;
    uint64 SinceGC;
};
struct FLegacy
{
    FObjectKey Owner;
    UObject* Object;
};
TArray<FSlot> Slots;
TMap<FBindingKey, int32> SlotIndices;
TMap<uint64, int32> LeaseIndices;
TArray<FLambdaClosureInvocation*> Invocations;
TArray<USoloASLambdaClosureObject*> ActiveObjects;
TArray<FIdle> IdleObjects;
TArray<FLegacy> Legacy;
TMap<FName, TWeakObjectPtr<UFunction>> Prototypes;
TMap<FObjectKey, TArray<FName>> DelegateRoots;
TMap<UPTRINT, FStorage> StorageIndex;
uint64 ObjectScans = 0, ObjectsVisited = 0;
double ObjectScanSeconds = 0;
TMap<FObjectKey, TArray<TUniquePtr<FProperty>>> ScriptFields;
TMap<FObjectKey, TMap<FName, int32>> ScriptOffsets;
class FScriptFieldReferences final : public FGCObject
{
public:
    void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        for (auto& Row : ScriptFields)
            if (UObject* Class = Row.Key.ResolveObjectPtr(); IsValid(Class) && !CastChecked<UClass>(Class)->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated))
                for (auto& Field : Row.Value) Field->AddReferencedObjects(Collector);
    }
    FString GetReferencerName() const override { return TEXT("Lambda script storage schemas"); }
};
TUniquePtr<FScriptFieldReferences> FieldReferences;
FProperty* FindProperty(UClass* Class, FName Name)
{
    if (auto* Property = FindFProperty<FProperty>(Class, Name)) return Property;
    for (UClass* Current = Class; Current; Current = Current->GetSuperClass())
        if (const auto* Fields = ScriptFields.Find(FObjectKey(Current)))
            for (const auto& Field : *Fields) if (Field->GetFName() == Name) return Field.Get();
    return nullptr;
}
void* FieldAddress(UObject* Object, FProperty* Property, int32 Index = 0)
{
    for (UClass* Current = Object->GetClass(); Current; Current = Current->GetSuperClass())
        if (const auto* Fields = ScriptFields.Find(FObjectKey(Current)))
            for (const auto& Field : *Fields) if (Field.Get() == Property)
                if (const auto* Offsets = ScriptOffsets.Find(FObjectKey(Current)))
                    if (const int32* Offset = Offsets->Find(Property->GetFName()))
                        return reinterpret_cast<uint8*>(Object) + *Offset + Index * Property->ElementSize;
    return Property->ContainerPtrToValuePtr<void>(static_cast<void*>(Object), Index);
}
void AllProperties(UClass* Class, TFunctionRef<void(FProperty*)> Callback)
{
    TSet<FName> Seen;
    for (TFieldIterator<FProperty> It(Class); It; ++It) { Seen.Add(It->GetFName()); Callback(*It); }
    for (UClass* Current = Class; Current; Current = Current->GetSuperClass())
        if (const auto* Fields = ScriptFields.Find(FObjectKey(Current)))
            for (const auto& Field : *Fields) if (!Seen.Contains(Field->GetFName())) { Seen.Add(Field->GetFName()); Callback(Field.Get()); }
}
FDelegateHandle PreGC;
FDelegateHandle CaptureClient;
bool bInitialized = false;
uint64 GCCycle = 0;
uint64 NextLease = 0, NextItem = 0;
int32 Created = 0, Reused = 0, Peak = 0;
TArray<int32> RecentPeaks;
TAutoConsoleVariable<int32> MaxIdle(TEXT("soloas.LambdaPool.MaxIdle"), 128, TEXT("Maximum idle lambda target objects retained."));
TAutoConsoleVariable<int32> IdleGCs(TEXT("soloas.LambdaPool.IdleGCs"), 3, TEXT("Minimum idle GC cycles before lambda pool shrink."));
TAutoConsoleVariable<int32> PeakGCs(TEXT("soloas.LambdaPool.PeakGCs"), 4, TEXT("Recent GC window for lambda target high water."));

bool GameThread() { return ensureAlwaysMsgf(IsInGameThread(), TEXT("Lambda operations require game thread")); }

bool HasDelegates(FProperty* Property, TSet<const UStruct*>& Visiting)
{
    if (Property->IsA<FDelegateProperty>() || Property->IsA<FMulticastDelegateProperty>()) return true;
    if (auto* Struct = CastField<FStructProperty>(Property))
    {
        if (Struct->Struct == FSoloASLambdaCaptureStore::StaticStruct()) return false;
        if (Struct->Struct == FInstancedStruct::StaticStruct()) return true;
        if (Visiting.Contains(Struct->Struct)) return false;
        Visiting.Add(Struct->Struct);
        bool Found = false;
        for (TFieldIterator<FProperty> It(Struct->Struct); It && !Found; ++It) Found = HasDelegates(*It, Visiting);
        Visiting.Remove(Struct->Struct);
        return Found;
    }
    if (auto* Array = CastField<FArrayProperty>(Property)) return HasDelegates(Array->Inner, Visiting);
    if (auto* Map = CastField<FMapProperty>(Property)) return HasDelegates(Map->KeyProp, Visiting) || HasDelegates(Map->ValueProp, Visiting);
    if (auto* Set = CastField<FSetProperty>(Property)) return HasDelegates(Set->ElementProp, Visiting);
    return false;
}
using FVisit = TFunctionRef<void(FProperty*, void*, const FScriptDelegate*, const FMulticastScriptDelegate*)>;
void Visit(FProperty* Property, void* Data, FVisit Callback)
{
    if (auto* Single = CastField<FDelegateProperty>(Property)) Callback(Single, Data, static_cast<FScriptDelegate*>(Data), nullptr);
    else if (auto* Multi = CastField<FMulticastDelegateProperty>(Property)) Callback(Multi, Data, nullptr, Multi->GetMulticastDelegate(Data));
    else if (auto* Struct = CastField<FStructProperty>(Property))
    {
        if (Struct->Struct == FSoloASLambdaCaptureStore::StaticStruct()) return;
        if (Struct->Struct == FInstancedStruct::StaticStruct())
        {
            auto* Value = static_cast<FInstancedStruct*>(Data);
            if (Value->IsValid())
                for (TFieldIterator<FProperty> It(Value->GetScriptStruct()); It; ++It)
                    for (int32 Index = 0; Index < It->ArrayDim; ++Index)
                        Visit(*It, It->ContainerPtrToValuePtr<void>(Value->GetMutableMemory(), Index), Callback);
        }
        else for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
            for (int32 Index = 0; Index < It->ArrayDim; ++Index) Visit(*It, It->ContainerPtrToValuePtr<void>(Data, Index), Callback);
    }
    else if (auto* Array = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Values(Array, Data);
        for (int32 Index = 0; Index < Values.Num(); ++Index) Visit(Array->Inner, Values.GetRawPtr(Index), Callback);
    }
    else if (auto* Map = CastField<FMapProperty>(Property))
    {
        FScriptMapHelper Values(Map, Data);
        for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index) if (Values.IsValidIndex(Index))
        { Visit(Map->KeyProp, Values.GetKeyPtr(Index), Callback); Visit(Map->ValueProp, Values.GetValuePtr(Index), Callback); }
    }
    else if (auto* Set = CastField<FSetProperty>(Property))
    {
        FScriptSetHelper Values(Set, Data);
        for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index) if (Values.IsValidIndex(Index)) Visit(Set->ElementProp, Values.GetElementPtr(Index), Callback);
    }
}
using FObjectVisit = TFunctionRef<void(UObject*, FProperty*, FProperty*, void*, const FScriptDelegate*, const FMulticastScriptDelegate*)>;
void VisitObjects(FObjectVisit Callback)
{
    ++ObjectScans;
    const double Started = FPlatformTime::Seconds();
    ON_SCOPE_EXIT { ObjectScanSeconds += FPlatformTime::Seconds() - Started; };
    for (TObjectIterator<UObject> It(RF_NoFlags); It; ++It)
    {
        ++ObjectsVisited;
        UObject* Object = *It;
        if (!IsValid(Object) || Object->IsUnreachable() || Object->IsA<USoloASLambdaClosureObject>()) continue;
        auto& Roots = DelegateRoots.FindOrAdd(FObjectKey(Object->GetClass()));
        if (Roots.IsEmpty())
        {
            TSet<const UStruct*> Visiting;
            AllProperties(Object->GetClass(), [&](FProperty* Property)
            { if (HasDelegates(Property, Visiting)) Roots.Add(Property->GetFName()); });
            // An empty schema is cached as a single sentinel.
            if (Roots.IsEmpty()) Roots.Add(NAME_None);
        }
        for (FName Name : Roots)
            if (!Name.IsNone()) if (FProperty* Root = FindProperty(Object->GetClass(), Name))
                for (int32 Index = 0; Index < Root->ArrayDim; ++Index)
                    Visit(Root, FieldAddress(Object, Root, Index), [&](FProperty* Property, void* Data, const FScriptDelegate* Single, const FMulticastScriptDelegate* Multi)
                    { Callback(Object, Root, Property, Data, Single, Multi); });
    }
}
const FMulticastScriptDelegate* ReadMulticast(const void* Address, USparseDelegateFunction* Signature)
{
    if (!Signature) return static_cast<const FMulticastScriptDelegate*>(Address);
    const auto& Value = *static_cast<const FSparseDelegate*>(Address);
    UObject* Owner = FSparseDelegateStorage::ResolveSparseOwner(Value, Signature->OwningClassName, Signature->DelegateName);
    return FSparseDelegateStorage::GetMulticastDelegate(Owner, Signature->DelegateName);
}
void RemoveMulticast(void* Address, USparseDelegateFunction* Signature, UObject* Target, FName Entry)
{
    FScriptDelegate Item; Item.BindUFunction(Target, Entry);
    if (!Signature) static_cast<FMulticastScriptDelegate*>(Address)->Remove(Item);
    else
    {
        auto* Value = static_cast<FSparseDelegate*>(Address);
        UObject* Owner = FSparseDelegateStorage::ResolveSparseOwner(*Value, Signature->OwningClassName, Signature->DelegateName);
        Value->__Internal_Remove(Owner, Signature->DelegateName, Item);
    }
}
FStorage FindStorage(UObject* Owner, const void* Address, UObject* Holder = nullptr, FName RootProperty = NAME_None)
{
    FStorage Result;
    auto Check = [&](UObject* Object, FProperty* Root)
    {
        for (int32 Index = 0; Index < Root->ArrayDim && !Result.bKnown; ++Index)
            Visit(Root, FieldAddress(Object, Root, Index), [&](FProperty*, void* Data, const FScriptDelegate*, const FMulticastScriptDelegate*)
            { if (Data == Address) Result = {FObjectKey(Object), Root->GetFName(), true}; });
    };
    const UPTRINT AddressKey = reinterpret_cast<UPTRINT>(Address);
    // A source-level holder hint is borrowed only for this call. Validate its
    // root against the actual delegate address before retaining weak identities.
    if (IsValid(Holder) && !RootProperty.IsNone())
        if (FProperty* Root = FindProperty(Holder->GetClass(), RootProperty))
        {
            if (FieldAddress(Holder, Root) == Address
                && (Root->IsA<FDelegateProperty>() || Root->IsA<FMulticastDelegateProperty>()))
                Result = {FObjectKey(Holder), RootProperty, true};
            else Check(Holder, Root);
            if (Result.bKnown) { StorageIndex.Add(AddressKey, Result); return Result; }
        }

    if (const auto* Cached = StorageIndex.Find(AddressKey))
        if (UObject* CachedHolder = Cached->Object.ResolveObjectPtr(); IsValid(CachedHolder))
            if (FProperty* Root = FindProperty(CachedHolder->GetClass(), Cached->Property)) Check(CachedHolder, Root);
    if (Result.bKnown) return Result;
    StorageIndex.Remove(AddressKey);
    TSet<const UStruct*> Visiting;
    AllProperties(Owner->GetClass(), [&](FProperty* Property) { if (!Result.bKnown && HasDelegates(Property, Visiting)) Check(Owner, Property); });
    if (!Result.bKnown)
        VisitObjects([&](UObject* Object, FProperty* Root, FProperty*, void* Data, const FScriptDelegate*, const FMulticastScriptDelegate*)
        { if (Data == Address) Result = {FObjectKey(Object), Root->GetFName(), true}; });
    if (Result.bKnown) StorageIndex.Add(AddressKey, Result);
    return Result;
}
struct FSnapshot
{
    TMap<FBindingKey, int32> Counts;
};
FSnapshot Observe()
{
    FSnapshot Result;
    TMap<const UObject*, TSet<FName>> Targets;
    for (const auto& Slot : Slots) if (UObject* Target = Slot.Key.Target.ResolveObjectPtr()) Targets.FindOrAdd(Target).Add(Slot.Key.Entry);
    VisitObjects([&](UObject*, FProperty*, FProperty*, void*, const FScriptDelegate* Single, const FMulticastScriptDelegate* Multi)
    {
        auto Bound = [&](const UObject* Target, FName Entry)
        {
            FBindingKey Key{FObjectKey(Target), Entry};
            if (!SlotIndices.Contains(Key)) return;
            ++Result.Counts.FindOrAdd(Key);
        };
        if (Single) { if (const UObject* Target = Single->GetUObject()) Bound(Target, Single->GetFunctionName()); }
        else if (Multi)
        {
            TSet<UObject*> Seen;
            for (UObject* Target : Multi->GetAllObjects())
                if (!Seen.Contains(Target))
                {
                    Seen.Add(Target);
                    if (const auto* Entries = Targets.Find(Target))
                        for (FName Entry : *Entries) if (Multi->Contains(Target, Entry)) Bound(Target, Entry);
                }
        }
    });
    return Result;
}
struct FInvocationOverride
{
    UObject* Target;
    UObject* Owner;
    FName Entry;
    bool bConsumed = false;
};
TArray<FInvocationOverride> InvocationOverrides;
FSlot* FindSlot(UObject* Target, FName Entry)
{
    const int32* Index = SlotIndices.Find({FObjectKey(Target), Entry});
    return Index ? &Slots[*Index] : nullptr;
}
FSoloASLambdaCaptureStore* StoreFor(UObject* Owner, FName Name)
{
    auto* Property = Owner ? FindFProperty<FStructProperty>(Owner->GetClass(), Name) : nullptr;
    return Property && Property->Struct == FSoloASLambdaCaptureStore::StaticStruct()
        ? Property->ContainerPtrToValuePtr<FSoloASLambdaCaptureStore>(Owner) : nullptr;
}
FSoloASLambdaCaptureList* ValuesFor(const FSlot& Slot)
{
    if (auto* Target = Cast<USoloASLambdaClosureObject>(Slot.Key.Target.ResolveObjectPtr()))
    { auto* Row = Target->Slots.Find(Slot.Key.Entry); return Row ? &Row->Captures : nullptr; }
    if (Slot.bDirect) return &const_cast<FSlot&>(Slot).Direct;
    auto* Store = StoreFor(Slot.Owner.ResolveObjectPtr(), Slot.Store);
    return Store ? Store->Sites.Find(Slot.Key.Entry) : nullptr;
}
bool TimerAlive(const FSlot& Slot)
{
    for (const auto& Timer : Slot.Timers)
        if (Timer.World.IsValid() && Timer.World->GetTimerManager().TimerExists(Timer.Handle)) return true;
    return false;
}
bool Locatable(const FSlot& Slot)
{
    if (!Slot.Timers.IsEmpty()) return true;
    if (!Slot.Storage.bKnown) return false;
    UObject* Object = Slot.Storage.Object.ResolveObjectPtr();
    return !IsValid(Object) || FindProperty(Object->GetClass(), Slot.Storage.Property) != nullptr;
}
bool Bound(const FSlot& Slot)
{
    if (Slot.bRetired || !Slot.Storage.bKnown) return false;
    UObject* Holder = Slot.Storage.Object.ResolveObjectPtr();
    UObject* Target = Slot.Key.Target.ResolveObjectPtr();
    bool Found = false;
    if (IsValid(Holder) && IsValid(Target))
        if (FProperty* Property = FindProperty(Holder->GetClass(), Slot.Storage.Property))
            for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
                Visit(Property, FieldAddress(Holder, Property, Index), [&](FProperty*, void*, const FScriptDelegate* Single, const FMulticastScriptDelegate* Multi)
                { Found |= Single ? Single->GetUObject() == Target && Single->GetFunctionName() == Slot.Key.Entry : Multi && Multi->Contains(Target, Slot.Key.Entry); });
    return Found;
}
void ReleaseCaptureLease(AngelscriptCoreExtensionPoints::FModuleGenerationLease& Lease)
{
    if (!Lease.Id) return;
    if (ensure(AngelscriptCoreExtensionPoints::ReleaseModuleGenerationLease(CaptureClient, Lease)
        == AngelscriptCoreExtensionPoints::ERemoveResult::Removed)) Lease = {};
}
void ClearValues(FSlot& Slot)
{
    Slot.ItemIndices.Reset();
    if (auto* Values = ValuesFor(Slot)) { Values->Values.Reset(); Values->Items.Reset(); ++Values->Generation; }
    ReleaseCaptureLease(Slot.CaptureLease);
    if (auto* Target = Cast<USoloASLambdaClosureObject>(Slot.Key.Target.ResolveObjectPtr()))
        if (auto* Row = Target->Slots.Find(Slot.Key.Entry)) Row->Owner.Reset();
}
bool IsExecuting(const FSlot& Slot)
{
    for (const auto* Invocation : Invocations)
        if (Invocation->Lease == Slot.Lease) return true;
    return false;
}
void FinishRetirement(FSlot& Slot)
{
    Slot.bRetirePending = false;
    Slot.bRetired = true;
    ClearValues(Slot);
}
void FlushIdle()
{
    for (auto& Idle : IdleObjects) Idle.Object->RemoveFromRoot();
    IdleObjects.Reset(); RecentPeaks.Reset();
}
void ReturnEmptyObjects()
{
    TSet<FObjectKey> Owned, Dropped;
    for (const auto& Slot : Slots)
        if (IsValid(Slot.Owner.ResolveObjectPtr()) && (!Slot.bRetired || Slot.bNativeRetained || Slot.bUntracked)) Owned.Add(Slot.Key.Target);
    for (auto It = ActiveObjects.CreateIterator(); It; ++It)
    {
        auto* Object = *It;
        if (Object->ActiveInvocations) continue;
        if (Object->Slots.Num() && Owned.Contains(FObjectKey(Object))) continue;
        if (Object->Slots.Num())
        {
            // Retired identities can still exist in native delegates. Never pool them.
            Dropped.Add(FObjectKey(Object)); Object->Slots.Reset(); Object->bNeverPool = true;
        }
        if (!Object->bNeverPool && IdleObjects.Num() < FMath::Clamp(MaxIdle.GetValueOnGameThread(), 0, 4096)) IdleObjects.Add({Object, GCCycle});
        else Object->RemoveFromRoot();
        It.RemoveCurrentSwap();
    }
    Slots.RemoveAll([&](const FSlot& Slot) { return Dropped.Contains(Slot.Key.Target); });
}
void Sweep(const FSnapshot& Snapshot, bool bGC = false)
{
    for (auto It = Slots.CreateIterator(); It; ++It)
    {
        auto* Object = Cast<USoloASLambdaClosureObject>(It->Key.Target.ResolveObjectPtr());
        if (IsExecuting(*It) || (Object && Object->ActiveInvocations)) continue;
        UObject* Owner = It->Owner.ResolveObjectPtr();
        const bool Alive = IsValid(Owner) && !Owner->IsUnreachable();
        const bool StillBound = Snapshot.Counts.FindRef(It->Key) > 0 || TimerAlive(*It);
        if (bGC && It->bTimerPending) { It->bPending = false; It->bUntracked = true; }
        if (Alive && !It->bRetired && (StillBound || It->bPending || It->bNativeRetained || It->bUntracked)) continue;
        if (Alive && !It->bRetired && !Locatable(*It)) { It->bUntracked = true; if (Object) Object->bNeverPool = true; continue; }
        ClearValues(*It);
        if (Alive && It->bRetired && (It->bNativeRetained || It->bUntracked)) continue;
        if (Object && (StillBound || It->bNativeRetained || It->bUntracked))
        { It->bRetired = true; Object->bNeverPool |= It->bNativeRetained || It->bUntracked; continue; }
        if (Object) Object->Slots.Remove(It->Key.Entry);
        It.RemoveCurrentSwap();
    }
    for (auto It = Legacy.CreateIterator(); It; ++It)
        if (!IsValid(It->Owner.ResolveObjectPtr())) { It->Object->RemoveFromRoot(); It.RemoveCurrentSwap(); }
    ReturnEmptyObjects();
    SlotIndices.Reset(); LeaseIndices.Reset();
    for (int32 Index = 0; Index < Slots.Num(); ++Index)
    { SlotIndices.Add(Slots[Index].Key, Index); LeaseIndices.Add(Slots[Index].Lease, Index); }
}
void RemovePrototype(UClass* Class, FName Name)
{
    if (auto* Function = Class->FindFunctionByName(Name))
    {
        Class->RemoveFunctionFromFunctionMap(Function);
        Function->RemoveFromRoot();
        Function->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
    }
}
void PrunePrototypes()
{
    for (auto It = Prototypes.CreateIterator(); It; ++It)
    {
        UFunction* Source = It.Value().Get();
        if (Source && !Source->GetOuterUClass()->HasAnyClassFlags(CLASS_NewerVersionExists)) continue;
        bool Used = false;
        for (const auto& Slot : Slots)
            if (Slot.Key.Entry == It.Key() && Cast<USoloASLambdaClosureObject>(Slot.Key.Target.ResolveObjectPtr())) { Used = true; break; }
        if (Used) continue;
        RemovePrototype(USoloASLambdaClosureObject::StaticClass(), It.Key());
        It.RemoveCurrent();
    }
}
void BeforeGC()
{
    if (!GameThread()) return;
    ++GCCycle; StorageIndex.Reset();
    const auto Snapshot = Observe();
    for (auto* Invocation : Invocations)
        if (const auto* Slot = FindSlot(Invocation->Target.Get(), Invocation->Entry);
            Slot && !Slot->bRetirePending && !Slot->bPending && !Slot->bNativeRetained && !Slot->bUntracked
            && Snapshot.Counts.FindRef(Slot->Key) == 0 && !TimerAlive(*Slot)) Invocation->bCancelled = true;
    for (auto& Slot : Slots)
    {
        if (Slot.bRetirePending && IsExecuting(Slot)) continue;
        if (auto* Values = ValuesFor(Slot))
        {
            int32 Write = 0;
            for (int32 Read = 0; Read < Values->Items.Num(); ++Read)
                if (Values->Items[Read])
                {
                    if (Write != Read)
                    {
                        Values->Items[Write] = Values->Items[Read];
                        if (!Slot.bDirect) Values->Values[Write] = MoveTemp(Values->Values[Read]);
                    }
                    ++Write;
                }
            if (Write != Values->Items.Num())
            {
                Values->Items.SetNum(Write, EAllowShrinking::No);
                if (!Slot.bDirect) Values->Values.SetNum(Write, EAllowShrinking::No);
                ++Values->Generation;
            }
        }
        Slot.ItemIndices.Reset();
        if (auto* Values = ValuesFor(Slot))
            for (int32 Index = 0; Index < Values->Items.Num(); ++Index) Slot.ItemIndices.Add(Values->Items[Index], Index);
    }
    Sweep(Snapshot, true); PrunePrototypes();
    for (auto It = ScriptFields.CreateIterator(); It; ++It)
        if (UObject* Class = It.Key().ResolveObjectPtr(); !IsValid(Class) || CastChecked<UClass>(Class)->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated))
        { ScriptOffsets.Remove(It.Key()); It.RemoveCurrent(); DelegateRoots.Reset(); StorageIndex.Reset(); }
    RecentPeaks.Add(FMath::Max(Peak, ActiveObjects.Num()));
    const int32 Window = FMath::Clamp(PeakGCs.GetValueOnGameThread(), 1, 64);
    if (RecentPeaks.Num() > Window) RecentPeaks.RemoveAt(0, RecentPeaks.Num() - Window);
    Peak = ActiveObjects.Num();
    int32 Keep = 0; for (int32 Value : RecentPeaks) Keep = FMath::Max(Keep, Value);
    Keep = FMath::Min(Keep, FMath::Clamp(MaxIdle.GetValueOnGameThread(), 0, 4096));
    const uint64 Age = uint64(FMath::Clamp(IdleGCs.GetValueOnGameThread(), 0, 1024));
    for (auto It = IdleObjects.CreateIterator(); It && IdleObjects.Num() > Keep; ++It)
        if (GCCycle - It->SinceGC >= Age) { It->Object->RemoveFromRoot(); It.RemoveCurrent(); }
}
USoloASLambdaClosureObject* Acquire()
{
    USoloASLambdaClosureObject* Object;
    if (!IdleObjects.IsEmpty()) { Object = IdleObjects.Pop(EAllowShrinking::No).Object; ++Reused; }
    else { Object = NewObject<USoloASLambdaClosureObject>(GetTransientPackage(), NAME_None, RF_Transient); Object->AddToRoot(); ++Created; }
    Object->bNeverPool = false; ActiveObjects.Add(Object); Peak = FMath::Max(Peak, ActiveObjects.Num()); return Object;
}
void EnsurePrototype(UFunction* Source)
{
    UClass* Class = USoloASLambdaClosureObject::StaticClass();
    if (Prototypes.FindRef(Source->GetFName()).Get() == Source) return;
    RemovePrototype(Class, Source->GetFName());
    UFunction* Function = NewObject<UFunction>(Class, Source->GetFName(), RF_Transient);
    Function->AddToRoot();
    Function->FunctionFlags = FUNC_Public;
    TArray<FProperty*> Properties;
    for (TFieldIterator<FProperty> It(Source); It && It->HasAnyPropertyFlags(CPF_Parm); ++It) Properties.Add(*It);
    for (int32 Index = Properties.Num() - 1; Index >= 0; --Index) Function->AddCppProperty(CastFieldChecked<FProperty>(FField::Duplicate(Properties[Index], Function)));
    Function->StaticLink(true); Function->InitializeDerivedMembers();
    Class->AddFunctionToFunctionMap(Function, Source->GetFName()); Prototypes.Add(Source->GetFName(), Source);
}
class FDeleteListener final : public FUObjectArray::FUObjectDeleteListener
{
public:
    void NotifyUObjectDeleted(const UObjectBase*, int32) override {}
    void OnUObjectArrayShutdown() override { ShutdownLambdaClosures(); }
} Listener;
}
void DrainLambdaClosures()
{
    if (!bInitialized) return;
    check(IsInGameThread()); bInitialized = false;
    FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(PreGC);
    for (auto* Invocation : Invocations)
    {
        Invocation->bCancelled = true;
        Invocation->Captures.Reset(); Invocation->Items.Reset();
        ReleaseCaptureLease(Invocation->CaptureLease);
    }
    for (auto& Slot : Slots) ClearValues(Slot);
    Slots.Reset(); SlotIndices.Reset(); LeaseIndices.Reset();
    for (auto* Object : ActiveObjects) { Object->Slots.Reset(); Object->RemoveFromRoot(); }
    ActiveObjects.Reset(); FlushIdle();
    for (auto& Item : Legacy) Item.Object->RemoveFromRoot(); Legacy.Reset();
    UClass* Class = USoloASLambdaClosureObject::StaticClass();
    for (const auto& Prototype : Prototypes) RemovePrototype(Class, Prototype.Key);
    DelegateRoots.Reset(); StorageIndex.Reset(); Prototypes.Reset(); ScriptFields.Reset(); ScriptOffsets.Reset(); FieldReferences.Reset();
    GUObjectArray.RemoveUObjectDeleteListener(&Listener);
}
void InitializeLambdaClosures()
{
    check(IsInGameThread()); check(!bInitialized);
    if (CaptureClient.IsValid())
    {
        if (!ensure(AngelscriptCoreExtensionPoints::UnregisterModuleCacheClient(CaptureClient)
            == AngelscriptCoreExtensionPoints::ERemoveResult::Removed)) return;
        CaptureClient.Reset();
    }
    CaptureClient = AngelscriptCoreExtensionPoints::RegisterModuleGenerationLeaseClient(
        TEXT("soloas.ce.lambda-captures/v1"), &DrainLambdaClosures);
    if (!ensure(CaptureClient.IsValid())) return;
    bInitialized = true;
    FieldReferences = MakeUnique<FScriptFieldReferences>();
    PreGC = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddStatic(&BeforeGC);
    GUObjectArray.AddUObjectDeleteListener(&Listener);
}
void ShutdownLambdaClosures()
{
    check(IsInGameThread());
    if (!CaptureClient.IsValid()) return;
    // Close invokes the same drain before core execution shutdown. Busy keeps
    // the client and callback alive until every value has released its hold.
    if (AngelscriptCoreExtensionPoints::CloseModuleCacheClient(CaptureClient)
        == AngelscriptCoreExtensionPoints::ERemoveResult::Busy) return;
    if (AngelscriptCoreExtensionPoints::UnregisterModuleCacheClient(CaptureClient)
        == AngelscriptCoreExtensionPoints::ERemoveResult::Removed) CaptureClient.Reset();
}
}

namespace SoloASBridge
{
void ResetLambdaScriptFields()
{
    if (!Private::GameThread()) return;
    Private::ScriptFields.Reset(); Private::ScriptOffsets.Reset(); Private::DelegateRoots.Reset(); Private::StorageIndex.Reset();
}
void RegisterLambdaScriptFields(UClass* Class, TArray<TUniquePtr<FProperty>>&& Fields, TMap<FName, int32>&& Offsets)
{
    if (!Private::GameThread()) return;
    Private::ScriptFields.Add(FObjectKey(Class), MoveTemp(Fields));
    Private::ScriptOffsets.Add(FObjectKey(Class), MoveTemp(Offsets));
    Private::DelegateRoots.Reset(); Private::StorageIndex.Reset();
}
SIZE_T GetLambdaScriptFieldSize(UClass* Class)
{
    auto Size = [](auto&& Self, FProperty* Field) -> SIZE_T
    {
// SOLOAS-COMPAT-BEGIN(<5.8) [id: CS.Compatibility.CoreExtensions.SoloASBridgeLambdaClosure.PropertyAllocationSize]
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION < 8
        SIZE_T Result = FMemory::GetAllocSize(Field);
#else
        SIZE_T Result = Field->GetFieldSize();
#endif
// SOLOAS-COMPAT-END
        if (auto* Array = CastField<FArrayProperty>(Field)) Result += Self(Self, Array->Inner);
        if (auto* Map = CastField<FMapProperty>(Field)) Result += Self(Self, Map->KeyProp) + Self(Self, Map->ValueProp);
        if (auto* Set = CastField<FSetProperty>(Field)) Result += Self(Self, Set->ElementProp);
        return Result;
    };
    SIZE_T Bytes = 0;
    if (const auto* Offsets = Private::ScriptOffsets.Find(FObjectKey(Class))) Bytes += Offsets->GetAllocatedSize();
    if (const auto* Fields = Private::ScriptFields.Find(FObjectKey(Class)))
    { Bytes += Fields->GetAllocatedSize(); for (const auto& Field : *Fields) Bytes += Size(Size, Field.Get()); }
    return Bytes;
}
void* GetLambdaStorageAddress(UObject* Owner, FName Name)
{
    FProperty* Property = Private::FindProperty(Owner->GetClass(), Name);
    return Property ? Private::FieldAddress(Owner, Property) : nullptr;
}
FProperty* FindLambdaStorageProperty(UClass* Class, FName Name)
{
    return Private::FindProperty(Class, Name);
}
UObject* CreateLambdaClosure(UObject* Owner, UClass* ClosureClass)
{
    if (!Private::GameThread() || !Private::bInitialized || !IsValid(Owner) || !IsValid(ClosureClass)
        || ClosureClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Interface | CLASS_Deprecated | CLASS_NewerVersionExists)) return nullptr;
    UObject* Closure = NewObject<UObject>(GetTransientPackage(), ClosureClass, NAME_None, RF_Transient);
    Closure->AddToRoot(); Private::Legacy.Add({FObjectKey(Owner), Closure}); return Closure;
}
UObject* PrepareLambdaSlot(UObject* Owner, FName Site, FName Store, FName Body, const void* Delegate,
    bool bMulticast, bool bTimer, int32 SlotsPerSite, UScriptStruct* Type, const void* Capture, FName& OutEntry, FScriptLambdaHandle& OutHandle, USparseDelegateFunction* Sparse, UObject* Holder, FName RootProperty)
{
    if (!Private::GameThread() || !Private::bInitialized || !IsValid(Owner)) return nullptr;
    const auto Storage = Private::FindStorage(Owner, Delegate, Holder, RootProperty);
    const FObjectKey OwnerKey(Owner);
    Private::FSlot* Selected = nullptr;
    const int32 Capacity = FMath::Clamp(SlotsPerSite, 1, 32);
    // A live multicast group shares later appends, including reflected copies.
    // Detached identities are left intact until the pre-GC alias reconciliation.
    if (bMulticast)
        if (const auto* Multi = Private::ReadMulticast(Delegate, Sparse))
            for (UObject* Candidate : Multi->GetAllObjects())
            {
                for (int32 Index = 0; Index < Capacity; ++Index)
                {
                    const FName CandidateEntry(*FString::Printf(TEXT("%s_Slot_%d"), *Site.ToString(), Index));
                    auto* Slot = Private::FindSlot(Candidate, CandidateEntry);
                    if (Slot && Slot->Owner == OwnerKey && !Slot->bRetired && !Slot->bRetirePending && !Slot->bNativeRetained && !Slot->bUntracked
                        && Multi->Contains(Candidate, CandidateEntry))
                    { Selected = Slot; break; }
                }
                if (Selected) break;
            }
    auto FindEntry = [&](UObject* Target) -> FName
    {
        for (int32 Index = 0; Index < Capacity; ++Index)
        {
            const FName Entry(*FString::Printf(TEXT("%s_Slot_%d"), *Site.ToString(), Index));
            if (!Private::FindSlot(Target, Entry)) return Entry;
        }
        return NAME_None;
    };
    UObject* Target = Selected ? Selected->Key.Target.ResolveObjectPtr() : Owner;
    FName Entry = Selected ? Selected->Key.Entry : FindEntry(Owner);
    if (!Selected && Entry.IsNone())
    {
        Target = Private::ActiveObjects.IsEmpty() ? nullptr : Private::ActiveObjects.Last();
        auto* Object = Cast<USoloASLambdaClosureObject>(Target);
        if (!Object || Object->bNeverPool || Object->Slots.Num() >= Capacity || (Entry = FindEntry(Object)).IsNone())
        { Target = Private::Acquire(); Entry = FindEntry(Target); }
    }
    if (!Selected)
    {
        Private::FSlot Slot;
        Slot.bDirect = Type == nullptr;
        Slot.Lease = ++Private::NextLease;
        Slot.Owner = OwnerKey; Slot.Key = {FObjectKey(Target), Entry}; Slot.Site = Site; Slot.Store = Store; Slot.Body = Body;
        UFunction* Signature = Owner->FindFunction(Entry);
        if (!Signature) return nullptr;
        const UASClass* Definition = Cast<UASClass>(Signature->GetOuterUClass());
        if (!Definition || !Definition->ScriptTypePtr) return nullptr;
        const auto Module = FAngelscriptManager::Get().GetModule(Definition->ScriptTypePtr->GetModule());
        if (!Module.IsValid() || !Module->CoreGenerationId) return nullptr;
        Slot.ModuleName = Module->ModuleName; Slot.ModuleGeneration = Module->CoreGenerationId;
        Slot.Storage = Storage; Slot.bTimerPending = bTimer && !Slot.Storage.bKnown;
        Slot.bUntracked = !Slot.Storage.bKnown && !bTimer;
        if (Slot.bUntracked)
        {
            static TSet<FName> Warned;
            if (!Warned.Contains(Site)) { Warned.Add(Site); UE_LOG(LogTemp, Warning, TEXT("Lambda delegate storage is unreflected; captures retain owner lifetime and are never pooled. Register timers or native holders explicitly.")); }
        }
        if (auto* Object = Cast<USoloASLambdaClosureObject>(Target))
        {
            Private::EnsurePrototype(Signature);
            auto& Row = Object->Slots.FindOrAdd(Entry); Row.Owner = Owner; Row.Body = Body;
            Object->bNeverPool |= Slot.bUntracked;
        }
        else if (Type)
        {
            auto* Values = Private::StoreFor(Owner, Store);
            if (!Values) return nullptr;
            Values->Sites.FindOrAdd(Entry);
        }
        const int32 Index = Private::Slots.Add(Slot);
        Private::SlotIndices.Add(Slot.Key, Index); Private::LeaseIndices.Add(Slot.Lease, Index);
        Selected = &Private::Slots[Index];
    }
    auto* Values = Private::ValuesFor(*Selected);
    if (!Values) return nullptr;
    if (!bMulticast)
    {
        Values->Values.Reset(); Values->Items.Reset(); ++Values->Generation;
        Private::ReleaseCaptureLease(Selected->CaptureLease);
    }
    const uint64 Item = ++Private::NextItem;
    Selected->ItemIndices.Add(Item, Values->Items.Num());
    Values->Items.Add(Item);
    if (Type)
    {
        if (!Selected->CaptureLease.Id
            && AngelscriptCoreExtensionPoints::AcquireModuleGenerationLease(Private::CaptureClient, Type, Selected->CaptureLease)
                != AngelscriptCoreExtensionPoints::ECacheAdmission::Completed)
        {
            Values->Items.Pop(EAllowShrinking::No); Selected->ItemIndices.Remove(Item);
            return nullptr;
        }
        Values->Values.Emplace_GetRef().InitializeAs(Type, static_cast<const uint8*>(Capture));
    }
    Selected->bPending = true;
    OutEntry = Entry;
    OutHandle.Lease = Selected->Lease; OutHandle.Item = Item;
    return Target;
}
void DispatchLambdaFallback(UObject* Target, UFunction* Function, void* Parameters)
{
    if (!Private::GameThread()) return;
    const auto* Slot = Private::FindSlot(Target, Function->GetFName());
    UObject* Owner = Slot ? Slot->Owner.ResolveObjectPtr() : nullptr;
    UFunction* Entry = IsValid(Owner) ? Owner->FindFunction(Function->GetFName()) : nullptr;
    if (!Entry || !Slot || Slot->bRetired || Slot->bRetirePending) return;
    TStrongObjectPtr<UObject> RunningTarget(Target);
    TStrongObjectPtr<UObject> RunningOwner(Owner);
    TStrongObjectPtr<UFunction> RunningFunction(Function);
    Private::InvocationOverrides.Add({Target, Owner, Function->GetFName(), false});
    Owner->ProcessEvent(Entry, Parameters);
    Private::InvocationOverrides.Pop(EAllowShrinking::No);
}
void TrackLambdaSlot(UObject* Target, FName Entry, const void*)
{
    if (!Private::GameThread()) return;
    if (auto* Slot = Private::FindSlot(Target, Entry)) Slot->bPending = Slot->bTimerPending;
}
void TrackLambdaTimer(const FScriptDelegate& Delegate, UWorld* World, FTimerHandle Handle)
{
    if (!Private::GameThread()) return;
    if (auto* Slot = Private::FindSlot(const_cast<UObject*>(Delegate.GetUObject()), Delegate.GetFunctionName()))
    {
        if (!Slot->Timers.ContainsByPredicate([&](const auto& Timer) { return Timer.World == World && Timer.Handle == Handle; }))
            Slot->Timers.Add({World, Handle});
        Slot->bTimerPending = false; Slot->bPending = false; Slot->bUntracked = false;
    }
}
void SweepLambdaClosures()
{
    if (!Private::GameThread()) return;
    // A mutation schedules reclamation for pre-GC. Only executing snapshots
    // need an immediate cancellation check; detached identities stay intact.
    for (auto* Invocation : Private::Invocations)
        if (Invocation->bStorageBoundAtEntry)
            if (const auto* Slot = Private::FindSlot(Invocation->Target.Get(), Invocation->Entry); Slot && !Slot->bRetirePending && !Private::Bound(*Slot))
                Invocation->bCancelled = true;
}
void RemoveLambdaClosureBindings(FMulticastScriptDelegate& Delegate, UObject* Owner)
{
    if (!Private::GameThread()) return;
    Delegate.RemoveAll(Owner);
    for (UObject* Target : Delegate.GetAllObjects())
        if (auto* Object = Cast<USoloASLambdaClosureObject>(Target))
            for (const auto& Row : Object->Slots)
                if (const auto* Slot = Private::FindSlot(Target, Row.Key); Slot && Slot->Owner == FObjectKey(Owner))
                { FScriptDelegate Item; Item.BindUFunction(Target, Row.Key); Delegate.Remove(Item); }
    SweepLambdaClosures();
}
void RemoveLambdaHandle(void* Delegate, bool bMulticast, const FScriptLambdaHandle& Handle, USparseDelegateFunction* Sparse)
{
    if (!Private::GameThread()) return;
    const int32* SlotIndex = Private::LeaseIndices.Find(Handle.Lease);
    auto* Slot = SlotIndex ? &Private::Slots[*SlotIndex] : nullptr;
    auto* Values = Slot ? Private::ValuesFor(*Slot) : nullptr;
    const int32* ItemIndex = Slot ? Slot->ItemIndices.Find(Handle.Item) : nullptr;
    const int32 Index = ItemIndex ? *ItemIndex : INDEX_NONE;
    UObject* Target = Slot ? Slot->Key.Target.ResolveObjectPtr() : nullptr;
    bool Matches = false;
    if (Slot && Target && IsValid(Slot->Owner.ResolveObjectPtr()) && !Slot->bRetired && Index != INDEX_NONE)
    {
        const auto* Multi = bMulticast ? Private::ReadMulticast(Delegate, Sparse) : nullptr;
        Matches = bMulticast ? Multi && Multi->Contains(Target, Slot->Key.Entry)
            : static_cast<FScriptDelegate*>(Delegate)->GetUObject() == Target && static_cast<FScriptDelegate*>(Delegate)->GetFunctionName() == Slot->Key.Entry;
        const auto Storage = Private::FindStorage(Slot->Owner.ResolveObjectPtr(), Delegate);
        if (Slot->Storage.bKnown && Storage.bKnown)
            Matches &= Slot->Storage.Object == Storage.Object && Slot->Storage.Property == Storage.Property;
    }
    if (!Matches)
    {
        static bool Warned = false;
        if (!Warned) { Warned = true; UE_LOG(LogTemp, Warning, TEXT("Ignored stale or mismatched script lambda handle.")); }
        return;
    }
    // Tombstone now; keep the captured value alive until the pre-GC pass.
    Slot->ItemIndices.Remove(Handle.Item); Values->Items[Index] = 0;
    if (Slot->ItemIndices.IsEmpty())
    {
        if (bMulticast) Private::RemoveMulticast(Delegate, Sparse, Target, Slot->Key.Entry);
        else static_cast<FScriptDelegate*>(Delegate)->Unbind();
    }
    SweepLambdaClosures();
}
void RemoveSparseLambdaBindings(void* Delegate, USparseDelegateFunction* Signature, UObject* Owner)
{
    if (!Private::GameThread()) return;
    auto* Value = static_cast<FSparseDelegate*>(Delegate);
    UObject* Holder = FSparseDelegateStorage::ResolveSparseOwner(*Value, Signature->OwningClassName, Signature->DelegateName);
    FSparseDelegateStorage::RemoveAll(Holder, Signature->DelegateName, Owner);
    if (const auto* Multi = Private::ReadMulticast(Delegate, Signature))
        for (UObject* Target : Multi->GetAllObjects())
            if (auto* Object = Cast<USoloASLambdaClosureObject>(Target))
                for (const auto& Row : Object->Slots)
                    if (const auto* Slot = Private::FindSlot(Target, Row.Key); Slot && Slot->Owner == FObjectKey(Owner))
                        Private::RemoveMulticast(Delegate, Signature, Target, Row.Key);
    SweepLambdaClosures();
}
void RetainLambdaForNativeStorage(const FScriptDelegate& Delegate)
{
    if (!Private::GameThread()) return;
    if (auto* Slot = Private::FindSlot(const_cast<UObject*>(Delegate.GetUObject()), Delegate.GetFunctionName()))
    { Slot->bNativeRetained = true; if (auto* Target = Cast<USoloASLambdaClosureObject>(Slot->Key.Target.ResolveObjectPtr())) Target->bNeverPool = true; }
}
void RetainLambdaForNativeStorage(const FMulticastScriptDelegate& Delegate)
{
    if (!Private::GameThread()) return;
    for (auto& Slot : Private::Slots)
        if (UObject* Target = Slot.Key.Target.ResolveObjectPtr(); Target && Delegate.Contains(Target, Slot.Key.Entry))
        { Slot.bNativeRetained = true; if (auto* Object = Cast<USoloASLambdaClosureObject>(Target)) Object->bNeverPool = true; }
}
void ReleaseLambdaSlotsForClasses(const TArray<UClass*>& Classes)
{
    if (!Private::GameThread()) return;
    (void)Classes;
    Private::DelegateRoots.Reset(); Private::StorageIndex.Reset(); Private::FlushIdle();
    for (auto* Object : Private::ActiveObjects) Object->bNeverPool = true;
    TSet<Private::FBindingKey> Released;
    for (auto& Slot : Private::Slots)
        if (UObject* Owner = Slot.Owner.ResolveObjectPtr(); Owner && ([&]
        {
            const auto Module = FAngelscriptManager::Get().GetModule(Slot.ModuleName);
            return !Module || Module->CoreGenerationId != Slot.ModuleGeneration;
        }()))
        {
            Released.Add(Slot.Key);
            for (auto& Timer : Slot.Timers) if (Timer.World.IsValid()) Timer.World->GetTimerManager().ClearTimer(Timer.Handle);
            if (Private::IsExecuting(Slot)) Slot.bRetirePending = true;
            else Private::FinishRetirement(Slot);
        }
    Private::VisitObjects([&](UObject* Holder, FProperty*, FProperty* Property, void* Data, const FScriptDelegate* Single, const FMulticastScriptDelegate* Multi)
    {
        if (Single && Released.Contains({FObjectKey(Single->GetUObject()), Single->GetFunctionName()})) const_cast<FScriptDelegate*>(Single)->Unbind();
        else if (Multi)
            for (const auto& Key : Released) if (UObject* Target = Key.Target.ResolveObjectPtr(); Target && Multi->Contains(Target, Key.Entry))
            { FScriptDelegate Item; Item.BindUFunction(Target, Key.Entry); CastFieldChecked<FMulticastDelegateProperty>(Property)->RemoveDelegate(Item, Holder, Data); }
    });
    SweepLambdaClosures(); Private::PrunePrototypes();
}
FLambdaClosureInvocation::FLambdaClosureInvocation(UObject* Object, FName InEntry) : Entry(InEntry)
{
    if (!Private::InvocationOverrides.IsEmpty())
    {
        auto& Override = Private::InvocationOverrides.Last();
        if (!Override.bConsumed && Override.Owner == Object && Override.Entry == InEntry)
        { Object = Override.Target; Override.bConsumed = true; }
    }
    Target.Reset(Object);
    if (const auto* Slot = Private::FindSlot(Object, Entry))
    {
        Owner.Reset(Slot->Owner.ResolveObjectPtr()); Store = Slot->Store; Lease = Slot->Lease; bDirect = Slot->bDirect;
        bStorageBoundAtEntry = Private::Bound(*Slot);
        bCancelled = Slot->bRetirePending;
        if (auto* Values = Private::ValuesFor(*Slot))
        {
            if (Slot->CaptureLease.Id
                && AngelscriptCoreExtensionPoints::CloneModuleGenerationLease(Private::CaptureClient, Slot->CaptureLease, CaptureLease)
                    != AngelscriptCoreExtensionPoints::ECacheAdmission::Completed) bCancelled = true;
            if (!bCancelled) { Captures = Values->Values; Items = Values->Items; Generation = Values->Generation; }
        }
    }
    Private::Invocations.Add(this);
}
FLambdaClosureInvocation::~FLambdaClosureInvocation()
{
    Captures.Reset();
    Private::ReleaseCaptureLease(CaptureLease);
    Private::Invocations.RemoveSingleSwap(this);
    if (auto* Slot = Private::FindSlot(Target.Get(), Entry); Slot && Slot->Lease == Lease && Slot->bRetirePending && !Private::IsExecuting(*Slot))
        Private::FinishRetirement(*Slot);
}
bool FLambdaClosureInvocation::IsBound() const
{
    const auto* Slot = Private::FindSlot(Target.Get(), Entry);
    // Reaching this entry already proves a live delegate or timer dispatched it,
    // including an unobserved native copy whose original storage was cleared.
    // Recheck original storage locally for native Clear without a notification.
    return !bCancelled && Slot && !Slot->bRetired && IsValid(Slot->Owner.ResolveObjectPtr())
        && (Slot->bRetirePending || !bStorageBoundAtEntry || Private::Bound(*Slot));
}
bool FLambdaClosureInvocation::IsItemLive(int32 Index) const
{
    const auto* Slot = Private::FindSlot(Target.Get(), Entry);
    const auto* Values = Slot ? Private::ValuesFor(*Slot) : nullptr;
    return Slot && Slot->Lease == Lease && Items.IsValidIndex(Index) && Values && Slot->ItemIndices.Contains(Items[Index]);
}
bool FLambdaClosureInvocation::Read(int32 Index, UScriptStruct* Type, void* Destination) const
{
    if (!Captures.IsValidIndex(Index) || Captures[Index].GetScriptStruct() != Type) return false;
    Type->CopyScriptStruct(Destination, Captures[Index].GetMemory()); return true;
}
bool FLambdaClosureInvocation::Commit(int32 Index, UScriptStruct* Type, const void* Source)
{
    const auto* Slot = Private::FindSlot(Target.Get(), Entry);
    auto* Values = Slot ? Private::ValuesFor(*Slot) : nullptr;
    if (!Slot || Slot->Lease != Lease || !Items.IsValidIndex(Index) || !Values || Values->Generation != Generation) return false;
    const int32* CurrentIndex = Slot->ItemIndices.Find(Items[Index]);
    if (!CurrentIndex || Values->Values[*CurrentIndex].GetScriptStruct() != Type) return false;
    Type->CopyScriptStruct(Values->Values[*CurrentIndex].GetMutableMemory(), Source); return true;
}
void FLambdaClosureInvocation::AddReferencedObjects(FReferenceCollector& Collector)
{ for (auto& Capture : Captures) Capture.AddStructReferencedObjects(Collector); }
FLambdaClosureStats GetLambdaClosureStats()
{
    return {Private::Created, Private::Reused, Private::ActiveObjects.Num(), Private::IdleObjects.Num(), Private::Slots.Num(),
        Private::ObjectScans, Private::ObjectsVisited, Private::ObjectScanSeconds};
}
int32 GetLambdaCaptureCount(UObject* Owner)
{
    int32 Count = 0;
    for (const auto& Slot : Private::Slots) if (Slot.Owner == FObjectKey(Owner)) if (auto* Values = Private::ValuesFor(Slot)) Count += Values->Values.Num();
    return Count;
}
}
