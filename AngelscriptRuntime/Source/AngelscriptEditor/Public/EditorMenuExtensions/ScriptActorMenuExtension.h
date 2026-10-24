#pragma once
#include "ScriptEditorMenuExtension.h"
                                                                                                             
#include "GameFramework/Actor.h"
                                                                                                           
#include "Templates/SubclassOf.h"
#include "ScriptActorMenuExtension.generated.h"

UCLASS(BlueprintType)
class UScriptActorMenuExtension : public UScriptEditorMenuExtension
{
	GENERATED_BODY()
public:

	UScriptActorMenuExtension()
	{
		ExtensionMenu = EScriptEditorMenuExtensionLocation::LevelViewport_ContextMenu;
		ExtensionPoint = "ActorPreview";
		ExtensionOrder = EScriptEditorMenuExtensionOrder::Before;
	}

	UPROPERTY(EditDefaultsOnly, Category = "Menu Extension")
	TArray<TSubclassOf<AActor>> SupportedClasses;

	UFUNCTION(BlueprintNativeEvent)
	bool SupportsActor(AActor* Actor) const;
	bool SupportsActor_Implementation(AActor* Actor) const { return true; }

	UFUNCTION(ScriptCallable, Meta = (NotAngelscriptProperty))
	AActor* GetSelectedActor() const
	{
		if (!CurrentSelection.SelectedObjects.IsEmpty())
			return Cast<AActor>(CurrentSelection.SelectedObjects[0].Get());
		return nullptr;
	}

	UFUNCTION(ScriptCallable, Meta = (NotAngelscriptProperty))
	TArray<AActor*> GetSelectedActors() const
	{
		TArray<AActor*> Actors;
		for (auto Object : CurrentSelection.SelectedObjects)
		{
			AActor* Actor = Cast<AActor>(Object.Get());
			if (Actor != nullptr)
				Actors.Add(Actor);
		}

		return Actors;
	}

	UFUNCTION(ScriptCallable, Meta = (DeterminesOutputType = "ActorClass"))
	TArray<AActor*> GetSelectedActorsOfClass(TSubclassOf<AActor> ActorClass) const
	{
		TArray<AActor*> Actors;
		if (ActorClass == nullptr)
			return Actors;

		for (auto Object : CurrentSelection.SelectedObjects)
		{
			AActor* Actor = Cast<AActor>(Object.Get());
			if (Actor != nullptr && Actor->IsA(ActorClass))
				Actors.Add(Actor);
		}

		return Actors;
	}

	virtual TArray<UFunction*> GatherExtensionFunctions() const;
	virtual void CallFunctionOnSelection(UFunction* Function, FExtenderSelection Selection) const;
};
