// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "FeatureMods/ModKitCompilationManager.h"
#include "ApiPolicy/ModKitApiAllowlist.h"

#include "AngelscriptCodeModule.h"
#include "AngelscriptManager.h"
#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptReloadSafety.h"
#include "FeatureMods/ModKitConflictCheck.h"
#include "FeatureMods/ModKitModRegistry.h"
#include "Core/ModKitSettings.h"
#include "FeatureMods/ModKitSignature.h"
#include "ScriptCache/ModKitCacheTransform.h"
#include "RuntimeServices/ModKitScriptExecutionTimeout.h"
#if WITH_EDITOR
#include "DeveloperTools/ModKitCommandlets.h"
#endif
#include "Preprocessor/AngelscriptPreprocessor.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProperties.h"
#include "Misc/App.h"
#include "PluginDescriptor.h"
#include "Misc/CommandLine.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformTime.h"
#include "Containers/Ticker.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/LinkerLoad.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

#include "angelscript.h"
#include "as_module.h"
#include "AngelscriptCoreAdmission.h"
#include "AngelscriptCoreExtensionPoints.h"

DEFINE_LOG_CATEGORY(LogAngelscriptModKit);

FDelegateHandle FModKitCompilationManager::InitialCompileFinishedHandle;
FDelegateHandle FModKitCompilationManager::PostEngineInitHandle;
FDelegateHandle FModKitCompilationManager::ReloadHadErrorsHandle;
FTSTicker::FDelegateHandle FModKitCompilationManager::AppendRetryHandle;
double FModKitCompilationManager::AppendRetryDeadlineSeconds = 0.0;
bool FModKitCompilationManager::bIsAppendingModules = false;
bool FModKitCompilationManager::bInitialCompileFinished = false;
bool FModKitCompilationManager::bPostEngineInit = false;
bool FModKitCompilationManager::bDidRunAppend = false;
bool FModKitCompilationManager::bIsRuntimeOpInProgress = false;

namespace
{
	struct FSourceAdmission
	{
		FModKitSourceOwner Owner;
		FString RelativePath;
		bool bDeleted = false;
		int32 PendingUses = 0;
	};
	struct FCandidateSource
	{
		TWeakPtr<FAngelscriptModuleDesc> Module;
		FString ModuleName;
		FModKitSourceOwner Owner;
		TArray<FAngelscriptModuleDesc::FCodeSection> Sections;
		FString Error;
	};
	TMap<FString, FSourceAdmission> SourceAdmissions;
	TArray<FCandidateSource> CandidateSources;
	struct FAdmittedSource
	{
		TWeakPtr<FAngelscriptModuleDesc> Module;
		FString ModuleName;
		FModKitSourceOwner Owner;
	};
	TMap<uint64, FAdmittedSource> AdmittedSources;
	TMap<FString, TArray<AngelscriptCoreExtensionPoints::FModuleGenerationId>> SourceRemovalGenerations;
	bool bRemovalClosing = false;
	FDelegateHandle SourceBindingHandle;
	FDelegateHandle TypeScopeHandle;
	FDelegateHandle ModuleAdmissionHandle;
	FDelegateHandle CacheClient;
	TMap<FString, AngelscriptCoreExtensionPoints::FCacheLoadResult> CacheSets;
	TMap<FString, FModKitSourceOwner> CacheOwners;
	FTSTicker::FDelegateHandle SourceCleanupHandle;

	struct FVerifiedModCacheSet
	{
		FString Format, Build, Configuration;
		FGuid Guid;
		TArray<uint8> Manifest;
		TArray<FString> Paths, Identities;
		TArray<TArray<uint8>> Bytes;
		TArray<AngelscriptCoreExtensionPoints::FVerifiedCacheFileView> Files;

		AngelscriptCoreExtensionPoints::FVerifiedCacheSetView View()
		{
			Files.Reset();
			for (int32 Index = 0; Index < Paths.Num(); ++Index)
				Files.Add({Paths[Index], Identities[Index], Bytes[Index]});
			return {Format, Build, Configuration, Guid, Manifest, Files};
		}
	};
	// Valid only while TryLoadModuleCacheSet synchronously admits this exact snapshot.
	const FVerifiedModCacheSet* PendingCacheSet = nullptr;
	FModKitSourceOwner PendingCacheOwner;
	FString SourcePath(FString Path);

	bool BuildVerifiedCacheSet(const TMap<FString, TArray<uint8>>& VerifiedFiles,
		const FString& CacheDirectory, FVerifiedModCacheSet& Out, FString& Error)
	{
		auto Fail = [&](const TCHAR* Reason) { Error = Reason; return false; };
		const auto* Manifest = VerifiedFiles.Find(SourcePath(CacheDirectory / TEXT("ModuleCaches.json")));
		if (!Manifest || Manifest->IsEmpty()) return Fail(TEXT("cache-manifest-not-in-signed-snapshot"));
		FString Text;
		FFileHelper::BufferToString(Text, Manifest->GetData(), Manifest->Num());
		TSharedPtr<FJsonObject> Root;
		FString Guid;
		const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Layouts = nullptr;
		double Version = 0;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root
			|| !Root->TryGetNumberField(TEXT("schemaVersion"), Version) || Version != 1
			|| !Root->TryGetStringField(TEXT("format"), Out.Format) || Out.Format != TEXT("soloas-module-cache-set-v1")
			|| !Root->TryGetStringField(TEXT("buildIdentity"), Out.Build)
			|| !Root->TryGetStringField(TEXT("configurationIdentity"), Out.Configuration)
			|| !Root->TryGetStringField(TEXT("setGuid"), Guid) || !FGuid::ParseExact(Guid, EGuidFormats::Digits, Out.Guid)
			|| !Out.Guid.IsValid() || !Root->TryGetArrayField(TEXT("modules"), Modules) || !Modules || Modules->IsEmpty()
			|| !Root->TryGetArrayField(TEXT("nativeTypeLayouts"), Layouts) || !Layouts)
			return Fail(TEXT("cache-set-manifest-or-nativeTypeLayouts-missing"));
		Out.Manifest = *Manifest;
		FModKitCacheEnvelopePolicy Policy;
		if (!FModKitCacheTransform::GetTransformPolicy(EModKitCacheDirection::Load, Policy))
			return Fail(TEXT("cache-decryption-policy-unavailable"));
		for (const auto& Value : *Modules)
		{
			const auto Entry = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			FString File, Identity;
			if (!Entry || !Entry->TryGetStringField(TEXT("file"), File)
				|| !Entry->TryGetStringField(TEXT("contentIdentity"), Identity)
				|| File.IsEmpty() || File.StartsWith(TEXT("/"))
				|| File.Contains(TEXT("\\")) || File.Contains(TEXT(":"))
				|| Out.Paths.Contains(File)) return Fail(TEXT("cache-set-file-invalid"));
			TArray<FString> Segments;
			File.ParseIntoArray(Segments, TEXT("/"), false);
			if (Segments.ContainsByPredicate([](const FString& Segment)
				{ return Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT(".."); }))
				return Fail(TEXT("cache-set-file-invalid"));
			const auto* Bytes = VerifiedFiles.Find(SourcePath(CacheDirectory / File));
			if (!Bytes) return Fail(TEXT("cache-member-not-in-signed-snapshot"));
			TArray<uint8> Plaintext = *Bytes;
			if (!FModKitCacheTransform::TransformCacheBytes(EModKitCacheDirection::Load, Policy, Plaintext))
				return Fail(TEXT("cache-member-decryption-failed"));
			Out.Paths.Add(MoveTemp(File)); Out.Identities.Add(MoveTemp(Identity)); Out.Bytes.Add(MoveTemp(Plaintext));
		}
		return true;
	}

	FString SourcePath(FString Path)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
#if PLATFORM_WINDOWS
		Path.ToLowerInline();
#endif
		return Path;
	}

	bool CurrentOwner(const FModKitSourceOwner& Owner)
	{
		return Owner.Layer != EModKitSourceLayer::Leaf || FModKitModRegistry::IsSourceOwnerCurrent(Owner);
	}

	bool SameSections(const TArray<FAngelscriptModuleDesc::FCodeSection>& A,
		const TArray<FAngelscriptModuleDesc::FCodeSection>& B)
	{
		if (A.Num() != B.Num()) return false;
		for (int32 Index = 0; Index < A.Num(); ++Index)
			if (A[Index].AbsoluteFilename != B[Index].AbsoluteFilename
				|| A[Index].RelativeFilename != B[Index].RelativeFilename || A[Index].Code != B[Index].Code) return false;
		return true;
	}

	void BindPreprocessedSources(FAngelscriptPreprocessor& Processor)
	{
		CandidateSources.RemoveAll([](const FCandidateSource& Entry) { return !Entry.Module.IsValid(); });
		for (auto It = AdmittedSources.CreateIterator(); It; ++It)
			if (!It.Value().Module.IsValid()) It.RemoveCurrent();
		for (const auto& File : Processor.Files)
		{
			if (!File.Module.IsValid() || !FModKitModRegistry::IsModSourcePath(File.AbsoluteFilename)) continue;
			FCandidateSource* Bound = CandidateSources.FindByPredicate([&](const FCandidateSource& Entry)
				{ return Entry.Module.Pin() == File.Module; });
			if (!Bound)
			{
				Bound = &CandidateSources.AddDefaulted_GetRef();
				Bound->Module = File.Module;
				Bound->ModuleName = File.Module->ModuleName;
			}
			const FSourceAdmission* Admission = SourceAdmissions.Find(SourcePath(File.AbsoluteFilename));
			if (!Admission || Admission->RelativePath != File.RelativeFilename
				|| Admission->bDeleted != File.bTreatAsDeleted || !CurrentOwner(Admission->Owner))
				Bound->Error = TEXT("MODKIT_SOURCE_OWNER missing or expired loaded-source admission");
			else if (!Bound->Owner.Id.IsEmpty()
				&& (Bound->Owner.Id != Admission->Owner.Id || Bound->Owner.Revision != Admission->Owner.Revision
					|| Bound->Owner.ScriptRoot != Admission->Owner.ScriptRoot))
				Bound->Error = TEXT("MODKIT_SOURCE_OWNER mixed candidate owners");
			else Bound->Owner = Admission->Owner;
			Bound->Sections.Add({File.RelativeFilename, File.AbsoluteFilename, File.ProcessedCode});
			if (FSourceAdmission* Pending = SourceAdmissions.Find(SourcePath(File.AbsoluteFilename)))
				if (--Pending->PendingUses == 0) SourceAdmissions.Remove(SourcePath(File.AbsoluteFilename));
		}
	}
}

bool FModKitCompilationManager::IsSourceAdmissionPending()
{
	return !SourceAdmissions.IsEmpty();
}

FString FModKitCompilationManager::WrapLeafSource(const FString& AbsolutePath, const FString& Source, bool bWarn)
{
	FModKitSourceOwner Owner;
	FString Error;
	if (!FModKitModRegistry::ResolveSourceOwner(SourcePath(AbsolutePath), Owner, Error)
		|| Owner.Layer != EModKitSourceLayer::Leaf) return Source;
	const FTCHARToUTF8 Id(*Owner.Id);
	asUINT TokenLength = 0;
	const auto* Engine = FAngelscriptManager::Get().GetScriptEngine();
	if (!Engine || Engine->ParseToken(Id.Get(), Id.Length(), &TokenLength) != asTC_IDENTIFIER
		|| TokenLength != static_cast<asUINT>(Id.Length()))
	{
		if (bWarn) UE_LOG(LogAngelscriptModKit, Warning,
			TEXT("MODKIT_NAMESPACE_SKIPPED name=%s reason=invalid-script-identifier; leaf types will not use the Mod ID namespace"), *Owner.Id);
		return Source;
	}
	return TEXT("namespace ") + Owner.Id + TEXT(" {\n") + Source + TEXT("\n}\n");
}

bool FModKitCompilationManager::IsLeafPatchPath(const FString& RelativePath)
{
	return RelativePath.StartsWith(TEXT("FeatureMods/"), ESearchCase::IgnoreCase)
		|| RelativePath.StartsWith(TEXT("Plugins/FeatureMods/"), ESearchCase::IgnoreCase);
}

bool FModKitCompilationManager::AdmitSource(FStringView AbsolutePath, FStringView RelativePath,
	bool bDeleted, FString& Error)
{
	check(IsInGameThread());
	const FString Path = SourcePath(FString(AbsolutePath));
	FSourceAdmission Admission;
	Admission.RelativePath = FString(RelativePath);
	Admission.bDeleted = bDeleted;
	if (!FModKitModRegistry::IsModSourcePath(Path))
	{
		FString Relative(RelativePath);
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Relative.StartsWith(TEXT("FeatureMods/")) || Relative.StartsWith(TEXT("Plugins/FeatureMods/")))
		{ Error = TEXT("MODKIT_SOURCE_OWNER source escaped its declared Mod root"); return false; }
		return true; // The core owns every non-Mod source, including native temporary inputs.
	}
	if (!FModKitModRegistry::ResolveSourceOwner(Path, Admission.Owner, Error))
	{
		if (Error.IsEmpty()) Error = TEXT("MODKIT_SOURCE_OWNER unknown FeatureMod root");
		return false;
	}
#if !WITH_EDITOR
	if (!bDeleted && Admission.Owner.Trust == EModKitModTrust::BuiltIn
		&& FPlatformFileManager::Get().GetPlatformPhysical().FileExists(*Path))
	{
		Error = TEXT("MODKIT_SOURCE_OWNER loose source cannot acquire packaged BuiltIn trust");
		return false;
	}
#endif
	Admission.PendingUses = 1;
	if (const FSourceAdmission* Pending = SourceAdmissions.Find(Path))
	{
		if (Pending->Owner.Revision != Admission.Owner.Revision || Pending->Owner.Id != Admission.Owner.Id)
		{ Error = TEXT("MODKIT_SOURCE_OWNER source revision changed during preprocessing"); return false; }
		Admission.PendingUses += Pending->PendingUses;
	}
	SourceAdmissions.Add(Path, MoveTemp(Admission));
	return true;
}

bool FModKitCompilationManager::GetSourceOwner(const TSharedRef<FAngelscriptModuleDesc>& Module,
	FModKitSourceOwner& Owner, FString& Error)
{
	if (Module->CoreCacheSetGuid.IsValid()) return GetGenerationOwner(*Module, Owner, Error);
	FCandidateSource* Bound = CandidateSources.FindByPredicate([&](const FCandidateSource& Entry)
		{ return Entry.Module.Pin() == Module; });
	if (!Bound && Module->ScriptModule)
	{
		auto* ScriptModule = static_cast<asCModule*>(Module->ScriptModule);
		const auto Old = ScriptModule->ReloadOldModule
			? FAngelscriptManager::Get().GetModule(ScriptModule->ReloadOldModule) : nullptr;
		if (Old.IsValid() && Old.Get() != &Module.Get() && Old->ModuleName == Module->ModuleName)
		{
			const FCandidateSource* Previous = CandidateSources.FindByPredicate([&](const FCandidateSource& Entry)
				{ return Entry.Module.Pin() == Old; });
			if (Previous && Previous->Error.IsEmpty() && CurrentOwner(Previous->Owner)
				&& SameSections(Previous->Sections, Module->Code))
			{
				FCandidateSource Copy = *Previous;
				Copy.Module = Module;
				Bound = &CandidateSources.Add_GetRef(MoveTemp(Copy));
			}
		}
	}
	if (!Bound || Bound->ModuleName != Module->ModuleName || !Bound->Error.IsEmpty() || !SameSections(Bound->Sections, Module->Code))
	{
		Error = Bound && !Bound->Error.IsEmpty() ? Bound->Error : TEXT("MODKIT_SOURCE_OWNER unbound candidate or changed sections");
		return false;
	}
	Owner = Bound->Owner;
	return true;
}

bool FModKitCompilationManager::CanReferenceOwner(const FModKitSourceOwner& Source, const FModKitSourceOwner& Target)
{
	if (Source.Id.IsEmpty() || Target.Id.IsEmpty()) return false;
	return Target.Layer != EModKitSourceLayer::Leaf
		|| (Source.Layer == EModKitSourceLayer::Leaf
			&& (Source.Id == Target.Id || Source.LeafDependencies.Contains(Target.Id)));
}

bool FModKitCompilationManager::GetModuleOwner(const TSharedRef<FAngelscriptModuleDesc>& Module,
	FModKitSourceOwner& Owner, FString& Error)
{
	if (Module->CoreGenerationId && Module->ScriptModule) return GetGenerationOwner(*Module, Owner, Error);
	if (!GetSourceOwner(Module, Owner, Error)) return false;
	if (CurrentOwner(Owner)) return true;
	Error = TEXT("MODKIT_SOURCE_OWNER expired owner");
	return false;
}

bool FModKitCompilationManager::RequiresModAdmission(const FAngelscriptModuleDesc& Module)
{
	if (PendingCacheSet && Module.CoreCacheSetGuid == PendingCacheSet->Guid) return true;
	for (const auto& Entry : CacheSets)
		for (const auto& Generation : Entry.Value.CommittedModules)
			if (Module.CoreGenerationId && Module.CoreGenerationId == Generation.Generation
				&& Module.ModuleName == Generation.ModuleName) return true;
	for (const auto& Section : Module.Code)
		if (FModKitModRegistry::IsModSourcePath(Section.AbsoluteFilename)) return true;
	return false;
}

bool FModKitCompilationManager::GetGenerationOwner(const FAngelscriptModuleDesc& Module,
	FModKitSourceOwner& Owner, FString& Error)
{
	check(IsInGameThread());
	if (Module.CoreCacheSetGuid.IsValid())
	{
		for (const auto& Entry : CacheSets)
			for (const auto& Generation : Entry.Value.CommittedModules)
				if (Generation.Generation == Module.CoreGenerationId && Generation.ModuleName == Module.ModuleName)
				{
					const auto* Bound = CacheOwners.Find(Entry.Key);
					if (!Bound) { Error = TEXT("cache-generation-owner-missing"); return false; }
					Owner = *Bound;
					return true;
				}
		if (!PendingCacheSet || Module.CoreCacheSetGuid != PendingCacheSet->Guid
			|| !CurrentOwner(PendingCacheOwner)) { Error = TEXT("cache-generation-owner-missing"); return false; }
		for (int32 Index = 0; Index < PendingCacheSet->Paths.Num(); ++Index)
			if (Module.CoreCacheRelativePath == PendingCacheSet->Paths[Index]
				&& Module.CoreCacheFileIdentity == PendingCacheSet->Identities[Index])
			{ Owner = PendingCacheOwner; return true; }
		Error = TEXT("cache-generation-identity-mismatch");
		return false;
	}
	// Teardown consumes the immutable admission fact, including after Failed.
	if (const auto* Admitted = AdmittedSources.Find(Module.CoreGenerationId))
	{
		if (Admitted->Module.Pin().Get() != &Module || Admitted->ModuleName != Module.ModuleName)
		{ Error = TEXT("source-generation-identity-mismatch"); return false; }
		Owner = Admitted->Owner;
		return true;
	}
	const auto* Bound = CandidateSources.FindByPredicate([&](const FCandidateSource& Entry)
		{ return Entry.Module.Pin().Get() == &Module; });
	if (!Bound || !Bound->Error.IsEmpty() || Bound->ModuleName != Module.ModuleName
		|| !SameSections(Bound->Sections, Module.Code) || !CurrentOwner(Bound->Owner))
	{ Error = TEXT("source-generation-owner-missing"); return false; }
	if (Module.CoreGenerationId)
		AdmittedSources.Add(Module.CoreGenerationId, {Bound->Module, Bound->ModuleName, Bound->Owner});
	Owner = Bound->Owner;
	return true;
}

bool FModKitCompilationManager::OwnsModule(const TSharedRef<FAngelscriptModuleDesc>& Module, const FString& Id)
{
	FModKitSourceOwner Owner;
	FString Error;
	return GetModuleOwner(Module, Owner, Error) && Owner.Layer == EModKitSourceLayer::Leaf && Owner.Id == Id;
}

bool FModKitCompilationManager::ValidateModuleAdmission(TConstArrayView<TSharedRef<FAngelscriptModuleDesc>> Modules,
	FString& Error)
{
	TArray<TSharedRef<FAngelscriptModuleDesc>> Candidates(Modules.GetData(), Modules.Num());
	FModKitApiAllowlist::FBlueprintAuditScope Scope(Candidates);
	for (const auto& Module : Candidates)
	{
		if (!RequiresModAdmission(*Module)) continue;
		FModKitSourceOwner Owner;
		if (!GetSourceOwner(Module, Owner, Error)) return false;
		if (!CurrentOwner(Owner)) { Error = TEXT("MODKIT_SOURCE_OWNER expired candidate revision"); return false; }
		if (!Module->ScriptModule) { Error = TEXT("MODKIT_SOURCE_OWNER missing compiled candidate"); return false; }
		for (const auto& Dependency : static_cast<asCModule*>(Module->ScriptModule)->moduleDependencies)
		{
			const auto Target = FAngelscriptManager::Get().GetModule(Dependency.Key);
			TSharedPtr<FAngelscriptModuleDesc> Actual = Target;
			for (const auto& Candidate : Candidates)
				if (Candidate->ScriptModule == Dependency.Key) Actual = Candidate;
			FModKitSourceOwner TargetOwner;
			if (!Actual.IsValid() || !RequiresModAdmission(*Actual)) continue;
			if (!GetSourceOwner(Actual.ToSharedRef(), TargetOwner, Error)) return false;
			if (!CanReferenceOwner(Owner, TargetOwner))
			{ Error = TEXT("MODKIT-LEAF-DIRECTION: undeclared leaf dependency"); return false; }
		}
		FModKitApiAllowlist::FModApiUsage Usage;
		Usage.Module = &Module.Get();
		if (FModKitApiAllowlist::CheckApiAllowlist(Module->ModuleName, Usage, Error)) return false;
	}
	return true;
}

namespace
{
	TSharedPtr<FAngelscriptPreprocessor> PendingAppendPreprocessor;


	const TCHAR* LexECompileResult(ECompileResult Result)
	{
		switch (Result)
		{
		case ECompileResult::FullyHandled: return TEXT("FullyHandled");
		case ECompileResult::Deferred: return TEXT("Deferred");
		case ECompileResult::PartiallyHandled: return TEXT("PartiallyHandled");
		case ECompileResult::Error: return TEXT("Error");
		case ECompileResult::NativeReloadFailed: return TEXT("NativeReloadFailed");
		case ECompileResult::ErrorNeedFullReload: return TEXT("ErrorNeedFullReload");
		default: return TEXT("Unknown");
		}
	}

	void EmitErrorSummary()
	{
		FString Diagnostics = FAngelscriptManager::Get().FormatDiagnostics();
		if (Diagnostics.IsEmpty())
		{
			Diagnostics = TEXT("none");
		}

                                                                       
		Diagnostics = Diagnostics
			.Replace(TEXT("\r\n"), TEXT(" "))
			.Replace(TEXT("\n"), TEXT(" "))
			.Replace(TEXT("\r"), TEXT(" "));

		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_ERROR_SUMMARY %s"), *Diagnostics);
	}

	FString NormalizeSourceOnlyPrefix(FString Prefix)
	{
		Prefix.TrimStartAndEndInline();
		Prefix.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Prefix.StartsWith(TEXT("/")))
		{
			Prefix.RightChopInline(1);
		}
		while (Prefix.EndsWith(TEXT("/")))
		{
			Prefix.LeftChopInline(1);
		}
		return Prefix;
	}

	void FindScriptFilesRecursive(
		IFileManager& FileManager,
		const FString& RelativeRoot,
		const FString& SearchDirectory,
		TArray<FAngelscriptManager::FFilenamePair>& OutFilenames)
	{
		TArray<FString> Files;
		FileManager.FindFiles(Files, *(SearchDirectory / TEXT("*.as")), true, false);
		for (const FString& File : Files)
		{
			OutFilenames.Add(FAngelscriptManager::FFilenamePair{ SearchDirectory / File, RelativeRoot / File });
		}

		TArray<FString> Directories;
		FileManager.FindFiles(Directories, *(SearchDirectory / TEXT("*")), false, true);
		for (const FString& Directory : TSet<FString>(Directories))
		{
			FindScriptFilesRecursive(FileManager, RelativeRoot / Directory, SearchDirectory / Directory, OutFilenames);
		}
	}

	void ScanModScriptFiles(TArray<FAngelscriptManager::FFilenamePair>& OutFilenames)
	{
		IFileManager& FileManager = IFileManager::Get();
		struct FRoot
		{
			FString RelativePath;
			FString AbsolutePath;
		};
		const FRoot Roots[] = {
			{ TEXT("FeatureMods"), FPaths::ProjectDir() / TEXT("FeatureMods") },
			{ TEXT("Plugins/FeatureMods"), FPaths::ProjectPluginsDir() / TEXT("FeatureMods") }
		};

		for (const FRoot& Root : Roots)
		{
			if (!FileManager.DirectoryExists(*Root.AbsolutePath))
			{
				continue;
			}

			TArray<FString> ModDirectories;
			FileManager.FindFiles(ModDirectories, *(Root.AbsolutePath / TEXT("*")), false, true);
			ModDirectories.Sort();
			for (const FString& ModDirectory : ModDirectories)
			{
				const FString ScriptDirectory = Root.AbsolutePath / ModDirectory / TEXT("Script");
				if (!FileManager.DirectoryExists(*ScriptDirectory))
				{
					continue;
				}
				FindScriptFilesRecursive(FileManager,
					Root.RelativePath / ModDirectory / TEXT("Script"), ScriptDirectory, OutFilenames);
			}
		}
	}

	/**
 * Scans only the named FeatureMod directories. Runtime load/unload
 * builds its compile set from exactly these files, so a
 * runtime-loaded mod never picks up siblings.
 */
	void CollectModScriptFiles(const TArray<FString>& ModNames, TArray<FAngelscriptManager::FFilenamePair>& OutFilenames)
	{
		IFileManager& FileManager = IFileManager::Get();

		for (const FString& ModName : ModNames)
		{
			FString ScriptDirectory;
			FString RelativeRoot;
			if (!FModKitModRegistry::GetModScriptPaths(ModName, ScriptDirectory, RelativeRoot))
			{
				continue;
			}
			if (!FileManager.DirectoryExists(*ScriptDirectory))
			{
				continue;
			}

			FindScriptFilesRecursive(FileManager, RelativeRoot, ScriptDirectory, OutFilenames);
		}
	}

	bool GetModCacheLocation(const FString& ModName, FString& OutCacheDirectory, FString& OutModulePrefix)
	{
		FString ScriptDirectory;
		FString RelativeRoot;
		if (!FModKitModRegistry::GetModScriptPaths(ModName, ScriptDirectory, RelativeRoot))
		{
			return false;
		}

		OutCacheDirectory = ScriptDirectory / TEXT("ModuleCaches");
		OutModulePrefix = ModName;
		return true;
	}

	/**
 * Roster filter for the append compile set : FeatureMod scripts
 * are kept only when the owning plugin ID
 * is on the roster (activated FeatureMod). Source-only
 * prefixes are never mod scripts and always compile.
 */
	bool IsRosterModScript(const FString& RelativePath)
	{
		FString Rest;
		if (RelativePath.StartsWith(TEXT("FeatureMods/")))
		{
			Rest = RelativePath.RightChop(12);
		}
		else if (RelativePath.StartsWith(TEXT("Plugins/FeatureMods/")))
		{
			Rest = RelativePath.RightChop(20);
		}
		else
		{
			return true;
		}
		int32 SlashIndex;
		if (!Rest.FindChar(TEXT('/'), SlashIndex))
		{
			return true;
		}
		return FModKitModRegistry::ShouldCompileMod(Rest.Left(SlashIndex));
	}

	/**
	 * Scans the configured source-only prefixes below the project Script
	 * root. Misconfiguration fails closed with an explicit error per entry:
	 * an empty prefix, a missing directory, or a directory without script
	 * sources each emit MODKIT_CONFIG_ERROR and are skipped.
	 */
	void ScanSourceOnlyScriptFiles(TArray<FAngelscriptManager::FFilenamePair>& OutFilenames)
	{
		IFileManager& FileManager = IFileManager::Get();
		const FString ScriptRoot = FPaths::ProjectDir() / TEXT("Script");

		TSet<FString> Existing;
		for (const FAngelscriptManager::FFilenamePair& Pair : OutFilenames)
		{
			Existing.Add(Pair.AbsolutePath);
		}

		for (const FString& Entry : UModKitSettings::Get()->SourceOnlyScriptPaths)
		{
			const FString Prefix = NormalizeSourceOnlyPrefix(Entry);
			if (Prefix.IsEmpty())
			{
				UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR entry=%s reason=empty-prefix"), *Entry);
				continue;
			}

			// FeatureMod trees are covered by the built-in discovery scan.
			if (Prefix == TEXT("FeatureMods") || Prefix.StartsWith(TEXT("FeatureMods/"))
				|| Prefix == TEXT("Plugins/FeatureMods") || Prefix.StartsWith(TEXT("Plugins/FeatureMods/")))
			{
				continue;
			}

			const FString SearchDirectory = ScriptRoot / Prefix;
			if (!FileManager.DirectoryExists(*SearchDirectory))
			{
				UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR prefix=%s reason=missing-directory"), *Prefix);
				continue;
			}

			const int32 Before = OutFilenames.Num();
			FindScriptFilesRecursive(FileManager, Prefix, SearchDirectory, OutFilenames);
			for (int32 Index = OutFilenames.Num() - 1; Index >= Before; --Index)
			{
				if (Existing.Contains(OutFilenames[Index].AbsolutePath))
				{
					OutFilenames.RemoveAt(Index);
				}
				else
				{
					Existing.Add(OutFilenames[Index].AbsolutePath);
				}
			}

			if (OutFilenames.Num() == Before)
			{
				UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CONFIG_ERROR prefix=%s reason=no-sources"), *Prefix);
			}
		}
	}

	/**
 
 * duplicate predicate (an empty placeholder module is treated as absent,
 * v2 §2.5), and compiles with FullReload. When bRunConflictCheck is set,
 * a module/class collision is reported before any compile work.
 */
	struct FAppendOutcome
	{
		ECompileResult Result = ECompileResult::Error;
		bool bNothingToCompile = false;
		bool bDeferred = false;
		bool bConflict = false;
		FString ConflictReason;
		TArray<FString> CompiledModuleNames;
	};

	FAppendOutcome RunAppendBatch(
		const TArray<FAngelscriptManager::FFilenamePair>& Filenames,
		bool bRunConflictCheck,
		bool bIgnoreAlreadyActiveModules = false,
		TSharedPtr<FAngelscriptPreprocessor>* PendingPreprocessor = nullptr,
		const TArray<FString>& ReplaceOwners = {})
	{
		FAppendOutcome Outcome;
		// CompileOwnedSourceModules acquires the retained compile surface for
		// independent additions. Cache ownership and replacement are checked
		// before compilation; an unrelated loaded cache is not a conflict.

		TSharedPtr<FAngelscriptPreprocessor> Prepared = PendingPreprocessor ? *PendingPreprocessor : nullptr;
		if (!Prepared.IsValid())
		{
			Prepared = MakeShared<FAngelscriptPreprocessor>();
			for (const FAngelscriptManager::FFilenamePair& Filename : Filenames)
			{
				const bool bDeleted = !ReplaceOwners.IsEmpty() && !IFileManager::Get().FileExists(*Filename.AbsolutePath);
				Prepared->AddFile(Filename.RelativePath, Filename.AbsolutePath, false, bDeleted);
				if (const FString* Verified = FModKitSignature::GetVerifiedSource(Filename.AbsolutePath))
					Prepared->Files.Last().RawCode = *Verified;
				if (!bDeleted)
					Prepared->Files.Last().RawCode = FModKitCompilationManager::WrapLeafSource(
						Filename.AbsolutePath, Prepared->Files.Last().RawCode, true);
			}
			const auto AdmissionsBeforePreprocess = SourceAdmissions;
			if (!Prepared->Preprocess())
			{
				SourceAdmissions = AdmissionsBeforePreprocess;
				UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_FAILED result=PreprocessError"));
				EmitErrorSummary();
				return Outcome;
			}
		}

		// Blueprint imports can start package loading during preprocessing. Keep
		// that exact prepared batch until admission, without scheduling Runtime's
		// failed-file retry or repeating import side effects on later ticks.
		if (PendingPreprocessor && AngelscriptReloadSafety::ShouldDeferForPackageLoading())
		{
			*PendingPreprocessor = Prepared;
			Outcome.bDeferred = true;
			return Outcome;
		}

		TArray<TSharedRef<FAngelscriptModuleDesc>> Modules = Prepared->GetModulesToCompile();

		// Startup append includes configured source-only modules, which may
		// already be active because the editor compiled them before cache
		// generation. Those modules are skipped below; exclude them from the
		// collision set so they do not reject unrelated FeatureMod modules.
		TArray<TSharedRef<FAngelscriptModuleDesc>> ModulesToCheck;
		if (bIgnoreAlreadyActiveModules)
		{
			for (const TSharedRef<FAngelscriptModuleDesc>& Module : Modules)
			{
				TSharedPtr<FAngelscriptModuleDesc> Active = FAngelscriptManager::Get().GetModule(Module->ModuleName);
				if (!Active.IsValid() || FModKitCompilationManager::IsEmptyPlaceholderModule(Module->ModuleName))
				{
					ModulesToCheck.Add(Module);
				}
			}
		}
		else
		{
			ModulesToCheck = Modules;
		}

		// UX-D4: a runtime load colliding with active modules or resolvable
		// script classes is rejected before any compile work.
		auto IsOwned = [&](const TSharedRef<FAngelscriptModuleDesc>& Module)
		{
			return ReplaceOwners.ContainsByPredicate([&](const FString& Owner)
			{
				return FModKitCompilationManager::OwnsModule(Module, Owner);
			});
		};
		if (!ReplaceOwners.IsEmpty())
		{
			for (const auto& Module : Modules)
			{
				const auto Active = FAngelscriptManager::Get().GetModule(Module->ModuleName);
				if (!IsOwned(Module) || (Active.IsValid() && !IsOwned(Active.ToSharedRef())))
					Outcome.ConflictReason = TEXT("reload-owner-conflict ") + Module->ModuleName;
				for (const auto& Class : Module->Classes)
				{
					if (Class->bIsStaticsClass) continue;
					TSharedPtr<FAngelscriptModuleDesc> Owner;
					if (FAngelscriptManager::Get().GetClass(Class->GetTypeIdentity(), &Owner).IsValid()
						&& (!Owner.IsValid() || !IsOwned(Owner.ToSharedRef())))
						Outcome.ConflictReason = TEXT("reload-class-conflict ") + Class->ClassName;
				}
			}
			if (!Outcome.ConflictReason.IsEmpty())
			{
				Outcome.bConflict = true;
				return Outcome;
			}
		}
		if (ReplaceOwners.IsEmpty() && bRunConflictCheck && FModKitConflictCheck::CheckModuleSet(ModulesToCheck, Outcome.ConflictReason))
		{
			Outcome.bConflict = true;
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_FAILED reason=%s"), *Outcome.ConflictReason);
			return Outcome;
		}

		                                                                      
		// one. An empty placeholder module (a previously unloaded mod) is
		// treated as absent and allowed to recompile (v2 §2.5).
		TArray<TSharedRef<FAngelscriptModuleDesc>> ModulesToCompile;
		for (const TSharedRef<FAngelscriptModuleDesc>& Module : Modules)
		{
			TSharedPtr<FAngelscriptModuleDesc> Active = FAngelscriptManager::Get().GetModule(Module->ModuleName);
			if (ReplaceOwners.IsEmpty() && Active.IsValid() && !FModKitCompilationManager::IsEmptyPlaceholderModule(Module->ModuleName))
			{
				UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_APPEND_SKIP name=%s reason=duplicate-active-module"), *Module->ModuleName);
				continue;
			}

			ModulesToCompile.Add(Module);
		}

		if (ModulesToCompile.Num() == 0)
		{
			Outcome.bNothingToCompile = true;
			Outcome.Result = ECompileResult::FullyHandled;
			return Outcome;
		}

		// Startup append is safe with full reload (no surviving game instances).
		                                                                   
		const TSet<FString> PreAppendModuleNames = [&]()
		{
			// Empty placeholders (v2 §2.5) stay in ActiveModules after an
			// unload and are legitimately recompiled on re-load; the
			// cache-module-recompiled invariant below targets only
			// cache-instantiated modules, so placeholders are excluded here.
			TSet<FString> Names;
			for (const TSharedRef<FAngelscriptModuleDesc>& ActiveModule : FAngelscriptManager::Get().GetActiveModules())
			{
				if (!FModKitCompilationManager::IsEmptyPlaceholderModule(ActiveModule->ModuleName)
					&& (ReplaceOwners.IsEmpty() || !IsOwned(ActiveModule)))
				{
					Names.Add(ActiveModule->ModuleName);
				}
			}
			return Names;
		}();

		TArray<TSharedRef<FAngelscriptModuleDesc>> CompiledModules;
		Outcome.Result = FModKitCompilationManager::CompileOwnedSourceModules(
			ECompileType::FullReload, ModulesToCompile, CompiledModules);
		if (Outcome.Result == ECompileResult::Deferred)
		{
			if (PendingPreprocessor) *PendingPreprocessor = Prepared;
			Outcome.bDeferred = true;
			return Outcome;
		}

		// Invariant (advisor R4): cache-instantiated modules carry no dependency
		// edges and must never be selected for recompilation by an append. A
		// violation would recompile them from empty descriptors and silently drop
		// their classes.
		bool bCacheModuleRecompiled = false;
		for (const TSharedRef<FAngelscriptModuleDesc>& Module : CompiledModules)
		{
			if (PreAppendModuleNames.Contains(Module->ModuleName))
			{
				bCacheModuleRecompiled = true;
				UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_INVARIANT_VIOLATION cache-module-recompiled name=%s"), *Module->ModuleName);
			}
		}
		if (bCacheModuleRecompiled)
		{
			Outcome.Result = ECompileResult::Error;
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_FAILED reason=cache-module-recompiled"));
			return Outcome;
		}

		UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_APPEND_RESULT result=%s modules=%d"), LexECompileResult(Outcome.Result), CompiledModules.Num());

		if (Outcome.Result == ECompileResult::FullyHandled)
		{
			for (const TSharedRef<FAngelscriptModuleDesc>& Module : CompiledModules)
			{
				UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_APPEND_MODULE name=%s"), *Module->ModuleName);
				Outcome.CompiledModuleNames.Add(Module->ModuleName);
			}
		}
		else
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_FAILED result=%s"), LexECompileResult(Outcome.Result));
		}
		return Outcome;
	}
}

TArray<FString> FModKitCompilationManager::GetSourceOnlyPrefixes()
{
	TArray<FString> Prefixes;
	Prefixes.Add(TEXT("FeatureMods"));
	Prefixes.Add(TEXT("Plugins/FeatureMods"));
	for (const FString& Entry : UModKitSettings::Get()->SourceOnlyScriptPaths)
	{
		FString Prefix = NormalizeSourceOnlyPrefix(Entry);
		if (!Prefix.IsEmpty() && !Prefixes.Contains(Prefix))
		{
			Prefixes.Add(MoveTemp(Prefix));
		}
	}
	return Prefixes;
}

bool FModKitCompilationManager::IsSourceOnlyModule(const FString& ModuleName, const TArray<FString>& RelativeFilenames)
{
	if (ModuleName == TEXT("Mods") || ModuleName.StartsWith(TEXT("Mods.")))
	{
		return true;
	}
	for (const FString& Prefix : GetSourceOnlyPrefixes())
	{
		for (const FString& Filename : RelativeFilenames)
		{
			if (Filename == Prefix || Filename.StartsWith(Prefix + TEXT("/")))
			{
				return true;
			}
		}
	}
	return false;
}

bool FModKitCompilationManager::IsLeafModuleName(const FString& ModuleName, FString* OutRoot)
{
	const auto Active = FAngelscriptManager::Get().GetModule(ModuleName);
	FModKitSourceOwner Owner;
	FString Error;
	if (!Active.IsValid() || !GetSourceOwner(Active.ToSharedRef(), Owner, Error)
		|| Owner.Layer != EModKitSourceLayer::Leaf) return false;
	if (OutRoot) *OutRoot = TEXT("FeatureMods.") + Owner.Id;
	return true;
}

void FModKitCompilationManager::GetLeafModuleNames(TArray<FString>& OutModuleNames)
{
	TArray<FAngelscriptManager::FFilenamePair> Filenames;
	ScanModScriptFiles(Filenames);
	ScanSourceOnlyScriptFiles(Filenames);
	FAngelscriptPreprocessor Resolver;
	for (const FAngelscriptManager::FFilenamePair& Pair : Filenames)
		OutModuleNames.Add(Resolver.FilenameToModuleName(Pair.RelativePath));
}

bool FModKitCompilationManager::CheckLeafDirection(const FString& ModuleName, const TArray<FString>& DependencyModuleNames, FString& OutError)
{
	const auto Module = FAngelscriptManager::Get().GetModule(ModuleName);
	FModKitSourceOwner Owner;
	if (!Module.IsValid() || !RequiresModAdmission(*Module)) return false;
	if (!GetSourceOwner(Module.ToSharedRef(), Owner, OutError)) return true;
	for (const auto& DependencyName : DependencyModuleNames)
	{
		const auto Target = FAngelscriptManager::Get().GetModule(DependencyName);
		FModKitSourceOwner TargetOwner;
		if (!Target.IsValid() || !RequiresModAdmission(*Target)) continue;
		if (!GetSourceOwner(Target.ToSharedRef(), TargetOwner, OutError)) return true;
		if (!CanReferenceOwner(Owner, TargetOwner))
		{
			OutError = TEXT("MODKIT-LEAF-DIRECTION: undeclared leaf dependency");
			return true;
		}
	}
	return false;
}

ECompileResult FModKitCompilationManager::CompileOwnedSourceModules(ECompileType Type,
	const TArray<TSharedRef<FAngelscriptModuleDesc>>& Modules,
	TArray<TSharedRef<FAngelscriptModuleDesc>>& Compiled)
{
	check(IsInGameThread() && Type != ECompileType::Initial);
	auto& Manager = FAngelscriptManager::Get();
	if (Manager.bIsCompilingModules || IsAngelscriptExecutionShutdownStarted()) return ECompileResult::Error;
	FAngelscriptExecutionWriteScope Admission(FAngelscriptExecutionWriteScope::EAcquire::Try);
	// Preserve FIFO without donating an externally owned transaction to the ordinary source queue.
	if (!Admission.IsAcquired() || !Manager.DeferredSourceCompilations.IsEmpty())
	{
		Compiled.Reset();
		return ECompileResult::Deferred;
	}
	AngelscriptCoreExtensionPoints::FCompileSurfaceHandle Surface;
	const bool bNewModules = !Modules.IsEmpty() && !Modules.ContainsByPredicate([&](const auto& Module)
		{ return Manager.GetModule(Module->ModuleName).IsValid() && !IsEmptyPlaceholderModule(Module->ModuleName); });
	if (Manager.bUsedPrecompiledDataForPreprocessor || !CacheSets.IsEmpty()
		|| (Type == ECompileType::FullReload && bNewModules))
	{
		FString Error;
		const auto Result = AngelscriptCoreExtensionPoints::AcquireTransientCompileSurface(CacheClient, Surface, Error);
		if (Result == AngelscriptCoreExtensionPoints::ECompileSurfaceAdmission::Busy)
		{
			Compiled.Reset();
			return ECompileResult::Deferred;
		}
		if (Result != AngelscriptCoreExtensionPoints::ECompileSurfaceAdmission::Ready)
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_FAILED reason=compile-surface-unavailable detail=%s"), *Error);
			return ECompileResult::Error;
		}
	}
	ON_SCOPE_EXIT
	{
		if (Surface.Id)
		{
			const auto Released = AngelscriptCoreExtensionPoints::ReleaseTransientCompileSurface(CacheClient, Surface);
			check(Released == AngelscriptCoreExtensionPoints::ERemoveResult::Removed);
		}
	};
	// Deleted sources leave unscoped empty placeholders. Give the transient
	// admission check the verified same-owner scope only for this compilation;
	// keeping it on placeholders would prevent provider shutdown.
	TArray<TSharedPtr<FAngelscriptModuleDesc>> ScopedPlaceholders;
	if (Surface.Id && Type == ECompileType::FullReload)
	{
		for (const auto& Module : Modules)
		{
			const auto Active = Manager.GetModule(Module->ModuleName);
			FModKitSourceOwner PreviousOwner, NewOwner;
			FString Error;
			if (Active.IsValid() && !Active->CoreTypeScope && Module->CoreTypeScope
				&& IsEmptyPlaceholderModule(Module->ModuleName)
				&& GetModuleOwner(Active.ToSharedRef(), PreviousOwner, Error)
				&& GetSourceOwner(Module, NewOwner, Error) && CurrentOwner(NewOwner)
				&& PreviousOwner.Layer == EModKitSourceLayer::Leaf && NewOwner.Layer == PreviousOwner.Layer
				&& PreviousOwner.Id == NewOwner.Id && PreviousOwner.ScriptRoot == NewOwner.ScriptRoot
				&& Module->CoreTypeScope->Namespace == NewOwner.Id)
			{
				Active->CoreTypeScope = Module->CoreTypeScope;
				ScopedPlaceholders.Add(Active);
			}
		}
	}
	ON_SCOPE_EXIT
	{
		for (const auto& Placeholder : ScopedPlaceholders) Placeholder->CoreTypeScope.Reset();
	};
	FModKitApiAllowlist::FBlueprintAuditScope BlueprintAudit(Modules);
	return Manager.CompileModules(Type, Modules, Compiled);
}

void FModKitCompilationManager::Startup()
{
	bRemovalClosing = false;
    TypeScopeHandle = AngelscriptCoreExtensionPoints::RegisterTypeScope(
        [](FStringView AbsolutePath, AngelscriptCoreExtensionPoints::FTypeScope& Scope)
        {
            FModKitSourceOwner Owner;
            FString Error;
            if (AbsolutePath.IsEmpty())
            {
                if (!PendingCacheSet || !CurrentOwner(PendingCacheOwner)) return false;
                Owner = PendingCacheOwner;
            }
            else if (!FModKitModRegistry::ResolveSourceOwner(SourcePath(FString(AbsolutePath)), Owner, Error)) return false;
            if (Owner.Layer != EModKitSourceLayer::Leaf) return false;
            const FTCHARToUTF8 Id(*Owner.Id);
            asUINT Length = 0;
            auto* Engine = FAngelscriptManager::Get().GetScriptEngine();
            if (!Engine || Engine->ParseToken(Id.Get(), Id.Length(), &Length) != asTC_IDENTIFIER
                || Length != static_cast<asUINT>(Id.Length())) return false;
            Scope.Namespace = Owner.Id;
            Scope.Dependencies = Owner.LeafDependencies;
            return true;
        });
    if (!TypeScopeHandle.IsValid())
        UE_LOG(LogAngelscriptModKit, Fatal, TEXT("ModKit type scope provider unavailable"));
	CacheClient = AngelscriptCoreExtensionPoints::RegisterTransientCompileClient(FName(TEXT("ModKit")));
	if (!CacheClient.IsValid())
		UE_LOG(LogAngelscriptModKit, Fatal, TEXT("ModKit module cache client unavailable"));

	SourceCleanupHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		SourceAdmissions.Reset();
		CandidateSources.RemoveAll([](const FCandidateSource& Entry) { return !Entry.Module.IsValid(); });
		for (auto It = AdmittedSources.CreateIterator(); It; ++It)
			if (!It.Value().Module.IsValid()) It.RemoveCurrent();
		return true;
	}));
	SourceBindingHandle = FAngelscriptPreprocessor::OnPostProcessCode.AddStatic(&BindPreprocessedSources);
	ModuleAdmissionHandle = AngelscriptCoreExtensionPoints::RegisterModuleAdmission(&ValidateModuleAdmission);
	if (!ModuleAdmissionHandle.IsValid())
		UE_LOG(LogAngelscriptModKit, Fatal, TEXT("ModKit module admission provider unavailable"));
	ReloadHadErrorsHandle = FAngelscriptCodeModule::GetReloadHadErrors()
		.AddStatic(&FModKitCompilationManager::OnReloadHadErrors);

	if (FAngelscriptManager::IsInitialized())
	{
		// The runtime finished initial compilation before this module was loaded
		// (both load during the PostDefault phase). The broadcast has already
		// fired; record the event instead of running the append synchronously,
		// because a FullReload during plugin module loading triggers an
		// AssetManager assertion. Execution waits for the engine-init event.
		bInitialCompileFinished = true;
	}
	else
	{
		InitialCompileFinishedHandle = FAngelscriptCodeModule::GetOnInitialCompileFinished()
			.AddStatic(&FModKitCompilationManager::OnInitialCompileFinished);
	}

	// Fires after every plugin module has loaded and the AssetManager is
	// initialized, for both the Editor and Game binaries. The two events are
	// order-independent; the later one triggers the append.
	PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&FModKitCompilationManager::OnPostEngineInit);
}

bool FModKitCompilationManager::TryRevokeProviders()
{
	if (!IsInGameThread() || bIsAppendingModules || bIsRuntimeOpInProgress
		|| (FAngelscriptManager::IsInitialized() && FAngelscriptManager::Get().bIsCompilingModules)) return false;
	// Cancel queued preparation before attempting to retire any provider.
	bRemovalClosing = true;
	if (AppendRetryHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(AppendRetryHandle);
		AppendRetryHandle.Reset();
	}
	PendingAppendPreprocessor.Reset();
	AppendRetryDeadlineSeconds = 0.0;
	bDidRunAppend = true;
	SourceRemovalGenerations.Reset();
	if (CacheClient.IsValid())
	{
		if (AngelscriptCoreExtensionPoints::UnregisterTransientCompileClient(CacheClient)
			!= AngelscriptCoreExtensionPoints::ERemoveResult::Removed) return false;
		CacheClient.Reset();
	}
    if (TypeScopeHandle.IsValid())
    {
        if (AngelscriptCoreExtensionPoints::UnregisterTypeScope(TypeScopeHandle)
            != AngelscriptCoreExtensionPoints::ERemoveResult::Removed) return false;
        TypeScopeHandle.Reset();
    }
	if (ModuleAdmissionHandle.IsValid())
	{
		if (AngelscriptCoreExtensionPoints::UnregisterModuleAdmission(ModuleAdmissionHandle)
			!= AngelscriptCoreExtensionPoints::ERemoveResult::Removed) return false;
		ModuleAdmissionHandle.Reset();
	}

	return true;
}

bool FModKitCompilationManager::HasProviders()
{
	return ModuleAdmissionHandle.IsValid() || CacheClient.IsValid() || TypeScopeHandle.IsValid();
}

void FModKitCompilationManager::Shutdown()
{
	if (!TryRevokeProviders())
	{
		UE_LOG(LogAngelscriptModKit, Warning, TEXT("ModKit retains busy module admission or cache client"));
		return;
	}
	CacheSets.Reset();
	CacheOwners.Reset();
	FAngelscriptPreprocessor::OnPostProcessCode.Remove(SourceBindingHandle);
	SourceBindingHandle.Reset();
	FTSTicker::GetCoreTicker().RemoveTicker(SourceCleanupHandle);
	SourceCleanupHandle.Reset();
	CandidateSources.Reset();
	AdmittedSources.Reset();
	SourceRemovalGenerations.Reset();
	SourceAdmissions.Reset();
	PendingAppendPreprocessor.Reset();
	if (AppendRetryHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(AppendRetryHandle);
		AppendRetryHandle.Reset();
	}

	FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
	PostEngineInitHandle.Reset();

	FAngelscriptCodeModule::GetOnInitialCompileFinished().Remove(InitialCompileFinishedHandle);
	InitialCompileFinishedHandle.Reset();

	FAngelscriptCodeModule::GetReloadHadErrors().Remove(ReloadHadErrorsHandle);
	ReloadHadErrorsHandle.Reset();
}

void FModKitCompilationManager::OnInitialCompileFinished()
{
	check(IsInGameThread());
	bInitialCompileFinished = true;
	MaybeRunAppend();
}

void FModKitCompilationManager::OnPostEngineInit()
{
	check(IsInGameThread());
	bPostEngineInit = true;

	                                                                   
	// The registry settles asynchronously (ticker pump) and re-triggers
	// MaybeRunAppend once every uplugin mod reached a terminal state.
	FModKitModRegistry::StartActivation();

	MaybeRunAppend();
}

bool FModKitCompilationManager::OnAppendRetryTick(float DeltaTime)
{
	check(IsInGameThread());
	(void)DeltaTime;

	if (AngelscriptReloadSafety::ShouldDeferForPackageLoading())
	{
		AppendRetryDeadlineSeconds = 0.0;
		return true;
	}
	if (!bIsAppendingModules && !FAngelscriptManager::Get().bIsCompilingModules)
	{
		AppendRetryHandle.Reset();
		MaybeRunAppend();
		return false;
	}
	if (AppendRetryDeadlineSeconds == 0.0)
		AppendRetryDeadlineSeconds = FPlatformTime::Seconds() + UModKitSettings::Get()->ModStartupActivationTimeoutSec;

	if (AppendRetryDeadlineSeconds > 0.0 && FPlatformTime::Seconds() >= AppendRetryDeadlineSeconds)
	{
		AppendRetryHandle.Reset();
		PendingAppendPreprocessor.Reset();
		bDidRunAppend = true;
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_FAILED reason=compile-timeout"));
		return false;
	}
	if (bIsAppendingModules || FAngelscriptManager::Get().bIsCompilingModules)
	{
		if (FPlatformTime::Seconds() < AppendRetryDeadlineSeconds)
		{
			return true;
		}

		AppendRetryHandle.Reset();
		bDidRunAppend = true;
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_FAILED reason=compile-timeout"));
		return false;
	}

	AppendRetryHandle.Reset();
	MaybeRunAppend();
	return false;
}

void FModKitCompilationManager::MaybeRunAppend()
{
	check(IsInGameThread());
	if (IsRunningCookCommandlet() || FAngelscriptManager::bGeneratePrecompiledData)
	{
		return;
	}

	// Triple-event rendezvous: only run the append after the initial compile
	// finished, engine init arrived, and the mod activation roster settled.
	// Runs at most once.
	if (!bInitialCompileFinished || !bPostEngineInit || !FModKitModRegistry::IsActivationSettled() || bDidRunAppend)
	{
		return;
	}

	TArray<FAngelscriptManager::FFilenamePair> Filenames;
	ScanModScriptFiles(Filenames);
	ScanSourceOnlyScriptFiles(Filenames);

	// Roster-driven append : disabled mods contribute no scripts,
	// so a disabled mod's modules never emit MODKIT_APPEND_MODULE (invariant).
	Filenames.RemoveAll([](const FAngelscriptManager::FFilenamePair& Pair)
	{
		return !IsRosterModScript(Pair.RelativePath);
	});

	TArray<FString> ActivatedMods;
	FModKitModRegistry::GetActivatedModNames(ActivatedMods);
	if (Filenames.Num() == 0 && ActivatedMods.IsEmpty())
	{
		bDidRunAppend = true;
		return;
	}

	// A successful GameFeature activation callback can run while its packages
	// are still loading. Runtime FullReload admission rejects that window, so
	// keep the startup append pending until package loading ends. The compile
	// timeout applies only after the package-loading window has closed.
	if (AngelscriptReloadSafety::ShouldDeferForPackageLoading()
		|| bIsAppendingModules || FAngelscriptManager::Get().bIsCompilingModules)
	{
		if (!AppendRetryHandle.IsValid())
		{
			AppendRetryDeadlineSeconds = AngelscriptReloadSafety::ShouldDeferForPackageLoading() ? 0.0
				: FPlatformTime::Seconds() + UModKitSettings::Get()->ModStartupActivationTimeoutSec;
			AppendRetryHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateStatic(&FModKitCompilationManager::OnAppendRetryTick), 0.1f);
		}
		return;
	}

	bDidRunAppend = true;


	// Guard both source appends and cache-only loads.
	if (bIsAppendingModules || FAngelscriptManager::Get().bIsCompilingModules)
	{
		return;
	}

	TGuardValue<bool> AppendInProgress(bIsAppendingModules, true);
	FlushAsyncLoading();
	if (Filenames.Num() > 0)
	{
		UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_APPEND_BEGIN files=%d"), Filenames.Num());
		const FAppendOutcome Outcome = RunAppendBatch(Filenames, true, true, &PendingAppendPreprocessor);
		if (Outcome.bDeferred)
		{
			bDidRunAppend = false;
			if (!AppendRetryHandle.IsValid())
			{
				AppendRetryDeadlineSeconds = 0.0;
				AppendRetryHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateStatic(&FModKitCompilationManager::OnAppendRetryTick), 0.1f);
			}
			return;
		}
		PendingAppendPreprocessor.Reset();
		AppendRetryDeadlineSeconds = 0.0;
	}

	FModKitModRegistry::GetActivatedModNames(ActivatedMods);
	for (const FString& ModName : ActivatedMods)
	{
		TArray<FAngelscriptManager::FFilenamePair> SourceFiles;
		CollectModScriptFiles({ ModName }, SourceFiles);
		if (!SourceFiles.IsEmpty())
		{
			continue;
		}

		FString CacheDirectory;
		FString ModulePrefix;
		if (!GetModCacheLocation(ModName, CacheDirectory, ModulePrefix))
		{
			continue;
		}
		if (!IFileManager::Get().FileExists(*(CacheDirectory / TEXT("ModuleCaches.json"))))
		{
			continue;
		}

		FModKitCompileResult CacheResult = RequestCompileModSet({ ModName });
		if (!CacheResult.bSuccess)
		{
			UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_LOAD_FAILED name=%s"), *ModName);
			FModKitModRegistry::MarkModFailed(ModName, TEXT("cache-load-failed"));
		}
	}
}

FModKitCompileResult FModKitCompilationManager::RequestCompileModSet(const TArray<FString>& ModNames,
	TSharedPtr<FAngelscriptPreprocessor>* PendingPreprocessor)
{
	check(IsInGameThread());
	FModKitCompileResult Out;

	// Operation lock: reject overlapping runtime ops and compiles in progress.
	if (bIsRuntimeOpInProgress || FAngelscriptManager::Get().bIsCompilingModules)
	{
		Out.bBusy = true;
		return Out;
	}
	TGuardValue<bool> RuntimeOpInProgress(bIsRuntimeOpInProgress, true);

	TArray<FAngelscriptManager::FFilenamePair> Filenames;
	CollectModScriptFiles(ModNames, Filenames);

	// Roster filter (UX-D2): a disabled mod's scripts never compile, so the
	// runtime-load set must be on the roster before it can produce modules.
	Filenames.RemoveAll([](const FAngelscriptManager::FFilenamePair& Pair)
	{
		return !IsRosterModScript(Pair.RelativePath);
	});

	// Cache-only mods have no .as sources and need a complete cache set plus an activated roster entry.
	if (Filenames.Num() == 0)
	{
		if (ModNames.Num() != 1 || !FModKitModRegistry::ShouldCompileMod(ModNames[0]))
		{
			Out.bNoSource = true;
			return Out;
		}
		FString CacheDirectory;
		FString ModulePrefix;
		if (!GetModCacheLocation(ModNames[0], CacheDirectory, ModulePrefix))
		{
			Out.bNoSource = true;
			return Out;
		}
		if (!IFileManager::Get().FileExists(*(CacheDirectory / TEXT("ModuleCaches.json"))))
		{
			Out.bNoSource = true;
			return Out;
		}
		if (CacheSets.Contains(ModNames[0])) { Out.bConflict = true; Out.ConflictReason = TEXT("cache-set-already-loaded"); return Out; }

		const auto* VerifiedFiles = FModKitSignature::GetVerifiedPackageFiles(FPaths::GetPath(CacheDirectory));
		if (!VerifiedFiles) { Out.bConflict = true; Out.ConflictReason = TEXT("cache-snapshot-unverified"); return Out; }
		FVerifiedModCacheSet VerifiedSet;
		if (!BuildVerifiedCacheSet(*VerifiedFiles, CacheDirectory, VerifiedSet, Out.ConflictReason))
		{ Out.bConflict = true; return Out; }
		FModKitSourceOwner Owner;
		FString OwnerError;
		if (!FModKitModRegistry::ResolveSourceOwner(SourcePath(CacheDirectory / TEXT("ModuleCaches.json")), Owner, OwnerError)
			|| Owner.Id != ModNames[0] || Owner.Layer != EModKitSourceLayer::Leaf)
		{ Out.bConflict = true; Out.ConflictReason = TEXT("cache-owner-unverified"); return Out; }
		TGuardValue<const FVerifiedModCacheSet*> PendingSet(PendingCacheSet, &VerifiedSet);
		TGuardValue<FModKitSourceOwner> PendingOwner(PendingCacheOwner, Owner);
		AngelscriptCoreExtensionPoints::FCacheLoadResult Loaded;
		const auto Admission = AngelscriptCoreExtensionPoints::TryLoadModuleCacheSet(CacheClient, VerifiedSet.View(), Loaded);
		Out.bBusy = Admission == AngelscriptCoreExtensionPoints::ECacheAdmission::Busy;
		Out.bSuccess = Admission == AngelscriptCoreExtensionPoints::ECacheAdmission::Completed
			&& Loaded.bSucceeded && Loaded.bVmCommitted && Loaded.Set.Id != 0 && !Loaded.CommittedModules.IsEmpty();
		if (Out.bSuccess)
		{
			for (const auto& Module : Loaded.CommittedModules) Out.CompiledModuleNames.Add(Module.ModuleName);
			Out.Result = ECompileResult::FullyHandled;
		}
		else UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_LOAD_FAILED name=%s detail=%s"), *ModNames[0], *Loaded.Error);
		if (Loaded.bVmCommitted && Loaded.Set.Id != 0)
		{
			CacheSets.Add(ModNames[0], MoveTemp(Loaded));
			CacheOwners.Add(ModNames[0], Owner);
		}
		return Out;
	}

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_APPEND_BEGIN files=%d op=load"), Filenames.Num());

	// UX-D4 conflict detection runs inside the batch on the preprocessed set.
	FAppendOutcome Outcome = RunAppendBatch(Filenames, /*bRunConflictCheck=*/true,
		/*bIgnoreAlreadyActiveModules=*/false, PendingPreprocessor);
	Out.bDeferred = Outcome.bDeferred;

	if (Outcome.bConflict)
	{
		Out.bConflict = true;
		Out.ConflictReason = Outcome.ConflictReason;
		return Out;
	}

	Out.Result = Outcome.Result;
	Out.CompiledModuleNames = MoveTemp(Outcome.CompiledModuleNames);
	Out.bSuccess = Outcome.bNothingToCompile
		|| Outcome.Result == ECompileResult::FullyHandled
		|| Outcome.Result == ECompileResult::PartiallyHandled;
	return Out;
}

FModKitCompileResult FModKitCompilationManager::RequestReplaceMod(const FString& ModName,
	TSharedPtr<FAngelscriptPreprocessor>* PendingPreprocessor)
{
	check(IsInGameThread());
	FModKitCompileResult Out;
	if (bIsRuntimeOpInProgress || FAngelscriptManager::Get().bIsCompilingModules)
	{ Out.bBusy = true; return Out; }
	TGuardValue<bool> RuntimeOpInProgress(bIsRuntimeOpInProgress, true);
	if (!FModKitModRegistry::IsModActivated(ModName))
	{ Out.bConflict = true; Out.ConflictReason = TEXT("not-loaded"); return Out; }
	TArray<FString> ReloadOwners { ModName };
	const auto ActiveModules = FAngelscriptManager::Get().GetActiveModules();
	for (int32 Index = 0; Index < ReloadOwners.Num(); ++Index)
	{
		for (const auto& Module : ActiveModules)
		{
			FModKitSourceOwner Owner;
			FString Error;
			if (GetModuleOwner(Module, Owner, Error) && Owner.Layer == EModKitSourceLayer::Leaf
				&& FModKitModRegistry::IsModActivated(Owner.Id)
				&& Owner.LeafDependencies.Contains(ReloadOwners[Index]))
				ReloadOwners.AddUnique(Owner.Id);
		}
	}
	TArray<FAngelscriptManager::FFilenamePair> Filenames;
	CollectModScriptFiles(ReloadOwners, Filenames);
	for (const auto& Module : ActiveModules)
		if (ReloadOwners.ContainsByPredicate([&](const FString& Owner) { return OwnsModule(Module, Owner); }))
			for (const auto& Section : Module->Code)
				if (!Filenames.ContainsByPredicate([&](const auto& File) { return File.AbsolutePath == Section.AbsoluteFilename; }))
					Filenames.Add({ Section.AbsoluteFilename, Section.RelativeFilename });
	if (Filenames.IsEmpty()) { Out.bNoSource = true; return Out; }
	FAppendOutcome Outcome = RunAppendBatch(Filenames, true, false, PendingPreprocessor, ReloadOwners);
	Out.Result = Outcome.Result;
	Out.bDeferred = Outcome.bDeferred;
	Out.bConflict = Outcome.bConflict;
	Out.ConflictReason = MoveTemp(Outcome.ConflictReason);
	Out.CompiledModuleNames = MoveTemp(Outcome.CompiledModuleNames);
	Out.bSuccess = Outcome.Result == ECompileResult::FullyHandled;
	return Out;
}

bool FModKitCompilationManager::CanBeginRemoval(const FString& ModName, FString& Error)
{
	check(IsInGameThread());
	const FString Key = ModName.ToLower();
	if (bRemovalClosing) { Error = TEXT("source-removal-closing"); return false; }
	if (const auto* Generations = SourceRemovalGenerations.Find(Key))
	{
		for (const auto& Generation : *Generations)
		{
			const auto Active = FAngelscriptManager::Get().GetModuleByModuleName(Generation.ModuleName);
			if (!Active.IsValid() || Active->CoreGenerationId != Generation.Generation)
			{
				// Reject this request, never silently substitute a newer generation.
				SourceRemovalGenerations.Remove(Key);
				Error = TEXT("source-removal-generation-expired");
				return false;
			}
		}
		return true;
	}
	if (CacheSets.Contains(ModName)) return true;
	if (FAngelscriptManager::Get().bUsedPrecompiledDataForPreprocessor || !CacheSets.IsEmpty())
	{
		TArray<AngelscriptCoreExtensionPoints::FModuleGenerationId> Generations;
		for (const auto& Module : FAngelscriptManager::Get().GetActiveModules())
			if (OwnsModule(Module, ModName)) Generations.Add({Module->ModuleName, Module->CoreGenerationId});
		if (Generations.IsEmpty()) { Error = TEXT("source-owner-unknown"); return false; }
		// Pin before owner teardown callbacks can change another Mod's cache state.
		// Only an in-flight request may retain these identities across Busy retries.
		SourceRemovalGenerations.Add(Key, MoveTemp(Generations));
		return true;
	}
	FString CacheDirectory, ModulePrefix;
	if (GetModCacheLocation(ModName, CacheDirectory, ModulePrefix)
		&& IFileManager::Get().FileExists(*(CacheDirectory / TEXT("ModuleCaches.json"))))
	{
		Error = TEXT("cache-owner-unknown");
		return false;
	}
	return true;
}

void FModKitCompilationManager::CancelModuleRemoval(const FString& ModName)
{
	check(IsInGameThread());
	SourceRemovalGenerations.Remove(ModName.ToLower());
}

bool FModKitCompilationManager::HasCacheSet(const FString& ModName)
{
	return CacheSets.Contains(ModName);
}

FModKitCompileResult FModKitCompilationManager::RequestModuleRemoval(const FString& ModName)
{
	check(IsInGameThread());
	FModKitCompileResult Out;
	if (!CanBeginRemoval(ModName, Out.ConflictReason)) { Out.bConflict = true; return Out; }

	if (bIsRuntimeOpInProgress || FAngelscriptManager::Get().bIsCompilingModules)
	{
		Out.bBusy = true;
		return Out;
	}
	TGuardValue<bool> RuntimeOpInProgress(bIsRuntimeOpInProgress, true);

	const FString Key = ModName.ToLower();
	if (const auto* Generations = SourceRemovalGenerations.Find(Key))
	{
		AngelscriptCoreExtensionPoints::FSourceRemoveResult Removed;
		const auto Admission = AngelscriptCoreExtensionPoints::TryRemoveSourceGenerations(CacheClient, *Generations, Removed);
		Out.bBusy = Admission == AngelscriptCoreExtensionPoints::ECacheAdmission::Busy;
		Out.bSuccess = Admission == AngelscriptCoreExtensionPoints::ECacheAdmission::Completed && Removed.bSucceeded;
		Out.ConflictReason = Removed.Error;
		if (Out.bSuccess)
		{
			for (const auto& Generation : Removed.RemovedGenerations)
			{
				Out.CompiledModuleNames.Add(Generation.ModuleName);
				AdmittedSources.Remove(Generation.Generation);
			}
			Out.Result = ECompileResult::FullyHandled;
		}
		if (!Out.bBusy) CancelModuleRemoval(ModName);
		return Out;
	}

	TArray<FAngelscriptManager::FFilenamePair> Filenames;
	CollectModScriptFiles({ ModName }, Filenames);

	if (CacheSets.Contains(ModName) || Filenames.Num() == 0)
	{
		FString CacheDirectory;
		FString ModulePrefix;
		if (CacheSets.Contains(ModName) || (GetModCacheLocation(ModName, CacheDirectory, ModulePrefix)
			&& IFileManager::Get().FileExists(*(CacheDirectory / TEXT("ModuleCaches.json")))))
		{
			const auto* Loaded = CacheSets.Find(ModName);
			if (!Loaded) { Out.bConflict = true; Out.ConflictReason = TEXT("cache-owner-unknown"); return Out; }
			AngelscriptCoreExtensionPoints::FCacheRemoveResult Removed;
			const auto Admission = AngelscriptCoreExtensionPoints::TryRemoveModuleCacheSet(CacheClient, Loaded->Set, Removed);
			Out.bBusy = Admission == AngelscriptCoreExtensionPoints::ECacheAdmission::Busy;
			Out.bSuccess = Admission == AngelscriptCoreExtensionPoints::ECacheAdmission::Completed && Removed.bSucceeded;
			if (Out.bSuccess)
			{
				for (const auto& Module : Removed.RemovedModules) Out.CompiledModuleNames.Add(Module.ModuleName);
				CacheSets.Remove(ModName);
				CacheOwners.Remove(ModName);
				Out.Result = ECompileResult::FullyHandled;
			}
			else UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_CACHE_REMOVE_FAILED name=%s detail=%s"), *ModName, *Removed.Error);
			return Out;
		}
	}

	// Remove the active generation, including sections deleted from disk since load.
	for (const auto& Module : FAngelscriptManager::Get().GetActiveModules())
		if (FModKitCompilationManager::OwnsModule(Module, ModName)
			&& Module->PrecompiledData == nullptr)
			for (const auto& Section : Module->Code)
				if (!Filenames.ContainsByPredicate([&](const auto& File) { return File.AbsolutePath == Section.AbsoluteFilename; }))
					Filenames.Add({ Section.AbsoluteFilename, Section.RelativeFilename });

	if (Filenames.Num() == 0)
	{
		Out.bNoSource = true;
		return Out;
	}
	if (FAngelscriptManager::Get().bUsedPrecompiledDataForPreprocessor || !CacheSets.IsEmpty())
	{
		Out.bConflict = true;
		Out.ConflictReason = TEXT("compile-surface-unavailable");
		return Out;
	}

	// Same defensive PrecompiledData nulling as the append batch.
	{
		FAngelscriptManager& Mgr = FAngelscriptManager::Get();
		for (const TSharedRef<FAngelscriptModuleDesc>& ActiveModule : Mgr.GetActiveModules())
		{
			ActiveModule->PrecompiledData = nullptr;
		}
	}

	// The removal path recompiles every script file with bTreatAsDeleted=true:
	// the preprocessor blanks RawCode, the class generator marks the old
	// classes as removed, and the FullReload destroys the runtime-tracked
	// instances and deactivates the classes. The empty placeholder modules stay
	// in ActiveModules (C0+1 constant module-count semantics, v2 §2.5).
	FAngelscriptPreprocessor Preprocessor;
	for (const FAngelscriptManager::FFilenamePair& Filename : Filenames)
	{
		Preprocessor.AddFile(Filename.RelativePath, Filename.AbsolutePath, false, /*bTreatAsDeleted=*/true);
	}

	const auto AdmissionsBeforePreprocess = SourceAdmissions;
	if (!Preprocessor.Preprocess())
	{
		SourceAdmissions = AdmissionsBeforePreprocess;
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_FAILED result=PreprocessError"));
		EmitErrorSummary();
		return Out;
	}

	TArray<TSharedRef<FAngelscriptModuleDesc>> Modules = Preprocessor.GetModulesToCompile();
	if (Modules.Num() == 0)
	{
		Out.bNoSource = true;
		return Out;
	}

	TArray<TSharedRef<FAngelscriptModuleDesc>> CompiledModules;
	Out.Result = CompileOwnedSourceModules(ECompileType::FullReload, Modules, CompiledModules);
	Out.bDeferred = Out.Result == ECompileResult::Deferred;
	if (Out.bDeferred) return Out;

	Out.bSuccess = Out.Result == ECompileResult::FullyHandled || Out.Result == ECompileResult::PartiallyHandled;
	for (const TSharedRef<FAngelscriptModuleDesc>& Module : CompiledModules)
	{
		Out.CompiledModuleNames.Add(Module->ModuleName);
	}

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_APPEND_RESULT result=%s modules=%d op=unload"), LexECompileResult(Out.Result), CompiledModules.Num());
	if (!Out.bSuccess)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_APPEND_FAILED result=%s"), LexECompileResult(Out.Result));
	}
	return Out;
}

bool FModKitCompilationManager::IsEmptyPlaceholderModule(const FString& ModuleName)
{
	TSharedPtr<FAngelscriptModuleDesc> Desc = FAngelscriptManager::Get().GetModule(ModuleName);
	if (!Desc.IsValid())
	{
		return false;
	}
	if (Desc->Classes.Num() != 0 || Desc->Enums.Num() != 0 || Desc->Delegates.Num() != 0)
	{
		return false;
	}

	// Function/global counts come from the live asCModule via the public
	// asIScriptEngine interface (v2 §2.5).
	asIScriptEngine* Engine = FAngelscriptManager::Get().GetScriptEngine();
	if (Engine == nullptr)
	{
		return true;
	}
	asIScriptModule* ScriptModule = Engine->GetModule(TCHAR_TO_ANSI(*ModuleName));
	if (ScriptModule == nullptr)
	{
		return true;
	}
	return ScriptModule->GetFunctionCount() == 0 && ScriptModule->GetGlobalVarCount() == 0;
}

bool FModKitCompilationManager::HasEmptyPlaceholderModule(const FString& ModName)
{
	bool bFound = false;
	for (const TSharedRef<FAngelscriptModuleDesc>& ActiveModule : FAngelscriptManager::Get().GetActiveModules())
	{
		if (!OwnsModule(ActiveModule, ModName))
		{
			continue;
		}
		bFound = true;
		if (!IsEmptyPlaceholderModule(ActiveModule->ModuleName))
		{
			return false;
		}
	}
	return bFound;
}

void FModKitCompilationManager::OnReloadHadErrors()
{
	check(IsInGameThread());
	// Runtime already logs each compiler diagnostic before broadcasting this event.
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_APPEND_ERROR_SUMMARY see compiler diagnostics"));
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/ScopeExit.h"

namespace
{
class FModKitRemovedSourceCommand : public IAutomationLatentCommand
{
public:
	FModKitRemovedSourceCommand(FAutomationTestBase* InTest, bool bInLastSource)
		: Test(InTest), bLastSource(bInLastSource)
	{
		ModName = bLastSource ? TEXT("M02bRemoveLastMod") : TEXT("M02bRemoveSectionMod");
	}
	~FModKitRemovedSourceCommand() override
	{
		if (bOwnsFixture)
		{
			auto& Disk = FPlatformFileManager::Get().GetPlatformPhysical();
			const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("FeatureMods"));
			Disk.DeleteDirectoryRecursively(*(Root / ModName));
			Disk.DeleteFile(*(Root / (ModName + TEXT(".manifest.json"))));
		}
	}
	bool Update() override
	{
		auto& Manager = FAngelscriptManager::Get();
		auto& Disk = FPlatformFileManager::Get().GetPlatformPhysical();
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("FeatureMods"));
		const FString Fixture = FPaths::ProjectDir() / TEXT("FeatureModPackages/review") / ModName;
		if (Phase == 0)
		{
			if (!Test->TestFalse(TEXT("Source fixture does not overwrite an existing FeatureMod"), Disk.DirectoryExists(*(Root / ModName))
				|| Disk.FileExists(*(Root / (ModName + TEXT(".manifest.json")))))) return true;
			bOwnsFixture = true;
			if (!Test->TestTrue(TEXT("Install signed V1"), Disk.CopyDirectoryTree(*Root, *(Fixture / TEXT("v1")), true))) return true;
			if (!Test->TestTrue(TEXT("Discover signed V1"), FModKitModRegistry::RescanOrAdd(ModName))) return true;
			if (!Test->TestTrue(TEXT("Activate V1 owner"), FModKitModRegistry::StartRuntimeActivation(ModName))) return true;
			Deadline = FPlatformTime::Seconds() + 60.; Phase = 1; return false;
		}
		if (FPlatformTime::Seconds() > Deadline) { Test->AddError(TEXT("Signed source replacement activation timed out")); return true; }
		if (!FModKitModRegistry::IsModActivated(ModName)) return false;
		const auto Initial = FModKitCompilationManager::RequestCompileModSet({ModName});
		if (!Test->TestTrue(TEXT("Compile V1 sources"), Initial.bSuccess)) return true;
		TArray<FAngelscriptManager::FFilenamePair> OldFiles;
		FString RemovedModule, KeptModule;
		for (const auto& Module : Manager.GetActiveModules())
			if (FModKitCompilationManager::OwnsModule(Module, ModName))
				for (const auto& Section : Module->Code)
				{
					OldFiles.Add({Section.AbsoluteFilename, Section.RelativeFilename});
					if (Section.RelativeFilename.EndsWith(TEXT("A.as"))) RemovedModule = Module->ModuleName;
					if (Section.RelativeFilename.EndsWith(TEXT("B.as"))) KeptModule = Module->ModuleName;
				}
		ON_SCOPE_EXIT
		{
			FAngelscriptPreprocessor Removal;
			for (const auto& File : OldFiles) Removal.AddFile(File.RelativePath, File.AbsolutePath, false, true);
			TArray<TSharedRef<FAngelscriptModuleDesc>> Compiled;
			if (Removal.Preprocess()) FModKitCompilationManager::CompileOwnedSourceModules(
				ECompileType::FullReload, Removal.GetModulesToCompile(), Compiled);
			FModKitModRegistry::MarkModUnloaded(ModName);
		};
		auto Read = [&](const FString& ModuleName, const char* FunctionName) -> int32
		{
			auto* Module = Manager.GetScriptEngine()->GetModule(TCHAR_TO_ANSI(*ModuleName));
			const FString Declaration = FString::Printf(TEXT("int %s::%s()"), *ModName, ANSI_TO_TCHAR(FunctionName));
			auto* Function = Module ? Module->GetFunctionByDecl(TCHAR_TO_ANSI(*Declaration)) : nullptr;
			if (!Test->TestNotNull(TEXT("The signed section owns an executable function"), Function)) return -1;
			auto* Context = Manager.GetScriptEngine()->CreateContext(); Context->Prepare(Function);
			Test->TestEqual(TEXT("Execute the signed source body"), Context->Execute(), int(asEXECUTION_FINISHED));
			const int32 Value = int32(Context->GetReturnDWord()); Context->Release(); return Value;
		};
		Test->TestEqual(TEXT("V1 A executes"), Read(RemovedModule, "M02bRemovedValue"), 17);
		if (!bLastSource) Test->TestEqual(TEXT("V1 B executes"), Read(KeptModule, "M02bKeptValue"), 27);
		if (!Test->TestTrue(TEXT("Remove physical V1 package"), Disk.DeleteDirectoryRecursively(*(Root / ModName)))) return true;
		if (!Test->TestTrue(TEXT("Install complete signed V2 source set"), Disk.CopyDirectoryTree(*Root, *(Fixture / TEXT("v2")), true))) return true;
		FString Reason;
		if (!Test->TestTrue(TEXT("Verify signed V2 before replacement"), FModKitModRegistry::PrepareReload(ModName, Reason))) { Test->AddError(Reason); return true; }
		const auto Result = FModKitCompilationManager::RequestReplaceMod(ModName);
		FModKitModRegistry::CompleteReload(ModName, Result.bSuccess, Result.Result == ECompileResult::NativeReloadFailed);
		Test->TestFalse(TEXT("Deletion is a replacement, not no-source rejection"), Result.bNoSource);
		Test->TestTrue(TEXT("Signed V2 replaces the complete owner source set"), Result.bSuccess);
		Test->TestTrue(TEXT("Removed A section is a tombstoned empty placeholder"), FModKitCompilationManager::IsEmptyPlaceholderModule(RemovedModule));
		auto* Removed = Manager.GetScriptEngine()->GetModule(TCHAR_TO_ANSI(*RemovedModule));
		const FString RemovedDeclaration = FString::Printf(TEXT("int %s::M02bRemovedValue()"), *ModName);
		Test->TestTrue(TEXT("No live A function survives replacement"), !Removed || !Removed->GetFunctionByDecl(TCHAR_TO_ANSI(*RemovedDeclaration)));
		if (!bLastSource) Test->TestEqual(TEXT("Only V2 B executes"), Read(KeptModule, "M02bKeptValue"), 37);
		return true;
	}
private:
	FAutomationTestBase* Test;
	bool bLastSource;
	bool bOwnsFixture = false;
	FString ModName;
	int32 Phase = 0;
	double Deadline = 0.;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitRemoveSectionTest, "AngelscriptModKit.Reload.RemoveSection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitRemoveSectionTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(FModKitRemovedSourceCommand(this, false));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitRemoveLastSourceTest, "AngelscriptModKit.Reload.RemoveLastSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitRemoveLastSourceTest::RunTest(const FString&)
{
	ADD_LATENT_AUTOMATION_COMMAND(FModKitRemovedSourceCommand(this, true));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitLeafDependencyGraphTest,
	"AngelscriptModKit.FeatureMods.LeafDependencyGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FModKitLeafDependencyGraphTest::RunTest(const FString&)
{
	FModKitSourceOwner Inventory, Weapons;
	Inventory.Id = TEXT("DeveloperInventory");
	Weapons.Id = TEXT("DeveloperWeapons");
	Inventory.Layer = Weapons.Layer = EModKitSourceLayer::Leaf;
	Inventory.LeafDependencies.Add(Weapons.Id);
	TestTrue(TEXT("Declared candidate dependency is permitted"), FModKitCompilationManager::CanReferenceOwner(Inventory, Weapons));
	TestFalse(TEXT("Reverse leaf edge is not implicitly declared"), FModKitCompilationManager::CanReferenceOwner(Weapons, Inventory));
	Inventory.LeafDependencies.Reset();
	TestFalse(TEXT("Removing the candidate dependency revokes permission"), FModKitCompilationManager::CanReferenceOwner(Inventory, Weapons));
	Inventory.LeafDependencies.Add(Weapons.Id);
	for (const auto Layer : {EModKitSourceLayer::Trunk, EModKitSourceLayer::Branch})
	{
		Inventory.Layer = Layer;
		TestFalse(TEXT("Declarations do not permit trunk or branch to reference a leaf"), FModKitCompilationManager::CanReferenceOwner(Inventory, Weapons));
	}
	FModKitSourceOwner Missing;
	TestFalse(TEXT("Unknown target is not fabricated from its declared name"), FModKitCompilationManager::CanReferenceOwner(Inventory, Missing));
	FString Error;
	const TSharedRef<FAngelscriptModuleDesc> Unknown = MakeShared<FAngelscriptModuleDesc>();
	Unknown->ModuleName = TEXT("FeatureMods.Official.Forged");
	FModKitSourceOwner Owner;
	TestFalse(TEXT("A module name cannot supply ownership"), FModKitCompilationManager::GetSourceOwner(Unknown, Owner, Error));
	TestFalse(TEXT("Unknown candidate cannot acquire a known owner"), FModKitCompilationManager::OwnsModule(Unknown, TEXT("Official")));
	TestFalse(TEXT("An unknown generation cannot acquire a cache owner"), FModKitCompilationManager::GetModuleOwner(Unknown, Owner, Error));
	TestFalse(TEXT("A forged module name cannot acquire early budget ownership"),
		FModKitCompilationManager::GetGenerationOwner(*Unknown, Owner, Error));
	const TMap<FString, TArray<uint8>> EmptyVerifiedFiles;
	FVerifiedModCacheSet EmptySet;
	TestFalse(TEXT("An unspecified cache-set contract cannot produce a verified view"),
		BuildVerifiedCacheSet(EmptyVerifiedFiles, FString(), EmptySet, Error));
	return true;
}

#if WITH_EDITOR
#include "FeatureMods/ModKitRuntimeOps.h"

class FModKitSourceRemovalRouteCommand final : public IAutomationLatentCommand
{
public:
    explicit FModKitSourceRemovalRouteCommand(FAutomationTestBase* InTest, bool InLifecycle = false, bool InRevoke = false)
        : Test(InTest), bLifecycle(InLifecycle), bRevoke(InRevoke) {}
    ~FModKitSourceRemovalRouteCommand() override
    {
        if (Hold) { Hold->Unprepare(); Hold->Release(); }
    }
    bool Update() override
    {
        const FString Source = TEXT("H5TimeoutSource");
        const FString Key = Source.ToLower();
        auto& Manager = FAngelscriptManager::Get();
        const double Now = FPlatformTime::Seconds();
        if (Phase == 0)
        {
            if (!Test->TestFalse(TEXT("Route regression uses a non-precompiled host"), Manager.bUsedPrecompiledDataForPreprocessor)) return true;
            if (!Test->TestTrue(TEXT("Exactly one H5 set starts loaded"), CacheSets.Num() == 1 && FModKitCompilationManager::HasCacheSet(TEXT("H5Collection")))) return true;
            int32 Sources = 0;
            for (const auto& Module : Manager.GetActiveModules())
                if (FModKitCompilationManager::OwnsModule(Module, Source))
                {
                    ++Sources;
                    Generation = {Module->ModuleName, Module->CoreGenerationId};
                    Hold = Manager.GetScriptEngine()->CreateContext();
                    if (!Test->TestTrue(TEXT("Prepare a real source hold"), Hold->Prepare(Module->ScriptModule->GetFunctionByDecl("int H5TimeoutSource::ModKitSourceTimeoutValue()")) >= 0)) return true;
                }
            if (!Test->TestEqual(TEXT("One admitted source is active"), Sources, 1)) return true;
            Baseline = Manager.GetActiveModules().Num() - 3;
            Deadline = Now + 20.0;
            if (bLifecycle) FModKitRuntimeOps::RequestUnload(Source);
            else FModKitRuntimeOps::RequestTimeoutDisable(Source);
            Phase = bLifecycle ? 10 : 1;
            return false;
        }
        if (Now > Deadline) { Test->AddError(TEXT("Stable source teardown route did not drain")); return true; }
        if (Phase == 10)
        {
            if (FModKitRuntimeOps::IsOperationPending(Source)) return false;
            Test->TestFalse(TEXT("Ordinary Busy unload ends its cleanup identity when dequeued"), SourceRemovalGenerations.Contains(Key));
            Test->TestTrue(TEXT("Rejected ordinary unload preserves the held generation"), Manager.GetModuleByModuleName(Generation.ModuleName).IsValid());
            Hold->Unprepare(); Hold->Release(); Hold = nullptr;
            FString Error;
            Test->TestTrue(TEXT("Failure test starts with a freshly confirmed admitted identity"), FModKitCompilationManager::CanBeginRemoval(Source, Error));
            {
                TGuardValue<FDelegateHandle> InvalidClient(CacheClient, FDelegateHandle());
                const auto Failed = FModKitCompilationManager::RequestModuleRemoval(Source);
                Test->TestFalse(TEXT("Non-Busy failure is terminal"), Failed.bBusy || Failed.bSuccess);
                Test->TestFalse(TEXT("Non-Busy failure clears the identity"), SourceRemovalGenerations.Contains(Key));
            }
            Test->TestTrue(TEXT("Cancellation starts with a real admitted identity"), FModKitCompilationManager::CanBeginRemoval(Source, Error));
            FModKitCompilationManager::CancelModuleRemoval(Source);
            Test->TestFalse(TEXT("Explicit cancellation releases the identity"), SourceRemovalGenerations.Contains(Key));
            Test->TestTrue(TEXT("Queue cancellation starts with an admitted identity"), FModKitCompilationManager::CanBeginRemoval(Source, Error));
            FModKitRuntimeOps::RequestUnload(Source);
            FModKitRuntimeOps::Shutdown();
            Test->TestFalse(TEXT("RuntimeOps Shutdown cancels queued requests"), FModKitRuntimeOps::IsOperationPending(Source));
            Test->TestFalse(TEXT("RuntimeOps Shutdown clears the cancelled request identity"), SourceRemovalGenerations.Contains(Key));
            FModKitRuntimeOps::Startup();
            Test->TestTrue(TEXT("Close starts with a real admitted identity"), FModKitCompilationManager::CanBeginRemoval(Source, Error));
            {
                // Restore only the local close latch for the remaining scenarios;
                // the failed core unregister leaves the actual provider registered.
                TGuardValue<bool> RestoreClosing(bRemovalClosing, bRemovalClosing);
                FModKitCompilationManager::Shutdown();
                Test->TestTrue(TEXT("Busy provider remains registered after failed Shutdown"), FModKitCompilationManager::HasProviders());
                Test->TestFalse(TEXT("Failed Shutdown still cancels cleanup identities"), SourceRemovalGenerations.Contains(Key));
                Test->TestFalse(TEXT("Closing refuses creation of a new identity"), FModKitCompilationManager::CanBeginRemoval(Source, Error));
                Test->TestEqual(TEXT("Closing refusal is explicit"), Error, FString(TEXT("source-removal-closing")));
            }
            if (!Test->TestTrue(TEXT("Pin the real generation before unloading H5"), FModKitCompilationManager::CanBeginRemoval(Source, Error))) return true;
            FModKitRuntimeOps::RequestUnload(TEXT("H5Collection"));
            Phase = 11;
            return false;
        }
        if (Phase == 11)
        {
            if (FModKitRuntimeOps::IsOperationPending(TEXT("H5Collection"))) return false;
            if (!Test->TestTrue(TEXT("Replacement uses the ordinary host after H5 unload"), CacheSets.IsEmpty())) return true;
            FString Error;
            {
            const auto Active = Manager.GetModuleByModuleName(Generation.ModuleName);
            if (!Active.IsValid()) { Test->AddError(TEXT("Held generation disappeared before replacement")); return true; }
            FAngelscriptPreprocessor Processor;
            for (const auto& Section : Active->Code) Processor.AddFile(Section.RelativeFilename, Section.AbsoluteFilename);
            if (!Test->TestTrue(TEXT("Preprocess the real signed source for replacement"), Processor.Preprocess())) return true;
            TArray<TSharedRef<FAngelscriptModuleDesc>> Compiled;
            if (!Test->TestTrue(TEXT("Ordinary FullReload replaces after Busy dequeue and H5 unload"),
                FModKitCompilationManager::CompileOwnedSourceModules(ECompileType::FullReload, Processor.GetModulesToCompile(), Compiled)
                    == ECompileResult::FullyHandled)) return true;
            const auto Replacement = Manager.GetModuleByModuleName(Generation.ModuleName);
            if (!Test->TestTrue(TEXT("Real SwapInModules publishes a new generation"), Replacement.IsValid()
                && Replacement->CoreGenerationId != Generation.Generation)) return true;
            const auto Refused = FModKitCompilationManager::RequestModuleRemoval(Source);
            Test->TestFalse(TEXT("Expired identity rejects this operation without retry or commit"), Refused.bBusy || Refused.bSuccess);
            Test->TestEqual(TEXT("Expired identity has an explicit reason"), Refused.ConflictReason, FString(TEXT("source-removal-generation-expired")));
            Test->TestFalse(TEXT("Invalidation releases the expired identity"), SourceRemovalGenerations.Contains(Key));
            Test->TestTrue(TEXT("Refusal preserves the new active generation"), Manager.GetModuleByModuleName(Generation.ModuleName) == Replacement);
            FModKitSourceOwner Owner;
            Test->TestTrue(TEXT("A separate request can confirm the replacement's actual admission"),
                FModKitCompilationManager::GetGenerationOwner(*Replacement, Owner, Error) && Owner.Id == Source);
            const auto Removed = FModKitCompilationManager::RequestModuleRemoval(Source);
            Test->TestTrue(TEXT("Subsequent independent cleanup succeeds on the ordinary host"), Removed.bSuccess);
            }
            Test->TestTrue(TEXT("Successful cleanup leaves only the existing ordinary empty placeholder"), FModKitCompilationManager::HasEmptyPlaceholderModule(Source));
            Test->TestFalse(TEXT("Success leaves no cleanup identity"), SourceRemovalGenerations.Contains(Key));
            // The shared host also admits this independent source fixture at startup.
            Test->TestTrue(TEXT("Drain the other admitted fixture before provider revocation"),
                FModKitCompilationManager::RequestModuleRemoval(TEXT("BuiltInPackaging")).bSuccess);
            if (bRevoke)
            {
                Test->TestTrue(TEXT("Drain the startup visibility fixture before provider revocation"),
                    FModKitCompilationManager::RequestModuleRemoval(TEXT("ModKitStartupVisibility")).bSuccess);
                CollectGarbage(RF_NoFlags);
                DrainFrames = 0;
                DrainAttempts = 0;
                Deadline = Now + 30.0;
                Phase = 14;
                return false;
            }
            Test->TestTrue(TEXT("Restore the source fixture for following automation tests"),
                FModKitCompilationManager::RequestCompileModSet({Source}).bSuccess);
            Test->TestTrue(TEXT("Restore the built-in source fixture"),
                FModKitCompilationManager::RequestCompileModSet({TEXT("BuiltInPackaging")}).bSuccess);
            Phase = 12;
            Deadline = Now + 20.0;
            return false;
        }
        if (Phase == 14)
        {
            if (Manager.bIsCompilingModules || FAngelscriptManager::bIsHotReloading || IsGarbageCollecting()
                || !Manager.FileChangesDetectedForReload.IsEmpty() || !Manager.FileDeletionsDetectedForReload.IsEmpty()
                || FModKitRuntimeOps::IsOperationPending(Source) || FModKitRuntimeOps::IsOperationPending(TEXT("H5Collection")))
                return false;
            if (++DrainFrames < 3) return false;
            DrainFrames = 0;
            const bool Revoked = FModKitCompilationManager::TryRevokeProviders();
            Test->AddInfo(FString::Printf(TEXT("MK03_DRAIN attempt=%d revoked=%d"), ++DrainAttempts, int32(Revoked)));
            if (!Revoked && DrainAttempts < 5) return false;
            Test->TestTrue(TEXT("Drained Shutdown revokes the actual providers"), Revoked);
            FModKitCompilationManager::Shutdown();
            Test->TestFalse(TEXT("Drained Shutdown revokes all providers"), FModKitCompilationManager::HasProviders());
            Test->TestTrue(TEXT("Successful Shutdown leaves no cleanup identities"), SourceRemovalGenerations.IsEmpty());
            FString Error;
            Test->TestFalse(TEXT("Successful Shutdown cannot recreate cleanup identity"), FModKitCompilationManager::CanBeginRemoval(Source, Error));
            FModKitCompilationManager::Shutdown();
            Test->TestTrue(TEXT("Repeated Shutdown remains empty"), SourceRemovalGenerations.IsEmpty());
            return true;
        }
        if (Phase == 12)
        {
            FModKitRuntimeOps::RequestLoad(TEXT("H5Collection"));
            Phase = 13;
            return false;
        }
        if (Phase == 13)
        {
            if (FModKitRuntimeOps::IsOperationPending(TEXT("H5Collection"))) return false;
            Test->TestTrue(TEXT("Restore the startup cache set for following automation tests"),
                FModKitCompilationManager::HasCacheSet(TEXT("H5Collection")));
            return true;
        }
        if (Phase == 1)
        {
            if (!SourceRemovalGenerations.Contains(Key)) return false;
            Test->TestTrue(TEXT("Busy source cleanup stays queued"), FModKitRuntimeOps::IsOperationPending(Source));
            FModKitSourceOwner Owner;
            FString Error;
            const auto Module = Manager.GetModuleByModuleName(Generation.ModuleName);
            Test->TestTrue(TEXT("Failed source keeps its admitted owner"), Module.IsValid()
                && FModKitCompilationManager::GetGenerationOwner(*Module, Owner, Error));
            Test->TestFalse(TEXT("Failed source still rejects fresh admission"), FModKitModRegistry::IsSourceOwnerCurrent(Owner));
            FModKitRuntimeOps::RequestUnload(TEXT("H5Collection"));
            Phase = 2;
            return false;
        }
        if (Phase == 2)
        {
            if (FModKitRuntimeOps::IsOperationPending(TEXT("H5Collection"))) return false;
            if (!Test->TestTrue(TEXT("The last H5 set has really unloaded"), CacheSets.IsEmpty())) return true;
            EmptySince = Now;
            Phase = 3;
            return false;
        }
        if (Phase == 3)
        {
            const auto Module = Manager.GetModuleByModuleName(Generation.ModuleName);
            if (!Test->TestTrue(TEXT("Cache disappearance preserves the held source"), Module.IsValid()
                && Module->ScriptModule && Module->CoreGenerationId == Generation.Generation)) return true;
            if (!Test->TestTrue(TEXT("Source retry remains queued after the last cache leaves"), FModKitRuntimeOps::IsOperationPending(Source))) return true;
            const auto* Exact = SourceRemovalGenerations.Find(Key);
            if (!Test->TestTrue(TEXT("The cleanup retains its exact generation identity"), Exact && Exact->Num() == 1
                && (*Exact)[0].Generation == Generation.Generation && (*Exact)[0].ModuleName == Generation.ModuleName)) return true;
            if (Now - EmptySince < 0.8) return false;
            Test->TestTrue(TEXT("After multiple retry intervals the exact hold still returns Busy"), FModKitCompilationManager::RequestModuleRemoval(Source).bBusy);
            Test->TestEqual(TEXT("Busy preserves executable source"), Hold->Execute(), int(asEXECUTION_FINISHED));
            Test->TestEqual(TEXT("Held source still returns its value"), Hold->GetReturnDWord(), asDWORD(73));
            Hold->Unprepare(); Hold->Release(); Hold = nullptr;
            Phase = 4;
            return false;
        }
        if (FModKitRuntimeOps::IsOperationPending(Source)) return false;
        Test->TestFalse(TEXT("Successful retry clears the stable cleanup identity"), SourceRemovalGenerations.Contains(Key));
        Test->TestFalse(TEXT("Successful retry removes the source module"), Manager.GetModuleByModuleName(Generation.ModuleName).IsValid());
        Test->TestTrue(TEXT("Last cache remains unloaded"), CacheSets.IsEmpty());
        Test->TestEqual(TEXT("Source and cache module resources return to baseline"), Manager.GetActiveModules().Num(), Baseline);
        Phase = 12;
        Deadline = Now + 20.0;
        return false;
    }
private:
    FAutomationTestBase* Test;
    bool bLifecycle = false;
    bool bRevoke = false;
    asIScriptContext* Hold = nullptr;
    AngelscriptCoreExtensionPoints::FModuleGenerationId Generation;
    int32 Phase = 0, Baseline = 0, DrainFrames = 0, DrainAttempts = 0;
    double Deadline = 0, EmptySince = 0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitSourceRemovalRouteTest,
    "AngelscriptModKit.FeatureMods.SourceRemovalRoute",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitSourceRemovalRouteTest::RunTest(const FString&)
{
    AddExpectedError(TEXT("MODKIT_MOD_DISABLED name=H5TimeoutSource reason=script-timeout"), EAutomationExpectedErrorFlags::Contains, 1);
    ADD_LATENT_AUTOMATION_COMMAND(FModKitSourceRemovalRouteCommand(this));
    return true;
}

class FModKitSourceUserSequenceCommand final : public IAutomationLatentCommand
{
public:
    explicit FModKitSourceUserSequenceCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    bool Update() override
    {
        const FString Source = TEXT("H5TimeoutSource");
        const FString Cache = TEXT("H5Collection");
        if (Phase == 0)
        {
            Deadline = FPlatformTime::Seconds() + 40.0;
            if (!Test->TestTrue(TEXT("Source and cache start loaded"),
                FModKitModRegistry::IsModActivated(Source) && FModKitCompilationManager::HasCacheSet(Cache))) return true;
            FModKitRuntimeOps::RequestUnload(Cache);
            Phase = 1;
            return false;
        }
        if (FPlatformTime::Seconds() > Deadline) { Test->AddError(TEXT("User operation sequence timed out")); return true; }
        if (Phase == 1)
        {
            if (FModKitRuntimeOps::IsOperationPending(Cache)) return false;
            if (!Test->TestFalse(TEXT("Cache Mod unloaded"), FModKitCompilationManager::HasCacheSet(Cache))) return true;
            FModKitRuntimeOps::RequestUnload(Source);
            Phase = 2;
            return false;
        }
        if (Phase == 2)
        {
            if (FModKitRuntimeOps::IsOperationPending(Source)) return false;
            if (!Test->TestEqual(TEXT("Source Mod unloaded without cache"), FModKitRuntimeOps::GetOperationStatus(Source), FString(TEXT("Unloaded")))) return true;
            FModKitRuntimeOps::RequestLoad(Source);
            Phase = 3;
            return false;
        }
        if (Phase == 3)
        {
            if (FModKitRuntimeOps::IsOperationPending(Source)) return false;
            if (!Test->TestEqual(TEXT("Source Mod reloaded without cache"), FModKitRuntimeOps::GetOperationStatus(Source), FString(TEXT("Loaded")))) return true;
            FModKitRuntimeOps::RequestLoad(Cache);
            Phase = 4;
            return false;
        }
        if (Phase == 4)
        {
            if (FModKitRuntimeOps::IsOperationPending(Cache)) return false;
            if (!Test->TestTrue(TEXT("Cache Mod reloaded"), FModKitCompilationManager::HasCacheSet(Cache))) return true;
            FModKitRuntimeOps::RequestUnload(Source);
            Phase = 5;
            return false;
        }
        if (FModKitRuntimeOps::IsOperationPending(Source)) return false;
        if (Phase == 5)
        {
            if (!Test->TestEqual(TEXT("Reloaded source Mod unloads while cache Mod remains loaded"),
                FModKitRuntimeOps::GetOperationStatus(Source), FString(TEXT("Unloaded")))) return true;
            FModKitRuntimeOps::RequestLoad(Source);
            Phase = 6;
            return false;
        }
        Test->TestEqual(TEXT("Restore source Mod for subsequent tests"),
            FModKitRuntimeOps::GetOperationStatus(Source), FString(TEXT("Loaded")));
        return true;
    }
private:
    FAutomationTestBase* Test;
    int32 Phase = 0;
    double Deadline = 0.0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitSourceRemovalLifecycleTest,
    "AngelscriptModKit.FeatureMods.SourceRemovalLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitSourceRemovalLifecycleTest::RunTest(const FString&)
{
    AddExpectedError(TEXT("MODKIT_RUNTIME_OP_REJECTED op=unload name=H5TimeoutSource reason=busy"), EAutomationExpectedErrorFlags::Contains, 1);
    ADD_LATENT_AUTOMATION_COMMAND(FModKitSourceUserSequenceCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FModKitSourceRemovalRouteCommand(this, true));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitSourceRemovalProviderShutdownTest,
    "AngelscriptModKit.FeatureMods.SourceRemovalProviderShutdown",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitSourceRemovalProviderShutdownTest::RunTest(const FString&)
{
    AddExpectedError(TEXT("MODKIT_RUNTIME_OP_REJECTED op=unload name=H5TimeoutSource reason=busy"), EAutomationExpectedErrorFlags::Contains, 1);
    ADD_LATENT_AUTOMATION_COMMAND(FModKitSourceRemovalRouteCommand(this, true, true));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitH5ProducerTest,
    "AngelscriptModKit.FeatureMods.H5Producer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitH5ProducerTest::RunTest(const FString&)
{
    const FString Directory = TEXT("ModKitH5Produced_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString OutputRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / Directory);
    TArray<TSharedPtr<FJsonValue>> Units;
    for (const FString Id : {FString(TEXT("H5Collection")), FString(TEXT("H5Missing")), FString(TEXT("H5Tampered"))})
    {
        FString ScriptRoot, Relative;
        if (!TestTrue(TEXT("Producer uses a discovered FeatureMod"), FModKitModRegistry::GetModScriptPaths(Id, ScriptRoot, Relative))) return false;
        auto* Assemble = NewObject<UModKitAssembleCommandlet>();
        const FString Params = FString::Printf(TEXT("-Source=\"%s\" -Output=\"%s\" -CacheOnly -Signer=%s -PublicKey=%s"),
            *FPaths::GetPath(ScriptRoot), *(OutputRoot / Id), *UModKitSettings::Get()->PatchVerifyKeyId,
            *FModKitSignature::PublicKeyBase64());
        if (!TestEqual(TEXT("The production cache-only assembler succeeds"), Assemble->Main(Params), 0)) return false;
        FString ManifestText;
        const FString CacheManifest = OutputRoot / Id / Id / TEXT("Script/ModuleCaches/ModuleCaches.json");
        if (!TestTrue(TEXT("Producer writes the sealed manifest"), FFileHelper::LoadFileToString(ManifestText, *CacheManifest))) return false;
        TSharedPtr<FJsonObject> Manifest;
        const TArray<TSharedPtr<FJsonValue>>* Layouts = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
        if (!TestTrue(TEXT("The real native witness and two-member dependency collection are present"),
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestText), Manifest) && Manifest
            && Manifest->TryGetArrayField(TEXT("nativeTypeLayouts"), Layouts) && Layouts && !Layouts->IsEmpty()
            && Manifest->TryGetArrayField(TEXT("modules"), Modules) && Modules && Modules->Num() == 2)) return false;
        Units.Add(MakeShared<FJsonValueString>(Id));
    }
    auto Index = MakeShared<FJsonObject>();
    Index->SetStringField(TEXT("directory"), Directory);
    Index->SetArrayField(TEXT("units"), Units);
    FString IndexText;
    FJsonSerializer::Serialize(Index, TJsonWriterFactory<>::Create(&IndexText));
    TestTrue(TEXT("Export only a portable producer artifact location"), FFileHelper::SaveStringToFile(IndexText,
        *(FPaths::ProjectSavedDir() / TEXT("ModKitH5Produced.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitCachedProducerRejectionTest,
    "AngelscriptModKit.FeatureMods.CachedProducerRejection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitCachedProducerRejectionTest::RunTest(const FString&)
{
    const FString Id = TEXT("H5Collection");
    FString ScriptRoot, Relative;
    if (!TestTrue(TEXT("The cache-only FeatureMod is loaded"), FModKitCompilationManager::HasCacheSet(Id))
        || !TestTrue(TEXT("The cache-only FeatureMod has a registered root"),
            FModKitModRegistry::GetModScriptPaths(Id, ScriptRoot, Relative))) return false;
    const FString Output = FPaths::ProjectSavedDir() / (TEXT("ModKitCachedProducer_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Output, false, true); };
    auto* Assemble = NewObject<UModKitAssembleCommandlet>();
    const FString Params = FString::Printf(TEXT("-Source=\"%s\" -Output=\"%s\" -CacheOnly -Signer=%s -PublicKey=%s"),
        *FPaths::GetPath(ScriptRoot), *Output, *UModKitSettings::Get()->PatchVerifyKeyId,
        *FModKitSignature::PublicKeyBase64());
    AddExpectedError(TEXT("cache-owned modules cannot be serialized again"), EAutomationExpectedErrorFlags::Contains, 1);
    TestEqual(TEXT("Cache-only input is rejected by the production assembler"), Assemble->Main(Params), 1);
    TestFalse(TEXT("A rejected producer does not create output"), IFileManager::Get().DirectoryExists(*Output));
    TestTrue(TEXT("The original cache remains loaded"), FModKitCompilationManager::HasCacheSet(Id));
    return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitH5CollectionTest,
    "AngelscriptModKit.FeatureMods.H5Collection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitH5CollectionTest::RunTest(const FString&)
{
    using namespace AngelscriptCoreExtensionPoints;
    const FString Id = TEXT("H5Collection");
    auto& Manager = FAngelscriptManager::Get();
    if (!TestTrue(TEXT("Default startup loaded the signed FeatureMod H5 collection"), FModKitCompilationManager::HasCacheSet(Id))) return false;
    ON_SCOPE_EXIT { if (FModKitCompilationManager::HasCacheSet(Id)) FModKitCompilationManager::RequestModuleRemoval(Id); };
    for (const FString Rejected : {FString(TEXT("H5Missing")), FString(TEXT("H5Tampered"))})
    {
        TestFalse(TEXT("A signed missing/tampered native witness never commits"), FModKitCompilationManager::HasCacheSet(Rejected));
        FString RejectedRoot, RejectedRelative;
        if (!TestTrue(TEXT("Negative package was discovered"), FModKitModRegistry::GetModScriptPaths(Rejected, RejectedRoot, RejectedRelative))) return false;
        const auto* Verified = FModKitSignature::GetVerifiedPackageFiles(RejectedRoot);
        if (!TestTrue(TEXT("Negative witness belongs to an actually verified signature snapshot"), Verified != nullptr)) return false;
        FVerifiedModCacheSet RejectedSet;
        FString Reason;
        const bool bPrepared = BuildVerifiedCacheSet(*Verified, RejectedRoot / TEXT("ModuleCaches"), RejectedSet, Reason);
        if (Rejected == TEXT("H5Missing"))
        {
            TestFalse(TEXT("Missing witness rejects at the consumer snapshot boundary"), bPrepared);
            TestTrue(TEXT("Missing native witness is the actual rejection reason"), Reason.Contains(TEXT("nativeTypeLayouts")));
        }
        else
        {
            if (!TestTrue(TEXT("Tampered witness is structurally valid and authenticated"), bPrepared)) return false;
            AddExpectedError(TEXT("Cache native size/alignment/flags differ"), EAutomationExpectedErrorFlags::Contains, 2);
            AddExpectedError(TEXT("Hot reload failed due to script compile errors"), EAutomationExpectedErrorFlags::Contains, 1);
            FCacheLoadResult RejectedResult;
            TestTrue(TEXT("Core evaluates the signed tampered native witness"),
                TryLoadModuleCacheSet(CacheClient, RejectedSet.View(), RejectedResult) == ECacheAdmission::Completed);
            TestFalse(TEXT("Layout mismatch rejects before publication"), RejectedResult.bSucceeded || RejectedResult.bVmCommitted || RejectedResult.Set.Id);
        }
        for (const auto& Module : Manager.GetActiveModules())
            TestNull(TEXT("Rejected collection has no executable member"), Module->ScriptModule
                ? Module->ScriptModule->GetFunctionByDecl(TCHAR_TO_ANSI(*(TEXT("int ") + Rejected + TEXT("::ModKit") + Rejected + TEXT("Marker()")))) : nullptr);
    }
    FString Root, Relative;
    if (!TestTrue(TEXT("Fixture has its real registry root"), FModKitModRegistry::GetModScriptPaths(Id, Root, Relative))) return false;
    const auto* Snapshot = FModKitSignature::GetVerifiedPackageFiles(Root);
    const FString ManifestPath = SourcePath(Root / TEXT("ModuleCaches/ModuleCaches.json"));
    const auto* Manifest = Snapshot ? Snapshot->Find(ManifestPath) : nullptr;
    if (!TestTrue(TEXT("The exact manifest is part of the verified package"), Manifest != nullptr)) return false;
    const TArray<uint8> OriginalManifest = *Manifest;
    auto ReadMarker = [&]() -> uint64
    {
        FAngelscriptExecutionReadScope Reader;
        for (const auto& Module : Manager.GetActiveModules())
        {
            if (!FModKitCompilationManager::OwnsModule(Module, Id)) continue;
            auto* Function = Module->ScriptModule->GetFunctionByDecl("int H5Collection::ModKitH5CollectionMarker()");
            if (!Function) continue;
            auto* Context = Manager.GetScriptEngine()->CreateContext();
            ON_SCOPE_EXIT { Context->Release(); };
            TestTrue(TEXT("Prepare the cache's own executable function"), Context->Prepare(Function) >= 0);
            TestEqual(TEXT("The H5 collection executes"), Context->Execute(), int(asEXECUTION_FINISHED));
            TestEqual(TEXT("The native layout representative returns 31"), Context->GetReturnDWord(), asDWORD(31));
            return Module->CoreGenerationId;
        }
        AddError(TEXT("H5 collection has no marker function"));
        return 0;
    };
    const uint64 FirstGeneration = ReadMarker();
    const auto FirstSet = CacheSets.FindChecked(Id).Set.Id;
    const auto FirstRemoval = FModKitCompilationManager::RequestModuleRemoval(Id);
    TestTrue(TEXT("Public H5 removal drains the first collection"), FirstRemoval.bSuccess && !FModKitCompilationManager::HasCacheSet(Id));
    for (const auto& Name : FirstRemoval.CompiledModuleNames)
        TestFalse(TEXT("Removed members are absent"), Manager.GetModuleByModuleName(Name).IsValid());

    // Exercise H4 in the real ModKit call, preserving its production validator.
    TestTrue(TEXT("Temporarily remove the production H4 provider"), UnregisterModuleAdmission(ModuleAdmissionHandle) == ERemoveResult::Removed);
    ModuleAdmissionHandle = RegisterModuleAdmission([&](auto Modules, FString& Error)
    {
        if (!FModKitCompilationManager::ValidateModuleAdmission(Modules, Error)) return false;
        Error = TEXT("MODKIT_H5_H4_VETO");
        return false;
    });
    AddExpectedError(TEXT("MODKIT_H5_H4_VETO"), EAutomationExpectedErrorFlags::Contains, 2);
    AddExpectedError(TEXT("Hot reload failed due to script compile errors"), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedError(TEXT("MODKIT_CACHE_LOAD_FAILED"), EAutomationExpectedErrorFlags::Contains, 1);
    const int32 BeforeVeto = Manager.GetActiveModules().Num();
    const auto Vetoed = FModKitCompilationManager::RequestCompileModSet({Id});
    TestFalse(TEXT("H4 veto never publishes a cache set"), Vetoed.bSuccess || FModKitCompilationManager::HasCacheSet(Id));
    TestEqual(TEXT("H4 rollback preserves the active world"), Manager.GetActiveModules().Num(), BeforeVeto);
    TestTrue(TEXT("Remove only the test veto"), UnregisterModuleAdmission(ModuleAdmissionHandle) == ERemoveResult::Removed);
    ModuleAdmissionHandle = RegisterModuleAdmission(&FModKitCompilationManager::ValidateModuleAdmission);
    if (!TestTrue(TEXT("Restore the production H4 provider"), ModuleAdmissionHandle.IsValid())) return false;
    const auto Reloaded = FModKitCompilationManager::RequestCompileModSet({Id});
    if (!TestTrue(TEXT("Identical signed bytes reload after rollback/removal"), Reloaded.bSuccess)) return false;
    TestTrue(TEXT("Runtime set identity is fresh"), CacheSets.FindChecked(Id).Set.Id != FirstSet);
    TestTrue(TEXT("Constructor generation is fresh"), ReadMarker() != FirstGeneration);
    const auto* ReloadSnapshot = FModKitSignature::GetVerifiedPackageFiles(Root);
    TestTrue(TEXT("Reload uses the same authenticated manifest bytes"), ReloadSnapshot
        && ReloadSnapshot->FindChecked(ManifestPath) == OriginalManifest);
    TestTrue(TEXT("The reloaded collection removes"), FModKitCompilationManager::RequestModuleRemoval(Id).bSuccess);

    FModKitScriptExecutionTimeout::Shutdown();
    TestFalse(TEXT("Drained ModKit budget provider unregisters"), FModKitScriptExecutionTimeout::IsRetained());
    const auto WithoutBudget = FModKitCompilationManager::RequestCompileModSet({Id});
    if (!TestTrue(TEXT("The same signed collection runs after budget provider removal"), WithoutBudget.bSuccess)) return false;
    ReadMarker();
    TestTrue(TEXT("The collection removes with no ModKit budget provider"), FModKitCompilationManager::RequestModuleRemoval(Id).bSuccess);

    // This isolated consumer process ends after proving real owner revocation.
    if (TestTrue(TEXT("The drained production H5 owner unregisters"),
        UnregisterTransientCompileClient(CacheClient) == ERemoveResult::Removed))
    {
        CacheClient.Reset();
        AddExpectedError(TEXT("MODKIT_CACHE_LOAD_FAILED"), EAutomationExpectedErrorFlags::Contains, 1);
        TestFalse(TEXT("An unregistered owner cannot consume the signed collection"), FModKitCompilationManager::RequestCompileModSet({Id}).bSuccess);
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitSourceReloadWithCacheTest,
    "AngelscriptModKit.FeatureMods.SourceReloadWithCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitSourceReloadWithCacheTest::RunTest(const FString&)
{
    const FString Id = TEXT("BuiltInPackaging");
    const FString Source = FPaths::ProjectPluginsDir() / TEXT("FeatureMods/BuiltInPackaging/Script/BuiltInPackaging.as");
    FString Original;
    if (!TestTrue(TEXT("The independent signed cache is loaded"), FModKitCompilationManager::HasCacheSet(TEXT("H5Collection")))
        || !TestTrue(TEXT("The source FeatureMod is active"), FModKitModRegistry::IsModActivated(Id))
        || !TestTrue(TEXT("Read the source fixture"), FFileHelper::LoadFileToString(Original, *Source))) return false;
    const FString Updated = Original.Replace(TEXT("return 73;"), TEXT("return 74;"));
    if (!TestTrue(TEXT("The source fixture has its expected value"), Updated != Original)
        || !TestTrue(TEXT("Write the changed source"), FFileHelper::SaveStringToFile(Updated, *Source))) return false;
    ON_SCOPE_EXIT
    {
        TestTrue(TEXT("Restore the source fixture"), FFileHelper::SaveStringToFile(Original, *Source));
        TestTrue(TEXT("Restore the original source generation"), FModKitCompilationManager::RequestReplaceMod(Id).bSuccess);
    };
    const auto Result = FModKitCompilationManager::RequestReplaceMod(Id);
    if (!TestTrue(TEXT("Ordinary source reload succeeds while a cache is loaded"), Result.bSuccess)) return false;
    TestTrue(TEXT("The independent cache stays loaded"), FModKitCompilationManager::HasCacheSet(TEXT("H5Collection")));
    auto& Manager = FAngelscriptManager::Get();
    FAngelscriptExecutionReadScope Reader;
    for (const auto& Module : Manager.GetActiveModules())
        if (FModKitCompilationManager::OwnsModule(Module, Id))
            if (auto* Function = Module->ScriptModule->GetFunctionByDecl("int BuiltInPackaging::ModKitBuiltInPackagingMarker()"))
            {
                auto* Context = Manager.GetScriptEngine()->CreateContext();
                ON_SCOPE_EXIT { Context->Release(); };
                if (!TestTrue(TEXT("Prepare the reloaded source function"), Context->Prepare(Function) >= 0)) return false;
                TestEqual(TEXT("Execute the reloaded source"), Context->Execute(), int(asEXECUTION_FINISHED));
                TestEqual(TEXT("The changed source is visible"), Context->GetReturnDWord(), asDWORD(74));
                return !HasAnyErrors();
            }
    AddError(TEXT("The reloaded source function was not found"));
    return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitCacheLifecycleStressTest,
    "AngelscriptModKit.FeatureMods.CacheLifecycleStress",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitCacheLifecycleStressTest::RunTest(const FString&)
{
    const FString Id = TEXT("H5Collection");
    auto& Manager = FAngelscriptManager::Get();
    if (!TestTrue(TEXT("The normal startup path loaded the signed collection"), FModKitCompilationManager::HasCacheSet(Id))) return false;
    if (!TestTrue(TEXT("Remove the startup collection before measuring baseline"), FModKitCompilationManager::RequestModuleRemoval(Id).bSuccess)) return false;
    const int32 BaselineModules = Manager.GetActiveModules().Num();
    const int32 BaselineSets = CacheSets.Num();
    const int32 BaselineOwners = CacheOwners.Num();
    uint64 PreviousGeneration = 0;
    ON_SCOPE_EXIT { if (FModKitCompilationManager::HasCacheSet(Id)) FModKitCompilationManager::RequestModuleRemoval(Id); };
    for (int32 Cycle = 0; Cycle < 20; ++Cycle)
    {
        if (!TestTrue(TEXT("Load the same authenticated package repeatedly"), FModKitCompilationManager::RequestCompileModSet({Id}).bSuccess)) return false;
        TestEqual(TEXT("Each load adds exactly one cache set"), CacheSets.Num(), BaselineSets + 1);
        TestEqual(TEXT("Each load adds exactly two executable modules"), Manager.GetActiveModules().Num(), BaselineModules + 2);
        {
            FAngelscriptExecutionReadScope Reader;
            asIScriptFunction* Marker = nullptr;
            uint64 Generation = 0;
            for (const auto& Module : Manager.GetActiveModules())
                if (FModKitCompilationManager::OwnsModule(Module, Id))
                    if (auto* Function = Module->ScriptModule->GetFunctionByDecl("int H5Collection::ModKitH5CollectionMarker()"))
                    { Marker = Function; Generation = Module->CoreGenerationId; }
            if (!TestNotNull(TEXT("Each cycle exposes the real package function"), Marker)) return false;
            TestTrue(TEXT("Reloaded code has a fresh generation"), Generation && Generation != PreviousGeneration);
            PreviousGeneration = Generation;
            auto* Context = Manager.GetScriptEngine()->CreateContext();
            ON_SCOPE_EXIT { Context->Release(); };
            if (!TestTrue(TEXT("Prepare package function"), Context->Prepare(Marker) >= 0)) return false;
            TestEqual(TEXT("Package executes on every cycle"), Context->Execute(), int(asEXECUTION_FINISHED));
            TestEqual(TEXT("Package result survives repeated load and unload"), Context->GetReturnDWord(), asDWORD(31));
        }
        const auto Removed = FModKitCompilationManager::RequestModuleRemoval(Id);
        if (!TestTrue(TEXT("Unload drains the collection on every cycle"), Removed.bSuccess)) return false;
        TestEqual(TEXT("Executable modules return to baseline"), Manager.GetActiveModules().Num(), BaselineModules);
        TestEqual(TEXT("Cache sets return to baseline"), CacheSets.Num(), BaselineSets);
        TestEqual(TEXT("Cache owner registrations return to baseline"), CacheOwners.Num(), BaselineOwners);
        for (const auto& Name : Removed.CompiledModuleNames)
            TestFalse(TEXT("Removed code is no longer reachable by module lookup"), Manager.GetModuleByModuleName(Name).IsValid());
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitBuiltInPackagingTest,
    "AngelscriptModKit.FeatureMods.BuiltInPackaging",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitBuiltInPackagingTest::RunTest(const FString&)
{
    const FString Id = TEXT("BuiltInPackaging");
    if (!TestTrue(TEXT("The built-in FeatureMod activates without an experimental switch"), FModKitModRegistry::IsModActivated(Id))) return false;
    TestTrue(TEXT("The published unit retains built-in trust"), FModKitModRegistry::GetModTrust(Id) == EModKitModTrust::BuiltIn);
    FString Root, Relative;
    if (!TestTrue(TEXT("The packaged unit has a discoverable script root"), FModKitModRegistry::GetModScriptPaths(Id, Root, Relative))) return false;
#if !WITH_EDITOR
    TestFalse(TEXT("Packaged metadata is read from the container, not a loose trusted override"),
        FPlatformFileManager::Get().GetPlatformPhysical().FileExists(*(FPaths::GetPath(Root) / TEXT("FeatureMod.json"))));
    const FString LooseSource = Root / TEXT("LooseOverride.as");
    if (!TestTrue(TEXT("Create the physical override directory"),
        FPlatformFileManager::Get().GetPlatformPhysical().CreateDirectoryTree(*Root))) return false;
    if (!TestTrue(TEXT("Create a loose source inside the packaged unit's directory"),
        FFileHelper::SaveStringToFile(TEXT("int InjectedBuiltIn() { return 99; }"), *LooseSource))) return false;
    ON_SCOPE_EXIT { FPlatformFileManager::Get().GetPlatformPhysical().DeleteFile(*LooseSource); };
    FString Reason;
    TestFalse(TEXT("Loose additions cannot borrow built-in permissions"), FModKitSignature::CheckLoadedSource(
        LooseSource, Relative / TEXT("LooseOverride.as"), TEXT("int InjectedBuiltIn() { return 99; }"), false, Reason));
    TestTrue(TEXT("The rejection explains the trust boundary"), Reason.Contains(TEXT("loose source")));
#endif
    FAngelscriptExecutionReadScope Reader;
    auto& Manager = FAngelscriptManager::Get();
    for (const auto& Module : Manager.GetActiveModules())
        if (FModKitCompilationManager::OwnsModule(Module, Id))
            if (auto* Function = Module->ScriptModule->GetFunctionByDecl("int BuiltInPackaging::ModKitBuiltInPackagingMarker()"))
            {
                auto* Context = Manager.GetScriptEngine()->CreateContext();
                ON_SCOPE_EXIT { Context->Release(); };
                if (!TestTrue(TEXT("Prepare the automatically loaded built-in function"), Context->Prepare(Function) >= 0)) return false;
                TestEqual(TEXT("Published built-in code executes"), Context->Execute(), int(asEXECUTION_FINISHED));
                TestEqual(TEXT("Published built-in code returns its expected value"), Context->GetReturnDWord(), asDWORD(73));
                return !HasAnyErrors();
            }
    AddError(TEXT("The built-in FeatureMod was discovered but its script did not become available"));
    return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitVerifiedCacheSnapshotTest,
    "AngelscriptModKit.FeatureMods.VerifiedCacheSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitVerifiedCacheSnapshotTest::RunTest(const FString&)
{
    const FString Root = FPaths::ProjectSavedDir() / TEXT("ModKitCacheSnapshotTest/Script/ModuleCaches");
    const FString Header = TEXT("{\"schemaVersion\":1,\"format\":\"soloas-module-cache-set-v1\",\"buildIdentity\":\"test\",\"configurationIdentity\":\"258\",\"setGuid\":\"12345678123456781234567812345678\",\"modules\":[{\"file\":\"A.Cache\",\"contentIdentity\":\"blake3:untrusted\"}]");
    auto Check = [&](const FString& Text, const TCHAR* Expected)
    {
        FTCHARToUTF8 Encoded(*Text);
        TArray<uint8> Bytes;
        Bytes.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
        TMap<FString, TArray<uint8>> Snapshot;
        Snapshot.Add(SourcePath(Root / TEXT("ModuleCaches.json")), MoveTemp(Bytes));
        FVerifiedModCacheSet Set;
        FString Error;
        TestFalse(TEXT("Malformed or incomplete signed snapshots never produce a view"),
            BuildVerifiedCacheSet(Snapshot, Root, Set, Error));
        TestEqual(TEXT("Snapshot rejects at its intended boundary"), Error, FString(Expected));
    };
    Check(Header + TEXT("}"), TEXT("cache-set-manifest-or-nativeTypeLayouts-missing"));
    Check(Header + TEXT(",\"nativeTypeLayouts\":null}"), TEXT("cache-set-manifest-or-nativeTypeLayouts-missing"));
    Check(Header + TEXT(",\"nativeTypeLayouts\":[]}"), TEXT("cache-member-not-in-signed-snapshot"));
    Check(Header.Replace(TEXT("A.Cache"), TEXT("modules/A.Cache")) + TEXT(",\"nativeTypeLayouts\":[]}"), TEXT("cache-member-not-in-signed-snapshot"));
    Check(Header.Replace(TEXT("A.Cache"), TEXT("../A.Cache")) + TEXT(",\"nativeTypeLayouts\":[]}"), TEXT("cache-set-file-invalid"));
    Check(Header.Replace(TEXT("soloas-module-cache-set-v1"), TEXT("legacy-cache")) + TEXT(",\"nativeTypeLayouts\":[]}"), TEXT("cache-set-manifest-or-nativeTypeLayouts-missing"));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitCompileSurfaceUnavailableTest,
	"AngelscriptModKit.FeatureMods.CompileSurfaceUnavailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitCompileSurfaceUnavailableTest::RunTest(const FString&)
{
	auto& Manager = FAngelscriptManager::Get();
	const int32 PreviousModules = Manager.GetActiveModules().Num();
	TGuardValue<FDelegateHandle> MissingClient(CacheClient, FDelegateHandle());
	TArray<TSharedRef<FAngelscriptModuleDesc>> Candidates {MakeShared<FAngelscriptModuleDesc>()}, Compiled;
	Candidates[0]->ModuleName = TEXT("ModKitMissingCompileClient");
	AddExpectedError(TEXT("MODKIT_APPEND_FAILED reason=compile-surface-unavailable"), EAutomationExpectedErrorFlags::Contains, 1);
	TestEqual(TEXT("An unregistered client rejects source compilation before touching active modules"),
		FModKitCompilationManager::CompileOwnedSourceModules(ECompileType::FullReload, Candidates, Compiled), ECompileResult::Error);
	TestEqual(TEXT("The rejected request leaves active modules unchanged"), Manager.GetActiveModules().Num(), PreviousModules);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitStartupVisibilityTest,
    "AngelscriptModKit.FeatureMods.StartupVisibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitStartupVisibilityTest::RunTest(const FString&)
{
    TSharedPtr<FAngelscriptModuleDesc> Module;
    const auto Class = FAngelscriptManager::Get().GetClass(TEXT("ModKitStartupVisibility::UModKitStartupVisibility"), &Module);
    if (!TestTrue(TEXT("Startup FeatureMod is compiled before automation begins"), Class.IsValid() && Class->Class != nullptr && Module.IsValid())) return false;
    FModKitSourceOwner Owner;
    FString Error;
    TestTrue(TEXT("Startup compilation retains its discovered leaf identity"),
        FModKitCompilationManager::GetSourceOwner(Module.ToSharedRef(), Owner, Error)
        && Owner.Layer == EModKitSourceLayer::Leaf && Owner.Id == TEXT("ModKitStartupVisibility"));
    TestTrue(TEXT("The startup FeatureMod is in the active roster"), FModKitModRegistry::ShouldCompileMod(Owner.Id));
    const auto* Value = FindFProperty<FIntProperty>(Class->Class, TEXT("Value"));
    if (!TestNotNull(TEXT("Startup reflection is published"), Value)) return false;
    TestEqual(TEXT("Startup reflection default is preserved"), Value->GetPropertyValue_InContainer(Class->Class->GetDefaultObject()), 91);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitTransientCompileSurfaceTest,
    "AngelscriptModKit.FeatureMods.TransientCompileSurface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitTransientCompileSurfaceTest::RunTest(const FString&)
{
    using namespace AngelscriptCoreExtensionPoints;
    TArray<TSharedRef<FAngelscriptModuleDesc>> CachedModules;
    for (const auto& Set : CacheSets)
        for (const auto& Generation : Set.Value.CommittedModules)
            if (const auto Module = FAngelscriptManager::Get().GetModule(Generation.ModuleName))
                CachedModules.Add(Module.ToSharedRef());
    TestFalse(TEXT("Late registration cannot capture an initialized VM"),
        RegisterTransientCompileClient(FName(TEXT("ModKitLateTest"))).IsValid());
    FString Error;
    FCompileSurfaceHandle BeforeRegistration;
    if (!TestTrue(TEXT("Compile surface is available before late native registration"),
        AcquireTransientCompileSurface(CacheClient, BeforeRegistration, Error) == ECompileSurfaceAdmission::Ready)) return false;
    if (!TestTrue(TEXT("Release the surface before native registration"),
        ReleaseTransientCompileSurface(CacheClient, BeforeRegistration) == ERemoveResult::Removed)) return false;
    // The VM owns this enum until exit. Run this test in a separate process.
    const FString EnumName = TEXT("ModKitLateNative_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    auto* Engine = FAngelscriptManager::Get().GetScriptEngine();
    if (!TestTrue(TEXT("Register a native declaration after startup capture"),
        Engine->RegisterEnum(TCHAR_TO_ANSI(*EnumName)) >= 0)) return false;
    if (!TestTrue(TEXT("Complete the late native enum declaration"),
        Engine->RegisterEnumValue(TCHAR_TO_ANSI(*EnumName), "Ready", 1) >= 0)) return false;
    const FString Id = TEXT("ModKitH6Probe");
    const FString Root = FPaths::ProjectPluginsDir() / TEXT("FeatureMods") / Id;
    const FString Relative = TEXT("Plugins/FeatureMods/") + Id + TEXT("/Script/Probe.as");
    const FString File = Root / TEXT("Script/Probe.as");
    if (!TestFalse(TEXT("Never overwrite an existing FeatureMod"), IFileManager::Get().DirectoryExists(*Root))) return false;
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Root, false, true); };
    if (!TestTrue(TEXT("Create project-owned FeatureMod"), IFileManager::Get().MakeDirectory(*(Root / TEXT("Script")), true))) return false;
    if (!TestTrue(TEXT("Write descriptor"), FFileHelper::SaveStringToFile(
        TEXT("{\"FileVersion\":3,\"FeatureMod\":true,\"ExplicitlyLoaded\":true,\"CanContainContent\":false,\"BuiltInInitialFeatureState\":\"Installed\",\"Modules\":[],\"Plugins\":[{\"Name\":\"GameFeatures\",\"Enabled\":true}]}"),
        *(Root / (Id + TEXT(".uplugin")))))) return false;
    if (!TestTrue(TEXT("Write configuration"), FFileHelper::SaveStringToFile(
        TEXT("{\"schemaVersion\":1,\"version\":\"1.0.0\",\"dependencies\":{\"trunkModules\":[],\"branches\":[],\"featureMods\":[]},\"BlueprintBindings\":{},\"BlueprintDependencies\":[],\"requiredWhitelistCapabilities\":[]}"),
        *(Root / TEXT("FeatureMod.json"))))) return false;
    if (!TestTrue(TEXT("Write reflected source"), FFileHelper::SaveStringToFile(
        TEXT("class UModKitH6Receiver : UObject { UPROPERTY() int Value = 73; UFUNCTION() int Read() const { return Value; } }"), *File))) return false;
    if (!TestTrue(TEXT("Discover the actual FeatureMod"), FModKitModRegistry::RescanOrAdd(Id))) return false;
    FAngelscriptPreprocessor Processor;
    Processor.AddFile(Relative, File);
    if (!TestTrue(TEXT("Preprocess through production source admission"), Processor.Preprocess())) return false;
    const auto Candidates = Processor.GetModulesToCompile();
    if (!TestEqual(TEXT("One FeatureMod module"), Candidates.Num(), 1)) return false;
    FModKitSourceOwner Owner;
    if (!TestTrue(TEXT("Candidate has its real leaf owner"), FModKitCompilationManager::GetSourceOwner(Candidates[0], Owner, Error))) return false;
    TestTrue(TEXT("Source identity is a leaf"), Owner.Layer == EModKitSourceLayer::Leaf && Owner.Id == Id);
    TArray<TSharedRef<FAngelscriptModuleDesc>> Compiled;
    if (!TestEqual(TEXT("H6 appends the new FeatureMod after native declarations change"),
        FModKitCompilationManager::CompileOwnedSourceModules(ECompileType::FullReload, Candidates, Compiled), ECompileResult::FullyHandled)) return false;
    auto& Manager = FAngelscriptManager::Get();
    for (const auto& Cached : CachedModules)
    {
        TestTrue(TEXT("Independent source append preserves the loaded cache module"),
            Manager.GetModule(Cached->ModuleName) == Cached);
        auto Replacement = MakeShared<FAngelscriptModuleDesc>();
        Replacement->ModuleName = Cached->ModuleName;
        TArray<TSharedRef<FAngelscriptModuleDesc>> Rejected;
        AddExpectedError(TEXT("Cache-owned module must be removed"), EAutomationExpectedErrorFlags::Contains, 1);
        TestEqual(TEXT("Source cannot replace a cache-owned module"),
            FModKitCompilationManager::CompileOwnedSourceModules(ECompileType::FullReload, {Replacement}, Rejected), ECompileResult::Error);
        TestTrue(TEXT("Rejected replacement preserves the cache module"), Manager.GetModule(Cached->ModuleName) == Cached);
    }
    const auto Class = Manager.GetClass(Id + TEXT("::UModKitH6Receiver"));
    if (!TestTrue(TEXT("Manager publishes the reflected class"), Class.IsValid() && Class->Class != nullptr)) return false;
    UObject* Instance = NewObject<UObject>(GetTransientPackage(), Class->Class);
    auto* Value = FindFProperty<FIntProperty>(Class->Class, TEXT("Value"));
    if (!TestNotNull(TEXT("UPROPERTY is reflected"), Value)) return false;
    TestEqual(TEXT("Reflected default is initialized"), Value->GetPropertyValue_InContainer(Instance), 73);
    auto* Read = Instance->FindFunction(TEXT("Read"));
    if (!TestNotNull(TEXT("UFUNCTION is reflected"), Read)) return false;
    struct { int32 ReturnValue = 0; } Params;
    Instance->ProcessEvent(Read, &Params);
    TestEqual(TEXT("Published FeatureMod function executes"), Params.ReturnValue, 73);
    FCompileSurfaceHandle Surface;
    const auto Acquired = AcquireTransientCompileSurface(CacheClient, Surface, Error);
    TestTrue(TEXT("Production compile released its surface"), Acquired == ECompileSurfaceAdmission::Ready);
    if (Acquired == ECompileSurfaceAdmission::Ready)
    {
        const auto Released = ReleaseTransientCompileSurface(CacheClient, Surface);
        TestTrue(TEXT("Release the verification surface"), Released == ERemoveResult::Removed);
    }
    if (!CachedModules.IsEmpty())
        TestTrue(TEXT("Live receiver delays cache-mode source removal"),
            FModKitCompilationManager::RequestModuleRemoval(Id).bBusy);
    Instance = nullptr;
    CollectGarbage(RF_NoFlags);
    const auto Removed = FModKitCompilationManager::RequestModuleRemoval(Id);
    TestTrue(TEXT("Ordinary source removal revokes the appended FeatureMod"), Removed.bSuccess);
    const auto Remaining = Manager.GetModule(Candidates[0]->ModuleName);
    TestTrue(TEXT("Revoked module has no remaining script content"), !Remaining.IsValid()
        || FModKitCompilationManager::IsEmptyPlaceholderModule(Remaining->ModuleName));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitSourceOwnershipBoundaryTest,
    "AngelscriptModKit.FeatureMods.SourceOwnershipBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitSourceOwnershipBoundaryTest::RunTest(const FString&)
{
    const FString Unique = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Relative = TEXT("ModKitOutside_") + Unique + TEXT(".as");
    const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / Relative);
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*File); };
    if (!TestTrue(TEXT("Write native temporary source under Saved"), FFileHelper::SaveStringToFile(
        TEXT("int ModKitOutsideValue() { return 11; }"), *File))) return false;
    FAngelscriptPreprocessor Processor;
    Processor.AddFile(Relative, File);
    if (!TestTrue(TEXT("The core accepts a non-Mod temporary source with ModKit enabled"), Processor.Preprocess())) return false;
    const auto Modules = Processor.GetModulesToCompile();
    if (!TestEqual(TEXT("One native temporary candidate"), Modules.Num(), 1)) return false;
    TestFalse(TEXT("ModKit does not claim its source or H4 policy"), FModKitCompilationManager::RequiresModAdmission(*Modules[0]));
    TArray<TSharedRef<FAngelscriptModuleDesc>> Compiled;
    TestEqual(TEXT("The ordinary core compile is preserved"), FAngelscriptManager::Get().CompileModules(
        ECompileType::FullReload, Modules, Compiled), ECompileResult::FullyHandled);
    {
        FAngelscriptExecutionReadScope Reader;
        auto* Function = Modules[0]->ScriptModule->GetFunctionByName("ModKitOutsideValue");
        auto* Context = FAngelscriptManager::Get().GetScriptEngine()->CreateContext();
        ON_SCOPE_EXIT { Context->Release(); };
        if (!TestNotNull(TEXT("The temporary native caller has an executable function"), Function)) return false;
        TestTrue(TEXT("Prepare ordinary native temporary code"), Context->Prepare(Function) >= 0);
        TestEqual(TEXT("Run ordinary native temporary code"), Context->Execute(), int(asEXECUTION_FINISHED));
        TestEqual(TEXT("Temporary source keeps its expected behavior"), Context->GetReturnDWord(), asDWORD(11));
    }
    FString Error;
    const FString ModRelative = TEXT("FeatureMods/Unregistered_") + Unique + TEXT("/Script/Injected.as");
    const FString ModFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / ModRelative);
    TestFalse(TEXT("Unregistered content under a protected Mod root still rejects"),
        FModKitCompilationManager::AdmitSource(ModFile, ModRelative, false, Error));
    TestTrue(TEXT("The rejection names the protected Mod root"), Error.Contains(TEXT("FeatureMod root")));
    Error.Reset();
    TestFalse(TEXT("A declared Mod source cannot escape into Saved"),
        FModKitCompilationManager::AdmitSource(File, ModRelative, false, Error));
    TestTrue(TEXT("Root escape is diagnosed"), Error.Contains(TEXT("escaped")));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitModuleAdmissionTest,
	"AngelscriptModKit.FeatureMods.ModuleAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FModKitModuleAdmissionTest::RunTest(const FString&)
{
	const FString Relative = TEXT("Automation/ModKitAdmission_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".as");
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Script") / Relative);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*File); };
	if (!TestTrue(TEXT("Write host source"), FFileHelper::SaveStringToFile(
		TEXT("int AdmissionValue() { return 7; }"), *File))) return false;
	FAngelscriptPreprocessor Processor;
	Processor.AddFile(Relative, File);
	if (!TestTrue(TEXT("Host source follows ordinary preprocessing"), Processor.Preprocess())) return false;
	const auto Candidates = Processor.GetModulesToCompile();
	if (!TestEqual(TEXT("One real candidate"), Candidates.Num(), 1)) return false;
	FModKitSourceOwner Owner;
	FString Error;
	TestFalse(TEXT("ModKit does not claim non-Mod source ownership"), FModKitCompilationManager::GetSourceOwner(Candidates[0], Owner, Error));
	TArray<TSharedRef<FAngelscriptModuleDesc>> Compiled;
	auto& Manager = FAngelscriptManager::Get();
	TestEqual(TEXT("Ordinary host compile succeeds with ModKit"), Manager.CompileModules(
		ECompileType::FullReload, Candidates, Compiled), ECompileResult::FullyHandled);
	const auto Forge = [&]()
	{
		auto Module = MakeShared<FAngelscriptModuleDesc>();
		Module->ModuleName = Candidates[0]->ModuleName;
		Module->Code = Candidates[0]->Code;
		Module->Code[0].Code.ReplaceInline(TEXT("return 7"), TEXT("return 13"));
		return TArray<TSharedRef<FAngelscriptModuleDesc>>{Module};
	};
	Compiled.Reset();
	TestEqual(TEXT("Non-Mod replacement keeps ordinary core behavior"), Manager.CompileModules(
		ECompileType::FullReload, Forge(), Compiled), ECompileResult::FullyHandled);
	const auto Preserved = Manager.GetModule(Candidates[0]->ModuleName);
	auto OwnedForgery = Forge();
	OwnedForgery[0]->Code[0].AbsoluteFilename = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("FeatureMods/UnregisteredAdmission/Script/Injected.as"));
	OwnedForgery[0]->Code[0].RelativeFilename = TEXT("FeatureMods/UnregisteredAdmission/Script/Injected.as");
	OwnedForgery[0]->Code[0].Code += TEXT(" int AdmissionRejectedInit() { Error(\"modkit-admission-global-executed\"); return 13; } const int AdmissionRejectedGlobal = AdmissionRejectedInit();");
	AddExpectedError(TEXT("MODKIT_SOURCE_OWNER unbound candidate"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("Hot reload failed due to script compile errors"), EAutomationExpectedErrorFlags::Contains, 1);
	Compiled.Reset();
	TestEqual(TEXT("Changed unbound candidate is refused before globals"), Manager.CompileModules(
		ECompileType::FullReload, OwnedForgery, Compiled), ECompileResult::Error);
	TestTrue(TEXT("Refusal preserves the active object"), Manager.GetModule(Candidates[0]->ModuleName) == Preserved);
	TestTrue(TEXT("Remove the actual ModKit provider"), AngelscriptCoreExtensionPoints::UnregisterModuleAdmission(ModuleAdmissionHandle)
		== AngelscriptCoreExtensionPoints::ERemoveResult::Removed);
	ModuleAdmissionHandle.Reset();
	ON_SCOPE_EXIT
	{
		ModuleAdmissionHandle = AngelscriptCoreExtensionPoints::RegisterModuleAdmission(&FModKitCompilationManager::ValidateModuleAdmission);
		TestTrue(TEXT("Restore the actual ModKit provider"), ModuleAdmissionHandle.IsValid());
	};
	Compiled.Reset();
	TestEqual(TEXT("Removing ModKit admission restores ordinary compilation"), Manager.CompileModules(
		ECompileType::FullReload, Forge(), Compiled), ECompileResult::FullyHandled);
	return !HasAnyErrors();
}


#endif
