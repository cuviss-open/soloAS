// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptReflectionAdapter.h"

#include "ClassGenerator/ASClass.h"
#include "ClassGenerator/ASStruct.h"
#include "Shared/SoloASBridgeReflection.h"
#include "Runtime/SoloASBridgeScriptMetadata.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr uint16 CppPropertyTraitMask =
	static_cast<uint16>(EAngelscriptPropertyTrait::CppConst)
	| static_cast<uint16>(EAngelscriptPropertyTrait::CppRef)
	| static_cast<uint16>(EAngelscriptPropertyTrait::CppEnumAsByte)
	| static_cast<uint16>(EAngelscriptPropertyTrait::ConstTemplateArg);

constexpr uint16 NativePropertyTraitMask =
	CppPropertyTraitMask
	| static_cast<uint16>(EAngelscriptPropertyTrait::WorldContext);

constexpr uint16 KnownPropertyTraitMask =
	NativePropertyTraitMask
	| static_cast<uint16>(EAngelscriptPropertyTrait::RuntimeGenerated);

constexpr uint16 NativeFunctionTraitMask =
	static_cast<uint16>(ESoloASBridgeFunctionTrait::ScriptCallable)
	| static_cast<uint16>(ESoloASBridgeFunctionTrait::WorldContext);

static_assert(
	KnownPropertyTraitMask == SoloASBridgePropertyTraitMask,
	"Runtime and Bridge property trait masks must remain identical."
);

const FName RuntimeGeneratedFunctionTraitOwner(
	TEXT("CS.Runtime.ReflectionAdapter.RuntimeGeneratedFunction")
);

FString GetReflectedCppName(const UStruct* Struct)
{
	if (Struct == nullptr)
		return {};

	FString Name(Struct->GetPrefixCPP());
	Name += Struct->GetName();
	return Name;
}

bool TryGetParameterOrdinal(
	const UFunction* Function,
	const FProperty* Property,
	uint32& OutOrdinal
)
{
	if (Function == nullptr || Property == nullptr)
		return false;

	uint32 Ordinal = 0;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		const FProperty* Candidate = *It;
		if (!Candidate->HasAnyPropertyFlags(CPF_Parm)
			|| Candidate->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			continue;
		}

		if (Candidate == Property)
		{
			OutOrdinal = Ordinal;
			return true;
		}
		++Ordinal;
	}
	return false;
}

uint16 GetNativeSidecarPropertyTraits(const FProperty* Property)
{
	const UFunction* Function = Property != nullptr
		? Cast<UFunction>(Property->GetOwnerStruct())
		: nullptr;
	if (Function == nullptr)
		return 0;

	uint32 ParameterOrdinal = 0;
	if (!TryGetParameterOrdinal(Function, Property, ParameterOrdinal))
		return 0;

	const FString OwnerName = GetReflectedCppName(Function->GetOuterUClass());
	const FString FunctionName = Function->GetName();
	const FString PropertyName = Property->GetName();
	const auto Metadata = SoloASBridge::GetScriptMetadata();
	const auto& Descriptors = Metadata->PropertyTraits;
	int32 First = 0;
	int32 Last = Descriptors.Num();
	// Match the exporter's ordinal owner/function/parameter/property ordering.
	while (First < Last)
	{
		const int32 Middle = First + (Last - First) / 2;
		const auto& Descriptor = Descriptors[Middle];
		int32 Order = OwnerName.Compare(
			ANSI_TO_TCHAR(Descriptor.OwnerName), ESearchCase::CaseSensitive);
		if (Order == 0)
		{
			Order = FunctionName.Compare(
				ANSI_TO_TCHAR(Descriptor.FunctionName), ESearchCase::CaseSensitive);
		}
		if (Order == 0 && ParameterOrdinal != Descriptor.ParameterOrdinal)
		{
			Order = ParameterOrdinal < Descriptor.ParameterOrdinal ? -1 : 1;
		}
		if (Order == 0)
		{
			Order = PropertyName.Compare(
				ANSI_TO_TCHAR(Descriptor.PropertyName), ESearchCase::CaseSensitive);
		}
		if (Order == 0)
		{
			return static_cast<uint16>(Descriptor.Traits)
				& NativePropertyTraitMask;
		}
		if (Order < 0)
			Last = Middle;
		else
			First = Middle + 1;
	}
	return 0;
}

uint16 GetNativeSidecarFunctionTraits(const UFunction* Function)
{
	if (Function == nullptr)
		return 0;

	const FString OwnerName = GetReflectedCppName(Function->GetOwnerStruct());
	const FString FunctionName = Function->GetName();
	const auto Metadata = SoloASBridge::GetScriptMetadata();
	const auto& Descriptors = Metadata->FunctionTraits;
	int32 Lower = 0;
	int32 Upper = Descriptors.Num();
	// The exporter sorts function traits by ordinal owner/function name.
	while (Lower < Upper)
	{
		const int32 Middle = Lower + (Upper - Lower) / 2;
		const auto& Descriptor = Descriptors[Middle];
		int32 Order = OwnerName.Compare(
			ANSI_TO_TCHAR(Descriptor.OwnerName), ESearchCase::CaseSensitive);
		if (Order == 0)
		{
			Order = FunctionName.Compare(
				ANSI_TO_TCHAR(Descriptor.FunctionName), ESearchCase::CaseSensitive);
		}
		if (Order == 0)
		{
			return static_cast<uint16>(Descriptor.Traits)
				& NativeFunctionTraitMask;
		}
		if (Order < 0)
			Upper = Middle;
		else
			Lower = Middle + 1;
	}
	return 0;
}

bool IsRuntimeGeneratedOwner(const UStruct* Owner)
{
	if (Owner == nullptr)
		return false;

	if (Owner->IsA<UASClass>()
		|| Owner->IsA<UASStruct>()
		|| Owner->IsA<UASFunction>())
	{
		return true;
	}

	const UFunction* OwnerFunction = Cast<UFunction>(Owner);
	return OwnerFunction != nullptr
		&& IsAngelscriptRuntimeGeneratedFunction(OwnerFunction);
}

bool IsRuntimeWorldContextProperty(const FProperty* Property)
{
	const UASFunction* Function = Property != nullptr
		? Cast<UASFunction>(Property->GetOwnerStruct())
		: nullptr;
	if (Function == nullptr || Function->WorldContextIndex < 0)
		return false;

	uint32 ParameterOrdinal = 0;
	return TryGetParameterOrdinal(Function, Property, ParameterOrdinal)
		&& ParameterOrdinal == static_cast<uint32>(Function->WorldContextIndex);
}

bool IsNativeMetadataWorldContextProperty(const FProperty* Property)
{
	const UFunction* Function = Property != nullptr
		? Cast<UFunction>(Property->GetOwnerStruct())
		: nullptr;
	if (Function == nullptr)
		return false;

                                                                                                         
// Cuviss-owned hook body
// Cuviss Studio owned guard: begin metadata-dependent WorldContext lookup.
#if WITH_METADATA
	const FString WorldContextName =
		Function->GetMetaData(TEXT("WorldContext"));
	return !WorldContextName.IsEmpty()
		&& Property->GetName().Equals(
			WorldContextName,
			ESearchCase::CaseSensitive
		);
#else
	return false;
#endif
// Cuviss Studio owned guard: end metadata-dependent WorldContext lookup.
                                                                                                       
}
}

EAngelscriptPropertyTrait GetAngelscriptPropertyTraits(
	const FProperty* Property
)
{
	if (Property == nullptr)
		return EAngelscriptPropertyTrait::None;

	uint16 TraitBits = 0;
	ESoloASBridgePropertyTrait RegisteredTraits =
		ESoloASBridgePropertyTrait::None;
	if (SoloASBridge::GetPropertyTraits(Property, RegisteredTraits))
	{
		TraitBits |= static_cast<uint16>(RegisteredTraits)
			& KnownPropertyTraitMask;
	}
	TraitBits |= GetNativeSidecarPropertyTraits(Property);

	if (IsRuntimeGeneratedOwner(Property->GetOwnerStruct()))
	{
		TraitBits |=
			static_cast<uint16>(EAngelscriptPropertyTrait::RuntimeGenerated);
	}

	if (IsRuntimeWorldContextProperty(Property)
		|| IsNativeMetadataWorldContextProperty(Property))
	{
		TraitBits |=
			static_cast<uint16>(EAngelscriptPropertyTrait::WorldContext);
	}

	return static_cast<EAngelscriptPropertyTrait>(TraitBits);
}

bool HasAngelscriptPropertyTrait(
	const FProperty* Property,
	EAngelscriptPropertyTrait Trait
)
{
	if (Trait == EAngelscriptPropertyTrait::None)
		return false;

	const uint16 TraitBits = static_cast<uint16>(Trait);
	const uint16 PropertyBits = static_cast<uint16>(
		GetAngelscriptPropertyTraits(Property)
	);
	return (PropertyBits & TraitBits) == TraitBits;
}

bool RegisterAngelscriptRuntimeGeneratedFunction(UFunction* Function)
{
	if (Function == nullptr || !Function->IsA<UDelegateFunction>())
		return false;

	return SoloASBridge::SetFunctionTraits(
		Function,
		RuntimeGeneratedFunctionTraitOwner,
		ESoloASBridgeFunctionTrait::RuntimeGenerated
	);
}

void ClearAngelscriptRuntimeGeneratedFunctionTraits()
{
	SoloASBridge::ClearFunctionTraits(
		RuntimeGeneratedFunctionTraitOwner
	);
}

bool IsAngelscriptRuntimeGeneratedFunction(const UFunction* Function)
{
	if (Function == nullptr)
		return false;

	const UStruct* Owner = Function->GetOwnerStruct();
	if (Function->IsA<UASFunction>()
		|| (Owner != nullptr
			&& (Owner->IsA<UASClass>() || Owner->IsA<UASStruct>())))
	{
		return true;
	}

	// Only delegate functions can carry the registered fallback below.
	if (!Function->IsA<UDelegateFunction>())
		return false;

	uint16 TraitBits = GetNativeSidecarFunctionTraits(Function);
	ESoloASBridgeFunctionTrait RegisteredTraits =
		ESoloASBridgeFunctionTrait::None;
	if (SoloASBridge::GetFunctionTraits(Function, RegisteredTraits))
	{
		TraitBits |= static_cast<uint16>(RegisteredTraits);
	}

	return Function->IsA<UDelegateFunction>()
		&& (TraitBits
		& static_cast<uint16>(
			ESoloASBridgeFunctionTrait::RuntimeGenerated
		)) != 0;
}
