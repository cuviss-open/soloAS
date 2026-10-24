// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptBinds.h"

#include "Components/Widget.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerInput.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind
    Bind_CallableReferenceReturns(FAngelscriptBinds::EOrder::Late, []
{
    auto ActorBinds = FAngelscriptBinds::ExistingClass("AActor");
    ActorBinds.Method(
        "const TSet<UActorComponent>& GetComponents() const",
        [](const AActor* Actor) -> const TSet<UActorComponent*>&
        {
            return Actor->GetComponents();
        }
    );

    auto PlayerBinds = FAngelscriptBinds::ExistingClass("UPlayerInput");
    PlayerBinds.Method(
        "const TArray<FInputActionKeyMapping>& GetKeysForAction(const FName ActionName)",
        [](UPlayerInput* PlayerInput, const FName ActionName)
            -> const TArray<FInputActionKeyMapping>&
        {
            return PlayerInput->GetKeysForAction(ActionName);
        }
    );
    PlayerBinds.Method(
        "const TArray<FInputAxisKeyMapping>& GetKeysForAxis(const FName AxisName)",
        [](UPlayerInput* PlayerInput, const FName AxisName)
            -> const TArray<FInputAxisKeyMapping>&
        {
            return PlayerInput->GetKeysForAxis(AxisName);
        }
    );
    PlayerBinds.Method(
        "const TArray<FInputActionKeyMapping>& GetEngineDefinedActionMappings(const FName ActionName)",
        [](UPlayerInput* PlayerInput, const FName ActionName)
            -> const TArray<FInputActionKeyMapping>&
        {
            static_cast<void>(ActionName);
            return PlayerInput->GetEngineDefinedActionMappings();
        }
    );
    PlayerBinds.Method(
        "const TArray<FInputAxisKeyMapping>& GetEngineDefinedAxisMappings(const FName AxisName)",
        [](UPlayerInput* PlayerInput, const FName AxisName)
            -> const TArray<FInputAxisKeyMapping>&
        {
            static_cast<void>(AxisName);
            return PlayerInput->GetEngineDefinedAxisMappings();
        }
    );

    auto WidgetBinds = FAngelscriptBinds::ExistingClass("UWidget");
    WidgetBinds.Method(
        "const FWidgetTransform& GetRenderTransform() const",
        [](const UWidget* Widget) -> const FWidgetTransform&
        {
            return Widget->GetRenderTransform();
        }
    );
});
