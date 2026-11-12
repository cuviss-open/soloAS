// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Core/ModKitSettings.h"

#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

const UModKitSettings* UModKitSettings::Get()
{
	static const TStrongObjectPtr<UModKitSettings> Policy([]()
	{
		UModKitSettings* Snapshot = DuplicateObject<UModKitSettings>(GetDefault<UModKitSettings>(), GetTransientPackage());
		if (!FMath::IsFinite(Snapshot->ModStartupActivationTimeoutSec) || Snapshot->ModStartupActivationTimeoutSec < 0.1f)
		{
			Snapshot->ModStartupActivationTimeoutSec = 10.0f;
		}
		return Snapshot;
	}());
	return Policy.Get();
}
