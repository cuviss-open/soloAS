// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "SoloASToolchainCommandlet.h"

#include "SoloASToolchain.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

USoloASToolchainCommandlet::USoloASToolchainCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = false;
}

int32 USoloASToolchainCommandlet::Main(const FString& Params)
{
    using namespace SoloASToolchain;
    FString ResponsePath;
    if (!FParse::Value(*Params, TEXT("ResponseFile="), ResponsePath)
        || ResponsePath.IsEmpty() || FPaths::IsRelative(ResponsePath)) return 2;

    const bool bOk = RunPrecheck();
    auto CandidateJson = [](const FCandidate& Candidate)
    {
        const TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetBoolField(TEXT("usable"), Candidate.bUsable);
        Value->SetStringField(TEXT("compiler"), Candidate.Compiler);
        Value->SetStringField(TEXT("compilerVersion"), Candidate.CompilerVersion);
        Value->SetStringField(TEXT("sdkRoot"), Candidate.SdkRoot);
        Value->SetStringField(TEXT("sdkVersion"), Candidate.SdkVersion);
        Value->SetStringField(TEXT("error"), Candidate.Error);
        return Value;
    };
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("schemaVersion"), 1);
    const ESoloASToolchainPreference Preference = GetDefault<USoloASToolchainSettings>()->Preference;
    Data->SetStringField(TEXT("platform"), TEXT("Win64"));
    Data->SetStringField(TEXT("preference"), Preference == ESoloASToolchainPreference::ForceOwn ? TEXT("ForceOwn")
        : Preference == ESoloASToolchainPreference::ForceManaged ? TEXT("ForceManaged") : TEXT("Auto"));
    Data->SetStringField(TEXT("selection"), ActiveSelection() == ESelection::Own ? TEXT("own")
        : ActiveSelection() == ESelection::Managed ? TEXT("managed") : TEXT("missing"));
    Data->SetBoolField(TEXT("restartRequiredForSettings"), true);
    Data->SetObjectField(TEXT("own"), CandidateJson(DetectedOwn()));
    Data->SetObjectField(TEXT("managed"), CandidateJson(DetectedManaged()));
    FString Json;
    FJsonSerializer::Serialize(Data, TJsonWriterFactory<>::Create(&Json));
    if (!FFileHelper::SaveStringToFile(Json, *ResponsePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return 2;
    return bOk ? 0 : 3;
}
