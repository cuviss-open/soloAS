// SPDX-License-Identifier: (Apache-2.0 OR MIT)
// Copyright Cuviss Studio.
#include "AngelscriptBinds.h"
#include "SoloASEngineCompat.h"
#include "AngelscriptManager.h"
#include "Mixin/AbilitySystemComponentMixinLibrary.h"

// SOLOAS-COMPAT-BEGIN(<5.8) native ability tag query was not reflected [test: GasAbilityTagsBlocked]
#if !SOLOAS_UE_AT_LEAST(5, 8)
AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_AbilityTagsBlocked(
    (int32)FAngelscriptBinds::EOrder::Late + 101, []
{
    auto Component = FAngelscriptBinds::ExistingClass("UAbilitySystemComponent");
    Component.Method("bool AreAbilityTagsBlocked(const FGameplayTagContainer& Tags) const",
        &UAbilitySystemComponent::AreAbilityTagsBlocked);
});
#endif
// SOLOAS-COMPAT-END

// SOLOAS-COMPAT-BEGIN(<5.7) expose the latest API without silently degrading unsupported replication [test: GasLooseTagReplication]
#if !SOLOAS_UE_AT_LEAST(5, 7)
AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_LegacyGameplayTagReplicationState(FAngelscriptBinds::EOrder::Early, []
{
    auto State = FAngelscriptBinds::Enum("EGameplayTagReplicationState");
    State["None"] = EGameplayTagReplicationState::None;
    State["SimulatedTagOnly"] = EGameplayTagReplicationState::SimulatedTagOnly;
    State["TagOnly"] = EGameplayTagReplicationState::TagOnly;
    State["CountToOwner"] = EGameplayTagReplicationState::CountToOwner;
    State["TagAndCountToAll"] = EGameplayTagReplicationState::TagAndCountToAll;
});
AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_AbilitySystemLooseTags(
    (int32)FAngelscriptBinds::EOrder::Late + 101, []
{
    auto Component = FAngelscriptBinds::ExistingClass("UAbilitySystemComponent");
    Component.Method("void AddLooseGameplayTag(const FGameplayTag& GameplayTag, int Count = 1, EGameplayTagReplicationState TagRepState = EGameplayTagReplicationState::None)",
        &UAbilitySystemComponentMixinLibrary::AddLooseGameplayTag);
    Component.Method("void AddLooseGameplayTags(const FGameplayTagContainer& GameplayTags, int Count = 1, EGameplayTagReplicationState TagRepState = EGameplayTagReplicationState::None)",
        &UAbilitySystemComponentMixinLibrary::AddLooseGameplayTags);
    Component.Method("void RemoveLooseGameplayTag(const FGameplayTag& GameplayTag, int Count = 1, EGameplayTagReplicationState TagRepState = EGameplayTagReplicationState::None)",
        &UAbilitySystemComponentMixinLibrary::RemoveLooseGameplayTag);
    Component.Method("void RemoveLooseGameplayTags(const FGameplayTagContainer& GameplayTags, int Count = 1, EGameplayTagReplicationState TagRepState = EGameplayTagReplicationState::None)",
        &UAbilitySystemComponentMixinLibrary::RemoveLooseGameplayTags);
    Component.Method("void SetLooseGameplayTagCount(const FGameplayTag& GameplayTag, int NewCount, EGameplayTagReplicationState TagRepState = EGameplayTagReplicationState::None)",
        &UAbilitySystemComponentMixinLibrary::SetLooseGameplayTagCount);
});
#endif
// SOLOAS-COMPAT-END

                                                                                          
// SOLOAS-COMPAT-BEGIN(<5.7) native tag-only replication with unsupported modes rejected [test: GasLooseTagReplication]
#if !SOLOAS_UE_AT_LEAST(5, 7)
namespace
{
// Keep the native per-tag promotion rule until its explicit count reaches zero.
TMap<TWeakObjectPtr<UAbilitySystemComponent>, TMap<FGameplayTag, bool>> LegacyReplicatedTags;

bool CheckLegacyReplicationState(EGameplayTagReplicationState State)
{
    if (State == EGameplayTagReplicationState::None || State == EGameplayTagReplicationState::TagOnly)
        return true;
    if (State == EGameplayTagReplicationState::SimulatedTagOnly)
        FAngelscriptManager::Throw("EGameplayTagReplicationState::SimulatedTagOnly requires UE 5.8 or newer; UE 5.4-5.6 support only None and TagOnly.");
    else if (State == EGameplayTagReplicationState::CountToOwner)
        FAngelscriptManager::Throw("EGameplayTagReplicationState::CountToOwner requires UE 5.7 or newer; UE 5.4-5.6 cannot replicate tag counts.");
    else if (State == EGameplayTagReplicationState::TagAndCountToAll)
        FAngelscriptManager::Throw("EGameplayTagReplicationState::TagAndCountToAll requires UE 5.7 or newer; UE 5.4-5.6 cannot replicate tag counts.");
    else
        FAngelscriptManager::Throw("Invalid EGameplayTagReplicationState; UE 5.4-5.6 support only None and TagOnly.");
    return false;
}

int32 ExplicitTagCount(UAbilitySystemComponent* Component, const FGameplayTag& Tag)
{
    int32 Count = Component->GetTagCount(Tag);
    TSet<FGameplayTag> Children;
    for (FGameplayTag Child : Component->GetOwnedGameplayTags())
    {
        if (Child == Tag || !Child.MatchesTag(Tag))
            continue;
        while (Child.RequestDirectParent() != Tag)
            Child = Child.RequestDirectParent();
        Children.Add(Child);
    }
    for (const FGameplayTag& Child : Children)
        Count -= Component->GetTagCount(Child);
    return Count;
}

void PrepareLegacyReplication(UAbilitySystemComponent* Component, const FGameplayTag& Tag, int32 Delta, EGameplayTagReplicationState State)
{
    if (State != EGameplayTagReplicationState::TagOnly || Delta <= 0 || !Tag.IsValid())
        return;
    for (auto It = LegacyReplicatedTags.CreateIterator(); It; ++It)
        if (!It.Key().IsValid()) It.RemoveCurrent();
    auto& Tags = LegacyReplicatedTags.FindOrAdd(Component);
    if (!Tags.Contains(Tag))
    {
        Tags.Add(Tag, false);
        Component->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::AnyCountChange).AddWeakLambda(Component,
            [Component, Tag](const FGameplayTag&, int32)
        {
            auto* States = LegacyReplicatedTags.Find(Component);
            bool* Replicated = States ? States->Find(Tag) : nullptr;
            if (!Replicated || !*Replicated) return;
            const bool Exists = Component->GetOwnedGameplayTags().HasTagExact(Tag);
            Component->SetReplicatedLooseGameplayTagCount(Tag, Exists ? 1 : 0);
            if (!Exists) *Replicated = false;
        });
    }
    Tags[Tag] = true;
}
}

void UAbilitySystemComponentMixinLibrary::AddLooseGameplayTag(UAbilitySystemComponent* Component, const FGameplayTag& Tag, int32 Count, EGameplayTagReplicationState State)
{
    if (!CheckLegacyReplicationState(State) || !Component) return;
    PrepareLegacyReplication(Component, Tag, Count, State);
    Component->AddLooseGameplayTag(Tag, Count);
}

void UAbilitySystemComponentMixinLibrary::AddLooseGameplayTags(UAbilitySystemComponent* Component, const FGameplayTagContainer& Tags, int32 Count, EGameplayTagReplicationState State)
{
    if (!CheckLegacyReplicationState(State) || !Component) return;
    for (const FGameplayTag& Tag : Tags) PrepareLegacyReplication(Component, Tag, Count, State);
    Component->AddLooseGameplayTags(Tags, Count);
}

void UAbilitySystemComponentMixinLibrary::RemoveLooseGameplayTag(UAbilitySystemComponent* Component, const FGameplayTag& Tag, int32 Count, EGameplayTagReplicationState State)
{
    if (!CheckLegacyReplicationState(State) || !Component) return;
    PrepareLegacyReplication(Component, Tag, -Count, State);
    Component->RemoveLooseGameplayTag(Tag, Count);
}

void UAbilitySystemComponentMixinLibrary::RemoveLooseGameplayTags(UAbilitySystemComponent* Component, const FGameplayTagContainer& Tags, int32 Count, EGameplayTagReplicationState State)
{
    if (!CheckLegacyReplicationState(State) || !Component) return;
    for (const FGameplayTag& Tag : Tags) PrepareLegacyReplication(Component, Tag, -Count, State);
    Component->RemoveLooseGameplayTags(Tags, Count);
}

void UAbilitySystemComponentMixinLibrary::SetLooseGameplayTagCount(UAbilitySystemComponent* Component, const FGameplayTag& Tag, int32 Count, EGameplayTagReplicationState State)
{
    if (!CheckLegacyReplicationState(State) || !Component) return;
    PrepareLegacyReplication(Component, Tag, Count - ExplicitTagCount(Component, Tag), State);
    Component->SetLooseGameplayTagCount(Tag, Count);
}
#endif
// SOLOAS-COMPAT-END
                                                                                        
