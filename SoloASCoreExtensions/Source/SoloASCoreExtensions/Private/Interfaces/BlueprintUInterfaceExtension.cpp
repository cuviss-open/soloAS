#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "BlueprintUInterfaceExtension.h"
#include "AngelscriptNativeInterfaceBinding.h"
#include "AngelscriptNativeInterfaceEventRegistry.h"
#include "AngelscriptBindDatabase.h"
#include "Interfaces/ScriptInterfacesExtension.h"

#include "Extensions/AngelscriptExtensionHooks.h"
#include "Shared/ExtensionRegistration.h"

#include "AngelscriptBinds.h"
#include "UObject/Class.h"
#include "UObject/Interface.h"

namespace
{
EAngelscriptExtensionResult ExecuteNativeInterfaceBinding(
	FAngelscriptNativeInterfaceSelectionContext& Context
)
{
    Context.Result = EAngelscriptExtensionResult::NotHandled;
    if (!SoloASCoreExtensions::ScriptInterfaces::IsEnabled()) return Context.Result;
    if (Context.Point == EAngelscriptNativeInterfacePoint::Select) return HandleBlueprintUInterfaceSelection(Context);
    if (AngelscriptNativeInterfaceBinding::IsInterface(Context.NativeClass)
        || Context.Point == EAngelscriptNativeInterfacePoint::ResolveEvent
        || Context.Point == EAngelscriptNativeInterfacePoint::RegisterEvent)
    {
        using namespace AngelscriptNativeInterfaceBinding;
        using namespace AngelscriptNativeInterfaceEventRegistry;
        bool Handled = true;
        switch (Context.Point)
        {
        case EAngelscriptNativeInterfacePoint::RegisterType:
            BindClosure(Context.NativeClass, Context.TypeName, Context.RegisterType);
            break;
        case EAngelscriptNativeInterfacePoint::PrepareDatabase:
            Handled = PrepareDatabaseClass(Context.NativeClass, Context.ResolveDatabaseClass);
            break;
        case EAngelscriptNativeInterfacePoint::BindDatabaseMethods:
            BindDatabaseMethodsIfInterface(Context.NativeClass, Context.Type, Context,
                &DispatchBlueprintEvent, *Context.EventsByScriptName);
            break;
        case EAngelscriptNativeInterfacePoint::BindReflectedMethods:
            BindRuntimeMethodsIfInterface(Context.NativeClass, Context.Type.ToSharedRef(), Context,
                &DispatchBlueprintEvent, *Context.EventsByScriptName);
            break;
        case EAngelscriptNativeInterfacePoint::FinalizeRecord:
            if (Context.FinalizeRecord)
                FinalizeRuntimeInterfaceRecord(Context.NativeClass, Context.Type.ToSharedRef(), Context.FinalizeRecord);
            break;
        case EAngelscriptNativeInterfacePoint::RegisterEvent:
            Register(*Context.EventsByScriptName, Context.ResolvedFunction, Context.ScriptName);
            break;
        case EAngelscriptNativeInterfacePoint::ResolveEvent:
            Context.ResolvedFunction = FindInterface(*Context.EventsByScriptName, Context.NativeClass, Context.ScriptName);
            break;
        default:
            Handled = false;
        }
        Context.Result = Handled ? EAngelscriptExtensionResult::Handled : EAngelscriptExtensionResult::NotHandled;
    }
    return Context.Result;
}

EAngelscriptExtensionResult DispatchBlueprintUInterfaceSelection(FAngelscriptNativeInterfaceSelectionContext& Context)
{
    return HandleBlueprintUInterfaceSelection(Context);
}
}

FAngelscriptExtensionCallbackRegistration RegisterBlueprintUInterfaceExtension()
{
	FAngelscriptExtensionCallbackRegistration Registration;
	FString Error;
	if (!RegisterAngelscriptNativeInterfaceSelectionExtension(
		&DispatchBlueprintUInterfaceSelection,
		Registration,
		Error, &ExecuteNativeInterfaceBinding))
	{
		UE_LOG(
			LogTemp,
			Fatal,
			TEXT("Failed to register Blueprint UInterface extension: %s"),
			*Error
		);
	}
	return Registration;
}

namespace
{
AS_FORCE_LINK const SoloASCoreExtensions::ExtensionRegistration::
	FExtensionRegistrar BlueprintUInterfaceRegistrar(
		"soloas.blueprint-uinterface/v1",
		&RegisterBlueprintUInterfaceExtension
	);
}

namespace
{
#if WITH_METADATA
const FName BlueprintTypeMetadata(TEXT("BlueprintType"));
const FName BlueprintBaseMetadata(TEXT("IsBlueprintBase"));
const FName CannotImplementMetadata(
	TEXT("CannotImplementInterfaceInBlueprint")
);
const FName NotBlueprintTypeMetadata(TEXT("NotBlueprintType"));
const FName NotInAngelscriptMetadata(TEXT("NotInAngelscript"));
#endif

bool IsBlueprintUInterface(const UClass& NativeClass)
{
#if WITH_METADATA
	if (&NativeClass == UInterface::StaticClass())
		return false;

	bool bBlueprintType = false;
	bool bBlueprintBase = false;
	TSet<const UClass*> VisitedInterfaces;
	for (const UClass* InterfaceClass = &NativeClass;
		InterfaceClass != nullptr && InterfaceClass != UInterface::StaticClass();
		InterfaceClass = InterfaceClass->GetSuperClass())
	{
		if (!InterfaceClass->HasAllClassFlags(CLASS_Interface | CLASS_Native)
			|| InterfaceClass->HasAnyClassFlags(
				CLASS_Deprecated | CLASS_NewerVersionExists)
			|| InterfaceClass->HasMetaData(CannotImplementMetadata)
			|| InterfaceClass->HasMetaData(NotBlueprintTypeMetadata)
			|| InterfaceClass->HasMetaData(NotInAngelscriptMetadata)
			|| VisitedInterfaces.Contains(InterfaceClass))
		{
			return false;
		}
		VisitedInterfaces.Add(InterfaceClass);
		bBlueprintType |= InterfaceClass->GetBoolMetaData(BlueprintTypeMetadata);
		bBlueprintBase |= InterfaceClass->GetBoolMetaData(BlueprintBaseMetadata);
	}
	return bBlueprintType || bBlueprintBase;
#else
	return false;
#endif
}
}

EAngelscriptExtensionResult HandleBlueprintUInterfaceSelection(
	FAngelscriptNativeInterfaceSelectionContext& Context
)
{
	if (!SoloASCoreExtensions::ScriptInterfaces::IsEnabled()
        || Context.NativeClass == nullptr)
		return EAngelscriptExtensionResult::NotHandled;

    if (IsBlueprintUInterface(*Context.NativeClass))
        Context.bBindAsInterface = true;
	return EAngelscriptExtensionResult::Handled;
}

#else
#endif
