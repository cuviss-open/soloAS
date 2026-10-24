#pragma once

#include "Containers/Array.h"
#include "CoreTypes.h"
#include "Templates/Tuple.h"
#include "UObject/NameTypes.h"

class UUserDefinedEnum;

namespace SoloASBridge
{
SOLOASBRIDGEEDITOR_API void RefreshRuntimeEnumEditorDependents(
    UUserDefinedEnum& Enum,
    const TArray<TPair<FName, int64>>& OldNames
);
}
