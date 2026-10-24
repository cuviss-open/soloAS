#include "Editor/SoloASScriptBlueprintEditorInternal.h"

#include "KismetCompilerModule.h"
#include "Modules/ModuleManager.h"
#include "Shared/SoloASScriptBlueprint.h"

namespace
{
FDelegateHandle ScriptClassRegisteredHandle;
TSet<FTopLevelAssetPath> MappedScriptClasses;

void RegisterScriptClassBlueprintType(UClass* ScriptClass)
{
    if (ScriptClass == nullptr)
    {
        return;
    }
    const FTopLevelAssetPath ScriptClassPath =
        ScriptClass->GetClassPathName();
    if (MappedScriptClasses.Contains(ScriptClassPath))
    {
        return;
    }
    // Registration can publish a derived script class before its ancestors.
    // Select the compiler route now; the create/compile/save validators check
    // the complete ancestry when a Blueprint actually consumes that route.
    IKismetCompilerInterface& Compiler =
        FModuleManager::LoadModuleChecked<IKismetCompilerInterface>(
            "KismetCompiler"
        );
    // Script classes use the Bridge-owned Blueprint route regardless of native
    // Blueprint specializations.
    Compiler.OverrideBPTypeForClass(
        ScriptClass,
        USoloASScriptBlueprint::StaticClass()
    );
    MappedScriptClasses.Add(ScriptClassPath);
}
}

namespace SoloASBridge::Editor
{
bool IsSupportedScriptBlueprintParent(
    const UClass* ParentClass,
    FString& OutReason
)
{
    OutReason.Reset();
    if (!SoloASBridge::HasRegisteredScriptClassInAncestry(ParentClass))
    {
        OutReason = TEXT("the parent has no registered AngelScript class");
        return false;
    }

    const UClass* Cursor = ParentClass;
    while (Cursor != nullptr && !Cursor->HasAllClassFlags(CLASS_Native))
    {
        if (!SoloASBridge::IsRegisteredScriptClass(Cursor)
            && Cast<UBlueprintGeneratedClass>(Cursor) == nullptr)
        {
            OutReason = FString::Printf(
                TEXT("unregistered non-native boundary %s"),
                *Cursor->GetPathName()
            );
            return false;
        }
        Cursor = Cursor->GetSuperClass();
    }
    if (Cursor == nullptr)
    {
        OutReason = TEXT("the ancestry has no native boundary");
        return false;
    }

    // The Bridge compiler owns the generated class and its persistent-frame
    // reference collector. It intentionally uses one stable Blueprint route
    // even when the native boundary is normally a Widget or Anim Blueprint.
    return true;
}

void InitializeScriptBlueprintProvider()
{
    IKismetCompilerInterface& Compiler =
        FModuleManager::LoadModuleChecked<IKismetCompilerInterface>(
            "KismetCompiler"
        );
    Compiler.OverrideBPGCTypeForBPType(
        USoloASScriptBlueprint::StaticClass(),
        USoloASScriptBlueprintGeneratedClass::StaticClass()
    );

    if (!ScriptClassRegisteredHandle.IsValid())
    {
        ScriptClassRegisteredHandle = SoloASBridge::OnScriptClassRegistered()
            .AddStatic(&RegisterScriptClassBlueprintType);
    }
    TArray<UClass*> ScriptClasses;
    SoloASBridge::GetRegisteredScriptClasses(ScriptClasses);
    for (UClass* ScriptClass : ScriptClasses)
    {
        RegisterScriptClassBlueprintType(ScriptClass);
    }
}

void ShutdownScriptBlueprintProvider()
{
    if (ScriptClassRegisteredHandle.IsValid())
    {
        SoloASBridge::OnScriptClassRegistered().Remove(
            ScriptClassRegisteredHandle
        );
        ScriptClassRegisteredHandle.Reset();
    }
    MappedScriptClasses.Reset();
}
}
