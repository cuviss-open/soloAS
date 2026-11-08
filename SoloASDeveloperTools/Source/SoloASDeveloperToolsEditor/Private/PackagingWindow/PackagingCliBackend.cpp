#include "PackagingWindowModel.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace SoloASDeveloperTools
{
FString QuotePackagingArgument(const FString& Argument)
{
#if PLATFORM_WINDOWS
    // Windows CRT argv quoting, including quotes in --overrides and trailing slashes.
    FString Result = TEXT("\"");
    int32 Slashes = 0;
    for (TCHAR C : Argument)
    {
        if (C == '\\') { ++Slashes; continue; }
        Result += FString::ChrN(C == '"' ? Slashes * 2 + 1 : Slashes, '\\');
        Result += C;
        Slashes = 0;
    }
    return Result + FString::ChrN(Slashes * 2, '\\') + TEXT("\"");
#else
    FString Escaped = Argument.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
    return TEXT("\"") + Escaped + TEXT("\"");
#endif
}

namespace
{
class FPackagingCliBackend final : public IPackagingWindowBackend
{
public:
    ~FPackagingCliBackend() override
    {
        if (Process.IsValid())
        {
            if (FPlatformProcess::IsProcRunning(Process)) FPlatformProcess::TerminateProc(Process, true);
            FPlatformProcess::CloseProc(Process);
        }
        ClosePipes();
    }

    bool IsBusy() const override { return Process.IsValid(); }

    bool Start(const TArray<FString>& Arguments, FString& Error) override
    {
        if (IsBusy()) { Error = TEXT("A CLI request is still running."); return false; }
        const FString Module = FModuleManager::Get().GetModuleFilename(TEXT("SoloASDeveloperToolsEditor"));
        const FString Script = FPaths::ConvertRelativePathToFull(FPaths::GetPath(Module) / TEXT("../../Build/PackagingTool/soloas_package.py"));
#if PLATFORM_WINDOWS
        const FString Python = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
#elif PLATFORM_MAC
        const FString Python = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Mac/bin/python3"));
#else
        const FString Python = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Linux/bin/python3"));
#endif
        if (!IFileManager::Get().FileExists(*Script) || !IFileManager::Get().FileExists(*Python))
        {
            Error = TEXT("Packaging CLI or engine Python is missing: ") + Script + TEXT(" ; ") + Python;
            return false;
        }
        if (!FPlatformProcess::CreatePipe(ReadOut, WriteOut) || !FPlatformProcess::CreatePipe(ReadErr, WriteErr))
        {
            ClosePipes(); Error = TEXT("Cannot create CLI output pipes."); return false;
        }
        FString Parameters = TEXT("-X utf8 ") + QuotePackagingArgument(Script);
        for (const auto& Argument : Arguments) Parameters += TEXT(" ") + QuotePackagingArgument(Argument);
        Output.Reset(); Errors.Reset();
        Process = FPlatformProcess::CreateProc(*Python, *Parameters, false, true, true, nullptr, 0, nullptr, WriteOut, nullptr, WriteErr);
        if (!Process.IsValid()) { ClosePipes(); Error = TEXT("Cannot start packaging CLI."); return false; }
        StartedAt = FPlatformTime::Seconds();
        return true;
    }

    bool Poll(FPackagingCliResult& Result) override
    {
        if (!IsBusy()) return false;
        Output += FPlatformProcess::ReadPipe(ReadOut);
        Errors += FPlatformProcess::ReadPipe(ReadErr);
        const bool bTimeout = FPlatformTime::Seconds() - StartedAt > 2700.0;
        const bool bOverflow = Output.Len() + Errors.Len() > 8 * 1024 * 1024;
        if (bTimeout || bOverflow)
        {
            FPlatformProcess::TerminateProc(Process, true);
            Errors += bTimeout ? TEXT("CLI exceeded the 45 minute timeout.") : TEXT("CLI output exceeded 8 MiB.");
            bAborted = true;
        }
        if (FPlatformProcess::IsProcRunning(Process)) return false;
        Output += FPlatformProcess::ReadPipe(ReadOut);
        Errors += FPlatformProcess::ReadPipe(ReadErr);
        Result.ExitCode = -1;
        FPlatformProcess::GetProcReturnCode(Process, &Result.ExitCode);
        if (bAborted) Result.ExitCode = -1;
        Result.Output = MoveTemp(Output);
        Result.Error = MoveTemp(Errors);
        FPlatformProcess::CloseProc(Process);
        Process.Reset();
        ClosePipes();
        bAborted = false;
        return true;
    }

private:
    void ClosePipes()
    {
        FPlatformProcess::ClosePipe(ReadOut, WriteOut);
        FPlatformProcess::ClosePipe(ReadErr, WriteErr);
        ReadOut = WriteOut = ReadErr = WriteErr = nullptr;
    }
    FProcHandle Process;
    void* ReadOut = nullptr;
    void* WriteOut = nullptr;
    void* ReadErr = nullptr;
    void* WriteErr = nullptr;
    FString Output, Errors;
    double StartedAt = 0;
    bool bAborted = false;
};
}

TSharedRef<IPackagingWindowBackend> MakePackagingCliBackend()
{
    return MakeShared<FPackagingCliBackend>();
}
}
