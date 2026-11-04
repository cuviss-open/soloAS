#pragma once

#include "CoreMinimal.h"
#include "SoloASEngineCompat.h"
#include "BlueprintCompilationManager.h"

class UBlueprint;

namespace SoloASBridge::Editor
{
// SOLOAS-COMPAT-BEGIN(5.4) Preserve request-time defaults missing from FBPCompileRequest [test: SoloAS.BlueprintHierarchyFullReload] [id: CS.Compatibility.Bridge.SoloASScriptBlueprintDependencies.QueuedDefaultOptionsAPI]
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 4
SOLOASBRIDGEEDITOR_API void QueueScriptBlueprintForCompilation(UBlueprint* Blueprint);
SOLOASBRIDGEEDITOR_API void ClearScriptBlueprintCompileOptions();
#else
inline void QueueScriptBlueprintForCompilation(UBlueprint* Blueprint)
{
    FBlueprintCompilationManager::QueueForCompilation(Blueprint);
}
inline void ClearScriptBlueprintCompileOptions() {}
#endif
// SOLOAS-COMPAT-END

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
