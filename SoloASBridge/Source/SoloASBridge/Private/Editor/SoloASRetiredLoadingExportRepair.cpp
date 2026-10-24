// Copyright Cuviss Studio. All Rights Reserved.
#include "Editor/SoloASRetiredLoadingExportRepair.h"
#include "Editor/SoloASCopyProperties.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTLS.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/LinkerLoad.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoloASRetiredLoadingExportRepair, Log, All);
static TAutoConsoleVariable<int32> CVarRepairRetiredLoadingExports(
    TEXT("soloas.RepairRetiredLoadingExports"), 1,
    TEXT("Repair detached loading exports retired by SoloAS object copying."));
static constexpr EObjectFlags PendingFlags = RF_NeedLoad | RF_NeedPostLoad;

namespace SoloASBridge::Private
{
static FDelegateHandle RetiredExportRenameHandle;

static void RepairRenamedLoadingExport(UObject* Object, UObject* OldOuter, FName OldName)
{
    if (CVarRepairRetiredLoadingExports.GetValueOnAnyThread() == 0 || IsGarbageCollecting()
        || !Object || !OldOuter || OldOuter == GetTransientPackage()
        || Object->GetOuter() != GetTransientPackage() || Object->GetLinker()
        || !Object->HasAnyFlags(RF_WasLoaded) || !Object->HasAnyFlags(PendingFlags)
        || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) return;
    FLinkerLoad* Linker = OldOuter->GetLinker();
    if (!Linker || OldOuter->GetLinkerIndex() == INDEX_NONE) return;
    const FPackageIndex OuterIndex = FPackageIndex::FromExport(OldOuter->GetLinkerIndex());
    int32 Matches = 0;
    for (const FObjectExport& Export : Linker->ExportMap)
    {
        if (Export.ObjectName == OldName && Export.OuterIndex == OuterIndex)
        {
            if (Export.Object != nullptr) return;
            ++Matches;
        }
    }
    if (Matches == 1) Object->ClearFlags(PendingFlags);
}

void InitializeRetiredLoadingExportRepair()
{
#if WITH_EDITOR
    RetiredExportRenameHandle = FCoreUObjectDelegates::OnObjectRenamed.AddStatic(&RepairRenamedLoadingExport);
#endif
}

void ShutdownRetiredLoadingExportRepair()
{
#if WITH_EDITOR
    FCoreUObjectDelegates::OnObjectRenamed.Remove(RetiredExportRenameHandle);
    RetiredExportRenameHandle.Reset();
#endif
}
}

FSoloASRetiredLoadingExportRepair::FSoloASRetiredLoadingExportRepair(UObject* NewOwner)
    : ThreadId(FPlatformTLS::GetCurrentThreadId()),
      bEnabled(CVarRepairRetiredLoadingExports.GetValueOnAnyThread() != 0 && !IsGarbageCollecting()),
      bPreclearLoadingFlags(NewOwner != nullptr && bEnabled && IsInGameThread())
{
    if (!bEnabled) return;
    if (NewOwner)
    {
        TArray<UObject*> Children;
        GetObjectsWithOuter(NewOwner, Children, EGetObjectsFlags::IncludeNestedObjects);
        for (UObject* Object : Children) Capture(Object);
    }
    else
    {
        // Snapshot actual exports rather than inferring retirement from objects
        // newly created in transient during a reload or a reentrant callback.
        for (FThreadSafeObjectIterator It(UObject::StaticClass()); It; ++It) Capture(*It);
    }
}

void FSoloASRetiredLoadingExportRepair::Capture(UObject* Object)
{
    if (!Object || Object->GetOuter() == GetTransientPackage()
        || !Object->HasAnyFlags(PendingFlags)
        || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) return;
    FLinkerLoad* Linker = Object->GetLinker();
    if (!Linker) return;
    Exports.Add({FObjectKey(Object), FObjectKey(Object->GetOuter()), Object->GetFName(),
        Linker, Object->GetLinkerIndex(), Object->GetFlags() & PendingFlags});
    if (bPreclearLoadingFlags) Object->ClearFlags(PendingFlags);
}

FSoloASRetiredLoadingExportRepair::~FSoloASRetiredLoadingExportRepair()
{
    if (!bEnabled) return;
    if (!ensureMsgf(ThreadId == FPlatformTLS::GetCurrentThreadId() && !IsGarbageCollecting(),
        TEXT("Retired loading export repair must return on its entering thread outside GC"))) return;
    int32 Repaired = 0;
    for (const FExport& Export : Exports)
    {
        UObject* Object = Export.Key.ResolveObjectPtr();
        if (!Object || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) continue;
        if (Object->GetOuter() == GetTransientPackage() && !Object->GetLinker())
        {
            // Rename detached this same allocation from its export. Subobject
            // PostLoad remains owned by the engine; only these two bits differ.
            const bool bHadPendingFlags = Object->HasAnyFlags(PendingFlags)
                || Export.PendingFlags != RF_NoFlags;
            Object->ClearFlags(PendingFlags);
            Repaired += bHadPendingFlags;
            continue;
        }
        if (!bPreclearLoadingFlags || Export.PendingFlags == RF_NoFlags) continue;

        UObject* OriginalOuter = Export.Outer.ResolveObjectPtr();
        FLinkerLoad* Linker = Object->GetLinker();
        if (Object->GetOuter() != OriginalOuter || Object->GetFName() != Export.Name
            || Linker != Export.Linker || Object->GetLinkerIndex() != Export.LinkerIndex
            || !Linker || !Linker->ExportMap.IsValidIndex(Export.LinkerIndex)
            || Linker->ExportMap[Export.LinkerIndex].Object != Object) continue;
        Object->SetFlags(Export.PendingFlags);
    }
    UE_LOG(LogSoloASRetiredLoadingExportRepair, Display, TEXT("retired_repaired=%d"), Repaired);
}

void SoloAS::CopyPropertiesForUnrelatedObjectsRepairingLoads(UObject* OldObject, UObject* NewObject,
    UEngine::FCopyPropertiesForUnrelatedObjectsParams Params)
{
    FSoloASRetiredLoadingExportRepair Repair(NewObject);
    SoloAS::CopyPropertiesForUnrelatedObjectsWithArchetypePolicy(OldObject, NewObject, Params);
}
