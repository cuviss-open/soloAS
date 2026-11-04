// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
// UE 5.4 ModuleRules has no ForceIncludeFiles. A private PCH supplies the same
// module-wide adapter declarations before each translation unit. 
#include "CoreMinimal.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "AngelscriptReflectionAdapter.h"
#include "AngelscriptCallableDispatchAdapter.h"
#include "Runtime/SoloASBridgeCallableDispatch.h"
#include "AngelscriptExecutionQuiescence.h"
#include "ClassGenerator/AngelscriptClassState.h"
#include "AngelscriptRuntimeGcLifecycle.h"
#include "Runtime/SoloASBridgeObjectLifecycle.h"
#include "UObject/CoreNet.h"
#include "UObject/ObjectKey.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
