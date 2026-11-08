#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "Shared/ExtensionRegistration.h"
#include "AngelscriptBinds.h"
#include "Interfaces/ScriptInterfacesExtension.h"
#include "BlueprintStructs/BlueprintStructsExtension.h"

namespace SoloASCoreExtensions::ExtensionRegistration
{
namespace
{
EAngelscriptExtensionResult GenerateCoreTypes(FAngelscriptTypeGenerationContext& Context)
{
    return DispatchCoreTypeGeneration(Context,
        &ScriptInterfaces::GenerateScriptInterfaceTypes,
        &BlueprintStructs::GenerateBlueprintStructTypes,
        ScriptInterfaces::IsEnabled(), GetFeaturePolicy().bBlueprintStructs);
}

FAngelscriptExtensionCallbackRegistration RegisterCoreTypes()
{
    FAngelscriptExtensionCallbackRegistration Registration;
    FString Error;
    if (!RegisterAngelscriptTypeGenerationExtension(&GenerateCoreTypes, Registration, Error))
        UE_LOG(LogTemp, Fatal, TEXT("Core type-generation registration failed: %s"), *Error);
    return Registration;
}

// Migrates the interface owner's identity without weakening its v7 cache invalidation.
AS_FORCE_LINK const FExtensionRegistrar Registrar("soloas.script-interfaces/v7", &RegisterCoreTypes);
}
}

#else
#endif
