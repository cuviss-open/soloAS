#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace SoloASBridge::Editor
{
SOLOASBRIDGEEDITOR_API bool IsFieldNativeForScriptEditor(const FField* Field);
SOLOASBRIDGEEDITOR_API bool IsFunctionHiddenForScriptEditor(
    const UFunction* Function,
    const UClass* Class);
SOLOASBRIDGEEDITOR_API bool ShouldSuppressDuplicateDetailPropertyCheck(
    const ANSICHAR* Expression,
    const ANSICHAR* File);
SOLOASBRIDGEEDITOR_API UField* FindTypeForScriptEditCondition(
    UClass* TypeClass,
    FStringView Name,
    EFindFirstObjectOptions Options);
void InitializeEditorExperienceImports();
void ShutdownEditorExperienceImports();
}
