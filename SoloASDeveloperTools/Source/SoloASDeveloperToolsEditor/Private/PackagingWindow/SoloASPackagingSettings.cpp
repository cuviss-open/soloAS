// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "SoloASPackagingSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/PolyglotTextData.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "SoloASPackagingSettings"

USoloASPackagingSettings::USoloASPackagingSettings()
{ OutputDirectory.Path = TEXT("Saved/Packages"); }

FText USoloASPackagingSettings::GetSectionText() const
{ return LOCTEXT("SettingsTitle", "soloAS Packaging"); }
FText USoloASPackagingSettings::GetSectionDescription() const
{
    return LOCTEXT("SettingsDescription", "Delivery and output settings are for soloAS CLI packaging; other options: Project > Packaging.");
}
void USoloASPackagingSettings::PreEditChange(FProperty* Property)
{
    PreviousOutput = OutputDirectory.Path;
    Super::PreEditChange(Property);
}
void USoloASPackagingSettings::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
    if (Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(USoloASPackagingSettings, OutputDirectory) ||
        (Event.MemberProperty && Event.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(USoloASPackagingSettings, OutputDirectory)))
    {
        FString Relative = OutputDirectory.Path;
        FPaths::NormalizeDirectoryName(Relative);
        if (!FPaths::IsRelative(Relative)) FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
        TArray<FString> Parts; Relative.ParseIntoArray(Parts, TEXT("/"));
        if (Relative.IsEmpty() || Relative.StartsWith(TEXT("/")) || Relative.Contains(TEXT(":")) || Parts.Contains(TEXT("..")))
        {
            OutputDirectory.Path = PreviousOutput;
            FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("InvalidOutput", "Choose an output folder inside this project, without parent traversal."));
        }
        else OutputDirectory.Path = Relative;
    }
    Super::PostEditChangeProperty(Event);
}

namespace SoloASDeveloperTools
{
void RegisterPackagingSettingsLocalization()
{
    static bool bRegistered = false;
    auto LocalizeCategory = []
    {
        const FString Category = LOCTEXT("Category", "Script packaging").ToString();
        for (const FName Name : {GET_MEMBER_NAME_CHECKED(USoloASPackagingSettings, DeliveryMode),
            GET_MEMBER_NAME_CHECKED(USoloASPackagingSettings, OutputDirectory), GET_MEMBER_NAME_CHECKED(USoloASPackagingSettings, RequiredProviders)})
            FindFProperty<FProperty>(USoloASPackagingSettings::StaticClass(), Name)->SetMetaData(TEXT("Category"), *Category);
    };
    if (bRegistered) { LocalizeCategory(); return; }
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("SoloASDeveloperTools"));
    FString Text;
    TSharedPtr<FJsonObject> Catalog;
    if (!Plugin.IsValid() || !FFileHelper::LoadFileToString(Text, *(Plugin->GetBaseDir() / TEXT("Resources/Localization/Packaging.json"))) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Catalog)) return;
    TArray<FPolyglotTextData> Entries;
    auto Add = [&Entries](FString Namespace, FString Key, FString Native, FString Chinese)
    {
        FPolyglotTextData Data(ELocalizedTextSourceCategory::Editor, Namespace, Key, Native, TEXT("en"));
        Data.AddLocalizedString(TEXT("en"), Native); Data.AddLocalizedString(TEXT("zh"), Chinese);
        Entries.Add(MoveTemp(Data));
    };
    for (const auto& Entry : Catalog->GetObjectField(TEXT("texts"))->Values)
    {
        const auto Strings = Entry.Value->AsObject();
        Add(TEXT("SoloASPackagingSettings"), FString(Entry.Key), Strings->GetStringField(TEXT("en")), Strings->GetStringField(TEXT("zh")));
    }
    for (TFieldIterator<FProperty> It(USoloASPackagingSettings::StaticClass()); It; ++It)
    {
        const TSharedPtr<FJsonObject>* Translation = nullptr;
        if (!Catalog->GetObjectField(TEXT("properties"))->TryGetObjectField(It->GetName(), Translation)) continue;
        Add(TEXT("UObjectDisplayNames"), It->GetFullGroupName(false), It->GetMetaData(TEXT("DisplayName")), (*Translation)->GetStringField(TEXT("name")));
        Add(TEXT("UObjectToolTips"), It->GetFullGroupName(false), It->GetMetaData(TEXT("ToolTip")), (*Translation)->GetStringField(TEXT("tooltip")));
    }
    const UEnum* Enum = StaticEnum<ESoloASScriptDelivery>();
    for (int32 Index = 0; Index < 3; ++Index)
    {
        const FString Name = Enum->GetNameStringByIndex(Index);
        Add(TEXT("UObjectDisplayNames"), Enum->GetFullGroupName(false) + TEXT(".") + Name,
            Enum->GetMetaData(TEXT("DisplayName"), Index), Catalog->GetObjectField(TEXT("deliveryModes"))->GetStringField(Name));
    }
    FTextLocalizationManager::Get().RegisterPolyglotTextData(Entries);
    LocalizeCategory();
    bRegistered = true;
}
}
#undef LOCTEXT_NAMESPACE
