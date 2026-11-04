// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "UObject/UnrealType.h"
#include "SoloASEngineCompat.h"

/** Preserves already-constructed resource fields; parameters and nested values stay stock. */
FProperty* CreateAngelscriptProperty(FFieldClass* PropertyClass, FFieldVariant Owner, const FName& Name);

FStructProperty* CreateAngelscriptStructProperty(FFieldVariant Owner, const FName& Name);

                                                                                                                                               
#if SOLOAS_UE_BEFORE(5, 7)
void FinishDestroyAngelscriptProperty(const FProperty& Property, void* Object);
#endif
// SOLOAS-COMPAT-END
// SOLOAS-COMPAT-BEGIN(<5.5) UE 5.4 has no finish-clear query; script fields register by the UE 5.5+ rule [test: SoloAS.RuntimeReloadCrossModuleDestructorException] [id: CS.Compatibility.AngelscriptRuntime.AngelscriptPropertyInitialization_h.LegacyFinishClear]
#if SOLOAS_UE_BEFORE(5, 5)
bool AngelscriptPropertyContainsLegacyFinishClear(const FProperty& Property);
#endif
// SOLOAS-COMPAT-END
