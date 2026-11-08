// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "SoloASDeveloperToolsProviders.h"

#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"
#include "Misc/StringOutputDevice.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <type_traits>

namespace SoloASDeveloperTools
{
namespace
{
using FObject = TSharedPtr<FJsonObject>;
TSet<IModularFeature*> ActiveCalls;

FString Json(const FObject& Object)
{
    FString Text;
    FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
    return Text;
}

bool Parse(const FString& Text, FObject& Object)
{
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
}

TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Result;
    for (const FString& Value : Values)
        Result.Add(MakeShared<FJsonValueString>(Value));
    return Result;
}

bool ValidId(const FString& Id)
{
    if (Id.IsEmpty()) return false;
    for (TCHAR C : Id)
        if (!((C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_' || C == '-')) return false;
    return Id != TEXT("soloas");
}

template <typename T> TArray<T*> Providers()
{
    auto Result = IModularFeatures::Get().GetModularFeatureImplementations<T>(T::FeatureName());
    Result.Sort([](const T& A, const T& B) { return A.GetProviderId() < B.GetProviderId(); });
    return Result;
}

template <typename T> T* Find(const FString& Id)
{
    for (T* Provider : Providers<T>())
        if (Provider->GetProviderId() == Id) return Provider;
    return nullptr;
}

template <typename T> bool CanRegister(T& Provider, FString& Error)
{
    check(IsInGameThread());
    Error.Reset();
    if (!ValidId(Provider.GetProviderId()) || Provider.GetVersion().IsEmpty())
        Error = TEXT("invalid_provider_identity");
    else if (Find<T>(Provider.GetProviderId()))
        Error = TEXT("duplicate_provider");
    return Error.IsEmpty();
}

template <typename T> bool Remove(T& Provider)
{
    check(IsInGameThread());
    if (ActiveCalls.Contains(&Provider) || Find<T>(Provider.GetProviderId()) != &Provider) return false;
    // No discovery/call can interleave on this thread. Join owned work before releasing the instance.
    ActiveCalls.Add(&Provider);
    Provider.Quiesce();
    ActiveCalls.Remove(&Provider);
    IModularFeatures::Get().UnregisterModularFeature(T::FeatureName(), &Provider);
    return true;
}

FObject Identity(const FString& Id, const FString& Version)
{
    FObject Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("providerId"), Id);
    Result->SetStringField(TEXT("version"), Version);
    return Result;
}

FObject List()
{
    FObject Result = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Packaging, Mcp;
    for (const auto* Provider : Providers<IPackagingStepProvider>())
    {
        FObject Entry = Identity(Provider->GetProviderId(), Provider->GetVersion());
        TArray<TSharedPtr<FJsonValue>> Steps;
        for (const FPackagingStep& Step : Provider->GetSteps())
        {
            FObject Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("name"), Step.Name);
            Item->SetStringField(TEXT("phase"), Step.Phase);
            Item->SetArrayField(TEXT("modes"), Strings(Step.Modes));
            Item->SetArrayField(TEXT("inputs"), Strings(Step.Inputs));
            Item->SetArrayField(TEXT("artifacts"), Strings(Step.Artifacts));
            Steps.Add(MakeShared<FJsonValueObject>(Item));
        }
        Entry->SetArrayField(TEXT("steps"), Steps);
        Packaging.Add(MakeShared<FJsonValueObject>(Entry));
    }
    for (const auto* Provider : Providers<IMcpToolProvider>())
    {
        FObject Entry = Identity(Provider->GetProviderId(), Provider->GetVersion());
        TArray<TSharedPtr<FJsonValue>> Tools;
        for (const FMcpTool& Tool : Provider->GetTools())
        {
            FObject Schema;
            Parse(Tool.InputSchemaJson, Schema);
            FObject Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("name"), Tool.Name);
            Item->SetStringField(TEXT("description"), Tool.Description);
            Item->SetObjectField(TEXT("inputSchema"), Schema);
            Item->SetBoolField(TEXT("readOnly"), Tool.bReadOnly);
            Item->SetBoolField(TEXT("destructive"), Tool.bDestructive);
            Tools.Add(MakeShared<FJsonValueObject>(Item));
        }
        Entry->SetArrayField(TEXT("tools"), Tools);
        Mcp.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Result->SetArrayField(TEXT("packagingProviders"), Packaging);
    Result->SetArrayField(TEXT("mcpProviders"), Mcp);
    Result->SetBoolField(TEXT("restartRequiredForSettings"), true);
    return Result;
}

template <typename T> FCallResult Invoke(T* Provider, const FString& Operation, const FString& Name,
    const FString& JobId, const FString& Arguments)
{
    if (!Provider) return {false, TEXT("provider_missing"), TEXT("Required provider is not registered.")};
    if (ActiveCalls.Contains(Provider)) return {false, TEXT("provider_busy"), TEXT("Provider call is already active.")};
    ActiveCalls.Add(Provider);
    FCallResult Result;
    if (Operation == TEXT("cancel"))
    {
        Provider->Cancel(JobId);
        Result.bOk = true;
    }
    else
    {
        if constexpr (std::is_same_v<T, IPackagingStepProvider>)
        {
            if (!Provider->GetSteps().ContainsByPredicate([&](const FPackagingStep& Step) { return Step.Name == Name; }))
                Result = {false, TEXT("step_missing"), TEXT("Packaging step is not registered.")};
            else Result = Provider->Execute(Name, JobId, Arguments);
        }
        else
        {
            if (!Provider->GetTools().ContainsByPredicate([&](const FMcpTool& Tool) { return Tool.Name == Name; }))
                Result = {false, TEXT("tool_missing"), TEXT("MCP tool is not registered.")};
            else Result = Provider->Call(Name, JobId, Arguments);
        }
    }
    ActiveCalls.Remove(Provider);
    return Result;
}

// The v1 console endpoint lets independently installed extensions publish their
// synchronous provider without either plugin linking against the other.
class FConsolePackagingProvider final : public IPackagingStepProvider
{
public:
    explicit FConsolePackagingProvider(FString InCommand) : Command(MoveTemp(InCommand)) {}

    bool Describe(FString& Error)
    {
        FObject Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("operation"), TEXT("describe"));
        FObject Data;
        FCallResult Result = CallEndpoint(Request, Data);
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Result.bOk || !Data->TryGetStringField(TEXT("providerId"), Id)
            || !Data->TryGetStringField(TEXT("version"), Version)
            || Command != Prefix() + Id || !Data->TryGetArrayField(TEXT("steps"), Values))
        { Error = TEXT("invalid_console_provider_description"); return false; }
        for (const auto& Value : *Values)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            { Error = TEXT("invalid_console_provider_description"); return false; }
            FObject Item = Value->AsObject();
            FPackagingStep Step;
            if (!Item->TryGetStringField(TEXT("name"), Step.Name)
                || !Item->TryGetStringField(TEXT("phase"), Step.Phase)
                || !ReadStrings(Item, TEXT("modes"), Step.Modes)
                || !ReadStrings(Item, TEXT("inputs"), Step.Inputs)
                || !ReadStrings(Item, TEXT("artifacts"), Step.Artifacts))
            { Error = TEXT("invalid_console_provider_description"); return false; }
            Steps.Add(MoveTemp(Step));
        }
        return true;
    }

    static FString Prefix() { return TEXT("SoloAS.PackagingProvider.v1."); }
    FString GetProviderId() const override { return Id; }
    FString GetVersion() const override { return Version; }
    TArray<FPackagingStep> GetSteps() const override { return Steps; }
    FCallResult Execute(const FString& Name, const FString& JobId, const FString& ArgumentsJson) override
    {
        FObject Arguments;
        if (!Parse(ArgumentsJson, Arguments)) return {false, TEXT("invalid_arguments"), TEXT("Arguments must be a JSON object.")};
        FObject Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("operation"), TEXT("execute"));
        Request->SetStringField(TEXT("name"), Name);
        Request->SetStringField(TEXT("jobId"), JobId);
        Request->SetObjectField(TEXT("arguments"), Arguments);
        FObject Data;
        FCallResult Result = CallEndpoint(Request, Data);
        if (Result.bOk) Result.ResultJson = Json(Data);
        return Result;
    }
    void Cancel(const FString& JobId) override
    {
        FObject Request = MakeShared<FJsonObject>();
        Request->SetStringField(TEXT("operation"), TEXT("cancel"));
        Request->SetStringField(TEXT("jobId"), JobId);
        FObject Data;
        CallEndpoint(Request, Data);
    }
    void Quiesce() override {}

private:
    static bool ReadStrings(const FObject& Object, const TCHAR* Name, TArray<FString>& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Object->TryGetArrayField(Name, Values)) return false;
        for (const auto& Value : *Values)
        {
            if (!Value.IsValid() || Value->Type != EJson::String) return false;
            Out.Add(Value->AsString());
        }
        return true;
    }
    FCallResult CallEndpoint(const FObject& Request, FObject& Data) const
    {
        IConsoleObject* Object = IConsoleManager::Get().FindConsoleObject(*Command);
        IConsoleCommand* Endpoint = Object ? Object->AsCommand() : nullptr;
        if (!Endpoint) return {false, TEXT("provider_missing"), TEXT("Console provider is no longer registered.")};
        Request->SetNumberField(TEXT("schemaVersion"), 1);
        FStringOutputDevice Output;
        Endpoint->Execute({Json(Request)}, nullptr, Output);
        FObject Envelope;
        bool bSuccess = false;
        const FObject* ResultData = nullptr;
        if (!Parse(Output, Envelope) || !Envelope->TryGetBoolField(TEXT("ok"), bSuccess)
            || !Envelope->TryGetObjectField(TEXT("data"), ResultData) || !ResultData || !ResultData->IsValid())
            return {false, TEXT("invalid_console_provider_response"), TEXT("Console provider returned an invalid response.")};
        Data = *ResultData;
        FCallResult Result;
        Result.bOk = bSuccess;
        Envelope->TryGetStringField(TEXT("code"), Result.Code);
        Envelope->TryGetStringField(TEXT("message"), Result.Message);
        return Result;
    }
    FString Command, Id, Version;
    TArray<FPackagingStep> Steps;
};

class FScopedConsoleProviders
{
public:
    bool Discover(FCallResult& Error)
    {
        TArray<FString> Commands;
        IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateLambda(
            [&Commands](const TCHAR* Name, IConsoleObject* Object)
            {
                if (Object && Object->AsCommand()) Commands.Add(Name);
            }), *FConsolePackagingProvider::Prefix());
        Commands.Sort();
        for (const FString& Command : Commands)
        {
            auto Provider = MakeUnique<FConsolePackagingProvider>(Command);
            FString Detail;
            if (!Provider->Describe(Detail) || !Register(*Provider, Detail))
            {
                Error = {false, TEXT("provider_discovery_failed"), Command + TEXT(": ") + Detail};
                return false;
            }
            Registered.Add(MoveTemp(Provider));
        }
        return true;
    }
    ~FScopedConsoleProviders()
    {
        for (const auto& Provider : Registered) Unregister(*Provider);
    }
private:
    TArray<TUniquePtr<FConsolePackagingProvider>> Registered;
};
}

FName IPackagingStepProvider::FeatureName() { return TEXT("SoloASPackagingStepProvider"); }
FName IMcpToolProvider::FeatureName() { return TEXT("SoloASMcpToolProvider"); }

bool Register(IPackagingStepProvider& Provider, FString& Error)
{
    if (!CanRegister(Provider, Error)) return false;
    TSet<FString> Names;
    const TArray<FString> Phases = {TEXT("precheck"), TEXT("after_generate"), TEXT("before_jit_sync"),
        TEXT("before_stage"), TEXT("after_package_check")};
    const TArray<FString> Modes = {TEXT("source"), TEXT("bytecode"), TEXT("semi_aot")};
    for (const FPackagingStep& Step : Provider.GetSteps())
    {
        if (!Step.Name.StartsWith(Provider.GetProviderId() + TEXT(".")) || Names.Contains(Step.Name)
            || !Phases.Contains(Step.Phase) || Step.Modes.IsEmpty())
        { Error = TEXT("invalid_step_descriptor"); return false; }
        for (const FString& Mode : Step.Modes)
            if (!Modes.Contains(Mode)) { Error = TEXT("invalid_delivery_mode"); return false; }
        Names.Add(Step.Name);
    }
    IModularFeatures::Get().RegisterModularFeature(IPackagingStepProvider::FeatureName(), &Provider);
    return true;
}

bool Register(IMcpToolProvider& Provider, FString& Error)
{
    if (!CanRegister(Provider, Error)) return false;
    TSet<FString> Names;
    for (const FMcpTool& Tool : Provider.GetTools())
    {
        FObject Schema;
        FString Type;
        if (!Tool.Name.StartsWith(Provider.GetProviderId() + TEXT(".")) || Names.Contains(Tool.Name)
            || !Parse(Tool.InputSchemaJson, Schema) || !Schema->TryGetStringField(TEXT("type"), Type) || Type != TEXT("object"))
        { Error = TEXT("invalid_tool_descriptor"); return false; }
        Names.Add(Tool.Name);
    }
    IModularFeatures::Get().RegisterModularFeature(IMcpToolProvider::FeatureName(), &Provider);
    return true;
}

bool Unregister(IPackagingStepProvider& Provider) { return Remove(Provider); }
bool Unregister(IMcpToolProvider& Provider) { return Remove(Provider); }

FString HandleRequest(const FString& RequestJson, bool& bOk)
{
    check(IsInGameThread());
    FObject Request, Result = MakeShared<FJsonObject>();
    FCallResult Call;
    FScopedConsoleProviders ConsoleProviders;
    FString Operation;
    double Version = 0;
    if (!Parse(RequestJson, Request) || !Request->TryGetNumberField(TEXT("schemaVersion"), Version)
        || Version != 1 || !Request->TryGetStringField(TEXT("operation"), Operation))
        Call = {false, TEXT("invalid_request"), TEXT("Expected schemaVersion 1 and an operation.")};
    else if ((Operation == TEXT("list") || Operation == TEXT("packaging.execute")
        || Operation == TEXT("mcp.call") || Operation == TEXT("cancel")) && !ConsoleProviders.Discover(Call))
    {
        // Discovery failures must not masquerade as an empty provider list.
    }
    else if (Operation == TEXT("list"))
    {
        Result = List();
        Call.bOk = true;
    }
    else if (Operation == TEXT("packaging.execute") || Operation == TEXT("mcp.call") || Operation == TEXT("cancel"))
    {
        FString ProviderId, Name, JobId, Kind;
        const FObject* Arguments = nullptr;
        if (!Request->TryGetStringField(TEXT("providerId"), ProviderId) || !ValidId(ProviderId)
            || !Request->TryGetStringField(TEXT("jobId"), JobId) || JobId.IsEmpty()
            || (Operation != TEXT("cancel") && (!Request->TryGetStringField(TEXT("name"), Name)
                || !Request->TryGetObjectField(TEXT("arguments"), Arguments)))
            || (Operation == TEXT("cancel") && (!Request->TryGetStringField(TEXT("kind"), Kind)
                || (Kind != TEXT("packaging") && Kind != TEXT("mcp")))))
            Call = {false, TEXT("invalid_request"), TEXT("Missing providerId, jobId, name/arguments or cancel kind.")};
        else
        {
            const FString Args = Arguments ? Json(*Arguments) : TEXT("{}");
            Call = (Operation == TEXT("packaging.execute") || Kind == TEXT("packaging"))
                ? Invoke(Find<IPackagingStepProvider>(ProviderId), Operation, Name, JobId, Args)
                : Invoke(Find<IMcpToolProvider>(ProviderId), Operation, Name, JobId, Args);
            if (Call.bOk && !Parse(Call.ResultJson, Result))
                Call = {false, TEXT("invalid_provider_result"), TEXT("Provider must return a JSON object.")};
        }
    }
    else Call = {false, TEXT("unknown_operation"), TEXT("Unknown provider operation.")};

    FObject Response = MakeShared<FJsonObject>();
    Response->SetNumberField(TEXT("schemaVersion"), 1);
    Response->SetNumberField(TEXT("contractVersion"), ContractVersion);
    Response->SetBoolField(TEXT("ok"), Call.bOk);
    Response->SetObjectField(TEXT("data"), Call.bOk ? Result : MakeShared<FJsonObject>());
    if (!Call.bOk)
    {
        FObject Error = MakeShared<FJsonObject>();
        Error->SetStringField(TEXT("code"), Call.Code);
        Error->SetStringField(TEXT("message"), Call.Message);
        Response->SetObjectField(TEXT("error"), Error);
    }
    else Response->SetField(TEXT("error"), MakeShared<FJsonValueNull>());
    bOk = Call.bOk;
    return Json(Response);
}
}
