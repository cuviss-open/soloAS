// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "SoloASDeveloperTools.h"
#include "McpMenu.h"
#include "McpEditServer.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "AngelscriptSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "SoloASPackagingSettings.h"
#include "ISettingsModule.h"
#include "ISettingsContainer.h"
#include "ISettingsCategory.h"
#include "ISettingsSection.h"

#define LOCTEXT_NAMESPACE "SoloASPackagingSettings"

namespace SoloASDeveloperTools
{
bool BuildMcpConfiguration(const FString& Host, const FString& Engine, const FString& Project, int32 Port, FString& Out)
{
    Out.Reset();
    if (!FPaths::FileExists(Host)) return false;
    TSharedRef<FJsonObject> Server = MakeShared<FJsonObject>();
    Server->SetStringField(TEXT("type"), TEXT("stdio"));
    Server->SetStringField(TEXT("command"), Host);
    TArray<TSharedPtr<FJsonValue>> Args;
    for (const FString& Arg : { FString(TEXT("--engine")), Engine, FString(TEXT("--project")), Project,
        FString(TEXT("--lane")), FString(TEXT("stock")), FString(TEXT("--debug-host")), FString(TEXT("127.0.0.1")),
        FString(TEXT("--debug-port")), FString::FromInt(Port) })
        Args.Add(MakeShared<FJsonValueString>(Arg));
    Server->SetArrayField(TEXT("args"), Args);
    TSharedRef<FJsonObject> Servers = MakeShared<FJsonObject>();
    Servers->SetObjectField(TEXT("soloas-angelscript"), Server);
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetObjectField(TEXT("mcpServers"), Servers);
    return FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Out));
}

static FText MenuText(const TCHAR* Chinese, const TCHAR* English)
{
    return FText::FromString(FInternationalization::Get().GetCurrentLanguage()->GetTwoLetterISOLanguageName() == TEXT("zh") ? Chinese : English);
}

static void CopyMcpConfiguration()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SoloASDeveloperTools"));
    const FString Host = Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() /
        TEXT("Binaries/ThirdParty/SoloASMcpHost/Win64/SoloASMcpHost.exe")) : FString();
    int32 Port = GetDefault<UAngelscriptSettings>()->ConnectionPort;
    FParse::Value(FCommandLine::Get(), TEXT("-asdebugport="), Port);
    FString Configuration;
    const bool bSuccess = BuildMcpConfiguration(Host, FPaths::ConvertRelativePathToFull(FPaths::EngineDir()),
        FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), Port, Configuration);
    if (bSuccess) FPlatformApplicationMisc::ClipboardCopy(*Configuration);
    FNotificationInfo Info(bSuccess
        ? MenuText(TEXT("MCP 配置已复制到剪贴板。"), TEXT("MCP configuration copied to clipboard."))
        : MenuText(TEXT("未找到 SoloASMcpHost，请先发布 Host 再复制 MCP 配置。"), TEXT("SoloASMcpHost was not found. Publish the host before copying the MCP configuration.")));
    Info.ExpireDuration = 5.0f;
    FSlateNotificationManager::Get().AddNotification(Info);
}

void PopulateMcpMenu(UToolMenu* Sub)
{
    Sub->FindOrAddSection("AIAssistance").AddMenuEntry("SoloAS.CopyMcpConfiguration",
        MenuText(TEXT("复制 MCP 配置"), TEXT("Copy MCP configuration")),
        MenuText(TEXT("将项目 MCP 配置复制到剪贴板。"), TEXT("Copy the project MCP configuration to the clipboard.")),
        FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&CopyMcpConfiguration)));
    Sub->FindOrAddSection("AIAssistance").AddMenuEntry("SoloAS.ResumeAutomaticCompilation",
        MenuText(TEXT("立即恢复脚本自动编译"), TEXT("Resume automatic script compilation")),
        MenuText(TEXT("结束全部暂停并编译待处理的脚本修改。"), TEXT("End all pauses and compile pending script changes.")),
        FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&ResumeMcpAutomaticCompilation)));
}

bool OpenPackagingSettings()
{
    if (IsRunningCommandlet()) return false;
    RegisterPackagingSettingsLocalization();
    auto& Settings = FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings"));
    Settings.ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("SoloASPackaging"));
    return true;
}
bool OpenUEPackagingSettings()
{
    if (IsRunningCommandlet()) return false;
    FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings")).ShowViewer(TEXT("Project"), TEXT("Project"), TEXT("Packaging"));
    return true;
}
bool OpenProjectLauncher()
{
    if (IsRunningCommandlet()) return false;
    FModuleManager::Get().LoadModule(TEXT("ProjectLauncher"));
    return FGlobalTabmanager::Get()->TryInvokeTab(FName(TEXT("ProjectLauncher"))).IsValid();
}

}

class FSoloASDeveloperToolsEditorModule final : public IModuleInterface
{
    void RegisterMenus()
    {
        FToolMenuOwnerScoped Owner(this);
        UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.SoloAS");
        Menu->AddSection("AIAssistance", LOCTEXT("MenuAISection", "AI Assistance"), FToolMenuInsert(NAME_None, EToolMenuInsertType::First)).AddSubMenu("SoloAS.AIAssistance",
            SoloASDeveloperTools::MenuText(TEXT("AI 辅助开发"), TEXT("AI-assisted development")),
            SoloASDeveloperTools::MenuText(TEXT("配置 AI 辅助开发。"), TEXT("Configure AI-assisted development.")),
            FNewToolMenuDelegate::CreateStatic(&SoloASDeveloperTools::PopulateMcpMenu));
        auto& Packaging = Menu->AddSection("Packaging", LOCTEXT("MenuSection", "Packaging"), FToolMenuInsert(TEXT("SoloASBlueprint"), EToolMenuInsertType::After));
        Packaging.AddMenuEntry("SoloAS.PackagingSettings", LOCTEXT("MenuSettings", "soloAS Packaging Settings"),
            LOCTEXT("MenuSettingsTip", "Open project defaults for soloAS script delivery and CLI packaging."), FSlateIcon(),
            FToolUIAction(FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext&) { SoloASDeveloperTools::OpenPackagingSettings(); })));
        Packaging.AddMenuEntry("SoloAS.UEPackagingSettings", LOCTEXT("MenuUE", "UE Packaging Settings"),
            LOCTEXT("MenuUETip", "Set maps, cooking, Pak/IoStore, target and build configuration in Unreal Engine's Packaging page."), FSlateIcon(),
            FToolUIAction(FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext&) { SoloASDeveloperTools::OpenUEPackagingSettings(); })));
        Packaging.AddMenuEntry("SoloAS.PackageProject", LOCTEXT("MenuPackage", "Package Project…"),
            LOCTEXT("MenuPackageTip", "Open Unreal Engine's Project Launcher to build, cook and package your project."), FSlateIcon(),
            FToolUIAction(FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext&) { SoloASDeveloperTools::OpenProjectLauncher(); })));
        Packaging.AddMenuEntry("SoloAS.PackagingDocumentation", LOCTEXT("MenuDocs", "Packaging Documentation"),
            LOCTEXT("MenuDocsTip", "Read about native packaging and soloAS CLI/CI delivery workflows."), FSlateIcon(),
            FToolUIAction(FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext&)
            {
                const bool bChinese = FInternationalization::Get().GetCurrentLanguage()->GetTwoLetterISOLanguageName() == TEXT("zh");
                FPlatformProcess::LaunchURL(bChinese ? TEXT("https://soloas.cuviss.com/docs/zh/packaging.html") : TEXT("https://soloas.cuviss.com/docs/en/packaging.html"), nullptr, nullptr);
            })));

    }

public:
    void StartupModule() override
    {
        SoloASDeveloperTools::RegisterPackagingSettingsLocalization();
        if (!IsRunningCommandlet())
        {
            if (!FParse::Param(FCommandLine::Get(), TEXT("NoMessaging"))
                && !FParse::Param(FCommandLine::Get(), TEXT("NoASDebugServer")))
                SoloASDeveloperTools::StartMcpEditServer();
            auto* Packaging = GetMutableDefault<USoloASPackagingSettings>();
            FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings")).RegisterSettings(
                TEXT("Project"), TEXT("Plugins"), TEXT("SoloASPackaging"), Packaging->GetSectionText(), Packaging->GetSectionDescription(), Packaging);
            UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(
                this, &FSoloASDeveloperToolsEditorModule::RegisterMenus));
        }
    }

    void ShutdownModule() override
    {
        SoloASDeveloperTools::StopMcpEditServer();
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        if (auto* Settings = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
            Settings->UnregisterSettings(TEXT("Project"), TEXT("Plugins"), TEXT("SoloASPackaging"));
    }
};

IMPLEMENT_MODULE(FSoloASDeveloperToolsEditorModule, SoloASDeveloperToolsEditor)

#undef LOCTEXT_NAMESPACE
