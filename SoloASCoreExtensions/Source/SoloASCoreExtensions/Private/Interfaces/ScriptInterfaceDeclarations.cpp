#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ScriptInterfacesExtension.h"
#include "Shared/ExtensionRegistration.h"
#include "Compat/SoloASRuntimeCompat.h"
#include "BlueprintInheritance/ScriptBlueprintGeneration.h"
#include "Core/SoloASCoreExtensionsModule.h"
#include "BlueprintInheritance/BlueprintQuickCallsExtension.h"
#include "AngelscriptBinds.h"
#include "AngelscriptCodeModule.h"
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "ClassGenerator/ASClass.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AngelscriptManager.h"
#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptType.h"
#include "AngelscriptBinds/Helper_PODType.h"
#include "UObject/GarbageCollectionSchema.h"
#include "UObject/ScriptInterface.h"
#include "UObject/Stack.h"
#include "Extensions/AngelscriptExtensionHooks.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "Internationalization/Regex.h"
#include "Misc/SecureHash.h"
#include "Misc/FileHelper.h"
#include "UObject/LinkerInstancingContext.h"
#include "Serialization/ArchiveUObject.h"
#include "Serialization/ObjectWriter.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "Misc/ScopeExit.h"
#include "Misc/CoreDelegates.h"
#include "UObject/UObjectArray.h"
#include "UObject/ObjectKey.h"
#include "UObject/GCObject.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Base64.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/Class.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/StrongObjectPtr.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/UObjectIterator.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "ComponentInstanceDataCache.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/Level.h"
#include "GameFramework/Pawn.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "ComponentReregisterContext.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/UnrealType.h"
#include "StructUtils/InstancedStruct.h"
#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "as_module.h"
#include "EndAngelscriptHeaders.h"
#if WITH_EDITOR && !IS_MONOLITHIC
#include "Editor.h"
#include "Containers/Ticker.h"
#include "Engine/Blueprint.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Self.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/ReferencerFinder.h"
#include "UObject/PackageReload.h"
#include "UObject/LinkerInstancingContext.h"
#include "PackageTools.h"
#endif

namespace SoloASCoreExtensions::ScriptInterfaces
{
namespace
{
const FName InterfacesKey(TEXT("SoloASInterfaces"));
const FName SchemaKey(TEXT("SoloASInterfaceSchema"));
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
// Definitions own strings and rooted reflection classes, never borrowed compiler state.
TMap<FString, TSharedRef<FInterface>> Interfaces;
TMap<FString, TSharedRef<FInterface>> InterfaceVersions;
TMap<FString, TSharedPtr<FInterface>> PreviousInterfaces;
// A retired BPI owner can lose CLASS_Interface while its bound VM methods live.
TSet<UFunction*> BlueprintInterfaceBoundFunctions;
void RebindBlueprintReflectedTypes(const TMap<UObject*, UObject*>& Replacements, const UPackage* PreviousPackage)
{
    TSet<FField*> Visited;
    TFunction<void(FField*)> Rebind = [&](FField* Field)
    {
        if (!Field || Visited.Contains(Field)) return;
        Visited.Add(Field);
        if (auto* Object = CastField<FObjectPropertyBase>(Field))
            if (UClass* Replacement = Cast<UClass>(Replacements.FindRef(Object->PropertyClass))) Object->PropertyClass = Replacement;
        if (auto* Class = CastField<FClassProperty>(Field))
            if (UClass* Replacement = Cast<UClass>(Replacements.FindRef(Class->MetaClass))) Class->MetaClass = Replacement;
        if (auto* Interface = CastField<FInterfaceProperty>(Field))
            if (UClass* Replacement = Cast<UClass>(Replacements.FindRef(Interface->InterfaceClass))) Interface->InterfaceClass = Replacement;
        TArray<FField*> InnerFields;
        Field->GetInnerFields(InnerFields);
        for (FField* Inner : InnerFields) Rebind(Inner);
    };
    for (TObjectIterator<UStruct> It; It; ++It)
    {
        if (It->GetOutermost() == PreviousPackage) continue;
        for (TFieldIterator<FProperty> Property(*It, EFieldIteratorFlags::ExcludeSuper); Property; ++Property) Rebind(*Property);
    }
}

struct FImportedBlueprintLifetime
{
    TStrongObjectPtr<UClass> Class;
    FString View;
    FMD5Hash DiskHash;
    TArray<FString> Abi;
    TArray<TStrongObjectPtr<UPackage>> RetiredPackages;
    TArray<TStrongObjectPtr<UClass>> RetiredClasses;
    TSet<UObject*> OwnedMetadataRoots;
};
TMap<FString, FImportedBlueprintLifetime> BlueprintClassLifetimes;

TUniquePtr<FPendingBlueprintGeneration> PendingBlueprintGeneration;
// Native views cannot unregister. Registered classes, including rejected
// candidates, remain alive for the process lifetime.
TArray<TStrongObjectPtr<UClass>> RegisteredBlueprintGenerationClasses;


FMD5Hash BlueprintPackageHash(const FString& Filename)
{
#if WITH_EDITOR && !IS_MONOLITHIC
    return FMD5Hash::HashFile(*Filename);
#else
    FMD5 Digest;
    for (const TCHAR* Extension : {TEXT("uasset"), TEXT("uexp"), TEXT("ubulk")})
    {
        const FString Part = FPaths::ChangeExtension(Filename, Extension);
        if (!FPaths::FileExists(Part)) continue;
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *Part)) return {};
        const uint64 Size = Bytes.Num();
        Digest.Update(reinterpret_cast<const uint8*>(&Size), sizeof(Size));
        Digest.Update(Bytes.GetData(), Bytes.Num());
    }
    FMD5Hash Result;
    Result.Set(Digest);
    return Result;
#endif
}

#if WITH_EDITOR && !IS_MONOLITHIC
bool CompatibleBlueprintAbi(const TArray<FString>& Previous, const TArray<FString>& Replacement)
{
    auto Group = [](const TArray<FString>& Rows)
    {
        TMap<FString, TArray<FString>> Groups;
        for (const FString& Row : Rows)
        {
            const FString Key = Row.StartsWith(TEXT("function:"))
                ? Row.Left(Row.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 9)) : TEXT("layout");
            Groups.FindOrAdd(Key).Add(Row);
        }
        return Groups;
    };
    const auto Before = Group(Previous), After = Group(Replacement);
    for (const auto& Pair : Before)
    {
        const auto* Rows = After.Find(Pair.Key);
        if (!Rows || *Rows != Pair.Value) return false;
    }
    return true;
}
#endif
TMap<FString, TMap<FString, TSharedPtr<FJsonValue>>> BlueprintImportSchemas;

void RestoreInterfaceDeclarations()
{
    for (const auto& Pair : PreviousInterfaces)
    {
        if (Pair.Value.IsValid())
        {
            Interfaces.Add(Pair.Key, Pair.Value.ToSharedRef());
            InterfaceVersions.Add(Pair.Value->ScriptName, Pair.Value.ToSharedRef());
            FAngelscriptType::RegisterAlias(Pair.Key,
                FAngelscriptType::GetByAngelscriptTypeName(Pair.Value->ScriptName).ToSharedRef());
        }
        else Interfaces.Remove(Pair.Key);
    }
    PreviousInterfaces.Reset();
}

#if WITH_EDITOR && !IS_MONOLITHIC
void CommitInterfaceDeclarations(const TArray<UObject*>& PreviousTypes);
void RefreshInterfaceBlueprints(bool bFullReload);
#endif

FDelegateHandle ReloadHadErrorsHandle;
FDelegateHandle PostCompileHandle;
#if WITH_EDITOR && !IS_MONOLITHIC
FDelegateHandle PreFullReloadHandle;
FDelegateHandle PostReloadHandle;
#endif

FInterface* FindInterface(const FString& Name)
{
    const auto* Definition = Interfaces.Find(Name);
    return Definition ? &Definition->Get() : nullptr;
}

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
    Method.ReturnType = Method.Result.GetAngelscriptDeclaration(FAngelscriptType::FunctionReturnValue);
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

bool DescribeBlueprintInterface(UClass* Class, TArray<FMethod>& Methods, FString& Signature, FString& Error)
{
    TArray<FString> Signatures;
    for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        if (It->HasAnyFunctionFlags(FUNC_UbergraphFunction | FUNC_Static)
            || !It->HasAnyFunctionFlags(FUNC_BlueprintEvent | FUNC_BlueprintCallable | FUNC_BlueprintPure)) continue;
        FMethod Method;
        if (!DescribeBlueprintMethod(*It, Method, Error)) return false;
        Signatures.Add(Method.Declaration + (Method.bPure ? TEXT(" pure") : TEXT("")));
        Methods.Add(MoveTemp(Method));
    }
    Signatures.Sort();
    Signature = FMD5::HashAnsiString(*FString::Join(Signatures, TEXT("\n")));
    return true;
}

FString ImportBlueprint(const FString& Filename, const FString& Name, FString& Error, UClass* CachedClass = nullptr)
{
    if (!IsScriptIdentifier(Name)) { Error = TEXT("Blueprint import names must be valid script identifiers."); return {}; }
    UClass* Class = CachedClass ? CachedClass : ResolveBlueprintMapping(Filename, Name, Error);
    if (!Class) return {};
    const bool bInterface = Class->HasAnyClassFlags(CLASS_Interface);
    if (bInterface ? !IsEnabled(Filename) : !ExtensionRegistration::GetFeaturePolicy().bBlueprintInheritance)
    {
        Error = bInterface
            ? TEXT("Blueprint interfaces require bEnableScriptInterfaces=true; restart after changing [SoloASCoreExtensions].")
            : TEXT("Blueprint imports require bEnableBlueprintInheritance=true; restart after changing [SoloASCoreExtensions].");
        return {};
    }
    const FString BaseTypeName = TEXT("__SoloASBlueprint_") + FMD5::HashAnsiString(*Class->GetPathName());
    FString TypeName = BaseTypeName;
    const bool bPendingGeneration = PendingBlueprintGeneration && PendingBlueprintGeneration->Candidate.Get() == Class;
    FString SourceClassPath = bPendingGeneration ? PendingBlueprintGeneration->ClassPath : Class->GetPathName();
    if (bPendingGeneration) TypeName = PendingBlueprintGeneration->View;
    else
        for (const auto& Entry : BlueprintClassLifetimes)
            if (Entry.Value.Class.Get() == Class && !Entry.Value.View.IsEmpty())
            { TypeName = Entry.Value.View; SourceClassPath = Entry.Key; break; }
    TArray<FMethod> InterfaceMethods;
    FString InterfaceSignature;
    if (Class->HasAnyClassFlags(CLASS_Interface))
    {
        if (!DescribeBlueprintInterface(Class, InterfaceMethods, InterfaceSignature, Error)) return {};
        // A BPI compile can replace the functions on the same generated UClass.
        // Give each declaration layout its own immutable VM method table.
        TypeName += TEXT("_") + InterfaceSignature;
    }
    if (!Class->HasAnyClassFlags(CLASS_Interface))
    {
        const auto ExistingType = FAngelscriptType::GetByAngelscriptTypeName(TypeName);
        if (ExistingType.IsValid() && ExistingType->GetClass(FAngelscriptTypeUsage()) != Class)
        {
#if WITH_EDITOR && !IS_MONOLITHIC
            if (!RebindAngelscriptObjectView(ExistingType->GetClass(FAngelscriptTypeUsage()), Class, Error)) return {};
            auto& Lifetime = BlueprintClassLifetimes.FindChecked(Class->GetPathName());
            Lifetime.Class.Reset(Class);
            Lifetime.Abi = BlueprintImportAbi(Class);
#else
            // Preserve the existing runtime generation policy. Package refresh
            // uses in-place body replacement; new class identities retain generation views.
            for (int32 Generation = 1; ; ++Generation)
            {
                const auto RuntimeType = FAngelscriptType::GetByAngelscriptTypeName(TypeName);
                if (!RuntimeType.IsValid() || RuntimeType->GetClass(FAngelscriptTypeUsage()) == Class) break;
                TypeName = BaseTypeName + TEXT("_") + FString::FromInt(Generation);
            }
#endif
        }
    }
    auto ImportSchema = MakeShared<FJsonObject>();
    ImportSchema->SetStringField(TEXT("name"), Name);
    ImportSchema->SetStringField(TEXT("path"), SourceClassPath);
    ImportSchema->SetStringField(TEXT("view"), TypeName);
    auto& ImportRecord = (PendingBlueprintGeneration ? PendingBlueprintGeneration->ImportSchemas : BlueprintImportSchemas)
        .FindOrAdd(Filename).FindOrAdd(Name);
    ImportRecord = MakeShared<FJsonValueObject>(ImportSchema);
    if (const auto ExistingType = FAngelscriptType::GetByAngelscriptTypeName(TypeName); ExistingType.IsValid())
    {
        if (Class->HasAnyClassFlags(CLASS_Interface)) ActiveInterfaceTypes.Add(Class, ExistingType.ToSharedRef());
        if (!Class->HasAnyClassFlags(CLASS_Interface))
            FAngelscriptManager::Get().RegisterBlueprintDependencyClass(FName(*TypeName), Class, SourceClassPath);
        if (const FInterface* Existing = FindInterface(TypeName))
        {
            TArray<FString> Signatures;
            for (const auto& Method : Existing->Methods) Signatures.Add(Method.Declaration + (Method.bPure ? TEXT(" pure") : TEXT("")));
            Signatures.Sort();
            ImportSchema->SetStringField(TEXT("signature"), FMD5::HashAnsiString(*FString::Join(Signatures, TEXT("\n"))));
        }
        return TypeName;
    }
    if (!Class->HasAnyClassFlags(CLASS_Interface))
    {
        if (const auto Existing = FAngelscriptType::GetByClass(Class))
        {
#if !WITH_EDITOR || IS_MONOLITHIC
            auto& Lifetime = BlueprintClassLifetimes.FindOrAdd(Class->GetPathName());
            if (!Lifetime.Class.IsValid())
            {
                Lifetime.Class.Reset(Class);
                Lifetime.View = Existing->GetAngelscriptTypeName();
                FString PackageFilename;
                if (FPackageName::DoesPackageExist(Class->GetOutermost()->GetName(), &PackageFilename))
                    Lifetime.DiskHash = BlueprintPackageHash(PackageFilename);
            }
#endif
            ImportSchema->SetStringField(TEXT("view"), Existing->GetAngelscriptTypeName());
            FAngelscriptManager::Get().RegisterBlueprintDependencyClass(FName(*Existing->GetAngelscriptTypeName()), Class, SourceClassPath);
            return Existing->GetAngelscriptTypeName();
        }
        if (!RegisterAngelscriptObjectView(Class, TypeName, Error)) return {};
        if (bPendingGeneration) RegisteredBlueprintGenerationClasses.Emplace(Class);
        else
        {
            auto& Lifetime = BlueprintClassLifetimes.FindOrAdd(Class->GetPathName());
            Lifetime.Class.Reset(Class);
            Lifetime.View = TypeName;
            Lifetime.Abi = BlueprintImportAbi(Class);
            FString PackageFilename;
            if (FPackageName::DoesPackageExist(Class->GetOutermost()->GetName(), &PackageFilename))
            {
    #if WITH_EDITOR && !IS_MONOLITHIC
                Lifetime.DiskHash = FMD5Hash::HashFile(*PackageFilename);
    #else
                Lifetime.DiskHash = BlueprintPackageHash(PackageFilename);
    #endif
            }
        }
        FAngelscriptManager::Get().RegisterBlueprintDependencyClass(FName(*TypeName), Class, SourceClassPath);
    }
    UClass* BoundParent = nullptr;
    if (!bInterface)
        for (UClass* Parent = Class->GetSuperClass(); Parent; Parent = Parent->GetSuperClass())
            if (FAngelscriptType::GetByClass(Parent).IsValid()) { BoundParent = Parent; break; }
    TArray<FMethod> Methods = MoveTemp(InterfaceMethods);
    for (TFieldIterator<UFunction> It(Class, bInterface ? EFieldIteratorFlags::ExcludeSuper : EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        UFunction* Function = *It;
        if (bInterface) continue;
        if (!bInterface && BoundParent && BoundParent->IsChildOf(Function->GetOuterUClass())) continue;
        if (Function == CastChecked<UBlueprintGeneratedClass>(Class)->UberGraphFunction) continue;
        if (Function->HasAnyFunctionFlags(FUNC_UbergraphFunction | FUNC_Static)
            || !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent | FUNC_BlueprintCallable | FUNC_BlueprintPure)) continue;
        FMethod Method;
        if (!DescribeBlueprintMethod(Function, Method, Error)) return {};
        Methods.Add(MoveTemp(Method));
    }
    if (bInterface) BindInterfaceValue(Class, TypeName);
    auto Binds = FAngelscriptBinds::ExistingClass(TypeName);
    if (!bInterface)
    {
        if (BoundParent)
        {
            const auto ParentType = FAngelscriptType::GetByClass(BoundParent);
            Binds.GetTypeInfo()->CopySystemType(ParentType->GetAngelscriptTypeInfo(FAngelscriptTypeUsage::DefaultUsage));
        }
        for (TFieldIterator<FProperty> It(Class); It; ++It)
        {
            FProperty* Property = *It;
            if ((BoundParent && BoundParent->IsChildOf(Property->GetOwnerClass()))
                || !Property->HasAnyPropertyFlags(CPF_BlueprintVisible)
                || (!FAngelscriptManager::bUseEditorScripts && Property->HasAnyPropertyFlags(CPF_EditorOnly))) continue;
            const auto Usage = FAngelscriptTypeUsage::FromProperty(Property);
            if (!Usage.IsValid()) continue;
            FAngelscriptType::FBindParams Params;
            Params.BindClass = &Binds;
            Params.NameOverride = Property->GetName();
            Params.bCanRead = true;
            Params.bCanWrite = !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
            if (!Usage.Type->BindProperty(Usage, Params, Property))
                Binds.Property(Usage.GetAngelscriptDeclaration(FAngelscriptType::MemberVariable)
                    + TEXT(" ") + Property->GetName(), Property->GetOffset_ForUFunction(), Params);
        }
    }
    for (const FMethod& Method : Methods)
    {
        UFunction* Function = Class->FindFunctionByName(FName(*Method.Name));
        if (bInterface) BlueprintInterfaceBoundFunctions.Add(Function);
        Binds.GenericMethod(Method.Declaration + TEXT(" const"), &Invoke, Function);
    }
    if (bInterface)
    {
        TArray<FString> Signatures;
        for (const auto& Method : Methods) Signatures.Add(Method.Declaration + (Method.bPure ? TEXT(" pure") : TEXT("")));
        Signatures.Sort();
        ImportSchema->SetStringField(TEXT("signature"), FMD5::HashAnsiString(*FString::Join(Signatures, TEXT("\n"))));
        auto Definition = MakeShared<FInterface>();
        Definition->Class.Reset(Class);
        Definition->Methods = MoveTemp(Methods);
        Definition->ScriptName = TypeName;
        Definition->Identity = SourceClassPath;
        // A rejected source edit may leave old bytecode active. Keep its original
        // signature alive even when the BPI compiler replaces the UFunctions.
        for (const FMethod& Method : Definition->Methods)
            Definition->BoundFunctions.Emplace(Class->FindFunctionByName(FName(*Method.Name)));
        Interfaces.Add(TypeName, Definition);
    }
    return TypeName;
}

bool RewriteBlueprintImports(FAngelscriptPreprocessor& Preprocessor, FString& Error, bool& bHandled)
{
    const FRegexPattern Import(TEXT("\\bimport\\s+blueprint\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*;"));
    const FRegexPattern Parent(TEXT("\\bclass\\s+[A-Za-z_][A-Za-z0-9_]*\\s*:\\s*([A-Za-z_][A-Za-z0-9_]*)"));
    const FRegexPattern Identifier(TEXT("\\b[A-Za-z_][A-Za-z0-9_]*\\b"));
    for (auto& File : Preprocessor.Files)
    {
        (PendingBlueprintGeneration ? PendingBlueprintGeneration->ImportSchemas : BlueprintImportSchemas).Remove(File.AbsoluteFilename);
        TMap<FString, FString> Aliases;
        FString Code = Mask(File.RawCode);
        FRegexMatcher Match(Import, Code);
        while (Match.FindNext())
        {
            const FString Name = Match.GetCaptureGroup(1);
            if (Aliases.Contains(Name))
            { Error = FString::Printf(TEXT("Blueprint alias '%s' is imported more than once in this file."), *Name); return false; }
            const FString Type = ImportBlueprint(File.AbsoluteFilename, Name, Error);
            if (Type.IsEmpty()) return false;
            Aliases.Add(Name, Type);
            for (int32 Position = Match.GetMatchBeginning(); Position < Match.GetMatchEnding(); ++Position)
                if (File.RawCode[Position] != '\n' && File.RawCode[Position] != '\r') File.RawCode[Position] = ' ';
        }
        FRegexMatcher Super(Parent, Code);
        while (Super.FindNext())
        {
            const FString Name = Super.GetCaptureGroup(1);
            if (Aliases.Contains(Name) || File.Module->ScriptUnit.Layer != EAngelscriptScriptLayer::Leaf) continue;
            if (!IsFeatureModBlueprintDependency(File.AbsoluteFilename, Name, Error))
            {
                if (!Error.IsEmpty()) return false;
                continue;
            }
            const FString Type = ImportBlueprint(File.AbsoluteFilename, Name, Error);
            if (Type.IsEmpty()) return false;
            Aliases.Add(Name, Type);
        }
        bHandled |= !Aliases.IsEmpty();
        Code = Mask(File.RawCode);
        FRegexMatcher Token(Identifier, Code);
        struct FEdit { int32 Begin, End; FString Text; };
        TArray<FEdit> Edits;
        while (Token.FindNext())
            if (const FString* Type = Aliases.Find(Code.Mid(Token.GetMatchBeginning(), Token.GetMatchEnding() - Token.GetMatchBeginning())))
                Edits.Add({Token.GetMatchBeginning(), Token.GetMatchEnding(), *Type});
        for (int32 Index = Edits.Num() - 1; Index >= 0; --Index)
            File.RawCode = File.RawCode.Left(Edits[Index].Begin) + Edits[Index].Text + File.RawCode.Mid(Edits[Index].End);
    }
    return true;
}

#if WITH_EDITOR && !IS_MONOLITHIC
bool CollectAddedBlueprintMethods(UClass* Candidate, const FImportedBlueprintLifetime& Record,
    const TMap<UObject*, UObject*>& ReboundTypes, TArray<FMethod>& Added, FString& Error)
{
    auto* Info = FAngelscriptType::GetByAngelscriptTypeName(Record.View)->GetAngelscriptTypeInfo(FAngelscriptTypeUsage());
    for (TFieldIterator<UFunction> It(Candidate); It; ++It)
    {
        UFunction* Function = *It;
        const FString Name = Function->GetName(), Prefix = TEXT("function:") + Name + TEXT(":");
        if (Function->HasAnyFunctionFlags(FUNC_UbergraphFunction | FUNC_Static)
            || !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent | FUNC_BlueprintCallable | FUNC_BlueprintPure)
            || Record.Abi.ContainsByPredicate([&](const FString& Row) { return Row.StartsWith(Prefix); })) continue;
        UObject* Owner = Function->GetOuterUClass();
        if (UObject* const* Previous = ReboundTypes.Find(Owner)) Owner = *Previous;
        const auto OwnerType = FAngelscriptType::GetByClass(Cast<UClass>(Owner));
        if (Function->GetOuterUClass() != Candidate && OwnerType.IsValid()
            && Info->ShadowsFrom(OwnerType->GetAngelscriptTypeInfo(FAngelscriptTypeUsage()))) continue;
        FTCHARToUTF8 Utf8(*Name);
        asUINT TokenLength = 0;
        if (FAngelscriptManager::Get().GetScriptEngine()->ParseToken(Utf8.Get(), Utf8.Length(), &TokenLength) != asTC_IDENTIFIER
            || TokenLength != asUINT(Utf8.Length()) || Name == Record.View
            || (Name.StartsWith(TEXT("op"), ESearchCase::CaseSensitive) && Name.Len() > 2 && FChar::IsUpper(Name[2])))
        {
            Error = TEXT("An added Blueprint method must use a non-reserved script identifier.");
            return false;
        }
        if (Info->GetMethodByName(Utf8.Get()))
        {
            Error = TEXT("An added Blueprint method conflicts with an existing script method.");
            return false;
        }
        for (UClass* Parent = Record.Class.Get(); Parent; Parent = Parent->GetSuperClass())
            if (const auto Type = FAngelscriptType::GetByClass(Parent))
                for (asUINT Index = 0; Index < Type->GetAngelscriptTypeInfo(FAngelscriptTypeUsage())->GetPropertyCount(); ++Index)
                {
                    const char* Property = nullptr;
                    Type->GetAngelscriptTypeInfo(FAngelscriptTypeUsage())->GetProperty(Index, &Property);
                    if (Name == UTF8_TO_TCHAR(Property))
                    {
                        Error = TEXT("An added Blueprint method conflicts with an existing script property.");
                        return false;
                    }
                }
        FMethod Method;
        if (!DescribeBlueprintMethod(Function, Method, Error)) return false;
        Added.Add(MoveTemp(Method));
    }
    return true;
}
#endif

}

bool RefreshFeatureModBlueprintPackageInPlace(const FString& ClassPath, FString& Error);

#if WITH_EDITOR && !IS_MONOLITHIC
bool RefreshFeatureModBlueprintPackage(const FString& ClassPath, FString& Error)
{
    if (PendingBlueprintGeneration && PendingBlueprintGeneration->ClassPath == ClassPath)
    {
        if (PendingBlueprintGeneration->bCommitAttempted)
        { Error = TEXT("This Blueprint class is locked after a failed generation commit."); return false; }
        return true;
    }
    auto* Lifetime = BlueprintClassLifetimes.Find(ClassPath);
    if (!Lifetime) return true;
    if (!Lifetime->Class->HasAnyClassFlags(CLASS_Interface))
    {
        if (Lifetime->Class->GetOutermost()->IsDirty())
        {
            Error = TEXT("A changed FeatureMod Blueprint package must have no unsaved editor changes; the live Actor graph is unchanged.");
            return false;
        }
        return RefreshFeatureModBlueprintPackageInPlace(ClassPath, Error);
    }
    const FString PackageName = FPackageName::ObjectPathToPackageName(ClassPath);
    FString Filename;
    if (!FPackageName::DoesPackageExist(PackageName, &Filename))
    {
        Error = TEXT("The imported FeatureMod Blueprint package is missing on disk.");
        return false;
    }
    const FMD5Hash Hash = FMD5Hash::HashFile(*Filename);
    if (!Hash.IsValid())
    {
        Error = TEXT("The imported FeatureMod Blueprint package could not be read.");
        return false;
    }
    if (Hash == Lifetime->DiskHash && FSoftObjectPath(ClassPath).ResolveObject() == Lifetime->Class.Get()) return true;
    static bool bReloading = false;
    if (bReloading)
    {
        Error = TEXT("A FeatureMod Blueprint package reload cannot reenter Blueprint import.");
        return false;
    }
    UPackage* Package = FindPackage(nullptr, *PackageName);
    if (!Package || Package->IsDirty())
    {
        Error = TEXT("A changed FeatureMod Blueprint package must be loaded and have no unsaved editor changes.");
        return false;
    }
    FAngelscriptExecutionWriteScope Writer;
    TGuardValue<bool> ReloadingGuard(bReloading, true);
    TArray<UPackage*> Packages{Package};
    TMap<UPackage*, FString> Filenames;
    TMap<FString, FMD5Hash> DiskHashes;
    TArray<FString> ReplacedPaths;
    for (int32 Index = 0; Index < Packages.Num(); ++Index)
    {
        UPackage* CurrentPackage = Packages[Index];
        FString CurrentFilename;
        if (CurrentPackage->IsDirty() || !FPackageName::DoesPackageExist(CurrentPackage->GetName(), &CurrentFilename))
        {
            Error = TEXT("Every imported Blueprint dependency package must exist on disk and have no unsaved editor changes.");
            return false;
        }
        const FMD5Hash CurrentHash = FMD5Hash::HashFile(*CurrentFilename);
        if (!CurrentHash.IsValid())
        {
            Error = TEXT("An imported Blueprint dependency package could not be read; the current packages are unchanged.");
            return false;
        }
        Filenames.Add(CurrentPackage, CurrentFilename);
        DiskHashes.Add(CurrentPackage->GetName(), CurrentHash);
        for (const auto& Pair : BlueprintClassLifetimes)
        {
            UClass* Previous = Pair.Value.Class.Get();
            if (Previous->GetOutermost() != CurrentPackage) continue;
            ReplacedPaths.Add(Pair.Key);
            for (const auto& Type : FAngelscriptType::GetTypes())
                if (UClass* Bound = Type->GetClass(FAngelscriptTypeUsage()); Bound && Bound != Previous && Bound->IsChildOf(Previous))
                {
                    const auto* BoundLifetime = BlueprintClassLifetimes.Find(Bound->GetPathName());
                    if (BoundLifetime && BoundLifetime->Class.Get() == Bound)
                    {
                        Packages.AddUnique(Bound->GetOutermost());
                        continue;
                    }
#if PLATFORM_WINDOWS || PLATFORM_MAC
                    // PackageTools' existing child-relink hook queues active script
                    // classes for the normal compiler/reinstance transaction.
                    if (CanReloadBlueprintSubclass(Bound)) continue;
#endif
                    Error = TEXT("FeatureMod Blueprint update requires a transaction for its bound subclasses; the current package is unchanged.");
                    return false;
                }
            TArray<UBlueprint*> Dependents;
            if (UBlueprint* Blueprint = Cast<UBlueprint>(Previous->ClassGeneratedBy))
                FBlueprintEditorUtils::FindDependentBlueprints(Blueprint, Dependents);
            for (UBlueprint* Dependent : Dependents)
                if (Dependent->GetOutermost() != CurrentPackage && FAngelscriptType::GetByClass(Dependent->GeneratedClass).IsValid())
                {
                    if (!BlueprintClassLifetimes.Contains(Dependent->GeneratedClass->GetPathName()))
                    {
                        Error = TEXT("An imported Blueprint dependency has no package-update lifetime; the current packages are unchanged.");
                        return false;
                    }
                    Packages.AddUnique(Dependent->GetOutermost());
                }
        }
    }
    SortPackagesForReload(Packages);
    ReplacedPaths.StableSort([&](const FString& Left, const FString& Right)
    {
        return Packages.IndexOfByKey(BlueprintClassLifetimes.FindChecked(Left).Class->GetOutermost())
            < Packages.IndexOfByKey(BlueprintClassLifetimes.FindChecked(Right).Class->GetOutermost());
    });
    TMap<FString, TArray<FString>> ExpectedAbis;
    TMap<FString, TArray<FMethod>> AddedMethods;
    TMap<FString, TArray<FName>> AddedProperties;
    TSet<FString> GenerationRebinds;

    // The public reload API cannot veto PrePackageFixup: by that point the
    // package has been replaced. Inspect the disk candidate in a separate
    // namespace before allowing it to touch any live consumer or VM binding.
    {
        TArray<TStrongObjectPtr<UPackage>> Previews;
        FLinkerInstancingContext Instancing;
        for (UPackage* CurrentPackage : Packages)
        {
            const FString PreviewName = TEXT("/Temp/SoloASBlueprintImport_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
            Previews.Emplace(CreatePackage(*PreviewName));
            Previews.Last()->SetPackageFlags(PKG_ForDiffing);
            Instancing.AddPackageMapping(CurrentPackage->GetFName(), FName(*PreviewName));
        }
        ON_SCOPE_EXIT
        {
            for (auto& Preview : Previews)
            {
                ResetLoaders(Preview.Get());
                TArray<UObject*> Objects;
                SoloASBridge::Compat::GetPackageObjects(Preview.Get(), Objects);
                for (UObject* Object : Objects)
                {
                    Object->ClearFlags(RF_Standalone);
                    Object->SetFlags(RF_Transient);
                }
                Preview->ClearFlags(RF_Standalone);
                Preview->SetFlags(RF_Transient);
                Preview->SetDirtyFlag(false);
            }
        };
        for (int32 Index = 0; Index < Packages.Num(); ++Index)
            if (!LoadPackage(Previews[Index].Get(), *Filenames.FindChecked(Packages[Index]), LOAD_DisableCompileOnLoad, nullptr, &Instancing))
            {
                Error = TEXT("The changed FeatureMod Blueprint package could not be preflighted; the current package is unchanged.");
                return false;
            }
        for (const auto& Preview : Previews)
        {
            TArray<UObject*> CandidateObjects;
            GetObjectsWithPackage(Preview.Get(), CandidateObjects);
            for (UObject* Object : CandidateObjects)
                if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
                {
                    // Specialized Blueprint compilers can own additional state and
                    // need their own preflight before a destructive package reload.
                    if (Blueprint->GetClass() != UBlueprint::StaticClass())
                    {
                        Error = TEXT("This FeatureMod Blueprint compiler does not support package update preflight; the current package is unchanged.");
                        return false;
                    }
                    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
                    if (Blueprint->Status == BS_Error)
                    {
                        Error = TEXT("The changed FeatureMod Blueprint failed isolated compilation; the current package is unchanged.");
                        return false;
                    }
                }
        }
        TMap<UObject*, UObject*> PreflightedTypes;
        for (const FString& Path : ReplacedPaths)
        {
            const auto& Record = BlueprintClassLifetimes.FindChecked(Path);
            UPackage* CurrentPackage = Record.Class->GetOutermost();
            UPackage* Preview = Previews[Packages.IndexOfByKey(CurrentPackage)].Get();
            UClass* Candidate = FindObject<UClass>(Preview, *Record.Class->GetPathName(CurrentPackage));
            const auto CandidateAbi = Candidate ? BlueprintImportAbi(Candidate, &PreflightedTypes) : TArray<FString>();
            bool bCompatible = CompatibleBlueprintAbi(Record.Abi, CandidateAbi);
            if (Candidate && !bCompatible)
            {
                FBlueprintGenerationSchema Schema;
                if (!BuildBlueprintGenerationSchema(Record.Class.Get(), Candidate, PreflightedTypes, Schema, Error)
                    || !CanRebindAngelscriptObjectViewGeneration(Record.Class.Get(), Candidate, Error)) return false;
                for (FProperty* Property : Schema.AddedProperties) AddedProperties.FindOrAdd(Path).Add(Property->GetFName());
                GenerationRebinds.Add(Path);
                bCompatible = true;
            }
            if (!Candidate || !Candidate->HasAnyClassFlags(CLASS_CompiledFromBlueprint)
                || Candidate->HasAnyClassFlags(CLASS_Interface)
                || !bCompatible
                || Record.Abi != BlueprintImportAbi(Record.Class.Get()))
            {
                Error = FString::Printf(TEXT("FeatureMod Blueprint package update rejected before reload: ")
                    TEXT("'%s' changed its parent, property layout, method signatures or referenced types; ")
                    TEXT("current instances and shared bindings are unchanged."), *Path);
                return false;
            }
            if (!CollectAddedBlueprintMethods(Candidate, Record, PreflightedTypes, AddedMethods.FindOrAdd(Path), Error)) return false;
            ExpectedAbis.Add(Path, CandidateAbi);
            // Only earlier, independently checked views can supply a type
            // replacement. Forward or unknown identities still fail preflight.
            PreflightedTypes.Add(Candidate, Record.Class.Get());
        }
        for (UPackage* CurrentPackage : Packages)
            if (FMD5Hash::HashFile(*Filenames.FindChecked(CurrentPackage)) != DiskHashes.FindChecked(CurrentPackage->GetName()))
            {
                Error = TEXT("The FeatureMod Blueprint package changed during preflight; the current package is unchanged.");
                return false;
            }
    }

    bool bRebound = true;
    const FDelegateHandle Handle = FCoreUObjectDelegates::OnPackageReloaded.AddLambda(
        [&](EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
        {
            if (Phase == EPackageReloadPhase::PrePackageFixup && Event && Packages.Contains(Event->GetOldPackage()))
            {
                for (const FString& Path : ReplacedPaths)
                {
                    UClass* Previous = BlueprintClassLifetimes.FindChecked(Path).Class.Get();
                    if (Previous->GetOutermost() != Event->GetOldPackage()) continue;
                    if (!Cast<UClass>(Event->GetRepointedObjects().FindRef(Previous)))
                    {
                        Error = TEXT("The FeatureMod Blueprint class has no replacement in the reloaded package.");
                        bRebound = false;
                    }
                }
                // Stock validates replacement values against the property's
                // declared class. Update external reflection metadata first,
                // including script fields and container inner properties.
                if (bRebound) RebindBlueprintReflectedTypes(Event->GetRepointedObjects(), Event->GetOldPackage());
            }
        });
    ON_SCOPE_EXIT
    {
        FCoreUObjectDelegates::OnPackageReloaded.Remove(Handle);
    };
    FText ReloadError;
    if (!UPackageTools::ReloadPackages(Packages, ReloadError, EReloadPackagesInteractionMode::AssumeNegative))
    {
        Error = TEXT("The changed FeatureMod Blueprint package could not be reloaded: ") + ReloadError.ToString();
        return false;
    }
    if (!bRebound) return false;
    // All reload batches and PreGC compiler subscribers must finish before
    // publishing any final function. Old classes/functions stay pinned meanwhile.
    TMap<UObject*, UObject*> ReplacedTypes;
    for (const FString& Path : ReplacedPaths)
    {
        const auto& Record = BlueprintClassLifetimes.FindChecked(Path);
        UClass* Replacement = Cast<UClass>(FSoftObjectPath(Path).ResolveObject());
        const auto View = FAngelscriptType::GetByAngelscriptTypeName(Record.View);
        const auto Existing = FAngelscriptType::GetByClass(Replacement);
        if (!Replacement || Replacement == Record.Class.Get()
            || ExpectedAbis.FindChecked(Path) != BlueprintImportAbi(Replacement, &ReplacedTypes) || !View.IsValid()
            || View->GetClass(FAngelscriptTypeUsage()) != Record.Class.Get()
            || (Existing.IsValid() && Existing != View))
        {
            Error = TEXT("The FeatureMod Blueprint reload did not replace every imported class with its preflighted ABI.");
            return false;
        }
        ReplacedTypes.Add(Replacement, Record.Class.Get());
    }
    for (const FString& Path : ReplacedPaths)
    {
        const auto& Record = BlueprintClassLifetimes.FindChecked(Path);
        UClass* Replacement = Cast<UClass>(FSoftObjectPath(Path).ResolveObject());
        if (GenerationRebinds.Contains(Path))
        {
            if (!RebindAngelscriptObjectViewGeneration(Record.Class.Get(), Replacement, Error)) return false;
        }
        else if (!RebindAngelscriptObjectView(Record.Class.Get(), Replacement, Error)) return false;
        auto Binds = FAngelscriptBinds::ExistingClass(Record.View);
        for (FName Name : AddedProperties.FindOrAdd(Path))
        {
            FProperty* Property = FindFProperty<FProperty>(Replacement, Name);
            if (!Property || !Property->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;
            const auto Usage = FAngelscriptTypeUsage::FromProperty(Property);
            if (!Usage.IsValid()) continue;
            FAngelscriptType::FBindParams Params;
            Params.BindClass = &Binds;
            Params.NameOverride = Property->GetName();
            Params.bCanRead = true;
            Params.bCanWrite = !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
            if (!Usage.Type->BindProperty(Usage, Params, Property))
                Binds.Property(Usage.GetAngelscriptDeclaration(FAngelscriptType::MemberVariable)
                    + TEXT(" ") + Property->GetName(), Property->GetOffset_ForUFunction(), Params);
        }
        for (const FMethod& Method : AddedMethods.FindChecked(Path))
        {
            Binds.GenericMethod(Method.Declaration + TEXT(" const"), &Invoke, Replacement->FindFunctionByName(FName(*Method.Name)));
            if (!FAngelscriptBinds::GetPreviousBind())
            {
                Error = TEXT("An added Blueprint method could not be registered in its existing script view.");
                return false;
            }
        }
    }
    for (const FString& Path : ReplacedPaths)
    {
        auto& Record = BlueprintClassLifetimes.FindChecked(Path);
        UClass* Replacement = Cast<UClass>(FSoftObjectPath(Path).ResolveObject());
        FAngelscriptManager::Get().RegisterBlueprintDependencyClass(FName(*Record.View), Replacement, Path);
        Record.Class.Reset(Replacement);
        Record.DiskHash = DiskHashes.FindChecked(FPackageName::ObjectPathToPackageName(Path));
        Record.Abi = BlueprintImportAbi(Replacement);
    }
    if (FSoftObjectPath(ClassPath).ResolveObject() != BlueprintClassLifetimes.FindChecked(ClassPath).Class.Get())
    {
        Error = TEXT("The reloaded FeatureMod Blueprint does not match its rebound VM identity.");
        return false;
    }
    return true;
}
#endif
namespace
{
bool HasLatentParameters(UFunction* Function)
{
    for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        if (const auto* Struct = CastField<FStructProperty>(*It))
            if (Struct->Struct->GetFName() == TEXT("LatentActionInfo")) return true;
    return false;
}

// SerializeExpr owns bytecode parsing. This archive changes only reflection
// references in the isolated candidate; scalar operands stay byte-for-byte intact.
class FBlueprintBodyReferenceArchive final : public FArchiveUObject
{
    UPackage* CandidatePackage;
    const TMap<UObject*, UObject*>& Objects;
    const TMap<FField*, FField*>& Fields;
public:
    FBlueprintBodyReferenceArchive(UPackage* Package, const TMap<UObject*, UObject*>& InObjects,
        const TMap<FField*, FField*>& InFields)
        : CandidatePackage(Package), Objects(InObjects), Fields(InFields) {}
    using FArchiveUObject::operator<<;
    virtual void Serialize(void*, int64) override {}
    virtual FArchive& operator<<(UObject*& Object) override
    {
        if (UFunction* Function = Cast<UFunction>(Object); Function && HasLatentParameters(Function)) SetError();
        if (UObject* const* Live = Objects.Find(Object)) Object = *Live;
        else if (Object && Object->GetOutermost() == CandidatePackage) SetError();
        return *this;
    }
    virtual FArchive& operator<<(FObjectPtr& Value) override
    {
        UObject* Object = Value.Get();
        *this << Object;
        Value = Object;
        return *this;
    }
    virtual FArchive& operator<<(FField*& Field) override
    {
        if (FField* const* Live = Fields.Find(Field)) Field = *Live;
        else if (Field && Field->GetOwnerUObject()->GetOutermost() == CandidatePackage) SetError();
        return *this;
    }
};

void RetainPublishedBlueprintMetadata(UObject* Object, UObject* Owner);

class FBlueprintGenerationReferenceArchive final : public FArchiveReplaceObjectRef<UObject>
{
    const TMap<FField*, FField*>& Fields;
    TMap<UObject*, TSet<UObject*>>* PermanentReferences;
    bool bValidateAssignment;
    const TSet<UObject*>* Excluded;
    const TMap<FSoftObjectPath, UObject*>* SoftTargets;
    bool bStrongReferencesOnly;
    bool bWeakStorage = false;
    virtual void SerializeObject(UObject* Object) override
    {
        if (!Excluded || !Excluded->Contains(Object))
            FArchiveReplaceObjectRef<UObject>::SerializeObject(Object);
    }
public:
    FBlueprintGenerationReferenceArchive(UObject* Object, const TMap<UObject*, UObject*>& Objects,
        const TMap<FField*, FField*>& InFields, TMap<UObject*, TSet<UObject*>>* InReferences = nullptr,
        bool bInValidateAssignment = true, const TSet<UObject*>* InExcluded = nullptr,
        const TMap<FSoftObjectPath, UObject*>* InSoftTargets = nullptr, bool bInStrongReferencesOnly = false)
        : FArchiveReplaceObjectRef(Object, Objects, EArchiveReplaceObjectFlags::DelayStart
            | EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef), Fields(InFields), PermanentReferences(InReferences), bValidateAssignment(bInValidateAssignment), Excluded(InExcluded), SoftTargets(InSoftTargets), bStrongReferencesOnly(bInStrongReferencesOnly)
    {
        if (bStrongReferencesOnly) ArIsModifyingWeakAndStrongReferences = false;
        SerializeSearchObject();
    }
    using FArchiveReplaceObjectRef<UObject>::operator<<;
    virtual FArchive& operator<<(UObject*& Object) override
    {
        UObject* Previous = Object;
        FArchiveReplaceObjectRef<UObject>::operator<<(Object);
        if (PermanentReferences && Object && ReplacementMap.Contains(Object)
            && SerializingObject && GUObjectArray.IsDisregardForGC(SerializingObject))
            PermanentReferences->FindOrAdd(Object).Add(SerializingObject);
        if (bValidateAssignment && Object != Previous && Object)
        {
            auto AssignmentClass = [&](UClass* Class)
            {
                if (Excluded)
                    if (UObject* const* Replacement = ReplacementMap.Find(Class)) return CastChecked<UClass>(*Replacement);
                return Class;
            };
            if (const auto* Property = CastField<FObjectPropertyBase>(GetSerializedProperty());
                Property && !Object->IsA(AssignmentClass(Property->PropertyClass))) Object = nullptr;
            if (const auto* Property = CastField<FClassProperty>(GetSerializedProperty());
                Excluded && Property && Object && !CastChecked<UClass>(Object)->IsChildOf(AssignmentClass(Property->MetaClass))) Object = nullptr;
            if (const auto* Property = CastField<FInterfaceProperty>(GetSerializedProperty());
                Property && Object && !Object->GetClass()->ImplementsInterface(AssignmentClass(Property->InterfaceClass))) Object = nullptr;
            // Native AddReferencedObjects may omit a reflected slot. Its old
            // actual class is the only proven assignment bound in that case.
            if (!CastField<FObjectPropertyBase>(GetSerializedProperty())
                && !CastField<FInterfaceProperty>(GetSerializedProperty())
                && Object && !Object->IsA(AssignmentClass(Previous->GetClass()))) Object = nullptr;
        }
        if (Excluded && !bWeakStorage && Object && Object != Previous && ReplacementMap.Contains(Previous)
            && SerializingObject && GUObjectArray.IsDisregardForGC(SerializingObject)
            && (Object->IsA<UClass>() || Object->HasAnyFlags(RF_ClassDefaultObject)))
            RetainPublishedBlueprintMetadata(Object, SerializingObject);
        return *this;
    }
    virtual FArchive& operator<<(FSoftObjectPath& Path) override
    {
        if (bStrongReferencesOnly) return *this;
        TGuardValue<bool> WeakGuard(bWeakStorage, true);
        if (SoftTargets)
            if (UObject* const* Previous = SoftTargets->Find(Path); Previous && ReplacementMap.Contains(*Previous))
            {
                UObject* Object = *Previous;
                *this << Object;
                Path = FSoftObjectPath(Object);
                return *this;
            }
        return FArchiveReplaceObjectRef<UObject>::operator<<(Path);
    }
    virtual FArchive& operator<<(FSoftObjectPtr& Value) override
    {
        TGuardValue<bool> WeakGuard(bWeakStorage, true);
        FSoftObjectPath Path = Value.ToSoftObjectPath();
        UObject* Previous = SoftTargets ? SoftTargets->FindRef(Path) : nullptr;
        if (Previous && ReplacementMap.Contains(Previous))
        {
            *this << Path;
            Value = FSoftObjectPtr(Path);
            return *this;
        }
        return FArchiveUObject::operator<<(Value);
    }
    virtual FArchive& operator<<(FWeakObjectPtr& Value) override
    {
        TGuardValue<bool> WeakGuard(bWeakStorage, true);
        return FArchiveUObject::operator<<(Value);
    }
    virtual FArchive& operator<<(FField*& Field) override
    {
        if (FField* const* Replacement = Fields.Find(Field)) Field = *Replacement;
        return *this;
    }
};

// Imported classes are already held strongly by BlueprintClassLifetimes. Own only
// roots needed by their permanent metadata edges, never Actor/component instances.
TSet<UObject*> BlueprintMetadataSources()
{
    TSet<UObject*> Sources;
    for (const auto& Pair : BlueprintClassLifetimes)
    {
        TArray<UObject*> Objects{Pair.Value.Class.Get()};
        SoloASBridge::Compat::GetNestedObjects(Pair.Value.Class.Get(), Objects);
        if (UObject* Defaults = Pair.Value.Class->GetDefaultObject(false))
        {
            Objects.Add(Defaults);
            SoloASBridge::Compat::GetNestedObjects(Defaults, Objects);
        }
        for (UObject* Object : Objects) Sources.Add(Object);
    }
    return Sources;
}

TMap<UObject*, TSet<UObject*>> BlueprintPermanentMetadataReferences(const TSet<UObject*>& Targets, bool bStrongOnly = false)
{
    TMap<UObject*, UObject*> Self;
    for (UObject* Object : Targets) Self.Add(Object, Object);
    TMap<UObject*, TSet<UObject*>> References;
    const TMap<FField*, FField*> NoFields;
    for (TObjectIterator<UObject> It; It; ++It)
        if (GUObjectArray.IsDisregardForGC(*It)
            && static_cast<const void*>(*It) != static_cast<const void*>(FGCObject::GGCObjectReferencer))
        {
            FBlueprintGenerationReferenceArchive Archive(*It, Self, NoFields, &References, true, nullptr, nullptr, bStrongOnly);
        }
    return References;
}

void ReleaseBlueprintOwnedMetadataRoots()
{
    TSet<UObject*> Roots;
    for (const auto& Pair : BlueprintClassLifetimes) Roots.Append(Pair.Value.OwnedMetadataRoots);
    if (Roots.IsEmpty()) return;
    const TSet<UObject*> Sources = BlueprintMetadataSources();
    for (const auto& Reference : BlueprintPermanentMetadataReferences(Roots))
        for (UObject* Source : Reference.Value)
            checkf(Sources.Contains(Source), TEXT("Blueprint metadata root has an external permanent owner: %s"), *Source->GetPathName());
    // Only imported class/SCS/CDO metadata is changed during owner teardown.
    for (const auto& Pair : BlueprintClassLifetimes)
        if (auto* Class = Cast<UBlueprintGeneratedClass>(Pair.Value.Class.Get()); Class && Class->SimpleConstructionScript)
        {
            const TArray<USCS_Node*> Nodes = Class->SimpleConstructionScript->GetAllNodes();
            for (USCS_Node* Node : Nodes)
                if (Roots.Contains(Node)) Class->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Node);
        }
    TMap<UObject*, UObject*> Clear;
    for (UObject* Object : Roots) Clear.Add(Object, nullptr);
    const TMap<FField*, FField*> NoFields;
    for (UObject* Source : Sources)
    {
        FBlueprintGenerationReferenceArchive Archive(Source, Clear, NoFields);
    }
    checkf(BlueprintPermanentMetadataReferences(Roots).IsEmpty(), TEXT("Blueprint owned metadata still has permanent references at teardown"));
    for (auto& Pair : BlueprintClassLifetimes)
    {
        for (UObject* Object : Pair.Value.OwnedMetadataRoots) Object->RemoveFromRoot();
        Pair.Value.OwnedMetadataRoots.Reset();
    }
}

struct FBlueprintMetadataRootLifetime
{
    FDelegateHandle PreExit, PreGc;
    TMap<FObjectKey, TSet<FObjectKey>> PermanentOwners;
    uint32 Acquired = 0, Released = 0;
    bool bStopped = false;
    void Retain(UObject* Object, UObject* Owner);
    void Refresh();
    void Log(const TCHAR* Event) const;
    static void BeforeGc();
    static void Stop();
    FBlueprintMetadataRootLifetime() { PreExit = FCoreDelegates::OnEnginePreExit.AddStatic(&Stop); }
    ~FBlueprintMetadataRootLifetime() { Stop(); }
};
FBlueprintMetadataRootLifetime& BlueprintMetadataRootLifetime()
{
    static FBlueprintMetadataRootLifetime Lifetime;
    return Lifetime;
}
void FBlueprintMetadataRootLifetime::Log(const TCHAR* Event) const
{
    int32 OwnerEdges = 0;
    for (const auto& Pair : PermanentOwners) OwnerEdges += Pair.Value.Num();
    UE_LOG(Angelscript, Display, TEXT("N04_METADATA_ROOTS event=%s acquired=%u released=%u held=%d owner_edges=%d editor_noop=%d"),
        Event, Acquired, Released, PermanentOwners.Num(), OwnerEdges, !GUObjectArray.DisregardForGCEnabled());
}
void FBlueprintMetadataRootLifetime::Retain(UObject* Object, UObject* Owner)
{
    check(!bStopped);
    const FObjectKey Key(Object);
    if (auto* Owners = PermanentOwners.Find(Key)) { Owners->Add(FObjectKey(Owner)); return; }
    // An independent root belongs to its original owner, never to this lifetime.
    if (Object->IsRooted() || GUObjectArray.IsDisregardForGC(Object)) return;
    Object->AddToRoot();
    PermanentOwners.FindOrAdd(Key).Add(FObjectKey(Owner));
    ++Acquired;
    if (!PreGc.IsValid()) PreGc = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddStatic(&BeforeGc);
    Log(TEXT("acquire"));
}
void RetainPublishedBlueprintMetadata(UObject* Object, UObject* Owner)
{
    BlueprintMetadataRootLifetime().Retain(Object, Owner);
}
void FBlueprintMetadataRootLifetime::Refresh()
{
    TSet<UObject*> Targets;
    for (const auto& Pair : PermanentOwners)
        if (UObject* Object = Pair.Key.ResolveObjectPtrEvenIfGarbage()) Targets.Add(Object);
    const auto References = Targets.IsEmpty() ? TMap<UObject*, TSet<UObject*>>()
        : BlueprintPermanentMetadataReferences(Targets, true);
    for (auto It = PermanentOwners.CreateIterator(); It; ++It)
    {
        UObject* Object = It.Key().ResolveObjectPtrEvenIfGarbage();
        const auto* Owners = References.Find(Object);
        if (Owners && !Owners->IsEmpty())
        {
            It.Value().Reset();
            for (UObject* Owner : *Owners) It.Value().Add(FObjectKey(Owner));
            continue;
        }
        if (Object && Object->IsRooted()) Object->RemoveFromRoot();
        It.RemoveCurrent();
        ++Released;
        Log(TEXT("release"));
    }
    if (PermanentOwners.IsEmpty() && PreGc.IsValid())
    {
        FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(PreGc);
        PreGc.Reset();
    }
}
void FBlueprintMetadataRootLifetime::BeforeGc() { BlueprintMetadataRootLifetime().Refresh(); }
void FBlueprintMetadataRootLifetime::Stop()
{
    auto& Lifetime = BlueprintMetadataRootLifetime();
    if (Lifetime.bStopped) return;
    Lifetime.bStopped = true;
    Lifetime.Refresh();
    if (Lifetime.PreGc.IsValid()) FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(Lifetime.PreGc);
    Lifetime.PreGc.Reset();
    // Still-referenced permanent targets stay rooted until UObject exit purge.
    // The identity-only owner map never dereferences objects in static teardown.
    Lifetime.Log(TEXT("exit"));
    FCoreDelegates::OnEnginePreExit.Remove(Lifetime.PreExit);
    Lifetime.PreExit.Reset();
    ReleaseBlueprintOwnedMetadataRoots();
}

TSet<TWeakObjectPtr<UClass>> FailedBlueprintOwnedGraphs;

// Actor graphs share the outer schema admission. Their owned identities are matched
// independently by SCS GUID/actual class and by relative template subobject path.
bool PrepareBlueprintOwnedGraph(UBlueprintGeneratedClass* Candidate, UBlueprintGeneratedClass* Live,
    TMap<UObject*, UObject*>& Objects, FBlueprintOwnedGraphUpdate& Update, FString& Error)
{
    if (!Live->IsChildOf(AActor::StaticClass())) return true;
    auto Reject = [&](const TCHAR* Reason)
    {
        Error = FString::Printf(TEXT("Actor owned-graph preflight failed: %s; the live graph is unchanged."), Reason);
        return false;
    };
    for (TObjectIterator<UClass> It; It; ++It)
        if (*It != Live && It->IsChildOf(Live)) return Reject(TEXT("a loaded subclass needs its own graph migration"));
    if (FailedBlueprintOwnedGraphs.Contains(Live)) return Reject(TEXT("an earlier commit failed in this process; restart before publishing"));
    USimpleConstructionScript* Before = Live->SimpleConstructionScript;
    USimpleConstructionScript* After = Candidate->SimpleConstructionScript;
    if (!Before || !After) return Reject(TEXT("missing construction script"));
    Update.Class = Live;
    Update.Script = Before;
    Update.PreviousNodes = Before->GetAllNodes();
    TMap<FGuid, USCS_Node*> Identities;
    TSet<FGuid> CandidateIdentities;
    TMap<USCS_Node*, USCS_Node*> Parents;
    for (USCS_Node* Node : Update.PreviousNodes)
    {
        if (!Node->VariableGuid.IsValid() || Identities.Contains(Node->VariableGuid) || !Node->ComponentTemplate)
            return Reject(TEXT("invalid current node identity or template"));
        Identities.Add(Node->VariableGuid, Node);
    }
    for (USCS_Node* Node : After->GetAllNodes())
    {
        if (!Node->VariableGuid.IsValid() || CandidateIdentities.Contains(Node->VariableGuid)
            || !Node->ComponentTemplate || Node->ComponentTemplate->GetClass() != Node->ComponentClass
            || Node->ComponentTemplate->GetIsReplicated()) return Reject(TEXT("invalid candidate identity, actual class or replicated template"));
        CandidateIdentities.Add(Node->VariableGuid);
        FObjectPropertyBase* Slot = FindFProperty<FObjectPropertyBase>(Live, Node->GetVariableName());
        if (!Slot || !Node->ComponentTemplate->IsA(Slot->PropertyClass))
            return Reject(TEXT("a candidate component requires a different generated field schema"));
        if (!Node->ParentComponentOrVariableName.IsNone() || !Node->ParentComponentOwnerClassName.IsNone()
            || Node->bIsParentComponentNative) return Reject(TEXT("an inherited/native SCS parent needs a separate owner migration"));
        if (auto* Primitive = Cast<UPrimitiveComponent>(Node->ComponentTemplate))
            if (const FBodyInstance* Body = Primitive->GetBodyInstance(); Body && Body->bSimulatePhysics)
                return Reject(TEXT("a candidate component simulates physics"));
        for (USCS_Node* Child : Node->GetChildNodes())
        {
            if (!After->GetAllNodes().Contains(Child) || Parents.Contains(Child)) return Reject(TEXT("invalid candidate parent graph"));
            Parents.Add(Child, Node);
        }
    }
    Objects.Add(After, Before);
    // Stage new templates outside the live class. Only commit moves them into
    // their final outers; Actor component allocation is also a commit effect.
    for (USCS_Node* Node : After->GetAllNodes())
    {
        USCS_Node* Previous = Identities.FindRef(Node->VariableGuid);
        const bool Keep = Previous && Previous->ComponentTemplate->GetClass() == Node->ComponentTemplate->GetClass();
        USCS_Node* Target = Keep ? Previous : DuplicateObject<USCS_Node>(Node, GetTransientPackage());
        if (!Target) return Reject(TEXT("could not stage a candidate node"));
        if (!Keep)
        {
            Update.Prepared.Emplace(Target);
            Update.Moves.Add({Target, Before, MakeUniqueObjectName(Before, USCS_Node::StaticClass())});
        }
        Objects.Add(Node, Target);
        Update.Nodes.Add({Node, Target, Previous, Parents.FindRef(Node)});
        if (Previous) Update.Replacements.Add(Previous, Target);
        TArray<UObject*> NewObjects{Node->ComponentTemplate};
        SoloASBridge::Compat::GetNestedObjects(Node->ComponentTemplate, NewObjects, RF_Transient);
        NewObjects.Sort([](const UObject& A, const UObject& B) { return A.GetPathName().Len() < B.GetPathName().Len(); });
        for (UObject* Object : NewObjects)
        {
            if (Objects.Contains(Object)) continue;
            const FString Path = Object == Node->ComponentTemplate ? FString() : Object->GetPathName(Node->ComponentTemplate);
            UObject* Old = Previous ? (Path.IsEmpty() ? Previous->ComponentTemplate.Get()
                : FindObject<UObject>(Previous->ComponentTemplate, *Path)) : nullptr;
            if (Keep && Old && Old->GetClass() == Object->GetClass())
            {
                Objects.Add(Object, Old);
                Update.Defaults.Add({Object, Old, {}});
                Update.Replacements.Add(Old, Old);
                continue;
            }
            UObject* Outer = Path.IsEmpty() ? Live : Objects.FindRef(Object->GetOuter());
            if (!Outer) return Reject(TEXT("candidate subobject outer is outside its template graph"));
            UObject* Clone = StaticDuplicateObject(Object, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), Object->GetClass()));
            if (!Clone) return Reject(TEXT("could not stage a candidate template subobject"));
            Update.Prepared.Emplace(Clone);
            Update.Moves.Add({Clone, Outer, Object->GetFName()});
            Objects.Add(Object, Clone);
            if (Old) Update.Replacements.Add(Old, Clone);
            TArray<UObject*> Nested;
            SoloASBridge::Compat::GetNestedObjects(Object, Nested);
            for (UObject* Child : Nested)
            {
                UObject* Copy = FindObject<UObject>(Clone, *Child->GetPathName(Object));
                if (!Copy) return Reject(TEXT("staged subobject copy is incomplete"));
                Objects.Add(Child, Copy);
                UObject* Prior = Old ? FindObject<UObject>(Old, *Child->GetPathName(Object)) : nullptr;
                if (Prior) Update.Replacements.Add(Prior, Copy);
            }
        }
    }
    for (USCS_Node* Node : Update.PreviousNodes)
    {
        if (!Update.Replacements.Contains(Node)) Update.Replacements.Add(Node, nullptr);
        TArray<UObject*> OldObjects{Node->ComponentTemplate};
        SoloASBridge::Compat::GetNestedObjects(Node->ComponentTemplate, OldObjects);
        for (UObject* Object : OldObjects)
            if (!Update.Replacements.Contains(Object)) Update.Replacements.Add(Object, nullptr);
    }
    for (const auto& Node : Update.Nodes)
    {
        TSet<USCS_Node*> Visited;
        for (USCS_Node* Parent = Node.Candidate; Parent; Parent = Parents.FindRef(Parent))
        {
            if (!Objects.Contains(Parent) || Visited.Contains(Parent)) return Reject(TEXT("candidate hierarchy is cyclic"));
            Visited.Add(Parent);
        }
        if (!Node.Parent && Node.Candidate->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
        {
            if (Update.Root) return Reject(TEXT("multiple scene roots need an explicit native parent"));
            Update.Root = Node.Candidate;
        }
    }
    if (!Update.Root) return Reject(TEXT("candidate has no scene root"));
    for (const auto& Move : Update.Moves)
        if (UObject* Collision = FindObjectFast<UObject>(Move.Outer, Move.Name);
            Collision && (!Update.Replacements.Contains(Collision) || Update.Replacements.FindRef(Collision) == Collision))
            return Reject(TEXT("a new owned object collides with an unrelated live object"));
    for (TObjectIterator<AActor> It; It; ++It)
    {
        AActor* Actor = *It;
        if (Actor->GetClass() != Live || Actor->IsTemplate()) continue;
        if (!IsValid(Actor) || Actor->IsActorBeingDestroyed() || Actor->GetIsReplicated()) return Reject(TEXT("invalid or replicated live Actor"));
        auto& Instance = Update.Actors.AddDefaulted_GetRef();
        Instance.Object = Actor;
        Instance.Transform = Actor->GetActorTransform();
        for (USCS_Node* Node : Update.PreviousNodes)
        {
            FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Live, Node->GetVariableName());
            UActorComponent* Component = Property ? Cast<UActorComponent>(Property->GetObjectPropertyValue_InContainer(Actor)) : nullptr;
            if (!IsValid(Component) || Component->GetOwner() != Actor || Component->GetClass() != Node->ComponentTemplate->GetClass()
                || Component->CreationMethod != EComponentCreationMethod::SimpleConstructionScript || Component->GetIsReplicated()
                || Component->IsTemplate()) return Reject(TEXT("current component ownership or actual class differs from its node"));
            if (auto* Primitive = Cast<UPrimitiveComponent>(Component))
                if (const FBodyInstance* Body = Primitive->GetBodyInstance(); Body && Body->bSimulatePhysics)
                    return Reject(TEXT("a current component simulates physics"));
            Instance.Objects.Add(Node->ComponentTemplate, Component);
            TArray<UObject*> Subobjects;
            SoloASBridge::Compat::GetNestedObjects(Node->ComponentTemplate, Subobjects, RF_Transient);
            for (UObject* Subobject : Subobjects)
            {
                UObject* Owned = FindObject<UObject>(Component, *Subobject->GetPathName(Node->ComponentTemplate));
                if (!Owned || Owned->GetClass() != Subobject->GetClass()) return Reject(TEXT("current instance template subobject is missing or changed"));
                Instance.Objects.Add(Subobject, Owned);
            }
            if (Update.Replacements.FindRef(Node->ComponentTemplate) != Node->ComponentTemplate) Instance.Retired.Add(Component);
        }
        for (USCS_Node* Node : Update.PreviousNodes)
        {
            auto* Component = Cast<USceneComponent>(Instance.Objects.FindRef(Node->ComponentTemplate));
            if (!Component) continue;
            USCS_Node* OldParent = Before->FindParentNode(Node);
            USceneComponent* Parent = OldParent ? Cast<USceneComponent>(Instance.Objects.FindRef(OldParent->ComponentTemplate)) : nullptr;
            if ((OldParent && Component->GetAttachParent() != Parent)
                || (!OldParent && Actor->GetRootComponent() != Component)
                || Component->GetAttachSocketName() != Node->AttachToName)
                return Reject(TEXT("a live SCS attachment was overridden"));
        }
        for (UActorComponent* Retired : Instance.Retired)
            if (auto* Scene = Cast<USceneComponent>(Retired))
                for (USceneComponent* Child : Scene->GetAttachChildren())
                {
                    if (Child->GetOwner() != Actor) return Reject(TEXT("a cross-Actor attachment targets a retired component"));
                    if (Child->CreationMethod != EComponentCreationMethod::SimpleConstructionScript) Instance.Reattach.AddUnique(Child);
                }
        for (int32 Index = 0; Index < Update.Defaults.Num(); ++Index)
        {
            const auto& Default = Update.Defaults[Index];
            UObject* Object = Instance.Objects.FindRef(Default.Live);
            if (!Object) return Reject(TEXT("kept template has no live owned instance"));
            auto& Prepared = Update.Instances.AddDefaulted_GetRef();
            Prepared.Object = Object;
            Prepared.Default = Index;
            Prepared.Objects = Instance.Objects;
        }
    }
    return true;
}

bool PrepareBlueprintOwnedGraphDefaults(FBlueprintOwnedGraphUpdate& Update,
    const TMap<UObject*, UObject*>& Objects, const TMap<FField*, FField*>& Fields)
{
    for (auto& Default : Update.Defaults)
    {
        FBlueprintGenerationReferenceArchive Archive(Default.Candidate, Objects, Fields);
        for (TFieldIterator<FProperty> It(Default.Live->GetClass()); It; ++It)
            if (It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)
                && !It->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient)
                && !It->Identical_InContainer(Default.Live, Default.Candidate)) Default.Changed.Add(*It);
    }
    const TMap<FField*, FField*> NoFields;
    for (auto& Instance : Update.Instances)
    {
        const auto& Default = Update.Defaults[Instance.Default];
        Instance.Values = MakeUnique<FStructOnScope>(Default.Live->GetClass());
        for (FProperty* Property : Default.Changed)
        {
            Property->CopyCompleteValue_InContainer(Instance.Values->GetStructMemory(), Default.Live);
            FBlueprintBodyReferenceArchive OldReferences(nullptr, Instance.Objects, NoFields);
            for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
            {
                FStructuredArchiveFromArchive Archive(OldReferences);
                Property->SerializeItem(Archive.GetSlot(), Property->ContainerPtrToValuePtr<void>(Instance.Values->GetStructMemory(), Index));
            }
            if (OldReferences.IsError()) return false;
            // Stock SCS duplication may retain a sibling template reference.
            // Both its literal old default and the normalized instance default
            // represent inheritance; a different instance value is an override.
            if (!Property->Identical_InContainer(Instance.Object, Default.Live)
                && !Property->Identical_InContainer(Instance.Object, Instance.Values->GetStructMemory())) continue;
            Instance.Inherited.Add(Property);
            Property->CopyCompleteValue_InContainer(Instance.Values->GetStructMemory(), Default.Candidate);
            FBlueprintBodyReferenceArchive NewReferences(nullptr, Instance.Objects, NoFields);
            for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
            {
                FStructuredArchiveFromArchive Archive(NewReferences);
                Property->SerializeItem(Archive.GetSlot(), Property->ContainerPtrToValuePtr<void>(Instance.Values->GetStructMemory(), Index));
            }
            if (NewReferences.IsError()) return false;
        }
    }
    const FByteProperty* Mobility = FindFProperty<FByteProperty>(USceneComponent::StaticClass(),
        GET_MEMBER_NAME_CHECKED(USceneComponent, Mobility));
    auto EffectiveMobility = [&](const FBlueprintOwnedGraphUpdate::FActor& Actor, USCS_Node* Node)
    {
        UObject* Template = Objects.FindRef(Node->ComponentTemplate);
        UObject* Component = Actor.Objects.FindRef(Template);
        for (const auto& Instance : Update.Instances)
            if (Instance.Object == Component && Instance.Inherited.Contains(Mobility))
                return Mobility->GetPropertyValue_InContainer(Instance.Values->GetStructMemory());
        return Component ? Mobility->GetPropertyValue_InContainer(Component)
            : Mobility->GetPropertyValue_InContainer(Template);
    };
    for (const auto& Actor : Update.Actors)
        for (const auto& Node : Update.Nodes)
            if (Node.Parent && Node.Candidate->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
            {
                if (!Node.Parent->ComponentClass->IsChildOf(USceneComponent::StaticClass())) return false;
                if (EffectiveMobility(Actor, Node.Candidate) == EComponentMobility::Static
                    && EffectiveMobility(Actor, Node.Parent) != EComponentMobility::Static) return false;
            }
    return true;
}

bool CommitBlueprintOwnedGraph(FBlueprintOwnedGraphUpdate& Update, const TMap<UObject*, UObject*>& Objects, FString& Error)
{
    if (!Update.Class) return true;
    auto Fail = [&](const TCHAR* Reason)
    {
        FailedBlueprintOwnedGraphs.Add(Update.Class);
        Error = FString::Printf(TEXT("Actor owned-graph publication failed: %s; the live graph may be partially modified and no rollback was performed. Stop further publication in this process."), Reason);
        return false;
    };
    const TMap<FField*, FField*> NoFields;
    const ERenameFlags RenameFlags = REN_DoNotDirty | REN_DontCreateRedirectors | REN_AllowPackageLinkerMismatch;
    auto ReserveName = [&](UObject* Outer, FName Name)
    {
        UObject* Collision = FindObjectFast<UObject>(Outer, Name);
        return !Collision || Collision->Rename(*MakeUniqueObjectName(Outer, Collision->GetClass(), TEXT("RetiredBlueprintGraph")).ToString(), nullptr, RenameFlags);
    };
    for (const auto& Move : Update.Moves)
        if (!ReserveName(Move.Outer, Move.Name) || !Move.Object->Rename(*Move.Name.ToString(), Move.Outer, RenameFlags))
            return Fail(TEXT("could not install a staged node or template"));
    for (auto& Node : Update.Nodes)
    {
        Node.Live->ComponentTemplate = CastChecked<UActorComponent>(Objects.FindRef(Node.Candidate->ComponentTemplate));
        FBlueprintGenerationReferenceArchive References(Node.Live->ComponentTemplate, Objects, NoFields);
    }
    TMap<UObject*, UObject*> Replacements = Update.Replacements;
    TArray<UActorComponent*> Created;
    // Allocate only new identities. Retired same-name objects have a reserved name
    // before duplication, so StaticDuplicateObjectEx cannot recycle their memory.
    for (auto& Actor : Update.Actors)
    {
        for (UActorComponent* Retired : Actor.Retired)
            if (!Retired->Rename(*MakeUniqueObjectName(Actor.Object, Retired->GetClass(), TEXT("RetiredBlueprintGraph")).ToString(), nullptr, RenameFlags))
                return Fail(TEXT("could not reserve a retired component name"));
        auto MapInstance = [&](UObject* Template, UObject* Instance)
        {
            Actor.Objects.Add(Template, Instance);
            TArray<UObject*> Subobjects;
            SoloASBridge::Compat::GetNestedObjects(Template, Subobjects, RF_Transient);
            for (UObject* Subobject : Subobjects)
                Actor.Objects.Add(Subobject, FindObject<UObject>(Instance, *Subobject->GetPathName(Template)));
        };
        for (const auto& Node : Update.Nodes)
        {
            UActorComponent* Template = Node.Live->ComponentTemplate;
            UActorComponent* Component = Node.Live == Node.Previous
                ? Cast<UActorComponent>(Actor.Objects.FindRef(Template))
                : Actor.Object->CreateComponentFromTemplate(Template, Node.Candidate->GetVariableName());
            if (!Component) return Fail(TEXT("could not allocate a new Actor component"));
            if (Node.Live != Node.Previous)
            {
                Component->CreationMethod = EComponentCreationMethod::SimpleConstructionScript;
                Component->SetNetAddressable();
                Created.Add(Component);
            }
            if (Node.Live != Node.Previous) MapInstance(Template, Component);
        }
        for (const auto& Move : Update.Moves)
        {
            // Top-level templates were instantiated above. New subobjects of a
            // kept template need their corresponding instance-owned identities.
            if (Move.Outer == Update.Class || Move.Outer == Update.Script || Actor.Objects.FindRef(Move.Object)) continue;
            UObject* Outer = Actor.Objects.FindRef(Move.Outer);
            if (!Outer || !ReserveName(Outer, Move.Name)) return Fail(TEXT("could not install an instance-owned subobject"));
            FObjectDuplicationParameters Duplication(Move.Object, Outer);
            Duplication.DestName = Move.Name;
            Duplication.FlagMask = RF_AllFlags & ~(RF_ArchetypeObject | RF_Transactional | RF_WasLoaded | RF_Public | RF_InheritableComponentTemplate);
            Duplication.PortFlags = PPF_DuplicateVerbatim;
            UObject* Instance = StaticDuplicateObjectEx(Duplication);
            if (!Instance) return Fail(TEXT("could not allocate an instance-owned subobject"));
            MapInstance(Move.Object, Instance);
        }
        for (const auto& Replacement : Update.Replacements)
        {
            UObject* Old = Actor.Objects.FindRef(Replacement.Key);
            if (!Old) continue;
            UObject* New = Replacement.Value ? Actor.Objects.FindRef(Replacement.Value) : nullptr;
            Replacements.Add(Old, New);
        }
        for (const auto& Node : Update.Nodes)
        {
            UActorComponent* Component = CastChecked<UActorComponent>(Actor.Objects.FindRef(Node.Live->ComponentTemplate));
            FObjectPropertyBase* Field = FindFProperty<FObjectPropertyBase>(Update.Class, Node.Candidate->GetVariableName());
            if (!Field || !Component->IsA(Field->PropertyClass)) return Fail(TEXT("generated component slot is incompatible"));
            Field->SetObjectPropertyValue_InContainer(Actor.Object, Component);
        }
        // All new component/subobject identities now exist for this Actor.
        for (const auto& Move : Update.Moves)
            if (UObject* Instance = Actor.Objects.FindRef(Move.Object))
            {
                FBlueprintGenerationReferenceArchive References(Instance, Actor.Objects, NoFields);
            }
    }
    TArray<TUniquePtr<FComponentReregisterContext>> Registration;
    for (const auto& Actor : Update.Actors)
        for (const auto& Node : Update.Nodes)
            if (Node.Live == Node.Previous)
                Registration.Emplace(MakeUnique<FComponentReregisterContext>(CastChecked<UActorComponent>(Actor.Objects.FindRef(Node.Live->ComponentTemplate))));
    for (const auto& Instance : Update.Instances)
    {
        const auto* Actor = Update.Actors.FindByPredicate([&](const auto& Item) { return Instance.Object->IsIn(Item.Object); });
        if (!Actor) return Fail(TEXT("a kept default lost its Actor owner"));
        for (FProperty* Property : Instance.Inherited)
        {
            FBlueprintBodyReferenceArchive References(nullptr, Actor->Objects, NoFields);
            for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
            {
                FStructuredArchiveFromArchive Archive(References);
                Property->SerializeItem(Archive.GetSlot(), Property->ContainerPtrToValuePtr<void>(Instance.Values->GetStructMemory(), Index));
            }
            Property->CopyCompleteValue_InContainer(Instance.Object, Instance.Values->GetStructMemory());
        }
    }
    for (const auto& Default : Update.Defaults)
        for (FProperty* Property : Default.Changed) Property->CopyCompleteValue_InContainer(Default.Live, Default.Candidate);
    // Rebuild both public SCS collections through their owning APIs.
    for (USCS_Node* Node : Update.PreviousNodes)
    {
        const TArray<USCS_Node*> Children = Node->GetChildNodes();
        for (USCS_Node* Child : Children) Node->RemoveChildNode(Child);
    }
    const TArray<USCS_Node*> Roots = Update.Script->GetRootNodes();
    for (USCS_Node* Root : Roots) Update.Script->RemoveNode(Root, false);
    for (auto& Node : Update.Nodes)
    {
        const TArray<USCS_Node*> Children = Node.Live->GetChildNodes();
        for (USCS_Node* Child : Children) Node.Live->RemoveChildNode(Child, false);
        Node.Live->AttachToName = Node.Candidate->AttachToName;
        Node.Live->ParentComponentOrVariableName = Node.Candidate->ParentComponentOrVariableName;
        Node.Live->ParentComponentOwnerClassName = Node.Candidate->ParentComponentOwnerClassName;
        Node.Live->bIsParentComponentNative = Node.Candidate->bIsParentComponentNative;
    }
    // Install the final root before adding children; AddNode validates roots.
    Update.Script->AddNode(CastChecked<USCS_Node>(Objects.FindRef(Update.Root)));
    for (auto& Node : Update.Nodes)
        if (Node.Parent) CastChecked<USCS_Node>(Objects.FindRef(Node.Parent))->AddChildNode(Node.Live);
        else if (Node.Candidate != Update.Root) Update.Script->AddNode(Node.Live);
    Update.Script->CreateNameToSCSNodeMap();
    Update.Script->RemoveNameToSCSNodeMap();
    for (const auto& Node : Update.Nodes)
    {
        auto& Data = Node.Live->CookedComponentInstancingData;
        Data.~FBlueprintCookedComponentInstancingData();
        new (&Data) FBlueprintCookedComponentInstancingData();
        Data.ChangedPropertyList = Node.Candidate->CookedComponentInstancingData.ChangedPropertyList;
        Data.bHasValidCookedData = Node.Candidate->CookedComponentInstancingData.bHasValidCookedData;
        if (Data.bHasValidCookedData) Data.BuildCachedPropertyDataFromTemplate(Node.Live->ComponentTemplate);
    }
    auto& MetadataRoots = BlueprintClassLifetimes.FindChecked(Update.Class->GetPathName()).OwnedMetadataRoots;
    TSet<UObject*> MetadataTargets = MetadataRoots;
    for (const auto& Move : Update.Moves)
    {
        MetadataTargets.Add(Move.Object);
        TArray<UObject*> Nested;
        SoloASBridge::Compat::GetNestedObjects(Move.Object, Nested);
        for (UObject* Object : Nested) MetadataTargets.Add(Object);
    }
    const TSet<UObject*> MetadataSources = BlueprintMetadataSources();
    auto AcquireMetadataRoots = [&](const TMap<UObject*, TSet<UObject*>>& References)
    {
        for (const auto& Reference : References)
        {
            for (UObject* Source : Reference.Value)
                if (!MetadataSources.Contains(Source)) return Fail(TEXT("new metadata has an external permanent owner"));
            UObject* Object = Reference.Key;
            if (!Object->IsRooted() && !GUObjectArray.IsDisregardForGC(Object))
            {
                BlueprintMetadataRootLifetime();
                Object->AddToRoot();
                MetadataRoots.Add(Object);
            }
        }
        return true;
    };
    if (!AcquireMetadataRoots(BlueprintPermanentMetadataReferences(MetadataTargets))) return false;
    for (const auto& Actor : Update.Actors)
    {
        auto* Root = CastChecked<USceneComponent>(Actor.Objects.FindRef(CastChecked<USCS_Node>(Objects.FindRef(Update.Root))->ComponentTemplate));
        // Detach the old hierarchy before changing direction, so valid root
        // switches are not rejected as temporary attachment cycles.
        for (const auto& Node : Update.Nodes)
            if (auto* Component = Cast<USceneComponent>(Actor.Objects.FindRef(Node.Live->ComponentTemplate)))
                Component->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
        if (!Actor.Object->SetRootComponent(Root)) return Fail(TEXT("could not set final Actor root"));
        Root->SetWorldTransform(Actor.Transform);
        for (const auto& Node : Update.Nodes)
            if (Node.Parent)
            {
                auto* Component = Cast<USceneComponent>(Actor.Objects.FindRef(Node.Live->ComponentTemplate));
                auto* Parent = Cast<USceneComponent>(Actor.Objects.FindRef(CastChecked<USCS_Node>(Objects.FindRef(Node.Parent))->ComponentTemplate));
                if (Component && (!Parent || !Component->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform, Node.Candidate->AttachToName)))
                    return Fail(TEXT("could not attach a component to its final parent"));
            }
        for (USceneComponent* Component : Actor.Reattach)
            if (!Component->AttachToComponent(Root, FAttachmentTransformRules::KeepWorldTransform))
                return Fail(TEXT("could not migrate a non-SCS child from its retired parent"));
    }
    // Includes native reflected slots and legal persistent AS UPROPERTY fields.
    // Unserialized raw pointers are not covered. Incompatible typed slots become null.
    for (TObjectIterator<UObject> It; It; ++It)
        if (IsValid(*It))
        {
            FBlueprintGenerationReferenceArchive References(*It, Replacements, NoFields);
        }
    const auto RemainingMetadata = BlueprintPermanentMetadataReferences(MetadataTargets);
    if (!AcquireMetadataRoots(RemainingMetadata)) return false;
    for (auto It = MetadataRoots.CreateIterator(); It; ++It)
        if (!RemainingMetadata.Contains(*It))
        {
            (*It)->RemoveFromRoot();
            It.RemoveCurrent();
        }
    for (const auto& Actor : Update.Actors)
        for (UActorComponent* Retired : Actor.Retired) Retired->DestroyComponent();
    Registration.Reset();
    for (UActorComponent* Component : Created)
    {
        if (!Component->HasBeenCreated()) Component->OnComponentCreated();
        Component->RegisterComponentWithWorld(Component->GetOwner()->GetWorld());
        if (!Component->IsRegistered()) return Fail(TEXT("a new component did not register"));
    }
    return true;
}


}

void RebindBlueprintGenerationMetadata(UObject* Object, const TMap<UObject*, UObject*>& Objects,
    const TMap<FField*, FField*>& Fields, const TSet<UObject*>* Excluded,
    const TMap<FSoftObjectPath, UObject*>* SoftTargets)
{
    FBlueprintGenerationReferenceArchive Archive(Object, Objects, Fields, nullptr, Excluded != nullptr, Excluded, SoftTargets);
}

bool GatherBlueprintSubclassGenerations(FPendingBlueprintGeneration& Pending, TSet<UClass*>& Affected, FString& Error)
{
    TArray<UClass*> Classes;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        if (Affected.Contains(Class) || Class->HasAnyClassFlags(CLASS_NewerVersionExists)) continue;
#if WITH_EDITORONLY_DATA
        if (auto* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy); Blueprint && Blueprint->GeneratedClass != Class) continue;
#endif
        bool bAffectedParent = false;
        for (UClass* Parent = Class->GetSuperClass(); Parent; Parent = Parent->GetSuperClass())
            bAffectedParent |= Affected.Contains(Parent);
        if (!bAffectedParent) continue;
        if (Cast<UASClass>(Class))
        { Error = TEXT("A loaded script subclass is outside the prepared module closure."); return false; }
        if ((Class->GetClass() != UBlueprintGeneratedClass::StaticClass()
                && Class->GetClass() != USoloASScriptBlueprintGeneratedClass::StaticClass())
            || Class->IsChildOf(UActorComponent::StaticClass()) || Class->HasAnyClassFlags(CLASS_Interface)
            || !Class->GetDefaultObject(false) || Class->GetSparseClassData(EGetSparseClassDataMethod::ReturnIfNull))
        { Error = TEXT("A loaded Blueprint subclass requires a specialized compiler, component or sparse-class migration."); return false; }
        if (CastChecked<UBlueprintGeneratedClass>(Class)->UberGraphFramePointerProperty)
        { Error = TEXT("A loaded Blueprint subclass requires persistent event-graph frame migration; the previous generation is unchanged."); return false; }
#if WITH_EDITORONLY_DATA
        if (auto* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy);
            Blueprint && (Blueprint->Status == BS_Dirty || Blueprint->Status == BS_Error))
        { Error = TEXT("A loaded Blueprint subclass has uncompiled editor graph changes; compile the asset before updating its parent."); return false; }
#endif
        for (TFieldIterator<UFunction> Function(Class); Function; ++Function)
            if (!Function->GetOwnerClass()->HasAnyClassFlags(CLASS_Native)
                && (Function->HasAnyFunctionFlags(FUNC_Net) || HasLatentParameters(*Function)))
            { Error = TEXT("A loaded Blueprint subclass has a latent or network function."); return false; }
        Classes.Add(Class);
    }
    Classes.StableSort([](const UClass& A, const UClass& B)
    {
        auto Depth = [](const UClass* Class) { int32 Count = 0; for (; Class; Class = Class->GetSuperClass()) ++Count; return Count; };
        return Depth(&A) < Depth(&B);
    });
    for (UClass* Class : Classes)
    {
        if (Pending.Subclasses.ContainsByPredicate([&](const auto& Entry)
            { return Entry.CanonicalPackage == Class->GetOutermost()->GetName(); }))
        { Error = TEXT("A Blueprint subclass package has more than one generated asset class."); return false; }
        auto& Subclass = Pending.Subclasses.AddDefaulted_GetRef();
        Subclass.Previous.Reset(Class);
        Subclass.CanonicalPackage = Class->GetOutermost()->GetName();
        Subclass.ClassPath = Class->GetPathName();
        Subclass.Schema = BlueprintImportAbi(Class);
        Affected.Add(Class);
        TArray<UObject*> Metadata;
        SoloASBridge::Compat::GetPackageObjects(Class->GetOutermost(), Metadata);
        for (UObject* Object : Metadata)
        {
            Pending.OwnedBlueprintMetadata.Add(Object);
            Pending.BlueprintMetadataPins.Emplace(Object);
        }
    }
    return true;
}

bool PublishBlueprintSubclassGenerations(FPendingBlueprintGeneration& Pending, FString& Error)
{
    if (Pending.Subclasses.IsEmpty()) return true;
    const ERenameFlags Flags = REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders;
    for (auto& Subclass : Pending.Subclasses)
    {
        UClass* Before = Subclass.Previous.Get();
        UClass* Candidate = Subclass.Candidate.Get();
        UPackage* OldPackage = Before->GetOutermost();
        const FString Retired = TEXT("/Temp/SoloASBlueprintSubclassRetired_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        if (!OldPackage->Rename(*Retired, nullptr, Flags)
            || !Subclass.Package->Rename(*Subclass.CanonicalPackage, nullptr, Flags | REN_Test)
            || !Subclass.Package->Rename(*Subclass.CanonicalPackage, nullptr, Flags))
        { Error = TEXT("Blueprint subclass namespace publication failed after module swap; restart is required."); return false; }
        Before->ClassFlags |= CLASS_NewerVersionExists;
        TArray<UObject*> Published;
        SoloASBridge::Compat::GetPackageObjects(Subclass.Package.Get(), Published);
        for (UObject* Object : Published)
        {
            Object->ClearFlags(RF_Transient);
            if (UObject* const* Source = Pending.BlueprintMetadata.FindKey(Object))
                Object->SetFlags((*Source)->GetFlags() & (RF_Public | RF_Standalone | RF_Transactional));
        }
        if (FindObject<UClass>(nullptr, *Subclass.ClassPath) != Candidate)
        { Error = TEXT("Blueprint subclass canonical lookup did not resolve its published generation; restart is required."); return false; }
        if (FAngelscriptType::GetByClass(Before).IsValid()
            && !RebindAngelscriptObjectViewGeneration(Before, Candidate, Error)) return false;
#if WITH_EDITOR && !IS_MONOLITHIC
        if (auto* Blueprint = Cast<UBlueprint>(Candidate->ClassGeneratedBy))
        {
            const FString CanonicalAsset = Blueprint->GetPathName();
            FAssetRegistryModule::AssetRenamed(Blueprint, CanonicalAsset);
        }
#endif
    }
    // Class/default identities publish only after canonical names are final.
    // Retired object layouts and their metadata are never visited by this pass.
    TSet<UObject*> Retired = Pending.OwnedBlueprintMetadata;
    for (const auto& Previous : Pending.PreviousInstances)
    {
        Retired.Add(Previous.Get());
        TArray<UObject*> Owned;
        SoloASBridge::Compat::GetNestedObjects(Previous.Get(), Owned);
        for (UObject* Object : Owned) Retired.Add(Object);
    }
    for (const auto& Pair : Pending.BlueprintMetadata) Retired.Add(Pair.Key);
    TArray<UObject*> Owners;
    for (TObjectIterator<UObject> It; It; ++It)
        if (IsValid(*It) && !Retired.Contains(*It))
        {
            bool bRetiredOuter = false;
            for (UObject* Outer = It->GetOuter(); Outer; Outer = Outer->GetOuter()) bRetiredOuter |= Retired.Contains(Outer);
            if (!bRetiredOuter) Owners.Add(*It);
        }
    for (UObject* Object : Owners)
        RebindBlueprintGenerationMetadata(Object, Pending.BlueprintMetadata, Pending.BlueprintFields, &Retired, &Pending.PreviousSoftTargets);
    if (GUObjectArray.DisregardForGCEnabled()) BlueprintMetadataRootLifetime().Log(TEXT("publication"));
    else UE_LOG(Angelscript, Display, TEXT("N04_METADATA_ROOTS event=publication acquired=0 released=0 held=0 owner_edges=0 editor_noop=1"));
    return true;
}

bool RefreshFeatureModBlueprintPackageInPlace(const FString& ClassPath, FString& Error)
{
    if (PendingBlueprintGeneration && PendingBlueprintGeneration->ClassPath == ClassPath)
    {
        if (PendingBlueprintGeneration->bCommitAttempted)
        { Error = TEXT("This Blueprint class is locked after a failed generation commit."); return false; }
        return true;
    }
    auto* Lifetime = BlueprintClassLifetimes.Find(ClassPath);
    if (!Lifetime) return true;
    if (!IsInGameThread())
    {
        Error = TEXT("FeatureMod Blueprint update requires the game thread.");
        return false;
    }
    const FString PackageName = FPackageName::ObjectPathToPackageName(ClassPath);
    FString Filename;
    if (!FPackageName::DoesPackageExist(PackageName, &Filename))
    {
        Error = TEXT("The imported FeatureMod Blueprint package is missing on disk.");
        return false;
    }
    const FMD5Hash Hash = BlueprintPackageHash(Filename);
    if (!Hash.IsValid())
    {
        Error = TEXT("The imported FeatureMod Blueprint package could not be read.");
        return false;
    }
    if (Hash == Lifetime->DiskHash) return true;
    static bool bReloading = false;
    if (bReloading)
    {
        Error = TEXT("FeatureMod Blueprint update cannot reenter Blueprint import.");
        return false;
    }
    UPackage* LivePackage = Lifetime->Class->GetOutermost();
    for (FFrame* Frame = FFrame::GetThreadLocalTopStackFrame(); Frame; Frame = Frame->PreviousTrackingFrame)
        if ((Frame->Node && Frame->Node->GetOutermost() == LivePackage)
            || (Frame->Object && Frame->Object->GetClass()->GetOutermost() == LivePackage))
        {
            Error = TEXT("FeatureMod Blueprint update cannot replace an executing Blueprint.");
            return false;
        }
    FAngelscriptExecutionWriteScope Writer;
    TGuardValue<bool> ReloadingGuard(bReloading, true);
    const FString PreviewName = TEXT("/Temp/SoloASBlueprintImport_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TStrongObjectPtr<UPackage> Preview(CreatePackage(*PreviewName));
    bool bPublishedGeneration = false;
    // Keep serialized class flags intact: diff packages force CLASS_Deprecated
    // during UBlueprintGeneratedClass::Serialize, hiding real flag changes.
    ON_SCOPE_EXIT
    {
        if (bPublishedGeneration) return;
        ResetLoaders(Preview.Get());
        TArray<UObject*> Objects;
        SoloASBridge::Compat::GetPackageObjects(Preview.Get(), Objects);
        for (UObject* Object : Objects)
        {
            Object->ClearFlags(RF_Standalone);
            Object->SetFlags(RF_Transient);
        }
        Preview->ClearFlags(RF_Standalone);
        Preview->SetFlags(RF_Transient);
    };
    FLinkerInstancingContext Instancing;
    Instancing.AddPackageMapping(FName(*PackageName), Preview->GetFName());
    if (!LoadPackage(Preview.Get(), *PackageName, LOAD_DisableCompileOnLoad, nullptr, &Instancing))
    {
        Error = TEXT("The cooked FeatureMod Blueprint candidate could not be loaded in isolation.");
        return false;
    }
#if WITH_EDITOR && !IS_MONOLITHIC
    TArray<UObject*> PreviewObjects;
    GetObjectsWithPackage(Preview.Get(), PreviewObjects);
    for (UObject* Object : PreviewObjects)
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
        {
            if (Blueprint->GetClass() != UBlueprint::StaticClass())
            {
                Error = TEXT("This Blueprint compiler requires a separate isolated update preflight.");
                return false;
            }
            FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
            if (Blueprint->Status == BS_Error)
            {
                Error = TEXT("The isolated Actor Blueprint candidate failed compilation; the live graph is unchanged.");
                return false;
            }
        }
#endif
    TArray<UObject*> Candidates;
    SoloASBridge::Compat::GetPackageObjects(Preview.Get(), Candidates);
    TMap<UObject*, UObject*> Objects;
    TMap<FField*, FField*> Fields;
    TArray<TPair<UClass*, UClass*>> Classes;
    TArray<TPair<UFunction*, UFunction*>> Functions;
    Objects.Add(Preview.Get(), LivePackage);
    for (UObject* Object : Candidates)
        if (UClass* Candidate = Cast<UClass>(Object))
        {
#if WITH_EDITOR && !IS_MONOLITHIC
            if (UBlueprint* Blueprint = Cast<UBlueprint>(Candidate->ClassGeneratedBy);
                Blueprint && Blueprint->GeneratedClass != Candidate) continue;
#endif
            UClass* Live = FindObject<UClass>(LivePackage, *Candidate->GetName());
            if (!Live)
            {
                Error = FString::Printf(TEXT("The cooked FeatureMod Blueprint candidate added class '%s'; ")
                    TEXT("no matching class exists in live package '%s'."), *Candidate->GetPathName(), *LivePackage->GetName());
                return false;
            }
            if (!Candidate->HasAnyClassFlags(CLASS_CompiledFromBlueprint) || Candidate->HasAnyClassFlags(CLASS_Interface))
            {
                Error = FString::Printf(TEXT("The cooked FeatureMod Blueprint candidate class '%s' is not an ordinary Blueprint class ")
                    TEXT("(candidate flags=0x%08x, live flags=0x%08x)."),
                    *Candidate->GetPathName(), uint32(Candidate->ClassFlags), uint32(Live->ClassFlags));
                return false;
            }
            if (Candidate->ClassFlags != Live->ClassFlags)
            {
                Error = FString::Printf(TEXT("The cooked FeatureMod Blueprint candidate class '%s' changed its flags ")
                    TEXT("(candidate=0x%08x, live=0x%08x, difference=0x%08x)."),
                    *Candidate->GetPathName(), uint32(Candidate->ClassFlags), uint32(Live->ClassFlags),
                    uint32(Candidate->ClassFlags) ^ uint32(Live->ClassFlags));
                return false;
            }
            Classes.Emplace(Candidate, Live);
            Objects.Add(Candidate, Live);
            Objects.Add(Candidate->GetDefaultObject(), Live->GetDefaultObject());
            for (TFieldIterator<UFunction> It(Candidate, EFieldIteratorFlags::ExcludeSuper); It; ++It)
            {
                UFunction* LiveFunction = Live->FindFunctionByName(It->GetFName(), EIncludeSuperFlag::ExcludeSuper);
                if (!LiveFunction)
                {
                    Error = TEXT("The cooked FeatureMod Blueprint candidate added a function.");
                    return false;
                }
                Objects.Add(*It, LiveFunction);
                Functions.Emplace(*It, LiveFunction);
            }
        }
    auto Reject = [&]()
    {
        Error = TEXT("FeatureMod Blueprint update requires identical class, field, parameter and local layouts, ordinary functions and primitive-only default changes; the live package is unchanged.");
        return false;
    };
    if (Classes.IsEmpty()) return Reject();
    TArray<UObject*> LiveObjects;
    SoloASBridge::Compat::GetPackageObjects(LivePackage, LiveObjects);
    for (UObject* Object : LiveObjects)
    {
#if WITH_EDITOR && !IS_MONOLITHIC
        UClass* Owner = Cast<UClass>(Object);
        if (UFunction* Function = Cast<UFunction>(Object)) Owner = Function->GetOuterUClass();
        if (Owner)
            if (UBlueprint* Blueprint = Cast<UBlueprint>(Owner->ClassGeneratedBy);
                Blueprint && Blueprint->GeneratedClass != Owner) continue;
#endif
        if ((Object->IsA<UClass>() || Object->IsA<UFunction>())
            && !Objects.FindKey(Object)) return Reject();
    }
    TArray<FBlueprintOwnedGraphUpdate> GraphUpdates;
    TMap<UClass*, FBlueprintGenerationSchema> GenerationSchemas;
    bool bNewGeneration = false;
    for (const auto& Pair : Classes)
    {
        if (!BuildBlueprintGenerationSchema(Pair.Value, Pair.Key, Objects,
            GenerationSchemas.FindOrAdd(Pair.Value), Error)) return false;
        bNewGeneration |= BlueprintImportAbi(Pair.Key, &Objects) != BlueprintImportAbi(Pair.Value);
    }
    if (bNewGeneration)
    {
        if (BlueprintPackageHash(Filename) != Hash) return Reject();
        if (PendingBlueprintGeneration || Classes.Num() != 1)
        {
            Error = TEXT("A Blueprint schema compile batch currently requires one ordinary UObject class.");
            return false;
        }
        UClass* Candidate = Classes[0].Key;
        UClass* Previous = Classes[0].Value;
        if (Candidate->GetSuperClass() != Previous->GetSuperClass()
            || Candidate->IsChildOf(UActorComponent::StaticClass())
            || CastChecked<UBlueprintGeneratedClass>(Candidate)->UberGraphFunction
            || Candidate->ClassWithin != Previous->ClassWithin || !Candidate->Interfaces.IsEmpty())
        {
            Error = TEXT("This Blueprint schema generation requires additional owner, parent, interface or persistent-frame migration.");
            return false;
        }
        for (TObjectIterator<UClass> It; It; ++It)
            if (*It != Previous && It->IsChildOf(Previous)
                && !Cast<UASClass>(*It) && !Cast<UBlueprintGeneratedClass>(*It))
            {
                Error = TEXT("A loaded subclass requires an unsupported generation owner.");
                return false;
            }
        const bool bActor = Candidate->IsChildOf(AActor::StaticClass());
        if (bActor && (!Candidate->GetSuperClass()->HasAnyClassFlags(CLASS_Native)
            || Candidate->GetDefaultObject<AActor>()->GetIsReplicated()))
        {
            Error = TEXT("An Actor schema generation requires a non-network Actor with a stable native parent.");
            return false;
        }
        for (TFieldIterator<FProperty> It(Candidate); It; ++It)
            if ((!bActor || (IsBlueprintActorStateField(*It) && !BlueprintActorComponentSlot(Candidate, *It)))
                && It->HasAnyPropertyFlags(CPF_Net))
            {
                Error = TEXT("A network or owned-object field requires an additional schema migration.");
                return false;
            }
        for (TFieldIterator<UFunction> It(Candidate); It; ++It)
            if ((!bActor || !It->GetOuterUClass()->HasAnyClassFlags(CLASS_Native))
                && (HasLatentParameters(*It) || It->HasAnyFunctionFlags(FUNC_Net)))
            {
                Error = TEXT("A latent or network Blueprint function requires an additional schema migration.");
                return false;
            }
        PendingBlueprintGeneration = MakeUnique<FPendingBlueprintGeneration>();
        auto& Pending = *PendingBlueprintGeneration;
        Pending.Package.Reset(Preview.Get());
        Pending.Previous.Reset(Previous);
        Pending.Candidate.Reset(Candidate);
        Pending.ClassPath = ClassPath;
        Pending.Filename = Filename;
        Pending.Hash = Hash;
        Pending.ImportSchemas = BlueprintImportSchemas;
        Pending.View = TEXT("__SoloASBlueprint_") + FMD5::HashAnsiString(*Candidate->GetPathName());
        bPublishedGeneration = true; // The pending owner retains the isolated package.
        return true;
    }
    for (const auto& Pair : Classes)
    {
        if (BlueprintImportAbi(Pair.Key, &Objects) != BlueprintImportAbi(Pair.Value)
            || Pair.Key->ClassWithin != Pair.Value->ClassWithin
            || Pair.Key->Interfaces.Num() != Pair.Value->Interfaces.Num()) return Reject();
        for (int32 Index = 0; Index < Pair.Key->Interfaces.Num(); ++Index)
        {
            const auto& A = Pair.Key->Interfaces[Index];
            const auto& B = Pair.Value->Interfaces[Index];
            UObject* Interface = A.Class;
            if (UObject* const* Mapped = Objects.Find(Interface)) Interface = *Mapped;
            if (Interface != B.Class || A.PointerOffset != B.PointerOffset
                || A.bImplementedByK2 != B.bImplementedByK2) return Reject();
        }
    }
    for (const auto& Pair : Classes)
    {
        auto& Graph = GraphUpdates.AddDefaulted_GetRef();
        if (!PrepareBlueprintOwnedGraph(CastChecked<UBlueprintGeneratedClass>(Pair.Key),
            CastChecked<UBlueprintGeneratedClass>(Pair.Value), Objects, Graph, Error)) return false;
    }
    // Check all fields including locals (the imported VM ABI only records params).
    TFunction<void(FProperty*, FProperty*)> MapNestedFields = [&](FProperty* A, FProperty* B)
    {
        Fields.Add(A, B);
        if (auto* Array = CastField<FArrayProperty>(A))
            MapNestedFields(Array->Inner, CastFieldChecked<FArrayProperty>(B)->Inner);
        if (auto* Set = CastField<FSetProperty>(A))
            MapNestedFields(Set->ElementProp, CastFieldChecked<FSetProperty>(B)->ElementProp);
        if (auto* Map = CastField<FMapProperty>(A))
        {
            auto* LiveMap = CastFieldChecked<FMapProperty>(B);
            MapNestedFields(Map->KeyProp, LiveMap->KeyProp);
            MapNestedFields(Map->ValueProp, LiveMap->ValueProp);
        }
    };
    auto MapFields = [&](UStruct* Candidate, UStruct* Live)
    {
        TArray<FProperty*> Before, After;
        for (TFieldIterator<FProperty> It(Candidate, EFieldIteratorFlags::ExcludeSuper); It; ++It) Before.Add(*It);
        for (TFieldIterator<FProperty> It(Live, EFieldIteratorFlags::ExcludeSuper); It; ++It) After.Add(*It);
        if (Before.Num() != After.Num() || Candidate->GetStructureSize() != Live->GetStructureSize()
            || Candidate->GetMinAlignment() != Live->GetMinAlignment()) return false;
        for (int32 Index = 0; Index < Before.Num(); ++Index)
        {
            FProperty* A = Before[Index];
            FProperty* B = After[Index];
            if (A->GetFName() != B->GetFName() || A->GetClass() != B->GetClass()
                || A->GetOffset_ForInternal() != B->GetOffset_ForInternal()
                || A->GetElementSize() != B->GetElementSize() || A->ArrayDim != B->ArrayDim
                || A->PropertyFlags != B->PropertyFlags) return false;
            // SameType checks nested containers and referenced struct/enum identity.
            // Self object/class properties need the checked candidate-to-live map.
            if (auto* OA = CastField<FObjectPropertyBase>(A))
            {
                auto* OB = CastFieldChecked<FObjectPropertyBase>(B);
                if (Objects.Contains(OA->PropertyClass))
                {
                    if (Objects.FindRef(OA->PropertyClass) != OB->PropertyClass) return false;
                }
                else if (!A->SameType(B)) return false;
                if (auto* CA = CastField<FClassProperty>(A))
                {
                    auto* CB = CastFieldChecked<FClassProperty>(B);
                    UObject* Meta = CA->MetaClass;
                    if (UObject* const* Mapped = Objects.Find(Meta)) Meta = *Mapped;
                    if (Meta != CB->MetaClass) return false;
                }
            }
            else if (!A->SameType(B)) return false;
            if (auto* BA = CastField<FBoolProperty>(A))
            {
                auto* BB = CastFieldChecked<FBoolProperty>(B);
                if (BA->GetFieldMask() != BB->GetFieldMask() || BA->GetByteOffset() != BB->GetByteOffset()) return false;
            }
            MapNestedFields(A, B);
        }
        return true;
    };
    for (const auto& Pair : Classes)
    {
        if (!MapFields(Pair.Key, Pair.Value)) return Reject();
        UObject* NewDefaults = Pair.Key->GetDefaultObject();
        UObject* OldDefaults = Pair.Value->GetDefaultObject();
        if (Pair.Value->IsChildOf(AActor::StaticClass()))
        {
            FBlueprintGenerationReferenceArchive Archive(NewDefaults, Objects, Fields);
        }
        for (TFieldIterator<FProperty> It(Pair.Value); It; ++It)
            if (!It->Identical_InContainer(NewDefaults, OldDefaults)
                && !CastField<FNumericProperty>(*It) && !CastField<FBoolProperty>(*It)
                && !CastField<FEnumProperty>(*It)) return Reject();
    }
    for (const auto& Pair : Functions)
    {
        UFunction* Candidate = Pair.Key;
        UFunction* Live = Pair.Value;
        if (!MapFields(Candidate, Live) || Candidate->FunctionFlags != Live->FunctionFlags
            || Candidate->NumParms != Live->NumParms || Candidate->ParmsSize != Live->ParmsSize
            || Candidate->ReturnValueOffset != Live->ReturnValueOffset || HasLatentParameters(Candidate)
            || (Candidate->HasAnyFunctionFlags(FUNC_UbergraphFunction) && (!Candidate->Script.IsEmpty() || !Live->Script.IsEmpty())))
            return Reject();
    }
    for (const auto& Pair : Functions)
    {
        UFunction* Candidate = Pair.Key;
        FBlueprintBodyReferenceArchive Archive(Preview.Get(), Objects, Fields);
        int32 Offset = 0;
        while (Offset < Candidate->Script.Num() && !Archive.IsError()) Candidate->SerializeExpr(Offset, Archive);
        if (Archive.IsError() || Offset != Candidate->Script.Num()) return Reject();
    }
    for (auto& Graph : GraphUpdates)
        if (!PrepareBlueprintOwnedGraphDefaults(Graph, Objects, Fields))
        {
            Error = TEXT("An Actor template default could not be mapped to its instance ownership graph; the live graph is unchanged.");
            return false;
        }
    if (BlueprintPackageHash(Filename) != Hash) return Reject();
    if (PendingBlueprintGeneration)
    {
        Error = TEXT("A Blueprint compile transaction currently requires one changed package.");
        return false;
    }
    PendingBlueprintGeneration = MakeUnique<FPendingBlueprintGeneration>();
    auto& Pending = *PendingBlueprintGeneration;
    Pending.Package.Reset(Preview.Get());
    Pending.Previous.Reset(Lifetime->Class.Get());
    Pending.Candidate.Reset(Lifetime->Class.Get()); // Import the existing native view.
    Pending.ClassPath = ClassPath;
    Pending.Filename = Filename;
    Pending.Hash = Hash;
    Pending.View = Lifetime->View;
    Pending.ImportSchemas = BlueprintImportSchemas;
    Pending.InPlaceUpdate = MakeUnique<FBlueprintInPlaceUpdate>();
    auto& Update = *Pending.InPlaceUpdate;
    Update.PreviousHash = Lifetime->DiskHash;
    Update.Objects = MoveTemp(Objects);
    Update.Classes = MoveTemp(Classes);
    Update.Functions = MoveTemp(Functions);
    Update.Graphs = MoveTemp(GraphUpdates);
    TSet<UObject*> Targets;
    for (const auto& Pair : Update.Objects)
    {
        if (Pair.Key) Targets.Add(Pair.Key);
        if (Pair.Value) Targets.Add(Pair.Value);
    }
    for (const auto& Graph : Update.Graphs)
        for (const auto& Actor : Graph.Actors)
        {
            Targets.Add(Actor.Object);
            for (const auto& Pair : Actor.Objects) if (Pair.Value) Targets.Add(Pair.Value);
            for (USceneComponent* Child : Actor.Reattach) Targets.Add(Child);
        }
    for (UObject* Target : Targets) Update.Targets.Emplace(Target);
    bPublishedGeneration = true; // Pending owns preparation; no live writes yet.
    return true;
}

#if !WITH_EDITOR || IS_MONOLITHIC
bool RefreshFeatureModBlueprintPackage(const FString& ClassPath, FString& Error)
{
    return RefreshFeatureModBlueprintPackageInPlace(ClassPath, Error);
}
#endif

bool TransferBlueprintGenerationValues(UObject* OldInstance, UObject* NewInstance, FString& Error)
{
    if (!IsInGameThread() || !OldInstance || !NewInstance || OldInstance == NewInstance
        || OldInstance->IsTemplate() || NewInstance->IsTemplate())
    {
        Error = TEXT("Blueprint generation value transfer requires two explicit instances on the game thread.");
        return false;
    }
    if (OldInstance->IsA<AActor>() || NewInstance->IsA<AActor>()
        || OldInstance->IsA<UActorComponent>() || NewInstance->IsA<UActorComponent>())
    {
        Error = TEXT("Explicit Blueprint value transfer supports ordinary UObject instances; Actors and components require their lifecycle migration.");
        return false;
    }
    UClass* Previous = OldInstance->GetClass();
    UClass* Candidate = NewInstance->GetClass();
    const FImportedBlueprintLifetime* Record = nullptr;
    for (const auto& Entry : BlueprintClassLifetimes)
        if (Entry.Value.Class.Get() == Candidate && Entry.Value.RetiredClasses.ContainsByPredicate(
            [&](const auto& Class) { return Class.Get() == Previous; })) { Record = &Entry.Value; break; }
    if (!Record)
    {
        Error = TEXT("The explicit instances do not belong to an admitted old/current Blueprint schema pair.");
        return false;
    }
    TArray<UObject*> Children;
    GetObjectsWithOuter(OldInstance, Children);
    GetObjectsWithOuter(NewInstance, Children);
    if (!Children.IsEmpty() || OldInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
        || NewInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
    {
        Error = TEXT("Blueprint generation value transfer requires live instances without nested objects.");
        return false;
    }
    TMap<UObject*, UObject*> CandidateTypes;
    CandidateTypes.Add(Candidate, Previous);
    FBlueprintGenerationSchema Schema;
    if (!BuildBlueprintGenerationSchema(Previous, Candidate, CandidateTypes, Schema, Error)) return false;
    for (const auto& Property : Schema.Properties)
        if (!Property.Key->SameType(Property.Value))
        {
            Error = TEXT("A field references its previous schema; the caller must handle that reference before value transfer.");
            return false;
        }
    FAngelscriptExecutionWriteScope Writer;
    for (const auto& Property : Schema.Properties)
        if (!Property.Key->Identical_InContainer(OldInstance, Previous->GetDefaultObject()))
            Property.Value->CopyCompleteValue(Property.Value->ContainerPtrToValuePtr<void>(NewInstance),
                Property.Key->ContainerPtrToValuePtr<void>(OldInstance));
    return true;
}

UClass* FindPendingBlueprintClass(const FString& ClassPath)
{
    return PendingBlueprintGeneration && PendingBlueprintGeneration->ClassPath == ClassPath
        ? PendingBlueprintGeneration->Candidate.Get() : nullptr;
}

bool VisitBlueprintGenerationSoftPath(const FPendingBlueprintGeneration& Pending, FSoftObjectPath& Path, UClass* Constraint,
    TFunctionRef<bool(UObject*&, UClass*, bool)> Visit, bool bReplace)
{
    if (Path.IsNull()) return true;
    // Resolve against identities frozen before package/class renaming. Never load
    // an external asset, or let a canonical class path silently change generation.
    UObject* Before = Pending.PreviousSoftTargets.FindRef(Path);
    if (!Before) Before = Path.ResolveObject();
    if (Pending.RejectedSoftTargets.Contains(Before)
        && !Pending.Subclasses.ContainsByPredicate([&](const auto& Subclass)
        {
            return Before == Subclass.Previous.Get()
                || Before == Subclass.Previous->GetDefaultObject(false);
        })) return false;
    if (!Before)
    {
        const FString Text = Path.ToString();
        for (const auto& Target : Pending.PreviousSoftTargets)
        {
            const FString Root = Target.Key.ToString();
            if (Text.StartsWith(Root + TEXT(":"), ESearchCase::IgnoreCase)
                || Text.StartsWith(Root + TEXT("."), ESearchCase::IgnoreCase)) return false;
        }
        return true; // Unresolved external paths retain their exact stored value.
    }
    UObject* After = Before;
    if (!Visit(After, Constraint, false)) return false;
    if (bReplace && Before != After) Path = FSoftObjectPath(After);
    return true;
}

// Visit reflected storage rather than serializing metadata or guessing raw
// pointer owners. Container keys are rehashed during replacement, never preflight.
bool VisitBlueprintGenerationReferences(FPendingBlueprintGeneration& Pending, FProperty* Property, void* Value,
    TFunctionRef<bool(UObject*&, UClass*, bool)> Visit, bool bReplace)
{
    if (CastField<FMulticastSparseDelegateProperty>(Property) && Property->ArrayDim != 1)
    {
        Pending.DelegateError = TEXT("Sparse multicast arrays require separate owner-index storage; publication was not started.");
        return false;
    }
    for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
    {
        void* Element = static_cast<uint8*>(Value) + Index * Property->GetElementSize();
        if (auto* Soft = CastField<FSoftObjectProperty>(Property))
        {
            FSoftObjectPath Path = Soft->GetPropertyValue(Element).ToSoftObjectPath();
            const FSoftObjectPath Before = Path;
            if (!VisitBlueprintGenerationSoftPath(Pending, Path, Soft->PropertyClass, Visit, bReplace)) return false;
            if (Path != Before) Soft->SetPropertyValue(Element, FSoftObjectPtr(Path));
        }
        else if (CastField<FDelegateProperty>(Property) || CastField<FMulticastDelegateProperty>(Property))
        {
            auto* Delegate = CastField<FDelegateProperty>(Property);
            auto* Multicast = CastField<FMulticastDelegateProperty>(Property);
            TArray<FScriptDelegate> Bindings;
            UFunction* Signature = Delegate ? Delegate->SignatureFunction : Multicast->SignatureFunction;
            if (Delegate) Bindings.Add(*Delegate->GetPropertyValuePtr(Element));
            else
            {
                const auto* Storage = Multicast->GetMulticastDelegate(Element);
                if (!Storage) continue;
                // Stock's public serializer stores the ordered script-delegate array.
                // Use it rather than assuming the private invocation-list layout.
                FMulticastScriptDelegate Copy = *Storage;
                TArray<uint8> Bytes;
                FObjectWriter Writer(Bytes);
                Writer << Copy;
                FObjectReader Reader(Bytes);
                Reader.SetCustomVersions(Writer.GetCustomVersions());
                Reader << Bindings;
            }
            bool bChanged = false;
            for (auto& Binding : Bindings)
            {
                UObject* Before = Binding.GetUObject();
                if (!Before) continue;
                auto Reject = [&](const TCHAR* Reason)
                {
                    Pending.DelegateError = FString::Printf(TEXT("%s delegate '%s': %s."),
                        Delegate ? TEXT("Singlecast") : TEXT("Multicast"), *Property->GetName(), Reason);
                    return false;
                };
                if (Pending.RejectedSoftTargets.Contains(Before)) return Reject(TEXT("the target has no instance-specific replacement"));
                UObject* After = Before;
                if (!Visit(After, UObject::StaticClass(), false)) return false;
                if (!Pending.PreviousSoftTargets.Contains(FSoftObjectPath(Before)) && Before == After) continue;
                const FName Name = Binding.GetFunctionName();
                UFunction* Function = Before->FindFunction(Name);
                if (!Function) return Reject(TEXT("an affected target has no reflected handler"));
                const bool bNative = Function->HasAnyFunctionFlags(FUNC_Native)
                    && Function->GetOwnerClass()->HasAnyClassFlags(CLASS_Native);
                if (!Signature || !Function->IsSignatureCompatibleWith(Signature))
                    return Reject(TEXT("an affected target requires a handler with a compatible signature"));
                if (!bNative)
                {
                    const auto* Receiver = Pending.Receivers.FindByPredicate(
                        [&](const auto& Item) { return Item.Previous.Get() == Before->GetClass(); });
                    if (!Receiver) return Reject(TEXT("an affected script or Blueprint target has no prepared receiver"));
                    const auto Previous = Receiver->PreviousDesc->GetMethod(Name.ToString());
                    const auto Candidate = Receiver->CandidateDesc->GetMethod(Name.ToString());
                    bool bSameSignature = Previous && Candidate && Previous->ReturnType == Candidate->ReturnType
                        && Previous->Arguments.Num() == Candidate->Arguments.Num();
                    for (int32 ArgumentIndex = 0; bSameSignature && ArgumentIndex < Previous->Arguments.Num(); ++ArgumentIndex)
                        bSameSignature = Previous->Arguments[ArgumentIndex].IsDefinitionEquivalent(Candidate->Arguments[ArgumentIndex]);
                    if (!bSameSignature || !Previous->IsDefinitionEquivalent(*Candidate))
                        return Reject(TEXT("the candidate script handler was removed or changed its signature or definition"));
                }
                if (Before != After)
                {
                    UFunction* Replacement = After ? After->FindFunction(Name) : nullptr;
                    if (!Replacement || !Replacement->IsSignatureCompatibleWith(Function)
                        || !Replacement->IsSignatureCompatibleWith(Signature) || (bNative && Replacement != Function))
                        return Reject(TEXT("the new target changed the admitted handler or signature"));
                    if (bReplace) { Binding.BindUFunction(After, Name); bChanged = true; }
                }
            }
            if (!bChanged) continue;
            if (Delegate) *Delegate->GetPropertyValuePtr(Element) = Bindings[0];
            else
            {
                TArray<uint8> Bytes;
                FObjectWriter Writer(Bytes);
                Writer << Bindings;
                FMulticastScriptDelegate Copy;
                FObjectReader Reader(Bytes);
                Reader.SetCustomVersions(Writer.GetCustomVersions());
                Reader << Copy;
                Multicast->SetMulticastDelegate(Element, MoveTemp(Copy));
            }
        }
        else if (auto* Object = CastField<FObjectPropertyBase>(Property))
        {
            UObject* Before = Object->GetObjectPropertyValue(Element);
            UObject* After = Before;
            if (!Visit(After, Object->PropertyClass, false)) return false;
            if (Before != After) Object->SetObjectPropertyValue(Element, After);
        }
        else if (auto* Interface = CastField<FInterfaceProperty>(Property))
        {
            auto* Storage = static_cast<FScriptInterface*>(Element);
            UObject* Before = Storage->GetObject();
            UObject* After = Before;
            if (!Visit(After, Interface->InterfaceClass, true)) return false;
            if (Before != After)
            {
                Storage->SetObject(After);
                Storage->SetInterface(After ? After->GetInterfaceAddress(Interface->InterfaceClass) : nullptr);
            }
        }
        else if (auto* Struct = CastField<FStructProperty>(Property))
        {
            if (Struct->Struct == FInstancedStruct::StaticStruct())
            {
                auto& Instance = *static_cast<FInstancedStruct*>(Element);
                if (!Instance.IsValid()) continue;
                for (TFieldIterator<FProperty> Field(Instance.GetScriptStruct()); Field; ++Field)
                    if (!VisitBlueprintGenerationReferences(Pending, *Field,
                        Field->ContainerPtrToValuePtr<void>(Instance.GetMutableMemory()), Visit, bReplace)) return false;
                continue;
            }
            if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get()
                || Struct->Struct == TBaseStructure<FSoftClassPath>::Get())
            {
                if (!VisitBlueprintGenerationSoftPath(Pending, *static_cast<FSoftObjectPath*>(Element),
                    UObject::StaticClass(), Visit, bReplace)) return false;
                continue;
            }
            for (TFieldIterator<FProperty> Field(Struct->Struct); Field; ++Field)
                if (!VisitBlueprintGenerationReferences(Pending, *Field, Field->ContainerPtrToValuePtr<void>(Element), Visit, bReplace)) return false;
        }
        else if (auto* Array = CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper Storage(Array, Element);
            for (int32 Item = 0; Item < Storage.Num(); ++Item)
                if (!VisitBlueprintGenerationReferences(Pending, Array->Inner, Storage.GetRawPtr(Item), Visit, bReplace)) return false;
        }
        else if (auto* Map = CastField<FMapProperty>(Property))
        {
            FScriptMapHelper Storage(Map, Element);
            for (int32 Item = 0; Item < Storage.GetMaxIndex(); ++Item)
                if (Storage.IsValidIndex(Item)
                    && (!VisitBlueprintGenerationReferences(Pending, Map->KeyProp, Storage.GetKeyPtr(Item), Visit, bReplace)
                        || !VisitBlueprintGenerationReferences(Pending, Map->ValueProp, Storage.GetValuePtr(Item), Visit, bReplace))) return false;
            if (bReplace) Storage.Rehash();
        }
        else if (auto* Set = CastField<FSetProperty>(Property))
        {
            FScriptSetHelper Storage(Set, Element);
            for (int32 Item = 0; Item < Storage.GetMaxIndex(); ++Item)
                if (Storage.IsValidIndex(Item)
                    && !VisitBlueprintGenerationReferences(Pending, Set->ElementProp, Storage.GetElementPtr(Item), Visit, bReplace)) return false;
            if (bReplace) Storage.Rehash();
        }
    }
    return true;
}

bool IsBlueprintGenerationInstance(UObject* Object)
{
    return IsValid(Object) && !Object->IsTemplate()
        && !Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
        && !Object->IsA<UField>() && !Object->IsA<UPackage>();
}

// Native constructors establish this complete graph before deferred Construction.
// Match its template identities and edges rather than enumerating component classes.
USceneComponent* BlueprintNativeSceneRoot(AActor* Actor)
{
    if (!Actor) return nullptr;
    USceneComponent* Root = Actor->GetRootComponent();
    if (!Root) return nullptr;
    const auto& Components = Actor->GetComponents();
    TArray<UObject*> Owned;
    SoloASBridge::Compat::GetNestedObjects(Actor, Owned);
    if (Owned.Num() != Components.Num()) return nullptr;
    for (UActorComponent* Component : Components)
    {
        if (!IsValid(Component) || !Component->GetClass()->HasAnyClassFlags(CLASS_Native)
            || Component->GetOuter() != Actor || !Component->HasAnyFlags(RF_DefaultSubObject)
            || Component->CreationMethod != EComponentCreationMethod::Native
            || Component->GetIsReplicated() || !Owned.Contains(Component)) return nullptr;
        if (auto* Scene = Cast<USceneComponent>(Component))
        {
            TSet<USceneComponent*> Visited;
            for (USceneComponent* Parent = Scene; Parent != Root; Parent = Parent->GetAttachParent())
                if (!Parent || !Components.Contains(Parent) || Visited.Contains(Parent)) return nullptr;
                else Visited.Add(Parent);
            for (USceneComponent* Child : Scene->GetAttachChildren())
                if (!Components.Contains(Child) && Child != Child->GetOwner()->GetRootComponent()) return nullptr;
        }
    }
    return Components.Contains(Root) ? Root : nullptr;
}

bool BlueprintNativeGraphMatches(AActor* Actor, AActor* Template)
{
    auto* Root = BlueprintNativeSceneRoot(Actor);
    auto* TemplateRoot = BlueprintNativeSceneRoot(Template);
    if (!Root || !TemplateRoot || Root->GetFName() != TemplateRoot->GetFName()
        || Actor->GetComponents().Num() != Template->GetComponents().Num()) return false;
    for (UActorComponent* Component : Actor->GetComponents())
    {
        auto* Match = FindObject<UActorComponent>(Template, *Component->GetName());
        if (!Match || Match->GetClass() != Component->GetClass()) return false;
        bool bArchetype = Component == Match;
        for (UObject* Archetype = Component->GetArchetype(); !bArchetype && Archetype;
            Archetype = Archetype->GetArchetype())
        {
            bArchetype = Archetype == Match;
            if (Archetype == Archetype->GetArchetype()) break;
        }
        if (!bArchetype) return false;
        if (auto* Scene = Cast<USceneComponent>(Component))
        {
            auto* Other = CastChecked<USceneComponent>(Match);
            const FName Parent = Scene->GetAttachParent() ? Scene->GetAttachParent()->GetFName() : NAME_None;
            const FName OtherParent = Other->GetAttachParent() ? Other->GetAttachParent()->GetFName() : NAME_None;
            if (Scene != Root && (Parent != OtherParent || Scene->GetAttachSocketName() != Other->GetAttachSocketName())) return false;
        }
    }
    return true;
}

bool PrepareBlueprintActorGeneration(FPendingBlueprintGeneration& Pending, FString& Error)
{
    auto Reject = [&](const TCHAR* Reason)
    {
        Error = FString::Printf(TEXT("Actor schema preflight failed: %s; no Actor was spawned or retired."), Reason);
        return false;
    };
    if (!Pending.bScriptParentOnly && Pending.Previous->IsChildOf(AActor::StaticClass()))
    {
        auto* Before = CastChecked<UBlueprintGeneratedClass>(Pending.Previous.Get());
        auto* After = CastChecked<UBlueprintGeneratedClass>(Pending.Candidate.Get());
        if (!Before->SimpleConstructionScript || !After->SimpleConstructionScript)
            return Reject(TEXT("missing construction script"));
        const auto& OldNodes = Before->SimpleConstructionScript->GetAllNodes();
        const auto& NewNodes = After->SimpleConstructionScript->GetAllNodes();
        if (OldNodes.Num() != NewNodes.Num()) return Reject(TEXT("component topology changes need a separate schema migration"));
        for (USCS_Node* Old : OldNodes)
        {
            USCS_Node* const* New = NewNodes.FindByPredicate([&](USCS_Node* Node)
                { return Node->GetVariableName() == Old->GetVariableName(); });
            if (!New || !Old->ComponentTemplate || !(*New)->ComponentTemplate
                || Old->ComponentClass != (*New)->ComponentClass || (*New)->ComponentTemplate->GetIsReplicated()
                || Old->ParentComponentOrVariableName != (*New)->ParentComponentOrVariableName
                || Old->ParentComponentOwnerClassName != (*New)->ParentComponentOwnerClassName
                || Old->bIsParentComponentNative != (*New)->bIsParentComponentNative
                || Old->GetChildNodes().Num() != (*New)->GetChildNodes().Num())
                return Reject(TEXT("component identity, replication or parent changes need a separate migration"));
            for (USCS_Node* Child : Old->GetChildNodes())
                if (!(*New)->GetChildNodes().ContainsByPredicate([&](USCS_Node* Node)
                    { return Node->GetVariableName() == Child->GetVariableName(); }))
                    return Reject(TEXT("component attachment topology changed"));
            Pending.ActorTemplates.Add(Old->ComponentTemplate, (*New)->ComponentTemplate);
        }
    }
    // Receiver CDOs have their own default subobjects, outside the imported
    // Blueprint class graph. None can be mapped to a particular new instance.
    TSet<UObject*> PreviousDefaults;
    TArray<UClass*> PreviousClasses;
    if (!Pending.bScriptParentOnly) PreviousClasses.Add(Pending.Previous.Get());
    for (const auto& Receiver : Pending.Receivers) PreviousClasses.Add(Receiver.Previous.Get());
    for (UClass* PreviousClass : PreviousClasses)
    {
        UObject* Defaults = PreviousClass->GetDefaultObject();
        PreviousDefaults.Add(Defaults);
        TArray<UObject*> Subobjects;
        SoloASBridge::Compat::GetNestedObjects(Defaults, Subobjects);
        for (UObject* Subobject : Subobjects) PreviousDefaults.Add(Subobject);
    }
    for (const auto& Instance : Pending.PreviousInstances)
        if (AActor* Actor = Cast<AActor>(Instance.Get()))
        {
            UWorld* World = Actor->GetWorld();
            if (!World || !IsValid(Actor->GetLevel()) || World->bIsTearingDown
                || World->GetWorldSettings() == Actor || Actor->GetIsReplicated()
                || Actor->GetLocalRole() != ROLE_Authority || Actor->IsActorBeingDestroyed()
                || Actor->IsActorBeginningPlay() || (Actor->GetParentActor()
                    && !Pending.ChildActors.ContainsByPredicate([&](const auto& Child) { return Child.Previous.Get() == Actor; })))
                return Reject(TEXT("the World, network role or Actor lifecycle does not permit stock replacement"));
            auto& Plan = Pending.Actors.AddDefaulted_GetRef();
            Plan.Previous.Reset(Actor);
            Plan.World.Reset(World);
            Plan.Level.Reset(Actor->GetLevel());
            Plan.Transform = Actor->GetActorTransform();
            Plan.Owner = Actor->GetOwner();
            Plan.Instigator = Actor->GetInstigator();
            if (Pending.Instancing->RequiresMigration(Actor))
            {
                TArray<UObject*> Owned;
                SoloASBridge::Compat::GetNestedObjects(Actor, Owned);
                for (UObject* Object : Owned)
                    if (Pending.Instancing->Contains(Object))
                    {
                        Pending.ActorComponents.Add(Object);
                        Pending.NativeActorComponents.Add(Object);
                    }
                continue;
            }
            USceneComponent* NativeRoot = BlueprintNativeSceneRoot(Actor);
            if (NativeRoot)
            {
                UClass* NativeParent = Actor->GetClass();
                while (NativeParent && !NativeParent->HasAnyClassFlags(CLASS_Native))
                    NativeParent = NativeParent->GetSuperClass();
                auto* SourceRoot = BlueprintNativeSceneRoot(Cast<AActor>(Actor->GetClass()->GetDefaultObject()));
                auto* ParentRoot = NativeParent ? BlueprintNativeSceneRoot(Cast<AActor>(NativeParent->GetDefaultObject())) : nullptr;
                if (!SourceRoot || !ParentRoot
                    || !BlueprintNativeGraphMatches(Actor, CastChecked<AActor>(Actor->GetClass()->GetDefaultObject()))
                    || !BlueprintNativeGraphMatches(CastChecked<AActor>(Actor->GetClass()->GetDefaultObject()),
                        CastChecked<AActor>(NativeParent->GetDefaultObject())))
                    return Reject(TEXT("a native default graph does not match its unchanged native parent CDO"));
                Plan.NativeRoot.Reset(NativeRoot);

                // Stock instance data can omit editable native roots in Editor.
                // Preserve the same archetype business delta independently of
                // that editor visibility gate; transforms stay on the stock path.
                auto* SourceDefaults = CastChecked<AActor>(Actor->GetClass()->GetDefaultObject());
                // Receiver candidate classes are generated only after module swap.
                // Admission uses the unchanged native parent; commit validates each
                // generated receiver CDO before spawning its replacement.
                auto* TargetDefaults = CastChecked<AActor>(NativeParent->GetDefaultObject());
                TFunction<bool(FProperty*, void*)> SupportsDelta = [&](FProperty* Property, void* Value)
                {
                    if (CastField<FDelegateProperty>(Property) || CastField<FMulticastDelegateProperty>(Property))
                        return true; // The reference visitor validates every binding.
                    for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
                    {
                        void* Element = static_cast<uint8*>(Value) + Index * Property->GetElementSize();
                        if (auto* Object = CastField<FObjectPropertyBase>(Property))
                        {
                            if (!Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference)) continue;
                            UObject* Reference = Object->GetObjectPropertyValue(Element);
                            if (!Reference) continue;
                            auto* Component = Cast<UActorComponent>(Reference);
                            auto* Owner = Component ? Cast<AActor>(Component->GetOuter()) : nullptr;
                            if (!Owner || !Pending.PreviousInstances.ContainsByPredicate(
                                [&](const auto& Instance) { return Instance.Get() == Owner; })) return false;
                            UClass* Parent = Owner->GetClass();
                            while (Parent && !Parent->HasAnyClassFlags(CLASS_Native)) Parent = Parent->GetSuperClass();
                            // Every owner is admitted independently below; the complete
                            // native instance map exists before any delta is copied.
                            auto* Defaults = Owner == Actor ? TargetDefaults : Parent->GetDefaultObject();
                            auto* Target = FindObject<UActorComponent>(Defaults, *Reference->GetName());
                            if (!Owner->GetComponents().Contains(Component)
                                || !Component->HasAnyFlags(RF_DefaultSubObject) || Component->CreationMethod != EComponentCreationMethod::Native
                                || !Target || Target->GetClass() != Component->GetClass() || !Target->IsA(Object->PropertyClass)) return false;
                        }
                        else if (auto* Struct = CastField<FStructProperty>(Property))
                        {
                            if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get()
                                || Struct->Struct == TBaseStructure<FSoftClassPath>::Get()) continue;
                            // Native structs may carry unreflected runtime internals.
                            if (Struct->Struct->StructFlags & STRUCT_Native) return false;
                            for (TFieldIterator<FProperty> Field(Struct->Struct); Field; ++Field)
                                if (Field->HasAnyPropertyFlags(CPF_Transient)
                                    || !SupportsDelta(*Field, Field->ContainerPtrToValuePtr<void>(Element))) return false;
                        }
                        else if (auto* Array = CastField<FArrayProperty>(Property))
                        {
                            FScriptArrayHelper Storage(Array, Element);
                            for (int32 Item = 0; Item < Storage.Num(); ++Item)
                                if (!SupportsDelta(Array->Inner, Storage.GetRawPtr(Item))) return false;
                        }
                        else if (auto* Map = CastField<FMapProperty>(Property))
                        {
                            FScriptMapHelper Storage(Map, Element);
                            for (int32 Item = 0; Item < Storage.GetMaxIndex(); ++Item)
                                if (Storage.IsValidIndex(Item)
                                    // UHT propagates value instancing flags onto scalar FName keys.
                                    && ((!CastField<FNameProperty>(Map->KeyProp)
                                            && !SupportsDelta(Map->KeyProp, Storage.GetKeyPtr(Item)))
                                        || !SupportsDelta(Map->ValueProp, Storage.GetValuePtr(Item)))) return false;
                        }
                        else if (auto* Set = CastField<FSetProperty>(Property))
                        {
                            FScriptSetHelper Storage(Set, Element);
                            for (int32 Item = 0; Item < Storage.GetMaxIndex(); ++Item)
                                if (Storage.IsValidIndex(Item) && !SupportsDelta(Set->ElementProp, Storage.GetElementPtr(Item))) return false;
                        }
                        else if (Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference)) return false;
                    }
                    return true;
                };
                for (UActorComponent* Component : Actor->GetComponents())
                {
                    auto& State = Plan.NativeGraph.AddDefaulted_GetRef();
                    State.Previous.Reset(Component);
                    Pending.NativeActorComponents.Add(Component);
                    for (TFieldIterator<FProperty> Field(Component->GetClass()); Field; ++Field)
                    {
                        const FName Name = Field->GetFName();
                        if (!Field->HasAnyPropertyFlags(CPF_Edit | CPF_Interp) || Field->HasAnyPropertyFlags(CPF_Transient)
                            || Name == USceneComponent::GetRelativeLocationPropertyName()
                            || Name == USceneComponent::GetRelativeRotationPropertyName()
                            || Name == USceneComponent::GetRelativeScale3DPropertyName()
                            || Field->Identical_InContainer(Component, Component->GetArchetype())) continue;
                        if (!SupportsDelta(*Field, Field->ContainerPtrToValuePtr<void>(Component)))
                        {
                            Error = FString::Printf(TEXT("Actor schema preflight failed: native component property '%s' requires new instancing or unsupported native state; no Actor was spawned or retired."), *Field->GetName());
                            return false;
                        }
                        State.Properties.Add(*Field);
                    }
                }
            }
            for (UActorComponent* Component : Actor->GetComponents())
            {
                if (Pending.NativeActorComponents.Contains(Component))
                {
                    Pending.ActorComponents.Add(Component);
                    continue;
                }
                if (!IsValid(Component) || Component->GetOuter() != Actor || Component->GetIsReplicated()
                    || Component->CreationMethod != EComponentCreationMethod::SimpleConstructionScript
                    || !Pending.ActorTemplates.Contains(Component->GetArchetype()))
                    return Reject(TEXT("a component has no stock SCS instance-cache migration"));
                TArray<UObject*> Nested;
                SoloASBridge::Compat::GetNestedObjects(Component, Nested);
                // CAC-owned dead orphans have no live state; stock recreates its active template graph.
                if (auto* Child = Cast<UChildActorComponent>(Component))
                {
                    AActor* Template = Child->GetChildActorTemplate();
                    Nested.RemoveAll([&](UObject* Object)
                    {
                        const bool bOrphan = !IsValid(Object) && Object->GetOuter() == Child;
                        const bool bTemplate = IsValid(Template) && Template->IsTemplate()
                            && Template->GetClass() == Child->GetChildActorClass()
                            && (Object == Template || Object->IsIn(Template));
                        if (!bOrphan && !bTemplate) return false;
                        Pending.ActorComponents.Add(Object);
                        return true;
                    });
                }
                TSet<UObject*> Internals, Rebuildable;
                for (UObject* Object : Nested)
                {
                    if (!Object->GetClass()->HasAnyClassFlags(CLASS_Native)
                        || !Object->HasAnyFlags(RF_Transient)
                        || Object->HasAnyFlags(RF_Public | RF_Standalone | RF_DefaultSubObject | RF_ArchetypeObject))
                        { Error = FString::Printf(TEXT("Actor schema preflight failed: component '%s' owns unsupported state '%s' (class=%s flags=0x%08x); no Actor was spawned or retired."), *Component->GetName(), *Object->GetPathName(), *Object->GetClass()->GetName(), uint32(Object->GetFlags())); return false; }
                    Internals.Add(Object);
                }
                // Native transient fields describe runtime-generated component
                // internals. Stock construction recreates them; never copy them.
                TFunction<bool(FProperty*, void*, bool, bool)> CheckInternalReferences =
                    [&](FProperty* Property, void* Value, bool bNative, bool bTransient)
                {
                    bTransient |= Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient);
                    if (auto* Struct = CastField<FStructProperty>(Property))
                    {
                        for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
                            for (TFieldIterator<FProperty> Field(Struct->Struct); Field; ++Field)
                                if (!CheckInternalReferences(*Field,
                                    Field->ContainerPtrToValuePtr<void>(static_cast<uint8*>(Value) + Index * Property->GetElementSize()),
                                    bNative && (Struct->Struct->StructFlags & STRUCT_Native), bTransient)) return false;
                        return true;
                    }
                    return VisitBlueprintGenerationReferences(Pending, Property, Value,
                        [&](UObject*& Value, UClass*, bool)
                        {
                            if (!Internals.Contains(Value)) return true;
                            if (!bNative || !bTransient) return false;
                            Rebuildable.Add(Value);
                            return true;
                        });
                };
                TArray<UObject*> Sources = Nested;
                Sources.Add(Component);
                for (UObject* Source : Sources)
                    for (TFieldIterator<FProperty> Field(Source->GetClass()); Field; ++Field)
                        if (!CheckInternalReferences(*Field, Field->ContainerPtrToValuePtr<void>(Source),
                            Field->GetOwnerClass() && Field->GetOwnerClass()->HasAnyClassFlags(CLASS_Native), false))
                            return Reject(TEXT("a component internal is reachable through a persistent or script field"));
                if (Rebuildable.Num() != Internals.Num())
                    return Reject(TEXT("a component internal has no native transient reflected owner"));
                for (UObject* Object : Internals) Pending.ActorComponents.Add(Object);
                Pending.ActorComponents.Add(Component);
            }
            TArray<UObject*> Owned;
            GetObjectsWithOuter(Actor, Owned);
            for (UObject* Object : Owned)
                if (!Pending.ActorComponents.Contains(Object))
                    return Reject(TEXT("an Actor owns a non-component object"));
            Plan.Components = MakeUnique<FComponentInstanceDataCache>(Actor);
            FBlueprintActorCacheReferences References(Plan.CacheReferences);
            Plan.Components->AddReferencedObjects(References);
            for (auto& State : Plan.NativeGraph)
            {
                State.Data = State.Previous->GetComponentInstanceData();
                State.Data->AddReferencedObjects(References);
            }
        }
    auto ReferencesSupported = [&](UObject* Object, bool bActor)
    {
        for (TFieldIterator<FProperty> Field(Object->GetClass()); Field; ++Field)
        {
            if (bActor && (!IsBlueprintActorStateField(*Field) || BlueprintActorComponentSlot(Object->GetClass(), *Field))) continue;
            if (!VisitBlueprintGenerationReferences(Pending, *Field, Field->ContainerPtrToValuePtr<void>(Object),
                [&](UObject*& Value, UClass*, bool)
                {
                    if (PreviousDefaults.Contains(Value)) return false;
                    if (Pending.Instancing->Contains(Value) || Pending.NativeActorComponents.Contains(Value)
                        || (Value && Value == BlueprintNativeSceneRoot(Cast<AActor>(Object)))) return true;
                    return !Value || (!Pending.ActorComponents.Contains(Value)
                        && (Pending.bScriptParentOnly || (!Value->IsIn(Pending.Previous.Get())
                            && !Value->IsIn(Pending.Candidate.Get()))));
                })) return false;
        }
        return true;
    };
    if (!Pending.bScriptParentOnly && !ReferencesSupported(Pending.Candidate->GetDefaultObject(), true))
        return Reject(TEXT("a business default references owned components or template internals"));
    for (const auto& Instance : Pending.PreviousInstances)
        if (!ReferencesSupported(Instance.Get(), Instance->IsA<AActor>()))
            return Reject(TEXT("a business field needs a post-SCS internal-object reference migration"));
    for (auto& Plan : Pending.Actors)
        for (auto& State : Plan.NativeGraph)
        for (FProperty* Property : State.Properties)
            if (!VisitBlueprintGenerationReferences(Pending, Property, Property->ContainerPtrToValuePtr<void>(State.Previous.Get()),
                [&](UObject*& Value, UClass*, bool)
                {
                    if (!Value) return true;
                    // Check targets through every reflected object/interface and
                    // container route, regardless of instanced-property flags.
                    if (PreviousDefaults.Contains(Value)) return false;
                    if ((Pending.ActorComponents.Contains(Value) && !Pending.NativeActorComponents.Contains(Value))
                        || (!Pending.bScriptParentOnly
                            && (Value->IsIn(Pending.Previous.Get()) || Value->IsIn(Pending.Candidate.Get()))
                            && !Pending.ActorTemplates.Contains(Value))) return false;
                    Plan.CacheReferences.Emplace(Value);
                    return true;
                })) return Reject(TEXT("a native root business delta references an unsupported component or template internal"));
    for (const auto& Plan : Pending.Actors)
        for (const auto& Reference : Plan.CacheReferences)
            if (Pending.ActorComponents.Contains(Reference.Get()) && !Pending.NativeActorComponents.Contains(Reference.Get()))
                return Reject(TEXT("a cached component override references a component that does not exist until SCS"));
    return true;
}

bool PrepareBlueprintReceiverMigration(FPendingBlueprintGeneration& Pending,
    const TArray<TSharedRef<FAngelscriptModuleDesc>>& Modules, FString& Error)
{
    auto& Manager = FAngelscriptManager::Get();
    TMap<asITypeInfo*, asITypeInfo*> ScriptTypes;
    TSet<UClass*> Affected;
    if (!Pending.bScriptParentOnly) Affected.Add(Pending.Previous.Get());
    // The Runtime prepares an independent identity for every class in this
    // compiled closure, including otherwise-soft siblings without Blueprint fields.
    // Each old instance therefore needs the same schema and holder ownership.
    for (const auto& Module : Modules)
    {
        const auto PreviousModule = Manager.GetModule(Module->ModuleName);
        if (!PreviousModule.IsValid()) continue;
        for (const auto& Previous : PreviousModule->Classes)
        {
            if (!Previous->Class || Previous->bIsStruct || Previous->bIsStaticsClass
                || Affected.Contains(Previous->Class)) continue;
            const auto Candidate = Module->GetClass(Previous->ClassName);
            if (!Candidate.IsValid())
            {
                Error = FString::Printf(TEXT("Receiver '%s::%s' was removed from the candidate compile closure."),
                    *Module->ModuleName, *Previous->ClassName);
                return false;
            }
            if (Previous->Class->IsChildOf(UActorComponent::StaticClass()) || Previous->bAbstract != Candidate->bAbstract
                || Previous->SuperClass != Candidate->SuperClass)
            {
                Error = FString::Printf(TEXT("Receiver '%s::%s' requires component, abstract or hierarchy migration."),
                    *Module->ModuleName, *Previous->ClassName);
                return false;
            }
            auto& Receiver = Pending.Receivers.AddDefaulted_GetRef();
            Receiver.PreviousDesc = Previous;
            Receiver.CandidateDesc = Candidate;
            Receiver.Previous.Reset(Previous->Class);
            Receiver.Schema = BlueprintImportAbi(Previous->Class, nullptr, false);
            ScriptTypes.Add(Previous->ScriptType, Candidate->ScriptType);
            Affected.Add(Previous->Class);
        }
    }
    if (!GatherBlueprintSubclassGenerations(Pending, Affected, Error)) return false;
    TFunction<bool(const FAngelscriptTypeUsage&, const FAngelscriptTypeUsage&)> Compatible =
        [&](const auto& Before, const auto& After)
    {
        if (Before == After) return true;
        if (Before.bIsConst != After.bIsConst || Before.bIsReference != After.bIsReference) return false;
        if (!Pending.bScriptParentOnly && Before.GetClass() == Pending.Previous.Get()
            && After.GetClass() == Pending.Candidate.Get()) return true;
        if (asITypeInfo* const* Replacement = ScriptTypes.Find(Before.ScriptClass);
            Replacement && *Replacement == After.ScriptClass) return true;
        if (Before.Type != After.Type || Before.SubTypes.IsEmpty() || Before.SubTypes.Num() != After.SubTypes.Num()) return false;
        auto Normalized = After;
        Normalized.SubTypes = Before.SubTypes;
        if (Before != Normalized) return false;
        for (int32 Index = 0; Index < Before.SubTypes.Num(); ++Index)
            if (!Compatible(Before.SubTypes[Index], After.SubTypes[Index])) return false;
        return true;
    };
    for (auto& Receiver : Pending.Receivers)
    {
        for (const auto& Property : Receiver.PreviousDesc->Properties)
        {
            const auto Candidate = Receiver.CandidateDesc->GetProperty(Property->PropertyName);
            if (!Candidate.IsValid() || !Compatible(Property->PropertyType, Candidate->PropertyType)
                || !Property->IsDefinitionEquivalent(*Candidate)
                || Property->GetUnrealPropertyName() != Candidate->GetUnrealPropertyName())
            {
                Error = TEXT("A Blueprint receiver field has no compatible candidate before module swap.");
                return false;
            }
        }
        auto* Before = Receiver.PreviousDesc->ScriptType;
        auto* After = Receiver.CandidateDesc->ScriptType;
        for (asUINT Index = 0; Before && Index < Before->GetPropertyCount(); ++Index)
        {
            const char* Name = nullptr;
            int Offset = 0;
            Before->GetProperty(Index, &Name, nullptr, nullptr, nullptr, &Offset);
            const auto Reflected = Receiver.PreviousDesc->GetProperty(ANSI_TO_TCHAR(Name));
            if ((Reflected.IsValid() && Reflected->bHasUnrealProperty)
                || FindFProperty<FProperty>(Receiver.Previous.Get(), ANSI_TO_TCHAR(Name))) continue;
            const auto Type = FAngelscriptTypeUsage::FromProperty(Before, Index);
            int CandidateIndex = -1, CandidateOffset = 0;
            for (asUINT Other = 0; After && Other < After->GetPropertyCount(); ++Other)
            {
                const char* OtherName = nullptr;
                After->GetProperty(Other, &OtherName, nullptr, nullptr, nullptr, &CandidateOffset);
                if (FCStringAnsi::Strcmp(Name, OtherName) == 0) { CandidateIndex = int(Other); break; }
            }
            if (CandidateIndex < 0 || Type != FAngelscriptTypeUsage::FromProperty(After, CandidateIndex)
                || !Type.CanCopy() || !Type.CanCompareForHotReload() || Type.HasReferences())
            {
                Error = TEXT("An unreflected receiver field has no safe value-copy contract before module swap.");
                return false;
            }
            Receiver.Values.Add({Type, Offset, CandidateOffset});
        }
        TFunction<void(FField*)> RememberTypes = [&](FField* Field)
        {
            if (auto* Object = CastField<FObjectPropertyBase>(Field)) Receiver.TypedFields.Emplace(Object, Object->PropertyClass);
            TArray<FField*> Inner;
            Field->GetInnerFields(Inner);
            for (FField* Item : Inner) RememberTypes(Item);
        };
        for (TFieldIterator<FProperty> Field(Receiver.Previous.Get()); Field; ++Field) RememberTypes(*Field);
        for (TFieldIterator<UFunction> Function(Receiver.Previous.Get()); Function; ++Function)
            for (TFieldIterator<FProperty> Parameter(*Function); Parameter; ++Parameter) RememberTypes(*Parameter);
    }
    TSet<UObject*> Instances;
    for (TObjectIterator<UObject> It; It; ++It)
        if (IsBlueprintGenerationInstance(*It) && Affected.Contains(It->GetClass()))
        {
            Instances.Add(*It);
            Pending.PreviousInstances.Emplace(*It);
        }
    for (UObject* Instance : Instances)
        if (!Instance->IsA<AActor>() && !Cast<UPackage>(Instance->GetOuter()) && !Instances.Contains(Instance->GetOuter())
            && Pending.Subclasses.ContainsByPredicate([&](const auto& Subclass) { return Subclass.Previous.Get() == Instance->GetClass(); }))
        { Error = TEXT("A Blueprint subclass instance has an unprepared non-package Outer."); return false; }
    TArray<UObject*> InstancingRoots = Instances.Array();
    if (!GatherBlueprintChildActors(InstancingRoots, Affected, Pending.ChildActors, Error)) return false;
    for (UObject* Root : InstancingRoots)
        if (!Instances.Contains(Root))
        {
            Instances.Add(Root);
            Pending.PreviousInstances.Emplace(Root);
        }
    // Templates outside the prepared metadata/default closure cannot have their
    // object class changed in place when an inherited storage layout grows.
    for (TObjectIterator<UObject> It; !Pending.Subclasses.IsEmpty() && It; ++It)
        if (IsValid(*It) && It->IsTemplate() && Affected.Contains(It->GetClass())
            && !It->HasAnyFlags(RF_ClassDefaultObject) && !Pending.OwnedBlueprintMetadata.Contains(*It))
        {
            bool bOwnedDefault = false;
            for (UObject* Outer = It->GetOuter(); Outer; Outer = Outer->GetOuter())
                bOwnedDefault |= Outer->HasAnyFlags(RF_ClassDefaultObject) && Affected.Contains(Outer->GetClass());
            if (!bOwnedDefault)
            {
                Error = FString::Printf(TEXT("An affected archetype is outside the prepared class/default closure. Object=%s Class=%s Outer=%s Flags=0x%08x"),
                    *It->GetPathName(), *It->GetClass()->GetPathName(), *GetPathNameSafe(It->GetOuter()), uint32(It->GetFlags()));
                return false;
            }
        }
    // Snapshot all affected identities before admission and publication can rename
    // them. Defaults and classes have no instance-specific migration target.
    auto RememberSoftGraph = [&](UObject* Root, bool bReject)
    {
        TArray<UObject*> Graph;
        SoloASBridge::Compat::GetNestedObjects(Root, Graph);
        Graph.Add(Root);
        for (UObject* Object : Graph)
        {
            Pending.PreviousSoftTargets.Add(FSoftObjectPath(Object), Object);
            if (bReject) Pending.RejectedSoftTargets.Add(Object);
        }
    };
    for (UClass* Class : Affected)
    {
        Pending.PreviousSoftTargets.Add(FSoftObjectPath(Class), Class);
        Pending.RejectedSoftTargets.Add(Class);
        RememberSoftGraph(Class->GetDefaultObject(), true);
    }
    for (UObject* Instance : Instances) RememberSoftGraph(Instance, false);
    Pending.Instancing = MakeUnique<FScriptBlueprintInstancing>();
    auto VisitReferences = [&](FProperty* Property, void* Value, FBlueprintObjectReference Visit, bool bReplace)
    {
        return VisitBlueprintGenerationReferences(Pending, Property, Value, Visit, bReplace);
    };
    if (!Pending.Instancing->Prepare(InstancingRoots, Pending.ChildActors, VisitReferences, Error)) return false;
    for (UObject* Instance : Instances)
        if (Pending.Subclasses.ContainsByPredicate([&](const auto& Subclass) { return Subclass.Previous.Get() == Instance->GetClass(); }))
            Pending.Instancing->RequireMigration(Instance);
    for (UObject* Instance : Instances)
        if (!Instance->IsA<AActor>())
        {
            TArray<UObject*> Children;
            SoloASBridge::Compat::GetNestedObjects(Instance, Children);
            for (UObject* Child : Children)
                if (!Pending.Instancing->Contains(Child))
                {
                    Error = TEXT("A Blueprint generation receiver or instance owns nested objects requiring owner migration.");
                    return false;
                }
        }
    // Constructor-created AS components exist before SCS, but still need the
    // same per-instance identity map as runtime-created components.
    for (const auto& Receiver : Pending.Receivers)
        for (const auto& Property : Receiver.PreviousDesc->Properties)
            if (Property->Meta.Contains(TEXT("DefaultComponent")) || Property->Meta.Contains(TEXT("OverrideComponent")))
                for (UObject* Instance : Instances)
                    if (Instance->GetClass() == Receiver.Previous.Get()) Pending.Instancing->RequireMigration(Instance);
    if (!PrepareBlueprintActorGeneration(Pending, Error)) return false;
    TSet<UClass*> CurrentScriptClasses;
    if (!Pending.Actors.IsEmpty())
        for (const auto& Module : Manager.GetActiveModules())
            for (const auto& Class : Module->Classes)
                if (Class->Class) CurrentScriptClasses.Add(Class->Class);
    for (TObjectIterator<UObject> It; It; ++It)
    {
        UObject* Owner = *It;
        if (!IsBlueprintGenerationInstance(Owner)
            || (!Owner->GetClass()->HasAnyClassFlags(CLASS_Native) && !CurrentScriptClasses.Contains(Owner->GetClass())
                && (Pending.Actors.IsEmpty() || !Cast<UBlueprintGeneratedClass>(Owner->GetClass())))
            || (!Pending.bScriptParentOnly && Owner->GetOutermost() == Pending.Previous->GetOutermost())
            || Owner->GetOutermost() == Pending.Package.Get() || Instances.Contains(Owner)) continue;
        if (!Pending.Actors.IsEmpty() && (Owner->IsA<UWorld>() || Owner->IsA<ULevel>()
            || Pending.ActorComponents.Contains(Owner))) continue;
        bool bRetainedOwner = false;
        for (const auto& Record : BlueprintClassLifetimes)
        {
            bRetainedOwner |= Record.Value.RetiredClasses.ContainsByPredicate(
                [&](const auto& Class) { return Class.Get() == Owner->GetClass(); });
            bRetainedOwner |= Record.Value.RetiredPackages.ContainsByPredicate(
                [&](const auto& Package) { return Package.Get() == Owner->GetOutermost()
                    || Package.Get() == Owner->GetClass()->GetOutermost(); });
        }
        for (UObject* Outer = Owner->GetOuter(); Outer; Outer = Outer->GetOuter())
            bRetainedOwner |= Outer->IsTemplate() || Instances.Contains(Outer);
        if (bRetainedOwner) continue;
        for (TFieldIterator<FProperty> Field(Owner->GetClass()); Field; ++Field)
        {
            if (!Pending.Actors.IsEmpty()
                && ((Owner->IsA<AActor>() && !IsBlueprintActorStateField(*Field))
                    || (Owner->IsA<UActorComponent>() && (Field->GetOwnerClass() == UActorComponent::StaticClass()
                        || Field->GetOwnerClass() == USceneComponent::StaticClass()
                        || Field->GetOwnerClass() == UPrimitiveComponent::StaticClass())))) continue;
            bool bCurrent = false;
            const bool Valid = VisitBlueprintGenerationReferences(Pending, *Field, Field->ContainerPtrToValuePtr<void>(Owner),
                [&](UObject*& Value, UClass* Constraint, bool bInterface)
                {
                    if (Pending.ActorComponents.Contains(Value) && !Pending.NativeActorComponents.Contains(Value))
                    {
                        Pending.Instancing->RequireMigration(Value);
                        Pending.NativeActorComponents.Add(Value);
                    }
                    if (!Instances.Contains(Value) && !Pending.NativeActorComponents.Contains(Value)
                        && !Pending.Instancing->Contains(Value)) return true;
                    bCurrent = true;
                    // A native slot bound to a retired exact script/Blueprint
                    // class cannot accept the new identity without changing metadata.
                    return !Affected.Contains(Constraint) && (!bInterface || Value->GetClass()->ImplementsInterface(Constraint));
                });
            if (!Valid)
            {
                Error = TEXT("A current native reference requires retired type metadata; no receiver generation was swapped.");
                return false;
            }
            if (bCurrent) Pending.CurrentReferences.Add({Owner, *Field});
        }
    }
    for (auto& Plan : Pending.Actors)
        if (Pending.Instancing->RequiresMigration(Plan.Previous.Get()))
        {
            Plan.Components.Reset();
            Plan.NativeRoot.Reset();
            Plan.NativeGraph.Reset();
            TArray<UObject*> Owned;
            SoloASBridge::Compat::GetNestedObjects(Plan.Previous.Get(), Owned);
            for (UObject* Object : Owned)
                if (Pending.Instancing->Contains(Object))
                {
                    Pending.ActorComponents.Add(Object);
                    Pending.NativeActorComponents.Add(Object);
                }
        }
    if (!Pending.Instancing->HasMigration()) Pending.Instancing = MakeUnique<FScriptBlueprintInstancing>();
    Pending.ClassReloadHandle = FAngelscriptClassGenerator::OnClassReload.AddLambda(
        [](UClass* Previous, UClass* Candidate)
        {
            if (!PendingBlueprintGeneration) return;
            for (auto& Receiver : PendingBlueprintGeneration->Receivers)
                if (Receiver.Previous.Get() == Previous) Receiver.Candidate = Candidate;
        });
#if WITH_EDITOR && !IS_MONOLITHIC
    Pending.ObjectsReinstancedHandle = FCoreUObjectDelegates::OnObjectsReinstanced.AddLambda(
        [](const TMap<UObject*, UObject*>& Objects)
        {
            if (!PendingBlueprintGeneration) return;
            for (const auto& Previous : PendingBlueprintGeneration->PreviousInstances)
                if (UObject* const* Replacement = Objects.Find(Previous.Get()))
                {
                    if (*Replacement)
                        PendingBlueprintGeneration->ReinstancedInstances.Emplace(*Replacement);
                    PendingBlueprintGeneration->ReinstancedObjects.Add(Previous.Get(), *Replacement);
                }
        });
#endif
    return true;
}

bool ValidateBlueprintInPlaceUpdate(const FPendingBlueprintGeneration& Pending, FString& Error)
{
    const auto& Update = *Pending.InPlaceUpdate;
    const auto* Record = BlueprintClassLifetimes.Find(Pending.ClassPath);
    auto Reject = [&]()
    {
        Error = TEXT("The live Blueprint target changed after in-place preparation; publication was not started.");
        return false;
    };
    if (!Record || Record->Class.Get() != Pending.Previous.Get() || Record->DiskHash != Update.PreviousHash
        || FSoftObjectPath(Pending.ClassPath).ResolveObject() != Pending.Previous.Get()) return Reject();
    for (const auto& Target : Update.Targets) if (!IsValid(Target.Get())) return Reject();
    for (const auto& Pair : Update.Classes)
        if (BlueprintImportAbi(Pair.Key, &Update.Objects) != BlueprintImportAbi(Pair.Value)) return Reject();
    for (const auto& Pair : Update.Functions)
        if (Pair.Value->GetOuterUClass()->FindFunctionByName(Pair.Key->GetFName(), EIncludeSuperFlag::ExcludeSuper)
            != Pair.Value) return Reject();
    for (const auto& Graph : Update.Graphs)
    {
        if (!Graph.Class) continue;
        if (Graph.Class->SimpleConstructionScript != Graph.Script
            || Graph.Script->GetAllNodes() != Graph.PreviousNodes) return Reject();
        for (const auto& Actor : Graph.Actors)
        {
            if (Actor.Object->IsActorBeingDestroyed() || Actor.Object->GetClass() != Graph.Class
                || !Actor.Object->GetActorTransform().Equals(Actor.Transform)) return Reject();
            for (USCS_Node* Node : Graph.PreviousNodes)
            {
                FObjectPropertyBase* Field = FindFProperty<FObjectPropertyBase>(Graph.Class, Node->GetVariableName());
                if (!Field || Field->GetObjectPropertyValue_InContainer(Actor.Object)
                    != Actor.Objects.FindRef(Node->ComponentTemplate)) return Reject();
            }
        }
    }
    return true;
}

bool CommitBlueprintInPlaceUpdate(FPendingBlueprintGeneration& Pending, FString& Error)
{
    auto& Update = *Pending.InPlaceUpdate;
    // Called only after AS modules commit. Component callbacks are commit effects;
    // a failure keeps Pending locked and reports partial publication, not rollback.
    for (const auto& Pair : Update.Functions)
    {
        Pair.Value->Script = MoveTemp(Pair.Key->Script);
        Pair.Value->CollectBytecodeAndPropertyReferencedObjects();
    }
    for (const auto& Pair : Update.Classes)
        for (TFieldIterator<FProperty> It(Pair.Value); It; ++It)
            if (CastField<FNumericProperty>(*It) || CastField<FBoolProperty>(*It) || CastField<FEnumProperty>(*It))
                It->CopyCompleteValue_InContainer(Pair.Value->GetDefaultObject(), Pair.Key->GetDefaultObject());
    for (auto& Graph : Update.Graphs)
        if (!CommitBlueprintOwnedGraph(Graph, Update.Objects, Error)) return false;
    for (auto& Pair : BlueprintClassLifetimes)
        if (Pair.Value.Class->GetOutermost() == Pending.Previous->GetOutermost()) Pair.Value.DiskHash = Pending.Hash;
    BlueprintImportSchemas = MoveTemp(Pending.ImportSchemas);
    return true;
}

// Inspect the old identities at selection, then the full compiled closure at
// admission. No imported Blueprint or changed asset bytes are required.
static FString FindScriptParentBlueprintUnit(const TArray<TSharedRef<FAngelscriptModuleDesc>>& Modules)
{
    TMap<UClass*, FString> Parents;
    for (const auto& Module : Modules)
        if (const auto Previous = FAngelscriptManager::Get().GetModule(Module->ModuleName); Previous.IsValid())
            for (const auto& Class : Previous->Classes)
                if (Class->Class && !Class->bIsStruct && !Class->bIsStaticsClass)
                    Parents.Add(Class->Class, Module->ScriptUnit.Id);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        if (!Cast<UBlueprintGeneratedClass>(Class) || Class->HasAnyClassFlags(CLASS_NewerVersionExists)) continue;
#if WITH_EDITORONLY_DATA
        if (auto* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy); Blueprint && Blueprint->GeneratedClass != Class) continue;
#endif
        for (UClass* Parent = Class->GetSuperClass(); Parent; Parent = Parent->GetSuperClass())
            if (const FString* Unit = Parents.Find(Parent)) return *Unit;
    }
    return {};
}

EAngelscriptExtensionResult CompleteBlueprintImports(FAngelscriptTypeGenerationContext& Context)
{
    if (!PendingBlueprintGeneration)
    {
        if (!ExtensionRegistration::GetFeaturePolicy().bBlueprintInheritance) return EAngelscriptExtensionResult::NotHandled;
        if (!Context.Modules || (Context.Point != EAngelscriptTypeGenerationPoint::SelectModuleTransaction
            && Context.Point != EAngelscriptTypeGenerationPoint::PrepareModuleCommit))
            return EAngelscriptExtensionResult::NotHandled;
        const FString Unit = FindScriptParentBlueprintUnit(*Context.Modules);
        if (Unit.IsEmpty()) return EAngelscriptExtensionResult::NotHandled;
        if (Context.Point == EAngelscriptTypeGenerationPoint::SelectModuleTransaction)
        {
            Context.bCompileDependencyClosure = true;
            return EAngelscriptExtensionResult::Handled;
        }
        // Admission is all-or-none for this reload. Unsupported descendants or
        // receiver schemas keep the existing stock path before any class swap.
        auto Candidate = MakeUnique<FPendingBlueprintGeneration>();
        Candidate->bScriptParentOnly = true;
        Candidate->UnitId = Unit;
        FString AdmissionError;
        if (Context.bHasExternalModuleConsumers
            || !PrepareBlueprintReceiverMigration(*Candidate, *Context.Modules, AdmissionError)
            || Candidate->Subclasses.IsEmpty()) return EAngelscriptExtensionResult::NotHandled;
        Candidate->bPrepared = true;
        PendingBlueprintGeneration = MoveTemp(Candidate);
        Context.bPrepareClassGeneration = true;
        return EAngelscriptExtensionResult::Handled;
    }
    auto& Pending = *PendingBlueprintGeneration;
    if (Context.Point == EAngelscriptTypeGenerationPoint::RetainPreviousInstances)
    {
        if (!Pending.bPrepared || Pending.bCommitAttempted || !Context.PreviousInstancesToRetain)
            return EAngelscriptExtensionResult::NotHandled;
        for (const auto& Previous : Pending.PreviousInstances)
            Context.PreviousInstancesToRetain->Add(Previous.Get());
        if (!Pending.Subclasses.IsEmpty() && Context.PreparedBlueprintClasses
            && Context.PreparedBlueprintObjects && Context.OwnedBlueprintMetadata && Context.bModulesSwapped)
        {
            // Editor is already past phase 6. The old metadata must not enter
            // the stock compiler while this transaction owns its replacement.
            LockBlueprintReceiverMigration(Pending);
            if (!Pending.bScriptParentOnly)
            {
                Context.PreparedBlueprintClasses->Add(Pending.Previous.Get(), Pending.Candidate.Get());
                Pending.Previous->ClassFlags |= CLASS_NewerVersionExists;
                TArray<UObject*> RootMetadata;
                SoloASBridge::Compat::GetPackageObjects(Pending.Previous->GetOutermost(), RootMetadata);
                for (UObject* Object : RootMetadata) Context.OwnedBlueprintMetadata->Add(Object);
            }
            for (const auto& Subclass : Pending.Subclasses)
            {
                Context.PreparedBlueprintClasses->Add(Subclass.Previous.Get(), Subclass.Candidate.Get());
                Subclass.Previous->ClassFlags |= CLASS_NewerVersionExists;
            }
            Context.PreparedBlueprintObjects->Append(Pending.BlueprintMetadata);
            AppendPreparedBlueprintInstances(Pending, *Context.PreparedBlueprintObjects);
            Context.OwnedBlueprintMetadata->Append(Pending.OwnedBlueprintMetadata);
#if WITH_EDITORONLY_DATA
            // Reference refresh runs before canonical package publication. Retain
            // its exact private aliases so publication can finalize soft storage.
            for (const auto& Pair : Pending.BlueprintMetadata)
                Pending.PreviousSoftTargets.Add(FSoftObjectPath(Pair.Value), Pair.Key);
            // Skeleton classes are private generations too. Stock's derived-class
            // walk must not reinstance their already mapped defaults a second time.
            for (const auto& Pair : Pending.BlueprintMetadata)
                if (auto* Before = Cast<UBlueprintGeneratedClass>(Pair.Key);
                    Before && Context.OwnedBlueprintMetadata->Contains(Before))
                {
                    Context.PreparedBlueprintClasses->Add(Before, CastChecked<UClass>(Pair.Value));
                    Before->ClassFlags |= CLASS_NewerVersionExists;
                }
#endif
        }
        return EAngelscriptExtensionResult::Handled;
    }
    if (!Context.Modules || !Context.Modules->ContainsByPredicate([&](const auto& Module)
        { return Module->ScriptUnit.Id == Pending.UnitId; })) return EAngelscriptExtensionResult::NotHandled;
    if (Context.Point == EAngelscriptTypeGenerationPoint::SelectModuleTransaction)
    {
        Context.bCompileDependencyClosure = true;
        return EAngelscriptExtensionResult::Handled;
    }
    if (Context.Point == EAngelscriptTypeGenerationPoint::AbortModules)
    {
        Pending.RemoveMigrationDelegates();
        if (Context.bModulesSwapped)
        {
            LockBlueprintReceiverMigration(Pending);
            Pending.bCommitAttempted = true;
            Context.Error = TEXT("Module generation failed after swap; Blueprint publication is locked for owner review.");
            return EAngelscriptExtensionResult::Error;
        }
        if (!Pending.bCommitAttempted) PendingBlueprintGeneration.Reset();
        return EAngelscriptExtensionResult::Handled;
    }
    if (Pending.bCommitAttempted)
    {
        Context.Error = TEXT("This Blueprint generation is locked after a failed commit.");
        return EAngelscriptExtensionResult::Error;
    }
    if (Context.Point == EAngelscriptTypeGenerationPoint::PrepareModuleCommit)
    {
        if (Context.bHasExternalModuleConsumers)
        {
            Context.Error = TEXT("A real VM consumer is outside the Blueprint generation compile closure; the current generation is unchanged.");
            return EAngelscriptExtensionResult::Error;
        }
        for (const FString& Name : Pending.RequiredModules)
            if (!Context.Modules->ContainsByPredicate([&](const auto& Module) { return Module->ModuleName == Name; }))
            {
                Context.Error = TEXT("The Blueprint generation did not recompile its complete source unit.");
                return EAngelscriptExtensionResult::Error;
            }
        if (BlueprintPackageHash(Pending.Filename) != Pending.Hash)
        {
            Context.Error = TEXT("The Blueprint package changed after schema preparation.");
            return EAngelscriptExtensionResult::Error;
        }
        if (Pending.InPlaceUpdate)
        {
            if (!ValidateBlueprintInPlaceUpdate(Pending, Context.Error)) return EAngelscriptExtensionResult::Error;
            Pending.bPrepared = true;
            return EAngelscriptExtensionResult::Handled;
        }
        const FString Retired = TEXT("/Temp/SoloASBlueprintRetired_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        if (!Pending.Previous->GetOutermost()->Rename(*Retired, nullptr,
            REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders | REN_Test))
        {
            Context.Error = TEXT("The previous Blueprint generation cannot enter a retained namespace.");
            return EAngelscriptExtensionResult::Error;
        }
        if (!PrepareBlueprintReceiverMigration(Pending, *Context.Modules, Context.Error))
        {
            if (!Pending.DelegateError.IsEmpty()) Context.Error = Pending.DelegateError;
            return EAngelscriptExtensionResult::Error;
        }
        Pending.bPrepared = true;
        Context.bPrepareClassGeneration = true;
        return EAngelscriptExtensionResult::Handled;
    }
    if (!Pending.bPrepared)
    {
        Context.Error = TEXT("The Blueprint generation has no final compile admission.");
        return EAngelscriptExtensionResult::Error;
    }
    if (Context.Point == EAngelscriptTypeGenerationPoint::PrepareInstanceCommit)
    {
        for (auto& Receiver : Pending.Receivers) Receiver.Candidate = Receiver.CandidateDesc->Class;
        if (!PrepareBlueprintReceiverCommit(Pending, Context.Error))
        {
            if (!Pending.DelegateError.IsEmpty()) Context.Error = Pending.DelegateError;
            return EAngelscriptExtensionResult::Error;
        }
        return EAngelscriptExtensionResult::Handled;
    }
    // Runtime module/class publication already crossed the irreversible boundary.
    // Lock before any fallible namespace operation, not only before Actor retirement.
    LockBlueprintReceiverMigration(Pending);
    Pending.bCommitAttempted = true;
    Pending.RemoveMigrationDelegates();
    if (Pending.InPlaceUpdate)
    {
        if (!CommitBlueprintInPlaceUpdate(Pending, Context.Error)) return EAngelscriptExtensionResult::Error;
        PendingBlueprintGeneration.Reset();
        return EAngelscriptExtensionResult::Handled;
    }
    UPackage* PreviousPackage = nullptr;
    if (!Pending.bScriptParentOnly)
    {
        PreviousPackage = Pending.Previous->GetOutermost();
        const FString PackageName = FPackageName::ObjectPathToPackageName(Pending.ClassPath);
        const FString Retired = TEXT("/Temp/SoloASBlueprintRetired_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const ERenameFlags Flags = REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders;
        if (!PreviousPackage->Rename(*Retired, nullptr, Flags)
            || !Pending.Package->Rename(*PackageName, nullptr, Flags | REN_Test)
            || !Pending.Package->Rename(*PackageName, nullptr, Flags))
        {
            Context.Error = TEXT("Blueprint generation publication failed after module swap; the class is locked for owner review.");
            return EAngelscriptExtensionResult::Error;
        }
    }
    if (!PublishBlueprintSubclassGenerations(Pending, Context.Error)) return EAngelscriptExtensionResult::Error;
    if (!CommitBlueprintReceiverMigration(Pending, Context.Error))
    {
        if (!Pending.DelegateError.IsEmpty()) Context.Error = Pending.DelegateError;
        return EAngelscriptExtensionResult::Error;
    }
    if (!Pending.bScriptParentOnly)
    {
        auto& Record = BlueprintClassLifetimes.FindChecked(Pending.ClassPath);
        Record.RetiredPackages.Emplace(PreviousPackage);
        Record.RetiredClasses.Emplace(Pending.Previous.Get());
        Record.Class.Reset(Pending.Candidate.Get());
        Record.View = Pending.View;
        Record.DiskHash = Pending.Hash;
        Record.Abi = BlueprintImportAbi(Pending.Candidate.Get());
        BlueprintImportSchemas = MoveTemp(Pending.ImportSchemas);
    }
    PendingBlueprintGeneration.Reset();
    return EAngelscriptExtensionResult::Handled;
}

EAngelscriptExtensionResult PrepareBlueprintImports(FAngelscriptTypeGenerationContext& Context)
{
    if (!Context.bPreloadOnly)
    {
        if (PendingBlueprintGeneration && PendingBlueprintGeneration->bCommitAttempted)
        {
            Context.Error = TEXT("A failed Blueprint generation commit requires owner review before another publication.");
            return EAngelscriptExtensionResult::Error;
        }
        PendingBlueprintGeneration.Reset();
        auto& Preprocessor = *Context.Preprocessor;
        bool bSourceReady = false;
        ON_SCOPE_EXIT { if (!bSourceReady) PendingBlueprintGeneration.Reset(); };
        const FRegexPattern Import(TEXT("\\bimport\\s+blueprint\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*;"));
        for (const auto& File : Preprocessor.Files)
        {
            const FString Source = Mask(File.RawCode);
            FRegexMatcher Match(Import, Source);
            while (Match.FindNext())
            {
                UClass* ImportedClass = ResolveBlueprintMapping(File.AbsoluteFilename, Match.GetCaptureGroup(1), Context.Error);
                if (!ImportedClass) return EAngelscriptExtensionResult::Error;
                const bool bInterface = ImportedClass->HasAnyClassFlags(CLASS_Interface);
                if (bInterface ? !IsEnabled(File.AbsoluteFilename) : !ExtensionRegistration::GetFeaturePolicy().bBlueprintInheritance)
                {
                    Context.Error = bInterface
                        ? TEXT("Blueprint interfaces require bEnableScriptInterfaces=true; restart after changing [SoloASCoreExtensions].")
                        : TEXT("Blueprint imports require bEnableBlueprintInheritance=true; restart after changing [SoloASCoreExtensions].");
                    return EAngelscriptExtensionResult::Error;
                }
                if (PendingBlueprintGeneration && PendingBlueprintGeneration->UnitId.IsEmpty())
                    PendingBlueprintGeneration->UnitId = File.Module->ScriptUnit.Id;
            }
        }
        FString ScriptParentUnit;
        if (!PendingBlueprintGeneration && ExtensionRegistration::GetFeaturePolicy().bBlueprintInheritance)
        {
            TArray<TSharedRef<FAngelscriptModuleDesc>> Modules;
            for (const auto& File : Preprocessor.Files) Modules.Add(File.Module.ToSharedRef());
            ScriptParentUnit = FindScriptParentBlueprintUnit(Modules);
        }
        if (PendingBlueprintGeneration || !ScriptParentUnit.IsEmpty())
        {
            FAngelscriptPreprocessor Additional;
            Additional.ScriptUnits = Preprocessor.ScriptUnits;
            Additional.bHasScriptUnitCatalog = true;
            const auto Active = FAngelscriptManager::Get().GetActiveModules();
            TSet<FString> AffectedUnits;
            AffectedUnits.Add(PendingBlueprintGeneration ? PendingBlueprintGeneration->UnitId : ScriptParentUnit);
            for (const auto& File : Preprocessor.Files) AffectedUnits.Add(File.Module->ScriptUnit.Id);
            // Native Blueprint views have no VM module edge. Seed their direct
            // users, then follow real VM consumers to a fixed point by unit.
            const auto* Record = PendingBlueprintGeneration ? BlueprintClassLifetimes.Find(PendingBlueprintGeneration->ClassPath) : nullptr;
            for (const auto& Module : Active)
                for (const auto& Section : Module->Code)
                    if (Record && Section.Code.Contains(Record->View)) AffectedUnits.Add(Module->ScriptUnit.Id);
            bool bAdded = true;
            while (bAdded)
            {
                bAdded = false;
                for (const auto& Module : Active)
                {
                    if (!Module->ScriptModule || AffectedUnits.Contains(Module->ScriptUnit.Id)) continue;
                    for (const auto& Dependency : Module->ScriptModule->moduleDependencies)
                        if (AffectedUnits.Contains(ANSI_TO_TCHAR(Dependency.Key->scriptUnitId.AddressOf())))
                        {
                            AffectedUnits.Add(Module->ScriptUnit.Id);
                            bAdded = true;
                            break;
                        }
                }
            }
            for (const auto& Module : Active)
            {
                if (!AffectedUnits.Contains(Module->ScriptUnit.Id) || Module->bTreatAsDeleted) continue;
                if (PendingBlueprintGeneration) PendingBlueprintGeneration->RequiredModules.Add(Module->ModuleName);
                for (const auto& Section : Module->Code)
                    if (!Preprocessor.Files.ContainsByPredicate([&](const auto& File) { return File.AbsoluteFilename == Section.AbsoluteFilename; }))
                        Additional.AddFile(Section.RelativeFilename, Section.AbsoluteFilename);
            }
            for (auto& File : Additional.Files) File.LoadedSource = File.RawCode;
            Preprocessor.Files.Append(MoveTemp(Additional.Files));
        }
        bSourceReady = true;
    }
    // Ordinary script classes carry module dependencies. Native interface views do
    // not, so re-preprocess their users before entering the same compiler/reloader.
    if (IsEnabled() && !Context.bPreloadOnly && !Interfaces.IsEmpty())
    {
        auto& Preprocessor = *Context.Preprocessor;
        TSet<FString> Changed;
        for (const auto& File : Preprocessor.Files)
        {
            const FString Code = Mask(File.RawCode);
            FRegexMatcher Header(FRegexPattern(TEXT("\\binterface\\s+([A-Za-z_][A-Za-z0-9_]*)(?:\\s*:\\s*([A-Za-z_][A-Za-z0-9_]*))?\\s*\\{")), Code);
            while (Header.FindNext())
            {
                const FString Name = Header.GetCaptureGroup(1);
                const FInterface* Existing = FindInterface(Name);
                const int32 Open = Header.GetMatchEnding() - 1, Close = CloseBrace(Code, Open);
                TArray<FMethod> Methods;
                if (Existing && Close != INDEX_NONE
                    && ParseMethods(File.RawCode.Mid(Open + 1, Close - Open - 1), Methods, Context.Error)
                    && Existing->ScriptName != InterfaceScriptName(Name, Header.GetCaptureGroup(2), Methods)) Changed.Add(Name);
                if (!Context.Error.IsEmpty()) return EAngelscriptExtensionResult::Error;
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
        if (!Changed.IsEmpty())
        {
            FAngelscriptPreprocessor Additional;
            for (const auto& Module : FAngelscriptManager::Get().GetActiveModules())
            {
                bool bUsesInterface = false;
                for (const FString& Name : Changed)
                {
                    const FInterface& Definition = *FindInterface(Name);
                    bUsesInterface |= Module->ModuleName == Definition.ModuleName;
                    for (const auto& Section : Module->Code) bUsesInterface |= Section.Code.Contains(Definition.ScriptName);
                }
                if (!bUsesInterface) continue;
                for (const auto& Section : Module->Code)
                    if (!Preprocessor.Files.ContainsByPredicate([&](const auto& File) { return File.AbsoluteFilename == Section.AbsoluteFilename; }))
                        Additional.AddFile(Section.RelativeFilename, Section.AbsoluteFilename);
            }
            Preprocessor.Files.Append(MoveTemp(Additional.Files));
        }
    }
    HandleLegacyBlueprintImports(*Context.Preprocessor, Context.bPreloadOnly);
    if (Context.bPreloadOnly) return EAngelscriptExtensionResult::Handled;
    bool bHandled = false;
    if (!RewriteBlueprintImports(*Context.Preprocessor, Context.Error, bHandled)) return EAngelscriptExtensionResult::Error;
    return bHandled ? EAngelscriptExtensionResult::Handled : EAngelscriptExtensionResult::NotHandled;
}

EAngelscriptExtensionResult RewriteDeclarations(FAngelscriptPreParseContext& Context)
{
    bool bSucceeded = false;
    ON_SCOPE_EXIT { if (!bSucceeded) RestoreInterfaceDeclarations(); };
    auto& Preprocessor = *Context.Preprocessor;
    bool bHandled = false;
    const FRegexPattern InterfaceHeader(TEXT("\\binterface\\s+([A-Za-z_][A-Za-z0-9_]*)(?:\\s*:\\s*([A-Za-z_][A-Za-z0-9_]*))?\\s*\\{"));
    const FRegexPattern ClassHeader(TEXT("\\bclass\\s+([A-Za-z_][A-Za-z0-9_]*)(?:\\s*:\\s*([^{};]+))?\\s*\\{"));
    struct FDeclaration
    {
        int32 FileIndex, Begin, Close;
        FString Name, Parent, Source;
        TArray<FMethod> Methods;
    };
    TArray<FDeclaration> Pending;
    TSet<FString> DeclaredNames;
    for (int32 FileIndex = 0; FileIndex < Preprocessor.Files.Num(); ++FileIndex)
    {
        auto& File = Preprocessor.Files[FileIndex];
        const FString Code = Mask(File.RawCode);
        FRegexMatcher Declaration(InterfaceHeader, Code);
        int32 End = 0;
        while (Declaration.FindNext())
        {
            if (Declaration.GetMatchBeginning() < End) continue;
            if (!IsEnabled())
            {
                Context.Error = TEXT("SoloASCoreExtensions script interfaces extension is not enabled; ")
                    TEXT("set [SoloASCoreExtensions] bEnableScriptInterfaces=true and restart.");
                return EAngelscriptExtensionResult::Error;
            }
            const int32 Begin = Declaration.GetMatchBeginning();
            const int32 Open = Declaration.GetMatchEnding() - 1;
            const int32 Close = CloseBrace(Code, Open);
            if (Close == INDEX_NONE) { Context.Error = TEXT("Unclosed interface declaration."); return EAngelscriptExtensionResult::Error; }
            const FString Name = Declaration.GetCaptureGroup(1);
            if (DeclaredNames.Contains(Name))
            {
                Context.Error = TEXT("An interface name is declared more than once in this scope.");
                return EAngelscriptExtensionResult::Error;
            }
            DeclaredNames.Add(Name);
            FDeclaration Item{FileIndex, Begin, Close, Name, Declaration.GetCaptureGroup(2), File.RawCode.Mid(Begin, Close - Begin + 1), {}};
            if (!ParseMethods(File.RawCode.Mid(Open + 1, Close - Open - 1), Item.Methods, Context.Error))
                return EAngelscriptExtensionResult::Error;
            Pending.Add(MoveTemp(Item));
            End = Close + 1;
        }
    }
    // A registered native view outlives its source declaration. Do not let that
    // old alias silently accept a deletion while live script users still need it.
    for (const auto& Pair : Interfaces)
    {
        const FInterface& Definition = Pair.Value.Get();
        if (Definition.Source.IsEmpty() || DeclaredNames.Contains(Pair.Key)
            || !Preprocessor.Files.ContainsByPredicate([&](const auto& File)
                { return File.Module->ModuleName == Definition.ModuleName; })) continue;
        TArray<FString> Referencers;
        for (const auto& File : Preprocessor.Files)
        {
            FRegexMatcher Reference(FRegexPattern(TEXT("\\b") + Pair.Key + TEXT("\\b")), Mask(File.RawCode));
            if (Reference.FindNext()) Referencers.AddUnique(File.RelativeFilename);
        }
        for (const auto& Module : FAngelscriptManager::Get().GetActiveModules())
        {
            if (Preprocessor.Files.ContainsByPredicate([&](const auto& File)
                { return File.Module->ModuleName == Module->ModuleName; })) continue;
            for (const auto& Section : Module->Code)
                if (Section.Code.Contains(Definition.ScriptName)) Referencers.AddUnique(Section.RelativeFilename);
        }
        if (!Referencers.IsEmpty())
        {
            Referencers.Sort();
            Context.Error = FString::Printf(TEXT("Cannot delete interface '%s'; still referenced by %s. Restart the editor to apply this change."),
                *Pair.Key, *FString::Join(Referencers, TEXT(", ")));
            return EAngelscriptExtensionResult::Error;
        }
    }
    while (!Pending.IsEmpty())
    {
        bool bProgress = false;
        for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
        {
            auto& Item = Pending[Index];
            if (!Item.Parent.IsEmpty() && (!Resolve(Item.Parent)
                || Pending.ContainsByPredicate([&](const FDeclaration& Other) { return Other.Name == Item.Parent; }))) continue;
            if (!Create(Item.Name, Item.Parent, Item.Source, Preprocessor.Files[Item.FileIndex].Module->ModuleName, MoveTemp(Item.Methods), Context.Error))
                return EAngelscriptExtensionResult::Error;
            auto& Source = Preprocessor.Files[Item.FileIndex].RawCode;
            for (int32 Position = Item.Begin; Position <= Item.Close; ++Position)
                if (Source[Position] != '\n' && Source[Position] != '\r') Source[Position] = ' ';
            Pending.RemoveAt(Index);
            bProgress = bHandled = true;
        }
        if (!bProgress) { Context.Error = TEXT("An interface parent is missing or its inheritance forms a cycle."); return EAngelscriptExtensionResult::Error; }
    }
    for (auto& File : Preprocessor.Files)
    {
        for (const FString& Name : DeclaredNames)
        {
            const FInterface& Definition = Interfaces.FindChecked(Name).Get();
            for (const FMethod& Method : Definition.Methods)
            {
                if (Method.DefaultFunction.IsEmpty() || Method.ModuleName != File.Module->ModuleName
                    || !Method.DefaultFunction.StartsWith(TEXT("__SoloASDefault_") + Name + TEXT("_"))) continue;
                FString Body = Method.Body;
                const FString Code = Mask(Body);
                struct FEdit { int32 Begin, End; FString Text; };
                TArray<FEdit> Edits;
                FRegexMatcher Self(FRegexPattern(TEXT("\\bthis\\b")), Code);
                while (Self.FindNext()) Edits.Add({Self.GetMatchBeginning(), Self.GetMatchEnding(), TEXT("__SoloASInterfaceView")});
                for (const FMethod& Other : Definition.Methods)
                {
                    FRegexMatcher Call(FRegexPattern(TEXT("(?<![A-Za-z0-9_.])") + Other.Name + TEXT("\\s*\\(")), Code);
                    while (Call.FindNext()) Edits.Add({Call.GetMatchBeginning(), Call.GetMatchBeginning(), TEXT("__SoloASInterfaceView.")});
                }
                Edits.Sort([](const FEdit& A, const FEdit& B) { return A.Begin > B.Begin; });
                for (const FEdit& Edit : Edits) Body = Body.Left(Edit.Begin) + Edit.Text + Body.Mid(Edit.End);
                Body.InsertAt(1, TEXT(" ") + Name + TEXT(" __SoloASInterfaceView = Cast<") + Name + TEXT(">(__SoloASInterfaceTarget); "));
                FString Declaration = Method.Declaration.Replace(*(Method.Name + TEXT("(")), *(Method.DefaultFunction + TEXT("(UObject __SoloASInterfaceTarget")
                    + (Method.Arguments.IsEmpty() ? TEXT("") : TEXT(", "))));
                FString FunctionCode = TEXT("\nUFUNCTION(BlueprintCallable, meta=(WorldContext=\"__SoloASInterfaceTarget\")) ")
                    + Declaration + TEXT(" ") + Body + TEXT("\n");
                if (File.Module->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf)
                    FunctionCode = TEXT("\nnamespace ") + File.Module->ScriptUnit.Id + TEXT(" { ") + FunctionCode + TEXT("\n}\n");
                File.RawCode += FunctionCode;
            }
        }
    }
    for (auto& File : Preprocessor.Files)
    {
        const FString Code = Mask(File.RawCode);
        struct FReplacement { int32 Begin, End; FString Text; };
        TArray<FReplacement> Replacements;
        FRegexMatcher Class(ClassHeader, Code);
        while (Class.FindNext())
        {
            TArray<FString> Parents;
            Class.GetCaptureGroup(2).ParseIntoArray(Parents, TEXT(","));
            if (!Parents.IsEmpty() && Resolve(Parents[0].TrimStartAndEnd()))
            {
                Context.Error = IsEnabled()
                    ? TEXT("The first parent must be the unique UObject class; list interfaces after it.")
                    : TEXT("SoloASCoreExtensions script interfaces extension is not enabled; set [SoloASCoreExtensions] bEnableScriptInterfaces=true and restart.");
                return EAngelscriptExtensionResult::Error;
            }
            if (const auto Existing = FAngelscriptManager::Get().GetClass(Class.GetCaptureGroup(1)))
            {
                TArray<FString> Previous, Current;
                if (const FString* Names = Existing->Meta.Find(InterfacesKey)) Names->ParseIntoArray(Previous, TEXT(";"));
                for (int32 Index = 1; Index < Parents.Num(); ++Index) Current.AddUnique(Parents[Index].TrimStartAndEnd());
                for (FString& Name : Previous)
                    if (const FInterface* Definition = FindInterface(Name)) Name = Definition->Identity;
                for (FString& Name : Current)
                    if (const FInterface* Definition = FindInterface(Name)) Name = Definition->Identity;
                Previous.Sort(); Current.Sort();
                if (Previous != Current)
                {
                    Context.Error = FString::Printf(TEXT("Class '%s' changed its implemented interfaces; restart the editor to apply this change. Keeping the previous class."), *Class.GetCaptureGroup(1));
                    return EAngelscriptExtensionResult::Error;
                }
            }
            if (Parents.Num() < 2) continue;
            if (!IsEnabled())
            {
                Context.Error = TEXT("SoloASCoreExtensions script interfaces extension is not enabled; ")
                    TEXT("set [SoloASCoreExtensions] bEnableScriptInterfaces=true and restart.");
                return EAngelscriptExtensionResult::Error;
            }
            const int32 Open = Class.GetMatchEnding() - 1;
            const int32 Close = CloseBrace(Code, Open);
            if (Close == INDEX_NONE) { Context.Error = TEXT("Unclosed interface implementation class."); return EAngelscriptExtensionResult::Error; }
            const FString Body = Code.Mid(Open + 1, Close - Open - 1);
            FString Defaults, Forwarders, Names;
            TSet<FString> Implementations;
            TMap<FString, const FMethod*> DefaultOwners;
            TMap<FName, UFunction*> Signatures;
            for (int32 Index = 1; Index < Parents.Num(); ++Index)
            {
                const FString Name = Parents[Index].TrimStartAndEnd();
                UClass* Interface = Resolve(Name);
                if (!Interface)
                {
                    Context.Error = FString::Printf(
                        TEXT("'%s' is not a reflected interface; only the first parent can be a UObject class."), *Name);
                    return EAngelscriptExtensionResult::Error;
                }
                if (Index > 1) Names += TEXT(";");
                Names += Name;
                for (TFieldIterator<UFunction> It(Interface); It; ++It)
                {
                    if (!It->GetOuterUClass()->HasAnyClassFlags(CLASS_Interface)
                        || It->HasAnyFunctionFlags(FUNC_UbergraphFunction | FUNC_Static)
                        || !It->HasAnyFunctionFlags(FUNC_BlueprintEvent | FUNC_BlueprintCallable | FUNC_BlueprintPure)) continue;
                    if (UFunction** Previous = Signatures.Find(It->GetFName()))
                    {
                        if (!(*Previous)->IsSignatureCompatibleWith(*It))
                        {
                            Context.Error = TEXT("Interface methods with the same name have incompatible signatures.");
                            return EAngelscriptExtensionResult::Error;
                        }
                    }
                    else Signatures.Add(It->GetFName(), *It);
                }
                if (const FInterface* Definition = FindInterface(Name))
                {
                    for (const FMethod& Method : Definition->Methods)
                    {
                        if (const FMethod* const* Previous = DefaultOwners.Find(Method.Name))
                        {
                            FRegexMatcher Explicit(FRegexPattern(TEXT("UFUNCTION\\s*\\([^)]*BlueprintOverride[^)]*\\)\\s*[^{};]+\\b") + Method.Name + TEXT("\\s*\\(")), Body);
                            if (!(*Previous)->DefaultFunction.IsEmpty() && !Method.DefaultFunction.IsEmpty()
                                && (*Previous)->DefaultFunction != Method.DefaultFunction && !Explicit.FindNext())
                            {
                                Context.Error = FString::Printf(TEXT("Interface method '%s' has conflicting defaults; implement it explicitly with UFUNCTION(BlueprintOverride)."), *Method.Name);
                                return EAngelscriptExtensionResult::Error;
                            }
                        }
                        else DefaultOwners.Add(Method.Name, &Method);
                        if (Implementations.Contains(Method.Name)) continue;
                        Implementations.Add(Method.Name);
                        Forwarders += TEXT(" ") + Method.Declaration + TEXT(" final { ");
                        if (Method.ReturnType != TEXT("void")) Forwarders += TEXT("return ");
                        Forwarders += TEXT("Cast<") + Name + TEXT(">(this).") + Method.Name + TEXT("(")
                            + FString::Join(Method.Arguments, TEXT(", ")) + TEXT("); }");
                        FRegexMatcher Implementation(FRegexPattern(TEXT("\\b") + Method.Name + TEXT("\\s*\\(")), Body);
                        if (Implementation.FindNext())
                        {
                            FRegexMatcher Override(
                                FRegexPattern(TEXT("UFUNCTION\\s*\\([^)]*BlueprintOverride[^)]*\\)\\s*[^{};]+\\b") + Method.Name + TEXT("\\s*\\(")), Body);
                            if (!Override.FindNext())
                            {
                                Context.Error = FString::Printf(
                                    TEXT("Interface method '%s' requires UFUNCTION(BlueprintOverride)."), *Method.Name);
                                return EAngelscriptExtensionResult::Error;
                            }
                            continue;
                        }
                        if (Method.Body.IsEmpty())
                        {
                            Context.Error = FString::Printf(
                                TEXT("Class '%s' must implement interface method '%s' with BlueprintOverride."), *Class.GetCaptureGroup(1), *Method.Name);
                            return EAngelscriptExtensionResult::Error;
                        }
                        Defaults += TEXT(" UFUNCTION(BlueprintOverride) ") + Method.Declaration + TEXT(" { ");
                        if (Method.ReturnType != TEXT("void")) Defaults += TEXT("return ");
                        Defaults += Method.DefaultFunction + TEXT("(this")
                            + (Method.Arguments.IsEmpty() ? TEXT("") : TEXT(", ") + FString::Join(Method.Arguments, TEXT(", ")))
                            + TEXT("); }");
                    }
                }
            }
            const FString Header = TEXT("UCLASS(Meta=(SoloASInterfaces=\"") + Names + TEXT("\")) class ")
                + Class.GetCaptureGroup(1) + TEXT(" : ") + Parents[0].TrimStartAndEnd() + TEXT(" {");
            Replacements.Add({Class.GetMatchBeginning(), Open + 1, Header});
            if (!(Defaults + Forwarders).IsEmpty()) Replacements.Add({Close, Close, Defaults + Forwarders});
            bHandled = true;
        }
        Replacements.Sort([](const FReplacement& A, const FReplacement& B) { return A.Begin > B.Begin; });
        for (const auto& Replacement : Replacements)
            File.RawCode = File.RawCode.Left(Replacement.Begin) + Replacement.Text + File.RawCode.Mid(Replacement.End);
        const FString Rewritten = Mask(File.RawCode);
        const FRegexPattern CastHeader(TEXT("\\bCast\\s*<\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*>\\s*\\("));
        FRegexMatcher Cast(CastHeader, Rewritten);
        TArray<FReplacement> Casts;
        while (Cast.FindNext())
            if (Interfaces.Contains(Cast.GetCaptureGroup(1)))
                Casts.Add({Cast.GetMatchBeginning(), Cast.GetMatchEnding(), TEXT("__SoloASCast_") + Cast.GetCaptureGroup(1) + TEXT("(")});
        for (int32 Index = Casts.Num() - 1; Index >= 0; --Index)
            File.RawCode = File.RawCode.Left(Casts[Index].Begin) + Casts[Index].Text + File.RawCode.Mid(Casts[Index].End);
        // Native VM bindings are immutable. New source sees a signature-specific view;
        // old modules keep the view they compiled against until ordinary retirement.
        const FString TypedCode = Mask(File.RawCode);
        FRegexMatcher Identifier(FRegexPattern(TEXT("\\b[A-Za-z_][A-Za-z0-9_]*\\b")), TypedCode);
        TArray<FReplacement> Types;
        while (Identifier.FindNext())
        {
            const FString Token = TypedCode.Mid(Identifier.GetMatchBeginning(), Identifier.GetMatchEnding() - Identifier.GetMatchBeginning());
            if (const FInterface* Definition = FindInterface(Token))
                Types.Add({Identifier.GetMatchBeginning(), Identifier.GetMatchEnding(), Definition->ScriptName});
            else if (Token.StartsWith(TEXT("__SoloASCast_")))
                if (const FInterface* CastDefinition = FindInterface(Token.Mid(13)))
                    Types.Add({Identifier.GetMatchBeginning(), Identifier.GetMatchEnding(), TEXT("__SoloASCast_") + CastDefinition->ScriptName});
        }
        for (int32 Index = Types.Num() - 1; Index >= 0; --Index)
            File.RawCode = File.RawCode.Left(Types[Index].Begin) + Types[Index].Text + File.RawCode.Mid(Types[Index].End);

        // Ordinary class metadata is already serialized and covered by the cache
        // companion. Keep a carrier even for an interface-only script module.
        TArray<TSharedPtr<FJsonValue>> Declarations, Imports;
        TSet<FString> Required;
        for (const auto& Pair : Interfaces)
            if (!Pair.Value->Source.IsEmpty() && (File.RawCode.Contains(Pair.Value->ScriptName)
                || (DeclaredNames.Contains(Pair.Key) && Pair.Value->ModuleName == File.Module->ModuleName)))
            {
                FString Name = Pair.Key;
                while (!Name.IsEmpty() && !Required.Contains(Name))
                {
                    Required.Add(Name);
                    const FInterface* Definition = FindInterface(Name);
                    Name = Definition ? Definition->Parent : FString();
                }
            }
        for (const auto& Pair : Interfaces)
        {
            const auto& Definition = Pair.Value.Get();
            if (!Required.Contains(Pair.Key) || Definition.Source.IsEmpty()) continue;
            auto Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Pair.Key);
            Entry->SetStringField(TEXT("parent"), Definition.Parent);
            Entry->SetStringField(TEXT("module"), Definition.ModuleName);
            // Default bodies are already bytecode in their owning module. Only
            // their presence belongs to the schema; body edits stay SoftReload.
            FString Declaration = TEXT("interface ") + Pair.Key
                + (Definition.Parent.IsEmpty() ? TEXT("") : TEXT(" : ") + Definition.Parent) + TEXT(" {\n");
            for (const auto& Method : Definition.Methods)
            {
                if (const FInterface* Parent = FindInterface(Definition.Parent))
                    if (Parent->Methods.ContainsByPredicate([&](const auto& Other) { return Other.Name == Method.Name; })) continue;
                Declaration += (Method.bPure ? TEXT("UFUNCTION(BlueprintPure) ") : TEXT(""))
                    + Method.Declaration + (Method.Body.IsEmpty() ? TEXT(";\n") : TEXT(" {}\n"));
            }
            Entry->SetStringField(TEXT("source"), Declaration + TEXT("}"));
            Entry->SetStringField(TEXT("view"), Definition.ScriptName);
            Declarations.Add(MakeShared<FJsonValueObject>(Entry));
        }
        TSet<FString> ImportedViews;
        for (const auto& Mappings : (PendingBlueprintGeneration ? PendingBlueprintGeneration->ImportSchemas : BlueprintImportSchemas))
            for (const auto& Pair : Mappings.Value)
            {
                const FString View = Pair.Value->AsObject()->GetStringField(TEXT("view"));
                if (!ImportedViews.Contains(View) && (Mappings.Key == File.AbsoluteFilename
                    || Required.Contains(View) || File.RawCode.Contains(View)))
                { ImportedViews.Add(View); Imports.Add(Pair.Value); }
            }
        if (!Declarations.IsEmpty() || !Imports.IsEmpty())
        {
            auto Schema = MakeShared<FJsonObject>();
            Schema->SetNumberField(TEXT("version"), 1);
            Schema->SetArrayField(TEXT("declarations"), Declarations);
            Schema->SetArrayField(TEXT("imports"), Imports);
            FString Json;
            const auto Writer = TJsonWriterFactory<>::Create(&Json);
            FJsonSerializer::Serialize(Schema, Writer);
            FString Carrier = TEXT("\nUCLASS(Abstract, NotBlueprintable, meta=(SoloASInterfaceSchema=\"")
                + FBase64::Encode(Json) + TEXT("\")) class U__SoloASInterfaceSchema_")
                + FMD5::HashAnsiString(*File.Module->ModuleName) + TEXT(" : UObject {}\n");
            if (File.Module->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf)
                Carrier = TEXT("\nnamespace ") + File.Module->ScriptUnit.Id + TEXT(" { ") + Carrier + TEXT("\n}\n");
            File.RawCode += Carrier;
        }
    }
    bSucceeded = true;
    return bHandled ? EAngelscriptExtensionResult::Handled : EAngelscriptExtensionResult::NotHandled;
}

EAngelscriptExtensionResult RestoreCachedInterfaces(FAngelscriptTypeGenerationContext& Context)
{
    struct FDeclaration { FString Name, Parent, Source, View, Module; TArray<FMethod> Methods; };
    TArray<FDeclaration> Pending;
    TArray<TSharedPtr<FJsonObject>> Imports;
    TSet<FString> Views;
    const auto Reject = [&](const FString& Detail)
    {
        Context.Error = TEXT("SOLOAS-INTERFACE-CACHE-SCHEMA: ") + Detail
            + TEXT(" Rebuild the script cache with the matching SoloASCoreExtensions version and Blueprint assets.");
        return EAngelscriptExtensionResult::Error;
    };
    for (const auto& Cached : *Context.CachedTypes)
    {
        const FString* Encoded = Cached.Metadata.Find(SchemaKey);
        if (!Encoded && Cached.ClassName.StartsWith(TEXT("U__SoloASInterfaceSchema_")))
            return Reject(TEXT("Missing interface schema in module '") + Cached.ModuleName + TEXT("'."));
        if (!Encoded) continue;
        FString Json;
        TSharedPtr<FJsonObject> Schema;
        double Version = 0;
        const TArray<TSharedPtr<FJsonValue>> *Declarations = nullptr, *Mappings = nullptr;
        if (!FBase64::Decode(*Encoded, Json)
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Schema) || !Schema
            || !Schema->TryGetNumberField(TEXT("version"), Version) || Version != 1
            || !Schema->TryGetArrayField(TEXT("declarations"), Declarations)
            || !Schema->TryGetArrayField(TEXT("imports"), Mappings))
            return Reject(TEXT("Missing, malformed or unsupported interface schema version."));
        if (!Declarations->IsEmpty() && !IsEnabled())
            return Reject(TEXT("Cached interfaces require bEnableScriptInterfaces=true."));
        for (const auto& Value : *Declarations)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FDeclaration Declaration;
            if (!Value->TryGetObject(Entry) || !Entry || !*Entry
                || !(*Entry)->TryGetStringField(TEXT("name"), Declaration.Name)
                || !(*Entry)->TryGetStringField(TEXT("parent"), Declaration.Parent)
                || !(*Entry)->TryGetStringField(TEXT("module"), Declaration.Module)
                || !(*Entry)->TryGetStringField(TEXT("source"), Declaration.Source)
                || !(*Entry)->TryGetStringField(TEXT("view"), Declaration.View))
                return Reject(TEXT("Incomplete interface declaration."));
            if (Views.Contains(Declaration.View)) continue;
            const FString Code = Mask(Declaration.Source);
            const int32 Open = Code.Find(TEXT("{")), Close = Open == INDEX_NONE ? INDEX_NONE : CloseBrace(Code, Open);
            if (Close == INDEX_NONE || !ParseMethods(Declaration.Source.Mid(Open + 1, Close - Open - 1), Declaration.Methods, Context.Error))
                return Reject(TEXT("Invalid cached interface declaration: ") + Context.Error);
            Views.Add(Declaration.View);
            Pending.Add(MoveTemp(Declaration));
        }
        for (const auto& Value : *Mappings)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FString Name, Path, View;
            if (!Value->TryGetObject(Entry) || !Entry || !*Entry
                || !(*Entry)->TryGetStringField(TEXT("name"), Name)
                || !(*Entry)->TryGetStringField(TEXT("path"), Path)
                || !(*Entry)->TryGetStringField(TEXT("view"), View))
                return Reject(TEXT("Incomplete cached Blueprint mapping."));
            if (!Views.Contains(View)) { Views.Add(View); Imports.Add(*Entry); }
        }
    }
    for (const FString& Name : *Context.CachedNativeTypeNames)
        if ((Name.StartsWith(TEXT("__SoloASInterface_")) || Name.StartsWith(TEXT("__SoloASBlueprint_"))) && !Views.Contains(Name))
            return Reject(TEXT("Missing schema for native VM view '") + Name + TEXT("'."));
    for (const auto& Entry : Imports)
    {
        const FString Name = Entry->GetStringField(TEXT("name"));
        UClass* Class = Cast<UClass>(FSoftObjectPath(Entry->GetStringField(TEXT("path"))).TryLoad());
        if (!Class) return Reject(TEXT("Cached Blueprint class could not be loaded: ") + Name);
        FString ExpectedSignature;
        const bool bHasSignature = Entry->TryGetStringField(TEXT("signature"), ExpectedSignature);
        if (Class->HasAnyClassFlags(CLASS_Interface) && !bHasSignature)
            return Reject(TEXT("Cached Blueprint interface signature is missing: ") + Name);
        const FString View = ImportBlueprint(FString(), Name, Context.Error, Class);
        if (View != Entry->GetStringField(TEXT("view"))) return Reject(TEXT("Blueprint view identity changed: ") + Name);
        if (bHasSignature)
        {
            const auto& Actual = BlueprintImportSchemas.FindChecked(FString()).FindChecked(Name)->AsObject();
            if (Actual->GetStringField(TEXT("signature")) != ExpectedSignature)
                return Reject(TEXT("Blueprint interface signature changed: ") + Name);
        }
    }
    while (!Pending.IsEmpty())
    {
        bool bProgress = false;
        for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
        {
            auto& Declaration = Pending[Index];
            if (!Declaration.Parent.IsEmpty() && (!Resolve(Declaration.Parent)
                || Pending.ContainsByPredicate([&](const auto& Other) { return Other.Name == Declaration.Parent; }))) continue;
            if (InterfaceScriptName(Declaration.Name, Declaration.Parent, Declaration.Methods) != Declaration.View)
                return Reject(TEXT("Versioned interface view does not match its declaration: ") + Declaration.Name);
            if (!Create(Declaration.Name, Declaration.Parent, Declaration.Source, Declaration.Module, MoveTemp(Declaration.Methods), Context.Error))
                return Reject(Context.Error);
            Pending.RemoveAt(Index);
            bProgress = true;
        }
        if (!bProgress) return Reject(TEXT("Cached interface parent is missing or cyclic."));
    }
    PreviousInterfaces.Reset();
    return Views.IsEmpty() ? EAngelscriptExtensionResult::NotHandled : EAngelscriptExtensionResult::Handled;
}

EAngelscriptExtensionResult ConfigureInterfaces(FAngelscriptTypeGenerationContext& Context)
{
    if (Context.Point != EAngelscriptTypeGenerationPoint::ResolveFunction
        && Context.Point != EAngelscriptTypeGenerationPoint::ConfigureClass) return EAngelscriptExtensionResult::NotHandled;
    const FString* Names = Context.Class->Meta.Find(InterfacesKey);
    if (!Names) return EAngelscriptExtensionResult::NotHandled;
    TArray<FString> List; Names->ParseIntoArray(List, TEXT(";"));
    for (const FString& Name : List)
    {
        UClass* Interface = Resolve(Name);
        if (!Interface) { Context.Error = TEXT("The declared interface no longer exists."); return EAngelscriptExtensionResult::Error; }
        if (Context.Point == EAngelscriptTypeGenerationPoint::ConfigureClass)
        {
            UClass* Class = Context.Class->Class;
            if (!Class->ImplementsInterface(Interface))
            {
                FImplementedInterface& Edge = Class->Interfaces.AddDefaulted_GetRef();
                Edge.Class = Interface;
                Edge.PointerOffset = 0;
                Edge.bImplementedByK2 = true;
            }
        }
        else if (UFunction* Function = Interface->FindFunctionByName(FName(*Context.Function->FunctionName)))
        {
            if (Context.ResolvedFunction && !Context.ResolvedFunction->IsSignatureCompatibleWith(Function))
            { Context.Error = TEXT("Interface methods with the same name have incompatible signatures."); return EAngelscriptExtensionResult::Error; }
            Context.ResolvedFunction = Function;
        }
    }
    return EAngelscriptExtensionResult::Handled;
}
#if WITH_EDITOR && !IS_MONOLITHIC
namespace
{
FDelegateHandle DefaultCompileHandle;
FDelegateHandle BlueprintCompiledHandle;
FTSTicker::FDelegateHandle BlueprintInterfaceReloadTicker;
TArray<TWeakObjectPtr<UBlueprint>> InterfaceBlueprints;

struct FSuspendedBlueprintInterface
{
    TWeakObjectPtr<UClass> Class;
    FImplementedInterface Edge;
};
TArray<FSuspendedBlueprintInterface> SuspendedBlueprintInterfaces;

// Entries are weak: an incompatible generation must not be kept alive by its guard.
TMap<TWeakObjectPtr<UFunction>, FNativeFuncPtr> SuspendedBlueprintFunctions;

void RejectSuspendedBlueprintFunction(UObject* Context, FFrame& Stack, RESULT_DECL)
{
    UFunction* Function = Stack.CurrentNativeFunction;
    UE_LOG(LogTemp, Display, TEXT("SoloASCoreExtensions: rejected incompatible cached Blueprint interface call %s; "
        "repair its script implementation before calling it again."), *Function->GetPathName());
    if (Stack.Code)
    {
        // This is the caller's BP frame, not the old AS callee's layout. Do not
        // Step/SkipFunction: new bytecode arguments may be wider than old parms.
        // ProcessLocalScriptFunction clears its own true return property on abort.
        Stack.bAbortingExecution = true;
    }
    else if (RESULT_PARAM)
    {
        // ProcessEvent's compiled-in frame obeys this UFunction's own parameter ABI.
        if (FProperty* Return = Function->GetReturnProperty()) Return->ClearValue(RESULT_PARAM);
    }
}

void RestoreBlueprintFunctionGuards()
{
    for (const auto& Pair : SuspendedBlueprintFunctions)
        if (UFunction* Function = Pair.Key.Get())
            if (Function->GetNativeFunc() == &RejectSuspendedBlueprintFunction) Function->SetNativeFunc(Pair.Value);
    SuspendedBlueprintFunctions.Reset();
}

void SuspendBlueprintFunctions(UClass* Class, UClass* Interface)
{
    for (TFieldIterator<UFunction> It(Interface); It; ++It)
        if (UFunction* Function = Class->FindFunctionByName(It->GetFName()))
            if (!Function->IsSignatureCompatibleWith(*It)
                && Function->HasAnyFunctionFlags(FUNC_Native) && !Function->HasAnyFunctionFlags(FUNC_NetFuncFlags)
                && !SuspendedBlueprintFunctions.Contains(Function))
            {
                SuspendedBlueprintFunctions.Add(Function, Function->GetNativeFunc());
                Function->SetNativeFunc(&RejectSuspendedBlueprintFunction);
            }
}

bool MatchesBlueprintInterface(UClass* Class, UClass* Interface)
{
    for (TFieldIterator<UFunction> It(Interface); It; ++It)
        if (UFunction* Implementation = Class->FindFunctionByName(It->GetFName()))
            if (!Implementation->IsSignatureCompatibleWith(*It)) return false;
    return true;
}

void RefreshBlueprintInterfaceImplementations()
{
    if (!FAngelscriptManager::IsInitialized()) return;
    RestoreBlueprintFunctionGuards();
    // Blueprint messages use ImplementsInterface before ProcessEvent. A failed AS
    // rebuild must never expose an implementation with the old parameter layout.
    SuspendedBlueprintInterfaces.RemoveAll([](const FSuspendedBlueprintInterface& Suspended)
    {
        UClass* Class = Suspended.Class.Get();
        if (!Class) return true;
        if (!MatchesBlueprintInterface(Class, Suspended.Edge.Class))
        {
            SuspendBlueprintFunctions(Class, Suspended.Edge.Class);
            return false;
        }
        if (!Class->Interfaces.ContainsByPredicate([&](const FImplementedInterface& Edge)
            { return Edge.Class == Suspended.Edge.Class; })) Class->Interfaces.Add(Suspended.Edge);
        return true;
    });
    for (const auto& Module : FAngelscriptManager::Get().GetActiveModules())
        for (const auto& Desc : Module->Classes)
            if (UClass* Class = Desc->Class)
                for (int32 Index = Class->Interfaces.Num() - 1; Index >= 0; --Index)
                {
                    const FImplementedInterface Edge = Class->Interfaces[Index];
                    // Only imported Blueprint interfaces belong to this adapter.
                    if (!ActiveInterfaceTypes.Contains(Edge.Class)
                        || !Cast<UBlueprint>(Edge.Class->ClassGeneratedBy)
                        || MatchesBlueprintInterface(Class, Edge.Class)) continue;
                    SuspendBlueprintFunctions(Class, Edge.Class);
                    SuspendedBlueprintInterfaces.Add({Class, Edge});
                    Class->Interfaces.RemoveAt(Index);
                    UE_LOG(LogTemp, Display, TEXT("SoloASCoreExtensions: suspended incompatible Blueprint interface %s on %s "
                        "until its script implementation is rebuilt."),
                        *Edge.Class->GetName(), *Class->GetName());
                }
}

bool ReloadChangedBlueprintInterfaces(float)
{
    if (!FAngelscriptManager::IsInitialized()) { BlueprintInterfaceReloadTicker.Reset(); return false; }
    auto& Manager = FAngelscriptManager::Get();
    if (Manager.bIsCompilingModules || IsAsyncLoading() || IsGarbageCollecting()) return true;
    BlueprintInterfaceReloadTicker.Reset();
    bool bChanged = false;
    for (const auto& File : BlueprintImportSchemas)
    {
        if (File.Key.IsEmpty()) continue;
        for (const auto& Mapping : File.Value)
        {
            const auto& Schema = Mapping.Value->AsObject();
            FString Previous;
            if (!Schema->TryGetStringField(TEXT("signature"), Previous)) continue;
            UClass* Class = FindObject<UClass>(nullptr, *Schema->GetStringField(TEXT("path")));
            if (!Class || !Class->HasAnyClassFlags(CLASS_Interface)) continue;
            TArray<FMethod> Methods;
            FString Signature, Error;
            if (!DescribeBlueprintInterface(Class, Methods, Signature, Error) || Previous != Signature)
            {
                for (const auto& Module : Manager.GetActiveModules())
                    for (const auto& Section : Module->Code)
                        if (Section.AbsoluteFilename == File.Key)
                        {
                            Manager.FileChangesDetectedForReload.AddUnique({Section.AbsoluteFilename, Section.RelativeFilename});
                            for (const auto& Desc : Module->Classes) Desc->bPendingFullReload = true;
                            bChanged = true;
                        }
                break;
            }
        }
    }
    if (bChanged)
    {
        Manager.LastFileChangeDetectedTime = -1.0;
        Manager.CheckForHotReload(ECompileType::FullReload);
    }
    return false;
}

void CommitInterfaceDeclarations(const TArray<UObject*>&)
{
    TMap<UObject*, UObject*> Replacements;
    TSet<UObject*> Retired;
    TSet<UObject*> SignatureClasses;
    for (const auto& Version : InterfaceVersions) SignatureClasses.Add(Version.Value->Class.Get());
    for (const auto& Pair : PreviousInterfaces)
    {
        if (!Pair.Value.IsValid()) continue;
        UClass* Old = Pair.Value->Class.Get();
        UClass* Current = Interfaces.FindChecked(Pair.Key)->Class.Get();
        if (Old == Current) continue;
        const FName CanonicalName = Old->GetFName();
        Old->Rename(*Pair.Value->ScriptName, nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
        Current->Rename(*CanonicalName.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
        Old->ClassFlags |= CLASS_NewerVersionExists;
        Current->ClassFlags &= ~CLASS_NewerVersionExists;
        Replacements.Add(Old, Current);
        Retired.Add(Old);
        for (TFieldIterator<UFunction> It(Old, EFieldIteratorFlags::ExcludeSuper); It; ++It)
            if (UFunction* Function = Current->FindFunctionByName(It->GetFName())) Replacements.Add(*It, Function);
    }
    if (Replacements.IsEmpty()) return;
    for (UObject* Referencer : FReferencerFinder::GetAllReferencers(Retired, &SignatureClasses))
    {
        bool bOldSignature = false;
        for (UObject* Outer = Referencer; Outer; Outer = Outer->GetOuter()) bOldSignature |= SignatureClasses.Contains(Outer);
        if (bOldSignature) continue;
        FArchiveReplaceObjectRef<UObject> Replace(Referencer, Replacements,
            EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
        UBlueprint* Blueprint = Cast<UBlueprint>(Referencer);
        if (!Blueprint) Blueprint = Referencer->GetTypedOuter<UBlueprint>();
        if (!Blueprint)
            if (UClass* Class = Cast<UClass>(Referencer)) Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy);
        if (Blueprint) InterfaceBlueprints.AddUnique(Blueprint);
    }
    for (const auto& Weak : InterfaceBlueprints)
        if (UBlueprint* Blueprint = Weak.Get())
        {
            TArray<UEdGraph*> Graphs;
            Blueprint->GetAllGraphs(Graphs);
            for (UEdGraph* Graph : Graphs)
            {
                const auto Nodes = Graph->Nodes;
                for (UEdGraphNode* Node : Nodes)
                {
                    FArchiveReplaceObjectRef<UObject> ReplaceNode(Node, Replacements,
                        EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
                    // Interface terminators must resolve against the declaration,
                    // rather than the still-old Blueprint skeleton signature.
                    for (const auto& Implemented : Blueprint->ImplementedInterfaces)
                        if (Implemented.Graphs.Contains(Graph))
                            if (auto* Terminator = Cast<UK2Node_FunctionTerminator>(Node))
                            {
                                Terminator->FunctionReference.SetExternalMember(Graph->GetFName(), Implemented.Interface);
                                UFunction* Signature = Implemented.Interface->FindFunctionByName(Graph->GetFName());
                                for (UEdGraphPin* Pin : Terminator->Pins)
                                    if (Pin->LinkedTo.IsEmpty())
                                        if (FProperty* Property = FindFProperty<FProperty>(Signature, Pin->PinName))
                                        {
                                            FEdGraphPinType Type;
                                            const auto* Schema = GetDefault<UEdGraphSchema_K2>();
                                            Schema->ConvertPropertyToPinType(Property, Type);
                                            const auto Numeric = [](const FEdGraphPinType& Value)
                                            { return Value.PinCategory == UEdGraphSchema_K2::PC_Int || Value.PinCategory == UEdGraphSchema_K2::PC_Real; };
                                            // A literal default needs no conversion node. Keeping it as
                                            // text also permits restoring float back to int without a
                                            // newly-created lossy conversion wire in the user's graph.
                                            if (Numeric(Pin->PinType) && Numeric(Type)) Pin->PinType = Type;
                                        }
                                Terminator->ReconstructNode();
                            }
                }
            }
        }
    PreviousInterfaces.Reset();
}

void RefreshInterfaceBlueprints(bool)
{
    RefreshBlueprintInterfaceImplementations();
    // A declaration used only by Blueprint/local variables may leave every AS
    // reflected class unchanged. Publish at the same accepted reload boundary.
    if (!PreviousInterfaces.IsEmpty()) CommitInterfaceDeclarations({});
    if (InterfaceBlueprints.IsEmpty()) return;
    auto Blueprints = MoveTemp(InterfaceBlueprints);
    InterfaceBlueprints.Reset();
    for (const auto& Weak : Blueprints)
        if (UBlueprint* Blueprint = Weak.Get())
        {
            FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
        }
}

void SupplyBlueprintDefaults(UBlueprint* Blueprint)
{
    if (!Blueprint || Blueprint->BlueprintType == BPTYPE_Interface) return;
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    for (FBPInterfaceDescription& Implemented : Blueprint->ImplementedInterfaces)
    {
        const FInterface* Definition = nullptr;
        for (const auto& Pair : Interfaces)
            if (Pair.Value->Class.Get() == Implemented.Interface) { Definition = &Pair.Value.Get(); break; }
        if (!Definition) continue;
        for (const FMethod& Method : Definition->Methods)
        {
            if (Method.DefaultFunction.IsEmpty()) continue;
            const auto Module = FAngelscriptManager::Get().GetModule(Method.ModuleName);
            const auto Statics = Module.IsValid() ? Module->GetStaticsClass() : nullptr;
            UFunction* Default = Statics.IsValid() && Statics->Class
                ? Statics->Class->FindFunctionByName(FName(*Method.DefaultFunction)) : nullptr;
            if (!Default) continue;
            bool bEventImplemented = false;
            for (UEdGraph* Graph : Blueprint->UbergraphPages)
                for (UEdGraphNode* Node : Graph->Nodes)
                    if (const auto* Event = Cast<UK2Node_Event>(Node))
                        bEventImplemented |= Event->EventReference.GetMemberName() == FName(*Method.Name);
            if (bEventImplemented) continue;
            UEdGraph* Graph = nullptr;
            for (UEdGraph* Candidate : Implemented.Graphs)
                if (Candidate->GetFName() == FName(*Method.Name)) { Graph = Candidate; break; }
            if (!Graph)
            {
                Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Method.Name),
                    UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
                FBlueprintEditorUtils::AddInterfaceGraph(Blueprint, Graph, Implemented.Interface);
                Implemented.Graphs.Add(Graph);
            }
            UK2Node_FunctionEntry* Entry = nullptr;
            bool bAuthored = false;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (auto* Candidate = Cast<UK2Node_FunctionEntry>(Node)) Entry = Candidate;
                else if (auto* Result = Cast<UK2Node_FunctionResult>(Node))
                {
                    for (UEdGraphPin* Pin : Result->Pins)
                        if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                            bAuthored |= Pin->LinkedTo.Num() > 0 || !Pin->DoesDefaultValueMatchAutogenerated();
                }
                else bAuthored = true;
            }
            if (!Entry || bAuthored) continue;
            UK2Node_FunctionResult* Result = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
            auto* Call = NewObject<UK2Node_CallFunction>(Graph);
            Graph->AddNode(Call, false, false);
            Call->SetFromFunction(Default);
            Call->CreateNewGuid(); Call->PostPlacedNewNode(); Call->AllocateDefaultPins();
            auto* Self = NewObject<UK2Node_Self>(Graph);
            Graph->AddNode(Self, false, false);
            Self->CreateNewGuid(); Self->PostPlacedNewNode(); Self->AllocateDefaultPins();
            Schema->TryCreateConnection(Self->FindPin(UEdGraphSchema_K2::PN_Self), Call->FindPin(TEXT("__SoloASInterfaceTarget")));
            if (UEdGraphPin* Then = Entry->FindPin(UEdGraphSchema_K2::PN_Then))
            {
                Then->BreakAllPinLinks();
                Schema->TryCreateConnection(Then, Call->FindPin(UEdGraphSchema_K2::PN_Execute));
            }
            Schema->TryCreateConnection(Call->FindPin(UEdGraphSchema_K2::PN_Then), Result->FindPin(UEdGraphSchema_K2::PN_Execute));
            for (const FString& Name : Method.Arguments)
                Schema->TryCreateConnection(Entry->FindPin(FName(*Name)), Call->FindPin(FName(*Name)));
            for (UEdGraphPin* Pin : Result->Pins)
                if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                    if (UEdGraphPin* Value = Call->FindPin(Pin->PinName)) Schema->TryCreateConnection(Value, Pin);
        }
    }
}
}
void StartInterfaceReloadCallbacks()
{
    if (ReloadHadErrorsHandle.IsValid()) return;
    ReloadHadErrorsHandle = FAngelscriptCodeModule::GetReloadHadErrors().AddStatic(&RestoreInterfaceDeclarations);
    PostCompileHandle = FAngelscriptCodeModule::GetPostCompile().AddLambda([] { PreviousInterfaces.Reset(); });
#if WITH_EDITOR && !IS_MONOLITHIC
    PreFullReloadHandle = FAngelscriptClassGenerator::OnPreFullReload.AddStatic(&CommitInterfaceDeclarations);
    PostReloadHandle = FAngelscriptClassGenerator::OnPostReload.AddStatic(&RefreshInterfaceBlueprints);
#endif
}
void StopInterfaceReloadCallbacks()
{
    if (!ReloadHadErrorsHandle.IsValid()) return;
    FAngelscriptCodeModule::GetReloadHadErrors().Remove(ReloadHadErrorsHandle);
    FAngelscriptCodeModule::GetPostCompile().Remove(PostCompileHandle);
    ReloadHadErrorsHandle.Reset();
    PostCompileHandle.Reset();
#if WITH_EDITOR && !IS_MONOLITHIC
    FAngelscriptClassGenerator::OnPreFullReload.Remove(PreFullReloadHandle);
    FAngelscriptClassGenerator::OnPostReload.Remove(PostReloadHandle);
    PreFullReloadHandle.Reset();
    PostReloadHandle.Reset();
#endif
}
void StartBlueprintInterfaceDefaults()
{
    if (GEditor && !DefaultCompileHandle.IsValid())
    {
        DefaultCompileHandle = GEditor->OnBlueprintPreCompile().AddStatic(&SupplyBlueprintDefaults);
        BlueprintCompiledHandle = GEditor->OnBlueprintCompiled().AddLambda([]
        {
            RefreshBlueprintInterfaceImplementations();
            if (!BlueprintInterfaceReloadTicker.IsValid())
                BlueprintInterfaceReloadTicker = FTSTicker::GetCoreTicker().AddTicker(
                    FTickerDelegate::CreateStatic(&ReloadChangedBlueprintInterfaces));
        });
    }
}
void StopBlueprintInterfaceDefaults()
{
    RestoreBlueprintFunctionGuards();
    if (GEditor)
    {
        GEditor->OnBlueprintPreCompile().Remove(DefaultCompileHandle);
        GEditor->OnBlueprintCompiled().Remove(BlueprintCompiledHandle);
    }
    if (BlueprintInterfaceReloadTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(BlueprintInterfaceReloadTicker);
    BlueprintInterfaceReloadTicker.Reset();
    BlueprintCompiledHandle.Reset();
    DefaultCompileHandle.Reset();
}
#endif

}

#else
#endif
