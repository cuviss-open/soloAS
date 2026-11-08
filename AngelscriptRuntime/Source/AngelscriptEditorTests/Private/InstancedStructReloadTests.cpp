// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "InstancedStructReloadTestObject.h"
#include "UObject/GCObject.h"

namespace { TMap<const void*, int32> LayoutDestructions; TMap<int32, int32> ReferenceDestructions; }

void UInstancedStructReloadTestObject::RecordPayloadDestruction(int32 Generation, int32 Value)
{
    if (Value == 6707) ++ReferenceDestructions.FindOrAdd(Generation);
}

FInstancedStructReloadLifetime::~FInstancedStructReloadLifetime() { ++LayoutDestructions.FindOrAdd(this); }
int32 FInstancedStructReloadLifetime::DestructionsAt(const void* Address) { return LayoutDestructions.FindRef(Address); }

void UInstancedStructReloadTestObject::Serialize(FArchive& Ar)
{
    Super::Serialize(Ar);
    if (!bCollectorOnly || Ar.IsObjectReferenceCollector() || Ar.IsModifyingWeakAndStrongReferences())
        Hidden.Serialize(Ar);
}

void UInstancedStructReloadTestObject::AddReferencedObjects(UObject* Object, FReferenceCollector& Collector)
{
    auto* Owner = CastChecked<UInstancedStructReloadTestObject>(Object);
    if (Owner->bExposeARO) Owner->Hidden.AddStructReferencedObjects(Collector);
    Super::AddReferencedObjects(Object, Collector);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "InstancedStructReload.h"
#include "AngelscriptManager.h"
#include "ClassGenerator/ASStruct.h"
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "BlueprintCompilationManager.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_VariableGet.h"
#include "SoloASEngineCompat.h"
#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "EndAngelscriptHeaders.h"
#include "UObject/UnrealType.h"

namespace
{
UASStruct* MakeLayoutStruct(const TCHAR* Name, bool bAdded)
{
    UASStruct* Struct = NewObject<UASStruct>(GetTransientPackage(), MakeUniqueObjectName(
        GetTransientPackage(), UASStruct::StaticClass(), Name), RF_Transient);
    Struct->SetPropertiesSize(bAdded ? 2 * sizeof(int32) : sizeof(int32));
    Struct->MinAlignment = alignof(int32);
    Struct->Bind();
    Struct->SetAngelscriptCppStructOps(Struct->CreateCppStructOps());
    Struct->PrepareCppStructOps();
    for (const TCHAR* Field : bAdded ? TArray<const TCHAR*>{TEXT("AddedValue"), TEXT("Value")}
        : TArray<const TCHAR*>{TEXT("Value")})
    {
        FIntProperty* Property = CastFieldChecked<FIntProperty>(FIntProperty::StaticClass()->Construct(Struct, Field));
        Property->SetPropertyFlags(CPF_HasGetValueTypeHash);
        Property->Next = Struct->ChildProperties;
        Struct->ChildProperties = Property;
    }
    Struct->StaticLink(true);
    Struct->DestructorLink = nullptr;
    return Struct;
}

// Collects the payload lease's attachment summaries so a test can count the holders it released.
class FPayloadAttachLog final : public FOutputDevice
{
public:
    using FOutputDevice::Serialize;
    FPayloadAttachLog() { GLog->AddOutputDevice(this); }
    virtual ~FPayloadAttachLog() override { Stop(); }
    void Stop()
    {
        if (!bListening) return;
        GLog->Flush();
        GLog->RemoveOutputDevice(this);
        bListening = false;
    }
    FString Lines(const TCHAR* Marker) const
    {
        FScopeLock Lock(&Mutex);
        return FString::Join(Collected.FilterByPredicate([Marker](const FString& Line) { return Line.StartsWith(Marker); }), TEXT("|"));
    }
    virtual bool CanBeUsedOnAnyThread() const override { return true; }
    virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName&) override
    {
        if (!FCString::Strstr(Message, TEXT("SCRIPT_PAYLOAD_"))) return;
        FScopeLock Lock(&Mutex);
        Collected.Add(Message);
    }

private:
    mutable FCriticalSection Mutex;
    TArray<FString> Collected;
    bool bListening = true;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructContainerPreMigrationTest,
    "SoloAS.InstancedStructContainerPreMigration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInstancedStructContainerPreMigrationTest::RunTest(const FString&)
{
    TStrongObjectPtr<UASStruct> Old(MakeLayoutStruct(TEXT("ContainerLayoutOld"), false));
    TStrongObjectPtr<UASStruct> Next(MakeLayoutStruct(TEXT("ContainerLayoutNext"), true));
    Old->NewerVersion = Next.Get();
    TStrongObjectPtr<UInstancedStructReloadTestObject> Owner(NewObject<UInstancedStructReloadTestObject>());
    FInstancedStruct Value;
    Value.InitializeAs(Old.Get());
    FindFProperty<FIntProperty>(Old.Get(), TEXT("Value"))->SetPropertyValue_InContainer(Value.GetMutableMemory(), 6707);
    const TArray<FInstancedStruct> Values = {Value, FInstancedStruct(), Value};
    Owner->Container = MakeArrayView(Values);
    FString Error;
    const bool bMigrated = MigrateInstancedStructsForReload({{Old.Get(), Next.Get()}}, Error);
    TestTrue(*FString::Printf(TEXT("Container preparation succeeds: %s"), *Error), bMigrated);
    TestEqual(TEXT("Container count is preserved including the empty slot"), Owner->Container.Num(), 3);
    TestFalse(TEXT("The middle slot stays empty"), Owner->Container[1].IsValid());
    for (int32 Index : {0, 2})
    {
        const FStructView Slot = Owner->Container[Index];
        if (TestTrue(TEXT("Every affected slot is allocated with the new layout"), Slot.GetScriptStruct() == Next.Get()))
        {
            TestEqual(TEXT("Old scalar data survives"), FindFProperty<FIntProperty>(Next.Get(), TEXT("Value"))
                ->GetPropertyValue_InContainer(Slot.GetMemory()), 6707);
            TestEqual(TEXT("Added storage has its initialized value"), FindFProperty<FIntProperty>(Next.Get(), TEXT("AddedValue"))
                ->GetPropertyValue_InContainer(Slot.GetMemory()), 0);
        }
    }
    Owner->Container.Empty();
    return true;
}

namespace
{
bool RunLayoutFullReload(FAutomationTestBase& Test, bool bContainer, bool bCollectorOnly, int32 CdoKind = 0, bool bExposeAro = true, bool bReferencePayload = false, int32 Scenario = 0)
{
    FAngelscriptManager& Manager = FAngelscriptManager::Get();
    const FString StructName = bReferencePayload
        ? FString::Printf(TEXT("FInstancedStructLayoutReloadReference_%d_%d"), CdoKind, bContainer)
        : TEXT("FInstancedStructLayoutReloadValue");
    const FString Relative = bReferencePayload ? StructName + TEXT(".as") : TEXT("InstancedStructLayoutReloadTest.as");
    const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FAngelscriptManager::GetScriptRootDirectory(), Relative));
    const FAngelscriptManager::FFilenamePair File{Path, Relative};
    FString Initial(TEXT("#if EDITOR\nstruct FInstancedStructLayoutReloadValue\n{\n    UPROPERTY()\n    FInstancedStructReloadLifetime Lifetime;\n    UPROPERTY()\n    int Value = 1;\n    UPROPERTY()\n    int RetiredValue = 991;\n}\n#endif\n"));
    // Equal byte sizes keep this negative deterministic: wrong offsets/defaults are
    // observable without depending on an allocator overrun or a crash.
    FString Candidate(TEXT("#if EDITOR\nstruct FInstancedStructLayoutReloadValue\n{\n    UPROPERTY()\n    FInstancedStructReloadLifetime Lifetime;\n    UPROPERTY()\n    int AddedValue = 421067;\n    UPROPERTY()\n    int Value = 1;\n}\n#endif\n"));
    if (bReferencePayload)
    {
        Initial.ReplaceInline(TEXT("FInstancedStructLayoutReloadValue"), *StructName);
        Candidate.ReplaceInline(TEXT("FInstancedStructLayoutReloadValue"), *StructName);
        Initial.ReplaceInline(TEXT("    UPROPERTY()\n    FInstancedStructReloadLifetime Lifetime;\n"), TEXT(""));
        Candidate.ReplaceInline(TEXT("    UPROPERTY()\n    FInstancedStructReloadLifetime Lifetime;\n"), TEXT(""));
        Initial.ReplaceInline(TEXT("    int RetiredValue = 991;\n"),
            TEXT("    int RetiredValue = 991;\n    UPROPERTY()\n    UObject Reference;\n"));
        Candidate.ReplaceInline(TEXT("    int Value = 1;\n"),
            TEXT("    int Value = 1;\n    UPROPERTY()\n    int RetiredValue = 991;\n    UPROPERTY()\n    UObject Reference;\n"));
    }
    if (Scenario == 8)
    {
        Initial.ReplaceInline(TEXT("UObject Reference;"), TEXT("TArray<UObject> References;"));
        Candidate.ReplaceInline(TEXT("UObject Reference;"), TEXT("TArray<UObject> References;"));
    }
    if (Scenario == 7)
    {
        const FString InitialDestructor = FString::Printf(TEXT("    ~%s() { UInstancedStructReloadTestObject::RecordPayloadDestruction(1, Value); }\n"), *StructName);
        const FString CandidateDestructor = FString::Printf(TEXT("    ~%s() { UInstancedStructReloadTestObject::RecordPayloadDestruction(2, Value); }\n"), *StructName);
        Initial.ReplaceInline(TEXT("}\n#endif"), *(InitialDestructor + TEXT("}\n#endif")));
        Candidate.ReplaceInline(TEXT("}\n#endif"), *(CandidateDestructor + TEXT("}\n#endif")));
    }
    if (Scenario == 3)
    {
        Initial += TEXT("#if EDITOR\nstruct FInstancedStructMixedScalar { UPROPERTY() int Value = 7; }\n#endif\n");
        Candidate += TEXT("#if EDITOR\nstruct FInstancedStructMixedScalar { UPROPERTY() int Added = 19; UPROPERTY() int Value = 7; }\n#endif\n");
    }
    // 10/11: the soft owner's payload comes from its initializer; 12: the initializer is removed;
    // 13: a soft owner container root shortens before attachment; 18: a Blueprint child overrides the initializer.
    const bool bSoftOwner = Scenario == 2 || Scenario == 9 || (Scenario >= 10 && Scenario <= 13) || Scenario == 18;
    const bool bSoftChild = Scenario == 9 || Scenario == 11 || Scenario == 18;
    const bool bInitializedDefault = (Scenario >= 10 && Scenario <= 12) || Scenario == 18;
    const bool bNewInitializer = Scenario == 10 || Scenario == 11 || Scenario == 18;
    if (bSoftOwner)
    {
        const FString Initializer = FString::Printf(TEXT(" = FInstancedStruct::Make(%s())"), *StructName);
        const FString Class = TEXT("#if EDITOR\nclass UInstancedStructSoftOwner : UInstancedStructReloadTestObject\n{\n    UPROPERTY()\n    FInstancedStruct Payload%s;\n}\n#endif\n");
        Initial += Class.Replace(TEXT("%s"), bInitializedDefault ? *Initializer : TEXT(""));
        Candidate += Class.Replace(TEXT("%s"), bNewInitializer ? *Initializer : TEXT(""));
        // The new constructor builds a different value, so a migrated old initializer is distinguishable.
        if (bNewInitializer) Candidate.ReplaceInline(TEXT("    int Value = 1;\n"), TEXT("    int Value = 2;\n"));
    }
    if (Scenario == 12)
    {
        // The scalar companion shows the ordinary soft-reload result for the same removed initializer.
        Initial.ReplaceInline(TEXT("    FInstancedStruct Payload"), TEXT("    FInstancedStruct ScalarPayload = FInstancedStruct::Make(FInstancedStructSoftScalar());\n    UPROPERTY()\n    FInstancedStruct Payload"));
        Candidate.ReplaceInline(TEXT("    FInstancedStruct Payload"), TEXT("    FInstancedStruct ScalarPayload;\n    UPROPERTY()\n    FInstancedStruct Payload"));
        Initial += TEXT("#if EDITOR\nstruct FInstancedStructSoftScalar { UPROPERTY() int Value = 7; }\n#endif\n");
        Candidate += TEXT("#if EDITOR\nstruct FInstancedStructSoftScalar { UPROPERTY() int Added = 19; UPROPERTY() int Value = 7; }\n#endif\n");
    }
    // 14/15: the payload references an AS object whose class is replaced by the same reload.
    const bool bRequiredReference = Scenario == 14 || Scenario == 15;
    if (bRequiredReference)
    {
        Initial += TEXT("#if EDITOR\nclass UInstancedStructRequiredTarget : UObject\n{\n    UPROPERTY()\n    int Generation = 1;\n}\n#endif\n");
        Candidate += TEXT("#if EDITOR\nclass UInstancedStructRequiredTarget : UObject\n{\n    UPROPERTY()\n    int Generation = 1;\n    UPROPERTY()\n    int AddedGeneration = 2;\n}\n#endif\n");
    }
    if (Scenario == 17)
    {
        // An unchanged script class: its live objects keep their identity through the reload.
        const TCHAR* Target = TEXT("#if EDITOR\nclass UInstancedStructLiveTarget : UObject\n{\n    UPROPERTY()\n    int Generation = 1;\n}\n#endif\n");
        Initial += Target;
        Candidate += Target;
    }
    if (Scenario == 21)
    {
        // A larger old layout keeps an in-place rewrite to the new layout inside the old allocation.
        Initial.ReplaceInline(TEXT("    int RetiredValue = 991;\n"), TEXT("    int RetiredValue = 991;\n    UPROPERTY()\n    int RetiredPadA = 0;\n    UPROPERTY()\n    int RetiredPadB = 0;\n    UPROPERTY()\n    int RetiredPadC = 0;\n    UPROPERTY()\n    int RetiredPadD = 0;\n"));
    }
    if (CdoKind == 3)
    {
        Initial += TEXT("#if EDITOR\nclass UInstancedStructLayoutReloadOwner : UInstancedStructReloadTestObject\n{\n    UPROPERTY()\n    int Generation = 1;\n}\n#endif\n");
        Candidate += TEXT("#if EDITOR\nclass UInstancedStructLayoutReloadOwner : UInstancedStructReloadTestObject\n{\n    UPROPERTY()\n    int Generation = 1;\n    UPROPERTY()\n    int AddedGeneration = 2;\n}\n#endif\n");
    }
    if (!Test.TestFalse(TEXT("The temporary AS module is initially absent"), FPaths::FileExists(Path))) return false;
    auto Compile = [&]()
    {
        Manager.FileChangesDetectedForReload.AddUnique(File);
        for (int32 Attempt = 0; Attempt < 40 && Manager.FileChangesDetectedForReload.Contains(File); ++Attempt)
        {
            Manager.CheckForHotReload(ECompileType::FullReload);
            if (Manager.FileChangesDetectedForReload.Contains(File)) FPlatformProcess::Sleep(0.025f);
        }
        return !Manager.FileChangesDetectedForReload.Contains(File);
    };
    TStrongObjectPtr<UInstancedStructReloadTestObject> Owner(NewObject<UInstancedStructReloadTestObject>());
    TStrongObjectPtr<UBlueprint> Blueprint;
    TStrongObjectPtr<UPackage> DefaultPackage;
    // 23: the payload Blueprint derives from another Blueprint.
    TStrongObjectPtr<UBlueprint> ParentBlueprint;
    if (CdoKind == 1) Owner.Reset(GetMutableDefault<UInstancedStructReloadTestObject>());
    if (CdoKind == 2)
    {
        DefaultPackage.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/InstancedStructLayout_%s"), *FGuid::NewGuid().ToString())));
        UClass* PayloadParent = UInstancedStructReloadTestObject::StaticClass();
        if (Scenario == 23)
        {
            ParentBlueprint.Reset(FKismetEditorUtilities::CreateBlueprint(PayloadParent, DefaultPackage.Get(), TEXT("PayloadParent"),
                BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
            if (!Test.TestNotNull(TEXT("The parent Blueprint of the payload Blueprint is created"), ParentBlueprint.Get())) return false;
            PayloadParent = ParentBlueprint->GeneratedClass;
        }
        Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(PayloadParent,
            DefaultPackage.Get(), TEXT("LayoutDefault"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
        if (!Test.TestNotNull(TEXT("The user Blueprint default carrier is created"), Blueprint.Get())) return false;
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
    }
    // 21: a collector-only native carrier in its own package; the reload must neither rewrite nor dirty it.
    TStrongObjectPtr<UPackage> CarrierPackage;
    if (Scenario == 21)
    {
        CarrierPackage.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/InstancedStructCollectorCarrier_%s"), *FGuid::NewGuid().ToString())));
        Owner.Reset(NewObject<UInstancedStructReloadTestObject>(CarrierPackage.Get(), TEXT("CollectorOnlyCarrier")));
    }
    FInstancedStruct* NativeSlot = CdoKind ? &Owner->Reflected : &Owner->Hidden;
    Owner->bCollectorOnly = bCollectorOnly;
    Owner->bExposeARO = bExposeAro && !bCollectorOnly;
    ON_SCOPE_EXIT
    {
        Owner->Container.Empty();
        if (bSoftOwner) NativeSlot = FindFProperty<FStructProperty>(Owner->GetClass(), TEXT("Payload"))->ContainerPtrToValuePtr<FInstancedStruct>(Owner.Get());
        NativeSlot->Reset();
        if (const FStructProperty* Scalar = FindFProperty<FStructProperty>(Owner->GetClass(), TEXT("ScalarPayload")))
            Scalar->ContainerPtrToValuePtr<FInstancedStruct>(Owner.Get())->Reset();
        IFileManager::Get().Delete(*Path);
        Manager.FileChangesDetectedForReload.Remove(File);
        Manager.FileDeletionsDetectedForReload.AddUnique(File);
        for (int32 Attempt = 0; Attempt < 40 && Manager.FileDeletionsDetectedForReload.Contains(File); ++Attempt)
        {
            Manager.CheckForHotReload(ECompileType::FullReload);
            if (Manager.FileDeletionsDetectedForReload.Contains(File)) FPlatformProcess::Sleep(0.025f);
        }
        Test.TestFalse(TEXT("The temporary AS source is removed"), FPaths::FileExists(Path));
    };
    if (!Test.TestTrue(TEXT("The initial ordinary AS module compiles"),
        FFileHelper::SaveStringToFile(Initial, *Path) && Compile())) return false;
    const auto OldDesc = Manager.GetClass(StructName);
    UASStruct* Old = OldDesc.IsValid() ? Cast<UASStruct>(OldDesc->Struct) : nullptr;
    if (!Test.TestNotNull(TEXT("The initial struct is a real generated AS type"), Old)) return false;
    TStrongObjectPtr<UASStruct> OldPin(Old);
    if (bSoftOwner)
    {
        const auto Class = Manager.GetClass(TEXT("UInstancedStructSoftOwner"));
        if (!Test.TestTrue(TEXT("Soft owner class exists"), Class.IsValid() && Class->Class)) return false;
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Class->Class->GetDefaultObject()));
        if (bSoftChild)
        {
            DefaultPackage.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/InstancedStructSoftParent_%s"), *FGuid::NewGuid().ToString())));
            Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(Class->Class, DefaultPackage.Get(), TEXT("SoftParentDefault"),
                BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
            if (!Test.TestNotNull(TEXT("The Blueprint child of the soft AS owner is created"), Blueprint.Get())) return false;
            Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
        }
        NativeSlot = FindFProperty<FStructProperty>(Owner->GetClass(), TEXT("Payload"))->ContainerPtrToValuePtr<FInstancedStruct>(Owner.Get());
    }

    const auto RetiredOwnerDesc = CdoKind == 3 ? Manager.GetClass(TEXT("UInstancedStructLayoutReloadOwner")) : nullptr;
    if (CdoKind == 3)
    {
        if (!Test.TestTrue(TEXT("The AS default owner is a real generated class"), RetiredOwnerDesc.IsValid() && RetiredOwnerDesc->Class)) return false;
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(RetiredOwnerDesc->Class->GetDefaultObject()));
        NativeSlot = &Owner->Reflected;
    }
    if (Scenario == 4)
    {
        DefaultPackage.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/InstancedStructParent_%s"), *FGuid::NewGuid().ToString())));
        Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(RetiredOwnerDesc->Class,
            DefaultPackage.Get(), TEXT("ReplacingParentDefault"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
        NativeSlot = &Owner->Reflected;
    }
    if (Scenario == 5)
    {
        Owner.Reset(NewObject<UInstancedStructReloadTestObject>(GetTransientPackage(), Blueprint->GeneratedClass));
        NativeSlot = &Owner->Reflected;
    }
    // 16: the payload Blueprint is only a recorded dependent of a Blueprint this reload recompiles;
    // 17: a live script reference is outered to the generated class of such a Blueprint;
    // 19: the payload Blueprint has an uncompiled edit and such a Blueprint depends on it;
    // 20: the payload Blueprint is already queued for compilation when the reload compiles such a Blueprint;
    // 21: the side Blueprint is the only Blueprint that a reload of the changing struct would compile;
    // 22: the side Blueprint uses a macro whose library depends on the payload Blueprint, which has an uncompiled edit;
    // 23: the payload Blueprint's parent has an uncompiled edit and such a Blueprint depends on the parent.
    TStrongObjectPtr<UBlueprint> SideBlueprint;
    TStrongObjectPtr<UPackage> SidePackage;
    TStrongObjectPtr<UBlueprint> MacroLibrary;
    TStrongObjectPtr<UPackage> MacroPackage;
    if (Scenario == 16 || Scenario == 17 || (Scenario >= 19 && Scenario <= 23))
    {
        SidePackage.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/InstancedStructSide_%s"), *FGuid::NewGuid().ToString())));
        SideBlueprint.Reset(FKismetEditorUtilities::CreateBlueprint(UInstancedStructReloadTestObject::StaticClass(), SidePackage.Get(),
            Scenario == 16 ? TEXT("StaleDependencySource") : Scenario == 17 ? TEXT("LiveReferenceOuter")
                : Scenario == 19 ? TEXT("DirtyDependencyUser") : Scenario == 20 ? TEXT("QueuedCompileUser")
                : Scenario == 21 ? TEXT("CollectorCarrierUser") : Scenario == 22 ? TEXT("MacroDependencyUser")
                : TEXT("DirtyParentUser"), BPTYPE_Normal,
            UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
        FEdGraphPinType Pin;
        Pin.PinCategory = UEdGraphSchema_K2::PC_Struct;
        Pin.PinSubCategoryObject = Old;
        if (!Test.TestTrue(TEXT("The side Blueprint declares the changing AS struct"), SideBlueprint.IsValid()
            && FBlueprintEditorUtils::AddMemberVariable(SideBlueprint.Get(), TEXT("GraphPayload"), Pin))) return false;
        FKismetEditorUtilities::CompileBlueprint(SideBlueprint.Get());
        if (Scenario == 19 || Scenario == 22 || Scenario == 23)
        {
            // An uncompiled edit adds a cast to the payload Blueprint's class (19), a macro whose library casts to it
            // (22) or a cast to its parent's class (23): the side Blueprint depends on that Blueprint now, but no
            // dependency record names it yet, so nothing in that Blueprint names the side Blueprint.
            UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(SideBlueprint.Get());
            if (!Test.TestNotNull(TEXT("The side Blueprint has an event graph"), Graph)) return false;
            if (Scenario == 22)
            {
                MacroPackage.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/InstancedStructMacro_%s"), *FGuid::NewGuid().ToString())));
                MacroLibrary.Reset(FKismetEditorUtilities::CreateBlueprint(UInstancedStructReloadTestObject::StaticClass(), MacroPackage.Get(),
                    TEXT("PayloadCastMacros"), BPTYPE_MacroLibrary, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
                UEdGraph* Macro = MacroLibrary.IsValid() ? FBlueprintEditorUtils::CreateNewGraph(MacroLibrary.Get(), TEXT("CastToPayload"),
                    UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()) : nullptr;
                if (!Test.TestNotNull(TEXT("The macro library and its macro graph are created"), Macro)) return false;
                FBlueprintEditorUtils::AddMacroGraph(MacroLibrary.Get(), Macro, true, nullptr);
                FGraphNodeCreator<UK2Node_DynamicCast> MacroCast(*Macro);
                MacroCast.CreateNode(false)->TargetType = Blueprint->GeneratedClass;
                MacroCast.Finalize();
                FKismetEditorUtilities::CompileBlueprint(MacroLibrary.Get());
                FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
                Creator.CreateNode(false)->SetMacroGraph(Macro);
                Creator.Finalize();
            }
            else
            {
                FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
                UK2Node_DynamicCast* CastNode = Creator.CreateNode(false);
                CastNode->TargetType = Scenario == 23 ? ParentBlueprint->GeneratedClass : Blueprint->GeneratedClass;
                Creator.Finalize();
            }
            FBlueprintEditorUtils::MarkBlueprintAsModified(SideBlueprint.Get());
            TSet<TWeakObjectPtr<UBlueprint>> Dependencies;
            TSet<TWeakObjectPtr<UStruct>> Structs;
            FBlueprintEditorUtils::GatherDependencies(SideBlueprint.Get(), Dependencies, Structs);
            const TWeakObjectPtr<UBlueprint> PayloadBlueprint(Blueprint.Get());
            const TWeakObjectPtr<UBlueprint> SideUser(SideBlueprint.Get());
            if (Scenario == 19 && !Test.TestTrue(TEXT("The payload Blueprint is a current dependency of the side Blueprint that no record names"),
                Dependencies.Contains(PayloadBlueprint) && !SideBlueprint->CachedDependencies.Contains(PayloadBlueprint)
                && !Blueprint->CachedDependents.Contains(SideUser))) return false;
            if (Scenario == 22)
            {
                // Only the macro library's own dependencies name the payload Blueprint.
                TSet<TWeakObjectPtr<UBlueprint>> MacroDependencies;
                FBlueprintEditorUtils::GatherDependencies(MacroLibrary.Get(), MacroDependencies, Structs);
                const TWeakObjectPtr<UBlueprint> Library(MacroLibrary.Get());
                UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_MACRO_DEPENDENCY side_names_library=%d side_names_payload=%d library_names_payload=%d payload_records_library=%d"),
                    Dependencies.Contains(Library), Dependencies.Contains(PayloadBlueprint), MacroDependencies.Contains(PayloadBlueprint),
                    Blueprint->CachedDependents.Contains(Library));
                if (!Test.TestTrue(TEXT("The payload Blueprint is reached from the side Blueprint only through the macro library"),
                    Dependencies.Contains(Library) && !Dependencies.Contains(PayloadBlueprint) && MacroDependencies.Contains(PayloadBlueprint)
                    && !SideBlueprint->CachedDependencies.Contains(Library) && !SideBlueprint->CachedDependencies.Contains(PayloadBlueprint)
                    && !Blueprint->CachedDependents.Contains(SideUser) && MacroLibrary->Status != BS_Dirty)) return false;
            }
            if (Scenario == 23)
            {
                // The side Blueprint names only the payload Blueprint's parent. Whether the parent records the new child
                // as a dependent differs by UE version, so it is only logged.
                const TWeakObjectPtr<UBlueprint> Parent(ParentBlueprint.Get());
                UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_DIRTY_PARENT_DEPENDENCY side_names_parent=%d side_names_payload=%d parent_records_payload=%d parent_records_side=%d"),
                    Dependencies.Contains(Parent), Dependencies.Contains(PayloadBlueprint), ParentBlueprint->CachedDependents.Contains(PayloadBlueprint),
                    ParentBlueprint->CachedDependents.Contains(SideUser));
                if (!Test.TestTrue(TEXT("The side Blueprint depends on the payload Blueprint's parent and not on the payload Blueprint"),
                    Dependencies.Contains(Parent) && !Dependencies.Contains(PayloadBlueprint)
                    && !SideBlueprint->CachedDependencies.Contains(Parent))) return false;
            }
        }
    }
    if (Scenario == 16)
    {
        // The payload Blueprint once derived from the side Blueprint. UE recorded it as a dependent and keeps that
        // record after it is reparented away; nothing in the payload Blueprint names the side Blueprint any more.
        UBlueprint* Source = SideBlueprint.Get();
        auto Reparent = [&](UClass* Parent)
        {
            Blueprint->ParentClass = Parent;
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint.Get());
            FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
        };
        Reparent(Source->GeneratedClass.Get());
        const bool bRecorded = Source->CachedDependents.Contains(MakeWeakObjectPtr(Blueprint.Get()));
        Reparent(UInstancedStructReloadTestObject::StaticClass());
        const bool bStale = Source->CachedDependents.Contains(MakeWeakObjectPtr(Blueprint.Get()))
            && !Blueprint->CachedDependencies.Contains(MakeWeakObjectPtr(Source));
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_STALE_DEPENDENT recorded=%d stale=%d native_parent=%d"), bRecorded, bStale,
            Blueprint->GeneratedClass->GetSuperClass() == UInstancedStructReloadTestObject::StaticClass());
        if (!Test.TestTrue(TEXT("The payload Blueprint stays a recorded dependent after its reference is removed"), bRecorded && bStale
            && Blueprint->GeneratedClass->GetSuperClass() == UInstancedStructReloadTestObject::StaticClass())) return false;
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
        NativeSlot = &Owner->Reflected;
    }
    // Blueprint reinstancing clears private references into other packages; these inputs keep a public identity.
    const bool bPublicReference = Scenario == 1 || Scenario == 4 || Scenario == 5 || Scenario == 9 || Scenario == 16 || Scenario == 18
        || Scenario == 19 || Scenario == 20 || Scenario == 22 || Scenario == 23;
    TStrongObjectPtr<UObject> StableReference(bReferencePayload ? NewObject<UInstancedStructReloadTestObject>(
        GetTransientPackage(), NAME_None, bPublicReference ? RF_Public : RF_NoFlags) : nullptr);
    TStrongObjectPtr<UObject> RequiredTarget;
    UClass* RequiredClass = nullptr;
    // UE 5.4 strong pointers are GC referencers that reinstancing rewrites; old identities are kept by address.
    const UObject* RequiredIdentity = nullptr;
    if (bRequiredReference)
    {
        const auto TargetDesc = Manager.GetClass(TEXT("UInstancedStructRequiredTarget"));
        if (!Test.TestTrue(TEXT("The replaced AS target class exists"), TargetDesc.IsValid() && TargetDesc->Class)) return false;
        RequiredClass = TargetDesc->Class;
        RequiredTarget.Reset(NewObject<UObject>(GetTransientPackage(), RequiredClass));
        RequiredIdentity = RequiredTarget.Get();
        StableReference.Reset(RequiredTarget.Get());
    }
    UObject* LiveTarget = nullptr;
    if (Scenario == 17)
    {
        const auto TargetDesc = Manager.GetClass(TEXT("UInstancedStructLiveTarget"));
        if (!Test.TestTrue(TEXT("The unchanged AS target class exists"), TargetDesc.IsValid() && TargetDesc->Class)) return false;
        // Blueprint compilation moves every other object outered to its generated class out of it.
        LiveTarget = NewObject<UObject>(SideBlueprint->GeneratedClass.Get(), TargetDesc->Class, TEXT("LiveScriptReference"));
        StableReference.Reset(LiveTarget);
    }
    UObject* Replacement = nullptr;
    int32 ReplacementPairs = 0;
    const FDelegateHandle ReplacementHandle = FCoreUObjectDelegates::OnObjectsReinstanced.AddLambda(
        [&](const TMap<UObject*, UObject*>& Pairs)
        {
            for (const TPair<UObject*, UObject*>& Pair : Pairs)
                if (bRequiredReference && Pair.Key == RequiredIdentity) { ++ReplacementPairs; Replacement = Pair.Value; }
                else if (Scenario == 21) ++ReplacementPairs;
        });
    ON_SCOPE_EXIT
    {
        FCoreUObjectDelegates::OnObjectsReinstanced.Remove(ReplacementHandle);
        if (!bRequiredReference && Scenario != 17) return;
        // No payload or test pointer keeps either target generation alive into the deleting reload.
        NativeSlot->Reset();
        Owner->Container.Empty();
        StableReference.Reset();
        RequiredTarget.Reset();
        Replacement = nullptr;
        if (IsValid(LiveTarget)) LiveTarget->MarkAsGarbage();
        LiveTarget = nullptr;
        CollectGarbage(RF_NoFlags, true);
    };
    FInstancedStruct Seed;
    Seed.InitializeAs(Old);
    if (bReferencePayload)
    {
        if (Scenario == 8)
        {
            const FArrayProperty* Array = FindFProperty<FArrayProperty>(Old, TEXT("References"));
            FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Seed.GetMutableMemory()));
            Values.AddValues(3);
            const FObjectProperty* Leaf = CastFieldChecked<FObjectProperty>(Array->Inner);
            Leaf->SetObjectPropertyValue(Values.GetRawPtr(0), StableReference.Get());
            Leaf->SetObjectPropertyValue(Values.GetRawPtr(2), StableReference.Get());
        }
        else FindFProperty<FObjectProperty>(Old, TEXT("Reference"))->SetObjectPropertyValue_InContainer(
            Seed.GetMutableMemory(), StableReference.Get());
    }
    FindFProperty<FIntProperty>(Old, TEXT("Value"))->SetPropertyValue_InContainer(Seed.GetMutableMemory(), 6707);
    FindFProperty<FIntProperty>(Old, TEXT("RetiredValue"))->SetPropertyValue_InContainer(Seed.GetMutableMemory(), 991);
    if (bContainer)
    {
        const TArray<FInstancedStruct> Seeds = {Seed, FInstancedStruct(), Seed};
        Owner->Container = MakeArrayView(Seeds);
    }
    else if (!bInitializedDefault || Scenario == 18) *NativeSlot = Seed;
    FInstancedStruct* ScalarSlot = Scenario == 12 ? FindFProperty<FStructProperty>(Owner->GetClass(), TEXT("ScalarPayload"))
        ->ContainerPtrToValuePtr<FInstancedStruct>(Owner.Get()) : nullptr;
    if (bInitializedDefault && !Test.TestTrue(TEXT("The soft owner default holds its initializer payload of the old schema"),
        NativeSlot->GetScriptStruct() == Old && (!ScalarSlot || ScalarSlot->IsValid()))) return false;
    TStrongObjectPtr<UInstancedStructReloadTestObject> RetiredDefault;
    ON_SCOPE_EXIT { if (RetiredDefault.IsValid() && IsValid(RetiredDefault.Get())) RetiredDefault->Reflected.Reset(); };
    if (Scenario == 1 || Scenario == 15)
    {
        // Compile while the default holds the old payload: the retired default keeps an old copy outside the reload.
        FEdGraphPinType Pin;
        Pin.PinCategory = UEdGraphSchema_K2::PC_Struct;
        Pin.PinSubCategoryObject = Old;
        if (!Test.TestTrue(TEXT("Blueprint graph declares the changing AS struct"),
            FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(), TEXT("GraphPayload"), Pin))) return false;
        RetiredDefault.Reset(Owner.Get());
        FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
        NativeSlot = &Owner->Reflected;
        *NativeSlot = Seed;
        if (!Test.TestTrue(TEXT("The retired Blueprint default still holds the old payload"), RetiredDefault.Get() != Owner.Get()
            && RetiredDefault->GetClass() != Blueprint->GeneratedClass.Get()
            && RetiredDefault->Reflected.GetScriptStruct() == Old)) return false;
    }
    if (Scenario == 3)
    {
        const auto Scalar = Manager.GetClass(TEXT("FInstancedStructMixedScalar"));
        FInstancedStructReloadWrapper Wrapper;
        Wrapper.Payload.InitializeAs(CastChecked<UASStruct>(Scalar->Struct));
        FindFProperty<FIntProperty>(Scalar->Struct, TEXT("Value"))->SetPropertyValue_InContainer(Wrapper.Payload.GetMutableMemory(), 55);
        FInstancedStruct Direct;
        Direct.InitializeAs(CastChecked<UASStruct>(Scalar->Struct));
        FindFProperty<FIntProperty>(Scalar->Struct, TEXT("Value"))->SetPropertyValue_InContainer(Direct.GetMutableMemory(), 77);
        const TArray<FInstancedStruct> Values = {Seed, FInstancedStruct::Make(Wrapper), FInstancedStruct(), Direct};
        Owner->Container = MakeArrayView(Values);
    }
    Seed.Reset();
    const FStructProperty* Lifetime = FindFProperty<FStructProperty>(Old, TEXT("Lifetime"));
    TArray<TPair<const void*, int32>> OldLifetimes;
    auto CaptureLifetime = [&](FConstStructView Slot)
    {
        const void* Address = Lifetime->ContainerPtrToValuePtr<void>(Slot.GetMemory());
        OldLifetimes.Add({Address, FInstancedStructReloadLifetime::DestructionsAt(Address)});
    };
    if (Lifetime)
    {
        if (bContainer) { CaptureLifetime(Owner->Container[0]); CaptureLifetime(Owner->Container[2]); }
        else CaptureLifetime(FConstStructView(*NativeSlot));
    }
    const int32 OldDestructionCount = ReferenceDestructions.FindRef(1);
    const void* const PayloadMemory = NativeSlot->GetMemory();
    TWeakObjectPtr<UObject> WeakReference(StableReference.Get());
    UObject* ReferenceIdentity = StableReference.Get();
    if (Scenario == 6) StableReference.Reset();
    UClass* DefaultClass = Owner->GetClass();
    const UObject* const DefaultIdentity = Owner.Get();
    // Blueprint compilation reuses its generated class object, so the replaced parent is captured by identity.
    UClass* const OldParent = DefaultClass->GetSuperClass();
    int32 BlueprintCompiles = 0;
    int32 SideCompiles = 0;
    int32 ParentCompiles = 0;
    const FDelegateHandle BlueprintHandle = GEditor->OnBlueprintPreCompile().AddLambda([&](UBlueprint* Compiling)
    {
        if (Compiling == Blueprint.Get()) ++BlueprintCompiles;
        if (Compiling == SideBlueprint.Get()) ++SideCompiles;
        if (Compiling == ParentBlueprint.Get()) ++ParentCompiles;
    });
    ON_SCOPE_EXIT { GEditor->OnBlueprintPreCompile().Remove(BlueprintHandle); };
    if (DefaultPackage.IsValid()) DefaultPackage->SetDirtyFlag(false);
    if (SidePackage.IsValid()) SidePackage->SetDirtyFlag(false);
    if (CarrierPackage.IsValid()) CarrierPackage->SetDirtyFlag(false);
    ON_SCOPE_EXIT { if (Blueprint.IsValid() && Blueprint->bQueuedForCompilation) FBlueprintCompilationManager::FlushCompilationQueueAndReinstance(); };
    if (Scenario == 19 || Scenario == 22)
    {
        // An unsaved, uncompiled edit to the payload Blueprint.
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint.Get());
        if (!Test.TestTrue(TEXT("The payload Blueprint is dirty and not queued for compilation"),
            Blueprint->Status == BS_Dirty && !Blueprint->bQueuedForCompilation)) return false;
    }
    if (Scenario == 23)
    {
        // An unsaved, uncompiled edit to the payload Blueprint's parent only.
        FBlueprintEditorUtils::MarkBlueprintAsModified(ParentBlueprint.Get());
        if (!Test.TestTrue(TEXT("The parent Blueprint is dirty and neither it nor the payload Blueprint is queued or dirty"),
            ParentBlueprint->Status == BS_Dirty && !ParentBlueprint->bQueuedForCompilation
            && Blueprint->Status != BS_Dirty && !Blueprint->bQueuedForCompilation)) return false;
    }
    if (Scenario == 20)
    {
        FBlueprintCompilationManager::QueueForCompilation(Blueprint.Get());
        if (!Test.TestTrue(TEXT("The payload Blueprint is queued for compilation"), Blueprint->bQueuedForCompilation)) return false;
    }
    int32 ReloadPairs = 0;
    // The struct reload callback runs after soft reconstruction and before the payload lease attaches.
    bool bReconstructedTarget = false;
    bool bReconstructedEmpty = false;
    const FDelegateHandle Handle = FAngelscriptClassGenerator::OnStructReload.AddLambda(
        [&](UScriptStruct* Previous, UScriptStruct* Current)
        {
            if (Previous != Old) return;
            ++ReloadPairs;
            bReconstructedTarget = Current && NativeSlot->GetScriptStruct() == Current;
            bReconstructedEmpty = !NativeSlot->IsValid() && ScalarSlot && !ScalarSlot->IsValid();
            if (Scenario == 13)
            {
                // A third-party callback shortens the soft owner's container root before attachment.
                const TArray<FInstancedStruct> Shortened = {FInstancedStruct()};
                Owner->Container = MakeArrayView(Shortened);
            }
            if (Scenario == 6)
            {
                // The local value is invisible to UE GC: only the prepared lease may keep this reference alive.
                FInstancedStruct Unreflected = MoveTemp(*NativeSlot);
                CollectGarbage(RF_NoFlags, true);
                Test.TestTrue(TEXT("The migration lease alone keeps an unpinned payload reference alive"), WeakReference.IsValid());
                *NativeSlot = MoveTemp(Unreflected);
            }
        });
    ON_SCOPE_EXIT { FAngelscriptClassGenerator::OnStructReload.Remove(Handle); };
    if (Scenario == 5)
        Test.AddExpectedError(TEXT("SCRIPT_RELOAD_ADMISSION_REJECTED before_swap=1 reason=Reference payload reload requires a direct stable native owner"),
            EAutomationExpectedErrorFlags::Contains, 1);
    if (Scenario == 15)
        Test.AddExpectedError(TEXT("SCRIPT_RELOAD_ADMISSION_REJECTED before_swap=1 reason=Recompiled Blueprint default reference payloads cannot share a reload with replaced script object references"),
            EAutomationExpectedErrorFlags::Contains, 1);
    if (Scenario == 17)
        Test.AddExpectedError(TEXT("SCRIPT_RELOAD_ADMISSION_REJECTED before_swap=1 reason=Live script reference is owned by an object this reload may recompile"),
            EAutomationExpectedErrorFlags::Contains, 1);
    if (Scenario == 21)
        Test.AddExpectedError(FString::Printf(TEXT("SCRIPT_RELOAD_ADMISSION_REJECTED before_swap=1 reason=Collector-only script payload reference is not admitted: object=%s class="),
            *Owner->GetPathName()), EAutomationExpectedErrorFlags::Contains, 1);
    FPayloadAttachLog AttachLog;
    if (!Test.TestTrue(TEXT("The changed ordinary AS module completes FullReload"),
        FFileHelper::SaveStringToFile(Candidate, *Path) && Compile())) return false;
    AttachLog.Stop();
    const FString SoftReleased = AttachLog.Lines(TEXT("SCRIPT_PAYLOAD_SOFT_DEFAULTS_RELEASED"));
    const FString EarlyPublished = AttachLog.Lines(TEXT("SCRIPT_PAYLOAD_BLUEPRINT_DEFAULTS_PUBLISHED"));
    const FString Predicted = AttachLog.Lines(TEXT("SCRIPT_PAYLOAD_BLUEPRINT_RECOMPILE_PREDICTED"));
    if (Scenario == 17)
    {
        // The outer of a live script reference is in the stable domain; a Blueprint this reload may recompile
        // moves it, so the reload is refused before swap.
        const auto Current = Manager.GetClass(StructName);
        Test.TestTrue(TEXT("A live script reference outered to a recompiled Blueprint class refuses the reload before swap"),
            ReloadPairs == 0 && Current.IsValid() && Current->Struct == Old);
        const bool bUntouched = NativeSlot->GetScriptStruct() == Old
            && FindFProperty<FIntProperty>(Old, TEXT("Value"))->GetPropertyValue_InContainer(NativeSlot->GetMemory()) == 6707
            && FindFProperty<FObjectProperty>(Old, TEXT("Reference"))->GetObjectPropertyValue_InContainer(NativeSlot->GetMemory()) == LiveTarget
            && IsValid(LiveTarget) && LiveTarget->GetOuter() == SideBlueprint->GeneratedClass.Get();
        Test.TestTrue(TEXT("The refused reload leaves the payload and its live target in place"), bUntouched);
        Test.TestEqual(TEXT("The refused reload compiles no Blueprint"), SideCompiles, 0);
        Test.TestFalse(TEXT("The refused reload leaves the Blueprint package clean"), SidePackage->IsDirty());
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_LIVE_REFERENCE_RECOMPILED_OUTER rejected_before_swap=1 pairs=%d untouched=%d compiles=%d"),
            ReloadPairs, bUntouched, SideCompiles);
        return true;
    }
    if (Scenario == 21)
    {
        // Only reference collectors reach this payload, so no replacement pass can rebuild its memory for a new
        // layout: the reload is refused before swap and the old payload allocation stays exactly as it was.
        const auto Current = Manager.GetClass(StructName);
        const UStruct* CurrentStruct = Current.IsValid() ? Current->Struct : nullptr;
        const UScriptStruct* SlotType = NativeSlot->GetScriptStruct();
        const void* SlotMemory = NativeSlot->GetMemory();
        const FIntProperty* ValueProperty = SlotType ? FindFProperty<FIntProperty>(SlotType, TEXT("Value")) : nullptr;
        const FIntProperty* RetiredProperty = SlotType ? FindFProperty<FIntProperty>(SlotType, TEXT("RetiredValue")) : nullptr;
        const int32 Value = ValueProperty && SlotMemory ? ValueProperty->GetPropertyValue_InContainer(SlotMemory) : -1;
        const int32 Retired = RetiredProperty && SlotMemory ? RetiredProperty->GetPropertyValue_InContainer(SlotMemory) : -1;
        const int32 OldLifetimeDestroyed = OldLifetimes.Num() == 1
            ? FInstancedStructReloadLifetime::DestructionsAt(OldLifetimes[0].Key) - OldLifetimes[0].Value : -1;
        const bool bRejected = ReloadPairs == 0 && ReplacementPairs == 0 && CurrentStruct == Old;
        const bool bUntouched = SlotType == Old && SlotMemory == PayloadMemory && Value == 6707 && Retired == 991
            && OldLifetimeDestroyed == 0;
        Test.TestTrue(TEXT("A collector-only payload carrier refuses the reload before swap"), bRejected);
        Test.TestTrue(TEXT("The refused reload leaves the old payload type, allocation and values in place"), bUntouched);
        Test.TestEqual(TEXT("The refused reload compiles no Blueprint"), BlueprintCompiles + SideCompiles, 0);
        Test.TestFalse(TEXT("The refused reload leaves the carrier and Blueprint packages clean"),
            CarrierPackage->IsDirty() || SidePackage->IsDirty());
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_COLLECTOR_ONLY_ADMISSION rejected_before_swap=%d pairs=%d replaced=%d slot_type=%s same_allocation=%d value=%d retired=%d old_lifetime_destroyed=%d old_size=%d slot_size=%d compiles=%d side_compiles=%d carrier_dirty=%d side_dirty=%d"),
            bRejected, ReloadPairs, ReplacementPairs, SlotType == Old ? TEXT("old") : SlotType && SlotType == CurrentStruct ? TEXT("new") : SlotType ? TEXT("other") : TEXT("none"),
            SlotMemory == PayloadMemory, Value, Retired, OldLifetimeDestroyed, Old->GetStructureSize(), SlotType ? SlotType->GetStructureSize() : 0,
            BlueprintCompiles, SideCompiles, CarrierPackage->IsDirty(), SidePackage->IsDirty());
        return true;
    }
    if (Scenario == 15)
    {
        // A Blueprint default that this reload recompiles cannot wait for replaced script objects: refused before swap.
        const auto Current = Manager.GetClass(StructName);
        const auto Target = Manager.GetClass(TEXT("UInstancedStructRequiredTarget"));
        Test.TestTrue(TEXT("A recompiled Blueprint default with a replaced script reference refuses the reload before swap"),
            ReloadPairs == 0 && ReplacementPairs == 0 && Current.IsValid() && Current->Struct == Old
            && Target.IsValid() && Target->Class == RequiredClass);
        const bool bUntouched = NativeSlot->GetScriptStruct() == Old
            && FindFProperty<FIntProperty>(Old, TEXT("Value"))->GetPropertyValue_InContainer(NativeSlot->GetMemory()) == 6707
            && FindFProperty<FObjectProperty>(Old, TEXT("Reference"))->GetObjectPropertyValue_InContainer(NativeSlot->GetMemory()) == RequiredTarget.Get();
        Test.TestTrue(TEXT("The refused reload leaves the Blueprint default payload and its target untouched"), bUntouched);
        Test.TestEqual(TEXT("The refused reload compiles no Blueprint"), BlueprintCompiles, 0);
        Test.TestFalse(TEXT("The refused reload leaves the Blueprint package clean"), DefaultPackage->IsDirty());
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_RECOMPILED_BLUEPRINT_REQUIRED_ADMISSION rejected_before_swap=1 pairs=%d replaced=%d untouched=%d"),
            ReloadPairs, ReplacementPairs, bUntouched);
        return true;
    }
    if (Scenario == 5)
    {
        // Current product fact (M2b): a Blueprint instance owner is refused before swap; nothing is migrated or lost.
        const auto Current = Manager.GetClass(StructName);
        Test.TestTrue(TEXT("A Blueprint instance reference payload refuses the reload before swap"),
            ReloadPairs == 0 && Current.IsValid() && Current->Struct == Old);
        const bool bUntouched = NativeSlot->GetScriptStruct() == Old
            && FindFProperty<FIntProperty>(Old, TEXT("Value"))->GetPropertyValue_InContainer(NativeSlot->GetMemory()) == 6707
            && FindFProperty<FObjectProperty>(Old, TEXT("Reference"))->GetObjectPropertyValue_InContainer(NativeSlot->GetMemory()) == ReferenceIdentity;
        Test.TestTrue(TEXT("The refused reload leaves the Blueprint instance payload untouched"), bUntouched);
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_BLUEPRINT_INSTANCE_ADMISSION rejected_before_swap=1 pairs=%d untouched=%d"),
            ReloadPairs, bUntouched);
        return true;
    }
    const auto NextDesc = Manager.GetClass(StructName);
    UASStruct* Next = NextDesc.IsValid() ? Cast<UASStruct>(NextDesc->Struct) : nullptr;
    if (!Test.TestTrue(TEXT("A real replacement pair and changed field layout are observed"), Next && Next != Old
        && ReloadPairs == 1 && (bReferencePayload || Old->GetStructureSize() == Next->GetStructureSize())
        && (bReferencePayload || !FindFProperty<FProperty>(Next, TEXT("RetiredValue")))
        && FindFProperty<FIntProperty>(Old, TEXT("Value"))->GetOffset_ForInternal()
            != FindFProperty<FIntProperty>(Next, TEXT("Value"))->GetOffset_ForInternal())) return false;
    if (Scenario == 1)
    {
        Test.TestTrue(TEXT("The dependent Blueprint was compiled"), BlueprintCompiles > 0);
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
        NativeSlot = &Owner->Reflected;
    }
    if (Scenario == 16)
    {
        // The reload recompiles the recorded dependent with its source; the default was published before that.
        Test.TestTrue(TEXT("The side Blueprint and its recorded dependent are both compiled"), SideCompiles > 0 && BlueprintCompiles > 0);
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
        NativeSlot = &Owner->Reflected;
        Test.TestTrue(TEXT("The recorded dependent's default was replaced by its compilation"), Owner.Get() != DefaultIdentity);
        Test.TestEqual(TEXT("The recorded dependent's default is published before Blueprint compilation"), EarlyPublished,
            FString(TEXT("SCRIPT_PAYLOAD_BLUEPRINT_DEFAULTS_PUBLISHED released=1 retained=0 phase=attach")));
        // The payload Blueprint names nothing this reload compiles: only the recorded dependent edge reaches it.
        Test.TestEqual(TEXT("The recorded dependent edge predicts the recompilation"), Predicted,
            FString(TEXT("SCRIPT_PAYLOAD_BLUEPRINT_RECOMPILE_PREDICTED direct=1 queued=0 recorded=1 dirty=0 parent=0")));
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_STALE_DEPENDENT_RELOAD compiles=%d side_compiles=%d replaced_default=%d early=%s predicted=%s"),
            BlueprintCompiles, SideCompiles, Owner.Get() != DefaultIdentity, *EarlyPublished, *Predicted);
    }
    if (Scenario == 19 || Scenario == 20 || Scenario == 22 || Scenario == 23)
    {
        Test.TestTrue(TEXT("The side Blueprint is compiled by the reload"), SideCompiles > 0);
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
        NativeSlot = &Owner->Reflected;
        const bool bReplacedDefault = Owner.Get() != DefaultIdentity;
        Test.TestEqual(TEXT("The predicted default is published before Blueprint compilation"), EarlyPublished,
            FString(TEXT("SCRIPT_PAYLOAD_BLUEPRINT_DEFAULTS_PUBLISHED released=1 retained=0 phase=attach")));
        // 22/23: the payload Blueprint's own references name another Blueprint (the compiled macro library that it records
        // as a dependent, or its parent Blueprint's class), which alone predicts its recompilation.
        Test.TestEqual(TEXT("The dirty dependency or the queued compilation predicts the recompilation"), Predicted, FString(Scenario == 20
            ? TEXT("SCRIPT_PAYLOAD_BLUEPRINT_RECOMPILE_PREDICTED direct=0 queued=1 recorded=0 dirty=0 parent=0")
            : Scenario == 19 ? TEXT("SCRIPT_PAYLOAD_BLUEPRINT_RECOMPILE_PREDICTED direct=1 queued=0 recorded=0 dirty=1 parent=0")
            : TEXT("SCRIPT_PAYLOAD_BLUEPRINT_RECOMPILE_PREDICTED direct=1 queued=0 recorded=0 dirty=0 parent=0")));
        // 23: the dirty parent is compiled, and its child is relinked without a compilation of its own.
        const int32 DirtyCompiles = Scenario == 23 ? ParentCompiles : BlueprintCompiles;
        if (Scenario == 20)
            Test.TestTrue(TEXT("The queued Blueprint is compiled with the reload and its default replaced"), BlueprintCompiles > 0 && bReplacedDefault);
        else
        {
            // UE 5.5+ fully compiles a dirty dependency of a Blueprint it compiles and replaces its default. UE 5.4 has no
            // such pass: the default keeps its identity and the payload published into it. Both versions publish early.
            // 23: the relinked child's default may only be replaced when its parent is compiled.
            if (Scenario == 23)
                Test.TestTrue(TEXT("The relinked child's default is replaced only when its dirty parent is compiled"), !bReplacedDefault || DirtyCompiles > 0);
            else
                Test.TestTrue(TEXT("The dirty dependency's default is replaced exactly when it is compiled"), bReplacedDefault == (DirtyCompiles > 0));
// SOLOAS-COMPAT-BEGIN(<5.5) UE 5.4 has no out-of-date dependency compilation pass [test: SoloAS.InstancedStructDirtyBlueprintDependencyReload, SoloAS.InstancedStructMacroDirtyDependencyReload, SoloAS.InstancedStructDirtyParentBlueprintReload] [id: CS.Compatibility.Runtime.InstancedStructReloadTests.DirtyDependencyPass]
#if SOLOAS_UE_BEFORE(5, 5)
            Test.TestEqual(TEXT("UE 5.4 compiles no dirty dependency of a Blueprint it compiles"), DirtyCompiles, 0);
#else
            Test.TestTrue(TEXT("The out-of-date dependency pass compiles the dirty Blueprint and replaces the payload default"),
                DirtyCompiles > 0 && (bReplacedDefault || Scenario == 23));
#endif
// SOLOAS-COMPAT-END
        }
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_UNPLANNED_BLUEPRINT_RELOAD scenario=%d compiles=%d side_compiles=%d parent_compiles=%d replaced_default=%d early=%s predicted=%s"),
            Scenario, BlueprintCompiles, SideCompiles, ParentCompiles, bReplacedDefault, *EarlyPublished, *Predicted);
    }
    if (bSoftOwner && !bSoftChild)
    {
        const auto Class = Manager.GetClass(TEXT("UInstancedStructSoftOwner"));
        Test.TestTrue(TEXT("The AS owner follows soft reconstruction with unchanged UClass"), Class.IsValid() && Class->Class == DefaultClass);
        NativeSlot = FindFProperty<FStructProperty>(Owner->GetClass(), TEXT("Payload"))->ContainerPtrToValuePtr<FInstancedStruct>(Owner.Get());
    }
    if (bSoftChild)
    {
        const auto Class = Manager.GetClass(TEXT("UInstancedStructSoftOwner"));
        Test.TestTrue(TEXT("The soft AS parent keeps its UClass"), Class.IsValid() && Class->Class == DefaultClass->GetSuperClass());
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
        NativeSlot = FindFProperty<FStructProperty>(Owner->GetClass(), TEXT("Payload"))->ContainerPtrToValuePtr<FInstancedStruct>(Owner.Get());
    }
    if (Scenario == 10 || Scenario == 11)
    {
        // The new constructor already built the current schema: that default is the soft-reload result.
        Test.TestTrue(TEXT("Soft reconstruction gives the initialized default the target schema before attachment"), bReconstructedTarget);
        if (Test.TestTrue(TEXT("The initialized soft default owns the current schema"), NativeSlot->GetScriptStruct() == Next))
        {
            const void* Memory = NativeSlot->GetMemory();
            const int32 Value = FindFProperty<FIntProperty>(Next, TEXT("Value"))->GetPropertyValue_InContainer(Memory);
            const int32 Added = FindFProperty<FIntProperty>(Next, TEXT("AddedValue"))->GetPropertyValue_InContainer(Memory);
            const int32 Retained = FindFProperty<FIntProperty>(Next, TEXT("RetiredValue"))->GetPropertyValue_InContainer(Memory);
            UObject* Reference = FindFProperty<FObjectProperty>(Next, TEXT("Reference"))->GetObjectPropertyValue_InContainer(Memory);
            Test.TestEqual(TEXT("The initialized soft default has the new initializer's value, not the old one"), Value, 2);
            Test.TestEqual(TEXT("The initialized soft default has the added script default"), Added, 421067);
            Test.TestEqual(TEXT("The initialized soft default keeps its other script default"), Retained, 991);
            Test.TestNull(TEXT("The initialized soft default keeps an empty reference"), Reference);
            UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_SOFT_INITIALIZER child=%d reconstructed_target=%d current_type=1 value=%d added=%d"),
                bSoftChild, bReconstructedTarget, Value, Added);
        }
        // The script default and, for 11, its Blueprint child are rebuilt by soft reconstruction; the lease releases both.
        Test.TestEqual(TEXT("The lease releases every soft-rebuilt holder and keeps none"), SoftReleased, FString::Printf(
            TEXT("SCRIPT_PAYLOAD_SOFT_DEFAULTS_RELEASED released=%d retained=0 phase=attach"), bSoftChild ? 2 : 1));
        Test.TestEqual(TEXT("No released holder is published early"), EarlyPublished, FString());
        return true;
    }
    if (Scenario == 18)
    {
        // The Blueprint child's own default differs from the script parent's initializer, so soft reconstruction
        // keeps it in the admitted schema and the lease migrates it; the parent takes the new initializer.
        Test.TestFalse(TEXT("Soft reconstruction keeps the Blueprint child's override in the admitted schema"), bReconstructedTarget);
        const auto Parent = Manager.GetClass(TEXT("UInstancedStructSoftOwner"));
        const FStructProperty* ParentProperty = Parent.IsValid() && Parent->Class
            ? FindFProperty<FStructProperty>(Parent->Class, TEXT("Payload")) : nullptr;
        const FInstancedStruct* ParentSlot = ParentProperty
            ? ParentProperty->ContainerPtrToValuePtr<FInstancedStruct>(Parent->Class->GetDefaultObject()) : nullptr;
        if (Test.TestTrue(TEXT("The script parent default owns its new initializer"), ParentSlot && ParentSlot->GetScriptStruct() == Next))
            Test.TestEqual(TEXT("The script parent default has the new initializer value"),
                FindFProperty<FIntProperty>(Next, TEXT("Value"))->GetPropertyValue_InContainer(ParentSlot->GetMemory()), 2);
        Test.TestEqual(TEXT("The lease releases the rebuilt parent and keeps the overriding child"), SoftReleased,
            FString(TEXT("SCRIPT_PAYLOAD_SOFT_DEFAULTS_RELEASED released=1 retained=1 phase=attach")));
        Test.TestEqual(TEXT("The overriding child is published before Blueprint compilation"), EarlyPublished,
            FString(TEXT("SCRIPT_PAYLOAD_BLUEPRINT_DEFAULTS_PUBLISHED released=1 retained=0 phase=attach")));
        // The prediction is conservative for a script parent; whether or not the child is compiled, its final default
        // holds the published payload (checked below), and it is replaced exactly when it is compiled.
        const bool bReplacedDefault = Owner.Get() != DefaultIdentity;
        Test.TestTrue(TEXT("The overriding child's default is replaced exactly when it is compiled"), bReplacedDefault == (BlueprintCompiles > 0));
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_SOFT_OVERRIDE_RELOAD compiles=%d replaced_default=%d early=%s"),
            BlueprintCompiles, bReplacedDefault, *EarlyPublished);
    }
    if (Scenario == 12)
    {
        // Removing the initializer empties the default on soft reload; the scalar payload shows the same rule.
        ScalarSlot = FindFProperty<FStructProperty>(Owner->GetClass(), TEXT("ScalarPayload"))->ContainerPtrToValuePtr<FInstancedStruct>(Owner.Get());
        Test.TestTrue(TEXT("Soft reconstruction empties both initializer-removed defaults before attachment"), bReconstructedEmpty);
        Test.TestFalse(TEXT("The scalar soft default stays empty after its initializer is removed"), ScalarSlot->IsValid());
        Test.TestFalse(TEXT("The reference soft default stays empty like the scalar one"), NativeSlot->IsValid());
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_SOFT_INITIALIZER_REMOVED reconstructed_empty=%d scalar_valid=%d reference_valid=%d"),
            bReconstructedEmpty, ScalarSlot->IsValid(), NativeSlot->IsValid());
        return true;
    }
    if (Scenario == 13)
    {
        // The shortened root holds no admitted old payload, so the lease leaves it as the callback wrote it.
        const bool bShortened = Owner->Container.Num() == 1 && !Owner->Container[0].IsValid();
        Test.TestTrue(TEXT("A soft owner container shortened before attachment keeps its current contents"), bShortened);
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_SOFT_CONTAINER_SHORTENED count=%d empty_slot=%d"),
            Owner->Container.Num(), bShortened);
        return true;
    }
    if (Scenario == 1)
    {
        const bool bRetiredAlive = RetiredDefault.IsValid() && IsValid(RetiredDefault.Get());
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_RETIRED_BLUEPRINT_DEFAULT alive=%d old_payload=%d"),
            bRetiredAlive, bRetiredAlive && RetiredDefault->Reflected.GetScriptStruct() == Old);
        if (Test.TestTrue(TEXT("The retired Blueprint default is still held and valid after the reload"), bRetiredAlive))
            Test.TestTrue(TEXT("The retired Blueprint default keeps its old payload outside the reload"),
                RetiredDefault->Reflected.GetScriptStruct() == Old);
    }
    if (bRequiredReference)
    {
        const auto Target = Manager.GetClass(TEXT("UInstancedStructRequiredTarget"));
        Test.TestTrue(TEXT("The referenced AS object is replaced exactly once by a current class instance"),
            ReplacementPairs == 1 && IsValid(Replacement) && Replacement != ReferenceIdentity
            && Target.IsValid() && Target->Class && Target->Class != RequiredClass && Replacement->GetClass() == Target->Class);
        ReferenceIdentity = Replacement;
    }
    if (Scenario == 4)
    {
        const auto CurrentParent = Manager.GetClass(TEXT("UInstancedStructLayoutReloadOwner"));
        Test.TestTrue(TEXT("The Blueprint default follows its replaced AS parent"),
            CurrentParent.IsValid() && CurrentParent->Class && CurrentParent->Class != OldParent
            && Blueprint->GeneratedClass->GetSuperClass() == CurrentParent->Class);
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_BLUEPRINT_PARENT super=%s current=%s old=%s same_generated_class=%d"),
            *GetPathNameSafe(Blueprint->GeneratedClass->GetSuperClass()), *GetPathNameSafe(CurrentParent.IsValid() ? CurrentParent->Class : nullptr),
            *GetPathNameSafe(OldParent), Blueprint->GeneratedClass.Get() == DefaultClass);
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
        NativeSlot = &Owner->Reflected;
    }
    if (CdoKind == 3 && Scenario != 4)
    {
        const auto CurrentOwnerDesc = Manager.GetClass(TEXT("UInstancedStructLayoutReloadOwner"));
        Test.TestTrue(TEXT("The old AS class and its CDO remain in the class replacement domain"),
            CurrentOwnerDesc.IsValid() && CurrentOwnerDesc->Class != DefaultClass
            && CurrentOwnerDesc->Class->GetDefaultObject() != DefaultIdentity
            && DefaultClass->HasAnyClassFlags(CLASS_NewerVersionExists));
        Test.TestTrue(TEXT("Pre-migration leaves the retired AS CDO's old payload untouched"), NativeSlot->GetScriptStruct() == Old);
        for (const auto& Previous : OldLifetimes)
            Test.TestEqual(TEXT("Pre-migration does not destroy the retired AS CDO's payload"),
                FInstancedStructReloadLifetime::DestructionsAt(Previous.Key) - Previous.Value, 0);
        return true;
    }
    if (CdoKind && Scenario != 1 && Scenario != 4 && Scenario != 5 && Scenario != 9 && Scenario != 16 && Scenario != 18
        && Scenario != 19 && Scenario != 20 && Scenario != 22 && Scenario != 23)
    {
        Test.TestTrue(TEXT("Stable native or Blueprint CDO identity is preserved"),
            Owner->GetClass() == DefaultClass && DefaultClass->GetDefaultObject() == Owner.Get());
        Test.TestEqual(TEXT("Payload reload does not recompile the Blueprint"), BlueprintCompiles, 0);
        if (DefaultPackage.IsValid()) Test.TestFalse(TEXT("Payload reload does not dirty the default package"), DefaultPackage->IsDirty());
    }
    if (Scenario == 7)
        Test.TestEqual(TEXT("Each reference payload runs its old destructor exactly once"),
            ReferenceDestructions.FindRef(1) - OldDestructionCount, bContainer ? 2 : 1);
    if (bReferencePayload)
    {
        auto CheckMigrated = [&](FConstStructView Slot)
        {
            if (!Test.TestTrue(TEXT("Reference CDO payload owns the current schema"), Slot.GetScriptStruct() == Next)) return;
            const int32 Value = FindFProperty<FIntProperty>(Next, TEXT("Value"))->GetPropertyValue_InContainer(Slot.GetMemory());
            const int32 Added = FindFProperty<FIntProperty>(Next, TEXT("AddedValue"))->GetPropertyValue_InContainer(Slot.GetMemory());
            UObject* Reference = ReferenceIdentity;
            if (Scenario == 8)
            {
                const FArrayProperty* Array = FindFProperty<FArrayProperty>(Next, TEXT("References"));
                FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Slot.GetMemory()));
                const FObjectProperty* Leaf = CastFieldChecked<FObjectProperty>(Array->Inner);
                Test.TestEqual(TEXT("Reference array keeps its length"), Values.Num(), 3);
                if (Values.Num() == 3)
                {
                    Test.TestTrue(TEXT("Reference array keeps both aliases"),
                        Leaf->GetObjectPropertyValue(Values.GetRawPtr(0)) == ReferenceIdentity
                        && Leaf->GetObjectPropertyValue(Values.GetRawPtr(2)) == ReferenceIdentity);
                    Test.TestNull(TEXT("Reference array preserves its empty element"), Leaf->GetObjectPropertyValue(Values.GetRawPtr(1)));
                }
            }
            else Reference = FindFProperty<FObjectProperty>(Next, TEXT("Reference"))->GetObjectPropertyValue_InContainer(Slot.GetMemory());
            Test.TestEqual(TEXT("Reference CDO scalar survives layout growth"), Value, 6707);
            Test.TestEqual(TEXT("Reference CDO added field gets its script default"), Added, 421067);
            Test.TestTrue(TEXT("Reference CDO keeps its stable object identity"), Reference == ReferenceIdentity);
            UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_CDO_REFERENCE_MIGRATED cdo=%d container=%d current_type=1 value=%d added=%d reference_preserved=%d migration_credit=1"),
                CdoKind, bContainer, Value, Added, Reference == ReferenceIdentity);
        };
        if (bContainer)
        {
            const int32 Slots = Scenario == 3 ? 4 : 3;
            Test.TestEqual(TEXT("Reference CDO container slot count is retained"), Owner->Container.Num(), Slots);
            if (Owner->Container.Num() == Slots)
            {
                CheckMigrated(Owner->Container[0]);
                if (Scenario == 3)
                {
                    const auto Scalar = Manager.GetClass(TEXT("FInstancedStructMixedScalar"));
                    const FConstStructView Nested = Owner->Container[1];
                    if (Test.TestTrue(TEXT("The mixed root keeps its native wrapper slot"),
                        Nested.GetScriptStruct() == FInstancedStructReloadWrapper::StaticStruct() && Nested.GetMemory()))
                    {
                        const FInstancedStruct& Payload = static_cast<const FInstancedStructReloadWrapper*>(static_cast<const void*>(Nested.GetMemory()))->Payload;
                        if (Test.TestTrue(TEXT("A scalar nested in a mixed reference root owns its current schema"), Payload.GetScriptStruct() == Scalar->Struct))
                        {
                            Test.TestEqual(TEXT("The nested scalar keeps its value"), FindFProperty<FIntProperty>(Scalar->Struct, TEXT("Value"))
                                ->GetPropertyValue_InContainer(Payload.GetMemory()), 55);
                            Test.TestEqual(TEXT("The nested scalar gets its added default"), FindFProperty<FIntProperty>(Scalar->Struct, TEXT("Added"))
                                ->GetPropertyValue_InContainer(Payload.GetMemory()), 19);
                        }
                    }
                    Test.TestFalse(TEXT("Mixed reference root preserves its empty slot"), Owner->Container[2].IsValid());
                    const FConstStructView Direct = Owner->Container[3];
                    if (Test.TestTrue(TEXT("A top-level scalar in a mixed reference root owns its current schema"), Direct.GetScriptStruct() == Scalar->Struct))
                    {
                        Test.TestEqual(TEXT("The top-level scalar keeps its value"), FindFProperty<FIntProperty>(Scalar->Struct, TEXT("Value"))
                            ->GetPropertyValue_InContainer(Direct.GetMemory()), 77);
                        Test.TestEqual(TEXT("The top-level scalar gets its added default"), FindFProperty<FIntProperty>(Scalar->Struct, TEXT("Added"))
                            ->GetPropertyValue_InContainer(Direct.GetMemory()), 19);
                    }
                }
                else
                {
                    Test.TestFalse(TEXT("Reference root preserves its empty slot"), Owner->Container[1].IsValid());
                    CheckMigrated(Owner->Container[2]);
                }
            }
        }
        else CheckMigrated(FConstStructView(*NativeSlot));
        return true;
    }
    for (const auto& Previous : OldLifetimes)
        Test.TestEqual(TEXT("Each old payload lifetime ends exactly once during layout migration"),
            FInstancedStructReloadLifetime::DestructionsAt(Previous.Key) - Previous.Value, 1);
    auto Check = [&](FConstStructView Slot)
    {
        if (!Test.TestTrue(TEXT("The published slot owns the current type and storage"),
            Slot.GetScriptStruct() == Next && Slot.GetMemory())) return;
        const int32 Value = FindFProperty<FIntProperty>(Next, TEXT("Value"))->GetPropertyValue_InContainer(Slot.GetMemory());
        const int32 Added = FindFProperty<FIntProperty>(Next, TEXT("AddedValue"))->GetPropertyValue_InContainer(Slot.GetMemory());
        UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_LAYOUT container=%d collector_only=%d value=%d added=%d pairs=%d old_size=%d new_size=%d"),
            bContainer, bCollectorOnly, Value, Added, ReloadPairs, Old->GetStructureSize(), Next->GetStructureSize());
        Test.TestEqual(TEXT("FullReload preserves the old named field across offset changes"), Value, 6707);
        Test.TestEqual(TEXT("FullReload initializes the added field from its script default"), Added, 421067);
    };
    if (bContainer)
    {
        Test.TestEqual(TEXT("FullReload preserves container slot count"), Owner->Container.Num(), 3);
        if (Owner->Container.Num() == 3)
        {
            Test.TestFalse(TEXT("FullReload preserves the empty slot"), Owner->Container[1].IsValid());
            Check(Owner->Container[0]);
            Check(Owner->Container[2]);
        }
    }
    else Check(FConstStructView(*NativeSlot));
    return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructContainerFullReloadTest,
    "SoloAS.InstancedStructContainerFullReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructContainerFullReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, true, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructHiddenFullReloadTest,
    "SoloAS.InstancedStructHiddenFullReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructHiddenFullReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructCollectorOnlyFullReloadTest,
    "SoloAS.InstancedStructCollectorOnlyFullReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructCollectorOnlyFullReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, true); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructNativeCdoFullReloadTest,
    "SoloAS.InstancedStructNativeCdoFullReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructNativeCdoFullReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructBlueprintCdoFullReloadTest,
    "SoloAS.InstancedStructBlueprintCdoFullReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructBlueprintCdoFullReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 2); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructRetiredScriptCdoFullReloadTest,
    "SoloAS.InstancedStructRetiredScriptCdoFullReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructRetiredScriptCdoFullReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 3); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructHiddenSerializedFullReloadTest,
    "SoloAS.InstancedStructHiddenSerializedFullReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructHiddenSerializedFullReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 0, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructCdoReferencePayloadDoesNotAbortTest,
    "SoloAS.InstancedStructCdoReferencePayloadFullReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructCdoReferencePayloadDoesNotAbortTest::RunTest(const FString&)
{
    for (int32 CdoKind : {1, 2})
        for (bool bContainer : {false, true})
            if (!RunLayoutFullReload(*this, bContainer, false, CdoKind, true, true)) return false;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructGraphDependencyReloadTest,
    "SoloAS.InstancedStructGraphDependencyReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructGraphDependencyReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 2, true, true, 1); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructSoftOwnerReloadTest,
    "SoloAS.InstancedStructSoftOwnerReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructSoftOwnerReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1, true, true, 2); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructSoftBlueprintChildReloadTest,
    "SoloAS.InstancedStructSoftBlueprintChildReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructSoftBlueprintChildReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1, true, true, 9); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructMixedContainerReloadTest,
    "SoloAS.InstancedStructMixedContainerReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructMixedContainerReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, true, false, 1, true, true, 3); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructBlueprintParentReloadTest,
    "SoloAS.InstancedStructBlueprintParentReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructBlueprintParentReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 3, true, true, 4); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructBlueprintInstanceReloadTest,
    "SoloAS.InstancedStructBlueprintInstanceReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructBlueprintInstanceReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 2, true, true, 5); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructReferenceLeaseGcTest,
    "SoloAS.InstancedStructReferenceLeaseGc", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructReferenceLeaseGcTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1, true, true, 6); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructReferenceDestructionTest,
    "SoloAS.InstancedStructReferenceDestruction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructReferenceDestructionTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1, true, true, 7); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructReferenceArrayCdoReloadTest,
    "SoloAS.InstancedStructReferenceArrayCdoReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructReferenceArrayCdoReloadTest::RunTest(const FString&)
{
    return RunLayoutFullReload(*this, false, false, 1, true, true, 8)
        && RunLayoutFullReload(*this, true, false, 1, true, true, 8);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructSoftInitializerReloadTest,
    "SoloAS.InstancedStructSoftInitializerReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructSoftInitializerReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1, true, true, 10); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructSoftInitializerBlueprintChildReloadTest,
    "SoloAS.InstancedStructSoftInitializerBlueprintChildReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructSoftInitializerBlueprintChildReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1, true, true, 11); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructSoftInitializerRemovedReloadTest,
    "SoloAS.InstancedStructSoftInitializerRemovedReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructSoftInitializerRemovedReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1, true, true, 12); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructSoftContainerShortenedReloadTest,
    "SoloAS.InstancedStructSoftContainerShortenedReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructSoftContainerShortenedReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, true, false, 1, true, true, 13); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructRequiredReferenceNativeCdoReloadTest,
    "SoloAS.InstancedStructRequiredReferenceNativeCdoReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructRequiredReferenceNativeCdoReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1, true, true, 14); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructRequiredReferenceBlueprintCdoReloadTest,
    "SoloAS.InstancedStructRequiredReferenceBlueprintCdoReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructRequiredReferenceBlueprintCdoReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 2, true, true, 14); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructRecompiledBlueprintRequiredReferenceTest,
    "SoloAS.InstancedStructRecompiledBlueprintRequiredReference", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructRecompiledBlueprintRequiredReferenceTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 2, true, true, 15); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructRequiredReferenceContainerReloadTest,
    "SoloAS.InstancedStructRequiredReferenceContainerReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructRequiredReferenceContainerReloadTest::RunTest(const FString&)
{
    return RunLayoutFullReload(*this, true, false, 1, true, true, 14)
        && RunLayoutFullReload(*this, true, false, 2, true, true, 14);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructStaleBlueprintDependentReloadTest,
    "SoloAS.InstancedStructStaleBlueprintDependentReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructStaleBlueprintDependentReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 2, true, true, 16); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructLiveReferenceRecompiledOuterReloadTest,
    "SoloAS.InstancedStructLiveReferenceRecompiledOuterReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructLiveReferenceRecompiledOuterReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1, true, true, 17); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructSoftInitializerBlueprintOverrideReloadTest,
    "SoloAS.InstancedStructSoftInitializerBlueprintOverrideReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructSoftInitializerBlueprintOverrideReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 1, true, true, 18); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructDirtyBlueprintDependencyReloadTest,
    "SoloAS.InstancedStructDirtyBlueprintDependencyReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructDirtyBlueprintDependencyReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 2, true, true, 19); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructQueuedBlueprintReloadTest,
    "SoloAS.InstancedStructQueuedBlueprintReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructQueuedBlueprintReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 2, true, true, 20); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructCollectorOnlyPayloadAdmissionTest,
    "SoloAS.InstancedStructCollectorOnlyPayloadAdmission", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructCollectorOnlyPayloadAdmissionTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, true, 0, true, false, 21); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructMacroDirtyDependencyReloadTest,
    "SoloAS.InstancedStructMacroDirtyDependencyReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructMacroDirtyDependencyReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 2, true, true, 22); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructDirtyParentBlueprintReloadTest,
    "SoloAS.InstancedStructDirtyParentBlueprintReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructDirtyParentBlueprintReloadTest::RunTest(const FString&) { return RunLayoutFullReload(*this, false, false, 2, true, true, 23); }

namespace
{
constexpr const TCHAR* PersistStructName = TEXT("FInstancedStructCdoPersistValue");
constexpr const TCHAR* PersistScriptName = TEXT("InstancedStructCdoPersist.as");
constexpr const TCHAR* PersistPackageName = TEXT("/Game/IsreloadR2/BP_ReferencePayload");
constexpr const TCHAR* PersistBaselineName = TEXT("/Game/IsreloadR2/BP_ReferencePayload_Baseline");
constexpr const TCHAR* PersistReloadName = TEXT("/Game/IsreloadR2/BP_ReferencePayload_Reloaded");
constexpr const TCHAR* PersistAssetName = TEXT("BP_ReferencePayload");

FString PersistFilename(const TCHAR* PackageName)
{
    return FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
}

bool CheckPersistedBlueprint(FAutomationTestBase& Test, const TCHAR* PackageName, int32 ExpectedValue, const TCHAR* Phase)
{
    UPackage* Package = LoadPackage(nullptr, PackageName, LOAD_None);
    UBlueprint* Blueprint = Package ? FindObject<UBlueprint>(Package, PersistAssetName) : nullptr;
    auto* Owner = Blueprint && Blueprint->GeneratedClass
        ? Cast<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
    const auto Current = FAngelscriptManager::Get().GetClass(PersistStructName);
    UASStruct* Schema = Current.IsValid() ? Cast<UASStruct>(Current->Struct) : nullptr;
    if (!Test.TestTrue(TEXT("The saved Blueprint and current AS schema load"), Owner && Schema)) return false;
    const FInstancedStruct& Payload = Owner->Reflected;
    const UScriptStruct* Actual = Payload.GetScriptStruct();
    const FIntProperty* Value = Actual ? FindFProperty<FIntProperty>(Actual, TEXT("Value")) : nullptr;
    const FIntProperty* Added = Actual ? FindFProperty<FIntProperty>(Actual, TEXT("AddedValue")) : nullptr;
    const FObjectProperty* Reference = Actual ? FindFProperty<FObjectProperty>(Actual, TEXT("Reference")) : nullptr;
    const int32 ObservedValue = Value ? Value->GetPropertyValue_InContainer(Payload.GetMemory()) : INDEX_NONE;
    const int32 ObservedAdded = Added ? Added->GetPropertyValue_InContainer(Payload.GetMemory()) : INDEX_NONE;
    UObject* ObservedReference = Reference ? Reference->GetObjectPropertyValue_InContainer(Payload.GetMemory()) : nullptr;
    UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_PERSIST_READ phase=%s type=%s current_type=%d value=%d added=%d self_reference=%d"),
        Phase, *GetPathNameSafe(Actual), Actual == Schema, ObservedValue, ObservedAdded, ObservedReference == Blueprint);
    Test.TestTrue(TEXT("Reloaded default payload resolves to the current AS struct"), Actual == Schema);
    Test.TestEqual(TEXT("Reloaded Blueprint preserves its saved scalar default"), ObservedValue, ExpectedValue);
    Test.TestEqual(TEXT("Reloaded Blueprint initializes the added default field"), ObservedAdded, 421067);
    Test.TestTrue(TEXT("Reloaded Blueprint preserves its self asset reference"), ObservedReference == Blueprint);
    return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructReferenceCdoPersistWriteTest,
    "SoloAS.InstancedStructReferenceCdoPersistWrite", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInstancedStructReferenceCdoPersistWriteTest::RunTest(const FString&)
{
    FAngelscriptManager& Manager = FAngelscriptManager::Get();
    const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Manager.GetScriptRootDirectory(), PersistScriptName));
    const FAngelscriptManager::FFilenamePair File{Path, PersistScriptName};
    const FString Initial = TEXT(R"AS(
struct FInstancedStructCdoPersistValue
{
    UPROPERTY()
    int Value = 1;
    UPROPERTY()
    UObject Reference;
}
)AS");
    const FString Candidate = Initial.Replace(TEXT("    int Value = 1;"),
        TEXT("    int AddedValue = 421067;\n    UPROPERTY()\n    int Value = 1;"));
    auto Compile = [&]()
    {
        Manager.FileChangesDetectedForReload.AddUnique(File);
        for (int32 Attempt = 0; Attempt < 40 && Manager.FileChangesDetectedForReload.Contains(File); ++Attempt)
        {
            Manager.CheckForHotReload(ECompileType::FullReload);
            if (Manager.FileChangesDetectedForReload.Contains(File)) FPlatformProcess::Sleep(0.025f);
        }
        return !Manager.FileChangesDetectedForReload.Contains(File);
    };
    // A reused project keeps the previous writer's packages on disk. A new package whose file exists is
    // treated as partially loaded and cannot be saved, so this writer removes the files it owns first.
    for (const TCHAR* Owned : {PersistPackageName, PersistBaselineName, PersistReloadName})
    {
        if (!TestNull(TEXT("A previous persistence package is not loaded in this editor"), FindPackage(nullptr, Owned))) return false;
        const FString Stale = PersistFilename(Owned);
        if (IFileManager::Get().FileExists(*Stale)
            && !TestTrue(TEXT("A previous persistence package file is removed"), IFileManager::Get().Delete(*Stale, false, true))) return false;
    }
    if (!TestTrue(TEXT("Initial persistence script compiles, including a reused test project"),
        FFileHelper::SaveStringToFile(Initial, *Path) && Compile())) return false;
    const auto OldDesc = Manager.GetClass(PersistStructName);
    UASStruct* Old = OldDesc.IsValid() ? Cast<UASStruct>(OldDesc->Struct) : nullptr;
    if (!TestNotNull(TEXT("Initial persistence AS struct exists"), Old)) return false;
    TStrongObjectPtr<UASStruct> OldPin(Old);
    TStrongObjectPtr<UPackage> Package(CreatePackage(PersistPackageName));
    TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(UInstancedStructReloadTestObject::StaticClass(),
        Package.Get(), PersistAssetName, BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
    if (!TestTrue(TEXT("Persistence Blueprint has a real generated class"), Blueprint.IsValid() && Blueprint->GeneratedClass)) return false;
    auto* Owner = CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject());
    Owner->Reflected.InitializeAs(Old);
    FindFProperty<FIntProperty>(Old, TEXT("Value"))->SetPropertyValue_InContainer(Owner->Reflected.GetMutableMemory(), 6707);
    FindFProperty<FObjectProperty>(Old, TEXT("Reference"))->SetObjectPropertyValue_InContainer(Owner->Reflected.GetMutableMemory(), Blueprint.Get());
    CastChecked<UBlueprintGeneratedClass>(Blueprint->GeneratedClass)->UpdateCustomPropertyListForPostConstruction();
    {
        TStrongObjectPtr<UInstancedStructReloadTestObject> BaselineInstance(NewObject<UInstancedStructReloadTestObject>(
            GetTransientPackage(), Blueprint->GeneratedClass));
        if (!TestTrue(TEXT("Before reload a new instance receives the populated CDO payload"), BaselineInstance->Reflected.GetScriptStruct() == Old)) return false;
        TestEqual(TEXT("Before reload the instance copies the scalar default"),
            FindFProperty<FIntProperty>(Old, TEXT("Value"))->GetPropertyValue_InContainer(BaselineInstance->Reflected.GetMemory()), 6707);
        BaselineInstance->Reflected.Reset();
    }
    Blueprint->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    const FString Filename = PersistFilename(PersistPackageName);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    if (!TestTrue(TEXT("Blueprint saves before the struct reload"), UPackage::SavePackage(Package.Get(), Blueprint.Get(), *Filename, Args))) return false;
    if (!TestEqual(TEXT("A disk checkpoint is preserved before reload"),
        IFileManager::Get().Copy(*PersistFilename(PersistBaselineName), *Filename), uint32(COPY_OK))) return false;
    const FMD5Hash BeforeHash = FMD5Hash::HashFile(*Filename);
    if (!TestTrue(TEXT("Persistence candidate completes real FullReload"), FFileHelper::SaveStringToFile(Candidate, *Path) && Compile())) return false;
    const auto NextDesc = Manager.GetClass(PersistStructName);
    UASStruct* Next = NextDesc.IsValid() ? Cast<UASStruct>(NextDesc->Struct) : nullptr;
    if (!TestTrue(TEXT("Persistence candidate creates a new struct with added storage"), Next && Next != Old
        && FindFProperty<FIntProperty>(Next, TEXT("AddedValue")))) return false;

    const FString ReaderRelative(TEXT("InstancedStructCdoPersistReader.as"));
    const FString ReaderPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Manager.GetScriptRootDirectory(), ReaderRelative));
    const FString ReaderSource = TEXT(R"AS(
class UInstancedStructCdoPersistReader : UObject
{
    int Read(const FInstancedStruct& Payload) const
    {
        FInstancedStructCdoPersistValue Value;
        Payload.Get(Value);
        return Value.Value;
    }
}
)AS");
    const FAngelscriptManager::FFilenamePair ReaderFile{ReaderPath, ReaderRelative};
    if (!TestTrue(TEXT("A fresh AS reader source is written after the type swap"), FFileHelper::SaveStringToFile(ReaderSource, *ReaderPath))) return false;
    Manager.FileChangesDetectedForReload.AddUnique(ReaderFile);
    for (int32 Attempt = 0; Attempt < 40 && Manager.FileChangesDetectedForReload.Contains(ReaderFile); ++Attempt)
    {
        Manager.CheckForHotReload(ECompileType::FullReload);
        if (Manager.FileChangesDetectedForReload.Contains(ReaderFile)) FPlatformProcess::Sleep(0.025f);
    }
    const auto ReaderDesc = Manager.GetClass(TEXT("UInstancedStructCdoPersistReader"));
    if (!TestTrue(TEXT("The current AS reader class exists"), ReaderDesc.IsValid() && ReaderDesc->Class && ReaderDesc->ScriptType)) return false;
    TStrongObjectPtr<UInstancedStructReloadTestObject> Instance(NewObject<UInstancedStructReloadTestObject>(
        GetTransientPackage(), Blueprint->GeneratedClass));
    const UScriptStruct* InstanceType = Instance->Reflected.GetScriptStruct();
    const FIntProperty* InstanceValue = InstanceType ? FindFProperty<FIntProperty>(InstanceType, TEXT("Value")) : nullptr;
    const int32 Value = InstanceValue ? InstanceValue->GetPropertyValue_InContainer(Instance->Reflected.GetMemory()) : INDEX_NONE;
    TestTrue(TEXT("The new Blueprint instance receives the current payload type"), InstanceType == Next);
    TestEqual(TEXT("The new instance keeps the old scalar default"), Value, 6707);
    TStrongObjectPtr<UObject> Reader(NewObject<UObject>(GetTransientPackage(), ReaderDesc->Class));
    asIScriptFunction* Function = ReaderDesc->ScriptType->GetMethodByName("Read");
    asIScriptContext* Context = Manager.GetScriptEngine()->RequestContext();
    if (!TestTrue(TEXT("The current AS reader and context resolve"), Function && Context)) return false;
    ON_SCOPE_EXIT { Context->Unprepare(); Manager.GetScriptEngine()->ReturnContext(Context); };
    const int32 Prepared = Context->Prepare(Function);
    const int32 ObjectSet = Prepared >= 0 ? Context->SetObject(Reader.Get()) : INDEX_NONE;
    const int32 ArgSet = ObjectSet >= 0 ? Context->SetArgAddress(0, &Instance->Reflected) : INDEX_NONE;
    const int32 Executed = ArgSet >= 0 ? Context->Execute() : INDEX_NONE;
    const FString Exception = Executed == asEXECUTION_EXCEPTION ? ANSI_TO_TCHAR(Context->GetExceptionString()) : TEXT("");
    UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_PERSIST_INSTANCE old_type=%d new_type=%d value=%d as_execute=%d exception=%s"),
        InstanceType == Old, InstanceType == Next, Value, Executed, *Exception);
    TestEqual(TEXT("Current AS code reads the new instance payload"), Executed, int32(asEXECUTION_FINISHED));
    if (Executed == asEXECUTION_FINISHED)
        TestEqual(TEXT("The AS read returns the preserved scalar"), int32(Context->GetReturnDWord()), 6707);
    Context->Unprepare();

    const UScriptStruct* DefaultType = Owner->Reflected.GetScriptStruct();
    if (!TestNotNull(TEXT("The default payload is still populated before saving"), DefaultType)) return false;
    FindFProperty<FIntProperty>(DefaultType, TEXT("Value"))->SetPropertyValue_InContainer(Owner->Reflected.GetMutableMemory(), 6713);
    Package->MarkPackageDirty();
    const bool SavedAfter = UPackage::SavePackage(Package.Get(), Blueprint.Get(), *Filename, Args);
    const bool DiskUnchanged = FMD5Hash::HashFile(*Filename) == BeforeHash;
    UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_PERSIST_SAVE saved=%d disk_unchanged=%d default_type=%s edited_value=6713"),
        SavedAfter, DiskUnchanged, *GetPathNameSafe(DefaultType));
    TestTrue(TEXT("The edited Blueprint saves after reference payload reload"), SavedAfter);
    if (TestEqual(TEXT("The saved bytes are copied for a same-session disk reload"),
        IFileManager::Get().Copy(*PersistFilename(PersistReloadName), *Filename), uint32(COPY_OK)))
        CheckPersistedBlueprint(*this, PersistReloadName, 6713, TEXT("same-session-saved-copy"));
    // Keep the candidate source and disk checkpoints for the fresh-editor reader.
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructReferenceCdoPersistReadTest,
    "SoloAS.InstancedStructReferenceCdoPersistRead", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInstancedStructReferenceCdoPersistReadTest::RunTest(const FString&)
{
    const auto Current = FAngelscriptManager::Get().GetClass(PersistStructName);
    if (!TestTrue(TEXT("The fresh editor compiles the persisted candidate AS source"), Current.IsValid() && Current->Struct
        && FindFProperty<FIntProperty>(Current->Struct, TEXT("AddedValue")))) return false;
    CheckPersistedBlueprint(*this, PersistBaselineName, 6707, TEXT("fresh-editor-unsaved-checkpoint"));
    CheckPersistedBlueprint(*this, PersistPackageName, 6713, TEXT("fresh-editor-save-attempt"));
    return true;
}

namespace
{
bool EngineHasModule(const asIScriptModule* Module)
{
    asIScriptEngine* Engine = FAngelscriptManager::Get().GetScriptEngine();
    for (asUINT Index = 0; Index < Engine->GetModuleCount(); ++Index)
        if (Engine->GetModuleByIndex(Index) == Module) return true;
    return false;
}

// Values that outlive a reload are destroyed later by GC, not by the test. 0: a Blueprint default retired
// by a Blueprint compile keeps a payload of a struct that a full reload then replaces; 1: a native owner keeps
// an unreflected payload of such a struct; 2: a soft reload keeps the struct and gives it a new script type.
bool RunReloadedPayloadGc(FAutomationTestBase& Test, int32 Mode)
{
    FAngelscriptManager& Manager = FAngelscriptManager::Get();
    const FString StructName = FString::Printf(TEXT("FInstancedStructGcPayload_%d"), Mode);
    const FString Relative = StructName + TEXT(".as");
    const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FAngelscriptManager::GetScriptRootDirectory(), Relative));
    const FAngelscriptManager::FFilenamePair File{Path, Relative};
    const FString Initial = FString(TEXT("#if EDITOR\nstruct NAME\n{\n    UPROPERTY()\n    int Value = 1;\n    ~NAME() { UInstancedStructReloadTestObject::RecordPayloadDestruction(1, Value); }\n}\n#endif\n"))
        .Replace(TEXT("NAME"), *StructName);
    // Every candidate destructor records generation 2. Only the full reload candidates change the layout.
    FString Candidate = Initial.Replace(TEXT("RecordPayloadDestruction(1,"), TEXT("RecordPayloadDestruction(2,"));
    if (Mode != 2) Candidate.ReplaceInline(TEXT("    UPROPERTY()\n    int Value = 1;\n"), TEXT("    UPROPERTY()\n    int AddedValue = 421067;\n    UPROPERTY()\n    int Value = 1;\n"));
    if (!Test.TestFalse(TEXT("The temporary AS module is initially absent"), FPaths::FileExists(Path))) return false;
    auto Compile = [&]()
    {
        Manager.FileChangesDetectedForReload.AddUnique(File);
        for (int32 Attempt = 0; Attempt < 40 && Manager.FileChangesDetectedForReload.Contains(File); ++Attempt)
        {
            Manager.CheckForHotReload(ECompileType::FullReload);
            if (Manager.FileChangesDetectedForReload.Contains(File)) FPlatformProcess::Sleep(0.025f);
        }
        return !Manager.FileChangesDetectedForReload.Contains(File);
    };
    TStrongObjectPtr<UInstancedStructReloadTestObject> Owner;
    TStrongObjectPtr<UBlueprint> Blueprint;
    TStrongObjectPtr<UPackage> Package;
    ON_SCOPE_EXIT
    {
        // Only the current payload remains for the deleting reload; the GC above owned the old one.
        if (Blueprint.IsValid() && Blueprint->GeneratedClass)
            CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject())->Reflected.Reset();
        if (Owner.IsValid()) { Owner->Reflected.Reset(); Owner->Hidden.Reset(); }
        IFileManager::Get().Delete(*Path);
        Manager.FileChangesDetectedForReload.Remove(File);
        Manager.FileDeletionsDetectedForReload.AddUnique(File);
        for (int32 Attempt = 0; Attempt < 40 && Manager.FileDeletionsDetectedForReload.Contains(File); ++Attempt)
        {
            Manager.CheckForHotReload(ECompileType::FullReload);
            if (Manager.FileDeletionsDetectedForReload.Contains(File)) FPlatformProcess::Sleep(0.025f);
        }
        Test.TestFalse(TEXT("The temporary AS source is removed"), FPaths::FileExists(Path));
    };
    if (!Test.TestTrue(TEXT("The initial AS module compiles"), FFileHelper::SaveStringToFile(Initial, *Path) && Compile())) return false;
    const auto OldDesc = Manager.GetClass(StructName);
    UASStruct* Old = OldDesc.IsValid() ? Cast<UASStruct>(OldDesc->Struct) : nullptr;
    if (!Test.TestTrue(TEXT("The initial struct is a generated AS type with a script module"),
        Old && Old->ScriptType && Old->ScriptType->GetModule())) return false;
    TStrongObjectPtr<UASStruct> OldPin(Old);
    const asIScriptModule* const OldModule = Old->ScriptType->GetModule();
    const FIntProperty* const OldValue = FindFProperty<FIntProperty>(Old, TEXT("Value"));
    FInstancedStruct* Slot = nullptr;
    if (Mode == 0)
    {
        Package.Reset(CreatePackage(*FString::Printf(TEXT("/Temp/InstancedStructGcPayload_%s"), *FGuid::NewGuid().ToString())));
        Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(UInstancedStructReloadTestObject::StaticClass(), Package.Get(),
            TEXT("GcPayloadDefault"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
        if (!Test.TestTrue(TEXT("The Blueprint default carrier is created"), Blueprint.IsValid() && Blueprint->GeneratedClass)) return false;
        Owner.Reset(CastChecked<UInstancedStructReloadTestObject>(Blueprint->GeneratedClass->GetDefaultObject()));
        Owner->Reflected.InitializeAs(Old);
        OldValue->SetPropertyValue_InContainer(Owner->Reflected.GetMutableMemory(), 5);
        // A user adds a variable of the AS struct and compiles: the previous default is retired with its payload.
        FEdGraphPinType Pin;
        Pin.PinCategory = UEdGraphSchema_K2::PC_Struct;
        Pin.PinSubCategoryObject = Old;
        if (!Test.TestTrue(TEXT("The Blueprint graph declares the AS struct"),
            FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(), TEXT("GraphPayload"), Pin))) return false;
        FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
        if (!Test.TestTrue(TEXT("The compile retires the default that holds the old payload"),
            Owner->GetClass() != Blueprint->GeneratedClass.Get() && Owner->Reflected.GetScriptStruct() == Old)) return false;
        // Only the retired default's payload carries the counted value.
        OldValue->SetPropertyValue_InContainer(Owner->Reflected.GetMutableMemory(), 6707);
        Slot = &Owner->Reflected;
    }
    else
    {
        Owner.Reset(NewObject<UInstancedStructReloadTestObject>());
        Slot = Mode == 1 ? &Owner->Hidden : &Owner->Reflected;
        Slot->InitializeAs(Old);
        OldValue->SetPropertyValue_InContainer(Slot->GetMutableMemory(), 6707);
    }
    const int32 FirstBefore = ReferenceDestructions.FindRef(1);
    const int32 SecondBefore = ReferenceDestructions.FindRef(2);
    if (!Test.TestTrue(TEXT("The changed AS module completes its reload"), FFileHelper::SaveStringToFile(Candidate, *Path) && Compile())) return false;
    const auto NextDesc = Manager.GetClass(StructName);
    UASStruct* Next = NextDesc.IsValid() ? Cast<UASStruct>(NextDesc->Struct) : nullptr;
    const bool bSameStruct = Next == Old;
    const bool bKeptOld = Slot->GetScriptStruct() == Old;
    const bool bOldModuleAfterReload = EngineHasModule(OldModule);
    const int32 FirstAfterReload = ReferenceDestructions.FindRef(1) - FirstBefore;
    UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_RELOADED_PAYLOAD_GC mode=%d phase=reload same_struct=%d kept_old=%d old_module=%d first=%d second=%d"),
        Mode, bSameStruct, bKeptOld, bOldModuleAfterReload, FirstAfterReload, ReferenceDestructions.FindRef(2) - SecondBefore);
    if (Mode == 2)
    {
        Test.TestTrue(TEXT("A destructor body change soft reloads the same struct"), bSameStruct && bKeptOld && Old->ScriptType != nullptr);
        // Existing values follow the struct to its current type, so nothing keeps the old module.
        Test.TestFalse(TEXT("The soft reload discards the old module"), bOldModuleAfterReload);
    }
    else
    {
        if (!Test.TestTrue(TEXT("The layout change replaces the struct"), Next && !bSameStruct && Old->ScriptType == nullptr)) return false;
        if (Mode == 0)
            Test.TestTrue(TEXT("The retired Blueprint default still holds the old payload after the reload"), bKeptOld);
        if (Mode == 1)
            Test.TestTrue(TEXT("The unreflected payload still holds the old value after the reload"), bKeptOld);
        // A value of the replaced type keeps the module that destroys it.
        Test.TestTrue(TEXT("The old module stays exactly while an old value is alive"), bOldModuleAfterReload == bKeptOld);
    }
    Test.TestEqual(TEXT("The reload does not destroy a value it keeps"), FirstAfterReload, bKeptOld ? 0 : 1);
    // Nothing in the test destroys the payload: releasing its owner lets GC do it.
    const TWeakObjectPtr<UInstancedStructReloadTestObject> WeakOwner(Owner.Get());
    Owner.Reset();
    Slot = nullptr;
    CollectGarbage(RF_NoFlags, true);
    const bool bCollected = !WeakOwner.IsValid();
    const int32 First = ReferenceDestructions.FindRef(1) - FirstBefore;
    const int32 Second = ReferenceDestructions.FindRef(2) - SecondBefore;
    const bool bOldModuleAfterGc = EngineHasModule(OldModule);
    UE_LOG(LogTemp, Display, TEXT("INSTANCED_STRUCT_RELOADED_PAYLOAD_GC mode=%d phase=gc collected=%d old_module=%d first=%d second=%d"),
        Mode, bCollected, bOldModuleAfterGc, First, Second);
    Test.TestTrue(TEXT("GC collects the owner of the payload"), bCollected);
    if (Mode == 2)
    {
        // As in the fork, a soft reloaded struct's values run the current destructor.
        Test.TestEqual(TEXT("The soft reloaded value does not run the discarded destructor"), First, 0);
        Test.TestEqual(TEXT("The soft reloaded value runs its current destructor exactly once"), Second, 1);
    }
    else
    {
        Test.TestEqual(TEXT("The old value runs its old destructor exactly once"), First, 1);
        Test.TestEqual(TEXT("A value of the replaced type never runs the new destructor"), Second, bKeptOld ? 0 : 1);
    }
    Test.TestFalse(TEXT("The old module is released after its last value is destroyed"), bOldModuleAfterGc);
    return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructRetiredDefaultPayloadGcTest,
    "SoloAS.InstancedStructRetiredDefaultPayloadGc", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructRetiredDefaultPayloadGcTest::RunTest(const FString&) { return RunReloadedPayloadGc(*this, 0); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructHiddenPayloadGcTest,
    "SoloAS.InstancedStructHiddenPayloadGc", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructHiddenPayloadGcTest::RunTest(const FString&) { return RunReloadedPayloadGc(*this, 1); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancedStructSoftReloadPayloadGcTest,
    "SoloAS.InstancedStructSoftReloadPayloadGc", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FInstancedStructSoftReloadPayloadGcTest::RunTest(const FString&) { return RunReloadedPayloadGc(*this, 2); }

namespace
{
// A temporary AS module for the script struct address tests; a deleting reload removes it again.
class FScriptStructAddressModule
{
public:
    FScriptStructAddressModule(FAutomationTestBase& InTest, const TCHAR* Relative)
        : Test(InTest)
        , Manager(FAngelscriptManager::Get())
        , Path(FPaths::ConvertRelativePathToFull(FPaths::Combine(FAngelscriptManager::GetScriptRootDirectory(), Relative)))
        , File{Path, Relative}
    {
    }

    ~FScriptStructAddressModule()
    {
        if (!bWritten) return;
        IFileManager::Get().Delete(*Path);
        Manager.FileChangesDetectedForReload.Remove(File);
        Manager.FileDeletionsDetectedForReload.AddUnique(File);
        for (int32 Attempt = 0; Attempt < 40 && Manager.FileDeletionsDetectedForReload.Contains(File); ++Attempt)
        {
            Manager.CheckForHotReload(ECompileType::FullReload);
            if (Manager.FileDeletionsDetectedForReload.Contains(File)) FPlatformProcess::Sleep(0.025f);
        }
        Test.TestFalse(TEXT("The temporary AS source is removed"), FPaths::FileExists(Path));
    }

    bool Compile(const FString& Source)
    {
        if (!bWritten && !Test.TestFalse(TEXT("The temporary AS module is initially absent"), FPaths::FileExists(Path))) return false;
        bWritten = true;
        if (!FFileHelper::SaveStringToFile(Source, *Path)) return false;
        Manager.FileChangesDetectedForReload.AddUnique(File);
        for (int32 Attempt = 0; Attempt < 40 && Manager.FileChangesDetectedForReload.Contains(File); ++Attempt)
        {
            Manager.CheckForHotReload(ECompileType::FullReload);
            if (Manager.FileChangesDetectedForReload.Contains(File)) FPlatformProcess::Sleep(0.025f);
        }
        return !Manager.FileChangesDetectedForReload.Contains(File);
    }

    UASStruct* Struct(const TCHAR* Name) const
    {
        const auto Desc = Manager.GetClass(Name);
        UASStruct* Result = Desc.IsValid() ? Cast<UASStruct>(Desc->Struct) : nullptr;
        return Result && Result->ScriptType ? Result : nullptr;
    }

private:
    FAutomationTestBase& Test;
    FAngelscriptManager& Manager;
    FString Path;
    FAngelscriptManager::FFilenamePair File;
    bool bWritten = false;
};

template <typename T>
T* ScriptStructMember(const UStruct* Struct, const TCHAR* Name, void* Container)
{
    const FProperty* Property = Struct ? FindFProperty<FProperty>(Struct, Name) : nullptr;
    return Property && Container ? Property->ContainerPtrToValuePtr<T>(Container) : nullptr;
}

// Script value types lay out from offset 0, so FIsrAliasOuter::Inner shares its enclosing value's address.
// The enclosing layout is much larger than FIsrAliasOther past its Inner2 member.
const TCHAR* const ScriptStructAliasSource = TEXT(
    "#if EDITOR\n"
    "struct FIsrAliasInner\n{\n"
    "    UPROPERTY(BlueprintReadWrite)\n    int X = 11;\n"
    "    UPROPERTY(BlueprintReadWrite)\n    FString S = \"inner-default\";\n"
    "    bool opEquals(const FIsrAliasInner& Other) const { return X == Other.X && S == Other.S; }\n"
    "}\n"
    "struct FIsrAliasOuter\n{\n"
    "    UPROPERTY(BlueprintReadWrite)\n    FIsrAliasInner Inner;\n"
    "    UPROPERTY(BlueprintReadWrite)\n    FString Name = \"outer-default\";\n"
    "    UPROPERTY()\n    FString PadA = \"outer-pad-a\";\n"
    "    UPROPERTY()\n    FString PadB = \"outer-pad-b\";\n"
    "    UPROPERTY()\n    FString PadC = \"outer-pad-c\";\n"
    "    UPROPERTY()\n    FString PadD = \"outer-pad-d\";\n"
    "    UPROPERTY(BlueprintReadWrite)\n    int Tail = 77;\n"
    "    ~FIsrAliasOuter() { UInstancedStructReloadTestObject::RecordPayloadDestruction(31, Tail); }\n"
    "}\n"
    "struct FIsrAliasOther\n{\n"
    "    UPROPERTY(BlueprintReadWrite)\n    int Head = 5;\n"
    "    UPROPERTY(BlueprintReadWrite)\n    FIsrAliasInner Inner2;\n"
    "}\n"
    "struct FIsrStaleA\n{\n"
    "    UPROPERTY()\n    int Tag = 0;\n"
    "    UPROPERTY()\n    FString A1 = \"stale-a-1\";\n"
    "    UPROPERTY()\n    FString A2 = \"stale-a-2\";\n"
    "    ~FIsrStaleA() { UInstancedStructReloadTestObject::RecordPayloadDestruction(41, Tag); }\n"
    "}\n"
    "struct FIsrStaleB\n{\n"
    "    UPROPERTY()\n    int Tag = 0;\n"
    "    ~FIsrStaleB() { UInstancedStructReloadTestObject::RecordPayloadDestruction(42, Tag); }\n"
    "}\n"
    "#endif\n");

// 0: the member is copied out of the enclosing value; 1: the member is copied into it; 2: the member is cleared in place.
bool RunScriptStructFirstMemberAlias(FAutomationTestBase& Test, int32 Scenario)
{
    FScriptStructAddressModule Module(Test, TEXT("ScriptStructAddressAlias.as"));
    if (!Test.TestTrue(TEXT("The alias AS module compiles"), Module.Compile(ScriptStructAliasSource))) return false;
    UASStruct* Inner = Module.Struct(TEXT("FIsrAliasInner"));
    UASStruct* Outer = Module.Struct(TEXT("FIsrAliasOuter"));
    UASStruct* Other = Module.Struct(TEXT("FIsrAliasOther"));
    if (!Test.TestTrue(TEXT("The alias structs are generated AS types"), Inner && Outer && Other)) return false;
    const FStructProperty* InnerMember = FindFProperty<FStructProperty>(Outer, TEXT("Inner"));
    const FStructProperty* Inner2Member = FindFProperty<FStructProperty>(Other, TEXT("Inner2"));
    if (!Test.TestTrue(TEXT("The first member shares the enclosing value's address"), InnerMember && Inner2Member
        && InnerMember->Struct == Inner && Inner2Member->Struct == Inner && InnerMember->GetOffset_ForInternal() == 0)) return false;
    if (!Test.TestTrue(TEXT("The enclosing layout reaches past the other value's member"),
        Outer->GetStructureSize() > Other->GetStructureSize() - Inner2Member->GetOffset_ForInternal())) return false;
    // Exact-size allocations: under MallocStomp2 an access past either value faults.
    void* OuterValue = FMemory::Malloc(Outer->GetStructureSize(), Outer->GetMinAlignment());
    void* OtherValue = FMemory::Malloc(Other->GetStructureSize(), Other->GetMinAlignment());
    Outer->InitializeStruct(OuterValue);
    Other->InitializeStruct(OtherValue);
    void* InnerValue = InnerMember->ContainerPtrToValuePtr<void>(OuterValue);
    void* Inner2Value = Inner2Member->ContainerPtrToValuePtr<void>(OtherValue);
    int32* X = ScriptStructMember<int32>(Inner, TEXT("X"), InnerValue);
    FString* S = ScriptStructMember<FString>(Inner, TEXT("S"), InnerValue);
    FString* Name = ScriptStructMember<FString>(Outer, TEXT("Name"), OuterValue);
    int32* Tail = ScriptStructMember<int32>(Outer, TEXT("Tail"), OuterValue);
    int32* Head = ScriptStructMember<int32>(Other, TEXT("Head"), OtherValue);
    int32* X2 = ScriptStructMember<int32>(Inner, TEXT("X"), Inner2Value);
    FString* S2 = ScriptStructMember<FString>(Inner, TEXT("S"), Inner2Value);
    const int32 OuterDestroyed = ReferenceDestructions.FindRef(31);
    if (Test.TestTrue(TEXT("The alias members are reflected"), X && S && Name && Tail && Head && X2 && S2))
    {
        *X = 101;
        *S = TEXT("alias-source");
        *Name = TEXT("keep-name");
        *Tail = 6707;
        if (Scenario == 0)
        {
            Inner->CopyScriptStruct(Inner2Value, InnerValue);
            Test.TestEqual(TEXT("The member copy writes the member value"), *X2, 101);
            Test.TestEqual(TEXT("The member copy writes the member string"), *S2, FString(TEXT("alias-source")));
            Test.TestEqual(TEXT("The member copy leaves the other value's members"), *Head, 5);
            Test.TestEqual(TEXT("The member copy leaves the enclosing value's members"), *Name, FString(TEXT("keep-name")));
            Test.TestTrue(TEXT("The member comparison uses the member type"), Inner->CompareScriptStruct(Inner2Value, InnerValue, PPF_None));
            *X2 = 102;
            Test.TestFalse(TEXT("The member comparison sees a member difference"), Inner->CompareScriptStruct(Inner2Value, InnerValue, PPF_None));
        }
        else if (Scenario == 1)
        {
            *X2 = 202;
            *S2 = TEXT("alias-read");
            Inner->CopyScriptStruct(InnerValue, Inner2Value);
            Test.TestEqual(TEXT("The member copy writes the enclosing value's member"), *X, 202);
            Test.TestEqual(TEXT("The member copy writes the enclosing value's member string"), *S, FString(TEXT("alias-read")));
            Test.TestEqual(TEXT("The member copy keeps the enclosing value's name"), *Name, FString(TEXT("keep-name")));
            Test.TestEqual(TEXT("The member copy keeps the enclosing value's tail"), *Tail, 6707);
        }
        else
        {
            Inner->ClearScriptStruct(InnerValue);
            Test.TestEqual(TEXT("Clearing the member resets the member value"), *X, 11);
            Test.TestEqual(TEXT("Clearing the member resets the member string"), *S, FString(TEXT("inner-default")));
            Test.TestEqual(TEXT("Clearing the member keeps the enclosing value's name"), *Name, FString(TEXT("keep-name")));
            Test.TestEqual(TEXT("Clearing the member keeps the enclosing value's tail"), *Tail, 6707);
            Test.TestEqual(TEXT("Clearing the member does not destroy the enclosing value"), ReferenceDestructions.FindRef(31) - OuterDestroyed, 0);
        }
    }
    Outer->DestroyStruct(OuterValue);
    Other->DestroyStruct(OtherValue);
    UE_LOG(LogTemp, Display, TEXT("SCRIPT_STRUCT_FIRST_MEMBER_ALIAS scenario=%d outer_destroyed=%d"), Scenario, ReferenceDestructions.FindRef(31) - OuterDestroyed);
    Test.TestEqual(TEXT("The enclosing value is destroyed exactly once"), ReferenceDestructions.FindRef(31) - OuterDestroyed, 1);
    FMemory::Free(OuterValue);
    FMemory::Free(OtherValue);
    return true;
}

// The standard Blueprint flow: a Break of the enclosing variable feeds Set Members of the other variable, which
// the Kismet VM runs as a struct member context copy (execStructMemberContext -> CopyScriptStruct).
bool RunScriptStructBlueprintMemberContext(FAutomationTestBase& Test)
{
    FScriptStructAddressModule Module(Test, TEXT("ScriptStructAddressAlias.as"));
    if (!Test.TestTrue(TEXT("The alias AS module compiles"), Module.Compile(ScriptStructAliasSource))) return false;
    UASStruct* Inner = Module.Struct(TEXT("FIsrAliasInner"));
    UASStruct* Outer = Module.Struct(TEXT("FIsrAliasOuter"));
    UASStruct* Other = Module.Struct(TEXT("FIsrAliasOther"));
    if (!Test.TestTrue(TEXT("The alias structs are generated AS types"), Inner && Outer && Other)) return false;
    TStrongObjectPtr<UPackage> Package(CreatePackage(*FString::Printf(TEXT("/Temp/ScriptStructAddressAlias_%s"), *FGuid::NewGuid().ToString())));
    TStrongObjectPtr<UBlueprint> Blueprint(FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(), Package.Get(),
        TEXT("ScriptStructAddressAlias"), BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
    if (!Test.TestTrue(TEXT("The Blueprint is created"), Blueprint.IsValid())) return false;
    UEdGraph* Graph = nullptr;
    TStrongObjectPtr<UObject> Instance;
    ON_SCOPE_EXIT
    {
        // Nothing of the Blueprint names the AS structs when the temporary module is deleted.
        Instance.Reset();
        if (Graph) FBlueprintEditorUtils::RemoveGraph(Blueprint.Get(), Graph);
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint.Get(), TEXT("OuterVar"));
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint.Get(), TEXT("OtherVar"));
        FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
    };
    FEdGraphPinType OuterPin;
    OuterPin.PinCategory = UEdGraphSchema_K2::PC_Struct;
    OuterPin.PinSubCategoryObject = Outer;
    FEdGraphPinType OtherPin = OuterPin;
    OtherPin.PinSubCategoryObject = Other;
    // The written variable is declared last, so a write past it leaves the object's allocation.
    if (!Test.TestTrue(TEXT("The Blueprint declares both AS struct variables"),
        FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(), TEXT("OuterVar"), OuterPin)
        && FBlueprintEditorUtils::AddMemberVariable(Blueprint.Get(), TEXT("OtherVar"), OtherPin))) return false;
    Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(), TEXT("CopyFirstMember"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint.Get(), Graph, true, nullptr);
    TArray<UK2Node_FunctionEntry*> Entries;
    Graph->GetNodesOfClass(Entries);
    FGraphNodeCreator<UK2Node_VariableGet> GetOuterCreator(*Graph);
    UK2Node_VariableGet* GetOuter = GetOuterCreator.CreateNode(false);
    GetOuter->VariableReference.SetSelfMember(TEXT("OuterVar"));
    GetOuterCreator.Finalize();
    FGraphNodeCreator<UK2Node_BreakStruct> BreakCreator(*Graph);
    UK2Node_BreakStruct* Break = BreakCreator.CreateNode(false);
    Break->StructType = Outer;
    BreakCreator.Finalize();
    FGraphNodeCreator<UK2Node_VariableGet> GetOtherCreator(*Graph);
    UK2Node_VariableGet* GetOther = GetOtherCreator.CreateNode(false);
    GetOther->VariableReference.SetSelfMember(TEXT("OtherVar"));
    GetOtherCreator.Finalize();
    FGraphNodeCreator<UK2Node_SetFieldsInStruct> SetCreator(*Graph);
    UK2Node_SetFieldsInStruct* Set = SetCreator.CreateNode(false);
    Set->StructType = Other;
    SetCreator.Finalize();
    for (FOptionalPinFromProperty& Option : Set->ShowPinForProperties)
        Option.bShowPin = Option.PropertyName == TEXT("Inner2");
    Set->ReconstructNode();
    UEdGraphPin* BreakInput = nullptr;
    for (UEdGraphPin* Pin : Break->Pins)
        if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) BreakInput = Pin;
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    auto Link = [Schema](UEdGraphPin* From, UEdGraphPin* To) { return From && To && Schema->TryCreateConnection(From, To); };
    const bool bLinked = Entries.Num() == 1
        && Link(Entries[0]->FindPin(UEdGraphSchema_K2::PN_Then), Set->FindPin(UEdGraphSchema_K2::PN_Execute))
        && Link(GetOuter->FindPin(TEXT("OuterVar")), BreakInput)
        && Link(Break->FindPin(TEXT("Inner")), Set->FindPin(TEXT("Inner2")))
        && Link(GetOther->FindPin(TEXT("OtherVar")), Set->FindPin(TEXT("StructRef")));
    if (!Test.TestTrue(TEXT("The graph breaks the enclosing variable into the other variable's member"), bLinked)) return false;
    FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
    UClass* Generated = Blueprint->GeneratedClass;
    if (!Test.TestTrue(TEXT("The Blueprint compiles"), Blueprint->Status != BS_Error && Generated)) return false;
    UFunction* Function = Generated->FindFunctionByName(TEXT("CopyFirstMember"));
    const FStructProperty* OuterVar = FindFProperty<FStructProperty>(Generated, TEXT("OuterVar"));
    const FStructProperty* OtherVar = FindFProperty<FStructProperty>(Generated, TEXT("OtherVar"));
    if (!Test.TestTrue(TEXT("The generated class has the function and both variables"), Function && OuterVar && OtherVar
        && OuterVar->Struct == Outer && OtherVar->Struct == Other)) return false;
    Instance.Reset(NewObject<UObject>(GetTransientPackage(), Generated));
    void* OuterValue = OuterVar->ContainerPtrToValuePtr<void>(Instance.Get());
    void* OtherValue = OtherVar->ContainerPtrToValuePtr<void>(Instance.Get());
    void* Inner2Value = ScriptStructMember<void>(Other, TEXT("Inner2"), OtherValue);
    int32* X = ScriptStructMember<int32>(Inner, TEXT("X"), OuterValue);
    FString* S = ScriptStructMember<FString>(Inner, TEXT("S"), OuterValue);
    FString* Name = ScriptStructMember<FString>(Outer, TEXT("Name"), OuterValue);
    int32* Tail = ScriptStructMember<int32>(Outer, TEXT("Tail"), OuterValue);
    int32* Head = ScriptStructMember<int32>(Other, TEXT("Head"), OtherValue);
    int32* X2 = ScriptStructMember<int32>(Inner, TEXT("X"), Inner2Value);
    FString* S2 = ScriptStructMember<FString>(Inner, TEXT("S"), Inner2Value);
    if (!Test.TestTrue(TEXT("The instance members are reflected"), X && S && Name && Tail && Head && X2 && S2)) return false;
    *X = 303;
    *S = TEXT("alias-blueprint");
    *Name = TEXT("keep-name");
    *Tail = 4242;
    Instance->ProcessEvent(Function, nullptr);
    UE_LOG(LogTemp, Display, TEXT("SCRIPT_STRUCT_BLUEPRINT_MEMBER_CONTEXT x2=%d head=%d name=%s"), *X2, *Head, **Name);
    Test.TestEqual(TEXT("The Blueprint writes the member value"), *X2, 303);
    Test.TestEqual(TEXT("The Blueprint writes the member string"), *S2, FString(TEXT("alias-blueprint")));
    Test.TestEqual(TEXT("The Blueprint leaves the other variable's members"), *Head, 5);
    Test.TestEqual(TEXT("The Blueprint leaves the enclosing variable's members"), *Name, FString(TEXT("keep-name")));
    Test.TestEqual(TEXT("The Blueprint leaves the enclosing variable's tail"), *Tail, 4242);
    return true;
}

// A value moved bitwise (array growth) keeps its first address registered. Another struct's value that later
// arrives at that address the same way must still run its own destructor.
bool RunScriptStructStaleAddressReuse(FAutomationTestBase& Test)
{
    FScriptStructAddressModule Module(Test, TEXT("ScriptStructAddressAlias.as"));
    if (!Test.TestTrue(TEXT("The stale address AS module compiles"), Module.Compile(ScriptStructAliasSource))) return false;
    UASStruct* A = Module.Struct(TEXT("FIsrStaleA"));
    UASStruct* B = Module.Struct(TEXT("FIsrStaleB"));
    if (!Test.TestTrue(TEXT("The stale address structs are generated AS types"), A && B)) return false;
    const int32 ASize = A->GetStructureSize();
    const int32 BSize = B->GetStructureSize();
    const FIntProperty* ATag = FindFProperty<FIntProperty>(A, TEXT("Tag"));
    const FIntProperty* BTag = FindFProperty<FIntProperty>(B, TEXT("Tag"));
    if (!Test.TestTrue(TEXT("The arriving value is smaller than the moved one"), ATag && BTag && BSize < ASize)) return false;
    void* First = FMemory::Malloc(ASize, A->GetMinAlignment());
    void* Moved = FMemory::Malloc(ASize, A->GetMinAlignment());
    void* Arriving = FMemory::Malloc(BSize, B->GetMinAlignment());
    A->InitializeStruct(First);
    ATag->SetPropertyValue_InContainer(First, 6707);
    FMemory::Memcpy(Moved, First, ASize);
    B->InitializeStruct(Arriving);
    BTag->SetPropertyValue_InContainer(Arriving, 6707);
    // The B value moves into the old address; A's moved-out members past it are cleared.
    FMemory::Memzero(First, ASize);
    FMemory::Memcpy(First, Arriving, BSize);
    ForgetAngelscriptStructScriptType(Arriving, nullptr);
    FMemory::Free(Arriving);
    const int32 ABefore = ReferenceDestructions.FindRef(41);
    const int32 BBefore = ReferenceDestructions.FindRef(42);
    B->DestroyStruct(First);
    const int32 AAfterB = ReferenceDestructions.FindRef(41) - ABefore;
    const int32 BAfterB = ReferenceDestructions.FindRef(42) - BBefore;
    const bool bStaleAfterB = GetAngelscriptStructScriptType(First) != nullptr;
    A->DestroyStruct(Moved);
    const int32 AAfterA = ReferenceDestructions.FindRef(41) - ABefore;
    UE_LOG(LogTemp, Display, TEXT("SCRIPT_STRUCT_STALE_ADDRESS a_after_b=%d b_after_b=%d stale_after_b=%d a_after_a=%d"),
        AAfterB, BAfterB, bStaleAfterB, AAfterA);
    Test.TestEqual(TEXT("The arriving value runs its own destructor"), BAfterB, 1);
    Test.TestEqual(TEXT("The arriving value does not run the moved value's destructor"), AAfterB, 0);
    Test.TestFalse(TEXT("Destroying another struct's value at the address drops the stale registration"), bStaleAfterB);
    Test.TestEqual(TEXT("The moved value runs its destructor exactly once"), AAfterA, 1);
    FMemory::Free(First);
    FMemory::Free(Moved);
    return true;
}

// Growing a script array moves its registered values bitwise; the moved-from addresses keep registrations of the
// type. A full reload then retires the type while those registrations decide how long its module stays.
bool RunScriptStructRelocatedLease(FAutomationTestBase& Test)
{
    const FString Initial = TEXT(
        "#if EDITOR\n"
        "struct FIsrRelocElem\n{\n"
        "    UPROPERTY()\n    int Value = 1;\n"
        "    UPROPERTY()\n    FString Text = \"reloc\";\n"
        "    ~FIsrRelocElem() { UInstancedStructReloadTestObject::RecordPayloadDestruction(51, Value); }\n"
        "}\n"
        "struct FIsrRelocHolder\n{\n"
        "    UPROPERTY()\n    TArray<FIsrRelocElem> Items;\n"
        "}\n"
        "#endif\n");
    FString Candidate = Initial.Replace(TEXT("RecordPayloadDestruction(51,"), TEXT("RecordPayloadDestruction(52,"));
    Candidate.ReplaceInline(TEXT("    UPROPERTY()\n    int Value = 1;\n"), TEXT("    UPROPERTY()\n    int Added = 421067;\n    UPROPERTY()\n    int Value = 1;\n"));
    FScriptStructAddressModule Module(Test, TEXT("ScriptStructAddressRelocation.as"));
    if (!Test.TestTrue(TEXT("The relocation AS module compiles"), Module.Compile(Initial))) return false;
    UASStruct* Elem = Module.Struct(TEXT("FIsrRelocElem"));
    UASStruct* Holder = Module.Struct(TEXT("FIsrRelocHolder"));
    if (!Test.TestTrue(TEXT("The relocation structs are generated AS types"), Elem && Holder && Elem->ScriptType->GetModule())) return false;
    const asIScriptModule* const OldModule = Elem->ScriptType->GetModule();
    const FString OldModulePrefix = FString(ANSI_TO_TCHAR(OldModule->GetName())) + TEXT("_OLD_");
    auto CountOldModules = [&OldModulePrefix]()
    {
        int32 Count = 0;
        asIScriptEngine* Engine = FAngelscriptManager::Get().GetScriptEngine();
        for (asUINT Index = 0; Index < Engine->GetModuleCount(); ++Index)
            Count += FString(ANSI_TO_TCHAR(Engine->GetModuleByIndex(Index)->GetName())).StartsWith(OldModulePrefix);
        return Count;
    };
    const FArrayProperty* Items = FindFProperty<FArrayProperty>(Holder, TEXT("Items"));
    const FIntProperty* Value = FindFProperty<FIntProperty>(Elem, TEXT("Value"));
    if (!Test.TestTrue(TEXT("The holder array and element value are reflected"), Items && Value)) return false;
    void* HolderValue = FMemory::Malloc(Holder->GetStructureSize(), Holder->GetMinAlignment());
    Holder->InitializeStruct(HolderValue);
    bool bHolderAlive = true;
    ON_SCOPE_EXIT
    {
        if (bHolderAlive) Holder->DestroyStruct(HolderValue);
        FMemory::Free(HolderValue);
    };
    FScriptArrayHelper Array(Items, Items->ContainerPtrToValuePtr<void>(HolderValue));
    TSet<const void*> MovedFrom;
    int32 Relocations = 0;
    for (int32 Index = 0; Index < 6; ++Index)
    {
        const void* Before = Array.Num() ? Array.GetRawPtr(0) : nullptr;
        const int32 Added = Array.AddValue();
        Value->SetPropertyValue_InContainer(Array.GetRawPtr(Added), 6707);
        if (Before && Before != Array.GetRawPtr(0)) ++Relocations;
        for (int32 Element = 0; Element < Array.Num(); ++Element) MovedFrom.Add(Array.GetRawPtr(Element));
    }
    const int32 ElementCount = Array.Num();
    int32 RegisteredInPlace = 0;
    for (int32 Element = 0; Element < Array.Num(); ++Element)
    {
        MovedFrom.Remove(Array.GetRawPtr(Element));
        RegisteredInPlace += GetAngelscriptStructScriptType(Array.GetRawPtr(Element)) != nullptr;
    }
    // Only the registry is read at moved-from addresses; their memory belongs to the allocator again.
    auto CountMovedFromRegistrations = [&MovedFrom]()
    {
        int32 Count = 0;
        for (const void* Address : MovedFrom) Count += GetAngelscriptStructScriptType(Address) != nullptr;
        return Count;
    };
    const int32 MovedFromBefore = CountMovedFromRegistrations();
    if (!Test.TestTrue(TEXT("Growing past five elements moves registered values"), Relocations > 0 && MovedFrom.Num() > 0)) return false;
    const int32 FirstBefore = ReferenceDestructions.FindRef(51);
    const int32 SecondBefore = ReferenceDestructions.FindRef(52);
    if (!Test.TestTrue(TEXT("The changed AS module completes its reload"), Module.Compile(Candidate))) return false;
    UASStruct* NextElem = Module.Struct(TEXT("FIsrRelocElem"));
    UASStruct* NextHolder = Module.Struct(TEXT("FIsrRelocHolder"));
    if (!Test.TestTrue(TEXT("The layout change replaces the element struct and its holder"), NextElem && NextElem != Elem
        && Elem->ScriptType == nullptr && NextHolder && NextHolder != Holder && Holder->ScriptType == nullptr))
    {
        // A holder that kept its struct no longer describes the old elements; leave the value alone.
        bHolderAlive = false;
        return false;
    }
    const bool bOldModuleAfterReload = OldModule && CountOldModules() > 0;
    Holder->DestroyStruct(HolderValue);
    bHolderAlive = false;
    const int32 First = ReferenceDestructions.FindRef(51) - FirstBefore;
    const int32 Second = ReferenceDestructions.FindRef(52) - SecondBefore;
    CollectGarbage(RF_NoFlags, true);
    const int32 MovedFromAfter = CountMovedFromRegistrations();
    const int32 OldModulesAfterGc = CountOldModules();
    UE_LOG(LogTemp, Display, TEXT("SCRIPT_STRUCT_RELOCATED_LEASE relocations=%d moved_from=%d registered_in_place=%d moved_from_registered_before=%d moved_from_registered_after=%d old_module_after_reload=%d old_modules_after_gc=%d first=%d second=%d"),
        Relocations, MovedFrom.Num(), RegisteredInPlace, MovedFromBefore, MovedFromAfter, bOldModuleAfterReload, OldModulesAfterGc, First, Second);
    Test.TestTrue(TEXT("The old module stays while values of the replaced type are alive"), bOldModuleAfterReload);
    // Values still registered where they live run their old destructor; moved ones may run it; none runs it twice.
    Test.TestTrue(TEXT("Each value runs its old destructor at most once, every in-place registration exactly once"),
        First >= RegisteredInPlace && First <= ElementCount);
    Test.TestEqual(TEXT("No value of the replaced type runs the new destructor"), Second, 0);
    // The module lease follows registrations of the type, not the values that moved away from them.
    Test.TestTrue(TEXT("The old module stays exactly while registrations of its type remain"), (OldModulesAfterGc > 0) == (MovedFromAfter > 0));
    return true;
}

// ClearScriptStruct on a value of a struct that a full reload retired runs the value's old destructor. Like the
// fork, the retired struct constructs nothing in its place, so destroying the value later runs no destructor again.
bool RunScriptStructRetiredClear(FAutomationTestBase& Test)
{
    const FString Initial = TEXT(
        "#if EDITOR\n"
        "struct FIsrRetiredClear\n{\n"
        "    UPROPERTY()\n    int Value = 1;\n"
        "    UPROPERTY()\n    FString Text = \"retired\";\n"
        "    ~FIsrRetiredClear() { UInstancedStructReloadTestObject::RecordPayloadDestruction(61, Value); }\n"
        "}\n"
        "#endif\n");
    FString Candidate = Initial.Replace(TEXT("RecordPayloadDestruction(61,"), TEXT("RecordPayloadDestruction(62,"));
    Candidate.ReplaceInline(TEXT("    UPROPERTY()\n    int Value = 1;\n"), TEXT("    UPROPERTY()\n    int Added = 421067;\n    UPROPERTY()\n    int Value = 1;\n"));
    FScriptStructAddressModule Module(Test, TEXT("ScriptStructAddressRetired.as"));
    if (!Test.TestTrue(TEXT("The retired clear AS module compiles"), Module.Compile(Initial))) return false;
    UASStruct* Old = Module.Struct(TEXT("FIsrRetiredClear"));
    if (!Test.TestNotNull(TEXT("The retired clear struct is a generated AS type"), Old)) return false;
    const FIntProperty* Value = FindFProperty<FIntProperty>(Old, TEXT("Value"));
    if (!Test.TestNotNull(TEXT("The retired clear value is reflected"), Value)) return false;
    void* Data = FMemory::Malloc(Old->GetStructureSize(), Old->GetMinAlignment());
    Old->InitializeStruct(Data);
    Value->SetPropertyValue_InContainer(Data, 6707);
    bool bAlive = true;
    ON_SCOPE_EXIT
    {
        if (bAlive) Old->DestroyStruct(Data);
        FMemory::Free(Data);
    };
    if (!Test.TestTrue(TEXT("The changed AS module completes its reload"), Module.Compile(Candidate))) return false;
    UASStruct* Next = Module.Struct(TEXT("FIsrRetiredClear"));
    if (!Test.TestTrue(TEXT("The layout change retires the struct"), Next && Next != Old && Old->ScriptType == nullptr)) return false;
    const int32 FirstBefore = ReferenceDestructions.FindRef(61);
    const int32 SecondBefore = ReferenceDestructions.FindRef(62);
    Old->ClearScriptStruct(Data);
    const int32 FirstAfterClear = ReferenceDestructions.FindRef(61) - FirstBefore;
    const bool bRegisteredAfterClear = GetAngelscriptStructScriptType(Data) != nullptr;
    // Whatever the clear left at the address is destroyed next; mark it so a second old destructor would count.
    Value->SetPropertyValue_InContainer(Data, 6707);
    Old->DestroyStruct(Data);
    bAlive = false;
    const int32 FirstAfterDestroy = ReferenceDestructions.FindRef(61) - FirstBefore;
    const int32 Second = ReferenceDestructions.FindRef(62) - SecondBefore;
    UE_LOG(LogTemp, Display, TEXT("SCRIPT_STRUCT_RETIRED_CLEAR clear=%d registered_after_clear=%d destroy=%d second=%d"),
        FirstAfterClear, bRegisteredAfterClear, FirstAfterDestroy, Second);
    Test.TestEqual(TEXT("Clearing the retired value runs its old destructor once"), FirstAfterClear, 1);
    Test.TestFalse(TEXT("Clearing the retired value leaves no registration of its old type"), bRegisteredAfterClear);
    Test.TestEqual(TEXT("The old destructor runs once across the clear and the destroy"), FirstAfterDestroy, 1);
    Test.TestEqual(TEXT("No value of the retired struct runs the new destructor"), Second, 0);
    return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScriptStructAddressFirstMemberCopyTest,
    "SoloAS.ScriptStructAddress.FirstMemberCopy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScriptStructAddressFirstMemberCopyTest::RunTest(const FString&) { return RunScriptStructFirstMemberAlias(*this, 0); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScriptStructAddressFirstMemberReadTest,
    "SoloAS.ScriptStructAddress.FirstMemberRead", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScriptStructAddressFirstMemberReadTest::RunTest(const FString&) { return RunScriptStructFirstMemberAlias(*this, 1); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScriptStructAddressFirstMemberClearTest,
    "SoloAS.ScriptStructAddress.FirstMemberClear", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScriptStructAddressFirstMemberClearTest::RunTest(const FString&) { return RunScriptStructFirstMemberAlias(*this, 2); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScriptStructAddressBlueprintMemberContextTest,
    "SoloAS.ScriptStructAddress.BlueprintMemberContext", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScriptStructAddressBlueprintMemberContextTest::RunTest(const FString&) { return RunScriptStructBlueprintMemberContext(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScriptStructAddressStaleReuseTest,
    "SoloAS.ScriptStructAddress.StaleReuse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScriptStructAddressStaleReuseTest::RunTest(const FString&) { return RunScriptStructStaleAddressReuse(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScriptStructAddressRelocatedLeaseTest,
    "SoloAS.ScriptStructAddress.RelocatedLease", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScriptStructAddressRelocatedLeaseTest::RunTest(const FString&) { return RunScriptStructRelocatedLease(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScriptStructAddressRetiredClearTest,
    "SoloAS.ScriptStructAddress.RetiredClear", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScriptStructAddressRetiredClearTest::RunTest(const FString&) { return RunScriptStructRetiredClear(*this); }

#endif
