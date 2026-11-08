// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once

#include "CoreMinimal.h"
#include "AngelscriptCoreExtensionPoints.h"
#include "UObject/GCObject.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/ScriptDelegates.h"
#include "Lambda/SoloASLambdaCaptureStore.h"

class UWorld;
class FProperty;
class USparseDelegateFunction;
struct FTimerHandle;
namespace SoloASBridge
{
// Creates the fallback object for native callers; script rewrites use typed slots.
SOLOASCOREEXTENSIONS_API UObject* CreateLambdaClosure(UObject* Owner, UClass* ClosureClass);

struct SOLOASCOREEXTENSIONS_API FLambdaClosureInvocation : FGCObject
{
    TStrongObjectPtr<UObject> Target;
    TStrongObjectPtr<UObject> Owner;
    FName Entry;
    FName Store;
    int32 Generation = 0;
    uint64 Lease = 0;
    bool bDirect = false;
    bool bStorageBoundAtEntry = false;
    bool bCancelled = false;
    TArray<uint64> Items;
    AngelscriptCoreExtensionPoints::FModuleGenerationLease CaptureLease;
    TArray<FInstancedStruct> Captures;
    FLambdaClosureInvocation(UObject* Object, FName InEntry);
    ~FLambdaClosureInvocation() override;
    int32 Num() const { return bDirect ? Items.Num() : Captures.Num(); }
    bool IsBound() const;
    bool IsItemLive(int32 Index) const;
    bool Read(int32 Index, UScriptStruct* Type, void* Destination) const;
    bool Commit(int32 Index, UScriptStruct* Type, const void* Source);
    void AddReferencedObjects(FReferenceCollector& Collector) override;
    FString GetReferencerName() const override { return TEXT("Lambda capture invocation"); }
};

// Delegate storage is borrowed during these calls. Reflected address hints are
// revalidated through weak holder/property identities and never dereferenced.
SOLOASCOREEXTENSIONS_API UObject* PrepareLambdaSlot(UObject* Owner, FName Site, FName Store, FName Body,
    const void* Delegate, bool bMulticast, bool bTimer, int32 SlotsPerSite,
    UScriptStruct* CaptureType, const void* Capture, FName& OutEntry, FScriptLambdaHandle& OutHandle, USparseDelegateFunction* Sparse = nullptr,
    UObject* Holder = nullptr, FName RootProperty = NAME_None);
SOLOASCOREEXTENSIONS_API void TrackLambdaSlot(UObject* Target, FName Entry, const void* Delegate);
SOLOASCOREEXTENSIONS_API void TrackLambdaTimer(const FScriptDelegate& Delegate, UWorld* World, FTimerHandle Handle);
SOLOASCOREEXTENSIONS_API void SweepLambdaClosures();
// Isolated descriptors for ordinary AS fields; they are not added to UClass's
// reflection chain and do not expose script members to Blueprint.
SOLOASCOREEXTENSIONS_API void ResetLambdaScriptFields();
SOLOASCOREEXTENSIONS_API void RegisterLambdaScriptFields(UClass* Class, TArray<TUniquePtr<FProperty>>&& Fields, TMap<FName, int32>&& Offsets);
SOLOASCOREEXTENSIONS_API SIZE_T GetLambdaScriptFieldSize(UClass* Class);
SOLOASCOREEXTENSIONS_API void* GetLambdaStorageAddress(UObject* Owner, FName Name);
SOLOASCOREEXTENSIONS_API FProperty* FindLambdaStorageProperty(UClass* Class, FName Name);
SOLOASCOREEXTENSIONS_API void DispatchLambdaFallback(UObject* Target, UFunction* Function, void* Parameters);
SOLOASCOREEXTENSIONS_API void RemoveLambdaHandle(void* Delegate, bool bMulticast, const FScriptLambdaHandle& Handle, USparseDelegateFunction* Sparse = nullptr);
SOLOASCOREEXTENSIONS_API void RemoveLambdaClosureBindings(FMulticastScriptDelegate& Delegate, UObject* Owner);
SOLOASCOREEXTENSIONS_API void RemoveSparseLambdaBindings(void* Delegate, USparseDelegateFunction* Signature, UObject* Owner);
SOLOASCOREEXTENSIONS_API void ReleaseLambdaSlotsForClasses(const TArray<UClass*>& Classes);
// Call before copying a script delegate into unreflected C++ storage. Those
// captures then use owner lifetime and their identities are never pooled.
SOLOASCOREEXTENSIONS_API void RetainLambdaForNativeStorage(const FScriptDelegate& Delegate);
SOLOASCOREEXTENSIONS_API void RetainLambdaForNativeStorage(const FMulticastScriptDelegate& Delegate);
struct FLambdaClosureStats
{
    int32 CreatedObjects = 0;
    int32 ReusedObjects = 0;
    int32 ActiveObjects = 0;
    int32 IdleObjects = 0;
    int32 ActiveSlots = 0;
    uint64 ObjectScans = 0;
    uint64 ObjectsVisited = 0;
    double ObjectScanSeconds = 0;
};
SOLOASCOREEXTENSIONS_API FLambdaClosureStats GetLambdaClosureStats();
SOLOASCOREEXTENSIONS_API int32 GetLambdaCaptureCount(UObject* Owner);
}
