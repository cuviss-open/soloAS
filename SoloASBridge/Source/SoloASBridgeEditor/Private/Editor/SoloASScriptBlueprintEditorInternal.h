#pragma once

#include "CoreMinimal.h"
#include "EditorValidatorBase.h"

#include "SoloASScriptBlueprintEditorInternal.generated.h"

class UBlueprint;

UCLASS()
class SOLOASBRIDGEEDITOR_API USoloASScriptBlueprintValidator final : public UEditorValidatorBase
{
    GENERATED_BODY()

public:
    virtual bool CanValidateAsset_Implementation(
        const FAssetData& InAssetData,
        UObject* InAsset,
        FDataValidationContext& Context
    ) const override;

    virtual EDataValidationResult ValidateLoadedAsset_Implementation(
        const FAssetData& InAssetData,
        UObject* InAsset,
        FDataValidationContext& Context
    ) override;
};

namespace SoloASBridge::Editor
{
bool IsSupportedScriptBlueprintParent(
    const UClass* ParentClass,
    FString& OutReason
);

bool ValidateScriptBlueprint(
    const UBlueprint* Blueprint,
    bool bRequireGeneratedClasses,
    FString& OutReason
);

bool ValidateScriptBlueprintAsset(
    const UBlueprint* Blueprint,
    bool bRequireGeneratedClasses,
    FString& OutReason
);

void InitializeScriptBlueprintProvider();
void ShutdownScriptBlueprintProvider();
void InitializeScriptBlueprintCompiler();
void InitializeScriptBlueprintValidator();
void ShutdownScriptBlueprintValidator();
}
