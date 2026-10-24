// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptActorBindingState.h"

#include "AngelscriptExecutionQuiescence.h"
#include "ClassGenerator/ASClass.h"
#include "GameFramework/Actor.h"

namespace AngelscriptActorBindingState
{
namespace
{
struct FLinkedActorBindingState
{
	TWeakObjectPtr<UASClass> ClassGeneration;
	// Borrowed from the AngelScript engine; this cache never owns the cookie.
	asITypeInfo* TypeCookie = nullptr;
};

TMap<TWeakObjectPtr<AActor>, FLinkedActorBindingState>
	LinkedActorBindingStates;
uint32 ActorBindingCallsSincePrune = 0;
}

void PruneInvalidActorBindingStates()
{
	if (++ActorBindingCallsSincePrune < 256)
	{
		return;
	}

	ActorBindingCallsSincePrune = 0;
	for (auto It = LinkedActorBindingStates.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

bool IsActorBindingStateCurrent(
	AActor* Actor,
	UASClass* ClassGeneration,
	asITypeInfo* TypeCookie
)
{
	const FLinkedActorBindingState* Existing =
		LinkedActorBindingStates.Find(TWeakObjectPtr<AActor>(Actor));
	return Existing != nullptr
		&& Existing->ClassGeneration.Get() == ClassGeneration
		&& Existing->TypeCookie == TypeCookie;
}

void RememberActorBindingState(
	AActor* Actor,
	UASClass* ClassGeneration,
	asITypeInfo* TypeCookie
)
{
	LinkedActorBindingStates.Add(
		TWeakObjectPtr<AActor>(Actor),
		FLinkedActorBindingState{
			TWeakObjectPtr<UASClass>(ClassGeneration),
			TypeCookie,
		}
	);
}

void ForgetActorBindingState(AActor* Actor)
{
	LinkedActorBindingStates.Remove(TWeakObjectPtr<AActor>(Actor));
}

void ResetActorBindingState()
{
	checkf(
		IsInGameThread(),
		TEXT("Actor BindComponents cache reset requires the game thread.")
	);
	checkf(
		IsAngelscriptExecutionWriterHeldByCurrentThread(),
		TEXT("Actor BindComponents cache reset requires the execution writer.")
	);
	LinkedActorBindingStates.Empty();
	ActorBindingCallsSincePrune = 0;
}

bool IsActorBindingStateEmpty()
{
	return LinkedActorBindingStates.Num() == 0;
}
}
