// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace AngelscriptBpgcSemanticsHooks::Tests
{
enum class ECallerRange : uint8
{
	ActorCdoCopy,
	AnimCdoCopy,
	KismetWarning
};

ANGELSCRIPTEDITOR_API void RegisterCallerModule(ECallerRange Range, FName ModuleName);
ANGELSCRIPTEDITOR_API void UnregisterCallerModule(ECallerRange Range, FName ModuleName);
}
#endif
