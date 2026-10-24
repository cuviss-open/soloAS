#pragma once

#include "CoreMinimal.h"

#include "GameplayEffectTypes.h"

#include "GameplayCueParametersMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FGameplayCueParameters"))
class ANGELSCRIPTGAS_API UGameplayCueParametersMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Gameplay Cue")
	static void SetInstigator(FGameplayCueParameters& GameplayCueParams, AActor* Instigator)
	{
		if (Instigator)
		{
			GameplayCueParams.Instigator = Instigator;
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Cue")
	static void SetEffectCauser(FGameplayCueParameters& GameplayCueParams, AActor* EffectCauser)
	{
		if (EffectCauser)
		{
			GameplayCueParams.EffectCauser = EffectCauser;
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Cue")
	static void SetSourceObject(FGameplayCueParameters& GameplayCueParams, const UObject* SourceObject)
	{
		if (SourceObject)
		{
			GameplayCueParams.SourceObject = SourceObject;
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Cue")
	static void SetPhysicalMaterial(FGameplayCueParameters& GameplayCueParams, const UPhysicalMaterial* PhysicalMaterial)
	{
		if (PhysicalMaterial)
		{
			GameplayCueParams.PhysicalMaterial = PhysicalMaterial;
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Cue")
	static void SetTargetAttachComponent(FGameplayCueParameters& GameplayCueParams, USceneComponent* TargetAttachComponent)
	{
		if (TargetAttachComponent)
		{
			GameplayCueParams.TargetAttachComponent = TargetAttachComponent;
		}
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Cue")
	static AActor* GetInstigator(const FGameplayCueParameters& GameplayCueParams)
	{
		return GameplayCueParams.GetInstigator();
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Cue")
	static AActor* GetEffectCauser(const FGameplayCueParameters& GameplayCueParams)
	{
		return GameplayCueParams.GetEffectCauser();
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Cue")
	static const UObject* GetSourceObject(const FGameplayCueParameters& GameplayCueParams)
	{
		return GameplayCueParams.SourceObject.Get();
	}

	UFUNCTION(BlueprintCallable, Category = "Gameplay Cue")
	static const UPhysicalMaterial* GetPhysicalMaterial(const FGameplayCueParameters& GameplayCueParams)
	{
		return GameplayCueParams.PhysicalMaterial.Get();
	}

	UFUNCTION(BlueprintPure, Category = "Gameplay Cue")
	static USceneComponent* GetTargetAttachComponent(const FGameplayCueParameters& GameplayCueParams)
	{
		return GameplayCueParams.TargetAttachComponent.Get();
	}
};
