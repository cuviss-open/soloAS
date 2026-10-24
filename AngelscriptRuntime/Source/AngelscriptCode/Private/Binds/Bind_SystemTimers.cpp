// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "Kismet/KismetSystemLibrary.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_SystemTimers(
	FAngelscriptBinds::EOrder::Late,
	[]
	{
		FAngelscriptBinds::FNamespace Namespace("System");
		FAngelscriptBinds::BindGlobalFunction(
			"FTimerHandle SetTimer(UObject Object, const FName& FunctionName, float32 Time, "
			"bool bLooping = false, bool bMaxOncePerFrame = false, "
			"float32 InitialStartDelay = 0.f, float32 InitialStartDelayVariance = 0.f)",
			[](
				UObject* Object,
				const FName& FunctionName,
				float Time,
				bool bLooping,
				bool bMaxOncePerFrame,
				float InitialStartDelay,
				float InitialStartDelayVariance
			)
			{
				return UKismetSystemLibrary::K2_SetTimer(
					Object,
#if WITH_ANGELSCRIPT_K2_SETTIMER_FNAME
					FunctionName,
#else
					FunctionName.ToString(),
#endif
					Time,
					bLooping,
					bMaxOncePerFrame,
					InitialStartDelay,
					InitialStartDelayVariance
				);
			}
		);
	}
);
