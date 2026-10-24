// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "UObject/ObjectKey.h"

class FLinkerLoad;

// Prepares loading exports for a synchronous replacement operation. Flags stay
// cleared when an export is retired; an unchanged export keeps its original flags.
class SOLOASBRIDGE_API FSoloASRetiredLoadingExportRepair final
{
public:
    // Null selects a batch snapshot of existing loading exports.
    explicit FSoloASRetiredLoadingExportRepair(UObject* NewOwner = nullptr);
    ~FSoloASRetiredLoadingExportRepair();
    FSoloASRetiredLoadingExportRepair(const FSoloASRetiredLoadingExportRepair&) = delete;
    FSoloASRetiredLoadingExportRepair& operator=(const FSoloASRetiredLoadingExportRepair&) = delete;

private:
    struct FExport
    {
        FObjectKey Key;
        FObjectKey Outer;
        FName Name;
        FLinkerLoad* Linker;
        int32 LinkerIndex;
        EObjectFlags PendingFlags;
    };
    void Capture(UObject* Object);
    TArray<FExport> Exports;
    uint32 ThreadId;
    bool bEnabled;
    bool bPreclearLoadingFlags;
};

namespace SoloAS
{
SOLOASBRIDGE_API void CopyPropertiesForUnrelatedObjectsRepairingLoads(
    UObject* OldObject, UObject* NewObject,
    UEngine::FCopyPropertiesForUnrelatedObjectsParams Params = {});
}
