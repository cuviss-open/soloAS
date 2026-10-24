#include "Editor/SoloASScriptBlueprintDependencies.h"

#include "Engine/Blueprint.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
class FDependencyScope final
    : public SoloASBridge::Editor::FScriptBlueprintDependencyScope
{
    TArray<TStrongObjectPtr<UClass>> PinnedClasses;
    TArray<TStrongObjectPtr<UBlueprint>> PinnedBlueprints;
    TSet<UClass*> OldClasses;
    TMap<UClass*, UClass*> ReplacementMap;
    struct FInheritedDefaults
    {
        TSet<FName> ExistingFields;
        TSet<FName> FollowingParent;
    };
    TMap<UBlueprint*, FInheritedDefaults> TransientDefaults;

public:
    void AddReplacement(UClass* OldClass, UClass* NewClass)
    {
        PinnedClasses.Emplace(OldClass);
        PinnedClasses.Emplace(NewClass);
        OldClasses.Add(OldClass);
        ReplacementMap.Add(OldClass, NewClass);
    }

    virtual bool IsOldScriptClass(const UStruct* Dependency) const override
    {
        const UClass* Class = Cast<UClass>(Dependency);
        return Class != nullptr && OldClasses.Contains(Class);
    }

    virtual bool PinAffectedBlueprint(UBlueprint* Blueprint) override
    {
        if (Blueprint == nullptr) return false;
        const bool bTransient = Blueprint->HasAnyFlags(RF_Transient);
        UClass* Generated = Blueprint->GeneratedClass;
        if (bTransient && (Generated == nullptr || Generated->ClassGeneratedBy != Blueprint
            || Generated->HasAnyClassFlags(CLASS_NewerVersionExists))) return false;

        for (const TStrongObjectPtr<UBlueprint>& Pinned : PinnedBlueprints)
        {
            if (Pinned.Get() == Blueprint) return !bTransient;
        }

        PinnedBlueprints.Emplace(Blueprint);
        if (bTransient)
        {
            UClass* Parent = Generated->GetSuperClass();
            bool bOldScriptAncestor = false;
            for (UClass* Cursor = Parent; Cursor; Cursor = Cursor->GetSuperClass())
                bOldScriptAncestor |= OldClasses.Contains(Cursor);
            if (bOldScriptAncestor)
            {
                FInheritedDefaults& Defaults = TransientDefaults.Add(Blueprint);
                UObject* OldDefault = Generated->GetDefaultObject(false);
                UObject* ParentDefault = Parent->GetDefaultObject(false);
                for (TFieldIterator<FProperty> It(Parent); It; ++It)
                {
                    FProperty* Property = *It;
                    if (!SoloASBridge::IsRegisteredScriptClass(Property->GetOwnerClass())) continue;
                    Defaults.ExistingFields.Add(Property->GetFName());
                    if (!Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_ExportObject)
                        && OldDefault && ParentDefault && Property->Identical_InContainer(OldDefault, ParentDefault))
                        Defaults.FollowingParent.Add(Property->GetFName());
                }
            }
        }
        return !bTransient;
    }

    virtual void RefreshTransientBlueprintDefaults() override
    {
        TArray<UBlueprint*> Ordered;
        TransientDefaults.GetKeys(Ordered);
        auto Depth = [](const UClass* Class)
        {
            int32 Result = 0;
            for (; Class; Class = Class->GetSuperClass()) ++Result;
            return Result;
        };
        Ordered.Sort([&](const UBlueprint& Left, const UBlueprint& Right)
        {
            const int32 LeftDepth = Depth(Left.GeneratedClass);
            const int32 RightDepth = Depth(Right.GeneratedClass);
            return LeftDepth != RightDepth ? LeftDepth < RightDepth : Left.GetPathName() < Right.GetPathName();
        });
        for (UBlueprint* Blueprint : Ordered)
        {
            UClass* Generated = Blueprint->GeneratedClass;
            if (!Generated || Generated->ClassGeneratedBy != Blueprint
                || Generated->HasAnyClassFlags(CLASS_NewerVersionExists)) continue;
            Generated->ClassFlags &= ~CLASS_ReplicationDataIsSetUp;
            Generated->ClassReps.Reset();
        }
        for (UBlueprint* Blueprint : Ordered)
        {
            UClass* Generated = Blueprint->GeneratedClass;
            if (!Generated || Generated->ClassGeneratedBy != Blueprint
                || Generated->HasAnyClassFlags(CLASS_NewerVersionExists)) continue;
            UClass* Parent = Generated->GetSuperClass();
            UObject* Default = Generated->GetDefaultObject(false);
            UObject* ParentDefault = Parent ? Parent->GetDefaultObject(false) : nullptr;
            const FInheritedDefaults& Defaults = TransientDefaults.FindChecked(Blueprint);
            if (Default && ParentDefault)
            {
                for (TFieldIterator<FProperty> It(Parent); It; ++It)
                {
                    FProperty* Property = *It;
                    if (!SoloASBridge::IsRegisteredScriptClass(Property->GetOwnerClass())
                        || Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_ExportObject)
                        || (Defaults.ExistingFields.Contains(Property->GetFName())
                            && !Defaults.FollowingParent.Contains(Property->GetFName()))) continue;
                    if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
                    {
                        UObject* Value = ObjectProperty->GetObjectPropertyValue_InContainer(ParentDefault);
                        if (Value && Value->IsTemplate()) continue;
                    }
                    FProperty* Destination = FindFProperty<FProperty>(Generated, Property->GetFName());
                    if (!Destination || !Destination->SameType(Property)) continue;
                    Destination->CopyCompleteValue(Destination->ContainerPtrToValuePtr<void>(Default),
                        Property->ContainerPtrToValuePtr<void>(ParentDefault));
                }
            }
            Generated->SetUpRuntimeReplicationData();
        }
    }
};
}

void SoloASBridge::Editor::RelinkBlueprintParentScriptClasses(const TArray<UObject*>& Classes)
{
    for (UObject* Object : Classes)
    {
        UClass* Class = Cast<UClass>(Object);
        if (!IsRegisteredScriptClass(Class)) continue;
        UClass* Parent = Class->GetSuperClass();
        if (!Parent || !Parent->HasAnyClassFlags(CLASS_CompiledFromBlueprint)) continue;
        // A Blueprint compile can retire inherited fields before the deferred child reload runs.
        Class->Bind();
        Class->StaticLink();
    }
}

TUniquePtr<SoloASBridge::Editor::FScriptBlueprintDependencyScope>
SoloASBridge::Editor::PinScriptBlueprintReplacements(
    const TMap<UClass*, UClass*>& Replacements
)
{
    TUniquePtr<FDependencyScope> Scope = MakeUnique<FDependencyScope>();
    for (const TPair<UClass*, UClass*>& Pair : Replacements)
    {
        if (Pair.Key == Pair.Value
            || !IsRegisteredScriptClass(Pair.Key)
            || !IsRegisteredScriptClass(Pair.Value)) return nullptr;

        Scope->AddReplacement(Pair.Key, Pair.Value);
    }
    return Scope;
}
