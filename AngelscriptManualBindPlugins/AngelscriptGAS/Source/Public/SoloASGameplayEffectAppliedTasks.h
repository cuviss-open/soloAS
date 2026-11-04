// SPDX-License-Identifier: (Apache-2.0 OR MIT)
// Copyright Cuviss Studio.
#pragma once
#include "Abilities/Tasks/AbilityTask_WaitGameplayEffectApplied_Self.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEffectApplied_Target.h"
#include "SoloASGameplayEffectAppliedTasks.generated.h"

UCLASS(NotBlueprintable, HideDropdown, meta=(NotInAngelscript))
class ANGELSCRIPTGAS_API USoloASWaitGameplayEffectAppliedSelf : public UAbilityTask_WaitGameplayEffectApplied_Self
{
    GENERATED_BODY()
public:
    USoloASWaitGameplayEffectAppliedSelf(const FObjectInitializer& Initializer) : Super(Initializer) {}
    static UAbilityTask_WaitGameplayEffectApplied_Self* Create(UGameplayAbility* OwningAbility, const FGameplayTargetDataFilterHandle& InFilter, const FGameplayTagRequirements& Source, const FGameplayTagRequirements& Target, const FGameplayTagRequirements& Asset, const FGameplayTagRequirements& Granted, bool Once, AActor* External, bool Periodic);
    static UAbilityTask_WaitGameplayEffectApplied_Self* CreateQuery(UGameplayAbility* OwningAbility, const FGameplayTargetDataFilterHandle& InFilter, const FGameplayTagQuery& Source, const FGameplayTagQuery& Target, const FGameplayTagQuery& Asset, const FGameplayTagQuery& Granted, bool Once, AActor* External, bool Periodic);
protected:
    void RegisterDelegate() override;
    void RemoveDelegate() override;
private:
    void FilteredCallback(UAbilitySystemComponent* Other, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle);
    FGameplayTagRequirements AssetTagRequirements, GrantedTagRequirements;
    FGameplayTagQuery AssetTagQuery, GrantedTagQuery;
    FDelegateHandle AppliedHandle, PeriodicHandle;
    TArray<const UClass*> BroadcastingEffects;
};

UCLASS(NotBlueprintable, HideDropdown, meta=(NotInAngelscript))
class ANGELSCRIPTGAS_API USoloASWaitGameplayEffectAppliedTarget : public UAbilityTask_WaitGameplayEffectApplied_Target
{
    GENERATED_BODY()
public:
    USoloASWaitGameplayEffectAppliedTarget(const FObjectInitializer& Initializer) : Super(Initializer) {}
    static UAbilityTask_WaitGameplayEffectApplied_Target* Create(UGameplayAbility* OwningAbility, const FGameplayTargetDataFilterHandle& InFilter, const FGameplayTagRequirements& Source, const FGameplayTagRequirements& Target, const FGameplayTagRequirements& Asset, const FGameplayTagRequirements& Granted, bool Once, AActor* External, bool Periodic);
    static UAbilityTask_WaitGameplayEffectApplied_Target* CreateQuery(UGameplayAbility* OwningAbility, const FGameplayTargetDataFilterHandle& InFilter, const FGameplayTagQuery& Source, const FGameplayTagQuery& Target, const FGameplayTagQuery& Asset, const FGameplayTagQuery& Granted, bool Once, AActor* External, bool Periodic);
protected:
    void RegisterDelegate() override;
    void RemoveDelegate() override;
private:
    void FilteredCallback(UAbilitySystemComponent* Other, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle);
    FGameplayTagRequirements AssetTagRequirements, GrantedTagRequirements;
    FGameplayTagQuery AssetTagQuery, GrantedTagQuery;
    FDelegateHandle AppliedHandle, PeriodicHandle;
    TArray<const UClass*> BroadcastingEffects;
};
