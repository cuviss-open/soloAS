// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "Engine/DeveloperSettings.h"
#include "SoloASToolchainSettings.generated.h"

UENUM()
enum class ESoloASToolchainPreference : uint8
{
    Auto UMETA(DisplayName="Auto (prefer my own toolchain)"),
    ForceManaged UMETA(DisplayName="Managed toolchain"),
    ForceOwn UMETA(DisplayName="My own toolchain")
};

UCLASS(Config=EditorPerProjectUserSettings, meta=(DisplayName="C++ Toolchain"))
class SOLOASDEVELOPERTOOLSTOOLCHAINEDITOR_API USoloASToolchainSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    virtual void OverrideConfigSection(FString& InOutSectionName) override
    {
        InOutSectionName = TEXT("/Script/AngelscriptToolchainEditor.SoloASToolchainSettings");
    }

    virtual FName GetCategoryName() const override { return TEXT("SoloAS"); }
    virtual FName GetSectionName() const override { return TEXT("CppToolchain"); }

    UPROPERTY(Config, EditAnywhere, Category="C++ Toolchain", meta=(ConfigRestartRequired=true,
        ToolTip="Applies to builds launched by this Editor. Restart the Editor after changing the selection."))
    ESoloASToolchainPreference Preference = ESoloASToolchainPreference::Auto;
};
