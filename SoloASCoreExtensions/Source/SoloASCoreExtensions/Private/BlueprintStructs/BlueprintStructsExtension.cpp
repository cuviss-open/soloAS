// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "BlueprintStructs/BlueprintStructsExtension.h"
#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "GameDelegates.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/GarbageCollectionSchema.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/Stack.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#if WITH_EDITOR
#include "AngelscriptExecutionQuiescence.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "UObject/UObjectGlobals.h"
#endif
#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "EndAngelscriptHeaders.h"

namespace SoloASCoreExtensions::BlueprintStructs
{
namespace
{
struct FBinding final : FAngelscriptType
{
    FString Name, Path, Schema;
    TStrongObjectPtr<UUserDefinedStruct> Asset;
    TStrongObjectPtr<UUserDefinedStruct> Layout;
    asITypeInfo* Info = nullptr;
    FString GetAngelscriptTypeName() const override { return Name; }
    UStruct* GetUnrealStruct(const FAngelscriptTypeUsage&) const override { return Layout.Get(); }
    void* GetData() const override { return Layout.Get(); }
    asITypeInfo* GetAngelscriptTypeInfo(const FAngelscriptTypeUsage&) const override { return Info; }
    bool IsUnrealStruct() const override { return true; }
    bool CanCreateProperty(const FAngelscriptTypeUsage&) const override { return true; }
    FProperty* CreateProperty(const FAngelscriptTypeUsage&, const FPropertyParams& Params) const override
    {
        auto* Property = CastFieldChecked<FStructProperty>(FStructProperty::StaticClass()->Construct(Params.Outer, Params.PropertyName));
        Property->Struct = Layout.Get();
        return Property;
    }
    bool MatchesProperty(const FAngelscriptTypeUsage&, const FProperty* Property, EPropertyMatchType) const override
    {
        const auto* Value = CastField<FStructProperty>(Property);
        // Only the immutable layout is registered. Mutable asset memory must not
        // enter a compiled thunk that can outlive a structure editor rebuild.
        return Value && Value->Struct == Layout.Get();
    }
    bool CanCopy(const FAngelscriptTypeUsage&) const override { return true; }
    void CopyValue(const FAngelscriptTypeUsage&, void* Source, void* Destination) const override
    { Layout->CopyScriptStruct(Destination, Source); }
    bool CanConstruct(const FAngelscriptTypeUsage&) const override { return true; }
    void ConstructValue(const FAngelscriptTypeUsage&, void* Destination) const override { Layout->InitializeStruct(Destination); }
    bool CanDestruct(const FAngelscriptTypeUsage&) const override { return true; }
    void DestructValue(const FAngelscriptTypeUsage&, void* Destination) const override { Layout->DestroyStruct(Destination); }
    int32 GetValueSize(const FAngelscriptTypeUsage&) const override { return Layout->GetStructureSize(); }
    int32 GetValueAlignment(const FAngelscriptTypeUsage&) const override { return Layout->GetMinAlignment(); }
    bool CanCompare(const FAngelscriptTypeUsage&) const override { return true; }
    bool IsValueEqual(const FAngelscriptTypeUsage&, void* A, void* B) const override { return Layout->CompareScriptStruct(A, B, 0); }
    bool HasReferences(const FAngelscriptTypeUsage&) const override
    {
        TArray<const FStructProperty*> Encountered;
        for (TFieldIterator<FProperty> It(Layout.Get()); It; ++It)
            if (It->ContainsObjectReference(Encountered)) return true;
        return false;
    }
    void EmitReferenceInfo(const FAngelscriptTypeUsage&, FGCReferenceParams& Params) const override
    {
        TArray<const FStructProperty*> Encountered;
        UE::GC::FPropertyStack PathStack;
        for (TFieldIterator<FProperty> It(Layout.Get()); It; ++It)
            It->EmitReferenceInfo(*Params.Schema, Params.AtOffset, Encountered, PathStack);
    }
    bool CanBeArgument(const FAngelscriptTypeUsage&) const override { return true; }
    void SetArgument(const FAngelscriptTypeUsage& Usage, int32 Index, asIScriptContext* Context, FFrame& Stack, const FArgData& Data) const override
    {
        Layout->InitializeStruct(Data.StackPtr);
        if (Usage.bIsReference)
        {
            uint8& Value = Stack.StepCompiledInRef<FStructProperty, uint8>(static_cast<uint8*>(Data.StackPtr));
            Context->SetArgAddress(Index, &Value);
        }
        else
        {
            Stack.StepCompiledIn<FStructProperty>(Data.StackPtr);
            Context->SetArgObject(Index, Data.StackPtr);
        }
    }
    bool CanBeReturned(const FAngelscriptTypeUsage&) const override { return true; }
    void GetReturnValue(const FAngelscriptTypeUsage& Usage, asIScriptContext* Context, void* Destination) const override
    {
        if (Usage.bIsReference) *static_cast<void**>(Destination) = Context->GetReturnAddress();
        else if (void* Value = Context->GetReturnObject()) Layout->CopyScriptStruct(Destination, Value);
    }
};
// The type database and VM retain these type/lifecycle owners for the VM lifetime.
TArray<TSharedRef<FBinding>> Bindings;
FDelegateHandle CookHandle, SourceHandle;
FString PreparationError;
bool bPrepared = false, bDeclared = false, bBound = false, bExpired = false;
const TCHAR* RestartDiagnostic = TEXT("SOLOAS-BLUEPRINT-STRUCT-RESTART: A configured Blueprint struct changed. Existing script values keep the previous layout; restart the editor and regenerate caches to use the new layout.");

bool Enabled()
{
    bool Value = true;
    GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableBlueprintStructs"), Value, GEngineIni);
    return Value;
}

bool IsIdentifier(const FString& Name)
{
    if (Name.IsEmpty()) return false;
    for (int32 I = 0; I < Name.Len(); ++I)
    {
        const TCHAR C = Name[I];
        if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_'
            || (I > 0 && C >= '0' && C <= '9'))) return false;
    }
    return true;
}

FString Describe(UScriptStruct* Struct)
{
    FString Result = FString::Printf(TEXT("%s|%d|%d\n"), *Struct->GetPathName(), Struct->GetStructureSize(), Struct->GetMinAlignment());
    for (TFieldIterator<FProperty> It(Struct); It; ++It)
        Result += FString::Printf(TEXT("%s|%s|%s|%d|%d|%d\n"), *It->GetName(), *It->GetAuthoredName(),
            *It->GetCPPType(), It->GetOffset_ForInternal(), It->GetElementSize(), It->ArrayDim);
    return Result;
}

bool ValidateFields(UScriptStruct* Struct, FString& Error, bool bTypes)
{
    TSet<FString> Names;
    for (TFieldIterator<FProperty> It(Struct); It; ++It)
    {
        const FString Name = It->GetAuthoredName();
        if (!IsIdentifier(Name) || Names.Contains(Name) || It->ArrayDim != 1)
        { Error = TEXT("Blueprint struct fields require unique ASCII identifiers: ") + Name; return false; }
        Names.Add(Name);
        if (bTypes && !FAngelscriptTypeUsage::FromProperty(*It).IsValid())
        { Error = TEXT("Unsupported Blueprint struct field: ") + Name + TEXT(". Configure all nested UDS types in BlueprintStructBindings."); return false; }
    }
    return true;
}

void RemapField(FProperty* Property, const TMap<UScriptStruct*, UScriptStruct*>& Layouts)
{
    if (auto* Struct = CastField<FStructProperty>(Property))
    {
        if (UScriptStruct* const* Frozen = Layouts.Find(Struct->Struct)) Struct->Struct = *Frozen;
    }
    else if (auto* Array = CastField<FArrayProperty>(Property)) RemapField(Array->Inner, Layouts);
    else if (auto* Set = CastField<FSetProperty>(Property)) RemapField(Set->ElementProp, Layouts);
    else if (auto* Map = CastField<FMapProperty>(Property)) { RemapField(Map->KeyProp, Layouts); RemapField(Map->ValueProp, Layouts); }
}
#if WITH_EDITOR
class FStructEditListener final : public FStructureEditorUtils::INotifyOnStructChanged
{
    TUniquePtr<FAngelscriptExecutionWriteScope> Scope;
    uint32 ChangeDepth = 0;
    void PreChange(const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo) override
    {
        if (!Bindings.ContainsByPredicate([&](const auto& Binding) { return Binding->Asset.Get() == Changed; })) return;
        if (ChangeDepth++ == 0)
            Scope = MakeUnique<FAngelscriptExecutionWriteScope>(FAngelscriptExecutionWriteScope::EAcquire::StructLayoutEdit);
        bExpired = true;
        UE_LOG(LogTemp, Warning, TEXT("%s"), RestartDiagnostic);
    }
    void PostChange(const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo) override
    {
        if (!Bindings.ContainsByPredicate([&](const auto& Binding) { return Binding->Asset.Get() == Changed; })) return;
        if (ChangeDepth > 0 && --ChangeDepth == 0) Scope.Reset();
    }
};
TUniquePtr<FStructEditListener> EditListener;
#endif

void PrepareConfigured()
{
    if (bPrepared) return;
    bPrepared = true;
    if (!Enabled()) return;
    PrepareConfiguredBlueprintStructs(PreparationError);
}

void CheckSource(FAngelscriptPreprocessor& P)
{
    FString Error = PreparationError;
    if (Error.IsEmpty()) ValidateBlueprintStructLayouts(Error);
    if (!Error.IsEmpty())
    {
        P.bHasError = true;
        if (!P.Files.IsEmpty()) P.FileWideError(P.Files[0], Error);
    }
}

AS_FORCE_LINK const FAngelscriptBinds::FBind Declarations(int32(FAngelscriptBinds::EOrder::Early) + 2, []
{
    PrepareConfigured();
    FString Error = PreparationError;
    if (Error.IsEmpty()) BindBlueprintStructs(true, Error);
    if (!Error.IsEmpty()) UE_LOG(LogTemp, Fatal, TEXT("%s"), *Error);
});
AS_FORCE_LINK const FAngelscriptBinds::FBind Details(int32(FAngelscriptBinds::EOrder::Late) + 110, []
{
    FString Error;
    if (!BindBlueprintStructs(false, Error)) UE_LOG(LogTemp, Fatal, TEXT("%s"), *Error);
    const FString Root = FAngelscriptManager::GetScriptRootDirectory();
    const bool HasCache = FPaths::FileExists(Root / TEXT("PrecompiledScript.Cache"))
        || FPaths::FileExists(Root / TEXT("PrecompiledScript_Development.Cache"))
        || FPaths::FileExists(Root / TEXT("PrecompiledScript_Test.Cache"))
        || FPaths::FileExists(Root / TEXT("PrecompiledScript_Shipping.Cache"));
    if (FAngelscriptManager::Get().bUsePrecompiledData && HasCache && !Bindings.IsEmpty())
    {
        FString Text, Schema;
        TSharedPtr<FJsonObject> Manifest;
        if (!FFileHelper::LoadFileToString(Text, *(FAngelscriptManager::GetScriptRootDirectory() / TEXT("CoreExtensions.packaging.json")))
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Manifest) || !Manifest
            || !Manifest->TryGetStringField(TEXT("blueprintStructSchema"), Schema)
            || !ValidateBlueprintStructSchema(Schema, Error))
            UE_LOG(LogTemp, Fatal, TEXT("SOLOAS-BLUEPRINT-STRUCT-CACHE-SCHEMA: Missing or mismatched schema. Regenerate the script cache."));
    }
});
}

bool PrepareConfiguredBlueprintStructs(FString& Error)
{
    TArray<FString> Entries;
    GConfig->GetArray(TEXT("SoloASCoreExtensions"), TEXT("BlueprintStructBindings"), Entries, GEngineIni);
    TMap<FName, FString> Values;
    TArray<FString> Names;
    for (const FString& Entry : Entries)
    {
        FString Name, Path;
        if (!FParse::Value(*Entry, TEXT("Name="), Name) || !FParse::Value(*Entry, TEXT("Path="), Path)
            || Values.Contains(FName(*Name)))
        { Error = TEXT("BlueprintStructBindings requires unique (Name=FType,Path=\"/Game/Folder/Asset.Asset\") entries."); return false; }
        Values.Add(FName(*Name), Path); Names.Add(Name);
    }
    return PrepareBlueprintStructBindings(Values, Names, Error);
}

bool PrepareBlueprintStructBindings(const TMap<FName, FString>& Values, const TArray<FString>& Dependencies, FString& Error)
{
    Error.Reset();
    TArray<TSharedRef<FBinding>> Candidate;
    TSet<FString> Seen;
    TSet<UUserDefinedStruct*> Assets;
    TArray<FString> Sorted = Dependencies;
    Sorted.Sort();
    for (const FString& Name : Sorted)
    {
        const FString* Path = Values.Find(FName(*Name));
        if (!IsIdentifier(Name) || Seen.Contains(Name) || !Path)
        { Error = TEXT("Invalid, duplicate or unmapped BlueprintStructBindings name: ") + Name; return false; }
        auto Binding = MakeShared<FBinding>();
        Binding->Name = Name; Binding->Path = *Path;
        auto* Struct = Cast<UUserDefinedStruct>(FSoftObjectPath(*Path).TryLoad());
        if (!Struct || Struct->Status != UDSS_UpToDate || Assets.Contains(Struct))
        { Error = TEXT("BlueprintStructBindings requires distinct compiled UDS assets: ") + Name; return false; }
        if (!ValidateFields(Struct, Error, false)) return false;
        Seen.Add(Name); Assets.Add(Struct);
        Binding->Asset.Reset(Struct); Binding->Schema = Describe(Struct);
        Candidate.Add(Binding);
    }
    if (bDeclared) { Error = TEXT("Blueprint struct bindings are fixed for this VM. Restart to change configuration."); return false; }
    // Duplicate before exposing any type, so no old code ever points at the
    // structure editor's mutable field descriptors or default-instance memory.
    TMap<UScriptStruct*, UScriptStruct*> Layouts;
    for (const auto& Binding : Candidate)
    {
#if WITH_EDITOR
        TGuardValue<decltype(GIsDuplicatingClassForReinstancing), bool> Duplicating(GIsDuplicatingClassForReinstancing, true);
        auto* Frozen = CastChecked<UUserDefinedStruct>(StaticDuplicateObject(Binding->Asset.Get(), GetTransientPackage(),
            MakeUniqueObjectName(GetTransientPackage(), UUserDefinedStruct::StaticClass(), *FString(TEXT("SoloAS_") + Binding->Name)), ~RF_Transactional));
        // UDS PostDuplicate assigns a fresh GUID. Keep the asset's serialization
        // identity while its mutable layout is isolated, as UE reinstancing does.
        Frozen->Guid = Binding->Asset->Guid;
        Frozen->Status = UDSS_UpToDate;
        Frozen->PrimaryStruct = nullptr;
        Frozen->SetFlags(RF_Transient);
        Binding->Layout.Reset(Frozen);
#else
        Binding->Layout.Reset(Binding->Asset.Get());
#endif
        Layouts.Add(Binding->Asset.Get(), Binding->Layout.Get());
    }
    for (const auto& Binding : Candidate)
    {
        for (TFieldIterator<FProperty> It(Binding->Layout.Get()); It; ++It) RemapField(*It, Layouts);
        Binding->Layout->Bind(); Binding->Layout->StaticLink(true);
#if WITH_EDITOR
        CastChecked<UUserDefinedStructEditorData>(Binding->Layout->EditorData)->RecreateDefaultInstance();
#endif
    }
    Bindings = MoveTemp(Candidate);
    return true;
}

bool BindBlueprintStructs(bool bDeclare, FString& Error)
{
    Error.Reset();
    if (Bindings.IsEmpty() || (bDeclare ? bDeclared : bBound)) return true;
    auto* Engine = FAngelscriptManager::Get().GetScriptEngine();
    if (bDeclare)
    {
        for (const auto& Binding : Bindings)
            if (Engine->GetTypeInfoByName(TCHAR_TO_ANSI(*Binding->Name)) || FAngelscriptType::GetByAngelscriptTypeName(Binding->Name))
            { Error = TEXT("Blueprint struct script name already registered: ") + Binding->Name; return false; }
        for (const auto& Binding : Bindings)
        {
            auto Binds = FAngelscriptBinds::ValueClass(Binding->Name, Binding->Layout.Get(), FBindFlags());
            Binding->Info = Binds.GetTypeInfo();
            Binding->Info->SetUserData(Binding->Layout.Get());
            FAngelscriptType::Register(Binding);
            Binds.Constructor("void f()", [](void* Address) { FAngelscriptManager::GetCurrentFunctionUserData<UScriptStruct>()->InitializeStruct(Address); }, Binding->Layout.Get());
            Binds.Destructor("void f()", [](void* Address) { FAngelscriptManager::GetCurrentFunctionUserData<UScriptStruct>()->DestroyStruct(Address); }, Binding->Layout.Get());
            Binds.Constructor(TEXT("void f(const ") + Binding->Name + TEXT("& Other)"), [](void* Address, void* Other)
            { auto* Layout = FAngelscriptManager::GetCurrentFunctionUserData<UScriptStruct>(); Layout->InitializeStruct(Address); Layout->CopyScriptStruct(Address, Other); }, Binding->Layout.Get());
            Binds.Method(Binding->Name + TEXT("& opAssign(const ") + Binding->Name + TEXT("& Other)"), [](void* Address, void* Other) -> void*
            { if (Address != Other) FAngelscriptManager::GetCurrentFunctionUserData<UScriptStruct>()->CopyScriptStruct(Address, Other); return Address; }, Binding->Layout.Get());
        }
        bDeclared = true;
        return true;
    }
    for (const auto& Binding : Bindings) if (!ValidateFields(Binding->Layout.Get(), Error, true)) return false;
    for (const auto& Binding : Bindings)
    {
        auto Binds = FAngelscriptBinds::ExistingClass(Binding->Name);
        for (TFieldIterator<FProperty> It(Binding->Layout.Get()); It; ++It)
        {
            FProperty* Property = *It;
            const auto Usage = FAngelscriptTypeUsage::FromProperty(Property);
            FAngelscriptType::FBindParams Params;
            Params.BindClass = &Binds; Params.NameOverride = Property->GetAuthoredName();
            if (!Usage.Type->BindProperty(Usage, Params, Property))
                Binds.Property(Usage.GetAngelscriptDeclaration(FAngelscriptType::MemberVariable) + TEXT(" ") + Params.NameOverride, Property->GetOffset_ForInternal(), Params);
        }
    }
    bBound = true;
    return true;
}

TArray<FString> GetBlueprintStructAssetPaths()
{
    TArray<FString> Result;
    for (const auto& Binding : Bindings) Result.Add(Binding->Path);
    return Result;
}
FString GetBlueprintStructSchema()
{
    FString Result = TEXT("soloas.blueprint-structs/v2\n");
    for (const auto& Binding : Bindings) Result += Binding->Name + TEXT("|") + Binding->Path + TEXT("\n") + Binding->Schema;
    return Result;
}
bool ValidateBlueprintStructSchema(const FString& Schema, FString& Error)
{
    if (Schema == GetBlueprintStructSchema()) return ValidateBlueprintStructLayouts(Error);
    Error = TEXT("SOLOAS-BLUEPRINT-STRUCT-CACHE-SCHEMA: Blueprint struct layout or configuration differs. Regenerate the script cache.");
    return false;
}
bool ValidateBlueprintStructLayouts(FString& Error)
{
    if (bExpired) { Error = RestartDiagnostic; return false; }
    for (const auto& Binding : Bindings)
        if (Binding->Schema != Describe(Binding->Asset.Get())) { Error = RestartDiagnostic; return false; }
    Error.Reset(); return true;
}
void RegisterBlueprintStructCookDependencies()
{
#if WITH_EDITOR
    if (!EditListener) EditListener = MakeUnique<FStructEditListener>();
    if (!CookHandle.IsValid()) CookHandle = FGameDelegates::Get().GetModifyCookDelegate().AddLambda(
        [](TConstArrayView<const ITargetPlatform*>, TArray<FName>& Packages, TArray<FName>&)
        {
            for (const FString& Path : GetBlueprintStructAssetPaths()) Packages.AddUnique(FName(*FPackageName::ObjectPathToPackageName(Path)));
        });
#endif
}
void UnregisterBlueprintStructCookDependencies()
{
#if WITH_EDITOR
    EditListener.Reset();
    if (CookHandle.IsValid()) FGameDelegates::Get().GetModifyCookDelegate().Remove(CookHandle);
    CookHandle.Reset();
#endif
}
void StartBlueprintStructs()
{
    if (!Enabled() || SourceHandle.IsValid()) return;
    SourceHandle = FAngelscriptPreprocessor::OnProcessChunks.AddStatic(&CheckSource);
    RegisterBlueprintStructCookDependencies();
}
void StopBlueprintStructs()
{
    FAngelscriptPreprocessor::OnProcessChunks.Remove(SourceHandle); SourceHandle.Reset();
    UnregisterBlueprintStructCookDependencies();
}
}
