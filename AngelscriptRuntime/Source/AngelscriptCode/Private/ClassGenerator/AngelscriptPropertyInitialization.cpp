// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptPropertyInitialization.h"

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
protected:
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


protected:
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

    virtual void FinishDestroyInternal(void* Data) const override
    {
        FStructProperty::FinishDestroyInternal(Data);
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
        ObserveDelayedFieldOperationForTest(*this, Data, EAngelscriptDelayedFieldOperationForTest::FinishDestroy);
#endif
    }
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
