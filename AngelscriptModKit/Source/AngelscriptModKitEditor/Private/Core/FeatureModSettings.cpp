// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Core/FeatureModSettings.h"

#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

const UFeatureModSettings* UFeatureModSettings::Get()
{
	static const TStrongObjectPtr<UFeatureModSettings> Policy(
		DuplicateObject<UFeatureModSettings>(GetDefault<UFeatureModSettings>(), GetTransientPackage()));
	return Policy.Get();
}
