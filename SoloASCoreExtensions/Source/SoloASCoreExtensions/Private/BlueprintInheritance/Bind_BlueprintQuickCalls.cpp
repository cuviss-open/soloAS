// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.

#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "BlueprintQuickCallShared.h"
#include "BlueprintQuickCallsExtension.h"
#include "AngelscriptExecutionQuiescence.h"
#include "AngelscriptBinds/Bind_Delegates.h"

#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "EndAngelscriptHeaders.h"

namespace
{
using namespace AngelscriptBlueprintQuickCall;
constexpr int32 MaxQuickCallArguments = 16;

void GetBlueprintProperty(asIScriptGeneric* Generic)
{
	UObject* Object = static_cast<UObject*>(Generic->GetArgObject(0));
	const FName Name = *static_cast<const FName*>(Generic->GetArgObject(1));
	FProperty* Property = FindNamedProperty(Object, Name);
	if (Object == nullptr)
	{
		ThrowQuickCallError(TEXT("GetBlueprintProperty received a null object."));
		return;
	}
	if (Property == nullptr)
	{
		ThrowQuickCallError(FString::Printf(TEXT("Blueprint property '%s' was not found on '%s'."), *Name.ToString(), *Object->GetClass()->GetName()));
		return;
	}
	if (!IsSupportedBlueprintProperty(Property))
	{
		ThrowQuickCallError(FString::Printf(TEXT("Blueprint property '%s' has unsupported type '%s'."), *Name.ToString(), *Property->GetCPPType()));
		return;
	}

	const int32 OutputTypeId = Generic->GetArgTypeId(2);
	if (!IsBlueprintTypeCompatible(OutputTypeId, Property, true))
	{
		ThrowQuickCallError(FString::Printf(TEXT("Blueprint property '%s' expects '%s', but GetBlueprintProperty output is '%s'."),
			*Name.ToString(), *FAngelscriptTypeUsage::FromProperty(Property).GetFriendlyTypeName(), *GetAngelscriptTypeName(OutputTypeId)));
		return;
	}

	CopyValue(Property, Generic->GetArgAddress(2), Property->ContainerPtrToValuePtr<void>(Object));
}

void SetBlueprintProperty(asIScriptGeneric* Generic)
{
	UObject* Object = static_cast<UObject*>(Generic->GetArgObject(0));
	const FName Name = *static_cast<const FName*>(Generic->GetArgObject(1));
	FProperty* Property = FindNamedProperty(Object, Name);
	if (Object == nullptr)
	{
		ThrowQuickCallError(TEXT("SetBlueprintProperty received a null object."));
		return;
	}
	if (Property == nullptr)
	{
		ThrowQuickCallError(FString::Printf(TEXT("Blueprint property '%s' was not found on '%s'."), *Name.ToString(), *Object->GetClass()->GetName()));
		return;
	}
	if (!IsSupportedBlueprintProperty(Property))
	{
		ThrowQuickCallError(FString::Printf(TEXT("Blueprint property '%s' has unsupported type '%s'."), *Name.ToString(), *Property->GetCPPType()));
		return;
	}

	const int32 ValueTypeId = Generic->GetArgTypeId(2);
	if (!IsBlueprintTypeCompatible(ValueTypeId, Property))
	{
		ThrowQuickCallError(FString::Printf(TEXT("Blueprint property '%s' expects '%s', but SetBlueprintProperty received '%s'."),
			*Name.ToString(), *FAngelscriptTypeUsage::FromProperty(Property).GetFriendlyTypeName(), *GetAngelscriptTypeName(ValueTypeId)));
		return;
	}

	CopyValue(Property, Property->ContainerPtrToValuePtr<void>(Object), Generic->GetArgAddress(2));
}

void InvokeBlueprint(asIScriptGeneric* Generic)
{
	UObject* Object = static_cast<UObject*>(Generic->GetArgObject(0));
	const FName Name = *static_cast<const FName*>(Generic->GetArgObject(1));
	if (!IsInGameThread())
	{
		ThrowQuickCallError(TEXT("CallBlueprint can only run on the game thread."));
		return;
	}
	if (Object == nullptr)
	{
		ThrowQuickCallError(TEXT("CallBlueprint received a null object."));
		return;
	}
	UFunction* Function = Object->FindFunction(Name);
	if (Function == nullptr)
	{
		ThrowQuickCallError(FString::Printf(TEXT("Blueprint function '%s' was not found on '%s'."), *Name.ToString(), *Object->GetClass()->GetName()));
		return;
	}
	TArray<FProperty*> Parameters;
	FProperty* ReturnProperty = nullptr;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Parm))
			continue;
		if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
			ReturnProperty = Property;
		else
			Parameters.Add(Property);
	}

	const int32 ProvidedArgumentCount = Generic->GetArgCount() - 2;
	const bool bHasReturnArgument = ReturnProperty != nullptr
		&& ProvidedArgumentCount == Parameters.Num() + 1;
	if (ProvidedArgumentCount != Parameters.Num() && !bHasReturnArgument)
	{
		const FString Expected = ReturnProperty != nullptr
			? FString::Printf(TEXT("%d arguments, or %d including a return slot"), Parameters.Num(), Parameters.Num() + 1)
			: FString::Printf(TEXT("%d arguments"), Parameters.Num());
		ThrowQuickCallError(FString::Printf(TEXT("Blueprint function '%s' expects %s, but CallBlueprint received %d arguments."),
			*Name.ToString(), *Expected, ProvidedArgumentCount));
		return;
	}
	const int32 ArgumentOffset = bHasReturnArgument ? 3 : 2;
	const int32 ReturnTypeId = bHasReturnArgument ? Generic->GetArgTypeId(2) : asTYPEID_VOID;
	if (bHasReturnArgument && !IsBlueprintTypeCompatible(ReturnTypeId, ReturnProperty, true))
	{
		ThrowQuickCallError(FString::Printf(TEXT("Blueprint function '%s' return value expects '%s', but CallBlueprint output is '%s'."),
			*Name.ToString(), *FAngelscriptTypeUsage::FromProperty(ReturnProperty).GetFriendlyTypeName(), *GetAngelscriptTypeName(ReturnTypeId)));
		return;
	}

	bool bHasOutParameters = false;
	bool bOnlyOutParameters = !Parameters.IsEmpty();
	for (FProperty* Parameter : Parameters)
	{
		const bool bIsOut = Parameter->HasAnyPropertyFlags(CPF_OutParm);
		bHasOutParameters |= bIsOut;
		bOnlyOutParameters &= bIsOut;
	}
	if (bHasOutParameters && !bOnlyOutParameters)
	{
		for (FProperty* Parameter : Parameters)
		{
			if (Parameter->HasAnyPropertyFlags(CPF_OutParm))
			{
				ThrowQuickCallError(FString::Printf(TEXT("Blueprint function '%s' parameter '%s' is an out parameter; "
					"CallBlueprint supports functions whose parameters are all out."),
					*Name.ToString(), *Parameter->GetName()));
				return;
			}
		}
	}

	for (int32 Index = 0; Index < Parameters.Num(); ++Index)
	{
		FProperty* Parameter = Parameters[Index];
		if (!bOnlyOutParameters && Parameter->HasAnyPropertyFlags(CPF_ReferenceParm)
			&& !Parameter->HasAnyPropertyFlags(CPF_ConstParm))
		{
			ThrowQuickCallError(FString::Printf(TEXT("Blueprint function '%s' parameter '%s' is an out/reference parameter, "
				"which CallBlueprint does not support."),
				*Name.ToString(), *Parameter->GetName()));
			return;
		}
		if (!IsSupportedBlueprintProperty(Parameter))
		{
			ThrowQuickCallError(FString::Printf(TEXT("Blueprint function '%s' parameter '%s' has unsupported type '%s'."),
				*Name.ToString(), *Parameter->GetName(), *Parameter->GetCPPType()));
			return;
		}
		const int32 ArgumentTypeId = Generic->GetArgTypeId(Index + ArgumentOffset);
		if (!IsBlueprintTypeCompatible(ArgumentTypeId, Parameter, bOnlyOutParameters))
		{
			ThrowQuickCallError(FString::Printf(TEXT("Blueprint function '%s' parameter '%s' expects '%s', but CallBlueprint received '%s'."),
				*Name.ToString(), *Parameter->GetName(),
					*FAngelscriptTypeUsage::FromProperty(Parameter).GetFriendlyTypeName(), *GetAngelscriptTypeName(ArgumentTypeId)));
			return;
		}
	}

	FStructOnScope ParameterMemory(Function);
	void* ParameterData = ParameterMemory.GetStructMemory();
	for (int32 Index = 0; Index < Parameters.Num(); ++Index)
	{
		FProperty* Parameter = Parameters[Index];
		if (!bOnlyOutParameters)
			CopyValue(Parameter, Parameter->ContainerPtrToValuePtr<void>(ParameterData), Generic->GetArgAddress(Index + ArgumentOffset));
	}

	Object->ProcessEvent(Function, ParameterData);
	if (bHasReturnArgument)
		CopyValue(ReturnProperty, Generic->GetArgAddress(2), ReturnProperty->ContainerPtrToValuePtr<void>(ParameterData));
	if (bOnlyOutParameters)
	{
		for (int32 Index = 0; Index < Parameters.Num(); ++Index)
		{
			FProperty* Parameter = Parameters[Index];
			CopyValue(Parameter, Generic->GetArgAddress(Index + ArgumentOffset), Parameter->ContainerPtrToValuePtr<void>(ParameterData));
		}
	}
}

void CallBlueprint(asIScriptGeneric* Generic)
{
	InvokeBlueprint(Generic);
}

void BindBlueprintQuickCallEvent(UObject* Object, FName EventName, UObject* Target, FName HandlerName)
{
	if (Object == nullptr)
	{
		FAngelscriptManager::Throw("BindBlueprintEvent received a null event object.");
		return;
	}
	if (Target == nullptr)
	{
		FAngelscriptManager::Throw("BindBlueprintEvent received a null handler target.");
		return;
	}

	FMulticastDelegateProperty* EventProperty = FindFProperty<FMulticastDelegateProperty>(Object->GetClass(), EventName);
	if (EventProperty == nullptr)
	{
		const FString Message = FString::Printf(TEXT("Blueprint event '%s' was not found on '%s' as a multicast delegate."),
			*EventName.ToString(), *Object->GetClass()->GetName());
		FAngelscriptManager::Throw(TCHAR_TO_ANSI(*Message));
		return;
	}
	if (Target->FindFunction(HandlerName) == nullptr)
	{
		const FString Message = FString::Printf(TEXT("Blueprint event handler '%s' was not found on '%s'."),
			*HandlerName.ToString(), *Target->GetClass()->GetName());
		FAngelscriptManager::Throw(TCHAR_TO_ANSI(*Message));
		return;
	}
	UDelegateFunction* Signature = Cast<UDelegateFunction>(EventProperty->SignatureFunction);
	if (Signature == nullptr)
	{
		FAngelscriptManager::Throw("Blueprint multicast event has an invalid delegate signature.");
		return;
	}

	auto* Delegate = EventProperty->ContainerPtrToValuePtr<FMulticastScriptDelegate>(Object);
	FAngelscriptMulticastDelegateOperations::AddUFunction_Signature(
		Delegate, Target, HandlerName, Signature);
}
}

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_BlueprintQuickCalls(
	FAngelscriptBinds::EOrder::Late,
	[]
	{
        if (!SoloASCoreExtensions::BlueprintInheritance::AreQuickCallsEnabled())
            return;
		FAngelscriptBinds::BindGlobalGenericFunction(
			"void GetBlueprintProperty(UObject Object, FName PropertyName, ?&out Value)",
			&GetBlueprintProperty);
		FAngelscriptBinds::BindGlobalGenericFunction(
			"void SetBlueprintProperty(UObject Object, FName PropertyName, const ?&in Value)",
			&SetBlueprintProperty);
		FAngelscriptBinds::BindGlobalFunction(
			"void BindBlueprintEvent(UObject Object, FName EventName, UObject Target, FName HandlerName)",
			FUNC(BindBlueprintQuickCallEvent));

		for (int32 ArgumentCount = 0; ArgumentCount <= MaxQuickCallArguments; ++ArgumentCount)
		{
			FString Declaration = TEXT("void __CallBlueprintInternal(UObject Object, FName FunctionName");
			for (int32 Index = 0; Index < ArgumentCount; ++Index)
				Declaration += FString::Printf(TEXT(", const ?&in Arg%d"), Index);
			Declaration += TEXT(")");
			FAngelscriptBinds::BindGlobalGenericFunction(Declaration, &CallBlueprint);
		}
	});
