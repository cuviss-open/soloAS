#pragma once

#include "CoreMinimal.h"

#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbilityTypes.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/MovementComponent.h"

#include "GameplayAbilityActorInfoMixinLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FGameplayAbilityActorInfo"))
class ANGELSCRIPTGAS_API UGameplayAbilityActorInfoMixinLibrary : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(ScriptCallable)
	static void SetOwnerActor(FGameplayAbilityActorInfo& Info, AActor* Actor)
	{
		Info.OwnerActor = Actor;
	}

	UFUNCTION(ScriptCallable)
	static AActor* GetOwnerActor(const FGameplayAbilityActorInfo& Info)
	{
		return Info.OwnerActor.IsValid() ? Info.OwnerActor.Get() : nullptr;
	}

	UFUNCTION(ScriptCallable)
	static void SetAvatarActor(FGameplayAbilityActorInfo& Info, AActor* Actor)
	{
		Info.SetAvatarActor(Actor);
	}
	
	UFUNCTION(ScriptCallable)
	static AActor* GetAvatarActor(const FGameplayAbilityActorInfo& Info)
	{
		return Info.AvatarActor.IsValid() ? Info.AvatarActor.Get() : nullptr;
	}

	UFUNCTION(ScriptCallable)
	static void SetPlayerController(FGameplayAbilityActorInfo& Info, APlayerController* Controller)
	{
		Info.PlayerController = Controller;
	}

	UFUNCTION(ScriptCallable)
	static APlayerController* GetPlayerController(const FGameplayAbilityActorInfo& Info)
	{
		return Info.PlayerController.IsValid() ? Info.PlayerController.Get() : nullptr;
	}

	UFUNCTION(ScriptCallable)
	static void SetAbilitySystemComponent(FGameplayAbilityActorInfo& Info, UAbilitySystemComponent* Component)
	{
		Info.AbilitySystemComponent = Component;
	}

	UFUNCTION(ScriptCallable)
	static UAbilitySystemComponent* GetAbilitySystemComponent(const FGameplayAbilityActorInfo& Info)
	{
		return Info.AbilitySystemComponent.IsValid() ? Info.AbilitySystemComponent.Get() : nullptr;
	}

	UFUNCTION(ScriptCallable)
	static void SetSkeletalMeshComponent(FGameplayAbilityActorInfo& Info, USkeletalMeshComponent* Component)
	{
		Info.SkeletalMeshComponent = Component;
	}

	UFUNCTION(ScriptCallable)
	static USkeletalMeshComponent* GetSkeletalMeshComponent(const FGameplayAbilityActorInfo& Info)
	{
		return Info.SkeletalMeshComponent.IsValid() ? Info.SkeletalMeshComponent.Get() : nullptr;
	}

	UFUNCTION(ScriptCallable)
	static void SetAnimInstance(FGameplayAbilityActorInfo& Info, UAnimInstance* Instance)
	{
		Info.AnimInstance = Instance;
	}

	UFUNCTION(ScriptCallable)
	static UAnimInstance* GetAnimInstance(const FGameplayAbilityActorInfo& Info)
	{
		return Info.AnimInstance.IsValid() ? Info.AnimInstance.Get() : nullptr;
	}

	UFUNCTION(ScriptCallable)
	static void SetMovementComponent(FGameplayAbilityActorInfo& Info, UMovementComponent* Instance)
	{
		Info.MovementComponent = Instance;
	}

	UFUNCTION(ScriptCallable)
	static UMovementComponent* GetMovementComponent(const FGameplayAbilityActorInfo& Info)
	{
		return Info.MovementComponent.IsValid() ? Info.MovementComponent.Get() : nullptr;
	}

	UFUNCTION(ScriptCallable)
	static void SetAffectedAnimInstanceTag(FGameplayAbilityActorInfo& Info, FName Name)
	{
		Info.AffectedAnimInstanceTag = Name;
	}

	UFUNCTION(ScriptCallable)
	static FName GetAffectedAnimInstanceTag(const FGameplayAbilityActorInfo& Info)
	{
		return Info.AffectedAnimInstanceTag;
	}

	UFUNCTION(ScriptCallable, meta = (NotAngelscriptProperty))
	static UAnimInstance* GetAnimInstanceFromSkeletalMesh(const FGameplayAbilityActorInfo& Info)
	{
		return Info.GetAnimInstance();
	}

	UFUNCTION(ScriptCallable)
	static bool IsLocallyControlled(const FGameplayAbilityActorInfo& Info)
	{
		return Info.IsLocallyControlled();
	}

	UFUNCTION(ScriptCallable)
	static bool IsLocallyControlledPlayer(const FGameplayAbilityActorInfo& Info)
	{
		return Info.IsLocallyControlledPlayer();
	}

	UFUNCTION(ScriptCallable)
	static bool IsNetAuthority(const FGameplayAbilityActorInfo& Info)
	{
		return Info.IsNetAuthority();
	}

	UFUNCTION(ScriptCallable)
	static void InitFromActor(FGameplayAbilityActorInfo& Info, AActor *OwnerActor, AActor *AvatarActor, UAbilitySystemComponent* InAbilitySystemComponent)
	{
		Info.InitFromActor(OwnerActor, AvatarActor, InAbilitySystemComponent);
	}

	UFUNCTION(ScriptCallable)
	static void ClearActorInfo(FGameplayAbilityActorInfo& Info)
	{
		Info.ClearActorInfo();
	}
};
