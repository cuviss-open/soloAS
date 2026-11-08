#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "Core/SoloASCoreExtensionsModule.h"

#include "Shared/ExtensionRegistration.h"
#include "Core/RemovedScriptClassReclamation.h"
#include "Lambda/ExplicitCaptureLambdaExtension.h"
#include "Interfaces/ScriptInterfacesExtension.h"
#if WITH_EDITOR && __has_include("BlueprintInheritance/BlueprintQuickCallEditor.h")
#include "BlueprintInheritance/BlueprintQuickCallEditor.h"
#define SOLOAS_HAS_QUICK_CALL_EDITOR 1
#else
#define SOLOAS_HAS_QUICK_CALL_EDITOR 0
#endif
#if __has_include("BlueprintStructs/BlueprintStructsExtension.h")
#include "BlueprintStructs/BlueprintStructsExtension.h"
#define SOLOAS_HAS_BLUEPRINT_STRUCTS 1
#else
#define SOLOAS_HAS_BLUEPRINT_STRUCTS 0
#endif

#include "AngelscriptExecutionQuiescence.h"
#include "Extensions/AngelscriptExtensionHooks.h"

#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"

namespace SoloASBridge::Private
{
void InitializeLambdaClosures();
void ShutdownLambdaClosures();
}

DEFINE_LOG_CATEGORY_STATIC(LogSoloASCoreExtensions, Log, All);

namespace
{
FDelegateHandle LambdaClosurePreExitHandle;
bool bLambdaClosuresShutdownAtPreExit = false;

void ShutdownBridgeLambdaClosuresPreExit()
{
    checkf(
        IsInGameThread(),
        TEXT("lambda closure pre-exit shutdown requires the game thread")
    );
    if (!bLambdaClosuresShutdownAtPreExit)
    {
        SoloASBridge::Private::ShutdownLambdaClosures();
        bLambdaClosuresShutdownAtPreExit = true;
    }
}
}

FSoloASCoreExtensionsModule::FSoloASCoreExtensionsModule() = default;

FSoloASCoreExtensionsModule::~FSoloASCoreExtensionsModule() = default;

void FSoloASCoreExtensionsModule::StartupModule()
{
	checkf(IsInGameThread(), TEXT("SoloAS Extensions must start on the game thread."));
	FModuleManager::Get().LoadModuleChecked(TEXT("AngelscriptCode"));
    SoloASCoreExtensions::BlueprintStructs::StartBlueprintStructs();
    SoloASCoreExtensions::StartRemovedScriptClassReclamation();
    bLambdaClosuresShutdownAtPreExit = false;
    if (SoloASExplicitCaptureLambdaPrivate::IsEnabled())
    {
        SoloASBridge::Private::InitializeLambdaClosures();
        SoloASExplicitCaptureLambdaPrivate::ResetStorageMetadata();
        SoloASExplicitCaptureLambdaPrivate::RefreshStorageMetadata();
        LambdaClosurePreExitHandle = FCoreDelegates::OnEnginePreExit.AddStatic(
            &ShutdownBridgeLambdaClosuresPreExit);
    }
    else
    {
        bLambdaClosuresShutdownAtPreExit = true;
    }
    const auto& Features = SoloASCoreExtensions::ExtensionRegistration::GetFeaturePolicy();
    UE_LOG(LogSoloASCoreExtensions, Display,
        TEXT("Core extensions startup: interfaces=%s, lambda=%s, blueprintStructs=%s, nativeEventSuper=%s, "
             "blueprintInheritance=%s, blueprintQuickCalls=%s; settings take effect after restart."),
        SoloASCoreExtensions::ScriptInterfaces::IsEnabled() ? TEXT("on") : TEXT("off"),
        SoloASExplicitCaptureLambdaPrivate::IsEnabled() ? TEXT("on") : TEXT("off"),
        Features.bBlueprintStructs ? TEXT("on") : TEXT("off"),
        Features.bNativeEventSuper ? TEXT("on") : TEXT("off"),
        Features.bBlueprintInheritance ? TEXT("on") : TEXT("off"),
        Features.bBlueprintQuickCalls ? TEXT("on") : TEXT("off"));


	if (ExtensionRegistration.IsValid())
	{
		UE_LOG(
			LogSoloASCoreExtensions,
			Fatal,
			TEXT("SoloAS Extensions attempted to register its provider twice.")
		);
		return;
	}

	SoloASCoreExtensions::ExtensionRegistration::FExtensionFactorySnapshot Snapshot;
	FString Error;
	if (!SoloASCoreExtensions::ExtensionRegistration::
		SnapshotAndFreezeExtensionFactories(Snapshot, Error))
	{
		UE_LOG(
			LogSoloASCoreExtensions,
			Fatal,
			TEXT("SoloAS extension factory snapshot failed: %s"),
			*Error
		);
		return;
	}
	if (Snapshot.Entries.IsEmpty())
		return;

	FAngelscriptExtensionProvider Provider;
	Provider.ProviderId = FName(TEXT("SoloASCoreExtensions"));
	Provider.CompatibilityTag = Snapshot.CompatibilityTag;

	TUniquePtr<FSoloASCoreExtensionsRegistration> Candidate =
		MakeUnique<FSoloASCoreExtensionsRegistration>();
	{
		FAngelscriptExecutionWriteScope ExecutionWriteScope;
		if (!RegisterAngelscriptExtensionProvider(
			Provider,
			Candidate->ProviderRegistration,
			Error))
		{
			UE_LOG(
				LogSoloASCoreExtensions,
				Fatal,
				TEXT("SoloAS Extensions provider registration failed: %s"),
				*Error
			);
			return;
		}

		Candidate->Factories = MoveTemp(Snapshot);
        if (!SoloASCoreExtensions::ExtensionRegistration::RegisterExtensionCallbacks(
            Candidate->Factories, Candidate->Tokens, Error))
        {
            const FString RegistrationError = Error;
            if (Candidate->Tokens.IsEmpty())
                UnregisterAngelscriptExtensionProvider(Candidate->ProviderRegistration, Error);
            UE_LOG(LogSoloASCoreExtensions, Fatal,
                TEXT("SoloAS Extensions callback registration failed: %s; %s"), *RegistrationError, *Error);
            return;
        }
	}
	ExtensionRegistration = MoveTemp(Candidate);
#if SOLOAS_HAS_BLUEPRINT_STRUCTS
    if (SoloASCoreExtensions::ExtensionRegistration::GetFeaturePolicy().bBlueprintStructs)
        SoloASCoreExtensions::BlueprintStructs::RegisterBlueprintStructCookDependencies();
#endif
#if SOLOAS_HAS_QUICK_CALL_EDITOR
    if (SoloASCoreExtensions::ExtensionRegistration::GetFeaturePolicy().bBlueprintQuickCalls)
        SoloASCoreExtensions::BlueprintInheritance::StartQuickCallEditor();
#endif
    UE_LOG(LogSoloASCoreExtensions, Display,
        TEXT("Core extension retained registrations: provider=1 callbacks=%d callbackTokenBytes=%llu "
             "factorySnapshotBytes=%llu lambdaClosureLifecycle=%s. Counts exclude VM bindings and feature metadata."),
        ExtensionRegistration->Tokens.Num(), uint64(ExtensionRegistration->Tokens.GetAllocatedSize()),
        uint64(ExtensionRegistration->Factories.Entries.GetAllocatedSize()),
        bLambdaClosuresShutdownAtPreExit ? TEXT("off") : TEXT("on"));
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
	if (Features.bBlueprintInheritance || SoloASCoreExtensions::ScriptInterfaces::IsEnabled())
        SoloASCoreExtensions::ScriptInterfaces::StartBlueprintRelink();
#endif
}

void FSoloASCoreExtensionsModule::ShutdownModule()
{
    SoloASCoreExtensions::StopRemovedScriptClassReclamation();
    SoloASCoreExtensions::BlueprintStructs::StopBlueprintStructs();
#if SOLOAS_HAS_QUICK_CALL_EDITOR
    SoloASCoreExtensions::BlueprintInheritance::StopQuickCallEditor();
#endif
#if SOLOAS_HAS_BLUEPRINT_STRUCTS
    SoloASCoreExtensions::BlueprintStructs::UnregisterBlueprintStructCookDependencies();
#endif
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
	SoloASCoreExtensions::ScriptInterfaces::StopBlueprintRelink();
#endif
	checkf(IsInGameThread(), TEXT("SoloAS Extensions must stop on the game thread."));
    SoloASExplicitCaptureLambdaPrivate::ResetStorageMetadata();
    if (LambdaClosurePreExitHandle.IsValid())
    {
        FCoreDelegates::OnEnginePreExit.Remove(LambdaClosurePreExitHandle);
        LambdaClosurePreExitHandle.Reset();
    }
    if (!bLambdaClosuresShutdownAtPreExit)
    {
        SoloASBridge::Private::ShutdownLambdaClosures();
        bLambdaClosuresShutdownAtPreExit = true;
    }
	if (!ExtensionRegistration.IsValid())
		return;

	FAngelscriptExecutionWriteScope ExecutionWriteScope;
	FString Error;
	if (!SoloASCoreExtensions::ExtensionRegistration::UnregisterExtensionCallbacksReverse(
			ExtensionRegistration->Tokens,
			Error))
	{
		UE_LOG(
			LogSoloASCoreExtensions,
			Fatal,
			TEXT("SoloAS Extensions callback unregistration failed: %s"),
			*Error
		);
		return;
	}
	Error.Reset();
	if (!UnregisterAngelscriptExtensionProvider(
		ExtensionRegistration->ProviderRegistration,
		Error))
	{
		UE_LOG(
			LogSoloASCoreExtensions,
			Fatal,
			TEXT("SoloAS Extensions provider unregistration failed: %s"),
			*Error
		);
		return;
	}
	ExtensionRegistration.Reset();
}

IMPLEMENT_MODULE(FSoloASCoreExtensionsModule, SoloASCoreExtensions)

#else
#include "Core/SoloASCoreExtensionsModule.h"
#include "BlueprintStructs/BlueprintStructsExtension.h"
#include "AngelscriptCodeModule.h"
namespace SoloASCoreExtensions { void StartPackagingProducer(); void StopPackagingProducer(); }
#include "Core/RemovedScriptClassReclamation.h"
#include "Lambda/ExplicitCaptureLambdaExtension.h"
#include "NativeEventSuper/NativeEventSuperExtension.h"
#include "Interfaces/ScriptInterfacesExtension.h"
#include "BlueprintInheritance/BlueprintQuickCallsExtension.h"
#include "BlueprintInheritance/BlueprintQuickCallEditor.h"
#include "Modules/ModuleManager.h"
FSoloASCoreExtensionsModule::FSoloASCoreExtensionsModule() = default;
FSoloASCoreExtensionsModule::~FSoloASCoreExtensionsModule() = default;
void FSoloASCoreExtensionsModule::StartupModule()
{
    FModuleManager::Get().LoadModuleChecked(TEXT("AngelscriptCode"));
    SoloASCoreExtensions::BlueprintStructs::StartBlueprintStructs();
    SoloASCoreExtensions::StartRemovedScriptClassReclamation();
    SoloASCoreExtensions::BlueprintInheritance::StartQuickCalls();
    SoloASCoreExtensions::BlueprintInheritance::StartBlueprintImports();
    // Interface conversion rebuilds chunks before the native-super consumer rewrites them.
    SoloASCoreExtensions::ScriptInterfaces::Start();
    SoloASCoreExtensions::NativeEventSuper::Start();
    SoloASCoreExtensions::Algorithms::Start();
    SoloASExplicitCaptureLambdaPrivate::Start();
    SoloASCoreExtensions::StartPackagingProducer();
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
    StartExplicitFullReloadTransaction();
#endif
#if WITH_EDITOR
    if (SoloASCoreExtensions::BlueprintInheritance::AreQuickCallsEnabled()) SoloASCoreExtensions::BlueprintInheritance::StartQuickCallEditor();
#endif
    UE_LOG(LogTemp, Display, TEXT("CoreExtensions public features: quickCalls=%s algorithms=%s nativeEventSuper=%s; settings require restart."),
        SoloASCoreExtensions::BlueprintInheritance::AreQuickCallsEnabled() ? TEXT("on") : TEXT("off"),
        SoloASCoreExtensions::Algorithms::IsEnabled() ? TEXT("on") : TEXT("off"),
        SoloASCoreExtensions::NativeEventSuper::IsEnabled() ? TEXT("on") : TEXT("off"));
    UE_LOG(LogTemp, Display, TEXT("CoreExtensions: script interface provider=%s."), SoloASCoreExtensions::ScriptInterfaces::IsProviderRegistered() ? TEXT("on") : TEXT("off"));
}
void FSoloASCoreExtensionsModule::ShutdownModule()
{
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
    StopExplicitFullReloadTransaction();
#endif
#if WITH_EDITOR
    SoloASCoreExtensions::BlueprintInheritance::StopQuickCallEditor();
#endif
    SoloASCoreExtensions::StopPackagingProducer();
    SoloASExplicitCaptureLambdaPrivate::Stop();
    SoloASCoreExtensions::Algorithms::Stop();
    SoloASCoreExtensions::NativeEventSuper::Stop();
    SoloASCoreExtensions::ScriptInterfaces::Stop();
    SoloASCoreExtensions::BlueprintInheritance::StopBlueprintImports();
    SoloASCoreExtensions::BlueprintInheritance::StopQuickCalls();
    SoloASCoreExtensions::StopRemovedScriptClassReclamation();
    SoloASCoreExtensions::BlueprintStructs::StopBlueprintStructs();
}
IMPLEMENT_MODULE(FSoloASCoreExtensionsModule, SoloASCoreExtensions)
#endif

#include "AngelscriptManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
namespace SoloASCoreExtensions::ScriptInterfaces { TSharedRef<FJsonObject> ExportInterfaceSchema(); }
namespace SoloASCoreExtensions
{
bool QueryPackagingState(FString& OutFingerprint, FString& OutError)
{
    OutFingerprint.Reset();
    OutError.Reset();
    bool Enabled = true;
    if (GConfig) GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableToolsPackagingProvider"), Enabled, GEngineIni);
    if (!Enabled) { OutError = TEXT("CE packaging provider is disabled."); return false; }
    // Only implemented features contribute. Dormant legacy producers cannot attest caches.
    OutFingerprint = FString::Printf(TEXT("soloas.tools-packaging/v1;source=%s;quick-calls=%d;container-algorithms=%d;native-event-super=%d;script-interfaces=%d"),
        ANSI_TO_TCHAR(SOLOAS_CE_BINDING_SOURCE_ID),
        SoloASCoreExtensions::BlueprintInheritance::AreQuickCallsEnabled() ? 1 : 0,
        SoloASCoreExtensions::Algorithms::IsEnabled() ? 1 : 0,
        SoloASCoreExtensions::NativeEventSuper::IsEnabled() ? 1 : 0,
        SoloASCoreExtensions::ScriptInterfaces::IsEnabled() ? 1 : 0);
    return true;
}
bool ValidatePackagingInputs(const FString& Directory, FString& OutError)
{
    FString Identity;
    if (!QueryPackagingState(Identity, OutError)) return false;
    FString Text;
    TSharedPtr<FJsonObject> Manifest;
    const FString ManifestPath = Directory / TEXT("CoreExtensions.packaging.json");
    if (!FPaths::FileExists(ManifestPath) || !FFileHelper::LoadFileToString(Text, *ManifestPath)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Manifest) || !Manifest)
    { OutError = TEXT("Missing or invalid CE packaging manifest."); return false; }
    FString Producer;
    if (!Manifest->TryGetStringField(TEXT("producer"), Producer) || Producer != Identity)
    { OutError = TEXT("CE packaging producer identity differs."); return false; }
    FString StructSchema;
    if (!Manifest->TryGetStringField(TEXT("blueprintStructSchema"), StructSchema)
        || !BlueprintStructs::ValidateBlueprintStructSchema(StructSchema, OutError))
    { if (OutError.IsEmpty()) OutError = TEXT("Missing CE Blueprint struct schema."); return false; }
    for (const TCHAR* Name : {TEXT("Binds.Cache"), TEXT("Binds.Cache.Headers"), TEXT("PrecompiledScript.Cache")})
    {
        const FString Path = Directory / Name;
        FString Expected;
        if (!FPaths::FileExists(Path) || !Manifest->TryGetStringField(Name, Expected)
            || Expected != LexToString(FMD5Hash::HashFile(*Path)))
        { OutError = FString::Printf(TEXT("CE packaging input missing or changed: %s"), Name); return false; }
    }
    return true;
}
namespace
{
FDelegateHandle PackagingProducerHandle;
void RecordGeneratedPackagingInputs()
{
    if (!FAngelscriptManager::bGeneratePrecompiledData || !FAngelscriptManager::Get().bDidInitialCompileSucceed) return;
    FString Identity, Error;
    if (!QueryPackagingState(Identity, Error)) return;
    const FString Directory = FAngelscriptManager::GetScriptRootDirectory();
    auto Manifest = MakeShared<FJsonObject>();
    Manifest->SetStringField(TEXT("producer"), Identity);
    Manifest->SetObjectField(TEXT("interfaces"), ScriptInterfaces::ExportInterfaceSchema());
    Manifest->SetStringField(TEXT("blueprintStructSchema"), BlueprintStructs::GetBlueprintStructSchema());
    for (const TCHAR* Name : {TEXT("Binds.Cache"), TEXT("Binds.Cache.Headers"), TEXT("PrecompiledScript.Cache")})
    {
        const FString Path = Directory / Name;
        if (!FPaths::FileExists(Path))
        { UE_LOG(LogTemp, Error, TEXT("CE packaging producer output is missing: %s"), Name); return; }
        Manifest->SetStringField(Name, LexToString(FMD5Hash::HashFile(*Path)));
    }
    FString Json;
    FJsonSerializer::Serialize(Manifest, TJsonWriterFactory<>::Create(&Json));
    if (!FFileHelper::SaveStringToFile(Json, *(Directory / TEXT("CoreExtensions.packaging.json"))))
        UE_LOG(LogTemp, Error, TEXT("CE packaging producer could not write its output identity."));
}
}
void StartPackagingProducer()
{
    FString Identity, Error;
    if (QueryPackagingState(Identity, Error) && !PackagingProducerHandle.IsValid())
        PackagingProducerHandle = FAngelscriptCodeModule::GetOnInitialCompileFinished().AddStatic(&RecordGeneratedPackagingInputs);
}
void StopPackagingProducer()
{
    FAngelscriptCodeModule::GetOnInitialCompileFinished().Remove(PackagingProducerHandle);
    PackagingProducerHandle.Reset();
}

}
