#pragma once

#include "Containers/Map.h"
#include "Containers/Set.h"
#include "CoreTypes.h"
#include "Templates/Function.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UClass;
class UDelegateFunction;
class UEnum;
class UObject;
class UScriptStruct;

enum class ESoloASBridgeReloadResult : uint8
{
    Completed,
    UnsupportedDelegates,
    UnsupportedAssets,
    WrongThread,
    ReloadAlreadyActive,
};

namespace SoloASBridge
{
// Unknown serializer implementations return false and retain conservative capture.
SOLOASBRIDGEEDITOR_API bool HasOnlyReflectedReferences(const UObject* Object);

// Refreshes retained reflected field paths and soft references around reinstancing.
SOLOASBRIDGEEDITOR_API void RefreshRetainedScriptTypeReferences(
    const TMap<UClass*, UClass*>& Classes,
    const TMap<UScriptStruct*, UScriptStruct*>& Structs,
    const TMap<UObject*, UObject*>& Objects,
    const TSet<TWeakObjectPtr<UObject>>* CandidateObjects = nullptr
);

SOLOASBRIDGEEDITOR_API ESoloASBridgeReloadResult ValidateStockReloadInputs(
    const TMap<UDelegateFunction*, UDelegateFunction*>& Delegates,
    const TMap<UObject*, UObject*>& Assets
);

SOLOASBRIDGEEDITOR_API const TCHAR* DescribeStockReloadResult(ESoloASBridgeReloadResult Result);

// Executes synchronously under a plugin game-thread/reentrancy guard.
// Callers with partially changed objects must not assume rollback.
SOLOASBRIDGEEDITOR_API ESoloASBridgeReloadResult RunWithStockCopyPropertiesScope(
    TFunctionRef<void()> Body, int32* OutFailureDetail = nullptr
);

SOLOASBRIDGEEDITOR_API ESoloASBridgeReloadResult ReinstanceWithStockReload(
    const TMap<UClass*, UClass*>& Classes,
    const TMap<UScriptStruct*, UScriptStruct*>& Structs,
    const TSet<UEnum*>& Enums,
    const TMap<UDelegateFunction*, UDelegateFunction*>& Delegates,
    const TMap<UObject*, UObject*>& Assets
);

// Failure detail is reserved and initialized to zero; failure does not imply rollback.
SOLOASBRIDGEEDITOR_API ESoloASBridgeReloadResult ReinstanceWithStockReload(
    const TMap<UClass*, UClass*>& Classes,
    const TMap<UScriptStruct*, UScriptStruct*>& Structs,
    const TSet<UEnum*>& Enums,
    const TMap<UDelegateFunction*, UDelegateFunction*>& Delegates,
    const TMap<UObject*, UObject*>& Assets,
    int32* OutFailureDetail,
    bool bRunGC = true
);
}
