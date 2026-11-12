// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/** Synchronous first-load consent on the game thread. Return true to allow the player FeatureMod. */
DECLARE_DELEGATE_RetVal_OneParam(bool, FFeatureModPlayerApprovalHandler, const FString&);

/** Host UI may replace the built-in dialog before discovery. Pass an unbound handler to restore it.
 * Called on the game thread. Decisions persist in Saved/FeatureModApprovals.ini; headless hosts
 * without a handler must provision consent or disable bConfirmPlayerModsOnFirstLoad in settings. */
ANGELSCRIPTMODKIT_API void SetFeatureModPlayerApprovalHandler(FFeatureModPlayerApprovalHandler Handler);
