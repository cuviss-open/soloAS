// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "InstancedStructReload.h"

#include "AngelscriptManager.h"
#include "AngelscriptInclude.h"
#include "StartAngelscriptHeaders.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"
#include "AngelscriptReloadSafety.h"
#include "AngelscriptType.h"
#include "ClassGenerator/ASStruct.h"
#include "ClassGenerator/ASClass.h"
#include "ClassGenerator/ASEnum.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/UObjectArray.h"
#include "UObject/GCObject.h"
#include <atomic>

namespace
{
bool HasSchemaReferences(const UStruct* Struct)
{
    if (!Struct) return false;
    if (const UScriptStruct* Script = Cast<UScriptStruct>(Struct))
        if (const UScriptStruct::ICppStructOps* Ops = Script->GetCppStructOps())
            if (Ops->HasAddStructReferencedObjects()
                || Ops->HasSerializerObjectReferences(EPropertyObjectReferenceType_Any)) return true;
    TArray<const FStructProperty*> Encountered;
    for (TFieldIterator<FProperty> It(Struct); It; ++It)
        if (It->ContainsObjectReference(Encountered, EPropertyObjectReferenceType_Any)) return true;
    return false;
}

bool IsStrongObjectProperty(const FProperty* Property)
{
    return CastField<FObjectProperty>(Property) && !CastField<FClassProperty>(Property);
}


bool IsQuietNativeEnumShape(const FProperty* Property)
{
#if UE_WITH_REMOTE_OBJECT_HANDLE || !UE_WITH_OBJECT_HANDLE_LATE_RESOLVE || !UE_WITH_OBJECT_HANDLE_TRACKING \
    || !UE_WITH_OBJECT_HANDLE_TYPE_SAFETY || UE_WITH_CONSTINIT_UOBJECT
    return false;
#else
    if (Property->GetClass() != FEnumProperty::StaticClass() || Property->ArrayDim != 1
        || Property->HasGetter() || Property->HasSetter()) return false;
    const FNumericProperty* Inner = CastFieldChecked<FEnumProperty>(Property)->GetUnderlyingProperty();
    // Bind_UEnum's native scoped-enum factory uses an unassociated uint8 inner.
    if (!Inner || Inner->GetClass() != FByteProperty::StaticClass() || Inner->ArrayDim != 1
        || Inner->GetElementSize() != 1 || Property->GetElementSize() != 1
        || Inner->GetOwnerVariant() != FFieldVariant(const_cast<FProperty*>(Property))
        || Inner->HasGetter() || Inner->HasSetter()) return false;
    const auto& ByteEnum = CastFieldChecked<FByteProperty>(Inner)->Enum;
    return ByteEnum.IsResolved() && !ByteEnum;
#endif
}


bool IsQuietScriptEnumShape(const FProperty* Property)
{
#if UE_WITH_REMOTE_OBJECT_HANDLE || !UE_WITH_OBJECT_HANDLE_LATE_RESOLVE || !UE_WITH_OBJECT_HANDLE_TRACKING \
    || !UE_WITH_OBJECT_HANDLE_TYPE_SAFETY || UE_WITH_CONSTINIT_UOBJECT
    return false;
#else
    if (Property->GetClass() != FEnumProperty::StaticClass() || Property->ArrayDim != 1
        || Property->GetElementSize() != 1 || Property->HasGetter() || Property->HasSetter()) return false;
    const FNumericProperty* Inner = CastFieldChecked<FEnumProperty>(Property)->GetUnderlyingProperty();
    return Inner && Inner->GetClass() == FInt8Property::StaticClass() && Inner->ArrayDim == 1
        && Inner->GetElementSize() == 1 && !Inner->HasGetter() && !Inner->HasSetter()
        && Inner->GetOwnerVariant() == FFieldVariant(const_cast<FProperty*>(Property));
#endif
}

asITypeInfo* DeclaredScriptEnumCookie(const FAngelscriptClassDesc& Desc, const FProperty* Field, bool bActualOffset)
{
    asITypeInfo* Result = nullptr;
    if (!Desc.ScriptType) return nullptr;
    for (const auto& Property : Desc.Properties)
    {
        const FString& UnrealName = Property->UnrealPropertyNameOverride.IsSet()
            ? Property->UnrealPropertyNameOverride.GetValue() : Property->PropertyName;
        if (FName(*UnrealName) != Field->GetFName()) continue;
        const char* Name = nullptr;
        int32 Id = 0;
        int32 Offset = 0;
        if (Result || Property->ScriptPropertyIndex < 0
            || Desc.ScriptType->GetProperty(Property->ScriptPropertyIndex, &Name, &Id, nullptr, nullptr, &Offset) < 0
            || !Name || Property->PropertyName != ANSI_TO_TCHAR(Name) || Offset < 0
            || Property->ScriptPropertyOffset != static_cast<SIZE_T>(Offset)
            || (bActualOffset && Field->GetOffset_ForInternal() != Offset)
            || !Property->PropertyType.EqualsUnqualified(FAngelscriptTypeUsage::FromTypeId(Id))) return nullptr;
        Result = Desc.ScriptType->GetEngine()->GetTypeInfoById(Id);
        if (!Result || (Result->GetFlags() & asOBJ_ENUM) == 0 || !Result->GetModule()
            || (Result->GetFlags() & asOBJ_SHARED) != 0) return nullptr;
    }
    return Result;
}

const AngelscriptReloadSafety::FPreparedScriptEnumReloadPlan* FindScriptEnumInput(asITypeInfo* Next)
{
    const auto* Plans = AngelscriptReloadSafety::GetPreparedScriptEnumReloadPlans();
    const AngelscriptReloadSafety::FPreparedScriptEnumReloadPlan* Result = nullptr;
    if (Plans)
        for (const auto& Plan : *Plans)
            if (Plan.CandidateCookie == Next)
            {
                if (Result) return nullptr;
                Result = &Plan;
            }
    return Result && Result->bEligibleFixedDefinition ? Result : nullptr;
}

asITypeInfo* DeclaredNativeEnumCookie(const FAngelscriptClassDesc& Desc, const FProperty* Field, bool bActualOffset)
{
    asITypeInfo* Result = nullptr;
    if (!Desc.ScriptType) return nullptr;
    for (const auto& Property : Desc.Properties)
    {
        const FString& UnrealName = Property->UnrealPropertyNameOverride.IsSet()
            ? Property->UnrealPropertyNameOverride.GetValue() : Property->PropertyName;
        if (FName(*UnrealName) != Field->GetFName()) continue;
        const char* Name = nullptr;
        int32 Id = 0;
        int32 Offset = 0;
        if (Result || Property->ScriptPropertyIndex < 0
            || Desc.ScriptType->GetProperty(Property->ScriptPropertyIndex, &Name, &Id, nullptr, nullptr, &Offset) < 0
            || !Name || Property->PropertyName != ANSI_TO_TCHAR(Name) || Offset < 0
            || Property->ScriptPropertyOffset != static_cast<SIZE_T>(Offset)
            || (bActualOffset && Field->GetOffset_ForInternal() != Offset)
            || !Property->PropertyType.EqualsUnqualified(FAngelscriptTypeUsage::FromTypeId(Id))) return nullptr;
        Result = Desc.ScriptType->GetEngine()->GetTypeInfoById(Id);
        if (!Result || (Result->GetFlags() & asOBJ_ENUM) == 0 || Result->GetModule() != nullptr) return nullptr;
    }
    return Result;
}

TSharedPtr<FAngelscriptClassDesc> ActiveStructDescriptor(const UASStruct* Struct)
{
    if (!Struct || !Struct->ScriptType) return nullptr;
    const auto Module = FAngelscriptManager::Get().GetModule(Struct->ScriptType->GetModule());
    if (!Module.IsValid()) return nullptr;
    for (const auto& Desc : Module->Classes)
        if (Desc->Struct == Struct && Desc->ScriptType == Struct->ScriptType) return Desc;
    return nullptr;
}

struct FNativeEnumSymbol
{
    TArray<ANSICHAR> Name;
    int32 Value = 0;
};


struct FScriptEnumReloadBinding
{
    AngelscriptReloadSafety::FPreparedScriptEnumReloadPlan Plan;
    asIScriptEngine* Engine = nullptr;
    asIScriptModule* PreviousOwner = nullptr;
    asIScriptModule* CandidateOwner = nullptr;
    FString ModuleName;
    TArray<ANSICHAR> Namespace;
    TArray<FNativeEnumSymbol> Symbols;
    TArray<FName> ReflectedNames;
    FString Documentation;
    TMap<TPair<FName, int32>, FString> Metadata;
    TArray<TPair<FName, int64>> Definition;
    mutable TObjectPtr<UEnum> Root;
    const UEnum* Identity = nullptr;
    FObjectKey Key;
    FObjectHandle Handle{};
    int32 Index = INDEX_NONE;
    int32 Serial = 0;
    FName Name;
    const UObject* Outer = nullptr;
    void* InitialCandidateUserData = nullptr;
    uint64 Epoch = 0;
    uint64 ClaimRevision = 0;
    uint64 CommitRevision = 0;
    bool bPreviousPinned = false;
    bool bCandidatePinned = false;

    ~FScriptEnumReloadBinding()
    {
        if (bPreviousPinned) Plan.PreviousCookie->Release();
        if (bCandidatePinned) Plan.CandidateCookie->Release();
    }

    void ReleasePreviousPin()
    {
        if (!bPreviousPinned) return;
        bPreviousPinned = false;
        Plan.PreviousCookie->Release();
    }

    bool HasModule(asIScriptModule* Module) const
    {
        for (asUINT I = 0; I < Engine->GetModuleCount(); ++I)
            if (Engine->GetModuleByIndex(I) == Module)
            {
                const char* ActualName = Module->GetName();
                return ActualName && Engine->GetModule(ActualName, asGM_ONLY_IF_EXISTS) == Module;
            }
        return false;
    }

    bool HasCookie(asIScriptModule* Module, const asITypeInfo* Cookie) const
    {
        if (!Module || !Cookie || !HasModule(Module)) return false;
        for (asUINT I = 0; I < Module->GetEnumCount(); ++I)
            if (Module->GetEnumByIndex(I) == Cookie) return true;
        return false;
    }

    bool BindingMatches(const TSharedPtr<FAngelscriptModuleDesc>& Module,
        const TSharedPtr<FAngelscriptEnumDesc>& Enum, asIScriptModule* Owner, asITypeInfo* Cookie, void* UserData) const
    {
        if (!Module.IsValid() || !Enum.IsValid() || Module->ModuleName != ModuleName || Module->ScriptModule != Owner
            || !HasCookie(Owner, Cookie) || Enum->ScriptType != Cookie || Enum->Enum != Identity
            || Enum->bPendingFullReload || Cookie->GetEngine() != Engine || Cookie->GetModule() != Owner
            || Cookie->GetSize() != 1 || Enum->Documentation != Documentation
            || !Enum->Meta.OrderIndependentCompareEqual(Metadata)
            || (Cookie->GetFlags() & asOBJ_ENUM) == 0 || (Cookie->GetFlags() & asOBJ_SHARED) != 0
            || Cookie->GetUserData() != UserData
            || Cookie->GetEnumValueCount() != static_cast<asUINT>(Symbols.Num())) return false;
        const char* Space = Cookie->GetNamespace();
        if (FCStringAnsi::Strcmp(Space ? Space : "", Namespace.GetData()) != 0) return false;
        int32 Owners = 0;
        for (const auto& Desc : Module->Enums)
            if (Desc->ScriptType == Cookie)
            {
                if (Desc != Enum) return false;
                ++Owners;
            }
        if (Owners != 1 || Enum->ValueNames.Num() != Symbols.Num() || Enum->EnumValues.Num() != Symbols.Num()) return false;
        for (int32 I = 0; I < Symbols.Num(); ++I)
        {
            int32 Value = 0;
            const char* Symbol = Cookie->GetEnumValueByIndex(I, &Value);
            if (!Symbol || Value != Symbols[I].Value || FCStringAnsi::Strcmp(Symbol, Symbols[I].Name.GetData()) != 0
                || Enum->ValueNames[I] != ReflectedNames[I] || Enum->EnumValues[I] != Value) return false;
        }
        return true;
    }

    bool PhysicalMatches() const
    {
        if (!Root.IsResolved() || Root.GetHandle() != Handle || Index < 0 || Serial <= 0) return false;
        const FUObjectItem* Slot = GUObjectArray.IndexToObject(Index);
        if (!Slot || Slot->GetSerialNumber() != Serial || Slot->GetObject() != Identity
            || Slot->HasAnyFlags(EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable)) return false;
        if (Identity->GetClass() != UASEnum::StaticClass() || Identity->GetFName() != Name || Identity->GetOuter() != Outer
            || Identity->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
            || Identity->HasAnyEnumFlags(EEnumFlags::Flags | EEnumFlags::NewerVersionExists)
            || Identity->GetCppForm() != UEnum::ECppForm::EnumClass
            || Identity->GetUnderlyingType() != UEnum::EUnderlyingType::int8 || Identity->NumEnums() != Definition.Num()) return false;
        for (int32 I = 0; I < Definition.Num(); ++I)
            if (Identity->GetNameByIndex(I) != Definition[I].Key || Identity->GetValueByIndex(I) != Definition[I].Value) return false;
        return true;
    }

    bool Matches(bool bCommitCheck = false) const
    {
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope || Scope->Epoch != Epoch || FAngelscriptManager::Get().Engine != Engine || !PhysicalMatches()) return false;
        if (ClaimRevision && ClaimRevision != CommitRevision && !bCommitCheck) return false;
        const auto Active = FAngelscriptManager::Get().GetModule(ModuleName);
        if (Plan.bOwnerRecompiled && Scope->bCommitted)
        {
            if (Active != Plan.CandidateModule) return false;
            return BindingMatches(Plan.CandidateModule, Plan.Candidate, CandidateOwner, Plan.CandidateCookie,
                CommitRevision || bCommitCheck ? const_cast<UEnum*>(Identity) : InitialCandidateUserData);
        }
        if (Active != Plan.PreviousModule || ClaimRevision || CommitRevision) return false;
        return BindingMatches(Plan.PreviousModule, Plan.Previous, PreviousOwner, Plan.PreviousCookie,
            const_cast<UEnum*>(Identity))
            && (!Plan.bOwnerRecompiled || BindingMatches(Plan.CandidateModule, Plan.Candidate, CandidateOwner,
                Plan.CandidateCookie, InitialCandidateUserData));
    }

    bool Complete() const
    {
        return Matches() && (Plan.bOwnerRecompiled ? ClaimRevision == 1 && CommitRevision == 1
            : ClaimRevision == 0 && CommitRevision == 0);
    }

    bool Capture(const AngelscriptReloadSafety::FPreparedScriptEnumReloadPlan& Input)
    {
        Plan = Input;
        Engine = FAngelscriptManager::Get().Engine;
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope || !Plan.bEligibleFixedDefinition || !Plan.PreviousModule.IsValid() || !Plan.CandidateModule.IsValid()
            || !Plan.Previous.IsValid() || !Plan.Candidate.IsValid()) return false;
        PreviousOwner = Plan.PreviousModule->ScriptModule;
        CandidateOwner = Plan.CandidateModule->ScriptModule;
        ModuleName = Plan.PreviousModule->ModuleName;
        if (ModuleName != Plan.CandidateModule->ModuleName || !HasCookie(PreviousOwner, Plan.PreviousCookie)
            || !HasCookie(CandidateOwner, Plan.CandidateCookie)
            || (Plan.bOwnerRecompiled && (PreviousOwner == CandidateOwner || Plan.PreviousCookie == Plan.CandidateCookie))
            || (!Plan.bOwnerRecompiled && (Plan.PreviousModule != Plan.CandidateModule
                || Plan.Previous != Plan.Candidate || Plan.PreviousCookie != Plan.CandidateCookie))) return false;
        Plan.PreviousCookie->AddRef();
        Plan.CandidateCookie->AddRef();
        bPreviousPinned = bCandidatePinned = true;
        Epoch = Scope->Epoch;
        for (TObjectIterator<UASEnum> It; It; ++It)
            if (*It == Plan.Previous->Enum && It->GetClass() == UASEnum::StaticClass())
            { Identity = *It; Root = *It; break; }
        if (!Identity || Plan.Candidate->Enum != Identity) return false;
        Index = GUObjectArray.ObjectToIndex(Identity);
        Serial = GUObjectArray.AllocateSerialNumber(Index);
        Key = FObjectKey(const_cast<UEnum*>(Identity));
        Handle = Root.GetHandle();
        Name = Identity->GetFName();
        Outer = Identity->GetOuter();
        InitialCandidateUserData = Plan.CandidateCookie->GetUserData();
        if (InitialCandidateUserData && InitialCandidateUserData != Identity) return false;
        ReflectedNames = Plan.Previous->ValueNames;
        Documentation = Plan.Previous->Documentation;
        Metadata = Plan.Previous->Meta;
        const char* Space = Plan.PreviousCookie->GetNamespace();
        if (!Space) Space = "";
        Namespace.Append(Space, FCStringAnsi::Strlen(Space) + 1);
        for (int32 I = 0; I < Identity->NumEnums(); ++I)
            Definition.Emplace(Identity->GetNameByIndex(I), Identity->GetValueByIndex(I));
        for (asUINT I = 0; I < Plan.PreviousCookie->GetEnumValueCount(); ++I)
        {
            auto& Symbol = Symbols.AddDefaulted_GetRef();
            const char* Text = Plan.PreviousCookie->GetEnumValueByIndex(I, &Symbol.Value);
            if (!Text || Symbol.Value < MIN_int8 || Symbol.Value > MAX_int8) return false;
            Symbol.Name.Append(Text, FCStringAnsi::Strlen(Text) + 1);
            int32 Count = 0;
            for (const auto& Row : Definition)
            {
                FString Short = Row.Key.ToString();
                int32 Colon = INDEX_NONE;
                if (Short.FindLastChar(TEXT(':'), Colon)) Short.RightChopInline(Colon + 1);
                if (Short == ANSI_TO_TCHAR(Text) && Row.Value == Symbol.Value) ++Count;
            }
            if (Count != 1) return false;
        }
        return Matches();
    }
};

struct FNativeEnumTypeWitness
{
    TSharedPtr<FScriptEnumReloadBinding> ScriptBinding;
    mutable TObjectPtr<UEnum> Root;
    const UEnum* Identity = nullptr;
    FObjectHandle Handle{};
    int32 Index = INDEX_NONE;
    int32 Serial = 0;
    uint64 Epoch = 0;
    asIScriptEngine* Engine = nullptr;
    asITypeInfo* Cookie = nullptr;
    TArray<FNativeEnumSymbol> Symbols;
    TArray<TPair<FName, int64>> Definition;
    FName EnumName;
    const UObject* EnumOuter = nullptr;
    bool bFlags = false;
    UEnum::EUnderlyingType UnderlyingKind = UEnum::EUnderlyingType::uint8;
    const FNumericProperty* Inner = nullptr;
    FFieldVariant InnerOwner;
    const FFieldClass* InnerClass = nullptr;
    FName InnerName;
    EPropertyFlags InnerFlags = CPF_None;
    int32 InnerSize = 0;
    int32 InnerArrayDim = 0;
    int32 InnerOffset = 0;
    FObjectHandle InnerEnumHandle{};
    TUniquePtr<FEnumProperty> ShadowStorage;
    const FEnumProperty* Shadow = nullptr;

    ~FNativeEnumTypeWitness() { if (Cookie) Cookie->Release(); }

    static bool Registered(asIScriptEngine* InEngine, const asITypeInfo* InCookie)
    {
        if (!InEngine || !InCookie) return false;
        for (asUINT I = 0; I < InEngine->GetEnumCount(); ++I)
            if (InEngine->GetEnumByIndex(I) == InCookie) return true;
        return false;
    }

    bool BindingMatches() const
    {
        // Membership is checked before calling methods on a potentially retired registration.
        if (FAngelscriptManager::Get().Engine != Engine || !Registered(Engine, Cookie)
            || Cookie->GetEngine() != Engine || (Cookie->GetFlags() & asOBJ_ENUM) == 0
            || Cookie->GetModule() != nullptr || Cookie->GetUserData() != Identity
            || Cookie->GetEnumValueCount() != static_cast<asUINT>(Symbols.Num())) return false;
        for (int32 I = 0; I < Symbols.Num(); ++I)
        {
            int32 Value = 0;
            const char* Name = Cookie->GetEnumValueByIndex(I, &Value);
            if (!Name || Value != Symbols[I].Value || FCStringAnsi::Strcmp(Name, Symbols[I].Name.GetData()) != 0) return false;
        }
        return true;
    }

    bool EnumMatches() const
    {
        if (ScriptBinding.IsValid())
            return Root.IsResolved() && Root.GetHandle() == Handle && ScriptBinding->Matches();
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope || Scope->Epoch != Epoch || Index < 0 || Serial <= 0 || !Root.IsResolved()
            || Root.GetHandle() != Handle) return false;
        const FUObjectItem* Slot = GUObjectArray.IndexToObject(Index);
        if (!Slot || Slot->GetSerialNumber() != Serial || Slot->GetObject() != Identity
            || Slot->HasAnyFlags(EInternalObjectFlags::Garbage | EInternalObjectFlags::Unreachable)) return false;
        // Identity is safe to inspect only after the public slot/serial/address proof above.
        if (Identity->GetClass() != UEnum::StaticClass()
            || Identity->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
            || Identity->HasAnyEnumFlags(EEnumFlags::NewerVersionExists)
            || Identity->GetFName() != EnumName || Identity->GetOuter() != EnumOuter
            || Identity->GetCppForm() != UEnum::ECppForm::EnumClass || Identity->GetUnderlyingType() != UnderlyingKind
            || Identity->HasAnyEnumFlags(EEnumFlags::Flags) != bFlags || Identity->NumEnums() != Definition.Num()) return false;
        for (int32 I = 0; I < Definition.Num(); ++I)
            if (Identity->GetNameByIndex(I) != Definition[I].Key || Identity->GetValueByIndex(I) != Definition[I].Value) return false;
        return BindingMatches();
    }

    bool Matches(const FProperty* Field) const
    {
        if (!(ScriptBinding.IsValid() ? IsQuietScriptEnumShape(Field) : IsQuietNativeEnumShape(Field))
            || !EnumMatches()) return false;
        const FEnumProperty* EnumField = CastFieldChecked<FEnumProperty>(Field);
        const FNumericProperty* Current = EnumField->GetUnderlyingProperty();
        if (Current != Inner || Current->GetOwnerVariant() != InnerOwner || Current->GetClass() != InnerClass
            || Current->GetFName() != InnerName || Current->GetPropertyFlags() != InnerFlags
            || Current->ArrayDim != InnerArrayDim || Current->GetElementSize() != InnerSize
            || Current->GetOffset_ForInternal() != InnerOffset) return false;
        if (!ScriptBinding.IsValid())
        {
            const auto& ByteEnum = CastFieldChecked<FByteProperty>(Current)->Enum;
            if (!ByteEnum.IsResolved() || ByteEnum.GetHandle() != InnerEnumHandle) return false;
        }
        // SameType reads the private original association itself; it does not query GetEnum or resolve it.
        // The resolved shadow side is kept alive above even if the original side becomes unresolved.
        return EnumField->FEnumProperty::SameType(Shadow);
    }

    bool Capture(const FProperty* Field, asITypeInfo* InCookie, const FNativeEnumTypeWitness* SealedBinding = nullptr,
        TSharedPtr<FScriptEnumReloadBinding> InScriptBinding = nullptr)
    {
        if (InScriptBinding.IsValid())
        {
            if (!IsQuietScriptEnumShape(Field) || !InScriptBinding->Matches()
                || (InCookie != InScriptBinding->Plan.PreviousCookie && InCookie != InScriptBinding->Plan.CandidateCookie)
                || (SealedBinding && SealedBinding->ScriptBinding != InScriptBinding)) return false;
            ScriptBinding = InScriptBinding;
            Identity = ScriptBinding->Identity;
            Root = const_cast<UEnum*>(Identity);
            Handle = ScriptBinding->Handle;
            return CaptureProperty(Field);
        }
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        Engine = FAngelscriptManager::Get().Engine;
        if (!Scope || !IsQuietNativeEnumShape(Field) || !Registered(Engine, InCookie)
            || InCookie->GetModule() != nullptr) return false;
        Cookie = InCookie;
        Cookie->AddRef(); // Prevent cookie/address ABA; current registration is checked separately.
        Epoch = Scope->Epoch;
        if (SealedBinding)
        {
            if (Cookie != SealedBinding->Cookie || Epoch != SealedBinding->Epoch || !SealedBinding->EnumMatches()) return false;
            // Candidate fields inherit the original pre-swap binding; never recapture its definition.
            Identity = SealedBinding->Identity;
            Root = const_cast<UEnum*>(Identity);
            Handle = SealedBinding->Handle;
            Index = SealedBinding->Index;
            Serial = SealedBinding->Serial;
            EnumName = SealedBinding->EnumName;
            EnumOuter = SealedBinding->EnumOuter;
            bFlags = SealedBinding->bFlags;
            UnderlyingKind = SealedBinding->UnderlyingKind;
            Definition = SealedBinding->Definition;
            Symbols = SealedBinding->Symbols;
        }
        else
        {
            for (TObjectIterator<UEnum> It; It; ++It)
                if (static_cast<const void*>(*It) == Cookie->GetUserData() && It->GetClass() == UEnum::StaticClass())
                { Root = *It; Identity = *It; break; }
            if (!Identity || Identity->IsUnreachable() || Identity->HasAnyInternalFlags(EInternalObjectFlags::Garbage)
                || Identity->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
                || Identity->HasAnyEnumFlags(EEnumFlags::NewerVersionExists)
                || Identity->GetCppForm() != UEnum::ECppForm::EnumClass
                || Identity->GetUnderlyingType() != UEnum::EUnderlyingType::uint8) return false;
            Index = GUObjectArray.ObjectToIndex(Identity);
            Serial = GUObjectArray.AllocateSerialNumber(Index);
            Handle = Root.GetHandle();
            EnumName = Identity->GetFName();
            EnumOuter = Identity->GetOuter();
            bFlags = Identity->HasAnyEnumFlags(EEnumFlags::Flags);
            UnderlyingKind = Identity->GetUnderlyingType();
            for (int32 I = 0; I < Identity->NumEnums(); ++I)
                Definition.Emplace(Identity->GetNameByIndex(I), Identity->GetValueByIndex(I));
            for (asUINT I = 0; I < Cookie->GetEnumValueCount(); ++I)
            {
                FNativeEnumSymbol& Symbol = Symbols.AddDefaulted_GetRef();
                const char* Name = Cookie->GetEnumValueByIndex(I, &Symbol.Value);
                if (!Name) return false;
                const int32 Length = FCStringAnsi::Strlen(Name) + 1;
                Symbol.Name.Append(Name, Length);
                int32 Matches = 0;
                for (const auto& Row : Definition)
                {
                    FString ShortName = Row.Key.ToString();
                    int32 Colon = INDEX_NONE;
                    if (ShortName.FindLastChar(TEXT(':'), Colon)) ShortName.RightChopInline(Colon + 1);
                    if (ShortName == ANSI_TO_TCHAR(Name) && Row.Value == Symbol.Value) ++Matches;
                }
                if (Matches != 1) return false;
            }
        }
        return CaptureProperty(Field);
    }

    bool CaptureProperty(const FProperty* Field)
    {
        const FEnumProperty* EnumField = CastFieldChecked<FEnumProperty>(Field);
        Inner = EnumField->GetUnderlyingProperty();
        InnerOwner = Inner->GetOwnerVariant();
        InnerClass = Inner->GetClass();
        InnerName = Inner->GetFName();
        InnerFlags = Inner->GetPropertyFlags();
        InnerSize = Inner->GetElementSize();
        InnerArrayDim = Inner->ArrayDim;
        InnerOffset = Inner->GetOffset_ForInternal();
        if (!ScriptBinding.IsValid()) InnerEnumHandle = CastFieldChecked<FByteProperty>(Inner)->Enum.GetHandle();
        ShadowStorage.Reset(CastFieldChecked<FEnumProperty>(
            FEnumProperty::StaticClass()->Construct(Field->GetOwnerVariant(), NAME_None)));
        FEnumProperty* MutableShadow = ShadowStorage.Get();
        MutableShadow->SetEnum(const_cast<UEnum*>(Identity));
        FProperty* OwnedInner = CastFieldChecked<FProperty>(
            (ScriptBinding.IsValid() ? FInt8Property::StaticClass() : FByteProperty::StaticClass())->Construct(
                MutableShadow, FName(TEXT("UnderlyingType"))));
        if (OwnedInner == Inner) return false;
        MutableShadow->AddCppProperty(OwnedInner);
        Shadow = MutableShadow;
        return Matches(Field);
    }
};

bool IsQuietStringShape(const FProperty* Property)
{
    return Property && Property->GetClass() == FStrProperty::StaticClass() && Property->ArrayDim == 1
        && !Property->HasGetter() && !Property->HasSetter() && Property->GetElementSize() == sizeof(FString)
        && Property->GetOffset_ForInternal() % alignof(FString) == 0;
}

TSharedPtr<FAngelscriptType> CanonicalStringType(FStrProperty& Shadow)
{
    // The ordered property registry comes from native Register, not the mutable name alias map.
    // Disable TypeFinders: Bind_FString's FStrProperty implementation is the original native owner.
    return FAngelscriptType::GetByProperty(&Shadow, false);
}

bool IsRegisteredStringCookie(asIScriptEngine* Engine, const asITypeInfo* Cookie)
{
    if (!Engine || !Cookie) return false;
    for (asUINT I = 0; I < Engine->GetObjectTypeCount(); ++I)
        if (Engine->GetObjectTypeByIndex(I) == Cookie) return true;
    return false;
}

asITypeInfo* NativeStringCookie(const FAngelscriptTypeUsage& Usage)
{
    if (!Usage.Type.IsValid() || Usage.bIsReference || !Usage.SubTypes.IsEmpty()) return nullptr;
    TUniquePtr<FStrProperty> Shadow(CastField<FStrProperty>(
        FStrProperty::StaticClass()->Construct(FFieldVariant(), NAME_None)));
    if (!Shadow) return nullptr;
    const auto Type = CanonicalStringType(*Shadow);
    if (!Type.IsValid() || Usage.Type != Type
        || FAngelscriptType::GetByAngelscriptTypeName(TEXT("FString")) != Type) return nullptr;
    asIScriptEngine* Engine = FAngelscriptManager::Get().Engine;
    asITypeInfo* Cookie = Engine ? Engine->GetTypeInfoByName("FString") : nullptr;
    if (!IsRegisteredStringCookie(Engine, Cookie) || Cookie->GetEngine() != Engine || Cookie->GetModule()
        || (Cookie->GetFlags() & asOBJ_VALUE) == 0
        || (Cookie->GetFlags() & (asOBJ_REF | asOBJ_SCRIPT_OBJECT | asOBJ_ENUM | asOBJ_TEMPLATE)) != 0
        || Cookie->GetSize() != sizeof(FString)) return nullptr;
    return Cookie;
}

asITypeInfo* DeclaredStringCookie(const FAngelscriptClassDesc& Desc, const FProperty* Field, bool bActualOffset)
{
    asITypeInfo* Result = nullptr;
    if (!Desc.ScriptType || !IsQuietStringShape(Field)) return nullptr;
    for (const auto& Property : Desc.Properties)
    {
        const FString& UnrealName = Property->UnrealPropertyNameOverride.IsSet()
            ? Property->UnrealPropertyNameOverride.GetValue() : Property->PropertyName;
        if (FName(*UnrealName) != Field->GetFName()) continue;
        const char* Name = nullptr;
        int32 Id = 0;
        int32 Offset = 0;
        if (Result || Property->ScriptPropertyIndex < 0
            || Desc.ScriptType->GetProperty(Property->ScriptPropertyIndex, &Name, &Id, nullptr, nullptr, &Offset) < 0
            || !Name || Property->PropertyName != ANSI_TO_TCHAR(Name) || Offset < 0
            || Property->ScriptPropertyOffset != static_cast<SIZE_T>(Offset)
            || (bActualOffset && Field->GetOffset_ForInternal() != Offset)) return nullptr;
        Result = NativeStringCookie(Property->PropertyType);
        if (!Result || Desc.ScriptType->GetEngine() != Result->GetEngine()
            || Desc.ScriptType->GetEngine()->GetTypeInfoById(Id) != Result
            || !Property->PropertyType.EqualsUnqualified(FAngelscriptTypeUsage::FromTypeId(Id))) return nullptr;
    }
    return Result;
}

struct FNativeStringTypeWitness
{
    TUniquePtr<FStrProperty> Shadow;
    TSharedPtr<FAngelscriptType> Type;
    asIScriptEngine* Engine = nullptr;
    asITypeInfo* Cookie = nullptr;
    asDWORD Flags = 0;
    int32 TypeId = 0;
    uint64 Epoch = 0;

    ~FNativeStringTypeWitness() { if (Cookie) Cookie->Release(); }

    bool Matches() const
    {
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope || Scope->Epoch != Epoch || FAngelscriptManager::Get().Engine != Engine
            || !Shadow || !Type.IsValid() || CanonicalStringType(*Shadow) != Type
            || FAngelscriptType::GetByAngelscriptTypeName(TEXT("FString")) != Type
            || !IsRegisteredStringCookie(Engine, Cookie)) return false;
        // The pin keeps storage alive; only current membership authorizes cookie methods.
        return Cookie->GetEngine() == Engine && Cookie->GetModule() == nullptr
            && Cookie->GetFlags() == Flags && Cookie->GetSize() == sizeof(FString)
            && Cookie->GetTypeId() == TypeId && Engine->GetTypeInfoById(TypeId) == Cookie
            && Engine->GetTypeInfoByName("FString") == Cookie;
    }

    bool Capture(asITypeInfo* InCookie)
    {
        if (Cookie) return false;
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope) return false;
        Shadow = MakeUnique<FStrProperty>(FFieldVariant(), NAME_None);
        Type = CanonicalStringType(*Shadow);
        FAngelscriptTypeUsage Usage;
        Usage.Type = Type;
        if (!InCookie || NativeStringCookie(Usage) != InCookie) return false;
        Engine = FAngelscriptManager::Get().Engine;
        Epoch = Scope->Epoch;
        Cookie = InCookie;
        Cookie->AddRef();
        Flags = Cookie->GetFlags();
        TypeId = Cookie->GetTypeId();
        return Matches();
    }
};

// This route admits one top-level array with an exact strong object leaf only.
const FObjectProperty* QuietReferenceArrayLeaf(const FProperty* Property)
{
    if (!Property || Property->GetClass() != FArrayProperty::StaticClass()
        || Property->ArrayDim != 1 || Property->HasGetter() || Property->HasSetter()
        || Property->GetElementSize() != sizeof(FScriptArray)) return nullptr;
    const FArrayProperty* Array = CastFieldChecked<FArrayProperty>(Property);
    if (Array->ArrayFlags != EArrayPropertyFlags::None) return nullptr;
    const FProperty* Inner = Array->Inner;
    if (!Inner || Inner->GetClass() != FObjectProperty::StaticClass() || Inner->ArrayDim != 1
        || Inner->GetOwnerVariant() != FFieldVariant(const_cast<FProperty*>(Property))
        || Inner->GetOffset_ForInternal() != 0 || Inner->HasGetter() || Inner->HasSetter()
        || Inner->GetElementSize() != sizeof(TObjectPtr<UObject>)
        || Inner->GetMinAlignment() != alignof(TObjectPtr<UObject>)
        || Inner->HasAnyPropertyFlags(CPF_SkipSerialization | CPF_Deprecated)) return nullptr;
    return CastFieldChecked<FObjectProperty>(Inner);
}

bool IsQuietReferenceArrayStorage(const FProperty* Property, const void* Address)
{
    if (!QuietReferenceArrayLeaf(Property) || !Address
        || reinterpret_cast<UPTRINT>(Address) % alignof(FScriptArray) != 0) return false;
    const FScriptArray& Array = *static_cast<const FScriptArray*>(Address);
    const int32 Count = Array.NumUnchecked();
    return Count >= 0 && Array.Max() >= Count
        && (!Count || (Array.GetData()
            && reinterpret_cast<UPTRINT>(Array.GetData()) % alignof(TObjectPtr<UObject>) == 0));
}

bool IsQuietPayloadField(const FProperty* Property)
{
    if (Property->GetClass() == FArrayProperty::StaticClass()) return QuietReferenceArrayLeaf(Property) != nullptr;
    const FFieldClass* Class = Property->GetClass();
    if (Class == FStrProperty::StaticClass()) return IsQuietStringShape(Property);
    if (Class == FEnumProperty::StaticClass())
        return IsQuietNativeEnumShape(Property) || IsQuietScriptEnumShape(Property);
    const bool bNumeric = Class == FByteProperty::StaticClass() || Class == FInt8Property::StaticClass()
        || Class == FInt16Property::StaticClass() || Class == FIntProperty::StaticClass()
        || Class == FInt64Property::StaticClass() || Class == FUInt16Property::StaticClass()
        || Class == FUInt32Property::StaticClass() || Class == FUInt64Property::StaticClass()
        || Class == FFloatProperty::StaticClass() || Class == FDoubleProperty::StaticClass();
    if (!bNumeric && Class != FBoolProperty::StaticClass()
        && Class != FObjectProperty::StaticClass()) return false;
    if (Property->ArrayDim != 1 || Property->HasGetter() || Property->HasSetter()
        || Property->GetElementSize() <= 0 || Property->GetElementSize() > 8) return false;
    if (Class == FObjectProperty::StaticClass()) return Property->GetElementSize() == sizeof(TObjectPtr<UObject>);
    if (Class == FBoolProperty::StaticClass())
    {
        const FBoolProperty* Bool = CastFieldChecked<FBoolProperty>(Property);
        return Bool->GetBoolFieldSize() && Bool->GetFieldMask() && Bool->GetByteOffset() < Property->GetElementSize();
    }
    return true;
}

bool ArePayloadFieldsSerializable(const UScriptStruct* Struct)
{
    TArray<uint8> Bytes;
    FObjectWriter Save(Bytes);
    Save.ArNoDelta = true;
    Save.SetPortFlags(PPF_None);
    for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeDeprecated); It; ++It)
        if (!IsQuietPayloadField(*It) || It->HasAnyPropertyFlags(CPF_SkipSerialization | CPF_Deprecated)
            || !It->ShouldSerializeValue(Save)
            || (It->GetClass() == FArrayProperty::StaticClass()
                && !QuietReferenceArrayLeaf(*It)->ShouldSerializeValue(Save))) return false;
    return true;
}

bool IsReferencePayloadSchema(const UScriptStruct* Struct, FString* Error = nullptr)
{
    const UASStruct* Script = Cast<UASStruct>(Struct);
    if (!Script || !Script->ScriptType || !ArePayloadFieldsSerializable(Struct)) return false;
    asUINT ReflectedFields = 0;
    for (TFieldIterator<FProperty> It(Struct); It; ++It)
    {
        ++ReflectedFields;
        if (!IsQuietPayloadField(*It))
        {
            if (Error) *Error = FString::Printf(TEXT("Unsupported field %s class=%s type=%s"),
                *It->GetName(), *It->GetClass()->GetName(), *It->GetCPPType());
            return false;
        }
    }
    if (ReflectedFields != Script->ScriptType->GetPropertyCount())
    {
        if (Error) *Error = FString::Printf(TEXT("Hidden schema fields script=%u reflected=%u"),
            Script->ScriptType->GetPropertyCount(), ReflectedFields);
        return false;
    }
    return true;
}

bool IsStableNativeObject(const UObject* Object, const TSet<const UClass*>& ReplacementClasses)
{
    TSet<const UObject*> Visited;
    for (const UObject* Cursor = Object; Cursor; Cursor = Cursor->GetOuter())
    {
        if (Visited.Contains(Cursor) || !IsValid(Cursor) || Cursor->IsTemplate()
            || Cursor->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_DefaultSubObject)
            || Cast<UClass>(Cursor) || Cast<UScriptStruct>(Cursor)) return false;
        Visited.Add(Cursor);
        for (const UClass* Class = Cursor->GetClass(); Class; Class = Class->GetSuperClass())
            if (!Class->HasAnyClassFlags(CLASS_Native)
                || Class->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_CompiledFromBlueprint)
                || Cast<UASClass>(Class) || ReplacementClasses.Contains(Class)) return false;
    }
    return Object != nullptr;
}

class FPayloadAdmission
{
public:
    TMap<const UScriptStruct*, const FAngelscriptClassDesc*> Replaced;
    TMap<const asITypeInfo*, const FAngelscriptClassDesc*> Candidates;
    TSet<const asITypeInfo*> CheckedTypes;
    TSet<const void*> ActiveStorage;
    TSet<const UClass*> ReplacementClasses;
    FString& Error;

    explicit FPayloadAdmission(FString& InError) : Error(InError) {}

    bool Reject(const TCHAR* Reason) { Error = Reason; return false; }

    bool Type(const FAngelscriptTypeUsage& Usage, bool bAllowDirectReferences = false)
    {
        if (!Usage.Type.IsValid()) return Reject(TEXT("Unresolved candidate payload type"));
        if (Usage.Type->IsObjectPointer() || Usage.Type->IsUnresolvedObjectPointer() || Usage.GetClass())
        {
            const FAngelscriptClassDesc* const* Candidate =
                Candidates.Find(Usage.Type->GetAngelscriptTypeInfo(Usage));
            if (!Candidate && Usage.Type == FAngelscriptType::GetScriptObject())
                Candidate = Candidates.Find(Usage.ScriptClass);
            if (bAllowDirectReferences && Usage.Type->IsObjectPointer()
                && !Usage.Type->IsUnresolvedObjectPointer() && Usage.SubTypes.IsEmpty()
                && (Usage.GetClass() || (Candidate && !(*Candidate)->bIsStruct))) return true;
            return Reject(TEXT("Candidate payload contains an object reference"));
        }
        if (Usage.IsPrimitive() || Usage.Type == FAngelscriptType::GetScriptEnum()) return true;
        if (bAllowDirectReferences && NativeStringCookie(Usage)) return true;
        if (bAllowDirectReferences)
        {
            if (Usage.Type->GetAngelscriptTypeName(Usage) == TEXT("TArray") && Usage.SubTypes.Num() == 1)
            {
                const auto& Leaf = Usage.SubTypes[0];
                if (!Leaf.Type.IsValid() || !Leaf.Type->IsObjectPointer()
                    || Leaf.Type->IsUnresolvedObjectPointer() || !Leaf.SubTypes.IsEmpty())
                    return Reject(TEXT("Reference payload arrays require one direct strong object subtype"));
                return Type(Leaf, true);
            }
            return Reject(TEXT("Reference payload candidates require scalar, native string, direct strong object ")
                TEXT("or top-level strong object array fields"));
        }
        if (Usage.Type == FAngelscriptType::GetScriptStruct())
        {
            // ScriptClass is a tagged union member; read it only for script structures.
            if (const FAngelscriptClassDesc* const* Candidate = Candidates.Find(Usage.ScriptClass))
                return Schema(**Candidate);
            const UStruct* Existing = Usage.GetUnrealStruct();
            return (Existing && !HasSchemaReferences(Existing))
                || Reject(TEXT("Unresolved or reference-bearing script payload member"));
        }
        const asITypeInfo* Info = Usage.Type->GetAngelscriptTypeInfo(Usage);
        if (Info && (Info->GetFlags() & asOBJ_ENUM) != 0) return true;
        if (const FAngelscriptClassDesc* const* Candidate = Candidates.Find(Info)) return Schema(**Candidate);
        if (const UStruct* Native = Usage.GetUnrealStruct())
            return !HasSchemaReferences(Native) || Reject(TEXT("Candidate native payload member contains references"));
        const FString Name = Usage.Type->GetAngelscriptTypeName(Usage);
        if (Name == TEXT("FString") || Name == TEXT("FName") || Name == TEXT("FText") || Name == TEXT("string")) return true;
        const int32 Arity = Name == TEXT("TMap") ? 2
            : (Name == TEXT("TArray") || Name == TEXT("TSet") || Name == TEXT("TOptional")) ? 1 : 0;
        if (Arity != 0 && Usage.SubTypes.Num() == Arity)
        {
            for (const FAngelscriptTypeUsage& Sub : Usage.SubTypes)
                if (!Type(Sub)) return false;
            return true;
        }
        return Reject(TEXT("Candidate payload type is not classified for reference-safe migration"));
    }

    bool Schema(const FAngelscriptClassDesc& Candidate, bool bAllowDirectReferences = false)
    {
        if (!Candidate.bIsStruct || !Candidate.ScriptType)
            return Reject(TEXT("Candidate payload member is not a resolved value structure"));
        if (bAllowDirectReferences && Candidate.Properties.Num() != Candidate.ScriptType->GetPropertyCount())
            return Reject(TEXT("Reference payload candidates require a reflected descriptor for every script field"));
        if (!bAllowDirectReferences && CheckedTypes.Contains(Candidate.ScriptType)) return true;
        if (!bAllowDirectReferences) CheckedTypes.Add(Candidate.ScriptType);
        TSet<int32> CandidateFields;
        for (const auto& Property : Candidate.Properties)
        {
            if (bAllowDirectReferences)
            {
                const char* ScriptName = nullptr;
                int32 TypeId = 0;
                int32 Offset = 0;
                if (Property->ScriptPropertyIndex < 0 || CandidateFields.Contains(Property->ScriptPropertyIndex)
                    || Candidate.ScriptType->GetProperty(Property->ScriptPropertyIndex, &ScriptName, &TypeId, nullptr, nullptr, &Offset) < 0
                    || !ScriptName || Property->PropertyName != ANSI_TO_TCHAR(ScriptName)
                    || Offset < 0 || Property->ScriptPropertyOffset != static_cast<SIZE_T>(Offset)
                    || !Property->PropertyType.EqualsUnqualified(FAngelscriptTypeUsage::FromTypeId(TypeId)))
                    return Reject(TEXT("Reference payload candidate reflected descriptor does not match its script field"));
                CandidateFields.Add(Property->ScriptPropertyIndex);
                // AddClassProperties creates the FProperty after module swap; prove its declared input here.
                if (!Property->PropertyType.CanCreateProperty() || Property->bSkipSerialization)
                    return Reject(*FString::Printf(TEXT("Reference payload fields must have serialized reflected storage field=%s skipped=%d"),
                        *Property->PropertyName, Property->bSkipSerialization));
                const asITypeInfo* Info = Candidate.ScriptType->GetEngine()->GetTypeInfoById(TypeId);
                if (asITypeInfo* StringCookie = NativeStringCookie(Property->PropertyType))
                    if (Info != StringCookie || Offset % alignof(FString) != 0)
                        return Reject(TEXT("Native string candidate differs from its compiled field type"));
                if (Info && (Info->GetFlags() & asOBJ_ENUM) != 0)
                {
                    // Bind_UEnum creates FByteProperty only for a native unscoped UEnum.
                    // Script enums, enum classes and user-defined enums create FEnumProperty.
                    bool bQuietByteEnum = false;
                    if (Property->PropertyType.Type != FAngelscriptType::GetScriptEnum())
                        for (TObjectIterator<UEnum> Enum; Enum; ++Enum)
                            if (static_cast<const void*>(*Enum) == Info->GetUserData()
                                && Enum->GetClass() == UEnum::StaticClass() && !Enum->IsUnreachable()
                                && !Enum->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
                                && Info->GetModule() == nullptr
                                && (Enum->GetCppForm() != UEnum::ECppForm::EnumClass
                                    || Enum->GetUnderlyingType() == UEnum::EUnderlyingType::uint8)) bQuietByteEnum = true;
                    if (!bQuietByteEnum && !FindScriptEnumInput(const_cast<asITypeInfo*>(Info)))
                        return Reject(TEXT("Reference payload enum field has no complete quiet type identity"));
                }
            }
            if (!Type(Property->PropertyType, bAllowDirectReferences))
            {
                Error = Candidate.ClassName + TEXT(".") + Property->PropertyName + TEXT(": ") + Error;
                return false;
            }
        }
        return true;
    }

    bool ReferenceRoute(const UScriptStruct* Old, const FAngelscriptClassDesc& Candidate) const
    {
        if (HasSchemaReferences(Old)) return true;
        for (const auto& Property : Candidate.Properties)
        {
            const auto& Usage = Property->PropertyType;
            if (!Usage.Type.IsValid()) continue;
            if (Usage.Type->IsObjectPointer() || Usage.Type->IsUnresolvedObjectPointer() || Usage.GetClass()) return true;
            if (Usage.Type->GetAngelscriptTypeName(Usage) == TEXT("TArray") && Usage.SubTypes.Num() == 1)
            {
                const auto& Leaf = Usage.SubTypes[0];
                if (Leaf.Type.IsValid() && (Leaf.Type->IsObjectPointer()
                    || Leaf.Type->IsUnresolvedObjectPointer() || Leaf.GetClass())) return true;
            }
        }
        return false;
    }

    bool Storage(const UStruct* Struct, const void* Data, UObject* Owner = nullptr)
    {
        if (ActiveStorage.Contains(Data)) return Reject(TEXT("Cyclic dynamic payload storage before module swap"));
        ActiveStorage.Add(Data);
        ON_SCOPE_EXIT { ActiveStorage.Remove(Data); };
        for (FPropertyValueIterator It(FStructProperty::StaticClass(), Struct, Data); It; ++It)
        {
            if (CastFieldChecked<FStructProperty>(It.Key())->Struct != FInstancedStruct::StaticStruct()) continue;
            It.SkipRecursiveProperty();
            const FInstancedStruct& Payload = *static_cast<const FInstancedStruct*>(It.Value());
            if (!Payload.IsValid()) continue;
            if (const FAngelscriptClassDesc* const* Candidate = Replaced.Find(Payload.GetScriptStruct()))
            {
                const bool bReferenceRoute = ReferenceRoute(Payload.GetScriptStruct(), **Candidate);
                if (bReferenceRoute)
                {
                    TArray<const FProperty*> Chain;
                    It.GetPropertyChain(Chain);
                    if (!Owner || Chain.Num() != 1 || Chain.Last()->ArrayDim != 1
                        || !IsStableNativeObject(Owner, ReplacementClasses))
                        return Reject(TEXT("Reference payload reload requires a direct stable native owner"));
                    if (!ArePayloadFieldsSerializable(Payload.GetScriptStruct()))
                        return Reject(TEXT("Existing reference payload fields are not completely serializable"));
                    FString SchemaError;
                    if (!IsReferencePayloadSchema(Payload.GetScriptStruct(), &SchemaError))
                        return Reject(*(TEXT("Reference payload schema requires scalar or direct strong object fields: ") + SchemaError));
                    if (!Schema(**Candidate, true)) return false;
                    for (TFieldIterator<FProperty> Field(Payload.GetScriptStruct()); Field; ++Field)
                    {
                        const FObjectProperty* Leaf = Field->GetClass() == FObjectProperty::StaticClass()
                            ? CastFieldChecked<FObjectProperty>(*Field) : QuietReferenceArrayLeaf(*Field);
                        if (!Leaf) continue;
                        const FAngelscriptPropertyDesc* NewField = nullptr;
                        for (const auto& Property : (*Candidate)->Properties)
                        {
                            const FString& Name = Property->UnrealPropertyNameOverride.IsSet()
                                ? Property->UnrealPropertyNameOverride.GetValue() : Property->PropertyName;
                            if (FName(*Name) == Field->GetFName()) { NewField = &Property.Get(); break; }
                        }
                        if (!NewField) return Reject(TEXT("Reference payload reload must retain every old reference field"));
                        const FAngelscriptTypeUsage* Usage = &NewField->PropertyType;
                        if (Field->GetClass() == FArrayProperty::StaticClass())
                        {
                            if (!Usage->Type.IsValid() || Usage->Type->GetAngelscriptTypeName(*Usage) != TEXT("TArray")
                                || Usage->SubTypes.Num() != 1)
                                return Reject(TEXT("Reference payload reload must retain the object array field shape"));
                            Usage = &Usage->SubTypes[0];
                        }
                        if (!Usage->Type.IsValid() || !Usage->Type->IsObjectPointer()
                            || Usage->Type->IsUnresolvedObjectPointer() || !Usage->SubTypes.IsEmpty())
                            return Reject(TEXT("Reference payload reload must retain every strong object leaf"));
                        UClass* ExpectedClass = Usage->GetClass();
                        auto AdmitReference = [&](UObject* Reference)
                        {
                            if (!Reference) return true;
                            if (!IsValid(Reference) || Reference->IsUnreachable()
                                || Reference->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
                                return Reject(TEXT("Payload reference is not a live object identity"));
                            if (ExpectedClass && !Reference->IsA(ExpectedClass))
                                return Reject(TEXT("Reference payload candidate class is incompatible with the saved identity"));
                            const UASClass* ScriptClass = Cast<UASClass>(Reference->GetClass());
                            if (ScriptClass && ReplacementClasses.Contains(ScriptClass)
                                && !Reference->IsTemplate() && !Reference->HasAnyFlags(
                                    RF_ClassDefaultObject | RF_ArchetypeObject | RF_DefaultSubObject)) return true;
                            if (ScriptClass)
                            {
                                const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
                                const auto* Plan = AngelscriptReloadSafety::FindScriptObjectReloadPlan(Reference->GetClass());
                                AngelscriptRuntimeGcMirror::FLiveScriptReferenceWitness Witness;
                                return Scope && Plan && AngelscriptRuntimeGcMirror::CaptureLiveScriptReferenceWitness(
                                    Reference, *Plan, Scope->Epoch, Witness, Error);
                            }
                            return IsStableNativeObject(Reference, ReplacementClasses)
                                || Reject(TEXT("Payload reference has no stable native or ordinary replacement identity"));
                        };
                        if (Field->GetClass() == FArrayProperty::StaticClass())
                        {
                            const FArrayProperty* Array = CastFieldChecked<FArrayProperty>(*Field);
                            if (!IsQuietReferenceArrayStorage(Array, Array->ContainerPtrToValuePtr<void>(Payload.GetMemory())))
                                return Reject(TEXT("Payload reference array has invalid storage"));
                            FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Payload.GetMemory()));
                            const int32 Count = Values.Num();
                            if (Count < 0) return Reject(TEXT("Payload reference array has invalid storage"));
                            for (int32 Element = 0; Element < Count; ++Element)
                            {
                                if (!IsQuietReferenceArrayStorage(Array, Array->ContainerPtrToValuePtr<void>(Payload.GetMemory())))
                                    return Reject(TEXT("Payload reference array storage changed during admission"));
                                FScriptArrayHelper Current(Array, Array->ContainerPtrToValuePtr<void>(Payload.GetMemory()));
                                if (Current.Num() != Count) return Reject(TEXT("Payload reference array length changed during admission"));
                                if (!AdmitReference(Leaf->GetObjectPropertyValue(Current.GetRawPtr(Element))))
                                    return false;
                            }
                        }
                        else if (!AdmitReference(Leaf->GetObjectPropertyValue_InContainer(Payload.GetMemory()))) return false;
                    }
                }
                else if (!Schema(**Candidate)) return false;
            }
            if (!Storage(Payload.GetScriptStruct(), Payload.GetMemory())) return false;
        }
        return true;
    }
};

bool ContainsReloadPayload(const UStruct* Struct, const void* Data,
    const TMap<UScriptStruct*, UScriptStruct*>& Replacements, TSet<const void*>& Active, FString& Error)
{
    if (Active.Contains(Data)) { Error = TEXT("Cyclic dynamic script payload storage"); return false; }
    Active.Add(Data);
    ON_SCOPE_EXIT { Active.Remove(Data); };
    bool Found = false;
    for (FPropertyValueIterator It(FStructProperty::StaticClass(), Struct, Data); It; ++It)
    {
        if (CastFieldChecked<FStructProperty>(It.Key())->Struct != FInstancedStruct::StaticStruct()) continue;
        It.SkipRecursiveProperty();
        const FInstancedStruct& Payload = *static_cast<const FInstancedStruct*>(It.Value());
        if (!Payload.IsValid()) continue;
        const bool Nested = ContainsReloadPayload(Payload.GetScriptStruct(), Payload.GetMemory(), Replacements, Active, Error);
        if (!Error.IsEmpty()) return false;
        Found |= Replacements.Contains(const_cast<UScriptStruct*>(Payload.GetScriptStruct())) || Nested;
    }
    return Found;
}

struct FPreparedProperty
{
    TStrongObjectPtr<UObject> Owner;
    const FProperty* Property;
    void* Value;

    FPreparedProperty(UObject* InOwner, const FProperty* InProperty)
        : Owner(InOwner), Property(InProperty), Value(Property->AllocateAndInitializeValue())
    {
        Property->CopyCompleteValue(Value, Property->ContainerPtrToValuePtr<void>(Owner.Get()));
    }
    ~FPreparedProperty() { Property->DestroyAndFreeValue(Value); }
};

class FPayloadMigration
{
public:
    const TMap<UScriptStruct*, UScriptStruct*>& Replacements;
    FString& Error;
    TSet<const void*> ActivePayloads;

    bool Struct(const UStruct* Type, void* Data)
    {
        for (TFieldIterator<FProperty> It(Type); It; ++It)
            for (int32 Index = 0; Index < It->ArrayDim; ++Index)
                if (!Value(*It, It->ContainerPtrToValuePtr<void>(Data, Index))) return false;
        return true;
    }

    bool Value(const FProperty* Property, void* Data)
    {
        if (const FStructProperty* Field = CastField<FStructProperty>(Property))
        {
            if (Field->Struct != FInstancedStruct::StaticStruct()) return Struct(Field->Struct, Data);
            FInstancedStruct& Payload = *static_cast<FInstancedStruct*>(Data);
            if (!Payload.IsValid()) return true;
            if (ActivePayloads.Contains(Data)) return Fail(TEXT("Cyclic prepared script payload storage"));
            ActivePayloads.Add(Data);
            ON_SCOPE_EXIT { ActivePayloads.Remove(Data); };
            UScriptStruct* Old = const_cast<UScriptStruct*>(Payload.GetScriptStruct());
            if (UScriptStruct* const* Target = Replacements.Find(Old))
            {
                UASStruct* OldScript = Cast<UASStruct>(Old);
                UASStruct* NewScript = Cast<UASStruct>(*Target);
                if (!OldScript || !NewScript || OldScript == NewScript || OldScript->NewerVersion != NewScript
                    || NewScript->ResolveNewestStruct() != NewScript)
                    return Fail(TEXT("Invalid script payload replacement chain"));
                TArray<const FStructProperty*> Encountered;
                for (TFieldIterator<FProperty> It(Old); It; ++It)
                    if (It->ContainsObjectReference(Encountered, EPropertyObjectReferenceType_Any))
                        return Fail(TEXT("Reference-bearing payload migration requires a validated instance replacement route"));
                Encountered.Reset();
                for (TFieldIterator<FProperty> It(NewScript); It; ++It)
                    if (It->ContainsObjectReference(Encountered, EPropertyObjectReferenceType_Any))
                        return Fail(TEXT("Reference-bearing replacement payload is not admitted"));
                TArray<uint8> Bytes;
                FMemoryWriter Writer(Bytes, false);
                FObjectAndNameAsStringProxyArchive Save(Writer, false);
                Save.ArNoDelta = true;
                Old->SerializeTaggedProperties(Save, Payload.GetMutableMemory(), nullptr, nullptr);
                if (Save.IsError()) return Fail(TEXT("Script payload tagged save failed"));
                FInstancedStruct Replacement;
                Replacement.InitializeAs(NewScript);
                FMemoryReader Reader(Bytes, false);
                FObjectAndNameAsStringProxyArchive Load(Reader, false);
                NewScript->SerializeTaggedProperties(Load, Replacement.GetMutableMemory(), nullptr, nullptr);
                if (Load.IsError() || Reader.Tell() != Bytes.Num()) return Fail(TEXT("Script payload tagged load failed"));
                Payload = MoveTemp(Replacement);
            }
            return Struct(Payload.GetScriptStruct(), Payload.GetMutableMemory());
        }
        if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property))
        {
            if (void* Content = Optional->GetValuePointerForReplaceIfSet(Data))
                return Value(Optional->GetValueProperty(), Content);
        }
        else if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper Helper(Array, Data);
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
                if (!Value(Array->Inner, Helper.GetRawPtr(Index))) return false;
        }
        else if (const FMapProperty* Map = CastField<FMapProperty>(Property))
        {
            FScriptMapHelper Helper(Map, Data);
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
                if (Helper.IsValidIndex(Index) && (!Value(Map->KeyProp, Helper.GetKeyPtr(Index))
                    || !Value(Map->ValueProp, Helper.GetValuePtr(Index)))) return false;
            TMap<uint32, TArray<int32>> HashBuckets;
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index)) continue;
                TArray<int32>& Bucket = HashBuckets.FindOrAdd(Map->KeyProp->GetValueTypeHash(Helper.GetKeyPtr(Index)));
                for (int32 Previous : Bucket)
                    if (Map->KeyProp->Identical(Helper.GetKeyPtr(Previous), Helper.GetKeyPtr(Index)))
                        return Fail(TEXT("Payload migration would merge map entries"));
                Bucket.Add(Index);
            }
            Helper.Rehash();
        }
        else if (const FSetProperty* Set = CastField<FSetProperty>(Property))
        {
            FScriptSetHelper Helper(Set, Data);
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
                if (Helper.IsValidIndex(Index) && !Value(Set->ElementProp, Helper.GetElementPtr(Index))) return false;
            TMap<uint32, TArray<int32>> HashBuckets;
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index)) continue;
                TArray<int32>& Bucket = HashBuckets.FindOrAdd(Set->ElementProp->GetValueTypeHash(Helper.GetElementPtr(Index)));
                for (int32 Previous : Bucket)
                    if (Set->ElementProp->Identical(Helper.GetElementPtr(Previous), Helper.GetElementPtr(Index)))
                        return Fail(TEXT("Payload migration would merge set elements"));
                Bucket.Add(Index);
            }
            Helper.Rehash();
        }
        return true;
    }

    bool Fail(const TCHAR* Message) { Error = Message; return false; }
};
}

bool ValidateInstancedStructReload(const TArray<FAngelscriptPayloadReloadType>& Types, FString& Error)
{
    Error.Reset();
    FPayloadAdmission Admission(Error);
    if (!IsInGameThread()) return Admission.Reject(TEXT("Script payload admission requires the game thread"));
    for (const FAngelscriptPayloadReloadType& Type : Types)
    {
        if (!Type.Candidate.IsValid() || (Type.bWillReplace && !Type.Previous))
            return Admission.Reject(TEXT("Incomplete candidate payload identity"));
        if (Type.Candidate->ScriptType) Admission.Candidates.Add(Type.Candidate->ScriptType, Type.Candidate.Get());
        if (Type.bWillReplace) Admission.Replaced.Add(Type.Previous, Type.Candidate.Get());
        if (Type.bWillReplaceObjectClass) Admission.ReplacementClasses.Add(Type.PreviousObjectClass);
    }
    if (Admission.Replaced.IsEmpty()) return true;
    for (TObjectIterator<UObject> It; It; ++It)
    {
        UObject* Owner = *It;
        if (Owner->IsUnreachable() || Owner->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) continue;
        if (const UDataTable* Table = Cast<UDataTable>(Owner))
        {
            const UScriptStruct* Rows = Table->GetRowStruct();
            if (const FAngelscriptClassDesc* const* Candidate = Admission.Replaced.Find(Rows))
                if (Admission.ReferenceRoute(Rows, **Candidate))
                    return Admission.Reject(TEXT("Reference payload reload does not admit DataTable rows"));
            if (Rows)
                for (const TPair<FName, uint8*>& Row : Table->GetRowMap())
                    if (!Admission.Storage(Rows, Row.Value)) return false;
        }
        if (!Admission.Storage(Owner->GetClass(), Owner, Owner)) return false;
    }
    return FInstancedStructReloadScope::CaptureAdmission(Types, Error);
}

bool MigrateInstancedStructsForReload(const TMap<UScriptStruct*, UScriptStruct*>& Replacements, FString& Error)
{
    Error.Reset();
    if (Replacements.IsEmpty()) return true;
    if (!IsInGameThread()) { Error = TEXT("Script payload migration requires the game thread"); return false; }
    TArray<TUniquePtr<FPreparedProperty>> Prepared;
    FPayloadMigration Migration{Replacements, Error, {}};
    for (TObjectIterator<UObject> It; It; ++It)
    {
        UObject* Owner = *It;
        if (Owner->IsUnreachable() || Owner->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) continue;
        TSet<const FProperty*> Roots;
        for (FPropertyValueIterator Value(FStructProperty::StaticClass(), Owner->GetClass(), Owner); Value; ++Value)
        {
            if (CastFieldChecked<FStructProperty>(Value.Key())->Struct != FInstancedStruct::StaticStruct()) continue;
            Value.SkipRecursiveProperty();
            const FInstancedStruct& Payload = *static_cast<const FInstancedStruct*>(Value.Value());
            if (!Payload.IsValid()) continue;
            if (UScriptStruct* const* Target = Replacements.Find(const_cast<UScriptStruct*>(Payload.GetScriptStruct())))
                if (HasSchemaReferences(Payload.GetScriptStruct()) || HasSchemaReferences(*Target)) continue;
            TSet<const void*> Active;
            const bool Nested = ContainsReloadPayload(Payload.GetScriptStruct(), Payload.GetMemory(), Replacements, Active, Error);
            if (!Error.IsEmpty()) return false;
            if (!Replacements.Contains(const_cast<UScriptStruct*>(Payload.GetScriptStruct())) && !Nested) continue;
            TArray<const FProperty*> Chain;
            Value.GetPropertyChain(Chain);
            Roots.Add(Chain.Last());
        }
        for (const FProperty* Property : Roots)
        {
            TUniquePtr<FPreparedProperty> Item = MakeUnique<FPreparedProperty>(Owner, Property);
            for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
                if (!Migration.Value(Property, static_cast<uint8*>(Item->Value) + Index * Property->GetElementSize()))
                {
                    Error = Owner->GetPathName() + TEXT(".") + Property->GetName() + TEXT(": ") + Error;
                    return false;
                }
            Prepared.Add(MoveTemp(Item));
        }
    }
    for (const TUniquePtr<FPreparedProperty>& Item : Prepared)
        Item->Property->CopyCompleteValue(Item->Property->ContainerPtrToValuePtr<void>(Item->Owner.Get()), Item->Value);
    return true;
}

namespace
{
struct FPayloadReferenceIdentity
{
    FObjectKey OldKey;
    const void* OldIdentity = nullptr;
    int32 WitnessIndex = INDEX_NONE;
    UObject* Stable = nullptr;
    UObject* Resolved = nullptr;
    bool bRead = false;
    UObject* Captured = nullptr;
    bool bRequiresReplacement = false;
    bool bLiveAS = false;
    AngelscriptRuntimeGcMirror::FLiveScriptReferenceWitness LiveWitness;
};

struct FPayloadMappedIdentity
{
    FObjectKey Key;
    FObjectKey ClassKey;
    FObjectHandle Handle{};
};

struct FPayloadFieldSnapshot
{
    const FProperty* Property = nullptr;
    const UStruct* Owner = nullptr;
    const FFieldClass* Class = nullptr;
    FName Name;
    TObjectPtr<UClass> ObjectClass;
    TObjectPtr<UEnum> Enum;
    FObjectHandle ObjectClassHandle{};
    FObjectHandle EnumHandle{};
    EPropertyFlags Flags = CPF_None;
    int32 Offset = 0;
    int32 Size = 0;
    uint8 BoolMask = 0;
    uint8 BoolOffset = 0;
    uint8 BoolByteMask = 0;
    uint8 BoolSize = 0;
    uint8 Value[8]{};
    FObjectHandle ObjectHandle{};
    // Collector may rewrite the strong root. The comparison handle is never refreshed by GC.
    TObjectPtr<UObject> Object;
    TSharedPtr<const FNativeEnumTypeWitness> EnumWitness;
    TSharedPtr<const FNativeStringTypeWitness> StringWitness;
    // Owned value units exclude the terminator; no live FString header or allocation is aliased.
    TArray<TCHAR> StringUnits;
    const FObjectProperty* ArrayInner = nullptr;
    EPropertyFlags ArrayInnerFlags = CPF_None;
    FName ArrayInnerName;
    int32 ArrayInnerSize = 0;
    // Ordered owned roots and immutable handles; never retain a live array header/backing allocation.
    TArray<TObjectPtr<UObject>> ArrayObjects;
    TArray<FObjectHandle> ArrayHandles;

    static bool ReadStringUnits(const void* Address, const TCHAR*& Data, int32& Count)
    {
        if (!Address || reinterpret_cast<UPTRINT>(Address) % alignof(FString) != 0) return false;
        const auto& Chars = static_cast<const FString*>(Address)->GetCharArray();
        const int32 Num = Chars.Num();
        if (Num < 0 || Chars.Max() < Num || (Num && (!Chars.GetData() || Chars[Num - 1] != TCHAR(0)))) return false;
        Data = Chars.GetData();
        Count = Num ? Num - 1 : 0;
        return true;
    }

    bool CaptureMetadata(const FProperty* Field, asITypeInfo* EnumCookie = nullptr,
        const FNativeEnumTypeWitness* SealedBinding = nullptr,
        TSharedPtr<FScriptEnumReloadBinding> ScriptBinding = nullptr,
        TSharedPtr<const FNativeStringTypeWitness> InStringWitness = nullptr)
    {
        if (!IsQuietPayloadField(Field)) return false;
        Property = Field;
        Owner = Field->GetOwnerStruct();
        Class = Field->GetClass();
        Name = Field->GetFName();
        Flags = Field->GetPropertyFlags();
        Offset = Field->GetOffset_ForInternal();
        Size = Field->GetElementSize();
        if (Class == FStrProperty::StaticClass())
        {
            if (!InStringWitness.IsValid() || !InStringWitness->Matches()) return false;
            StringWitness = MoveTemp(InStringWitness);
        }
        if (Class == FEnumProperty::StaticClass())
        {
            TSharedPtr<FNativeEnumTypeWitness> Witness = MakeShared<FNativeEnumTypeWitness>();
            if (!Witness->Capture(Field, EnumCookie, SealedBinding, ScriptBinding)) return false;
            EnumWitness = Witness;
        }
        if (Class == FArrayProperty::StaticClass())
        {
            ArrayInner = QuietReferenceArrayLeaf(Field);
            if (!ArrayInner) return false;
            ArrayInnerFlags = ArrayInner->GetPropertyFlags();
            ArrayInnerName = ArrayInner->GetFName();
            ArrayInnerSize = ArrayInner->GetElementSize();
            ObjectClass = ArrayInner->PropertyClass;
            if (!ObjectClass.IsResolved()) return false;
            ObjectClassHandle = ObjectClass.GetHandle();
        }
        if (Class == FObjectProperty::StaticClass())
        {
            ObjectClass = CastFieldChecked<FObjectProperty>(Field)->PropertyClass;
            if (!ObjectClass.IsResolved()) return false;
            ObjectClassHandle = ObjectClass.GetHandle();
        }
        if (Class == FByteProperty::StaticClass())
        {
            Enum = CastFieldChecked<FByteProperty>(Field)->Enum;
            if (!Enum.IsResolved()) return false;
            EnumHandle = Enum.GetHandle();
        }
        if (Class == FBoolProperty::StaticClass())
        {
            const FBoolProperty* Bool = CastFieldChecked<FBoolProperty>(Field);
            BoolMask = Bool->GetFieldMask();
            BoolOffset = Bool->GetByteOffset();
            BoolByteMask = Bool->GetByteMask();
            BoolSize = Bool->GetBoolFieldSize();
        }
        return true;
    }

    bool MatchesMetadata(const FProperty* Field) const
    {
        if (Field != Property || !IsQuietPayloadField(Field) || Field->GetOwnerStruct() != Owner
            || Field->GetClass() != Class || Field->GetFName() != Name || Field->GetPropertyFlags() != Flags
            || Field->GetOffset_ForInternal() != Offset || Field->GetElementSize() != Size) return false;
        if (Class == FStrProperty::StaticClass()) return StringWitness.IsValid() && StringWitness->Matches();
        if (Class == FEnumProperty::StaticClass()) return EnumWitness.IsValid() && EnumWitness->Matches(Field);
        if (Class == FArrayProperty::StaticClass())
        {
            const FObjectProperty* Inner = QuietReferenceArrayLeaf(Field);
            return Inner == ArrayInner && Inner->GetFName() == ArrayInnerName
                && Inner->GetPropertyFlags() == ArrayInnerFlags && Inner->GetElementSize() == ArrayInnerSize
                && Inner->PropertyClass.IsResolved() && IsObjectHandleResolved(ObjectClassHandle)
                && Inner->PropertyClass.GetHandle() == ObjectClassHandle;
        }
        if (Class == FObjectProperty::StaticClass())
        {
            const auto& CurrentClass = CastFieldChecked<FObjectProperty>(Field)->PropertyClass;
            return CurrentClass.IsResolved() && IsObjectHandleResolved(ObjectClassHandle)
                && CurrentClass.GetHandle() == ObjectClassHandle;
        }
        if (Class == FByteProperty::StaticClass())
        {
            const auto& CurrentEnum = CastFieldChecked<FByteProperty>(Field)->Enum;
            if (!CurrentEnum.IsResolved() || !IsObjectHandleResolved(EnumHandle)
                || CurrentEnum.GetHandle() != EnumHandle) return false;
        }
        if (Class == FBoolProperty::StaticClass())
        {
            const FBoolProperty* Bool = CastFieldChecked<FBoolProperty>(Field);
            return Bool->GetFieldMask() == BoolMask && Bool->GetByteOffset() == BoolOffset
                && Bool->GetByteMask() == BoolByteMask && Bool->GetBoolFieldSize() == BoolSize;
        }
        return true;
    }

    bool CaptureValue(const void* Memory)
    {
        if (!MatchesMetadata(Property)) return false;
        const void* Address = Property->ContainerPtrToValuePtr<void>(Memory);
        if (Class == FArrayProperty::StaticClass())
        {
            const FArrayProperty* Array = CastFieldChecked<FArrayProperty>(Property);
            if (!IsQuietReferenceArrayStorage(Array, Address)) return false;
            FScriptArrayHelper Values(Array, Address);
            const int32 Count = Values.Num();
            if (Count < 0) return false;
            ArrayObjects.Reset(Count);
            ArrayHandles.Reset(Count);
            for (int32 Element = 0; Element < Count; ++Element)
            {
                if (!IsQuietReferenceArrayStorage(Array, Property->ContainerPtrToValuePtr<void>(Memory))) return false;
                FScriptArrayHelper Current(Array, Property->ContainerPtrToValuePtr<void>(Memory));
                if (Current.Num() != Count) return false;
                const auto& ObjectValue = FObjectProperty::GetPropertyValue(Current.GetRawPtr(Element));
                if (!ObjectValue.IsResolved()) return false;
                ArrayObjects.Add(ObjectValue);
                ArrayHandles.Add(ObjectValue.GetHandle());
            }
            return true;
        }
        if (Class == FObjectProperty::StaticClass())
        {
            Object = FObjectProperty::GetPropertyValue(Address);
            if (!Object.IsResolved()) return false;
            ObjectHandle = Object.GetHandle();
            return true;
        }
        if (Class == FStrProperty::StaticClass())
        {
            const TCHAR* Data = nullptr;
            int32 Count = 0;
            if (!ReadStringUnits(Address, Data, Count)) return false;
            StringUnits.Reset(Count);
            if (Count) StringUnits.Append(Data, Count);
            return true;
        }
        if (Class == FBoolProperty::StaticClass())
            Value[0] = static_cast<const uint8*>(Address)[BoolOffset] & BoolMask;
        else FMemory::Memcpy(Value, Address, Size);
        return true;
    }

    bool Capture(const FProperty* Field, const void* Memory, asITypeInfo* EnumCookie = nullptr,
        TSharedPtr<FScriptEnumReloadBinding> ScriptBinding = nullptr,
        TSharedPtr<const FNativeStringTypeWitness> InStringWitness = nullptr)
    {
        return CaptureMetadata(Field, EnumCookie, nullptr, ScriptBinding, InStringWitness) && CaptureValue(Memory);
    }

    bool Matches(const FProperty* Field, const void* Memory) const
    {
        if (!MatchesMetadata(Field)) return false;
        const void* Address = Field->ContainerPtrToValuePtr<void>(Memory);
        if (Class == FArrayProperty::StaticClass())
        {
            for (int32 Element = 0; Element < ArrayHandles.Num(); ++Element)
                if (!MatchesProjectedObject(Field, Memory, ArrayHandles[Element], Element)) return false;
            if (!IsQuietReferenceArrayStorage(Field, Field->ContainerPtrToValuePtr<void>(Memory))) return false;
            FScriptArrayHelper Current(CastFieldChecked<FArrayProperty>(Field), Field->ContainerPtrToValuePtr<void>(Memory));
            return Current.Num() == ArrayHandles.Num();
        }
        if (Class == FObjectProperty::StaticClass())
        {
            const auto& Current = FObjectProperty::GetPropertyValue(Address);
            // Both resolved handles compare without resolution, access tracking or property callbacks.
            return Current.IsResolved() && IsObjectHandleResolved(ObjectHandle) && Current.GetHandle() == ObjectHandle;
        }
        if (Class == FStrProperty::StaticClass())
        {
            const TCHAR* Data = nullptr;
            int32 Count = 0;
            return ReadStringUnits(Address, Data, Count) && Count == StringUnits.Num()
                && (!Count || FMemory::Memcmp(Data, StringUnits.GetData(), SIZE_T(Count) * sizeof(TCHAR)) == 0);
        }
        if (Class == FBoolProperty::StaticClass())
            return (static_cast<const uint8*>(Address)[BoolOffset] & BoolMask) == Value[0];
        return FMemory::Memcmp(Address, Value, Size) == 0;
    }

    const TCHAR* MismatchCategory(const FProperty* Field) const
    {
        if (!MatchesMetadata(Field))
        {
            if (Class == FObjectProperty::StaticClass() && Field->GetClass() == Class)
            {
                const auto& CurrentClass = CastFieldChecked<FObjectProperty>(Field)->PropertyClass;
                if (!CurrentClass.IsResolved() || CurrentClass.GetHandle() != ObjectClassHandle)
                    return TEXT("property-class");
            }
            return TEXT("metadata");
        }
        if (Class == FArrayProperty::StaticClass()) return TEXT("array-value");
        if (Class == FStrProperty::StaticClass()) return TEXT("string-value");
        return Class == FObjectProperty::StaticClass() ? TEXT("object-value") : TEXT("scalar-value");
    }

    bool MatchesProjectedObject(const FProperty* Field, const void* Memory, FObjectHandle ExpectedHandle,
        int32 Element = INDEX_NONE) const
    {
        if (!MatchesMetadata(Field) || !IsObjectHandleResolved(ExpectedHandle)) return false;
        const void* Address = Field->ContainerPtrToValuePtr<void>(Memory);
        if (Class == FArrayProperty::StaticClass())
        {
            if (!IsQuietReferenceArrayStorage(Field, Address)) return false;
            FScriptArrayHelper Current(CastFieldChecked<FArrayProperty>(Field), Address);
            if (Current.Num() != ArrayHandles.Num() || !ArrayHandles.IsValidIndex(Element)) return false;
            const auto& CurrentObject = FObjectProperty::GetPropertyValue(Current.GetRawPtr(Element));
            return CurrentObject.IsResolved() && CurrentObject.GetHandle() == ExpectedHandle;
        }
        if (Class != FObjectProperty::StaticClass() || Element != INDEX_NONE) return false;
        const auto& Current = FObjectProperty::GetPropertyValue(Address);
        return Current.IsResolved() && Current.GetHandle() == ExpectedHandle;
    }
};

struct FReferencePayloadField
{
    const FProperty* NewProperty = nullptr;
    FName Name;
    bool bObject = false;
    int32 ReferenceIndex = INDEX_NONE;
    TArray<int32> ArrayReferenceIndices;
    FPayloadFieldSnapshot Before;
};

struct FReferencePayload
{
    UObject* Owner = nullptr;
    FObjectKey OwnerKey;
    const FStructProperty* Property = nullptr;
    UScriptStruct* OldStruct = nullptr;
    UScriptStruct* NewStruct = nullptr;
    const void* OldMemory = nullptr;
    const void* ExpectedSchemaCookie = nullptr;
    TArray<uint8> Bytes;
    TArray<FPayloadReferenceIdentity> References;
    TArray<FReferencePayloadField> Fields;
    TArray<FPayloadFieldSnapshot> ExpectedFields;
    TSharedPtr<const FNativeStringTypeWitness> StringBinding;
    const void* CandidateMemory = nullptr;
    const void* PublishedMemory = nullptr;
    bool bCandidateReady = false;
    FInstancedStruct Candidate;
    FInstancedStruct Retirement;

    FInstancedStruct& Live() const
    {
        return *Property->ContainerPtrToValuePtr<FInstancedStruct>(Owner);
    }
};

class FPayloadIdentityWriter final : public FObjectWriter
{
public:
    FPayloadIdentityWriter(TArray<uint8>& Bytes, TFunction<bool(UObject*, int32&)> InCapture)
        : FObjectWriter(Bytes), Capture(MoveTemp(InCapture))
    {
        ArNoDelta = true;
        SetPortFlags(PPF_None);
    }

    virtual FArchive& operator<<(UObject*& Object) override
    {
        int32 Index = INDEX_NONE;
        if (Object && !Capture(Object, Index)) SetError();
        Serialize(&Index, sizeof(Index));
        return *this;
    }

    virtual FArchive& operator<<(FObjectPtr& Object) override
    {
        UObject* Resolved = Object.Get();
        return operator<<(Resolved);
    }

private:
    TFunction<bool(UObject*, int32&)> Capture;
};

class FPayloadIdentityReader final : public FObjectReader
{
public:
    FPayloadIdentityReader(const TArray<uint8>& Bytes, TArray<FPayloadReferenceIdentity>& InReferences)
        : FObjectReader(Bytes), References(InReferences) {}

    virtual FArchive& operator<<(UObject*& Object) override
    {
        int32 Index = INDEX_NONE;
        Serialize(&Index, sizeof(Index));
        Object = nullptr;
        if (Index == INDEX_NONE) return *this;
        if (!References.IsValidIndex(Index) || !References[Index].Resolved)
        {
            SetError();
            return *this;
        }
        References[Index].bRead = true;
        Object = References[Index].Resolved;
        return *this;
    }

    virtual FArchive& operator<<(FObjectPtr& Object) override
    {
        UObject* Resolved = nullptr;
        operator<<(Resolved);
        Object = Resolved;
        return *this;
    }

private:
    TArray<FPayloadReferenceIdentity>& References;
};
}

class FInstancedStructReloadScope::FImpl final : public FGCObject, public AngelscriptReloadSafety::IPreparedPayloadReloadLease
{
public:
    FImpl() = default;
    FImpl(const TMap<UScriptStruct*, UScriptStruct*>& InReplacements,
        const TMap<UClass*, UClass*>& ReloadClasses,
        const TArray<AngelscriptRuntimeGcMirror::FRuntimeScriptReplacementWitness>& InWitnesses)
        : Replacements(InReplacements), Witnesses(InWitnesses)
    {
        for (const TPair<UClass*, UClass*>& Pair : ReloadClasses)
        {
            ReplacementClasses.Add(Pair.Key);
            ReplacementClasses.Add(Pair.Value);
        }
    }

    ~FImpl()
    {
        DetachCopyMaps();
        if (Listener.IsValid()) FCoreUObjectDelegates::OnObjectsReinstanced.Remove(Listener);
        if (bOwnScriptEnumHandler)
        {
            auto& Handler = AngelscriptReloadSafety::GetPreparedScriptEnumReloadTransition();
            auto& Reader = AngelscriptReloadSafety::GetPreparedScriptEnumReloadWitnessReader();
            if (Handler.IsBoundToObject(this)) Handler.Unbind();
            if (Reader.IsBoundToObject(this)) Reader.Unbind();
        }
    }

    virtual FString GetReferencerName() const override { return TEXT("AngelscriptInstancedStructReload"); }

    virtual void AddReferencedObjects(FReferenceCollector& Collector) override
    {
        for (auto& Binding : ScriptEnumBindings) Collector.AddReferencedObject(Binding->Root);
        for (UScriptStruct*& Schema : DomainSchemas) Collector.AddReferencedObject(Schema);
        for (FReferencePayload& Item : Payloads)
        {
            Collector.AddReferencedObject(Item.Owner);
            Collector.AddReferencedObject(Item.OldStruct);
            Collector.AddReferencedObject(Item.NewStruct);
            for (FPayloadReferenceIdentity& Reference : Item.References)
            {
                Collector.AddReferencedObject(Reference.Stable);
                Collector.AddReferencedObject(Reference.Resolved);
                Collector.AddReferencedObject(Reference.Captured);
            }
            if (Item.Candidate.IsValid())
                Collector.AddPropertyReferencesWithStructARO(FInstancedStruct::StaticStruct(), &Item.Candidate);
            for (FPayloadFieldSnapshot& Field : Item.ExpectedFields)
            {
                Collector.AddReferencedObject(Field.Object);
                for (auto& Object : Field.ArrayObjects) Collector.AddReferencedObject(Object);
                if (Field.EnumWitness.IsValid()) Collector.AddReferencedObject(Field.EnumWitness->Root);
            }
            for (FReferencePayloadField& Field : Item.Fields)
                if (Field.Before.EnumWitness.IsValid()) Collector.AddReferencedObject(Field.Before.EnumWitness->Root);
        }
        for (TPair<const void*, UObject*>& Value : LiveMapValues)
            Collector.AddReferencedObject(Value.Value);
    }

    bool Prepare(FString& Error)
    {
        if (!IsInGameThread()) return Reject(Error, TEXT("Reference payload capture requires the game thread"));
        for (const TPair<UScriptStruct*, UScriptStruct*>& Pair : Replacements)
            if (Pair.Key && (bBeforeSwap || HasSchemaReferences(Pair.Key) || HasSchemaReferences(Pair.Value)))
            {
                ReferenceReplacements.Add(Pair.Key, Pair.Value);
                DomainSchemas.Add(Pair.Key);
            }
        if (!VisitAffectedPayloads([&](UObject* Owner, const FStructProperty* Property,
            const FInstancedStruct& Payload, UScriptStruct* Old, UScriptStruct* Target)
        {
            UASStruct* OldScript = Cast<UASStruct>(Old);
            UASStruct* NewScript = Cast<UASStruct>(Target);
            if (!OldScript || !NewScript || (!bBeforeSwap && (OldScript == NewScript || OldScript->NewerVersion != NewScript
                || NewScript->ResolveNewestStruct() != NewScript))
                || !IsReferencePayloadSchema(Old) || (!bBeforeSwap && !IsReferencePayloadSchema(NewScript)))
                return Reject(Error, TEXT("Reference payload replacement schema or identity is unsupported"));
            for (const FReferencePayload& Existing : Payloads)
                if (&Existing.Live() == &Payload)
                    return Reject(Error, TEXT("Captured payload domain aliases a slot"));
            FReferencePayload& Item = Payloads.AddDefaulted_GetRef();
            Item.Owner = Owner;
            Item.OwnerKey = FObjectKey(Owner);
            Item.Property = Property;
            Item.OldStruct = Old;
            Item.NewStruct = bBeforeSwap ? nullptr : NewScript;
            Item.OldMemory = Payload.GetMemory();
            Item.ExpectedSchemaCookie = ExpectedSchemaCookies.FindRef(Old);
            AddStableDomain(Owner);
            for (TFieldIterator<FProperty> Field(Old); Field; ++Field)
            {
                const FProperty* NewField = FindFProperty<FProperty>(NewScript, Field->GetFName());
                if (!NewField || Field->GetClass() != NewField->GetClass() || NewField->ArrayDim != 1)
                    return Reject(Error, TEXT("Reference payload reload must retain each old field type"));
                FReferencePayloadField& SavedField = Item.Fields.AddDefaulted_GetRef();
                SavedField.NewProperty = NewField;
                SavedField.Name = Field->GetFName();
                SavedField.bObject = IsStrongObjectProperty(*Field);
                asITypeInfo* EnumCookie = nullptr;
                TSharedPtr<FScriptEnumReloadBinding> ScriptBinding;
                if (Field->GetClass() == FEnumProperty::StaticClass())
                {
                    const auto Before = ActiveStructDescriptor(OldScript);
                    const auto After = SchemaCandidates.FindRef(Old);
                    if (!Before.IsValid() || !After.IsValid())
                        return Reject(Error, TEXT("Native enum field has no complete compiled descriptor"));
                    if (IsQuietScriptEnumShape(*Field))
                    {
                        asITypeInfo* Next = DeclaredScriptEnumCookie(*After, *Field, false);
                        // Existing class type usages may already point at next types. FEnumData owns the old cookie.
                        ScriptBinding = FindScriptEnumBinding(nullptr, Next, Error);
                        if (!ScriptBinding.IsValid()) return false;
                        EnumCookie = ScriptBinding->Plan.PreviousCookie;
                    }
                    else EnumCookie = DeclaredNativeEnumCookie(*Before, *Field, true);
                    if (!ScriptBinding.IsValid() && (!EnumCookie || EnumCookie != DeclaredNativeEnumCookie(*After, *Field, false)))
                        return Reject(Error, TEXT("Enum binding/definition changes need a separate prepared plan"));
                }
                if (Field->GetClass() == FStrProperty::StaticClass())
                {
                    const auto Before = ActiveStructDescriptor(OldScript);
                    const auto After = SchemaCandidates.FindRef(Old);
                    asITypeInfo* Cookie = Before.IsValid() ? DeclaredStringCookie(*Before, *Field, true) : nullptr;
                    if (!Cookie || !After.IsValid() || DeclaredStringCookie(*After, *Field, false) != Cookie)
                        return Reject(Error, TEXT("Native string field has no complete compiled binding"));
                    if (!Item.StringBinding.IsValid())
                    {
                        auto Binding = MakeShared<FNativeStringTypeWitness>();
                        if (!Binding->Capture(Cookie))
                            return Reject(Error, TEXT("Native string binding capture failed"));
                        Item.StringBinding = Binding;
                    }
                    if (!Item.StringBinding->Matches() || Item.StringBinding->Cookie != Cookie)
                        return Reject(Error, TEXT("Native string binding changed during capture"));
                }
                if (!SavedField.Before.Capture(*Field, Payload.GetMemory(), EnumCookie, ScriptBinding,
                    Field->GetClass() == FStrProperty::StaticClass() ? Item.StringBinding : nullptr))
                    return Reject(Error, TEXT("Reference payload field has no quiet value snapshot"));
                if (ScriptBinding.IsValid())
                {
                    int8 Value = 0;
                    FMemory::Memcpy(&Value, SavedField.Before.Value, sizeof(Value));
                    if (!ScriptBinding->Symbols.ContainsByPredicate([Value](const auto& Symbol) { return Symbol.Value == Value; }))
                        return Reject(Error, TEXT("Script enum payload value has no fixed serialized symbol"));
                }
                if (SavedField.bObject)
                {
                    UObject* Object = SavedField.Before.Object.Get();
                    if (Object && !CaptureReference(Item, Object, SavedField.ReferenceIndex, Error)) return false;
                }
                else if (SavedField.Before.Class == FArrayProperty::StaticClass())
                {
                    for (int32 Element = 0; Element < SavedField.Before.ArrayObjects.Num(); ++Element)
                    {
                        int32 ReferenceIndex = INDEX_NONE;
                        UObject* Object = SavedField.Before.ArrayObjects[Element].Get();
                        if (Object && !CaptureReference(Item, Object, ReferenceIndex, Error)) return false;
                        SavedField.ArrayReferenceIndices.Add(ReferenceIndex);
                    }
                }
            }
            const auto CandidateDesc = SchemaCandidates.FindRef(Old);
            if (!CandidateDesc.IsValid()) return Reject(Error, TEXT("Reference payload has no candidate descriptor"));
            for (const auto& CandidateField : CandidateDesc->Properties)
            {
                int32 Id = 0;
                if (CandidateField->ScriptPropertyIndex < 0
                    || CandidateDesc->ScriptType->GetProperty(CandidateField->ScriptPropertyIndex, nullptr, &Id) < 0)
                    return Reject(Error, TEXT("Native enum default has no compiled field descriptor"));
                const asITypeInfo* Cookie = CandidateDesc->ScriptType->GetEngine()->GetTypeInfoById(Id);
                if (asITypeInfo* StringCookie = NativeStringCookie(CandidateField->PropertyType))
                {
                    if (Cookie != StringCookie)
                        return Reject(Error, TEXT("Native string default has a foreign field type"));
                    if (!Item.StringBinding.IsValid())
                    {
                        auto Binding = MakeShared<FNativeStringTypeWitness>();
                        if (!Binding->Capture(StringCookie))
                            return Reject(Error, TEXT("Native string default binding capture failed"));
                        Item.StringBinding = Binding;
                    }
                    if (!Item.StringBinding->Matches() || Item.StringBinding->Cookie != StringCookie)
                        return Reject(Error, TEXT("Native string default binding changed before swap"));
                }
                if (!Cookie || (Cookie->GetFlags() & asOBJ_ENUM) == 0) continue;
                if (Cookie->GetModule())
                {
                    if (!FindScriptEnumBinding(nullptr, const_cast<asITypeInfo*>(Cookie), Error).IsValid()) return false;
                    continue;
                }
                bool bFoundNative = false;
                for (TObjectIterator<UEnum> Enum; Enum; ++Enum)
                    if (static_cast<const void*>(*Enum) == Cookie->GetUserData()
                        && Enum->GetClass() == UEnum::StaticClass())
                    {
                        bFoundNative = true;
                        if (Enum->GetCppForm() != UEnum::ECppForm::EnumClass) break;
                        const bool bSealed = Item.Fields.ContainsByPredicate([&](const FReferencePayloadField& Saved)
                        {
                            return Saved.Before.EnumWitness.IsValid() && Saved.Before.EnumWitness->Cookie == Cookie
                                && Saved.Before.EnumWitness->EnumMatches();
                        });
                        if (!bSealed) return Reject(Error, TEXT("New enum defaults require a pre-swap sealed native binding"));
                        break;
                    }
                if (!bFoundNative) return Reject(Error, TEXT("Native enum default has no exact UEnum binding"));
            }
            FPayloadIdentityWriter Save(Item.Bytes, [&](UObject* Object, int32& Index)
            {
                return CaptureReference(Item, Object, Index, Error);
            });
            Old->SerializeTaggedProperties(Save, const_cast<uint8*>(Payload.GetMemory()), nullptr, nullptr);
            if (Save.IsError() || Item.Bytes.IsEmpty())
                return Reject(Error, TEXT("Reference payload identity-tagged capture failed"));
            return true;
        }, Error)) return false;
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope || !Scope->Epoch || CopyState.IsValid()) return Reject(Error, TEXT("Copy map consumer has no unique reload owner"));
        CopyScope = Scope;
        CopyEpoch = Scope->Epoch;
        CopyState = MakeShared<FCopyState, ESPMode::ThreadSafe>();
        const auto State = CopyState;
        CopyListener = FCoreUObjectDelegates::OnObjectsReplaced.AddLambda([this, State](const TMap<UObject*, UObject*>& Map)
        {
            if (!IsInGameThread()) { State->Foreign.store(true); return; }
            CaptureCopyMap(Map);
        });
        Listener = FCoreUObjectDelegates::OnObjectsReinstanced.AddLambda([this, State](const TMap<UObject*, UObject*>& Map)
        {
            if (!IsInGameThread()) { State->Foreign.store(true); return; }
            ConsumeAndObserveMap(Map);
        });
        return ValidateHolders(Error);
    }

    bool CaptureBeforeSwap(const TArray<FAngelscriptPayloadReloadType>& Types, FString& Error)
    {
        bBeforeSwap = true;
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope) return Reject(Error, TEXT("Payload capture has no pre-swap owner"));
        for (const auto& Plan : Scope->Plans)
            if (Plan.Kind == AngelscriptRuntimeGcMirror::EScriptObjectReloadPlan::RequiresReplacement)
                ReplacementClasses.Add(Plan.Class);
        FPayloadAdmission Admission(Error);
        for (const auto& Type : Types)
            if (Type.bWillReplace && Admission.ReferenceRoute(Type.Previous, *Type.Candidate))
            {
                Replacements.Add(Type.Previous, Type.Previous);
                ExpectedSchemaCookies.Add(Type.Previous, Type.Candidate->ScriptType);
                SchemaCandidates.Add(Type.Previous, Type.Candidate);
            }
        if (!Prepare(Error)) return false;
        if (!ScriptEnumBindings.IsEmpty())
        {
            auto& Handler = AngelscriptReloadSafety::GetPreparedScriptEnumReloadTransition();
            auto& Reader = AngelscriptReloadSafety::GetPreparedScriptEnumReloadWitnessReader();
            if (Handler.IsBound() || Reader.IsBound()) return Reject(Error, TEXT("Script enum transition already has an owner"));
            Handler.BindRaw(this, &FImpl::ObserveScriptEnumTransition);
            Reader.BindRaw(this, &FImpl::ReadScriptEnumWitness);
            bOwnScriptEnumHandler = true;
        }
        return true;
    }

    bool SealScriptEnumWitnessProvider(FString& Error)
    {
        return !bOwnScriptEnumHandler
            || AngelscriptReloadSafety::SealPreparedScriptEnumReloadWitnessProvider(this, this, Error);
    }

    bool Attach(const TMap<UScriptStruct*, UScriptStruct*>& InStructs, const TMap<UClass*, UClass*>& InClasses,
        const TArray<AngelscriptRuntimeGcMirror::FRuntimeScriptReplacementWitness>& InWitnesses, FString& Error) override
    {
        if (bAttaching || bAttached || bEarlyMap) return Reject(Error, TEXT("Payload lease was reused or received an early replacement map"));
        TGuardValue<bool> AttachGuard(bAttaching, true);
        if (!bBeforeSwap)
        {
            // A newly added type has no old payload instances to capture or migrate.
            for (const auto& Pair : InStructs)
                if (Pair.Key && (HasSchemaReferences(Pair.Key) || HasSchemaReferences(Pair.Value)))
                    return Reject(Error, TEXT("Reference payload has no pre-swap captured lease"));
            bAttached = true;
            return Prepare(Error);
        }
        if (!ValidateHolders(Error)) return false;
        for (const auto& Binding : ScriptEnumBindings)
            if (!Binding->Complete()) return Reject(Error, TEXT("Script enum binding did not consume its planned operation"));
        Replacements = InStructs;
        Witnesses = InWitnesses;
        for (FReferencePayload& Item : Payloads)
        {
            UASStruct* Old = Cast<UASStruct>(Item.OldStruct);
            UASStruct* New = Cast<UASStruct>(InStructs.FindRef(Item.OldStruct));
            if (!Old || !New || Old->NewerVersion != New || New->ResolveNewestStruct() != New
                || New->ScriptType != Item.ExpectedSchemaCookie || !IsReferencePayloadSchema(New))
                return Reject(Error, TEXT("Prepared payload target differs from the admitted schema"));
            Item.NewStruct = New;
            const auto CandidateDesc = SchemaCandidates.FindRef(Item.OldStruct);
            for (TFieldIterator<FProperty> Field(New); Field; ++Field)
            {
                TSharedPtr<FScriptEnumReloadBinding> ScriptBinding;
                asITypeInfo* EnumCookie = nullptr;
                if (Field->GetClass() == FEnumProperty::StaticClass() && CandidateDesc.IsValid())
                {
                    EnumCookie = IsQuietScriptEnumShape(*Field) ? DeclaredScriptEnumCookie(*CandidateDesc, *Field, true)
                        : DeclaredNativeEnumCookie(*CandidateDesc, *Field, true);
                    if (IsQuietScriptEnumShape(*Field))
                    {
                        ScriptBinding = FindScriptEnumBinding(nullptr, EnumCookie, Error);
                        if (!ScriptBinding.IsValid() || !ScriptBinding->Complete()) return false;
                    }
                }
                const FReferencePayloadField* Sealed = EnumCookie ? Item.Fields.FindByPredicate([&](const auto& Saved)
                {
                    return Saved.Before.EnumWitness.IsValid() && Saved.Before.EnumWitness->Cookie == EnumCookie;
                }) : nullptr;
                if (EnumCookie && !Sealed && !ScriptBinding.IsValid())
                    return Reject(Error, TEXT("Prepared enum field has no pre-swap binding witness"));
                if (Field->GetClass() == FStrProperty::StaticClass()
                    && (!CandidateDesc.IsValid() || !Item.StringBinding.IsValid() || !Item.StringBinding->Matches()
                        || DeclaredStringCookie(*CandidateDesc, *Field, true) != Item.StringBinding->Cookie))
                    return Reject(Error, TEXT("Prepared string field has no pre-swap canonical binding"));
                if (!Item.ExpectedFields.AddDefaulted_GetRef().CaptureMetadata(
                    *Field, EnumCookie, Sealed ? Sealed->Before.EnumWitness.Get() : nullptr, ScriptBinding,
                    Field->GetClass() == FStrProperty::StaticClass() ? Item.StringBinding : nullptr))
                    return Reject(Error, TEXT("Prepared payload field has no quiet metadata snapshot"));
            }
            for (FReferencePayloadField& Field : Item.Fields)
            {
                const FProperty* Property = FindFProperty<FProperty>(New, Field.Name);
                if (!Property || Property->ArrayDim != 1 || Property->GetClass() != Field.NewProperty->GetClass()
                    || Property->GetElementSize() != Field.Before.Size)
                    return Reject(Error, TEXT("Prepared payload target changed an old field type"));
                if (Field.Before.Class == FArrayProperty::StaticClass() && !QuietReferenceArrayLeaf(Property))
                    return Reject(Error, TEXT("Prepared payload target changed a strong object array leaf"));
                Field.NewProperty = Property;
            }
            for (FPayloadReferenceIdentity& Reference : Item.References)
            {
                if (Reference.bLiveAS && InClasses.Contains(Reference.Stable->GetClass()))
                    return Reject(Error, TEXT("Admitted live AS class entered the native replacement domain"));
                if (!Reference.bRequiresReplacement) continue;
                if (!AngelscriptRuntimeGcMirror::ValidatePreparedScriptReferenceWitness(
                    Reference.Captured, Reference.LiveWitness, Error)) return false;
                for (int32 Index = 0; Index < Witnesses.Num(); ++Index)
                    if (Witnesses[Index].OldIdentity == Reference.OldIdentity && Witnesses[Index].OldKey == Reference.OldKey)
                    {
                        if (Reference.WitnessIndex != INDEX_NONE || !Witnesses[Index].CaptureGeneration)
                            return Reject(Error, TEXT("Conflicting prepared replacement capture identity"));
                        Reference.WitnessIndex = Index;
                    }
                if (Reference.WitnessIndex == INDEX_NONE)
                    return Reject(Error, TEXT("Prepared required seed is absent from the replacement ledger"));
                Reference.Captured = nullptr;
            }
        }
        bAttached = true;
        if (!ValidateHolders(Error) || !ValidateLiveReferences(Error)) return false;
        for (FReferencePayload& Item : Payloads)
            for (FPayloadReferenceIdentity& Reference : Item.References)
            {
                if (!Reference.bLiveAS) continue;
                AngelscriptRuntimeGcMirror::FLiveScriptReferenceWitness Current;
                if (!AngelscriptRuntimeGcMirror::ValidateLiveScriptReferenceWitness(
                    Reference.Stable, Reference.LiveWitness, Error, &Current)) return false;
                const auto& Before = Reference.LiveWitness;
                UE_LOG(Angelscript, Display,
                    TEXT("SCRIPT_PAYLOAD_LIVE_AS_WITNESS epoch=%llu plan=%s track=%llu before_revision=%llu claim_revision=%llu ")
                    TEXT("commit_revision=%llu before_count=%llu claim_count=%llu same_object=1 class_key=1 cookie_current=1 ")
                    TEXT("old_cookie=%llu new_cookie=%llu"),
                    Before.Epoch, Before.Kind == AngelscriptRuntimeGcMirror::EScriptObjectReloadPlan::Unchanged
                        ? TEXT("unchanged") : TEXT("soft-reconstruction"), Before.TrackSequence, Before.Revision,
                    Current.Revision, Current.CommittedRevision, Before.ClaimCount, Current.ClaimCount,
                    uint64(reinterpret_cast<UPTRINT>(Before.PreviousCookie)), uint64(reinterpret_cast<UPTRINT>(Before.ExpectedCookie)));
            }
        return ValidateAfterCallout(Error);
    }

    bool ValidateCompletion(FString& Error) override
    {
        if (bPublishing) return Reject(Error, TEXT("Payload publication has not returned from its callout"));
        if (ScriptEnumBindings.IsEmpty())
            return (bAttached && bPublished && !bEarlyMap && ValidateAfterCallout(Error) && CompleteCopyMaps(Error))
                || Reject(Error, TEXT("Prepared payload lease did not complete its final validation"));
        if (bScriptEnumCompletion) return Reject(Error, TEXT("Reentrant script enum completion"));
        TGuardValue<bool> Guard(bScriptEnumCompletion, true);
        if (!bAttached || !bPublished || bEarlyMap || !ValidateAfterCallout(Error)) return false;
        for (const auto& Binding : ScriptEnumBindings)
        {
            Binding->ReleasePreviousPin();
            if (!ValidateAfterCallout(Error)) return false;
        }
        return ValidateAfterCallout(Error) && CompleteCopyMaps(Error);
    }

    bool ValidateBeforeSwap(FString& Error) override
    {
        if (!bBeforeSwap || bAttached || bEarlyMap)
            return Reject(Error, TEXT("Prepared payload changed before module swap"));
        if (!ValidateHolders(Error)) return false;
        for (FReferencePayload& Item : Payloads)
            for (FPayloadReferenceIdentity& Reference : Item.References)
                if ((Reference.bLiveAS || Reference.bRequiresReplacement)
                    && !AngelscriptRuntimeGcMirror::ValidatePreparedScriptReferenceWitness(
                        Reference.Captured, Reference.LiveWitness, Error)) return false;
        return true;
    }

    bool Publish(FString& Error) override
    {
        if (bPublishing) return Reject(Error, TEXT("Reentrant reference payload publication"));
        TGuardValue<bool> PublishGuard(bPublishing, true);
        if (!IsInGameThread() || !bAttached || bPublished)
            return Reject(Error, TEXT("Invalid reference payload publication state"));
        if (!ValidateAfterCallout(Error)) return false;
        for (FReferencePayload& Item : Payloads)
        {
            for (FPayloadReferenceIdentity& Reference : Item.References)
                if (!ResolveReference(Reference, Error)) return false;
            for (const FReferencePayloadField& Field : Item.Fields)
            {
                const FObjectProperty* Leaf = Field.bObject ? CastFieldChecked<FObjectProperty>(Field.NewProperty)
                    : QuietReferenceArrayLeaf(Field.NewProperty);
                if (!Leaf) continue;
                auto Compatible = [&](int32 Index)
                {
                    if (Index == INDEX_NONE) return true;
                    return Item.References.IsValidIndex(Index) && Item.References[Index].Resolved
                        && Item.References[Index].Resolved->IsA(Leaf->PropertyClass);
                };
                if (Field.bObject && !Compatible(Field.ReferenceIndex))
                    return Reject(Error, TEXT("Reference payload candidate class is incompatible with the resolved identity"));
                for (int32 Index : Field.ArrayReferenceIndices)
                    if (!Compatible(Index))
                        return Reject(Error, TEXT("Reference payload array leaf class is incompatible with the resolved identity"));
            }
            Item.Candidate.InitializeAs(Item.NewStruct);
            if (!Item.Candidate.IsValid() || Item.Candidate.GetScriptStruct() != Item.NewStruct
                || !Item.Candidate.GetMutableMemory())
                return Reject(Error, TEXT("Reference payload candidate default construction failed"));
            Item.CandidateMemory = Item.Candidate.GetMemory();
            if (!ValidateAfterCallout(Error)) return false;
            // Fix all new defaults before tagged replay can invoke any engine callbacks.
            for (FPayloadFieldSnapshot& Field : Item.ExpectedFields)
                if (!Field.CaptureValue(Item.Candidate.GetMemory()))
                    return Reject(Error, TEXT("Reference payload default has no quiet value snapshot"));
            for (const FReferencePayloadField& Saved : Item.Fields)
            {
                FPayloadFieldSnapshot* Expected = Item.ExpectedFields.FindByPredicate(
                    [&](const auto& Field) { return Field.Property == Saved.NewProperty; });
                if (!Expected) return Reject(Error, TEXT("Reference payload retained field is missing"));
                if (Saved.bObject)
                {
                    Expected->Object = Saved.ReferenceIndex == INDEX_NONE ? nullptr : Item.References[Saved.ReferenceIndex].Resolved;
                    Expected->ObjectHandle = Expected->Object.GetHandle();
                }
                else if (Saved.Before.Class == FArrayProperty::StaticClass())
                {
                    Expected->ArrayObjects.Reset(Saved.ArrayReferenceIndices.Num());
                    Expected->ArrayHandles.Reset(Saved.ArrayReferenceIndices.Num());
                    for (int32 Index : Saved.ArrayReferenceIndices)
                    {
                        TObjectPtr<UObject> Object = Index == INDEX_NONE ? nullptr : Item.References[Index].Resolved;
                        Expected->ArrayObjects.Add(Object);
                        Expected->ArrayHandles.Add(Object.GetHandle());
                    }
                }
                else if (Saved.Before.Class == FStrProperty::StaticClass())
                {
                    if (Expected->StringWitness != Saved.Before.StringWitness || !Expected->StringWitness.IsValid()
                        || !Expected->StringWitness->Matches())
                        return Reject(Error, TEXT("Retained string field changed its canonical binding"));
                    Expected->StringUnits = Saved.Before.StringUnits;
                }
                else FMemory::Memcpy(Expected->Value, Saved.Before.Value, sizeof(Expected->Value));
            }
            if (!ValidateAfterCallout(Error)) return false;
            FPayloadIdentityReader Load(Item.Bytes, Item.References);
            for (const FReferencePayloadField& Field : Item.Fields)
                if (!Field.NewProperty->ShouldSerializeValue(Load))
                    return Reject(Error, TEXT("Reference payload candidate skips an old serialized field"));
            Item.NewStruct->SerializeTaggedProperties(Load, Item.Candidate.GetMutableMemory(), nullptr, nullptr);
            Item.bCandidateReady = true;
            if (!ValidateAfterCallout(Error)) return false;
            if (Load.IsError() || Load.Tell() != Item.Bytes.Num())
                return Reject(Error, TEXT("Reference payload identity-tagged replay failed"));
            for (const FPayloadReferenceIdentity& Reference : Item.References)
                if (!Reference.bRead) return Reject(Error, TEXT("Reference payload replay omitted an old reference"));
            for (TFieldIterator<FProperty> Field(Item.NewStruct); Field; ++Field)
            {
                const FObjectProperty* Leaf = Field->GetClass() == FObjectProperty::StaticClass()
                    ? CastFieldChecked<FObjectProperty>(*Field) : QuietReferenceArrayLeaf(*Field);
                if (!Leaf) continue;
                auto ValidateLeaf = [&](UObject* Object, int32 Element)
                {
                    if (Object && (!IsValid(Object) || !Object->IsA(Leaf->PropertyClass)))
                        return Reject(Error, TEXT("Reference payload candidate field identity is incompatible"));
                    if (Object && !IsRetainedCandidateReference(Item, *Field, Object, Element))
                    {
                        if (!IsStableNativeObject(Object, ReplacementClasses))
                        {
                            Error = FString::Printf(
                                TEXT("Reference payload default has no stable or consumed replacement identity field=%s element=%d"),
                                *Field->GetFName().ToString(), Element);
                            return false;
                        }
                        AddStableDomain(Object);
                    }
                    return ValidateAfterCallout(Error);
                };
                if (Field->GetClass() == FArrayProperty::StaticClass())
                {
                    const FArrayProperty* Array = CastFieldChecked<FArrayProperty>(*Field);
                    if (!IsQuietReferenceArrayStorage(Array, Field->ContainerPtrToValuePtr<void>(Item.Candidate.GetMemory())))
                        return Reject(Error, TEXT("Candidate reference array has invalid storage"));
                    FScriptArrayHelper Values(Array, Field->ContainerPtrToValuePtr<void>(Item.Candidate.GetMemory()));
                    const int32 Count = Values.Num();
                    if (Count < 0) return Reject(Error, TEXT("Candidate reference array has invalid storage"));
                    for (int32 Element = 0; Element < Count; ++Element)
                    {
                        if (!IsQuietReferenceArrayStorage(Array, Field->ContainerPtrToValuePtr<void>(Item.Candidate.GetMemory())))
                            return Reject(Error, TEXT("Candidate reference array storage changed during validation"));
                        FScriptArrayHelper Current(Array, Field->ContainerPtrToValuePtr<void>(Item.Candidate.GetMemory()));
                        if (Current.Num() != Count) return Reject(Error, TEXT("Candidate reference array length changed during validation"));
                        if (!ValidateLeaf(Leaf->GetObjectPropertyValue(Current.GetRawPtr(Element)), Element)) return false;
                    }
                }
                else if (!ValidateLeaf(Leaf->GetObjectPropertyValue_InContainer(Item.Candidate.GetMemory()), INDEX_NONE)) return false;
            }
        }
        if (!ValidateAfterCallout(Error)) return false;
        for (FReferencePayload& Item : Payloads)
            for (FPayloadReferenceIdentity& Reference : Item.References)
            {
                UObject* PreparedIdentity = Reference.Resolved;
                if (!ResolveReference(Reference, Error) || Reference.Resolved != PreparedIdentity)
                    return Reject(Error, TEXT("Reference payload prepared identity changed before publication"));
            }
        if (!ValidateAfterCallout(Error)) return false;
        // All allocation, constructors, tagged replay and identity checks precede these empty-slot moves.
        // Game-thread readers observe the batch after every live slot has received its candidate.
        for (FReferencePayload& Item : Payloads)
        {
            Item.PublishedMemory = Item.CandidateMemory;
            Item.Retirement = MoveTemp(Item.Live());
            Item.Live() = MoveTemp(Item.Candidate);
            Item.bCandidateReady = false;
        }
        bPublished = true;
        for (FReferencePayload& Item : Payloads)
        {
            Item.Retirement.Reset();
            if (!ValidateAfterCallout(Error)) return false;
        }
        return ValidateAfterCallout(Error);
    }

private:
    TMap<UScriptStruct*, UScriptStruct*> Replacements;
    TMap<UScriptStruct*, UScriptStruct*> ReferenceReplacements;
    TArray<UScriptStruct*> DomainSchemas;
    TMap<UScriptStruct*, const void*> ExpectedSchemaCookies;
    TMap<UScriptStruct*, TSharedPtr<FAngelscriptClassDesc>> SchemaCandidates;
    TArray<TSharedPtr<FScriptEnumReloadBinding>> ScriptEnumBindings;
    bool bOwnScriptEnumHandler = false;
    bool bScriptEnumCallback = false;
    bool bScriptEnumCompletion = false;
    TArray<AngelscriptRuntimeGcMirror::FRuntimeScriptReplacementWitness> Witnesses;
    TSet<const UClass*> ReplacementClasses;
    TSet<const void*> StableDomain;
    TArray<FReferencePayload> Payloads;
    TMap<const void*, const void*> Edges;
    TMap<const void*, UObject*> LiveMapValues;
    TMap<const void*, FPayloadMappedIdentity> MapValueIdentities;
    TMap<FObjectKey, AngelscriptRuntimeGcMirror::FReinstancedScriptPairReplayToken> TokensByNewKey;
    FDelegateHandle Listener;
    bool bPublished = false;
    bool bBeforeSwap = false;
    bool bAttached = false;
    bool bAttaching = false;
    bool bPublishing = false;
    bool bEarlyMap = false;
    const TCHAR* ValidationPhase = TEXT("lease");
    const TMap<UObject*, UObject*>* PendingProjectionMap = nullptr;
    bool bMapCallbackInProgress = false;

    bool Reject(FString& Error, const TCHAR* Reason) const { Error = Reason; return false; }


    TSharedPtr<FScriptEnumReloadBinding> FindScriptEnumBinding(asITypeInfo* Previous, asITypeInfo* Next, FString& Error)
    {
        for (const auto& Binding : ScriptEnumBindings)
            if (Binding->Plan.CandidateCookie == Next)
            {
                if ((Previous && Binding->Plan.PreviousCookie != Previous) || !Binding->Matches())
                { Reject(Error, TEXT("Script enum pair or generation changed")); return nullptr; }
                return Binding;
            }
        // A post-swap field may not silently capture a new enum plan or definition.
        const auto* Input = bBeforeSwap && !bAttached ? FindScriptEnumInput(Next) : nullptr;
        if (!Input || (Previous && Input->PreviousCookie != Previous))
        { Reject(Error, TEXT("Script enum field has no fixed ordinary binding plan")); return nullptr; }
        auto Binding = MakeShared<FScriptEnumReloadBinding>();
        if (!Binding->Capture(*Input))
        { Reject(Error, TEXT("Script enum binding is not current, fixed or ordinary")); return nullptr; }
        if (ScriptEnumBindings.ContainsByPredicate([&](const auto& Existing) { return Existing->Identity == Binding->Identity; }))
        { Reject(Error, TEXT("Distinct script enum bindings alias one physical enum")); return nullptr; }
        ScriptEnumBindings.Add(Binding);
        StableDomain.Add(Binding->Identity);
        return Binding;
    }

    bool ObserveScriptEnumTransition(const AngelscriptReloadSafety::FPreparedScriptEnumReloadTransition& Event, FString& Error)
    {
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope || Scope->Epoch != Event.Epoch || bScriptEnumCallback || bAttached || bPublished
            || (Event.Phase != AngelscriptReloadSafety::EPreparedScriptEnumReloadPhase::Begin
                && Event.Phase != AngelscriptReloadSafety::EPreparedScriptEnumReloadPhase::Commit))
            return Reject(Error, TEXT("Reentrant or late script enum transition"));
        TGuardValue<bool> Guard(bScriptEnumCallback, true);
        auto* Entry = ScriptEnumBindings.FindByPredicate([&](const auto& Binding)
            { return Binding->Plan.Candidate.Get() == Event.Candidate; });
        if (!Entry)
        {
            if (ScriptEnumBindings.ContainsByPredicate([&](const auto& Binding) { return Binding->Identity == Event.ExistingEnum; }))
                return Reject(Error, TEXT("An unplanned enum operation aliases a referenced physical enum"));
            return ValidateHolders(Error); // Unreferenced enums retain their original reload behavior.
        }
        auto& Binding = **Entry;
        if (!Binding.Plan.bOwnerRecompiled || Event.bDefinitionSetter || Event.ExistingEnum != Binding.Identity
            || Event.PreviousModule != Binding.Plan.PreviousModule.Get()
            || Event.CandidateModule != Binding.Plan.CandidateModule.Get()
            || Event.Previous != Binding.Plan.Previous.Get())
            return Reject(Error, TEXT("Script enum operation differs from its sealed soft-link plan"));
        if (Event.Phase == AngelscriptReloadSafety::EPreparedScriptEnumReloadPhase::Begin)
        {
            if (Binding.ClaimRevision || Binding.CommitRevision || !ValidateHolders(Error))
                return Reject(Error, TEXT("Script enum claim was reused or its input changed"));
            ++Binding.ClaimRevision; // A failed claim is never reusable.
            return true;
        }
        if (Binding.ClaimRevision != 1 || Binding.CommitRevision || !Binding.Matches(true))
            return Reject(Error, TEXT("Script enum commit has no exact live operation witness"));
        Binding.CommitRevision = Binding.ClaimRevision;
        if (!ValidateHolders(Error)) return false;
        UE_LOG(Angelscript, Display,
            TEXT("SCRIPT_PAYLOAD_SCRIPT_ENUM_WITNESS epoch=%llu owner=recompiled claim=%llu commit=%llu fixed_definition=1 same_object=1"),
            Binding.Epoch, Binding.ClaimRevision, Binding.CommitRevision);
        return ValidateHolders(Error);
    }

    bool ReadScriptEnumWitness(const UEnum* Enum, AngelscriptReloadSafety::FPreparedScriptEnumReloadWitness& Out)
    {
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope || Scope->Lease.Get() != static_cast<AngelscriptReloadSafety::IPreparedPayloadReloadLease*>(this)) return false;
        for (const auto& Binding : ScriptEnumBindings)
            if (Binding->Identity == Enum && Binding->Matches())
            {
                Out.Epoch = Binding->Epoch;
                Out.EnumIdentity = Binding->Key;
                Out.PreviousCookieKey = Binding->Plan.PreviousCookie;
                Out.CandidateCookieKey = Binding->Plan.CandidateCookie;
                Out.PreviousModuleKey = Binding->Plan.PreviousModule.Get();
                Out.CandidateModuleKey = Binding->Plan.CandidateModule.Get();
                Out.bOwnerRecompiled = Binding->Plan.bOwnerRecompiled;
                Out.ClaimRevision = Binding->ClaimRevision;
                Out.CommitRevision = Binding->CommitRevision;
                using Phase = AngelscriptReloadSafety::EPreparedScriptEnumWitnessPhase;
                Out.Phase = !Binding->Plan.bOwnerRecompiled ? Phase::Unchanged
                    : Binding->CommitRevision ? Phase::Committed : Phase::Prepared;
                return true;
            }
        return false;
    }

    void AddStableDomain(const UObject* Object)
    {
        for (const UObject* Cursor = Object; Cursor; Cursor = Cursor->GetOuter())
        {
            StableDomain.Add(Cursor);
            for (const UClass* Class = Cursor->GetClass(); Class; Class = Class->GetSuperClass())
                StableDomain.Add(Class);
        }
    }

    template<typename Visitor>
    bool VisitAffectedPayloads(Visitor&& Visit, FString& Error) const
    {
        // An empty captured slot set still scans whenever an affected reference schema exists.
        if (ReferenceReplacements.IsEmpty()) return true;
        for (TObjectIterator<UObject> It; It; ++It)
        {
            UObject* Owner = *It;
            if (Owner->IsUnreachable() || Owner->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) continue;
            if (const UDataTable* Table = Cast<UDataTable>(Owner))
            {
                UScriptStruct* Rows = const_cast<UScriptStruct*>(Table->GetRowStruct());
                if (ReferenceReplacements.Contains(Rows))
                    return Reject(Error, TEXT("Reference payload capture does not admit DataTable rows"));
                if (Rows)
                    for (const TPair<FName, uint8*>& Row : Table->GetRowMap())
                    {
                        TSet<const void*> Active;
                        if (ContainsReloadPayload(Rows, Row.Value, ReferenceReplacements, Active, Error) || !Error.IsEmpty())
                            return Reject(Error, TEXT("Reference payload capture does not admit DataTable row storage"));
                    }
            }
            for (FPropertyValueIterator Value(FStructProperty::StaticClass(), Owner->GetClass(), Owner); Value; ++Value)
            {
                const FStructProperty* Property = CastFieldChecked<FStructProperty>(Value.Key());
                if (Property->Struct != FInstancedStruct::StaticStruct()) continue;
                Value.SkipRecursiveProperty();
                const FInstancedStruct& Payload = *static_cast<const FInstancedStruct*>(Value.Value());
                if (!Payload.IsValid()) continue;
                UScriptStruct* Old = const_cast<UScriptStruct*>(Payload.GetScriptStruct());
                UScriptStruct* const* Target = ReferenceReplacements.Find(Old);
                if (!Target) continue;
                TArray<const FProperty*> Chain;
                Value.GetPropertyChain(Chain);
                if (Chain.Num() != 1 || Property->ArrayDim != 1 || !IsStableNativeObject(Owner, ReplacementClasses))
                    return Reject(Error, TEXT("Reference payload capture requires a direct stable native owner"));
                if (!Visit(Owner, Property, Payload, Old, *Target)) return false;
            }
        }
        return true;
    }

    bool CaptureReference(FReferencePayload& Item, UObject* Object, int32& Index, FString& Error)
    {
        if (!IsValid(Object) || Object->IsUnreachable()
            || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
            return Reject(Error, TEXT("Reference payload capture encountered a retired object"));
        const FObjectKey Key(Object);
        for (int32 Existing = 0; Existing < Item.References.Num(); ++Existing)
            if (Item.References[Existing].OldKey == Key) { Index = Existing; return true; }
        FPayloadReferenceIdentity Reference;
        Reference.OldKey = Key;
        Reference.OldIdentity = Object;
        Reference.Captured = Object;
        if (bBeforeSwap && Cast<UASClass>(Object->GetClass()))
        {
            const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
            const auto* Plan = AngelscriptReloadSafety::FindScriptObjectReloadPlan(Object->GetClass());
            if (!Scope || !Plan) return Reject(Error, TEXT("AS reference has no explicit generator reload plan"));
            Reference.bRequiresReplacement = Plan->Kind == AngelscriptRuntimeGcMirror::EScriptObjectReloadPlan::RequiresReplacement;
            if (!AngelscriptRuntimeGcMirror::CaptureLiveScriptReferenceWitness(
                Object, *Plan, Scope->Epoch, Reference.LiveWitness, Error)) return false;
            if (!Reference.bRequiresReplacement)
            {
                Reference.bLiveAS = true;
                Reference.Stable = Object;
                StableDomain.Add(Object);
                for (const UClass* Class = Object->GetClass(); Class; Class = Class->GetSuperClass()) StableDomain.Add(Class);
                AddStableDomain(Object->GetOuter());
            }
            Index = Item.References.Add(Reference);
            return true;
        }
        for (int32 Witness = 0; Witness < Witnesses.Num(); ++Witness)
        {
            if (Witnesses[Witness].OldIdentity != Reference.OldIdentity) continue;
            if (Reference.WitnessIndex != INDEX_NONE || Witnesses[Witness].OldKey != Key
                || Witnesses[Witness].CaptureGeneration == 0)
                return Reject(Error, TEXT("Conflicting reference payload capture witness"));
            Reference.WitnessIndex = Witness;
        }
        if (Reference.WitnessIndex == INDEX_NONE)
        {
            if (!IsStableNativeObject(Object, ReplacementClasses))
                return Reject(Error, TEXT("Reference payload required seed is absent from the capture ledger"));
            Reference.Stable = Object;
            AddStableDomain(Object);
        }
        Index = Item.References.Add(Reference);
        return true;
    }

    bool ValidateHolders(FString& Error) const
    {
        if (bOwnScriptEnumHandler && (!AngelscriptReloadSafety::GetPreparedScriptEnumReloadTransition().IsBoundToObject(this)
            || !AngelscriptReloadSafety::GetPreparedScriptEnumReloadWitnessReader().IsBoundToObject(this)))
            return Reject(Error, TEXT("Script enum channel ownership changed"));
        for (const auto& Binding : ScriptEnumBindings)
            if (!Binding->Matches()) return Reject(Error, TEXT("Script enum binding or fixed definition changed"));
        for (const FReferencePayload& Item : Payloads)
        {
            if (Item.StringBinding.IsValid() && !Item.StringBinding->Matches())
                return Reject(Error, TEXT("Native string binding changed during replacement"));
            if (!IsStableNativeObject(Item.Owner, ReplacementClasses) || FObjectKey(Item.Owner) != Item.OwnerKey
                || Item.Live().GetScriptStruct() != (bPublished ? Item.NewStruct : Item.OldStruct)
                || Item.Live().GetMemory() != (bPublished ? Item.PublishedMemory : Item.OldMemory))
                return Reject(Error, TEXT("Reference payload holder changed during replacement"));
            int32 Index = 0;
            const UScriptStruct* Schema = bPublished ? Item.NewStruct : Item.OldStruct;
            for (TFieldIterator<FProperty> Field(Schema, EFieldIterationFlags::IncludeDeprecated); Field; ++Field, ++Index)
            {
                const FPayloadFieldSnapshot* Expected = bPublished
                    ? (Item.ExpectedFields.IsValidIndex(Index) ? &Item.ExpectedFields[Index] : nullptr)
                    : (Item.Fields.IsValidIndex(Index) ? &Item.Fields[Index].Before : nullptr);
                if (!Expected || (!Expected->Matches(*Field, Item.Live().GetMemory())
                    && !MatchesRequiredProjection(Item, Index, *Field)))
                {
                    Error = FString::Printf(TEXT("%s field=%s category=%s validation=%s"),
                        bPublished ? TEXT("Published payload field changed after preparation")
                            : TEXT("Captured payload field changed before publication"),
                        *Field->GetFName().ToString(), Expected ? Expected->MismatchCategory(*Field) : TEXT("field-domain"),
                        ValidationPhase);
                    return false;
                }
            }
            if (Index != (bPublished ? Item.ExpectedFields.Num() : Item.Fields.Num()))
                return Reject(Error, TEXT("Reference payload field domain changed"));
            if (bPublished && CastChecked<UASStruct>(Item.NewStruct)->ScriptType != Item.ExpectedSchemaCookie)
                return Reject(Error, TEXT("Published payload schema cookie changed"));
        }
        int32 OldSlots = 0;
        if (!VisitAffectedPayloads([&](UObject* Owner, const FStructProperty* Property,
            const FInstancedStruct& Payload, UScriptStruct* Old, UScriptStruct*)
        {
            if (bPublished) return Reject(Error, TEXT("Affected old payload remains after publication"));
            int32 Matches = 0;
            for (const FReferencePayload& Item : Payloads)
                if (Item.Owner == Owner && Item.OwnerKey == FObjectKey(Owner) && Item.Property == Property
                    && Item.OldStruct == Old && Item.OldMemory == Payload.GetMemory()) ++Matches;
            if (Matches != 1) return Reject(Error, TEXT("Affected old payload domain gained or reused a slot"));
            ++OldSlots;
            return true;
        }, Error)) return false;
        return OldSlots == (bPublished ? 0 : Payloads.Num())
            || Reject(Error, TEXT("Affected old payload domain lost a captured slot"));
    }

    bool ValidateCandidates(FString& Error) const
    {
        for (const FReferencePayload& Item : Payloads)
        {
            if (!Item.NewStruct || bPublished) continue;
            if (CastChecked<UASStruct>(Item.NewStruct)->ScriptType != Item.ExpectedSchemaCookie
                || (Item.CandidateMemory && (Item.Candidate.GetScriptStruct() != Item.NewStruct
                    || Item.Candidate.GetMemory() != Item.CandidateMemory)))
                return Reject(Error, TEXT("Prepared payload candidate identity changed"));
            int32 Index = 0;
            for (TFieldIterator<FProperty> Field(Item.NewStruct, EFieldIterationFlags::IncludeDeprecated); Field; ++Field, ++Index)
                if (!Item.ExpectedFields.IsValidIndex(Index) || !Item.ExpectedFields[Index].MatchesMetadata(*Field)
                    || (Item.bCandidateReady && !Item.ExpectedFields[Index].Matches(*Field, Item.Candidate.GetMemory())))
                    return Reject(Error, TEXT("Prepared payload candidate content changed"));
            if (Index != Item.ExpectedFields.Num()) return Reject(Error, TEXT("Prepared payload candidate field domain changed"));
        }
        return true;
    }

    bool ValidateAfterCallout(FString& Error) const
    {
        return ValidateHolders(Error) && ValidateCandidates(Error) && ValidateLiveReferences(Error)
            && ValidatePublishedRequiredReferences(Error);
    }

    bool ValidatePublishedRequiredReferences(FString& Error) const
    {
        if (bPublished)
            for (const FReferencePayload& Item : Payloads)
                for (const FPayloadReferenceIdentity& Reference : Item.References)
                    if (Reference.bRequiresReplacement)
                    {
                        UObject* Terminal = nullptr;
                        FObjectHandle Handle{};
                        if (!ReadRequiredTerminal(Reference, Terminal, Handle, Error)) return false;
                    }
        return true;
    }

    bool FindPendingRequiredTerminal(const FPayloadReferenceIdentity& Reference,
        const TMap<UObject*, UObject*>& Map, UObject*& Terminal, bool& bTouched, FString& Error) const
    {
        Terminal = nullptr;
        bTouched = false;
        if (!Reference.bRequiresReplacement || !Witnesses.IsValidIndex(Reference.WitnessIndex)) return false;
        const auto& Witness = Witnesses[Reference.WitnessIndex];
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope || Scope->Epoch != Reference.LiveWitness.Epoch || Witness.OldKey != Reference.OldKey
            || Witness.OldIdentity != Reference.OldIdentity || !Witness.CaptureGeneration)
            return Reject(Error, TEXT("Pending payload map differs from its original seed or epoch"));
        const void* Cursor = Witness.OldIdentity;
        TSet<const void*> Seen;
        while (true)
        {
            if (Seen.Contains(Cursor)) return Reject(Error, TEXT("Pending payload map contains a cycle"));
            Seen.Add(Cursor);
            // Old keys are addresses only. Never construct a new FObjectKey or dereference them.
            UObject* const* Next = Map.Find(static_cast<UObject*>(const_cast<void*>(Cursor)));
            if (Next && *Next != Cursor)
            {
                if (!*Next) return Reject(Error, TEXT("Pending payload map has no target"));
                bTouched = true;
                Terminal = *Next;
                Cursor = *Next;
                continue;
            }
            if (bTouched) break;
            const void* const* Previous = Edges.Find(Cursor);
            if (!Previous) break;
            Cursor = *Previous;
        }
        if (!bTouched) return true;
        return (Terminal && IsValid(Terminal) && !Terminal->IsTemplate()
            && static_cast<const void*>(Terminal->GetClass()) == Witness.ExpectedNewClassIdentity)
            || Reject(Error, TEXT("Pending payload map target differs from the expected live class"));
    }

    bool MatchesRequiredProjection(const FReferencePayload& Item, int32 Index, const FProperty* Field) const
    {
        if (bPublished || !Item.Fields.IsValidIndex(Index)) return false;
        const FReferencePayloadField& Saved = Item.Fields[Index];
        if (!Saved.Before.MatchesMetadata(Field)) return false;
        auto MatchesLeaf = [&](int32 ReferenceIndex, FObjectHandle BeforeHandle, int32 Element)
        {
            // Each position independently retains its original handle or presents its own authorized terminal.
            if (Saved.Before.MatchesProjectedObject(Field, Item.Live().GetMemory(), BeforeHandle, Element)) return true;
            if (!Item.References.IsValidIndex(ReferenceIndex)) return false;
            const FPayloadReferenceIdentity& Reference = Item.References[ReferenceIndex];
            if (!Reference.bRequiresReplacement) return false;
            FString Error;
            UObject* Terminal = nullptr;
            FObjectHandle Handle{};
            if (PendingProjectionMap)
            {
                bool bTouched = false;
                if (!FindPendingRequiredTerminal(Reference, *PendingProjectionMap, Terminal, bTouched, Error)) return false;
                // This batch projection is quarantine only; publication still requires the consumed ledger.
                if (bTouched) Handle = FObjectPtr(Terminal).GetHandle();
                else if (!ReadRequiredTerminal(Reference, Terminal, Handle, Error)) return false;
            }
            else if (!ReadRequiredTerminal(Reference, Terminal, Handle, Error)) return false;
            return Saved.Before.MatchesProjectedObject(Field, Item.Live().GetMemory(), Handle, Element);
        };
        if (Saved.bObject) return MatchesLeaf(Saved.ReferenceIndex, Saved.Before.ObjectHandle, INDEX_NONE);
        if (Saved.Before.Class != FArrayProperty::StaticClass()
            || Saved.ArrayReferenceIndices.Num() != Saved.Before.ArrayHandles.Num()) return false;
        if (!IsQuietReferenceArrayStorage(Field, Field->ContainerPtrToValuePtr<void>(Item.Live().GetMemory()))) return false;
        FScriptArrayHelper Current(CastFieldChecked<FArrayProperty>(Field), Field->ContainerPtrToValuePtr<void>(Item.Live().GetMemory()));
        if (Current.Num() != Saved.ArrayReferenceIndices.Num()) return false;
        for (int32 Element = 0; Element < Saved.ArrayReferenceIndices.Num(); ++Element)
            if (!MatchesLeaf(Saved.ArrayReferenceIndices[Element], Saved.Before.ArrayHandles[Element], Element)) return false;
        return true;
    }

    bool ReadRequiredTerminal(const FPayloadReferenceIdentity& Reference, UObject*& Terminal,
        FObjectHandle& Handle, FString& Error) const
    {
        if (!Witnesses.IsValidIndex(Reference.WitnessIndex))
            return Reject(Error, TEXT("Reference payload has no required witness index"));
        const auto& Witness = Witnesses[Reference.WitnessIndex];
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!Scope || Scope->Epoch != Reference.LiveWitness.Epoch || Witness.OldKey != Reference.OldKey
            || Witness.OldIdentity != Reference.OldIdentity || !Witness.CaptureGeneration)
            return Reject(Error, TEXT("Reference payload required projection epoch or seed differs"));
        const void* Cursor = Witness.OldIdentity;
        TSet<const void*> Visited;
        while (const void* const* Next = Edges.Find(Cursor))
        {
            if (Visited.Contains(Cursor) || !*Next || *Next == Cursor)
                return Reject(Error, TEXT("Reference payload replacement graph is cyclic or missing"));
            Visited.Add(Cursor);
            Cursor = *Next;
        }
        Terminal = LiveMapValues.FindRef(Cursor);
        AngelscriptRuntimeGcMirror::FReinstancedScriptPairReplayToken Token;
        if (!Terminal || !IsValid(Terminal) || Terminal->IsTemplate()
            || static_cast<const void*>(Terminal->GetClass()) != Witness.ExpectedNewClassIdentity
            || !AngelscriptRuntimeGcMirror::GetReinstancedScriptPairReplayToken(Terminal, Token)
            || Token.NewKey != FObjectKey(Terminal)
            || Token.CaptureGeneration != Witness.CaptureGeneration
            || AngelscriptRuntimeGcMirror::ReplayReinstancedScriptPairToken(Token)
                != AngelscriptRuntimeGcMirror::EReinstancedScriptPairReplayResult::Consumed)
            return Reject(Error, TEXT("Reference payload required terminal lacks a consumed replacement token"));
        const FPayloadMappedIdentity* Identity = MapValueIdentities.Find(Cursor);
        if (static_cast<const void*>(Terminal) != Cursor || !Identity || Identity->Key != Token.NewKey
            || Identity->ClassKey != FObjectKey(Terminal->GetClass()))
            return Reject(Error, TEXT("Reference payload required projection identity was reused"));
        Handle = Identity->Handle;
        FObjectKey CursorKey = Token.NewKey;
        TSet<FObjectKey> TokenPath;
        while (CursorKey != Witness.OldKey)
        {
            const auto* Hop = TokensByNewKey.Find(CursorKey);
            if (!Hop || TokenPath.Contains(CursorKey) || Hop->NewKey != CursorKey
                || Hop->CaptureGeneration != Witness.CaptureGeneration
                || AngelscriptRuntimeGcMirror::ReplayReinstancedScriptPairToken(*Hop)
                    != AngelscriptRuntimeGcMirror::EReinstancedScriptPairReplayResult::Consumed)
                return Reject(Error, TEXT("Reference payload token chain does not reach its capture witness"));
            TokenPath.Add(CursorKey);
            CursorKey = Hop->OldKey;
        }
        return true;
    }

    bool ResolveReference(FPayloadReferenceIdentity& Reference, FString& Error)
    {
        if (Reference.Stable)
        {
            if (Reference.bLiveAS)
            {
                if (!AngelscriptRuntimeGcMirror::ValidateLiveScriptReferenceWitness(
                    Reference.Stable, Reference.LiveWitness, Error)) return false;
                Reference.Resolved = Reference.Stable;
                return true;
            }
            if (FObjectKey(Reference.Stable) != Reference.OldKey
                || !IsStableNativeObject(Reference.Stable, ReplacementClasses))
                return Reject(Error, TEXT("Stable reference payload identity changed"));
            Reference.Resolved = Reference.Stable;
            return true;
        }
        UObject* Terminal = nullptr;
        FObjectHandle Handle{};
        if (!ReadRequiredTerminal(Reference, Terminal, Handle, Error)) return false;
        Reference.Resolved = Terminal;
        for (const FReferencePayload& Item : Payloads)
            for (const FPayloadReferenceIdentity& Other : Item.References)
                if (Other.Resolved == Terminal && Other.OldKey != Reference.OldKey)
                    return Reject(Error, TEXT("Reference payload replacement merged distinct seed identities"));
        return true;
    }

    bool ValidateLiveReferences(FString& Error) const
    {
        for (const FReferencePayload& Item : Payloads)
            for (const FPayloadReferenceIdentity& Reference : Item.References)
                if (Reference.bLiveAS && !AngelscriptRuntimeGcMirror::ValidateLiveScriptReferenceWitness(
                    Reference.Stable, Reference.LiveWitness, Error)) return false;
        return true;
    }

    bool IsPublishedReference(const UObject* Object) const
    {
        // Postpublication map-target refusal uses immutable identities. Candidate admission is field-bound below.
        const FPayloadMappedIdentity* Identity = MapValueIdentities.Find(Object);
        if (!Identity) return false;
        for (const FReferencePayload& Item : Payloads)
            for (const FPayloadFieldSnapshot& Field : Item.ExpectedFields)
            {
                if (Field.Class == FObjectProperty::StaticClass() && Field.ObjectHandle == Identity->Handle) return true;
                if (Field.Class == FArrayProperty::StaticClass())
                    for (int32 Element = 0; Element < Field.ArrayHandles.Num(); ++Element)
                        if (Field.ArrayHandles[Element] == Identity->Handle) return true;
            }
        return false;
    }

    bool IsRetainedCandidateReference(const FReferencePayload& Item, const FProperty* Field, UObject* Object,
        int32 Element = INDEX_NONE) const
    {
        const FReferencePayloadField* Saved = Item.Fields.FindByPredicate(
            [&](const auto& Value) { return Value.NewProperty == Field
                && (Element == INDEX_NONE ? Value.bObject : Value.Before.Class == FArrayProperty::StaticClass()); });
        const FPayloadFieldSnapshot* Expected = Item.ExpectedFields.FindByPredicate(
            [&](const auto& Value) { return Value.Property == Field; });
        // A new default AS reference cannot borrow another retained field's witness.
        if (!Saved || !Expected || (Element != INDEX_NONE && !Saved->ArrayReferenceIndices.IsValidIndex(Element))) return false;
        const int32 ReferenceIndex = Element == INDEX_NONE ? Saved->ReferenceIndex : Saved->ArrayReferenceIndices[Element];
        if (!Item.References.IsValidIndex(ReferenceIndex)) return false;
        const FPayloadReferenceIdentity& Reference = Item.References[ReferenceIndex];
        FString Error;
        if (Reference.bLiveAS)
        {
            return Object == Reference.LiveWitness.ObjectIdentity && FObjectKey(Object) == Reference.LiveWitness.ObjectKey
                && AngelscriptRuntimeGcMirror::ValidateLiveScriptReferenceWitness(Object, Reference.LiveWitness, Error)
                && Expected->MatchesProjectedObject(Field, Item.Candidate.GetMemory(),
                    Element == INDEX_NONE ? Saved->Before.ObjectHandle : Saved->Before.ArrayHandles[Element], Element);
        }
        if (Reference.bRequiresReplacement)
        {
            UObject* Terminal = nullptr;
            FObjectHandle Handle{};
            return ReadRequiredTerminal(Reference, Terminal, Handle, Error) && Object == Terminal
                && Expected->MatchesProjectedObject(Field, Item.Candidate.GetMemory(), Handle, Element);
        }
        return false;
    }

    bool ValidateMapBatch(const TMap<UObject*, UObject*>& Map, FString& Error) const
    {
        if (Payloads.IsEmpty() && StableDomain.IsEmpty()) return true;
        for (const TPair<UObject*, UObject*>& Pair : Map)
        {
            const void* OldIdentity = Pair.Key;
            const void* NewIdentity = Pair.Value;
            if (OldIdentity == NewIdentity) continue;
            if (StableDomain.Contains(OldIdentity))
                return Reject(Error, TEXT("Stable payload identity was replaced"));
            for (const FReferencePayload& Item : Payloads)
            {
                if ((!bPublished && OldIdentity == Item.OldStruct)
                    || (bPublished && (OldIdentity == Item.NewStruct || IsPublishedReference(Pair.Key))))
                    return Reject(Error, TEXT("Payload schema or published target was replaced"));
            }
            if (const void* const* Previous = Edges.Find(OldIdentity))
                if (*Previous != NewIdentity)
                {
                    UE_LOG(Angelscript, Error, TEXT("AS_PAYLOAD_MAP_CONFLICT payloads=%d stable=%d phase=%s old=%p prior=%p next=%p"),
                        Payloads.Num(), StableDomain.Num(), ValidationPhase, OldIdentity, *Previous, NewIdentity);
                    return Reject(Error, TEXT("Conflicting payload replacement identity"));
                }
        }
        return true;
    }

    struct FCopyState { std::atomic<bool> Foreign{false}; };
    struct FCopyPair
    {
        UObject* Old = nullptr;
        UObject* New = nullptr;
        FObjectKey OldKey;
        FObjectKey NewKey;
    };
    TMap<UObject*, FCopyPair> Copies;
    TSharedPtr<FCopyState, ESPMode::ThreadSafe> CopyState;
    FDelegateHandle CopyListener;
    const AngelscriptReloadSafety::FPreparedPayloadReloadScope* CopyScope = nullptr;
    const FInstancedStructReloadScope* CopyCaller = nullptr;
    uint64 CopyEpoch = 0;
    bool bCopyCapturing = false;
    bool bCopiesClosed = false;

    bool CopyOwnerMatches(FString& Error) const
    {
        const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
        if (!IsInGameThread() || !Scope || Scope != CopyScope || Scope->Epoch != CopyEpoch
            || !CopyState.IsValid() || CopyState->Foreign.load() || bCopiesClosed)
            return Reject(Error, TEXT("Copy map reload owner or callback thread changed"));
        const bool bOwner = bBeforeSwap
            ? (Scope->Lease.Get() == this || (!Scope->Lease && !Scope->bCommitted && !bAttached))
            : (!Scope->Lease && CopyCaller && CopyCaller->Impl.Get() == this
                && Scope->bCommitted && AngelscriptReloadSafety::IsCommittedReloadActive());
        return bOwner || Reject(Error, TEXT("Copy map consumer lost its prepared lease or class-only caller"));
    }

    static bool CopyAllocated(const FCopyPair& Pair)
    {
        return Pair.Old && Pair.OldKey.ResolveObjectPtrEvenIfGarbage() == Pair.Old
            && (!Pair.New || Pair.NewKey.ResolveObjectPtrEvenIfGarbage() == Pair.New);
    }

    void CaptureCopyMap(const TMap<UObject*, UObject*>& Map)
    {
        FString Error;
        if (!CopyOwnerMatches(Error) || !bAttached || bMapCallbackInProgress || bCopyCapturing)
            AngelscriptReloadSafety::StopCommittedReload(TEXT("copy-map-capture"), Error.IsEmpty() ? TEXT("Early or reentrant copy map") : *Error);
        TGuardValue<bool> Guard(bCopyCapturing, true);
        for (const auto& Pair : Map)
        {
            FCopyPair Saved{Pair.Key, Pair.Value, FObjectKey(Pair.Key), FObjectKey(Pair.Value)};
            if (!CopyAllocated(Saved) || Pair.Key == Pair.Value)
                AngelscriptReloadSafety::StopCommittedReload(TEXT("copy-map-capture"), TEXT("Copy map contains an invalid identity"));
            if (const FCopyPair* Previous = Copies.Find(Pair.Key))
            {
                if (Previous->OldKey != Saved.OldKey || Previous->NewKey != Saved.NewKey || Previous->New != Saved.New)
                    AngelscriptReloadSafety::StopCommittedReload(
                        TEXT("copy-map-capture"), *FString::Printf(
                            TEXT("Conflicting copy map pair. Old=%s (%p) First=%s (%p) Second=%s (%p)"),
                            *GetPathNameSafe(Saved.Old), Saved.Old,
                            *GetPathNameSafe(Previous->New), Previous->New,
                            *GetPathNameSafe(Saved.New), Saved.New));
            }
            else Copies.Add(Pair.Key, Saved);
        }
    }

    void DetachCopyMaps()
    {
        if (CopyListener.IsValid()) { FCoreUObjectDelegates::OnObjectsReplaced.Remove(CopyListener); CopyListener.Reset(); }
        if (CopyState.IsValid() && Listener.IsValid()) { FCoreUObjectDelegates::OnObjectsReinstanced.Remove(Listener); Listener.Reset(); }
    }

    bool CompleteCopyMaps(FString& Error)
    {
        if (!CopyState.IsValid()) return true;
        if (!CopyOwnerMatches(Error) || !bAttached || !bPublished || bEarlyMap || bCopyCapturing || bMapCallbackInProgress) return false;
        // ReloadComplete callbacks can copy objects after the final reinstanced event.
        // Consume their notifications under the same owner before closing the epoch.
        if (!Copies.IsEmpty()) ConsumeAndObserveMap(TMap<UObject*, UObject*>());
        const bool bPending = !Copies.IsEmpty();
        DetachCopyMaps();
        bCopiesClosed = true;
        return (!bPending && !CopyState->Foreign.load()) || Reject(Error, TEXT("Reload closed with unconsumed or foreign copy maps"));
    }

public:
    void BindCopyCaller(const FInstancedStructReloadScope* Caller) { check(!CopyCaller && !bBeforeSwap); CopyCaller = Caller; }
    bool CompleteCopyCaller(const FInstancedStructReloadScope* Caller, FString& Error)
    {
        return CopyCaller == Caller && ValidateCompletion(Error);
    }
private:
    void ConsumeAndObserveMap(const TMap<UObject*, UObject*>& ActualMap)
    {
        FString CopyError;
        if (!CopyOwnerMatches(CopyError)) AngelscriptReloadSafety::StopCommittedReload(TEXT("copy-map-union"), *CopyError);
        TMap<UObject*, UObject*> Map = ActualMap;
        for (const auto& Entry : Copies)
        {
            const auto& Pair = Entry.Value;
            if (!CopyAllocated(Pair))
                AngelscriptReloadSafety::StopCommittedReload(
                    TEXT("copy-map-union"), TEXT("Captured copy identity expired before consumption"));
            if (UObject* const* Existing = Map.Find(Pair.Old))
            {
                if (*Existing != Pair.New)
                    AngelscriptReloadSafety::StopCommittedReload(
                        TEXT("copy-map-union"), *FString::Printf(
                            TEXT("Copy and reinstanced maps conflict, including null targets. Old=%s (%p) Captured=%s (%p) Actual=%s (%p)"),
                            *GetPathNameSafe(Pair.Old), Pair.Old,
                            Pair.New ? *Pair.New->GetPathName() : TEXT("<nullptr>"), Pair.New,
                            *Existing ? *(*Existing)->GetPathName() : TEXT("<nullptr>"), *Existing));
            }
            else Map.Add(Pair.Old, Pair.New);
        }
        if (!bAttached) { bEarlyMap = true; return; }
        if (bMapCallbackInProgress)
            AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-map"), TEXT("Reentrant payload replacement map"));
        TGuardValue<bool> CallbackGuard(bMapCallbackInProgress, true);
        TGuardValue<const TMap<UObject*, UObject*>*> PendingGuard(PendingProjectionMap, &Map);
        TGuardValue<const TCHAR*> PhaseGuard(ValidationPhase, TEXT("map-before-consume"));
        FString Error;
        if (!ValidateMapBatch(Map, Error) || !ValidateAfterCallout(Error))
            AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-map"), *Error);
        for (const auto& Entry : Copies)
            if (!CopyAllocated(Entry.Value))
                AngelscriptReloadSafety::StopCommittedReload(
                    TEXT("copy-map-union"), TEXT("Copy identity expired during validation"));
        // The original event follows gameplay reinitialization. Keep its effects when
        // the ledger restores the pre-reinstance script frame into the replacement.
        struct FInitializedGameField
        {
            AActor* Actor;
            FProperty* Property;
            TArray<uint8> Value;
        };
        TArray<FInitializedGameField> InitializedGameFields;
        TSet<AActor*> SeenGameActors;
        for (const auto& Pair : Map)
        {
            AActor* Actor = Cast<AActor>(Pair.Value);
            if (!IsValid(Actor) || Actor->IsTemplate() || !Actor->GetWorld() || !Actor->GetWorld()->IsGameWorld()
                || !Actor->IsActorInitialized() || !Actor->HasActorBegunPlay() || SeenGameActors.Contains(Actor)) continue;
            SeenGameActors.Add(Actor);
            for (TFieldIterator<FProperty> Field(Actor->GetClass()); Field; ++Field)
            {
                FProperty* Property = *Field;
                if (!Cast<UASClass>(Property->GetOwnerClass()) || Property->ArrayDim != 1
                    || (!CastField<FNumericProperty>(Property) && !CastField<FBoolProperty>(Property)
                        && !CastField<FEnumProperty>(Property) && !CastField<FObjectPropertyBase>(Property))) continue;
                FInitializedGameField& Saved = InitializedGameFields.AddDefaulted_GetRef();
                Saved.Actor = Actor;
                Saved.Property = Property;
                Saved.Value.SetNumZeroed(Property->GetSize());
                Property->InitializeValue(Saved.Value.GetData());
                Property->CopyCompleteValue(Saved.Value.GetData(), Property->ContainerPtrToValuePtr<void>(Actor));
            }
        }
        AngelscriptRuntimeGcMirror::ConsumeEngineReinstancedScriptClassPairs(Map);
        for (FInitializedGameField& Saved : InitializedGameFields)
        {
            Saved.Property->CopyCompleteValue(Saved.Property->ContainerPtrToValuePtr<void>(Saved.Actor), Saved.Value.GetData());
            Saved.Property->DestroyValue(Saved.Value.GetData());
        }
        PendingProjectionMap = nullptr;
        ValidationPhase = TEXT("map-after-consume");
        // Old keys are address-only after retirement; only new allocations are read below.
        for (const auto& Entry : Copies)
            if (Entry.Value.New && Entry.Value.NewKey.ResolveObjectPtrEvenIfGarbage() != Entry.Value.New)
                AngelscriptReloadSafety::StopCommittedReload(TEXT("copy-map-union"), TEXT("Copy target expired during native consumption"));
        ObserveMap(Map);
        Copies.Reset();
    }

    void ObserveMap(const TMap<UObject*, UObject*>& Map)
    {
        FString Error;
        if (!ValidateMapBatch(Map, Error)) AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-map"), *Error);
        // Runtime ledger consumption remains mandatory; only the payload-private history is conditional.
        if (!Payloads.IsEmpty() || !StableDomain.IsEmpty())
        {
            for (const TPair<UObject*, UObject*>& Pair : Map)
            {
                const void* OldIdentity = Pair.Key;
                const void* NewIdentity = Pair.Value;
                if (OldIdentity == NewIdentity) continue;
                Edges.Add(OldIdentity, NewIdentity);
                LiveMapValues.Remove(OldIdentity);
            }
            for (const TPair<UObject*, UObject*>& Pair : Map)
            {
                if (!Pair.Value || Pair.Key == Pair.Value || Edges.Contains(Pair.Value)) continue;
                if (!IsValid(Pair.Value))
                    AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-map"), TEXT("Payload replacement map has an invalid live value"));
                LiveMapValues.Add(Pair.Value, Pair.Value);
                FPayloadMappedIdentity Identity;
                Identity.Key = FObjectKey(Pair.Value);
                Identity.ClassKey = FObjectKey(Pair.Value->GetClass());
                Identity.Handle = FObjectPtr(Pair.Value).GetHandle();
                if (const auto* Previous = MapValueIdentities.Find(Pair.Value))
                    if (Previous->Key != Identity.Key || Previous->ClassKey != Identity.ClassKey || Previous->Handle != Identity.Handle)
                        AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-map"), TEXT("Payload map value identity was reused"));
                MapValueIdentities.Add(Pair.Value, Identity);
                AngelscriptRuntimeGcMirror::FReinstancedScriptPairReplayToken Token;
                if (AngelscriptRuntimeGcMirror::GetReinstancedScriptPairReplayToken(Pair.Value, Token))
                {
                    if (const auto* Existing = TokensByNewKey.Find(Token.NewKey))
                        if (Existing->OldKey != Token.OldKey || Existing->CaptureGeneration != Token.CaptureGeneration
                            || Existing->NewTrackSequence != Token.NewTrackSequence)
                            AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-map"), TEXT("Conflicting payload replacement token"));
                    TokensByNewKey.Add(Token.NewKey, Token);
                }
            }
            for (const FReferencePayload& Item : Payloads)
                for (const FPayloadReferenceIdentity& Reference : Item.References)
                {
                    if (!Reference.bRequiresReplacement) continue;
                    UObject* Pending = nullptr;
                    bool bTouched = false;
                    if (!FindPendingRequiredTerminal(Reference, Map, Pending, bTouched, Error))
                        AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-map"), *Error);
                    FObjectHandle Handle{};
                    UObject* Terminal = nullptr;
                    if (bTouched && !ReadRequiredTerminal(Reference, Terminal, Handle, Error))
                        AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-map"), *Error);
                }
        }
        else
        {
			UE_LOG(
				Angelscript,
				Verbose,
				TEXT("AS_PAYLOAD_MAP_SCOPE payloads=0 stable=0 entries=%d ledger_called=1 private_history=0 retained_values=0"),
				Map.Num());
        }
        if (!ValidateAfterCallout(Error))
            AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-map"), *Error);
    }
};

FInstancedStructReloadScope::FInstancedStructReloadScope(
    const TMap<UScriptStruct*, UScriptStruct*>& Replacements,
    const TMap<UClass*, UClass*>& ReloadClasses,
    const TArray<AngelscriptRuntimeGcMirror::FRuntimeScriptReplacementWitness>& Witnesses)
    : Structs(Replacements), Classes(ReloadClasses), Witnesses(Witnesses)
{
    const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
    if (Scope && Scope->Lease) Impl = Scope->Lease;
    else
    {
        Impl = MakeShared<FImpl>(Replacements, ReloadClasses, Witnesses);
        static_cast<FImpl*>(Impl.Get())->BindCopyCaller(this);
    }
}

FInstancedStructReloadScope::~FInstancedStructReloadScope()
{
    const auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
    if (Scope && Scope->Lease == Impl) return;
    FString Error;
    if (!static_cast<FImpl*>(Impl.Get())->CompleteCopyCaller(this, Error))
        AngelscriptReloadSafety::StopCommittedReload(TEXT("class-only-copy-map-completion"), *Error);
}
bool FInstancedStructReloadScope::Prepare(FString& Error) { return Impl->Attach(Structs, Classes, Witnesses, Error); }
bool FInstancedStructReloadScope::Publish(FString& Error) { return Impl->Publish(Error); }

bool FInstancedStructReloadScope::CaptureAdmission(const TArray<FAngelscriptPayloadReloadType>& Types, FString& Error)
{
    auto* Scope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
    if (!Scope || Scope->Lease) { Error = TEXT("Payload admission has no unique prepared owner"); return false; }
    TSharedPtr<FImpl> Lease = MakeShared<FImpl>();
    if (!Lease->CaptureBeforeSwap(Types, Error)) return false;
    Scope->Lease = Lease;
    return Lease->SealScriptEnumWitnessProvider(Error);
}

#if WITH_DEV_AUTOMATION_TESTS && UE_WITH_OBJECT_HANDLE_TRACKING
#include "HAL/PlatformTLS.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "UObject/ObjectHandleTracking.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimePayloadNativeEnumQuietWitness,
    "SoloAS.RuntimePayloadNativeEnumQuietWitness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimePayloadNativeEnumQuietWitness::RunTest(const FString& Parameters)
{
    using namespace UE::CoreUObject;
    if (!TestTrue(TEXT("Game thread owns the helper test"), IsInGameThread())
        || !TestTrue(TEXT("No reload scope is already active"),
            AngelscriptReloadSafety::GetPreparedPayloadReloadScope() == nullptr)) return false;

    asIScriptEngine* Engine = FAngelscriptManager::Get().Engine;
    if (!TestNotNull(TEXT("Runtime script engine"), Engine)) return false;
    const FString EnumName = TEXT("ECuvissQuietUnit_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TStrongObjectPtr<UEnum> Enum(NewObject<UEnum>(GetTransientPackage(), FName(*EnumName)));
    TArray<TPair<FName, int64>> Definition;
    Definition.Emplace(FName(*(EnumName + TEXT("::First"))), 0);
    Definition.Emplace(FName(*(EnumName + TEXT("::Second"))), 1);
    if (!TestTrue(TEXT("Self-owned native scoped uint8 definition"), Enum->SetEnums(Definition,
        UEnum::ECppForm::EnumClass, UEnum::EUnderlyingType::uint8, EEnumFlags::None, UEnum::EAddMaxKeyIfMissing::No))) return false;
    TArray<TPair<FName, int64>> OriginalDefinition;
    for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
        OriginalDefinition.Emplace(Enum->GetNameByIndex(Index), Enum->GetValueByIndex(Index));
    if (!TestEqual(TEXT("Complete self-owned definition"), OriginalDefinition.Num(), 2)) return false;
    const auto SetDefinition = [&](const TArray<TPair<FName, int64>>& Values)
    {
        TArray<TPair<FName, int64>> Copy = Values;
        return Enum->SetEnums(Copy, UEnum::ECppForm::EnumClass, UEnum::EUnderlyingType::uint8,
            EEnumFlags::None, UEnum::EAddMaxKeyIfMissing::No);
    };

    const FTCHARToUTF8 NativeName(*EnumName);
    const int32 TypeId = Engine->RegisterEnum(NativeName.Get());
    if (!TestTrue(TEXT("Unique native-global registration"), TypeId >= 0)) return false;
    asITypeInfo* Cookie = Engine->GetTypeInfoById(TypeId);
    if (!TestNotNull(TEXT("Registered enum cookie"), Cookie)) return false;
    Cookie->SetUserData(Enum.Get());
    ON_SCOPE_EXIT { Cookie->SetUserData(nullptr); };
    if (!TestTrue(TEXT("First registered symbol"), Engine->RegisterEnumValue(NativeName.Get(), "First", 0) >= 0)
        || !TestTrue(TEXT("Second registered symbol"), Engine->RegisterEnumValue(NativeName.Get(), "Second", 1) >= 0)) return false;
    TestTrue(TEXT("Cookie has native-global lifetime"), Cookie->GetModule() == nullptr);

    TUniquePtr<FEnumProperty> Field(CastFieldChecked<FEnumProperty>(
        FEnumProperty::StaticClass()->Construct(GetTransientPackage(), FName(TEXT("QuietUnitField")))));
    Field->SetEnum(Enum.Get());
    FByteProperty* Inner = CastFieldChecked<FByteProperty>(
        FByteProperty::StaticClass()->Construct(Field.Get(), FName(TEXT("UnderlyingType"))));
    Field->AddCppProperty(Inner);
    Field->SetElementSize(Inner->GetElementSize());
    const EPropertyFlags OriginalInnerFlags = Inner->GetPropertyFlags();

    const uint32 TestThread = FPlatformTLS::GetCurrentThreadId();
    bool bObserve = false;
    uint32 Reads = 0;
    uint32 Resolved = 0;
    uint32 ClassResolved = 0;
    const auto Observing = [&] { return FPlatformTLS::GetCurrentThreadId() == TestThread && bObserve; };
    const auto ReadCallback = AddObjectHandleReadCallback([&](const TArrayView<const UObject* const>&)
    {
        if (Observing()) ++Reads;
    });
    const auto ResolveCallback = AddObjectHandleReferenceResolvedCallback([&](const FObjectRef&, UPackage*, UObject*)
    {
        if (Observing()) ++Resolved;
    });
    const auto ClassCallback = AddObjectHandleClassResolvedCallback([&](const FObjectRef&, UPackage*, UClass*)
    {
        if (Observing()) ++ClassResolved;
    });
    ON_SCOPE_EXIT
    {
        bObserve = false;
        RemoveObjectHandleClassResolvedCallback(ClassCallback);
        RemoveObjectHandleReferenceResolvedCallback(ResolveCallback);
        RemoveObjectHandleReadCallback(ReadCallback);
    };
    const auto ResetCounts = [&] { Reads = Resolved = ClassResolved = 0; };
    const auto PinCount = [&]
    {
        const int32 WithProbe = Cookie->AddRef();
        const int32 WithoutProbe = Cookie->Release();
        TestEqual(TEXT("Public pin probe balances one reference"), WithProbe, WithoutProbe + 1);
        return WithoutProbe;
    };
    const int32 BaselinePins = PinCount();
    uint32 Comparisons = 0;
    const auto CheckMatch = [&](const FNativeEnumTypeWitness& Witness, const TCHAR* Case, bool Expected,
        const FProperty* ComparedField = nullptr)
    {
        ResetCounts();
        bObserve = true;
        const bool Actual = Witness.Matches(ComparedField ? ComparedField : Field.Get());
        bObserve = false;
        ++Comparisons;
        TestEqual(FString::Printf(TEXT("%s actual helper result"), Case), Actual, Expected);
        TestEqual(FString::Printf(TEXT("%s handle reads"), Case), Reads, uint32(0));
        TestEqual(FString::Printf(TEXT("%s object resolves"), Case), Resolved, uint32(0));
        TestEqual(FString::Printf(TEXT("%s class resolves"), Case), ClassResolved, uint32(0));
        UE_LOG(Angelscript, Display,
            TEXT("SCRIPT_ENUM_QUIET_UNIT case=%s actual_helper=1 accepted=%d expected=%d reads=%u resolved=%u class_resolved=%u"),
            Case, Actual, Expected, Reads, Resolved, ClassResolved);
    };

    ResetCounts();
    bObserve = true;
    UEnum* Control = Field->GetEnum();
    bObserve = false;
    const uint32 ControlReads = Reads;
    TestTrue(TEXT("GetEnum control returns the self-owned enum"), Control == Enum.Get());
    TestTrue(TEXT("GetEnum proves the read observer is active"), ControlReads > 0);
    UE_LOG(Angelscript, Display,
        TEXT("SCRIPT_ENUM_QUIET_UNIT_CONTROL reads=%u resolved=%u class_resolved=%u capture_credit=0 comparison_credit=0"),
        Reads, Resolved, ClassResolved);

    TUniquePtr<FNativeEnumTypeWitness> Previous;
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        AngelscriptReloadSafety::FPreparedPayloadReloadScope Scope;
        if (Previous)
        {
            CheckMatch(*Previous, TEXT("previous-epoch"), false);
            Previous.Reset();
            TestEqual(TEXT("Previous witness releases its actual cookie pin"), PinCount(), BaselinePins);
        }
        TUniquePtr<FNativeEnumTypeWitness> Witness = MakeUnique<FNativeEnumTypeWitness>();
        ResetCounts();
        bObserve = true;
        const bool Captured = Witness->Capture(Field.Get(), Cookie);
        bObserve = false;
        UE_LOG(Angelscript, Display,
            TEXT("SCRIPT_ENUM_QUIET_UNIT_CAPTURE pass=%d accepted=%d reads=%u resolved=%u class_resolved=%u comparison_credit=0"),
            Pass, Captured, Reads, Resolved, ClassResolved);
        if (!TestTrue(TEXT("Actual helper captures the native field"), Captured)) return false;
        TestEqual(TEXT("Capture holds one actual cookie pin"), PinCount(), BaselinePins + 1);
        TestTrue(TEXT("Shadow owns a different inner"), Witness->Shadow->GetUnderlyingProperty() != Inner);
        CheckMatch(*Witness, TEXT("baseline"), true);

        if (Pass == 0)
        {
            if (!TestFalse(TEXT("Fresh inner has no deprecated flag"), Inner->HasAnyPropertyFlags(CPF_Deprecated))) return false;
            Inner->SetPropertyFlags(CPF_Deprecated);
            CheckMatch(*Witness, TEXT("inner-flags-drift"), false);
            Inner->ClearPropertyFlags(CPF_Deprecated);
            TestEqual(TEXT("Full inner flags restored"), Inner->GetPropertyFlags(), OriginalInnerFlags);
            CheckMatch(*Witness, TEXT("inner-flags-restored"), true);

            TArray<TPair<FName, int64>> Changed = OriginalDefinition;
            Changed[1].Value = 7;
            if (!TestTrue(TEXT("Change self-owned enum definition"), SetDefinition(Changed))) return false;
            TestEqual(TEXT("Definition mutation is visible"), Enum->GetValueByIndex(1), int64(7));
            CheckMatch(*Witness, TEXT("definition-drift"), false);
            if (!TestTrue(TEXT("Restore complete original definition"), SetDefinition(OriginalDefinition))) return false;
            CheckMatch(*Witness, TEXT("definition-restored"), true);

            Enum->SetEnumFlags(EEnumFlags::NewerVersionExists);
            TestTrue(TEXT("Self-owned enum is logically retired"), Enum->HasAnyEnumFlags(EEnumFlags::NewerVersionExists));
            CheckMatch(*Witness, TEXT("allocated-retirement"), false);
            if (!TestTrue(TEXT("Restore original definition and flags"), SetDefinition(OriginalDefinition))) return false;
            TestFalse(TEXT("Retirement flag actually restored"), Enum->HasAnyEnumFlags(EEnumFlags::NewerVersionExists));
            CheckMatch(*Witness, TEXT("retirement-restored"), true);

            const FString ForeignName = EnumName + TEXT("_Foreign");
            TStrongObjectPtr<UEnum> Foreign(NewObject<UEnum>(GetTransientPackage(), FName(*ForeignName)));
            TArray<TPair<FName, int64>> ForeignDefinition;
            ForeignDefinition.Emplace(FName(*(ForeignName + TEXT("::First"))), 0);
            ForeignDefinition.Emplace(FName(*(ForeignName + TEXT("::Second"))), 1);
            if (!TestTrue(TEXT("Self-owned foreign enum definition"), Foreign->SetEnums(ForeignDefinition,
                UEnum::ECppForm::EnumClass, UEnum::EUnderlyingType::uint8,
                EEnumFlags::None, UEnum::EAddMaxKeyIfMissing::No))) return false;
            Cookie->SetUserData(Foreign.Get());
            TestTrue(TEXT("Own cookie userdata really changed"), Cookie->GetUserData() == Foreign.Get());
            CheckMatch(*Witness, TEXT("userdata-drift"), false);
            Cookie->SetUserData(Enum.Get());
            TestTrue(TEXT("Own cookie userdata restored"), Cookie->GetUserData() == Enum.Get());
            CheckMatch(*Witness, TEXT("userdata-restored"), true);

            TUniquePtr<FEnumProperty> ForeignField(CastFieldChecked<FEnumProperty>(
                FEnumProperty::StaticClass()->Construct(GetTransientPackage(), FName(TEXT("ForeignUnitField")))));
            ForeignField->SetEnum(Foreign.Get());
            FByteProperty* ForeignInner = CastFieldChecked<FByteProperty>(
                FByteProperty::StaticClass()->Construct(ForeignField.Get(), FName(TEXT("UnderlyingType"))));
            ForeignField->AddCppProperty(ForeignInner);
            ForeignField->SetElementSize(ForeignInner->GetElementSize());
            TestTrue(TEXT("Foreign field has the same admitted builtin shape"), IsQuietNativeEnumShape(ForeignField.Get()));
            {
                FNativeEnumTypeWitness ForeignWitness;
                ResetCounts();
                bObserve = true;
                const bool ForeignCaptured = ForeignWitness.Capture(ForeignField.Get(), Cookie);
                bObserve = false;
                UE_LOG(Angelscript, Display,
                    TEXT("SCRIPT_ENUM_QUIET_UNIT_FOREIGN_CAPTURE accepted=%d reads=%u resolved=%u class_resolved=%u baseline_credit=0"),
                    ForeignCaptured, Reads, Resolved, ClassResolved);
                TestFalse(TEXT("Actual capture rejects the foreign association"), ForeignCaptured);
                TestNotNull(TEXT("Rejected capture constructed its expected association shadow"), ForeignWitness.Shadow);
                CheckMatch(ForeignWitness, TEXT("foreign-association-invalid-capture"), false, ForeignField.Get());
            }
            TestEqual(TEXT("Rejected foreign capture releases its actual pin"), PinCount(), BaselinePins + 1);
        }
        else
        {
            // Public registration cannot remove a symbol; append last on this test-owned cookie only.
            const asUINT BeforeSymbols = Cookie->GetEnumValueCount();
            if (!TestTrue(TEXT("Append a real own-cookie symbol"),
                Engine->RegisterEnumValue(NativeName.Get(), "Appended", 2) >= 0)) return false;
            TestEqual(TEXT("Registered symbol count really increased"), Cookie->GetEnumValueCount(), BeforeSymbols + 1);
            int32 AppendedValue = 0;
            const char* AppendedName = Cookie->GetEnumValueByIndex(BeforeSymbols, &AppendedValue);
            TestTrue(TEXT("Appended registered symbol identity"), AppendedName && FCStringAnsi::Strcmp(AppendedName, "Appended") == 0);
            TestEqual(TEXT("Appended registered symbol value"), AppendedValue, 2);
            CheckMatch(*Witness, TEXT("symbol-append"), false);
        }
        Previous = MoveTemp(Witness);
    }
    CheckMatch(*Previous, TEXT("outside-prepared-scope"), false);
    Previous.Reset();
    const int32 FinalPins = PinCount();
    TestEqual(TEXT("All witness cookie pins released"), FinalPins, BaselinePins);
    TestTrue(TEXT("Original live inner survives both shadow scopes"), Field->GetUnderlyingProperty() == Inner);
    TestEqual(TEXT("Original live inner metadata survives"), Inner->GetPropertyFlags(), OriginalInnerFlags);
    const bool bRegistered = FNativeEnumTypeWitness::Registered(Engine, Cookie);
    Cookie->SetUserData(nullptr);
    const bool bUserDataCleared = Cookie->GetUserData() == nullptr;
    Field.Reset();
    Enum.Reset();
    const bool bRootReleased = !Enum.IsValid();
    TestTrue(TEXT("Own registration userdata cleared"), bUserDataCleared);
    TestTrue(TEXT("Own enum strong root released"), bRootReleased);
    UE_LOG(Angelscript, Display,
        TEXT("SCRIPT_ENUM_QUIET_UNIT_COMPLETE scope=actual-helper comparisons=%u control_reads=%u before_pins=%d after_pins=%d ")
        TEXT("registration_retained=%d userdata_cleared=%d root_released=%d migration_credit=0 revocation_credit=0 address_reuse_credit=0"),
        Comparisons, ControlReads, BaselinePins, FinalPins, bRegistered, bUserDataCleared, bRootReleased);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRuntimePayloadScriptEnumQuietWitness,
    "SoloAS.RuntimePayloadScriptEnumQuietWitness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimePayloadScriptEnumQuietWitness::RunTest(const FString& Parameters)
{
    using namespace UE::CoreUObject;
    if (!TestTrue(TEXT("Game thread owns the helper test"), IsInGameThread())
        || !TestTrue(TEXT("No reload scope is already active"),
            AngelscriptReloadSafety::GetPreparedPayloadReloadScope() == nullptr)) return false;
    auto& Manager = FAngelscriptManager::Get();
    asIScriptEngine* Engine = Manager.Engine;
    if (!TestNotNull(TEXT("Runtime script engine"), Engine)) return false;
    TSharedPtr<FAngelscriptModuleDesc> Module;
    const auto Desc = Manager.GetEnum(TEXT("EReloadOwnerUntouchedEnum"), &Module);
    if (!TestTrue(TEXT("Real initially compiled untouched-owner enum exists"), Desc.IsValid() && Module.IsValid())) return false;
    asITypeInfo* Cookie = Desc->ScriptType;
    FScriptEnumReloadBinding Membership;
    Membership.Engine = Engine;
    if (!TestTrue(TEXT("Module index and current name registry agree before cookie access"),
            Membership.HasModule(Module->ScriptModule))
        || !TestTrue(TEXT("Real active module contains this cookie"), Membership.HasCookie(Module->ScriptModule, Cookie))) return false;
    UASEnum* Enum = Cast<UASEnum>(Desc->Enum);
    if (!TestNotNull(TEXT("Real reflected script enum"), Enum)
        || !TestTrue(TEXT("Exact signed-byte nonshared script enum association"),
            Enum->GetClass() == UASEnum::StaticClass() && Cookie->GetModule() == Module->ScriptModule
            && Cookie->GetSize() == 1 && Cookie->GetUserData() == Enum
            && (Cookie->GetFlags() & asOBJ_SHARED) == 0
            && Enum->GetCppForm() == UEnum::ECppForm::EnumClass
            && Enum->GetUnderlyingType() == UEnum::EUnderlyingType::int8
            && !Enum->HasAnyEnumFlags(EEnumFlags::Flags | EEnumFlags::NewerVersionExists))) return false;
    TStrongObjectPtr<UASEnum> KeepEnum(Enum);
    TArray<TPair<FName, int64>> OriginalDefinition;
    for (int32 I = 0; I < Enum->NumEnums(); ++I)
        OriginalDefinition.Emplace(Enum->GetNameByIndex(I), Enum->GetValueByIndex(I));
    if (!TestTrue(TEXT("Definition contains the genuine signed values"), OriginalDefinition.Num() >= 2)) return false;
    const auto SetDefinition = [&](const TArray<TPair<FName, int64>>& Values)
    {
        TArray<TPair<FName, int64>> Copy = Values;
        // The virtual UUserDefinedEnum setter always appends MAX, even when Copy already contains it.
        // The public base setter with No preserves the complete captured table, including its MAX row.
        if (!Enum->UEnum::SetEnums(Copy, UEnum::ECppForm::EnumClass, UEnum::EUnderlyingType::int8,
            EEnumFlags::None, UEnum::EAddMaxKeyIfMissing::No)) return false;
        if (Enum->GetCppForm() != UEnum::ECppForm::EnumClass
            || Enum->GetUnderlyingType() != UEnum::EUnderlyingType::int8
            || Enum->HasAnyEnumFlags(EEnumFlags::Flags | EEnumFlags::NewerVersionExists)
            || Enum->NumEnums() != Values.Num()) return false;
        for (int32 I = 0; I < Values.Num(); ++I)
            if (Enum->GetNameByIndex(I) != Values[I].Key || Enum->GetValueByIndex(I) != Values[I].Value) return false;
        return true;
    };
    void* const OriginalUserData = Cookie->GetUserData();
    bool bDefinitionDirty = false;
    bool bUserDataDirty = false;
    TUniquePtr<FEnumProperty> Field(CastFieldChecked<FEnumProperty>(
        FEnumProperty::StaticClass()->Construct(GetTransientPackage(), FName(TEXT("ScriptQuietUnitField")))));
    Field->SetEnum(Enum);
    FInt8Property* Inner = CastFieldChecked<FInt8Property>(
        FInt8Property::StaticClass()->Construct(Field.Get(), FName(TEXT("UnderlyingType"))));
    Field->AddCppProperty(Inner);
    Field->SetElementSize(Inner->GetElementSize());
    const EPropertyFlags OriginalInnerFlags = Inner->GetPropertyFlags();

    const uint32 TestThread = FPlatformTLS::GetCurrentThreadId();
    bool bObserve = false;
    uint32 Reads = 0;
    uint32 Resolved = 0;
    uint32 ClassResolved = 0;
    const auto Observing = [&] { return FPlatformTLS::GetCurrentThreadId() == TestThread && bObserve; };
    const auto ReadCallback = AddObjectHandleReadCallback([&](const TArrayView<const UObject* const>&)
    {
        if (Observing()) ++Reads;
    });
    const auto ResolveCallback = AddObjectHandleReferenceResolvedCallback([&](const FObjectRef&, UPackage*, UObject*)
    {
        if (Observing()) ++Resolved;
    });
    const auto ClassCallback = AddObjectHandleClassResolvedCallback([&](const FObjectRef&, UPackage*, UClass*)
    {
        if (Observing()) ++ClassResolved;
    });
    ON_SCOPE_EXIT
    {
        bObserve = false;
        RemoveObjectHandleClassResolvedCallback(ClassCallback);
        RemoveObjectHandleReferenceResolvedCallback(ResolveCallback);
        RemoveObjectHandleReadCallback(ReadCallback);
    };
    const auto ResetCounts = [&] { Reads = Resolved = ClassResolved = 0; };
    const auto PinCount = [&]
    {
        const int32 WithProbe = Cookie->AddRef();
        const int32 WithoutProbe = Cookie->Release();
        TestEqual(TEXT("Public pin probe balances one reference"), WithProbe, WithoutProbe + 1);
        return WithoutProbe;
    };
    const int32 BaselinePins = PinCount();
    uint32 Comparisons = 0;
    const auto CheckMatch = [&](const FNativeEnumTypeWitness& Witness, const TCHAR* Case, bool Expected,
        const FProperty* ComparedField = nullptr)
    {
        ResetCounts();
        bObserve = true;
        const bool Actual = Witness.Matches(ComparedField ? ComparedField : Field.Get());
        bObserve = false;
        ++Comparisons;
        TestEqual(FString::Printf(TEXT("%s actual helper result"), Case), Actual, Expected);
        TestEqual(FString::Printf(TEXT("%s handle reads"), Case), Reads, uint32(0));
        TestEqual(FString::Printf(TEXT("%s object resolves"), Case), Resolved, uint32(0));
        TestEqual(FString::Printf(TEXT("%s class resolves"), Case), ClassResolved, uint32(0));
        UE_LOG(Angelscript, Display,
            TEXT("SCRIPT_INT8_ENUM_QUIET_UNIT case=%s actual_helper=1 accepted=%d expected=%d reads=%u resolved=%u class_resolved=%u"),
            Case, Actual, Expected, Reads, Resolved, ClassResolved);
    };

    ResetCounts();
    bObserve = true;
    UEnum* Control = Field->GetEnum();
    bObserve = false;
    const uint32 ControlReads = Reads;
    TestTrue(TEXT("GetEnum control returns the real active script enum"), Control == Enum);
    TestEqual(TEXT("GetEnum proves exactly one control read"), ControlReads, uint32(1));
    UE_LOG(Angelscript, Display,
        TEXT("SCRIPT_INT8_ENUM_QUIET_UNIT_CONTROL reads=%u resolved=%u class_resolved=%u capture_credit=0 comparison_credit=0"),
        Reads, Resolved, ClassResolved);

    AngelscriptReloadSafety::FPreparedScriptEnumReloadPlan Input;
    Input.PreviousModule = Input.CandidateModule = Module;
    Input.Previous = Input.Candidate = Desc;
    Input.PreviousCookie = Input.CandidateCookie = Cookie;
    Input.bOwnerRecompiled = false;
    Input.bEligibleFixedDefinition = true;
    TSharedPtr<FScriptEnumReloadBinding> PreviousBinding;
    TUniquePtr<FNativeEnumTypeWitness> Previous;
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        AngelscriptReloadSafety::FPreparedPayloadReloadScope Scope;
        if (Previous)
        {
            CheckMatch(*Previous, TEXT("previous-epoch"), false);
            Previous.Reset();
            PreviousBinding.Reset();
            TestEqual(TEXT("Previous binding releases both same-cookie pins"), PinCount(), BaselinePins);
        }
        auto Binding = MakeShared<FScriptEnumReloadBinding>();
        ResetCounts();
        bObserve = true;
        const bool BindingCaptured = Binding->Capture(Input);
        bObserve = false;
        UE_LOG(Angelscript, Display,
            TEXT("SCRIPT_INT8_ENUM_QUIET_UNIT_BINDING_CAPTURE pass=%d accepted=%d reads=%u resolved=%u class_resolved=%u ")
            TEXT("quiet_credit=0 owner_recompile_credit=0"),
            Pass, BindingCaptured, Reads, Resolved, ClassResolved);
        if (!TestTrue(TEXT("Actual binding captures unchanged real owner"), BindingCaptured)) return false;
        TestEqual(TEXT("Binding holds previous and candidate storage pins"), PinCount(), BaselinePins + 2);
        TestTrue(TEXT("Unchanged real plan starts complete at zero revisions"),
            Binding->Complete() && Binding->ClaimRevision == 0 && Binding->CommitRevision == 0);
        TUniquePtr<FNativeEnumTypeWitness> Witness = MakeUnique<FNativeEnumTypeWitness>();
        ResetCounts();
        bObserve = true;
        const bool Captured = Witness->Capture(Field.Get(), Cookie, nullptr, Binding);
        bObserve = false;
        UE_LOG(Angelscript, Display,
            TEXT("SCRIPT_INT8_ENUM_QUIET_UNIT_CAPTURE pass=%d accepted=%d reads=%u resolved=%u class_resolved=%u comparison_credit=0"),
            Pass, Captured, Reads, Resolved, ClassResolved);
        if (!TestTrue(TEXT("Actual complete field helper captures signed enum field"), Captured)) return false;
        TestEqual(TEXT("Field witness adds no independent cookie pin"), PinCount(), BaselinePins + 2);
        TestTrue(TEXT("Shadow owns a separate signed-byte inner"),
            Witness->Shadow->GetUnderlyingProperty() != Inner
            && Witness->Shadow->GetUnderlyingProperty()->GetClass() == FInt8Property::StaticClass());
        // Declared after Witness: recovery runs before witness/binding pins are released on early return.
        ON_SCOPE_EXIT
        {
            bObserve = false;
            if (bUserDataDirty)
            {
                Cookie->SetUserData(OriginalUserData);
                bUserDataDirty = false;
            }
            if (bDefinitionDirty)
            {
                const bool Restored = SetDefinition(OriginalDefinition);
                TestTrue(TEXT("Early exit restores shared definition and flags"), Restored);
                bDefinitionDirty = !Restored;
            }
        };
        CheckMatch(*Witness, TEXT("baseline"), true);
        if (Pass == 0)
        {
            if (!TestFalse(TEXT("Fresh inner has no deprecated flag"), Inner->HasAnyPropertyFlags(CPF_Deprecated))) return false;
            Inner->SetPropertyFlags(CPF_Deprecated);
            CheckMatch(*Witness, TEXT("inner-flags-drift"), false);
            Inner->ClearPropertyFlags(CPF_Deprecated);
            TestEqual(TEXT("Full inner flags restored"), Inner->GetPropertyFlags(), OriginalInnerFlags);
            CheckMatch(*Witness, TEXT("inner-flags-restored"), true);

            TArray<TPair<FName, int64>> Changed = OriginalDefinition;
            Changed[0].Value = OriginalDefinition[0].Value == -6 ? -5 : -6;
            bDefinitionDirty = true;
            if (!TestTrue(TEXT("Change real reflected definition temporarily"), SetDefinition(Changed))) return false;
            TestEqual(TEXT("Definition mutation is visible"), Enum->GetValueByIndex(0), Changed[0].Value);
            CheckMatch(*Witness, TEXT("definition-drift"), false);
            if (!TestTrue(TEXT("Restore complete real definition"), SetDefinition(OriginalDefinition))) return false;
            bDefinitionDirty = false;
            CheckMatch(*Witness, TEXT("definition-restored"), true);

            bDefinitionDirty = true;
            Enum->SetEnumFlags(EEnumFlags::NewerVersionExists);
            TestTrue(TEXT("Still allocated enum is marked newer-version"), Enum->HasAnyEnumFlags(EEnumFlags::NewerVersionExists));
            CheckMatch(*Witness, TEXT("allocated-retirement"), false);
            if (!TestTrue(TEXT("Restore original flags and definition"), SetDefinition(OriginalDefinition))) return false;
            bDefinitionDirty = false;
            TestFalse(TEXT("Newer-version flag actually restored"), Enum->HasAnyEnumFlags(EEnumFlags::NewerVersionExists));
            CheckMatch(*Witness, TEXT("retirement-restored"), true);

            TStrongObjectPtr<UASEnum> Foreign(NewObject<UASEnum>(GetTransientPackage(),
                FName(*(TEXT("EScriptQuietForeign_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)))));
            bUserDataDirty = true;
            Cookie->SetUserData(Foreign.Get());
            TestTrue(TEXT("Active cookie userdata really changed"), Cookie->GetUserData() == Foreign.Get());
            CheckMatch(*Witness, TEXT("userdata-drift"), false);
            Cookie->SetUserData(OriginalUserData);
            bUserDataDirty = false;
            TestTrue(TEXT("Active cookie userdata really restored"), Cookie->GetUserData() == Enum);
            CheckMatch(*Witness, TEXT("userdata-restored"), true);

            TUniquePtr<FEnumProperty> ForeignField(CastFieldChecked<FEnumProperty>(
                FEnumProperty::StaticClass()->Construct(GetTransientPackage(), FName(TEXT("ForeignScriptUnitField")))));
            ForeignField->SetEnum(Foreign.Get());
            FInt8Property* ForeignInner = CastFieldChecked<FInt8Property>(
                FInt8Property::StaticClass()->Construct(ForeignField.Get(), FName(TEXT("UnderlyingType"))));
            ForeignField->AddCppProperty(ForeignInner);
            ForeignField->SetElementSize(ForeignInner->GetElementSize());
            TestTrue(TEXT("Foreign field has the admitted signed-byte shape"), IsQuietScriptEnumShape(ForeignField.Get()));
            {
                FNativeEnumTypeWitness ForeignWitness;
                ResetCounts();
                bObserve = true;
                const bool ForeignCaptured = ForeignWitness.Capture(ForeignField.Get(), Cookie, nullptr, Binding);
                bObserve = false;
                UE_LOG(Angelscript, Display,
                    TEXT("SCRIPT_INT8_ENUM_QUIET_UNIT_FOREIGN_CAPTURE accepted=%d reads=%u resolved=%u class_resolved=%u quiet_credit=0"),
                    ForeignCaptured, Reads, Resolved, ClassResolved);
                TestFalse(TEXT("Actual capture rejects the foreign field association"), ForeignCaptured);
                CheckMatch(ForeignWitness, TEXT("foreign-association-invalid-capture"), false, ForeignField.Get());
            }
            TestEqual(TEXT("Rejected foreign field leaves binding pin count unchanged"), PinCount(), BaselinePins + 2);
            CheckMatch(*Witness, TEXT("after-all-mutations-restored"), true);
            TestTrue(TEXT("Actual binding remains complete after restoration"), Binding->Complete());
        }
        Previous = MoveTemp(Witness);
        PreviousBinding = MoveTemp(Binding);
    }
    CheckMatch(*Previous, TEXT("outside-prepared-scope"), false);
    Previous.Reset();
    PreviousBinding.Reset();
    if (!TestTrue(TEXT("Original module remains actively registered before final pin probe"),
        Membership.HasCookie(Module->ScriptModule, Cookie))) return false;
    const int32 FinalPins = PinCount();
    TestEqual(TEXT("All actual binding pins released"), FinalPins, BaselinePins);
    TestTrue(TEXT("Original live inner survives all shadow destructors"), Field->GetUnderlyingProperty() == Inner);
    TestEqual(TEXT("Original live inner flags survive"), Inner->GetPropertyFlags(), OriginalInnerFlags);
    TestTrue(TEXT("All shared mutations restored inside prepared scope"), !bDefinitionDirty && !bUserDataDirty);
    TestTrue(TEXT("Active enum description and userdata still agree"), Desc->Enum == Enum && Cookie->GetUserData() == Enum);
    Field.Reset();
    KeepEnum.Reset();
    TestFalse(TEXT("Local strong root released"), KeepEnum.IsValid());
    UE_LOG(Angelscript, Display,
        TEXT("SCRIPT_INT8_ENUM_QUIET_UNIT_COMPLETE scope=actual-helper comparisons=%u control_reads=%u before_pins=%d after_pins=%d ")
        TEXT("owner_active=1 owner_recompiled=0 shared_mutations_restored=%d local_root_released=%d ")
        TEXT("migration_credit=0 transaction_credit=0 revocation_credit=0 address_reuse_credit=0"),
        Comparisons, ControlReads, BaselinePins, FinalPins, !bDefinitionDirty && !bUserDataDirty, !KeepEnum.IsValid());
    return !HasAnyErrors();
}
#endif
