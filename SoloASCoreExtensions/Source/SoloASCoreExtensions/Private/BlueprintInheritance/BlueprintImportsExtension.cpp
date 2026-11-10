// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "BlueprintQuickCallsExtension.h"
#include "BlueprintQuickCallShared.h"
#include "BlueprintInheritance/SoloASBlueprintImportEditor.h"
#include "AngelscriptBinds.h"
#include "AngelscriptBinds/Helper_PODType.h"
#include "AngelscriptManager.h"
#include "AngelscriptCodeModule.h"
#include "AngelscriptOptionalProviders.h"
#include "AngelscriptCoreExtensionPoints.h"
#include "ClassGenerator/ASClass.h"
#include "AngelscriptExecutionQuiescence.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "Internationalization/Regex.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "Misc/SecureHash.h"
#include "Misc/Base64.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Blueprint.h"
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "Editor.h"
#include "UObject/UObjectHash.h"
#endif
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/GarbageCollectionSchema.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "EndAngelscriptHeaders.h"

namespace SoloASCoreExtensions::BlueprintInheritance
{
static void PrepareConfiguredBlueprintImports();
namespace
{
using namespace AngelscriptBlueprintQuickCall;
TArray<FString> BlueprintLayout(UClass* Class)
{
    TArray<FString> Layout;
    for (TFieldIterator<FProperty> It(Class); It; ++It)
    {
        const FProperty* P = *It;
        FString Row = FString::Printf(TEXT("%s:%s:%d:%d:%d"), *P->GetName(), *P->GetCPPType(),
            P->GetOffset_ForInternal(), P->GetSize(), P->ArrayDim);
        if (const auto* Bool = CastField<FBoolProperty>(P))
            Row += FString::Printf(TEXT(":%u:%u"), Bool->GetByteMask(), Bool->GetFieldMask());
        Layout.Add(MoveTemp(Row));
    }
    return Layout;
}
struct FBlueprintView final : TAngelscriptPODType<UObject*>
{
    FString Path, Name;
    TStrongObjectPtr<UClass> Class;
    TStrongObjectPtr<UClass> PendingClass;
    asITypeInfo* Info = nullptr;
    TWeakObjectPtr<UClass> Parent;
    int32 PropertiesSize;
    TArray<FString> Layout;
#if WITH_EDITOR
    TArray<FBPVariableDescription> Variables;
#endif
    TSet<FString> Members;
    FBlueprintView(const FString& InPath, const FString& InName, UClass* InClass)
        : Path(InPath), Name(InName), Class(InClass), Parent(InClass->GetSuperClass()), PropertiesSize(InClass->GetPropertiesSize()), Layout(BlueprintLayout(InClass))
    {
#if WITH_EDITOR
        if (const auto* Blueprint = Cast<UBlueprint>(InClass->ClassGeneratedBy)) Variables = Blueprint->NewVariables;
#endif
    }
    FString GetAngelscriptTypeName() const override { return Name; }
    FString GetAngelscriptTypeName(const FAngelscriptTypeUsage&) const override { return Name; }
    UClass* GetClass(const FAngelscriptTypeUsage&) const override { return Class.Get(); }
    asITypeInfo* GetAngelscriptTypeInfo(const FAngelscriptTypeUsage&) const override { return Info; }
    bool IsObjectPointer() const override { return true; }
    bool CanCreateProperty(const FAngelscriptTypeUsage&) const override { return true; }
    FProperty* CreateProperty(const FAngelscriptTypeUsage&, const FPropertyParams& Params) const override
    {
        auto* Property = CastFieldChecked<FObjectProperty>(FObjectProperty::StaticClass()->Construct(Params.Outer, Params.PropertyName));
        Property->PropertyClass = Class.Get();
        return Property;
    }
    bool MatchesProperty(const FAngelscriptTypeUsage&, const FProperty* Property, EPropertyMatchType) const override
    {
        const auto* Object = CastField<FObjectPropertyBase>(Property);
        return Object && Object->PropertyClass == Class.Get();
    }
    bool NeedConstruct(const FAngelscriptTypeUsage&) const override { return true; }
    void ConstructValue(const FAngelscriptTypeUsage&, void* Address) const override { *static_cast<UObject**>(Address) = nullptr; }
    bool HasReferences(const FAngelscriptTypeUsage&) const override { return true; }
    void EmitReferenceInfo(const FAngelscriptTypeUsage&, FGCReferenceParams& Params) const override
    { Params.Schema->Add(UE::GC::DeclareMember(Params.Names.Top(), Params.AtOffset, UE::GC::EMemberType::Reference)); }
};
TSharedPtr<FBlueprintView> Bind(UClass* Class, FString& Error);
struct FDeferredBlueprintView final : TAngelscriptPODType<UObject*>
{
    FString Name, Path;
    FDeferredBlueprintView(FString InName, FString InPath) : Name(MoveTemp(InName)), Path(MoveTemp(InPath)) {}
    FString GetAngelscriptTypeName() const override { return Name; }
    UClass* GetClass(const FAngelscriptTypeUsage&) const override;
};
// VM type shapes and their callbacks live until the VM ends. Recompiles replace
// only the current class reference; no per-recompile types or function roots accumulate.
TMap<FString, TSharedRef<FBlueprintView>> Views;
TSet<FBlueprintView*> PendingViews;
FDelegateHandle SourceHandle, ReplacedHandle, ShutdownHandle, CarrierHandle, RetryHandle, ReloadClient;
FDelegateHandle DebugAliasHandle;
FDelegateHandle DebugAliasFailedHandle, DebugAliasCompiledHandle;
TMap<FString, TMap<FString, FString>> DebugAliasesBySource;
TMap<FString, TOptional<TMap<FString, FString>>> PreviousDebugAliases;
TSet<FString> PendingReloadModules;
uint64 NextReloadRequest = 1;
TMap<FString, TMap<FString, TWeakObjectPtr<UClass>>> ImportedParents;
bool bStarted = false;
#if WITH_EDITOR && !IS_MONOLITHIC
FImportEditorAdapter ImportEditorAdapter = nullptr;
#endif

FString Mask(const FString& Source)
{
    FString Result = Source;
    bool Line = false, Block = false;
    TCHAR Quote = 0;
    for (int32 I = 0; I < Source.Len(); ++I)
    {
        TCHAR C = Source[I], Next = I + 1 < Source.Len() ? Source[I + 1] : 0;
        if (Line) { if (C == '\n') Line = false; }
        else if (Block) { if (C == '*' && Next == '/') { Result[I++] = ' '; Result[I] = ' '; Block = false; continue; } }
        else if (Quote) { if (C == '\\') { Result[I++] = ' '; if (I < Source.Len()) Result[I] = ' '; continue; } if (C == Quote) Quote = 0; }
        else if (C == '/' && Next == '/') Line = true;
        else if (C == '/' && Next == '*') { Block = true; Result[I++] = ' '; }
        else if (C == '"' || C == '\'') Quote = C;
        else continue;
        if (C != '\n' && C != '\r') Result[I] = ' ';
    }
    return Result;
}

TMap<FString, FString> ProjectBlueprintBindings()
{
    FString Config;
    GConfig->GetString(TEXT("/Script/AngelscriptCode.AngelscriptSettings"), TEXT("BlueprintBindings"), Config, GEngineIni);
    TMap<FString, FString> Bindings;
    FRegexMatcher Entry(FRegexPattern(TEXT("\\(\\s*\\\"?([A-Za-z_][A-Za-z0-9_]*)\\\"?\\s*,\\s*\\\"([^\\\"]+)\\\"\\s*\\)")), Config);
    while (Entry.FindNext()) Bindings.Add(Entry.GetCaptureGroup(1), Entry.GetCaptureGroup(2));
    return Bindings;
}
TArray<FString> ProjectBlueprintDependencies()
{
    TArray<FString> Dependencies;
    GConfig->GetArray(TEXT("/Script/AngelscriptCode.AngelscriptSettings"), TEXT("BlueprintDependencies"), Dependencies, GEngineIni);
    return Dependencies;
}

UClass* Resolve(const FString& Filename, const FString& Name, FString& Error)
{
    FString Path;
    // Unit configuration takes precedence over project aliases. Never accept a
    // path embedded in AS source as a Blueprint identity.
    FString Directory = FPaths::GetPath(Filename);
    while (!Directory.IsEmpty())
    {
        const FString Manifest = Directory / TEXT("FeatureMod.json");
        if (FPaths::FileExists(Manifest))
        {
            FString Text;
            TSharedPtr<FJsonObject> Json;
            if (!FFileHelper::LoadFileToString(Text, *Manifest)
                || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json)
            { Error = TEXT("Cannot read FeatureMod BlueprintBindings configuration."); return nullptr; }
            const TSharedPtr<FJsonObject>* Bindings = nullptr;
            if (Json->TryGetObjectField(TEXT("BlueprintBindings"), Bindings)) (*Bindings)->TryGetStringField(Name, Path);
            break;
        }
        const FString Parent = FPaths::GetPath(Directory);
        if (Parent == Directory) break;
        Directory = Parent;
    }
    if (Path.IsEmpty())
    {
        TArray<FString> Aliases;
        GConfig->GetArray(TEXT("SoloASCoreExtensions"), TEXT("BlueprintAliases"), Aliases, GEngineIni);
        for (const FString& Alias : Aliases)
        {
            FString Key, Value;
            if (!FParse::Value(*Alias, TEXT("Name="), Key) || Key != Name) continue;
            if (!FParse::Value(*Alias, TEXT("Path="), Value) || Value.IsEmpty() || !Path.IsEmpty())
            { Error = TEXT("BlueprintAliases requires one nonempty mapping for '") + Name + TEXT("'."); return nullptr; }
            Path = Value;
        }
    }
    if (Path.IsEmpty() && ProjectBlueprintDependencies().Contains(Name))
    {
        const auto Bindings = ProjectBlueprintBindings();
        if (const FString* Binding = Bindings.Find(Name)) Path = *Binding;
    }
    if (!Path.IsEmpty())
    {
        if (!Path.Contains(TEXT("."))) Path += TEXT(".") + FPaths::GetCleanFilename(Path);
        if (!Path.EndsWith(TEXT("_C"))) Path += TEXT("_C");
        UClass* Class = LoadObject<UClass>(nullptr, *Path);
        if (Class && Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint) && !Class->HasAnyClassFlags(CLASS_Interface)) return Class;
    }
    else
    {
        auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FAssetData> Assets;
        Registry.GetAssetsByClass(FTopLevelAssetPath(UBlueprint::StaticClass()), Assets, true);
        UBlueprint* Match = nullptr;
        for (const auto& Asset : Assets) if (Asset.AssetName == FName(*Name))
        {
            if (Match) { Error = TEXT("Blueprint name is ambiguous; configure BlueprintAliases: ") + Name; return nullptr; }
            Match = Cast<UBlueprint>(Asset.GetAsset());
        }
        if (Match && Match->GeneratedClass && !Match->GeneratedClass->HasAnyClassFlags(CLASS_Interface)) return Match->GeneratedClass;
    }
    Error = TEXT("Blueprint import '") + Name + TEXT("' is missing or has no compiled class; check BlueprintBindings/BlueprintAliases.");
    return nullptr;
}

enum class EViewRefreshResult { Ready, Busy, Rejected };
EViewRefreshResult RefreshView(FBlueprintView& View, UClass* Class, FString& Error)
{
    if (View.Class.Get() == Class)
    {
        if (Class && Class->GetPathName() == View.Path && Class->GetSuperClass() == View.Parent.Get()
            && Class->GetPropertiesSize() == View.PropertiesSize && BlueprintLayout(Class) == View.Layout)
            return EViewRefreshResult::Ready;
        Error = TEXT("Blueprint replacement identity or frozen layout changed; restart the script VM.");
        return EViewRefreshResult::Rejected;
    }
    // Try also rejects a reentrant script callback: never upgrade its reader.
    FAngelscriptExecutionWriteScope Writer(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Writer.IsAcquired())
    { View.PendingClass.Reset(Class); PendingViews.Add(&View); return EViewRefreshResult::Busy; }
    View.PendingClass.Reset();
    PendingViews.Remove(&View);
    const auto Type = FAngelscriptType::GetByAngelscriptTypeName(View.Name);
    if (!Class || Class->GetPathName() != View.Path || Type.Get() != &View
        || Class->GetSuperClass() != View.Parent.Get() || Class->GetPropertiesSize() != View.PropertiesSize || BlueprintLayout(Class) != View.Layout)
    { Error = TEXT("Blueprint replacement identity or frozen layout changed; restart the script VM."); return EViewRefreshResult::Rejected; }
    bool bBusy = false;
    if (!FAngelscriptType::ReindexBlueprintCarrierView(Type.ToSharedRef(), View.Class.Get(), Class, Error, bBusy))
    {
        if (bBusy)
        { View.PendingClass.Reset(Class); PendingViews.Add(&View); Error.Reset(); return EViewRefreshResult::Busy; }
        return EViewRefreshResult::Rejected;
    }
    View.Class.Reset(Class);
    return EViewRefreshResult::Ready;
}
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
void QueueBlueprintConsumers(UClass* Previous, TSet<FString>& Modules)
{
    if (!bStarted) return;
    for (const auto& Module : ImportedParents)
        for (const auto& Parent : Module.Value)
            if (Parent.Value.Get() == Previous) { Modules.Add(Module.Key); break; }
}
#if !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
struct FCompilingBlueprintChild
{
    TStrongObjectPtr<UASClass> Class;
    TStrongObjectPtr<UObject> Default;
};
TArray<FCompilingBlueprintChild> CompilingBlueprintChildren;
TSet<FString> CompilingBlueprintModules;
TUniquePtr<FAngelscriptExecutionWriteScope> BlueprintCompileWriter;
FDelegateHandle BlueprintPreCompileHandle, BlueprintReinstancedHandle, BlueprintCompiledHandle, BlueprintEngineInitHandle;

void PrepareBlueprintChildren(UBlueprint* Blueprint)
{
    if (!bStarted || !Blueprint || !Blueprint->GeneratedClass) return;
    FBlueprintView* View = nullptr;
    for (const auto& Pair : Views)
        if (Pair.Value->Class.Get() == Blueprint->GeneratedClass) { View = &Pair.Value.Get(); break; }
    if (!View) return;
    TSet<FString> Modules;
    QueueBlueprintConsumers(Blueprint->GeneratedClass, Modules);
    if (Modules.IsEmpty()) return;
    // Skeletons include synthetic fields, such as UberGraphFrame, which do
    // not describe the published class storage. Compare the asset declarations.
    bool bSameVariables = Blueprint->NewVariables.Num() == View->Variables.Num();
    for (int32 I = 0; bSameVariables && I < View->Variables.Num(); ++I)
        bSameVariables = Blueprint->NewVariables[I].VarName == View->Variables[I].VarName
            && Blueprint->NewVariables[I].VarType == View->Variables[I].VarType;
    if (!bSameVariables || Blueprint->ParentClass != View->Parent.Get())
    {
        UE_LOG(LogTemp, Error, TEXT("CE-BLUEPRINT-LAYOUT-REJECTED: imported parent layout changed; "
            "public compile notifications cannot cancel this edit. Restart the editor before resuming scripts."));
        return;
    }
    if (!BlueprintCompileWriter)
    {
        auto Writer = MakeUnique<FAngelscriptExecutionWriteScope>(FAngelscriptExecutionWriteScope::EAcquire::Try);
        if (!Writer->IsAcquired())
        {
            UE_LOG(LogTemp, Error, TEXT("CE Blueprint child preparation requires drained script execution."));
            return;
        }
        BlueprintCompileWriter = MoveTemp(Writer);
    }
    TArray<UClass*> Children;
    GetDerivedClasses(Blueprint->GeneratedClass, Children, true);
    for (UClass* Child : Children)
    {
        auto* Script = Cast<UASClass>(Child);
        if (!Script || !Script->ScriptTypePtr) continue;
        auto Module = FAngelscriptManager::Get().GetModule(Script->ScriptTypePtr->GetModule());
        auto Desc = Module ? Module->GetClass(Script->ScriptTypePtr) : nullptr;
        if (!Desc || Desc->Class != Script) continue;
        if (UObject* Default = Script->GetDefaultObject(false))
        {
            CompilingBlueprintChildren.Add({TStrongObjectPtr<UASClass>(Script), TStrongObjectPtr<UObject>(Default)});
            // Stock expects children to have no CDO while it links the parent.
            // Preserve the initialized object, its overrides and its graph.
            Script->SetDefaultObject(nullptr);
        }
    }
    for (const FString& Module : Modules) CompilingBlueprintModules.Add(Module);
}
void RestoreBlueprintChildren()
{
    for (const auto& Child : CompilingBlueprintChildren) Child.Class->SetDefaultObject(Child.Default.Get());
    CompilingBlueprintChildren.Reset();
}
void CompleteBlueprintChildren()
{
    RestoreBlueprintChildren();
    BlueprintCompileWriter.Reset();
    for (const FString& Module : CompilingBlueprintModules) PendingReloadModules.Add(Module);
    CompilingBlueprintModules.Reset();
}
void InstallBlueprintCompileCallbacks()
{
    if (!bStarted || !ReloadClient.IsValid() || !GEditor) return;
    if (!BlueprintPreCompileHandle.IsValid())
        BlueprintPreCompileHandle = GEditor->OnBlueprintPreCompile().AddStatic(&PrepareBlueprintChildren);
    // Synchronous compilation collects garbage after reinstancing and before
    // OnBlueprintCompiled. Live instances need their class CDO during that GC.
    if (!BlueprintReinstancedHandle.IsValid())
        BlueprintReinstancedHandle = GEditor->OnBlueprintReinstanced().AddStatic(&RestoreBlueprintChildren);
    if (!BlueprintCompiledHandle.IsValid())
        BlueprintCompiledHandle = GEditor->OnBlueprintCompiled().AddStatic(&CompleteBlueprintChildren);
}
#endif
void ReloadBlueprintConsumers()
{
    using namespace AngelscriptCoreExtensionPoints;
    if (!bStarted || !ReloadClient.IsValid() || PendingReloadModules.IsEmpty()) return;
#if !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    if (BlueprintCompileWriter) return;
#endif
    FExplicitReloadRequest Request;
    Request.RequestId = NextReloadRequest++;
    TSet<FString> Paths;
    for (const FString& Name : PendingReloadModules)
    {
        auto Module = FAngelscriptManager::Get().GetModule(Name);
        if (!Module) continue;
        for (const auto& Class : Module->Classes) Class->bPendingFullReload = true;
        for (const auto& Section : Module->Code)
            if (!Paths.Contains(Section.RelativeFilename))
            {
                Paths.Add(Section.RelativeFilename);
                Request.Files.Add({Section.RelativeFilename, Section.AbsoluteFilename, false});
            }
    }
    if (Request.Files.IsEmpty()) { PendingReloadModules.Reset(); return; }
    FString Error;
    FReloadResultView Result;
    const auto Admission = TryExplicitReload(ReloadClient, Request,
        [&Result](const FReloadResultView& View) { Result = View; }, Error);
    if (Admission == EReloadAdmission::Busy) return;
    PendingReloadModules.Reset();
    if (Admission != EReloadAdmission::Accepted || Result.Terminal != EReloadTerminal::Committed)
        UE_LOG(LogTemp, Error, TEXT("CE Blueprint child reload failed: %s"), *Error);
}
#endif
bool CanRegisterAlias(const FString& Name, UClass* Class, FString& Error)
{
    const auto Existing = FAngelscriptType::GetByAngelscriptTypeName(Name);
    if (!Existing) return true;
    const auto* Owned = Views.Find(Class->GetPathName());
    if (Owned && Existing.Get() == &Owned->Get()) return true;
    Error = TEXT("CE-BLUEPRINT-ALIAS-CONFLICT: ") + Name + TEXT(" already names a different type; Blueprint alias skipped.");
    return false;
}
UClass* Current(FBlueprintView& View)
{
    UClass* Class = FindObject<UClass>(nullptr, *View.Path);
    // A deleted/renamed BP must not silently construct its retained old class.
    if (!Class || Class->HasAnyClassFlags(CLASS_NewerVersionExists) || Class != View.Class.Get())
    { ThrowQuickCallError(TEXT("Imported Blueprint is missing or was replaced; reload its script consumers: ") + View.Path); return nullptr; }
    return Class;
}
void NewInstance(asIScriptGeneric* G)
{
    auto& View = *static_cast<FBlueprintView*>(G->GetFunction()->GetUserData());
    UClass* Class = Current(View);
    if (!Class) return;
    if (Class->HasAnyClassFlags(CLASS_Abstract) || Class->IsChildOf(AActor::StaticClass()) || Class->IsChildOf(UActorComponent::StaticClass()))
    { ThrowQuickCallError(TEXT("Use the engine spawn/component APIs for this imported Blueprint class.")); return; }
    UObject* Created = NewObject<UObject>(GetTransientPackage(), Class);
    G->SetReturnAddress(Created);
}
void CastInstance(asIScriptGeneric* G)
{
    auto& View = *static_cast<FBlueprintView*>(G->GetFunction()->GetUserData());
    UClass* Class = Current(View);
    UObject* Object = static_cast<UObject*>(G->GetArgObject(0));
    G->SetReturnAddress(Class && IsValid(Object) && Object->IsA(Class) ? Object : nullptr);
}
UObject* Receiver(asIScriptGeneric* G)
{
    const FString Name(ANSI_TO_TCHAR(G->GetFunction()->GetObjectType()->GetName()));
    TSharedPtr<FBlueprintView> View;
    for (const auto& Pair : Views) if (Pair.Value->Name == Name) { View = Pair.Value; break; }
    UObject* Object = static_cast<UObject*>(G->GetObject());
    UClass* Class = View ? Current(*View) : nullptr;
    if (!Class || !IsValid(Object) || !Object->IsA(Class))
    { ThrowQuickCallError(TEXT("Blueprint call requires a valid instance of the imported class.")); return nullptr; }
    return Object;
}
void Invoke(asIScriptGeneric* G)
{
    if (!IsInGameThread()) { ThrowQuickCallError(TEXT("Blueprint calls require the game thread.")); return; }
    UObject* Object = Receiver(G);
    if (!Object) return;
    UFunction* Function = Object->FindFunction(FName(ANSI_TO_TCHAR(G->GetFunction()->GetName())));
    if (!Function) { ThrowQuickCallError(TEXT("Imported Blueprint function was removed; reload its script consumers.")); return; }
    TArray<FProperty*> Params;
    FProperty* Return = Function->GetReturnProperty();
    for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
        if (!It->HasAnyPropertyFlags(CPF_ReturnParm)) Params.Add(*It);
    if (Params.Num() != G->GetArgCount() || (Return == nullptr) != (G->GetReturnTypeId() == asTYPEID_VOID)
        || (Return && !IsBlueprintTypeCompatible(G->GetReturnTypeId(), Return, true)))
    { ThrowQuickCallError(TEXT("Imported Blueprint function signature changed; reload its script consumers.")); return; }
    for (int32 I = 0; I < Params.Num(); ++I)
        if (Params[I]->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm) || !IsBlueprintTypeCompatible(G->GetArgTypeId(I), Params[I]))
        { ThrowQuickCallError(TEXT("Imported Blueprint function argument changed or is unsupported.")); return; }
    FStructOnScope Memory(Function);
    for (int32 I = 0; I < Params.Num(); ++I) CopyValue(Params[I], Params[I]->ContainerPtrToValuePtr<void>(Memory.GetStructMemory()), G->GetAddressOfArg(I));
    Object->ProcessEvent(Function, Memory.GetStructMemory());
    if (Return)
    {
        void* Value = Return->ContainerPtrToValuePtr<void>(Memory.GetStructMemory());
        if (auto* ObjectReturn = CastField<FObjectPropertyBase>(Return)) G->SetReturnAddress(ObjectReturn->GetObjectPropertyValue(Value));
        else if (Return->IsA<FStructProperty>() || Return->IsA<FStrProperty>() || Return->IsA<FTextProperty>()) G->SetReturnObject(Value);
        else CopyValue(Return, G->GetAddressOfReturnLocation(), Value);
    }
}
void Property(asIScriptGeneric* G)
{
    if (!IsInGameThread()) { ThrowQuickCallError(TEXT("Blueprint calls require the game thread.")); return; }
    UObject* Object = Receiver(G);
    if (!Object) return;
    FString Method(ANSI_TO_TCHAR(G->GetFunction()->GetName()));
    bool Setter = Method.RemoveFromStart(TEXT("Set"));
    if (!Setter) Method.RemoveFromStart(TEXT("Get"));
    FProperty* Field = FindFProperty<FProperty>(Object->GetClass(), FName(*Method));
    const int32 TypeId = Setter ? G->GetArgTypeId(0) : G->GetReturnTypeId();
    if (!IsBlueprintTypeCompatible(TypeId, Field, !Setter))
    { ThrowQuickCallError(TEXT("Imported Blueprint property was removed or changed type; reload its script consumers.")); return; }
    void* Value = Field->ContainerPtrToValuePtr<void>(Object);
    if (Setter) CopyValue(Field, Value, G->GetAddressOfArg(0));
    else if (auto* ObjectReturn = CastField<FObjectPropertyBase>(Field)) G->SetReturnAddress(ObjectReturn->GetObjectPropertyValue(Value));
    else if (Field->IsA<FStructProperty>() || Field->IsA<FStrProperty>() || Field->IsA<FTextProperty>()) G->SetReturnObject(Value);
    else CopyValue(Field, G->GetAddressOfReturnLocation(), Value);
}

FString Declaration(FProperty* Field)
{
    auto Usage = FAngelscriptTypeUsage::FromProperty(Field);
    return Usage.IsValid() && IsSupportedBlueprintProperty(Field) ? Usage.GetAngelscriptDeclaration(FAngelscriptType::FunctionReturnValue) : FString();
}
TSharedPtr<FBlueprintView> Bind(UClass* Class, FString& Error)
{
    const FString Path = Class->GetPathName();
    const FString Name = TEXT("__SoloASBlueprint_") + FMD5::HashAnsiString(*Path);
    TSharedPtr<FBlueprintView> View;
    if (const auto* Existing = Views.Find(Path))
    {
        // The VM declaration set is immutable after its first registration.
        // Calls resolve current reflection by name and validate its signature;
        // repeat imports must never grow the native surface captured at startup.
        auto& Published = Existing->Get();
        const auto Result = RefreshView(Published, Class, Error);
        if (Result == EViewRefreshResult::Ready) return *Existing;
        if (Result == EViewRefreshResult::Busy)
        {
            UClass* Previous = Published.Class.Get();
            if (IsValid(Previous) && !Previous->HasAnyClassFlags(CLASS_NewerVersionExists)
                && Published.Info && Published.Info->GetEngine() == FAngelscriptManager::Get().GetScriptEngine()
                && Published.Info->GetUserData() == Previous && FAngelscriptType::GetByClass(Previous).Get() == &Published
                && Previous->GetSuperClass() == Published.Parent.Get() && Previous->GetPropertiesSize() == Published.PropertiesSize
                && BlueprintLayout(Previous) == Published.Layout)
                return *Existing;
            Error = TEXT("Blueprint import is busy and its published view is no longer usable; retry after script execution drains.");
        }
        return nullptr;
    }
    else
    {
        if (FAngelscriptType::GetByAngelscriptTypeName(Name).IsValid())
        { Error = TEXT("Blueprint view name is owned by another binding."); return nullptr; }
        auto Binds = FAngelscriptBinds::ReferenceClass(Name, Class);
        View = MakeShared<FBlueprintView>(Path, Name, Class);
        View->Info = Binds.GetTypeInfo();
        // Runtime expects native type user data to be a UClass; generic receiver
        // callbacks find our view by its stable VM name instead.
        Binds.SetTypeUserData(Class);
        FAngelscriptType::Register(View.ToSharedRef());
        View->Info->CopySystemType(FAngelscriptBinds::ExistingClass("UObject").GetTypeInfo());
        FAngelscriptBinds::BindGlobalGenericFunction(Name + TEXT(" __SoloASNew_") + Name + TEXT("()"), &NewInstance, View.Get());
        FAngelscriptBinds::BindGlobalGenericFunction(Name + TEXT(" __SoloASCast_") + Name + TEXT("(UObject Object)"), &CastInstance, View.Get());
        {
            FAngelscriptBinds::FNamespace Namespace(Name);
            FAngelscriptBinds::BindGlobalGenericFunction("UClass StaticClass()", [](asIScriptGeneric* G)
            { G->SetReturnAddress(Current(*static_cast<FBlueprintView*>(G->GetFunction()->GetUserData()))); }, View.Get());
        }
        Views.Add(Path, View.ToSharedRef());
#if WITH_EDITOR && !IS_MONOLITHIC
        if (bStarted && ImportEditorAdapter && !ImportEditorAdapter(true))
        { Error = TEXT("Cannot protect imported Blueprint structural editing."); return nullptr; }
#endif
    }
    View->Class.Reset(Class);
    auto Binds = FAngelscriptBinds::ExistingClass(Name);
    Binds.SetTypeUserData(Class);
    TSet<FString> CallableSignatures;
    for (TFieldIterator<UFunction> It(Class); It; ++It)
    {
        UFunction* Function = *It;
        if (Function->HasAnyFunctionFlags(FUNC_Static | FUNC_UbergraphFunction)
            || !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent)) continue;
        FString Signature = Function->GetReturnProperty() ? Declaration(Function->GetReturnProperty()) : TEXT("void");
        if (Signature.IsEmpty()) continue;
        Signature += TEXT(" ") + Function->GetName() + TEXT("(");
        FString CallableSignature = Function->GetName() + TEXT("(");
        bool Supported = true;
        int32 Count = 0;
        for (TFieldIterator<FProperty> P(Function); P && P->HasAnyPropertyFlags(CPF_Parm); ++P)
        {
            if (P->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
            FString Type = Declaration(*P);
            if (Type.IsEmpty() || P->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm)) { Supported = false; break; }
            if (Count++) Signature += TEXT(", ");
            Signature += Type + TEXT(" ") + P->GetName();
            CallableSignature += Type + TEXT(";");
        }
        Signature += TEXT(")");
        CallableSignature += TEXT(")");
        if (Supported && !View->Members.Contains(Signature))
        { Binds.GenericMethod(Signature, &Invoke, nullptr); View->Members.Add(Signature); }
        if (Supported) CallableSignatures.Add(CallableSignature);
    }
    for (TFieldIterator<FProperty> It(Class); It; ++It)
    {
        if (!It->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;
        const FString Type = Declaration(*It);
        if (Type.IsEmpty()) continue;
        const FString GetDeclaration = Type + TEXT(" Get") + It->GetName() + TEXT("() const");
        const FString SetDeclaration = TEXT("void Set") + It->GetName() + TEXT("(") + Type + TEXT(" Value)");
        const FString Get = GetDeclaration + TEXT(" property");
        const FString Set = SetDeclaration + TEXT(" property");
        if (!View->Members.Contains(Get) && !View->Info->GetMethodByDecl(TCHAR_TO_ANSI(*GetDeclaration)))
        { Binds.GenericMethod(Get, &Property, nullptr); View->Members.Add(Get); }
        if (!It->HasAnyPropertyFlags(CPF_BlueprintReadOnly) && !View->Members.Contains(Set)
            && !CallableSignatures.Contains(TEXT("Set") + It->GetName() + TEXT("(") + Type + TEXT(";)"))
            && !View->Info->GetMethodByDecl(TCHAR_TO_ANSI(*SetDeclaration)))
        { Binds.GenericMethod(Set, &Property, nullptr); View->Members.Add(Set); }
    }
    return View;
}

UClass* FDeferredBlueprintView::GetClass(const FAngelscriptTypeUsage&) const
{
    UClass* Class = FindObject<UClass>(nullptr, *Path);
    if (!Class) Class = LoadObject<UClass>(nullptr, *Path);
    if (!Class || !Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint) || Class->HasAnyClassFlags(CLASS_Interface)) return nullptr;
    FString Error;
    const auto View = Bind(Class, Error);
    if (!View)
    {
        UE_LOG(LogTemp, Warning, TEXT("CE Blueprint parent could not be bound: %s"), *Error);
        return nullptr;
    }
    FAngelscriptType::RegisterAlias(Name, View.ToSharedRef());
    return Class;
}

void Rewrite(FAngelscriptPreprocessor& P)
{
    TSet<FString> Changed;
    for (const auto& File : P.Files) ImportedParents.Remove(File.Module->ModuleName);
    const auto Dependencies = ProjectBlueprintDependencies();
    for (auto& File : P.Files)
    {
        if (!PreviousDebugAliases.Contains(File.AbsoluteFilename))
        {
            if (const auto* Previous = DebugAliasesBySource.Find(File.AbsoluteFilename))
                PreviousDebugAliases.Add(File.AbsoluteFilename, *Previous);
            else PreviousDebugAliases.Add(File.AbsoluteFilename, {});
        }
        DebugAliasesBySource.Remove(File.AbsoluteFilename);
        const FString Original = File.RawCode;
        FString Code = Mask(Original);
        struct FEdit { int32 Begin, End; FString Text; };
        TArray<FEdit> Edits;
        TMap<FString, FString> Names;
        TArray<TSharedPtr<FJsonValue>> Imports;
        FRegexMatcher Import(FRegexPattern(TEXT("\\bimport\\s+blueprint\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*;")), Code);
        while (Import.FindNext())
        {
            const FString Name = Import.GetCaptureGroup(1);
            FString Error;
            UClass* Class = Resolve(File.AbsoluteFilename, Name, Error);
            if (Class && !CanRegisterAlias(Name, Class, Error))
            { P.FileWideError(File, Error); P.bHasError = true; continue; }
            auto View = Class ? Bind(Class, Error) : nullptr;
            if (!View) { P.FileWideError(File, Error); P.bHasError = true; continue; }
            Names.Add(Name, View->Name);
            if (FAngelscriptType::GetByAngelscriptTypeName(Name).Get() != View.Get())
                FAngelscriptType::RegisterAlias(Name, View.ToSharedRef());
            FString Blank = Original.Mid(Import.GetMatchBeginning(), Import.GetMatchEnding() - Import.GetMatchBeginning());
            for (TCHAR& C : Blank) if (C != '\n' && C != '\r') C = ' ';
            Edits.Add({Import.GetMatchBeginning(), Import.GetMatchEnding(), Blank});
            auto Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Name); Row->SetStringField(TEXT("path"), View->Path); Row->SetStringField(TEXT("view"), View->Name);
            Imports.Add(MakeShared<FJsonValueObject>(Row));
        }
        if (!Dependencies.IsEmpty())
        {
            FRegexMatcher ConfiguredParent(FRegexPattern(TEXT("\\bclass\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*:\\s*([A-Za-z_][A-Za-z0-9_]*)")), Code);
            while (ConfiguredParent.FindNext())
            {
                const FString Name = ConfiguredParent.GetCaptureGroup(2);
                if (!Dependencies.Contains(Name) || Names.Contains(Name)) continue;
                FString Error;
                UClass* Class = Resolve(File.AbsoluteFilename, Name, Error);
                if (Class && !CanRegisterAlias(Name, Class, Error))
                { P.FileWideError(File, Error); P.bHasError = true; continue; }
                auto View = Class ? Bind(Class, Error) : nullptr;
                if (!View) { P.FileWideError(File, Error); P.bHasError = true; continue; }
                Names.Add(Name, View->Name);
                if (FAngelscriptType::GetByAngelscriptTypeName(Name).Get() != View.Get())
                    FAngelscriptType::RegisterAlias(Name, View.ToSharedRef());
                auto Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("name"), Name); Row->SetStringField(TEXT("path"), View->Path); Row->SetStringField(TEXT("view"), View->Name);
                Imports.Add(MakeShared<FJsonValueObject>(Row));
            }
        }
        if (Names.IsEmpty()) continue;
        DebugAliasesBySource.Add(File.AbsoluteFilename, Names);
        FRegexMatcher Parent(FRegexPattern(TEXT("\\bclass\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*:\\s*([A-Za-z_][A-Za-z0-9_]*)")), Code);
        while (Parent.FindNext()) if (const FString* ViewName = Names.Find(Parent.GetCaptureGroup(2)))
        {
            for (const auto& Pair : Views) if (Pair.Value->Name == *ViewName)
                ImportedParents.FindOrAdd(File.Module->ModuleName).Add(Parent.GetCaptureGroup(1), Pair.Value->Class.Get());
        }
        FRegexMatcher Cast(FRegexPattern(TEXT("\\bCast\\s*<\\s*([A-Za-z_][A-Za-z0-9_]*)\\s*>\\s*\\(")), Code);
        while (Cast.FindNext()) if (const FString* Name = Names.Find(Cast.GetCaptureGroup(1)))
            Edits.Add({Cast.GetMatchBeginning(), Cast.GetMatchEnding(), TEXT("__SoloASCast_") + *Name + TEXT("(")});
        FRegexMatcher Token(FRegexPattern(TEXT("\\b[A-Za-z_][A-Za-z0-9_]*\\b")), Code);
        while (Token.FindNext())
        {
            if (Edits.ContainsByPredicate([&](const FEdit& E) { return Token.GetMatchBeginning() >= E.Begin && Token.GetMatchEnding() <= E.End; })) continue;
            if (const FString* Name = Names.Find(Token.GetCaptureGroup(0)))
            {
                int32 Next = Token.GetMatchEnding();
                while (Next < Code.Len() && FChar::IsWhitespace(Code[Next])) ++Next;
                Edits.Add({Token.GetMatchBeginning(), Token.GetMatchEnding(), (Next < Code.Len() && Code[Next] == '(' ? TEXT("__SoloASNew_") : TEXT("")) + *Name});
            }
        }
        Edits.Sort([](const FEdit& A, const FEdit& B) { return A.Begin > B.Begin; });
        for (const FEdit& E : Edits) File.RawCode = File.RawCode.Left(E.Begin) + E.Text + File.RawCode.Mid(E.End);
        auto Schema = MakeShared<FJsonObject>(); Schema->SetArrayField(TEXT("imports"), Imports);
        FString Json; FJsonSerializer::Serialize(Schema, TJsonWriterFactory<>::Create(&Json));
        File.RawCode += TEXT("\nUCLASS(Abstract, NotBlueprintable, meta=(SoloASInterfaceSchema=\"") + FBase64::Encode(Json)
            + TEXT("\")) class U__SoloASInterfaceSchema_") + FMD5::HashAnsiString(*File.Module->ModuleName) + TEXT(" : UObject {}\n");
        Changed.Add(File.Module->ModuleName);
    }
    // All files of a changed module share its descriptor arrays.
    for (auto& File : P.Files) if (Changed.Contains(File.Module->ModuleName))
    {
        for (auto& Class : File.Module->Classes) P.PreprocessingClasses.Remove(Class->ClassName);
        File.Module->Classes.Reset(); File.Module->Enums.Reset(); File.Module->Delegates.Reset();
    }
    for (auto& File : P.Files) if (Changed.Contains(File.Module->ModuleName))
    {
        File.ChunkedCode.Empty(); File.GeneratedCode.Reset(); File.Delegates.Reset(); File.StaticsClass.Reset();
        P.ParseIntoChunks(File); P.DetectClasses(File);
    }
    for (auto& File : P.Files) if (Changed.Contains(File.Module->ModuleName))
    { P.AnalyzeClasses(File); P.ProcessMacros(File); P.ProcessDelegates(File); }
}
}

TSharedRef<FJsonObject> ExportBlueprintImportSchema()
{
    auto Schema = MakeShared<FJsonObject>();
    TArray<FString> Paths; Views.GetKeys(Paths); Paths.Sort();
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FString& Path : Paths)
    {
        auto Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("path"), Path);
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Schema->SetArrayField(TEXT("views"), Entries);
    return Schema;
}

static void PrepareConfiguredBlueprintImports()
{
    bool Enabled = true;
    GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableBlueprintInheritance"), Enabled, GEngineIni);
    if (!Enabled) return;
    const auto Dependencies = ProjectBlueprintDependencies();
    TMap<FString, FString> Known;
    TArray<FString> Aliases;
    GConfig->GetArray(TEXT("SoloASCoreExtensions"), TEXT("BlueprintAliases"), Aliases, GEngineIni);
    for (const FString& Alias : Aliases)
    {
        FString Name, Path;
        if (FParse::Value(*Alias, TEXT("Name="), Name) && FParse::Value(*Alias, TEXT("Path="), Path)) Known.Add(Name, Path);
    }
    const auto Bindings = ProjectBlueprintBindings();
    for (const FString& Root : {FPaths::ProjectDir() / TEXT("FeatureMods"), FPaths::ProjectPluginsDir() / TEXT("FeatureMods")})
    {
        TArray<FString> Units;
        IFileManager::Get().FindFiles(Units, *(Root / TEXT("*")), false, true);
        Units.Sort();
        for (const FString& Unit : Units)
        {
            FString Text; TSharedPtr<FJsonObject> Json;
            if (!FFileHelper::LoadFileToString(Text, *(Root / Unit / TEXT("FeatureMod.json")))
                || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json) continue;
            const TSharedPtr<FJsonObject>* FeatureBindings = nullptr;
            if (Json->TryGetObjectField(TEXT("BlueprintBindings"), FeatureBindings))
                for (const auto& Pair : (*FeatureBindings)->Values)
                    if (Pair.Value->Type == EJson::String) Known.Add(Unit + TEXT(":") + Pair.Key, Pair.Value->AsString());
        }
    }
    TArray<FString> Names; Known.GetKeys(Names); Names.Sort();
    for (const FString& Name : Names)
    {
        FString Path = Known.FindChecked(Name);
        if (!Path.Contains(TEXT("."))) Path += TEXT(".") + FPaths::GetCleanFilename(Path);
        if (!Path.EndsWith(TEXT("_C"))) Path += TEXT("_C");
        UClass* Class = LoadObject<UClass>(nullptr, *Path);
        FString Error;
        if (!Class || !Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint) || Class->HasAnyClassFlags(CLASS_Interface)) continue;
        if (!Name.Contains(TEXT(":")) && !CanRegisterAlias(Name, Class, Error))
        { UE_LOG(LogTemp, Warning, TEXT("%s"), *Error); continue; }
        const auto View = Bind(Class, Error);
        if (!View)
        {
            UE_LOG(LogTemp, Warning, TEXT("CE startup Blueprint view rejected: %s"), *Error);
        }
        else if (!Name.Contains(TEXT(":"))) FAngelscriptType::RegisterAlias(Name, View.ToSharedRef());
    }
    for (const FString& Dependency : Dependencies)
    {
        if (FAngelscriptType::GetByAngelscriptTypeName(Dependency).IsValid()) continue;
        const FString* Mapping = Known.Find(Dependency);
        if (!Mapping) Mapping = Bindings.Find(Dependency);
        if (!Mapping) continue;
        FString Path = *Mapping;
        if (!Path.Contains(TEXT("."))) Path += TEXT(".") + FPaths::GetCleanFilename(Path);
        if (!Path.EndsWith(TEXT("_C"))) Path += TEXT("_C");
        FAngelscriptType::RegisterAlias(Dependency, MakeShared<FDeferredBlueprintView>(Dependency, Path));
    }
}
AS_FORCE_LINK const FAngelscriptBinds::FBind PrepareKnownBlueprintImports(int32(FAngelscriptBinds::EOrder::Late) + 310, []
{
    PrepareConfiguredBlueprintImports();
});
AS_FORCE_LINK const FAngelscriptBinds::FBind RestoreBlueprintImportBindings(int32(FAngelscriptBinds::EOrder::Late) + 290, []
{
    bool Enabled = true;
    GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableBlueprintInheritance"), Enabled, GEngineIni);
    if (!Enabled || !FAngelscriptManager::Get().bUsePrecompiledData) return;
    const FString Filename = FAngelscriptManager::GetScriptRootDirectory() / TEXT("CoreExtensions.packaging.json");
    if (!FPaths::FileExists(Filename)) return;
    FString Text;
    TSharedPtr<FJsonObject> Manifest;
    if (!FFileHelper::LoadFileToString(Text, *Filename)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Manifest) || !Manifest)
    { UE_LOG(LogTemp, Fatal, TEXT("CE Blueprint import cache manifest is invalid.")); return; }
    const TSharedPtr<FJsonObject>* Interfaces = nullptr;
    const TSharedPtr<FJsonObject>* Schema = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
    if (!Manifest->TryGetObjectField(TEXT("interfaces"), Interfaces)
        || !(*Interfaces)->TryGetObjectField(TEXT("blueprintImports"), Schema)) return;
    if (!(*Schema)->TryGetArrayField(TEXT("views"), Entries))
    { UE_LOG(LogTemp, Fatal, TEXT("CE Blueprint import cache schema is incomplete.")); return; }
    for (const auto& Entry : *Entries)
    {
        FString Path, Error;
        if (!Entry || Entry->Type != EJson::Object || !Entry->AsObject()->TryGetStringField(TEXT("path"), Path))
        { UE_LOG(LogTemp, Fatal, TEXT("CE Blueprint import cache has an invalid path.")); return; }
        UClass* Class = LoadObject<UClass>(nullptr, *Path);
        if (!Class || !Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint) || !Bind(Class, Error))
        { UE_LOG(LogTemp, Fatal, TEXT("CE Blueprint import cache restore failed: %s %s"), *Path, *Error); return; }
    }
});

#if WITH_EDITOR && !IS_MONOLITHIC
bool SetImportEditorAdapter(FImportEditorAdapter Adapter)
{
    if (ImportEditorAdapter && !ImportEditorAdapter(false)) return false;
    ImportEditorAdapter = Adapter;
    return !Adapter || Adapter(bStarted && !Views.IsEmpty());
}
bool IsImportedBlueprintParent(const UClass* Class)
{
    if (!bStarted || !Class) return false;
    for (const auto& Pair : Views)
        if (Pair.Value->Class.Get() == Class) return true;
    return false;
}
#endif
void StartBlueprintImports()
{
    bool Enabled = true;
    GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableBlueprintInheritance"), Enabled, GEngineIni);
    if (!Enabled || bStarted) return;
    CarrierHandle = AngelscriptCoreExtensionPoints::RegisterBlueprintCarrierProvider(TEXT("SoloASCoreExtensions.BlueprintImports"),
        [](FStringView ModuleName, FStringView ClassName, const UClass* ProposedParent, FString& Diagnostic)
        {
            using AngelscriptCoreExtensionPoints::EBlueprintCarrierResult;
            const auto* Module = ImportedParents.Find(FString(ModuleName));
            const auto* Parent = Module ? Module->Find(FString(ClassName)) : nullptr;
            if (!Parent) return EBlueprintCarrierResult::Unhandled;
            if (Parent->Get() != ProposedParent || !IsValid(ProposedParent)
                || ProposedParent->HasAnyClassFlags(CLASS_NewerVersionExists))
            { Diagnostic = TEXT("Imported Blueprint parent identity changed; reload its script consumers."); return EBlueprintCarrierResult::Reject; }
            for (const auto& Pair : Views)
                if (Pair.Value->Class.Get() == ProposedParent && FindObject<UClass>(nullptr, *Pair.Value->Path) == ProposedParent)
                    return EBlueprintCarrierResult::Allow;
            Diagnostic = TEXT("Blueprint parent is not owned by this import.");
            return EBlueprintCarrierResult::Reject;
        });
    if (!CarrierHandle.IsValid()) return;
    DebugAliasHandle = AngelscriptCoreExtensionPoints::RegisterDebugTypeAliasProvider(TEXT("soloas.blueprint-inheritance"),
        [](TArray<AngelscriptCoreExtensionPoints::FDebugTypeAlias>& Out)
        {
            for (const auto& AliasSource : DebugAliasesBySource)
                for (const auto& Alias : AliasSource.Value)
                    Out.Add({Alias.Value, Alias.Key, AliasSource.Key, {}});
        });
    DebugAliasFailedHandle = FAngelscriptCodeModule::GetReloadHadErrors().AddLambda([]
    {
        for (const auto& Entry : PreviousDebugAliases)
            if (Entry.Value.IsSet()) DebugAliasesBySource.Add(Entry.Key, Entry.Value.GetValue());
            else DebugAliasesBySource.Remove(Entry.Key);
        PreviousDebugAliases.Reset();
    });
    DebugAliasCompiledHandle = FAngelscriptCodeModule::GetPostCompile().AddLambda([] { PreviousDebugAliases.Reset(); });
    // Normal project aliases must exist before the preprocessor's first class
    // analysis. The late bind also prepares them when startup precedes UObject.
    if (FAngelscriptType::GetByAngelscriptTypeName(TEXT("UObject")).IsValid()) PrepareConfiguredBlueprintImports();
    bStarted = true;
#if WITH_EDITOR && !IS_MONOLITHIC
    if (ImportEditorAdapter && !ImportEditorAdapter(!Views.IsEmpty()))
        UE_LOG(LogTemp, Error, TEXT("Cannot protect imported Blueprint structural editing."));
#endif
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
    ReloadClient = AngelscriptCoreExtensionPoints::RegisterExplicitReloadClient(TEXT("SoloASCoreExtensions.BlueprintChildren"));
#if !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    InstallBlueprintCompileCallbacks();
    if (!BlueprintPreCompileHandle.IsValid() && !BlueprintEngineInitHandle.IsValid())
        BlueprintEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&InstallBlueprintCompileCallbacks);
#endif
#endif
    SourceHandle = FAngelscriptPreprocessor::OnProcessChunks.AddStatic(&Rewrite);
    if (!ShutdownHandle.IsValid()) ShutdownHandle = FCoreDelegates::OnPreExit.AddLambda([]
    {
        StopBlueprintImports();
#if WITH_EDITOR
        FCoreUObjectDelegates::OnObjectsReplaced.Remove(ReplacedHandle); ReplacedHandle.Reset();
        FCoreDelegates::OnEndFrame.Remove(RetryHandle); RetryHandle.Reset();
        for (auto* View : PendingViews) View->PendingClass.Reset();
        PendingViews.Reset();
#endif
        FCoreDelegates::OnPreExit.Remove(ShutdownHandle); ShutdownHandle.Reset();
    });
#if WITH_EDITOR
    if (!RetryHandle.IsValid()) RetryHandle = FCoreDelegates::OnEndFrame.AddLambda([]
    {
        if (IsGarbageCollecting() || IsAngelscriptExecutionShutdownStarted()) return;
        const auto Pending = PendingViews.Array();
        for (auto* View : Pending) if (UClass* Class = View->PendingClass.Get())
        {
            FString Error;
            if (RefreshView(*View, Class, Error) == EViewRefreshResult::Rejected)
                UE_LOG(LogTemp, Warning, TEXT("CE Blueprint replacement rejected: %s"), *Error);
        }
#if AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
        ReloadBlueprintConsumers();
#endif
    });
    if (!ReplacedHandle.IsValid()) ReplacedHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddLambda([](const TMap<UObject*, UObject*>& Replacements)
    {
        for (auto& Pair : Views) if (UObject* const* New = Replacements.Find(
            Pair.Value->PendingClass.IsValid() ? Pair.Value->PendingClass.Get() : Pair.Value->Class.Get()))
        {
            if (UClass* Class = Cast<UClass>(*New))
            {
#if AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
                QueueBlueprintConsumers(Pair.Value->Class.Get(), PendingReloadModules);
#endif
                FString Error;
                if (RefreshView(Pair.Value.Get(), Class, Error) == EViewRefreshResult::Rejected)
                    UE_LOG(LogTemp, Warning, TEXT("CE Blueprint replacement rejected: %s"), *Error);
            }
        }
    });
#endif
}
void StopBlueprintImports()
{
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    if (BlueprintCompileWriter) return;
#endif
#if WITH_EDITOR && !IS_MONOLITHIC
    if (ImportEditorAdapter && !ImportEditorAdapter(false)) return;
#endif
    if (DebugAliasHandle.IsValid())
    {
        if (AngelscriptCoreExtensionPoints::UnregisterDebugTypeAliasProvider(DebugAliasHandle)
            == AngelscriptCoreExtensionPoints::ERemoveResult::Busy) return;
        DebugAliasHandle.Reset();
        DebugAliasesBySource.Reset();
        PreviousDebugAliases.Reset();
        FAngelscriptCodeModule::GetReloadHadErrors().Remove(DebugAliasFailedHandle); DebugAliasFailedHandle.Reset();
        FAngelscriptCodeModule::GetPostCompile().Remove(DebugAliasCompiledHandle); DebugAliasCompiledHandle.Reset();
    }
    if (CarrierHandle.IsValid())
    {
        const auto Result = AngelscriptCoreExtensionPoints::UnregisterBlueprintCarrierProvider(CarrierHandle);
        if (Result == AngelscriptCoreExtensionPoints::ERemoveResult::Busy) return;
        CarrierHandle.Reset();
    }
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
    if (ReloadClient.IsValid())
    {
        if (AngelscriptCoreExtensionPoints::UnregisterExplicitReloadClient(ReloadClient)
            == AngelscriptCoreExtensionPoints::ERemoveResult::Busy) return;
        ReloadClient.Reset();
    }
    PendingReloadModules.Reset();
#if !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    FCoreDelegates::OnPostEngineInit.Remove(BlueprintEngineInitHandle); BlueprintEngineInitHandle.Reset();
    if (GEditor)
    {
        GEditor->OnBlueprintPreCompile().Remove(BlueprintPreCompileHandle);
        GEditor->OnBlueprintReinstanced().Remove(BlueprintReinstancedHandle);
        GEditor->OnBlueprintCompiled().Remove(BlueprintCompiledHandle);
    }
    BlueprintPreCompileHandle.Reset(); BlueprintReinstancedHandle.Reset(); BlueprintCompiledHandle.Reset();
#endif
#endif
    bStarted = false;
    ImportedParents.Reset();
    FAngelscriptPreprocessor::OnProcessChunks.Remove(SourceHandle); SourceHandle.Reset();
    // Keep replacement tracking and VM type shapes valid while scripts still refer to them.
}
}
