#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "AngelscriptNativeInterfaceEventRegistry.h"

#include "Extensions/AngelscriptExtensionHooks.h"
#include "AngelscriptManager.h"
#include "AngelscriptType.h"

#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "EndAngelscriptHeaders.h"

#include "UObject/Class.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Interfaces/Interface_AssetUserData.h"
#include "Misc/AutomationTest.h"
#include "UObject/Interface.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#endif


namespace AngelscriptNativeInterfaceEventRegistry
{
// A present null entry is ambiguous until the owning event map is cleared.
static void RegisterEventIdentity(
	FEventMap& EventsByScriptName,
	UClass* DeclaringClass,
	UFunction* Function,
	const FString& ScriptName)
{
	auto& Events = EventsByScriptName.FindOrAdd(DeclaringClass);
	if (UFunction** Existing = Events.Find(ScriptName))
	{
		if (*Existing != Function)
			*Existing = nullptr;
		return;
	}
	Events.Add(ScriptName, Function);
}

void Register(
	FEventMap& EventsByScriptName,
	UFunction* Function,
	const FString& ScriptName)
{
	check(IsInGameThread());
	check(Function != nullptr);
	check(Function->HasAnyFunctionFlags(FUNC_BlueprintEvent));
	check(!ScriptName.IsEmpty());
	UClass* DeclaringClass = Function->GetOuterUClass();
	check(DeclaringClass != nullptr);
	RegisterEventIdentity(EventsByScriptName, DeclaringClass, Function, ScriptName);
}

UFunction* FindInterface(
	const FEventMap& EventsByScriptName,
	UClass* Class,
	const FString& ScriptName)
{
	UFunction* InterfaceMatch = nullptr;
	TSet<UClass*> VisitedInterfaces;
	for (UClass* CheckClass = Class; CheckClass != nullptr;
		CheckClass = CheckClass->GetSuperClass())
	{
		TArray<UClass*> PendingInterfaces;
		for (const FImplementedInterface& Implemented : CheckClass->Interfaces)
		{
			if (Implemented.Class != nullptr)
				PendingInterfaces.Add(Implemented.Class);
		}

		while (!PendingInterfaces.IsEmpty())
		{
			PendingInterfaces.Sort([](const UClass& Left, const UClass& Right)
			{
				return Left.GetPathName() < Right.GetPathName();
			});
			UClass* InterfaceClass = PendingInterfaces[0];
			PendingInterfaces.RemoveAt(0, 1, EAllowShrinking::No);
			if (InterfaceClass == nullptr
				|| !InterfaceClass->HasAllClassFlags(CLASS_Interface | CLASS_Native)
				|| VisitedInterfaces.Contains(InterfaceClass))
			{
				continue;
			}
			VisitedInterfaces.Add(InterfaceClass);

			if (const auto* List = EventsByScriptName.Find(InterfaceClass))
			{
				if (UFunction* const* Function = List->Find(ScriptName))
				{
					if (*Function == nullptr)
						return nullptr;
					if (InterfaceMatch != nullptr
						&& InterfaceMatch != *Function)
					{
						return nullptr;
					}
					InterfaceMatch = *Function;
				}
			}

			UClass* SuperInterface = InterfaceClass->GetSuperClass();
			if (SuperInterface != nullptr
				&& SuperInterface->HasAllClassFlags(CLASS_Interface | CLASS_Native))
			{
				PendingInterfaces.AddUnique(SuperInterface);
			}

			for (const FImplementedInterface& Parent : InterfaceClass->Interfaces)
			{
				if (Parent.Class != nullptr)
					PendingInterfaces.Add(Parent.Class);
			}
		}
	}
	return InterfaceMatch;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNativeInterfaceEventCollisionTest,
	"Cuviss.Angelscript.BlueprintUInterface.EventIdentityCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNativeInterfaceEventCollisionTest::RunTest(const FString& Parameters)
{
	// Exercise identity insertion without manufacturing BlueprintEvent flags.
	// All map state and consumer interface edges belong to this test.
	FEventMap Events;
	UClass* FirstInterface = UInterface_AssetUserData::StaticClass();
	UClass* ParentInterface = UInterface::StaticClass();
	UFunction* A = NewObject<UFunction>(UObject::StaticClass());
	UFunction* B = NewObject<UFunction>(UObject::StaticClass());
	UClass* Consumer = NewObject<UClass>(GetTransientPackage());
	Consumer->Interfaces.AddDefaulted();
	Consumer->Interfaces.Last().Class = FirstInterface;
	const FString Alias(TEXT("CollisionAlias"));

	RegisterEventIdentity(Events, FirstInterface, A, Alias);
	RegisterEventIdentity(Events, FirstInterface, A, Alias);
	TestTrue(TEXT("Same function twice remains accepted"),
		FindInterface(Events, Consumer, Alias) == A);
	RegisterEventIdentity(Events, FirstInterface, B, Alias);
	TestNull(TEXT("Distinct functions under one alias are rejected"),
		FindInterface(Events, Consumer, Alias));
	RegisterEventIdentity(Events, FirstInterface, A, Alias);
	RegisterEventIdentity(Events, FirstInterface, B, Alias);
	TestNull(TEXT("A B A B remains ambiguous"),
		FindInterface(Events, Consumer, Alias));

	RegisterEventIdentity(Events, ParentInterface, A, Alias);
	TestNull(TEXT("Ambiguous child cannot yield an unambiguous ancestor"),
		FindInterface(Events, Consumer, Alias));
	Consumer->Interfaces.AddDefaulted();
	Consumer->Interfaces.Last().Class = ParentInterface;
	TestNull(TEXT("Ambiguous interface cannot yield another interface event"),
		FindInterface(Events, Consumer, Alias));

	Events.Empty();
	RegisterEventIdentity(Events, FirstInterface, A, Alias);
	TestTrue(TEXT("Reset clears the owned ambiguity state"),
		FindInterface(Events, Consumer, Alias) == A);
	RegisterEventIdentity(Events, ParentInterface, A, Alias);
	TestTrue(TEXT("Same identity across interfaces remains accepted"),
		FindInterface(Events, Consumer, Alias) == A);
	Events.Empty();
	RegisterEventIdentity(Events, FirstInterface, A, Alias);
	RegisterEventIdentity(Events, ParentInterface, B, Alias);
	TestNull(TEXT("Distinct identities across interfaces remain rejected"),
		FindInterface(Events, Consumer, Alias));

	Events.Empty();
	RegisterEventIdentity(Events, FirstInterface, A, Alias);
	RegisterEventIdentity(Events, ParentInterface, A, Alias);
	RegisterEventIdentity(Events, ParentInterface, B, Alias);
	TestNull(TEXT("Unambiguous child cannot hide an ambiguous ancestor"),
		FindInterface(Events, Consumer, Alias));
	return true;
}
#endif

void DispatchBlueprintEvent(
	TSharedRef<FAngelscriptType> Type,
	UFunction* Function,
	const FAngelscriptBlueprintEventBindingSnapshot& Signature,
    FEventMap& EventsByScriptName)
{
	if (!IsAngelscriptExtensionStageRegistered(
			EAngelscriptExtensionStage::BlueprintEventBound))
	{
		return;
	}
	if (Function == nullptr)
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT("Native interface event dispatch received no UFunction for %s"),
			*Type->GetAngelscriptTypeName());
		return;
	}
	if (!Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
	{
		return;
	}

	UClass* TargetInterface = Type->GetClass(
		FAngelscriptTypeUsage::DefaultUsage);
	UClass* DeclaringClass = Function->GetOuterUClass();
	if (Signature.Target != EAngelscriptBlueprintEventTarget::Method
		|| Signature.Declaration.IsEmpty()
		|| Signature.ScriptName.IsEmpty()
		|| TargetInterface == nullptr
		|| !TargetInterface->HasAnyClassFlags(CLASS_Interface)
		|| DeclaringClass == nullptr
		|| Signature.ReturnType.bIsReference)
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT("Native interface event snapshot is invalid for %s"),
			*Function->GetPathName());
		return;
	}
	Register(EventsByScriptName, Function, Signature.ScriptName);

	if (!Function->HasAnyFunctionFlags(FUNC_Native))
		return;
    // Object views already notify extensions through BindBlueprintEvent.
    asITypeInfo* ScriptType = Type->GetAngelscriptTypeInfo(FAngelscriptTypeUsage::DefaultUsage);
    if (ScriptType && (ScriptType->GetFlags() & asOBJ_SCRIPT_OBJECT) == 0) return;

	FAngelscriptBlueprintEventBindingSnapshot Binding;
	Binding.Target = EAngelscriptBlueprintEventTarget::Method;
	Binding.TargetScriptTypeName = Type->GetAngelscriptTypeName();
	Binding.Declaration = Signature.Declaration;
	Binding.ScriptName = Signature.ScriptName;
	Binding.NativeDeclaringClassPath = DeclaringClass->GetPathName();
	Binding.NativeFunctionName = Function->GetFName();
	Binding.ArgumentTypes = Signature.ArgumentTypes;
	Binding.ReturnType = Signature.ReturnType;
	if (Binding.ReturnType.IsValid())
	{
		Binding.ReturnType.bIsReference = true;
		Binding.bInitializeReturn = Binding.ReturnType.CanConstruct()
			&& Binding.ReturnType.NeedConstruct();
		Binding.bZeroReturnPointer = !Binding.bInitializeReturn
			&& Binding.ReturnType.Type->IsObjectPointer();
	}

	FString ExtensionError;
	if (!DispatchAngelscriptBlueprintEventBound(Binding, ExtensionError))
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT("Native interface event extension failed for %s: %s"),
			*Function->GetPathName(),
			*ExtensionError);
	}
}
}

#else
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNativeInterfaceEventCollisionTest, "Cuviss.Angelscript.BlueprintUInterface.EventIdentityCollision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNativeInterfaceEventCollisionTest::RunTest(const FString&)
{
    AddInfo(TEXT("skipped-with-reason: Cuviss.Angelscript.BlueprintUInterface.EventIdentityCollision requires core extension contracts absent from official 0.1.3; no capability credit."));
    return true;
}
#endif
#endif
