// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.

#include "AngelscriptScriptUnit.h"

#include "Extensions/AngelscriptModdingInternal.h"
#include "Extensions/AngelscriptExtensionPipelineInternal.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectDescriptor.h"
#include "Modules/ModuleManager.h"
#include "PluginDescriptor.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AngelscriptManager.h"
#include "AngelscriptExecutionQuiescence.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "UObject/Class.h"

                                                                                                    
void FAngelscriptManager::DrainDeferredSourceCompilations()
{
	if (DeferredSourceCompilations.IsEmpty() || bDrainingDeferredSourceCompilations
		|| bIsCompilingModules || IsAngelscriptExecutionShutdownStarted()) return;
	FAngelscriptExecutionWriteScope Boundary(FAngelscriptExecutionWriteScope::EAcquire::Try);
	if (!Boundary.IsAcquired()) return;
	TGuardValue<bool> Draining(bDrainingDeferredSourceCompilations, true);
	auto Requests = MoveTemp(DeferredSourceCompilations);
	DeferredSourceCompilations.Reset();
	for (const auto& Request : Requests)
	{
		TArray<TSharedRef<FAngelscriptModuleDesc>> Compiled;
		const ECompileResult Result = CompileModules(Request.CompileType, Request.Modules, Compiled, Request.Units);
		if (Result != ECompileResult::FullyHandled && Result != ECompileResult::PartiallyHandled)
			UE_LOG(Angelscript, Warning, TEXT("Deferred source reload failed; inspect compilation diagnostics."));
	}
}

                                                                                                  

ECompileResult FAngelscriptManager::CompileModules(ECompileType CompileType,
	const TArray<TSharedRef<FAngelscriptModuleDesc>>& InputModules,
	TArray<TSharedRef<FAngelscriptModuleDesc>>& OutCompiledModules)
{
	FAngelscriptScriptUnitManager Units;
	Units.Discover();
	return CompileModules(CompileType, InputModules, OutCompiledModules, Units);
}

ECompileResult FAngelscriptManager::CompileModules(ECompileType CompileType,
	const TArray<TSharedRef<FAngelscriptModuleDesc>>& InputModules,
	TArray<TSharedRef<FAngelscriptModuleDesc>>& OutCompiledModules,
	const FAngelscriptScriptUnitManager& SourceUnits)
{
                                                                                                        
	check(IsInGameThread());
	TUniquePtr<FAngelscriptExecutionWriteScope> Admission;
	if (CompileType != ECompileType::Initial)
	{
		// Compilation callbacks cannot start a second transaction.
		if (bIsCompilingModules || IsAngelscriptExecutionShutdownStarted())
			return ECompileResult::Error;
		Admission = MakeUnique<FAngelscriptExecutionWriteScope>(FAngelscriptExecutionWriteScope::EAcquire::Try);
		if (!Admission->IsAcquired()
			|| (!bDrainingDeferredSourceCompilations && !DeferredSourceCompilations.IsEmpty()))
		{
			// The explicit transaction owns its reservation and retries on its reload tick.
			if (bForcedFullReloadExecution) return ECompileResult::Deferred;
			// Cache data is borrowed from the synchronous caller and cannot outlive it.
			for (const auto& Module : InputModules)
				if (Module->PrecompiledData || Module->PrecompiledDataContext || Module->ScriptModule)
					return ECompileResult::Error;
			DeferredSourceCompilations.Add({CompileType, InputModules, SourceUnits});
			OutCompiledModules.Reset();
			return ECompileResult::Deferred;
		}
	}

                                                                                                      
	FAngelscriptScriptUnitManager Units = SourceUnits;
	const auto PreviousModules = GetActiveModules();
	// Keep the current target's discovered dependency graph; only fill absent units from cache.
	if (GIsEditor)
	{
		for (const auto& Module : PreviousModules)
			if (Units.Find(Module->ScriptUnit.Id) == nullptr)
				Units.AddUnit(Module->ScriptUnit);
	}
	else
	{
		for (const auto& Module : PreviousModules)
			if (const FAngelscriptScriptUnit* CurrentUnit = Units.Find(Module->ScriptUnit.Id))
				Module->ScriptUnit.Dependencies = CurrentUnit->Dependencies;
	}
	TSet<FString> Changed;
	TMap<FString, TArray<TSharedRef<FAngelscriptModuleDesc>>> Pending;
	TSet<FString> RequestedModules;
	for (const auto& Module : InputModules)
	{
		if (const FAngelscriptScriptUnit* CurrentUnit = Units.Find(Module->ScriptUnit.Id))
			Module->ScriptUnit.Dependencies = CurrentUnit->Dependencies;
		if (Units.Find(Module->ScriptUnit.Id) == nullptr && (GIsEditor || Module->bNeedsSourcePreprocess))
			Units.AddUnit(Module->ScriptUnit);
		Changed.Add(Module->ScriptUnit.Id);
		Pending.FindOrAdd(Module->ScriptUnit.Id).Add(Module);
		RequestedModules.Add(Module->ModuleName);
	}
	TArray<FString> Order;
	TMap<FString, FString> Errors;
	Units.BuildOrder(Order, Errors);
    if (CompileType != ECompileType::Initial
        && IsAngelscriptExtensionStageRegistered(EAngelscriptExtensionStage::TypeGeneration))
    {
        FAngelscriptTypeGenerationContext Context;
        Context.Point = EAngelscriptTypeGenerationPoint::SelectModuleTransaction;
        Context.Modules = &InputModules;
        FString Error;
        if (!DispatchAngelscriptTypeGeneration(Context, Error))
        {
            UE_LOG(Angelscript, Error, TEXT("SCRIPT_EXTENSION_TRANSACTION_REJECTED before_swap=1 reason=%s"), *Error);
            return ECompileResult::Error;
        }
        if (Context.bCompileDependencyClosure)
        {
            OutCompiledModules.Reset();
            TMap<FString, FString> TypeOwners;
            auto OwnType = [&](const FString& Name, const FString& Unit)
            {
                const FString* Previous = TypeOwners.Find(Name);
                if (Previous && *Previous != Unit) return false;
                TypeOwners.Add(Name, Unit);
                return true;
            };
            for (const auto& Module : InputModules)
            {
                bool bUnique = true;
                for (const auto& Class : Module->Classes) bUnique &= OwnType(Class->ClassName, Module->ScriptUnit.Id);
                for (const auto& Enum : Module->Enums) bUnique &= OwnType(Enum->EnumName, Module->ScriptUnit.Id);
                for (const auto& Delegate : Module->Delegates) bUnique &= OwnType(Delegate->DelegateName, Module->ScriptUnit.Id);
                if (!bUnique)
                {
                    UE_LOG(Angelscript, Error, TEXT("SCRIPT_EXTENSION_TRANSACTION_AMBIGUOUS_TYPE before_swap=1"));
                    return ECompileResult::Error;
                }
            }
            // The provider supplies original source for every module in each
            // participating unit. Never fall through to per-unit partial commits.
            for (const auto& Module : InputModules)
                if (Errors.Contains(Module->ScriptUnit.Id) || Module->bCompileError || Module->bNeedsSourcePreprocess)
                    return ECompileResult::Error;
            for (const auto& Previous : PreviousModules)
                if (!Previous->bTreatAsDeleted && Changed.Contains(Previous->ScriptUnit.Id)
                    && !RequestedModules.Contains(Previous->ModuleName))
                {
                    UE_LOG(Angelscript, Error, TEXT("SCRIPT_EXTENSION_TRANSACTION_INCOMPLETE before_swap=1 module=%s"), *Previous->ModuleName);
                    return ECompileResult::Error;
                }
            return InputModules.IsEmpty() ? ECompileResult::Error
                : CompileScriptUnit(CompileType, InputModules, OutCompiledModules, Units);
        }
    }
	for (const auto& Module : InputModules)
		if (Module->bCompileError) Errors.Add(Module->ScriptUnit.Id, TEXT("Script unit preprocessing failed"));
	const auto Affected = CompileType == ECompileType::Initial ? Changed : Units.GetDependents(Changed);
	for (const auto& Module : PreviousModules)
		if (!Module->bTreatAsDeleted && Affected.Contains(Module->ScriptUnit.Id) && !RequestedModules.Contains(Module->ModuleName))
			Pending.FindOrAdd(Module->ScriptUnit.Id);

	bool bFailed = false;
	bool bSucceeded = false;
	bool bNeedsFullReload = false;
	bool bNativeFailed = false;
	bool bPartial = false;
	OutCompiledModules.Reset();
	auto Reject = [&](const FString& Id, FString Error)
	{
		bFailed = true;
		Errors.Add(Id, Error);
		for (const auto& Module : Pending.FindChecked(Id))
		{
			Module->bCompileError = true;
			FDiagnostic Diagnostic;
			Diagnostic.Message = Error;
			Diagnostic.Row = Diagnostic.Column = 1;
			Diagnostic.bIsError = true;
			Diagnostic.bIsInfo = false;
			ScriptCompileError(Module->Code.IsEmpty() ? Module->ModuleName : Module->Code[0].AbsoluteFilename, Diagnostic);
			if (CompileType != ECompileType::Initial)
				for (const auto& Section : Module->Code)
					PreviouslyFailedReloadFiles.Add(FFilenamePair{Section.AbsoluteFilename, Section.RelativeFilename});
		}
	};
	for (const auto& Pair : Pending)
		if (const auto* Error = Errors.Find(Pair.Key)) Reject(Pair.Key, *Error);
	for (const FString& Id : Order)
	{
		auto* Modules = Pending.Find(Id);
		if (Modules == nullptr || Errors.Contains(Id)) continue;
		FString FailedDependency;
		for (const FString& Dependency : Units.Find(Id)->Dependencies)
			if (Errors.Contains(Dependency)) { FailedDependency = Dependency; break; }
		if (!FailedDependency.IsEmpty())
		{
			Reject(Id, FString::Printf(TEXT("Script dependency '%s' failed; keeping the old unit"), *FailedDependency));
			continue;
		}
        if (Modules->ContainsByPredicate([](const TSharedRef<FAngelscriptModuleDesc>& Module) { return Module->bNeedsSourcePreprocess; }))
        {
            FAngelscriptPreprocessor Preprocessor(ConfigSettings->BlueprintBindings);
            for (const auto& Module : *Modules)
                for (const auto& Section : Module->Code)
                    Preprocessor.AddFile(Section.RelativeFilename, Section.AbsoluteFilename);
            const bool bPreprocessed = Preprocessor.Preprocess();
            *Modules = Preprocessor.GetModulesToCompile();
            if (!bPreprocessed || Modules->ContainsByPredicate([](const TSharedRef<FAngelscriptModuleDesc>& Module) { return Module->bCompileError; }))
            {
                Reject(Id, TEXT("Script unit preprocessing failed; keeping the old unit"));
                continue;
            }
        }
		// Changed files keep the existing intra-unit dependency/recompile avoidance.
		// Units reached through reverse dependencies reload their source together.
		if (CompileType != ECompileType::Initial && !Changed.Contains(Id))
		{
			FAngelscriptPreprocessor Preprocessor(ConfigSettings->BlueprintBindings);
			for (const auto& Old : PreviousModules)
				if (!Old->bTreatAsDeleted && Old->ScriptUnit.Id == Id && !RequestedModules.Contains(Old->ModuleName))
					for (const auto& Section : Old->Code)
						Preprocessor.AddFile(Section.RelativeFilename, Section.AbsoluteFilename);
			if (!Preprocessor.Preprocess())
			{
				Reject(Id, TEXT("Script unit preprocessing failed; keeping the old unit"));
				continue;
			}
			Modules->Append(Preprocessor.GetModulesToCompile());
			if (Modules->ContainsByPredicate([](const TSharedRef<FAngelscriptModuleDesc>& Module) { return Module->bCompileError; }))
			{
				Reject(Id, TEXT("Script unit preprocessing failed; keeping the old unit"));
				continue;
			}
		}
		if (Modules->IsEmpty()) continue;
		TArray<TSharedRef<FAngelscriptModuleDesc>> Compiled;
		const ECompileResult Result = CompileScriptUnit(CompileType, *Modules, Compiled, Units);
		if (Result == ECompileResult::Error || Result == ECompileResult::ErrorNeedFullReload || Result == ECompileResult::NativeReloadFailed)
		{
			bFailed = true;
			bNeedsFullReload |= Result == ECompileResult::ErrorNeedFullReload;
			bNativeFailed |= Result == ECompileResult::NativeReloadFailed;
			Errors.Add(Id, TEXT("Script unit compilation or reload failed"));
		}
		else
		{
			bSucceeded = true;
			bPartial |= Result == ECompileResult::PartiallyHandled;
		}
		for (const auto& Module : Compiled)
			if (Module->ScriptModule != nullptr && GetModule(Module->ModuleName) == Module)
				OutCompiledModules.Add(Module);
	}
#if WITH_METADATA
	const auto Active = GetActiveModules();
	if (Active.ContainsByPredicate([](const auto& Module) { return Module->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf; }))
	{
		for (const auto& Owner : Active)
			for (const auto& Desc : Owner->Classes)
			{
				UStruct* Symbol = Desc->Class ? static_cast<UStruct*>(Desc->Class) : Desc->Struct;
				if (!Symbol) continue;
				int32 Duplicates = 0;
				bool bHasLeaf = false;
				for (const auto& Other : Active)
					if (Other->GetClass(Desc->ClassName).IsValid())
					{
						++Duplicates;
						bHasLeaf |= Other->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf;
					}
				if (!bHasLeaf) continue;
				FString Display = Desc->Meta.FindRef(TEXT("DisplayName"));
				if (Display.IsEmpty())
				{
					Display = Desc->ClassName;
					if (Display.Len() > 1 && (Display[0] == 'U' || Display[0] == 'A' || Display[0] == 'F') && FChar::IsUpper(Display[1]))
					    Display.RightChopInline(1);
					Display = FName::NameToDisplayString(Display, false);
				}
				if (Duplicates > 1) Display += TEXT(" (") + Owner->ScriptUnit.Id + TEXT(")");
				Symbol->SetMetaData(TEXT("DisplayName"), *Display);
			}
		for (const auto& Owner : Active)
		{
			for (const auto& Desc : Owner->Enums)
			{
				if (!Desc->Enum) continue;
				int32 Duplicates = 0;
				bool bHasLeaf = false;
				for (const auto& Other : Active)
					if (Other->GetEnum(Desc->EnumName).IsValid())
					{
						++Duplicates;
						bHasLeaf |= Other->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf;
					}
				if (!bHasLeaf) continue;
				FString Display = FName::NameToDisplayString(Desc->EnumName, false);
				if (Duplicates > 1) Display += TEXT(" (") + Owner->ScriptUnit.Id + TEXT(")");
				Desc->Enum->SetMetaData(TEXT("DisplayName"), *Display);
			}
			for (const auto& Desc : Owner->Delegates)
			{
				if (!Desc->Function) continue;
				int32 Duplicates = 0;
				bool bHasLeaf = false;
				for (const auto& Other : Active)
					if (Other->Delegates.ContainsByPredicate([&](const TSharedRef<FAngelscriptDelegateDesc>& OtherDesc) { return
					    OtherDesc->DelegateName == Desc->DelegateName; }))
					{
						++Duplicates;
						bHasLeaf |= Other->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf;
					}
				if (!bHasLeaf) continue;
				FString Display = FName::NameToDisplayString(Desc->DelegateName, false);
				if (Duplicates > 1) Display += TEXT(" (") + Owner->ScriptUnit.Id + TEXT(")");
				Desc->Function->SetMetaData(TEXT("DisplayName"), *Display);
			}
		}
	}
#endif
	bHasSourceScriptUnits |= InputModules.ContainsByPredicate(
		[](const TSharedRef<FAngelscriptModuleDesc>& Module)
		{
			return Module->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf;
		});
#if AS_CAN_HOTRELOAD
	if (bIsInitialCompileFinished && !GIsEditor && bHasSourceScriptUnits && !bUseHotReloadCheckerThread)
	{
		bUseHotReloadCheckerThread = true;
		StartHotReloadThread();
	}
#endif
	if (bDiagnosticsDirty) EmitDiagnostics();
	if (bNativeFailed) return ECompileResult::NativeReloadFailed;
	if (bFailed && !bSucceeded) return bNeedsFullReload ? ECompileResult::ErrorNeedFullReload : ECompileResult::Error;
	return bFailed || bPartial ? ECompileResult::PartiallyHandled : ECompileResult::FullyHandled;
}

void FAngelscriptScriptUnitManager::Discover()
{
	Discover(TArray<FString>());
}

void FAngelscriptScriptUnitManager::Discover(const TArray<FString>& SourceFiles)
{
	const auto Plugins = IPluginManager::Get().GetDiscoveredPlugins();
#if WITH_EDITOR
	struct FCatalogCache
	{
		FAngelscriptScriptUnitManager Catalog;
		TArray<TSharedRef<IPlugin>> Plugins;
		TArray<TSharedPtr<FJsonObject>> Descriptors;
		TArray<bool> Enabled;
		TMap<FString, FString> FeatureModContents;
		const FProjectDescriptor* Project = nullptr;
		int32 ModuleCount = -1;
	};
	static FCatalogCache Cache;
	static FCriticalSection CacheLock;
	FScopeLock CatalogLock(&CacheLock);
	const FProjectDescriptor* ProjectDescriptor = IProjectManager::Get().GetCurrentProject();
	const int32 ModuleCount = FModuleManager::Get().GetModuleCount();
	bool bReuse = Cache.ModuleCount == ModuleCount && Cache.Project == ProjectDescriptor
		&& Cache.Plugins.Num() == Plugins.Num();
	for (int32 Index = 0; bReuse && Index < Plugins.Num(); ++Index)
	{
		const auto& Descriptor = Plugins[Index]->GetDescriptor().CachedJson;
		bReuse = Descriptor.IsValid() && Cache.Plugins[Index] == Plugins[Index]
			&& Cache.Descriptors[Index] == Descriptor && Cache.Enabled[Index] == Plugins[Index]->IsEnabled();
	}
	if (bReuse)
		for (const auto& File : Cache.FeatureModContents)
		{
			const FString ScriptRoot = FPaths::ConvertRelativePathToFull(FPaths::GetPath(File.Key) / TEXT("Script"));
			const auto* Owner = Cache.Catalog.Data->ScriptRoots.Find(ScriptRoot);
			if (!SourceFiles.IsEmpty() && Owner && !Cache.Catalog.Find(Owner->Id)
				&& !SourceFiles.ContainsByPredicate([&](const FString& Source)
					{ return FPaths::IsUnderDirectory(Source, ScriptRoot); }))
				continue;
			FString Contents;
			FFileHelper::LoadFileToString(Contents, *File.Key);
			if (Contents != File.Value) { bReuse = false; break; }
		}
	if (bReuse)
	{
		*this = Cache.Catalog;
		return;
	}
#endif
#if WITH_EDITOR
	TMap<FString, FString> FeatureModContents;
#endif
	Data = MakeShared<FData, ESPMode::ThreadSafe>();
    FAngelscriptScriptUnit Project;
    TMap<FString, FString> ModuleOwners;
    for (const auto& Plugin : Plugins)
        for (const auto& Module : Plugin->GetDescriptor().Modules)
            ModuleOwners.Add(Module.Name.ToString(), Plugin->GetName());
    if (const auto* Descriptor = IProjectManager::Get().GetCurrentProject())
        for (const auto& Module : Descriptor->Modules)
            ModuleOwners.Add(Module.Name.ToString(), Project.Id);
    TArray<FModuleStatus> NativeModules;
    FModuleManager::Get().QueryModules(NativeModules);
    for (const auto& Module : NativeModules)
        if (!ModuleOwners.Contains(Module.Name))
        {
            FAngelscriptScriptUnit Native;
            Native.Id = Module.Name;
            AddUnit(Native);
        }
    for (const auto& Plugin : Plugins)
	{
		// Registered target policy may exclude incompatible plugin units in Game.
		if (!GIsEditor && FAngelscriptModdingCacheGenerationFilterRuntime::IsGameCacheExcludedScriptUnit(Plugin->GetName()))
			continue;

		FAngelscriptScriptUnit Unit;
		Unit.Id = Plugin->GetName();
		FString DescriptorText;
		TSharedPtr<FJsonObject> Descriptor;
#if WITH_EDITOR
		Descriptor = Plugin->GetDescriptor().CachedJson;
#endif
		if (!Descriptor.IsValid()
			&& (!FFileHelper::LoadFileToString(DescriptorText, *Plugin->GetDescriptorFileName())
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DescriptorText), Descriptor)
			|| !Descriptor.IsValid()))
		{
			Unit.ConfigurationError = TEXT("Cannot read plugin descriptor");
		}
		else
		{
			bool bFeatureMod = false;
			if (Descriptor->HasField(TEXT("FeatureMod")) && !Descriptor->TryGetBoolField(TEXT("FeatureMod"), bFeatureMod))
				Unit.ConfigurationError = TEXT("The .uplugin FeatureMod marker must be a boolean");
			if (bFeatureMod)
				Unit.Layer = EAngelscriptScriptLayer::Leaf;
			else if (Descriptor->HasField(TEXT("BuiltInInitialFeatureState"))
				|| Plugin->GetBaseDir().Replace(TEXT("\\"), TEXT("/")).Contains(TEXT("/GameFeatures/")))
				Unit.Layer = EAngelscriptScriptLayer::Branch;
			for (const auto& Dependency : Plugin->GetDescriptor().Plugins)
			{
				const auto Target = IPluginManager::Get().FindPlugin(Dependency.Name);
				// Mirror UE's active plugin graph; native references retained in source
				// descriptors can be excluded from Game/Shipping by the target.
				if (Dependency.IsEnabledForPlatform(FPlatformMisc::GetUBTPlatform())
					&& Dependency.IsEnabledForTargetConfiguration(FApp::GetBuildConfiguration())
					&& Dependency.IsEnabledForTarget(FApp::GetBuildTargetType())
					&& Target.IsValid() && Target->IsEnabled()
					&& (GIsEditor || !FAngelscriptModdingCacheGenerationFilterRuntime::IsGameCacheExcludedScriptUnit(Dependency.Name)))
					Unit.Dependencies.AddUnique(Dependency.Name);
			}
			if (bFeatureMod)
			{
				FString ConfigurationText;
				TSharedPtr<FJsonObject> Configuration;
				if (!FFileHelper::LoadFileToString(ConfigurationText, *(Plugin->GetBaseDir() / TEXT("FeatureMod.json")))
					|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ConfigurationText), Configuration)
					|| !Configuration.IsValid())
				{
					Unit.ConfigurationError = TEXT("Cannot read FeatureMod.json");
				}
				else
				{
                    const TSharedPtr<FJsonObject>* Dependencies = nullptr;
                    if (!Configuration->TryGetObjectField(TEXT("dependencies"), Dependencies))
                        Unit.ConfigurationError = TEXT("FeatureMod.json dependencies must be an object");
                    else for (const TCHAR* Group : { TEXT("trunkModules"), TEXT("branches"), TEXT("featureMods") })
                    {
                        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
                        if (!(*Dependencies)->TryGetArrayField(Group, Values))
                        {
                            Unit.ConfigurationError = FString::Printf(TEXT("FeatureMod.json dependencies.%s must be an array"), Group);
                            continue;
                        }
                        for (const auto& Value : *Values)
                        {
                            FString Id;
                            if (!Value->TryGetString(Id) || Id.IsEmpty())
                                Unit.ConfigurationError = TEXT("FeatureMod.json dependencies must contain nonempty stable IDs");
                            else
                            {
                                const FString* Owner = FCString::Strcmp(Group, TEXT("trunkModules")) == 0 ? ModuleOwners.Find(Id) : nullptr;
                                Unit.Dependencies.AddUnique(Owner ? *Owner : Id);
                            }
                        }
                    }
				}
#if WITH_EDITOR
				FeatureModContents.Add(Plugin->GetBaseDir() / TEXT("FeatureMod.json"), ConfigurationText);
#endif
			}
		}
		// Editor-only native plugins are absent from staged Game manifests;
		// do not persist those source-checkout dependencies in the script cache.
		const bool bAvailableInTarget = Plugin->GetDescriptor().Modules.IsEmpty()
			|| Plugin->GetDescriptor().Modules.ContainsByPredicate([](const FModuleDescriptor& Module)
			{
				return Module.IsCompiledInCurrentConfiguration();
			});
		if (Plugin->IsEnabled() && Unit.Layer == EAngelscriptScriptLayer::Trunk && bAvailableInTarget)
			Project.Dependencies.AddUnique(Unit.Id);
		Data->ScriptRoots.Add(FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Script")), Unit);
		if (Plugin->IsEnabled())
			AddUnit(Unit);
	}
	FString CacheGenerationMod;
	if (FParse::Value(FCommandLine::Get(), TEXT("as-cache-unit="), CacheGenerationMod)
		&& !CacheGenerationMod.IsEmpty()
		&& CacheGenerationMod == FPaths::GetCleanFilename(CacheGenerationMod)
		&& !CacheGenerationMod.Contains(TEXT("..")))
	{
		FString SourceRoot;
		if (!FParse::Value(FCommandLine::Get(), TEXT("as-cache-source-root="), SourceRoot)
			|| SourceRoot.IsEmpty() || !FPaths::IsRelative(SourceRoot) || SourceRoot.Contains(TEXT("..")))
		{
			UE_LOG(Angelscript, Fatal, TEXT("Selected cache generation requires a project-relative as-cache-source-root"));
		}
		const FString ModRoots[] = { FPaths::GetPath(SourceRoot) };
		for (const FString& RelativeModRoot : ModRoots)
		{
			const FString ModRoot = FPaths::ProjectDir() / RelativeModRoot;
			const FString ScriptRoot = FPaths::ProjectDir() / SourceRoot;
			if (!IFileManager::Get().DirectoryExists(*ScriptRoot))
				continue;

			FAngelscriptScriptUnit Unit;
			Unit.Id = CacheGenerationMod;
			Unit.Layer = EAngelscriptScriptLayer::Leaf;
			FString DescriptorText;
			TSharedPtr<FJsonObject> Descriptor;
			bool bFeatureMod = false;
			if (!FFileHelper::LoadFileToString(DescriptorText, *(ModRoot / (CacheGenerationMod + TEXT(".uplugin"))))
				|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(DescriptorText), Descriptor)
				|| !Descriptor.IsValid()
				|| !Descriptor->TryGetBoolField(TEXT("FeatureMod"), bFeatureMod)
				|| !bFeatureMod)
			{
				Unit.ConfigurationError = TEXT("Cannot read FeatureMod descriptor for cache generation");
			}

#if WITH_EDITOR
			FeatureModContents.Add(ModRoot / (CacheGenerationMod + TEXT(".uplugin")), DescriptorText);
#endif
			if (Unit.ConfigurationError.IsEmpty())
			{
				FString ConfigurationText;
				TSharedPtr<FJsonObject> Configuration;
				const TSharedPtr<FJsonObject>* Dependencies = nullptr;
				if (!FFileHelper::LoadFileToString(ConfigurationText, *(ModRoot / TEXT("FeatureMod.json")))
					|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ConfigurationText), Configuration)
					|| !Configuration.IsValid()
					|| !Configuration->TryGetObjectField(TEXT("dependencies"), Dependencies))
				{
					Unit.ConfigurationError = TEXT("Cannot read FeatureMod.json for cache generation");
				}
				else for (const TCHAR* Group : { TEXT("trunkModules"), TEXT("branches"), TEXT("featureMods") })
				{
					const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
					if (!(*Dependencies)->TryGetArrayField(Group, Values))
					{
						Unit.ConfigurationError = FString::Printf(TEXT("FeatureMod.json dependencies.%s must be an array"), Group);
						continue;
					}
					for (const auto& Value : *Values)
					{
						FString Id;
						if (!Value->TryGetString(Id) || Id.IsEmpty())
							Unit.ConfigurationError = TEXT("FeatureMod.json dependencies must contain nonempty stable IDs");
						else
						{
							const FString* Owner = FCString::Strcmp(Group, TEXT("trunkModules")) == 0 ? ModuleOwners.Find(Id) : nullptr;
							Unit.Dependencies.AddUnique(Owner ? *Owner : Id);
						}
					}
				}
#if WITH_EDITOR
				FeatureModContents.Add(ModRoot / TEXT("FeatureMod.json"), ConfigurationText);
#endif
			}
			Data->ScriptRoots.Add(FPaths::ConvertRelativePathToFull(ScriptRoot), Unit);
			AddUnit(Unit);
		}
	}
	Data->ScriptRoots.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Script")), Project);
	AddUnit(Project);
#if WITH_EDITOR
	TArray<FString> Order;
	TMap<FString, FString> Errors;
	BuildOrder(Order, Errors);
	Cache.Catalog = *this;
	Cache.Plugins = Plugins;
	Cache.Project = ProjectDescriptor;
	Cache.ModuleCount = ModuleCount;
	Cache.Descriptors.Reset(Plugins.Num());
	Cache.Enabled.Reset(Plugins.Num());
	for (const auto& Plugin : Plugins)
	{
		Cache.Descriptors.Add(Plugin->GetDescriptor().CachedJson);
		Cache.Enabled.Add(Plugin->IsEnabled());
	}
	Cache.FeatureModContents = MoveTemp(FeatureModContents);
#endif
}

FAngelscriptScriptUnit FAngelscriptScriptUnitManager::ResolveOwner(const FString& AbsoluteFilename) const
{
	FString Filename = FPaths::ConvertRelativePathToFull(AbsoluteFilename);
	FPaths::NormalizeFilename(Filename);
	const FAngelscriptScriptUnit* Owner = Data->Units.Find(TEXT("Project"));
	int32 LongestRoot = 0;
	for (const auto& Root : Data->ScriptRoots)
	{
		if (Root.Key.Len() > LongestRoot && Filename.StartsWith(Root.Key)
			&& FPaths::IsUnderDirectory(Filename, Root.Key))
		{
			Owner = &Root.Value;
			LongestRoot = Root.Key.Len();
		}
	}
	return Owner != nullptr ? *Owner : FAngelscriptScriptUnit();
}

FString FAngelscriptScriptUnitManager::ResolveModuleName(const FString& AbsoluteFilename, const FString& RelativeFilename) const
{
	FString Filename = FPaths::ConvertRelativePathToFull(AbsoluteFilename);
	FPaths::NormalizeFilename(Filename);
	const FAngelscriptScriptUnit* Owner = nullptr;
	FString Relative = RelativeFilename;
	int32 LongestRoot = 0;
	for (const auto& Root : Data->ScriptRoots)
	{
		if (Root.Value.Layer == EAngelscriptScriptLayer::Leaf && Root.Key.Len() > LongestRoot
			&& FPaths::IsUnderDirectory(Filename, Root.Key))
		{
			Owner = &Root.Value;
			LongestRoot = Root.Key.Len();
			Relative = Filename;
			FPaths::MakePathRelativeTo(Relative, *(Root.Key + TEXT("/")));
		}
	}
	FString Name = Relative.Replace(TEXT(".as"), TEXT("")).Replace(TEXT("/"), TEXT("."));
	return Owner ? Owner->Id + TEXT(".") + Name : Name;
}

void FAngelscriptScriptUnitManager::AddUnit(const FAngelscriptScriptUnit& Unit)
{
	const auto* Existing = Data->Units.Find(Unit.Id);
	if (Existing && Existing->Layer == Unit.Layer && Existing->Dependencies == Unit.Dependencies
		&& Existing->AccessMask == Unit.AccessMask && Existing->ConfigurationError == Unit.ConfigurationError)
		return;
	if (!Data.IsUnique()) Data = MakeShared<FData, ESPMode::ThreadSafe>(*Data);
	Data->bHasBuildOrder = false;
	Data->Units.Add(Unit.Id, Unit);
}

bool FAngelscriptScriptUnitManager::CanReference(const FString& From, const FString& To, FString& OutError) const
{
	const auto* Source = Data->Units.Find(From);
	const auto* Target = Data->Units.Find(To);
	if (Source == nullptr || Target == nullptr)
		OutError = FString::Printf(TEXT("Missing script unit dependency '%s' -> '%s'"), *From, *To);
	else if (From == To)
		return true;
	else if (uint8(Target->Layer) > uint8(Source->Layer))
		OutError = FString::Printf(TEXT("Reverse script layer reference '%s' -> '%s' is forbidden"), *From, *To);
	else if (!Source->Dependencies.Contains(To))
		OutError = FString::Printf(TEXT("Undeclared script unit dependency '%s' -> '%s'"), *From, *To);
	else
		return true;
	return false;
}

void FAngelscriptScriptUnitManager::BuildOrder(TArray<FString>& OutOrder, TMap<FString, FString>& OutErrors) const
{
	if (Data->bHasBuildOrder)
	{
		OutOrder = Data->CachedBuildOrder;
		OutErrors = Data->CachedBuildErrors;
		return;
	}
	OutOrder.Reset();
	OutErrors.Reset();
	TArray<FString> Pending;
	Data->Units.GetKeys(Pending);
	Pending.Sort([&](const FString& A, const FString& B)
	{
		const auto LA = Data->Units.FindChecked(A).Layer;
		const auto LB = Data->Units.FindChecked(B).Layer;
		return LA != LB ? uint8(LA) < uint8(LB) : A < B;
	});
	for (const FString& Id : Pending)
	{
		const auto& Unit = Data->Units.FindChecked(Id);
		if (!Unit.ConfigurationError.IsEmpty())
			OutErrors.Add(Id, Unit.ConfigurationError);
		for (const FString& Dependency : Unit.Dependencies)
		{
			FString Error;
			if (Dependency == Id)
				OutErrors.Add(Id, TEXT("Script unit cannot depend on itself"));
			else if (!CanReference(Id, Dependency, Error))
				OutErrors.Add(Id, Error);
		}
	}
	TSet<FString> Ready;
	while (!Pending.IsEmpty())
	{
		bool bProgress = false;
		for (int32 Index = 0; Index < Pending.Num();)
		{
			const FString Id = Pending[Index];
			bool bWaiting = false;
			for (const FString& Dependency : Data->Units.FindChecked(Id).Dependencies)
			{
				if (OutErrors.Contains(Dependency))
					OutErrors.Add(Id, FString::Printf(TEXT("Script dependency '%s' failed"), *Dependency));
				if (!Ready.Contains(Dependency))
					bWaiting = true;
			}
			if (OutErrors.Contains(Id) || !bWaiting)
			{
				if (!OutErrors.Contains(Id))
				{
					Ready.Add(Id);
					OutOrder.Add(Id);
				}
				Pending.RemoveAt(Index);
				bProgress = true;
			}
			else
				++Index;
		}
		if (!bProgress)
		{
			for (const FString& Id : Pending)
				OutErrors.Add(Id, TEXT("Script dependency cycle or dependency on a cycle"));
			break;
		}
	}
	OutOrder.StableSort([&](const FString& A, const FString& B)
	{
		return uint8(Data->Units.FindChecked(A).Layer) < uint8(Data->Units.FindChecked(B).Layer);
	});
	Data->CachedBuildOrder = OutOrder;
	Data->CachedBuildErrors = OutErrors;
	Data->bHasBuildOrder = true;
}

TSet<FString> FAngelscriptScriptUnitManager::GetDependents(const TSet<FString>& ChangedUnits) const
{
	TSet<FString> Result = ChangedUnits;
	bool bChanged = true;
	while (bChanged)
	{
		bChanged = false;
		for (const auto& Pair : Data->Units)
		{
			if (Result.Contains(Pair.Key))
				continue;
			for (const FString& Dependency : Pair.Value.Dependencies)
			{
				if (Result.Contains(Dependency))
				{
					Result.Add(Pair.Key);
					bChanged = true;
					break;
				}
			}
		}
	}
	return Result;
}
