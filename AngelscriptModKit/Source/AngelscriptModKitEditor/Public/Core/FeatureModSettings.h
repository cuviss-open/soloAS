// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FeatureModSettings.generated.h"

UCLASS(Config=FeatureMods, DefaultConfig, meta=(DisplayName="FeatureMods"))
class ANGELSCRIPTMODKITEDITOR_API UFeatureModSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Editor discovery policy captured at module startup; restart to apply edits. */
	static const UFeatureModSettings* Get();

	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("SoloAS"); }
	virtual FName GetSectionName() const override { return TEXT("FeatureMods"); }

	/** Catalog roots relative to the project. Runtime discovery uses the two built-in FeatureMod roots. */
	UPROPERTY(Config, EditAnywhere, Category="Discovery", meta=(ConfigRestartRequired=true))
	TArray<FString> AllowedRoots = { TEXT("Plugins/FeatureMods"), TEXT("FeatureMods") };

	/** Enables the supported FeatureMod script reload action in the editor after restart. */
	UPROPERTY(Config, EditAnywhere, Category="Development", meta=(ConfigRestartRequired=true))
	bool bEnableDevelopmentHotReload = true;
};
