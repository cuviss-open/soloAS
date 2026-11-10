// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "McpEditServer.h"
#include "McpEditLeases.h"
#include "AngelscriptCoreExtensionPoints.h"
#include "AngelscriptManager.h"
#include "AngelscriptSettings.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Widgets/Notifications/SNotificationList.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#endif

namespace SoloASDeveloperTools
{
namespace
{
using namespace AngelscriptCoreExtensionPoints;

struct FClient
{
    FSocket* Socket = nullptr;
    FGuid Owner = FGuid::NewGuid();
    bool bAuthenticated = false;
    bool bAwaitingCompile = false;
    bool bCloseRequested = false;
    FString DebugEndpoint;
    TArray<uint8> Input;
    TArray<uint8> Output;
};

class FMcpEditServer
{
#if WITH_DEV_AUTOMATION_TESTS
    friend class FSoloASMcpCompilationStateTest;
    friend class FSoloASMcpExplicitCompileTest;
    friend class FSoloASMcpRecoveryTest;
#endif
public:
    bool Start()
    {
        ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (!Sockets) return false;
        Listener = Sockets->CreateSocket(NAME_Stream, TEXT("SoloAS MCP edit"), false);
        if (!Listener) return false;
        TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
        bool bValid = false;
        Address->SetIp(TEXT("127.0.0.1"), bValid);
        Address->SetPort(0);
        if (!bValid || !Listener->SetNonBlocking(true) || !Listener->Bind(*Address) || !Listener->Listen(16))
        {
            Stop();
            return false;
        }
        Listener->GetAddress(*Address);
        Port = Address->GetPort();
        int32 DebugPort = GetDefault<UAngelscriptSettings>()->ConnectionPort;
        FParse::Value(FCommandLine::Get(), TEXT("-asdebugport="), DebugPort);
        SessionPath = FPaths::ProjectSavedDir() / TEXT("SoloASDeveloperTools/Mcp") /
            FString::Printf(TEXT("edit-session-%d-%u.json"), DebugPort, FPlatformProcess::GetCurrentProcessId());
        if (!RotateToken()) { Stop(); return false; }
        Observer = RegisterAutomaticReloadObserver(TEXT("soloas.mcp-batch-edit"),
            [this] { return Leases.IsPaused() || bCompileQueued; },
            [this](uint64 InGeneration) { OnCompileBegin(InGeneration); },
            [this](uint64 InGeneration, bool bSuccess, const TArray<FReloadDiagnostic>& InDiagnostics)
            { OnCompileComplete(InGeneration, bSuccess, InDiagnostics); });
        ReloadClient = RegisterExplicitReloadClient(TEXT("soloas.mcp-batch-edit"));
        if (!Observer.IsValid() || !ReloadClient.IsValid()) { Stop(); return false; }
        Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FMcpEditServer::Tick));
        return true;
    }

    void Stop()
    {
        if (Ticker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(Ticker); Ticker.Reset(); }
        if (TSharedPtr<SNotificationItem> Item = Notice.Pin()) Item->ExpireAndFadeout();
        if (Observer.IsValid()) { UnregisterAutomaticReloadObserver(Observer); Observer.Reset(); }
        if (ReloadClient.IsValid()) { UnregisterExplicitReloadClient(ReloadClient); ReloadClient.Reset(); }
        Leases.ResumeAll();
        ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        for (FClient& Client : Clients)
            if (Client.Socket && Sockets) { Client.Socket->Close(); Sockets->DestroySocket(Client.Socket); }
        Clients.Empty();
        if (Listener && Sockets) { Listener->Close(); Sockets->DestroySocket(Listener); }
        Listener = nullptr;
        if (!SessionPath.IsEmpty()) IFileManager::Get().Delete(*SessionPath, false, true);
    }

    void Resume()
    {
        if (Leases.IsPaused())
        {
            Leases.ResumeAll();
            QueueCompile();
        }
    }

private:
    bool RotateToken()
    {
        Token = FGuid::NewGuid().ToString(EGuidFormats::Digits) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("port"), Port);
        Json->SetStringField(TEXT("token"), Token);
        FString Text;
        if (!FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text))) return false;
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(SessionPath), true);
        const FString Temp = SessionPath + TEXT(".tmp");
        return FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            && IFileManager::Get().Move(*SessionPath, *Temp, true, true);
    }

    TSharedRef<FJsonObject> Snapshot() const
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("generation"), static_cast<double>(Generation));
        Json->SetStringField(TEXT("state"), State);
        Json->SetStringField(TEXT("lastCompileTime"), LastCompileTime);
        Json->SetBoolField(TEXT("success"), bLastSuccess);
        Json->SetArrayField(TEXT("diagnostics"), Diagnostics);
        return Json;
    }

    static bool SerializeMessage(const TSharedRef<FJsonObject>& Json, FString& Text)
    {
        return FJsonSerializer::Serialize(Json, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    }

    void Send(FClient& Client, const TSharedRef<FJsonObject>& Json)
    {
        FString Text;
        if (!SerializeMessage(Json, Text)) return;
        Text += TEXT("\n");
        FTCHARToUTF8 Bytes(*Text);
        if (Client.Output.Num() + Bytes.Length() > 1048576) { Client.bCloseRequested = true; return; }
        Client.Output.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
        Flush(Client);
    }

    void Flush(FClient& Client)
    {
        if (Client.Output.IsEmpty()) return;
        int32 Sent = 0;
        if (Client.Socket->Send(Client.Output.GetData(), Client.Output.Num(), Sent) && Sent > 0)
            Client.Output.RemoveAt(0, Sent, EAllowShrinking::No);
    }

    void Broadcast()
    {
        for (FClient& Client : Clients)
            if (Client.bAuthenticated) Send(Client, Snapshot());
    }

    void OnCompileBegin(uint64 InGeneration)
    {
        Generation = InGeneration;
        Diagnostics.Reset();
        State = TEXT("compiling");
        Broadcast();
    }

    void OnCompileComplete(uint64 InGeneration, bool bSuccess, const TArray<FReloadDiagnostic>& InDiagnostics)
    {
        Generation = InGeneration;
        State = TEXT("done");
        bLastSuccess = bSuccess;
        LastCompileTime = FDateTime::UtcNow().ToIso8601();
        Diagnostics.Reset();
        for (const FReloadDiagnostic& Item : InDiagnostics)
        {
            TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
            D->SetStringField(TEXT("file"), Item.Filename);
            D->SetStringField(TEXT("message"), Item.Message);
            D->SetNumberField(TEXT("line"), Item.Line);
            D->SetNumberField(TEXT("character"), Item.Column);
            D->SetStringField(TEXT("severity"), Item.bIsError ? TEXT("error") : Item.bIsInfo ? TEXT("info") : TEXT("warning"));
            Diagnostics.Add(MakeShared<FJsonValueObject>(D));
        }
        Broadcast();
    }

    void QueueCompile()
    {
        bCompileQueued = true;
        const double Now = FPlatformTime::Seconds();
        CompileNotBefore = Now + 0.1;
        CompileDeadline = Now + 30.0;
        State = TEXT("pending");
        Broadcast();
    }

    void UpdateNotice()
    {
        if (Leases.IsPaused() && !Notice.IsValid())
        {
            FNotificationInfo Info(FText::FromString(TEXT("Script automatic compilation is paused. Manual IDE saves are also deferred. Use SoloAS > AI-assisted development > Resume automatic script compilation to resume now.")));
            Info.bFireAndForget = false;
            Notice = FSlateNotificationManager::Get().AddNotification(Info);
        }
        else if (!Leases.IsPaused())
        {
            if (TSharedPtr<SNotificationItem> Item = Notice.Pin()) Item->ExpireAndFadeout();
            Notice.Reset();
        }
    }

    void HandleDisconnect(const FGuid& Owner)
    {
        const bool bWasPaused = Leases.IsPaused();
        Leases.Disconnect(Owner);
        PendingIds.Remove(Owner);
        if (bWasPaused && !Leases.IsPaused()) QueueCompile();
    }

    void ExpireLeases(double Now)
    {
        const bool bWasPaused = Leases.IsPaused();
        Leases.Expire(Now);
        if (bWasPaused && !Leases.IsPaused()) QueueCompile();
    }

    void Reply(FClient& Client, const FString& Id, const FString& Error = FString())
    {
        TSharedRef<FJsonObject> Json = Snapshot();
        Json->SetStringField(TEXT("id"), Id);
        if (!Error.IsEmpty()) Json->SetStringField(TEXT("error"), Error);
        Send(Client, Json);
    }

    void Handle(FClient& Client, const FString& Text)
    {
        TSharedPtr<FJsonObject> Json;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid()) return;
        FString Id, Op;
        Json->TryGetStringField(TEXT("id"), Id);
        Json->TryGetStringField(TEXT("op"), Op);
        if (!Client.bAuthenticated)
        {
            FString Supplied, DebugEndpoint;
            if (Op != TEXT("hello") || !Json->TryGetStringField(TEXT("token"), Supplied) || Supplied != Token
                || !Json->TryGetStringField(TEXT("debugEndpoint"), DebugEndpoint) || !HasDebugClientEndpoint(DebugEndpoint)
                || !RotateToken()) { Client.bCloseRequested = true; return; }
            Client.bAuthenticated = true;
            Client.DebugEndpoint = MoveTemp(DebugEndpoint);
            Reply(Client, Id);
            return;
        }
        if (Op == TEXT("begin"))
        {
            if (Leases.HasOwner(Client.Owner) || Client.bAwaitingCompile) { Reply(Client, Id, TEXT("owner_busy")); return; }
            double Seconds = 120;
            Json->TryGetNumberField(TEXT("timeoutSeconds"), Seconds);
            const FGuid Lease = Leases.Begin(Client.Owner, FPlatformTime::Seconds(), Seconds);
            TSharedRef<FJsonObject> Result = Snapshot();
            Result->SetStringField(TEXT("id"), Id);
            Result->SetStringField(TEXT("leaseId"), Lease.ToString(EGuidFormats::Digits));
            Send(Client, Result);
        }
        else if (Op == TEXT("end"))
        {
            FString LeaseString;
            FGuid Lease;
            if (!Json->TryGetStringField(TEXT("leaseId"), LeaseString) || !FGuid::Parse(LeaseString, Lease)
                || !Leases.End(Client.Owner, Lease)) { Reply(Client, Id, TEXT("invalid_lease")); return; }
            Client.bAwaitingCompile = true;
            PendingIds.Add(Client.Owner, Id);
            QueueCompile();
        }
        else if (Op == TEXT("compile"))
        {
            if (Leases.IsPaused() || Client.bAwaitingCompile) { Reply(Client, Id, TEXT("edit_in_progress")); return; }
            Client.bAwaitingCompile = true;
            PendingIds.Add(Client.Owner, Id);
            QueueCompile();
        }
        else if (Op == TEXT("status")) Reply(Client, Id);
        else Reply(Client, Id, TEXT("unknown_operation"));
    }

    void RunCompile()
    {
        if (!FAngelscriptManager::IsInitialized())
        {
            if (FPlatformTime::Seconds() >= CompileDeadline) FinishRequests(TEXT("editor_not_ready"));
            return;
        }
        auto& Manager = FAngelscriptManager::Get();
        TArray<FAngelscriptManager::FFilenamePair> Files;
        Files.Append(Manager.FileChangesDetectedForReload);
        for (const auto& File : Manager.FileDeletionsDetectedForReload) Files.AddUnique(File);
        if (Files.IsEmpty()) Manager.FindAllScriptFilenames(Files);
        FExplicitReloadRequest Request;
        Request.RequestId = ++NextRequest;
        TSet<FString> RelativePaths;
        for (const auto& File : Files)
        {
            FString Relative = File.RelativePath, Absolute = File.AbsolutePath;
            FPaths::NormalizeFilename(Relative);
            FPaths::NormalizeFilename(Absolute);
            if (RelativePaths.Contains(Relative)) continue;
            RelativePaths.Add(Relative);
            Request.Files.Add({Relative, Absolute, !IFileManager::Get().FileExists(*Absolute)});
        }
        FString Error;
        TGuardValue<bool> CompileGuard(bExecutingCompile, true);
        const EReloadAdmission Admission = TryExplicitReload(ReloadClient, Request,
            [](const FReloadResultView&) {}, Error);
        if (Admission == EReloadAdmission::Busy && FPlatformTime::Seconds() < CompileDeadline)
        {
            CompileNotBefore = FPlatformTime::Seconds() + 0.1;
            return;
        }
        if (Admission == EReloadAdmission::Busy) Error = TEXT("compile_busy");
        FinishRequests(Admission == EReloadAdmission::Accepted ? FString() : Error);
    }

    void FinishRequests(const FString& Error)
    {
        bCompileQueued = false;
        if (!Error.IsEmpty())
        {
            State = Generation == 0 ? TEXT("idle") : TEXT("done");
            Broadcast();
        }
        for (FClient& Client : Clients)
        {
            if (!Client.bAwaitingCompile) continue;
            Client.bAwaitingCompile = false;
            const FString Id = PendingIds.FindRef(Client.Owner);
            Reply(Client, Id, Error);
            PendingIds.Remove(Client.Owner);
        }
    }

    bool Tick(float)
    {
        // Slow-task dialogs can pump the ticker inside a reload. Keep new requests
        // queued until its completion so they cannot receive the previous result.
        if (bExecutingCompile || State == TEXT("compiling")) return true;
        bool bPending = false;
        while (Listener && Listener->HasPendingConnection(bPending) && bPending)
        {
            FSocket* Socket = Listener->Accept(TEXT("SoloAS MCP edit client"));
            if (!Socket) break;
            Socket->SetNonBlocking(true);
            FClient& Client = Clients.AddDefaulted_GetRef();
            Client.Socket = Socket;
        }
        for (int32 Index = Clients.Num() - 1; Index >= 0; --Index)
        {
            FClient& Client = Clients[Index];
            bool bClosed = Client.bAuthenticated && !HasDebugClientEndpoint(Client.DebugEndpoint);
            uint8 Buffer[4096];
            while (!bClosed && Client.Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
            {
                int32 Read = 0;
                if (!Client.Socket->Recv(Buffer, sizeof(Buffer), Read) || Read <= 0) { bClosed = true; break; }
                Client.Input.Append(Buffer, Read);
                if (Client.Input.Num() > 1048576) { bClosed = true; break; }
                int32 Newline;
                while (Client.Input.Find(uint8('\n'), Newline))
                {
                    Client.Input[Newline] = 0;
                    const FString Text = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Client.Input.GetData()));
                    Handle(Client, Text);
                    Client.Input.RemoveAt(0, Newline + 1, EAllowShrinking::No);
                }
            }
            Flush(Client);
            if (bClosed || Client.bCloseRequested || Client.Socket->GetConnectionState() == SCS_ConnectionError)
            {
                HandleDisconnect(Client.Owner);
                Client.Socket->Close();
                ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client.Socket);
                Clients.RemoveAt(Index);
            }
        }
        ExpireLeases(FPlatformTime::Seconds());
        if (!bCompileQueued && FAngelscriptManager::IsInitialized())
        {
            const auto& Manager = FAngelscriptManager::Get();
            if ((!Manager.FileChangesDetectedForReload.IsEmpty() || !Manager.FileDeletionsDetectedForReload.IsEmpty())
                && State != TEXT("pending"))
            {
                State = TEXT("pending");
                Broadcast();
            }
        }
        UpdateNotice();
        if (bCompileQueued && !Leases.IsPaused() && FPlatformTime::Seconds() >= CompileNotBefore) RunCompile();
        return true;
    }

    FSocket* Listener = nullptr;
    TArray<FClient> Clients;
    FMcpEditLeases Leases;
    FDelegateHandle Observer, ReloadClient;
    FTSTicker::FDelegateHandle Ticker;
    TWeakPtr<SNotificationItem> Notice;
    TMap<FGuid, FString> PendingIds;
    FString Token, SessionPath, State = TEXT("idle"), LastCompileTime;
    TArray<TSharedPtr<FJsonValue>> Diagnostics;
    int32 Port = 0;
    uint64 Generation = 0, NextRequest = 0;
    double CompileNotBefore = 0, CompileDeadline = 0;
    bool bCompileQueued = false, bLastSuccess = false, bExecutingCompile = false;
};

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoloASMcpCompilationStateTest, "SoloAS.DeveloperTools.Mcp.CompilationState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSoloASMcpCompilationStateTest::RunTest(const FString&)
{
    FMcpEditServer Subject;
    Subject.OnCompileBegin(1);
    TArray<FReloadDiagnostic> First{{TEXT("first.as"), TEXT("first error"), 2, 3, true, false}};
    Subject.OnCompileComplete(1, false, First);
    FString Encoded;
    TestTrue(TEXT("State response serializes"), Subject.SerializeMessage(Subject.Snapshot(), Encoded));
    TestFalse(TEXT("State response fits one JSON line"), Encoded.Contains(TEXT("\n")) || Encoded.Contains(TEXT("\r")));
    TSharedPtr<FJsonObject> Decoded;
    TestTrue(TEXT("Single-line state response parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Encoded), Decoded) && Decoded.IsValid());
    TestEqual(TEXT("First compilation gets generation one"), Subject.Generation, uint64(1));
    TestEqual(TEXT("First compilation records its own diagnostic"), Subject.Diagnostics.Num(), 1);
    Subject.OnCompileBegin(2);
    TestEqual(TEXT("Generation increases monotonically"), Subject.Generation, uint64(2));
    TestEqual(TEXT("Previous diagnostics are absent while compiling"), Subject.Diagnostics.Num(), 0);
    TArray<FReloadDiagnostic> Second{{TEXT("second.as"), TEXT("second error"), 4, 5, true, false}};
    Subject.OnCompileComplete(2, false, Second);
    TestEqual(TEXT("Second compilation has one diagnostic"), Subject.Diagnostics.Num(), 1);
    if (Subject.Diagnostics.Num() == 1)
        TestEqual(TEXT("Second compilation owns its diagnostic"), Subject.Diagnostics[0]->AsObject()->GetStringField(TEXT("file")), FString(TEXT("second.as")));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoloASMcpExplicitCompileTest, "SoloAS.DeveloperTools.Mcp.ExplicitCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSoloASMcpExplicitCompileTest::RunTest(const FString&)
{
    if (!TestTrue(TEXT("Runtime initialized"), FAngelscriptManager::IsInitialized())) return false;
    FMcpEditServer Subject;
    Subject.Observer = RegisterAutomaticReloadObserver(TEXT("soloas.mcp-compile-test"),
        [&Subject] { return Subject.Leases.IsPaused() || Subject.bCompileQueued; },
        [&Subject](uint64 InGeneration) { Subject.OnCompileBegin(InGeneration); },
        [&Subject](uint64 InGeneration, bool bSuccess, const TArray<FReloadDiagnostic>& Items) { Subject.OnCompileComplete(InGeneration, bSuccess, Items); });
    Subject.ReloadClient = RegisterExplicitReloadClient(TEXT("soloas.mcp-compile-test"));
    const FDelegateHandle Client = Subject.ReloadClient;
    const FString Relative = TEXT("McpExplicit_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".as");
    FString Absolute = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / Relative);
    FPaths::NormalizeFilename(Absolute);
    auto& Manager = FAngelscriptManager::Get();
    const FAngelscriptManager::FFilenamePair File{Absolute, Relative};
    ON_SCOPE_EXIT
    {
        Manager.FileChangesDetectedForReload.Remove(File);
        Subject.Stop();
        IFileManager::Get().Delete(*Absolute);
    };
    if (!TestTrue(TEXT("Consumer registrations are valid"), Subject.Observer.IsValid() && Client.IsValid())) return false;
    if (!TestTrue(TEXT("Compile input is written"), FFileHelper::SaveStringToFile(TEXT("int McpExplicitValue() { return 1; }"), *Absolute))) return false;
    Manager.FileChangesDetectedForReload.AddUnique(File);
    Subject.QueueCompile();
    Subject.RunCompile();
    TestTrue(TEXT("Explicit consumer compilation commits"), Subject.Generation > 0 && Subject.bLastSuccess && Subject.State == TEXT("done"));
    TestEqual(TEXT("Successful compilation has no diagnostics"), Subject.Diagnostics.Num(), 0);
    Subject.Stop();
    FExplicitReloadRequest Request;
    Request.RequestId = 99;
    Request.Files.Add({Relative, Absolute, false});
    FString Error;
    TestTrue(TEXT("Removed explicit consumer rejects requests"), TryExplicitReload(Client, Request,
        [](const FReloadResultView&) {}, Error) == EReloadAdmission::Invalid);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoloASMcpRecoveryTest, "SoloAS.DeveloperTools.Mcp.Recovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSoloASMcpRecoveryTest::RunTest(const FString&)
{
    FMcpEditServer Subject;
    const FGuid A = FGuid::NewGuid(), B = FGuid::NewGuid();
    Subject.Leases.Begin(A, 100, 10);
    Subject.Leases.Begin(B, 100, 20);
    Subject.HandleDisconnect(A);
    TestTrue(TEXT("Other client's lease remains active after disconnect"), Subject.Leases.IsPaused());
    TestFalse(TEXT("Disconnect does not compile while another lease remains"), Subject.bCompileQueued);
    Subject.HandleDisconnect(B);
    TestFalse(TEXT("Last disconnect releases the pause"), Subject.Leases.IsPaused());
    TestTrue(TEXT("Last disconnect queues compilation"), Subject.bCompileQueued);
    Subject.bCompileQueued = false;
    Subject.Leases.Begin(A, 200, 2);
    Subject.State = TEXT("compiling");
    Subject.Tick(0);
    TestTrue(TEXT("Nested tick preserves the active lease"), Subject.Leases.IsPaused());
    TestFalse(TEXT("Nested tick cannot schedule another compile"), Subject.bCompileQueued);
    Subject.State = TEXT("done");
    Subject.ExpireLeases(202);
    TestFalse(TEXT("Expired lease releases the pause"), Subject.Leases.IsPaused());
    TestTrue(TEXT("Expired last lease queues compilation"), Subject.bCompileQueued);
    return !HasAnyErrors();
}
#endif

TUniquePtr<FMcpEditServer> GMcpEditServerInstance;
}

void StartMcpEditServer()
{
    GMcpEditServerInstance = MakeUnique<FMcpEditServer>();
    if (!GMcpEditServerInstance->Start()) GMcpEditServerInstance.Reset();
}
void StopMcpEditServer() { if (GMcpEditServerInstance) GMcpEditServerInstance->Stop(); GMcpEditServerInstance.Reset(); }
void ResumeMcpAutomaticCompilation() { if (GMcpEditServerInstance) GMcpEditServerInstance->Resume(); }
}
