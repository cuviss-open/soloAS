#include "SoloASProvidersCommandlet.h"

#include "SoloASDeveloperToolsProviders.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"

USoloASProvidersCommandlet::USoloASProvidersCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = false;
}

int32 USoloASProvidersCommandlet::Main(const FString& Params)
{
    FString RequestPath, ResponsePath;
    if (!FParse::Value(*Params, TEXT("RequestFile="), RequestPath)
        || !FParse::Value(*Params, TEXT("ResponseFile="), ResponsePath)
        || FPaths::IsRelative(RequestPath) || FPaths::IsRelative(ResponsePath)
        || FPaths::IsSamePath(RequestPath, ResponsePath))
        return 2;

    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*RequestPath));
    if (!Reader || Reader->TotalSize() < 1 || Reader->TotalSize() > 1024 * 1024)
        return 2;
    TArray<uint8> Bytes;
    Bytes.SetNumUninitialized(static_cast<int32>(Reader->TotalSize()));
    Reader->Serialize(Bytes.GetData(), Bytes.Num());
    if (Reader->IsError())
        return 2;
    const FUTF8ToTCHAR Decoded(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    const FString Request(Decoded.Length(), Decoded.Get());
    const FTCHARToUTF8 Encoded(*Request, Request.Len());
    if (Encoded.Length() != Bytes.Num() || FMemory::Memcmp(Encoded.Get(), Bytes.GetData(), Bytes.Num()) != 0)
        return 2;

    bool bOk = false;
    const FString Response = SoloASDeveloperTools::HandleRequest(Request, bOk);
    if (!FFileHelper::SaveStringToFile(Response, *ResponsePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        return 2;
    return bOk ? 0 : 3;
}
