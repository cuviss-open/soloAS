// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FFeatureModEditor
{
public:
	static const FName ManagerTabId;
	static void OpenManager();
	static void OpenSettings();
	static bool IsMenuEntryRegistered();
	static bool AreNavigationTargetsRegistered();

	static void SetMenuEntryRegistered(bool bRegistered);
	static void SetSettingsRegistered(bool bRegistered);
	static void SetManagerTabRegistered(bool bRegistered);
};
