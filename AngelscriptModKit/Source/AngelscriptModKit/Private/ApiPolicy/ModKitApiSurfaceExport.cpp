// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned public API surface export commandlet (-run=ModKitExportApiSurface).

#include "ApiPolicy/ModKitApiSurfaceExport.h"

#include "ApiPolicy/ModKitApiAllowlist.h"
#include "FeatureMods/ModKitCompilationManager.h"
#include "Core/ModKitSettings.h"

#if WITH_EDITOR
#include "AngelscriptBindDatabase.h"
#include "AngelscriptManager.h"
#include "AngelscriptOptionalProviders.h"
#include "Misc/ScopeExit.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Containers/StringConv.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectGlobals.h"

#include "angelscript.h"
#endif

#if WITH_EDITOR
namespace
{
	struct FExportFaceEntry
	{
		FString Declaration;
		FString Pattern;
		FString MatchedBy;
	};

	// Engine-primary enumeration (design card 1.5): global system functions,
	// native object type methods/behaviours (system calls + override
	// candidates) and native object type names (base-type candidates). Script
	// object types, enums, funcdefs and typedefs are excluded so the surface
	// is the native (bound) API the runtime audit sees.
	void ExportEnumerateEngineSurface(
		asIScriptEngine* Engine,
		TArray<FString>& OutSystemCallDeclarations,
		TArray<FString>& OutBaseTypeNames,
		TArray<FString>& OutOverrideDeclarations)
	{
		const asUINT GlobalCount = Engine->GetGlobalFunctionCount();
		for (asUINT Index = 0; Index < GlobalCount; ++Index)
		{
			asIScriptFunction* Function = Engine->GetGlobalFunctionByIndex(Index);
			if (Function != nullptr && Function->GetFuncType() == asFUNC_SYSTEM)
			{
				OutSystemCallDeclarations.Add(UTF8_TO_TCHAR(Function->GetDeclaration(true, true)));
			}
		}

		const asUINT TypeCount = Engine->GetObjectTypeCount();
		for (asUINT TypeIndex = 0; TypeIndex < TypeCount; ++TypeIndex)
		{
			asITypeInfo* Type = Engine->GetObjectTypeByIndex(TypeIndex);
			if (Type == nullptr)
			{
				continue;
			}
			const asDWORD Flags = Type->GetFlags();
			if ((Flags & (asOBJ_SCRIPT_OBJECT | asOBJ_ENUM | asOBJ_FUNCDEF | asOBJ_TYPEDEF)) != 0
				|| (Flags & (asOBJ_REF | asOBJ_VALUE)) == 0)
			{
				continue;
			}

			OutBaseTypeNames.Add(UTF8_TO_TCHAR(Type->GetName()));

			const asUINT MethodCount = Type->GetMethodCount();
			for (asUINT MethodIndex = 0; MethodIndex < MethodCount; ++MethodIndex)
			{
				asIScriptFunction* Method = Type->GetMethodByIndex(MethodIndex);
				if (Method != nullptr && Method->GetFuncType() == asFUNC_SYSTEM)
				{
					const FString Declaration = UTF8_TO_TCHAR(Method->GetDeclaration(true, true));
					OutSystemCallDeclarations.Add(Declaration);
					OutOverrideDeclarations.Add(Declaration);
				}
			}

			const asUINT BehaviourCount = Type->GetBehaviourCount();
			for (asUINT BehaviourIndex = 0; BehaviourIndex < BehaviourCount; ++BehaviourIndex)
			{
				asIScriptFunction* Behaviour = Type->GetBehaviourByIndex(BehaviourIndex, nullptr);
				if (Behaviour != nullptr && Behaviour->GetFuncType() == asFUNC_SYSTEM)
				{
					OutSystemCallDeclarations.Add(UTF8_TO_TCHAR(Behaviour->GetDeclaration(true, true)));
				}
			}
		}
	}

	// Filters one face's candidates with its allowlist patterns, records the
	// first matching pattern per declaration (pattern-order deterministic) and
	// collects patterns that hit nothing.
	void ExportFilterFace(const TArray<FString>& Candidates, const TArray<FString>& Patterns,
		const UModKitSettings& Settings, FModKitApiAllowlist::EApiFace Face,
		TArray<FExportFaceEntry>& OutEntries, TArray<FString>& OutUnmatchedPatterns)
	{
		TArray<FString> Unmatched = Patterns;
		for (const FString& Declaration : Candidates)
		{
			FString Reason;
			if (!FModKitApiAllowlist::IsAllowed(Declaration, Face, Settings, &Reason))
			{
				continue;
			}
			bool bPatternMatched = false;
			for (const FString& Pattern : Patterns)
			{
				if (FModKitApiAllowlist::MatchDeclaration(Declaration, Pattern))
				{
					OutEntries.Add(FExportFaceEntry{ Declaration, Pattern, Reason });
					Unmatched.Remove(Pattern);
					bPatternMatched = true;
					break;
				}
			}
			if (!bPatternMatched)
			{
				OutEntries.Add(FExportFaceEntry{ Declaration, FString(), Reason });
			}
		}

		OutUnmatchedPatterns.Reserve(OutUnmatchedPatterns.Num() + Unmatched.Num());
		for (const FString& Pattern : Unmatched)
		{
			OutUnmatchedPatterns.Add(Pattern);
		}
	}

	void ExportSortAndUniqueEntries(TArray<FExportFaceEntry>& Entries)
	{
		Entries.Sort([](const FExportFaceEntry& A, const FExportFaceEntry& B)
		{
			if (A.Declaration != B.Declaration)
			{
				return A.Declaration < B.Declaration;
			}
			if (A.Pattern != B.Pattern) return A.Pattern < B.Pattern;
			return A.MatchedBy < B.MatchedBy;
		});

		TArray<FExportFaceEntry> Unique;
		Unique.Reserve(Entries.Num());
		for (const FExportFaceEntry& Entry : Entries)
		{
			if (Unique.Num() == 0
				|| Unique.Last().Declaration != Entry.Declaration
			|| Unique.Last().Pattern != Entry.Pattern
			|| Unique.Last().MatchedBy != Entry.MatchedBy)
			{
				Unique.Add(Entry);
			}
		}
		Entries = MoveTemp(Unique);
	}

	void ExportSortAndUniqueStrings(TArray<FString>& Declarations)
	{
		Declarations.Sort();

		TArray<FString> Unique;
		Unique.Reserve(Declarations.Num());
		for (const FString& Declaration : Declarations)
		{
			if (Unique.Num() == 0 || Unique.Last() != Declaration)
			{
				Unique.Add(Declaration);
			}
		}
		Declarations = MoveTemp(Unique);
	}

	TArray<TSharedPtr<FJsonValue>> ExportBuildFaceJson(const TArray<FExportFaceEntry>& Entries)
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		for (const FExportFaceEntry& Entry : Entries)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("declaration"), Entry.Declaration);
			Object->SetStringField(TEXT("pattern"), Entry.Pattern);
			Object->SetStringField(TEXT("matchedBy"), Entry.MatchedBy);
			JsonArray.Add(MakeShared<FJsonValueObject>(Object));
		}
		return JsonArray;
	}

	// Canonical snapshot digest over the FAngelscriptBindDatabase classes and
	// structs surfaces (the fallback/authoritative binding list, same source as
	// the shipped Binds.Cache). Sorted before hashing so the digest is
	// registration-order independent within a fixed engine version.
	FString ExportComputeBindDatabaseDigest()
	{
		TArray<FString> Lines;
		const FAngelscriptBindDatabase& BindDatabase = FAngelscriptBindDatabase::Get();

		for (const FAngelscriptClassBind& Class : BindDatabase.Classes)
		{
			Lines.Add(FString(TEXT("C:")) + Class.TypeName);
			for (const FAngelscriptMethodBind& Method : Class.Methods)
			{
				Lines.Add(FString(TEXT("CM:")) + Method.Declaration);
			}
			for (const FAngelscriptPropertyBind& Property : Class.Properties)
			{
				Lines.Add(FString(TEXT("CP:")) + Property.Declaration);
			}
		}
		for (const FAngelscriptStructBind& Struct : BindDatabase.Structs)
		{
			Lines.Add(FString(TEXT("S:")) + Struct.TypeName);
			for (const FAngelscriptPropertyBind& Property : Struct.Properties)
			{
				Lines.Add(FString(TEXT("SP:")) + Property.Declaration);
			}
		}

		Lines.Sort();
		const FString Canonical = FString::Join(Lines, TEXT("\n"));

		uint8 Hash[20];
		FSHA1::HashBuffer(Canonical.GetCharArray().GetData(), Canonical.Len() * sizeof(TCHAR), Hash);

		FString Digest;
		for (int32 Index = 0; Index < 20; ++Index)
		{
			Digest += FString::Printf(TEXT("%02x"), Hash[Index]);
		}
		return Digest;
	}

	FString ExportEngineVersionString()
	{
		const FEngineVersion& Version = FEngineVersion::Current();
		return FString::Printf(TEXT("%d.%d.%d-%d %s"), Version.GetMajor(), Version.GetMinor(),
			Version.GetPatch(), Version.GetChangelist(), *Version.GetBranch());
	}

	FString ExportPolicyDigest(const UModKitSettings& Settings,
		const TArray<FExportFaceEntry>& Calls, const TArray<FExportFaceEntry>& Bases,
		const TArray<FExportFaceEntry>& Overrides)
	{
		// Include the effective classified surface so provider refresh/removal changes
		// the digest even when project configuration and binding bytes stay identical.
		FString Canonical;
		const auto Add = [&Canonical](const FString& Value)
		{
			Canonical += FString::Printf(TEXT("%d:"), Value.Len()) + Value;
		};
		for (const auto* Values : {&Settings.DeniedApiPatterns, &Settings.DeniedBaseClassPatterns,
			&Settings.DeniedOverridePatterns})
		{
			Add(FString::FromInt(Values->Num()));
			for (const FString& Value : *Values) Add(Value);
		}
		Add(TEXT("official-trusted"));
		for (const auto* Entries : {&Calls, &Bases, &Overrides})
		{
			Add(FString::FromInt(Entries->Num()));
			for (const auto& Entry : *Entries)
			{
				Add(Entry.Declaration);
				Add(Entry.MatchedBy);
			}
		}
		uint8 Hash[20];
		FTCHARToUTF8 Utf8(*Canonical);
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
		FString Digest;
		for (const uint8 Byte : Hash) Digest += FString::Printf(TEXT("%02x"), Byte);
		return Digest;
	}
}
#endif

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModKitApiExportPolicyTest,
	"AngelscriptModKit.Allowlist.PolicyContract.Export", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FModKitApiExportPolicyTest::RunTest(const FString& Parameters)
{
	UModKitSettings* Settings = NewObject<UModKitSettings>();
	const FString Key = TEXT("void __ModKitExportPolicy::Call()");
	using namespace AngelscriptCoreExtensionPoints;
    auto* Engine = FAngelscriptManager::Get().GetScriptEngine();
    FApiMetadata Metadata {TEXT("core"), TEXT("Core"), {}};
    FDelegateHandle ProducerA, ProducerB;
    ON_SCOPE_EXIT
    {
        if (ProducerB.IsValid()) UnregisterApiMetadataProvider(ProducerB);
        if (ProducerA.IsValid()) UnregisterApiMetadataProvider(ProducerA);
    };
    ProducerA = RegisterApiMetadataProvider(Engine, [Key](FStringView Declaration, FApiMetadata& Out)
    {
        if (Declaration != FStringView(Key)) return EApiMetadataResult::Unknown;
        Out = {TEXT("core"), TEXT("Core"), {}};
        return EApiMetadataResult::Classified;
    });
    if (!TestTrue(TEXT("Register export metadata provider"), ProducerA.IsValid())) return false;
	TArray<FExportFaceEntry> Entries;
	TArray<FString> Unmatched;
	ExportFilterFace({Key}, {}, *Settings, FModKitApiAllowlist::EApiFace::SystemCall, Entries, Unmatched);
	TestEqual(TEXT("export consumes registry core classification"), Entries.Num(), 1);
	const FString CoreDigest = ExportPolicyDigest(*Settings, Entries, {}, {});
	TestEqual(TEXT("effective policy digest is deterministic"), CoreDigest, ExportPolicyDigest(*Settings, Entries, {}, {}));
	Metadata.Tier = TEXT("internal");
    ProducerB = RegisterApiMetadataProvider(Engine, [&](FStringView Declaration, FApiMetadata& Out)
    {
        if (Declaration != FStringView(Key)) return EApiMetadataResult::Unknown;
        Out = Metadata;
        return EApiMetadataResult::Classified;
    });
    if (!TestTrue(TEXT("Register export conflict"), ProducerB.IsValid())) return false;
	Entries.Reset();
	Settings->AllowedApiPatterns = {TEXT("*")};
	ExportFilterFace({Key}, Settings->AllowedApiPatterns, *Settings, FModKitApiAllowlist::EApiFace::SystemCall, Entries, Unmatched);
	TestTrue(TEXT("conflicting provider is excluded despite wildcard"), Entries.IsEmpty());
	TestTrue(TEXT("provider conflict changes policy digest"), CoreDigest != ExportPolicyDigest(*Settings, Entries, {}, {}));
	TestTrue(TEXT("Remove export conflict"), UnregisterApiMetadataProvider(ProducerB) == ERemoveResult::Removed);
    ProducerB.Reset();
	Settings->AllowedApiPatterns.Reset();
	Settings->DeniedApiPatterns = {Key};
	ExportFilterFace({Key}, {}, *Settings, FModKitApiAllowlist::EApiFace::SystemCall, Entries, Unmatched);
	TestTrue(TEXT("export applies explicit deny before core"), Entries.IsEmpty());
	Settings->DeniedApiPatterns.Reset();
	TestTrue(TEXT("Remove export metadata"), UnregisterApiMetadataProvider(ProducerA) == ERemoveResult::Removed);
    ProducerA.Reset();
	ExportFilterFace({Key}, {}, *Settings, FModKitApiAllowlist::EApiFace::SystemCall, Entries, Unmatched);
	TestEqual(TEXT("missing provider is open"), Entries.Num(), 1);
	Settings->AllowedApiTiers.Reset();
	const FString ClosedDigest = ExportPolicyDigest(*Settings, Entries, {}, {});
	Settings->bApplyAllowlistToOfficialMods = true;
	TestTrue(TEXT("legacy official policy switch has no effect"), ClosedDigest == ExportPolicyDigest(*Settings, Entries, {}, {}));
	return true;
}

#endif

int32 UModKitExportApiSurfaceCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
	// --- CLI contract --------------------------------------------------------
	FString OutDir;
	FParse::Value(*Params, TEXT("OutDir="), OutDir);
	OutDir.TrimQuotesInline();
	if (OutDir.IsEmpty())
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_API_EXPORT_CONFIG reason=missing-outdir"));
		return 2;
	}
	OutDir = FPaths::ConvertRelativePathToFull(OutDir);

	asIScriptEngine* Engine = FAngelscriptManager::Get().GetScriptEngine();
	if (Engine == nullptr)
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_API_EXPORT_CONFIG reason=engine-unavailable"));
		return 2;
	}

	const UModKitSettings* Settings = UModKitSettings::Get();
	FlushAsyncLoading();
	FModKitCompilationManager::MaybeRunAppend();
	const FString EngineVersion = ExportEngineVersionString();
	const FString BindDigest = ExportComputeBindDatabaseDigest();
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_API_EXPORT_BEGIN engine=%s bindDigest=%s"),
		*EngineVersion, *BindDigest);

	// --- Enumerate + align to the three faces + filter -----------------------
	TArray<FString> SystemCallCandidates;
	TArray<FString> BaseTypeCandidates;
	TArray<FString> OverrideCandidates;
	ExportEnumerateEngineSurface(Engine, SystemCallCandidates, BaseTypeCandidates, OverrideCandidates);

	TArray<FExportFaceEntry> SystemCallEntries;
	TArray<FExportFaceEntry> BaseTypeEntries;
	TArray<FExportFaceEntry> OverrideEntries;
	TArray<FString> UnmatchedPatterns;
	ExportFilterFace(SystemCallCandidates, {}, *Settings, FModKitApiAllowlist::EApiFace::SystemCall, SystemCallEntries, UnmatchedPatterns);
	ExportFilterFace(BaseTypeCandidates, {}, *Settings, FModKitApiAllowlist::EApiFace::BaseType, BaseTypeEntries, UnmatchedPatterns);
	ExportFilterFace(OverrideCandidates, {}, *Settings, FModKitApiAllowlist::EApiFace::Override, OverrideEntries, UnmatchedPatterns);

	ExportSortAndUniqueEntries(SystemCallEntries);
	ExportSortAndUniqueEntries(BaseTypeEntries);
	ExportSortAndUniqueEntries(OverrideEntries);
	ExportSortAndUniqueStrings(UnmatchedPatterns);
	const FString PolicyDigest = ExportPolicyDigest(*Settings, SystemCallEntries, BaseTypeEntries, OverrideEntries);

	for (const FString& Pattern : UnmatchedPatterns)
	{
		UE_LOG(LogAngelscriptModKit, Warning, TEXT("MODKIT_API_EXPORT_WARNING pattern=%s hits=0"), *Pattern);
	}

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_API_EXPORT_SUMMARY systemCall=%d baseType=%d override=%d unmatched=%d"),
		SystemCallEntries.Num(), BaseTypeEntries.Num(), OverrideEntries.Num(), UnmatchedPatterns.Num());

	// --- Self-consistency pass (every export re-checks its recorded pattern) -
	const int32 TotalEntries = SystemCallEntries.Num() + BaseTypeEntries.Num() + OverrideEntries.Num();
	for (const FExportFaceEntry& Entry : SystemCallEntries)
	{
		check(FModKitApiAllowlist::IsAllowed(Entry.Declaration, FModKitApiAllowlist::EApiFace::SystemCall, *Settings));
	}
	for (const FExportFaceEntry& Entry : BaseTypeEntries)
	{
		check(FModKitApiAllowlist::IsAllowed(Entry.Declaration, FModKitApiAllowlist::EApiFace::BaseType, *Settings));
	}
	for (const FExportFaceEntry& Entry : OverrideEntries)
	{
		check(FModKitApiAllowlist::IsAllowed(Entry.Declaration, FModKitApiAllowlist::EApiFace::Override, *Settings));
	}
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_API_EXPORT_SELF_CONSISTENT count=%d"), TotalEntries);

	// --- Write outputs -------------------------------------------------------
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*OutDir))
	{
		FileManager.MakeDirectory(*OutDir, true);
	}

	// PublicApi.as: sorted declaration reference per face.
	FString AsText;
	AsText += TEXT("// PublicApi.as\n");
	AsText += TEXT("// Generated by ModKitExportApiSurface (-run=ModKitExportApiSurface -OutDir=).\n");
	AsText += FString::Printf(TEXT("// engineVersion: %s\n"), *EngineVersion);
	AsText += FString::Printf(TEXT("// bindDatabaseDigest: %s\n"), *BindDigest);
	AsText += FString::Printf(TEXT("// policyDigest: %s\n"), *PolicyDigest);
	AsText += TEXT("// Allowlisted declaration reference. The data source and matcher are the same\n");
	AsText += TEXT("// as the runtime ApiAllowlistAudit: UModKitSettings policy x\n");
	AsText += TEXT("// FModKitApiAllowlist::IsAllowed. Sorted and deterministic per engine\n");
	AsText += TEXT("// version; regenerate on engine/binding updates.\n");

	const TArray<TArray<FExportFaceEntry>*> FaceGroups = { &SystemCallEntries, &BaseTypeEntries, &OverrideEntries };
	const TArray<FString> FaceNames = { TEXT("systemCall"), TEXT("baseType"), TEXT("override") };
	for (int32 FaceIndex = 0; FaceIndex < FaceGroups.Num(); ++FaceIndex)
	{
		AsText += FString::Printf(TEXT("// face: %s\n"), *FaceNames[FaceIndex]);
		TArray<FString> Declarations;
		for (const FExportFaceEntry& Entry : *FaceGroups[FaceIndex])
		{
			Declarations.Add(Entry.Declaration);
		}
		ExportSortAndUniqueStrings(Declarations);
		for (const FString& Declaration : Declarations)
		{
			AsText += Declaration + TEXT("\n");
		}
	}

	const FString PublicApiPath = OutDir / TEXT("PublicApi.as");
	if (!FFileHelper::SaveStringToFile(AsText, *PublicApiPath))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_API_EXPORT_CONFIG reason=write-failed"));
		return 2;
	}

	// ApiSurface.json: machine-readable manifest with engine version + digest.
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 1);
	Root->SetStringField(TEXT("tool"), TEXT("ModKitExportApiSurface"));
	Root->SetStringField(TEXT("engineVersion"), EngineVersion);

	TSharedPtr<FJsonObject> VersionObject = MakeShared<FJsonObject>();
	const FEngineVersion& CurrentVersion = FEngineVersion::Current();
	VersionObject->SetNumberField(TEXT("major"), CurrentVersion.GetMajor());
	VersionObject->SetNumberField(TEXT("minor"), CurrentVersion.GetMinor());
	VersionObject->SetNumberField(TEXT("patch"), CurrentVersion.GetPatch());
	VersionObject->SetNumberField(TEXT("changelist"), CurrentVersion.GetChangelist());
	VersionObject->SetStringField(TEXT("branch"), CurrentVersion.GetBranch());
	Root->SetObjectField(TEXT("engineVersionInfo"), VersionObject);
	Root->SetStringField(TEXT("bindDatabaseDigest"), BindDigest);
	Root->SetStringField(TEXT("policyDigest"), PolicyDigest);

	TSharedPtr<FJsonObject> Faces = MakeShared<FJsonObject>();
	Faces->SetArrayField(TEXT("systemCall"), ExportBuildFaceJson(SystemCallEntries));
	Faces->SetArrayField(TEXT("baseType"), ExportBuildFaceJson(BaseTypeEntries));
	Faces->SetArrayField(TEXT("override"), ExportBuildFaceJson(OverrideEntries));
	Root->SetObjectField(TEXT("faces"), Faces);

	TArray<TSharedPtr<FJsonValue>> UnmatchedJson;
	for (const FString& Pattern : UnmatchedPatterns)
	{
		UnmatchedJson.Add(MakeShared<FJsonValueString>(Pattern));
	}
	Root->SetArrayField(TEXT("unmatchedPatterns"), UnmatchedJson);

	FString JsonText;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_API_EXPORT_CONFIG reason=serialize-failed"));
		return 2;
	}

	const FString JsonPath = OutDir / TEXT("ApiSurface.json");
	if (!FFileHelper::SaveStringToFile(JsonText, *JsonPath))
	{
		UE_LOG(LogAngelscriptModKit, Error, TEXT("MODKIT_API_EXPORT_CONFIG reason=write-failed"));
		return 2;
	}

	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_API_EXPORT_WRITTEN publicApi=%s json=%s"), *PublicApiPath, *JsonPath);
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_API_EXPORT_DONE"));
	return 0;
#else
	UE_LOG(LogAngelscriptModKit, Log, TEXT("MODKIT_API_EXPORT_SKIPPED reason=non-editor-target"));
	return 0;
#endif
}
