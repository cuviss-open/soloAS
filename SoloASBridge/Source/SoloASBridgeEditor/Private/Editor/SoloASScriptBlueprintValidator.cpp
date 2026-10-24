#include "Editor/SoloASScriptBlueprintEditorInternal.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Misc/DataValidation.h"
#include "Misc/CoreDelegates.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoloASScriptBlueprintValidation, Log, All);

namespace
{
FDelegateHandle ObjectPreSaveHandle;
FDelegateHandle BlueprintPreCompileHandle;
FDelegateHandle PostEngineInitHandle;

bool IsScriptBlueprintCandidate(const UBlueprint* Blueprint)
{
    return Blueprint != nullptr
        && (SoloASBridge::HasRegisteredScriptClassInAncestry(
                Blueprint->ParentClass
            )
            || Blueprint->IsA<USoloASScriptBlueprint>());
}

bool IsBridgeOwnedScriptBlueprint(const UBlueprint* Blueprint)
{
    return Blueprint != nullptr
        && Blueprint->GetClass() == USoloASScriptBlueprint::StaticClass();
}

void ValidateBeforeCompile(UBlueprint* Blueprint)
{
    if (!IsScriptBlueprintCandidate(Blueprint))
    {
        return;
    }

    FString Reason;
    if (!SoloASBridge::Editor::ValidateScriptBlueprintAsset(
        Blueprint,
        false,
        Reason
    ))
    {
        UE_LOG(
            LogSoloASScriptBlueprintValidation,
            Error,
            TEXT(
                "SOLOASBRIDGE_SCRIPT_BLUEPRINT_PRECOMPILE_INVALID "
                "asset=%s reason=%s"
            ),
            *Blueprint->GetPathName(),
            *Reason
        );
    }
}

void BindBlueprintPreCompile()
{
    if (GEditor != nullptr && !BlueprintPreCompileHandle.IsValid())
    {
        BlueprintPreCompileHandle = GEditor->OnBlueprintPreCompile().AddStatic(
            &ValidateBeforeCompile
        );
    }
}

void ValidateBeforeSave(UObject* Object, FObjectPreSaveContext)
{
    const UBlueprint* Blueprint = Cast<UBlueprint>(Object);
    if (!IsScriptBlueprintCandidate(Blueprint))
    {
        return;
    }

    FString Reason;
    if (!SoloASBridge::Editor::ValidateScriptBlueprintAsset(
        Blueprint,
        true,
        Reason
    ))
    {
        UE_LOG(
            LogSoloASScriptBlueprintValidation,
            Error,
            TEXT(
                "SOLOASBRIDGE_SCRIPT_BLUEPRINT_PRESAVE_INVALID "
                "asset=%s reason=%s"
            ),
            *Blueprint->GetPathName(),
            *Reason
        );
    }
}
}

bool USoloASScriptBlueprintValidator::CanValidateAsset_Implementation(
    const FAssetData&,
    UObject* InAsset,
    FDataValidationContext&
) const
{
    return IsScriptBlueprintCandidate(Cast<UBlueprint>(InAsset));
}

EDataValidationResult
USoloASScriptBlueprintValidator::ValidateLoadedAsset_Implementation(
    const FAssetData&,
    UObject* InAsset,
    FDataValidationContext& Context
)
{
    UBlueprint* Blueprint = Cast<UBlueprint>(InAsset);
    if (!IsScriptBlueprintCandidate(Blueprint))
    {
        return EDataValidationResult::NotValidated;
    }

    FString Reason;
    if (SoloASBridge::Editor::ValidateScriptBlueprintAsset(
        Blueprint,
        true,
        Reason
    ))
    {
        return EDataValidationResult::Valid;
    }

    Context.AddError(FText::FromString(FString::Printf(
        TEXT(
            "SOLOASBRIDGE_SCRIPT_BLUEPRINT_COOK_INVALID "
            "asset=%s reason=%s"
        ),
        *Blueprint->GetPathName(),
        *Reason
    )));
    return EDataValidationResult::Invalid;
}

namespace SoloASBridge::Editor
{
bool ValidateScriptBlueprint(
    const UBlueprint* Blueprint,
    bool bRequireGeneratedClasses,
    FString& OutReason
)
{
    OutReason.Reset();
    if (Blueprint == nullptr)
    {
        OutReason = TEXT("the Blueprint is null");
        return false;
    }
    if (Blueprint->GetClass() != USoloASScriptBlueprint::StaticClass())
    {
        OutReason = FString::Printf(
            TEXT("expected exact Blueprint type %s, found %s"),
            *USoloASScriptBlueprint::StaticClass()->GetPathName(),
            *Blueprint->GetClass()->GetPathName()
        );
        return false;
    }
    if (!IsSupportedScriptBlueprintParent(
        Blueprint->ParentClass,
        OutReason
    ))
    {
        return false;
    }

    const UClass* GeneratedClass = Blueprint->GeneratedClass;
    const UClass* SkeletonClass = Blueprint->SkeletonGeneratedClass;
    if (bRequireGeneratedClasses
        && (GeneratedClass == nullptr || SkeletonClass == nullptr))
    {
        OutReason = TEXT("generated or skeleton class is missing");
        return false;
    }
    if (GeneratedClass != nullptr
        && GeneratedClass->GetClass()
            != USoloASScriptBlueprintGeneratedClass::StaticClass())
    {
        OutReason = FString::Printf(
            TEXT("generated class has incompatible type %s"),
            *GeneratedClass->GetClass()->GetPathName()
        );
        return false;
    }
    if (SkeletonClass != nullptr
        && SkeletonClass->GetClass()
            != USoloASScriptBlueprintGeneratedClass::StaticClass())
    {
        OutReason = FString::Printf(
            TEXT("skeleton class has incompatible type %s"),
            *SkeletonClass->GetClass()->GetPathName()
        );
        return false;
    }
    return true;
}

bool ValidateScriptBlueprintAsset(
    const UBlueprint* Blueprint,
    bool bRequireGeneratedClasses,
    FString& OutReason
)
{
    if (IsBridgeOwnedScriptBlueprint(Blueprint))
    {
        return ValidateScriptBlueprint(
            Blueprint,
            bRequireGeneratedClasses,
            OutReason
        );
    }

                                                                                                              
    // Legacy stock Blueprint assets may inherit a registered script class, but
    // they must remain on their native Blueprint/generated-class route.
    OutReason.Reset();
    if (Blueprint == nullptr
        || !Blueprint->IsA<UBlueprint>()
        || !SoloASBridge::HasRegisteredScriptClassInAncestry(
            Blueprint->ParentClass
        ))
    {
        OutReason = TEXT("the asset is not a legacy stock Blueprint with script ancestry");
        return false;
    }
    if (!IsSupportedScriptBlueprintParent(
        Blueprint->ParentClass,
        OutReason
    ))
    {
        return false;
    }

    const UClass* ExpectedGeneratedClass = Blueprint->GetBlueprintClass();
    if (ExpectedGeneratedClass == nullptr)
    {
        OutReason = TEXT("legacy stock Blueprint has no expected generated class type");
        return false;
    }

    const UClass* GeneratedClass = Blueprint->GeneratedClass;
    const UClass* SkeletonClass = Blueprint->SkeletonGeneratedClass;
    if (bRequireGeneratedClasses
        && (GeneratedClass == nullptr || SkeletonClass == nullptr))
    {
        OutReason = TEXT("legacy stock Blueprint generated or skeleton class is missing");
        return false;
    }
    for (const UClass* Candidate : {GeneratedClass, SkeletonClass})
    {
        if (Candidate == nullptr)
        {
            continue;
        }
        if (Candidate->GetClass() != ExpectedGeneratedClass)
        {
            OutReason = FString::Printf(
                TEXT("legacy stock Blueprint expected generated class type %s, found %s"),
                *ExpectedGeneratedClass->GetPathName(),
                *Candidate->GetClass()->GetPathName()
            );
            return false;
        }
        if (Candidate->ClassGeneratedBy != Blueprint)
        {
            OutReason = FString::Printf(
                TEXT("legacy stock Blueprint generated class ownership mismatch: expected %s, found %s"),
                *Blueprint->GetPathName(),
                Candidate->ClassGeneratedBy != nullptr
                    ? *Candidate->ClassGeneratedBy->GetPathName()
                    : TEXT("<none>")
            );
            return false;
        }
    }
    return true;
                                                                                                            
}

void InitializeScriptBlueprintValidator()
{
    BindBlueprintPreCompile();
    if (!BlueprintPreCompileHandle.IsValid()
        && !PostEngineInitHandle.IsValid())
    {
        PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(
            &BindBlueprintPreCompile
        );
    }
    if (!ObjectPreSaveHandle.IsValid())
    {
        ObjectPreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddStatic(
            &ValidateBeforeSave
        );
    }
}

void ShutdownScriptBlueprintValidator()
{
    if (PostEngineInitHandle.IsValid())
    {
        FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
        PostEngineInitHandle.Reset();
    }
    if (BlueprintPreCompileHandle.IsValid())
    {
        if (GEditor != nullptr)
        {
            GEditor->OnBlueprintPreCompile().Remove(BlueprintPreCompileHandle);
        }
        BlueprintPreCompileHandle.Reset();
    }
    if (ObjectPreSaveHandle.IsValid())
    {
        FCoreUObjectDelegates::OnObjectPreSave.Remove(ObjectPreSaveHandle);
        ObjectPreSaveHandle.Reset();
    }
}
}
