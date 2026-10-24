#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectTypes.h"
#include "Engine/HitResult.h"

#include "GameplayEffectContextHandleMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FGameplayEffectContextHandle"))
class ANGELSCRIPTGAS_API UGameplayEffectContextHandleMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Ability System | Context", meta=(ScriptAllowTemporaryThis))
	static void Clear(FGameplayEffectContextHandle& Handle)
	{
		Handle.Clear();
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Context", meta=(ScriptAllowTemporaryThis))
	static void AddInstigator(FGameplayEffectContextHandle& Handle, AActor* InInstigator, AActor* InEffectCauser)
	{
		if (Handle.IsValid()) 
		{ 
			Handle.AddInstigator(InInstigator, InEffectCauser); 
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Context", meta=(ScriptAllowTemporaryThis))
	static void SetAbility(FGameplayEffectContextHandle& Handle, const UGameplayAbility* InGameplayAbility)
	{
		if (Handle.IsValid()) 
		{ 
			Handle.SetAbility(InGameplayAbility); 
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Context")
	static const UGameplayAbility* GetAbility(const FGameplayEffectContextHandle& Handle)
	{
		return (Handle.IsValid()) ? Handle.GetAbility() : nullptr;
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Context", meta=(ScriptAllowTemporaryThis))
	static void SetEffectCauser(FGameplayEffectContextHandle& Handle, AActor* NewEffectCauser)
	{
		if (Handle.IsValid())
		{
			Handle.Get()->SetEffectCauser(NewEffectCauser);
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Context", meta=(ScriptAllowTemporaryThis))
	static void AddSourceObject(FGameplayEffectContextHandle& Handle, UObject* NewSourceObject)
	{
		if (Handle.IsValid()) 
		{ 
			Handle.AddSourceObject(NewSourceObject); 
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Context", meta=(ScriptAllowTemporaryThis))
	static void AddHitResult(FGameplayEffectContextHandle& Handle, const FHitResult& InHitResult, bool bReset)
	{
		if (Handle.IsValid()) 
		{
			Handle.AddHitResult(InHitResult, bReset); 
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Context", meta=(ScriptAllowTemporaryThis))
	static void AddOrigin(FGameplayEffectContextHandle& Handle, FVector InOrigin)
	{
		if (Handle.IsValid()) 
		{ 
			Handle.AddOrigin(InOrigin); 
		}
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Context", meta=(ScriptAllowTemporaryThis))
	static void AddActors(FGameplayEffectContextHandle& Handle, const TArray<AActor*>& InActors, bool bReset)
	{
		if (!Handle.IsValid())
		{
			return;
		}

		TArray<TWeakObjectPtr<AActor>> ActorsToAdd;
		for (AActor* ActorPtr : InActors)
		{
			if (ActorPtr)
			{
				ActorsToAdd.Add(ActorPtr);
			}
		}

		Handle.AddActors(ActorsToAdd, bReset);
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Context")
	static TArray<AActor*> GetActors(const FGameplayEffectContextHandle& Handle)
	{
		TArray<AActor*> OutArray;
		if (!Handle.IsValid())
		{
			return OutArray;
		}

		for (const TWeakObjectPtr<AActor>& ActorPtr : Handle.Get()->GetActors())
		{
			if (ActorPtr.IsValid())
			{
				OutArray.Add(ActorPtr.Get());
			}
		}

		return OutArray;
	}

	UFUNCTION(BlueprintCallable, Category = "Ability System | Context")
	static void GetOwnedGameplayTags(const FGameplayEffectContextHandle& Handle, FGameplayTagContainer& ActorTagContainer, FGameplayTagContainer& SpecTagContainer)
	{
		if (Handle.IsValid()) 
		{ 
			Handle.GetOwnedGameplayTags(ActorTagContainer, SpecTagContainer); 
		}
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static bool IsValid(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid();
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static AActor* GetInstigator(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid() ? Handle.GetInstigator() : nullptr;
	}

                                                                                                                                         
#if 0
                                                                                                                                       
	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static int32 GetAbilityLevel(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid() ? Handle.GetAbilityLevel() : 0;
	}
                                                                                                                                   
#endif
	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static int32 GetAbilityLevel(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.GetAbilityLevel();
	}
                                                                                                                                 

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static AActor* GetEffectCauser(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid() ? Handle.GetEffectCauser() : nullptr;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static AActor* GetOriginalInstigator(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid() ? Handle.GetOriginalInstigator() : nullptr;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static UAbilitySystemComponent* GetOriginalInstigatorAbilitySystemComponent(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid() ? Handle.GetOriginalInstigatorAbilitySystemComponent() : nullptr;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static UObject* GetSourceObject(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid() ? Handle.GetSourceObject() : nullptr;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static bool IsLocallyControlled(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid() ? Handle.IsLocallyControlled() : false;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static bool IsLocallyControlledPlayer(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid() ? Handle.IsLocallyControlledPlayer() : false;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static FVector GetOrigin(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid() ? Handle.GetOrigin() : FVector::ZeroVector;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static bool HasOrigin(const FGameplayEffectContextHandle& Handle)
	{
		return Handle.IsValid() ? Handle.HasOrigin() : false;
	}

	UFUNCTION(BlueprintPure, Category = "Ability System | Context")
	static bool GetHitResult(const FGameplayEffectContextHandle& Handle, FHitResult& OutHitResult)
	{
		if (!Handle.IsValid())
		{
			return false;
		}

		const FHitResult* HitResultPtr = Handle.GetHitResult();
		if (HitResultPtr)
		{
			OutHitResult = *HitResultPtr;
			return true;
		}

		return false;
	}
};
