#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "UObjectTickable.generated.h"

UCLASS(Blueprintable)
class ANGELSCRIPTCODE_API UObjectTickable : public UObject, public FTickableGameObject
{
	GENERATED_BODY()

public:

	UObjectTickable(const FObjectInitializer& Initializer)
		: Super(Initializer)
		, FTickableGameObject(IsTemplate() ? ETickableTickType::Never : ETickableTickType::NewObject)
	{}

	UPROPERTY(ScriptReadWrite)
	TObjectPtr<UWorld> World;

	UPROPERTY(EditDefaultsOnly, Category = "Tickable Object")
	bool bTickWhilePaused = false;

	virtual UWorld* GetWorld() const override;

	UFUNCTION(ScriptCallable)
	void SetWorld(UWorld* InWorld);

	UFUNCTION(ScriptCallable)
	void SetWorldContext(UObject* WorldContext);

	UFUNCTION(ScriptCallable)
	void DestroyObject()
	{
		SetTickableTickType(ETickableTickType::Never);
		MarkAsGarbage();
	}

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Tick(float DeltaTime);

	bool IsTickableWhenPaused() const override { return bTickWhilePaused; }
	UWorld* GetTickableGameObjectWorld() const { return GetWorld(); }
	bool IsTickable() const override { return !HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed); }

	void Tick(float DeltaTime) override;
	TStatId GetStatId() const override;
};
