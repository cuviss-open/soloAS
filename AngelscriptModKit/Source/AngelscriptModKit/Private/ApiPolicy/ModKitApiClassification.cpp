// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "ApiPolicy/ModKitApiClassification.h"
#include "AngelscriptCodeModule.h"
#include "AngelscriptManager.h"
#include "AngelscriptOptionalProviders.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
using namespace AngelscriptCoreExtensionPoints;
TMap<FString, FApiMetadata> ClassificationEntries;
TSet<FString> Conflicts;
FDelegateHandle ClassificationProvider, InitialCompile;
bool ClassificationInvalid = false;

void LoadTable(const FString& Path, bool Required)
{
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *Path)) { ClassificationInvalid |= Required || FPaths::FileExists(Path); return; }
    TSharedPtr<FJsonObject> Root;
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()
        || !Root->TryGetArrayField(TEXT("entries"), Items)) { ClassificationInvalid = true; return; }
    for (const auto& Item : *Items)
    {
        const TSharedPtr<FJsonObject>* Object = nullptr;
        FString Declaration, Tier, Module;
        const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
        if (!Item->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("declaration"), Declaration)
            || Declaration.IsEmpty() || !(*Object)->TryGetStringField(TEXT("tier"), Tier) || Tier.IsEmpty())
        { ClassificationInvalid = true; continue; }
        (*Object)->TryGetStringField(TEXT("module"), Module);
        FApiMetadata Value {Tier, Module, {}};
        if ((*Object)->TryGetArrayField(TEXT("tags"), Tags))
            for (const auto& Tag : *Tags) { FString S; if (!Tag->TryGetString(S)) ClassificationInvalid = true; else Value.Tags.Add(S); }
        if (const FApiMetadata* Existing = ClassificationEntries.Find(Declaration))
        {
            if (Existing->Tier != Value.Tier || Existing->Tags != Value.Tags) Conflicts.Add(Declaration);
        }
        else ClassificationEntries.Add(Declaration, MoveTemp(Value));
    }
}
void Register()
{
    if (ClassificationProvider.IsValid()) return;
    auto* Engine = FAngelscriptManager::Get().GetScriptEngine();
    ClassificationProvider = RegisterApiMetadataProvider(Engine, [](FStringView Declaration, FApiMetadata& Out) {
        const FString Key(Declaration);
        if (ClassificationInvalid || Conflicts.Contains(Key)) return EApiMetadataResult::Conflict;
        if (const FApiMetadata* Entry = ClassificationEntries.Find(Key)) { Out = *Entry; return EApiMetadataResult::Classified; }
        return EApiMetadataResult::Unknown;
    });
}
}
void FModKitApiClassification::Startup()
{
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("AngelscriptModKit"));
    if (!Plugin.IsValid()) ClassificationInvalid = true;
    else LoadTable(Plugin->GetBaseDir() / TEXT("Config/ApiClassification.json"), true);
    LoadTable(FPaths::ProjectConfigDir() / TEXT("ModKitApiClassification.json"), false);
    if (FAngelscriptManager::IsInitialized()) Register();
    else InitialCompile = FAngelscriptCodeModule::GetOnInitialCompileFinished().AddStatic(&Register);
}
bool FModKitApiClassification::Shutdown()
{
    if (InitialCompile.IsValid()) { FAngelscriptCodeModule::GetOnInitialCompileFinished().Remove(InitialCompile); InitialCompile.Reset(); }
    if (ClassificationProvider.IsValid() && UnregisterApiMetadataProvider(ClassificationProvider) != ERemoveResult::Removed) return false;
    ClassificationProvider.Reset(); ClassificationEntries.Reset(); Conflicts.Reset(); ClassificationInvalid = false; return true;
}
bool FModKitApiClassification::IsRegistered() { return ClassificationProvider.IsValid(); }
