#include "Editor/SoloASScriptBlueprintEditorInternal.h"
#include "Editor/SoloASCopyProperties.h"
#include "Editor/SoloASScriptBlueprintDependencies.h"
#include "Editor/SoloASRetiredLoadingExportRepair.h"

#include "Kismet2/KismetReinstanceUtilities.h"
#include "KismetCompiler.h"
#include "KismetCompilerMisc.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "Engine/Engine.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogSoloASBlueprintCdoCopy, Log, All);

namespace
{
// SOLOAS-COMPAT-BEGIN(5.4) Only explicitly queued requests receive default options [test: SoloAS.BlueprintHierarchyFullReload] [id: CS.Compatibility.Bridge.SoloASScriptBlueprintCompiler.QueuedDefaultOptionsStorage]
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 4
TMap<TWeakObjectPtr<UBlueprint>, EBlueprintCompileOptions> QueuedDefaultCompileOptions;
#endif
// SOLOAS-COMPAT-END
constexpr EObjectFlags UnreadyDefaultFlags = RF_NeedLoad | RF_NeedPostLoad
    | RF_NeedPostLoadSubobjects | RF_NeedInitialization | RF_BeginDestroyed | RF_FinishDestroyed;
constexpr EInternalObjectFlags UnreadyDefaultInternalFlags = EInternalObjectFlags::Garbage
    | EInternalObjectFlags::Unreachable | EInternalObjectFlags::PendingConstruction;

class FSoloASScriptBlueprintCompilerContext final
    : public FKismetCompilerContext
    , public FSoloASBlueprintDefaultObjectScope
{
public:
    FSoloASScriptBlueprintCompilerContext(
        UBlueprint* Blueprint,
        FCompilerResultsLog& Results,
        const FKismetCompilerOptions& Options
    )
        : FKismetCompilerContext(Blueprint, Results, Options)
    {
        if (IsInGameThread())
        {
            USoloASScriptBlueprintGeneratedClass* Generated =
                Cast<USoloASScriptBlueprintGeneratedClass>(Blueprint->GeneratedClass);
            UObject* Existing = Generated != nullptr ? Generated->GetDefaultObject(false) : nullptr;
            if (Existing != nullptr && Existing->GetClass() == Generated
                && !Generated->HasAnyFlags(UnreadyDefaultFlags)
                && !Existing->HasAnyFlags(UnreadyDefaultFlags)
                && !Generated->HasAnyInternalFlags(UnreadyDefaultInternalFlags)
                && !Existing->HasAnyInternalFlags(UnreadyDefaultInternalFlags)
                && Generated->AttachDefaultObjectScope(this))
            {
                CapturedGeneratedClass = Generated;
                CurrentGeneratedClass.Reset(Generated);
                CapturedDefaultObject.Reset(Existing);
                LogCopyPhase(TEXT("capture"), nullptr);
            }
            else if (Existing != nullptr)
            {
                constexpr EObjectFlags LoadingFlags = RF_NeedLoad | RF_NeedPostLoad
                    | RF_NeedPostLoadSubobjects | RF_NeedInitialization;
                if (Generated->HasAnyFlags(LoadingFlags) || Existing->HasAnyFlags(LoadingFlags))
                {
                    // A cold-loaded CDO has no ready prior instance to migrate.
                    UE_LOG(LogSoloASBlueprintCdoCopy, Verbose,
                        TEXT("SOLOAS_BP_CDO_COPY_REJECT reason=loading_default_not_ready blueprint=%s class_load_flags=%x source_load_flags=%x"),
                        *Blueprint->GetPathName(), uint32(Generated->GetFlags() & LoadingFlags),
                        uint32(Existing->GetFlags() & LoadingFlags));
                }
                else
                {
                    UE_LOG(LogSoloASBlueprintCdoCopy, Warning,
                        TEXT("SOLOAS_BP_CDO_COPY_REJECT reason=capture_not_ready_or_scope_attached blueprint=%s captured_class=%p source=%p"),
                        *Blueprint->GetPathName(), static_cast<void*>(Generated), static_cast<void*>(Existing));
                }
            }
        }
        FString Reason;
        if (!SoloASBridge::Editor::ValidateScriptBlueprint(
            Blueprint,
            false,
            Reason
        ))
        {
            MessageLog.Error(
                *FString::Printf(
                    TEXT("Invalid script Blueprint @@: %s"),
                    *Reason
                ),
                Blueprint
            );
        }
    }

    virtual ~FSoloASScriptBlueprintCompilerContext() override
    {
        if (CurrentGeneratedClass.IsValid())
        {
            CurrentGeneratedClass->DetachDefaultObjectScope(this);
            UObject* Source = CapturedDefaultObject.Get();
            CapturedDefaultObject.Reset();
            LogCopyPhase(TEXT("release"), nullptr, Source);
            CurrentGeneratedClass.Reset();
        }
    }

    virtual void OnDefaultCreated(UClass* Class, UObject* Object) override
    {
        UObject* Source = CapturedDefaultObject.Get();
        if (!IsInGameThread() || GIsDuplicatingClassForReinstancing || bGeneratedClassChanged
            || Source == nullptr || Object == nullptr || Object == Source
            || Class != CurrentGeneratedClass.Get() || Class->ClassGeneratedBy != Blueprint
            || Class != Blueprint->GeneratedClass || Object->GetClass() != Class
            || Class->bLayoutChanging || Class->GetName().StartsWith(TEXT("REINST_"))
            || Class->HasAnyClassFlags(CLASS_NewerVersionExists)
            || Class->HasAnyFlags(UnreadyDefaultFlags) || Source->HasAnyFlags(UnreadyDefaultFlags)
            || Object->HasAnyFlags(UnreadyDefaultFlags)
            || Class->HasAnyInternalFlags(UnreadyDefaultInternalFlags)
            || Source->HasAnyInternalFlags(UnreadyDefaultInternalFlags)
            || Object->HasAnyInternalFlags(UnreadyDefaultInternalFlags)
            || Object != Class->GetDefaultObject(false) || CopiedDefaultObjects.Contains(FObjectKey(Object)))
        {
            return;
        }
        // Claim before CPFuo: copying can construct further defaults and reenter.
        CopiedDefaultObjects.Add(FObjectKey(Object));
        // CPFUO notifies tools synchronously; nested compilation may retire this CDO.
        TStrongObjectPtr<UObject> CopyTarget(Object);
        UEngine::FCopyPropertiesForUnrelatedObjectsParams Params;
        Params.bDoDelta = true;
        Params.bCopyDeprecatedProperties = true;
        Params.bSkipCompilerGeneratedDefaults = true;
        Params.bClearReferences = false;
        {
            FSoloASRetiredLoadingExportRepair LoadingExports(Object);
            SoloAS::CopyPropertiesForUnrelatedObjectsWithArchetypePolicy(Source, Object, Params);
        }
        if (Class != Blueprint->GeneratedClass || Object->GetClass() != Class
            || Object != Class->GetDefaultObject(false)
            || Class->HasAnyFlags(UnreadyDefaultFlags) || Source->HasAnyFlags(UnreadyDefaultFlags)
            || Object->HasAnyFlags(UnreadyDefaultFlags)
            || Class->HasAnyInternalFlags(UnreadyDefaultInternalFlags)
            || Source->HasAnyInternalFlags(UnreadyDefaultInternalFlags)
            || Object->HasAnyInternalFlags(UnreadyDefaultInternalFlags))
        {
            bGeneratedClassChanged = true;
            UE_LOG(LogSoloASBlueprintCdoCopy, Error,
                TEXT("SOLOAS_BP_CDO_COPY_REJECT reason=copy_identity_changed blueprint=%s captured_class=%p new=%p"),
                *Blueprint->GetPathName(), static_cast<void*>(CapturedGeneratedClass), static_cast<void*>(Object));
            return;
        }
        LogCopyPhase(TEXT("copy"), Object);
    }

private:
    void LogCopyPhase(const TCHAR* Phase, UObject* Object, UObject* ReleasedSource = nullptr) const
    {
        UE_LOG(LogSoloASBlueprintCdoCopy, Log,
            TEXT("SOLOAS_BP_CDO_COPY phase=%s blueprint=%s captured_class=%p current_class=%p source=%p new=%p copy_count=%d source_released=%d"),
            Phase, *Blueprint->GetPathName(), static_cast<void*>(CapturedGeneratedClass),
            static_cast<void*>(CurrentGeneratedClass.Get()),
            static_cast<void*>(ReleasedSource != nullptr ? ReleasedSource : CapturedDefaultObject.Get()),
            static_cast<void*>(Object), CopiedDefaultObjects.Num(), CapturedDefaultObject.IsValid() ? 0 : 1);
    }

    UClass* CapturedGeneratedClass = nullptr;
    TStrongObjectPtr<USoloASScriptBlueprintGeneratedClass> CurrentGeneratedClass;
    TStrongObjectPtr<UObject> CapturedDefaultObject;
    TSet<FObjectKey> CopiedDefaultObjects;
    bool bGeneratedClassChanged = false;

protected:
    virtual void SpawnNewClass(const FString& NewClassName) override
    {
        UClass* ExistingClass = FindObject<UClass>(
            Blueprint->GetOutermost(),
            *NewClassName
        );
        NewClass = Cast<USoloASScriptBlueprintGeneratedClass>(
            ExistingClass
        );
        if (ExistingClass != nullptr && NewClass == nullptr)
        {
            MessageLog.Error(
                *FString::Printf(
                    TEXT(
                        "Script Blueprint @@ has incompatible generated "
                        "class type %s."
                    ),
                    *ExistingClass->GetClass()->GetPathName()
                ),
                Blueprint
            );
            FKismetCompilerUtilities::ConsignToOblivion(
                ExistingClass,
                Blueprint->bIsRegeneratingOnLoad
            );
        }

        if (NewClass == nullptr)
        {
            NewClass = NewObject<USoloASScriptBlueprintGeneratedClass>(
                Blueprint->GetOutermost(),
                FName(*NewClassName),
                RF_Public | RF_Transactional
            );
        }
        else
        {
            NewClass->ClassGeneratedBy = Blueprint;
            FBlueprintCompileReinstancer::Create(NewClass);
        }

        if (CapturedDefaultObject.IsValid() && NewClass != CurrentGeneratedClass.Get())
        {
            bGeneratedClassChanged = true;
            UE_LOG(LogSoloASBlueprintCdoCopy, Warning,
                TEXT("SOLOAS_BP_CDO_COPY_REJECT reason=generated_class_changed blueprint=%s captured_class=%p new_class=%p"),
                *Blueprint->GetPathName(), static_cast<void*>(CapturedGeneratedClass), static_cast<void*>(NewClass));
        }

        FString Reason;
        if (!SoloASBridge::Editor::ValidateScriptBlueprint(
            Blueprint,
            false,
            Reason
        ))
        {
            MessageLog.Error(
                *FString::Printf(
                    TEXT("Cannot regenerate script Blueprint @@: %s"),
                    *Reason
                ),
                Blueprint
            );
        }
    }
};
}

namespace SoloASBridge::Editor
{
// SOLOAS-COMPAT-BEGIN(5.4) Capture before queueing, as UE 5.5+ FBPCompileRequest does [test: SoloAS.BlueprintHierarchyFullReload] [id: CS.Compatibility.Bridge.SoloASScriptBlueprintCompiler.QueuedDefaultOptionsCapture]
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 4
void QueueScriptBlueprintForCompilation(UBlueprint* Blueprint)
{
    if (!Blueprint->bQueuedForCompilation)
    {
        if (const USoloASScriptBlueprint* ScriptBlueprint = Cast<USoloASScriptBlueprint>(Blueprint))
        {
            QueuedDefaultCompileOptions.Add(Blueprint, ScriptBlueprint->ComputeScriptDefaultCompileOptions());
        }
    }
    FBlueprintCompilationManager::QueueForCompilation(Blueprint);
}

void ClearScriptBlueprintCompileOptions()
{
    QueuedDefaultCompileOptions.Reset();
}
#endif
// SOLOAS-COMPAT-END

void InitializeScriptBlueprintCompiler()
{
    FKismetCompilerContext::RegisterCompilerForBP(
        USoloASScriptBlueprint::StaticClass(),
        [](UBlueprint* Blueprint,
           FCompilerResultsLog& Results,
           const FKismetCompilerOptions& Options)
        {
// SOLOAS-COMPAT-BEGIN(5.4) FCompilerData owns the mutable options passed by reference [test: SoloAS.BlueprintHierarchyFullReload] [id: CS.Compatibility.Bridge.SoloASScriptBlueprintCompiler.QueuedDefaultOptionsApply]
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 4
            EBlueprintCompileOptions Defaults;
            if (QueuedDefaultCompileOptions.RemoveAndCopyValue(Blueprint, Defaults))
            {
                FKismetCompilerOptions& InternalOptions = const_cast<FKismetCompilerOptions&>(Options);
                InternalOptions.bUseDeltaSerializationDuringReinstancing |= EnumHasAnyFlags(Defaults,
                    EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing);
                InternalOptions.bSkipNewVariableDefaultsDetection |= EnumHasAnyFlags(Defaults,
                    EBlueprintCompileOptions::SkipNewVariableDefaultsDetection);
            }
#endif
// SOLOAS-COMPAT-END
            return MakeShared<FSoloASScriptBlueprintCompilerContext>(
                Blueprint,
                Results,
                Options
            );
        }
    );
}
}

