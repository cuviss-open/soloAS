// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "ScriptCache/ModKitJitGeneration.h"

#include "Core/AngelscriptModKit.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "Core/ModKitSettings.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

void FModKitJitGeneration::Startup()
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("as-generate-precompiled-data"))) return;
#if WITH_EDITOR
    UE_LOG(LogAngelscriptModKit, Fatal, TEXT("MODKIT_GENERATION_PRODUCER_REQUIRED: generate in a standalone Game producer, never the installed Editor RootDir"));
#endif
    const auto* Settings = UModKitSettings::Get();
    FString LegacyUnit;
    if (FParse::Value(FCommandLine::Get(), TEXT("as-cache-unit="), LegacyUnit)
        || Settings->SourceOnlyScriptPaths.Num() || Settings->JitOnlyModulePrefixes.Num()
        || !Settings->JitOutputDirectory.IsEmpty())
    {
        UE_LOG(LogAngelscriptModKit, Fatal,
            TEXT("MODKIT_GENERATION_STAGE_REQUIRED: prepare native roots using modkit_delivery.py stage; independent cache/JIT selection and core redirect are unsupported"));
    }
    if (Settings->bObfuscateOpcode || Settings->bStripLineNumbersOnCache || Settings->bStripBytecodeForJitModules)
    {
        UE_LOG(LogAngelscriptModKit, Fatal, TEXT("MODKIT_CACHE_POLICY_UNSUPPORTED: opcode/debug/bytecode stripping is not implemented for stock cache serialization"));
    }
    FString Root = FPaths::ConvertRelativePathToFull(FPaths::RootDir());
    FPaths::NormalizeDirectoryName(Root);
    UE_LOG(LogAngelscriptModKit, Display, TEXT("MODKIT_JIT_ROOT root=%s"), *Root);
    // Existing installed-editor generation writes under its engine RootDir.
    // Delivery must use a standalone packaged producer, never the installation.
    if (!FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), Root)
        || !IFileManager::Get().FileExists(*(FPaths::ProjectDir() / TEXT("Script") / TEXT("Binds.Cache"))))
    {
        UE_LOG(LogAngelscriptModKit, Fatal, TEXT("MODKIT_GENERATION_PRODUCER_REQUIRED: use a standalone prepared producer with its binding database"));
    }
    TArray<FString> Existing;
    IFileManager::Get().FindFiles(Existing, *(Root / TEXT("AS_JITTED_CODE") / TEXT("*")), true, true);
    if (Existing.Num())
    {
        UE_LOG(LogAngelscriptModKit, Fatal, TEXT("MODKIT_GENERATION_OUTPUT_NOT_EMPTY: use a new isolated producer output"));
    }
}
