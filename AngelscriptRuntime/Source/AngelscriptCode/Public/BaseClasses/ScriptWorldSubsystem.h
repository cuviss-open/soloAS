#pragma once
#include "CoreMinimal.h"
#include "Tickable.h"
#include "Engine/World.h"
#include "Subsystems/WorldSubsystem.h"
#include "ScriptWorldSubsystem.generated.h"

UCLASS(NotBlueprintable, Abstract, Meta = (NoBlueprintsOfChildren))
class ANGELSCRIPTCODE_API UScriptWorldSubsystem : public UWorldSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

	bool bInitialized = false;

public:

	UScriptWorldSubsystem(const FObjectInitializer& Initializer)
		: Super()
		, FTickableGameObject(ETickableTickType::Never)
	{}

	// Create this subsystem for worlds representing levels loaded in the editor
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "World Subsystem")
	bool bCreateForLevelEditorWorlds = false;

	// Create this subsystem for worlds in gameplay (including Play-In-Editor)
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "World Subsystem")
	bool bCreateForGameWorlds = true;

	// Whether this subsystem should continue ticking when the game is paused
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "World Subsystem")
	bool bTickWhenPaused = false;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override
	{
		if (IsUnreachable() || !Super::ShouldCreateSubsystem(Outer))
			return false;

		UWorld* World = Cast<UWorld>(Outer);
		switch (World->WorldType)
		{
		case EWorldType::Game:
		case EWorldType::PIE:
			if (!bCreateForGameWorlds)
				return false;
		break;
		case EWorldType::Editor:
			if (!bCreateForLevelEditorWorlds)
				return false;
		break;
		default:
			return false;
		break;
		}

		return BP_ShouldCreateSubsystem(Outer);
	}

	virtual void Initialize(FSubsystemCollectionBase& Collection) override
	{
		Super::Initialize(Collection);

		bInitialized = true;
		if (!IsUnreachable())
			BP_Initialize();

		SetTickableTickType(GetTickableTickType());
	}

	virtual void Deinitialize() override
	{
		if (!IsUnreachable() && bInitialized)
			BP_Deinitialize();
		bInitialized = false;
		Super::Deinitialize();

		SetTickableTickType(ETickableTickType::Never);
	}

	virtual void PostInitialize() override
	{
		Super::PostInitialize();

		if (!IsUnreachable())
			BP_PostInitialize();
	}

	virtual void Tick(float DeltaTime) override
	{
		check(!IsUnreachable());
		BP_Tick(DeltaTime);
	}

	virtual void OnWorldBeginPlay(UWorld& InWorld) override
	{
		Super::OnWorldBeginPlay(InWorld);

		if (!IsUnreachable())
			BP_OnWorldBeginPlay();
	}

	virtual void OnWorldComponentsUpdated(UWorld& InWorld) override
	{
		Super::OnWorldComponentsUpdated(InWorld);

		if (!IsUnreachable())
			BP_OnWorldComponentsUpdated();
	}

	UFUNCTION(BlueprintNativeEvent)
	bool BP_ShouldCreateSubsystem(UObject* Outer) const;
	bool BP_ShouldCreateSubsystem_Implementation(UObject* Outer) const { return true; }

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Initialize();

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Deinitialize();

	UFUNCTION(BlueprintImplementableEvent)
	void BP_PostInitialize();

	UFUNCTION(BlueprintImplementableEvent)
	void BP_OnWorldBeginPlay();

	UFUNCTION(BlueprintImplementableEvent)
	void BP_OnWorldComponentsUpdated();

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Tick(float DeltaTime);

	virtual UWorld* GetTickableGameObjectWorld() const override { return GetWorld(); }

	virtual ETickableTickType GetTickableTickType() const override
	{
		// CDOs never tick
		if (IsTemplate())
			return ETickableTickType::Never;

		// If no tick function is implemented in the script class, don't tick
		auto* TickFunction = GetClass()->FindFunctionByName("BP_Tick");
		if (TickFunction == nullptr || TickFunction->GetOuterUClass() == UScriptWorldSubsystem::StaticClass())
			return ETickableTickType::Never;

		return ETickableTickType::Always;
	}

	virtual TStatId GetStatId() const override
	{
		return GetStatID();
	}

	virtual bool IsTickableInEditor() const override
	{
		return bCreateForLevelEditorWorlds && (GetWorld() == nullptr || !GetWorld()->IsGameWorld());
	}

	virtual bool IsTickableWhenPaused() const override
	{
		return bTickWhenPaused;
	}

};