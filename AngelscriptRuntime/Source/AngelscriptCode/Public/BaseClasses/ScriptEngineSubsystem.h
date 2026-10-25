#pragma once
#include "CoreMinimal.h"
#include "Tickable.h"
#include "Subsystems/EngineSubsystem.h"
#include "ScriptEngineSubsystem.generated.h"

UCLASS(NotBlueprintable, Abstract, Meta = (NoBlueprintsOfChildren))
class ANGELSCRIPTCODE_API UScriptEngineSubsystem : public UEngineSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:

	UScriptEngineSubsystem(const FObjectInitializer& Initializer)
		: Super()
		, FTickableGameObject(ETickableTickType::Never)
	{}

	// Enable this subsystem to tick when paused
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Engine Subsystem")
	bool bIsTickableWhenPaused = true;

	// Enable this subsystem to tick in editor
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Engine Subsystem")
	bool bIsTickableInEditor = true;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override
	{
		if (IsUnreachable() || !Super::ShouldCreateSubsystem(Outer))
			return false;

		return BP_ShouldCreateSubsystem(Outer);
	}

	virtual void Initialize(FSubsystemCollectionBase& Collection) override
	{
		Super::Initialize(Collection);

		if (!IsUnreachable())
			BP_Initialize();

		check(!bInitialized);
		bInitialized = true;

		SetTickableTickType(GetTickableTickType());
	}

	virtual void Deinitialize() override
	{
		if (!IsUnreachable())
			BP_Deinitialize();

		Super::Deinitialize();

		SetTickableTickType(ETickableTickType::Never);
	}

	// FTickableGameObject implementation Begin
	virtual ETickableTickType GetTickableTickType() const override
	{
		// CDOs never tick
		if (IsTemplate())
			return ETickableTickType::Never;

		// If no tick function is implemented in the script class, don't tick
		auto* TickFunction = GetClass()->FindFunctionByName("BP_Tick");
		if (TickFunction == nullptr || TickFunction->GetOuterUClass() == UScriptEngineSubsystem::StaticClass())
			return ETickableTickType::Never;

		return ETickableTickType::Always;
	}
	
	virtual bool IsTickableWhenPaused() const override
	{
		return bIsTickableWhenPaused;
	}

	virtual bool IsTickableInEditor() const override
	{
		return bIsTickableInEditor;
	}

	virtual void Tick(float DeltaTime) override
	{
		checkf(IsInitialized(), TEXT("Ticking should have been disabled for an uninitialized subsystem : remember to call IsInitialized in the subsystem's IsTickable, IsTickableInEditor and/or IsTickableWhenPaused implementation"));

		if (!IsUnreachable())
			BP_Tick(DeltaTime);
	}
	
	virtual TStatId GetStatId() const override
	{
		return GetStatID();
	}
	// FTickableGameObject implementation End

	UFUNCTION(BlueprintNativeEvent)
	bool BP_ShouldCreateSubsystem(UObject* Outer) const;
	bool BP_ShouldCreateSubsystem_Implementation(UObject* Outer) const { return true; }

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Initialize();

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Deinitialize();

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Tick(float DeltaTime);

	bool IsInitialized() const { return bInitialized; }

private:
	bool bInitialized = false;
};