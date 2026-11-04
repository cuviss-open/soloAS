// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "CoreMinimal.h"
#include "SoloASEngineCompat.h"
// SOLOAS-COMPAT-BEGIN(<5.8) Compile the legacy editor backend in Bridge without changing Detours [test: SoloAS.EditorHookInstallation] [id: CS.File.SoloASBridge.Source.SoloASDetours.CompileModules.cpp]
#if WITH_EDITOR && !IS_MONOLITHIC && PLATFORM_WINDOWS && SOLOAS_UE_BEFORE(5, 8)
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/AllowWindowsPlatformAtomics.h"
#include "SoloASDetours/Exports.h"
#pragma warning(push, 0)
#include "modules.cpp"
#pragma warning(pop)
#include "Windows/HideWindowsPlatformAtomics.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif
// SOLOAS-COMPAT-END
