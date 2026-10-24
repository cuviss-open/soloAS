#pragma once
#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Editor.h"
#include "ScriptEditorSubsystem.generated.h"

UCLASS(NotBlueprintable, Abstract, Meta = (NoBlueprintsOfChildren))
class ANGELSCRIPTEDITOR_API UScriptEditorSubsystem : public UEditorSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

	bool bSubsystemInitialized = false;

public:

	UScriptEditorSubsystem(const FObjectInitializer& Initializer)
		: Super()
		, FTickableGameObject(IsTemplate() ? ETickableTickType::Never : ETickableTickType::NewObject)
	{}

	virtual UWorld* GetWorld() const override final
	{
		return (GEditor != nullptr) ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override
	{
		if (IsUnreachable())
			return false;
		return BP_ShouldCreateSubsystem(Outer);
	}

	virtual void Initialize(FSubsystemCollectionBase& Collection) override
	{
		bSubsystemInitialized = true;
		if (!IsUnreachable())
			BP_Initialize();
	}

	virtual void Deinitialize() override
	{
		bSubsystemInitialized = false;
		if (!IsUnreachable())
			BP_Deinitialize();
	}

	UFUNCTION(BlueprintNativeEvent)
	bool BP_ShouldCreateSubsystem(UObject* Outer) const;
	bool BP_ShouldCreateSubsystem_Implementation(UObject* Outer) const { return true; }

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Initialize();

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Deinitialize();

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Tick(float DeltaTime);

	// FTickableGameObject implementation Begin
	virtual UWorld* GetTickableGameObjectWorld() const override
	{
		return nullptr;
	}

	virtual ETickableTickType GetTickableTickType() const override
	{
		return IsTemplate() ? ETickableTickType::Never : FTickableGameObject::GetTickableTickType(); 
	}

	virtual bool IsTickableInEditor() const override
	{
		return true;
	}

	virtual bool IsAllowedToTick() const override final
	{
		return !IsTemplate() && bSubsystemInitialized && !IsUnreachable();
	}

	virtual void Tick(float DeltaTime) override
	{
		FEditorScriptExecutionGuard ScriptGuard;
		BP_Tick(DeltaTime);
	}

	virtual TStatId GetStatId() const override
	{
		return GetStatID();
	}
	// FTickableGameObject implementation End

};