// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once

#include "Extensions/AngelscriptExtensionHooks.h"

FAngelscriptExtensionCallbackRegistration RegisterBlueprintUInterfaceExtension();

EAngelscriptExtensionResult HandleBlueprintUInterfaceSelection(
	FAngelscriptNativeInterfaceSelectionContext& Context
);
