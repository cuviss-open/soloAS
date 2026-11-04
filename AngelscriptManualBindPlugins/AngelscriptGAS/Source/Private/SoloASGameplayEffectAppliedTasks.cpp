// SPDX-License-Identifier: (Apache-2.0 OR MIT)
// Copyright Cuviss Studio.
#include "SoloASGameplayEffectAppliedTasks.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"

UAbilityTask_WaitGameplayEffectApplied_Self* USoloASWaitGameplayEffectAppliedSelf::Create(UGameplayAbility* OwningAbility, const FGameplayTargetDataFilterHandle& InFilter, const FGameplayTagRequirements& Source, const FGameplayTagRequirements& Target, const FGameplayTagRequirements& Asset, const FGameplayTagRequirements& Granted, bool Once, AActor* External, bool Periodic)
{
    auto* Task = NewAbilityTask<USoloASWaitGameplayEffectAppliedSelf>(OwningAbility);
    Task->Filter = InFilter;
    Task->SourceTagRequirements = Source;
    Task->TargetTagRequirements = Target;
    Task->AssetTagRequirements = Asset;
    Task->GrantedTagRequirements = Granted;
    Task->TriggerOnce = Once;
    Task->SetExternalActor(External);
    Task->ListenForPeriodicEffects = Periodic;
    return Task;
}

UAbilityTask_WaitGameplayEffectApplied_Self* USoloASWaitGameplayEffectAppliedSelf::CreateQuery(UGameplayAbility* OwningAbility, const FGameplayTargetDataFilterHandle& InFilter, const FGameplayTagQuery& Source, const FGameplayTagQuery& Target, const FGameplayTagQuery& Asset, const FGameplayTagQuery& Granted, bool Once, AActor* External, bool Periodic)
{
    auto* Task = NewAbilityTask<USoloASWaitGameplayEffectAppliedSelf>(OwningAbility);
    Task->Filter = InFilter;
    Task->SourceTagQuery = Source;
    Task->TargetTagQuery = Target;
    Task->AssetTagQuery = Asset;
    Task->GrantedTagQuery = Granted;
    Task->TriggerOnce = Once;
    Task->SetExternalActor(External);
    Task->ListenForPeriodicEffects = Periodic;
    return Task;
}

void USoloASWaitGameplayEffectAppliedSelf::RegisterDelegate()
{
    AppliedHandle = GetASC()->OnGameplayEffectAppliedDelegateToSelf.AddUObject(this, &USoloASWaitGameplayEffectAppliedSelf::FilteredCallback);
    if (ListenForPeriodicEffects)
        PeriodicHandle = GetASC()->OnPeriodicGameplayEffectExecuteDelegateOnSelf.AddUObject(this, &USoloASWaitGameplayEffectAppliedSelf::FilteredCallback);
}
void USoloASWaitGameplayEffectAppliedSelf::RemoveDelegate()
{
    GetASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(AppliedHandle);
    if (PeriodicHandle.IsValid()) GetASC()->OnPeriodicGameplayEffectExecuteDelegateOnSelf.Remove(PeriodicHandle);
}
void USoloASWaitGameplayEffectAppliedSelf::FilteredCallback(UAbilitySystemComponent* Other, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle)
{
    AActor* Avatar = Other ? Other->GetAvatarActor_Direct() : nullptr;
    if (!Filter.FilterPassesForActor(Avatar)
        || !SourceTagRequirements.RequirementsMet(*Spec.CapturedSourceTags.GetAggregatedTags())
        || !TargetTagRequirements.RequirementsMet(*Spec.CapturedTargetTags.GetAggregatedTags())) return;
    FGameplayTagContainer AssetTags, GrantedTags;
    Spec.GetAllAssetTags(AssetTags);
    Spec.GetAllGrantedTags(GrantedTags);
    if (!AssetTagRequirements.RequirementsMet(AssetTags) || !GrantedTagRequirements.RequirementsMet(GrantedTags)
        || (!SourceTagQuery.IsEmpty() && !SourceTagQuery.Matches(*Spec.CapturedSourceTags.GetAggregatedTags()))
        || (!TargetTagQuery.IsEmpty() && !TargetTagQuery.Matches(*Spec.CapturedTargetTags.GetAggregatedTags()))
        || (!AssetTagQuery.IsEmpty() && !AssetTagQuery.Matches(AssetTags))
        || (!GrantedTagQuery.IsEmpty() && !GrantedTagQuery.Matches(GrantedTags))) return;
    const UClass* EffectClass = Spec.Def ? Spec.Def->GetClass() : nullptr;
    if (BroadcastingEffects.Contains(EffectClass))
    {
        UE_LOG(LogTemp, Error, TEXT("SoloAS gameplay effect task suppressed recursive application of the same effect class"));
        return;
    }
    FGameplayEffectSpecHandle SpecHandle(new FGameplayEffectSpec(Spec));
    BroadcastingEffects.Add(EffectClass);
    Super::BroadcastDelegate(Avatar, SpecHandle, Handle);
    BroadcastingEffects.Pop(EAllowShrinking::No);
    if (TriggerOnce) EndTask();
}

UAbilityTask_WaitGameplayEffectApplied_Target* USoloASWaitGameplayEffectAppliedTarget::Create(UGameplayAbility* OwningAbility, const FGameplayTargetDataFilterHandle& InFilter, const FGameplayTagRequirements& Source, const FGameplayTagRequirements& Target, const FGameplayTagRequirements& Asset, const FGameplayTagRequirements& Granted, bool Once, AActor* External, bool Periodic)
{
    auto* Task = NewAbilityTask<USoloASWaitGameplayEffectAppliedTarget>(OwningAbility);
    Task->Filter = InFilter;
    Task->SourceTagRequirements = Source;
    Task->TargetTagRequirements = Target;
    Task->AssetTagRequirements = Asset;
    Task->GrantedTagRequirements = Granted;
    Task->TriggerOnce = Once;
    Task->SetExternalActor(External);
    Task->ListenForPeriodicEffects = Periodic;
    return Task;
}

UAbilityTask_WaitGameplayEffectApplied_Target* USoloASWaitGameplayEffectAppliedTarget::CreateQuery(UGameplayAbility* OwningAbility, const FGameplayTargetDataFilterHandle& InFilter, const FGameplayTagQuery& Source, const FGameplayTagQuery& Target, const FGameplayTagQuery& Asset, const FGameplayTagQuery& Granted, bool Once, AActor* External, bool Periodic)
{
    auto* Task = NewAbilityTask<USoloASWaitGameplayEffectAppliedTarget>(OwningAbility);
    Task->Filter = InFilter;
    Task->SourceTagQuery = Source;
    Task->TargetTagQuery = Target;
    Task->AssetTagQuery = Asset;
    Task->GrantedTagQuery = Granted;
    Task->TriggerOnce = Once;
    Task->SetExternalActor(External);
    Task->ListenForPeriodicEffects = Periodic;
    return Task;
}

void USoloASWaitGameplayEffectAppliedTarget::RegisterDelegate()
{
    AppliedHandle = GetASC()->OnGameplayEffectAppliedDelegateToTarget.AddUObject(this, &USoloASWaitGameplayEffectAppliedTarget::FilteredCallback);
    if (ListenForPeriodicEffects)
        PeriodicHandle = GetASC()->OnPeriodicGameplayEffectExecuteDelegateOnTarget.AddUObject(this, &USoloASWaitGameplayEffectAppliedTarget::FilteredCallback);
}
void USoloASWaitGameplayEffectAppliedTarget::RemoveDelegate()
{
    GetASC()->OnGameplayEffectAppliedDelegateToTarget.Remove(AppliedHandle);
    if (PeriodicHandle.IsValid()) GetASC()->OnPeriodicGameplayEffectExecuteDelegateOnTarget.Remove(PeriodicHandle);
}
void USoloASWaitGameplayEffectAppliedTarget::FilteredCallback(UAbilitySystemComponent* Other, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle)
{
    AActor* Avatar = Other ? Other->GetAvatarActor_Direct() : nullptr;
    if (!Filter.FilterPassesForActor(Avatar)
        || !SourceTagRequirements.RequirementsMet(*Spec.CapturedSourceTags.GetAggregatedTags())
        || !TargetTagRequirements.RequirementsMet(*Spec.CapturedTargetTags.GetAggregatedTags())) return;
    FGameplayTagContainer AssetTags, GrantedTags;
    Spec.GetAllAssetTags(AssetTags);
    Spec.GetAllGrantedTags(GrantedTags);
    if (!AssetTagRequirements.RequirementsMet(AssetTags) || !GrantedTagRequirements.RequirementsMet(GrantedTags)
        || (!SourceTagQuery.IsEmpty() && !SourceTagQuery.Matches(*Spec.CapturedSourceTags.GetAggregatedTags()))
        || (!TargetTagQuery.IsEmpty() && !TargetTagQuery.Matches(*Spec.CapturedTargetTags.GetAggregatedTags()))
        || (!AssetTagQuery.IsEmpty() && !AssetTagQuery.Matches(AssetTags))
        || (!GrantedTagQuery.IsEmpty() && !GrantedTagQuery.Matches(GrantedTags))) return;
    const UClass* EffectClass = Spec.Def ? Spec.Def->GetClass() : nullptr;
    if (BroadcastingEffects.Contains(EffectClass))
    {
        UE_LOG(LogTemp, Error, TEXT("SoloAS gameplay effect task suppressed recursive application of the same effect class"));
        return;
    }
    FGameplayEffectSpecHandle SpecHandle(new FGameplayEffectSpec(Spec));
    BroadcastingEffects.Add(EffectClass);
    Super::BroadcastDelegate(Avatar, SpecHandle, Handle);
    BroadcastingEffects.Pop(EAllowShrinking::No);
    if (TriggerOnce) EndTask();
}
