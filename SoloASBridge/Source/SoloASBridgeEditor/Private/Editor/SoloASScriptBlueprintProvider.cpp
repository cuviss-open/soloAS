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
    // Resolve at the native boundary: an ancestor script class may already be
    // mapped to the Bridge route, and registration order is not guaranteed.
    UClass* NativeParent = ScriptClass;
    while (NativeParent != nullptr && !NativeParent->HasAllClassFlags(CLASS_Native))
    {
        NativeParent = NativeParent->GetSuperClass();
    }
    if (NativeParent != nullptr)
    {
        UClass* BlueprintType = nullptr;
        UClass* GeneratedClassType = nullptr;
        Compiler.GetBlueprintTypesForClass(NativeParent, BlueprintType, GeneratedClassType);
        // Widget and Anim Blueprints need their native asset, compiler and
        // generated class to retain Designer/AnimGraph support.
        if (BlueprintType != UBlueprint::StaticClass()
            && BlueprintType != USoloASScriptBlueprint::StaticClass())
        {
            return;
        }
    }
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

    // Standard specialized assets retain their native compiler/generated class;
    // ordinary script Blueprints use the Bridge persistent-frame collector.
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
