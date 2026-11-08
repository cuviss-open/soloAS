// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "SoloASPackagingSettings.generated.h"

UENUM()
enum class ESoloASScriptDelivery : uint8
{
    Source UMETA(DisplayName="Source scripts"),
    Bytecode UMETA(DisplayName="Bytecode"),
    SemiAot UMETA(DisplayName="Semi-AOT (static JIT)")
};

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="soloAS Packaging"))
class SOLOASDEVELOPERTOOLSEDITOR_API USoloASPackagingSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
    virtual FName GetSectionName() const override { return TEXT("SoloASPackaging"); }
    virtual FText GetSectionText() const override;
    virtual FText GetSectionDescription() const override;
    virtual bool SupportsAutoRegistration() const override { return false; }

    UPROPERTY(Config, EditAnywhere, Category="Script packaging", meta=(DisplayName="Script delivery",
        ToolTip="Source includes scripts. Bytecode omits source. Semi-AOT also compiles generated C++ into the Game. UE native packaging keeps script source and ignores this choice. Use the soloAS CLI to generate and verify bytecode or semi-AOT packages."))
    ESoloASScriptDelivery DeliveryMode = ESoloASScriptDelivery::Source;

    UPROPERTY(Config, EditAnywhere, Category="Script packaging", meta=(DisplayName="Output directory", RelativeToGameDir,
        ToolTip="Output folder relative to this project, used by the soloAS packaging CLI. Default: Saved/Packages. UE's native packaging dialog chooses its own output folder."))
    FDirectoryPath OutputDirectory;

    UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category="Script packaging", meta=(DisplayName="Required integrations",
        ToolTip="Provider IDs required by the soloAS packaging CLI. Leave empty unless a delivery integration is required. Profiles remain in Config/SoloASPackagingProfiles.json and can override these project defaults."))
    TArray<FString> RequiredProviders;

    USoloASPackagingSettings();
    virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
    virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
private:
    FString PreviousOutput;
};

namespace SoloASDeveloperTools
{
void RegisterPackagingSettingsLocalization();
}
