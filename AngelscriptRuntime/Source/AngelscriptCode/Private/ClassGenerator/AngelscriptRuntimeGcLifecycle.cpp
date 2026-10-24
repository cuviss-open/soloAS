// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptRuntimeGcLifecycle.h"
#include "AngelscriptReloadSafety.h"

#include "ClassGenerator/ASClass.h"
#include "ClassGenerator/AngelscriptRuntimeGcMirror.h"
#include "UObject/GCObject.h"
#include "ClassGenerator/AngelscriptRetiredModules.h"
#include "ClassGenerator/AngelscriptTerminalDestructorAdmission.h"

#include "AngelscriptManager.h"
#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptType.h"
#include "StructUtils/InstancedStruct.h"
#include "HAL/PlatformTLS.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/GarbageCollection.h"
#include <new>
#include "Runtime/SoloASBridgeObjectLifecycle.h"

#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"

#include "UObject/UnrealType.h"
#if WITH_EDITOR
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Shared/SoloASScriptBlueprint.h"
#endif

#include "StartAngelscriptHeaders.h"
#include "as_scriptobject.h"
#include "as_objecttype.h"
#include "as_scriptengine.h"
#include "as_module.h"
#include "as_property.h"
#include "EndAngelscriptHeaders.h"

DEFINE_LOG_CATEGORY(LogAngelscriptRuntimeGcLifecycle);

bool AngelscriptRuntimeGcMirror::IsRuntimeGcTraceEnabled()
{
    return FParse::Param(FCommandLine::Get(), TEXT("SoloASExperimentalGcTrace"));
}

namespace
{
FDelegateHandle RuntimeLifecyclePreExitHandle;
FDelegateHandle RuntimeLifecyclePostGcHandle;
bool bRuntimeLifecycleShutdownOwnerCalled = false;

void StopAngelscriptRuntimeAtPreExit()
{
	check(IsInGameThread());
	if (FAngelscriptManager::IsInitialized())
	{
		FAngelscriptManager::Get().StopHotReloadThread();
	}
}
}

namespace
{
FCriticalSection TerminalCompilationMutex;
uint64 TerminalPlanReservations = 0;
bool bTerminalCompilationInProgress = false;
bool bTerminalCompilationRetryPending = false;
uint64 TerminalCompilationEpoch = 0;
uint32 TerminalCompilationThread = 0;
bool bTerminalInitialCompilation = false;
uint64 ReinstanceCollectionEpoch = 0;
uint64 NextReinstanceCollectionEpoch = 0;
uint64 ReinstanceCompilationEpoch = 0;
bool bPreReinstanceCollection = false;

bool IsReinstanceCollectionCurrent(uint64 Epoch)
{
    return Epoch && ReinstanceCollectionEpoch == Epoch && bTerminalCompilationInProgress
        && !bTerminalInitialCompilation && ReinstanceCompilationEpoch == TerminalCompilationEpoch
        && TerminalCompilationThread == FPlatformTLS::GetCurrentThreadId()
        && IsInGameThread() && IsAngelscriptExecutionWriterHeldByCurrentThread();
}

// This primitive reservation is independent of map membership and execution writers.
// Its mutex never spans metadata reads, callouts, or module/context releases.
class FTerminalPlanReservation
{
public:
    FTerminalPlanReservation()
    {
        FScopeLock Lock(&TerminalCompilationMutex);
        if (!bTerminalCompilationInProgress)
        {
            ++TerminalPlanReservations;
            bAcquired = true;
        }
    }
    ~FTerminalPlanReservation()
    {
        if (!bAcquired) return;
        FScopeLock Lock(&TerminalCompilationMutex);
        if (!TerminalPlanReservations || (BorrowedEpoch && !IsReinstanceCollectionCurrent(BorrowedEpoch)))
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REINSTANCE_RESERVATION_LIFETIME_INVALID"));
        --TerminalPlanReservations;
    }
    bool IsAcquired() const { return bAcquired; }
    uint64 GetBorrowedEpoch() const { return BorrowedEpoch; }
    bool IsValid() const
    {
        FScopeLock Lock(&TerminalCompilationMutex);
        return bAcquired && (BorrowedEpoch ? IsReinstanceCollectionCurrent(BorrowedEpoch)
            : !bTerminalCompilationInProgress);
    }
    bool BorrowForReinstance(uint64 Epoch)
    {
        FScopeLock Lock(&TerminalCompilationMutex);
        if (bAcquired || !IsReinstanceCollectionCurrent(Epoch)) return false;
        BorrowedEpoch = Epoch;
        ++TerminalPlanReservations;
        bAcquired = true;
        return true;
    }
    FTerminalPlanReservation(const FTerminalPlanReservation&) = delete;
    FTerminalPlanReservation& operator=(const FTerminalPlanReservation&) = delete;

private:
    bool bAcquired = false;
    uint64 BorrowedEpoch = 0;
};

enum class ETerminalPlanPhase : uint8
{
    Preparing, Prepared, Deferred, Completing, Callback, Cleaning, Retired,
};
enum class ETerminalCaptureResult : uint8 { Captured, UnsupportedCode, InvalidIdentity };
enum class ETerminalFieldPhase : uint8
{
    OriginalLive, OriginalEnded, FreshLive, FreshEnding, FreshEnded,
};

struct FTerminalOwnedField
{
    const FStructProperty* Property = nullptr;
    FName Name;
    UASClass* Owner = nullptr;
    asITypeInfo* OwnerCookie = nullptr;
    const asCObjectProperty* Member = nullptr;
    asITypeInfo* ValueCookie = nullptr;
    void* Storage = nullptr;
    FInstancedStruct* Fresh = nullptr;
    int32 Offset = 0;
    uint64 Flags = 0;
    uint32 ChainIndex = 0;
    ETerminalFieldPhase Phase = ETerminalFieldPhase::OriginalLive;
    uint64 FreshEpoch = 0;
};

struct FTerminalOwnerCode
{
    UASClass* Owner = nullptr;
    FObjectKey Key;
    int32 Index = INDEX_NONE, Serial = 0;
    asITypeInfo* Cookie = nullptr;
    asIScriptFunction* Destructor = nullptr;
    const asDWORD* ByteCode = nullptr;
    asUINT Length = 0;
    uint64 Hash = 0;
};

// These records borrow the exact mirror lease; they never root classes or retain modules.
// A terminal plan copies its record while acquiring its own existing module lease.
enum class ERemovedRuntimeAssociationRole : uint8 { Removed, Replaced };

struct FRemovedRuntimeAssociation
{
    FObjectKey ObjectKey, ClassKey;
    FObjectKey ReplacementKey;
    UASClass* Replacement = nullptr;
    ERemovedRuntimeAssociationRole Role = ERemovedRuntimeAssociationRole::Removed;
    UObject* Object = nullptr;
    UASClass* Class = nullptr;
    asITypeInfo* Type = nullptr;
    asIScriptModule* Module = nullptr;
    asIScriptEngine* Engine = nullptr;
    uint64 Sequence = 0, LifecycleGeneration = 0, SourceGeneration = 0;
    int32 PropertiesSize = 0;
    uint64 RemovalCompilationEpoch = 0;
    uint32 RemovalCompilationThread = 0;
    bool bOrdinaryAdmitted = false, bOrdinaryConsumed = false;
    uint64 OrdinaryCollectionEpoch = 0, OrdinaryCompilationEpoch = 0;
};
TMap<FObjectKey, FRemovedRuntimeAssociation> RemovedRuntimeAssociations;
UASClass* RemovingRuntimeClass = nullptr;
struct FReinstanceCollectionReceiver
{
    FRemovedRuntimeAssociation Association;
    asITypeInfo* ReplacementType = nullptr;
    asIScriptModule* ReplacementModule = nullptr;
    bool bConsumed = false;
};
TMap<FObjectKey, FReinstanceCollectionReceiver> ReinstanceCollectionReceivers;

enum class ETerminalBlueprintReceiverRole : uint8 { None, Generated, Skeleton };

struct FTerminalFinalInt32Provider
{
    FAngelscriptTerminalFinalInt32Registration Registration;
    FAngelscriptTerminalFinalInt32Ticket Ticket;
    FObjectKey ReceiverKey;
    uint64 PlanId = 0;
    int32 FinalValue = 0;
    uint64 ConstructOrder = 0, WriteOrder = 0, PublishOrder = 0, EndOrder = 0;
    bool bWriting = false;
    bool bPublished = false;
    bool bEnded = false;
    bool bRuntimeCleanupComplete = false;
};
TMap<uint64, TUniquePtr<FTerminalFinalInt32Provider>> TerminalFinalInt32Providers;
bool bRequireTerminalFinalInt32 = false;
bool bTerminalStaleCaptureControl = false;
bool bTerminalRuntimeOwnedInt32 = false;
uint64 LastTerminalFinalProviderEpoch = 0;
uint64 TerminalFinalInt32Order = 0;

bool SameTerminalItem(int32 Index, int32 Serial, const UObject* Object);

// One development scope owns a stable off-list terminal node. It is installed
// before ordinary instances exist; neither GC nor a field callback relinks it.
class FTerminalNativeTailProperty;
// Each exact class owns its execution nodes. Declaration properties and all
// ancestor links remain untouched, including the native FinishDestroy node.
class FTerminalNativeOwnedProperty;
struct FTerminalNativeTailHead
{
    UASClass* Class = nullptr;
    asCObjectType* Type = nullptr;
    FObjectKey Key;
    int32 Index = INDEX_NONE, Serial = 0, ScriptOffset = 0, TypeSize = 0, TypeId = 0, ContainerSize = 0;
    FField* Children = nullptr;
    FProperty* Properties = nullptr;
    FProperty* References = nullptr;
    FProperty* PostConstruct = nullptr;
    FProperty* OriginalHead = nullptr;
    UClass* CodeSuper = nullptr;
    TUniquePtr<FTerminalNativeOwnedProperty> OwnedProxy;
    TUniquePtr<FTerminalNativeTailProperty> TailProxy;
    bool bWasRooted = false;
};

struct FTerminalNativeTailChain
{
    asCObjectType* DeclarationType = nullptr;
    asIScriptModule* ModulePin = nullptr;
    UASClass* Class = nullptr;
    UClass* Native = nullptr;
    FStructProperty* Owned = nullptr;
    const FStructProperty* Original = nullptr;
    const FIntProperty* Scalar = nullptr;
    TUniquePtr<FTerminalNativeTailProperty> Adapter;
    FObjectKey ClassKey, NativeKey;
    int32 ClassIndex = INDEX_NONE, ClassSerial = 0, NativeIndex = INDEX_NONE, NativeSerial = 0;
    int32 NativePrefix = 0, OwnedOffset = 0, TailOffset = 0, ScalarOffset = 0, PropertiesSize = 0;
    uint64 OwnedFlags = 0, TailFlags = 0, ScalarFlags = 0, Epoch = 0;
    FField* ChildProperties = nullptr;
    FProperty* PropertyLink = nullptr;
    FProperty* RefLink = nullptr;
    FProperty* PostConstructLink = nullptr;
    FProperty* NativeRefLink = nullptr;
    FProperty* NativePostConstructLink = nullptr;
    FName NativeTailName;
    const FProperty* Properties[3]{};
    const FProperty* PropertyNext[3]{};
    const FProperty* RefNext[3]{};
    const FProperty* PostNext[3]{};
    bool bClassWasRooted = false, bNativeWasRooted = false, bNativeCallout = false;
    bool bIndependentHeads = false, bOwnedCallout = false, bAutomatic = false;
    TArray<FTerminalNativeTailHead> Heads;
    const asCObjectProperty* Members[2]{};
    const FField* FieldNext[3]{};
};
TUniquePtr<FTerminalNativeTailChain> TerminalNativeTailChain;
uint64 NextTerminalNativeTailEpoch = 0;
void DestroyTerminalNativeTail(const FTerminalNativeTailProperty& Property, void* Data);

class FTerminalNativeTailProperty final : public FStructProperty
{
public:
    FTerminalNativeTailProperty(UASClass* Owner, const FStructProperty& Original)
        : FStructProperty(Owner, Original.GetFName())
    {
        Struct = Original.Struct;
        ArrayDim = Original.ArrayDim;
        ElementSize = Original.GetElementSize();
        SetOffset_Internal(Original.GetOffset_ForInternal());
        SetPropertyFlags(Original.GetPropertyFlags());
    }
    void DestroyValueInternal(void* Data) const override { DestroyTerminalNativeTail(*this, Data); }
};

void DestroyTerminalNativeOwned(const FTerminalNativeOwnedProperty& Property, void* Data);
class FTerminalNativeOwnedProperty final : public FStructProperty
{
public:
    FTerminalNativeOwnedProperty(UASClass* Owner, const FStructProperty& Original)
        : FStructProperty(Owner, Original.GetFName())
    {
        Struct = Original.Struct; ArrayDim = Original.ArrayDim; ElementSize = Original.GetElementSize();
        SetOffset_Internal(Original.GetOffset_ForInternal()); SetPropertyFlags(Original.GetPropertyFlags());
    }
    void DestroyValueInternal(void* Data) const override { DestroyTerminalNativeOwned(*this, Data); }
};

FTerminalNativeTailHead* FindTerminalNativeTailHead(FTerminalNativeTailChain& C, const UASClass* Class)
{
    if (!C.bIndependentHeads) return nullptr;
    for (auto& Head : C.Heads) if (Head.Class == Class) return &Head;
    return nullptr;
}

bool IsTerminalNativeTailClass(const FTerminalNativeTailChain& C, const UASClass* Class)
{
    if (!C.bIndependentHeads) return C.Class == Class;
    for (const auto& Head : C.Heads) if (Head.Class == Class) return true;
    return false;
}

TMap<UASClass*, TUniquePtr<FTerminalNativeTailChain>> RuntimeNativeTailChains;

FTerminalNativeTailChain* FindRuntimeNativeTailChain(const UASClass* Class)
{
    if (TerminalNativeTailChain && IsTerminalNativeTailClass(*TerminalNativeTailChain, Class))
        return TerminalNativeTailChain.Get();
    const auto* Entry = RuntimeNativeTailChains.Find(const_cast<UASClass*>(Class));
    return Entry ? Entry->Get() : nullptr;
}

bool IsCurrentNativeTailChain(const FTerminalNativeTailChain* Chain, const UASClass* Class)
{
    return Chain && FindRuntimeNativeTailChain(Class) == Chain;
}

FTerminalNativeTailChain* FindRuntimeNativeTailProperty(const FProperty* Property)
{
    auto Matches = [Property](const FTerminalNativeTailChain& Chain)
    {
        if (Chain.Adapter.Get() == Property) return true;
        for (const auto& Head : Chain.Heads)
            if (Head.OwnedProxy.Get() == Property || Head.TailProxy.Get() == Property) return true;
        return false;
    };
    if (TerminalNativeTailChain && Matches(*TerminalNativeTailChain)) return TerminalNativeTailChain.Get();
    for (const auto& Pair : RuntimeNativeTailChains)
        if (Matches(*Pair.Value)) return Pair.Value.Get();
    return nullptr;
}

bool ValidateAutomaticNativeTailChain(const FTerminalNativeTailChain& C, bool bInstalled)
{
    if (!C.bAutomatic || !C.bIndependentHeads || C.Heads.Num() != 1 || !C.Class || !C.Native
        || !SameTerminalItem(C.ClassIndex, C.ClassSerial, C.Class)
        || !SameTerminalItem(C.NativeIndex, C.NativeSerial, C.Native)
        || C.ClassKey.ResolveObjectPtrEvenIfUnreachable() != C.Class
        || C.NativeKey.ResolveObjectPtrEvenIfUnreachable() != C.Native
        || C.Class->bLayoutChanging || C.Native->bLayoutChanging
        || C.Class->GetSuperClass() != C.Native || C.Class->CodeSuperClass != C.Native
        || C.Class->UeDelayedDestroyFields.Num() != 1 || !C.Class->UeDelayedDestroyFields.Contains(C.Owned)
        || !C.Owned || !C.Original || !C.Scalar || C.Class->GetPropertiesSize() != C.PropertiesSize
        || C.Native->GetPropertiesSize() != C.NativePrefix || C.Native->DestructorLink != C.Original
        || C.Owned->GetOwnerStruct() != C.Class || C.Scalar->GetOwnerStruct() != C.Class
        || C.Original->GetOwnerStruct() != C.Native || C.Owned->DestructorLinkNext != C.Original
        || C.Original->DestructorLinkNext || C.Owned->Struct != FInstancedStruct::StaticStruct()
        || C.Original->Struct != C.Owned->Struct || C.Owned->GetOffset_ForInternal() != C.OwnedOffset
        || C.Original->GetOffset_ForInternal() != C.TailOffset || C.Scalar->GetOffset_ForInternal() != C.ScalarOffset
        || uint64(C.Owned->GetPropertyFlags()) != C.OwnedFlags
        || uint64(C.Original->GetPropertyFlags()) != C.TailFlags
        || uint64(C.Scalar->GetPropertyFlags()) != C.ScalarFlags) return false;
    const auto& H = C.Heads[0];
    auto* DeclarationType = C.DeclarationType;
    if (!H.Class || !H.Type || !H.OwnedProxy || !H.TailProxy || !DeclarationType
        || !SameTerminalItem(H.Index, H.Serial, H.Class) || H.Key.ResolveObjectPtrEvenIfUnreachable() != H.Class
        || H.Class->bLayoutChanging || (H.Class->ScriptTypePtr && H.Class->ScriptTypePtr != H.Type) || H.Type->GetUserData() != H.Class
        || H.Type->typeId != H.TypeId || H.TypeId <= 0 || H.Type->size != H.TypeSize
        || H.Type->engine->GetTypeInfoById(H.TypeId) != H.Type
        || H.Type->GetModule() != DeclarationType->GetModule() || !C.ModulePin || C.ModulePin != H.Type->GetModule()
        || H.Class->CodeSuperClass != H.CodeSuper || H.Class->ScriptPropertyOffset != H.ScriptOffset
        || H.Class->GetPropertiesSize() != H.ContainerSize || H.ContainerSize < H.TypeSize
        || H.Class->ChildProperties != H.Children || H.Class->PropertyLink != H.Properties
        || H.Class->RefLink != H.References || H.Class->PostConstructLink != H.PostConstruct
        || (H.Class != C.Class && (H.Class->GetSuperClass() != C.Class || H.Type->derivedFrom != DeclarationType
            || H.Type->localProperties.GetLength() || H.Type->size != DeclarationType->size))
        || H.Class->DestructorLink != (bInstalled ? H.OwnedProxy.Get() : H.OriginalHead)
        || H.OwnedProxy->DestructorLinkNext != H.TailProxy.Get() || H.TailProxy->DestructorLinkNext
        || H.OwnedProxy->GetOwnerStruct() != H.Class || H.TailProxy->GetOwnerStruct() != H.Class
        || H.OwnedProxy->GetOffset_ForInternal() != C.OwnedOffset || H.TailProxy->GetOffset_ForInternal() != C.TailOffset)
        return false;
    const FProperty* P = C.Class->PropertyLink;
    for (int32 I = 0; I != 3; ++I)
    {
        if (!P || P != C.Properties[I] || P->Next != C.FieldNext[I]
            || P->PropertyLinkNext != C.PropertyNext[I] || P->NextRef != C.RefNext[I]
            || P->PostConstructLinkNext != C.PostNext[I]) return false;
        P = P->PropertyLinkNext;
    }
    return P == nullptr;
}

bool ValidateTerminalIndependentNativeTailChain(const FTerminalNativeTailChain& C, bool bInstalled)
{
    if (!IsInGameThread() || !C.bIndependentHeads || !C.Class || !C.Native || !C.Owned || !C.Original || !C.Scalar
        || !SameTerminalItem(C.ClassIndex, C.ClassSerial, C.Class)
        || !SameTerminalItem(C.NativeIndex, C.NativeSerial, C.Native)
        || C.ClassKey.ResolveObjectPtrEvenIfUnreachable() != C.Class
        || C.NativeKey.ResolveObjectPtrEvenIfUnreachable() != C.Native
        || !C.Class->IsRooted() || !C.Native->IsRooted()
        || C.Class->bHasASClassParent || C.Class->GetSuperClass() != C.Native
        || C.Class->CodeSuperClass != C.Native || C.Class->ScriptPropertyOffset < C.NativePrefix
        || C.Native->GetSuperClass() != UObject::StaticClass() || !C.Native->HasAnyClassFlags(CLASS_Native)
        || C.Native->bLayoutChanging || C.Native->HasAnyClassFlags(CLASS_NewerVersionExists)
        || C.Native->GetPropertiesSize() != C.NativePrefix || C.NativePrefix <= int32(sizeof(UObject))
        || C.Class->UeDelayedDestroyFields.Num() != 1 || !C.Class->UeDelayedDestroyFields.Contains(C.Owned)
        || C.Owned->GetOwnerStruct() != C.Class || C.Scalar->GetOwnerStruct() != C.Class
        || C.Original->GetOwnerStruct() != C.Native || C.Native->DestructorLink != C.Original
        || C.Native->ChildProperties != C.Original || C.Native->PropertyLink != C.Original
        || C.Native->RefLink != C.NativeRefLink || C.Native->PostConstructLink != C.NativePostConstructLink
        || C.Owned->DestructorLinkNext != C.Original || C.Original->DestructorLinkNext || C.Original->Next
        || C.Owned->Struct != FInstancedStruct::StaticStruct() || C.Original->Struct != C.Owned->Struct
        || C.Owned->ArrayDim != 1 || C.Original->ArrayDim != 1 || C.Scalar->ArrayDim != 1
        || C.Owned->HasSetterOrGetter() || C.Original->HasSetterOrGetter() || C.Scalar->HasSetterOrGetter()
        || C.Owned->GetSize() != sizeof(FInstancedStruct) || C.Original->GetSize() != sizeof(FInstancedStruct)
        || C.Scalar->GetClass() != FIntProperty::StaticClass() || C.Scalar->GetSize() != sizeof(int32)
        || C.Owned->GetOffset_ForInternal() != C.OwnedOffset || C.Original->GetOffset_ForInternal() != C.TailOffset
        || C.Scalar->GetOffset_ForInternal() != C.ScalarOffset || C.Original->GetFName() != C.NativeTailName
        || uint64(C.Owned->GetPropertyFlags()) != C.OwnedFlags || uint64(C.Original->GetPropertyFlags()) != C.TailFlags
        || uint64(C.Scalar->GetPropertyFlags()) != C.ScalarFlags
        || C.OwnedOffset < C.NativePrefix || C.ScalarOffset < C.NativePrefix
        || C.OwnedOffset > C.PropertiesSize - int32(sizeof(FInstancedStruct))
        || C.ScalarOffset > C.PropertiesSize - int32(sizeof(int32))
        || C.TailOffset < int32(sizeof(UObject)) || C.TailOffset > C.NativePrefix - int32(sizeof(FInstancedStruct))) return false;
    const auto* ParentType = C.Heads[0].Type;
    if (C.Heads[0].Class != C.Class || !ParentType || ParentType->derivedFrom || ParentType->localProperties.GetLength() != 2
        || ParentType->properties.GetLength() != 2) return false;
    const FProperty* P = C.Class->PropertyLink;
    for (int32 Index = 0; Index != 3; ++Index)
    {
        if (!P || P != C.Properties[Index] || P->Next != C.FieldNext[Index]
            || P->PropertyLinkNext != C.PropertyNext[Index] || P->NextRef != C.RefNext[Index]
            || P->PostConstructLinkNext != C.PostNext[Index]) return false;
        P = P->PropertyLinkNext;
    }
    if (P) return false;
    for (int32 Index = 0; Index != 3; ++Index)
    {
        const auto& Head = C.Heads[Index];
        const auto* Class = Head.Class;
        if (!Class || !Head.Type || !Head.OwnedProxy || !Head.TailProxy
            || !SameTerminalItem(Head.Index, Head.Serial, Class) || Head.Key.ResolveObjectPtrEvenIfUnreachable() != Class
            || !Class->IsRooted() || Class->GetClass() != UASClass::StaticClass()
            || Class->bLayoutChanging || Class->NewerVersion || Class->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic | CLASS_NewerVersionExists)
            || Class->GetSuperClass() != (Index ? C.Class : C.Native) || Class->bHasASClassParent != (Index != 0)
            || Class->CodeSuperClass != Head.CodeSuper || Class->ScriptPropertyOffset != Head.ScriptOffset
            || Class->ScriptTypePtr != static_cast<asITypeInfo*>(Head.Type) || Head.Type->GetUserData() != Class
            || Head.Type->module != ParentType->module || Head.Type->size != ParentType->size
            || Head.Type->size != Head.TypeSize || Head.Type->typeId != Head.TypeId
            || Head.Type->derivedFrom != (Index ? ParentType : nullptr)
            || Head.Type->localProperties.GetLength() != (Index ? 0u : 2u) || Head.Type->properties.GetLength() != 2
            || Class->UeDelayedDestroyFields.Num() != (Index ? 0 : 1)
            || Class->GetPropertiesSize() != Head.ContainerSize || Head.ContainerSize < C.PropertiesSize
            || Head.ContainerSize < int32(Head.TypeSize)
            || Class->GetPropertiesStartOffset() != 0 || Class->ChildProperties != Head.Children
            || (Index && Head.Children) || Class->PropertyLink != Head.Properties || Head.Properties != C.PropertyLink
            || Class->RefLink != Head.References || Class->PostConstructLink != Head.PostConstruct
            || Head.OriginalHead != C.Owned || Class->DestructorLink != (bInstalled ? Head.OwnedProxy.Get() : Head.OriginalHead)
            || Head.OwnedProxy->DestructorLinkNext != Head.TailProxy.Get() || Head.TailProxy->DestructorLinkNext
            || Head.OwnedProxy->GetOwnerStruct() != Class || Head.TailProxy->GetOwnerStruct() != Class
            || Head.OwnedProxy->Next || Head.TailProxy->Next || Head.OwnedProxy->PropertyLinkNext || Head.TailProxy->PropertyLinkNext
            || Head.OwnedProxy->NextRef || Head.TailProxy->NextRef || Head.OwnedProxy->PostConstructLinkNext || Head.TailProxy->PostConstructLinkNext
            || Head.OwnedProxy->Struct != C.Owned->Struct || Head.TailProxy->Struct != C.Original->Struct
            || Head.OwnedProxy->GetFName() != C.Owned->GetFName() || Head.TailProxy->GetFName() != C.NativeTailName
            || Head.OwnedProxy->GetOffset_ForInternal() != C.OwnedOffset || Head.TailProxy->GetOffset_ForInternal() != C.TailOffset
            || Head.OwnedProxy->ArrayDim != 1 || Head.TailProxy->ArrayDim != 1
            || Head.OwnedProxy->GetSize() != sizeof(FInstancedStruct) || Head.TailProxy->GetSize() != sizeof(FInstancedStruct)
            || uint64(Head.OwnedProxy->GetPropertyFlags()) != C.OwnedFlags || uint64(Head.TailProxy->GetPropertyFlags()) != C.TailFlags) return false;
        for (asUINT Member = 0; Member != 2; ++Member)
            if (!ParentType->localProperties[Member] || ParentType->localProperties[Member] != C.Members[Member]
                || ParentType->properties[Member] != C.Members[Member] || Head.Type->properties[Member] != C.Members[Member]
                || C.Members[Member] == C.Members[1 - Member]) return false;
    }
    // Only the fixed two empty siblings are admitted. A new child/relink is not
    // silently repaired while a receiver's native teardown is in progress.
    for (TObjectIterator<UObject> It; It; ++It)
        if (const UClass* Class = Cast<UClass>(*It))
            if (Class != C.Class && Class->IsChildOf(C.Class)
                && Class != C.Heads[1].Class && Class != C.Heads[2].Class) return false;
    return true;
}

bool ValidateTerminalNativeTailChain(const FTerminalNativeTailChain& C, bool bInstalled = true)
{
    if (C.bAutomatic) return ValidateAutomaticNativeTailChain(C, bInstalled);
    if (C.bIndependentHeads) return ValidateTerminalIndependentNativeTailChain(C, bInstalled);
    if (!IsInGameThread() || !C.Class || !C.Native || !C.Owned || !C.Original || !C.Scalar || !C.Adapter
        || !SameTerminalItem(C.ClassIndex, C.ClassSerial, C.Class)
        || !SameTerminalItem(C.NativeIndex, C.NativeSerial, C.Native)
        || C.ClassKey.ResolveObjectPtrEvenIfUnreachable() != C.Class
        || C.NativeKey.ResolveObjectPtrEvenIfUnreachable() != C.Native
        || !C.Class->IsRooted() || !C.Native->IsRooted()
        || C.Class->GetClass() != UASClass::StaticClass() || C.Class->bLayoutChanging || C.Class->NewerVersion
        || C.Class->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic)
        || C.Class->bHasASClassParent || !C.Class->ScriptTypePtr
        || C.Class->GetSuperClass() != C.Native || C.Class->CodeSuperClass != C.Native
        || C.Native->GetSuperClass() != UObject::StaticClass() || !C.Native->HasAnyClassFlags(CLASS_Native)
        || C.Native->bLayoutChanging || C.Native->HasAnyClassFlags(CLASS_NewerVersionExists)
        || C.Native->GetPropertiesSize() != C.NativePrefix || C.NativePrefix <= int32(sizeof(UObject))
        || C.Class->ScriptPropertyOffset < C.NativePrefix || C.Class->GetPropertiesStartOffset() != 0
        || C.Class->GetPropertiesSize() != C.PropertiesSize
        || C.Class->ChildProperties != C.ChildProperties || C.Class->PropertyLink != C.PropertyLink
        || C.Class->RefLink != C.RefLink || C.Class->PostConstructLink != C.PostConstructLink
        || C.Class->UeDelayedDestroyFields.Num() != 1 || !C.Class->UeDelayedDestroyFields.Contains(C.Owned)
        || C.Class->DestructorLink != C.Owned || C.Native->DestructorLink != C.Original
        || C.Native->PropertyLink != C.Original || C.Native->ChildProperties != C.Original
        || C.Native->RefLink != C.NativeRefLink || C.Native->PostConstructLink != C.NativePostConstructLink
        || C.Original->DestructorLinkNext || C.Original->Next
        || C.Original->GetFName() != C.NativeTailName || C.Adapter->GetFName() != C.NativeTailName
        || C.Owned->DestructorLinkNext != (bInstalled ? C.Adapter.Get() : C.Original)
        || C.Adapter->DestructorLinkNext || C.Adapter->PropertyLinkNext || C.Adapter->NextRef
        || C.Adapter->PostConstructLinkNext || C.Adapter->Next
        || C.Owned->GetOwnerStruct() != C.Class || C.Original->GetOwnerStruct() != C.Native
        || C.Scalar->GetOwnerStruct() != C.Class || C.Adapter->GetOwnerStruct() != C.Class
        || C.Owned->Struct != FInstancedStruct::StaticStruct() || C.Original->Struct != C.Owned->Struct
        || C.Adapter->Struct != C.Original->Struct || C.Adapter->GetOffset_ForInternal() != C.TailOffset
        || C.Adapter->GetSize() != sizeof(FInstancedStruct) || C.Adapter->ArrayDim != 1
        || uint64(C.Adapter->GetPropertyFlags()) != C.TailFlags
        || C.Owned->GetOffset_ForInternal() != C.OwnedOffset || C.Original->GetOffset_ForInternal() != C.TailOffset
        || C.Scalar->GetOffset_ForInternal() != C.ScalarOffset
        || C.Owned->GetSize() != sizeof(FInstancedStruct) || C.Original->GetSize() != sizeof(FInstancedStruct)
        || C.Scalar->GetSize() != sizeof(int32) || C.Scalar->GetClass() != FIntProperty::StaticClass()
        || C.Owned->ArrayDim != 1 || C.Original->ArrayDim != 1 || C.Scalar->ArrayDim != 1
        || C.Owned->HasSetterOrGetter() || C.Original->HasSetterOrGetter() || C.Scalar->HasSetterOrGetter()
        || uint64(C.Owned->GetPropertyFlags()) != C.OwnedFlags
        || uint64(C.Original->GetPropertyFlags()) != C.TailFlags
        || uint64(C.Scalar->GetPropertyFlags()) != C.ScalarFlags
        || C.OwnedOffset < C.NativePrefix || C.ScalarOffset < C.NativePrefix
        || C.TailOffset < int32(sizeof(UObject)) || C.TailOffset > C.NativePrefix - int32(sizeof(FInstancedStruct))) return false;
    const auto* Type = CastToObjectType(static_cast<asCTypeInfo*>(static_cast<asITypeInfo*>(C.Class->ScriptTypePtr)));
    if (!Type || Type->derivedFrom || Type->properties.GetLength() != 2 || Type->localProperties.GetLength() != 2) return false;
    const FProperty* Current = C.Class->PropertyLink;
    for (int32 Index = 0; Index != 3; ++Index)
    {
        if (!Current || Current != C.Properties[Index] || Current->PropertyLinkNext != C.PropertyNext[Index]
            || Current->NextRef != C.RefNext[Index] || Current->PostConstructLinkNext != C.PostNext[Index]) return false;
        Current = Current->PropertyLinkNext;
    }
    if (Current) return false;
    for (TObjectIterator<UObject> It; It; ++It)
        if (const UClass* Class = Cast<UClass>(*It))
            if (Class != C.Class && Class->IsChildOf(C.Class)) return false;
    return true;
}

struct FTerminalDestroyPlan
{
    TOptional<FRemovedRuntimeAssociation> RemovedAssociation;
    TUniquePtr<FTerminalPlanReservation> CompilationReservation;
    SoloASBridge::FRuntimeDeferredDestroyToken Token;
    UASClass* Class = nullptr;
    FObjectKey ClassKey;
    UClass* ActualReceiverClass = nullptr;
    FObjectKey ActualReceiverClassKey;
    int32 ActualReceiverClassIndex = INDEX_NONE, ActualReceiverClassSerial = 0, ActualReceiverPropertiesSize = 0;
    UObject* ReceiverBlueprint = nullptr;
    FObjectKey ReceiverBlueprintKey;
    int32 ReceiverBlueprintIndex = INDEX_NONE, ReceiverBlueprintSerial = 0;
    ETerminalBlueprintReceiverRole ReceiverBlueprintRole = ETerminalBlueprintReceiverRole::None;
    bool bReceiverIsCurrentCdo = false;
    int32 ReceiverIndex = INDEX_NONE, ReceiverSerial = 0, ClassIndex = INDEX_NONE, ClassSerial = 0;
    const FStructProperty* Property = nullptr;
    void* Storage = nullptr;
    int32 Offset = 0;
    uint64 PropertyFlags = 0;
    const FProperty* Chain[64]{};
    uint32 ChainLength = 0, ThreadId = 0;
    asIScriptModule* ModulePin = nullptr;
    asIScriptFunction* Destructor = nullptr;
    const asDWORD* ByteCode = nullptr;
    asUINT ByteCodeLength = 0;
    uint64 CodeHash = 0;
    TArray<FTerminalOwnedField> Fields;
    TArray<FTerminalOwnerCode> Owners;
    Cuviss::TerminalAdmission::FInt32Slot Int32Slot;
    Cuviss::TerminalAdmission::EInt32SlotPhase Int32Phase = Cuviss::TerminalAdmission::EInt32SlotPhase::Absent;
    FTerminalFinalInt32Provider* FinalInt32Provider = nullptr;
    int32 FinalInt32Delivery = 0;
    uint64 FinalInt32Epoch = 0;
    FTerminalNativeTailChain* NativeTailChain = nullptr;
    FTerminalNativeTailHead* NativeTailHead = nullptr;
    uint32 OwnedFinishCalls = 0;
    bool bNativeTailReturned = false;
    bool bRuntimeOwnedFieldReturned = false;
    int32* FreshInt32 = nullptr;
    uint64 FreshInt32Epoch = 0;
    uint32 OriginalCompleted = 0, EmptyConstructed = 0, EmptyDestroyed = 0;
    int32 PropertiesSize = 0;
    ETerminalPlanPhase Phase = ETerminalPlanPhase::Preparing;
    TUniquePtr<FAngelscriptExecutionWriteScope> PrepareWriter;
    TSharedPtr<Cuviss::TerminalAdmission::FCompiledTerminalDestructorProof> Qualification;
    bool bDevFacade = false;
    bool bFaultInjected = false;
};
TMap<FObjectKey, TUniquePtr<FTerminalDestroyPlan>> TerminalPlans;
thread_local FTerminalDestroyPlan* ActiveRuntimeTerminalPlan = nullptr;
thread_local FTerminalDestroyPlan* ActiveTerminalOperation = nullptr;
uint64 LastTerminalArmEpoch = 0;
uint64 NextTerminalPlanId = 0;
uint64 NextTerminalInt32Epoch = 0;

bool IsAutomaticTerminalPlan(const FTerminalDestroyPlan& Plan)
{
    return Plan.NativeTailChain && Plan.NativeTailChain->bAutomatic
        && IsCurrentNativeTailChain(Plan.NativeTailChain, Plan.Class);
}

bool HasRuntimeOwnedTerminalScalar(const FTerminalDestroyPlan& Plan)
{
    return bTerminalRuntimeOwnedInt32 || IsAutomaticTerminalPlan(Plan);
}

bool HasTerminalFieldStorageIdentity(const FTerminalDestroyPlan& Plan, const void* ValueAddress)
{
    for (const auto& Field : Plan.Fields)
        if (Field.Storage == ValueAddress) return true;
    return false;
}

bool SameTerminalItem(int32 Index, int32 Serial, const UObject* Object)
{
    const FUObjectItem* Item = Index >= 0 ? GUObjectArray.IndexToObject(Index) : nullptr;
    return Serial != 0 && Item && Item->GetSerialNumber() == Serial && Item->GetObject() == Object;
}

// This proof reads only receiver metadata. It never calls the admission registry or a log sink.
bool IsTerminalBlueprintReceiverMetadata(const UClass* ReceiverClass, const UASClass* Owner,
    const FStructProperty* Property, ETerminalBlueprintReceiverRole* OutRole = nullptr,
    UObject** OutBlueprint = nullptr)
{
#if WITH_EDITOR
    if (!ReceiverClass || !Owner || !Property || ReceiverClass == Owner
        || ReceiverClass->GetClass() != USoloASScriptBlueprintGeneratedClass::StaticClass()
        || ReceiverClass->GetOutermost() != GetTransientPackage() || ReceiverClass->bLayoutChanging
        || ReceiverClass->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Hidden)
        || ReceiverClass->GetSuperClass() != Owner || Owner->GetClass() != UASClass::StaticClass()
        || Owner->GetSuperClass() != UObject::StaticClass() || Owner->bLayoutChanging
        || Owner->UeDelayedDestroyFields.Num() != 1 || !Owner->UeDelayedDestroyFields.Contains(Property)
        || Property->GetOwnerStruct() != Owner || Property->Struct != FInstancedStruct::StaticStruct()
        || Property->ArrayDim != 1 || Property->GetSize() != sizeof(FInstancedStruct)
        || Property->GetElementSize() != sizeof(FInstancedStruct)
        || Owner->DestructorLink != Property || ReceiverClass->DestructorLink != Property
        || Property->DestructorLinkNext) return false;
    const auto* Blueprint = Cast<UBlueprint>(ReceiverClass->ClassGeneratedBy);
    if (!Blueprint || Blueprint->GetClass() != USoloASScriptBlueprint::StaticClass()
        || Blueprint->GetOutermost() != GetTransientPackage()) return false;
    const bool bGenerated = Blueprint->GeneratedClass == ReceiverClass;
    const bool bSkeleton = Blueprint->SkeletonGeneratedClass == ReceiverClass;
    // Each receiver has exactly one current slot; names and former slot membership are not proof.
    if (bGenerated == bSkeleton) return false;
    const FProperty* ReceiverField = ReceiverClass->PropertyLink;
    if (ReceiverClass->ChildProperties)
    {
        // Bridge proves the exact compiler-owned Skeleton frame tail and all
        // four links. It does not grant a script destruction capability.
        if (!bSkeleton || !CastChecked<USoloASScriptBlueprintGeneratedClass>(ReceiverClass)
            ->CanOrderTerminalMetadataFinish(false)) return false;
        ReceiverField = ReceiverField->PropertyLinkNext;
    }
    else if (ReceiverClass->GetPropertiesSize() != Owner->GetPropertiesSize()) return false;
    const FProperty* OwnerField = Owner->PropertyLink;
    uint32 Seen = 0;
    while (ReceiverField || OwnerField)
    {
        if (++Seen > 1024 || !ReceiverField || ReceiverField != OwnerField
            || ReceiverField->GetOwnerStruct() == ReceiverClass) return false;
        ReceiverField = ReceiverField->PropertyLinkNext;
        OwnerField = OwnerField->PropertyLinkNext;
    }
    if (OutRole) *OutRole = bGenerated ? ETerminalBlueprintReceiverRole::Generated
        : ETerminalBlueprintReceiverRole::Skeleton;
    if (OutBlueprint) *OutBlueprint = const_cast<UBlueprint*>(Blueprint);
    return true;
#else
    return false;
#endif
}

bool IsTerminalReceiverTemplateAllowed(const UObject* Object, UClass* ReceiverClass,
    const UASClass* Owner, const FStructProperty* Property, bool bDevFacade, bool bRemovedReceiver)
{
    if (!Object || !ReceiverClass || Object->GetClass() != ReceiverClass) return false;
#if WITH_EDITOR
    const auto* Blueprint = Cast<UBlueprint>(ReceiverClass->ClassGeneratedBy);
    if (Blueprint && Blueprint->SkeletonGeneratedClass == ReceiverClass && ReceiverClass->ChildProperties)
    {
        // Only the current CDO's empty frame is supported, never ordinary
        // Skeleton instances or allocated persistent frames.
        const auto* Frame = CastField<FStructProperty>(ReceiverClass->ChildProperties);
        if (!Object->HasAllFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
            || ReceiverClass->GetDefaultObject(false) != Object
            || !IsTerminalBlueprintReceiverMetadata(ReceiverClass, Owner, Property)
            || !Frame || Frame->ContainerPtrToValuePtr<FPointerToUberGraphFrame>(Object)->RawPointer)
            return false;
    }
#endif
    if (!bDevFacade && !bRemovedReceiver && ReceiverClass == Owner
        && FindRuntimeNativeTailChain(Owner) && FindRuntimeNativeTailChain(Owner)->bAutomatic
        && Object->HasAllFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
        && !Object->HasAnyFlags(RF_DefaultSubObject) && Owner->GetDefaultObject(false) == Object)
        return true;
    if (!Object->IsTemplate()) return true;
    // Only the actual current CDO of a qualified BP slot is admitted. AS CDOs and other
    // archetypes retain the ordinary strict template rejection.
    return !bDevFacade && !bRemovedReceiver && Owner && ReceiverClass != Owner
        && Object->HasAllFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
        && !Object->HasAnyFlags(RF_DefaultSubObject)
        && Object->GetOuter() == ReceiverClass->GetOuter()
        && ReceiverClass->GetDefaultObject(false) == Object
        && Owner->ScriptTypePtr && !Owner->NewerVersion
        && !Owner->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Hidden)
        && IsTerminalBlueprintReceiverMetadata(ReceiverClass, Owner, Property)
#if WITH_EDITOR
        && CastChecked<USoloASScriptBlueprintGeneratedClass>(ReceiverClass)->CanOrderTerminalMetadataFinish(true)
#endif
        ;
}

bool ValidateTerminalActualReceiverMetadata(const FTerminalDestroyPlan& Plan)
{
    if (!Plan.Token.Object || !Plan.Class || !Plan.ActualReceiverClass
        || !SameTerminalItem(Plan.ReceiverIndex, Plan.ReceiverSerial, Plan.Token.Object)
        || !SameTerminalItem(Plan.ClassIndex, Plan.ClassSerial, Plan.Class)
        || !SameTerminalItem(Plan.ActualReceiverClassIndex, Plan.ActualReceiverClassSerial, Plan.ActualReceiverClass)
        || Plan.Token.Key.ResolveObjectPtrEvenIfUnreachable() != Plan.Token.Object
        || Plan.ClassKey.ResolveObjectPtrEvenIfUnreachable() != Plan.Class
        || Plan.ActualReceiverClassKey.ResolveObjectPtrEvenIfUnreachable() != Plan.ActualReceiverClass
        || Plan.Token.Object->GetClass() != Plan.ActualReceiverClass
        || Plan.Class->GetClass() != UASClass::StaticClass()
        || Plan.ActualReceiverClass->bLayoutChanging
        || Plan.ActualReceiverClass->GetPropertiesSize() != Plan.ActualReceiverPropertiesSize) return false;
    if (Plan.ActualReceiverClass == Plan.Class)
        return Plan.ActualReceiverPropertiesSize == Plan.PropertiesSize
            && !Plan.ReceiverBlueprint && Plan.ReceiverBlueprintRole == ETerminalBlueprintReceiverRole::None
            && (!Plan.bReceiverIsCurrentCdo || (IsAutomaticTerminalPlan(Plan)
                && Plan.Class->GetDefaultObject(false) == Plan.Token.Object));
#if WITH_EDITOR
    if (Plan.bDevFacade || Plan.RemovedAssociation.IsSet()
        || !Plan.ReceiverBlueprint || Plan.ReceiverBlueprintRole == ETerminalBlueprintReceiverRole::None
        || !SameTerminalItem(Plan.ReceiverBlueprintIndex, Plan.ReceiverBlueprintSerial, Plan.ReceiverBlueprint)
        || Plan.ReceiverBlueprintKey.ResolveObjectPtrEvenIfUnreachable() != Plan.ReceiverBlueprint
        || Plan.ActualReceiverClass->ClassGeneratedBy != Plan.ReceiverBlueprint
        || Plan.Token.Object->HasAnyFlags(RF_ClassDefaultObject) != Plan.bReceiverIsCurrentCdo
        || !IsTerminalReceiverTemplateAllowed(Plan.Token.Object, Plan.ActualReceiverClass, Plan.Class,
            Plan.Property, Plan.bDevFacade, Plan.RemovedAssociation.IsSet())) return false;
    ETerminalBlueprintReceiverRole CurrentRole = ETerminalBlueprintReceiverRole::None;
    UObject* CurrentBlueprint = nullptr;
    return IsTerminalBlueprintReceiverMetadata(Plan.ActualReceiverClass, Plan.Class, Plan.Property,
        &CurrentRole, &CurrentBlueprint)
        && CurrentBlueprint == Plan.ReceiverBlueprint && CurrentRole == Plan.ReceiverBlueprintRole;
#else
    return false;
#endif
}

uint64 HashTerminalCode(const asDWORD* Code, asUINT Length)
{
    uint64 Hash = 1469598103934665603ull;
    for (asUINT Index = 0; Index != Length; ++Index) Hash = (Hash ^ Code[Index]) * 1099511628211ull;
    return Hash;
}

asIScriptFunction* FindTerminalDestructor(asITypeInfo* Type)
{
    asIScriptFunction* Found = nullptr;
    for (asUINT Index = 0; Index != Type->GetBehaviourCount(); ++Index)
    {
        asEBehaviours Behaviour;
        auto* Function = Type->GetBehaviourByIndex(Index, &Behaviour);
        if (Behaviour != asBEHAVE_DESTRUCT) continue;
        if (Found || !Function) return nullptr;
        Found = Function;
    }
    return Found;
}

bool ValidateRemovedRuntimeTuple(const FRemovedRuntimeAssociation& Record, UObject* Object,
    const UASClass* Class, const asITypeInfo* Type, const asIScriptModule* Module, uint64 Sequence)
{
    // Resolve both serial keys before touching either UObject, then establish type registry ownership.
    if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread()
        || !Object || !Class || !Type || !Module || !Sequence
        || Record.Object != Object || Record.Class != Class || Record.Type != Type
        || Record.Module != Module || Record.Sequence != Sequence
        || !Record.LifecycleGeneration || Record.LifecycleGeneration != UASClass::RuntimeLifecycleGeneration
        || Record.ObjectKey.ResolveObjectPtrEvenIfUnreachable() != Object
        || Record.ClassKey.ResolveObjectPtrEvenIfUnreachable() != Class
        || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Type)) return false;
    return Record.Engine == FAngelscriptManager::Get().GetScriptEngine() && Type->GetEngine() == Record.Engine
        && Type->GetModule() == Module && Type->GetUserData() == Class
        && UASClass::GetFirstASClass(Object->GetClass()) == Class
        && !Class->bLayoutChanging && Class->GetPropertiesSize() == Record.PropertiesSize
        && !Class->ScriptTypePtr;
}

bool ValidateRemovedRuntimeAssociation(const FRemovedRuntimeAssociation& Record, UObject* Object,
    const UASClass* Class, const asITypeInfo* Type, const asIScriptModule* Module, uint64 Sequence)
{
    if (!ValidateRemovedRuntimeTuple(Record, Object, Class, Type, Module, Sequence)) return false;
    if (Record.Role == ERemovedRuntimeAssociationRole::Removed)
        return !Class->NewerVersion && !Class->HasAnyClassFlags(CLASS_NewerVersionExists);
    // The mirror replacement callback or the exact reinstance collection pair grants this role.
    // The new class supplies lineage identity, never the old receiver's resources or lease.
    return Record.Role == ERemovedRuntimeAssociationRole::Replaced && Record.Replacement
        && Record.Replacement != Class && Record.Replacement == Class->NewerVersion
        && Record.ReplacementKey.ResolveObjectPtrEvenIfUnreachable() == Record.Replacement
        && AngelscriptRuntimeGcMirror::IsRetiredRuntimeTypeIdentity(Type, Module, Class);
}

// Ordinary deletion never borrows the terminal field-lifetime certificate. Every AS
// owner and every reflected AS-owned field must independently exclude delayed FinishDestroy.
bool HasOrdinaryRemovedFieldShape(const FRemovedRuntimeAssociation& Record)
{
    if (!Record.Class || !Record.Type
        || Record.ClassKey.ResolveObjectPtrEvenIfUnreachable() != Record.Class
        || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Record.Type)
        || Record.Type->GetUserData() != Record.Class || Record.Type->GetEngine() != Record.Engine
        || Record.Type->GetModule() != Record.Module) return false;
    auto* Type = CastToObjectType(static_cast<asCTypeInfo*>(Record.Type));
    TSet<const UClass*> Owners;
    for (UClass* Current = Record.Class; Current; Current = Current->GetSuperClass())
    {
        auto* Owner = Cast<UASClass>(Current);
        // A native UE superclass owns its own ordinary fields. A remaining script
        // cookie cannot be silently treated as native when the AS owner chain ends.
        if (!Owner) return !Type || !(Type->flags & asOBJ_SCRIPT_OBJECT);
        if (!Type || Owners.Num() == 64 || Owners.Contains(Owner)
            || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Type)
            || Type->GetUserData() != Owner || Owner->bLayoutChanging
            || !Owner->UeDelayedDestroyFields.IsEmpty()) return false;
        Owners.Add(Owner);
        for (asUINT Index = 0; Index != Type->localProperties.GetLength(); ++Index)
            if (!Type->localProperties[Index] || Type->localProperties[Index]->bUeDelayedDestroyOwner) return false;
        TSet<const FProperty*> Seen;
        for (const FProperty* Property = Owner->PropertyLink; Property; Property = Property->PropertyLinkNext)
        {
            if (Seen.Contains(Property)) return false;
            Seen.Add(Property);
            if (Property->GetOwnerClass() != Owner) continue;
            TArray<const FStructProperty*> Encountered;
            if (Property->ContainsFinishDestroy(Encountered)) return false;
        }
        Type = Type->derivedFrom;
    }
    return Type == nullptr;
}

bool ValidateOrdinaryRemovedTuple(const FRemovedRuntimeAssociation& Record, UObject* Object,
    const UASClass* Class, const asITypeInfo* Type, const asIScriptModule* Module, uint64 Sequence)
{
    // This is a separate attached/detached ordinary identity check. Terminal identity stays unchanged.
    if (GExitPurge || !IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread()
        || ActiveTerminalOperation || ActiveRuntimeTerminalPlan || TerminalPlans.Contains(Record.ObjectKey)
        || !Object || !Class || !Type || !Module || !Sequence
        || Record.Object != Object || Record.Class != Class || Record.Type != Type
        || Record.Module != Module || Record.Sequence != Sequence
        || !Record.LifecycleGeneration || Record.LifecycleGeneration != UASClass::RuntimeLifecycleGeneration
        || Record.ObjectKey.ResolveObjectPtrEvenIfUnreachable() != Object
        || Record.ClassKey.ResolveObjectPtrEvenIfUnreachable() != Class
        || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Type)) return false;
    if (Record.Engine != FAngelscriptManager::Get().GetScriptEngine() || Type->GetEngine() != Record.Engine
        || Type->GetModule() != Module || Type->GetUserData() != Class
        || UASClass::GetFirstASClass(Object->GetClass()) != Class || Object->IsTemplate()
        || Class->bLayoutChanging || Class->GetPropertiesSize() != Record.PropertiesSize
        // Zero is compared as an identity value; it is never terminal qualification.
        || Cuviss::TerminalAdmission::ReadRetainedSourceGeneration(Record.Type) != Record.SourceGeneration
        || !HasOrdinaryRemovedFieldShape(Record)) return false;
    if (Class->ScriptTypePtr == Type)
        return Record.Role == ERemovedRuntimeAssociationRole::Removed && !Record.Replacement
            && !Class->NewerVersion && !Class->HasAnyClassFlags(CLASS_NewerVersionExists)
            && bPreReinstanceCollection && IsReinstanceCollectionCurrent(ReinstanceCollectionEpoch)
            && Record.RemovalCompilationEpoch == TerminalCompilationEpoch
            && Record.RemovalCompilationThread == TerminalCompilationThread;
    return !Class->ScriptTypePtr
        && ValidateRemovedRuntimeAssociation(Record, Object, Class, Type, Module, Sequence);
}

bool CaptureOrdinaryRemovedRuntimeReplacement(const FRemovedRuntimeAssociation& Record, UObject* Object,
    asITypeInfo* Type, asIScriptModule* Module, uint64 Sequence, FObjectKey& OutReplacementKey)
{
    // The old detached tuple and current live replacement declaration are both required.
    if (!ValidateRemovedRuntimeTuple(Record, Object, Record.Class, Type, Module, Sequence)
        || Record.Role != ERemovedRuntimeAssociationRole::Removed
        || !AngelscriptRuntimeGcMirror::IsRetiredRuntimeTypeIdentity(Type, Module, Record.Class)
        || Cuviss::TerminalAdmission::ReadRetainedSourceGeneration(Type) != Record.SourceGeneration
        || !HasOrdinaryRemovedFieldShape(Record) || Record.bOrdinaryAdmitted) return false;
    {
        FScopeLock Lock(&TerminalCompilationMutex);
        if (!bTerminalCompilationInProgress || bTerminalInitialCompilation || TerminalPlanReservations
            || TerminalCompilationThread != FPlatformTLS::GetCurrentThreadId()) return false;
    }
    UASClass* Newer = Record.Class->NewerVersion;
    if (!Newer || Newer == Record.Class) return false;
    TSharedPtr<FAngelscriptModuleDesc> ActiveModule;
    const auto Declaration = FAngelscriptManager::Get().GetClass(ANSI_TO_TCHAR(Type->GetName()), &ActiveModule);
    if (!Declaration || !ActiveModule || Declaration->Class != Newer || !Declaration->ScriptType
        || Declaration->ScriptType == Type
        || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Declaration->ScriptType)) return false;
    const auto* NewType = Declaration->ScriptType;
    if (NewType->GetUserData() != Newer || NewType->GetEngine() != Record.Engine
        || NewType->GetModule() != ActiveModule->ScriptModule || NewType->GetModule() == Module
        || FCStringAnsi::Strcmp(NewType->GetNamespace(), Type->GetNamespace()) != 0) return false;
    for (TObjectIterator<UASClass> It; It; ++It)
    {
        if (*It != Newer) continue;
        if (Newer->GetClass() != UASClass::StaticClass() || Newer->bLayoutChanging
            || Newer->IsUnreachable() || Newer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
            || Newer->ScriptTypePtr != NewType || Newer->NewerVersion) return false;
        OutReplacementKey = FObjectKey(Newer);
        return true;
    }
    return false;
}

bool AdmitOrdinaryRemovedRuntimeReceiver(FRemovedRuntimeAssociation& Record,
    const SoloASBridge::FRuntimeDestroyPreclaim& Source, UASClass* Owner,
    asITypeInfo* Type, asIScriptModule* Module, uint64 Sequence, bool bOwnReservation)
{
    if (Record.bOrdinaryAdmitted || Record.bOrdinaryConsumed || !Source.Object->IsUnreachable()
        || !ValidateOrdinaryRemovedTuple(Record, Source.Object, Owner, Type, Module, Sequence)) return false;
    uint64 CompilationEpoch = 0, CollectionEpoch = 0;
    {
        FScopeLock Lock(&TerminalCompilationMutex);
        if (TerminalPlanReservations != uint64(bOwnReservation) || (bTerminalCompilationInProgress
            && !IsReinstanceCollectionCurrent(ReinstanceCollectionEpoch))) return false;
        CompilationEpoch = bTerminalCompilationInProgress ? TerminalCompilationEpoch : 0;
        CollectionEpoch = ReinstanceCollectionEpoch;
    }
    {
        FScopeLock Lock(&AngelscriptRuntimeGcMirror::RuntimeTrackedObjectsMutex);
        auto* Entry = AngelscriptRuntimeGcMirror::RuntimeTrackedObjects.Find(Source.Key);
        if (!Entry || Entry->Key != Record.ObjectKey || Entry->ObjectIdentity != Source.Object
            || Entry->SavedTypeCookie != Type || Entry->RetainedModule != Module
            || Entry->TrackSequence != Sequence || Entry->TerminalPlanId
            || Entry->State != AngelscriptRuntimeGcMirror::ERuntimeMirrorState::Pending
            || Entry->bOrdinaryRemovedAdmissionRequired) return false;
        Entry->bOrdinaryRemovedAdmissionRequired = true;
        Entry->OrdinaryRemovedAdmissionCollectionEpoch = CollectionEpoch;
    }
    Record.OrdinaryCompilationEpoch = CompilationEpoch;
    Record.OrdinaryCollectionEpoch = CollectionEpoch;
    Record.bOrdinaryAdmitted = true;
    return true;
}

bool CaptureRemovedRuntimeReplacement(const FRemovedRuntimeAssociation& Record, UObject* Object,
    asITypeInfo* Type, asIScriptModule* Module, uint64 Sequence, FObjectKey& OutReplacementKey)
{
    if (!ValidateRemovedRuntimeTuple(Record, Object, Record.Class, Type, Module, Sequence)
        || Record.Role != ERemovedRuntimeAssociationRole::Removed
        || !AngelscriptRuntimeGcMirror::IsRetiredRuntimeTypeIdentity(Type, Module, Record.Class)
        || !Record.SourceGeneration
        || Cuviss::TerminalAdmission::ReadRetainedSourceGeneration(Type) != Record.SourceGeneration) return false;
    {
        FScopeLock Lock(&TerminalCompilationMutex);
        if (!bTerminalCompilationInProgress || TerminalPlanReservations != 0) return false;
    }
    UASClass* Newer = Record.Class->NewerVersion;
    if (!Newer || Newer == Record.Class) return false;
    TSharedPtr<FAngelscriptModuleDesc> ActiveModule;
    const auto Declaration = FAngelscriptManager::Get().GetClass(ANSI_TO_TCHAR(Type->GetName()), &ActiveModule);
    // Name lookup selects a declaration; it grants nothing without the exact active tuple and live UObject.
    if (!Declaration || !ActiveModule || Declaration->Class != Newer || !Declaration->ScriptType
        || Declaration->ScriptType == Type
        || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Declaration->ScriptType)) return false;
    const auto* NewType = Declaration->ScriptType;
    if (NewType->GetUserData() != Newer || NewType->GetEngine() != Record.Engine
        || NewType->GetModule() != ActiveModule->ScriptModule || NewType->GetModule() == Module
        || FCStringAnsi::Strcmp(NewType->GetNamespace(), Type->GetNamespace()) != 0) return false;
    // NewerVersion is a raw pointer. Do not construct a weak key or read class metadata until
    // UObject enumeration proves it is a live UASClass, independently of type userdata.
    for (TObjectIterator<UASClass> It; It; ++It)
    {
        if (*It != Newer) continue;
        if (Newer->GetClass() != UASClass::StaticClass() || Newer->bLayoutChanging
            || Newer->IsUnreachable() || Newer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
            || Newer->ScriptTypePtr != NewType || Newer->NewerVersion) return false;
        OutReplacementKey = FObjectKey(Newer);
        return true;
    }
    return false;
}

bool SameRemovedRuntimeAssociation(const FRemovedRuntimeAssociation& A, const FRemovedRuntimeAssociation& B)
{
    return A.ObjectKey == B.ObjectKey && A.ClassKey == B.ClassKey && A.ReplacementKey == B.ReplacementKey
        && A.Object == B.Object && A.Class == B.Class && A.Replacement == B.Replacement && A.Role == B.Role
        && A.Type == B.Type && A.Module == B.Module && A.Engine == B.Engine && A.Sequence == B.Sequence
        && A.LifecycleGeneration == B.LifecycleGeneration && A.SourceGeneration == B.SourceGeneration
        && A.PropertiesSize == B.PropertiesSize
        && A.RemovalCompilationEpoch == B.RemovalCompilationEpoch
        && A.RemovalCompilationThread == B.RemovalCompilationThread;
}

bool ValidateReinstanceReceiver(const FRemovedRuntimeAssociation& Record)
{
    const auto* Receiver = ReinstanceCollectionReceivers.Find(Record.ObjectKey);
    if (!Receiver || !SameRemovedRuntimeAssociation(Receiver->Association, Record)
        || Record.ReplacementKey.ResolveObjectPtrEvenIfUnreachable() != Record.Replacement
        || !Record.Replacement || !Receiver->ReplacementType
        || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Receiver->ReplacementType)) return false;
    const auto* NewType = Receiver->ReplacementType;
    return Record.Replacement->ScriptTypePtr == NewType && !Record.Replacement->NewerVersion
        && !Record.Replacement->bLayoutChanging && !Record.Replacement->IsUnreachable()
        && !Record.Replacement->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
        && NewType->GetUserData() == Record.Replacement && NewType->GetEngine() == Record.Engine
        && NewType->GetModule() == Receiver->ReplacementModule && Receiver->ReplacementModule != Record.Module;
}

bool BorrowReinstanceReservation(FTerminalPlanReservation& Reservation, const FRemovedRuntimeAssociation& Record)
{
    auto* Receiver = ReinstanceCollectionReceivers.Find(Record.ObjectKey);
    if (!Receiver || Receiver->bConsumed || !ValidateReinstanceReceiver(Record)
        || !IsReinstanceCollectionCurrent(ReinstanceCollectionEpoch)
        || Cuviss::TerminalAdmission::ReadRetainedSourceGeneration(Record.Type) != Record.SourceGeneration
        || !Reservation.BorrowForReinstance(ReinstanceCollectionEpoch)) return false;
    // A failed preparation cannot renew this permission or replay a partially claimed receiver.
    Receiver->bConsumed = true;
    return true;
}

bool ValidateTerminalOwnerAssociation(const FTerminalDestroyPlan& Plan, const UASClass* Owner,
    asITypeInfo* Type, bool bCheckSource = true)
{
    if (!Owner || !Type || !Plan.ModulePin || !Plan.CompilationReservation
        || !Plan.CompilationReservation->IsValid()) return false;
    if (Plan.RemovedAssociation.IsSet())
    {
        const auto& Record = Plan.RemovedAssociation.GetValue();
        return (!Plan.CompilationReservation->GetBorrowedEpoch() || ValidateReinstanceReceiver(Record))
            && Owner == Plan.Class && Type == Plan.Token.TypeCookie && Record.SourceGeneration
            && ValidateRemovedRuntimeAssociation(Record, Plan.Token.Object, Owner, Type,
                Plan.ModulePin, Plan.Token.TrackSequence)
            && (!bCheckSource || Cuviss::TerminalAdmission::ReadRetainedSourceGeneration(Type) == Record.SourceGeneration);
    }
    return AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Type)
        && Owner->ScriptTypePtr == Type && Type->GetUserData() == Owner && Type->GetModule() == Plan.ModulePin;
}

const FProperty* TerminalOwnedExecutionNode(const FTerminalDestroyPlan& Plan, const FProperty* Declaration)
{
    return Plan.NativeTailHead && Plan.NativeTailChain && Declaration == Plan.NativeTailChain->Owned
        ? Plan.NativeTailHead->OwnedProxy.Get() : Declaration;
}

UASClass* TerminalFieldDeclarationOwner(const FTerminalDestroyPlan& Plan)
{
    return Plan.NativeTailHead ? Plan.NativeTailChain->Class : Plan.Class;
}

asCObjectType* TerminalDeclarationType(const FTerminalDestroyPlan& Plan)
{
    auto* Owner = TerminalFieldDeclarationOwner(Plan);
    return Plan.NativeTailHead ? (Plan.NativeTailChain->bAutomatic ? Plan.NativeTailChain->DeclarationType
        : CastToObjectType(static_cast<asCTypeInfo*>(static_cast<asITypeInfo*>(Owner->ScriptTypePtr))))
        : CastToObjectType(static_cast<asCTypeInfo*>(Plan.Token.TypeCookie));
}

bool ValidateTerminalShapeDeclaration(const FTerminalDestroyPlan& Plan,
    const Cuviss::TerminalAdmission::FReceiverShape& Shape)
{
    auto* Owner = TerminalFieldDeclarationOwner(Plan);
    auto* Type = TerminalDeclarationType(Plan);
    const bool Allow = Plan.NativeTailHead && Plan.Class != Owner;
    return Shape.Type == CastToObjectType(static_cast<asCTypeInfo*>(Plan.Token.TypeCookie))
        && Shape.FieldOwnerType == Type && Shape.bAllowDefaultParentFrame == Allow
        && (!Plan.NativeTailHead || (IsCurrentNativeTailChain(Plan.NativeTailChain, Plan.Class)
            && ValidateTerminalNativeTailChain(*Plan.NativeTailChain)
            && Plan.NativeTailHead->Class == Plan.Class && !Plan.bDevFacade
            && (!Plan.RemovedAssociation.IsSet() || IsAutomaticTerminalPlan(Plan))));
}

bool ValidateTerminalOwners(const FTerminalDestroyPlan& Plan, bool bValidateCode = true)
{
    UClass* CurrentClass = Plan.Class;
    auto* CurrentType = CastToObjectType(static_cast<asCTypeInfo*>(Plan.Token.TypeCookie));
    int32 Registered = 0;
    for (const auto& Code : Plan.Owners)
    {
        if (CurrentClass != Code.Owner || CurrentType != Code.Cookie
            || !SameTerminalItem(Code.Index, Code.Serial, Code.Owner)
            || Code.Key.ResolveObjectPtrEvenIfUnreachable() != Code.Owner
            || Code.Owner->bLayoutChanging || !ValidateTerminalOwnerAssociation(Plan, Code.Owner, Code.Cookie)
            || Code.Cookie->GetUserData() != Code.Owner || Code.Cookie->GetModule() != Plan.ModulePin
            || FindTerminalDestructor(Code.Cookie) != Code.Destructor) return false;
        if (Code.Destructor && (Code.Destructor->GetObjectType() != Code.Cookie
            || Code.Destructor->GetModule() != Plan.ModulePin)) return false;
        if (Code.Destructor && bValidateCode)
        {
            asUINT Length = 0;
            const asDWORD* Bytes = Code.Destructor->GetByteCode(&Length);
            if (!Bytes || Bytes != Code.ByteCode || Length != Code.Length
                || Code.Destructor->GetObjectType() != Code.Cookie || Code.Destructor->GetModule() != Plan.ModulePin
                || HashTerminalCode(Bytes, Length) != Code.Hash) return false;
        }
        Registered += Code.Owner->UeDelayedDestroyFields.Num();
        int32 Marked = 0;
        for (asUINT Index = 0; Index != CurrentType->localProperties.GetLength(); ++Index)
            Marked += CurrentType->localProperties[Index]->bUeDelayedDestroyOwner ? 1 : 0;
        if (Marked != Code.Owner->UeDelayedDestroyFields.Num()) return false;
        CurrentClass = CurrentClass->GetSuperClass();
        CurrentType = CurrentType->derivedFrom;
    }
    if (Cast<UASClass>(CurrentClass) || Registered != Plan.Fields.Num()) return false;
    for (const auto& Field : Plan.Fields)
    {
        const auto* Property = Field.Property;
        if (!Property || Property->GetFName() != Field.Name || Property->GetOwnerStruct() != Field.Owner
            || !ValidateTerminalOwnerAssociation(Plan, Field.Owner, Field.OwnerCookie)
            || !Field.Owner->UeDelayedDestroyFields.Contains(Property) || Property->Struct != FInstancedStruct::StaticStruct()
            || Property->ArrayDim != 1 || Property->GetOffset_ForInternal() != Field.Offset
            || Property->GetElementSize() != sizeof(FInstancedStruct) || Property->GetSize() != sizeof(FInstancedStruct)
            || uint64(Property->GetPropertyFlags()) != Field.Flags
            || Property->ContainerPtrToValuePtr<void>(Plan.Token.Object) != Field.Storage
            || !Field.Storage || reinterpret_cast<UPTRINT>(Field.Storage) % alignof(FInstancedStruct) != 0
            || Field.Offset < Field.Owner->ScriptPropertyOffset
            || Field.Offset > Plan.Class->GetPropertiesSize() - int32(sizeof(FInstancedStruct))
            || Field.Offset > int32(static_cast<asITypeInfo*>(Plan.Token.TypeCookie)->GetSize()) - int32(sizeof(FInstancedStruct))) return false;
        auto* Type = CastToObjectType(static_cast<asCTypeInfo*>(Field.OwnerCookie));
        int32 Matches = 0;
        for (asUINT Index = 0; Index != Type->localProperties.GetLength(); ++Index)
        {
            const auto* Member = Type->localProperties[Index];
            if (Member != Field.Member) continue;
            if (!Member->bUeDelayedDestroyOwner || Member->byteOffset != Field.Offset
                || !Member->type.IsObject() || Member->type.IsReference() || Member->type.IsObjectHandle()
                || Member->type.GetSizeInMemoryBytes() != sizeof(FInstancedStruct)
                || Member->type.GetTypeInfo() != Field.ValueCookie) return false;
            ++Matches;
        }
        if (Matches != 1 || Field.ChainIndex >= Plan.ChainLength
            || Plan.Chain[Field.ChainIndex] != TerminalOwnedExecutionNode(Plan, Property)) return false;
        if (Plan.NativeTailHead)
        {
            const auto* Actual = CastToObjectType(static_cast<asCTypeInfo*>(Plan.Token.TypeCookie));
            uint32 InheritedMatches = 0;
            for (asUINT Index = 0; Index != Actual->properties.GetLength(); ++Index)
                if (Actual->properties[Index] == Field.Member) ++InheritedMatches;
            if (InheritedMatches != 1 || Field.Owner != Plan.NativeTailChain->Class) return false;
        }
    }
    return Plan.Fields.Num() >= 1 && uint32(Plan.Fields.Num()) <= Plan.ChainLength;
}

bool CaptureTerminalOwnedFields(FTerminalDestroyPlan& Plan)
{
    auto* Type = CastToObjectType(static_cast<asCTypeInfo*>(Plan.Token.TypeCookie));
    TSet<const UClass*> Seen;
    for (UClass* Current = Plan.Class; Current; Current = Current->GetSuperClass())
    {
        UASClass* Owner = Cast<UASClass>(Current);
        if (!Owner) break;
        if (!Type || Seen.Contains(Current) || Plan.Owners.Num() == 64
            || !ValidateTerminalOwnerAssociation(Plan, Owner, Type)) return false;
        Seen.Add(Current);
        FTerminalOwnerCode Code;
        Code.Owner = Owner;
        Code.Key = FObjectKey(Owner);
        Code.Index = GUObjectArray.ObjectToIndex(Owner);
        Code.Serial = GUObjectArray.GetSerialNumber(Code.Index);
        Code.Cookie = Type;
        Code.Destructor = FindTerminalDestructor(Type);
        if (Code.Destructor)
        {
            Code.ByteCode = Code.Destructor->GetByteCode(&Code.Length);
            if (Code.ByteCode && Code.Length && Code.Length <= 1048576)
                Code.Hash = HashTerminalCode(Code.ByteCode, Code.Length);
        }
        Plan.Owners.Add(Code);
        for (const auto* Property : Owner->UeDelayedDestroyFields)
        {
            if (uint32(Plan.Fields.Num()) == Plan.ChainLength) return false;
            FTerminalOwnedField Field;
            Field.Property = CastField<FStructProperty>(Property);
            if (!Field.Property) return false;
            Field.Name = Property->GetFName();
            Field.Owner = Owner;
            Field.OwnerCookie = Type;
            Field.Offset = Property->GetOffset_ForInternal();
            if (Field.Offset < Owner->ScriptPropertyOffset
                || Field.Offset > Plan.Class->GetPropertiesSize() - int32(sizeof(FInstancedStruct))
                || Field.Offset > int32(static_cast<asITypeInfo*>(Plan.Token.TypeCookie)->GetSize()) - int32(sizeof(FInstancedStruct))) return false;
            Field.Storage = Property->ContainerPtrToValuePtr<void>(Plan.Token.Object);
            Field.Flags = uint64(Property->GetPropertyFlags());
            for (asUINT MemberIndex = 0; MemberIndex != Type->localProperties.GetLength(); ++MemberIndex)
            {
                const auto* Member = Type->localProperties[MemberIndex];
                if (FName(ANSI_TO_TCHAR(Member->name.AddressOf())) != Field.Name) continue;
                if (Field.Member || !Member->bUeDelayedDestroyOwner || Member->byteOffset != Field.Offset
                    || FAngelscriptTypeUsage::FromDataType(Member->type).GetUnrealStruct() != FInstancedStruct::StaticStruct()) return false;
                Field.Member = Member;
                Field.ValueCookie = Member->type.GetTypeInfo();
            }
            TArray<const FStructProperty*> Encountered;
            if (!Field.Member || !Field.ValueCookie || !Property->ContainsFinishDestroy(Encountered)) return false;
            uint32 Matches = 0;
            for (uint32 Index = 0; Index != Plan.ChainLength; ++Index)
                if (Plan.Chain[Index] == TerminalOwnedExecutionNode(Plan, Property)) { Field.ChainIndex = Index; ++Matches; }
            if (Matches != 1) return false;
            for (const auto& Other : Plan.Fields)
                if (Other.Property == Property || Other.Storage == Field.Storage
                    || FMath::Abs(Other.Offset - Field.Offset) < int32(sizeof(FInstancedStruct))) return false;
            Plan.Fields.Add(Field);
        }
        Type = Type->derivedFrom;
    }
    Plan.Fields.Sort([](const FTerminalOwnedField& A, const FTerminalOwnedField& B) { return A.ChainIndex < B.ChainIndex; });
    return ValidateTerminalOwners(Plan, false) && !Plan.Fields.IsEmpty() && Plan.Fields.Last().Property == Plan.Property;
}

bool ValidateTerminalInt32SlotMetadata(const FTerminalDestroyPlan& Plan)
{
    using namespace Cuviss::TerminalAdmission;
    const auto& Slot = Plan.Int32Slot;
    UASClass* Declaration = TerminalFieldDeclarationOwner(Plan);
    if (Plan.ActualReceiverClass != Plan.Class && !ValidateTerminalActualReceiverMetadata(Plan)) return false;
    if (Slot.bRuntimeOwnedOriginal && (!Slot.FinalProviderEpoch || !HasRuntimeOwnedTerminalScalar(Plan)
        || Plan.ActualReceiverClass != Plan.Class || Plan.Class->GetPropertiesStartOffset() != 0)) return false;
    if (!Slot.Property)
        return Slot == FInt32Slot() && Plan.Int32Phase == EInt32SlotPhase::Absent
            && !Plan.FreshInt32 && !Plan.FreshInt32Epoch;
    if (!SameTerminalItem(Plan.ReceiverIndex, Plan.ReceiverSerial, Plan.Token.Object)
        || !SameTerminalItem(Plan.ClassIndex, Plan.ClassSerial, Plan.Class)
        || Plan.Token.Key.ResolveObjectPtrEvenIfUnreachable() != Plan.Token.Object
        || Plan.ClassKey.ResolveObjectPtrEvenIfUnreachable() != Plan.Class
        || Slot.Owner != Declaration || !Plan.ModulePin || !Slot.Epoch
        || !ValidateTerminalOwnerAssociation(Plan, Plan.Class, static_cast<asITypeInfo*>(Plan.Token.TypeCookie), false)) return false;
    const auto* ActualType = CastToObjectType(static_cast<asCTypeInfo*>(Plan.Token.TypeCookie));
    const auto* Type = TerminalDeclarationType(Plan);
    if (!ActualType || !Type || Type->derivedFrom || ActualType->size != Slot.TypeSize || Type->typeId != Slot.TypeId
        || (ActualType != Type && (!Plan.NativeTailHead || ActualType->derivedFrom != Type || ActualType->localProperties.GetLength()
            || ActualType->size != Type->size || ActualType->module != Type->module))
        || !ValidateTerminalOwnerAssociation(Plan, Declaration, const_cast<asCObjectType*>(Type), false)
        || Type->module != Plan.ModulePin || Plan.Class->bLayoutChanging
        || Plan.Class->GetPropertiesSize() != Slot.PropertiesSize || Plan.PropertiesSize != Slot.PropertiesSize
        || Declaration->ScriptPropertyOffset != Slot.ScriptOffset || Slot.MemberIndex < 0
        || Type->localProperties.GetLength() > 1024 || Type->properties.GetLength() > 1024
        || static_cast<uint32>(Slot.MemberIndex) >= Type->localProperties.GetLength()
        || Type->localProperties[Slot.MemberIndex] != Slot.Member
        || !IsTerminalInt32StorageRange(Slot.Offset, Slot.ScriptOffset, Slot.PropertiesSize, Slot.TypeSize,
            reinterpret_cast<UPTRINT>(Plan.Token.Object), reinterpret_cast<UPTRINT>(Slot.Storage))) return false;
    // First prove membership by pointer. Do not dereference a replaced property.
    uint32 Seen = 0, Matches = 0;
    for (const FProperty* Property = Plan.Class->PropertyLink; Property; Property = Property->PropertyLinkNext)
    {
        if (++Seen > 1024) return false;
        if (Property == Slot.Property) ++Matches;
        else if (Property->GetSize() <= 0 || Property->GetOffset_ForInternal() < 0
            || TerminalStorageRangesOverlap(Slot.Offset, sizeof(int32), Property->GetOffset_ForInternal(), Property->GetSize()))
            return false;
    }
    if (Matches != 1) return false;
    const auto* Property = Slot.Property;
    if (Property->GetClass() != FIntProperty::StaticClass() || Property->GetOwnerStruct() != Slot.Owner
        || Property->GetFName() != Slot.Name || Property->ArrayDim != 1
        || Property->GetSize() != sizeof(int32) || Property->GetElementSize() != sizeof(int32)
        || Property->GetOffset_ForInternal() != Slot.Offset || uint64(Property->GetPropertyFlags()) != Slot.PropertyFlags
        || Declaration->UeDelayedDestroyFields.Contains(Property)) return false;
    for (uint32 Index = 0; Index < Plan.ChainLength; ++Index)
        if (Plan.Chain[Index] == Property) return false;
    Matches = 0;
    for (asUINT Index = 0; Index < ActualType->properties.GetLength(); ++Index)
    {
        const auto* Member = ActualType->properties[Index];
        if (!Member) return false;
        if (Member == Slot.Member) ++Matches;
        else if (Member->byteOffset < 0 || Member->type.GetSizeInMemoryBytes() <= 0
            || TerminalStorageRangesOverlap(Slot.Offset, sizeof(int32), Member->byteOffset, Member->type.GetSizeInMemoryBytes()))
            return false;
    }
    if (Matches != 1 || !Slot.Member) return false;
    const auto* Member = Slot.Member;
    return Member->type == asCDataType::CreatePrimitive(ttInt, false) && Member->type.IsPrimitive()
        && !Member->type.IsReference() && !Member->type.IsObjectHandle() && Member->type.GetSizeInMemoryBytes() == sizeof(int32)
        && Member->byteOffset == Slot.Offset
        && !Member->compositeOffset && !Member->isCompositeIndirect && !Member->bUeDelayedDestroyOwner
        && !Member->isAppBindProperty;
}

bool ValidateTerminalPlan(const FTerminalDestroyPlan& Plan, bool bAfterOriginal);

bool ValidateTerminalFinalProviderBeforeCommit(const FTerminalDestroyPlan& Plan)
{
    const auto* Provider = Plan.FinalInt32Provider;
    if (!bRequireTerminalFinalInt32 && !IsAutomaticTerminalPlan(Plan)) return !Provider;
    if (!Plan.Int32Slot.Property) return !Provider;
    if (!Provider || Provider->bWriting || Provider->bPublished || Provider->bEnded || Provider->PlanId != Plan.Token.PlanId
        || Provider->Ticket.Storage != Plan.Int32Slot.Storage || Provider->Ticket.SlotEpoch != Plan.Int32Slot.Epoch
        || Provider->Registration.Receiver != Plan.Token.Object || Provider->Registration.Scalar != Plan.Int32Slot.Property
        || Provider->Registration.Owned != Plan.Property || !Provider->Registration.ProviderIdentity
        || Provider->Registration.ProviderEpoch != Provider->Ticket.ProviderEpoch
        || Plan.Int32Slot.FinalProviderEpoch != Provider->Ticket.ProviderEpoch
        || Plan.Int32Slot.bRuntimeOwnedOriginal != Provider->Registration.bRuntimeOwnedOriginal
        || Provider->Registration.bRuntimeOwnedOriginal != HasRuntimeOwnedTerminalScalar(Plan)) return false;
    // The wrapper and payload are still alive at this BEFORE-commit fence.
    const auto* Wrapper = static_cast<const FInstancedStruct*>(Plan.Storage);
    return Wrapper->GetScriptStruct() == Provider->Registration.PayloadType
        && Wrapper->GetMemory() == Provider->Registration.Payload
        && *Provider->Registration.DeclaredEpoch == Provider->Ticket.ProviderEpoch
        && *Provider->Registration.DeclaredProvider == Provider->Registration.ProviderIdentity;
}

bool CaptureTerminalInt32Slot(FTerminalDestroyPlan& Plan)
{
    using namespace Cuviss::TerminalAdmission;
    if (Plan.bDevFacade) return true;
    if (!Plan.ByteCode || !Plan.ByteCodeLength || Plan.ByteCodeLength > 4096) return false;
    auto* ActualType = CastToObjectType(static_cast<asCTypeInfo*>(Plan.Token.TypeCookie));
    UASClass* Declaration = TerminalFieldDeclarationOwner(Plan);
    auto* Type = TerminalDeclarationType(Plan);
    if (!ActualType || !Type || Type->derivedFrom || Type->localProperties.GetLength() > 1024
        || (ActualType != Type && (!Plan.NativeTailHead || ActualType->derivedFrom != Type || ActualType->localProperties.GetLength()
            || ActualType->size != Type->size))) return false;
    asUINT BodyLength = Plan.ByteCodeLength;
    const asDWORD* BodyCode = Plan.ByteCode;
    if (Plan.NativeTailHead)
    {
        const auto* BodyOwner = Plan.Owners.FindByPredicate([&](const FTerminalOwnerCode& Owner) { return Owner.Owner == Declaration; });
        if (!BodyOwner || BodyOwner->Cookie != Type || !BodyOwner->Destructor) return false;
        BodyCode = BodyOwner->ByteCode; BodyLength = BodyOwner->Length;
        if (!BodyCode || !BodyLength || BodyLength > 4096 || HashTerminalCode(BodyCode, BodyLength) != BodyOwner->Hash) return false;
    }
    int32 Offset = INDEX_NONE;
    for (uint32 PC = 0; PC < BodyLength;)
    {
        const auto* Instruction = BodyCode + PC;
        const uint8 Opcode = static_cast<uint8>(*Instruction);
        if (Opcode >= asBC_MAXBYTECODE || asBCInfo[Opcode].bc != Opcode) return false;
        const int Format = asBCInfo[Opcode].type;
        if (Format <= asBCTYPE_INFO || Format >= UE_ARRAY_COUNT(asBCTypeSize)) return false;
        const int Width = asBCTypeSize[Format];
        if (Width <= 0 || static_cast<uint32>(Width) > BodyLength - PC) return false;
        if (Opcode == asBC_LoadThisR)
        {
            const int32 Current = asBC_SWORDARG0(Instruction);
            if (Current <= 0 || asBC_DWORDARG(Instruction) != static_cast<uint32>(Type->typeId)
                || (Offset != INDEX_NONE && Offset != Current)) return false;
            Offset = Current;
        }
        PC += Width;
    }
    if (Offset == INDEX_NONE) return true;
    auto& Slot = Plan.Int32Slot;
    for (asUINT Index = 0; Index < Type->localProperties.GetLength(); ++Index)
    {
        const auto* Member = Type->localProperties[Index];
        if (!Member || Member->byteOffset != Offset) continue;
        if (Slot.Member || Member->type != asCDataType::CreatePrimitive(ttInt, false)
            || Member->type.IsReference() || Member->type.IsObjectHandle() || Member->compositeOffset
            || Member->isCompositeIndirect || Member->bUeDelayedDestroyOwner || Member->isAppBindProperty) return false;
        Slot.Member = Member;
        Slot.MemberIndex = Index;
        Slot.Name = FName(ANSI_TO_TCHAR(Member->name.AddressOf()));
    }
    if (!Slot.Member) return false;
    uint32 Seen = 0;
    for (const FProperty* Property = Plan.Class->PropertyLink; Property; Property = Property->PropertyLinkNext)
    {
        if (++Seen > 1024) return false;
        if (Property->GetFName() != Slot.Name || Property->GetOffset_ForInternal() != Offset) continue;
        if (Slot.Property || Property->GetOwnerStruct() != Declaration || Property->GetClass() != FIntProperty::StaticClass()) return false;
        Slot.Property = CastField<FIntProperty>(Property);
    }
    if (!Slot.Property || NextTerminalInt32Epoch == MAX_uint64) return false;
    Slot.Owner = Declaration;
    Slot.Offset = Offset;
    Slot.ScriptOffset = Declaration->ScriptPropertyOffset;
    Slot.PropertiesSize = Plan.PropertiesSize;
    Slot.TypeSize = ActualType->size;
    Slot.TypeId = Type->typeId;
    Slot.Storage = Slot.Property->ContainerPtrToValuePtr<void>(Plan.Token.Object);
    Slot.PropertyFlags = uint64(Slot.Property->GetPropertyFlags());
    if (IsAutomaticTerminalPlan(Plan))
    {
        if (LastTerminalFinalProviderEpoch == MAX_uint64 || TerminalFinalInt32Order == MAX_uint64) return false;
        auto Provider = MakeUnique<FTerminalFinalInt32Provider>();
        auto& R = Provider->Registration;
        R.Receiver = Plan.Token.Object; R.Scalar = Slot.Property; R.Owned = Plan.Property;
        const auto* Wrapper = static_cast<const FInstancedStruct*>(Plan.Storage);
        R.PayloadType = Wrapper->GetScriptStruct(); R.Payload = Wrapper->GetMemory();
        R.ProviderIdentity = Provider.Get(); R.DeclaredEpoch = &R.ProviderEpoch; R.DeclaredProvider = &R.ProviderIdentity;
        R.ProviderEpoch = ++LastTerminalFinalProviderEpoch; R.bRuntimeOwnedOriginal = true;
        Provider->Ticket.Storage = static_cast<int32*>(Slot.Storage);
        Provider->Ticket.ProviderEpoch = R.ProviderEpoch; Provider->Ticket.SlotEpoch = ++NextTerminalInt32Epoch;
        Provider->ReceiverKey = Plan.Token.Key; Provider->PlanId = Plan.Token.PlanId;
        Provider->ConstructOrder = ++TerminalFinalInt32Order;
        Slot.Epoch = Provider->Ticket.SlotEpoch; Slot.FinalProviderEpoch = R.ProviderEpoch;
        Slot.bRuntimeOwnedOriginal = true;
        Plan.FinalInt32Provider = Provider.Get();
        TerminalFinalInt32Providers.Add(R.ProviderEpoch, MoveTemp(Provider));
    }
    else if (bRequireTerminalFinalInt32)
    {
        for (const auto& Pair : TerminalFinalInt32Providers)
        {
            auto* Provider = Pair.Value.Get();
            if (Provider->Registration.Receiver != Plan.Token.Object) continue;
            if (Plan.FinalInt32Provider || Provider->PlanId || Provider->bEnded || Provider->bPublished
                || Provider->Ticket.Storage != Slot.Storage || Provider->Registration.Scalar != Slot.Property)
                return false;
            Plan.FinalInt32Provider = Provider;
            Provider->PlanId = Plan.Token.PlanId;
            Slot.Epoch = Provider->Ticket.SlotEpoch;
            Slot.FinalProviderEpoch = Provider->Ticket.ProviderEpoch;
            Slot.bRuntimeOwnedOriginal = Provider->Registration.bRuntimeOwnedOriginal;
        }
        if (!Plan.FinalInt32Provider)
        {
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_FINAL_PROVIDER_REJECT object=%p before_commit=1 reason=missing_or_wrong_provider"), Plan.Token.Object);
            return false;
        }
    }
    else Slot.Epoch = ++NextTerminalInt32Epoch;
    Plan.Int32Phase = EInt32SlotPhase::OriginalLive;
    if (!ValidateTerminalFinalProviderBeforeCommit(Plan)) return false;
    if (!Plan.FinalInt32Provider) return ValidateTerminalInt32SlotMetadata(Plan);
    // The original typed read occurs only while the original UObject and scalar
    // lifetime still exist. Capture is never renewed after observable callouts.
    if (!ValidateTerminalPlan(Plan, false)) return false;
    Slot.CapturedValue = Slot.Property->GetPropertyValue(Slot.Storage);
    Slot.CapturedEpoch = Slot.Epoch;
    return true;
}

bool ValidateTerminalOriginalInt32Value(const FTerminalDestroyPlan& Plan)
{
    using namespace Cuviss::TerminalAdmission;
    if (!Plan.Int32Slot.Property || !Plan.FinalInt32Provider) return ValidateTerminalInt32SlotMetadata(Plan);
    // Prove identity and live phase before dereferencing old storage; this fence
    // is never called from the original-field completion path.
    return Plan.Int32Phase == EInt32SlotPhase::OriginalLive
        && IsTerminalCapturedInt32Slot(Plan.Int32Slot) && ValidateTerminalPlan(Plan, false)
        && Plan.Int32Slot.Property->GetPropertyValue(Plan.Int32Slot.Storage) == Plan.Int32Slot.CapturedValue;
}

bool ValidateTerminalPlan(const FTerminalDestroyPlan& Plan, bool bAfterOriginal)
{
    check(IsAngelscriptExecutionWriterHeldByCurrentThread());
    const auto& Token = Plan.Token;
    if (Plan.Phase == ETerminalPlanPhase::Retired || !Token.Object || !Token.TypeCookie
        || !Token.TrackSequence || !Token.ArmEpoch || !Token.PlanId
        || Plan.ThreadId != FPlatformTLS::GetCurrentThreadId() || !Plan.ModulePin
        || !SameTerminalItem(Plan.ReceiverIndex, Plan.ReceiverSerial, Token.Object)
        || !SameTerminalItem(Plan.ClassIndex, Plan.ClassSerial, Plan.Class)
        || Token.Key.ResolveObjectPtrEvenIfUnreachable() != Token.Object
        || Plan.ClassKey.ResolveObjectPtrEvenIfUnreachable() != Plan.Class) return false;
    if (!ValidateTerminalActualReceiverMetadata(Plan) || Plan.Class->GetClass() != UASClass::StaticClass()
        || Plan.Class->bLayoutChanging || Plan.Class->GetPropertiesSize() != Plan.PropertiesSize
        || !ValidateTerminalOwnerAssociation(Plan, Plan.Class, static_cast<asITypeInfo*>(Token.TypeCookie))) return false;
    auto* Type = static_cast<asITypeInfo*>(Token.TypeCookie);
    if (Type->GetUserData() != Plan.Class || Type->GetModule() != Plan.ModulePin
        || Type->GetEngine() != FAngelscriptManager::Get().GetScriptEngine()
        || FindTerminalDestructor(Type) != Plan.Destructor || !Plan.Destructor
        || Plan.Destructor->GetObjectType() != Type || Plan.Destructor->GetModule() != Plan.ModulePin) return false;
    asUINT CodeLength = 0;
    const asDWORD* Code = Plan.Destructor->GetByteCode(&CodeLength);
    if (!Code || Code != Plan.ByteCode || CodeLength != Plan.ByteCodeLength
        || CodeLength == 0 || CodeLength > 1048576 || HashTerminalCode(Code, CodeLength) != Plan.CodeHash) return false;
    const auto* Property = Plan.Property;
    if (!Property || Property->Struct != FInstancedStruct::StaticStruct()
        || Property->ArrayDim != 1 || Property->GetOffset_ForInternal() != Plan.Offset
        || Property->GetSize() != sizeof(FInstancedStruct) || Property->GetElementSize() != sizeof(FInstancedStruct)
        || static_cast<uint64>(Property->GetPropertyFlags()) != Plan.PropertyFlags
        || Property->ContainerPtrToValuePtr<void>(Token.Object) != Plan.Storage
        || !ValidateTerminalOwners(Plan) || !ValidateTerminalInt32SlotMetadata(Plan)) return false;
    uint32 Seen = 0;
    for (const FProperty* Field = Plan.ActualReceiverClass->DestructorLink; Field; Field = Field->DestructorLinkNext)
    {
        if (Seen == Plan.ChainLength || Plan.Chain[Seen] != Field) return false;
        ++Seen;
    }
    if (!Seen || Seen != Plan.ChainLength) return false;
    if (Plan.NativeTailChain)
    {
        const auto* C = Plan.NativeTailChain;
        if (!IsCurrentNativeTailChain(C, Plan.Class) || !ValidateTerminalNativeTailChain(*C)
            || !IsTerminalNativeTailClass(*C, Plan.Class) || Plan.ActualReceiverClass != Plan.Class
            || Plan.ChainLength != 2 || Property != C->Owned || Plan.Fields.Num() != 1) return false;
        if (C->bIndependentHeads)
        {
            if (Plan.NativeTailHead != FindTerminalNativeTailHead(*Plan.NativeTailChain, Plan.Class)
                || !Plan.NativeTailHead || Plan.Chain[0] != Plan.NativeTailHead->OwnedProxy.Get()
                || Plan.Chain[1] != Plan.NativeTailHead->TailProxy.Get()) return false;
        }
        else if (Plan.NativeTailHead || Plan.Chain[0] != C->Owned || Plan.Chain[1] != C->Adapter.Get()) return false;
    }
    else if (Plan.Chain[Seen - 1] != Property || Property->DestructorLinkNext) return false;
    return bAfterOriginal
        ? (Plan.Phase == ETerminalPlanPhase::Deferred || Plan.Phase == ETerminalPlanPhase::Completing
            || Plan.Phase == ETerminalPlanPhase::Callback || Plan.Phase == ETerminalPlanPhase::Cleaning)
            && Token.Object->IsUnreachable()
            && Token.Object->HasAllFlags(RF_BeginDestroyed | RF_FinishDestroyed)
        : (Plan.Phase == ETerminalPlanPhase::Preparing || Plan.Phase == ETerminalPlanPhase::Prepared)
            && !Token.Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
}

void ReleaseTerminalPlan(FTerminalDestroyPlan& Plan)
{
    asIScriptModule* Module = Plan.ModulePin;
    Plan.ModulePin = nullptr;
    Plan.Phase = ETerminalPlanPhase::Retired;
    Plan.Qualification.Reset();
    if (Plan.NativeTailChain && Plan.NativeTailChain->bAutomatic && Plan.FinalInt32Provider)
    {
        TerminalFinalInt32Providers.Remove(Plan.Int32Slot.FinalProviderEpoch);
        Plan.FinalInt32Provider = nullptr;
    }
    auto Writer = MoveTemp(Plan.PrepareWriter);
    if (Module) AngelscriptRetiredModules::ReleaseModule(Module);
    Writer.Reset();
    Plan.CompilationReservation.Reset();
}

bool CanCleanupTerminalFreshField(const FTerminalDestroyPlan& Plan, const FTerminalOwnedField& Field)
{
    // A saved pointer is not proof that the receiver allocation and field generation still exist.
    if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread() || !Plan.ModulePin
        || Field.Phase != ETerminalFieldPhase::FreshLive || Field.Fresh != Field.Storage || Field.FreshEpoch != Plan.Token.ArmEpoch
        || !SameTerminalItem(Plan.ReceiverIndex, Plan.ReceiverSerial, Plan.Token.Object)
        || !SameTerminalItem(Plan.ClassIndex, Plan.ClassSerial, Plan.Class)
        || Plan.Token.Key.ResolveObjectPtrEvenIfUnreachable() != Plan.Token.Object
        || Plan.ClassKey.ResolveObjectPtrEvenIfUnreachable() != Plan.Class) return false;
    if (!ValidateTerminalActualReceiverMetadata(Plan) || Plan.Class->bLayoutChanging
        || Plan.Class->GetPropertiesSize() != Plan.PropertiesSize
        || !ValidateTerminalOwnerAssociation(Plan, Plan.Class, static_cast<asITypeInfo*>(Plan.Token.TypeCookie))) return false;
    const auto* Code = Plan.Owners.FindByPredicate([&](const FTerminalOwnerCode& Owner) { return Owner.Owner == Field.Owner; });
    if (!Code || !SameTerminalItem(Code->Index, Code->Serial, Code->Owner)
        || Code->Key.ResolveObjectPtrEvenIfUnreachable() != Code->Owner
        || Code->Owner->bLayoutChanging || Code->Cookie != Field.OwnerCookie
        || !ValidateTerminalOwnerAssociation(Plan, Code->Owner, Field.OwnerCookie)) return false;
    const auto* Current = FindFProperty<FStructProperty>(Code->Owner, Field.Name);
    if (!Current || Current != Field.Property || Current->GetOwnerStruct() != Code->Owner
        || !Code->Owner->UeDelayedDestroyFields.Contains(Current) || Current->Struct != FInstancedStruct::StaticStruct()
        || Current->ArrayDim != 1 || Current->GetSize() != sizeof(FInstancedStruct)
        || Current->GetElementSize() != sizeof(FInstancedStruct) || Current->GetOffset_ForInternal() != Field.Offset
        || uint64(Current->GetPropertyFlags()) != Field.Flags || Field.Offset < Code->Owner->ScriptPropertyOffset
        || Field.Offset > Plan.PropertiesSize - int32(sizeof(FInstancedStruct))
        || !Field.Storage || reinterpret_cast<UPTRINT>(Field.Storage) % alignof(FInstancedStruct) != 0) return false;
    return Current->ContainerPtrToValuePtr<void>(Plan.Token.Object) == Field.Storage;
}

bool ValidateRuntimeOwnedTerminalInt32LateFence(const FTerminalDestroyPlan& Plan)
{
    using namespace Cuviss::TerminalAdmission;
    const auto& Slot = Plan.Int32Slot;
    const auto* Provider = Plan.FinalInt32Provider;
    const auto* Entry = TerminalFinalInt32Providers.Find(Slot.FinalProviderEpoch);
    if (!IsInGameThread()
        || !IsAngelscriptExecutionWriterHeldByCurrentThread()
        || !HasRuntimeOwnedTerminalScalar(Plan)
        || (!bRequireTerminalFinalInt32 && !IsAutomaticTerminalPlan(Plan))
        || Plan.bDevFacade
        || (Plan.RemovedAssociation.IsSet() && !IsAutomaticTerminalPlan(Plan))
        || Plan.ReceiverBlueprint
        || (Plan.bReceiverIsCurrentCdo && !IsAutomaticTerminalPlan(Plan))
        || Plan.ReceiverBlueprintRole != ETerminalBlueprintReceiverRole::None
        || Plan.Phase != ETerminalPlanPhase::Deferred || Plan.ActualReceiverClass != Plan.Class
        || ActiveTerminalOperation != &Plan || ActiveRuntimeTerminalPlan
        || Plan.ThreadId != FPlatformTLS::GetCurrentThreadId()
        || !Slot.bRuntimeOwnedOriginal || !IsTerminalCapturedInt32Slot(Slot)
        || Plan.Int32Phase != EInt32SlotPhase::OriginalLive || !Plan.bRuntimeOwnedFieldReturned
        || Plan.Fields.Num() != 1 || Plan.Fields[0].Phase != ETerminalFieldPhase::FreshLive
        || Plan.Fields[0].Property != Plan.Property || Plan.Fields[0].Storage != Plan.Storage
        || Plan.Fields[0].Owner != TerminalFieldDeclarationOwner(Plan) || Plan.Fields[0].Offset != Plan.Offset
        || !Plan.Property || Plan.Class->DestructorLink != TerminalOwnedExecutionNode(Plan, Plan.Property)
        || (!Plan.NativeTailChain && Plan.Property->DestructorLinkNext)
        || (Plan.NativeTailChain && (!Plan.bNativeTailReturned
                || !ValidateTerminalNativeTailChain(*Plan.NativeTailChain)))
        || Plan.Property->GetOwnerStruct() != TerminalFieldDeclarationOwner(Plan) || Plan.Property->Struct != FInstancedStruct::StaticStruct()
        || Plan.Property->ArrayDim != 1 || Plan.Property->GetSize() != sizeof(FInstancedStruct)
        || Plan.Property->GetOffset_ForInternal() != Plan.Offset
        || uint64(Plan.Property->GetPropertyFlags()) != Plan.PropertyFlags
        || Plan.Chain[0] != TerminalOwnedExecutionNode(Plan, Plan.Property)
        || (Plan.NativeTailChain
            ? (Plan.ChainLength != 2
                || Plan.Chain[1] != (Plan.NativeTailHead
                    ? Plan.NativeTailHead->TailProxy.Get()
                    : Plan.NativeTailChain->Adapter.Get()))
            : Plan.ChainLength != 1)
        || reinterpret_cast<UPTRINT>(Plan.Storage) != reinterpret_cast<UPTRINT>(Plan.Token.Object) + Plan.Offset
        || Plan.Fields[0].Fresh != Plan.Storage || Plan.Fields[0].FreshEpoch != Plan.Token.ArmEpoch
        || Plan.OriginalCompleted != 1 || Plan.EmptyConstructed != 1 || Plan.EmptyDestroyed
        || Plan.FreshInt32 || Plan.FreshInt32Epoch || Plan.FinalInt32Epoch
        || !Entry || !Entry->IsValid() || Entry->Get() != Provider || !Provider
        || !Provider->Registration.bRuntimeOwnedOriginal || !Provider->ConstructOrder
        || Provider->bWriting || Provider->bPublished || Provider->bEnded
        || Provider->WriteOrder || Provider->PublishOrder || Provider->EndOrder
        || Provider->PlanId != Plan.Token.PlanId || Provider->Ticket.Storage != Slot.Storage
        || Provider->Ticket.SlotEpoch != Slot.Epoch || Provider->Ticket.ProviderEpoch != Slot.FinalProviderEpoch
        || Provider->Registration.Receiver != Plan.Token.Object || Provider->Registration.Scalar != Slot.Property
        || Provider->Registration.Owned != Plan.Property || Provider->Registration.ProviderEpoch != Slot.FinalProviderEpoch
        || Provider->ReceiverKey != Plan.Token.Key || !Plan.ModulePin
        || Plan.Class->GetPropertiesStartOffset() != 0
        || (Plan.NativeTailChain ? (TerminalFieldDeclarationOwner(Plan)->GetSuperClass() != Plan.NativeTailChain->Native
                || TerminalFieldDeclarationOwner(Plan)->CodeSuperClass != Plan.NativeTailChain->Native
                || Slot.Offset < Plan.NativeTailChain->NativePrefix)
            : (Plan.Class->GetSuperClass() != UObject::StaticClass() || Plan.Class->CodeSuperClass != UObject::StaticClass()))
        || (Plan.NativeTailHead && (Plan.OwnedFinishCalls != 1 || Plan.NativeTailHead->Class != Plan.Class))
        || !ValidateTerminalOwners(Plan) || !ValidateTerminalInt32SlotMetadata(Plan)) return false;
    // Saved bytecode and generation witnesses; no GetClass/flags/container method
    // on the ended receiver and no dereference of the ended native payload.
    return Plan.ByteCode && Plan.ByteCodeLength && HashTerminalCode(Plan.ByteCode, Plan.ByteCodeLength) == Plan.CodeHash
        && ValidateTerminalOwnerAssociation(Plan, Plan.Class, static_cast<asITypeInfo*>(Plan.Token.TypeCookie), false);
}

bool CleanupTerminalFreshInt32(FTerminalDestroyPlan& Plan)
{
    using namespace Cuviss::TerminalAdmission;
    if (!Plan.Int32Slot.Property) return ValidateTerminalInt32SlotMetadata(Plan);
    if (Plan.Int32Phase != EInt32SlotPhase::FreshLive)
        return !Plan.FreshInt32 && !Plan.FreshInt32Epoch;
    if (!ValidateTerminalInt32SlotMetadata(Plan) || Plan.FreshInt32 != Plan.Int32Slot.Storage
        || !Plan.FreshInt32Epoch || Plan.FreshInt32Epoch != Plan.Int32Slot.Epoch
        || Plan.Phase != ETerminalPlanPhase::Cleaning || !Plan.Qualification
        || !DidCompleteCompiledTerminalDestructor(*Plan.Qualification)) return false;
    using FTerminalInt32Value = int32;
    FTerminalInt32Value* Fresh = Plan.FreshInt32;
    const uint64 Epoch = Plan.FreshInt32Epoch;
    Plan.FreshInt32 = nullptr;
    Plan.FreshInt32Epoch = 0;
    Plan.Int32Phase = EInt32SlotPhase::FreshEnded;
    Fresh->~FTerminalInt32Value();
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
        TEXT("SOLOAS_TERMINAL_INT32_END object=%p offset=%d epoch=%llu old_read=0"),
        Plan.Token.Object, Plan.Int32Slot.Offset, Epoch);
    return true;
}

bool ValidateTerminalFieldSetForCallback(const FTerminalDestroyPlan& Plan)
{
    if (!ValidateTerminalPlan(Plan, true)
        || Plan.OriginalCompleted != uint32(Plan.Fields.Num())
        || Plan.EmptyConstructed != uint32(Plan.Fields.Num()) || Plan.EmptyDestroyed != 0) return false;
    for (const auto& Field : Plan.Fields)
        if (!CanCleanupTerminalFreshField(Plan, Field) || Field.Fresh->IsValid()) return false;
    return true;
}

bool CleanupTerminalEmptyFields(FTerminalDestroyPlan& Plan)
{
    if (!CleanupTerminalFreshInt32(Plan)) return false;
    bool Safe = true;
    for (int32 Index = Plan.Fields.Num() - 1; Index >= 0; --Index)
    {
        auto& Field = Plan.Fields[Index];
        if (!Field.Fresh) continue;
        if (!CanCleanupTerminalFreshField(Plan, Field))
        {
            Safe = false;
            Field.Fresh = nullptr;
            Field.Phase = ETerminalFieldPhase::FreshEnding;
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Error,
                TEXT("SOLOAS_TERMINAL_CLEANUP_STOP object=%p field=%p field_index=%d "
                     "reason=storage_generation_unproved pointer_dereferenced=0"), Plan.Token.Object, Field.Property, Index);
            continue;
        }
        if (!Plan.bDevFacade && Plan.Qualification
            && !Cuviss::TerminalAdmission::DidCompleteCompiledTerminalDestructor(*Plan.Qualification)) return false;
        if (Field.Fresh->GetMemory() || Field.Fresh->GetScriptStruct()) return false;
        FInstancedStruct* Fresh = Field.Fresh;
        Field.Fresh = nullptr;
        Field.Phase = ETerminalFieldPhase::FreshEnding;
        Fresh->~FInstancedStruct();
        Field.Phase = ETerminalFieldPhase::FreshEnded;
        ++Plan.EmptyDestroyed;
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_EMPTY_END object=%p field=%p field_index=%d empty_destroy=1 value_addr=%p epoch=%llu"),
            Plan.Token.Object, Field.Property, Index, Field.Storage, Field.FreshEpoch);
    }
    return Safe;
}

void FailTerminalPlan(FTerminalDestroyPlan& Plan, const TCHAR* Reason)
{
    CleanupTerminalEmptyFields(Plan);
    ReleaseTerminalPlan(Plan);
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal,
        TEXT("%s original_fields=%u empty_construct=%u empty_destroy=%u module_pin=0 fallback_early=0"),
        Reason, Plan.OriginalCompleted, Plan.EmptyConstructed, Plan.EmptyDestroyed);
}

ETerminalCaptureResult CaptureTerminalPlan(FTerminalDestroyPlan& Plan, UObject* Object, const FStructProperty& Property,
    asITypeInfo* Type, uint64 Sequence)
{
    Plan.Token.Object = Object;
    Plan.Token.Key = FObjectKey(Object);
    Plan.Token.TypeCookie = Type;
    Plan.Token.TrackSequence = Sequence;
    Plan.ActualReceiverClass = Object->GetClass();
    Plan.Class = UASClass::GetFirstASClass(Plan.ActualReceiverClass);
    if (!Plan.Class || Plan.Class->GetClass() != UASClass::StaticClass()
        || !IsTerminalReceiverTemplateAllowed(Object, Plan.ActualReceiverClass, Plan.Class, &Property,
            Plan.bDevFacade, RemovedRuntimeAssociations.Contains(Plan.Token.Key))
        || !Type || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Type)) return ETerminalCaptureResult::InvalidIdentity;
    if (Plan.ActualReceiverClass != Plan.Class
        && (Plan.bDevFacade || !IsTerminalBlueprintReceiverMetadata(Plan.ActualReceiverClass, Plan.Class, &Property,
            &Plan.ReceiverBlueprintRole, &Plan.ReceiverBlueprint)))
        return ETerminalCaptureResult::InvalidIdentity;
    if (Plan.ReceiverBlueprint)
    {
        Plan.ReceiverBlueprintKey = FObjectKey(Plan.ReceiverBlueprint);
        Plan.ReceiverBlueprintIndex = GUObjectArray.ObjectToIndex(Plan.ReceiverBlueprint);
        Plan.ReceiverBlueprintSerial = GUObjectArray.GetSerialNumber(Plan.ReceiverBlueprintIndex);
        Plan.bReceiverIsCurrentCdo = Object->HasAnyFlags(RF_ClassDefaultObject);
        if (!SameTerminalItem(Plan.ReceiverBlueprintIndex, Plan.ReceiverBlueprintSerial, Plan.ReceiverBlueprint))
            return ETerminalCaptureResult::InvalidIdentity;
    }
    if (const auto* Removed = RemovedRuntimeAssociations.Find(Plan.Token.Key))
    {
        if (Plan.ActualReceiverClass != Plan.Class || !ValidateRemovedRuntimeAssociation(*Removed, Object, Plan.Class, Type, Removed->Module, Sequence)
            || !Removed->SourceGeneration) return ETerminalCaptureResult::InvalidIdentity;
        Plan.RemovedAssociation = *Removed;
    }
    else if (Plan.Class->ScriptTypePtr != Type || Type->GetUserData() != Plan.Class)
        return ETerminalCaptureResult::InvalidIdentity;
    const auto* DeclaredOwner = Cast<UASClass>(Property.GetOwnerStruct());
    if (!DeclaredOwner || !Plan.Class->IsChildOf(DeclaredOwner) || !DeclaredOwner->UeDelayedDestroyFields.Contains(&Property)
        || Property.Struct != FInstancedStruct::StaticStruct() || Property.ArrayDim != 1
        || Property.GetSize() != sizeof(FInstancedStruct) || Property.GetElementSize() != sizeof(FInstancedStruct))
        return ETerminalCaptureResult::InvalidIdentity;
    Plan.PropertiesSize = Plan.Class->GetPropertiesSize();
    Plan.ClassKey = FObjectKey(Plan.Class);
    Plan.ReceiverIndex = GUObjectArray.ObjectToIndex(Object);
    Plan.ReceiverSerial = GUObjectArray.GetSerialNumber(Plan.ReceiverIndex);
    Plan.ClassIndex = GUObjectArray.ObjectToIndex(Plan.Class);
    Plan.ClassSerial = GUObjectArray.GetSerialNumber(Plan.ClassIndex);
    Plan.ActualReceiverClassKey = FObjectKey(Plan.ActualReceiverClass);
    Plan.ActualReceiverClassIndex = GUObjectArray.ObjectToIndex(Plan.ActualReceiverClass);
    Plan.ActualReceiverClassSerial = GUObjectArray.GetSerialNumber(Plan.ActualReceiverClassIndex);
    Plan.ActualReceiverPropertiesSize = Plan.ActualReceiverClass->GetPropertiesSize();
    Plan.Property = &Property;
    Plan.Offset = Property.GetOffset_ForInternal();
    if (Plan.Offset <= 0 || Plan.Offset > Plan.PropertiesSize - int32(sizeof(FInstancedStruct))) return ETerminalCaptureResult::InvalidIdentity;
    Plan.Storage = Property.ContainerPtrToValuePtr<void>(Object);
    if (!Plan.Storage || reinterpret_cast<UPTRINT>(Plan.Storage) % alignof(FInstancedStruct) != 0) return ETerminalCaptureResult::InvalidIdentity;
    Plan.PropertyFlags = uint64(Property.GetPropertyFlags());
    Plan.ThreadId = FPlatformTLS::GetCurrentThreadId();
    const FProperty* Last = nullptr;
    uint32 Matches = 0;
    for (const FProperty* Field = Plan.ActualReceiverClass->DestructorLink; Field; Field = Field->DestructorLinkNext)
    {
        if (Plan.ChainLength == UE_ARRAY_COUNT(Plan.Chain)) return ETerminalCaptureResult::InvalidIdentity;
        for (uint32 Seen = 0; Seen != Plan.ChainLength; ++Seen)
            if (Plan.Chain[Seen] == Field) return ETerminalCaptureResult::InvalidIdentity;
        Plan.Chain[Plan.ChainLength++] = Field;
        Matches += Field == &Property ? 1u : 0u;
        Last = Field;
    }
    if (auto* C = FindRuntimeNativeTailChain(Plan.Class))
    {
        Plan.NativeTailChain = C;
        Plan.NativeTailHead = FindTerminalNativeTailHead(*C, Plan.Class);
        if (!ValidateTerminalNativeTailChain(*C) || Plan.ActualReceiverClass != Plan.Class
            || &Property != C->Owned || Plan.ChainLength != 2 || Plan.RemovedAssociation.IsSet() || Plan.bDevFacade
            || (C->bIndependentHeads ? (!Plan.NativeTailHead || Matches != 0
                    || Plan.Chain[0] != Plan.NativeTailHead->OwnedProxy.Get() || Last != Plan.NativeTailHead->TailProxy.Get())
                : (Matches != 1 || Plan.Chain[0] != C->Owned || Last != C->Adapter.Get())))
            return ETerminalCaptureResult::InvalidIdentity;
    }
    else if (Matches != 1 || Last != &Property || Property.DestructorLinkNext)
    {
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_ARM_LAST_REJECT selected=%p actual_last=%p chain_nodes=%u replacement=0"),
            &Property, Last, Plan.ChainLength);
        return ETerminalCaptureResult::InvalidIdentity;
    }
    Plan.ModulePin = AngelscriptRetiredModules::RetainType(Type);
    Plan.Destructor = FindTerminalDestructor(Type);
    if (!Plan.ModulePin || Type->GetEngine() != FAngelscriptManager::Get().GetScriptEngine()
        || !CaptureTerminalOwnedFields(Plan)) return ETerminalCaptureResult::InvalidIdentity;
    bool bUnsupportedCode = Plan.Destructor == nullptr;
    for (const auto& Owner : Plan.Owners)
        if (Owner.Destructor && (!Owner.ByteCode || !Owner.Length || Owner.Length > 1048576)) bUnsupportedCode = true;
    if (bUnsupportedCode) return ETerminalCaptureResult::UnsupportedCode;
    Plan.ByteCode = Plan.Destructor->GetByteCode(&Plan.ByteCodeLength);
    if (!Plan.ByteCode || !Plan.ByteCodeLength || Plan.ByteCodeLength > 1048576)
        return ETerminalCaptureResult::InvalidIdentity;
    Plan.CodeHash = HashTerminalCode(Plan.ByteCode, Plan.ByteCodeLength);
    return ValidateTerminalPlan(Plan, false) ? ETerminalCaptureResult::Captured : ETerminalCaptureResult::InvalidIdentity;
}

Cuviss::TerminalAdmission::FReceiverShape TerminalReceiverShape(const FTerminalDestroyPlan& Plan)
{
    auto* Declaration = TerminalFieldDeclarationOwner(Plan);
    auto* FieldOwner = TerminalDeclarationType(Plan);
    return {CastToObjectType(static_cast<asCTypeInfo*>(Plan.Token.TypeCookie)), Plan.Token.Object, Plan.Storage,
        Plan.Offset, static_cast<asITypeInfo*>(Plan.Token.TypeCookie), Plan.Token.PlanId,
        Plan.RemovedAssociation.IsSet() ? Plan.RemovedAssociation->SourceGeneration : 0, Plan.Int32Slot,
        FieldOwner, Plan.NativeTailHead && Plan.Class != Declaration};
}

bool ValidateTerminalQualification(const FTerminalDestroyPlan& Plan)
{
    if (Plan.bDevFacade) return true;
    if (!Plan.Qualification || Plan.Fields.Num() != 1) return false;
    const auto& Field = Plan.Fields[0];
    return Cuviss::TerminalAdmission::ValidateCompiledTerminalDestructor(*Plan.Qualification,
        {TerminalReceiverShape(Plan), Field.FreshEpoch, Field.Phase == ETerminalFieldPhase::FreshLive,
            Plan.FreshInt32Epoch, Plan.Int32Phase});
}

bool BeginTerminalQualification(FTerminalDestroyPlan& Plan)
{
    if (Plan.bDevFacade) return true;
    if (!Plan.Qualification || Plan.Fields.Num() != 1
        || !Cuviss::TerminalAdmission::RefreshCompiledTerminalDestructorIdleContext(*Plan.Qualification)) return false;
    const auto& Field = Plan.Fields[0];
    return Cuviss::TerminalAdmission::BeginCompiledTerminalDestructor(*Plan.Qualification,
        {TerminalReceiverShape(Plan), Field.FreshEpoch, Field.Phase == ETerminalFieldPhase::FreshLive,
            Plan.FreshInt32Epoch, Plan.Int32Phase});
}

bool SameRuntimeDeferredIdentity(const SoloASBridge::FRuntimeDeferredDestroyToken& A,
    const SoloASBridge::FRuntimeDeferredDestroyToken& B)
{
    return A.Key == B.Key && A.Object == B.Object && A.TypeCookie == B.TypeCookie
        && A.TrackSequence == B.TrackSequence && A.ArmEpoch == B.ArmEpoch
        && A.Generation == B.Generation && A.ClaimSequence == B.ClaimSequence && A.PlanId == B.PlanId;
}

FTerminalDestroyPlan* FindTerminalPlan(const SoloASBridge::FRuntimeDeferredDestroyToken& Token)
{
    if (ActiveTerminalOperation && SameRuntimeDeferredIdentity(ActiveTerminalOperation->Token, Token))
        return ActiveTerminalOperation;
    const auto* Owner = TerminalPlans.Find(Token.Key);
    return Owner && *Owner && SameRuntimeDeferredIdentity((*Owner)->Token, Token) ? Owner->Get() : nullptr;
}

SoloASBridge::ERuntimeDestroyPrepareResult PrepareRuntimeDeferredDestroy(
    const SoloASBridge::FRuntimeDestroyPreclaim& Source, SoloASBridge::FRuntimeDeferredDestroyToken& OutToken)
{
    using FTerminalAdmissionResult = SoloASBridge::ERuntimeDestroyPrepareResult;
    OutToken = {};
    if (!IsInGameThread() || ActiveTerminalOperation || ActiveRuntimeTerminalPlan
        || !Source.Object || !Source.TypeCookie || !Source.ClaimSequence
        || Source.Generation != UASClass::RuntimeLifecycleGeneration) return FTerminalAdmissionResult::InvalidIdentity;
    auto Reservation = MakeUnique<FTerminalPlanReservation>();
    auto Writer = MakeUnique<FAngelscriptExecutionWriteScope>();
    uint64 Sequence = 0;
    asIScriptModule* RetainedModule = nullptr;
    if (!AngelscriptRuntimeGcMirror::ReadRuntimeDestroyTracking(Source, Sequence, &RetainedModule)) return FTerminalAdmissionResult::InvalidIdentity;
    UClass* ReceiverClass = Source.Object->GetClass();
    UASClass* SourceOwner = UASClass::GetFirstASClass(ReceiverClass);
    auto* Type = static_cast<asITypeInfo*>(Source.TypeCookie);
    auto* Removed = RemovedRuntimeAssociations.Find(Source.Key);
    bool bBlueprintDelayedReceiver = false;
    if (SourceOwner && ReceiverClass != SourceOwner)
    {
        UClass* Current = SourceOwner;
        for (uint32 Depth = 0; Current && Depth < 64; ++Depth, Current = Current->GetSuperClass())
        {
            const auto* Owner = Cast<UASClass>(Current);
            if (Owner && !Owner->UeDelayedDestroyFields.IsEmpty())
            {
                bBlueprintDelayedReceiver = true;
                break;
            }
        }
        // An unbounded/cyclic ancestry cannot establish the ordinary fallback policy either.
        if (Current) bBlueprintDelayedReceiver = true;
    }
    if (bBlueprintDelayedReceiver && Removed) return FTerminalAdmissionResult::InvalidIdentity;
    // Only a fresh ordinary deletion admission can select early normal destruction.
    // A prepared/deferred terminal plan and a failed ordinary admission never fall through.
    if (Removed && AdmitOrdinaryRemovedRuntimeReceiver(*Removed, Source, SourceOwner, Type, RetainedModule, Sequence, Reservation->IsAcquired()))
        return FTerminalAdmissionResult::NotApplicable;
    if (Removed && Removed->bOrdinaryAdmitted) return FTerminalAdmissionResult::InvalidIdentity;
    const bool bRemovedReceiver = Removed && ValidateRemovedRuntimeAssociation(*Removed, Source.Object,
        SourceOwner, Type, RetainedModule, Sequence);
    if (Removed && !bRemovedReceiver)
    {
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_REMOVED_PRECLAIM_REJECT stage=association object=%s receiver=%s owner=%s template=%d ")
            TEXT("role=%d attached=%d detached=%d compiling=%d compilation_epoch=%llu collection_epoch=%llu ")
            TEXT("collection_current=%d reservation=%d sequence=%llu saved_sequence=%llu production_credit=0"),
            *Source.Object->GetName(), *GetNameSafe(ReceiverClass), *GetNameSafe(SourceOwner), Source.Object->IsTemplate(),
            int32(Removed->Role), SourceOwner && SourceOwner->ScriptTypePtr == Type,
            SourceOwner && !SourceOwner->ScriptTypePtr, bTerminalCompilationInProgress,
            TerminalCompilationEpoch, ReinstanceCollectionEpoch, IsReinstanceCollectionCurrent(ReinstanceCollectionEpoch),
            Reservation->IsAcquired(), Sequence, Removed->Sequence);
        return FTerminalAdmissionResult::InvalidIdentity;
    }
    if (!SourceOwner || SourceOwner->bLayoutChanging
        || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Type)
        || Type->GetUserData() != SourceOwner
        || (SourceOwner->ScriptTypePtr != Type
            && !AngelscriptRuntimeGcMirror::IsRetiredRuntimeTypeIdentity(Type, RetainedModule, SourceOwner)
            && !bRemovedReceiver))
        return FTerminalAdmissionResult::InvalidIdentity;
    uint64 CurrentSequence = 0;
    asIScriptModule* CurrentRetainedModule = nullptr;
    if (!AngelscriptRuntimeGcMirror::ReadRuntimeDestroyTracking(Source, CurrentSequence, &CurrentRetainedModule)
        || CurrentSequence != Sequence || CurrentRetainedModule != RetainedModule)
        return FTerminalAdmissionResult::InvalidIdentity;
    // Only the exact old receiver captured around synchronous reinstance GC can borrow the barrier.
    if (!Reservation->IsAcquired() && bRemovedReceiver)
        BorrowReinstanceReservation(*Reservation, *Removed);
    if (!Reservation->IsAcquired()) return bRemovedReceiver || bBlueprintDelayedReceiver
        ? FTerminalAdmissionResult::InvalidIdentity : FTerminalAdmissionResult::NotApplicable;
    TUniquePtr<FTerminalDestroyPlan> HeapOwner;
    if (auto* Existing = TerminalPlans.Find(Source.Key))
    {
        if ((*Existing)->Phase == ETerminalPlanPhase::Retired) TerminalPlans.Remove(Source.Key);
        else
        {
            if (!(*Existing)->bDevFacade || (*Existing)->Phase != ETerminalPlanPhase::Preparing
                || (*Existing)->Token.Object != Source.Object || (*Existing)->Token.TypeCookie != Source.TypeCookie
                || (*Existing)->Token.TrackSequence != Sequence) return FTerminalAdmissionResult::InvalidIdentity;
            HeapOwner = MoveTemp(*Existing);
            TerminalPlans.Remove(Source.Key);
        }
    }
    if (!HeapOwner)
    {
        UASClass* Class = bBlueprintDelayedReceiver ? SourceOwner : Cast<UASClass>(ReceiverClass);
        auto* ReceiverChain = FindRuntimeNativeTailChain(Class);
        const bool bNativeTail = ReceiverChain != nullptr;
        UASClass* Declaration = bNativeTail ? ReceiverChain->Class : Class;
        auto* Head = bNativeTail ? FindTerminalNativeTailHead(*ReceiverChain, Class) : nullptr;
        if (!Class || Class->GetClass() != UASClass::StaticClass()
            || (Source.Object->IsTemplate() && !bBlueprintDelayedReceiver
                && !(ReceiverChain && ReceiverChain->bAutomatic && Source.Object->HasAnyFlags(RF_ClassDefaultObject)))
            || Class != SourceOwner || (Class->ScriptTypePtr != Source.TypeCookie && !bRemovedReceiver)
            || (Class->GetSuperClass() != UObject::StaticClass()
                && (!bNativeTail || !ValidateTerminalNativeTailChain(*ReceiverChain)))
            || !Declaration || Declaration->UeDelayedDestroyFields.Num() != 1)
        {
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_NOT_APPLICABLE reason=receiver_scope generation=%llu object=%s receiver=%s template=%d cdo=%d"),
                Source.Generation, *Source.Object->GetName(), *GetNameSafe(ReceiverClass),
                Source.Object->IsTemplate(), Source.Object->HasAnyFlags(RF_ClassDefaultObject));
            return bRemovedReceiver || bBlueprintDelayedReceiver
                ? FTerminalAdmissionResult::InvalidIdentity : FTerminalAdmissionResult::NotApplicable;
        }
        auto* Property = CastField<FStructProperty>(*Declaration->UeDelayedDestroyFields.CreateConstIterator());
        if (!Property || Property->GetOwnerStruct() != Declaration
            || !IsTerminalReceiverTemplateAllowed(Source.Object, ReceiverClass, Class, Property, false, Removed != nullptr)
            || (bBlueprintDelayedReceiver && !IsTerminalBlueprintReceiverMetadata(ReceiverClass, Class, Property)))
        {
#if WITH_EDITOR
            const auto* Blueprint = Cast<UBlueprint>(ReceiverClass->ClassGeneratedBy);
            const auto* Carrier = Cast<USoloASScriptBlueprintGeneratedClass>(ReceiverClass);
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_BP_SCOPE_REJECT object=%s receiver=%s class_flags=%u object_flags=%u "
                     "generated=%d skeleton=%d receiver_unreachable=%d blueprint_unreachable=%d "
                     "template_allowed=%d metadata_allowed=%d gate_any=%d gate_unreachable=%d "
                     "ref_same=%d post_same=%d child_null=%d outer_same=%d current_cdo=%d"),
                *Source.Object->GetName(), *GetNameSafe(ReceiverClass), uint32(ReceiverClass->ClassFlags),
                uint32(Source.Object->GetFlags()), Blueprint && Blueprint->GeneratedClass == ReceiverClass,
                Blueprint && Blueprint->SkeletonGeneratedClass == ReceiverClass, ReceiverClass->IsUnreachable(),
                Blueprint && Blueprint->IsUnreachable(),
                IsTerminalReceiverTemplateAllowed(Source.Object, ReceiverClass, Class, Property, false, Removed != nullptr),
                IsTerminalBlueprintReceiverMetadata(ReceiverClass, Class, Property),
                Carrier && Carrier->CanOrderTerminalMetadataFinish(false),
                Carrier && Carrier->CanOrderTerminalMetadataFinish(true),
                ReceiverClass->RefLink == Class->RefLink, ReceiverClass->PostConstructLink == Class->PostConstructLink,
                ReceiverClass->ChildProperties == nullptr, Source.Object->GetOuter() == ReceiverClass->GetOuter(),
                ReceiverClass->GetDefaultObject(false) == Source.Object);
#endif
            return FTerminalAdmissionResult::InvalidIdentity;
        }
        if (ReceiverClass->DestructorLink != (Head ? Head->OwnedProxy.Get() : Property)
            || (bNativeTail ? (ReceiverClass != Class || Property != ReceiverChain->Owned
                    || !ValidateTerminalNativeTailChain(*ReceiverChain)) : Property->DestructorLinkNext != nullptr))
        {
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_NOT_APPLICABLE reason=field_scope generation=%llu"), Source.Generation);
            return bRemovedReceiver || bBlueprintDelayedReceiver
                ? FTerminalAdmissionResult::InvalidIdentity : FTerminalAdmissionResult::NotApplicable;
        }
        HeapOwner = MakeUnique<FTerminalDestroyPlan>();
        HeapOwner->CompilationReservation = MoveTemp(Reservation);
        if (++NextTerminalPlanId == 0) return FTerminalAdmissionResult::InvalidIdentity;
        HeapOwner->Token.PlanId = NextTerminalPlanId;
        HeapOwner->Token.ArmEpoch = NextTerminalPlanId;
        {
            TGuardValue<FTerminalDestroyPlan*> CaptureGuard(ActiveTerminalOperation, HeapOwner.Get());
            const auto Captured = CaptureTerminalPlan(*HeapOwner, Source.Object, *Property,
                static_cast<asITypeInfo*>(Source.TypeCookie), Sequence);
            if (Captured != ETerminalCaptureResult::Captured)
            {
                if (Captured == ETerminalCaptureResult::UnsupportedCode)
                    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                        TEXT("SOLOAS_TERMINAL_NOT_APPLICABLE reason=unsupported_code generation=%llu"), Source.Generation);
                ReleaseTerminalPlan(*HeapOwner);
                return Captured == ETerminalCaptureResult::UnsupportedCode && !bRemovedReceiver && !bBlueprintDelayedReceiver
                    ? FTerminalAdmissionResult::NotApplicable : FTerminalAdmissionResult::InvalidIdentity;
            }
        }
    }
    auto& Plan = *HeapOwner;
    if (Reservation) Plan.CompilationReservation = MoveTemp(Reservation);
    TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, &Plan);
    Plan.Token.Generation = Source.Generation;
    Plan.Token.ClaimSequence = Source.ClaimSequence;
    if (!Plan.bDevFacade && (!CaptureTerminalInt32Slot(Plan)
        || (Plan.NativeTailChain && (!Plan.Int32Slot.Property || !Plan.Int32Slot.bRuntimeOwnedOriginal
            || !Plan.FinalInt32Provider || !HasRuntimeOwnedTerminalScalar(Plan)))))
    {
        ReleaseTerminalPlan(Plan);
        return bRemovedReceiver || bBlueprintDelayedReceiver
            ? FTerminalAdmissionResult::InvalidIdentity : FTerminalAdmissionResult::NotApplicable;
    }
    if (!ValidateTerminalPlan(Plan, false) || !Source.Object->IsUnreachable())
    {
        ReleaseTerminalPlan(Plan);
        return FTerminalAdmissionResult::InvalidIdentity;
    }
    if (!Plan.bDevFacade)
    {
        Cuviss::TerminalAdmission::FAdmissionDiagnostic Diagnostic;
        const bool bAdmissionPrepared = Cuviss::TerminalAdmission::PrepareCompiledTerminalDestructor(
            TerminalReceiverShape(Plan), Plan.Qualification, Diagnostic);
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_CONTEXT_SELECTION stage=%u primary_state=%d requested_before=%d ")
            TEXT("requested_after=%d prepare_result=%d game_thread=%d active_context=%d ")
            TEXT("active_execution=%d primary_context=%d line_callbacks=%d prepared=%d"),
            uint32(Diagnostic.ContextSelectionStage), Diagnostic.PrimaryContextState,
            Diagnostic.RequestedContextStateBeforePrepare, Diagnostic.RequestedContextStateAfterPrepare,
            Diagnostic.ContextPrepareResult, Diagnostic.bContextGameThread ? 1 : 0,
            Diagnostic.bHasActiveContext ? 1 : 0, Diagnostic.bHasActiveExecution ? 1 : 0,
            Diagnostic.bHasPrimaryContext ? 1 : 0, Diagnostic.bLineCallbacksPossible ? 1 : 0,
            bAdmissionPrepared ? 1 : 0);
        if (!bAdmissionPrepared)
        {
            constexpr int32 MaxDiagnosticEntries = 64;
            constexpr int32 MaxNativeCalls = 2;
            const int32 VariableCount = Diagnostic.ObjectVariables.Num();
            const int32 ScopeCount = Diagnostic.ObjectScopes.Num();
            const int32 NativeCallCount = Diagnostic.NativeCalls.Num();
            const int32 InstructionCount = Diagnostic.Instructions.Num();
            const int32 VariableEntries = FMath::Min(VariableCount, MaxDiagnosticEntries);
            const int32 ScopeEntries = FMath::Min(ScopeCount, MaxDiagnosticEntries);
            const int32 NativeCallEntries = FMath::Min(NativeCallCount, MaxNativeCalls);
            const int32 InstructionEntries = FMath::Min(InstructionCount, MaxDiagnosticEntries);
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_NOT_APPLICABLE reason=admission_%u pc=%u code_words=%u source_generation=%llu context_callbacks=%u ")
                TEXT("heap_objects=%u object_types=%u object_positions=%u object_scopes=%u variable_words=%u ")
                TEXT("stack_needed=%d argument_words=%d total_words=%d variables=%d/%d scopes=%d/%d ")
                TEXT("native_calls=%d/%d instructions=%d/%d metadata_truncated=%d"),
                uint32(Diagnostic.Failure), Diagnostic.WordOffset, Diagnostic.CodeWords,
                Diagnostic.SourceGeneration, Diagnostic.ContextCallbacks, Diagnostic.HeapObjectCount,
                Diagnostic.ObjectTypeCount, Diagnostic.ObjectPositionCount, Diagnostic.ObjectScopeCount,
                Diagnostic.VariableWords, Diagnostic.StackNeeded, Diagnostic.ArgumentWords, Diagnostic.TotalWords,
                VariableCount, VariableEntries, ScopeCount, ScopeEntries, NativeCallCount, NativeCallEntries,
                InstructionCount, InstructionEntries, Diagnostic.bMetadataTruncated ? 1 : 0);
            for (int32 Index = 0; Index < VariableEntries; ++Index)
            {
                const auto& Variable = Diagnostic.ObjectVariables[Index];
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                    TEXT("SOLOAS_TERMINAL_ADMISSION_VARIABLE index=%u offset=%d type_flags=%llu type_role=%u has_type=%d has_position=%d heap=%d"),
                    Variable.Index, Variable.Offset, static_cast<unsigned long long>(Variable.TypeFlags),
                    uint32(Variable.TypeRole), Variable.bHasType ? 1 : 0,
                    Variable.bHasPosition ? 1 : 0, Variable.bHeap ? 1 : 0);
            }
            for (int32 Index = 0; Index < ScopeEntries; ++Index)
            {
                const auto& Scope = Diagnostic.ObjectScopes[Index];
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                    TEXT("SOLOAS_TERMINAL_ADMISSION_SCOPE index=%d program_position=%u variable_offset=%d option=%u"),
                    Index, Scope.ProgramPosition, Scope.VariableOffset, uint32(Scope.Option));
            }
            for (int32 Index = 0; Index < NativeCallEntries; ++Index)
            {
                const auto& Call = Diagnostic.NativeCalls[Index];
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                    TEXT("SOLOAS_TERMINAL_ADMISSION_NATIVE index=%d role=%u caller=%d callconv=%d base_offset=%d argument_words=%d total_words=%d ")
                    TEXT("return_token=%d return_bytes=%d script_return_words=%d return_on_stack=%d has_object_type=%d hidden_arg=%d output_type_arg=%d"),
                    Index, uint32(Call.Role), Call.CallerType, Call.CallConvention, Call.BaseOffset,
                    Call.ArgumentWords, Call.TotalWords, Call.ReturnToken, Call.ReturnBytes,
                    Call.ScriptReturnWords, Call.bReturnOnStack ? 1 : 0, Call.bHasObjectType ? 1 : 0,
                    Call.HiddenArgument, Call.OutputTypeArgument);
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                    TEXT("SOLOAS_TERMINAL_ADMISSION_NATIVE_META index=%d parameters=%u offsets=%u modifiers=%u first_modifier=%d first_offset=%d ")
                    TEXT("first_token=%d first_words=%d first_type_flags=%llu first_reference=%d first_readonly=%d first_metadata=%u"),
                    Index, Call.ParameterCount, Call.OffsetCount, Call.ModifierCount, Call.FirstModifier,
                    Call.FirstParameterOffset, Call.FirstParameterToken, Call.FirstParameterWords,
                    static_cast<unsigned long long>(Call.FirstParameterTypeFlags),
                    Call.bFirstParameterReference ? 1 : 0, Call.bFirstParameterReadOnly ? 1 : 0,
                    uint32(Call.FirstParameterMetadata));
            }
            for (int32 Index = 0; Index < InstructionEntries; ++Index)
            {
                const auto& Instruction = Diagnostic.Instructions[Index];
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                    TEXT("SOLOAS_TERMINAL_ADMISSION_OPCODE pc=%u opcode=%u words=%u short=%u branch=%d native_role=%u literal=%d type_operand=%u"),
                    Instruction.WordOffset, Instruction.Opcode, Instruction.WordCount, Instruction.ShortOperand,
                    Instruction.BranchTarget, Instruction.NativeRole, Instruction.bLiveLiteral ? 1 : 0,
                    Instruction.TypeOperand);
            }
            ReleaseTerminalPlan(Plan);
            return bRemovedReceiver || bBlueprintDelayedReceiver
                ? FTerminalAdmissionResult::InvalidIdentity : FTerminalAdmissionResult::NotApplicable;
        }
    }
    Plan.PrepareWriter = MoveTemp(Writer);
    if (!AngelscriptRuntimeGcMirror::TransitionRuntimeTerminalTracking(
            Plan.Token, AngelscriptRuntimeGcMirror::ERuntimeTerminalMirrorTransition::Prepare))
    {
        ReleaseTerminalPlan(Plan);
        return FTerminalAdmissionResult::InvalidIdentity;
    }
    Plan.Phase = ETerminalPlanPhase::Prepared;
    TerminalPlans.Add(Source.Key, MoveTemp(HeapOwner));
    if (Plan.NativeTailChain)
    {
        UE_LOG(
            LogAngelscriptRuntimeGcLifecycle,
            Display,
            TEXT(
            "SOLOAS_TERMINAL_DEFER object=%p sequence=%llu epoch=%llu field_last=0 native_tail_last=1 chain_nodes=%u module_pin=1 before_claim=1 plan=%llu"),
            Source.Object, Sequence, Plan.Token.ArmEpoch, Plan.ChainLength, Plan.Token.PlanId);
    }
    else
    {
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_DEFER object=%p sequence=%llu epoch=%llu field_last=1 chain_nodes=%u module_pin=1 before_claim=1 plan=%llu"),
            Source.Object, Sequence, Plan.Token.ArmEpoch, Plan.ChainLength, Plan.Token.PlanId);
    }
    // No successful callout may follow this proof before Bridge compares and commits its snapshot.
    if (!ValidateTerminalPlan(Plan, false) || !ValidateTerminalOriginalInt32Value(Plan)
        || (!Plan.bDevFacade && (!Plan.Qualification
            || !Cuviss::TerminalAdmission::ValidatePreparedCompiledTerminalDestructor(*Plan.Qualification, TerminalReceiverShape(Plan)))))
    {
        AngelscriptRuntimeGcMirror::TransitionRuntimeTerminalTracking(
            Plan.Token, AngelscriptRuntimeGcMirror::ERuntimeTerminalMirrorTransition::Abort);
        ReleaseTerminalPlan(Plan);
        return FTerminalAdmissionResult::InvalidIdentity;
    }
    OutToken = Plan.Token;
    return FTerminalAdmissionResult::Prepared;
}

bool ControlRuntimeDeferredDestroy(const SoloASBridge::FRuntimeDeferredDestroyToken& Token,
    SoloASBridge::ERuntimeDestroyControl Control)
{
    auto* Plan = FindTerminalPlan(Token);
    if (!IsInGameThread() || !Plan || !IsAngelscriptExecutionWriterHeldByCurrentThread()) return false;
    if (Control != SoloASBridge::ERuntimeDestroyControl::CommitPrepared
        && Control != SoloASBridge::ERuntimeDestroyControl::AbortPrepared
        && Control != SoloASBridge::ERuntimeDestroyControl::ValidateForCallback) return false;
    if (Control == SoloASBridge::ERuntimeDestroyControl::ValidateForCallback)
    {
        if (ActiveTerminalOperation != Plan || Plan->Phase != ETerminalPlanPhase::Deferred
            || !ValidateTerminalFieldSetForCallback(*Plan)
            || (!Plan->bDevFacade && (!Plan->Qualification
                || !Cuviss::TerminalAdmission::RefreshCompiledTerminalDestructorIdleContext(*Plan->Qualification)))
            || !ValidateTerminalQualification(*Plan)) return false;
        Plan->Phase = ETerminalPlanPhase::Completing;
        return true;
    }
    if (ActiveTerminalOperation || Plan->Phase != ETerminalPlanPhase::Prepared || !Plan->PrepareWriter) return false;
    TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, Plan);
    const bool bCommit = Control == SoloASBridge::ERuntimeDestroyControl::CommitPrepared;
    if (bCommit && (!ValidateTerminalOriginalInt32Value(*Plan)
        || !ValidateTerminalFinalProviderBeforeCommit(*Plan))) return false;
    if (!AngelscriptRuntimeGcMirror::TransitionRuntimeTerminalTracking(Token,
        bCommit ? AngelscriptRuntimeGcMirror::ERuntimeTerminalMirrorTransition::Commit
                : AngelscriptRuntimeGcMirror::ERuntimeTerminalMirrorTransition::Abort)) return false;
    if (bCommit)
    {
        Plan->Phase = ETerminalPlanPhase::Deferred;
        Plan->PrepareWriter.Reset();
    }
    else ReleaseTerminalPlan(*Plan);
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
bool RevalidateTerminalDestroy(const SoloASBridge::FRuntimeTerminalDestroyTokenForTest& Token,
    SoloASBridge::ERuntimeTerminalDestroyPhaseForTest Phase)
{
    check(IsInGameThread());
    TOptional<FAngelscriptExecutionWriteScope> Writer;
    if (!IsAngelscriptExecutionWriterHeldByCurrentThread()) Writer.Emplace();
    for (auto& Pair : TerminalPlans)
    {
        auto& Plan = *Pair.Value;
        if (Plan.Token.Object != Token.Object || Plan.Token.Key != Token.Key
            || Plan.Token.TypeCookie != Token.TypeCookie || Plan.Token.TrackSequence != Token.TrackSequence
            || Plan.Token.ArmEpoch != Token.ArmEpoch || Plan.Token.Generation != Token.Generation) continue;
        if (ActiveTerminalOperation) return false;
        TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, &Plan);
        if (Phase == SoloASBridge::ERuntimeTerminalDestroyPhaseForTest::CancelPending)
        {
            if (Plan.Phase != ETerminalPlanPhase::Preparing && Plan.Phase != ETerminalPlanPhase::Retired)
            {
                FailTerminalPlan(Plan, TEXT("TERMINAL_CANCEL_AFTER_DEFERRAL_NO_RETRY"));
                return false;
            }
            SoloASBridge::DisarmRuntimeTerminalDestroyForTest(Plan.Token);
            ReleaseTerminalPlan(Plan);
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_CANCEL object=%p sequence=%llu epoch=%llu stage=api_pending module_pin=0 replacement=0"),
                Token.Object, Token.TrackSequence, Token.ArmEpoch);
            return true;
        }
        if (Plan.Phase == ETerminalPlanPhase::Retired) return false;
        const bool bAfterOriginal = Phase == SoloASBridge::ERuntimeTerminalDestroyPhaseForTest::AfterOriginalFieldDestroy;
        const bool bPlanValid = GExitPurge && (bAfterOriginal
            ? ValidateTerminalFieldSetForCallback(Plan) : ValidateTerminalPlan(Plan, false));
        bool bMirrorMatches = true;
        if (bPlanValid && !bAfterOriginal)
        {
            uint64 Sequence = 0;
            bMirrorMatches = AngelscriptRuntimeGcMirror::ReadRuntimeDestroyTracking(
                {Token.Key, Token.Object, Token.TypeCookie, Token.Generation, Token.ClaimSequence}, Sequence)
                && Sequence == Token.TrackSequence;
        }
        if (!bPlanValid || !Token.Object->IsUnreachable() || !bMirrorMatches)
        {
            if (Plan.Phase == ETerminalPlanPhase::Deferred)
            {
                FailTerminalPlan(Plan, TEXT("TERMINAL_INVALID_AFTER_DEFERRAL_NO_RETRY"));
                return false;
            }
            SoloASBridge::DisarmRuntimeTerminalDestroyForTest(Plan.Token);
            ReleaseTerminalPlan(Plan);
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display, TEXT("SOLOAS_TERMINAL_REJECT object=%p stage=before_claim fallback_early=1"), Token.Object);
            return false;
        }
        if (!bAfterOriginal)
        {
            Plan.Phase = ETerminalPlanPhase::Deferred;
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_DEFER object=%p sequence=%llu epoch=%llu field_last=1 chain_nodes=%u module_pin=1 before_claim=1"),
                Token.Object, Token.TrackSequence, Token.ArmEpoch, Plan.ChainLength);
        }
        return true;
    }
    return false;
}
#endif
}

void RetireRemovedRuntimeClass(UClass* Class)
{
    if (!Class) return;
    auto* Owner = Cast<UASClass>(Class);
    if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread() || RemovingRuntimeClass
        || !Owner || !Owner->ScriptTypePtr || Owner->NewerVersion
        || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Owner->ScriptTypePtr))
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REMOVED_CLASS_TRANSITION_INVALID"));
    TGuardValue<UASClass*> RemovingGuard(RemovingRuntimeClass, Owner);
    AngelscriptRuntimeGcMirror::DestroyRuntimeTrackedObjectsForClass(Class, true);
}

void RecordRemovedRuntimeReceiver(UObject* Object, asITypeInfo* Type, asIScriptModule* Module, uint64 Sequence)
{
    // The existing mirror retention callback also witnesses removed -> replaced. It may
    // change only the role/lineage of an existing record, never create or renew its old tuple.
    if (!RemovingRuntimeClass)
    {
        if (!Object || !IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread()) return;
        auto* Record = RemovedRuntimeAssociations.Find(FObjectKey(Object));
        if (!Record) return;
        if (Object->IsTemplate() || Object->IsUnreachable() || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REMOVED_REPLACEMENT_RECEIVER_INVALID"));
        if (Record->Role == ERemovedRuntimeAssociationRole::Removed
            && ValidateRemovedRuntimeAssociation(*Record, Object, Record->Class, Type, Module, Sequence)) return;
        if (Record->Role == ERemovedRuntimeAssociationRole::Replaced)
        {
            if (ValidateOrdinaryRemovedTuple(*Record, Object, Record->Class, Type, Module, Sequence)) return;
            if (!ValidateRemovedRuntimeAssociation(*Record, Object, Record->Class, Type, Module, Sequence)
                || !Record->SourceGeneration
                || Cuviss::TerminalAdmission::ReadRetainedSourceGeneration(Type) != Record->SourceGeneration)
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REMOVED_REPLACEMENT_IDENTITY_CHANGED"));
            return;
        }
        FObjectKey ReplacementKey;
        if (!CaptureRemovedRuntimeReplacement(*Record, Object, Type, Module, Sequence, ReplacementKey)
            && !CaptureOrdinaryRemovedRuntimeReplacement(*Record, Object, Type, Module, Sequence, ReplacementKey))
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REMOVED_REPLACEMENT_TRANSITION_INVALID"));
        Record->ReplacementKey = ReplacementKey;
        Record->Replacement = Record->Class->NewerVersion;
        Record->Role = ERemovedRuntimeAssociationRole::Replaced;
        return;
    }
    auto* Owner = RemovingRuntimeClass;
    if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread() || !Object || !Sequence || !Module
        || Object->IsTemplate() || Object->IsUnreachable() || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
        || UASClass::GetFirstASClass(Object->GetClass()) != Owner || Owner->bLayoutChanging
        || Owner->ScriptTypePtr != Type || Owner->NewerVersion
        || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Type)
        || Type->GetModule() != Module || Type->GetUserData() != Owner || !UASClass::RuntimeLifecycleGeneration)
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REMOVED_RECEIVER_TRANSITION_INVALID"));
    FRemovedRuntimeAssociation Record;
    Record.ObjectKey = FObjectKey(Object);
    Record.ClassKey = FObjectKey(Owner);
    Record.Object = Object;
    Record.Class = Owner;
    Record.Type = Type;
    Record.Module = Module;
    Record.Engine = Type->GetEngine();
    Record.Sequence = Sequence;
    Record.LifecycleGeneration = UASClass::RuntimeLifecycleGeneration;
    Record.SourceGeneration = Cuviss::TerminalAdmission::ReadRetainedSourceGeneration(Type);
    Record.PropertiesSize = Owner->GetPropertiesSize();
    {
        FScopeLock Lock(&TerminalCompilationMutex);
        if (bTerminalCompilationInProgress && !bTerminalInitialCompilation
            && TerminalCompilationThread == FPlatformTLS::GetCurrentThreadId())
        {
            Record.RemovalCompilationEpoch = TerminalCompilationEpoch;
            Record.RemovalCompilationThread = TerminalCompilationThread;
        }
    }
    if (RemovedRuntimeAssociations.Contains(Record.ObjectKey))
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REMOVED_RECEIVER_DUPLICATE_TRANSITION"));
    RemovedRuntimeAssociations.Add(Record.ObjectKey, Record);
}

bool IsRetiredRuntimeReceiverIdentity(UObject* Object, asITypeInfo* Type, asIScriptModule* Module,
    const UASClass* Class, uint64 Sequence)
{
    if (!Object) return false;
    const auto* Removed = RemovedRuntimeAssociations.Find(FObjectKey(Object));
    if (!Removed) return Class && AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Type)
        && AngelscriptRuntimeGcMirror::IsRetiredRuntimeTypeIdentity(Type, Module, Class);
    // Permit the existing retention branch to reach its recording callback, but do not
    // grant preclaim authority here. The original replacement predicate cannot bypass a record.
    FObjectKey ReplacementKey;
    if (Removed->Class == Class
        && (ValidateRemovedRuntimeAssociation(*Removed, Object, Class, Type, Module, Sequence)
            || CaptureRemovedRuntimeReplacement(*Removed, Object, Type, Module, Sequence, ReplacementKey)
            || CaptureOrdinaryRemovedRuntimeReplacement(*Removed, Object, Type, Module, Sequence, ReplacementKey))) return true;
    // False would select explicit destruction in the caller, before full terminal proof.
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REMOVED_RETENTION_IDENTITY_INVALID_NO_FALLBACK"));
    return false;
}

void ForgetRemovedRuntimeReceiver(UObject* Object, asITypeInfo* Type)
{
    check(IsInGameThread() && IsAngelscriptExecutionWriterHeldByCurrentThread());
    const FObjectKey Key(Object);
    const auto* Record = RemovedRuntimeAssociations.Find(Key);
    if (!Record) return;
    if (Record->Object != Object || Record->Type != Type)
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REMOVED_RECEIVER_RELEASE_MISMATCH"));
    // A completing plan already owns a copy and its own module lease for the final typed cleanup.
    RemovedRuntimeAssociations.Remove(Key);
}

bool ValidateOrdinaryRemovedRuntimeCallback(UObject* Object, UASClass* Owner, asITypeInfo* Type)
{
    using namespace AngelscriptRuntimeGcMirror;
    if (!Object || !Owner || !Type) return false;
    FRuntimeTrackedObject Snapshot;
    {
        FScopeLock Lock(&RuntimeTrackedObjectsMutex);
        const auto* Entry = RuntimeTrackedObjects.Find(FObjectKey(Object));
        // Every existing CallDestructor caller owns a tracked mirror claim. Losing
        // that claim during a log callout cannot grant an untracked callback.
        if (!Entry) return false;
        Snapshot = *Entry;
    }
    // Release the mirror mutex before class/type metadata or source registry access.
    auto* Record = RemovedRuntimeAssociations.Find(Snapshot.Key);
    // Preserve the existing state/owner policy for unrelated ordinary callbacks.
    if (!Record && !Snapshot.bOrdinaryRemovedAdmissionRequired) return true;
    if (!Record || !IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread()
        || Snapshot.ObjectIdentity != Object || Snapshot.SavedTypeCookie != Type
        || !Snapshot.TrackSequence || Snapshot.State != ERuntimeMirrorState::Dispatching
        || !Snapshot.bOrdinaryRemovedAdmissionRequired || Snapshot.TerminalPlanId || !Object->IsUnreachable()
        || !Record->bOrdinaryAdmitted || Record->bOrdinaryConsumed
        || Snapshot.OrdinaryRemovedAdmissionCollectionEpoch != Record->OrdinaryCollectionEpoch
        || !ValidateOrdinaryRemovedTuple(*Record, Object, Owner, Type, Snapshot.RetainedModule, Snapshot.TrackSequence)) return false;
    {
        FScopeLock Lock(&TerminalCompilationMutex);
        if (TerminalPlanReservations || (Record->OrdinaryCompilationEpoch
            ? (!IsReinstanceCollectionCurrent(Record->OrdinaryCollectionEpoch)
                || Record->OrdinaryCompilationEpoch != TerminalCompilationEpoch)
            : (bTerminalCompilationInProgress || ReinstanceCollectionEpoch))) return false;
    }
    Record->bOrdinaryConsumed = true;
    return true;
}

bool ValidateRuntimeTerminalSourceAssociation(void* Object, asITypeInfo* Type, uint64 PlanId, uint64 SourceGeneration)
{
    if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread() || !Object || !Type || !PlanId) return false;
    const FTerminalDestroyPlan* Plan = ActiveTerminalOperation;
    if (!Plan || Plan->Token.Object != Object || Plan->Token.PlanId != PlanId)
    {
        const auto* Found = TerminalPlans.Find(FObjectKey(static_cast<UObject*>(Object)));
        Plan = Found ? Found->Get() : nullptr;
    }
    if (!Plan || Plan->Phase == ETerminalPlanPhase::Retired || Plan->Token.Object != Object
        || Plan->Token.TypeCookie != Type || Plan->Token.PlanId != PlanId
        || !SameTerminalItem(Plan->ReceiverIndex, Plan->ReceiverSerial, Plan->Token.Object)
        || !SameTerminalItem(Plan->ClassIndex, Plan->ClassSerial, Plan->Class)
        || (Plan->ActualReceiverClass != Plan->Class && !ValidateTerminalActualReceiverMetadata(*Plan))
        || ((Plan->Phase == ETerminalPlanPhase::Preparing || Plan->Phase == ETerminalPlanPhase::Prepared)
            && Plan->Token.Object->GetClass() != Plan->ActualReceiverClass) || Plan->Class->bLayoutChanging
        || Plan->Class->GetPropertiesSize() != Plan->PropertiesSize) return false;
    const uint64 Expected = Plan->RemovedAssociation.IsSet() ? Plan->RemovedAssociation->SourceGeneration : 0;
    // Admission already holds its registry mutex and separately checks this exact source generation.
    return SourceGeneration == Expected && ValidateTerminalOwnerAssociation(*Plan, Plan->Class, Type, false);
}

bool Cuviss::TerminalAdmission::ValidateRuntimeTerminalInt32Slot(const FReceiverShape& Shape, bool bRequireFresh, bool bAllowEnded)
{
    if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread()) return false;
    const FTerminalDestroyPlan* Plan = ActiveTerminalOperation;
    if (!Plan || Plan->Token.Object != Shape.Receiver || Plan->Token.PlanId != Shape.PlanId)
    {
        Plan = nullptr;
        for (const auto& Pair : TerminalPlans)
            if (Pair.Value->Token.Object == Shape.Receiver && Pair.Value->Token.PlanId == Shape.PlanId)
            {
                Plan = Pair.Value.Get();
                break;
            }
    }
    if (!Plan || Plan->bDevFacade || Plan->Phase == ETerminalPlanPhase::Retired
        || Plan->Token.TypeCookie != Shape.OwnerCookie || !(Plan->Int32Slot == Shape.Int32Slot)
        || !ValidateTerminalShapeDeclaration(*Plan, Shape) || !ValidateTerminalInt32SlotMetadata(*Plan)) return false;
    if (!Shape.Int32Slot.Property) return !bRequireFresh;
    if (Shape.Int32Slot.FinalProviderEpoch && !IsTerminalCapturedInt32Slot(Shape.Int32Slot)) return false;
    // Cleanup ends the scalar before the owned wrapper. A completed proof may
    // still verify the ended slot's identity, but may never authorize a read.
    if (Plan->Int32Phase == EInt32SlotPhase::FreshEnded)
        return bAllowEnded && !bRequireFresh && Plan->Phase == ETerminalPlanPhase::Cleaning
            && !Plan->FreshInt32 && !Plan->FreshInt32Epoch && Plan->OriginalCompleted == 1
            && Plan->EmptyConstructed == 1 && Plan->EmptyDestroyed == 0;
    if (Plan->Int32Phase == EInt32SlotPhase::FreshLive)
        return Plan->FreshInt32 == Shape.Int32Slot.Storage && Plan->FreshInt32Epoch == Shape.Int32Slot.Epoch
            && (!Shape.Int32Slot.bRuntimeOwnedOriginal
                || (Plan->FinalInt32Epoch == Shape.Int32Slot.Epoch && Plan->bRuntimeOwnedFieldReturned))
            && Plan->OriginalCompleted == 1 && Plan->EmptyConstructed == 1 && Plan->EmptyDestroyed == 0
            && (Plan->Phase == ETerminalPlanPhase::Deferred || Plan->Phase == ETerminalPlanPhase::Completing
                || Plan->Phase == ETerminalPlanPhase::Callback || Plan->Phase == ETerminalPlanPhase::Cleaning);
    if (bRequireFresh || Plan->FreshInt32 || Plan->FreshInt32Epoch) return false;
    return (Plan->Int32Phase == EInt32SlotPhase::OriginalLive
            && (Plan->Phase == ETerminalPlanPhase::Preparing || Plan->Phase == ETerminalPlanPhase::Prepared
                || (Shape.Int32Slot.bRuntimeOwnedOriginal && HasRuntimeOwnedTerminalScalar(*Plan)
                    && Plan->Phase == ETerminalPlanPhase::Deferred && Plan->bRuntimeOwnedFieldReturned
                    && Plan->FinalInt32Provider && Plan->FinalInt32Provider->Registration.bRuntimeOwnedOriginal
                    && Plan->Fields.Num() == 1
                    && ((Plan->Fields[0].Phase == ETerminalFieldPhase::OriginalEnded
                            && Plan->OriginalCompleted == 0 && Plan->EmptyConstructed == 0)
                        || (Plan->Fields[0].Phase == ETerminalFieldPhase::FreshLive
                            && Plan->OriginalCompleted == 1 && Plan->EmptyConstructed == 1))
                    && Plan->EmptyDestroyed == 0)))
        || (Plan->Int32Phase == EInt32SlotPhase::OriginalEnded && Plan->Phase == ETerminalPlanPhase::Deferred
            && (!Shape.Int32Slot.bRuntimeOwnedOriginal
                || (Plan->bRuntimeOwnedFieldReturned && Plan->FinalInt32Epoch == Shape.Int32Slot.Epoch)));
}

bool Cuviss::TerminalAdmission::ValidateRuntimeTerminalInt32InitialValue(const FReceiverShape& Shape)
{
    if (!Shape.Int32Slot.Property) return ValidateRuntimeTerminalInt32Slot(Shape, false);
    // ValidateRuntimeTerminalInt32Slot binds the plan, allocation, metadata and
    // separate fresh epoch. Only the new lifetime is read at this execution fence.
    const auto* Plan = ActiveTerminalOperation;
    const int32 Expected = Plan && Plan->FinalInt32Provider ? Plan->FinalInt32Delivery : 0;
    if (Shape.Int32Slot.bRuntimeOwnedOriginal && (!Plan || Plan->FinalInt32Epoch != Shape.Int32Slot.Epoch)) return false;
    return ValidateRuntimeTerminalInt32Slot(Shape, true)
        && *static_cast<const int32*>(Shape.Int32Slot.Storage) == Expected;
}

bool Cuviss::TerminalAdmission::ValidateTerminalFreshStorage(const FReceiverShape& Shape, uint64 Epoch)
{
    const auto* Plan = ActiveRuntimeTerminalPlan;
    if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread() || !Plan || !Epoch
        || Plan->bDevFacade || Plan->Phase != ETerminalPlanPhase::Callback || Plan->Token.Object != Shape.Receiver
        || Plan->Token.TypeCookie != Shape.OwnerCookie || Plan->Token.PlanId != Shape.PlanId
        || !ValidateTerminalShapeDeclaration(*Plan, Shape) || !ValidateTerminalOwners(*Plan)
        || Plan->Fields.Num() != 1 || !Plan->ModulePin || Plan->OriginalCompleted != 1
        || Plan->EmptyConstructed != 1 || Plan->EmptyDestroyed
        || !SameTerminalItem(Plan->ReceiverIndex, Plan->ReceiverSerial, Plan->Token.Object)
        || !SameTerminalItem(Plan->ClassIndex, Plan->ClassSerial, Plan->Class)
        || (Plan->ActualReceiverClass != Plan->Class && !ValidateTerminalActualReceiverMetadata(*Plan))) return false;
    const auto& Field = Plan->Fields[0];
    return Field.Phase == ETerminalFieldPhase::FreshLive && Field.Storage == Shape.FieldStorage
        && Field.Fresh == Shape.FieldStorage && Field.Offset == Shape.FieldOffset
        && Field.FreshEpoch == Epoch && Epoch == Plan->Token.ArmEpoch;
}

FAngelscriptRuntimeGcCompilationScope::FAngelscriptRuntimeGcCompilationScope(bool bInitialCompile)
{
    if (!bInitialCompile && !IsInGameThread()) return;
    FScopeLock Lock(&TerminalCompilationMutex);
    if (!bTerminalCompilationInProgress && TerminalPlanReservations == 0)
    {
        if (++TerminalCompilationEpoch == 0)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_COMPILATION_EPOCH_EXHAUSTED"));
        TerminalCompilationThread = FPlatformTLS::GetCurrentThreadId();
        bTerminalInitialCompilation = bInitialCompile;
        bTerminalCompilationInProgress = true;
        bAdmitted = true;
    }
}

FAngelscriptRuntimeGcCompilationScope::~FAngelscriptRuntimeGcCompilationScope() noexcept
{
    if (!bAdmitted) return;
    FScopeLock Lock(&TerminalCompilationMutex);
    if (TerminalCompilationThread != FPlatformTLS::GetCurrentThreadId()
        || ReinstanceCollectionEpoch || TerminalPlanReservations)
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_COMPILER_RESUMED_WITH_BORROWED_WORK"));
    bTerminalCompilationInProgress = false;
    TerminalCompilationThread = 0;
    bTerminalInitialCompilation = false;
}

FAngelscriptRuntimeReinstanceGcScope::FAngelscriptRuntimeReinstanceGcScope(
    const TMap<UClass*, UClass*>& ReloadClasses, bool bBeforeReinstance)
{
    // No multicast ordering assumption: this scope encloses only the synchronous full-purge call.
    // The existing compiler writer remains held; neither the barrier nor source generations are reset.
    {
        FScopeLock Lock(&TerminalCompilationMutex);
        if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread()
            || !bTerminalCompilationInProgress || bTerminalInitialCompilation
            || TerminalCompilationThread != FPlatformTLS::GetCurrentThreadId()
            || !TerminalCompilationEpoch || ReinstanceCollectionEpoch || TerminalPlanReservations
            || ActiveTerminalOperation || ActiveRuntimeTerminalPlan)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REINSTANCE_COLLECTION_ENTRY_UNPROVED"));
        if (++NextReinstanceCollectionEpoch == 0)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REINSTANCE_COLLECTION_EPOCH_EXHAUSTED"));
        Epoch = ReinstanceCollectionEpoch = NextReinstanceCollectionEpoch;
        ReinstanceCompilationEpoch = TerminalCompilationEpoch;
        bPreReinstanceCollection = bBeforeReinstance;
    }
    if (!ReinstanceCollectionReceivers.IsEmpty())
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REINSTANCE_COLLECTION_STALE_RECEIVERS"));
    if (bBeforeReinstance) return;
    for (auto& Entry : RemovedRuntimeAssociations)
    {
        auto& Record = Entry.Value;
        UClass* const* Replacement = ReloadClasses.Find(Record.Class);
        if (!Replacement) continue;
        if (HasOrdinaryRemovedFieldShape(Record))
        {
            uint64 Sequence = 0;
            asIScriptModule* Module = nullptr;
            FObjectKey ReplacementKey = Record.ReplacementKey;
            if (Entry.Key != Record.ObjectKey
                || !AngelscriptRuntimeGcMirror::ReadRuntimeDestroyTracking(
                    {Record.ObjectKey, Record.Object, Record.Type, 0, 0}, Sequence, &Module)
                || Sequence != Record.Sequence || Module != Record.Module
                || (Record.Role == ERemovedRuntimeAssociationRole::Replaced
                    ? !ValidateRemovedRuntimeAssociation(Record, Record.Object, Record.Class, Record.Type, Module, Sequence)
                    : !CaptureOrdinaryRemovedRuntimeReplacement(Record, Record.Object, Record.Type, Module, Sequence, ReplacementKey))
                || ReplacementKey.ResolveObjectPtrEvenIfUnreachable() != *Replacement)
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("ORDINARY_REMOVED_REINSTANCE_LINEAGE_INVALID"));
            Record.ReplacementKey = ReplacementKey;
            Record.Replacement = Record.Class->NewerVersion;
            Record.Role = ERemovedRuntimeAssociationRole::Replaced;
            continue;
        }
        uint64 Sequence = 0;
        asIScriptModule* Module = nullptr;
        FObjectKey ReplacementKey;
        auto Candidate = Record;
        // A previously transitioned record must still prove the same replacement serial.
        if (Candidate.Role == ERemovedRuntimeAssociationRole::Replaced
            && !ValidateRemovedRuntimeAssociation(Candidate, Candidate.Object, Candidate.Class,
                Candidate.Type, Candidate.Module, Candidate.Sequence))
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REINSTANCE_REPLACEMENT_RECORD_INVALID"));
        Candidate.Role = ERemovedRuntimeAssociationRole::Removed;
        if (Entry.Key != Record.ObjectKey
            || !AngelscriptRuntimeGcMirror::ReadRuntimeDestroyTracking(
                {Record.ObjectKey, Record.Object, Record.Type, 0, 0}, Sequence, &Module)
            || Sequence != Record.Sequence || Module != Record.Module
            || !CaptureRemovedRuntimeReplacement(Candidate, Record.Object, Record.Type, Module, Sequence, ReplacementKey)
            || ReplacementKey.ResolveObjectPtrEvenIfUnreachable() != *Replacement)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REINSTANCE_REMOVED_SOURCE_PAIR_UNPROVED"));
        Candidate.ReplacementKey = ReplacementKey;
        Candidate.Replacement = Candidate.Class->NewerVersion;
        Candidate.Role = ERemovedRuntimeAssociationRole::Replaced;
        Record = Candidate;
        ReinstanceCollectionReceivers.Add(Entry.Key,
            {Candidate, Candidate.Replacement->ScriptTypePtr, Candidate.Replacement->ScriptTypePtr->GetModule(), false});
    }
}

FAngelscriptRuntimeReinstanceGcScope::~FAngelscriptRuntimeReinstanceGcScope() noexcept
{
    {
        FScopeLock Lock(&AngelscriptRuntimeGcMirror::RuntimeTrackedObjectsMutex);
        for (const auto& Entry : AngelscriptRuntimeGcMirror::RuntimeTrackedObjects)
            if (Entry.Value.bOrdinaryRemovedAdmissionRequired
                && Entry.Value.OrdinaryRemovedAdmissionCollectionEpoch == Epoch)
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("ORDINARY_REMOVED_COLLECTION_RETURN_WITH_LIVE_MIRROR"));
    }
    for (const auto& Entry : RemovedRuntimeAssociations)
        if (Entry.Value.bOrdinaryAdmitted && !Entry.Value.bOrdinaryConsumed
            && Entry.Value.OrdinaryCollectionEpoch == Epoch)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("ORDINARY_REMOVED_COLLECTION_RETURN_WITH_UNCONSUMED_ADMISSION"));
    // Plans release context, typed fresh values and their existing module lease before the reservation.
    // A deferred plan surviving this full-purge call must never resume the metadata writer.
    {
        FScopeLock Lock(&TerminalCompilationMutex);
        if (!IsReinstanceCollectionCurrent(Epoch) || TerminalPlanReservations
            || ActiveTerminalOperation || ActiveRuntimeTerminalPlan)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("REINSTANCE_COLLECTION_RETURN_WITH_LIVE_WORK"));
        ReinstanceCollectionEpoch = 0;
        ReinstanceCompilationEpoch = 0;
        bPreReinstanceCollection = false;
    }
    ReinstanceCollectionReceivers.Empty();
}

bool IsAngelscriptRuntimeGcCompilationAvailable()
{
    if (!IsInGameThread()) return false;
    FScopeLock Lock(&TerminalCompilationMutex);
    return !bTerminalCompilationInProgress && TerminalPlanReservations == 0;
}

void QueueAngelscriptRuntimeGcCompilationRetry()
{
    if (!IsInGameThread()) return;
    FScopeLock Lock(&TerminalCompilationMutex);
    bTerminalCompilationRetryPending = true;
}

bool HasAngelscriptRuntimeGcCompilationRetry()
{
    if (!IsInGameThread()) return false;
    FScopeLock Lock(&TerminalCompilationMutex);
    return bTerminalCompilationRetryPending;
}

bool ConsumeAngelscriptRuntimeGcCompilationRetry()
{
    if (!IsInGameThread()) return false;
    FScopeLock Lock(&TerminalCompilationMutex);
    if (!bTerminalCompilationRetryPending || bTerminalCompilationInProgress || TerminalPlanReservations != 0)
        return false;
    bTerminalCompilationRetryPending = false;
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
bool ArmRuntimeTerminalDestroyForTest(UObject* const (&Objects)[2], const FStructProperty& Property, uint64 ArmEpoch)
{
    if (!IsInGameThread() || !Objects[0] || !Objects[1] || Objects[0] == Objects[1]
        || !ArmEpoch || ArmEpoch <= LastTerminalArmEpoch || ActiveTerminalOperation || ActiveRuntimeTerminalPlan) return false;
    auto Reservation = MakeUnique<FTerminalPlanReservation>();
    if (!Reservation->IsAcquired()) return false;
    FAngelscriptExecutionWriteScope Writer;
    for (const auto& Pair : TerminalPlans)
        if (Pair.Value->Phase != ETerminalPlanPhase::Retired || Pair.Value->ModulePin) return false;
    TerminalPlans.Reset();
    LastTerminalArmEpoch = ArmEpoch;
    for (int32 Index = 0; Index != 2; ++Index)
    {
        const FObjectKey Key(Objects[Index]);
        auto& HeapPlan = TerminalPlans.Add(Key, MakeUnique<FTerminalDestroyPlan>());
        auto& Plan = *HeapPlan;
        if (Index == 0) Plan.CompilationReservation = MoveTemp(Reservation);
        else Plan.CompilationReservation = MakeUnique<FTerminalPlanReservation>();
        if (!Plan.CompilationReservation->IsAcquired()) break;
        TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, &Plan);
        Plan.bDevFacade = true;
        Plan.Token.Object = Objects[Index];
        Plan.Token.ArmEpoch = ArmEpoch;
        if (++NextTerminalPlanId == 0) return false;
        Plan.Token.PlanId = NextTerminalPlanId;
        asITypeInfo* Type = nullptr;
        uint64 Sequence = 0;
        if (!AngelscriptRuntimeGcMirror::ObserveRuntimeTrackedMirrorCookie(Objects[Index], Type)
            || !AngelscriptRuntimeGcMirror::ReadRuntimeDestroyTracking({Key, Objects[Index], Type, 0, 0}, Sequence)
            || CaptureTerminalPlan(Plan, Objects[Index], Property, Type, Sequence) != ETerminalCaptureResult::Captured) break;
        SoloASBridge::FRuntimeTerminalDestroyTokenForTest ArmedToken;
        if (!SoloASBridge::ArmRuntimeTerminalDestroyForTest(Objects[Index], Type, Sequence,
                ArmEpoch, &RevalidateTerminalDestroy, ArmedToken)) break;
        Plan.Token.Generation = ArmedToken.Generation;
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_ARM slot=%d object=%p sequence=%llu epoch=%llu field=%p field_last=1 chain_nodes=%u module_pin=1 code_words=%u"),
            Index, Objects[Index], Plan.Token.TrackSequence, ArmEpoch, &Property, Plan.ChainLength, Plan.ByteCodeLength);
        if (Plan.Fields.Num() > 1)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_FIELD_SET slot=%d object=%p fields=%d owners=%d last=%p module_pin=1"),
                Index, Objects[Index], Plan.Fields.Num(), Plan.Owners.Num(), Plan.Property);
        if (Index == 1) return true;
    }
    for (auto& Pair : TerminalPlans)
    {
        auto& Plan = *Pair.Value;
        TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, &Plan);
        if (Plan.Token.Generation) SoloASBridge::DisarmRuntimeTerminalDestroyForTest(Plan.Token);
        ReleaseTerminalPlan(Plan);
    }
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display, TEXT("SOLOAS_TERMINAL_ARM_REJECT both_armed=0"));
    return false;
}

void DisarmRuntimeTerminalDestroyForTest()
{
    check(IsInGameThread());
    if (ActiveTerminalOperation) return;
    FAngelscriptExecutionWriteScope Writer;
    for (auto& Pair : TerminalPlans)
    {
        auto& Plan = *Pair.Value;
        TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, &Plan);
        if (Plan.Phase != ETerminalPlanPhase::Preparing && Plan.Phase != ETerminalPlanPhase::Retired)
        {
            FailTerminalPlan(Plan, TEXT("TERMINAL_DISARM_AFTER_DEFERRAL_NO_RETRY"));
            return;
        }
        if (Plan.Phase == ETerminalPlanPhase::Preparing) SoloASBridge::DisarmRuntimeTerminalDestroyForTest(Plan.Token);
        ReleaseTerminalPlan(Plan);
    }
}

bool InjectRuntimeTerminalFieldFaultForTest(UObject* Receiver, const FStructProperty& FirstProperty,
    const void* ValueAddress, uint64 ArmEpoch, EAngelscriptTerminalFieldFaultForTest Fault)
{
    if (!IsInGameThread() || !GExitPurge || !Receiver || !ValueAddress || !ArmEpoch
        || ActiveTerminalOperation || ActiveRuntimeTerminalPlan || SoloASBridge::IsTerminalRuntimeDestroyCallbackForTest()
        || (Fault != EAngelscriptTerminalFieldFaultForTest::DuplicateCompletion
            && Fault != EAngelscriptTerminalFieldFaultForTest::WrongPropertyCompletion)) return false;
    FAngelscriptExecutionWriteScope Writer;
    for (auto& Pair : TerminalPlans)
    {
        auto& Plan = *Pair.Value;
        if (Plan.Token.Object != Receiver) continue;
        if (Plan.Phase != ETerminalPlanPhase::Deferred || Plan.bFaultInjected
            || Plan.Token.ArmEpoch != ArmEpoch || Plan.Fields.Num() != 2 || Plan.OriginalCompleted != 1
            || Plan.EmptyConstructed != 1 || Plan.EmptyDestroyed != 0 || !ValidateTerminalPlan(Plan, true)) return false;
        auto& First = Plan.Fields[0];
        const auto& Last = Plan.Fields[1];
        if (First.Property != &FirstProperty || First.Storage != ValueAddress
            || First.Phase != ETerminalFieldPhase::FreshLive || First.Fresh != First.Storage || First.FreshEpoch != ArmEpoch
            || Last.Phase != ETerminalFieldPhase::OriginalLive || Last.Fresh || Last.Property != Plan.Property
            || First.Property == Last.Property || First.Owner == Last.Owner
            || !CanCleanupTerminalFreshField(Plan, First)) return false;
        const bool Duplicate = Fault == EAngelscriptTerminalFieldFaultForTest::DuplicateCompletion;
        const FStructProperty* Incoming = Duplicate ? First.Property : Last.Property;
        Plan.bFaultInjected = true;
        FInstancedStruct* Fresh = First.Fresh;
        First.Fresh = nullptr;
        First.Phase = ETerminalFieldPhase::FreshEnding;
        {
            TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, &Plan);
            Fresh->~FInstancedStruct();
        }
        First.Phase = ETerminalFieldPhase::FreshEnded;
        // Completion consumes identities only; the ended wrapper must never be read or cleaned again.
        AngelscriptRuntimeGcMirror::CompleteRuntimeTerminalFieldDestroy(*Incoming, First.Storage);
        return false;
    }
    return false;
}
#endif

namespace
{
bool CompleteTerminalScalarLifetime(FTerminalDestroyPlan& Plan)
{
    if (Plan.Int32Slot.Property)
    {
        // Native destruction and the owned lifetime have ended; the retained
        // allocation/metadata fence precedes an independent scalar lifetime.
        if (!ValidateTerminalPlan(Plan, true) || !Plan.Qualification
            || !Cuviss::TerminalAdmission::ValidatePreparedCompiledTerminalDestructor(
                *Plan.Qualification, TerminalReceiverShape(Plan)))
        {
            FailTerminalPlan(Plan, TEXT("TERMINAL_INT32_PLACEMENT_PROOF_FAILED_NO_RETRY"));
            return false;
        }
        if (Plan.FinalInt32Provider && !Cuviss::TerminalAdmission::IsTerminalCapturedInt32Slot(Plan.Int32Slot))
        {
            FailTerminalPlan(Plan, TEXT("TERMINAL_INT32_CAPTURE_MISSING_NO_RETRY"));
            return false;
        }
        if (Plan.Int32Slot.bRuntimeOwnedOriginal)
        {
            // All logs, native destruction and context refresh precede this
            // fence. The success suffix is continuous and has no callout.
            if (!ValidateRuntimeOwnedTerminalInt32LateFence(Plan))
            {
                FailTerminalPlan(Plan, TEXT("TERMINAL_RUNTIME_OWNED_INT32_LATE_FENCE_FAILED_NO_READ"));
                return false;
            }
            int32* const OriginalLive = static_cast<int32*>(Plan.Int32Slot.Storage);
            const int32 FinalValue = *OriginalLive;
            Plan.FinalInt32Delivery = bTerminalStaleCaptureControl ? Plan.Int32Slot.CapturedValue : FinalValue;
            Plan.FinalInt32Epoch = Plan.Int32Slot.Epoch;
            using FCompleteTerminalInt32 = int32;
            OriginalLive->~FCompleteTerminalInt32();
            Plan.Int32Phase = Cuviss::TerminalAdmission::EInt32SlotPhase::OriginalEnded;
            Plan.FreshInt32 = ::new (Plan.Int32Slot.Storage) int32(Plan.FinalInt32Delivery);
            Plan.FreshInt32Epoch = Plan.FinalInt32Epoch;
            Plan.Int32Phase = Cuviss::TerminalAdmission::EInt32SlotPhase::FreshLive;
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_RUNTIME_OWNED_HANDOFF object=%p plan=%llu provider_epoch=%llu slot_epoch=%llu ")
                TEXT("captured=%d final=%d delivery=%d original_live_read=1 runtime_typed_end=1 owned_returned=1 ")
                TEXT("original_ended_after_read=1 fresh_after_end=1 stale_control=%d"),
                Plan.Token.Object, Plan.Token.PlanId, Plan.Int32Slot.FinalProviderEpoch, Plan.FinalInt32Epoch,
                Plan.Int32Slot.CapturedValue, FinalValue, Plan.FinalInt32Delivery, bTerminalStaleCaptureControl ? 1 : 0);
        }
        else
        {
            const int32 InitialValue = Plan.FinalInt32Provider ? Plan.FinalInt32Delivery : 0;
            Plan.FreshInt32 = ::new (Plan.Int32Slot.Storage) int32(InitialValue);
            Plan.FreshInt32Epoch = Plan.Int32Slot.Epoch;
            Plan.Int32Phase = Cuviss::TerminalAdmission::EInt32SlotPhase::FreshLive;
        }
        const int32 InitialValue = Plan.FinalInt32Provider ? Plan.FinalInt32Delivery : 0;
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_INT32_BEGIN object=%p offset=%d width=%u epoch=%llu old_read=0 captured=%d initial=%d capture_epoch=%llu"),
            Plan.Token.Object, Plan.Int32Slot.Offset, uint32(sizeof(int32)), Plan.FreshInt32Epoch,
            Plan.FinalInt32Provider ? 1 : 0, InitialValue, Plan.Int32Slot.CapturedEpoch);
    }
    return true;
}

void CompleteTerminalReadyPlan(FTerminalDestroyPlan& Plan)
{
    if (Plan.OriginalCompleted != uint32(Plan.Fields.Num()) || !ValidateTerminalPlan(Plan, true))
    {
        FailTerminalPlan(Plan, TEXT("TERMINAL_FIELD_SET_INCOMPLETE_NO_RETRY"));
        return;
    }
    for (const auto& Ready : Plan.Fields)
        if (Ready.Phase != ETerminalFieldPhase::FreshLive || Ready.Fresh != Ready.Storage
            || Ready.FreshEpoch != Plan.Token.ArmEpoch || !CanCleanupTerminalFreshField(Plan, Ready) || Ready.Fresh->IsValid())
        {
            FailTerminalPlan(Plan, TEXT("TERMINAL_EMPTY_LIFETIME_MISSING_NO_RETRY"));
            return;
        }
    if (Plan.Fields.Num() > 1)
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_FIELD_SET_FENCE object=%p fields=%d original_fields=%u empty_construct=%u "
                 "empty_destroy=0 last=%p chain_nodes=%u epoch=%llu callback=0 cert_inactive=1 module_pin=1"),
            Plan.Token.Object, Plan.Fields.Num(), Plan.OriginalCompleted, Plan.EmptyConstructed,
            Plan.Property, Plan.ChainLength, Plan.Token.ArmEpoch);
    bool bCompleted = false;
    {
        TGuardValue<FTerminalDestroyPlan*> RuntimeCert(ActiveRuntimeTerminalPlan, &Plan);
        bCompleted = SoloASBridge::CompleteRuntimeDeferredDestroy(Plan.Token);
    }
    Cuviss::TerminalAdmission::FCompiledTerminalDestructorObservation ReturnedObservation;
    const bool bObserveInherited = bCompleted && Plan.NativeTailHead && !Plan.bDevFacade
        && Plan.Qualification
        && Cuviss::TerminalAdmission::ReadCompletedTerminalDestructorObservation(
            *Plan.Qualification, ReturnedObservation);
    Plan.Phase = ETerminalPlanPhase::Cleaning;
    if (!CleanupTerminalEmptyFields(Plan))
    {
        FailTerminalPlan(Plan, TEXT("TERMINAL_CLEANUP_GENERATION_UNPROVED_NO_RETRY"));
        return;
    }
    if (!bCompleted)
    {
        FailTerminalPlan(Plan, TEXT("TERMINAL_REGISTERED_CALLBACK_FAILED_NO_RETRY"));
        return;
    }
    if (Plan.NativeTailHead && Plan.FinalInt32Provider && Plan.Phase == ETerminalPlanPhase::Cleaning
        && Plan.Int32Phase == Cuviss::TerminalAdmission::EInt32SlotPhase::FreshEnded
        && Plan.OriginalCompleted == 1 && Plan.EmptyConstructed == 1 && Plan.EmptyDestroyed == 1)
        Plan.FinalInt32Provider->bRuntimeCleanupComplete = true;
    ReleaseTerminalPlan(Plan);
    if (bObserveInherited)
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_INHERITED_PROOF object=%p dispatch=%p parent=%p child_source=%llu ")
            TEXT("parent_source=%llu default_source=%d phases=%u call=%u enter=%u parent_return=%u ")
            TEXT("resume=%u completed=1 frames=%u"),
            Plan.Token.Object, ReturnedObservation.DispatchFunction, ReturnedObservation.ParentFunction,
            ReturnedObservation.ChildSourceGeneration, ReturnedObservation.ParentSourceGeneration,
            ReturnedObservation.bDefaultSource ? 1 : 0, ReturnedObservation.PhaseMask,
            ReturnedObservation.ChildCallCount, ReturnedObservation.ParentEnterCount,
            ReturnedObservation.ParentReturnCount, ReturnedObservation.ChildResumeCount, ReturnedObservation.Frames);
    if (Plan.Fields.Num() > 1)
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_FIELD_SET_COMPLETE object=%p original_fields=%u empty_construct=%u "
                 "empty_destroy=%u callback=1 cert_inactive=%d module_pin=0"),
            Plan.Token.Object, Plan.OriginalCompleted, Plan.EmptyConstructed, Plan.EmptyDestroyed,
            !AngelscriptRuntimeGcMirror::IsRuntimeTerminalDestructorActive() && !SoloASBridge::GetActiveRuntimeDeferredDestroyToken());
    if (Plan.Fields.Num() == 1)
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_COMPLETE object=%p sequence=%llu epoch=%llu original_destroy=1 manual_finish=0 "
                 "empty_construct=1 empty_destroy=1 callback=%d cert_inactive=%d module_pin=%d"),
            Plan.Token.Object, Plan.Token.TrackSequence, Plan.Token.ArmEpoch, bCompleted,
            !AngelscriptRuntimeGcMirror::IsRuntimeTerminalDestructorActive()
                && !SoloASBridge::GetActiveRuntimeDeferredDestroyToken(),
            Plan.ModulePin != nullptr);
    return;
}

void DestroyTerminalNativeOwned(const FTerminalNativeOwnedProperty& Property, void* Data)
{
    auto* C = FindRuntimeNativeTailProperty(&Property);
    FTerminalNativeTailHead* Head = nullptr;
    if (C) for (auto& Candidate : C->Heads) if (Candidate.OwnedProxy.Get() == &Property) Head = &Candidate;
    if (!C || !Head || !IsInGameThread() || C->bOwnedCallout || C->bNativeCallout
        || ActiveTerminalOperation || ActiveRuntimeTerminalPlan || !ValidateTerminalNativeTailChain(*C))
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_OWNED_CHAIN_DRIFT_NO_RETRY"));
    FTerminalDestroyPlan* Selected = nullptr;
    for (const auto& Pair : TerminalPlans)
    {
        auto* Plan = Pair.Value.Get();
        if (reinterpret_cast<UPTRINT>(Plan->Token.Object) + uint32(C->OwnedOffset) != reinterpret_cast<UPTRINT>(Data)) continue;
        if (Selected || Plan->NativeTailChain != C || Plan->NativeTailHead != Head || Plan->Phase != ETerminalPlanPhase::Deferred)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_OWNED_RECEIVER_UNQUALIFIED_NO_RETRY"));
        Selected = Plan;
    }
    TOptional<FAngelscriptExecutionWriteScope> OwnedWriter;
    if (Selected)
    {
        OwnedWriter.Emplace();
        if (!ValidateTerminalPlan(*Selected, true) || !Selected->Qualification || Selected->OwnedFinishCalls
            || Selected->bRuntimeOwnedFieldReturned || Selected->OriginalCompleted || Selected->EmptyConstructed
            || Selected->EmptyDestroyed || Selected->Fields.Num() != 1
            || Selected->Fields[0].Phase != ETerminalFieldPhase::OriginalLive || Selected->Fields[0].Fresh
            || Selected->Int32Phase != Cuviss::TerminalAdmission::EInt32SlotPhase::OriginalLive)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_OWNED_BEFORE_ORIGINAL_UNQUALIFIED_NO_RETRY"));
        ++Selected->OwnedFinishCalls;
    }
    {
        TGuardValue<bool> OwnedCallout(C->bOwnedCallout, true);
        TGuardValue<FTerminalDestroyPlan*> OwnedOperation(ActiveTerminalOperation, Selected);
        C->Owned->FinishDestroy(Data);
    }
    if (!ValidateTerminalNativeTailChain(*C))
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_OWNED_AFTER_ORIGINAL_DRIFT_NO_RETRY"));
    if (!Selected) return;
    FAngelscriptExecutionWriteScope Writer;
    TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, Selected);
    auto& Plan = *Selected;
    if (!ValidateTerminalPlan(Plan, true) || Plan.OwnedFinishCalls != 1 || Plan.Fields[0].Property != C->Owned
        || Plan.Fields[0].Storage != Data || Plan.Fields[0].Phase != ETerminalFieldPhase::OriginalLive
        || Plan.Int32Phase != Cuviss::TerminalAdmission::EInt32SlotPhase::OriginalLive
        || static_cast<const FInstancedStruct*>(Data)->GetMemory() || static_cast<const FInstancedStruct*>(Data)->GetScriptStruct())
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_OWNED_CLEAR_RETURN_UNQUALIFIED_NO_RETRY"));
    // Original FinishDestroy already cleared and reconstructed this precise
    // wrapper. Adopt that existing live empty value; never placement-construct
    // over it and never repeat its original native destruction.
    auto& Field = Plan.Fields[0];
    Field.Fresh = static_cast<FInstancedStruct*>(Data);
    Field.Phase = ETerminalFieldPhase::FreshLive;
    Field.FreshEpoch = Plan.Token.ArmEpoch;
    Plan.OriginalCompleted = 1; Plan.EmptyConstructed = 1; Plan.bRuntimeOwnedFieldReturned = true;
    if (!Cuviss::TerminalAdmission::RefreshCompiledTerminalDestructorIdleContext(*Plan.Qualification)
        || !Cuviss::TerminalAdmission::ValidatePreparedCompiledTerminalDestructor(*Plan.Qualification, TerminalReceiverShape(Plan)))
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_OWNED_ADMISSION_AFTER_RETURN_NO_RETRY"));
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
        TEXT("SOLOAS_TERMINAL_NATIVE_OWNED_RETURN object=%p plan=%llu original_finish=1 reconstructed_empty=1 adopted_live_wrapper=1 placement=0 scalar_original_live=1 callback=0"),
        Plan.Token.Object, Plan.Token.PlanId);
    if (!ValidateTerminalPlan(Plan, true) || !CanCleanupTerminalFreshField(Plan, Field))
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_OWNED_ADOPTION_DRIFT_NO_RETRY"));
}

void DestroyTerminalNativeTail(const FTerminalNativeTailProperty& Property, void* Data)
{
    auto* C = FindRuntimeNativeTailProperty(&Property);
    FTerminalNativeTailHead* Head = nullptr;
    if (C && C->bIndependentHeads)
        for (auto& Candidate : C->Heads) if (Candidate.TailProxy.Get() == &Property) Head = &Candidate;
    if (!C || !IsInGameThread() || (C->bIndependentHeads ? !Head : C->Adapter.Get() != &Property)
        || C->bNativeCallout || C->bOwnedCallout
        || !ValidateTerminalNativeTailChain(*C))
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_CHAIN_DRIFT_NO_RETRY"));
    FTerminalDestroyPlan* Selected = nullptr;
    FObjectKey Key;
    for (const auto& Pair : TerminalPlans)
    {
        auto* Plan = Pair.Value.Get();
        if (reinterpret_cast<UPTRINT>(Plan->Token.Object) + uint32(C->TailOffset) != reinterpret_cast<UPTRINT>(Data)) continue;
        if (Selected || Plan->NativeTailChain != C || Plan->NativeTailHead != Head || Plan->Phase != ETerminalPlanPhase::Deferred)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_RECEIVER_UNQUALIFIED_NO_RETRY"));
        Selected = Plan;
        Key = Pair.Key;
    }
    TOptional<FAngelscriptExecutionWriteScope> NativeWriter;
    if (Selected)
    {
        NativeWriter.Emplace();
        if (ActiveTerminalOperation || ActiveRuntimeTerminalPlan || !ValidateTerminalPlan(*Selected, true)
            || Selected->bNativeTailReturned || !Selected->bRuntimeOwnedFieldReturned
            || Selected->OriginalCompleted != 1 || Selected->EmptyConstructed != 1 || Selected->EmptyDestroyed
            || !Selected->Int32Slot.bRuntimeOwnedOriginal || Selected->Int32Phase != Cuviss::TerminalAdmission::EInt32SlotPhase::OriginalLive)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_BEFORE_ORIGINAL_UNQUALIFIED_NO_RETRY"));
    }
    // Preserve UE's native ClearOnFinishDestroy semantics exactly: its wrapper
    // is cleared/reconstructed, not replaced with an ordinary DestroyValue.
    {
        TGuardValue<bool> NativeCallout(C->bNativeCallout, true);
        TGuardValue<FTerminalDestroyPlan*> NativeOperation(ActiveTerminalOperation, Selected);
        C->Original->FinishDestroy(Data);
    }
    if (!ValidateTerminalNativeTailChain(*C))
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_AFTER_ORIGINAL_DRIFT_NO_RETRY"));
    if (!Selected) return; // Unarmed values keep the original native behavior.
    if (static_cast<const FInstancedStruct*>(Data)->IsValid())
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_CLEAR_RETURN_UNQUALIFIED_NO_RETRY"));
    auto* Entry = TerminalPlans.Find(Key);
    if (!Entry || Entry->Get() != Selected)
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_PLAN_DRIFT_NO_RETRY"));
    TUniquePtr<FTerminalDestroyPlan> HeapOwner = MoveTemp(*Entry);
    TerminalPlans.Remove(Key);
    auto& Plan = *HeapOwner;
    FAngelscriptExecutionWriteScope Writer;
    TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, &Plan);
    Plan.bNativeTailReturned = true;
    if (!ValidateTerminalPlan(Plan, true) || !Plan.Qualification
        || !Cuviss::TerminalAdmission::RefreshCompiledTerminalDestructorIdleContext(*Plan.Qualification)
        || !ValidateTerminalFieldSetForCallback(Plan))
    {
        FailTerminalPlan(Plan, TEXT("TERMINAL_NATIVE_TAIL_COMPLETION_UNQUALIFIED_NO_RETRY"));
        return;
    }
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
        TEXT("SOLOAS_TERMINAL_NATIVE_TAIL_RETURN object=%p plan=%llu native_finish=1 owned_returned=1 scalar_original_live=1 callback=0"),
        Plan.Token.Object, Plan.Token.PlanId);
    if (!CompleteTerminalScalarLifetime(Plan)) return;
    CompleteTerminalReadyPlan(Plan);
}
}

bool IsRuntimeTerminalClassReadyForFinish(const UASClass* Class)
{
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    for (const auto& Pair : TerminalPlans)
        if (Pair.Value->Phase != ETerminalPlanPhase::Retired
            && (Pair.Value->Class == Class || (Pair.Value->NativeTailChain && Pair.Value->NativeTailChain->Class == Class)))
            return false;
    for (const auto& Pair : RuntimeNativeTailChains)
        if (Pair.Key != Class && Pair.Value->Class == Class) return false;
    return true;
#else
    return true;
#endif
}

void RetireRuntimeTerminalClassChain(UASClass* Class)
{
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    auto* Entry = RuntimeNativeTailChains.Find(Class);
    if (!Entry) return;
    auto& C = **Entry;
    for (const auto& Pair : TerminalPlans)
        if (Pair.Value->Phase != ETerminalPlanPhase::Retired && Pair.Value->NativeTailChain == &C)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_CLASS_RELINK_WITH_PENDING_DESTROY"));
    const auto& Head = C.Heads[0];
    // Final metadata retirement cannot dereference a native declaration that
    // ExitPurge may already have finished. It grants no receiver execution.
    const bool bFinalMetadataRetirement = Class->HasAnyFlags(RF_FinishDestroyed)
        && SameTerminalItem(Head.Index, Head.Serial, Class)
        && Head.Key.ResolveObjectPtrEvenIfUnreachable() == Class
        && Class->DestructorLink == Head.OwnedProxy.Get()
        && Head.OwnedProxy->GetOwnerStruct() == Class && Head.TailProxy->GetOwnerStruct() == Class
        && Head.OwnedProxy->DestructorLinkNext == Head.TailProxy.Get() && !Head.TailProxy->DestructorLinkNext
        && !ActiveTerminalOperation && !ActiveRuntimeTerminalPlan;
    if (C.bOwnedCallout || C.bNativeCallout
        || (!bFinalMetadataRetirement && !ValidateTerminalNativeTailChain(C)))
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_CLASS_RETIRE_CHAIN_DRIFT"));
    Class->DestructorLink = C.Heads[0].OriginalHead;
    asIScriptModule* Module = C.ModulePin;
    RuntimeNativeTailChains.Remove(Class);
    if (Module) AngelscriptRetiredModules::ReleaseModule(Module);
#else
#endif
}

void RebuildRuntimeTerminalClassChain(UASClass* Class)
{
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    FAngelscriptExecutionWriteScope Writer;
    if (!Class || !Class->ScriptTypePtr || !Class->CodeSuperClass || Class->bLayoutChanging) return;
    RetireRuntimeTerminalClassChain(Class);
    UASClass* Declaration = Class->bHasASClassParent ? Cast<UASClass>(Class->GetSuperClass()) : Class;
    if (!Declaration || !Declaration->ScriptTypePtr || Declaration->bHasASClassParent
        || Declaration->UeDelayedDestroyFields.Num() != 1) return;
    UClass* Native = Declaration->GetSuperClass();
    auto* Owned = const_cast<FStructProperty*>(CastField<FStructProperty>(*Declaration->UeDelayedDestroyFields.CreateConstIterator()));
    auto* Tail = Native ? CastField<FStructProperty>(Native->DestructorLink) : nullptr;
    if (!Native || !Native->HasAnyClassFlags(CLASS_Native) || Native->GetSuperClass() != UObject::StaticClass()
        || !Owned || !Tail || Tail->DestructorLinkNext || Owned->DestructorLinkNext != Tail
        || Owned->Struct != FInstancedStruct::StaticStruct() || Tail->Struct != Owned->Struct
        || Owned->ArrayDim != 1 || Tail->ArrayDim != 1 || Owned->HasSetterOrGetter() || Tail->HasSetterOrGetter()
        || Owned->GetSize() != sizeof(FInstancedStruct) || Tail->GetSize() != sizeof(FInstancedStruct)
        || !Tail->Struct->GetCppStructOps() || !Tail->Struct->GetCppStructOps()->HasClearOnFinishDestroy()) return;
    auto* Type = CastToObjectType(static_cast<asCTypeInfo*>(Class->ScriptTypePtr));
    auto* DeclType = CastToObjectType(static_cast<asCTypeInfo*>(Declaration->ScriptTypePtr));
    if (!Type || !DeclType || Type->GetUserData() != Class || DeclType->GetUserData() != Declaration
        || DeclType->derivedFrom || Type->module != DeclType->module
        || (Class != Declaration && (Type->derivedFrom != DeclType || Type->localProperties.GetLength()
            || Type->size != DeclType->size))) return;
    auto C = MakeUnique<FTerminalNativeTailChain>();
    C->bAutomatic = C->bIndependentHeads = true; C->Class = Declaration; C->Native = Native;
    C->DeclarationType = DeclType;
    C->Owned = Owned; C->Original = Tail; C->NativePrefix = Native->GetPropertiesSize();
    C->OwnedOffset = Owned->GetOffset_ForInternal(); C->TailOffset = Tail->GetOffset_ForInternal();
    C->OwnedFlags = uint64(Owned->GetPropertyFlags()); C->TailFlags = uint64(Tail->GetPropertyFlags());
    C->PropertiesSize = Declaration->GetPropertiesSize(); C->NativeTailName = Tail->GetFName();
    C->ClassKey = FObjectKey(Declaration); C->NativeKey = FObjectKey(Native);
    C->ClassIndex = GUObjectArray.ObjectToIndex(Declaration); C->ClassSerial = GUObjectArray.GetSerialNumber(C->ClassIndex);
    C->NativeIndex = GUObjectArray.ObjectToIndex(Native); C->NativeSerial = GUObjectArray.GetSerialNumber(C->NativeIndex);
    const FProperty* P = Declaration->PropertyLink;
    for (int32 I = 0; I != 3; ++I)
    {
        if (!P) return;
        if (const auto* Scalar = CastField<FIntProperty>(P))
        {
            if (C->Scalar || Scalar->GetOwnerStruct() != Declaration || Scalar->ArrayDim != 1
                || Scalar->HasSetterOrGetter()) return;
            C->Scalar = Scalar; C->ScalarOffset = Scalar->GetOffset_ForInternal(); C->ScalarFlags = uint64(Scalar->GetPropertyFlags());
        }
        else if (P != Owned && P != Tail) return;
        C->Properties[I] = P; C->FieldNext[I] = P->Next; C->PropertyNext[I] = P->PropertyLinkNext;
        C->RefNext[I] = P->NextRef; C->PostNext[I] = P->PostConstructLinkNext;
        P = P->PropertyLinkNext;
    }
    if (P || !C->Scalar || C->OwnedOffset < C->NativePrefix || C->ScalarOffset < C->NativePrefix
        || Cuviss::TerminalAdmission::TerminalStorageRangesOverlap(C->OwnedOffset, sizeof(FInstancedStruct), C->ScalarOffset, sizeof(int32))) return;
    C->Heads.SetNum(1);
    auto& H = C->Heads[0]; H.Class = Class; H.Type = Type; H.Key = FObjectKey(Class);
    H.Index = GUObjectArray.ObjectToIndex(Class); H.Serial = GUObjectArray.GetSerialNumber(H.Index);
    H.TypeId = Type->GetTypeId(); H.TypeSize = Type->size; H.ContainerSize = Class->GetPropertiesSize();
    H.ScriptOffset = Class->ScriptPropertyOffset; H.CodeSuper = Class->CodeSuperClass;
    H.Children = Class->ChildProperties; H.Properties = Class->PropertyLink;
    H.References = Class->RefLink; H.PostConstruct = Class->PostConstructLink; H.OriginalHead = Owned;
    H.OwnedProxy = MakeUnique<FTerminalNativeOwnedProperty>(Class, *Owned);
    H.TailProxy = MakeUnique<FTerminalNativeTailProperty>(Class, *Tail);
    H.OwnedProxy->DestructorLinkNext = H.TailProxy.Get();
    // Linking a child may inherit its parent's off-list head. The class takes
    // its own execution head without editing any declaration's shared Next.
    C->ModulePin = AngelscriptRetiredModules::RetainType(Type);
    if (!C->ModulePin) return;
    FProperty* Previous = Class->DestructorLink;
    Class->DestructorLink = Owned;
    if (!ValidateAutomaticNativeTailChain(*C, false))
    {
        Class->DestructorLink = Previous;
        AngelscriptRetiredModules::ReleaseModule(C->ModulePin);
        return;
    }
    Class->DestructorLink = H.OwnedProxy.Get();
    RuntimeNativeTailChains.Add(Class, MoveTemp(C));
#else
#endif
}

namespace AngelscriptRuntimeGcMirror
{
bool IsRetiredRuntimeTypeIdentity(const asITypeInfo* Type, const asIScriptModule* RetainedModule, const UASClass* Class)
{
    return Class != nullptr && Class->ScriptTypePtr == nullptr && Class->NewerVersion != nullptr
        && Class->HasAllClassFlags(CLASS_NewerVersionExists | CLASS_NotPlaceable | CLASS_HideDropDown | CLASS_Hidden)
        && Type != nullptr && Type->GetUserData() == Class && RetainedModule != nullptr
        && RetainedModule == Type->GetModule();
}

bool IsRegisteredRuntimeTypeIdentity(const asITypeInfo* Type)
{
    if (!Type || !IsAngelscriptExecutionWriterHeldByCurrentThread() || !FAngelscriptManager::IsInitialized()) return false;
    auto* Engine = FAngelscriptManager::Get().GetScriptEngine();
    if (!Engine) return false;
    // Deferred modules remain in the engine's module list until their retained types are released.
    for (asUINT ModuleIndex = 0; ModuleIndex != Engine->GetModuleCount(); ++ModuleIndex)
    {
        const auto* Module = Engine->GetModuleByIndex(ModuleIndex);
        if (!Module) continue;
        for (asUINT TypeIndex = 0; TypeIndex != Module->GetObjectTypeCount(); ++TypeIndex)
            if (Module->GetObjectTypeByIndex(TypeIndex) == Type)
                return Type->GetEngine() == Engine && Type->GetModule() == Module;
    }
    for (asUINT TypeIndex = 0; TypeIndex != Engine->GetObjectTypeCount(); ++TypeIndex)
        if (Engine->GetObjectTypeByIndex(TypeIndex) == Type) return Type->GetEngine() == Engine;
    return false;
}

FObjectKey ResolveRuntimeDestructorKey(UObject* Object)
{
    if (ActiveRuntimeTerminalPlan && Object == ActiveRuntimeTerminalPlan->Token.Object)
    {
        return ActiveRuntimeTerminalPlan->Token.Key;
    }
    return FObjectKey(Object);
}

bool MatchesRuntimeDestructorTracking(UObject* Object, const UObjectBase* Identity,
    void* SavedCookie, uint64 Sequence, asITypeInfo* ExpectedCookie, bool bAllowAnyCookie)
{
    if (ActiveRuntimeTerminalPlan)
    {
        const auto& Token = ActiveRuntimeTerminalPlan->Token;
        return Object == Token.Object && Identity == Token.Object && SavedCookie == Token.TypeCookie
            && ExpectedCookie == Token.TypeCookie && Sequence == Token.TrackSequence
            && IsRuntimeDestructorContextValid(Object, ExpectedCookie);
    }
    return Object == Identity && Sequence != 0
        && ((bAllowAnyCookie && ExpectedCookie == nullptr) || SavedCookie == ExpectedCookie);
}

void CollectRuntimeOwnerReferences(UObject* Object, void* TypeCookie, FReferenceCollector& Collector)
{
    // A reinstanced receiver's current class can differ from its saved AS owner.
    // Trace the owner from the receiver, so unreachable receivers never root it.
    auto* Type = static_cast<asITypeInfo*>(TypeCookie);
    UASClass* Owner = Type ? static_cast<UASClass*>(Type->GetUserData()) : nullptr;
    if (Owner)
    {
        check(!Owner->HasAnyFlags(RF_FinishDestroyed));
        Collector.AddReferencedObject(Owner, Object);
    }
}

void CancelRuntimeTerminalOrdinaryClaim(UObject* Object, asITypeInfo* ExpectedCookie)
{
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    if (!ActiveRuntimeTerminalPlan && IsInGameThread())
    {
        for (auto& Pair : TerminalPlans)
        {
            auto& Plan = *Pair.Value;
            if (Plan.Phase == ETerminalPlanPhase::Retired || Plan.Token.Object != Object) continue;
            if (ExpectedCookie && Plan.Token.TypeCookie != ExpectedCookie) continue;
            if (ActiveTerminalOperation)
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_ORDINARY_CLAIM_REENTRY_NO_RETRY"));
            TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, &Plan);
            if (Plan.Phase != ETerminalPlanPhase::Preparing)
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_ORDINARY_CLAIM_AFTER_DEFERRAL_NO_RETRY"));
            check(IsAngelscriptExecutionWriterHeldByCurrentThread());
            // Called by the outer operation after the mirror claim releases its mutex.
            ReleaseTerminalPlan(Plan);
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_CANCEL object=%p sequence=%llu epoch=%llu stage=ordinary_claim module_pin=0 replacement=0"),
                Object, Plan.Token.TrackSequence, Plan.Token.ArmEpoch);
        }
    }
#endif
}

bool IsRuntimeTerminalDestructorActive()
{
    return ActiveRuntimeTerminalPlan != nullptr;
}

bool PrepareRuntimeTerminalReconstruction(UObject* Object, asITypeInfo* ExpectedCookie)
{
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    check(IsAngelscriptExecutionWriterHeldByCurrentThread());
    if (ActiveTerminalOperation) return false;
    for (auto& Pair : TerminalPlans)
    {
        auto& Plan = *Pair.Value;
        if (Plan.Phase == ETerminalPlanPhase::Retired || Plan.Token.Object != Object) continue;
        if (!IsInGameThread()) return false;
        if (Plan.Phase != ETerminalPlanPhase::Preparing)
        {
            FailTerminalPlan(Plan, TEXT("TERMINAL_RECONSTRUCT_AFTER_DEFERRAL_NO_RETRY"));
            return false;
        }
        if (Plan.Token.TypeCookie != ExpectedCookie) continue;
        TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, &Plan);
        // This runs before the reconstruction claim takes RuntimeTrackedObjectsMutex.
        SoloASBridge::DisarmRuntimeTerminalDestroyForTest(Plan.Token);
        ReleaseTerminalPlan(Plan);
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
            TEXT("SOLOAS_TERMINAL_CANCEL object=%p sequence=%llu epoch=%llu stage=reconstruction module_pin=0 replacement=0"),
            Object, Plan.Token.TrackSequence, Plan.Token.ArmEpoch);
    }
#endif
    return true;
}

bool IsRuntimeDestructorContextValid(UObject* Object, void* Cookie)
{
    if (const auto* Certificate = SoloASBridge::GetActiveRuntimeDeferredDestroyToken())
        return ActiveRuntimeTerminalPlan
            && (ActiveRuntimeTerminalPlan->ActualReceiverClass == ActiveRuntimeTerminalPlan->Class
                || ValidateTerminalActualReceiverMetadata(*ActiveRuntimeTerminalPlan))
            && ActiveRuntimeTerminalPlan->Token.Object == Object
            && ActiveRuntimeTerminalPlan->Token.TypeCookie == Cookie
            && ActiveRuntimeTerminalPlan->Token.Key == Certificate->Key
            && ActiveRuntimeTerminalPlan->Token.PlanId == Certificate->PlanId
            && ActiveRuntimeTerminalPlan->Token.ArmEpoch == Certificate->ArmEpoch
            && ActiveRuntimeTerminalPlan->Token.ClaimSequence == Certificate->ClaimSequence
            && ActiveRuntimeTerminalPlan->Token.TrackSequence == Certificate->TrackSequence
            && ActiveRuntimeTerminalPlan->Token.Generation == Certificate->Generation
            && Certificate->Object == Object && Certificate->TypeCookie == Cookie
            && Certificate->Key.ResolveObjectPtrEvenIfUnreachable() == Object
            && Object->IsUnreachable() && Object->HasAllFlags(RF_BeginDestroyed | RF_FinishDestroyed);
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    if (SoloASBridge::IsTerminalRuntimeDestroyCallbackForTest())
        return ActiveRuntimeTerminalPlan && ActiveRuntimeTerminalPlan->Token.Object == Object
            && ActiveRuntimeTerminalPlan->Token.TypeCookie == Cookie
            && SoloASBridge::IsTerminalRuntimeDestroyReceiverForTest(Object);
#endif
    return true;
}

bool ObserveBridgeRuntimeCookieForMirror(UObject* Object, void*& OutCookie)
{
#if WITH_DEV_AUTOMATION_TESTS
    return SoloASBridge::ObserveRuntimeObjectTypeCookie(Object, OutCookie);
#else
    OutCookie = nullptr;
    return false;
#endif
}

void CompleteRuntimeTerminalFieldDestroy(const FStructProperty& Property, void* ValueAddress)
{
    // No fixture callback or terminal metadata is consulted on another thread.
    if (!IsInGameThread()) return;
    if (ActiveTerminalOperation)
    {
        bool bOwnedStorage = HasTerminalFieldStorageIdentity(*ActiveTerminalOperation, ValueAddress);
        for (const auto& Pair : TerminalPlans)
        {
            const auto& Plan = *Pair.Value;
            if (Plan.Phase == ETerminalPlanPhase::Deferred && HasTerminalFieldStorageIdentity(Plan, ValueAddress))
                bOwnedStorage = true;
        }
        if (!bOwnedStorage) return;
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_FIELD_OPERATION_REENTRY_NO_RETRY"));
    }
    for (auto& Pair : TerminalPlans)
    {
        auto& Plan = *Pair.Value;
        if (Plan.Phase == ETerminalPlanPhase::Retired) continue;
        const int32 FieldIndex = Plan.Fields.IndexOfByPredicate([&](const FTerminalOwnedField& Field)
            { return Field.Storage == ValueAddress; });
        if (FieldIndex == INDEX_NONE)
        {
            const UPTRINT Address = reinterpret_cast<UPTRINT>(ValueAddress);
            const UPTRINT Receiver = reinterpret_cast<UPTRINT>(Plan.Token.Object);
            if (Plan.Phase == ETerminalPlanPhase::Deferred && Address >= Receiver
                && Address - Receiver < UPTRINT(Plan.PropertiesSize))
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_COMPLETION_STORAGE_MISMATCH_NO_RETRY"));
            continue;
        }
        TUniquePtr<FTerminalDestroyPlan> HeapOwner = MoveTemp(Pair.Value);
        const FObjectKey Key = Plan.Token.Key;
        TerminalPlans.Remove(Key);
        FAngelscriptExecutionWriteScope Writer;
        TGuardValue<FTerminalDestroyPlan*> OperationGuard(ActiveTerminalOperation, &Plan);
        auto& Field = Plan.Fields[FieldIndex];
        const bool bInjectedEnd = Plan.bFaultInjected && FieldIndex == 0
            && Field.Phase == ETerminalFieldPhase::FreshEnded;
        if (Field.Phase == ETerminalFieldPhase::FreshLive || bInjectedEnd)
        {
            // The proof reads only still-live metadata and saved identities, never the ended wrapper.
            const bool OriginalEndedFresh = Field.Property == &Property
                && (bInjectedEnd || (Field.Fresh == ValueAddress && CanCleanupTerminalFreshField(Plan, Field)));
            const uint64 FreshEpoch = Field.FreshEpoch;
            const uint32 EmptyDestroyedBeforeRetirement = Plan.EmptyDestroyed;
            // Revoke cleanup permission even when the incoming operation's type/generation is unknown.
            Field.Fresh = nullptr;
            Field.Phase = ETerminalFieldPhase::FreshEnded;
            if (OriginalEndedFresh) ++Plan.EmptyDestroyed;
            if (Plan.bFaultInjected && FieldIndex == 0)
            {
                // Log only after revocation: synchronous log sinks cannot clean the ended wrapper again.
                const TCHAR* Mode = &Property == Field.Property ? TEXT("duplicate") : TEXT("wrong_property");
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                    TEXT("SOLOAS_TERMINAL_FIELD_FAULT object=%p mode=%s first=%p incoming=%p value_addr=%p epoch=%llu "
                         "fields=2 chain_nodes=%u original_fields=%u empty_construct=%u empty_destroy_before_retirement=%u "
                         "first_original=1 last_original=0 fresh_proved_live=1 fresh_epoch=%llu "
                         "metadata_unchanged=1 cert_inactive=1 proof_phase=before_typed_end cleanup_authorization_revoked=1"),
                    Plan.Token.Object, Mode, Field.Property, &Property, ValueAddress, Plan.Token.ArmEpoch,
                    Plan.ChainLength, Plan.OriginalCompleted, Plan.EmptyConstructed, EmptyDestroyedBeforeRetirement, FreshEpoch);
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                    TEXT("SOLOAS_TERMINAL_FIELD_FAULT_TYPED_END object=%p mode=%s first=%p incoming=%p value_addr=%p "
                         "epoch=%llu typed_empty_destroy=1 original_replay=0 callback=0 cleanup_authorization_revoked=1"),
                    Plan.Token.Object, Mode, Field.Property, &Property, ValueAddress, Plan.Token.ArmEpoch);
            }
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Error,
                TEXT("SOLOAS_TERMINAL_EMPTY_RETIRED object=%p field=%p field_index=%d "
                     "ended_by_original=%d end_proved=%d cleanup_authorization_revoked=1 cleanup_dereferenced=0"),
                Plan.Token.Object, Field.Property, FieldIndex, OriginalEndedFresh, OriginalEndedFresh);
        }
        if (Field.Property != &Property || Plan.Phase != ETerminalPlanPhase::Deferred
            || Field.Phase != ETerminalFieldPhase::OriginalLive || Field.Fresh
            || uint32(FieldIndex) != Plan.OriginalCompleted)
        {
            FailTerminalPlan(Plan, TEXT("TERMINAL_ORIGINAL_OUTSIDE_DEFERRED_PHASE"));
            return;
        }
        Field.Phase = ETerminalFieldPhase::OriginalEnded;
        if (Plan.Int32Slot.Property)
        {
            if (Plan.bDevFacade || Plan.Fields.Num() != 1
                || Plan.Int32Phase != Cuviss::TerminalAdmission::EInt32SlotPhase::OriginalLive
                || Plan.FreshInt32 || Plan.FreshInt32Epoch)
            {
                FailTerminalPlan(Plan, TEXT("TERMINAL_INT32_ORIGINAL_PHASE_UNPROVED_NO_RETRY"));
                return;
            }
            if (Plan.Int32Slot.bRuntimeOwnedOriginal)
            {
                // Only the owned wrapper/payload ended. The registered independent
                // complete int is still live until the late Runtime-owned typed end.
                Plan.bRuntimeOwnedFieldReturned = true;
            }
            else if (Plan.FinalInt32Provider)
            {
                const auto& Provider = *Plan.FinalInt32Provider;
                if (Provider.PlanId != Plan.Token.PlanId || Provider.Ticket.SlotEpoch != Plan.Int32Slot.Epoch
                    || Provider.Ticket.Storage != Plan.Int32Slot.Storage || !Provider.bWriting || !Provider.bPublished || !Provider.bEnded
                    || !Provider.ConstructOrder || Provider.ConstructOrder >= Provider.WriteOrder
                    || Provider.WriteOrder >= Provider.PublishOrder || Provider.PublishOrder >= Provider.EndOrder)
                {
                    FailTerminalPlan(Plan, TEXT("TERMINAL_FINAL_PROVIDER_NOT_PUBLISHED_AND_ENDED_NO_RETRY"));
                    return;
                }
                Plan.FinalInt32Delivery = bTerminalStaleCaptureControl ? Plan.Int32Slot.CapturedValue : Provider.FinalValue;
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                    TEXT("SOLOAS_TERMINAL_FINAL_HANDOFF object=%p plan=%llu provider_epoch=%llu slot_epoch=%llu ")
                    TEXT("captured=%d final=%d delivery=%d published=1 typed_end=1 old_read=0 stale_control=%d ")
                    TEXT("construct_order=%llu write_order=%llu publish_order=%llu end_order=%llu"),
                    Plan.Token.Object, Plan.Token.PlanId, Provider.Ticket.ProviderEpoch, Provider.Ticket.SlotEpoch,
                    Plan.Int32Slot.CapturedValue, Provider.FinalValue, Plan.FinalInt32Delivery, bTerminalStaleCaptureControl ? 1 : 0,
                    Provider.ConstructOrder, Provider.WriteOrder, Provider.PublishOrder, Provider.EndOrder);
            }
            if (!Plan.Int32Slot.bRuntimeOwnedOriginal)
                Plan.Int32Phase = Cuviss::TerminalAdmission::EInt32SlotPhase::OriginalEnded;
        }
        if (!ValidateTerminalPlan(Plan, true) || uint32(FieldIndex) != Plan.OriginalCompleted
            || Field.Phase != ETerminalFieldPhase::OriginalEnded || Field.Fresh)
        {
            FailTerminalPlan(Plan, TEXT("TERMINAL_FIELD_INVALID_AFTER_ORIGINAL_NO_RETRY"));
            return;
        }
        if (!Plan.bDevFacade && (!Plan.Qualification
            || !Cuviss::TerminalAdmission::RefreshCompiledTerminalDestructorIdleContext(*Plan.Qualification)))
        {
            FailTerminalPlan(Plan, TEXT("TERMINAL_IDLE_CONTEXT_REFRESH_FAILED_NO_RETRY"));
            return;
        }
        uint32 EarlierEmptyLive = 0;
        for (int32 EarlierIndex = 0; EarlierIndex != FieldIndex; ++EarlierIndex)
        {
            const auto& Earlier = Plan.Fields[EarlierIndex];
            if (!CanCleanupTerminalFreshField(Plan, Earlier) || Earlier.Fresh->IsValid())
            {
                FailTerminalPlan(Plan, TEXT("TERMINAL_EARLIER_EMPTY_LIFETIME_MISSING_NO_RETRY"));
                return;
            }
            ++EarlierEmptyLive;
        }
        // Original payload lifetime has ended. This precise type starts a fresh, empty wrapper lifetime.
        // The explicit refresh allows across-frame idle transitions; this final proof has no callout.
        if (!Plan.bDevFacade && !Cuviss::TerminalAdmission::ValidatePreparedCompiledTerminalDestructor(
            *Plan.Qualification, TerminalReceiverShape(Plan)))
        {
            FailTerminalPlan(Plan, TEXT("TERMINAL_FIELD_PLACEMENT_PROOF_FAILED_NO_RETRY"));
            return;
        }
        Field.Fresh = ::new (Field.Storage) FInstancedStruct();
        Field.Phase = ETerminalFieldPhase::FreshLive;
        Field.FreshEpoch = Plan.Token.ArmEpoch;
        ++Plan.OriginalCompleted;
        ++Plan.EmptyConstructed;
        if (Plan.NativeTailChain)
        {
            // The native tail still owns an outstanding FinishDestroy call. The
            // complete original scalar remains live through both native writes.
            TerminalPlans.Add(Key, MoveTemp(HeapOwner));
            return;
        }
        if (!CompleteTerminalScalarLifetime(Plan)) return;
        if (Plan.Fields.Num() > 1)
            for (int32 EarlierIndex = 0; EarlierIndex != FieldIndex; ++EarlierIndex)
            {
                const auto& Earlier = Plan.Fields[EarlierIndex];
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                    TEXT("SOLOAS_TERMINAL_EARLIER_EMPTY object=%p field_index=%d earlier_index=%d field=%p "
                         "value_addr=%p epoch=%llu original_destroy=1 fresh_live=1 empty=1 callback=0 cert_inactive=1"),
                    Plan.Token.Object, FieldIndex, EarlierIndex, Earlier.Property, Earlier.Storage, Earlier.FreshEpoch);
            }
        if (Plan.Fields.Num() > 1)
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
                TEXT("SOLOAS_TERMINAL_FIELD_READY object=%p field=%p field_index=%d owner=%s original_destroy=1 "
                     "empty_construct=1 fields_ready=%u fields_total=%d field_last=%d earlier_empty_live=%d "
                     "earlier_empty_count=%u value_addr=%p epoch=%llu callback=0 cert_inactive=1"),
                Plan.Token.Object, &Property, FieldIndex, *Field.Owner->GetName(), Plan.OriginalCompleted, Plan.Fields.Num(),
                &Property == Plan.Property, EarlierEmptyLive != 0, EarlierEmptyLive, Field.Storage, Field.FreshEpoch);
        if (&Property != Plan.Property)
        {
            TerminalPlans.Add(Key, MoveTemp(HeapOwner));
            return;
        }
        CompleteTerminalReadyPlan(Plan);
        return;
    }
}
}

bool UASClass::RuntimeDestroyObject(UObject* Object, asITypeInfo* TypeCookie)
{
#if !SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    if (!Object || !TypeCookie || TypeCookie->GetUserData() != this || IsAngelscriptExecutionShutdownStarted())
        return false;
    // Consume before native field cleanup or script callouts, including reentrant destruction.
    if (!AngelscriptRuntimeGcMirror::ClaimRuntimeScriptDestructor(Object)) return true;
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, VeryVerbose,
        TEXT("ASR_SCRIPT_FINISH object=%s class=%s phase=begin exit_purge=%d"), *Object->GetPathName(), *GetPathName(), GExitPurge);
    for (const UASClass* Owner = this; Owner; Owner = Cast<UASClass>(Owner->GetSuperClass()))
        for (const FProperty* Field : Owner->ScriptFinishDestroyFields)
            Field->FinishDestroy_InContainer(Object);
    reinterpret_cast<asCScriptObject*>(Object)->CallDestructor(static_cast<asCObjectType*>(TypeCookie));
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, VeryVerbose,
        TEXT("ASR_SCRIPT_FINISH object=%s class=%s phase=end exit_purge=%d"), *Object->GetPathName(), *GetPathName(), GExitPurge);
    return true;
#else

	checkf(IsAngelscriptExecutionWriterHeldByCurrentThread(),
		TEXT("Runtime destruction requires type-lifetime ownership."));
	checkf(Object != nullptr && Object->IsA(this) && TypeCookie != nullptr
		&& TypeCookie->GetUserData() == this,
		TEXT("Runtime destruction received a mismatched object or type cookie."));
	if (Object == nullptr || !Object->IsA(this) || TypeCookie == nullptr
		|| TypeCookie->GetUserData() != this)
	{
		return false;
	}
    if (AngelscriptRuntimeGcMirror::IsRuntimeTerminalDestructorActive()
        && (ActiveRuntimeTerminalPlan->Class != this || ActiveRuntimeTerminalPlan->Token.TypeCookie != TypeCookie
            || !AngelscriptRuntimeGcMirror::IsRuntimeDestructorContextValid(Object, TypeCookie))) return false;
	if (AngelscriptRuntimeGcMirror::IsRuntimeGcTraceEnabled())
	{
		UE_LOG(LogTemp, Log, TEXT("SOLOAS_GC_AS_DESTRUCTOR_BEGIN class=%s object=%p"),
			*GetName(), Object);
	}
	if (GExitPurge && !AngelscriptRuntimeGcMirror::IsRuntimeTerminalDestructorActive())
	{
		TSet<const FProperty*> DelayedFields;
		for (UClass* Owner = this; Owner != nullptr; Owner = Owner->GetSuperClass())
		{
			if (const UASClass* ScriptOwner = Cast<UASClass>(Owner))
			{
				DelayedFields.Append(ScriptOwner->UeDelayedDestroyFields);
			}
		}
		for (const FProperty* Property = DestructorLink; Property != nullptr; Property = Property->DestructorLinkNext)
		{
			if (DelayedFields.Remove(Property) != 0)
			{
				Property->FinishDestroy_InContainer(Object);
			}
		}
		if (!DelayedFields.IsEmpty())
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("EXIT_PURGE_FIELD_OWNER_LINK_MISSING"));
		}
	}
    // All trace/log callouts precede this final proof; successful proof has no callout before AS entry.
    if (ActiveRuntimeTerminalPlan && (!AngelscriptRuntimeGcMirror::IsRuntimeDestructorContextValid(Object, TypeCookie)
        || ActiveRuntimeTerminalPlan->Token.Object != Object || ActiveRuntimeTerminalPlan->Class != this
        || ActiveRuntimeTerminalPlan->Token.TypeCookie != TypeCookie
        || ActiveRuntimeTerminalPlan->Phase != ETerminalPlanPhase::Completing
        || !ValidateTerminalFieldSetForCallback(*ActiveRuntimeTerminalPlan)
        || !BeginTerminalQualification(*ActiveRuntimeTerminalPlan)))
    {
        FailTerminalPlan(*ActiveRuntimeTerminalPlan, TEXT("TERMINAL_AS_ENTRY_FIELD_SET_INVALID_NO_RETRY"));
        return false;
    }
    if (!ActiveRuntimeTerminalPlan && !ValidateOrdinaryRemovedRuntimeCallback(Object, this, TypeCookie))
        return false;
    if (!AngelscriptRuntimeGcMirror::ClaimRuntimeScriptDestructor(Object)) return true;
    if (ActiveRuntimeTerminalPlan) ActiveRuntimeTerminalPlan->Phase = ETerminalPlanPhase::Callback;
	reinterpret_cast<asCScriptObject*>(Object)->CallDestructor(
		(asCObjectType*)TypeCookie);
    if (ActiveRuntimeTerminalPlan && !ActiveRuntimeTerminalPlan->bDevFacade
        && (!ActiveRuntimeTerminalPlan->Qualification
            || !Cuviss::TerminalAdmission::DidCompleteCompiledTerminalDestructor(*ActiveRuntimeTerminalPlan->Qualification)))
    {
        FailTerminalPlan(*ActiveRuntimeTerminalPlan, TEXT("TERMINAL_AS_CONTEXT_DID_NOT_COMPLETE_NO_RETRY"));
        return false;
    }
	if (AngelscriptRuntimeGcMirror::IsRuntimeGcTraceEnabled())
	{
		UE_LOG(LogTemp, Log, TEXT("SOLOAS_GC_AS_DESTRUCTOR_END class=%s object=%p"),
			*GetName(), Object);
	}
	return true;
#endif
}

void UASClass::DestroyTrackedRuntimeObjectExplicitly(UObject* Object)
{
	AngelscriptRuntimeGcMirror::DestroyTrackedRuntimeObjectExplicitly(Object);
}

bool UASClass::UpdateRuntimeTrackedMirrorCookie(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	asITypeInfo* NewTypeCookie
)
{
	return AngelscriptRuntimeGcMirror::UpdateRuntimeTrackedMirrorCookie(
		Object, ExpectedTypeCookie, NewTypeCookie);
}

bool UASClass::ClaimRuntimeTrackedMirrorForReconstruction(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie
)
{
	return AngelscriptRuntimeGcMirror::ClaimRuntimeTrackedMirrorForReconstruction(
		Object, ExpectedTypeCookie);
}

bool UASClass::RuntimeDestroyObjectAdapter(
	UObject* Object,
	void* TypeCookie,
	uint64 Generation
)
{
	return AngelscriptRuntimeGcMirror::RuntimeDestroyObjectAdapter(
		Object, TypeCookie, Generation);
}

void UASClass::RegisterRuntimeGcLifecycleAdapter()
{
	check(IsInGameThread());
	checkf(!RuntimeLifecyclePreExitHandle.IsValid(),
		TEXT("PRE_EXIT_ALREADY_REGISTERED"));
	AngelscriptRuntimeGcMirror::VerifyRuntimeGcLifecycleStartup();
	AngelscriptRetiredModules::Startup();
    RuntimeLifecyclePostGcHandle = FCoreUObjectDelegates::GetPostGarbageCollect().AddStatic(
        &AngelscriptRuntimeGcMirror::CollectReleasedScriptDestructorClaims);
	checkf(RuntimeLifecycleGeneration != 0,
		TEXT("Runtime lifecycle generation must be non-zero."));
	// verifyf: the callback registration must execute in Shipping too; a
	// checkf-wrapped side effect is compiled out with bUseChecksInShipping=false.
	verifyf(SoloASBridge::RegisterRuntimeDestroyCallback(
		&UASClass::RuntimeDestroyObjectAdapter, RuntimeLifecycleGeneration,
		&PrepareRuntimeDeferredDestroy, &ControlRuntimeDeferredDestroy,
        &AngelscriptRuntimeGcMirror::CollectRuntimeOwnerReferences),
		TEXT("RUNTIME_GC_LIFECYCLE_REGISTRATION_FAILED"));
	bRuntimeLifecycleShutdownOwnerCalled = false;
	RuntimeLifecyclePreExitHandle =
		FCoreDelegates::OnPreExit.AddStatic(
			&StopAngelscriptRuntimeAtPreExit);
	checkf(RuntimeLifecyclePreExitHandle.IsValid(),
		TEXT("RUNTIME_GC_LIFECYCLE_PRE_EXIT_REGISTRATION_FAILED"));
}

void UASClass::ShutdownGcLifecycleAdapter()
{
	check(IsInGameThread());
	if (bRuntimeLifecycleShutdownOwnerCalled)
	{
		return;
	}
	bRuntimeLifecycleShutdownOwnerCalled = true;
	if (RuntimeLifecyclePreExitHandle.IsValid())
	{
		FCoreDelegates::OnPreExit.Remove(RuntimeLifecyclePreExitHandle);
		RuntimeLifecyclePreExitHandle.Reset();
	}
    FCoreUObjectDelegates::GetPostGarbageCollect().Remove(RuntimeLifecyclePostGcHandle);
    RuntimeLifecyclePostGcHandle.Reset();
	BeginAngelscriptExecutionShutdown();
	AngelscriptRuntimeGcMirror::BeginRuntimeLifecycleShutdown();
	FAngelscriptExecutionWriteScope TypeLifetimeScope;
	checkf(IsAngelscriptExecutionWriterHeldByCurrentThread(),
		TEXT("Runtime lifecycle shutdown requires type-lifetime ownership."));
	AngelscriptRuntimeGcMirror::AbandonRuntimeTrackedObjects();
	while (!RuntimeNativeTailChains.IsEmpty())
		RetireRuntimeTerminalClassChain(RuntimeNativeTailChains.CreateIterator().Key());
#if !SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    for (UASClass* Class : TObjectRange<UASClass>())
        Class->ReleaseRetiredScriptTypeForGc();
#endif
	AngelscriptRetiredModules::Shutdown();
	AngelscriptRuntimeGcMirror::VerifyRuntimeTrackedObjectsAbandoned();
	verifyf(SoloASBridge::QuiesceAndUnregisterRuntimeDestroyCallback(
		RuntimeLifecycleGeneration), TEXT("RUNTIME_GC_LIFECYCLE_QUIESCE_FAILED"));
}

void RegisterAngelscriptRuntimeGcLifecycle()
{
	UASClass::RegisterRuntimeGcLifecycleAdapter();
}

void ShutdownAngelscriptRuntimeGcLifecycle()
{
	UASClass::ShutdownGcLifecycleAdapter();
}

void UASClass::RejectUnknownNonGcOverwrite(
	UObject* Object,
	const TCHAR* Operation
)
{
	AngelscriptRuntimeGcMirror::FailRuntimeMirror(
		TEXT("NON_GC_OVERWRITE_UNSUPPORTED"), Object);
}


namespace AngelscriptRuntimeGcMirror
{
FCriticalSection RuntimeTrackedObjectsMutex;
TMap<FObjectKey, FRuntimeTrackedObject> RuntimeTrackedObjects;
TSet<FObjectKey> ExplicitlyDestroyedGarbageObjects;

bool ClaimRuntimeScriptDestructor(UObject* Object)
{
    FScopeLock Lock(&RuntimeTrackedObjectsMutex);
    const FObjectKey Key(Object);
    if (ExplicitlyDestroyedGarbageObjects.Contains(Key)) return false;
    ExplicitlyDestroyedGarbageObjects.Add(Key);
    return true;
}

void ResetRuntimeScriptDestructor(UObject* Object)
{
    FScopeLock Lock(&RuntimeTrackedObjectsMutex);
    ExplicitlyDestroyedGarbageObjects.Remove(FObjectKey(Object));
}

void CollectReleasedScriptDestructorClaims()
{
    FScopeLock Lock(&RuntimeTrackedObjectsMutex);
    for (auto It = ExplicitlyDestroyedGarbageObjects.CreateIterator(); It; ++It)
        if (It->ResolveObjectPtrEvenIfUnreachable() == nullptr) It.RemoveCurrent();
}
bool bRuntimeLifecycleShuttingDown = false;
namespace { uint64 NextReconstructionRevision = 1; }

bool UpdateRetainedRuntimeCookie(UObject* Object, asITypeInfo* ExpectedCookie, asITypeInfo* NewCookie,
    ERuntimeMirrorState ExpectedState, ERuntimeMirrorState NewState)
{
    asIScriptModule* OldModule = nullptr;
    uint64 Sequence = 0;
    {
        FScopeLock Lock(&RuntimeTrackedObjectsMutex);
        const auto* Entry = RuntimeTrackedObjects.Find(FObjectKey(Object));
        if (!Entry || Entry->ObjectIdentity != Object || Entry->State != ExpectedState
            || Entry->SavedTypeCookie != ExpectedCookie) return false;
        OldModule = Entry->RetainedModule;
        Sequence = Entry->TrackSequence;
    }
    asIScriptModule* NewModule = OldModule;
    if (OldModule && OldModule != NewCookie->GetModule())
    {
        NewModule = AngelscriptRetiredModules::RetainType(NewCookie);
        if (!NewModule) FailRuntimeMirror(TEXT("RETIRED_OBJECT_MODULE_MISSING"), Object);
    }
    const FObjectKey NewClassKey(Object->GetClass());
    const bool bReplacementCaptureExcluded = Cast<UASClass>(Object) != nullptr
        || Object->IsTemplate() || Object->HasAnyFlags(RF_ClassDefaultObject
            | RF_ArchetypeObject | RF_DefaultSubObject);
    bool bCommitted = false;
    {
        FScopeLock Lock(&RuntimeTrackedObjectsMutex);
        auto* Entry = RuntimeTrackedObjects.Find(FObjectKey(Object));
        if (Entry && Entry->ObjectIdentity == Object && Entry->State == ExpectedState
            && Entry->SavedTypeCookie == ExpectedCookie && Entry->RetainedModule == OldModule
            && Entry->TrackSequence == Sequence)
        {
            Entry->RetainedModule = NewModule;
            Entry->SavedTypeCookie = NewCookie;
            Entry->State = NewState;
            Entry->SavedClassKey = NewClassKey;
            Entry->bReplacementCaptureExcluded = bReplacementCaptureExcluded;
            bCommitted = true;
        }
    }
    if (bCommitted) ForgetRemovedRuntimeReceiver(Object, ExpectedCookie);
    if (NewModule != OldModule)
        AngelscriptRetiredModules::ReleaseModule(bCommitted ? OldModule : NewModule);
    return bCommitted;
}

#if WITH_DEV_AUTOMATION_TESTS
bool IsRuntimeReconstructionFaultEnabled(const TCHAR* FaultName)
{
	FString RequestedFault;
	return FParse::Value(
		FCommandLine::Get(),
		TEXT("SoloASRuntimeGcFault="),
		RequestedFault
	) && RequestedFault.Equals(FaultName, ESearchCase::CaseSensitive);
}

bool IsRuntimeReconstructionFaultTarget(const UObject* Object)
{
	return Object != nullptr
		&& Object->GetName().StartsWith(TEXT("RuntimeGcReconstructionFaultProbe"));
}
#endif

bool IsExactRuntimeReconstructionTypeCookie(
	UObject* Object,
	asITypeInfo* NewTypeCookie
)
{
	if (!IsValid(Object) || NewTypeCookie == nullptr)
	{
		return false;
	}

	UASClass* ObjectScriptClass = UASClass::GetFirstASClass(Object);
	return ObjectScriptClass != nullptr
		&& NewTypeCookie->GetUserData() == ObjectScriptClass
		&& NewTypeCookie == ObjectScriptClass->ScriptTypePtr;
}

static bool ClaimRuntimeTrackedMirrorForReconstructionImpl(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	bool bPlannedSoftReload
)
{
	checkf(IsAngelscriptExecutionWriterHeldByCurrentThread(),
		TEXT("Runtime reconstruction claim requires the execution writer."));
	if (Object == nullptr || ExpectedTypeCookie == nullptr || !PrepareRuntimeTerminalReconstruction(Object, ExpectedTypeCookie))
	{
		return false;
	}

	bool bCanClaim = false;
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		FRuntimeTrackedObject* Entry = RuntimeTrackedObjects.Find(FObjectKey(Object));
		bCanClaim = Entry != nullptr
			&& Entry->State == ERuntimeMirrorState::Pending
			&& Entry->SavedTypeCookie == ExpectedTypeCookie
			&& !bRuntimeLifecycleShuttingDown;
		if (bCanClaim)
		{
			// Claims consume revisions even when reconstruction later rolls back.
			// A saved payload witness must observe its one planned successful claim.
			if (NextReconstructionRevision == MAX_uint64 || Entry->ClaimCount == MAX_uint64) return false;
			Entry->ClaimRevision = NextReconstructionRevision++;
			++Entry->ClaimCount;
			Entry->ClaimOldCookie = ExpectedTypeCookie;
			Entry->ClaimEpoch = 0;
			Entry->ClaimExpectedCookie = nullptr;
			Entry->ClaimKind = EScriptObjectReloadPlan::RequiresReplacement;
			const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
			const auto* Plan = AngelscriptReloadSafety::FindScriptObjectReloadPlan(Object->GetClass());
			if (bPlannedSoftReload && !Object->IsTemplate()
				&& !Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_DefaultSubObject)
				&& Scope && Plan && Plan->PreviousCookie == ExpectedTypeCookie
				&& Plan->Kind == EScriptObjectReloadPlan::SoftReconstruction)
			{
				Entry->ClaimEpoch = Scope->Epoch;
				Entry->ClaimExpectedCookie = Plan->ExpectedCookie;
				Entry->ClaimKind = Plan->Kind;
			}
			Entry->State = ERuntimeMirrorState::Reconstructing;
		}
	}
	if (!bCanClaim)
	{
		// A failed ordinary claim has no pair context and cannot consume a
		// replacement tombstone. Pair callers fail their explicit reservation at
		// the pair boundary instead.
		return false;
	}
	return true;
}

bool ClaimRuntimeTrackedMirrorForReconstruction(UObject* Object, asITypeInfo* ExpectedCookie)
{
	return ClaimRuntimeTrackedMirrorForReconstructionImpl(Object, ExpectedCookie, false);
}

bool ClaimRuntimeTrackedMirrorForSoftReload(UObject* Object, asITypeInfo* ExpectedCookie)
{
	return ClaimRuntimeTrackedMirrorForReconstructionImpl(Object, ExpectedCookie, true);
}

bool CommitRuntimeReconstructionRevision(UObject* Object, asITypeInfo* PreviousCookie, asITypeInfo* CurrentCookie)
{
    // The caller has already committed Bridge, Mirror and pair finalization.
    FScopeLock Lock(&RuntimeTrackedObjectsMutex);
    auto* Entry = RuntimeTrackedObjects.Find(FObjectKey(Object));
    if (!Entry || bRuntimeLifecycleShuttingDown || Entry->ObjectIdentity != Object
        || Entry->State != ERuntimeMirrorState::Pending || Entry->SavedTypeCookie != CurrentCookie
        || !Entry->ClaimRevision || Entry->ClaimRevision <= Entry->CommittedRevision
        || Entry->ClaimOldCookie != PreviousCookie
        || (Entry->ClaimEpoch && Entry->ClaimExpectedCookie != CurrentCookie)) return false;
    Entry->CommittedRevision = Entry->ClaimRevision;
    return true;
}

bool UpdateRuntimeTrackedMirrorCookie(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	asITypeInfo* NewTypeCookie
)
{
	checkf(IsAngelscriptExecutionWriterHeldByCurrentThread(),
		TEXT("Runtime mirror cookie update requires the execution writer."));
	if (Object == nullptr || ExpectedTypeCookie == nullptr || NewTypeCookie == nullptr
		|| !IsExactRuntimeReconstructionTypeCookie(Object, NewTypeCookie))
	{
		return false;
	}

#if WITH_DEV_AUTOMATION_TESTS
	// This seam is opt-in, stateless, and absent from non-dev-automation
	// builds. RollbackRecovery deliberately reaches the fail-closed path below.
	if (IsRuntimeReconstructionFaultTarget(Object)
		&& (IsRuntimeReconstructionFaultEnabled(TEXT("MirrorCookieUpdate"))
			|| IsRuntimeReconstructionFaultEnabled(TEXT("RollbackRecovery"))))
	{
		return false;
	}
#endif

	return UpdateRetainedRuntimeCookie(Object, ExpectedTypeCookie, NewTypeCookie,
        ERuntimeMirrorState::Reconstructing, ERuntimeMirrorState::Pending);
}

bool ReadRuntimeDestroyTracking(const SoloASBridge::FRuntimeDestroyPreclaim& Source, uint64& OutSequence,
    asIScriptModule** OutRetainedModule)
{
    OutSequence = 0;
    if (OutRetainedModule) *OutRetainedModule = nullptr;
    if (!IsInGameThread() || !Source.Object || !Source.TypeCookie
        || Source.Key.ResolveObjectPtrEvenIfUnreachable() != Source.Object) return false;
    FScopeLock Lock(&RuntimeTrackedObjectsMutex);
    const auto* Entry = RuntimeTrackedObjects.Find(Source.Key);
    if (!Entry || Entry->State != ERuntimeMirrorState::Pending || Entry->ObjectIdentity != Source.Object
        || Entry->SavedTypeCookie != Source.TypeCookie || !Entry->TrackSequence || bRuntimeLifecycleShuttingDown) return false;
    OutSequence = Entry->TrackSequence;
    if (OutRetainedModule) *OutRetainedModule = Entry->RetainedModule;
    return true;
}

bool TransitionRuntimeTerminalTracking(const SoloASBridge::FRuntimeDeferredDestroyToken& Token,
    ERuntimeTerminalMirrorTransition Transition)
{
    if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread() || !Token.PlanId) return false;
    FScopeLock Lock(&RuntimeTrackedObjectsMutex);
    auto* Entry = RuntimeTrackedObjects.Find(Token.Key);
    const auto Before = Transition == ERuntimeTerminalMirrorTransition::Prepare
        ? ERuntimeMirrorState::Pending : ERuntimeMirrorState::TerminalPrepared;
    if (!Entry || Entry->State != Before || Entry->ObjectIdentity != Token.Object
        || Entry->SavedTypeCookie != Token.TypeCookie || Entry->TrackSequence != Token.TrackSequence
        || (Before == ERuntimeMirrorState::TerminalPrepared && Entry->TerminalPlanId != Token.PlanId)
        || bRuntimeLifecycleShuttingDown) return false;
    Entry->State = Transition == ERuntimeTerminalMirrorTransition::Prepare ? ERuntimeMirrorState::TerminalPrepared
        : Transition == ERuntimeTerminalMirrorTransition::Commit ? ERuntimeMirrorState::TerminalDeferred : ERuntimeMirrorState::Pending;
    Entry->TerminalPlanId = Transition == ERuntimeTerminalMirrorTransition::Abort ? 0 : Token.PlanId;
    return true;
}

namespace
{
bool IsLiveOrdinaryScriptReference(UObject* Object)
{
    if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread() || !IsValid(Object)
        || Object->IsUnreachable() || Object->IsTemplate()
        || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed | RF_ClassDefaultObject | RF_ArchetypeObject | RF_DefaultSubObject))
        return false;
    const UASClass* Class = Cast<UASClass>(Object->GetClass());
    if (!Class || !Class->ScriptTypePtr || Class->NewerVersion || Class->HasAnyClassFlags(CLASS_NewerVersionExists)) return false;
    for (const UClass* Super = Class->GetSuperClass(); Super; Super = Super->GetSuperClass())
    {
        const UASClass* ScriptSuper = Cast<UASClass>(Super);
        if (Super->HasAnyClassFlags(CLASS_CompiledFromBlueprint | CLASS_NewerVersionExists)
            || (ScriptSuper && ScriptSuper->NewerVersion)) return false;
    }
    for (const UObject* Outer = Object->GetOuter(); Outer; Outer = Outer->GetOuter())
    {
        if (!IsValid(Outer) || Outer->IsUnreachable() || Outer->IsTemplate()
            || Outer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) return false;
        for (const UClass* Owner = Outer->GetClass(); Owner; Owner = Owner->GetSuperClass())
            if (!Owner->HasAnyClassFlags(CLASS_Native) || Cast<UASClass>(Owner)
                || Owner->HasAnyClassFlags(CLASS_CompiledFromBlueprint | CLASS_NewerVersionExists)) return false;
    }
    return true;
}

bool HasCapturedScriptReferenceOuters(UObject* Object, const FLiveScriptReferenceWitness& Witness)
{
    int32 Index = 0;
    for (const UObject* Outer = Object->GetOuter(); Outer; Outer = Outer->GetOuter())
        if (!Witness.OuterKeys.IsValidIndex(Index) || FObjectKey(Outer) != Witness.OuterKeys[Index++]) return false;
    return Index == Witness.OuterKeys.Num();
}
}

bool CaptureLiveScriptReferenceWitness(UObject* Object, const FScriptObjectReloadPlan& Plan,
    uint64 Epoch, FLiveScriptReferenceWitness& Out, FString& Error)
{
    Out = {};
    const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
    const auto* ActivePlan = Object ? AngelscriptReloadSafety::FindScriptObjectReloadPlan(Object->GetClass()) : nullptr;
    if (!Scope || !Epoch || Scope->Epoch != Epoch || !IsLiveOrdinaryScriptReference(Object)
        || !ActivePlan || !Plan.ExpectedCookie || Object->GetClass() != Plan.Class
        || ActivePlan->Kind != Plan.Kind || ActivePlan->PreviousCookie != Plan.PreviousCookie
        || ActivePlan->ExpectedCookie != Plan.ExpectedCookie
        || (Plan.Kind == EScriptObjectReloadPlan::Unchanged && Plan.PreviousCookie != Plan.ExpectedCookie)
        || FObjectKey(Plan.Class) != Plan.ClassKey)
    { Error = TEXT("AS reference has no admitted class plan"); return false; }
    const FObjectKey Key(Object);
    FRuntimeTrackedObject Snapshot;
    {
        FScopeLock Lock(&RuntimeTrackedObjectsMutex);
        const auto* Entry = RuntimeTrackedObjects.Find(Key);
        if (!Entry || bRuntimeLifecycleShuttingDown || Entry->State != ERuntimeMirrorState::Pending
            || Entry->ObjectIdentity != Object || !Entry->TrackSequence || Entry->TerminalPlanId
            || Entry->SavedTypeCookie != Plan.PreviousCookie || Entry->ClaimRevision != Entry->CommittedRevision)
        { Error = TEXT("Live AS reference has no complete Pending lifecycle witness"); return false; }
        Snapshot = *Entry;
    }
    const auto* Class = CastChecked<UASClass>(Object->GetClass());
    const auto* Cookie = static_cast<asITypeInfo*>(Snapshot.SavedTypeCookie);
    if (Cookie != Class->ScriptTypePtr || !IsRegisteredRuntimeTypeIdentity(Cookie)
        || Cookie->GetUserData() != Class)
    { Error = TEXT("Live AS reference cookie is not the current registered class type"); return false; }
    Out.ObjectKey = Key;
    Out.ClassKey = Plan.ClassKey;
    Out.ObjectIdentity = Object;
    for (const UObject* Outer = Object->GetOuter(); Outer; Outer = Outer->GetOuter()) Out.OuterKeys.Add(FObjectKey(Outer));
    Out.PreviousCookie = Plan.PreviousCookie;
    Out.ExpectedCookie = Plan.ExpectedCookie;
    Out.TrackSequence = Snapshot.TrackSequence;
    Out.Revision = Snapshot.ClaimRevision;
    Out.CommittedRevision = Snapshot.CommittedRevision;
    Out.ClaimCount = Snapshot.ClaimCount;
    Out.Epoch = Epoch;
    Out.Kind = Plan.Kind;
    return true;
}

bool ValidatePreparedScriptReferenceWitness(UObject* Object, const FLiveScriptReferenceWitness& Witness, FString& Error)
{
    const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
    if (!Scope || Scope->Epoch != Witness.Epoch || !IsValid(Object) || Object->IsUnreachable() || Object->IsTemplate()
        || Object != Witness.ObjectIdentity || FObjectKey(Object) != Witness.ObjectKey
        || FObjectKey(Object->GetClass()) != Witness.ClassKey || !HasCapturedScriptReferenceOuters(Object, Witness))
    { Error = TEXT("Prepared AS reference identity or epoch changed"); return false; }
    FScopeLock Lock(&RuntimeTrackedObjectsMutex);
    const auto* Entry = RuntimeTrackedObjects.Find(Witness.ObjectKey);
    if (!Entry || bRuntimeLifecycleShuttingDown || Entry->State != ERuntimeMirrorState::Pending
        || Entry->ObjectIdentity != Object || Entry->TrackSequence != Witness.TrackSequence || Entry->TerminalPlanId
        || Entry->SavedTypeCookie != Witness.PreviousCookie || Entry->ClaimRevision != Witness.Revision
        || Entry->CommittedRevision != Witness.CommittedRevision || Entry->ClaimCount != Witness.ClaimCount)
    { Error = TEXT("Prepared AS reference lifecycle changed before replacement capture"); return false; }
    return true;
}

bool ValidateLiveScriptReferenceWitness(UObject* Object, const FLiveScriptReferenceWitness& Witness,
    FString& Error, FLiveScriptReferenceWitness* Current)
{
    const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
    if (!Scope || Scope->Epoch != Witness.Epoch || !IsLiveOrdinaryScriptReference(Object)
        || Object != Witness.ObjectIdentity || FObjectKey(Object) != Witness.ObjectKey
        || FObjectKey(Object->GetClass()) != Witness.ClassKey || !HasCapturedScriptReferenceOuters(Object, Witness))
    { Error = TEXT("Live AS reference identity or reload epoch changed"); return false; }
    FRuntimeTrackedObject Snapshot;
    {
        FScopeLock Lock(&RuntimeTrackedObjectsMutex);
        const auto* Entry = RuntimeTrackedObjects.Find(Witness.ObjectKey);
        if (!Entry || bRuntimeLifecycleShuttingDown || Entry->State != ERuntimeMirrorState::Pending
            || Entry->ObjectIdentity != Object || Entry->TrackSequence != Witness.TrackSequence || Entry->TerminalPlanId
            || Entry->SavedTypeCookie != Witness.ExpectedCookie || Entry->ClaimRevision != Entry->CommittedRevision)
        { Error = TEXT("Live AS reference has an incomplete or different lifecycle ledger"); return false; }
        Snapshot = *Entry;
    }
    const bool bUnchanged = Witness.Kind == EScriptObjectReloadPlan::Unchanged
        && Snapshot.ClaimRevision == Witness.Revision && Snapshot.ClaimCount == Witness.ClaimCount;
    const bool bRebuilt = Witness.Kind == EScriptObjectReloadPlan::SoftReconstruction
        && Witness.ClaimCount != MAX_uint64 && Snapshot.ClaimCount == Witness.ClaimCount + 1
        && Snapshot.ClaimRevision > Witness.Revision && Snapshot.ClaimEpoch == Witness.Epoch
        && Snapshot.ClaimKind == EScriptObjectReloadPlan::SoftReconstruction
        && Snapshot.ClaimOldCookie == Witness.PreviousCookie && Snapshot.ClaimExpectedCookie == Witness.ExpectedCookie;
    const auto* Class = CastChecked<UASClass>(Object->GetClass());
    const auto* Cookie = static_cast<asITypeInfo*>(Snapshot.SavedTypeCookie);
    if ((!bUnchanged && !bRebuilt) || Cookie != Class->ScriptTypePtr
        || !IsRegisteredRuntimeTypeIdentity(Cookie) || Cookie->GetUserData() != Class)
    { Error = TEXT("Live AS reference has no exact planned cookie transition"); return false; }
    if (Current)
    {
        *Current = Witness;
        Current->Revision = Snapshot.ClaimRevision;
        Current->CommittedRevision = Snapshot.CommittedRevision;
        Current->ClaimCount = Snapshot.ClaimCount;
    }
    return true;
}
}

#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
bool RequireAngelscriptTerminalFinalInt32ForExperiment(bool bStaleCaptureControl)
{
    if (!IsInGameThread() || ActiveTerminalOperation || ActiveRuntimeTerminalPlan
        || TerminalPlans.Num() || TerminalFinalInt32Providers.Num() || bRequireTerminalFinalInt32) return false;
    bRequireTerminalFinalInt32 = true;
    bTerminalStaleCaptureControl = bStaleCaptureControl;
    return true;
}

bool RequireAngelscriptTerminalRuntimeOwnedInt32ForExperiment(bool bStaleCaptureControl)
{
    if (!RequireAngelscriptTerminalFinalInt32ForExperiment(bStaleCaptureControl)) return false;
    bTerminalRuntimeOwnedInt32 = true;
    return true;
}

static bool RegisterTerminalFinalInt32(const FAngelscriptTerminalFinalInt32Registration& R,
    FAngelscriptTerminalFinalInt32Ticket& Out, bool bNativeTail)
{
    Out = {};
    if (!IsInGameThread() || !bRequireTerminalFinalInt32 || ActiveTerminalOperation || ActiveRuntimeTerminalPlan
        || !R.Receiver || !R.Scalar || !R.Owned || !R.PayloadType || !R.Payload || !R.ProviderIdentity
        || !R.DeclaredEpoch || !R.DeclaredProvider || !R.ProviderEpoch
        || R.bRuntimeOwnedOriginal != bTerminalRuntimeOwnedInt32
        || R.ProviderEpoch <= LastTerminalFinalProviderEpoch || NextTerminalInt32Epoch == MAX_uint64
        || R.Receiver->IsTemplate() || R.Receiver->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) return false;
    FAngelscriptExecutionWriteScope Writer;
    auto* Class = Cast<UASClass>(R.Receiver->GetClass());
    const auto* Chain = bNativeTail ? TerminalNativeTailChain.Get() : nullptr;
    const int32 NativePrefix = Chain ? Chain->NativePrefix : int32(sizeof(UObject));
    UASClass* Declaration = Chain ? Chain->Class : Class;
    auto* Head = Chain ? FindTerminalNativeTailHead(*const_cast<FTerminalNativeTailChain*>(Chain), Class) : nullptr;
    if (bNativeTail && (!Chain || !IsTerminalNativeTailClass(*Chain, Class) || R.Owned != Chain->Owned || R.Scalar != Chain->Scalar
        || !R.bRuntimeOwnedOriginal || !ValidateTerminalNativeTailChain(*Chain))) return false;
    if (!Class || Class->GetClass() != UASClass::StaticClass()
        || !Declaration || Declaration->GetSuperClass() != (Chain ? Chain->Native : UObject::StaticClass())
        || Declaration->CodeSuperClass != (Chain ? Chain->Native : UObject::StaticClass()) || Class->bLayoutChanging
        || (R.bRuntimeOwnedOriginal && Class->GetPropertiesStartOffset() != 0)
        || Declaration->ScriptPropertyOffset < NativePrefix || !Class->ScriptTypePtr
        || FindFProperty<FIntProperty>(Class, R.Scalar->GetFName()) != R.Scalar
        || FindFProperty<FStructProperty>(Class, R.Owned->GetFName()) != R.Owned
        || R.Scalar->GetOwnerStruct() != Declaration || R.Scalar->GetClass() != FIntProperty::StaticClass()
        || R.Scalar->ArrayDim != 1 || R.Scalar->GetSize() != sizeof(int32)
        || R.Owned->GetOwnerStruct() != Declaration || R.Owned->Struct != FInstancedStruct::StaticStruct()
        || !Declaration->UeDelayedDestroyFields.Contains(R.Owned)) return false;
    if (R.bRuntimeOwnedOriginal)
    {
        const auto* PayloadOps = R.PayloadType->GetCppStructOps();
        const auto* WrapperOps = R.Owned->Struct->GetCppStructOps();
        if (!PayloadOps || !WrapperOps || !PayloadOps->HasDestructor() || !WrapperOps->HasDestructor()
            || (R.PayloadType->StructFlags & (STRUCT_IsPlainOldData | STRUCT_NoDestructor))
            || (R.Owned->Struct->StructFlags & (STRUCT_IsPlainOldData | STRUCT_NoDestructor))
            || R.PayloadType->GetPropertiesSize() > PayloadOps->GetSize()
            || R.Owned->Struct->GetPropertiesSize() > WrapperOps->GetSize()
            || WrapperOps->GetSize() != sizeof(FInstancedStruct)) return false;
    }
    auto* Type = CastToObjectType(static_cast<asCTypeInfo*>(static_cast<asITypeInfo*>(Class->ScriptTypePtr)));
    void* Storage = R.Scalar->ContainerPtrToValuePtr<void>(R.Receiver);
    const int32 Offset = R.Scalar->GetOffset_ForInternal();
    const auto* OwnerType = Declaration ? CastToObjectType(static_cast<asCTypeInfo*>(static_cast<asITypeInfo*>(Declaration->ScriptTypePtr))) : nullptr;
    if (!Type || !OwnerType || OwnerType->derivedFrom
        || (Type != OwnerType && (!Head || Type->derivedFrom != OwnerType || Type->localProperties.GetLength()
            || Type->size != OwnerType->size || Type->module != OwnerType->module))
        || Offset < FMath::Max(NativePrefix, int32(sizeof(asCScriptObject)))
        || Type->properties.GetLength() > 1024
        || (R.bRuntimeOwnedOriginal && (Type->properties.GetLength() != 2 || OwnerType->localProperties.GetLength() != 2))
        || !Cuviss::TerminalAdmission::IsTerminalInt32StorageRange(Offset, Declaration->ScriptPropertyOffset,
            Class->GetPropertiesSize(), Type->size, reinterpret_cast<UPTRINT>(R.Receiver), reinterpret_cast<UPTRINT>(Storage))) return false;
    uint32 ScalarMatches = 0;
    for (asUINT Index = 0; Index != Type->properties.GetLength(); ++Index)
    {
        const auto* Member = Type->properties[Index];
        if (!Member || Member->byteOffset < 0 || Member->type.GetSizeInMemoryBytes() <= 0) return false;
        if (Member->byteOffset == Offset)
        {
            if (++ScalarMatches != 1 || Member->type != asCDataType::CreatePrimitive(ttInt, false)
                || Member->type.IsReference() || Member->type.IsObjectHandle() || Member->compositeOffset
                || Member->isCompositeIndirect || Member->bUeDelayedDestroyOwner || Member->isAppBindProperty
                || Member->type.GetSizeInMemoryBytes() != sizeof(int32)
                || R.Scalar->GetFName() != FName(ANSI_TO_TCHAR(Member->name.AddressOf()))) return false;
        }
        else if (Cuviss::TerminalAdmission::TerminalStorageRangesOverlap(
            Offset, sizeof(int32), Member->byteOffset, Member->type.GetSizeInMemoryBytes())) return false;
    }
    if (ScalarMatches != 1 || Class->DestructorLink != (Head ? Head->OwnedProxy.Get() : R.Owned)
        || R.Owned->DestructorLinkNext != (Head ? Chain->Original : (Chain ? Chain->Adapter.Get() : nullptr))) return false;
    uint32 Seen = 0;
    for (const FProperty* P = Class->PropertyLink; P; P = P->PropertyLinkNext)
    {
        if (++Seen > 1024 || P->GetSize() <= 0 || P->GetOffset_ForInternal() < 0) return false;
        if (P != R.Scalar && Cuviss::TerminalAdmission::TerminalStorageRangesOverlap(
            Offset, sizeof(int32), P->GetOffset_ForInternal(), P->GetSize())) return false;
    }
    if (R.bRuntimeOwnedOriginal && Seen != (Chain ? 3u : 2u)) return false;
    const auto* Wrapper = R.Owned->ContainerPtrToValuePtr<FInstancedStruct>(R.Receiver);
    if (Wrapper->GetScriptStruct() != R.PayloadType || Wrapper->GetMemory() != R.Payload) return false;
    const UPTRINT Begin = reinterpret_cast<UPTRINT>(R.Payload);
    const uint32 Size = R.PayloadType->GetStructureSize();
    const UPTRINT ScalarAt = reinterpret_cast<UPTRINT>(Storage);
    // Reset frees only this separate native payload allocation. It cannot free
    // the scalar's reserved receiver allocation or end the independent int.
    if (R.bRuntimeOwnedOriginal && (!Size || Begin > MAX_uint64 - Size
        || (ScalarAt < Begin + Size && Begin < ScalarAt + sizeof(int32)))) return false;
    const auto Within = [Begin, Size](const void* Address, uint32 Width)
    {
        const UPTRINT At = reinterpret_cast<UPTRINT>(Address);
        return At >= Begin && Width <= Size && At - Begin <= Size - Width;
    };
    if (!Within(R.DeclaredEpoch, sizeof(uint64)) || !Within(R.DeclaredProvider, sizeof(void*))
        || reinterpret_cast<UPTRINT>(R.DeclaredEpoch) % alignof(uint64)
        || reinterpret_cast<UPTRINT>(R.DeclaredProvider) % alignof(void*)
        || *R.DeclaredEpoch != R.ProviderEpoch || *R.DeclaredProvider != R.ProviderIdentity) return false;
    for (const auto& Pair : TerminalFinalInt32Providers)
        if (Pair.Value->Registration.Receiver == R.Receiver || Pair.Value->Ticket.Storage == Storage) return false;
    if (R.Scalar->GetPropertyValue(Storage) != R.InitialValue) return false;
    auto Provider = MakeUnique<FTerminalFinalInt32Provider>();
    Provider->Registration = R;
    Provider->ReceiverKey = FObjectKey(R.Receiver);
    Provider->Ticket.ProviderEpoch = R.ProviderEpoch;
    Provider->Ticket.SlotEpoch = ++NextTerminalInt32Epoch;
    // This replaces the live AS primitive only in this explicit experiment. The
    // placement-created int is a COMPLETE object in extra allocation tail storage,
    // outside every native UObject byte and every other reflected field.
    Provider->Ticket.Storage = ::new (Storage) int32(R.InitialValue);
    Provider->ConstructOrder = ++TerminalFinalInt32Order;
    Out = Provider->Ticket;
    LastTerminalFinalProviderEpoch = R.ProviderEpoch;
    TerminalFinalInt32Providers.Add(R.ProviderEpoch, MoveTemp(Provider));
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
        TEXT("SOLOAS_TERMINAL_FINAL_ORIGINAL_BEGIN object=%p offset=%d prefix=%u width=4 properties=%d ")
        TEXT("provider_epoch=%llu slot_epoch=%llu initial=%d complete_int=1 overlap=0 before_prepare=1"),
        R.Receiver, Offset, uint32(NativePrefix), Class->GetPropertiesSize(), Out.ProviderEpoch, Out.SlotEpoch, R.InitialValue);
    return true;
}

bool RegisterAngelscriptTerminalFinalInt32ForExperiment(
    const FAngelscriptTerminalFinalInt32Registration& R, FAngelscriptTerminalFinalInt32Ticket& Out)
{
    return RegisterTerminalFinalInt32(R, Out, false);
}

#if WITH_DEV_AUTOMATION_TESTS
bool RegisterAngelscriptTerminalNativeTailInt32ForTest(
    const FAngelscriptTerminalFinalInt32Registration& R, FAngelscriptTerminalFinalInt32Ticket& Out)
{
    return RegisterTerminalFinalInt32(R, Out, true);
}

bool ReadActiveAngelscriptTerminalNativeTailInt32ForTest(UObject* Receiver,
    const FAngelscriptTerminalFinalInt32Ticket& Ticket, int32& OutValue)
{
    auto* Plan = ActiveRuntimeTerminalPlan;
    if (!IsInGameThread() || !Plan || ActiveTerminalOperation != Plan || Plan->bDevFacade
        || Plan->Phase != ETerminalPlanPhase::Callback || Plan->Token.Object != Receiver
        || !Plan->NativeTailChain || !Plan->bNativeTailReturned || !Plan->bRuntimeOwnedFieldReturned
        || !Plan->ModulePin || !ValidateTerminalPlan(*Plan, true)
        || Plan->Int32Phase != Cuviss::TerminalAdmission::EInt32SlotPhase::FreshLive
        || Plan->FreshInt32 != Ticket.Storage || Plan->FreshInt32Epoch != Ticket.SlotEpoch
        || Plan->Int32Slot.FinalProviderEpoch != Ticket.ProviderEpoch
        || Plan->FinalInt32Epoch != Ticket.SlotEpoch || !Plan->Int32Slot.bRuntimeOwnedOriginal
        || !Cuviss::TerminalAdmission::IsTerminalCapturedInt32Slot(Plan->Int32Slot)) return false;
    // Observation of the proved fresh complete int during the actual AS callback.
    // Returning a value grants no permission to read it again after callback cleanup.
    OutValue = *Plan->FreshInt32;
    return true;
}
#endif

namespace
{
FTerminalDestroyPlan* FindTerminalFinalWriterPlan(const FAngelscriptTerminalFinalInt32Ticket& Ticket,
    const void* ProviderIdentity, FTerminalFinalInt32Provider*& OutProvider)
{
    OutProvider = nullptr;
    if (!IsInGameThread() || ActiveTerminalOperation || ActiveRuntimeTerminalPlan || !bRequireTerminalFinalInt32) return nullptr;
    const auto* Entry = TerminalFinalInt32Providers.Find(Ticket.ProviderEpoch);
    if (!Entry || !Entry->IsValid()) return nullptr;
    auto* Provider = Entry->Get();
    if (Provider->Ticket.Storage != Ticket.Storage || Provider->Ticket.SlotEpoch != Ticket.SlotEpoch
        || Provider->Registration.ProviderIdentity != ProviderIdentity || !Provider->PlanId || Provider->bEnded) return nullptr;
    const auto* PlanEntry = TerminalPlans.Find(Provider->ReceiverKey);
    auto* Plan = PlanEntry && PlanEntry->IsValid() ? PlanEntry->Get() : nullptr;
    if (!Plan || Plan->Phase != ETerminalPlanPhase::Deferred || Plan->Token.PlanId != Provider->PlanId
        || Plan->FinalInt32Provider != Provider || Plan->Int32Slot.Epoch != Ticket.SlotEpoch
        || Plan->Int32Slot.Storage != Ticket.Storage || Plan->OriginalCompleted != 0
        || Plan->Fields.Num() != 1 || Plan->Fields[0].Phase != ETerminalFieldPhase::OriginalLive) return nullptr;
    OutProvider = Provider;
    return Plan;
}
}

bool BeginAngelscriptTerminalFinalInt32WriterForExperiment(const FAngelscriptTerminalFinalInt32Ticket& Ticket,
    const void* ProviderIdentity, const void* NativePayload)
{
    FTerminalFinalInt32Provider* Provider = nullptr;
    if (!FindTerminalFinalWriterPlan(Ticket, ProviderIdentity, Provider) || Provider->Registration.bRuntimeOwnedOriginal
        || Provider->bWriting || Provider->bPublished
        || Provider->Registration.Payload != NativePayload) return false;
    Provider->bWriting = true;
    Provider->WriteOrder = ++TerminalFinalInt32Order;
    return true;
}

bool PublishAngelscriptTerminalFinalInt32ForExperiment(const FAngelscriptTerminalFinalInt32Ticket& Ticket,
    const void* ProviderIdentity, int32 FinalValue)
{
    FTerminalFinalInt32Provider* Provider = nullptr;
    if (!FindTerminalFinalWriterPlan(Ticket, ProviderIdentity, Provider) || Provider->Registration.bRuntimeOwnedOriginal
        || !Provider->bWriting || Provider->bPublished) return false;
    // No UObject method, scalar read, log sink, script context or virtual callback.
    Provider->FinalValue = FinalValue;
    Provider->bPublished = true;
    Provider->PublishOrder = ++TerminalFinalInt32Order;
    return true;
}

bool EndAngelscriptTerminalFinalInt32ForExperiment(const FAngelscriptTerminalFinalInt32Ticket& Ticket,
    const void* ProviderIdentity)
{
    FTerminalFinalInt32Provider* Provider = nullptr;
    if (!FindTerminalFinalWriterPlan(Ticket, ProviderIdentity, Provider) || Provider->Registration.bRuntimeOwnedOriginal
        || !Provider->bWriting || !Provider->bPublished) return false;
    Provider->bEnded = true;
    Provider->EndOrder = ++TerminalFinalInt32Order;
    return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
bool InstallTerminalIndependentNativeTailHeads(FTerminalNativeTailChain& C,
    const FAngelscriptTerminalNativeTailRegistrationForTest& R)
{
    if (!R.EmptyChildren[0] || !R.EmptyChildren[1] || R.EmptyChildren[0] == R.EmptyChildren[1]
        || R.EmptyChildren[0] == R.Class || R.EmptyChildren[1] == R.Class) return false;
    C.bIndependentHeads = true;
    C.Heads.SetNum(3);
    UASClass* Classes[] = {R.Class, R.EmptyChildren[0], R.EmptyChildren[1]};
    for (int32 Index = 0; Index != 3; ++Index)
    {
        auto& Head = C.Heads[Index];
        Head.Class = Classes[Index];
        if (!Head.Class || Head.Class->GetClass() != UASClass::StaticClass() || !Head.Class->ScriptTypePtr
            || Head.Class->GetSuperClass() != (Index ? R.Class : R.NativeClass)
            || Head.Class->DestructorLink != R.Owned || Head.Class->GetPropertiesSize() < C.PropertiesSize) return false;
        Head.Type = CastToObjectType(static_cast<asCTypeInfo*>(static_cast<asITypeInfo*>(Head.Class->ScriptTypePtr)));
        if (!Head.Type || !AngelscriptRuntimeGcMirror::IsRegisteredRuntimeTypeIdentity(Head.Type)) return false;
        Head.Key = FObjectKey(Head.Class); Head.Index = GUObjectArray.ObjectToIndex(Head.Class);
        Head.Serial = GUObjectArray.GetSerialNumber(Head.Index);
        Head.ScriptOffset = Head.Class->ScriptPropertyOffset; Head.CodeSuper = Head.Class->CodeSuperClass;
        Head.TypeSize = Head.Type->size; Head.TypeId = Head.Type->typeId;
        Head.ContainerSize = Head.Class->GetPropertiesSize();
        Head.Children = Head.Class->ChildProperties; Head.Properties = Head.Class->PropertyLink;
        Head.References = Head.Class->RefLink; Head.PostConstruct = Head.Class->PostConstructLink;
        Head.OriginalHead = Head.Class->DestructorLink; Head.bWasRooted = Head.Class->IsRooted();
        Head.OwnedProxy = MakeUnique<FTerminalNativeOwnedProperty>(Head.Class, *C.Owned);
        Head.TailProxy = MakeUnique<FTerminalNativeTailProperty>(Head.Class, *C.Original);
        Head.OwnedProxy->DestructorLinkNext = Head.TailProxy.Get();
    }
    if (!C.Heads[0].Type || C.Heads[0].Type->localProperties.GetLength() != 2) return false;
    for (asUINT Index = 0; Index != 2; ++Index) C.Members[Index] = C.Heads[0].Type->localProperties[Index];
    for (TObjectIterator<UObject> It; It; ++It)
        if (IsTerminalNativeTailClass(C, Cast<UASClass>(It->GetClass())) && !It->HasAnyFlags(RF_ClassDefaultObject)) return false;
    C.bNativeWasRooted = C.Native->IsRooted();
    for (auto& Head : C.Heads) if (!Head.bWasRooted) Head.Class->AddToRoot();
    if (!C.bNativeWasRooted) C.Native->AddToRoot();
    if (!ValidateTerminalIndependentNativeTailChain(C, false))
    {
        for (auto& Head : C.Heads) if (!Head.bWasRooted) Head.Class->RemoveFromRoot();
        if (!C.bNativeWasRooted) C.Native->RemoveFromRoot();
        return false;
    }
    for (auto& Head : C.Heads) Head.Class->DestructorLink = Head.OwnedProxy.Get();
    return true;
}

FAngelscriptTerminalNativeTailScopeForTest::FAngelscriptTerminalNativeTailScopeForTest(
    const FAngelscriptTerminalNativeTailRegistrationForTest& R, bool bEnable)
{
    if (!bEnable || !IsInGameThread() || IsGarbageCollecting() || GExitPurge || TerminalNativeTailChain
        || !bRequireTerminalFinalInt32 || !bTerminalRuntimeOwnedInt32
        || ActiveTerminalOperation || ActiveRuntimeTerminalPlan || TerminalPlans.Num()
        || !R.Class || !R.NativeClass || !R.Owned || !R.NativeTail || !R.Scalar
        || R.NativePrefixBytes <= int32(sizeof(UObject))) return;
    FAngelscriptExecutionWriteScope Writer;
    auto C = MakeUnique<FTerminalNativeTailChain>();
    C->Class = R.Class; C->Native = R.NativeClass; C->Owned = R.Owned; C->Original = R.NativeTail; C->Scalar = R.Scalar;
    C->NativePrefix = R.NativePrefixBytes; C->PropertiesSize = R.Class->GetPropertiesSize();
    C->OwnedOffset = R.Owned->GetOffset_ForInternal(); C->TailOffset = R.NativeTail->GetOffset_ForInternal();
    C->ScalarOffset = R.Scalar->GetOffset_ForInternal(); C->OwnedFlags = uint64(R.Owned->GetPropertyFlags());
    C->TailFlags = uint64(R.NativeTail->GetPropertyFlags()); C->ScalarFlags = uint64(R.Scalar->GetPropertyFlags());
    C->ClassKey = FObjectKey(R.Class); C->NativeKey = FObjectKey(R.NativeClass);
    C->ClassIndex = GUObjectArray.ObjectToIndex(R.Class); C->ClassSerial = GUObjectArray.GetSerialNumber(C->ClassIndex);
    C->NativeIndex = GUObjectArray.ObjectToIndex(R.NativeClass); C->NativeSerial = GUObjectArray.GetSerialNumber(C->NativeIndex);
    C->ChildProperties = R.Class->ChildProperties; C->PropertyLink = R.Class->PropertyLink;
    C->RefLink = R.Class->RefLink; C->PostConstructLink = R.Class->PostConstructLink;
    C->NativeRefLink = R.NativeClass->RefLink; C->NativePostConstructLink = R.NativeClass->PostConstructLink;
    C->NativeTailName = R.NativeTail->GetFName();
    const FProperty* P = R.Class->PropertyLink;
    for (int Index = 0; Index != 3; ++Index)
    {
        if (!P || (P != R.Owned && P != R.NativeTail && P != R.Scalar)) return;
        for (int Seen = 0; Seen != Index; ++Seen) if (C->Properties[Seen] == P) return;
        C->Properties[Index] = P; C->PropertyNext[Index] = P->PropertyLinkNext; C->FieldNext[Index] = P->Next;
        C->RefNext[Index] = P->NextRef; C->PostNext[Index] = P->PostConstructLinkNext;
        P = P->PropertyLinkNext;
    }
    if (P || NextTerminalNativeTailEpoch == MAX_uint64) return;
    for (TObjectIterator<UObject> It; It; ++It)
        if (It->GetClass() == R.Class && !It->HasAnyFlags(RF_ClassDefaultObject)) return;
    TArray<const FStructProperty*> Encountered;
    const auto* Ops = R.NativeTail->Struct ? R.NativeTail->Struct->GetCppStructOps() : nullptr;
    if (!Ops || !Ops->HasClearOnFinishDestroy() || !R.NativeTail->ContainsFinishDestroy(Encountered)
        || C->OwnedOffset > C->PropertiesSize - int32(sizeof(FInstancedStruct))
        || C->ScalarOffset > C->PropertiesSize - int32(sizeof(int32))
        || Cuviss::TerminalAdmission::TerminalStorageRangesOverlap(C->OwnedOffset, sizeof(FInstancedStruct), C->ScalarOffset, sizeof(int32))) return;
    if (R.EmptyChildren[0] || R.EmptyChildren[1])
    {
        if (!InstallTerminalIndependentNativeTailHeads(*C, R)) return;
        C->Epoch = Epoch = ++NextTerminalNativeTailEpoch;
        TerminalNativeTailChain = MoveTemp(C);
        UE_LOG(
            LogAngelscriptRuntimeGcLifecycle,
            Display,
            TEXT("SOLOAS_TERMINAL_NATIVE_TAIL_SCOPE epoch=%llu admitted=1 nodes=2 classes=3 independent_heads=1 original_next_unchanged=1 storage_added=0"),
            Epoch);
        return;
    }
    C->Adapter = MakeUnique<FTerminalNativeTailProperty>(R.Class, *R.NativeTail);
    C->bClassWasRooted = R.Class->IsRooted(); C->bNativeWasRooted = R.NativeClass->IsRooted();
    if (!C->bClassWasRooted) R.Class->AddToRoot();
    if (!C->bNativeWasRooted) R.NativeClass->AddToRoot();
    if (!ValidateTerminalNativeTailChain(*C, false))
    {
        if (!C->bClassWasRooted) R.Class->RemoveFromRoot();
        if (!C->bNativeWasRooted) R.NativeClass->RemoveFromRoot();
        return;
    }
    C->Epoch = Epoch = ++NextTerminalNativeTailEpoch;
    R.Owned->DestructorLinkNext = C->Adapter.Get();
    TerminalNativeTailChain = MoveTemp(C);
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
        TEXT("SOLOAS_TERMINAL_NATIVE_TAIL_SCOPE epoch=%llu admitted=1 nodes=2 off_list=1 native_prefix=%d storage_added=0"), Epoch, R.NativePrefixBytes);
}

FAngelscriptTerminalNativeTailScopeForTest::~FAngelscriptTerminalNativeTailScopeForTest()
{
    if (!Epoch) return;
    auto* C = TerminalNativeTailChain.Get();
    if (!IsInGameThread() || IsGarbageCollecting() || !C || C->Epoch != Epoch || C->bNativeCallout || C->bOwnedCallout || ActiveTerminalOperation
        || ActiveRuntimeTerminalPlan || TerminalPlans.Num() || !ValidateTerminalNativeTailChain(*C))
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_SCOPE_RELEASE_UNQUALIFIED_NO_RETRY"));
    for (TObjectIterator<UObject> It; It; ++It)
        if (IsTerminalNativeTailClass(*C, Cast<UASClass>(It->GetClass())) && !It->HasAnyFlags(RF_ClassDefaultObject))
        {
            // A rooted unregistered control has never entered the deferred route.
            // Restoring the native node preserves its later ordinary destruction.
            const FObjectKey ControlKey(*It);
            if (!It->IsRooted() || It->IsUnreachable() || It->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_SCOPE_RELEASE_WITH_PENDING_INSTANCE_NO_RETRY"));
            for (const auto& Pair : TerminalFinalInt32Providers)
                if (Pair.Value->ReceiverKey == ControlKey)
                    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_SCOPE_RELEASE_WITH_REGISTERED_INSTANCE_NO_RETRY"));
        }
    if (C->bIndependentHeads)
    {
        for (const auto& Pair : TerminalFinalInt32Providers)
            if (Pair.Value->Registration.Owned == C->Owned && !Pair.Value->bRuntimeCleanupComplete)
                UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_SCOPE_RELEASE_WITH_OPEN_PROVIDER_NO_RETRY"));
        // Restore every class head while both proxies and rooted declaration
        // metadata still exist. Original shared Next links are never written.
        for (auto& Head : C->Heads) Head.Class->DestructorLink = Head.OriginalHead;
        if (!ValidateTerminalNativeTailChain(*C, false))
            UE_LOG(LogAngelscriptRuntimeGcLifecycle, Fatal, TEXT("TERMINAL_NATIVE_TAIL_SCOPE_RESTORE_DRIFT_NO_RETRY"));
        for (auto It = TerminalFinalInt32Providers.CreateIterator(); It; ++It)
            if (It.Value()->Registration.Owned == C->Owned) It.RemoveCurrent();
        for (auto& Head : C->Heads) if (!Head.bWasRooted) Head.Class->RemoveFromRoot();
        if (!C->bNativeWasRooted) C->Native->RemoveFromRoot();
        TerminalNativeTailChain.Reset();
        UE_LOG(
            LogAngelscriptRuntimeGcLifecycle,
            Display,
            TEXT("SOLOAS_TERMINAL_NATIVE_TAIL_SCOPE_END epoch=%llu original_heads_restored=3 original_next_unchanged=1 adapters_released=6 providers_closed=1"),
            Epoch);
        return;
    }
    C->Owned->DestructorLinkNext = const_cast<FStructProperty*>(C->Original);
    if (!C->bClassWasRooted) C->Class->RemoveFromRoot();
    if (!C->bNativeWasRooted) C->Native->RemoveFromRoot();
    TerminalNativeTailChain.Reset();
    UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
        TEXT("SOLOAS_TERMINAL_NATIVE_TAIL_SCOPE_END epoch=%llu original_link_restored=1 adapter_released=1"), Epoch);
}
#endif
