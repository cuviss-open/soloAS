// Copyright Cuviss Studio. All Rights Reserved.
#include "SoloASToolchain.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/EngineVersion.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolMenus.h"
#include "XmlFile.h"

namespace SoloASToolchain
{
bool MigrateLegacySettings(const FString& Filename)
{
    const TCHAR* Section = TEXT("/Script/AngelscriptToolchainEditor.SoloASToolchainSettings");
    FString Preference;
    if (GConfig->GetString(Section, TEXT("Preference"), Preference, Filename)
        || !GConfig->GetString(TEXT("/Script/SoloASCoreExtensionsEditor.SoloASToolchainSettings"),
            TEXT("Preference"), Preference, Filename)) return false;
    GConfig->SetString(Section, TEXT("Preference"), *Preference, Filename);
    GConfig->Flush(false, Filename);
    return true;
}

ESelection Select(ESoloASToolchainPreference Preference, bool bOwn, bool bManaged)
{
    if (Preference == ESoloASToolchainPreference::ForceOwn)
        return bOwn ? ESelection::Own : ESelection::Missing;
    if (Preference == ESoloASToolchainPreference::ForceManaged)
        return bManaged ? ESelection::Managed : ESelection::Missing;
    return bOwn ? ESelection::Own : bManaged ? ESelection::Managed : ESelection::Missing;
}

FString RootForSelection(ESelection InSelection, const FString& InheritedRoot, const FString& ManagedRootPath)
{
    if (InSelection == ESelection::Managed) return ManagedRootPath;
    return !InheritedRoot.IsEmpty() && FPaths::IsSamePath(InheritedRoot, ManagedRootPath) ? FString() : InheritedRoot;
}

FString DetectionKey(const FString& EngineIdentity, const FString& Xml, const FString& Arguments, const FString& SdkRoot)
{
    return FMD5::HashAnsiString(*(EngineIdentity + TEXT("\n") + Xml + TEXT("\n") + Arguments + TEXT("\n") + SdkRoot));
}

FString ManagedRoot()
{
    return FPaths::Combine(FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA")),
        TEXT("SoloAS/Toolchains"), FEngineVersion::Current().ToString(EVersionComponent::Patch));
}

FCandidate ReadQuery(const FString& Json, int32 ExitCode, const FString& RequiredRoot,
    const FString& ExcludedRoot, TFunctionRef<bool(const FString&)> FileExists)
{
    FCandidate Result;
    Result.Error = TEXT("UBT did not validate a complete MSVC and Windows SDK combination.");
    TSharedPtr<FJsonObject> Object;
    if (ExitCode != 0 || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object)
        || !Object.IsValid()) return Result;
    const TSharedPtr<FJsonObject>* Modules = nullptr;
    if (!Object->TryGetObjectField(TEXT("moduleToCompileSettings"), Modules)) return Result;
    for (const auto& Pair : (*Modules)->Values)
    {
        const TSharedPtr<FJsonObject>* Settings = nullptr;
        if (!Pair.Value->TryGetObject(Settings)) continue;
        if (!(*Settings)->TryGetStringField(TEXT("compilerPath"), Result.Compiler)
            || !(*Settings)->TryGetStringField(TEXT("windowsSdkVersion"), Result.SdkVersion)) continue;
        FPaths::NormalizeFilename(Result.Compiler);
        if (!Result.Compiler.EndsWith(TEXT("/bin/Hostx64/x64/cl.exe"), ESearchCase::IgnoreCase)) continue;
        const TArray<TSharedPtr<FJsonValue>>* Includes = nullptr;
        if (!(*Settings)->TryGetArrayField(TEXT("includePaths"), Includes)) continue;
        const FString Suffix = TEXT("/Include/") + Result.SdkVersion + TEXT("/um");
        for (const auto& Include : *Includes)
        {
            FString Path;
            if (!Include->TryGetString(Path)) continue;
            FPaths::NormalizeDirectoryName(Path);
            if (Path.EndsWith(Suffix, ESearchCase::IgnoreCase))
                Result.SdkRoot = Path.LeftChop(Suffix.Len());
        }
        if (!Result.SdkRoot.IsEmpty()) break;
    }
    if (Result.Compiler.IsEmpty() || Result.SdkRoot.IsEmpty()) return Result;
    auto InRoot = [](const FString& Path, const FString& Root)
    {
        return FPaths::IsUnderDirectory(Path, Root);
    };
    if ((!RequiredRoot.IsEmpty() && (!InRoot(Result.Compiler, RequiredRoot) || !InRoot(Result.SdkRoot, RequiredRoot)))
        || (!ExcludedRoot.IsEmpty() && (InRoot(Result.Compiler, ExcludedRoot) || InRoot(Result.SdkRoot, ExcludedRoot))))
    {
        Result.Error = TEXT("UBT selected tools outside the requested toolchain source.");
        return Result;
    }
    Result.bUsable = CheckFiles(Result, FileExists, Result.Error);
    return Result;
}

bool CheckFiles(const FCandidate& Result, TFunctionRef<bool(const FString&)> FileExists, FString& Error)
{
    const FString Bin = FPaths::GetPath(Result.Compiler);
    const FString Msvc = Bin.LeftChop(FCString::Strlen(TEXT("/bin/Hostx64/x64")));
    const TArray<FString> Required = {
        Result.Compiler, Bin / TEXT("link.exe"), Bin / TEXT("lib.exe"), Bin / TEXT("cvtres.exe"),
        Msvc / TEXT("include/vector"), Msvc / TEXT("lib/x64/libcmt.lib"),
        Result.SdkRoot / TEXT("bin") / Result.SdkVersion / TEXT("x64/rc.exe"),
        Result.SdkRoot / TEXT("bin") / Result.SdkVersion / TEXT("x64/mt.exe"),
        Result.SdkRoot / TEXT("Include") / Result.SdkVersion / TEXT("um/windows.h"),
        Result.SdkRoot / TEXT("Include") / Result.SdkVersion / TEXT("ucrt/corecrt.h"),
        Result.SdkRoot / TEXT("Lib") / Result.SdkVersion / TEXT("um/x64/kernel32.lib"),
        Result.SdkRoot / TEXT("Lib") / Result.SdkVersion / TEXT("ucrt/x64/ucrt.lib")
    };
    for (const FString& File : Required)
    {
        if (!FileExists(File))
        {
            Error = TEXT("Toolchain file missing: ") + File;
            return false;
        }
    }
    Error.Reset();
    return true;
}

bool WriteManagedConfig(const FString& Filename, const FCandidate& Candidate, FString& Error)
{
    const FString Empty = TEXT("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n")
        TEXT("<Configuration xmlns=\"https://www.unrealengine.com/BuildConfiguration\"></Configuration>");
    Error.Reset();
    if (!RestoreProjectConfig(Filename, Error)) return false;
    FString Input = Empty;
    if (IFileManager::Get().FileExists(*Filename) && !FFileHelper::LoadFileToString(Input, *Filename))
    {
        Error = TEXT("Cannot read the project's UBT XML; it has been left unchanged.");
        return false;
    }
    // FXmlFile removes the entire XML declaration line. Accept legal minified XML
    // while preserving the original on-disk bytes for restoration.
    Input.TrimStartInline();
    if (Input.StartsWith(TEXT("<?xml"), ESearchCase::IgnoreCase))
    {
        const int32 End = Input.Find(TEXT("?>"));
        if (End != INDEX_NONE) Input.InsertAt(End + 2, TEXT('\n'));
    }
    FXmlFile Xml;
    if (!Xml.LoadFile(Input, EConstructMethod::ConstructFromBuffer)
        || !Xml.GetRootNode() || Xml.GetRootNode()->GetTag() != TEXT("Configuration"))
    {
        Error = TEXT("Cannot read the project's UBT XML; it has been left unchanged.");
        return false;
    }
    TArray<uint8> Original;
    if (IFileManager::Get().FileExists(*Filename))
    {
        if (!FFileHelper::LoadFileToArray(Original, *Filename))
        {
            Error = TEXT("Cannot preserve the project's existing UBT XML.");
            return false;
        }
    }
    else
    {
        const FTCHARToUTF8 Bytes(*Empty);
        Original.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
    }
    auto Set = [&Xml](const FString& Category, const FString& Key, const FString& Value)
    {
        FXmlNode* Section = Xml.GetRootNode()->FindChildNode(Category);
        if (!Section)
        {
            Xml.GetRootNode()->AppendChildNode(Category);
            Section = Xml.GetRootNode()->FindChildNode(Category);
        }
        FXmlNode* Node = Section->FindChildNode(Key);
        if (Node) Node->SetContent(Value);
        else Section->AppendChildNode(Key, Value);
    };
    Set(TEXT("BuildConfiguration"), TEXT("bAllowUBAExecutor"), TEXT("false"));
    Set(TEXT("BuildConfiguration"), TEXT("bAllowUBALocalExecutor"), TEXT("false"));
    Set(TEXT("WindowsPlatform"), TEXT("Compiler"), TEXT("VisualStudio2022"));
    Set(TEXT("WindowsPlatform"), TEXT("CompilerVersion"), Candidate.CompilerVersion);
    Set(TEXT("WindowsPlatform"), TEXT("WindowsSdkVersion"), Candidate.SdkVersion);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    const FString Backup = Filename + TEXT(".SoloAS-original.xml");
    const FString Applied = Filename + TEXT(".SoloAS-applied.xml");
    // Keep ordinary XML copies until restoration, including across an Editor crash.
    if (!FFileHelper::SaveArrayToFile(Original, *Backup) || !Xml.Save(Applied))
    {
        Error = TEXT("Cannot preserve the project's UBT configuration; no selection was applied.");
        return false;
    }
    if (IFileManager::Get().Copy(*Filename, *Applied) != COPY_OK)
    {
        Error = TEXT("Cannot write the project's UBT configuration.");
        return false;
    }
    return true;
}

bool RestoreProjectConfig(const FString& Filename, FString& Error)
{
    const FString Backup = Filename + TEXT(".SoloAS-original.xml");
    const FString Applied = Filename + TEXT(".SoloAS-applied.xml");
    if (!IFileManager::Get().FileExists(*Backup) && !IFileManager::Get().FileExists(*Applied)) return true;
    TArray<uint8> Before, Written, Current;
    if (!FFileHelper::LoadFileToArray(Before, *Backup) || !FFileHelper::LoadFileToArray(Written, *Applied))
    {
        Error = TEXT("SoloAS UBT XML recovery is incomplete. Restore the original XML in Saved/UnrealBuildTool before selecting managed tools.");
        return false;
    }
    if (!FFileHelper::LoadFileToArray(Current, *Filename) || (Current != Written && Current != Before))
    {
        Error = TEXT("Project UBT XML changed during managed selection. User edits and recovery copies were preserved in Saved/UnrealBuildTool; resolve them before restarting with managed tools.");
        return false;
    }
    if (!FFileHelper::SaveArrayToFile(Before, *Filename))
    {
        Error = TEXT("Cannot restore the original project UBT XML.");
        return false;
    }
    IFileManager::Get().Delete(*Applied);
    IFileManager::Get().Delete(*Backup);
    return true;
}

bool WrapPackageMenu(FToolMenuEntry& Entry, TFunction<bool()> PreCheck)
{
    if (Entry.Name != TEXT("PackageSubMenu") || !Entry.SubMenuData.ConstructMenu.NewToolMenu.IsBound()) return false;
    const FNewToolMenuDelegate Original = Entry.SubMenuData.ConstructMenu.NewToolMenu;
    Entry.SubMenuData.ConstructMenu.NewToolMenu = FNewToolMenuDelegate::CreateLambda([Original, PreCheck=MoveTemp(PreCheck)](UToolMenu* Menu)
    {
        if (PreCheck()) Original.ExecuteIfBound(Menu);
    });
    return true;
}

FOnInstallRequested& OnInstallRequested()
{
    static FOnInstallRequested Delegate;
    return Delegate;
}
}
