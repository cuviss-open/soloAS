// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptActorConstruction.h"
#include "ClassGenerator/ASClass.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
void RegisterWithAttachParents(UActorComponent* Component)
{
    if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
    {
        USceneComponent* Parent = Scene->GetAttachParent();
        if (Parent && Parent->GetOwner() == Scene->GetOwner() && !Parent->IsRegistered())
            RegisterWithAttachParents(Parent);
    }
    if (IsValid(Component) && !Component->IsRegistered() && Component->bAutoRegister)
        Component->RegisterComponent();
}
}
namespace AngelscriptActorConstruction
{
bool IsPureScriptActor(const AActor* Actor)
{
    const UASClass* Class = Actor ? Cast<UASClass>(Actor->GetClass()) : nullptr;
    return Class && !Actor->IsTemplate() && Class->HasValidScriptClassCarrier();
}
void Prepare(AActor* Actor, const FTransform* Transform, const FRotationConversionCache* RotationCache, bool bDefaultTransform)
{
    if (!IsPureScriptActor(Actor) || !Actor->GetWorld()) return;
    UWorld* World = Actor->GetWorld();
    TInlineComponentArray<UActorComponent*> Components;
    Actor->GetComponents(Components);
    for (UActorComponent* Component : Components)
    {
        USceneComponent* Scene = Cast<USceneComponent>(Component);
        if (Scene && Scene->CreationMethod == EComponentCreationMethod::Native
            && Scene->GetOuter()->IsA<AActor>() && !Actor->GetRootComponent() && !Scene->GetAttachParent())
        {
            Actor->SetRootComponent(Scene);
            if (Transform && !bDefaultTransform)
            {
                if (RotationCache) Scene->SetRelativeRotationCache(*RotationCache);
                Scene->SetWorldTransform(*Transform, false, nullptr, ETeleportType::TeleportPhysics);
            }
        }
    }
    if (Actor->HasDeferredComponentRegistration() && World->bIsWorldInitialized) Actor->RegisterAllComponents();
    // Pure AS has no SCS nodes. Native/script-constructor components were already registered
    // by PostSpawnInitialize; retained SCS components need the same final registration pass.
    for (UActorComponent* Component : Components)
        if (!Component->IsRegistered() && Component->bAutoRegister && IsValid(Component)
            && (World->bIsWorldInitialized || Actor->HasActorRegisteredAllComponents())
            && Component->CreationMethod == EComponentCreationMethod::SimpleConstructionScript)
            RegisterWithAttachParents(Component);
}
}
