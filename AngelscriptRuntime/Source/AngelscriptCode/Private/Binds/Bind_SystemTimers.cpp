// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "Kismet/KismetSystemLibrary.h"
                                                                                                    
#include "AngelscriptDocs.h"
#include "UObject/UnrealType.h"
                                                                                                  

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_SystemTimers(
	FAngelscriptBinds::EOrder::Late,
	[]
	{
		FAngelscriptBinds::FNamespace Namespace("System");
		FAngelscriptBinds::BindGlobalFunction(
			"FTimerHandle SetTimer(UObject Object, const FName& FunctionName, float32 Time, "
			"bool bLooping, bool bMaxOncePerFrame = false, "
			"float32 InitialStartDelay = 0.000000, float32 InitialStartDelayVariance = 0.000000)",
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
                                                                                                        
#if WITH_EDITOR
		// A: retain the reflected identity of the native timer called by this adapter.
		if (UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(FName(TEXT("K2_SetTimer"))))
		{
			FAngelscriptDocs::AddUnrealDocumentation(FAngelscriptBinds::GetPreviousFunctionId(),
				Function->GetMetaData(TEXT("ToolTip")), Function->GetMetaData(TEXT("Category")), Function);
		}
#endif
                                                                                                      
	}
);
