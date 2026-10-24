#include "AngelscriptDelegateWithPayload.h"
#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "AngelscriptBinds.h"
#include "AngelscriptDocs.h"

#include "StartAngelscriptHeaders.h"
#include "as_context.h"
#include "as_datatype.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

extern FString GetSignatureStringForFunction(UFunction* Function);

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_AngelscriptDelegateWithPayload((int32)FAngelscriptBinds::EOrder::Late, []
{
	auto FAngelscriptDelegateWithPayload_ = FAngelscriptBinds::ExistingClass("FAngelscriptDelegateWithPayload");

	FAngelscriptDelegateWithPayload_.Method("void ExecuteIfBound() const", &FAngelscriptDelegateWithPayload::ExecuteIfBound);
	FAngelscriptDelegateWithPayload_.Method("bool IsBound() const", &FAngelscriptDelegateWithPayload::IsBound);

	FAngelscriptDelegateWithPayload_.Method("void BindUFunction(UObject Object, const FName& FunctionName)",
	[](FAngelscriptDelegateWithPayload& Delegate, UObject* Object, const FName& FunctionName)
	{
		Delegate.BindUFunction(Object, FunctionName);
	});
	SCRIPT_MANUAL_BIND_META("DelegateObjectParam", "Object");
	SCRIPT_MANUAL_BIND_META("DelegateFunctionParam", "FunctionName");
	SCRIPT_MANUAL_BIND_META("DelegateBindType", "FInternalEmptyDelegate");

	FAngelscriptDelegateWithPayload_.Method("void BindWithPayload(UObject Object, const FName& FunctionName, const ?&in Payload)",
	[](FAngelscriptDelegateWithPayload& Delegate, UObject* Object, const FName& FunctionName, void* PayloadPtr, int PayloadScriptTypeId)
	{
		Delegate.BindUFunctionWithPayload(Object, FunctionName, PayloadPtr, PayloadScriptTypeId);
	});
	SCRIPT_MANUAL_BIND_META("DelegateObjectParam", "Object");
	SCRIPT_MANUAL_BIND_META("DelegateFunctionParam", "FunctionName");
	SCRIPT_MANUAL_BIND_META("DelegateBindType", "FInternalEmptyDelegateWithPayload");
	SCRIPT_MANUAL_BIND_META("DelegateWildcardParam", "Payload");
});

bool FAngelscriptDelegateWithPayload::IsBound() const
{
	return Object.IsValid() && !FunctionName.IsNone();
}

void FAngelscriptDelegateWithPayload::ExecuteIfBound() const
{
	if (!Object.IsValid())
		return;
	if (FunctionName.IsNone())
		return;

	UFunction* Function = Object->FindFunction(FunctionName);
	if (Function == nullptr)
		return;

	if (Payload.IsValid())
		Object->ProcessEvent(Function, (void*)Payload.GetMemory());
	else
		Object->ProcessEvent(Function, nullptr);
}

void FAngelscriptDelegateWithPayload::BindUFunction(UObject* InObject, FName InFunctionName)
{
	if (!IsValid(InObject))
	{
		FAngelscriptManager::Throw("Invalid object passed to BindUFunction.");
		return;
	}

	UFunction* Function = InObject->FindFunction(InFunctionName);
	if (Function == nullptr)
	{
		const FString Debug = FString::Printf(TEXT("\nCould not find function %s\nIs it declared UFUNCTION()?"), *InFunctionName.ToString());
		FAngelscriptManager::Throw(TCHAR_TO_ANSI(*Debug));
		return;
	}

	if (Function->NumParms != 0 || Function->GetReturnProperty() != nullptr)
	{
		FString Message = FString::Printf(TEXT("Specified function is not compatible with delegate function.\n\nDelegate: void FAngelscriptDelegateWithPayload()\n\nAttempted Bind: %s"),
			*GetSignatureStringForFunction(Function));
		FAngelscriptManager::Throw(TCHAR_TO_ANSI(*Message));
		return;
	}

	Payload.Reset();
	Object = InObject;
	FunctionName = InFunctionName;
}

void FAngelscriptDelegateWithPayload::BindUFunctionWithPayload(UObject* InObject, FName InFunctionName, void* PayloadPtr, int PayloadScriptTypeId)
{
	UScriptStruct* StructType = Cast<UScriptStruct>(FAngelscriptManager::Get().GetUnrealStructFromAngelscriptTypeId(PayloadScriptTypeId));
	if (StructType == nullptr)
		StructType = FAngelscriptDelegateWithPayload::GetBoxedPrimitiveStructFromTypeId(PayloadScriptTypeId);

	if (StructType == nullptr)
	{
		FAngelscriptManager::Throw("Invalid payload type");
		return;
	}

	if (!IsValid(InObject))
	{
		FAngelscriptManager::Throw("Invalid object passed to BindUFunction.");
		return;
	}

	UFunction* Function = InObject->FindFunction(InFunctionName);
	if (Function == nullptr)
	{
		const FString Debug = FString::Printf(TEXT("\nCould not find function %s\nIs it declared UFUNCTION()?"), *InFunctionName.ToString());
		FAngelscriptManager::Throw(TCHAR_TO_ANSI(*Debug));
		return;
	}

	bool bSignatureMismatch = false;

	FStructProperty* StructProp = CastField<FStructProperty>(Function->PropertyLink); 
	if (StructProp == nullptr && Function->PropertyLink == nullptr)
	{
		bSignatureMismatch = true;
	}
	else if (StructProp == nullptr)
	{
		switch (PayloadScriptTypeId & asTYPEID_MASK_SEQNBR)
		{
		case asTYPEID_BOOL:
			if (!Function->PropertyLink->IsA<FBoolProperty>())
				bSignatureMismatch = true;
		break;
		case asTYPEID_INT8:
			if (!Function->PropertyLink->IsA<FInt8Property>())
				bSignatureMismatch = true;
		break;
		case asTYPEID_UINT8:
			if (!Function->PropertyLink->IsA<FByteProperty>())
				bSignatureMismatch = true;
		break;

		case asTYPEID_INT16:
			if (!Function->PropertyLink->IsA<FInt16Property>())
				bSignatureMismatch = true;
		break;
		case asTYPEID_UINT16:
			if (!Function->PropertyLink->IsA<FUInt16Property>())
				bSignatureMismatch = true;
		break;

		case asTYPEID_INT32:
			if (!Function->PropertyLink->IsA<FIntProperty>())
				bSignatureMismatch = true;
		break;
		case asTYPEID_UINT32:
			if (!Function->PropertyLink->IsA<FUInt32Property>())
				bSignatureMismatch = true;
		break;

		case asTYPEID_INT64:
			if (!Function->PropertyLink->IsA<FInt64Property>())
				bSignatureMismatch = true;
		break;
		case asTYPEID_UINT64:
			if (!Function->PropertyLink->IsA<FUInt64Property>())
				bSignatureMismatch = true;
		break;

		case asTYPEID_FLOAT32:
			if (!Function->PropertyLink->IsA<FFloatProperty>())
				bSignatureMismatch = true;
		break;
		case asTYPEID_FLOAT64:
			if (!Function->PropertyLink->IsA<FDoubleProperty>())
				bSignatureMismatch = true;
		break;

		default:
		{
			auto& Manager = FAngelscriptManager::Get();
			asCTypeInfo* ScriptType = (asCTypeInfo*)Manager.Engine->GetTypeInfoById(PayloadScriptTypeId);
			if (ScriptType != nullptr && ScriptType->flags & asOBJ_ENUM && ScriptType->size == 1)
			{
				FEnumProperty* EnumProp = CastField<FEnumProperty>(Function->PropertyLink);
				FByteProperty* ByteProp = CastField<FByteProperty>(Function->PropertyLink);

				if (EnumProp == nullptr && ByteProp == nullptr)
				{
					bSignatureMismatch = true;
				}
			}
			else
			{
				bSignatureMismatch = true;
			}
		}
		break;
		}
	}
	else
	{
		if (StructProp->Struct != StructType)
			bSignatureMismatch = true;
	}

	if (Function->NumParms != 1 || bSignatureMismatch || Function->GetReturnProperty() != nullptr)
	{
		FString Message = FString::Printf(TEXT("Specified function is not compatible with delegate function.\n\nDelegate: void FAngelscriptDelegateWithPayload(? Payload)\n\nAttempted Bind: %s"),
			*GetSignatureStringForFunction(Function));
		FAngelscriptManager::Throw(TCHAR_TO_ANSI(*Message));
		return;
	}

	Payload.InitializeAs(StructType, (const uint8*)PayloadPtr);
	Object = InObject;
	FunctionName = InFunctionName;
}

void Reset();

UScriptStruct* FAngelscriptDelegateWithPayload::GetBoxedPrimitiveStructFromTypeId(int TypeId)
{
	switch (TypeId & asTYPEID_MASK_SEQNBR)
	{
	case asTYPEID_BOOL:
	case asTYPEID_INT8:
	case asTYPEID_UINT8:
		return FAngelscriptBoxedByte::StaticStruct();

	case asTYPEID_INT16:
	case asTYPEID_UINT16:
		return FAngelscriptBoxedShort::StaticStruct();

	case asTYPEID_INT32:
	case asTYPEID_UINT32:
		return FAngelscriptBoxedInt32::StaticStruct();

	case asTYPEID_INT64:
	case asTYPEID_UINT64:
		return FAngelscriptBoxedInt64::StaticStruct();

	case asTYPEID_FLOAT32:
		return FAngelscriptBoxedFloat::StaticStruct();
	case asTYPEID_FLOAT64:
		return FAngelscriptBoxedDouble::StaticStruct();
	}

	auto& Manager = FAngelscriptManager::Get();
	asCTypeInfo* ScriptType = (asCTypeInfo*)Manager.Engine->GetTypeInfoById(TypeId);
	if (ScriptType != nullptr && ScriptType->flags & asOBJ_ENUM && ScriptType->size == 1)
		return FAngelscriptBoxedByte::StaticStruct();

	return nullptr;
}
