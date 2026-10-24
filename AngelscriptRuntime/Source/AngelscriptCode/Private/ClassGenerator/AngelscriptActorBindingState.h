// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

class AActor;
class UASClass;
class asITypeInfo;

namespace AngelscriptActorBindingState
{
void PruneInvalidActorBindingStates();
bool IsActorBindingStateCurrent(
	AActor* Actor,
	UASClass* ClassGeneration,
	asITypeInfo* TypeCookie
);
void RememberActorBindingState(
	AActor* Actor,
	UASClass* ClassGeneration,
	asITypeInfo* TypeCookie
);
void ForgetActorBindingState(AActor* Actor);
void ResetActorBindingState();
bool IsActorBindingStateEmpty();
}
