// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
using UnrealBuildTool;
using System.IO;
using System.Text.RegularExpressions;
using EpicGames.Core;

namespace UnrealBuildTool.Rules
{
	public class AngelscriptModKit : ModuleRules
	{
		public AngelscriptModKit(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
			RuntimeDependencies.Add("$(PluginDir)/Config/ApiClassification.json", StagedFileType.UFS);
			if (Target.ProjectFile != null && File.Exists(Path.Combine(Target.ProjectFile.Directory.FullName, "Config", "ModKitApiClassification.json")))
				RuntimeDependencies.Add("$(ProjectDir)/Config/ModKitApiClassification.json", StagedFileType.UFS);

			// Shipping trust is frozen from project source configuration into the binary.
			string PackageKey = "", PackageKeyId = "";
			if (Target.Configuration == UnrealTargetConfiguration.Shipping && Target.ProjectFile != null)
			{
				string Config = Path.Combine(Target.ProjectFile.Directory.FullName, "Config", "DefaultEngine.ini");
				ExternalDependencies.Add(Config);
				bool InSection = false;
				if (File.Exists(Config)) foreach (string Raw in File.ReadLines(Config))
				{
					string Line = Raw.Trim();
					if (Line.StartsWith("[")) InSection = Line == "[/Script/AngelscriptModKit.ModKitSettings]";
					if (!InSection) continue;
					string[] Pair = Line.Split(new[] { '=' }, 2);
					if (Pair.Length != 2) continue;
					string Name = Pair[0].Trim(), Value = Pair[1].Trim().Trim('"');
					if (Name == "PatchPublicKeyBase64") PackageKey = Value;
					if (Name == "PatchVerifyKeyId") PackageKeyId = Value;
				}
			}
			if (!Regex.IsMatch(PackageKey, @"^[A-Za-z0-9+/=]*$") || !Regex.IsMatch(PackageKeyId, @"^[A-Za-z0-9_.-]*$"))
				throw new BuildException("Invalid FeatureMod delivery trust root");
			bUsePrecompiled = SoloASBridgeBuildPath.ShouldUsePrecompiled(this, Target, bUsePrecompiled);
			if (Target.Configuration == UnrealTargetConfiguration.Shipping && (!string.IsNullOrEmpty(PackageKey) || !string.IsNullOrEmpty(PackageKeyId)))
				bUsePrecompiled = false;
			PrivateDefinitions.Add("MODKIT_DELIVERY_PUBLIC_KEY=\"" + PackageKey + "\"");
			PrivateDefinitions.Add("MODKIT_DELIVERY_KEY_ID=\"" + PackageKeyId + "\"");


			// Runtime owns ModuleCaches; ModKit stages only the manifest-declared set.
			if (Target.ProjectFile != null)
			{
				string Manifest = Path.Combine(Target.ProjectFile.Directory.FullName, "Script", "ModuleCaches", "ModuleCaches.json");
				ExternalDependencies.Add(Manifest);
				if (File.Exists(Manifest))
				{
					RuntimeDependencies.Add("$(ProjectDir)/Script/ModuleCaches/ModuleCaches.json", StagedFileType.UFS);
					foreach (JsonObject Module in JsonObject.Read(new FileReference(Manifest)).GetObjectArrayField("modules"))
					{
						string Filename = Module.GetStringField("file");
						if (string.IsNullOrEmpty(Filename) || Filename == "." || Filename == ".." || Filename.IndexOfAny(new[] {'/', '\\', ':'}) >= 0)
							throw new BuildException("Invalid FeatureMod module cache filename");
						RuntimeDependencies.Add("$(ProjectDir)/Script/ModuleCaches/" + Filename, StagedFileType.UFS);
						RuntimeDependencies.Add("$(ProjectDir)/Script/ModuleCaches/" + Filename + ".cuviss-cache-v2", StagedFileType.UFS);
					}
				}
			}

			// Built-in FeatureMods must remain discoverable through the packaged file layer.
			if (Target.ProjectFile != null)
			{
				string Root = Path.Combine(Target.ProjectFile.Directory.FullName, "Plugins", "FeatureMods");
				if (Directory.Exists(Root)) foreach (string Unit in Directory.GetDirectories(Root))
				{
					string Descriptor = Path.Combine(Unit, Path.GetFileName(Unit) + ".uplugin");
					if (!File.Exists(Descriptor) || !JsonObject.Read(new FileReference(Descriptor)).TryGetBoolField("FeatureMod", out bool IsFeatureMod) || !IsFeatureMod)
						continue;
					RuntimeDependencies.Add(Descriptor, StagedFileType.UFS);
					RuntimeDependencies.Add(Path.Combine(Unit, "FeatureMod.json"), StagedFileType.UFS);
					foreach (string DirectoryName in new[] { "Script", "Config" })
					{
						string Payload = Path.Combine(Unit, DirectoryName);
						if (Directory.Exists(Payload))
							RuntimeDependencies.Add(Path.Combine(Payload, "..."), StagedFileType.UFS);
					}
				}
			}

			PublicIncludePaths.Add(ModuleDirectory + "/Public");
			PrivateIncludePaths.Add(ModuleDirectory + "/Private");

			PublicDependencyModuleNames.AddRange(new string[]
			{
				"Core",
				"CoreUObject",
				"Engine"
			});

PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"GameFeatures",
			"ModularGameplay",
			"Json",
			                                                                 
			// HTTP module; the ops fixture chain uses file:// feeds only.
			"HTTP",
			                                                                
			// Ed25519 verify-only inside ModKitPatchSignature.cpp (TweetNaCl-
			// derived, public domain). The UE-AS fork 5.7 fork has no Crypto module /
			// FEncryptionContext and its Runtime/RSA module FRSA::DecryptPublic
			// is unimplemented (red gate 8 fallback), so no engine crypto
			// module is linked at all.
			"AngelscriptCode"
		});

			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				PublicSystemLibraries.Add("bcrypt.lib");
			}

			if (Target.bBuildEditor)
			{
				// Validation commandlets (asset audit) use the AssetRegistry.
				PrivateDependencyModuleNames.Add("AssetRegistry");
			}
		}
	}
}
