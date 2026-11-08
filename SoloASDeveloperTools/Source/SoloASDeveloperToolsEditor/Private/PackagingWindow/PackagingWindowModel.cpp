#include "PackagingWindowModel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace SoloASDeveloperTools
{
namespace
{
bool IsJobCommand(const FString& Command)
{
    return Command == TEXT("run") || Command == TEXT("clean-cache") ||
        Command == TEXT("generate-precompiled") || Command == TEXT("sync-jit");
}

FString Json(const TSharedRef<FJsonValue>& Value)
{
    FString Text;
    FJsonSerializer::Serialize(Value, TEXT(""), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    return Text;
}

TSharedPtr<FJsonObject> Object(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field)
{
    const TSharedPtr<FJsonObject>* Value = nullptr;
    return Parent.IsValid() && Parent->TryGetObjectField(Field, Value) ? *Value : nullptr;
}

FString String(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field)
{
    FString Value;
    if (Parent.IsValid()) Parent->TryGetStringField(Field, Value);
    return Value;
}
}

FPackagingWindowModel::FPackagingWindowModel(TSharedRef<IPackagingWindowBackend> InBackend,
    TSharedRef<IPackagingWindowBackend> InControl, FString InProject)
    : Overrides(MakeShared<FJsonObject>()), Backend(InBackend), Control(InControl), Project(MoveTemp(InProject))
{
}

bool FPackagingWindowModel::Start(TSharedRef<IPackagingWindowBackend> Transport, const FString& Command,
    const TArray<FString>& Arguments)
{
    FString Error;
    if (!Transport->Start(Arguments, Error))
    {
        Message = Error;
        return false;
    }
    if (Command != TEXT("logs") && Command != TEXT("status")) Message = Command + TEXT("…");
    return true;
}

FString FPackagingWindowModel::OverridesJson() const
{
    return Json(MakeShared<FJsonValueObject>(Overrides));
}

bool FPackagingWindowModel::Request(const FString& Command)
{
    if (IsBusy()) return false;
    TArray<FString> Arguments = {TEXT("--project"), Project, TEXT("--json")};
    if (Command == TEXT("profiles")) Arguments.Append({TEXT("profiles"), TEXT("list")});
    else if (Command == TEXT("config") || Command == TEXT("precheck") || Command == TEXT("run") ||
        Command == TEXT("clean-cache") || Command == TEXT("generate-precompiled") || Command == TEXT("sync-jit"))
    {
        Arguments.Add(Command);
        if (Command == TEXT("config")) Arguments.Add(TEXT("read"));
        if (!Profile.IsEmpty()) Arguments.Append({TEXT("--profile"), Profile});
        Arguments.Append({TEXT("--overrides"), OverridesJson()});
        if (IsJobCommand(Command))
        {
            JobId = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
            Arguments.Append({TEXT("--job-id"), JobId});
            State = TEXT("queued"); Phase.Reset(); Log.Reset(); History.Reset(); Job.Reset();
            LogOffset = 0; bCancelRequested = false; bReadLogsNext = false; bLogEof = false; bRetryCancel = false; bLogTruncated = false;
        }
    }
    else
    {
        Message = TEXT("This operation is not available in the packaging CLI contract.");
        return false;
    }
    if (!Start(Backend, Command, Arguments))
    {
        if (IsJobCommand(Command)) { JobId.Reset(); State.Reset(); bLogEof = true; }
        return false;
    }
    PendingCommand = Command;
    PendingAction.Reset();
    return true;
}

bool FPackagingWindowModel::SaveProject(bool bPreview, const FString& Platform)
{
    if (IsBusy()) return false;
    if (!Config.IsValid()) { Message = TEXT("Resolve valid configuration before saving project settings."); return false; }
    auto Changes = MakeShared<FJsonObject>();
    for (const TCHAR* Field : {TEXT("target"), TEXT("configuration"), TEXT("pak"), TEXT("ioStore"), TEXT("maps"), TEXT("cookDirectories"), TEXT("extraFiles")})
        if (auto Value = Overrides->TryGetField(Field)) Changes->SetField(Field, Value);
    if (Changes->Values.IsEmpty()) { Message = TEXT("No UE packaging invocation edits to save. Delivery settings are saved in profiles."); return false; }
    TArray<FString> Arguments = {TEXT("--project"), Project, TEXT("--json"), TEXT("config"), TEXT("write"), TEXT("--overrides"), Json(MakeShared<FJsonValueObject>(Changes))};
    if (!Platform.IsEmpty()) Arguments.Append({TEXT("--platform"), Platform});
    if (bPreview) Arguments.Add(TEXT("--dry-run"));
    if (!Start(Backend, TEXT("config"), Arguments)) return false;
    PendingCommand = TEXT("config");
    PendingAction = bPreview ? TEXT("project_preview") : TEXT("project_save");
    bProjectPreviewReady = false;
    return true;
}

bool FPackagingWindowModel::SaveProfile(const FString& Name, bool bReplace)
{
    if (IsBusy() || !Config.IsValid() || !IsValidProfileName(Name))
    { Message = TEXT("Resolve valid configuration and enter a profile name before saving."); return false; }
    const auto Effective = Object(Config, TEXT("effective"));
    if (!Effective.IsValid()) return false;
    TArray<FString> Arguments = {TEXT("--project"), Project, TEXT("--json"), TEXT("profiles"), TEXT("save"), TEXT("--name"), Name,
        TEXT("--overrides"), Json(MakeShared<FJsonValueObject>(Effective))};
    if (bReplace) Arguments.Add(TEXT("--replace"));
    if (!Start(Backend, TEXT("profiles"), Arguments)) return false;
    PendingCommand = TEXT("profiles"); PendingAction = TEXT("profile_save");
    return true;
}

bool FPackagingWindowModel::IsValidProfileName(const FString& Name)
{
    if (Name.IsEmpty()) return false;
    for (TCHAR C : Name)
        if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_' || C == '-')) return false;
    return true;
}

bool FPackagingWindowModel::EditProfile(const FString& Name, const FString& NewName)
{
    if (IsBusy() || !Profiles.Contains(Name) || (!NewName.IsEmpty() && (!IsValidProfileName(NewName) || Profiles.Contains(NewName)))) return false;
    TArray<FString> Args = {TEXT("--project"), Project, TEXT("--json"), TEXT("profiles"), NewName.IsEmpty() ? TEXT("delete") : TEXT("rename"), TEXT("--name"), Name};
    if (!NewName.IsEmpty()) Args.Append({TEXT("--new-name"), NewName});
    if (!Start(Backend, TEXT("profiles"), Args)) return false;
    PendingCommand = TEXT("profiles"); PendingAction = TEXT("profile_edit");
    return true;
}

bool FPackagingWindowModel::CanPackage() const
{
    // A resolved configuration is required. The run itself retains the CLI's full precheck.
    return Config.IsValid() && !IsBusy();
}

bool FPackagingWindowModel::CheckPackage(const FString& Path)
{
    if (IsBusy()) return false;
    if (Path.TrimStartAndEnd().IsEmpty()) { Message = TEXT("Enter an existing package directory."); return false; }
    if (!Start(Backend, TEXT("check-package"), {TEXT("--project"), Project, TEXT("--json"), TEXT("check-package"), TEXT("--path"), Path, TEXT("--mode"), TEXT("auto")})) return false;
    PendingCommand = TEXT("check-package"); PendingAction.Reset();
    return true;
}

bool FPackagingWindowModel::QueryJob(const FString& Id)
{
    if (Control->IsBusy()) return false;
    bool bValid = Id.Len() == 32;
    for (TCHAR C : Id) bValid &= (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
    if (!bValid)
    {
        // Validate before passing the identifier across a process boundary.
        Message = TEXT("Job ID must contain 32 lowercase hexadecimal characters.");
        return false;
    }
    if (!Start(Control, TEXT("status"), {TEXT("--project"), Project, TEXT("--json"), TEXT("status"), TEXT("--job"), Id})) return false;
    if (Id != JobId) { Log.Reset(); History.Reset(); State.Reset(); Job.Reset(); LogOffset = 0; bCancelRequested = false; bLogEof = false; bLogTruncated = false; }
    JobId = Id;
    PendingControl = TEXT("status");
    return true;
}

bool FPackagingWindowModel::CanCancel() const
{
    return !Control->IsBusy() && !bCancelRequested && !JobId.IsEmpty() && (State == TEXT("queued") || State == TEXT("prechecking") || State == TEXT("running"));
}

bool FPackagingWindowModel::Cancel()
{
    if (!CanCancel()) return false;
    if (!Start(Control, TEXT("cancel"), {TEXT("--project"), Project, TEXT("--json"), TEXT("cancel"), TEXT("--job"), JobId})) return false;
    PendingControl = TEXT("cancel");
    bRetryCancel = false;
    // Only the executor may report cancelled after stopping and restoring its work.
    return true;
}

bool FPackagingWindowModel::PollJob()
{
    if (JobId.IsEmpty() || Control->IsBusy()) return false;
    if (bRetryCancel) return Cancel();
    const bool bLogs = bReadLogsNext;
    bReadLogsNext = !bReadLogsNext;
    if (!bLogs) return QueryJob(JobId);
    if (!Start(Control, TEXT("logs"), {TEXT("--project"), Project, TEXT("--json"), TEXT("logs"), TEXT("--job"), JobId,
        TEXT("--offset"), LexToString(LogOffset), TEXT("--limit"), TEXT("65536")})) return false;
    PendingControl = TEXT("logs");
    return true;
}

bool FPackagingWindowModel::NeedsJobPoll() const
{
    if (Backend->IsBusy() && !IsJobCommand(PendingCommand)) return false;
    return !JobId.IsEmpty() && (Backend->IsBusy() || !bLogEof || State == TEXT("running") || State == TEXT("prechecking") || State == TEXT("queued") || State == TEXT("cancelling"));
}

bool FPackagingWindowModel::IsBusy() const
{
    const bool bActiveJob = Job.IsValid() && (State == TEXT("queued") || State == TEXT("prechecking") || State == TEXT("running") || State == TEXT("cancelling"));
    return Backend->IsBusy() || Control->IsBusy() || bActiveJob;
}

bool FPackagingWindowModel::SetOverride(const FString& Field, const FString& JsonValue)
{
    if (IsBusy()) return false;
    TSharedPtr<FJsonObject> Parsed;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("{\"value\":") + JsonValue + TEXT("}")), Parsed))
    {
        Message = TEXT("Invalid JSON value for ") + Field;
        return false;
    }
    Overrides->SetField(Field, Parsed->Values[TEXT("value")]);
    Config.Reset();
    bProjectPreviewReady = false;
    bReadyToPackage = false;
    Message = TEXT("Unsaved invocation override. Refresh values or check configuration to resolve it.");
    return true;
}

void FPackagingWindowModel::ClearOverrides()
{
    if (IsBusy()) return;
    Overrides = MakeShared<FJsonObject>();
    bReadyToPackage = false;
    Request(TEXT("config"));
}

FString FPackagingWindowModel::Value(const FString& Layer, const FString& Field) const
{
    const auto Values = Object(Config, *Layer);
    const auto Value = Values.IsValid() ? Values->TryGetField(Field) : nullptr;
    return Value.IsValid() ? Json(Value.ToSharedRef()) : TEXT("—");
}

FString FPackagingWindowModel::Source(const FString& Field) const
{
    return String(Object(Config, TEXT("sources")), *Field);
}

void FPackagingWindowModel::SetJob(const TSharedPtr<FJsonObject>& NewJob)
{
    const FString NewState = String(NewJob, TEXT("state"));
    const bool bTerminal = State == TEXT("succeeded") || State == TEXT("failed") || State == TEXT("cancelled");
    if (String(NewJob, TEXT("jobId")) == JobId && bTerminal && (NewState == TEXT("queued") || NewState == TEXT("prechecking") || NewState == TEXT("running") || NewState == TEXT("cancelling"))) return;
    Job = NewJob;
    JobId = String(Job, TEXT("jobId"));
    State = String(Job, TEXT("state"));
    Phase = String(Job, TEXT("phase"));
    History.Reset();
    const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
    if (Job->TryGetArrayField(TEXT("history"), Entries))
        for (const auto& Entry : *Entries)
        {
            auto Item = Entry->AsObject();
            History += String(Item, TEXT("at")) + TEXT("  ") + String(Item, TEXT("state")) + TEXT("  ") + String(Item, TEXT("phase")) + LINE_TERMINATOR;
        }
}

void FPackagingWindowModel::Consume(const FPackagingCliResult& Result, const FString& Command)
{
    TSharedPtr<FJsonObject> Envelope;
    double Version = 0;
    bool bOk = false;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result.Output), Envelope) ||
        !Envelope.IsValid() || !Envelope->TryGetNumberField(TEXT("schemaVersion"), Version) || Version != 1 ||
        !Envelope->TryGetBoolField(TEXT("ok"), bOk) || String(Envelope, TEXT("command")) != Command || !Object(Envelope, TEXT("data")).IsValid())
    {
        Message = TEXT("Invalid CLI response (exit ") + FString::FromInt(Result.ExitCode) + TEXT("). ") + Result.Error + TEXT("\n") + Result.Output.Left(2048);
        bReadyToPackage = false;
        if (Command == TEXT("config")) Config.Reset();
        if (IsJobCommand(Command)) { State.Reset(); bLogEof = true; }
        return;
    }
    const auto Data = Object(Envelope, TEXT("data"));
    if (String(Object(Envelope, TEXT("error")), TEXT("code")) == TEXT("job_missing"))
    {
        if (Job.IsValid()) return; // A startup query may finish after run has returned its final snapshot.
        if (Backend->IsBusy())
        {
            if (Command == TEXT("cancel")) { bRetryCancel = true; Message = TEXT("Waiting for job creation to deliver cancellation."); }
            return;
        }
        bLogEof = true; bRetryCancel = false; State.Reset(); Phase.Reset();
    }
    const auto ReturnedJob = Object(Data, TEXT("job"));
    if (ReturnedJob.IsValid() && String(ReturnedJob, TEXT("jobId")) != JobId)
    { Message = TEXT("CLI returned a different job identifier."); return; }
    if (ReturnedJob.IsValid()) SetJob(ReturnedJob);
    if (Command == TEXT("logs") && bOk && Result.ExitCode == 0)
    {
        double Next = 0;
        if (!Data->TryGetNumberField(TEXT("nextOffset"), Next) || Next < static_cast<double>(LogOffset))
        { Message = TEXT("Invalid log byte offset in CLI response."); return; }
        Log += String(Data, TEXT("text"));
        if (Log.Len() > 1024 * 1024) { Log = Log.Right(1024 * 1024); bLogTruncated = true; }
        LogOffset = static_cast<int64>(Next);
        Data->TryGetBoolField(TEXT("eof"), bLogEof);
        return;
    }
    Data->TryGetBoolField(TEXT("cancelRequested"), bCancelRequested);
    if (Command == TEXT("config") && PendingAction.IsEmpty())
    {
        Config = bOk && Result.ExitCode == 0 ? Data : nullptr;
        if (Config.IsValid() && (!Object(Config, TEXT("effective")).IsValid() || !Object(Config, TEXT("projectValues")).IsValid() || !Object(Config, TEXT("profileOverrides")).IsValid()))
        { Config.Reset(); Message = TEXT("CLI configuration response is missing value layers."); return; }
    }
    if (Command == TEXT("config") && PendingAction == TEXT("project_preview"))
    {
        ProjectDiff = String(Data, TEXT("diff"));
        bProjectPreviewReady = bOk && Result.ExitCode == 0;
    }
    if (Command == TEXT("precheck"))
    {
        if (Object(Data, TEXT("config")).IsValid()) Config = Object(Data, TEXT("config"));
        bReadyToPackage = false;
        Data->TryGetBoolField(TEXT("readyToPackage"), bReadyToPackage);
        bReadyToPackage &= bOk && Result.ExitCode == 0;
    }
    if (Command == TEXT("profiles") && bOk && PendingAction != TEXT("profile_save"))
    {
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        Profiles.Reset();
        if (Data->TryGetArrayField(TEXT("profiles"), Entries))
            for (const auto& Entry : *Entries) Profiles.Add(String(Entry->AsObject(), TEXT("name")));
    }
    if (Command != TEXT("cancel") && Command != TEXT("status")) Details = Json(MakeShared<FJsonValueObject>(Data));
    Message = bOk && Result.ExitCode == 0 ? TEXT("Completed: ") + Command :
        String(Object(Envelope, TEXT("error")), TEXT("code")) + TEXT(": ") + String(Object(Envelope, TEXT("error")), TEXT("message"));
    if (Command == TEXT("precheck") && bOk && !bReadyToPackage)
        Message = TEXT("Configuration checked; packaging readiness has not been established. See pending checks below.");
    if (Command == TEXT("cancel") && bOk && bCancelRequested) Message = TEXT("Cancellation requested. Waiting for executor cleanup and final state.");
    if (Command == TEXT("profiles") && bOk && PendingAction == TEXT("profile_save"))
    {
        const FString Name = String(Data, TEXT("name"));
        if (!Name.IsEmpty()) Profiles.AddUnique(Name);
    }
    if (Command == TEXT("profiles") && bOk && Result.ExitCode == 0 && PendingAction == TEXT("profile_edit"))
    {
        Profile = String(Data, TEXT("name"));
        Overrides = MakeShared<FJsonObject>();
        Config.Reset();
    }
    if (ReturnedJob.IsValid() && (State == TEXT("failed") || State == TEXT("cancelled")))
    {
        const auto Error = Object(Job, TEXT("error"));
        const FString JobError = String(Error, TEXT("message"));
        Message = State + TEXT(": ") + (!JobError.IsEmpty() ? String(Error, TEXT("code")) + TEXT(": ") + JobError : Message);
    }
    if (IsJobCommand(Command))
    {
        bLogEof = !ReturnedJob.IsValid();
        if (!ReturnedJob.IsValid()) { State.Reset(); Phase.Reset(); }
    }
    if (!Result.Error.IsEmpty()) Message += TEXT("\n") + Result.Error;
    if (Command == TEXT("config") && PendingAction == TEXT("project_save") && bOk && Result.ExitCode == 0) Request(TEXT("config"));
}

void FPackagingWindowModel::Tick()
{
    FPackagingCliResult Result;
    if (Backend->Poll(Result)) Consume(Result, PendingCommand);
    if (Control->Poll(Result)) Consume(Result, PendingControl);
}
}
