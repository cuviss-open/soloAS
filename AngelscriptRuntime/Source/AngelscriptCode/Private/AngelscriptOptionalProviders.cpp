                                                                                                    
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptOptionalProviders.h"
#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "EndAngelscriptHeaders.h"

namespace AngelscriptCoreExtensionPoints
{
namespace
{
struct FMetadataRegistration
{
    FDelegateHandle Handle;
    asIScriptEngine* Engine;
    FApiMetadataQuery Query;
};
struct FNativeRegistration
{
    FDelegateHandle Handle;
    asIScriptEngine* Engine;
    FNativeDelegateProvider Provider;
};
TArray<FMetadataRegistration> MetadataProviders;
TArray<FNativeRegistration> NativeProviders;
bool bInProvider = false;

void NormalizeTags(TArray<FString>& Tags)
{
    Tags.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
    for (int32 Index = Tags.Num() - 1; Index > 0; --Index)
        if (Tags[Index].Equals(Tags[Index - 1], ESearchCase::CaseSensitive)) Tags.RemoveAt(Index);
}
const FNativeRegistration* FindNative(asIScriptEngine* Engine)
{
    return NativeProviders.FindByPredicate([&](const FNativeRegistration& Row) { return Row.Engine == Engine; });
}
}

FDelegateHandle RegisterApiMetadataProvider(asIScriptEngine* Engine, FApiMetadataQuery Query)
{
    if (!IsInGameThread() || bInProvider || !Engine || !Query) return {};
    const FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
    MetadataProviders.Add({Handle, Engine, MoveTemp(Query)});
    return Handle;
}

ERemoveResult UnregisterApiMetadataProvider(FDelegateHandle Handle)
{
    if (!IsInGameThread() || bInProvider) return ERemoveResult::Busy;
    const int32 Index = MetadataProviders.IndexOfByPredicate([&](const FMetadataRegistration& Row) { return Row.Handle == Handle; });
    if (Index == INDEX_NONE) return ERemoveResult::Unknown;
    MetadataProviders.RemoveAt(Index);
    return ERemoveResult::Removed;
}

EApiMetadataResult QueryApiMetadata(asIScriptEngine* Engine, FStringView Declaration, FApiMetadata& Out)
{
    Out = {};
    if (!IsInGameThread() || bInProvider) return EApiMetadataResult::Conflict;
    if (!Engine || Declaration.IsEmpty()) return EApiMetadataResult::Unknown;
    TGuardValue<bool> Guard(bInProvider, true);
    bool bClassified = false;
    FApiMetadata Agreed;
    for (const auto& Row : MetadataProviders)
    {
        if (Row.Engine != Engine) continue;
        FApiMetadata Candidate;
        const auto Result = Row.Query(Declaration, Candidate);
        if (Result == EApiMetadataResult::Unknown) continue;
        if (Result != EApiMetadataResult::Classified) return EApiMetadataResult::Conflict;
        NormalizeTags(Candidate.Tags);
        if (bClassified && (!Agreed.Tier.Equals(Candidate.Tier, ESearchCase::CaseSensitive)
            || !Agreed.Module.Equals(Candidate.Module, ESearchCase::CaseSensitive)
            || Agreed.Tags.Num() != Candidate.Tags.Num())) return EApiMetadataResult::Conflict;
        if (bClassified)
            for (int32 Index = 0; Index < Agreed.Tags.Num(); ++Index)
                if (!Agreed.Tags[Index].Equals(Candidate.Tags[Index], ESearchCase::CaseSensitive)) return EApiMetadataResult::Conflict;
        Agreed = MoveTemp(Candidate);
        bClassified = true;
    }
    if (!bClassified) return EApiMetadataResult::Unknown;
    Out = MoveTemp(Agreed);
    return EApiMetadataResult::Classified;
}

FDelegateHandle RegisterNativeDelegateProvider(asIScriptEngine* Engine, FNativeDelegateProvider Provider)
{
    if (!IsInGameThread() || bInProvider || !Engine || FindNative(Engine)
        || !Provider.IsNativeType || !Provider.Bind || !Provider.InvalidateCaptures) return {};
    const FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
    NativeProviders.Add({Handle, Engine, MoveTemp(Provider)});
    return Handle;
}

ERemoveResult UnregisterNativeDelegateProvider(FDelegateHandle Handle)
{
    if (!IsInGameThread() || bInProvider) return ERemoveResult::Busy;
    const int32 Index = NativeProviders.IndexOfByPredicate([&](const FNativeRegistration& Row) { return Row.Handle == Handle; });
    if (Index == INDEX_NONE) return ERemoveResult::Unknown;
    NativeProviders.RemoveAt(Index);
    return ERemoveResult::Removed;
}

bool IsNativeDelegateType(const asITypeInfo* Type)
{
    if (!IsInGameThread() || bInProvider || !Type) return false;
    const auto* Row = FindNative(Type->GetEngine());
    if (!Row) return false;
    TGuardValue<bool> Guard(bInProvider, true);
    return Row->Provider.IsNativeType(Type);
}

ENativeDelegateResult BindNativeDelegate(asIScriptGeneric& Context, uint32 DelegateArgument)
{
    if (!IsInGameThread() || bInProvider) return ENativeDelegateResult::Busy;
    const auto* Row = FindNative(Context.GetEngine());
    if (!Row) return ENativeDelegateResult::Unavailable;
    if (DelegateArgument >= uint32(Context.GetArgCount())) return ENativeDelegateResult::Rejected;
    auto* Type = Context.GetEngine()->GetTypeInfoById(Context.GetArgTypeId(DelegateArgument));
    if (!Type) return ENativeDelegateResult::NotNative;
    TGuardValue<bool> Guard(bInProvider, true);
    if (!Row->Provider.IsNativeType(Type)) return ENativeDelegateResult::NotNative;
    return Row->Provider.Bind(Context) ? ENativeDelegateResult::Completed : ENativeDelegateResult::Rejected;
}

ENativeDelegateResult InvalidateNativeDelegateCaptures(asIScriptEngine* Engine)
{
    if (!IsInGameThread() || bInProvider) return ENativeDelegateResult::Busy;
    const auto* Row = FindNative(Engine);
    if (!Row) return ENativeDelegateResult::Unavailable;
    TGuardValue<bool> Guard(bInProvider, true);
    Row->Provider.InvalidateCaptures();
    return ENativeDelegateResult::Completed;
}
}
                                                                                                  

                                                                                                  
#include "AngelscriptManager.h"
#include "AngelscriptExecutionQuiescence.h"
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "UObject/UnrealType.h"
#include "UObject/Interface.h"
#include "UObject/GarbageCollection.h"

namespace AngelscriptCoreExtensionPoints
{
namespace
{
struct FClassInterfaceRegistration
{
    FDelegateHandle Handle;
    FName Name;
    FClassInterfaceQuery Query;
};
TArray<FClassInterfaceRegistration> ClassInterfaceProviders;
bool bQueryingInterfaces = false;
bool InterfaceSafePoint()
{
    return IsInGameThread() && !bInProvider && !bQueryingInterfaces && !asGetActiveContext()
        && !IsGarbageCollecting()
        && (!FAngelscriptManager::IsInitialized() || !FAngelscriptManager::Get().bIsCompilingModules);
}
}
FDelegateHandle RegisterClassInterfaceProvider(FName Name, FClassInterfaceQuery Query)
{
    if (!InterfaceSafePoint() || IsAngelscriptExecutionShutdownStarted() || Name.IsNone() || !Query
        || ClassInterfaceProviders.ContainsByPredicate([&](const auto& Row) { return Row.Name == Name; })) return {};
    FAngelscriptExecutionWriteScope Boundary(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Boundary.IsAcquired()) return {};
    FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
    ClassInterfaceProviders.Add({Handle, Name, MoveTemp(Query)});
    return Handle;
}
ERemoveResult UnregisterClassInterfaceProvider(FDelegateHandle Handle)
{
    if (!InterfaceSafePoint()) return ERemoveResult::Busy;
    FAngelscriptExecutionWriteScope Boundary(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Boundary.IsAcquired()) return ERemoveResult::Busy;
    const int32 Index = ClassInterfaceProviders.IndexOfByPredicate([&](const auto& Row) { return Row.Handle == Handle; });
    if (Index == INDEX_NONE) return ERemoveResult::Unknown;
    ClassInterfaceProviders.RemoveAt(Index);
    return ERemoveResult::Removed;
}
namespace Private
{
void ShutdownClassInterfaceProviders()
{
    check(IsInGameThread() && IsAngelscriptExecutionWriterHeldByCurrentThread() && !bQueryingInterfaces);
    ClassInterfaceProviders.Reset();
}
bool HasClassInterfaceProviders() { return !ClassInterfaceProviders.IsEmpty(); }
bool QueryClassInterfaces(FStringView ModuleName, const FAngelscriptClassDesc& Class,
    TArray<UClass*>& Out, FString& Error)
{
    Out.Reset();
    if (!IsInGameThread() || bQueryingInterfaces) { Error = TEXT("Class interface query is busy"); return false; }
    TGuardValue<bool> Guard(bQueryingInterfaces, true);
    TMap<FName, UFunction*> Functions;
    TMap<FName, UClass*> Identities;
    for (const auto& Row : ClassInterfaceProviders)
    {
        TArray<UClass*> Candidate;
        if (!Row.Query(ModuleName, Class.ClassName, Candidate, Error)) return false;
        for (UClass* Interface : Candidate)
        {
            if (!Interface || !Interface->HasAnyClassFlags(CLASS_Interface) || Interface == UInterface::StaticClass())
            { Error = TEXT("Class interface provider returned a non-interface class"); return false; }
            if (auto* Previous = Identities.Find(Interface->GetFName()); Previous && *Previous != Interface)
            { Error = TEXT("Class interface name conflict"); return false; }
            Identities.Add(Interface->GetFName(), Interface);
            Out.AddUnique(Interface);
            for (TFieldIterator<UFunction> It(Interface, EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                UFunction* Function = *It;
                if (auto* Previous = Functions.Find(Function->GetFName()); Previous && !Function->IsSignatureCompatibleWith(*Previous))
                { Error = TEXT("Class interface function signature conflict"); return false; }
                Functions.Add(Function->GetFName(), Function);
            }
        }
    }
    // BlueprintEvent conversion needs no override lookup. Validate against the
    // declared interface here so conflicting names never silently claim it.
    for (const auto& Method : Class.Methods)
    {
        const auto& Desc = Method;
        auto* Found = Functions.Find(FName(*Desc->FunctionName));
        if (!Found) continue;
        UFunction* Function = *Found;
        FProperty* Return = Function->GetReturnProperty();
        bool bMatches = Desc->bBlueprintEvent && (Desc->ReturnType.IsValid() == (Return != nullptr));
        if (Return && Desc->ReturnType.IsValid())
            bMatches &= Desc->ReturnType.MatchesProperty(Return, FAngelscriptType::EPropertyMatchType::OverrideReturnValue);
        int32 Index = 0;
        for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
        {
            if (It->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
            bMatches &= Desc->Arguments.IsValidIndex(Index)
                && Desc->Arguments[Index].Type.MatchesProperty(*It, FAngelscriptType::EPropertyMatchType::OverrideArgument);
            ++Index;
        }
        bMatches &= Index == Desc->Arguments.Num();
        if (!bMatches) { Error = TEXT("Class interface implementation signature conflict"); return false; }
    }
    return true;
}
}
}
                                                                                                

                                                                                                   
#include "AngelscriptType.h"
#include "UObject/ObjectKey.h"
#include "Misc/ScopeRWLock.h"
#include <atomic>

namespace AngelscriptCoreExtensionPoints
{
namespace
{
struct FBlueprintCarrierRegistration
{
    FDelegateHandle Handle;
    FName Name;
    FBlueprintCarrierQuery Query;
};
TArray<FBlueprintCarrierRegistration> BlueprintCarrierProviders;
struct FBlueprintCarrierNameFact
{
    FDelegateHandle Owner;
    TWeakObjectPtr<const UClass> Parent;
    int32 PropertiesSize;
    FString Name;
};
TMap<FObjectKey, FBlueprintCarrierNameFact> BlueprintCarrierNames;
FRWLock BlueprintCarrierNamesLock;
std::atomic<bool> bHasBlueprintCarrierNames{false};
bool bQueryingBlueprintCarrier = false;
const TMap<FString, TSharedRef<FAngelscriptClassDesc>>* BlueprintCarrierCompilingClasses = nullptr;
const TMap<FString, TSharedRef<FAngelscriptModuleDesc>>* BlueprintCarrierCompilingModules = nullptr;
TSharedPtr<FAngelscriptClassDesc> FindBlueprintCarrierClass(const FString& Name,
    TSharedPtr<FAngelscriptModuleDesc>& Module, bool& bPending)
{
    bPending = false;
    if (BlueprintCarrierCompilingClasses)
        if (const auto* Found = BlueprintCarrierCompilingClasses->Find(Name))
        {
            bPending = true;
            for (const auto& Entry : *BlueprintCarrierCompilingModules)
                if (Entry.Value->Classes.Contains(*Found)) { Module = Entry.Value; return *Found; }
            return nullptr;
        }
    return FAngelscriptManager::Get().GetClass(Name, &Module);
}
bool BlueprintCarrierSafePoint()
{
    return InterfaceSafePoint() && !bInProvider && !bQueryingBlueprintCarrier;
}
}
FDelegateHandle RegisterBlueprintCarrierProvider(FName Name, FBlueprintCarrierQuery Query)
{
    if (!BlueprintCarrierSafePoint() || IsAngelscriptExecutionShutdownStarted() || Name.IsNone() || !Query
        || BlueprintCarrierProviders.ContainsByPredicate([&](const auto& Row) { return Row.Name == Name; })) return {};
    FAngelscriptExecutionWriteScope Boundary(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Boundary.IsAcquired()) return {};
    FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
    BlueprintCarrierProviders.Add({Handle, Name, MoveTemp(Query)});
    return Handle;
}
ERemoveResult UnregisterBlueprintCarrierProvider(FDelegateHandle Handle)
{
    if (!BlueprintCarrierSafePoint()) return ERemoveResult::Busy;
    FAngelscriptExecutionWriteScope Boundary(FAngelscriptExecutionWriteScope::EAcquire::Try);
    if (!Boundary.IsAcquired()) return ERemoveResult::Busy;
    const int32 Index = BlueprintCarrierProviders.IndexOfByPredicate([&](const auto& Row) { return Row.Handle == Handle; });
    if (Index == INDEX_NONE) return ERemoveResult::Unknown;
    {
        FWriteScopeLock NamesGuard(BlueprintCarrierNamesLock);
        for (auto It = BlueprintCarrierNames.CreateIterator(); It; ++It)
            if (It.Value().Owner == Handle || !It.Key().ResolveObjectPtr()) It.RemoveCurrent();
        bHasBlueprintCarrierNames.store(!BlueprintCarrierNames.IsEmpty(), std::memory_order_release);
    }
    BlueprintCarrierProviders.RemoveAt(Index);
    return ERemoveResult::Removed;
}
namespace Private
{
FBlueprintCarrierCompilationScope::FBlueprintCarrierCompilationScope(
    const TMap<FString, TSharedRef<FAngelscriptClassDesc>>& Classes,
    const TMap<FString, TSharedRef<FAngelscriptModuleDesc>>& Modules)
    : PreviousClasses(BlueprintCarrierCompilingClasses), PreviousModules(BlueprintCarrierCompilingModules)
{
    BlueprintCarrierCompilingClasses = &Classes;
    BlueprintCarrierCompilingModules = &Modules;
}
FBlueprintCarrierCompilationScope::~FBlueprintCarrierCompilationScope()
{
    BlueprintCarrierCompilingClasses = PreviousClasses;
    BlueprintCarrierCompilingModules = PreviousModules;
}
EBlueprintCarrierResult QueryBlueprintCarrier(FStringView ModuleName, FStringView ClassName,
    const UClass* ProposedParent, FString& Diagnostic)
{
    Diagnostic.Reset();
    if (!IsInGameThread() || bInProvider || bQueryingInterfaces || bQueryingBlueprintCarrier
        || asGetActiveContext() || IsGarbageCollecting() || !ProposedParent)
    { Diagnostic = TEXT("Blueprint carrier query is busy or invalid."); return EBlueprintCarrierResult::Reject; }
    FString OwnerModule(ModuleName), OwnerClass(ClassName);
    TSet<FString> VisitedClasses;
    for (;;)
    {
        if (VisitedClasses.Contains(OwnerClass))
        { Diagnostic = TEXT("The script carrier parent chain contains a cycle."); return EBlueprintCarrierResult::Reject; }
        VisitedClasses.Add(OwnerClass);
        TSharedPtr<FAngelscriptModuleDesc> Module;
        bool bPending = false;
        const auto Desc = FindBlueprintCarrierClass(OwnerClass, Module, bPending);
        if (!Desc) break;
        if (!Module || Module->ModuleName != OwnerModule || Module->bCompileError || Desc->CodeSuperClass != ProposedParent)
        { Diagnostic = TEXT("Script carrier parent identity or compilation changed."); return EBlueprintCarrierResult::Reject; }
        if (!bPending)
        {
            const auto* Published = Cast<UASClass>(Desc->Class);
            if (!Published || !Published->HasValidScriptClassCarrier())
            { Diagnostic = TEXT("Script carrier parent has no valid published ancestry facts."); return EBlueprintCarrierResult::Reject; }
        }
        if (Desc->bSuperIsCodeClass) break;
        TSharedPtr<FAngelscriptModuleDesc> ParentModule;
        bool bParentPending = false;
        const auto Parent = FindBlueprintCarrierClass(Desc->SuperClass, ParentModule, bParentPending);
        if (!Parent || !ParentModule || Parent->CodeSuperClass != ProposedParent)
        { Diagnostic = TEXT("Direct script carrier parent is missing or has different ancestry."); return EBlueprintCarrierResult::Reject; }
        OwnerClass = Parent->ClassName;
        OwnerModule = ParentModule->ModuleName;
    }
    TGuardValue<bool> Guard(bQueryingBlueprintCarrier, true);
    TGuardValue<bool> ProviderGuard(bInProvider, true);
    bool bAllowed = false;
    FDelegateHandle AllowedOwner;
    for (const auto& Row : BlueprintCarrierProviders)
    {
        FString CandidateDiagnostic;
        const auto Result = Row.Query(OwnerModule, OwnerClass, ProposedParent, CandidateDiagnostic);
        if (Result == EBlueprintCarrierResult::Unhandled) continue;
        if (Result != EBlueprintCarrierResult::Allow || bAllowed)
        {
            Diagnostic = CandidateDiagnostic.IsEmpty() ? TEXT("Blueprint carrier owner rejected or conflicts.") : MoveTemp(CandidateDiagnostic);
            return EBlueprintCarrierResult::Reject;
        }
        bAllowed = true;
        AllowedOwner = Row.Handle;
        Diagnostic = MoveTemp(CandidateDiagnostic);
    }
    if (bAllowed)
    {
        const auto Type = FAngelscriptType::GetByClass(const_cast<UClass*>(ProposedParent));
        asITypeInfo* Info = Type ? Type->GetAngelscriptTypeInfo(FAngelscriptTypeUsage::DefaultUsage) : nullptr;
        const bool bExactView = Type && Type->GetClass(FAngelscriptTypeUsage::DefaultUsage) == ProposedParent
            && Info && Info->GetUserData() == ProposedParent
            && Info->GetEngine() == FAngelscriptManager::Get().GetScriptEngine()
            && Info->GetSize() == static_cast<asUINT>(ProposedParent->GetStructureSize());
        FWriteScopeLock NamesGuard(BlueprintCarrierNamesLock);
        for (auto It = BlueprintCarrierNames.CreateIterator(); It; ++It)
            if (!It.Key().ResolveObjectPtr()) It.RemoveCurrent();
        BlueprintCarrierNames.Remove(FObjectKey(ProposedParent));
        if (bExactView)
            BlueprintCarrierNames.Add(FObjectKey(ProposedParent), FBlueprintCarrierNameFact{
                AllowedOwner, ProposedParent->GetSuperClass(), ProposedParent->GetPropertiesSize(), ANSI_TO_TCHAR(Info->GetName())});
        bHasBlueprintCarrierNames.store(!BlueprintCarrierNames.IsEmpty(), std::memory_order_release);
    }
    return bAllowed ? EBlueprintCarrierResult::Allow : EBlueprintCarrierResult::Unhandled;
}

bool TryGetBlueprintCarrierName(const UClass* Class, FString& OutName)
{
    if (!Class || !bHasBlueprintCarrierNames.load(std::memory_order_acquire)) return false;
    FReadScopeLock NamesGuard(BlueprintCarrierNamesLock);
    const auto* Fact = BlueprintCarrierNames.Find(FObjectKey(Class));
    if (!Fact || Class->HasAnyClassFlags(CLASS_NewerVersionExists)
        || Fact->Parent.Get() != Class->GetSuperClass() || Fact->PropertiesSize != Class->GetPropertiesSize()) return false;
    OutName = Fact->Name;
    return true;
}

bool ValidateBlueprintCarrierForCompilation(FStringView ModuleName, FStringView ClassName,
    const UClass* ProposedParent, FString& Diagnostic)
{
    Diagnostic.Reset();
    if (ProposedParent->HasAnyClassFlags(CLASS_Native)) return true;
    const UClass* Blueprint = nullptr;
    if (UASClass::HasValidScriptClassCarrierAncestry(ProposedParent, &Blueprint)) return true;
    if (!Blueprint) { Diagnostic = TEXT("The script class ancestry contains a cycle."); return false; }
    if (QueryBlueprintCarrier(ModuleName, ClassName, ProposedParent, Diagnostic) != EBlueprintCarrierResult::Allow)
    {
        if (Diagnostic.IsEmpty()) Diagnostic = TEXT("Blueprint ancestry has no owning carrier provider.");
        return false;
    }
    FString Name;
    if (!TryGetBlueprintCarrierName(ProposedParent, Name))
    { Diagnostic = TEXT("Blueprint carrier requires an exact registered native view with matching layout."); return false; }
    return true;
}
}
}
                                                                                                 
