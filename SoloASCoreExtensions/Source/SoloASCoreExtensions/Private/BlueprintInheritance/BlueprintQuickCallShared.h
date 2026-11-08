// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.

#pragma once

#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#if __has_include("UObject/StrProperty.h")
#include "UObject/StrProperty.h"
#endif

namespace AngelscriptBlueprintQuickCall
{
inline void ThrowQuickCallError(const FString& Message)
{
	FAngelscriptManager::Throw(TCHAR_TO_ANSI(*Message));
}

inline FString GetAngelscriptTypeName(int32 TypeId)
{
	const FAngelscriptTypeUsage Usage = FAngelscriptTypeUsage::FromTypeId(TypeId);
	return Usage.IsValid() ? Usage.GetFriendlyTypeName() : TEXT("unsupported type");
}

inline bool IsSupportedBlueprintProperty(const FProperty* Property)
{
	return Property != nullptr
		&& (Property->IsA<FBoolProperty>()
			|| Property->IsA<FNumericProperty>()
			|| Property->IsA<FEnumProperty>()
			|| Property->IsA<FObjectPropertyBase>()
			|| Property->IsA<FNameProperty>()
			|| Property->IsA<FStrProperty>()
			|| Property->IsA<FTextProperty>()
			|| Property->IsA<FStructProperty>());
}

inline bool IsBlueprintTypeCompatible(int32 TypeId, FProperty* Property, bool bOutput = false)
{
	if (Property == nullptr || !IsSupportedBlueprintProperty(Property))
		return false;

	const FAngelscriptTypeUsage Actual = FAngelscriptTypeUsage::FromTypeId(TypeId);
	const FAngelscriptTypeUsage Expected = FAngelscriptTypeUsage::FromProperty(Property);
	if (!Actual.IsValid() || !Expected.IsValid())
		return false;

	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		if (!Actual.IsObjectPointer() || !Expected.IsObjectPointer())
			return Actual.EqualsUnqualified(Expected);
		const UClass* ActualClass = Actual.GetClass();
		return ActualClass != nullptr && (bOutput
			? ObjectProperty->PropertyClass->IsChildOf(ActualClass)
			: ActualClass->IsChildOf(ObjectProperty->PropertyClass));
	}

	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		return Actual.GetUnrealStruct() == StructProperty->Struct;

	return Actual.EqualsUnqualified(Expected);
}

inline void CopyValue(FProperty* Property, void* Destination, const void* Source)
{
	Property->CopyCompleteValue(Destination, Source);
}

inline FProperty* FindNamedProperty(UObject* Object, const FName& Name)
{
	return Object != nullptr && !Name.IsNone()
		? FindFProperty<FProperty>(Object->GetClass(), Name)
		: nullptr;
}
}
