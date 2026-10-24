// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "UObject/UnrealType.h"

/** Preserves already-constructed resource fields; parameters and nested values stay stock. */
FProperty* CreateAngelscriptProperty(FFieldClass* PropertyClass, FFieldVariant Owner, const FName& Name);

FStructProperty* CreateAngelscriptStructProperty(FFieldVariant Owner, const FName& Name);
