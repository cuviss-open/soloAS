#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ScriptInterfacesExtension.h"
#include "Shared/ExtensionRegistration.h"
#include "Lambda/ExplicitCaptureLambdaExtension.h"

#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "AngelscriptSettings.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Interfaces/IPluginManager.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "UObject/SoftObjectPath.h"
#include "Shared/ExtensionRegistration.h"
#include "Extensions/AngelscriptExtensionHooks.h"
#include "Misc/ConfigCacheIni.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "UObject/Class.h"

namespace SoloASCoreExtensions::ScriptInterfaces
{
namespace
{
FString FindFeatureModManifest(const FString& SourceFilename);
TSet<TWeakObjectPtr<UClass>> FeatureModBlueprintClasses;

bool ReadEnabledPolicy(FConfigCacheIni& Config, const FString& IniFilename)
{
    bool bValue = true;
    if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SoloASCoreExtensions")))
    {
        FConfigFile Defaults;
        Defaults.Read(Plugin->GetBaseDir() / TEXT("Config/DefaultSoloASCoreExtensions.ini"));
        Defaults.GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableScriptInterfaces"), bValue);
    }
    if (!Config.GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableScriptInterfaces"), bValue, IniFilename))
        Config.GetBool(TEXT("SoloASExtensions"), TEXT("bEnableScriptInterfaces"), bValue, IniFilename);
    return bValue;
}
}

bool IsEnabled(const FString&)
{
    static const bool bEnabled = ReadEnabledPolicy(*GConfig, GEngineIni);
    return bEnabled;
}

namespace
{
const TArray<FString>& GetBlueprintDependencies()
{
    static const TArray<FString> Dependencies = []
    {
        TArray<FString> Values;
        GConfig->GetArray(TEXT("/Script/AngelscriptCode.AngelscriptSettings"),
            TEXT("BlueprintDependencies"), Values, GEngineIni);
        return Values;
    }();
    return Dependencies;
}

EAngelscriptExtensionResult GenerateTypes(FAngelscriptTypeGenerationContext& Context)
{
    HandleLambdaSlotReload(Context);
    if (Context.Point == EAngelscriptTypeGenerationPoint::SelectModuleTransaction
        || Context.Point == EAngelscriptTypeGenerationPoint::PrepareInstanceCommit
        || Context.Point == EAngelscriptTypeGenerationPoint::PrepareModuleCommit
        || Context.Point == EAngelscriptTypeGenerationPoint::CommitModules
        || Context.Point == EAngelscriptTypeGenerationPoint::AbortModules
        || Context.Point == EAngelscriptTypeGenerationPoint::RetainPreviousInstances) return CompleteBlueprintImports(Context);
    if (Context.Point == EAngelscriptTypeGenerationPoint::RestoreCachedTypes) return RestoreCachedInterfaces(Context);
    if (Context.Point == EAngelscriptTypeGenerationPoint::ConstructDefaultObject)
    {
        const FObjectInitializer& Initializer = *Context.ObjectInitializer;
        UObject* Object = Initializer.GetObj();
        UClass* Parent = Object->GetClass()->GetSuperClass();
        if (!Parent || !ExtensionRegistration::GetFeaturePolicy().bBlueprintInheritance
            || !Parent->HasAnyClassFlags(CLASS_CompiledFromBlueprint)
            || !FAngelscriptManager::Get().IsBlueprintDependencyClass(Parent)) return EAngelscriptExtensionResult::NotHandled;
        FScriptIntegrationObjectHelper::InitProperties(Initializer, Object, Parent, Parent->GetDefaultObject());
        return EAngelscriptExtensionResult::Handled;
    }
    if (Context.Point == EAngelscriptTypeGenerationPoint::PrepareSource) return PrepareBlueprintImports(Context);
    if (Context.Point == EAngelscriptTypeGenerationPoint::ReferenceType)
    {
        if (!IsEnabled() || !Context.ReflectionClass || !Context.ReflectionClass->HasAnyClassFlags(CLASS_Interface))
            return EAngelscriptExtensionResult::NotHandled;
        Context.bUseObjectView = true;
        return EAngelscriptExtensionResult::Handled;
    }
    const auto InterfaceResult = ConfigureInterfaces(Context);
    if (InterfaceResult != EAngelscriptExtensionResult::NotHandled) return InterfaceResult;
    if (Context.Point == EAngelscriptTypeGenerationPoint::PrepareBindings)
    {
        if (!ExtensionRegistration::GetFeaturePolicy().bBlueprintInheritance) return EAngelscriptExtensionResult::NotHandled;
        for (const FString& Name : GetBlueprintDependencies())
        {
            FString Error;
            UClass* Class = ResolveBlueprintMapping(FString(), Name, Error);
            if (Class == nullptr) continue;
            FAngelscriptManager::Get().RegisterBlueprintDependencyClass(FName(*Name), Class);
        }
        return EAngelscriptExtensionResult::Handled;
    }

    if (Context.Point != EAngelscriptTypeGenerationPoint::ResolveSuperClass)
        return EAngelscriptExtensionResult::NotHandled;
    FAngelscriptClassDesc& Desc = *Context.Class;
    if (!ExtensionRegistration::GetFeaturePolicy().bBlueprintInheritance)
    {
        if (!GetBlueprintDependencies().Contains(Desc.SuperClass))
            return EAngelscriptExtensionResult::NotHandled;
        Context.Error = TEXT("SoloASCoreExtensions Blueprint inheritance is not enabled; set [SoloASCoreExtensions] "
            "bEnableBlueprintInheritance=true and restart to inherit a Blueprint class.");
        return EAngelscriptExtensionResult::Error;
    }
    UClass* Parent = FAngelscriptManager::Get().FindBlueprintDependencyClass(FName(*Desc.SuperClass));
    if (Parent == nullptr)
    {
        if (!GetBlueprintDependencies().Contains(Desc.SuperClass))
            return EAngelscriptExtensionResult::NotHandled;
        FString Error;
        Parent = ResolveBlueprintMapping(FString(), Desc.SuperClass, Error);
        if (!Parent)
        {
            Context.Error = FString::Printf(TEXT("Blueprint dependency '%s' could not be loaded; check BlueprintBindings and the compiled asset."),
                *Desc.SuperClass);
            return EAngelscriptExtensionResult::Error;
        }
        FAngelscriptManager::Get().RegisterBlueprintDependencyClass(FName(*Desc.SuperClass), Parent);
    }
    if (Parent->HasAnyClassFlags(CLASS_Interface))
    {
        Context.Error = TEXT("An interface cannot be the UObject superclass; list interfaces after the UObject parent.");
        return EAngelscriptExtensionResult::Error;
    }
    Desc.bSuperIsCodeClass = true;
    Desc.CodeSuperClass = Parent;
    return EAngelscriptExtensionResult::Handled;
}


}
EAngelscriptExtensionResult GenerateScriptInterfaceTypes(FAngelscriptTypeGenerationContext& Context)
{
    return GenerateTypes(Context);
}
namespace
{
bool ReadFeatureMod(const FString& Filename, TSharedPtr<FJsonObject>& Out, FString& Error)
{
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Filename)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) || !Out)
    {
        Error = TEXT("FeatureMod.json could not be read as a JSON object.");
        return false;
    }
    return true;
}

FString FindFeatureModManifest(const FString& SourceFilename)
{
    FString Directory = FPaths::GetPath(SourceFilename);
    while (!Directory.IsEmpty())
    {
        const FString Candidate = Directory / TEXT("FeatureMod.json");
        if (FPaths::FileExists(Candidate)) return Candidate;
        const FString Parent = FPaths::GetPath(Directory);
        if (Parent == Directory) break;
        Directory = Parent;
    }
    return {};
}

FString FindFeatureModDependency(const FString& Manifest, const FString& Id, FString& Error)
{
    if (!FPaths::IsRelative(Id) || FPaths::GetCleanFilename(Id) != Id || Id == TEXT(".") || Id == TEXT(".."))
    {
        Error = TEXT("FeatureMod dependencies must name feature units, not filesystem paths.");
        return {};
    }
    TArray<FString> Candidates;
    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Id))
        Candidates.Add(Plugin->GetBaseDir() / TEXT("FeatureMod.json"));
    Candidates.Add(FPaths::GetPath(FPaths::GetPath(Manifest)) / Id / TEXT("FeatureMod.json"));
    Candidates.Add(FPaths::ProjectDir() / TEXT("Plugins/FeatureMods") / Id / TEXT("FeatureMod.json"));
    Candidates.Add(FPaths::ProjectDir() / TEXT("FeatureMods") / Id / TEXT("FeatureMod.json"));
    TSet<FString> Found;
    for (FString Candidate : Candidates)
    {
        Candidate = FPaths::ConvertRelativePathToFull(Candidate);
        FPaths::NormalizeFilename(Candidate);
        if (FPaths::FileExists(Candidate)) Found.Add(Candidate);
    }
    if (Found.Num() != 1)
    {
        Error = FString::Printf(TEXT("FeatureMod dependency '%s' has no unique manifest for Blueprint mapping resolution."), *Id);
        return {};
    }
    return *Found.CreateConstIterator();
}

bool FeatureModBinding(const TSharedPtr<FJsonObject>& Root, const FString& Name, FString& Path, FString& Error)
{
    const TSharedPtr<FJsonObject>* Bindings = nullptr;
    if (!Root->HasField(TEXT("BlueprintBindings"))) return false;
    if (!Root->TryGetObjectField(TEXT("BlueprintBindings"), Bindings) || !Bindings || !*Bindings)
    {
        Error = TEXT("FeatureMod BlueprintBindings must be an object of logical names and asset paths.");
        return false;
    }
    const TSharedPtr<FJsonValue> Value = (*Bindings)->TryGetField(FStringView(Name));
    if (!Value) return false;
    if (!Value->TryGetString(Path) || Path.IsEmpty())
    {
        Error = FString::Printf(TEXT("FeatureMod BlueprintBindings entry '%s' must be a nonempty asset path."), *Name);
        return false;
    }
    return true;
}

bool CollectDependencyBindings(const FString& Manifest, const TSharedPtr<FJsonObject>& Root,
    const FString& Name, TSet<FString>& Visited, TArray<FString>& Matches, FString& Error)
{
    const TSharedPtr<FJsonObject>* Dependencies = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* FeatureMods = nullptr;
    if (!Root->TryGetObjectField(TEXT("dependencies"), Dependencies)
        || !(*Dependencies)->TryGetArrayField(TEXT("featureMods"), FeatureMods)) return true;
    for (const auto& Entry : *FeatureMods)
    {
        FString Id;
        if (!Entry->TryGetString(Id) || Id.IsEmpty())
        { Error = TEXT("FeatureMod dependencies.featureMods must contain logical names."); return false; }
        const FString Filename = FindFeatureModDependency(Manifest, Id, Error);
        if (!Error.IsEmpty()) return false;
        if (Visited.Contains(Filename)) continue;
        Visited.Add(Filename);
        TSharedPtr<FJsonObject> Dependency;
        if (!ReadFeatureMod(Filename, Dependency, Error)) return false;
        FString Path;
        if (FeatureModBinding(Dependency, Name, Path, Error)) Matches.Add(Path);
        else if (!Error.IsEmpty()) return false;
        if (!CollectDependencyBindings(Filename, Dependency, Name, Visited, Matches, Error)) return false;
    }
    return true;
}
}

UClass* ResolveBlueprintMapping(const FString& SourceFilename, const FString& Name, FString& Error, const TMap<FName, FString>* ProjectBindings)
{
    FString Path;
    const FString Manifest = FindFeatureModManifest(SourceFilename);
    if (!Manifest.IsEmpty())
    {
        TSharedPtr<FJsonObject> Root;
        if (!ReadFeatureMod(Manifest, Root, Error)) return nullptr;
        if (!FeatureModBinding(Root, Name, Path, Error))
        {
            if (!Error.IsEmpty()) return nullptr;
            TArray<FString> Matches;
            TSet<FString> Visited{Manifest};
            if (!CollectDependencyBindings(Manifest, Root, Name, Visited, Matches, Error)) return nullptr;
            if (Matches.Num() > 1)
            { Error = FString::Printf(TEXT("Blueprint alias '%s' is ambiguous among dependency FeatureMods."), *Name); return nullptr; }
            if (Matches.Num() == 1) Path = Matches[0];
        }
    }
    if (Path.IsEmpty())
    {
        TArray<FString> Entries;
        if (!GConfig->GetArray(TEXT("SoloASCoreExtensions"), TEXT("BlueprintAliases"), Entries, GEngineIni))
            GConfig->GetArray(TEXT("SoloASExtensions"), TEXT("BlueprintAliases"), Entries, GEngineIni);
        for (const FString& Entry : Entries)
        {
            FString Alias, AssetPath;
            if (!FParse::Value(*Entry, TEXT("Name="), Alias) || Alias != Name) continue;
            if (!FParse::Value(*Entry, TEXT("Path="), AssetPath) || AssetPath.IsEmpty() || !Path.IsEmpty())
            { Error = FString::Printf(TEXT("BlueprintAliases requires one nonempty mapping for '%s'."), *Name); return nullptr; }
            Path = AssetPath;
        }
        if (Path.IsEmpty())
        {
            const auto& Bindings = ProjectBindings ? *ProjectBindings : FAngelscriptManager::Get().ConfigSettings->BlueprintBindings;
            if (const FString* Legacy = Bindings.Find(FName(*Name))) Path = *Legacy;
        }
    }
    if (Path.IsEmpty())
    { Error = FString::Printf(TEXT("Blueprint alias '%s' has no mapping; add BlueprintAliases or FeatureMod BlueprintBindings."), *Name); return nullptr; }
    if (!Path.Contains(TEXT("."))) Path += TEXT(".") + FPaths::GetCleanFilename(Path);
    const FString ClassPath = Path.EndsWith(TEXT("_C")) ? Path : Path + TEXT("_C");
    if (!Manifest.IsEmpty() && !RefreshFeatureModBlueprintPackage(ClassPath, Error)) return nullptr;
    if (UClass* Pending = FindPendingBlueprintClass(ClassPath))
    {
        if (!Manifest.IsEmpty() && IsEnabled(SourceFilename)) FeatureModBlueprintClasses.Add(Pending);
        return Pending;
    }
    UObject* Asset = FSoftObjectPath(ClassPath).ResolveObject();
    if (!Asset) Asset = FSoftObjectPath(ClassPath).TryLoad();
    if (!Asset) Asset = FSoftObjectPath(Path).TryLoad();
    UClass* Class = Cast<UBlueprintGeneratedClass>(Asset);
    if (const auto* Blueprint = Cast<UBlueprint>(Asset)) Class = Blueprint->GeneratedClass;
    if (!Class || !Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
        Error = FString::Printf(TEXT("Blueprint alias '%s' did not resolve to a compiled Blueprint class or interface."), *Name);
    if (Class && !Manifest.IsEmpty() && IsEnabled(SourceFilename)) FeatureModBlueprintClasses.Add(Class);
    return Class;
}

bool IsFeatureModBlueprintDependency(const FString& SourceFilename, const FString& Name, FString& Error)
{
    const FString Manifest = FindFeatureModManifest(SourceFilename);
    if (Manifest.IsEmpty()) return false;
    TSharedPtr<FJsonObject> Root;
    if (!ReadFeatureMod(Manifest, Root, Error)) return false;
    const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
    if (!Root->TryGetArrayField(TEXT("BlueprintDependencies"), Dependencies)) return false;
    for (const auto& Entry : *Dependencies)
    {
        FString Dependency;
        if (!Entry->TryGetString(Dependency))
        { Error = TEXT("FeatureMod BlueprintDependencies must contain logical names."); return false; }
        if (Dependency == Name) return true;
    }
    return false;
}

}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScriptInterfaceDefaultPolicyTest,
    "SoloAS.CoreExtensions.ScriptInterfaceDefaultPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FScriptInterfaceDefaultPolicyTest::RunTest(const FString&)
{
    using namespace SoloASCoreExtensions::ScriptInterfaces;
    FConfigCacheIni Config(EConfigCacheType::Temporary);
    const FString IniFilename = TEXT("ScriptInterfaceDefaultPolicy.ini");
    Config.Add(IniFilename, FConfigFile());
    TestTrue(TEXT("ordinary project scripts inherit the enabled code default"), ReadEnabledPolicy(Config, IniFilename));
    Config.SetBool(TEXT("SoloASExtensions"), TEXT("bEnableScriptInterfaces"), false, IniFilename);
    TestFalse(TEXT("legacy project config can disable interfaces"), ReadEnabledPolicy(Config, IniFilename));
    Config.SetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableScriptInterfaces"), true, IniFilename);
    TestTrue(TEXT("current section takes priority over the legacy alias"), ReadEnabledPolicy(Config, IniFilename));
    Config.SetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableScriptInterfaces"), false, IniFilename);
    TestFalse(TEXT("project config can disable interfaces"), ReadEnabledPolicy(Config, IniFilename));
    TestEqual(TEXT("plain project scripts share the frozen provider policy"),
        IsEnabled(FPaths::ProjectDir() / TEXT("Script/PlainProject.as")), IsEnabled());
    return true;
}
#endif

#else
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ScriptInterfacesExtension.h"
#include "AngelscriptBinds.h"
#include "AngelscriptCodeModule.h"
#include "AngelscriptManager.h"
#include "AngelscriptSettings.h"
#include "AngelscriptType.h"
#include "AngelscriptBinds/Helper_PODType.h"
#include "AngelscriptOptionalProviders.h"
#include "AngelscriptCoreExtensionPoints.h"
#include "AngelscriptExecutionQuiescence.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "Internationalization/Regex.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/GarbageCollectionSchema.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/ScriptInterface.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
namespace SoloASCoreExtensions::BlueprintInheritance { TSharedRef<FJsonObject> ExportBlueprintImportSchema(); }
namespace SoloASCoreExtensions::ScriptInterfaces
{
bool IsEnabled(const FString&)
{
    static const bool Enabled = [] { bool Value = true; if (GConfig) GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableScriptInterfaces"), Value, GEngineIni); return Value; }();
    return Enabled;
}
namespace
{
struct FMethod
{
    FString Name;
    FString ReturnType;
    TArray<FString> Arguments;
    TArray<FAngelscriptTypeUsage> ArgumentTypes;
    FAngelscriptTypeUsage Result;
    FString Body;
    FString Declaration;
    FString DefaultFunction;
    FString ModuleName;
    bool bPure = false;
};
struct FInterface
{
    TStrongObjectPtr<UClass> Class;
    FString Source;
    TArray<FMethod> Methods;
    FString ScriptName;
    FString Identity;
    FString Parent;
    FString ModuleName;
    TArray<TStrongObjectPtr<UFunction>> BoundFunctions;
};
TMap<FString, TSharedRef<FInterface>> Interfaces;
TMap<FString, TSharedRef<FInterface>> InterfaceVersions;
TMap<FString, TSharedPtr<FInterface>> PreviousInterfaces;
TSet<UFunction*> BlueprintInterfaceBoundFunctions;
FInterface* FindInterface(const FString& Name) { auto* Value = Interfaces.Find(Name); return Value ? &Value->Get() : nullptr; }
struct FInterfaceType final : TAngelscriptPODPropertyType<FInterfaceProperty>
{
    UClass* Interface;
    FString Name;
    asITypeInfo* ScriptType;
    FInterfaceType(UClass* InInterface, FString InName, asITypeInfo* InScriptType)
        : Interface(InInterface), Name(MoveTemp(InName)), ScriptType(InScriptType) {}
    FString GetAngelscriptTypeName() const override { return Name; }
    UClass* GetClass(const FAngelscriptTypeUsage&) const override { return Interface; }
    asITypeInfo* GetAngelscriptTypeInfo(const FAngelscriptTypeUsage&) const override { return ScriptType; }
    bool MatchesProperty(const FAngelscriptTypeUsage&, const FProperty* Property, EPropertyMatchType) const override
    {
        const auto* Value = CastField<FInterfaceProperty>(Property);
        return Value && Value->InterfaceClass == Interface;
    }
    FProperty* CreateProperty(const FAngelscriptTypeUsage&, const FPropertyParams& Params) const override
    {
        auto* Property = CastFieldChecked<FInterfaceProperty>(FInterfaceProperty::StaticClass()->Construct(Params.Outer, Params.PropertyName));
        Property->InterfaceClass = Interface;
        return Property;
    }
    bool NeedConstruct(const FAngelscriptTypeUsage&) const override { return true; }
    void ConstructValue(const FAngelscriptTypeUsage&, void* Address) const override { new(Address) FScriptInterface(); }
    bool HasReferences(const FAngelscriptTypeUsage&) const override { return true; }
    void EmitReferenceInfo(const FAngelscriptTypeUsage&, FGCReferenceParams& Params) const override
    {
        FScriptInterface Value;
        const SIZE_T ObjectOffset = reinterpret_cast<const uint8*>(&Value.GetObjectRef()) - reinterpret_cast<const uint8*>(&Value);
        Params.Schema->Add(UE::GC::DeclareMember(Params.Names.Top(), Params.AtOffset + ObjectOffset, UE::GC::EMemberType::Reference));
    }
    bool GetCppForm(const FAngelscriptTypeUsage&, FCppForm& Form) const override
    {
        Form.CppType = Form.CppGenericType = TEXT("FScriptInterface");
        Form.CppHeader = TEXT("UObject/ScriptInterface.h");
        return true;
    }
};

FScriptInterface MakeInterfaceValue(UObject* Object, UClass* Interface)
{
    FScriptInterface Value;
    if (Object && Object->GetClass()->ImplementsInterface(Interface))
    {
        Value.SetObject(Object);
        Value.SetInterface(Object->GetInterfaceAddress(Interface));
    }
    return Value;
}

TMap<UClass*, TSharedRef<FAngelscriptType>> ActiveInterfaceTypes;

void BindInterfaceValue(UClass* Class, const FString& Name)
{
    FBindFlags Flags;
    Flags.bPOD = true;
    auto Binds = FAngelscriptBinds::ValueClass<FScriptInterface>(Name, Flags);
    Binds.SetTypeUserData(Class);
    Binds.Constructor("void f()", [](FScriptInterface* Address) { new(Address) FScriptInterface(); });
    Binds.Constructor(TEXT("void f(const ") + Name + TEXT("& Other)"),
        [](FScriptInterface* Address, const FScriptInterface& Other) { new(Address) FScriptInterface(Other); });
    Binds.Constructor("void f(UObject Object)", [](FScriptInterface* Address, UObject* Object)
    {
        auto* Interface = static_cast<UClass*>(FAngelscriptManager::GetCurrentFunctionUserDataPtr());
        new(Address) FScriptInterface(MakeInterfaceValue(Object, Interface));
    }, Class);
    Binds.Method(Name + TEXT("& opAssign(const ") + Name + TEXT("& Other)"),
        [](FScriptInterface& Value, const FScriptInterface& Other) -> FScriptInterface& { return Value = Other; });
    Binds.Method(TEXT("bool opEquals(const ") + Name + TEXT("& Other) const"),
        [](const FScriptInterface& Value, const FScriptInterface& Other) { return Value.GetObject() == Other.GetObject(); });
    Binds.Method("bool opEquals(UObject Other) const",
        [](const FScriptInterface& Value, UObject* Other) { return Value.GetObject() == Other; });
    Binds.Method("UObject opImplCast() const", [](const FScriptInterface& Value) { return Value.GetObject(); });
    FAngelscriptBinds::BindGlobalGenericFunction(Name + TEXT(" __SoloASCast_") + Name + TEXT("(UObject Object)"),
        [](asIScriptGeneric* Generic)
        {
            auto* Interface = static_cast<UClass*>(Generic->GetFunction()->GetUserData());
            new (Generic->GetAddressOfReturnLocation()) FScriptInterface(
                MakeInterfaceValue(static_cast<UObject*>(Generic->GetArgObject(0)), Interface));
        }, Class);
    auto Type = MakeShared<FInterfaceType>(Class, Name, Binds.GetTypeInfo());
    if (FAngelscriptType::GetByClass(Class).IsValid()) FAngelscriptType::RegisterAlias(Name, Type);
    else FAngelscriptType::Register(Type);
    ActiveInterfaceTypes.Add(Class, Type);
    static const bool bFinderRegistered = []
    {
        FAngelscriptType::RegisterTypeFinder([](FProperty* Property, FAngelscriptTypeUsage& Usage)
        {
            const auto* Interface = CastField<FInterfaceProperty>(Property);
            const auto* Active = Interface ? ActiveInterfaceTypes.Find(Interface->InterfaceClass) : nullptr;
            if (!Active) return false;
            Usage.Type = *Active;
            return true;
        });
        return true;
    }();
}

FString Mask(const FString& Source)
{
    FString Result = Source;
    bool bLine = false, bBlock = false;
    TCHAR Quote = 0;
    for (int32 Index = 0; Index < Source.Len(); ++Index)
    {
        const TCHAR C = Source[Index];
        const TCHAR Next = Index + 1 < Source.Len() ? Source[Index + 1] : 0;
        if (bLine)
        {
            if (C == '\n') bLine = false;
        }
        else if (bBlock)
        {
            if (C == '*' && Next == '/') { Result[Index++] = ' '; Result[Index] = ' '; bBlock = false; continue; }
        }
        else if (Quote)
        {
            if (C == '\\') { Result[Index++] = ' '; if (Index < Source.Len()) Result[Index] = ' '; continue; }
            if (C == Quote) Quote = 0;
        }
        else if (C == '/' && Next == '/') bLine = true;
        else if (C == '/' && Next == '*') { bBlock = true; Result[Index++] = ' '; }
        else if (C == '"' || C == '\'') Quote = C;
        else continue;
        if (C != '\n' && C != '\r') Result[Index] = ' ';
    }
    return Result;
}

int32 CloseBrace(const FString& Code, int32 Open)
{
    int32 Depth = 0;
    for (int32 Index = Open; Index < Code.Len(); ++Index)
    {
        if (Code[Index] == '{') ++Depth;
        else if (Code[Index] == '}' && --Depth == 0) return Index;
    }
    return INDEX_NONE;
}

UClass* Resolve(const FString& Name)
{
    if (const FInterface* Definition = FindInterface(Name)) return Definition->Class.Get();
    const auto Type = FAngelscriptType::GetByAngelscriptTypeName(Name);
    UClass* Class = Type.IsValid() ? Type->GetClass(FAngelscriptTypeUsage::DefaultUsage) : nullptr;
    return Class && Class->HasAnyClassFlags(CLASS_Interface) ? Class : nullptr;
}

bool IsScriptIdentifier(const FString& Name)
{
    FTCHARToUTF8 Bytes(*Name);
    asUINT Length = 0;
    return FAngelscriptManager::Get().GetScriptEngine()->ParseToken(Bytes.Get(), Bytes.Length(), &Length) == asTC_IDENTIFIER
        && Length == static_cast<asUINT>(Bytes.Length());
}

bool ParseType(FString Name, FAngelscriptTypeUsage& Usage, FString& Error)
{
    Name = Name.TrimStartAndEnd();
    Usage.bIsConst = Name.RemoveFromStart(TEXT("const "));
    Usage.bIsReference = Name.Contains(TEXT("&"));
    Name = Name.Replace(TEXT("&inout"), TEXT("")).Replace(TEXT("&out"), TEXT(""))
        .Replace(TEXT("&in"), TEXT("")).Replace(TEXT("&"), TEXT("")).Replace(TEXT("*"), TEXT("")).TrimStartAndEnd();
    Usage.Type = FAngelscriptType::GetByAngelscriptTypeName(Name);
    if (!Usage.IsValid() || !Usage.CanCreateProperty())
    {
        Error = FString::Printf(TEXT("Interface signature type '%s' has no reflected property codec."), *Name);
        return false;
    }
    return true;
}

FProperty* AddParameter(UFunction* Function, const FString& Name, const FAngelscriptTypeUsage& Usage, bool bReturn)
{
    FAngelscriptType::FPropertyParams Params;
    Params.Outer = Function;
    Params.PropertyName = FName(*Name);
    FProperty* Property = Usage.CreateProperty(Params);
    Property->SetPropertyFlags(CPF_Parm);
    if (bReturn) Property->SetPropertyFlags(CPF_OutParm | CPF_ReturnParm);
    else if (Usage.bIsReference) Property->SetPropertyFlags(CPF_OutParm | CPF_ReferenceParm);
    if (Usage.bIsConst) Property->SetPropertyFlags(CPF_ConstParm);
    Function->AddCppProperty(Property);
    return Property;
}

bool ParseMethods(const FString& Source, TArray<FMethod>& Methods, FString& Error)
{
    const FString Code = Mask(Source);
    int32 Cursor = 0;
    const FRegexPattern Header(TEXT("\\s*(?:UFUNCTION\\s*\\([^)]*\\)\\s*)?([A-Za-z_][A-Za-z0-9_]*\\s*\\*?)\\s+"
        "([A-Za-z_][A-Za-z0-9_]*)\\s*\\(([^)]*)\\)\\s*([;{])"));
    while (Cursor < Code.Len())
    {
        if (Code.Mid(Cursor).TrimStartAndEnd().IsEmpty()) break;
        FRegexMatcher Match(Header, Code); Match.SetLimits(Cursor, Code.Len());
        if (!Match.FindNext() || !Code.Mid(Cursor, Match.GetMatchBeginning() - Cursor).TrimStartAndEnd().IsEmpty())
        {
            Error = TEXT("An interface may only declare reflected methods with supported property types.");
            return false;
        }
        FMethod Method;
        Method.ReturnType = Match.GetCaptureGroup(1).TrimStartAndEnd();
        Method.Name = Match.GetCaptureGroup(2);
        if (!IsScriptIdentifier(Method.Name)) { Error = TEXT("Interface methods must have valid script identifiers."); return false; }
        if (Method.ReturnType != TEXT("void") && !ParseType(Method.ReturnType, Method.Result, Error)) return false;
        const FString Prefix = Source.Mid(Match.GetMatchBeginning(), Match.GetCaptureGroupBeginning(1) - Match.GetMatchBeginning());
        const int32 OpenSpecifier = Prefix.Find(TEXT("("));
        if (OpenSpecifier != INDEX_NONE)
        {
            const int32 CloseSpecifier = Prefix.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            TArray<FString> Specifiers;
            Prefix.Mid(OpenSpecifier + 1, CloseSpecifier - OpenSpecifier - 1).ParseIntoArray(Specifiers, TEXT(","));
            for (const FString& Value : Specifiers)
            {
                const FString Specifier = Value.TrimStartAndEnd();
                if (Specifier == TEXT("BlueprintPure")) Method.bPure = true;
                else if (Specifier != TEXT("BlueprintCallable") && Specifier != TEXT("BlueprintEvent"))
                { Error = FString::Printf(TEXT("Unsupported interface UFUNCTION modifier '%s'."), *Specifier); return false; }
            }
        }
        const FString Arguments = Match.GetCaptureGroup(3).TrimStartAndEnd();
        TArray<FString> Parts;
        if (!Arguments.IsEmpty()) Arguments.ParseIntoArray(Parts, TEXT(","));
        const FRegexPattern Argument(TEXT("^\\s*((?:const\\s+)?[A-Za-z_][A-Za-z0-9_]*\\s*\\*?\\s*(?:&(?:inout|out|in)?)?)\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*$"));
        for (const FString& Part : Parts)
        {
            FRegexMatcher Arg(Argument, Part);
            if (!Arg.FindNext()) { Error = TEXT("Interface arguments must have a reflected type and a name."); return false; }
            FAngelscriptTypeUsage Usage;
            if (!ParseType(Arg.GetCaptureGroup(1), Usage, Error)) return false;
            const FString Name = Arg.GetCaptureGroup(2);
            if (!IsScriptIdentifier(Name) || Method.Arguments.Contains(Name) || (Method.Result.IsValid() && Name == TEXT("ReturnValue")))
            { Error = TEXT("Interface argument names must be unique script identifiers and cannot shadow ReturnValue."); return false; }
            Method.ArgumentTypes.Add(Usage);
            Method.Arguments.Add(Name);
        }
        Method.ReturnType = Method.Result.IsValid() ? Method.Result.GetAngelscriptDeclaration(FAngelscriptType::FunctionReturnValue) : TEXT("void");
        Method.Declaration = Method.ReturnType + TEXT(" ") + Method.Name + TEXT("(");
        for (int32 Index = 0; Index < Method.Arguments.Num(); ++Index)
        {
            if (Index) Method.Declaration += TEXT(", ");
            Method.Declaration += Method.ArgumentTypes[Index].GetAngelscriptDeclaration(FAngelscriptType::FunctionArgument)
                + TEXT(" ") + Method.Arguments[Index];
        }
        Method.Declaration += TEXT(")");
        Cursor = Match.GetMatchEnding();
        if (Match.GetCaptureGroup(4) == TEXT("{"))
        {
            const int32 Close = CloseBrace(Code, Cursor - 1);
            if (Close == INDEX_NONE) { Error = TEXT("Unclosed interface default implementation."); return false; }
            Method.Body = Source.Mid(Cursor - 1, Close - Cursor + 2);
            Cursor = Close + 1;
        }
        if (Methods.ContainsByPredicate([&](const FMethod& Other) { return Other.Name == Method.Name; }))
        { Error = TEXT("Interface method overloading requires distinct reflected names."); return false; }
        Methods.Add(MoveTemp(Method));
    }
    return true;
}

void Invoke(asIScriptGeneric* Generic)
{
    UFunction* Signature = static_cast<UFunction*>(Generic->GetFunction()->GetUserData());
    const bool bInterface = BlueprintInterfaceBoundFunctions.Contains(Signature)
        || Signature->GetOuterUClass()->HasAnyClassFlags(CLASS_Interface);
    UObject* Target = bInterface ? static_cast<FScriptInterface*>(Generic->GetObject())->GetObject()
        : static_cast<UObject*>(Generic->GetObject());
    if (!Target || (bInterface ? !Target->GetClass()->ImplementsInterface(Signature->GetOuterUClass())
        : !Target->IsA(Signature->GetOuterUClass())))
    { FAngelscriptManager::Throw("Interface call requires a non-null object implementing the interface."); return; }
    if (bInterface)
    {
        UFunction* Current = Signature->GetOuterUClass()->FindFunctionByName(Signature->GetFName());
        if (!Current || !Current->IsSignatureCompatibleWith(Signature))
        { FAngelscriptManager::Throw("The imported Blueprint interface declaration changed; reload its script consumers before calling it."); return; }
    }
    UFunction* Function = Target->FindFunction(Signature->GetFName());
    if (Function && !Function->IsSignatureCompatibleWith(Signature))
    { FAngelscriptManager::Throw("The interface implementation is missing or has a different signature."); return; }
    UFunction* Layout = Function ? Function : Signature;
    FStructOnScope Parameters(Layout);
    int32 Index = 0;
    for (TFieldIterator<FProperty> It(Layout); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
    {
        if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
        const auto Usage = FAngelscriptTypeUsage::FromParam(Generic->GetFunction(), Index);
        void* Source = Generic->GetAddressOfArg(Index++);
        if (Usage.bIsReference) Source = *static_cast<void**>(Source);
        void* Destination = It->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory());
        if (auto* Object = CastField<FObjectPropertyBase>(*It))
            Object->SetObjectPropertyValue(Destination, *static_cast<UObject**>(Source));
        else It->CopyCompleteValue(Destination, Source);
    }
    if (Function) Target->ProcessEvent(Function, Parameters.GetStructMemory());
    Index = 0;
    for (TFieldIterator<FProperty> It(Layout); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
    {
        if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
        const auto Usage = FAngelscriptTypeUsage::FromParam(Generic->GetFunction(), Index);
        if (Usage.bIsReference && !Usage.bIsConst)
        {
            void* Destination = *static_cast<void**>(Generic->GetAddressOfArg(Index));
            void* Source = It->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory());
            if (auto* Object = CastField<FObjectPropertyBase>(*It))
                *static_cast<UObject**>(Destination) = Object->GetObjectPropertyValue(Source);
            else It->CopyCompleteValue(Destination, Source);
        }
        ++Index;
    }
    if (FProperty* Return = Layout->GetReturnProperty())
    {
        void* Value = Return->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory());
        if (auto* Object = CastField<FObjectPropertyBase>(Return))
            Generic->SetReturnAddress(Object->GetObjectPropertyValue(Value));
        else if (Return->IsA<FInterfaceProperty>())
            new (Generic->GetAddressOfReturnLocation()) FScriptInterface(*static_cast<FScriptInterface*>(Value));
        else if (Return->IsA<FStructProperty>() || Return->IsA<FStrProperty>()) Generic->SetReturnObject(Value);
        else Return->CopyCompleteValue(Generic->GetAddressOfReturnLocation(), Value);
    }
}

FString InterfaceScriptName(const FString& Name, const FString& Parent, const TArray<FMethod>& Methods)
{
    FString Signature = Parent;
    if (const FInterface* Base = FindInterface(Parent)) Signature += Base->ScriptName;
    for (const FMethod& Method : Methods)
        Signature += TEXT("\n") + Method.Declaration + (Method.bPure ? TEXT(" pure") : TEXT(""))
            + (Method.Body.IsEmpty() ? TEXT(" required") : TEXT(" default"));
    return TEXT("__SoloASInterface_") + Name + TEXT("_") + FMD5::HashAnsiString(*Signature);
}

bool DescribeBlueprintMethod(UFunction* Function, FMethod& Method, FString& Error);

bool Create(const FString& Name, const FString& Parent, const FString& Source, const FString& ModuleName, TArray<FMethod> Methods, FString& Error)
{
    if (!IsScriptIdentifier(Name)) { Error = TEXT("Interface names must be valid script identifiers."); return false; }
    UClass* Super = Parent.IsEmpty() ? UInterface::StaticClass() : Resolve(Parent);
    if (!Super) { Error = FString::Printf(TEXT("Unknown parent interface '%s'. Declare its interface first."), *Parent); return false; }
    for (const FMethod& Method : Methods)
    {
        UFunction* Inherited = Super->FindFunctionByName(FName(*Method.Name));
        if (!Inherited) continue;
        int32 Arguments = 0;
        bool bTypesMatch = true;
        for (TFieldIterator<FProperty> It(Inherited); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
        {
            if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
            bTypesMatch &= Method.ArgumentTypes.IsValidIndex(Arguments)
                && Method.ArgumentTypes[Arguments].MatchesProperty(*It, FAngelscriptType::OverrideArgument);
            ++Arguments;
        }
        FProperty* Return = Inherited->GetReturnProperty();
        bTypesMatch &= Return ? Method.Result.IsValid() && Method.Result.MatchesProperty(Return, FAngelscriptType::OverrideReturnValue)
            : !Method.Result.IsValid();
        if (!bTypesMatch || Arguments != Method.Arguments.Num())
        { Error = FString::Printf(TEXT("Interface method '%s' cannot change an inherited signature."), *Method.Name); return false; }
    }
    for (FMethod& Method : Methods)
    {
        if (Method.Body.IsEmpty()) continue;
        Method.DefaultFunction = TEXT("__SoloASDefault_") + Name + TEXT("_") + Method.Name;
        Method.ModuleName = ModuleName;
    }
    const FString ScriptName = InterfaceScriptName(Name, Parent, Methods);
    if (FInterface* Existing = FindInterface(Name))
    {
        if (!Existing->Source.IsEmpty())
        {
            TSet<FString> PreviousMethods;
            for (TFieldIterator<UFunction> It(Existing->Class.Get(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
                PreviousMethods.Add(It->GetName());
            if (Existing->Parent != Parent || PreviousMethods.Num() != Methods.Num()
                || Methods.ContainsByPredicate([&](const FMethod& Method) { return !PreviousMethods.Contains(Method.Name); }))
            {
                Error = FString::Printf(TEXT("Interface '%s' changed its method set or inheritance; restart the editor to apply this change. Keeping the previous interface."), *Name);
                return false;
            }
        }
        // Body-only edits reuse the bound view, but a later compiler failure must
        // restore its declaration and defaults just like a signature edit.
        if (!PreviousInterfaces.Contains(Name))
            PreviousInterfaces.Add(Name, MakeShared<FInterface>(*Existing));
        if (Existing->ScriptName == ScriptName)
        {
            Existing->Source = Source;
            for (const FMethod& Method : Methods)
                *Existing->Methods.FindByPredicate([&](const FMethod& Other) { return Other.Name == Method.Name; }) = Method;
            TArray<FString> ChangedParents{ Name };
            for (int32 Index = 0; Index < ChangedParents.Num(); ++Index)
            {
                const FInterface* ChangedParent = FindInterface(ChangedParents[Index]);
                for (auto& Pair : Interfaces)
                {
                    FInterface& Child = Pair.Value.Get();
                    if (Child.Parent != ChangedParents[Index]) continue;
                    if (!PreviousInterfaces.Contains(Pair.Key))
                        PreviousInterfaces.Add(Pair.Key, MakeShared<FInterface>(Child));
                    for (const FMethod& Inherited : ChangedParent->Methods)
                    {
                        UFunction* Function = Child.Class->FindFunctionByName(FName(*Inherited.Name));
                        if (Function && Function->GetOuterUClass() == Child.Class.Get()) continue;
                        if (FMethod* Copy = Child.Methods.FindByPredicate([&](const FMethod& Method) { return Method.Name == Inherited.Name; }))
                            *Copy = Inherited;
                    }
                    ChangedParents.Add(Pair.Key);
                }
            }
            return true;
        }
    }
    if (!PreviousInterfaces.Contains(Name))
        PreviousInterfaces.Add(Name, Interfaces.Contains(Name) ? TSharedPtr<FInterface>(Interfaces.FindChecked(Name)) : nullptr);
    if (const auto* Version = InterfaceVersions.Find(ScriptName))
    {
        Interfaces.Add(Name, *Version);
        FAngelscriptType::RegisterAlias(Name, FAngelscriptType::GetByAngelscriptTypeName(ScriptName).ToSharedRef());
        Version->Get().Source = Source;
        for (const FMethod& Method : Methods)
            *Version->Get().Methods.FindByPredicate([&](const FMethod& Other) { return Other.Name == Method.Name; }) = Method;
        return true;
    }
    TArray<FMethod> ReflectedParentMethods;
    if (!FindInterface(Parent))
        for (TFieldIterator<UFunction> It(Super); It; ++It)
        {
            if (!It->GetOuterUClass()->HasAnyClassFlags(CLASS_Interface)
                || It->HasAnyFunctionFlags(FUNC_UbergraphFunction | FUNC_Static)
                || !It->HasAnyFunctionFlags(FUNC_BlueprintEvent | FUNC_BlueprintCallable | FUNC_BlueprintPure)) continue;
            if (Methods.ContainsByPredicate([&](const FMethod& Other) { return Other.Name == It->GetName(); })) continue;
            FMethod Method;
            if (!DescribeBlueprintMethod(*It, Method, Error)) return false;
            ReflectedParentMethods.Add(MoveTemp(Method));
        }
    const FString ReflectionName = Name.Len() > 1 && Name[0] == 'I' && FChar::IsUpper(Name[1]) ? Name.Mid(1) : Name;
    const FString CandidateName = FindInterface(Name) ? ScriptName : ReflectionName;
    if (FindObject<UClass>(FAngelscriptManager::GetPackage(), *CandidateName))
    { Error = TEXT("The reflection interface name collides with an existing class."); return false; }
    UClass* Class = NewObject<UClass>(FAngelscriptManager::GetPackage(), FName(*CandidateName), RF_Public | RF_Standalone | RF_MarkAsRootSet);
    Class->ClassFlags = CLASS_Interface | CLASS_Abstract | CLASS_CompiledFromBlueprint;
    Class->ClassCastFlags = Super->ClassCastFlags;
    Class->ClassWithin = UObject::StaticClass();
    Class->ClassConstructor = UInterface::StaticClass()->ClassConstructor;
    Class->SetSuperStruct(Super);
#if WITH_METADATA
    Class->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
    Class->SetMetaData(TEXT("IsBlueprintBase"), TEXT("true"));
    Class->SetMetaData(TEXT("DisplayName"), *FName::NameToDisplayString(ReflectionName, false));
#endif
    for (const FMethod& Method : Methods)
    {
        UFunction* Function = NewObject<UFunction>(Class, FName(*Method.Name), RF_Public);
        Function->FunctionFlags = FUNC_Public | FUNC_Event | FUNC_BlueprintCallable | FUNC_BlueprintEvent;
        if (Method.bPure) Function->FunctionFlags |= FUNC_BlueprintPure;
        if (Method.Result.IsValid()) AddParameter(Function, TEXT("ReturnValue"), Method.Result, true);
        for (int32 Index = Method.Arguments.Num() - 1; Index >= 0; --Index)
            AddParameter(Function, Method.Arguments[Index], Method.ArgumentTypes[Index], false);
        Function->Bind();
        Function->StaticLink(true);
        Function->Next = Class->Children;
        Class->Children = Function;
        Class->AddFunctionToFunctionMap(Function, Function->GetFName());
    }
    Class->Bind();
    Class->StaticLink(true);
    BindInterfaceValue(Class, ScriptName);
    FAngelscriptType::RegisterAlias(Name, FAngelscriptType::GetByAngelscriptTypeName(ScriptName).ToSharedRef());
    auto Binds = FAngelscriptBinds::ExistingClass(ScriptName);
    for (const FMethod& Method : Methods)
        Binds.GenericMethod(Method.Declaration + TEXT(" const"), &Invoke, Class->FindFunctionByName(FName(*Method.Name)));
    if (const FInterface* Base = FindInterface(Parent))
    {
        for (const FMethod& Method : Base->Methods)
        {
            if (Methods.ContainsByPredicate([&](const FMethod& Other) { return Other.Name == Method.Name; })) continue;
            Binds.GenericMethod(Method.Declaration + TEXT(" const"), &Invoke, Super->FindFunctionByName(FName(*Method.Name)));
            Methods.Add(Method);
        }
    }
    else
    {
        for (FMethod& Method : ReflectedParentMethods)
        {
            Binds.GenericMethod(Method.Declaration + TEXT(" const"), &Invoke, Super->FindFunctionByName(FName(*Method.Name)));
            Methods.Add(MoveTemp(Method));
        }
    }
    auto Definition = MakeShared<FInterface>();
    Definition->Class.Reset(Class);
    Definition->Source = Source;
    Definition->Methods = MoveTemp(Methods);
    Definition->ScriptName = ScriptName;
    Definition->Identity = Name;
    Definition->Parent = Parent;
    Definition->ModuleName = ModuleName;
    InterfaceVersions.Add(ScriptName, Definition);
    Interfaces.Add(Name, Definition);
    return true;
}

bool DescribeBlueprintMethod(UFunction* Function, FMethod& Method, FString& Error)
{
    Method.Name = Function->GetName();
    if (!IsScriptIdentifier(Method.Name))
    { Error = FString::Printf(TEXT("Blueprint interface method '%s' needs a valid script identifier."), *Method.Name); return false; }
    Method.bPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
    if (FProperty* Return = Function->GetReturnProperty()) Method.Result = FAngelscriptTypeUsage::FromProperty(Return);
    if (Function->GetReturnProperty() && !Method.Result.IsValid())
    { Error = FString::Printf(TEXT("Blueprint interface method '%s' has an unsupported result type."), *Method.Name); return false; }
    Method.ReturnType = Method.Result.IsValid() ? Method.Result.GetAngelscriptDeclaration(FAngelscriptType::FunctionReturnValue) : TEXT("void");
    Method.Declaration = Method.ReturnType + TEXT(" ") + Method.Name + TEXT("(");
    for (TFieldIterator<FProperty> Property(Function); Property && Property->HasAnyPropertyFlags(CPF_Parm); ++Property)
    {
        if (Property->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
        auto Usage = FAngelscriptTypeUsage::FromProperty(*Property);
        if (!Usage.IsValid())
        { Error = FString::Printf(TEXT("Blueprint interface argument '%s.%s' has an unsupported type."), *Method.Name, *Property->GetName()); return false; }
        Usage.bIsConst = Property->HasAnyPropertyFlags(CPF_ConstParm);
        Usage.bIsReference = Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm);
        if (!Method.Arguments.IsEmpty()) Method.Declaration += TEXT(", ");
        Method.Declaration += Usage.GetAngelscriptDeclaration(FAngelscriptType::FunctionArgument) + TEXT(" ") + Property->GetName();
        Method.Arguments.Add(Property->GetName());
        Method.ArgumentTypes.Add(Usage);
    }
    Method.Declaration += TEXT(")");
    return true;
}


FDelegateHandle SourceHandle, ProviderHandle, FailedHandle, CompiledHandle;
FDelegateHandle DependencyProviderHandle;
TMap<FString, TArray<UClass*>> ClassEdges, PreviousEdges;
bool bPending = false;
FString EdgeKey(FStringView Module, FStringView Class) { return FString(Module) + TEXT(":") + FString(Class); }
void Restore()
{
    if (!bPending) return;
    ClassEdges = MoveTemp(PreviousEdges);
    for (auto& Pair : PreviousInterfaces)
    {
        if (Pair.Value) Interfaces.Add(Pair.Key, Pair.Value.ToSharedRef());
        else Interfaces.Remove(Pair.Key);
    }
    PreviousInterfaces.Reset();
    bPending = false;
}

bool ImportInterface(const FString& Name, FString& Error)
{
    if (FindInterface(Name)) return true;
    UClass* Class = Resolve(Name);
    if (!Class)
    {
        FString MappedPath;
        TArray<FString> Mappings;
        if (!GConfig->GetArray(TEXT("SoloASCoreExtensions"), TEXT("BlueprintAliases"), Mappings, GEngineIni))
            GConfig->GetArray(TEXT("SoloASExtensions"), TEXT("BlueprintAliases"), Mappings, GEngineIni);
        for (const FString& Entry : Mappings)
        {
            FString Alias, Path;
            if (!FParse::Value(*Entry, TEXT("Name="), Alias) || Alias != Name) continue;
            if (!FParse::Value(*Entry, TEXT("Path="), Path) || Path.IsEmpty() || !MappedPath.IsEmpty())
            { Error = FString::Printf(TEXT("BlueprintAliases requires one nonempty mapping for '%s'."), *Name); return false; }
            MappedPath = Path;
        }
        const FString* Mapping = MappedPath.IsEmpty() ? nullptr : &MappedPath;
        if (Mapping)
        {
            FString Path = *Mapping;
            if (!Path.Contains(TEXT("."))) Path += TEXT(".") + FPaths::GetCleanFilename(Path);
            if (!Path.EndsWith(TEXT("_C"))) Path += TEXT("_C");
            Class = LoadObject<UClass>(nullptr, *Path);
        }
    }
    if (!Class || !Class->HasAnyClassFlags(CLASS_Interface))
    { Error = FString::Printf(TEXT("'%s' is not a reflected interface."), *Name); return false; }
    TArray<FMethod> Methods;
    for (TFieldIterator<UFunction> It(Class); It; ++It)
    {
        if (!It->GetOuterUClass()->HasAnyClassFlags(CLASS_Interface)
            || It->HasAnyFunctionFlags(FUNC_Static | FUNC_UbergraphFunction)) continue;
        FMethod Method;
        if (!DescribeBlueprintMethod(*It, Method, Error)) return false;
        Methods.Add(MoveTemp(Method));
    }
    const FString View = InterfaceScriptName(Name, FString(), Methods);
    BindInterfaceValue(Class, View);
    auto Binds = FAngelscriptBinds::ExistingClass(View);
    for (const FMethod& Method : Methods)
        Binds.GenericMethod(Method.Declaration + TEXT(" const"), &Invoke, Class->FindFunctionByName(FName(*Method.Name)));
    auto Definition = MakeShared<FInterface>();
    Definition->Class.Reset(Class);
    Definition->ScriptName = View;
    Definition->Identity = Name;
    Definition->Methods = MoveTemp(Methods);
    Interfaces.Add(Name, Definition);
    return true;
}

void CollectReloadDependencies(TConstArrayView<AngelscriptCoreExtensionPoints::FPreprocessSource> Sources,
    TArray<AngelscriptCoreExtensionPoints::FPreprocessDependency>& Out)
{
    TSet<FString> Changed;
    const FRegexPattern Header(TEXT("\\binterface\\s+([A-Za-z_][A-Za-z0-9_]*)(?:\\s*:\\s*([A-Za-z_][A-Za-z0-9_]*))?\\s*\\{"));
    for (const auto& File : Sources)
    {
        const FString Code = Mask(File.Source);
        FRegexMatcher Interface(Header, Code);
        while (Interface.FindNext())
        {
            const FString Name = Interface.GetCaptureGroup(1);
            const FInterface* Existing = FindInterface(Name);
            const int32 Close = CloseBrace(Code, Interface.GetMatchEnding() - 1);
            if (Existing && Close != INDEX_NONE
                && Existing->Source != File.Source.Mid(Interface.GetMatchBeginning(), Close - Interface.GetMatchBeginning() + 1))
                Changed.Add(Name);
        }
    }
    bool bAdded = true;
    while (bAdded)
    {
        bAdded = false;
        for (const auto& Pair : Interfaces)
            if (Changed.Contains(Pair.Value->Parent) && !Changed.Contains(Pair.Key))
            { Changed.Add(Pair.Key); bAdded = true; }
    }
    if (Changed.IsEmpty()) return;
    TSet<UClass*> ChangedClasses;
    for (const FString& Name : Changed) ChangedClasses.Add(Interfaces.FindChecked(Name)->Class.Get());
    for (const auto& Module : FAngelscriptManager::Get().GetActiveModules())
    {
        bool bImplements = false;
        const FString Prefix = Module->ModuleName + TEXT(":");
        for (const auto& Pair : ClassEdges)
            if (Pair.Key.StartsWith(Prefix) && Pair.Value.ContainsByPredicate([&](UClass* Class) { return ChangedClasses.Contains(Class); }))
            { bImplements = true; break; }
        if (!bImplements) continue;
        for (const auto& Section : Module->Code)
            if (!Sources.ContainsByPredicate([&](const auto& Source) { return Source.AbsolutePath == Section.AbsoluteFilename; })
                && !Out.ContainsByPredicate([&](const auto& File) { return File.AbsolutePath == Section.AbsoluteFilename; }))
                Out.Add({Section.RelativeFilename, Section.AbsoluteFilename});
    }
}

bool RewriteSource(FAngelscriptPreprocessor& P, FString& Error)
{
    struct FEdit { int32 Begin, End; FString Text; int32 InsertedOffset = INDEX_NONE, InsertedLength = 0; };
    auto Apply = [](FString& Source, TArray<FEdit>& Edits)
    {
        Edits.Sort([](const FEdit& A, const FEdit& B) { return A.Begin > B.Begin; });
        for (const FEdit& Edit : Edits) Source = Source.Left(Edit.Begin) + Edit.Text + Source.Mid(Edit.End);
    };
    if (!bPending) { PreviousEdges = ClassEdges; bPending = true; }
    TSet<FString> ChangedModules;
    TMap<FString, FString> OriginalSources;
    struct FDeclaration { int32 FileIndex, Begin, Close; FString Name, Parent, Source; TArray<FMethod> Methods; };
    TArray<FDeclaration> Pending;
    TSet<FString> DeclaredNames;
    for (int32 FileIndex = 0; FileIndex < P.Files.Num(); ++FileIndex)
    {
        auto& File = P.Files[FileIndex];
        OriginalSources.Add(File.RelativeFilename, File.RawCode);
        const FString Code = Mask(File.RawCode);
        FRegexMatcher Interface(FRegexPattern(TEXT("\\binterface\\s+([A-Za-z_][A-Za-z0-9_]*)(?:\\s*:\\s*([A-Za-z_][A-Za-z0-9_]*))?\\s*\\{")), Code);
        int32 PreviousClose = -1;
        while (Interface.FindNext())
        {
            if (Interface.GetMatchBeginning() <= PreviousClose) continue;
            const int32 Open = Interface.GetMatchEnding() - 1, Close = CloseBrace(Code, Open);
            if (Close == INDEX_NONE) { Error = TEXT("Unclosed interface declaration."); return false; }
            const FString Name = Interface.GetCaptureGroup(1);
            if (DeclaredNames.Contains(Name)) { Error = TEXT("An interface name is declared more than once."); return false; }
            DeclaredNames.Add(Name);
            FDeclaration Item{FileIndex, Interface.GetMatchBeginning(), Close, Name, Interface.GetCaptureGroup(2),
                File.RawCode.Mid(Interface.GetMatchBeginning(), Close - Interface.GetMatchBeginning() + 1), {}};
            if (!ParseMethods(File.RawCode.Mid(Open + 1, Close - Open - 1), Item.Methods, Error)) return false;
            Pending.Add(MoveTemp(Item));
            PreviousClose = Close;
        }
    }
    // File enumeration and class dependencies do not order interface declarations.
    // Resolve the complete declaration set before converting any implementing class.
    while (!Pending.IsEmpty())
    {
        bool Progress = false;
        for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
        {
            auto& Item = Pending[Index];
            if (!Item.Parent.IsEmpty())
            {
                if (Pending.ContainsByPredicate([&](const FDeclaration& Other) { return Other.Name == Item.Parent; })) continue;
                if (!FindInterface(Item.Parent) && !ImportInterface(Item.Parent, Error)) return false;
            }
            auto& File = P.Files[Item.FileIndex];
            if (!Create(Item.Name, Item.Parent, Item.Source, File.Module->ModuleName, MoveTemp(Item.Methods), Error)) return false;
            for (int32 Position = Item.Begin; Position <= Item.Close; ++Position)
                if (File.RawCode[Position] != '\n' && File.RawCode[Position] != '\r') File.RawCode[Position] = ' ';
            Pending.RemoveAt(Index); Progress = true;
        }
        if (!Progress) { Error = TEXT("Interface inheritance contains a cycle."); return false; }
    }
    for (auto& File : P.Files)
    {
        const FString Module = File.Module->ModuleName;
        TArray<FEdit> Edits;
        FString Code = Mask(File.RawCode);
        FRegexMatcher Class(FRegexPattern(TEXT("\\bclass\\s+([A-Za-z_][A-Za-z0-9_]*)(?:\\s*:\\s*([^{};]+))?\\s*\\{")), Code);
        for (auto It = ClassEdges.CreateIterator(); It; ++It)
            if (It.Key().StartsWith(Module + TEXT(":"))) It.RemoveCurrent();
        while (Class.FindNext())
        {
            TArray<FString> Parents; Class.GetCaptureGroup(2).ParseIntoArray(Parents, TEXT(","));
            const FString ClassName = Class.GetCaptureGroup(1);
            const FString Key = EdgeKey(Module, ClassName);
            if (const TArray<UClass*>* Previous = PreviousEdges.Find(Key))
            {
                TArray<UClass*> Current;
                for (int32 Index = 1; Index < Parents.Num(); ++Index)
                {
                    const FString Name = Parents[Index].TrimStartAndEnd();
                    if (!ImportInterface(Name, Error)) return false;
                    Current.AddUnique(Interfaces.FindChecked(Name)->Class.Get());
                }
                if (Previous->Num() != Current.Num() || Previous->ContainsByPredicate([&](UClass* Interface) { return !Current.Contains(Interface); }))
                {
                    Error = FString::Printf(TEXT("Class '%s' changed its implemented interfaces; restart the editor to apply this change. Keeping the previous class."), *ClassName);
                    return false;
                }
            }
            if (Parents.Num() < 2) continue;
            const int32 Open = Class.GetMatchEnding() - 1, Close = CloseBrace(Code, Open);
            if (Close == INDEX_NONE) { Error = TEXT("Unclosed interface implementation."); return false; }
            TArray<UClass*>& Edges = ClassEdges.Add(Key);
            FString Body = File.RawCode.Mid(Open + 1, Close - Open - 1);
            const FString OriginalBody = Mask(Body);
            FString Defaults;
            TSet<FString> Seen;
            TMap<FString, const FMethod*> DefaultOwners;
            TMap<FName, UFunction*> Signatures;
            for (int32 Index = 1; Index < Parents.Num(); ++Index)
            {
                const FString Name = Parents[Index].TrimStartAndEnd();
                if (!ImportInterface(Name, Error)) return false;
                const auto& Definition = Interfaces.FindChecked(Name).Get();
                Edges.AddUnique(Definition.Class.Get());
                for (TFieldIterator<UFunction> It(Definition.Class.Get()); It; ++It)
                {
                    if (!It->GetOuterUClass()->HasAnyClassFlags(CLASS_Interface)
                        || It->HasAnyFunctionFlags(FUNC_UbergraphFunction | FUNC_Static)
                        || !It->HasAnyFunctionFlags(FUNC_BlueprintEvent | FUNC_BlueprintCallable | FUNC_BlueprintPure)) continue;
                    if (UFunction** Previous = Signatures.Find(It->GetFName()))
                    {
                        if (!(*Previous)->IsSignatureCompatibleWith(*It))
                        { Error = TEXT("Interface methods with the same name have incompatible signatures."); return false; }
                    }
                    else Signatures.Add(It->GetFName(), *It);
                }
                for (const FMethod& Method : Definition.Methods)
                {
                    if (const FMethod* const* Previous = DefaultOwners.Find(Method.Name))
                    {
                        FRegexMatcher Explicit(FRegexPattern(TEXT("UFUNCTION\\s*\\([^)]*BlueprintOverride[^)]*\\)\\s*[^{};]+\\b") + Method.Name + TEXT("\\s*\\(")), OriginalBody);
                        if (!(*Previous)->DefaultFunction.IsEmpty() && !Method.DefaultFunction.IsEmpty()
                            && (*Previous)->DefaultFunction != Method.DefaultFunction && !Explicit.FindNext())
                        {
                            Error = FString::Printf(TEXT("Interface method '%s' has conflicting defaults; implement it explicitly with UFUNCTION(BlueprintOverride)."), *Method.Name);
                            return false;
                        }
                    }
                    else DefaultOwners.Add(Method.Name, &Method);
                    if (Seen.Contains(Method.Name)) continue;
                    Seen.Add(Method.Name);
                    FRegexMatcher Implementation(FRegexPattern(TEXT("\\b") + Method.ReturnType.Replace(TEXT("*"), TEXT("\\*")) + TEXT("\\s+") + Method.Name + TEXT("\\s*\\(")), Mask(Body));
                    if (Implementation.FindNext())
                    {
                        // Re-run the public function-macro parser below with BlueprintEvent,
                        // so ordinary calls and reflected calls share the generated wrapper.
                        const int32 MethodStart = Implementation.GetMatchBeginning();
                        int32 Boundary = MethodStart - 1;
                        while (Boundary >= 0 && Body[Boundary] != '}' && Body[Boundary] != ';' && Body[Boundary] != '{') --Boundary;
                        FString Prefix = Body.Mid(Boundary + 1, MethodStart - Boundary - 1);
                        if (Prefix.Contains(TEXT("BlueprintOverride")))
                        {
                            const int32 Where = Body.Find(TEXT("BlueprintOverride"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Boundary + 1);
                            Body = Body.Left(Where) + TEXT("BlueprintEvent") + Body.Mid(Where + 17);
                        }
                        else if (!Prefix.Contains(TEXT("UFUNCTION"))) Body.InsertAt(Boundary + 1, TEXT(" UFUNCTION(BlueprintEvent) "));
                    }
                    else if (!Method.Body.IsEmpty()) Defaults += TEXT(" UFUNCTION(BlueprintEvent) ") + Method.Declaration + TEXT(" ") + Method.Body;
                    else { Error = FString::Printf(TEXT("Class '%s' must implement interface method '%s'."), *ClassName, *Method.Name); return false; }
                }
            }
            FString Header = TEXT("class ") + ClassName + TEXT(" : ") + Parents[0].TrimStartAndEnd() + TEXT(" {");
            for (int32 Position = Class.GetMatchBeginning(); Position < Open; ++Position)
                if (File.RawCode[Position] == '\n') Header += '\n';
            Edits.Add({Class.GetMatchBeginning(), Close + 1, Header + Body + Defaults + TEXT("}"),
                Defaults.Contains(TEXT("\n")) ? Header.Len() + Body.Len() : INDEX_NONE, Defaults.Len()});
        }
        Edits.Sort([](const FEdit& A, const FEdit& B) { return A.Begin < B.Begin; });
        TArray<TPair<int32, int32>> InsertedDefaults;
        int32 Offset = 0;
        for (const FEdit& Edit : Edits)
        {
            if (Edit.InsertedOffset != INDEX_NONE)
            {
                const int32 Begin = Edit.Begin + Offset + Edit.InsertedOffset;
                InsertedDefaults.Emplace(Begin, Begin + Edit.InsertedLength);
            }
            Offset += Edit.Text.Len() - (Edit.End - Edit.Begin);
        }
        Apply(File.RawCode, Edits); Edits.Reset();
        if (!InsertedDefaults.IsEmpty())
        {
            File.SourceLines.Reset();
            File.SourceLines.Add(1);
            int32 OriginalLine = 1, Range = 0;
            for (int32 Position = 0; Position < File.RawCode.Len(); ++Position)
            {
                if (File.RawCode[Position] != '\n') continue;
                while (Range < InsertedDefaults.Num() && Position >= InsertedDefaults[Range].Value) ++Range;
                if (Range == InsertedDefaults.Num() || Position < InsertedDefaults[Range].Key) ++OriginalLine;
                File.SourceLines.Add(OriginalLine);
            }
        }
        Code = Mask(File.RawCode);
        FRegexMatcher Cast(FRegexPattern(TEXT("\\bCast\\s*<\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*>\\s*\\(")), Code);
        while (Cast.FindNext())
            if (const FInterface* Definition = FindInterface(Cast.GetCaptureGroup(1)))
                Edits.Add({Cast.GetMatchBeginning(), Cast.GetMatchEnding(), TEXT("__SoloASCast_") + Definition->ScriptName + TEXT("(")});
        FRegexMatcher Token(FRegexPattern(TEXT("\\b[A-Za-z_][A-Za-z0-9_]*\\b")), Code);
        while (Token.FindNext())
        {
            bool InCast = Edits.ContainsByPredicate([&](const FEdit& Edit) { return Token.GetMatchBeginning() >= Edit.Begin && Token.GetMatchEnding() <= Edit.End; });
            if (!InCast) if (const FInterface* Definition = FindInterface(Token.GetCaptureGroup(0)))
                Edits.Add({Token.GetMatchBeginning(), Token.GetMatchEnding(), Definition->ScriptName});
        }
        Apply(File.RawCode, Edits);
        // Existing public parsing entry points also produce the reflected descriptors.
        // Rebuild only after the complete source rewrite, before compilation consumes them.
        if (File.RawCode != OriginalSources.FindChecked(File.RelativeFilename)) ChangedModules.Add(Module);
    }
    for (const FString& Module : ChangedModules)
        for (auto& File : P.Files) if (File.Module->ModuleName == Module)
        {
            for (auto& Class : File.Module->Classes) P.PreprocessingClasses.Remove(Class->ClassName);
            File.Module->Classes.Reset(); File.Module->Enums.Reset(); File.Module->Delegates.Reset();
        }
    for (auto& File : P.Files)
    {
        if (!ChangedModules.Contains(File.Module->ModuleName)) continue;
        File.ChunkedCode.Empty(); File.GeneratedCode.Reset(); File.Delegates.Reset(); File.StaticsClass.Reset();
        P.ParseIntoChunks(File); P.DetectClasses(File);
    }
    for (auto& File : P.Files) if (ChangedModules.Contains(File.Module->ModuleName)) { P.AnalyzeClasses(File); P.ProcessMacros(File); P.ProcessDelegates(File); }
    return !P.bHasError;
}
TSharedRef<FJsonObject> ExportSchema()
{
    auto Schema = MakeShared<FJsonObject>();
    Schema->SetObjectField(TEXT("blueprintImports"), BlueprintInheritance::ExportBlueprintImportSchema());
    TArray<TSharedPtr<FJsonValue>> Declarations;
    TArray<FString> Names; Interfaces.GetKeys(Names); Names.Sort();
    for (const FString& Name : Names)
    {
        const auto& Definition = Interfaces.FindChecked(Name).Get();
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Name);
        Row->SetStringField(TEXT("parent"), Definition.Parent);
        Row->SetStringField(TEXT("source"), Definition.Source);
        Row->SetStringField(TEXT("module"), Definition.ModuleName);
        Declarations.Add(MakeShared<FJsonValueObject>(Row));
    }
    Schema->SetArrayField(TEXT("declarations"), Declarations);
    auto Implementers = MakeShared<FJsonObject>();
    TArray<FString> Keys; ClassEdges.GetKeys(Keys); Keys.Sort();
    for (const FString& Key : Keys)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (UClass* Class : ClassEdges.FindChecked(Key))
            for (const FString& Name : Names)
                if (Interfaces.FindChecked(Name)->Class.Get() == Class)
                { Values.Add(MakeShared<FJsonValueString>(Name)); break; }
        Implementers->SetArrayField(Key, Values);
    }
    Schema->SetObjectField(TEXT("implementers"), Implementers);
    return Schema;
}
bool ImportSchema(const FJsonObject& Schema, FString& Error)
{
    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    const TSharedPtr<FJsonObject>* Implementers = nullptr;
    if (!Schema.TryGetArrayField(TEXT("declarations"), Rows) || !Schema.TryGetObjectField(TEXT("implementers"), Implementers))
    { Error = TEXT("CE interface cache schema is incomplete."); return false; }
    TArray<TSharedPtr<FJsonValue>> Pending = *Rows;
    while (!Pending.IsEmpty())
    {
        bool Progress = false;
        for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
        {
            const auto& Row = Pending[Index]->AsObject();
            const FString Name = Row->GetStringField(TEXT("name")), Parent = Row->GetStringField(TEXT("parent"));
            if (!Parent.IsEmpty() && !FindInterface(Parent)) continue;
            const FString Source = Row->GetStringField(TEXT("source"));
            if (Source.IsEmpty()) { if (!ImportInterface(Name, Error)) return false; }
            else
            {
                const FString Code = Mask(Source);
                const int32 Open = Code.Find(TEXT("{")), Close = CloseBrace(Code, Open);
                TArray<FMethod> Methods;
                if (Open == INDEX_NONE || Close == INDEX_NONE || !ParseMethods(Source.Mid(Open + 1, Close - Open - 1), Methods, Error)
                    || !Create(Name, Parent, Source, Row->GetStringField(TEXT("module")), MoveTemp(Methods), Error)) return false;
            }
            Pending.RemoveAt(Index); Progress = true;
        }
        if (!Progress) { Error = TEXT("CE interface cache has an unresolved parent."); return false; }
    }
    for (const auto& Pair : (*Implementers)->Values)
    {
        TArray<UClass*> Edges;
        for (const auto& Value : Pair.Value->AsArray())
        {
            const auto* Definition = FindInterface(Value->AsString());
            if (!Definition) { Error = TEXT("CE interface cache implementer has an unknown interface."); return false; }
            Edges.Add(Definition->Class.Get());
        }
        ClassEdges.Add(FString(Pair.Key), MoveTemp(Edges));
    }
    PreviousInterfaces.Reset();
    return true;
}
AS_FORCE_LINK const FAngelscriptBinds::FBind RestoreInterfaceBindings(int32(FAngelscriptBinds::EOrder::Late) + 300, []
{
    if (!IsEnabled() || !FAngelscriptManager::Get().bUsePrecompiledData) return;
    const FString Root = FAngelscriptManager::GetScriptRootDirectory();
    if (!FPaths::FileExists(Root / TEXT("PrecompiledScript.Cache"))
        && !FPaths::FileExists(Root / TEXT("PrecompiledScript_Development.Cache"))
        && !FPaths::FileExists(Root / TEXT("PrecompiledScript_Test.Cache"))
        && !FPaths::FileExists(Root / TEXT("PrecompiledScript_Shipping.Cache"))) return;
    FString Text, Error;
    const FString Filename = FAngelscriptManager::GetScriptRootDirectory() / TEXT("CoreExtensions.packaging.json");
    TSharedPtr<FJsonObject> Manifest;
    if (!FFileHelper::LoadFileToString(Text, *Filename)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Manifest) || !Manifest)
    { UE_LOG(LogTemp, Fatal, TEXT("CE interface cache manifest missing: %s"), *Filename); return; }
    const TSharedPtr<FJsonObject>* Schema = nullptr;
    if (!Manifest->TryGetObjectField(TEXT("interfaces"), Schema) || !ImportSchema(**Schema, Error))
        UE_LOG(LogTemp, Fatal, TEXT("CE interface cache restore failed: %s"), *Error);
});

void Rewrite(FAngelscriptPreprocessor& P)
{
    FString Error;
    if (!RewriteSource(P, Error))
    {
        if (!Error.IsEmpty() && !P.Files.IsEmpty()) P.FileWideError(P.Files[0], Error);
        Restore();
    }
}
}
TSharedRef<FJsonObject> ExportInterfaceSchema() { return ExportSchema(); }
bool IsProviderRegistered() { return ProviderHandle.IsValid(); }
static FDelegateHandle DebugAliasHandle;
void Start()
{
    if (!IsEnabled() || ProviderHandle.IsValid()) return;
    FAngelscriptExecutionWriteScope Scope;
    ProviderHandle = AngelscriptCoreExtensionPoints::RegisterClassInterfaceProvider(TEXT("SoloASCoreExtensions.Interfaces"),
        [](FStringView Module, FStringView Class, TArray<UClass*>& Out, FString&)
        {
            if (const auto* Values = ClassEdges.Find(EdgeKey(Module, Class))) Out = *Values;
            return true;
        });
    check(ProviderHandle.IsValid());
    DependencyProviderHandle = AngelscriptCoreExtensionPoints::RegisterPreprocessDependencyProvider(
        TEXT("soloas.interfaces.reload-dependents/v1"), &CollectReloadDependencies);
    check(DependencyProviderHandle.IsValid());
    DebugAliasHandle = AngelscriptCoreExtensionPoints::RegisterDebugTypeAliasProvider(TEXT("soloas.class-interfaces"),
        [](TArray<AngelscriptCoreExtensionPoints::FDebugTypeAlias>& Out)
        {
            for (const auto& Entry : Interfaces)
            {
                const FInterface& Interface = Entry.Value.Get();
                Out.Add({Interface.ScriptName, Entry.Key, Interface.ModuleName, Interface.Parent});
            }
        });
    SourceHandle = FAngelscriptPreprocessor::OnProcessChunks.AddStatic(&Rewrite);
    FailedHandle = FAngelscriptCodeModule::GetReloadHadErrors().AddStatic(&Restore);
    CompiledHandle = FAngelscriptCodeModule::GetPostCompile().AddLambda([] { PreviousEdges.Reset(); PreviousInterfaces.Reset(); bPending = false; });
}
void Stop()
{
    if (!ProviderHandle.IsValid()) return;
    if (DependencyProviderHandle.IsValid())
    {
        const auto DependencyResult = AngelscriptCoreExtensionPoints::UnregisterPreprocessDependencyProvider(DependencyProviderHandle);
        check(DependencyResult == AngelscriptCoreExtensionPoints::ERemoveResult::Removed);
        DependencyProviderHandle.Reset();
    }
    if (DebugAliasHandle.IsValid())
    {
        const auto DebugResult = AngelscriptCoreExtensionPoints::UnregisterDebugTypeAliasProvider(DebugAliasHandle);
        check(DebugResult == AngelscriptCoreExtensionPoints::ERemoveResult::Removed);
        DebugAliasHandle.Reset();
    }
    FAngelscriptExecutionWriteScope Scope;
    const auto Result = AngelscriptCoreExtensionPoints::UnregisterClassInterfaceProvider(ProviderHandle);
    check(Result == AngelscriptCoreExtensionPoints::ERemoveResult::Removed);
    ProviderHandle.Reset();
    FAngelscriptPreprocessor::OnProcessChunks.Remove(SourceHandle); SourceHandle.Reset();
    FAngelscriptCodeModule::GetReloadHadErrors().Remove(FailedHandle); FailedHandle.Reset();
    FAngelscriptCodeModule::GetPostCompile().Remove(CompiledHandle); CompiledHandle.Reset();
    Restore();
    // Reflection owners remain rooted while VM method user data can refer to them.
}

}
#endif
