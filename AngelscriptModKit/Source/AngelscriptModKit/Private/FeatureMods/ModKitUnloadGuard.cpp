// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "FeatureMods/ModKitUnloadGuard.h"
#include "AngelscriptStructLiveValues.h"

#include "ClassGenerator/ASStruct.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectBaseUtility.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

bool FModKitUnloadGuard::HasUnknownOwnership(FString& OutModuleName)
{
	check(IsInGameThread());
	for (const auto& Module : FAngelscriptManager::Get().GetActiveModules())
	{
		FModKitSourceOwner Owner;
		FString Error;
		if (FModKitCompilationManager::RequiresModAdmission(*Module)
			&& !FModKitCompilationManager::GetModuleOwner(Module, Owner, Error))
		{
			OutModuleName = Module->ModuleName;
			return true;
		}
	}
	return false;
}

bool FModKitUnloadGuard::HasLiveInstances(const FString& ModName, int32& OutLiveCount, FString& OutLiveClassName)
{
	check(IsInGameThread());
	OutLiveCount = 0;
	OutLiveClassName.Reset();
	bool bHasUntrackedValueCarriers = false;
	if (HasUnknownOwnership(OutLiveClassName))
	{
		OutLiveCount = -2;
		return true;
	}

	for (const TSharedRef<FAngelscriptModuleDesc>& Module : FAngelscriptManager::Get().GetActiveModules())
	{
		FModKitSourceOwner Owner;
		FString Error;
		if (!FModKitCompilationManager::GetModuleOwner(Module, Owner, Error)
			|| Owner.Layer != EModKitSourceLayer::Leaf || Owner.Id != ModName)
		{
			continue;
		}
		// Enum value carriers are not tracked by the guard.
		if (Module->Enums.Num() > 0)
		{
			bHasUntrackedValueCarriers = true;
		}
		for (const TSharedRef<FAngelscriptClassDesc>& ClassDesc : Module->Classes)
		{
			if (ClassDesc->bIsStruct)
			{
				int32 Count = -1;
				if (!QueryAngelscriptStructLiveValues(ClassDesc->ScriptType, Count))
				{
					OutLiveCount = -1;
					OutLiveClassName = ClassDesc->ClassName;
					return true;
				}
				if (Count > 0)
				{
					OutLiveCount += Count;
					if (OutLiveClassName.IsEmpty())
					{
						OutLiveClassName = ClassDesc->ClassName;
					}
				}
				continue;
			}
			if (ClassDesc->bIsStaticsClass || ClassDesc->Class == nullptr)
			{
				continue;
			}

			UClass* Class = ClassDesc->Class;
			int32 Count = 0;
			for (TObjectIterator<UObject> It; It; ++It)
			{
				UObject* Object = *It;
				if (Object->IsA(Class)
					&& !Object->HasAnyFlags(RF_ClassDefaultObject)
					&& !Object->IsTemplate()
					&& IsValid(Object))
				{
					++Count;
				}
			}
			if (Count > 0)
			{
				OutLiveCount += Count;
				if (OutLiveClassName.IsEmpty())
				{
					OutLiveClassName = ClassDesc->ClassName;
				}
			}
		}
	}
	if (bHasUntrackedValueCarriers)
	{
		UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_UNLOAD_GUARD_LIMIT mod=%s detail=enum-value-carriers-not-scanned"), *ModName);
	}
	return OutLiveCount > 0;
}

void FModKitUnloadGuard::DestroyLiveInstances(const FString& ModName)
{
	check(IsInGameThread());
	FString UnknownModule;
	if (HasUnknownOwnership(UnknownModule)) return;

	TArray<TWeakObjectPtr<AActor>> Actors;
	TArray<TWeakObjectPtr<UActorComponent>> Components;
	TArray<TWeakObjectPtr<UObject>> Objects;
	TSet<UObject*> Seen;

	for (const TSharedRef<FAngelscriptModuleDesc>& Module : FAngelscriptManager::Get().GetActiveModules())
	{
		FModKitSourceOwner Owner;
		FString Error;
		if (!FModKitCompilationManager::GetModuleOwner(Module, Owner, Error)
			|| Owner.Layer != EModKitSourceLayer::Leaf || Owner.Id != ModName)
		{
			continue;
		}

		for (const TSharedRef<FAngelscriptClassDesc>& ClassDesc : Module->Classes)
		{
			if (ClassDesc->bIsStruct || ClassDesc->bIsStaticsClass || ClassDesc->Class == nullptr)
			{
				continue;
			}

			for (TObjectIterator<UObject> It; It; ++It)
			{
				UObject* Object = *It;
				if (!Object->IsA(ClassDesc->Class)
					|| Object->HasAnyFlags(RF_ClassDefaultObject)
					|| Object->IsTemplate()
					|| !IsValid(Object)
					|| Seen.Contains(Object))
				{
					continue;
				}
				Seen.Add(Object);

				if (AActor* Actor = Cast<AActor>(Object))
				{
					Actors.Add(Actor);
				}
				else if (UActorComponent* Component = Cast<UActorComponent>(Object))
				{
					Components.Add(Component);
				}
				else
				{
					Objects.Add(Object);
				}
			}
		}
	}

	for (const TWeakObjectPtr<AActor>& Actor : Actors)
	{
		if (Actor.IsValid())
		{
			Actor->Destroy();
		}
	}
	for (const TWeakObjectPtr<UActorComponent>& Component : Components)
	{
		if (Component.IsValid())
		{
			Component->DestroyComponent();
		}
	}
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		if (Object.IsValid())
		{
			Object->MarkAsGarbage();
		}
	}
}
