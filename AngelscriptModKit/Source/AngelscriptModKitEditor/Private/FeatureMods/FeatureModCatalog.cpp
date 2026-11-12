// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "FeatureMods/FeatureModCatalog.h"

#include "Core/ModKitSettings.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogFeatureModCatalog, Log, All);

namespace
{
	FString IdKey(const FString& Id)
	{
		return Id.ToLower();
	}

	bool ReadJsonObject(const FString& Filename, TSharedPtr<FJsonObject>& OutObject)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Filename))
		{
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
	}

	void ReadStringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName,
		TArray<FString>& OutValues, TArray<FString>& OutErrors, const FString& Prefix)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			OutErrors.Add(Prefix + FieldName + TEXT(" must be an array"));
			return;
		}

		TSet<FString> Seen;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid() || Value->Type != EJson::String || Value->AsString().TrimStartAndEnd().IsEmpty())
			{
				OutErrors.Add(Prefix + FieldName + TEXT(" entries must be non-empty strings"));
				continue;
			}

			const FString StringValue = Value->AsString();
			const FString Key = IdKey(StringValue);
			if (Seen.Contains(Key))
			{
				OutErrors.Add(Prefix + FieldName + TEXT(" contains duplicate entry: ") + StringValue);
				continue;
			}
			Seen.Add(Key);
			OutValues.Add(StringValue);
		}
	}

	void RejectUnknownFields(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Allowed,
		TArray<FString>& OutErrors, const FString& Prefix)
	{
		if (!Object.IsValid())
		{
			return;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			if (!Allowed.Contains(Field.Key))
			{
				OutErrors.Add(Prefix + TEXT("unknown field: ") + Field.Key);
			}
		}
	}

	bool ReadPluginDescriptor(const FString& Filename, bool bEnabled, FFeatureModPluginInfo& OutPlugin, FString& OutDisplayName)
	{
		TSharedPtr<FJsonObject> Object;
		if (!ReadJsonObject(Filename, Object))
		{
			return false;
		}

		OutPlugin = FFeatureModPluginInfo();
		OutPlugin.Id = FPaths::GetBaseFilename(Filename);
		OutPlugin.DescriptorPath = FPaths::ConvertRelativePathToFull(Filename);
		OutPlugin.bEnabled = bEnabled;
		Object->TryGetStringField(TEXT("FriendlyName"), OutDisplayName);

		const TSharedPtr<FJsonValue>* FeatureModMarker = Object->Values.Find(TEXT("FeatureMod"));
		OutPlugin.bFeatureMod = FeatureModMarker && FeatureModMarker->IsValid()
			&& (*FeatureModMarker)->Type == EJson::Boolean && (*FeatureModMarker)->AsBool();

		bool bExplicitlyLoaded = false;
		Object->TryGetBoolField(TEXT("bExplicitlyLoaded"), bExplicitlyLoaded);
		if (!bExplicitlyLoaded)
		{
			Object->TryGetBoolField(TEXT("ExplicitlyLoaded"), bExplicitlyLoaded);
		}
		Object->TryGetBoolField(TEXT("ExplicitlyLoaded"), OutPlugin.bExplicitlyLoaded);
		Object->TryGetStringField(TEXT("BuiltInInitialFeatureState"), OutPlugin.InitialFeatureState);
		FString Category;
		Object->TryGetStringField(TEXT("Category"), Category);
		OutPlugin.bGameFeature = !OutPlugin.bFeatureMod && (Object->HasField(TEXT("BuiltInInitialFeatureState"))
			|| FPaths::GetPath(Filename).Replace(TEXT("\\"), TEXT("/")).Contains(TEXT("/GameFeatures/")) || bExplicitlyLoaded
			|| Category.Equals(TEXT("GameFeature"), ESearchCase::IgnoreCase)
			|| Category.Equals(TEXT("GameFeatures"), ESearchCase::IgnoreCase));

		const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
		if (Object->TryGetArrayField(TEXT("Modules"), Modules) && Modules)
		{
			for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
			{
				if (!ModuleValue.IsValid() || ModuleValue->Type != EJson::Object)
				{
					continue;
				}
				FString ModuleName;
				if (ModuleValue->AsObject()->TryGetStringField(TEXT("Name"), ModuleName) && !ModuleName.IsEmpty())
				{
					OutPlugin.Modules.Add(ModuleName);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* PluginReferences = nullptr;
		if (Object->TryGetArrayField(TEXT("Plugins"), PluginReferences) && PluginReferences)
		{
			for (const TSharedPtr<FJsonValue>& ReferenceValue : *PluginReferences)
			{
				if (!ReferenceValue.IsValid() || ReferenceValue->Type != EJson::Object)
				{
					continue;
				}
				FString PluginName;
				if (ReferenceValue->AsObject()->TryGetStringField(TEXT("Name"), PluginName) && !PluginName.IsEmpty())
				{
					OutPlugin.PluginDependencies.Add(PluginName);
				}
			}
		}
		return true;
	}

	bool IsSamePath(const FString& Left, const FString& Right);

	void AddPluginInfo(TArray<FFeatureModPluginInfo>& Plugins, FFeatureModPluginInfo&& Plugin)
	{
		for (FFeatureModPluginInfo& Existing : Plugins)
		{
			if (Existing.Id.Equals(Plugin.Id, ESearchCase::IgnoreCase)
				&& IsSamePath(Existing.DescriptorPath, Plugin.DescriptorPath))
			{
				Existing.bEnabled = Existing.bEnabled || Plugin.bEnabled;
				return;
			}
		}
		Plugins.Add(MoveTemp(Plugin));
	}

	const FFeatureModPluginInfo* FindPlugin(const TArray<FFeatureModPluginInfo>& Plugins, const FString& Id)
	{
		for (const FFeatureModPluginInfo& Plugin : Plugins)
		{
			if (Plugin.Id.Equals(Id, ESearchCase::IgnoreCase))
			{
				return &Plugin;
			}
		}
		return nullptr;
	}

	int32 FindFeatureMod(const TArray<FFeatureModInfo>& FeatureMods, const FString& Id)
	{
		for (int32 Index = 0; Index < FeatureMods.Num(); ++Index)
		{
			if (FeatureMods[Index].Id.Equals(Id, ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	bool ContainsId(const TArray<FString>& Values, const FString& Id)
	{
		for (const FString& Value : Values)
		{
			if (Value.Equals(Id, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	void AddDiagnostic(FFeatureModInfo& FeatureMod, const FString& Message)
	{
		FeatureMod.Diagnostics.Add(Message);
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonValues(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	bool IsSafeId(const FString& Id)
	{
		if (Id.IsEmpty() || !FChar::IsAlpha(Id[0]))
		{
			return false;
		}
		for (const TCHAR Character : Id)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
			{
				return false;
			}
		}
		return true;
	}

	bool IsUnderRoot(const FString& Path, const FString& Root)
	{
		FString NormalizedPath = FPaths::ConvertRelativePathToFull(Path);
		FString NormalizedRoot = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(NormalizedPath);
		FPaths::NormalizeDirectoryName(NormalizedRoot);
		NormalizedRoot.RemoveFromEnd(TEXT("/"));
		return NormalizedPath.Equals(NormalizedRoot, ESearchCase::IgnoreCase)
			|| NormalizedPath.StartsWith(NormalizedRoot + TEXT("/"), ESearchCase::IgnoreCase);
	}

	bool IsSamePath(const FString& Left, const FString& Right)
	{
		FString NormalizedLeft = FPaths::ConvertRelativePathToFull(Left);
		FString NormalizedRight = FPaths::ConvertRelativePathToFull(Right);
		FPaths::NormalizeDirectoryName(NormalizedLeft);
		FPaths::NormalizeDirectoryName(NormalizedRight);
		return NormalizedLeft.Equals(NormalizedRight, ESearchCase::IgnoreCase);
	}
}

bool FFeatureModCatalog::IsSupportedRoot(const FString& Root)
{
	return Root.Equals(TEXT("Plugins/FeatureMods"), ESearchCase::IgnoreCase)
		|| Root.Equals(TEXT("FeatureMods"), ESearchCase::IgnoreCase);
}

void FFeatureModCatalog::CheckWhitelistCapabilities(FFeatureModInfo& FeatureMod, const UModKitSettings& Settings)
{
	for (const FString& Capability : FeatureMod.Manifest.RequiredWhitelistCapabilities)
	{
		if (Settings.IsCapabilityDenied(Capability))
		{
			FeatureMod.Diagnostics.Add(TEXT("missing whitelist capability: ") + Capability);
		}
	}
}

void FFeatureModCatalog::Discover(const FString& ProjectDirectory, const TArray<FString>& AllowedRoots,
	TArray<FFeatureModInfo>& OutFeatureMods, TArray<FFeatureModPluginInfo>& OutPlugins)
{
	OutFeatureMods.Reset();
	OutPlugins.Reset();

	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
	{
		const FString DescriptorPath = FPaths::Combine(Plugin->GetBaseDir(), Plugin->GetName() + TEXT(".uplugin"));
		FFeatureModPluginInfo PluginInfo;
		FString DisplayName;
		if (ReadPluginDescriptor(DescriptorPath, Plugin->IsEnabled(), PluginInfo, DisplayName))
		{
			AddPluginInfo(OutPlugins, MoveTemp(PluginInfo));
		}
	}

	FString NormalizedProjectDirectory = FPaths::ConvertRelativePathToFull(ProjectDirectory);
	FPaths::NormalizeDirectoryName(NormalizedProjectDirectory);
	const FString InternalRoot = FPaths::ConvertRelativePathToFull(NormalizedProjectDirectory, TEXT("Plugins/FeatureMods"));

	for (const FString& AllowedRoot : AllowedRoots)
	{
		if (!IsSupportedRoot(AllowedRoot))
		{
			continue;
		}
		const FString RootDirectory = FPaths::ConvertRelativePathToFull(NormalizedProjectDirectory, AllowedRoot);
		if (!IsUnderRoot(RootDirectory, NormalizedProjectDirectory) || !IFileManager::Get().DirectoryExists(*RootDirectory))
		{
			continue;
		}

		TArray<FString> ChildDirectories;
		IFileManager::Get().FindFiles(ChildDirectories, *(RootDirectory / TEXT("*")), false, true);
		ChildDirectories.Sort();
		for (const FString& ChildName : ChildDirectories)
		{
			const FString ChildDirectory = RootDirectory / ChildName;
			TArray<FString> DescriptorFiles;
			IFileManager::Get().FindFiles(DescriptorFiles, *(ChildDirectory / TEXT("*.uplugin")), true, false);
			DescriptorFiles.Sort();
			for (const FString& DescriptorName : DescriptorFiles)
			{
				const FString DescriptorPath = ChildDirectory / DescriptorName;
				FFeatureModPluginInfo PluginInfo;
				FString DisplayName;
				if (!ReadPluginDescriptor(DescriptorPath, false, PluginInfo, DisplayName))
				{
					continue;
				}

				const FFeatureModPluginInfo* ExistingPlugin = FindPlugin(OutPlugins, PluginInfo.Id);
				if (ExistingPlugin && IsSamePath(ExistingPlugin->DescriptorPath, PluginInfo.DescriptorPath))
				{
					PluginInfo.bEnabled = ExistingPlugin->bEnabled;
				}
				AddPluginInfo(OutPlugins, FFeatureModPluginInfo(PluginInfo));
				if (!PluginInfo.bFeatureMod)
				{
					continue;
				}

				FFeatureModInfo FeatureMod;
				FeatureMod.Id = ChildName;
				FeatureMod.DisplayName = DisplayName.IsEmpty() ? ChildName : DisplayName;
				FeatureMod.Directory = ChildDirectory;
				FeatureMod.DescriptorPath = DescriptorPath;
				FeatureMod.ManifestPath = ChildDirectory / TEXT("FeatureMod.json");
				FeatureMod.Location = IsUnderRoot(ChildDirectory, InternalRoot)
					? EFeatureModLocation::Internal : EFeatureModLocation::External;
				FeatureMod.bEnabled = PluginInfo.bEnabled;
				FeatureMod.LoadState = PluginInfo.bEnabled ? TEXT("Enabled") : TEXT("Not loaded");
				if (!FFileHelper::LoadFileToString(FeatureMod.ManifestText, *FeatureMod.ManifestPath))
				{
					FeatureMod.Diagnostics.Add(TEXT("FeatureMod.json is missing or unreadable"));
				}
				else
				{
					TArray<FString> ParseErrors;
					if (ParseManifest(FeatureMod.ManifestText, FeatureMod.Manifest, ParseErrors))
					{
						FeatureMod.Version = FeatureMod.Manifest.Version;
					}
					else
					{
						FeatureMod.Diagnostics.Append(ParseErrors);
					}
				}
				OutFeatureMods.Add(MoveTemp(FeatureMod));
			}
		}
	}

	Validate(OutFeatureMods, OutPlugins);
}

bool FFeatureModCatalog::ParseManifest(const FString& Json, FFeatureModManifest& OutManifest, TArray<FString>& OutErrors)
{
	OutManifest = FFeatureModManifest();
	OutErrors.Reset();
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutErrors.Add(TEXT("FeatureMod.json is not valid JSON"));
		return false;
	}
	TArray<FString> UnknownFields;
	RejectUnknownFields(Root, { TEXT("schemaVersion"), TEXT("dependencies"), TEXT("BlueprintBindings"),
		TEXT("BlueprintDependencies"), TEXT("version"), TEXT("signature"), TEXT("requiredWhitelistCapabilities") },
		UnknownFields, TEXT(""));

	double SchemaVersion = 0.0;
	if (!Root->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion) || SchemaVersion != 1.0)
	{
		OutErrors.Add(TEXT("schemaVersion must be 1"));
	}
	OutManifest.SchemaVersion = static_cast<int32>(SchemaVersion);

	const TSharedPtr<FJsonObject>* Dependencies = nullptr;
	if (!Root->TryGetObjectField(TEXT("dependencies"), Dependencies) || !Dependencies || !Dependencies->IsValid())
	{
		OutErrors.Add(TEXT("dependencies must be an object"));
	}
	else
	{
		RejectUnknownFields(*Dependencies, { TEXT("trunkModules"), TEXT("branches"), TEXT("featureMods") },
			UnknownFields, TEXT("dependencies."));
		ReadStringArray(*Dependencies, TEXT("trunkModules"), OutManifest.TrunkModules, OutErrors, TEXT("dependencies."));
		ReadStringArray(*Dependencies, TEXT("branches"), OutManifest.Branches, OutErrors, TEXT("dependencies."));
		ReadStringArray(*Dependencies, TEXT("featureMods"), OutManifest.FeatureMods, OutErrors, TEXT("dependencies."));
	}

	if (!UnknownFields.IsEmpty())
		UE_LOG(LogFeatureModCatalog, Warning, TEXT("FeatureMod manifest fields ignored by ModKit: %s"),
			*FString::Join(UnknownFields, TEXT("; ")));

	const TSharedPtr<FJsonObject>* Bindings = nullptr;
	if (!Root->TryGetObjectField(TEXT("BlueprintBindings"), Bindings) || !Bindings || !Bindings->IsValid())
	{
		OutErrors.Add(TEXT("BlueprintBindings must be an object"));
	}
	else
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Binding : (*Bindings)->Values)
		{
			if (Binding.Key.TrimStartAndEnd().IsEmpty() || !Binding.Value.IsValid()
				|| Binding.Value->Type != EJson::String || Binding.Value->AsString().TrimStartAndEnd().IsEmpty())
			{
				OutErrors.Add(TEXT("BlueprintBindings values must be non-empty asset paths keyed by script name"));
				continue;
			}
			OutManifest.BlueprintBindings.Add(Binding.Key, Binding.Value->AsString());
		}
	}

	ReadStringArray(Root, TEXT("BlueprintDependencies"), OutManifest.BlueprintDependencies, OutErrors, TEXT(""));
	if (!Root->HasTypedField<EJson::String>(TEXT("version"))
		|| !Root->TryGetStringField(TEXT("version"), OutManifest.Version) || OutManifest.Version.TrimStartAndEnd().IsEmpty())
	{
		OutErrors.Add(TEXT("version must be a non-empty string"));
	}
	ReadStringArray(Root, TEXT("requiredWhitelistCapabilities"), OutManifest.RequiredWhitelistCapabilities, OutErrors, TEXT(""));

	const TSharedPtr<FJsonValue>* SignatureValue = Root->Values.Find(TEXT("signature"));
	if (SignatureValue)
	{
		if (!SignatureValue->IsValid() || (*SignatureValue)->Type != EJson::Object)
		{
			OutErrors.Add(TEXT("signature must be an object when present"));
		}
		else
		{
			const TSharedPtr<FJsonObject> Signature = (*SignatureValue)->AsObject();
			RejectUnknownFields(Signature, { TEXT("algorithm"), TEXT("keyId"), TEXT("payloadSha256"), TEXT("value") },
				OutErrors, TEXT("signature."));
			OutManifest.bHasSignature = true;
			if (!Signature->HasTypedField<EJson::String>(TEXT("algorithm"))
				|| !Signature->TryGetStringField(TEXT("algorithm"), OutManifest.SignatureAlgorithm)
				|| OutManifest.SignatureAlgorithm.IsEmpty())
			{
				OutErrors.Add(TEXT("signature.algorithm must be a non-empty string"));
			}
			if (!Signature->HasTypedField<EJson::String>(TEXT("keyId"))
				|| !Signature->TryGetStringField(TEXT("keyId"), OutManifest.SignatureKeyId)
				|| OutManifest.SignatureKeyId.IsEmpty())
			{
				OutErrors.Add(TEXT("signature.keyId must be a non-empty string"));
			}
			if (!Signature->HasTypedField<EJson::String>(TEXT("payloadSha256"))
				|| !Signature->TryGetStringField(TEXT("payloadSha256"), OutManifest.SignaturePayloadSha256)
				|| OutManifest.SignaturePayloadSha256.IsEmpty())
			{
				OutErrors.Add(TEXT("signature.payloadSha256 must be a non-empty string"));
			}
			if (!Signature->HasTypedField<EJson::String>(TEXT("value"))
				|| !Signature->TryGetStringField(TEXT("value"), OutManifest.SignatureValue)
				|| OutManifest.SignatureValue.IsEmpty())
			{
				OutErrors.Add(TEXT("signature.value must be a non-empty string"));
			}
		}
	}

	return OutErrors.IsEmpty();
}

FString FFeatureModCatalog::SerializeManifest(const FFeatureModManifest& Manifest)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Dependencies = MakeShared<FJsonObject>();
	Dependencies->SetArrayField(TEXT("trunkModules"), ToJsonValues(Manifest.TrunkModules));
	Dependencies->SetArrayField(TEXT("branches"), ToJsonValues(Manifest.Branches));
	Dependencies->SetArrayField(TEXT("featureMods"), ToJsonValues(Manifest.FeatureMods));
	Root->SetNumberField(TEXT("schemaVersion"), Manifest.SchemaVersion);
	Root->SetObjectField(TEXT("dependencies"), Dependencies);
	TSharedRef<FJsonObject> Bindings = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Binding : Manifest.BlueprintBindings)
	{
		Bindings->SetStringField(Binding.Key, Binding.Value);
	}
	Root->SetObjectField(TEXT("BlueprintBindings"), Bindings);
	Root->SetArrayField(TEXT("BlueprintDependencies"), ToJsonValues(Manifest.BlueprintDependencies));
	Root->SetStringField(TEXT("version"), Manifest.Version);
	if (Manifest.bHasSignature)
	{
		TSharedRef<FJsonObject> Signature = MakeShared<FJsonObject>();
		Signature->SetStringField(TEXT("algorithm"), Manifest.SignatureAlgorithm);
		Signature->SetStringField(TEXT("keyId"), Manifest.SignatureKeyId);
		Signature->SetStringField(TEXT("payloadSha256"), Manifest.SignaturePayloadSha256);
		Signature->SetStringField(TEXT("value"), Manifest.SignatureValue);
		Root->SetObjectField(TEXT("signature"), Signature);
	}
	Root->SetArrayField(TEXT("requiredWhitelistCapabilities"), ToJsonValues(Manifest.RequiredWhitelistCapabilities));

	FString Result;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Result);
	FJsonSerializer::Serialize(Root, Writer);
	return Result;
}

void FFeatureModCatalog::Validate(TArray<FFeatureModInfo>& FeatureMods, const TArray<FFeatureModPluginInfo>& Plugins)
{
	TMap<FString, TArray<int32>> IdOwners;
	TMap<FString, int32> FeatureModById;
	TMap<FString, const FFeatureModPluginInfo*> PluginById;
	TMap<FString, FString> ModuleOwner;
	TSet<FString> AvailableModules;

	for (int32 Index = 0; Index < FeatureMods.Num(); ++Index)
	{
		FFeatureModInfo& FeatureMod = FeatureMods[Index];
		FeatureMod.Diagnostics.Reset();
		IdOwners.FindOrAdd(IdKey(FeatureMod.Id)).Add(Index);
		FeatureModById.Add(IdKey(FeatureMod.Id), Index);
		if (FPaths::GetBaseFilename(FeatureMod.DescriptorPath).Equals(FeatureMod.Id, ESearchCase::IgnoreCase) == false)
		{
			AddDiagnostic(FeatureMod, TEXT("descriptor name must match the FeatureMod folder ID"));
		}
		if (FeatureMod.ManifestText.IsEmpty())
		{
			AddDiagnostic(FeatureMod, TEXT("FeatureMod.json is missing or empty"));
			continue;
		}
		TArray<FString> ParseErrors;
		if (!ParseManifest(FeatureMod.ManifestText, FeatureMod.Manifest, ParseErrors))
		{
			FeatureMod.Diagnostics.Append(ParseErrors);
			continue;
		}
		FeatureMod.Version = FeatureMod.Manifest.Version;
		FeatureMod.bSigned = IFileManager::Get().FileExists(*(FPaths::GetPath(FeatureMod.Directory)
			/ (FeatureMod.Id + TEXT(".manifest.json"))));
		if (FeatureMod.Manifest.bHasSignature)
		{
			AddDiagnostic(FeatureMod, TEXT("signature in FeatureMod.json does not sign a package; use a separate <name>.manifest.json"));
		}
		const FFeatureModPluginInfo* Descriptor = FindPlugin(Plugins, FeatureMod.Id);
		if (Descriptor && IsSamePath(Descriptor->DescriptorPath, FeatureMod.DescriptorPath))
		{
			if (!Descriptor->bExplicitlyLoaded)
				AddDiagnostic(FeatureMod, TEXT(".uplugin requires ExplicitlyLoaded=true"));
			if (Descriptor->InitialFeatureState != TEXT("Installed"))
				AddDiagnostic(FeatureMod, TEXT(".uplugin requires BuiltInInitialFeatureState=Installed"));
		}
	}

	for (const TPair<FString, TArray<int32>>& Pair : IdOwners)
	{
		if (Pair.Value.Num() > 1)
		{
			for (const int32 Index : Pair.Value)
			{
				AddDiagnostic(FeatureMods[Index], TEXT("duplicate FeatureMod ID: ") + FeatureMods[Index].Id);
			}
		}
	}

	AvailableModules.Add(IdKey(TEXT("Project")));
	for (const FFeatureModPluginInfo& Plugin : Plugins)
	{
		PluginById.FindOrAdd(IdKey(Plugin.Id)) = &Plugin;
        if (!Plugin.bFeatureMod && !Plugin.bGameFeature)
        {
            AvailableModules.Add(IdKey(Plugin.Id));
            ModuleOwner.FindOrAdd(IdKey(Plugin.Id)) = Plugin.Id;
        }
		for (const FString& Module : Plugin.Modules)
		{
			ModuleOwner.FindOrAdd(IdKey(Module)) = Plugin.Id;
			AvailableModules.Add(IdKey(Module));
		}
	}
	TArray<FModuleStatus> ModuleStatuses;
	FModuleManager::Get().QueryModules(ModuleStatuses);
	for (const FModuleStatus& ModuleStatus : ModuleStatuses)
	{
		AvailableModules.Add(IdKey(ModuleStatus.Name));
	}

	for (FFeatureModInfo& FeatureMod : FeatureMods)
	{
		for (const FString& Module : FeatureMod.Manifest.TrunkModules)
		{
			const FString Key = IdKey(Module);
			if (!AvailableModules.Contains(Key))
			{
				AddDiagnostic(FeatureMod, TEXT("missing trunk module dependency: ") + Module);
				continue;
			}
			const FString* OwnerId = ModuleOwner.Find(Key);
			const FFeatureModPluginInfo* Owner = OwnerId ? PluginById.FindRef(IdKey(*OwnerId)) : nullptr;
			if (Owner && (Owner->bFeatureMod || Owner->bGameFeature))
			{
				AddDiagnostic(FeatureMod, FString::Printf(TEXT("cross-layer dependency: module %s belongs to %s %s"),
					*Module, Owner->bFeatureMod ? TEXT("FeatureMod") : TEXT("branch"), **OwnerId));
			}
		}

		for (const FString& BranchId : FeatureMod.Manifest.Branches)
		{
			const FFeatureModPluginInfo* Branch = PluginById.FindRef(IdKey(BranchId));
			if (!Branch)
			{
				AddDiagnostic(FeatureMod, TEXT("missing branch dependency: ") + BranchId);
			}
			else if (Branch->bFeatureMod)
			{
				AddDiagnostic(FeatureMod, TEXT("cross-layer dependency: branches cannot point to FeatureMods: ") + BranchId);
			}
			else if (!Branch->bGameFeature)
			{
				AddDiagnostic(FeatureMod, TEXT("dependency target is not a GameFeature branch: ") + BranchId);
			}
		}

		for (const FString& DependencyId : FeatureMod.Manifest.FeatureMods)
		{
			if (FindFeatureMod(FeatureMods, DependencyId) == INDEX_NONE)
			{
				AddDiagnostic(FeatureMod, TEXT("missing FeatureMod dependency: ") + DependencyId);
			}
		}
	}

	for (const FFeatureModPluginInfo& Plugin : Plugins)
	{
		for (const FString& DependencyId : Plugin.PluginDependencies)
		{
			const FFeatureModPluginInfo* Target = PluginById.FindRef(IdKey(DependencyId));
			if (!Target || (!Target->bFeatureMod && !Target->bGameFeature))
			{
				continue;
			}

			const int32* TargetIndexValue = FeatureModById.Find(IdKey(Target->Id));
			const int32 TargetIndex = TargetIndexValue ? *TargetIndexValue : INDEX_NONE;
			if (!Plugin.bFeatureMod)
			{
				if (TargetIndex != INDEX_NONE)
				{
					AddDiagnostic(FeatureMods[TargetIndex], FString::Printf(
						TEXT("reverse dependency from trunk or branch plugin %s is forbidden"), *Plugin.Id));
				}
				continue;
			}

			const int32* SourceIndexValue = FeatureModById.Find(IdKey(Plugin.Id));
			const int32 SourceIndex = SourceIndexValue ? *SourceIndexValue : INDEX_NONE;
			if (Target->bGameFeature && SourceIndex != INDEX_NONE
				&& !ContainsId(FeatureMods[SourceIndex].Manifest.Branches, Target->Id))
			{
				AddDiagnostic(FeatureMods[SourceIndex], TEXT("undeclared branch dependency in .uplugin: ") + Target->Id);
			}
			if (Target->bFeatureMod && SourceIndex != INDEX_NONE
				&& !ContainsId(FeatureMods[SourceIndex].Manifest.FeatureMods, Target->Id))
			{
				AddDiagnostic(FeatureMods[SourceIndex], TEXT("undeclared FeatureMod dependency in .uplugin: ") + Target->Id);
			}
		}
	}

	TArray<uint8> VisitState;
	VisitState.SetNumZeroed(FeatureMods.Num());
	TArray<int32> VisitStack;
	TFunction<void(int32)> Visit = [&](const int32 Index)
	{
		VisitState[Index] = 1;
		VisitStack.Add(Index);
		for (const FString& DependencyId : FeatureMods[Index].Manifest.FeatureMods)
		{
			const int32 DependencyIndex = FindFeatureMod(FeatureMods, DependencyId);
			if (DependencyIndex == INDEX_NONE)
			{
				continue;
			}
			if (VisitState[DependencyIndex] == 0)
			{
				Visit(DependencyIndex);
			}
			else if (VisitState[DependencyIndex] == 1)
			{
				int32 CycleStart = VisitStack.IndexOfByKey(DependencyIndex);
				FString Cycle = TEXT("FeatureMod dependency cycle: ");
				for (int32 StackIndex = CycleStart; StackIndex < VisitStack.Num(); ++StackIndex)
				{
					if (StackIndex > CycleStart)
					{
						Cycle += TEXT(" -> ");
					}
					Cycle += FeatureMods[VisitStack[StackIndex]].Id;
				}
				Cycle += TEXT(" -> ") + FeatureMods[DependencyIndex].Id;
				for (int32 StackIndex = CycleStart; StackIndex < VisitStack.Num(); ++StackIndex)
				{
					AddDiagnostic(FeatureMods[VisitStack[StackIndex]], Cycle);
				}
			}
		}
		VisitStack.Pop(EAllowShrinking::No);
		VisitState[Index] = 2;
	};
	for (int32 Index = 0; Index < FeatureMods.Num(); ++Index)
	{
		if (VisitState[Index] == 0)
		{
			Visit(Index);
		}
	}
}

FString FFeatureModCatalog::BuildDependencyView(const TArray<FFeatureModInfo>& FeatureMods,
	const TArray<FFeatureModPluginInfo>& Plugins)
{
	FString Report = TEXT("Trunk → Branch → FeatureMod\n\n");
	for (const FFeatureModInfo& FeatureMod : FeatureMods)
	{
		Report += FString::Printf(TEXT("Leaf %s [%s, %s]\n"), *FeatureMod.Id,
			FeatureMod.Location == EFeatureModLocation::Internal ? TEXT("internal") : TEXT("external"),
			FeatureMod.Version.IsEmpty() ? TEXT("no version") : *FeatureMod.Version);
		for (const FString& Module : FeatureMod.Manifest.TrunkModules)
		{
			Report += FString::Printf(TEXT("  Trunk module %s → %s\n"), *Module, *FeatureMod.Id);
		}
		for (const FString& BranchId : FeatureMod.Manifest.Branches)
		{
			const FFeatureModPluginInfo* Branch = FindPlugin(Plugins, BranchId);
			Report += FString::Printf(TEXT("  Trunk → Branch %s%s → %s\n"), *BranchId,
				Branch && Branch->bGameFeature ? TEXT("") : TEXT(" [not a discovered branch]"), *FeatureMod.Id);
		}
		for (const FString& DependencyId : FeatureMod.Manifest.FeatureMods)
		{
			Report += FString::Printf(TEXT("  FeatureMod %s → FeatureMod %s\n"), *DependencyId, *FeatureMod.Id);
		}
		for (const FString& Diagnostic : FeatureMod.Diagnostics)
		{
			Report += TEXT("  ! ") + Diagnostic + TEXT("\n");
		}
		Report += TEXT("\n");
	}
	if (FeatureMods.IsEmpty())
	{
		Report += TEXT("No marked FeatureMods were discovered.\n");
	}
	return Report;
}

bool FFeatureModCatalog::SetDependency(const FString& Json, const FString& Kind, const FString& Id,
	bool bEnabled, const FString& FeatureModId, TArray<FFeatureModInfo> FeatureMods,
	const TArray<FFeatureModPluginInfo>& Plugins, FString& OutJson, TArray<FString>& OutErrors)
{
	FFeatureModManifest Manifest;
	if (!ParseManifest(Json, Manifest, OutErrors)) return false;
	if (Kind != TEXT("trunkModules") && Kind != TEXT("branches") && Kind != TEXT("featureMods"))
	{
		OutErrors.Add(TEXT("Unknown dependency layer"));
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
	const auto Dependencies = Root->GetObjectField(TEXT("dependencies"));
	TArray<TSharedPtr<FJsonValue>> Values = Dependencies->GetArrayField(Kind);
	Values.RemoveAll([&Id](const TSharedPtr<FJsonValue>& Value)
	{
		return Value->AsString().Equals(Id, ESearchCase::IgnoreCase);
	});
	if (bEnabled) Values.Add(MakeShared<FJsonValueString>(Id));
	Dependencies->SetArrayField(Kind, Values);
	FString CandidateJson;
	FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&CandidateJson));
	const int32 Index = FindFeatureMod(FeatureMods, FeatureModId);
	if (Index == INDEX_NONE)
	{
		OutErrors.Add(TEXT("Selected FeatureMod is no longer in the discovered catalog"));
		return false;
	}
	FeatureMods[Index].ManifestText = CandidateJson;
	Validate(FeatureMods, Plugins);
	if (bEnabled && !FeatureMods[Index].Diagnostics.IsEmpty())
	{
		OutErrors = FeatureMods[Index].Diagnostics;
		return false;
	}
	OutJson = MoveTemp(CandidateJson);
	return true;
}

bool FFeatureModCatalog::SaveManifest(const FString& Filename, const FString& Json, const FString& FeatureModId,
	TArray<FFeatureModInfo> FeatureMods, const TArray<FFeatureModPluginInfo>& Plugins, TArray<FString>& OutErrors)
{
	OutErrors.Reset();
	const int32 Index = FindFeatureMod(FeatureMods, FeatureModId);
	if (Index == INDEX_NONE)
	{
		OutErrors.Add(TEXT("Selected FeatureMod is no longer in the discovered catalog"));
		return false;
	}
	FeatureMods[Index].ManifestText = Json;
	Validate(FeatureMods, Plugins);
	OutErrors = FeatureMods[Index].Diagnostics;
	if (!OutErrors.IsEmpty())
	{
		return false;
	}
	FString SavedJson = Json;
	TSharedPtr<FJsonObject> Existing;
	if (ReadJsonObject(Filename, Existing))
	{
		TSharedPtr<FJsonObject> Edited;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Edited);
		bool bPreserved = false;
		auto PreserveForeignFields = [&bPreserved](const TSharedPtr<FJsonObject>& From,
			const TSharedPtr<FJsonObject>& To, const TArray<FString>& Owned)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : From->Values)
			{
				if (!Owned.Contains(Field.Key))
				{
					To->SetField(Field.Key, Field.Value);
					bPreserved = true;
				}
			}
		};
		PreserveForeignFields(Existing, Edited, { TEXT("schemaVersion"), TEXT("dependencies"),
			TEXT("BlueprintBindings"), TEXT("BlueprintDependencies"), TEXT("version"), TEXT("signature"),
			TEXT("requiredWhitelistCapabilities") });
		const TSharedPtr<FJsonObject>* Dependencies = nullptr;
		if (Existing->TryGetObjectField(TEXT("dependencies"), Dependencies) && Dependencies && Dependencies->IsValid())
			PreserveForeignFields(*Dependencies, Edited->GetObjectField(TEXT("dependencies")),
				{ TEXT("trunkModules"), TEXT("branches"), TEXT("featureMods") });
		if (bPreserved)
		{
			SavedJson.Reset();
			FJsonSerializer::Serialize(Edited.ToSharedRef(), TJsonWriterFactory<>::Create(&SavedJson));
		}
	}
	if (!FFileHelper::SaveStringToFile(SavedJson, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutErrors.Add(TEXT("Could not write FeatureMod.json"));
		return false;
	}
	return true;
}

bool FFeatureModCatalog::CreateFromTemplate(const FString& ProjectDirectory, const FString& RelativeRoot,
	const FString& FeatureModId, FString& OutDirectory, FString& OutError)
{
	OutDirectory.Reset();
	OutError.Reset();
	if (!IsSafeId(FeatureModId))
	{
		OutError = TEXT("Use a stable ID with a leading letter followed by letters, numbers, or underscores.");
		return false;
	}
	if (!IsSupportedRoot(RelativeRoot))
	{
		OutError = TEXT("Use Plugins/FeatureMods or FeatureMods; other roots are not discovered by the runtime.");
		return false;
	}

	const FString NormalizedProject = FPaths::ConvertRelativePathToFull(ProjectDirectory);
	const FString RootDirectory = FPaths::ConvertRelativePathToFull(NormalizedProject, RelativeRoot);
	const FString TargetDirectory = RootDirectory / FeatureModId;
	if (!IsUnderRoot(RootDirectory, NormalizedProject) || !IsUnderRoot(TargetDirectory, RootDirectory))
	{
		OutError = TEXT("The selected root must remain inside the project.");
		return false;
	}
	if (IFileManager::Get().DirectoryExists(*TargetDirectory))
	{
		OutError = TEXT("A directory with this FeatureMod ID already exists.");
		return false;
	}

	const TSharedPtr<IPlugin> ModKitPlugin = IPluginManager::Get().FindPlugin(TEXT("AngelscriptModKit"));
	if (!ModKitPlugin.IsValid())
	{
		OutError = TEXT("Could not locate the AngelscriptModKit template directory.");
		return false;
	}
	const FString TemplateDirectory = FPaths::Combine(ModKitPlugin->GetBaseDir(), TEXT("Templates/ModName"));
	TArray<FString> TemplateFiles;
	IFileManager::Get().FindFilesRecursive(TemplateFiles, *TemplateDirectory, TEXT("*"), true, false, true);
	if (TemplateFiles.IsEmpty())
	{
		OutError = TEXT("The FeatureMod template is empty.");
		return false;
	}

	if (!IFileManager::Get().MakeDirectory(*TargetDirectory, true))
	{
		OutError = TEXT("Could not create the FeatureMod directory.");
		return false;
	}
	for (const FString& SourceFilename : TemplateFiles)
	{
		FString RelativeFilename = SourceFilename;
		if (!FPaths::MakePathRelativeTo(RelativeFilename, *(TemplateDirectory + TEXT("/"))))
		{
			OutError = TEXT("Could not resolve a template-relative path.");
			return false;
		}
		RelativeFilename.ReplaceInline(TEXT("ModName"), *FeatureModId);
		const FString DestinationFilename = FPaths::Combine(TargetDirectory, RelativeFilename);
		const FString DestinationDirectory = FPaths::GetPath(DestinationFilename);
		if (!IFileManager::Get().MakeDirectory(*DestinationDirectory, true))
		{
			OutError = TEXT("Could not create a template subdirectory.");
			return false;
		}

		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *SourceFilename))
		{
			OutError = TEXT("Could not read a template file.");
			return false;
		}
		Content.ReplaceInline(TEXT("ModName"), *FeatureModId);
		if (!FFileHelper::SaveStringToFile(Content, *DestinationFilename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("Could not write a FeatureMod template file.");
			return false;
		}
	}

	OutDirectory = TargetDirectory;
	return true;
}
