#pragma once

#include "CoreMinimal.h"

class UBlueprint;

namespace SoloASBridge::Editor
{
SOLOASBRIDGEEDITOR_API void RelinkBlueprintParentScriptClasses(const TArray<UObject*>& Classes);

class SOLOASBRIDGEEDITOR_API FScriptBlueprintDependencyScope
{
public:
    virtual ~FScriptBlueprintDependencyScope() = default;

    virtual bool IsOldScriptClass(const UStruct* Dependency) const = 0;
    virtual bool PinAffectedBlueprint(UBlueprint* Blueprint) = 0;
    virtual void RefreshTransientBlueprintDefaults() = 0;
};

SOLOASBRIDGEEDITOR_API TUniquePtr<FScriptBlueprintDependencyScope>
PinScriptBlueprintReplacements(const TMap<UClass*, UClass*>& Replacements);
}
