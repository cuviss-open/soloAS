// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.

#include "ClassGenerator/AngelscriptFieldDestruction.h"

#include "AngelscriptManager.h"
#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptType.h"
#include "ClassGenerator/ASClass.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectArray.h"
#include <new>

#include "StartAngelscriptHeaders.h"
#include "as_module.h"
#include "as_objecttype.h"
#include "as_property.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

namespace
{
bool IsDelayedDestroyValue(const asCObjectProperty& Property)
{
    return Property.type.IsObject() && !Property.type.IsReference() && !Property.type.IsObjectHandle()
        && FAngelscriptTypeUsage::FromDataType(Property.type).GetUnrealStruct() == FInstancedStruct::StaticStruct();
}
}

void PrepareAngelscriptFieldDestruction(TSharedRef<FAngelscriptModuleDesc> Module)
{
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    // Cached members restore their recorded owner. Never infer a bit after bytecode compilation.
    if (Module->bLoadedPrecompiledCode || Module->bCompileError || Module->ScriptModule == nullptr)
    {
        return;
    }
    asCModule* ScriptModule = Module->ScriptModule;
    for (const auto& ClassDesc : Module->Classes)
    {
        if (ClassDesc->bIsStruct || ClassDesc->bIsStaticsClass || ClassDesc->CodeSuperClass == nullptr
            || !ClassDesc->CodeSuperClass->IsChildOf(UObject::StaticClass()))
        {
            continue;
        }
        asSNameSpace* Namespace = ScriptModule->defaultNamespace;
        if (ClassDesc->Namespace.IsSet())
        {
            Namespace = ScriptModule->engine->FindNameSpace(TCHAR_TO_ANSI(*ClassDesc->Namespace.GetValue()));
        }
        if (Namespace == nullptr)
        {
            continue;
        }
        asCObjectType* Type = CastToObjectType(ScriptModule->GetType(TCHAR_TO_ANSI(*ClassDesc->ClassName), Namespace));
        if (Type == nullptr)
        {
            continue;
        }
        for (const auto& Desc : ClassDesc->Properties)
        {
            if (Desc->UnrealPropertyNameOverride.IsSet())
            {
                continue;
            }
            asCObjectProperty* Match = nullptr;
            int32 Matches = 0;
            for (asUINT Index = 0; Index < Type->localProperties.GetLength(); ++Index)
            {
                asCObjectProperty* Property = Type->localProperties[Index];
                if (Desc->PropertyName == ANSI_TO_TCHAR(Property->name.AddressOf()))
                {
                    Match = Property;
                    ++Matches;
                }
            }
            if (Matches == 1 && IsDelayedDestroyValue(*Match))
            {
                Match->bUeDelayedDestroyOwner = true;
            }
        }
    }
#endif
}

void ValidateAngelscriptFieldDestruction(UASClass* Class, asITypeInfo* ScriptType)
{
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    Class->UeDelayedDestroyFields.Reset();
    asCObjectType* Type = CastToObjectType(static_cast<asCTypeInfo*>(ScriptType));
    if (Type == nullptr || Class->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic))
    {
        return;
    }
    for (asUINT Index = 0; Index < Type->localProperties.GetLength(); ++Index)
    {
        asCObjectProperty* Member = Type->localProperties[Index];
        if (!Member->bUeDelayedDestroyOwner)
        {
            continue;
        }
        const FName MemberName(ANSI_TO_TCHAR(Member->name.AddressOf()));
        FStructProperty* Property = FindFProperty<FStructProperty>(Class, MemberName);
        TArray<const FStructProperty*> Encountered;
        if (Property == nullptr || Property->GetOwnerStruct() != Class || Property->ArrayDim != 1
            || Property->Struct != FInstancedStruct::StaticStruct() || !IsDelayedDestroyValue(*Member)
            || Property->GetOffset_ForInternal() != Member->byteOffset
            || Property->GetElementSize() != static_cast<int32>(sizeof(FInstancedStruct))
            || Member->type.GetSizeInMemoryBytes() != static_cast<int32>(sizeof(FInstancedStruct))
            || !Property->ContainsFinishDestroy(Encountered))
        {
            UE_LOG(Angelscript, Fatal, TEXT("ASR-SCRIPT-FIELD-OWNER-MISMATCH: %s:%s"),
                *Class->GetPathName(), ANSI_TO_TCHAR(Member->name.AddressOf()));
        }
        Class->UeDelayedDestroyFields.Add(Property);
    }
#endif
}

void VerifyAngelscriptFieldDestructorLinks(const UASClass* Class)
{
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    for (const FProperty* Owned : Class->UeDelayedDestroyFields)
    {
        int32 Matches = 0;
        for (const FProperty* Property = Class->DestructorLink; Property != nullptr;
             Property = Property->DestructorLinkNext)
        {
            Matches += Property == Owned ? 1 : 0;
        }
        if (Matches != 1)
        {
            UE_LOG(Angelscript, Fatal, TEXT("ASR-SCRIPT-FIELD-OWNER-LINK-MISSING: %s"), *Owned->GetPathName());
        }
    }
#endif
}

namespace
{
bool SameReconstructionObjectItem(int32 Index, int32 Serial, const UObject* Object)
{
    const FUObjectItem* Item = Index >= 0 ? GUObjectArray.IndexToObject(Index) : nullptr;
    return Serial != 0 && Item && Item->GetSerialNumber() == Serial && Item->GetObject() == Object;
}

bool SameReconstructionField(const FSameCookieReconstructionField& Left, const FSameCookieReconstructionField& Right)
{
    return Left.Property == Right.Property && Left.Owner == Right.Owner && Left.OwnerType == Right.OwnerType
        && Left.Storage == Right.Storage && Left.Offset == Right.Offset && Left.Flags == Right.Flags;
}
}

bool CaptureSameCookieReconstructionFields(UObject* Object, UASClass* Class,
    asITypeInfo* Cookie, FSameCookieReconstructionFieldPlan& OutPlan)
{
    OutPlan = {};
    if (!IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread() || GExitPurge
        || !IsValid(Object) || !Class || !Cookie || Object->IsUnreachable()
        || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
        || !Object->IsA(Class) || Cookie->GetUserData() != Class || Class->ScriptTypePtr != Cookie) return false;
    FSameCookieReconstructionFieldPlan Plan;
    Plan.Object = Object;
    Plan.Class = Class;
    Plan.ActualClass = Object->GetClass();
    Plan.Cookie = Cookie;
    Plan.ObjectIndex = GUObjectArray.ObjectToIndex(Object);
    Plan.ObjectSerial = GUObjectArray.GetSerialNumber(Plan.ObjectIndex);
    Plan.ClassIndex = GUObjectArray.ObjectToIndex(Plan.ActualClass);
    Plan.ClassSerial = GUObjectArray.GetSerialNumber(Plan.ClassIndex);
    if (!SameReconstructionObjectItem(Plan.ObjectIndex, Plan.ObjectSerial, Object)
        || !SameReconstructionObjectItem(Plan.ClassIndex, Plan.ClassSerial, Plan.ActualClass)
        || Plan.ActualClass->bLayoutChanging) return false;

    TSet<const FProperty*> SeenChain;
    for (const FProperty* Property = Plan.ActualClass->DestructorLink; Property; Property = Property->DestructorLinkNext)
    {
        if (SeenChain.Contains(Property)) return false;
        SeenChain.Add(Property);
        Plan.DestructorChain.Add(Property);
    }
    TSet<const FStructProperty*> SeenOwned;
    asCObjectType* OwnerType = CastToObjectType(static_cast<asCTypeInfo*>(Cookie));
    TSet<const UClass*> SeenOwners;
    for (UClass* OwnerClass = Class; OwnerClass; OwnerClass = OwnerClass->GetSuperClass())
    {
        if (SeenOwners.Contains(OwnerClass)) return false;
        SeenOwners.Add(OwnerClass);
        UASClass* Owner = Cast<UASClass>(OwnerClass);
        if (!Owner) break;
        if (!OwnerType || OwnerType->GetUserData() != Owner || Owner->ScriptTypePtr != OwnerType
            || Owner->bLayoutChanging || Owner->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) return false;
        int32 MarkedMembers = 0;
        for (asUINT Index = 0; Index < OwnerType->localProperties.GetLength(); ++Index)
        {
            const asCObjectProperty* Member = OwnerType->localProperties[Index];
            if (!Member->bUeDelayedDestroyOwner) continue;
            ++MarkedMembers;
            const FStructProperty* Property = FindFProperty<FStructProperty>(Owner, FName(ANSI_TO_TCHAR(Member->name.AddressOf())));
            TArray<const FStructProperty*> Encountered;
            if (!Property || Property->GetOwnerStruct() != Owner || !Owner->UeDelayedDestroyFields.Contains(Property)
                || !IsDelayedDestroyValue(*Member) || Property->Struct != FInstancedStruct::StaticStruct()
                || Property->ArrayDim != 1 || Property->GetElementSize() != int32(sizeof(FInstancedStruct))
                || Property->GetSize() != int32(sizeof(FInstancedStruct))
                || Member->type.GetSizeInMemoryBytes() != int32(sizeof(FInstancedStruct))
                || Property->GetOffset_ForInternal() != Member->byteOffset
                || Property->GetOffset_ForInternal() < Owner->ScriptPropertyOffset
                || !Property->ContainsFinishDestroy(Encountered) || SeenOwned.Contains(Property)
                || !SeenChain.Contains(Property)) return false;
            const int32 Offset = Property->GetOffset_ForInternal();
            if (Offset < 0 || Offset > Plan.ActualClass->GetPropertiesSize() - int32(sizeof(FInstancedStruct))
                || Offset > int32(Cookie->GetSize()) - int32(sizeof(FInstancedStruct))) return false;
            void* Storage = Property->ContainerPtrToValuePtr<void>(Object);
            if (!Storage || reinterpret_cast<UPTRINT>(Storage) % alignof(FInstancedStruct) != 0) return false;
            for (const auto& Existing : Plan.Fields)
                if (Existing.Storage == Storage || FMath::Abs(Existing.Offset - Offset) < int32(sizeof(FInstancedStruct))) return false;
            SeenOwned.Add(Property);
            Plan.Fields.Add({Property, Owner, OwnerType, Storage, Offset, uint64(Property->GetPropertyFlags())});
        }
        if (MarkedMembers != Owner->UeDelayedDestroyFields.Num()) return false;
        OwnerType = OwnerType->derivedFrom;
    }
    if (SeenOwned.Num() != Plan.Fields.Num()) return false;
    OutPlan = MoveTemp(Plan);
    return true;
}

bool CompleteSameCookieReconstructionFields(FSameCookieReconstructionFieldPlan& Plan)
{
    if (Plan.bCompleted || !IsInGameThread() || !IsAngelscriptExecutionWriterHeldByCurrentThread()
        || !SameReconstructionObjectItem(Plan.ObjectIndex, Plan.ObjectSerial, Plan.Object)
        || !SameReconstructionObjectItem(Plan.ClassIndex, Plan.ClassSerial, Plan.ActualClass)) return false;
    FSameCookieReconstructionFieldPlan Current;
    if (!CaptureSameCookieReconstructionFields(Plan.Object, Plan.Class, Plan.Cookie, Current)
        || Current.ActualClass != Plan.ActualClass || Current.DestructorChain != Plan.DestructorChain
        || Current.Fields.Num() != Plan.Fields.Num()) return false;
    for (int32 Index = 0; Index != Plan.Fields.Num(); ++Index)
        if (!SameReconstructionField(Plan.Fields[Index], Current.Fields[Index])) return false;
    // Revalidation is complete for every field before ending any value lifetime.
    Plan.bCompleted = true;
    for (const auto& Field : Plan.Fields)
    {
        FInstancedStruct* const OldValue = static_cast<FInstancedStruct*>(Field.Storage);
        OldValue->~FInstancedStruct();
        // The interpreted zero constructor writes bytes; establish a legal empty object first.
        ::new (Field.Storage) FInstancedStruct();
        UE_LOG(Angelscript, Display,
            TEXT("SOLOAS_RECONSTRUCTION_OWNED_FIELD object=%p property=%s offset=%d old_destroy=1 empty_anchor=1 before_constructor=1"),
            Plan.Object, *Field.Property->GetName(), Field.Offset);
    }
    return true;
}
