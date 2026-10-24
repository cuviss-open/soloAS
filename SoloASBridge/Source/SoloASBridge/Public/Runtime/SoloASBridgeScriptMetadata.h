#pragma once

#include "Containers/ArrayView.h"
#include "Containers/Array.h"
#include "Templates/SharedPointer.h"
#include "CoreTypes.h"
#include "Shared/SoloASBridgeScriptMetadataProvider.h"

// An owning snapshot keeps all descriptor strings alive across module unloads.
struct FSoloASBridgeScriptMetadata
{
    TArray<FSoloASBridgeScriptFunctionDescriptor> Functions;
    TArray<FSoloASBridgeScriptDefaultArgumentDescriptor> DefaultArguments;
    TArray<FSoloASBridgeNativeFunctionPropertyTraitDescriptor> PropertyTraits;
    TArray<FSoloASBridgeNativeFunctionTraitDescriptor> FunctionTraits;
    TArray<TArray<ANSICHAR>> Strings;
    TArray<TSharedPtr<const FSoloASBridgeScriptMetadata, ESPMode::ThreadSafe>> Sources;
};

namespace SoloASBridge
{
SOLOASBRIDGE_API TSharedRef<const FSoloASBridgeScriptMetadata, ESPMode::ThreadSafe>
GetScriptMetadata();

SOLOASBRIDGE_API void RegisterScriptMetadata(
    const void* Owner, const ANSICHAR* ModuleName,
    TConstArrayView<FSoloASBridgeScriptFunctionDescriptor> Functions,
    TConstArrayView<FSoloASBridgeScriptDefaultArgumentDescriptor> DefaultArguments,
    TConstArrayView<FSoloASBridgeNativeFunctionPropertyTraitDescriptor> PropertyTraits,
    TConstArrayView<FSoloASBridgeNativeFunctionTraitDescriptor> FunctionTraits);
SOLOASBRIDGE_API void UnregisterScriptMetadata(const void* Owner);

SOLOASBRIDGE_API void InitializeScriptMetadata();
SOLOASBRIDGE_API void ShutdownScriptMetadata();
}
