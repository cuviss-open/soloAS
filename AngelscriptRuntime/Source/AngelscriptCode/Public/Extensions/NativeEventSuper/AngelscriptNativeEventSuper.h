// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

struct FAngelscriptBlueprintEventBindingSnapshot;

/** Creates an internal alias for one accepted BlueprintNativeEvent binding. */
ANGELSCRIPTCODE_API bool RegisterAngelscriptNativeEventSuperAlias(
	const FAngelscriptBlueprintEventBindingSnapshot& Binding
);
