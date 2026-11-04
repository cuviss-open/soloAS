#include "Runtime/SoloASSceneComponentAttachment.h"

#include "Components/SceneComponent.h"
#include "Runtime/Launch/Resources/Version.h"
#include "SoloASEngineCompat.h"
#include "GameFramework/Actor.h"
#include "Shared/SoloASPrivateAccess.h"
#include "UObject/UObjectGlobals.h"

namespace SoloASBridge::PrivateAccess
{
struct FAttachChildrenTag
{
    using Type = TArray<TObjectPtr<USceneComponent>> USceneComponent::*;
    friend Type Resolve(FAttachChildrenTag) noexcept;
};
struct FClientAttachChildrenTag
{
    using Type = TArray<TObjectPtr<USceneComponent>> USceneComponent::*;
    friend Type Resolve(FClientAttachChildrenTag) noexcept;
};
struct FModifiedAttachChildrenTag
{
    using Type = void (USceneComponent::*)();
    friend Type Resolve(FModifiedAttachChildrenTag) noexcept;
};
template struct TMember<FAttachChildrenTag, &USceneComponent::AttachChildren>;
template struct TMember<FClientAttachChildrenTag, &USceneComponent::ClientAttachedChildren>;
template struct TMember<FModifiedAttachChildrenTag, &USceneComponent::ModifiedAttachChildren>;
}

bool SoloASBridge::NormalizeSceneComponentChildren(USceneComponent* Parent)
{
#if SOLOASBRIDGE_ENGINE_PROFILE_MATCHED
    if (!IsInGameThread() || IsGarbageCollecting() || !IsValid(Parent) || Parent->IsBeingDestroyed())
        return false;
    auto Normalize = [Parent](TArray<TObjectPtr<USceneComponent>>& Children)
    {
        bool bChanged = false;
        TSet<USceneComponent*> Seen;
        for (int32 Index = 0; Index < Children.Num();)
        {
            USceneComponent* Child = Children[Index].Get();
            if (!IsValid(Child) || Child->IsBeingDestroyed() || Child->GetAttachParent() != Parent
                || Seen.Contains(Child))
            {
                Children.RemoveAt(Index, 1, EAllowShrinking::No);
                bChanged = true;
            }
            else
            {
                Seen.Add(Child);
                ++Index;
            }
        }
        return bChanged;
    };
    const bool bChanged = Normalize(PrivateAccess::Get<PrivateAccess::FAttachChildrenTag>(*Parent));
    Normalize(PrivateAccess::Get<PrivateAccess::FClientAttachChildrenTag>(*Parent));
    if (bChanged) PrivateAccess::Call<PrivateAccess::FModifiedAttachChildrenTag>(*Parent);
    return true;
#else
    return false;
#endif
}

// SOLOAS-COMPAT-BEGIN(<5.8) CS.Compatibility.Bridge.SceneDestroy [test: SoloAS.SceneGcReentrantAttachment] [id: CS.Compatibility.Bridge.SceneDestroy]
#if WITH_EDITOR && SOLOAS_UE_BEFORE(5, 8)
void SoloASBridge::RemoveRetiredSceneComponentStaleChildren(USceneComponent* Parent)
{
    if (IsValid(Parent->GetOwner())
        && !Parent->GetClass()->HasAnyClassFlags(CLASS_NewerVersionExists)) return;
    auto& Children = PrivateAccess::Get<PrivateAccess::FAttachChildrenTag>(*Parent);
    for (int32 Index = Children.Num() - 1; Index >= 0; --Index)
    {
        USceneComponent* Child = Children[Index].Get();
        if (Child && Child->GetAttachParent() != Parent)
            Children.RemoveAt(Index, 1, EAllowShrinking::No);
    }
}
#endif
// SOLOAS-COMPAT-END
