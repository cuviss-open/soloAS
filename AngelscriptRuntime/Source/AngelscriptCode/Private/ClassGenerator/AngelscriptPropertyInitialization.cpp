// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptPropertyInitialization.h"
#include "SoloASEngineCompat.h"

#include "ClassGenerator/ASClass.h"
#include "Runtime/SoloASObjectPropertyInitialization.h"
#include "UObject/TextProperty.h"
#include "UObject/PropertyOptional.h"

#include "AngelscriptRuntimeGcLifecycle.h"
#include "ClassGenerator/AngelscriptRuntimeGcMirror.h"
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
#include <atomic>
#endif

namespace
{
template<class PropertyType>
class TConstructedScriptProperty final : public PropertyType
{
public:
    TConstructedScriptProperty(FFieldVariant Owner, const FName& Name)
        : PropertyType(Owner, Name, RF_NoFlags)
    {
        static_assert(sizeof(TConstructedScriptProperty) == sizeof(PropertyType));
    }

    virtual void InitializeValueInternal(void* Data) const override
    {
        // AS already constructed this precise field before the initializer
        // reached root/DSO property copying. Temporary values still construct.
        if (!SoloASBridge::IsConstructedObjectPropertyStorage(*this, Data))
        {
            PropertyType::InitializeValueInternal(Data);
        }
    }
                                                                                                                                                    
#if SOLOAS_UE_AT_LEAST(5, 5) && SOLOAS_UE_BEFORE(5, 7)
    void FinishOwnedValue(void* Data) const { this->FinishDestroyInternal(Data); }
#endif
// SOLOAS-COMPAT-END
protected:
// SOLOAS-COMPAT-BEGIN(<5.5) UE 5.4 has no finish-destroy clear query; preserve the host destructor route [test: SoloAS.RuntimeGcLifecycle] | CS.Runtime.CoreSurface.AngelscriptPropertyInitialization.Compat1
#if SOLOAS_UE_AT_LEAST(5, 5)
    virtual bool ContainsClearOnFinishDestroyInternal(TArray<const FStructProperty*>& Encountered) const override
    {
        // Link asks the original property implementation which fields require
        // clearing, while keeping script storage out of UE's DestructorLink.
        const bool bRequiresClear = PropertyType::ContainsClearOnFinishDestroyInternal(Encountered);
        if (bRequiresClear)
            if (auto* ScriptOwner = Cast<UASClass>(this->GetOwnerStruct()))
                ScriptOwner->ScriptFinishDestroyFields.AddUnique(this);
        return SOLOAS_LEGACY_TERMINAL_LIFECYCLE && bRequiresClear;
    }
#endif
// SOLOAS-COMPAT-END

};

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
std::atomic<FAngelscriptDelayedFieldObserverForTest> GDelayedFieldObserverForTest{ nullptr };
static_assert(std::atomic<FAngelscriptDelayedFieldObserverForTest>::is_always_lock_free);

void ObserveDelayedFieldOperationForTest(const FStructProperty& Property, const void* ValueAddress,
    EAngelscriptDelayedFieldOperationForTest Operation)
{
    // Temporary property operations on other threads never load a fixture pointer.
    if (!IsInGameThread()) return;
    if (const auto Observer = GDelayedFieldObserverForTest.load(std::memory_order_acquire))
        Observer(Property, ValueAddress, Operation, 1u);
}
#endif

template<>
class TConstructedScriptProperty<FStructProperty> final : public FStructProperty
{
public:
    TConstructedScriptProperty(FFieldVariant Owner, const FName& Name)
        : FStructProperty(Owner, Name, RF_NoFlags)
    {
        static_assert(sizeof(TConstructedScriptProperty) == sizeof(FStructProperty));
    }

    virtual void InitializeValueInternal(void* Data) const override
    {
        if (!SoloASBridge::IsConstructedObjectPropertyStorage(*this, Data))
            FStructProperty::InitializeValueInternal(Data);
    }

    virtual void DestroyValueInternal(void* Data) const override
    {
        FStructProperty::DestroyValueInternal(Data);
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
        AngelscriptRuntimeGcMirror::CompleteRuntimeTerminalFieldDestroy(*this, Data);
#endif
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
        ObserveDelayedFieldOperationForTest(*this, Data, EAngelscriptDelayedFieldOperationForTest::DestroyValue);
#endif
    }

                                                                                                                                                     
#if SOLOAS_UE_AT_LEAST(5, 5) && SOLOAS_UE_BEFORE(5, 7)
    void FinishOwnedValue(void* Data) const { FinishDestroyInternal(Data); }
#endif
// SOLOAS-COMPAT-END

protected:
// SOLOAS-COMPAT-BEGIN(<5.5) UE 5.4 has no finish-destroy clear query; preserve the host destructor route [test: SoloAS.RuntimeGcLifecycle] | CS.Runtime.CoreSurface.AngelscriptPropertyInitialization.Compat2
#if SOLOAS_UE_AT_LEAST(5, 5)
    virtual bool ContainsClearOnFinishDestroyInternal(TArray<const FStructProperty*>& Encountered) const override
    {
        // Link asks the original property implementation which fields require
        // clearing, while keeping script storage out of UE's DestructorLink.
        const bool bRequiresClear = FStructProperty::ContainsClearOnFinishDestroyInternal(Encountered);
        if (bRequiresClear)
            if (auto* ScriptOwner = Cast<UASClass>(this->GetOwnerStruct()))
                ScriptOwner->ScriptFinishDestroyFields.AddUnique(this);
        return SOLOAS_LEGACY_TERMINAL_LIFECYCLE && bRequiresClear;
    }
#endif
// SOLOAS-COMPAT-END

// SOLOAS-COMPAT-BEGIN(<5.5) The UE 5.4 host has no finish-clear virtual protocol [test: SoloAS.RuntimeGcLifecycle] | CS.Runtime.CoreSurface.AngelscriptPropertyInitialization.Compat3
#if SOLOAS_UE_AT_LEAST(5, 5)
    virtual void FinishDestroyInternal(void* Data) const override
    {
        FStructProperty::FinishDestroyInternal(Data);
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
        ObserveDelayedFieldOperationForTest(*this, Data, EAngelscriptDelayedFieldOperationForTest::FinishDestroy);
#endif
    }
#endif
// SOLOAS-COMPAT-END
};
}

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
void SetAngelscriptDelayedFieldObserverForTest(FAngelscriptDelayedFieldObserverForTest Observer)
{
    check(IsInGameThread());
    GDelayedFieldObserverForTest.store(Observer, std::memory_order_release);
}
#endif

FProperty* CreateAngelscriptProperty(FFieldClass* PropertyClass, FFieldVariant Owner, const FName& Name)
{
    if (Owner.IsUObject() && Cast<UASClass>(Owner.ToUObject()) != nullptr)
    {
        if (PropertyClass == FStructProperty::StaticClass())
            return new TConstructedScriptProperty<FStructProperty>(Owner, Name);
        if (PropertyClass == FSetProperty::StaticClass())
            return new TConstructedScriptProperty<FSetProperty>(Owner, Name);
        if (PropertyClass == FMapProperty::StaticClass())
            return new TConstructedScriptProperty<FMapProperty>(Owner, Name);
        if (PropertyClass == FTextProperty::StaticClass())
            return new TConstructedScriptProperty<FTextProperty>(Owner, Name);
        if (PropertyClass == FArrayProperty::StaticClass())
            return new TConstructedScriptProperty<FArrayProperty>(Owner, Name);
        if (PropertyClass == FOptionalProperty::StaticClass())
            return new TConstructedScriptProperty<FOptionalProperty>(Owner, Name);
        if (PropertyClass == FSoftObjectProperty::StaticClass())
            return new TConstructedScriptProperty<FSoftObjectProperty>(Owner, Name);
        if (PropertyClass == FSoftClassProperty::StaticClass())
            return new TConstructedScriptProperty<FSoftClassProperty>(Owner, Name);
    }
    return CastFieldChecked<FProperty>(PropertyClass->Construct(Owner, Name));
}

FStructProperty* CreateAngelscriptStructProperty(FFieldVariant Owner, const FName& Name)
{
    return CastFieldChecked<FStructProperty>(CreateAngelscriptProperty(FStructProperty::StaticClass(), Owner, Name));
}

                                                                                                                                                     
#if SOLOAS_UE_AT_LEAST(5, 5) && SOLOAS_UE_BEFORE(5, 7)
void FinishDestroyAngelscriptProperty(const FProperty& Property, void* Object)
{
    // Only the wrappers above register ScriptFinishDestroyFields. Call their
    // native virtual implementation without the legacy NoDestructor gate;
    // that flag must remain set for subsequently linked Blueprint subclasses.
    void* Data = Property.ContainerPtrToValuePtr<void>(Object);
    if (Property.GetClass() == FStructProperty::StaticClass())
        static_cast<const TConstructedScriptProperty<FStructProperty>&>(Property).FinishOwnedValue(Data);
    else if (Property.GetClass() == FSetProperty::StaticClass())
        static_cast<const TConstructedScriptProperty<FSetProperty>&>(Property).FinishOwnedValue(Data);
    else if (Property.GetClass() == FMapProperty::StaticClass())
        static_cast<const TConstructedScriptProperty<FMapProperty>&>(Property).FinishOwnedValue(Data);
    else if (Property.GetClass() == FTextProperty::StaticClass())
        static_cast<const TConstructedScriptProperty<FTextProperty>&>(Property).FinishOwnedValue(Data);
    else if (Property.GetClass() == FArrayProperty::StaticClass())
        static_cast<const TConstructedScriptProperty<FArrayProperty>&>(Property).FinishOwnedValue(Data);
    else if (Property.GetClass() == FOptionalProperty::StaticClass())
        static_cast<const TConstructedScriptProperty<FOptionalProperty>&>(Property).FinishOwnedValue(Data);
    else if (Property.GetClass() == FSoftObjectProperty::StaticClass())
        static_cast<const TConstructedScriptProperty<FSoftObjectProperty>&>(Property).FinishOwnedValue(Data);
    else if (Property.GetClass() == FSoftClassProperty::StaticClass())
        static_cast<const TConstructedScriptProperty<FSoftClassProperty>&>(Property).FinishOwnedValue(Data);
    else
        checkNoEntry();
}
#endif
// SOLOAS-COMPAT-END

// SOLOAS-COMPAT-BEGIN(<5.5) UE 5.4 has no finish-clear protocol; clear the struct types UE 5.5+ marks ClearOnFinishDestroy [test: SoloAS.RuntimeReloadCrossModuleDestructorException] [id: CS.Compatibility.AngelscriptRuntime.AngelscriptPropertyInitialization_cpp.LegacyFinishClear]
#if SOLOAS_UE_BEFORE(5, 5)
#include "StructUtils/InstancedStruct.h"
#include "InstancedStructContainer.h"

namespace
{
// The struct types UE 5.5+ declares WithClearOnFinishDestroy.
bool IsLegacyFinishClearStruct(const UScriptStruct* Struct)
{
    return Struct != nullptr
        && (Struct == FInstancedStruct::StaticStruct() || Struct == FInstancedStructContainer::StaticStruct());
}

// UE 5.5+ FProperty::ContainsFinishDestroy with its struct and container overrides. The script field
// itself carries our NoDestructor ownership flag, so only nested properties are gated by it.
bool ContainsLegacyFinishClear(const FProperty& Property, TArray<const FStructProperty*>& Encountered, bool bScriptField)
{
    if (!bScriptField && Property.HasAnyPropertyFlags(CPF_IsPlainOldData | CPF_NoDestructor))
        return false;
    if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
    {
        if (StructProperty->Struct == nullptr || Encountered.Contains(StructProperty))
            return false;
        if (IsLegacyFinishClearStruct(StructProperty->Struct))
            return true;
        Encountered.Add(StructProperty);
        bool bContains = false;
        for (const FProperty* Member = StructProperty->Struct->PropertyLink; Member && !bContains; Member = Member->PropertyLinkNext)
            bContains = ContainsLegacyFinishClear(*Member, Encountered, false);
        Encountered.RemoveSingleSwap(StructProperty, EAllowShrinking::No);
        return bContains;
    }
    if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
        return ContainsLegacyFinishClear(*Array->Inner, Encountered, false);
    if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
        return ContainsLegacyFinishClear(*Set->GetElementProperty(), Encountered, false);
    if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
        return ContainsLegacyFinishClear(*Map->GetKeyProperty(), Encountered, false)
            || ContainsLegacyFinishClear(*Map->GetValueProperty(), Encountered, false);
    if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(&Property))
        return ContainsLegacyFinishClear(*Optional->GetValueProperty(), Encountered, false);
    return false;
}

// UE 5.5+ FProperty::FinishDestroy with its struct and container FinishDestroyInternal overrides.
void FinishDestroyLegacyValue(const FProperty& Property, void* Data, bool bScriptField)
{
    if (Data == nullptr || (!bScriptField && Property.HasAnyPropertyFlags(CPF_IsPlainOldData | CPF_NoDestructor)))
        return;
    if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
    {
        if (IsLegacyFinishClearStruct(StructProperty->Struct))
        {
            StructProperty->Struct->ClearScriptStruct(Data, Property.ArrayDim);
            return;
        }
        const int32 Stride = StructProperty->Struct->GetStructureSize();
        for (int32 ArrayIndex = 0; ArrayIndex < Property.ArrayDim; ++ArrayIndex)
        {
            uint8* Item = static_cast<uint8*>(Data) + ArrayIndex * Stride;
            for (const FProperty* Member = StructProperty->Struct->PropertyLink; Member; Member = Member->PropertyLinkNext)
                FinishDestroyLegacyValue(*Member, Member->ContainerPtrToValuePtr<void>(Item), false);
        }
    }
    else if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
    {
        if (!Array->Inner->HasAnyPropertyFlags(CPF_IsPlainOldData | CPF_NoDestructor))
        {
            FScriptArrayHelper Helper(Array, Data);
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
                FinishDestroyLegacyValue(*Array->Inner, Helper.GetRawPtr(Index), false);
        }
    }
    else if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
    {
        if (!Set->GetElementProperty()->HasAnyPropertyFlags(CPF_IsPlainOldData | CPF_NoDestructor))
        {
            FScriptSetHelper Helper(Set, Data);
            for (FScriptSetHelper::FIterator It = Helper.CreateIterator(); It; ++It)
                FinishDestroyLegacyValue(*Set->GetElementProperty(), Helper.GetElementPtr(It), false);
        }
    }
    else if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
    {
        const bool bKey = !Map->GetKeyProperty()->HasAnyPropertyFlags(CPF_IsPlainOldData | CPF_NoDestructor);
        const bool bValue = !Map->GetValueProperty()->HasAnyPropertyFlags(CPF_IsPlainOldData | CPF_NoDestructor);
        if (bKey || bValue)
        {
            FScriptMapHelper Helper(Map, Data);
            for (FScriptMapHelper::FIterator It = Helper.CreateIterator(); It; ++It)
            {
                if (bKey)
                    FinishDestroyLegacyValue(*Map->GetKeyProperty(), Helper.GetKeyPtr(It), false);
                if (bValue)
                    FinishDestroyLegacyValue(*Map->GetValueProperty(), Helper.GetValuePtr(It), false);
            }
        }
    }
    else if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(&Property))
    {
        if (void* Value = Optional->GetValuePointerForReplaceIfSet(Data))
            FinishDestroyLegacyValue(*Optional->GetValueProperty(), Value, false);
    }
}
}

bool AngelscriptPropertyContainsLegacyFinishClear(const FProperty& Property)
{
    TArray<const FStructProperty*> Encountered;
    return ContainsLegacyFinishClear(Property, Encountered, true);
}

void FinishDestroyAngelscriptProperty(const FProperty& Property, void* Object)
{
    FinishDestroyLegacyValue(Property, Property.ContainerPtrToValuePtr<void>(Object), true);
}
#endif
// SOLOAS-COMPAT-END
