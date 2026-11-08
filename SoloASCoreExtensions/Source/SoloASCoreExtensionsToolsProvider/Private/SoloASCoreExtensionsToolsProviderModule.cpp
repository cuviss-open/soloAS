// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "Modules/ModuleManager.h"

#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"
#include "Core/SoloASCoreExtensionsModule.h"

#include "Misc/ConfigCacheIni.h"
#include "AngelscriptManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"

void StartBlueprintParentEditor();
void StopBlueprintParentEditor();

namespace
{
struct FPackagingStep
{
    FString Name, Phase;
    TArray<FString> Modes, Inputs, Artifacts;
};
struct FCallResult
{
    bool bOk = false;
    FString Code, Message;
    FString ResultJson = TEXT("{}");
};
class FCoreExtensionsPackagingProvider final
{
public:
    FString GetProviderId() const { return TEXT("core_extensions"); }
    FString GetVersion() const { return TEXT("0.2.0"); }
    TArray<FPackagingStep> GetSteps() const
    {
        return {
            {TEXT("core_extensions.precheck"), TEXT("precheck"),
                {TEXT("source"), TEXT("bytecode"), TEXT("semi_aot")},
                {TEXT("CE producer state"), TEXT("frozen feature settings")},
                {TEXT("core_extensions capability"), TEXT("extension fingerprint")}},
            {TEXT("core_extensions.validate_caches"), TEXT("before_stage"),
                {TEXT("bytecode"), TEXT("semi_aot")},
                {TEXT("Binds.Cache"), TEXT("Binds.Cache.Headers"), TEXT("PrecompiledScript.Cache"),
                    TEXT("CoreExtensions.packaging.json")},
                {TEXT("validated extension cache families")}}
        };
    }
    FCallResult Execute(const FString& Name, const FString&, const FString& ArgumentsJson)
    {
        FCallResult Result;
        if (!bAccepting) { Result.Code = TEXT("provider_stopped"); return Result; }
        TSharedPtr<FJsonObject> Arguments;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ArgumentsJson), Arguments) || !Arguments)
        { Result.Code = TEXT("invalid_arguments"); Result.Message = TEXT("Arguments must be a JSON object."); return Result; }
        FString Fingerprint, Error;
        if (!SoloASCoreExtensions::QueryPackagingState(Fingerprint, Error))
        { Result.Code = TEXT("extension_identity_invalid"); Result.Message = Error; return Result; }
        FString ExpectedFingerprint;
        if (Arguments->HasField(TEXT("expectedExtensionFingerprint"))
            && (!Arguments->HasTypedField<EJson::String>(TEXT("expectedExtensionFingerprint"))
                || !Arguments->TryGetStringField(TEXT("expectedExtensionFingerprint"), ExpectedFingerprint)))
        { Result.Code = TEXT("invalid_arguments"); return Result; }
        if (!ExpectedFingerprint.IsEmpty() && ExpectedFingerprint != Fingerprint)
        { Result.Code = TEXT("extension_identity_changed"); return Result; }
        auto Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("capability"), TEXT("core_extensions"));
        Data->SetStringField(TEXT("extensionFingerprint"), Fingerprint);
        Data->SetBoolField(TEXT("restartRequiredForSettings"), true);
        if (Name == TEXT("core_extensions.precheck"))
        {
            Data->SetArrayField(TEXT("requiredCompanions"), {
                MakeShared<FJsonValueString>(TEXT("CoreExtensions.packaging.json"))});
            TArray<FString> CookPaths;

            TArray<TSharedPtr<FJsonValue>> Assets;

            CookPaths.Sort();
            for (const auto& Path : CookPaths)
            {
                if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)))
                { Result.Code = TEXT("extension_cook_asset_missing"); Result.Message = Path; return Result; }
                Assets.Add(MakeShared<FJsonValueString>(Path));
            }
            Data->SetArrayField(TEXT("cookAssets"), Assets);
            Data->SetBoolField(TEXT("readyToPackage"), false);
        }
        else if (Name == TEXT("core_extensions.validate_caches"))
        {
            FString Directory = FAngelscriptManager::GetScriptRootDirectory();
            if (Arguments->HasField(TEXT("scriptDirectory"))
                && !Arguments->TryGetStringField(TEXT("scriptDirectory"), Directory))
            { Result.Code = TEXT("invalid_arguments"); return Result; }
            if (Directory.IsEmpty()) { Result.Code = TEXT("invalid_arguments"); return Result; }
            Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Directory);
            if (!SoloASCoreExtensions::ValidatePackagingInputs(Directory, Error))
            { Result.Code = TEXT("extension_cache_invalid"); Result.Message = Error; return Result; }
            Data->SetBoolField(TEXT("extensionCachesValid"), true);
            // The packaging executor still owns JIT pairing and container checks.
            Data->SetBoolField(TEXT("jitLinkageVerified"), false);
            Data->SetBoolField(TEXT("packageContainerVerified"), false);
        }
        else { Result.Code = TEXT("step_missing"); return Result; }
        Result.ResultJson.Reset();
        FJsonSerializer::Serialize(Data, TJsonWriterFactory<>::Create(&Result.ResultJson));
        Result.bOk = true;
        return Result;
    }
    void Request(const TArray<FString>& Args, FOutputDevice& Output)
    {
        check(IsInGameThread());
        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> Request;
        FString Operation, Name, JobId;
        const TSharedPtr<FJsonObject>* Arguments = nullptr;
        FCallResult Result;
        if (Args.Num() != 1
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Args[0]), Request) || !Request
            || !Request->TryGetStringField(TEXT("operation"), Operation))
            Result.Code = TEXT("invalid_request");
        else if (bActive) Result.Code = TEXT("provider_busy");
        else if (Operation == TEXT("describe"))
        {
            Response->SetStringField(TEXT("providerId"), GetProviderId());
            Response->SetStringField(TEXT("version"), GetVersion());
            TArray<TSharedPtr<FJsonValue>> Steps;
            auto Strings = [](const TArray<FString>& Values)
            {
                TArray<TSharedPtr<FJsonValue>> Array;
                for (const auto& Value : Values) Array.Add(MakeShared<FJsonValueString>(Value));
                return Array;
            };
            for (const auto& Step : GetSteps())
            {
                auto Item = MakeShared<FJsonObject>();
                Item->SetStringField(TEXT("name"), Step.Name);
                Item->SetStringField(TEXT("phase"), Step.Phase);
                Item->SetArrayField(TEXT("modes"), Strings(Step.Modes));
                Item->SetArrayField(TEXT("inputs"), Strings(Step.Inputs));
                Item->SetArrayField(TEXT("artifacts"), Strings(Step.Artifacts));
                Steps.Add(MakeShared<FJsonValueObject>(Item));
            }
            Response->SetArrayField(TEXT("steps"), Steps);
            Result.bOk = true;
        }
        else if (Request->TryGetStringField(TEXT("jobId"), JobId) && !JobId.IsEmpty())
        {
            TGuardValue<bool> Guard(bActive, true);
            if (Operation == TEXT("cancel")) { Cancel(JobId); Result.bOk = true; }
            else if (Operation == TEXT("execute") && Request->TryGetStringField(TEXT("name"), Name)
                && Request->TryGetObjectField(TEXT("arguments"), Arguments))
            {
                FString Json;
                FJsonSerializer::Serialize(Arguments->ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
                Result = Execute(Name, JobId, Json);
                FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result.ResultJson), Response);
            }
            else Result.Code = TEXT("invalid_request");
        }
        else Result.Code = TEXT("invalid_request");
        auto Envelope = MakeShared<FJsonObject>();
        Envelope->SetBoolField(TEXT("ok"), Result.bOk);
        Envelope->SetStringField(TEXT("code"), Result.Code);
        Envelope->SetStringField(TEXT("message"), Result.Message);
        Envelope->SetObjectField(TEXT("data"), Response);
        FString Json;
        FJsonSerializer::Serialize(Envelope, TJsonWriterFactory<>::Create(&Json));
        Output.Log(*Json);
    }
    // Calls are synchronous on the game thread; no owned background work survives Execute.
    void Cancel(const FString&) { }
    void Quiesce() { check(!bActive); bAccepting = false; }
private:
    bool bAccepting = true;
    bool bActive = false;
};
}

class FSoloASCoreExtensionsToolsProviderModule final : public IModuleInterface
{
public:
    void StartupModule() override
    {
        FModuleManager::LoadModuleChecked<FSoloASCoreExtensionsModule>(TEXT("SoloASCoreExtensions"));
        StartBlueprintParentEditor();
        bool Enabled = true;
        if (GConfig) GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableToolsPackagingProvider"), Enabled, GEngineIni);
        if (!Enabled) return;
        Provider = MakeUnique<FCoreExtensionsPackagingProvider>();
        check(!IConsoleManager::Get().FindConsoleObject(CommandName));
        Command = IConsoleManager::Get().RegisterConsoleCommand(CommandName,
            TEXT("Synchronous packaging provider JSON endpoint (v1)."),
            FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateRaw(Provider.Get(), &FCoreExtensionsPackagingProvider::Request));
    }
    bool SupportsDynamicReloading() override { return false; }
    void ShutdownModule() override
    {
        StopBlueprintParentEditor();
        if (Provider)
        {
            Provider->Quiesce();
            IConsoleManager::Get().UnregisterConsoleObject(Command, false);
            Command = nullptr;
            Provider.Reset();
        }
    }
private:
    static constexpr const TCHAR* CommandName = TEXT("SoloAS.PackagingProvider.v1.core_extensions");
    IConsoleCommand* Command = nullptr;
    TUniquePtr<FCoreExtensionsPackagingProvider> Provider;
};

IMPLEMENT_MODULE(FSoloASCoreExtensionsToolsProviderModule, SoloASCoreExtensionsToolsProvider)
