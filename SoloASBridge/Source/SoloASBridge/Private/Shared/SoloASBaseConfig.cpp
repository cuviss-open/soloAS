#include "CoreMinimal.h"

#if WITH_EDITOR
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ConfigContext.h"
#include "Misc/ConfigHierarchy.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"

namespace
{
void LoadBaseConfigLayer(const TCHAR* IniName, const FString& IniFilename,
    const FString& DefaultsFilename, const TCHAR* ClassName)
{
    FConfigFile* Existing = GConfig->FindConfigFile(IniFilename);
    if (!Existing) return;

    // Keep other settings, including changes already made by earlier modules.
    const bool bWasDirty = Existing->Dirty;
    TMap<FString, FConfigSection> PreviousSections;
    for (const auto& Section : AsConst(*Existing)) PreviousSections.Add(Section.Key, Section.Value);

                                                                                                                           
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 4
    const FConfigFileHierarchy PreviousHierarchy = Existing->SourceIniHierarchy;
    Existing->SourceIniHierarchy.Empty();
#else
    FConfigBranch* Branch = GConfig->FindBranch(FName(IniName), IniFilename);
    TArray<FConfigCommandStream> DynamicLayers;
    for (FConfigBranch::DynamicLayerList::TIterator Layer(Branch->DynamicLayers.GetHead()); Layer; ++Layer)
        DynamicLayers.Add(**Layer);
    // AddStaticLayer replaces map entries without changing their iteration
    // position. A new layer order therefore requires an empty hierarchy.
    Branch->Hierarchy.Empty();
#endif
// SOLOAS-COMPAT-END

    // OverrideLayers is public in UE 5.4–5.8. Let UE expand its usual platform,
    // custom, project and user layers and parse all array operators itself.
    FConfigContext Context = FConfigContext::ForceReloadIntoGConfig();
    Context.bWriteDestIni = false;
    Context.bUseHierarchyCache = false;
    for (const FConfigLayer& Layer : GConfigLayers)
    {
        if (FCString::Strcmp(Layer.EditorName, TEXT("ProjectDefault")) == 0)
            Context.OverrideLayers.Add({TEXT("SoloASBase"), *DefaultsFilename, EConfigLayerFlags::NoExpand});
        Context.OverrideLayers.Add(Layer);
    }
    if (!Context.Load(IniName))
    {
        UE_LOG(LogTemp, Error, TEXT("SoloAS could not merge %s defaults from %s"), IniName, *DefaultsFilename);
        return;
    }

    Existing = GConfig->FindConfigFile(IniFilename);
                                                                                                                           
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 4
    // UE 5.4 stores plugin layers in the file's source hierarchy.
    for (const auto& Layer : PreviousHierarchy)
        if (!Existing->SourceIniHierarchy.FindKey(Layer.Value))
            Existing->AddDynamicLayerToHierarchy(Layer.Value);
#else
    // UE 5.5+ keeps raw dynamic commands separately. Preserve string-based
    // hotfixes as well as disk-backed plugin layers without reparsing values.
    Branch = GConfig->FindBranch(FName(IniName), IniFilename);
    // UE 5.8 parses CLI in both hierarchy and Saved loading. Rebuild the raw
    // stream once so additive overrides are not replayed twice.
    Branch->CommandLineOverrides.Empty();
    FConfigFile::OverrideFromCommandline(&Branch->CommandLineOverrides, IniFilename);
    Branch->FinalCombinedLayers = Branch->CombinedStaticLayers;
    Branch->DynamicLayers.Empty();
    for (const FConfigCommandStream& Layer : DynamicLayers)
    {
        FConfigCommandStream* Restored = new FConfigCommandStream(Layer);
        Restored->Branch = Branch;
        Branch->DynamicLayers.AddTail(Restored);
        Branch->FinalCombinedLayers.ApplyFile(Restored);
    }
    Branch->InMemoryFile.Empty();
    for (const auto& Section : AsConst(Branch->FinalCombinedLayers))
        Branch->InMemoryFile.Add(Section.Key, Section.Value);
    Branch->InMemoryFile.ApplyFile(&Branch->SavedLayer);
    Branch->InMemoryFile.ApplyFile(&Branch->CommandLineOverrides);
    Branch->FinalCombinedLayers.ApplyFile(&Branch->CommandLineOverrides);
#endif
// SOLOAS-COMPAT-END

                                                                                                                           
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 4
    // Rebuild from the source baseline before Saved and CLI. The context
    // already applied CLI once; retaining that result would duplicate array
    // additions when CLI is replayed after Saved.
    Existing->Empty();
    for (const auto& Section : AsConst(*Existing->SourceConfigFile))
        Existing->Add(Section.Key, Section.Value);
    FConfigFile Saved;
    Saved.Read(IniFilename);
    for (const auto& Section : AsConst(Saved))
    {
        FConfigSection Merged;
        if (const FConfigSection* Current = Existing->FindSection(Section.Key)) Merged = *Current;
        for (const auto& Setting : Section.Value) Merged.Remove(Setting.Key);
        for (const auto& Setting : Section.Value) Merged.Add(Setting.Key, Setting.Value);
        Existing->Add(Section.Key, MoveTemp(Merged));
    }
    FConfigFile::OverrideFromCommandline(Existing, IniFilename);
#endif
// SOLOAS-COMPAT-END

    // Only the keys declared in this base layer belong to this adapter.
    FConfigFile Defaults;
    Defaults.Combine(DefaultsFilename);
    for (const auto& Section : AsConst(Defaults))
    {
        FConfigSection& Previous = PreviousSections.FindOrAdd(Section.Key);
        for (const auto& Setting : Section.Value) Previous.Remove(Setting.Key);
        if (const FConfigSection* Merged = Existing->FindSection(Section.Key))
            for (const auto& Setting : *Merged)
                if (Section.Value.Contains(Setting.Key)) Previous.Add(Setting.Key, Setting.Value);
    }
    Existing->Empty();
    for (const auto& Section : PreviousSections) Existing->Add(Section.Key, Section.Value);

    Existing->Dirty = bWasDirty;

    // DeveloperSettings CDOs may have been constructed before PreDefault.
    for (TObjectIterator<UClass> Class; Class; ++Class)
        if (Class->GetPathName() == ClassName)
            if (UObject* DefaultsObject = Class->GetDefaultObject(false))
                DefaultsObject->ReloadConfig(*Class);
}
}
#endif

namespace SoloASBridge::Private
{
void InitializeBaseConfigLayers()
{
#if WITH_EDITOR
    const TSharedPtr<IPlugin> Runtime = IPluginManager::Get().FindPlugin(TEXT("Angelscript"));
    if (Runtime.IsValid() && Runtime->IsEnabled())
        LoadBaseConfigLayer(TEXT("Editor"), GEditorIni,
            FPaths::Combine(Runtime->GetBaseDir(), TEXT("Config/BaseEditor.ini")),
            TEXT("/Script/AngelscriptCode.AngelscriptTestSettings"));
#endif
}
}
