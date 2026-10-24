// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Blueprint/AngelscriptBlueprintDefaultCopyFilterAdapter.h"

#include "UObject/Class.h"
#include "UObject/StrProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr EPropertyFlags UnsafePropertyFlags =
	CPF_Transient
	| CPF_EditConst
	| CPF_DisableEditOnInstance
	| CPF_DisableEditOnTemplate
	| CPF_InstancedReference
	| CPF_ContainsInstancedReference
	| CPF_ExportObject
	| CPF_PersistentInstance
	| CPF_EditorOnly
	| CPF_SkipSerialization;

thread_local int32 GAngelscriptBlueprintDefaultCopyDepth = 0;

bool HasUnsafePolicyFlags(const FProperty& Property)
{
	return Property.HasAnyPropertyFlags(UnsafePropertyFlags);
}

bool IsSupportedPropertyKind(const FProperty& Property)
{
	return Property.IsA<FBoolProperty>()
		|| Property.IsA<FNumericProperty>()
		|| Property.IsA<FNameProperty>()
		|| Property.IsA<FStrProperty>()
		|| Property.IsA<FTextProperty>();
}
}

EAngelscriptBlueprintDefaultCopyResult
FAngelscriptBlueprintDefaultCopyFilterAdapter::Validate(
	const FAngelscriptBlueprintDefaultCopyRequest& Request)
{
	if (Request.Source == nullptr)
		return EAngelscriptBlueprintDefaultCopyResult::NullSource;
	if (Request.Target == nullptr)
		return EAngelscriptBlueprintDefaultCopyResult::NullTarget;
	if (!IsValid(Request.Source) || !IsValid(Request.Target)
		|| Request.Source->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| Request.Target->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		return EAngelscriptBlueprintDefaultCopyResult::InvalidObject;
	if (Request.Source == Request.Target)
		return EAngelscriptBlueprintDefaultCopyResult::SameObject;
	if (Request.bRecursiveCopy)
		return EAngelscriptBlueprintDefaultCopyResult::RecursiveCopy;
	if (Request.Source->GetClass() != Request.Target->GetClass())
		return EAngelscriptBlueprintDefaultCopyResult::ClassMismatch;
	if (Request.PropertyNames.IsEmpty())
		return EAngelscriptBlueprintDefaultCopyResult::EmptyPropertySelection;
	if (!Request.PropertyPolicy)
		return EAngelscriptBlueprintDefaultCopyResult::UnsafePropertyPolicy;

	TSet<FName> SeenProperties;
	for (const FName PropertyName : Request.PropertyNames)
	{
		if (PropertyName.IsNone())
			return EAngelscriptBlueprintDefaultCopyResult::InvalidPropertyName;
		if (SeenProperties.Contains(PropertyName))
			return EAngelscriptBlueprintDefaultCopyResult::DuplicateProperty;
		SeenProperties.Add(PropertyName);

		const FProperty* SourceProperty = FindFProperty<FProperty>(
			Request.Source->GetClass(), PropertyName);
		const FProperty* TargetProperty = FindFProperty<FProperty>(
			Request.Target->GetClass(), PropertyName);
		if (SourceProperty == nullptr || TargetProperty == nullptr)
			return EAngelscriptBlueprintDefaultCopyResult::MissingProperty;
		if (SourceProperty->GetClass() != TargetProperty->GetClass()
			|| SourceProperty->GetSize() != TargetProperty->GetSize()
			|| SourceProperty->ArrayDim != TargetProperty->ArrayDim)
			return EAngelscriptBlueprintDefaultCopyResult::PropertyTypeMismatch;
		if (HasUnsafePolicyFlags(*SourceProperty) || HasUnsafePolicyFlags(*TargetProperty))
			return EAngelscriptBlueprintDefaultCopyResult::UnsafeProperty;
		if (!IsSupportedPropertyKind(*SourceProperty)
			|| !IsSupportedPropertyKind(*TargetProperty))
			return EAngelscriptBlueprintDefaultCopyResult::UnsupportedPropertyKind;
		if (!Request.PropertyPolicy(*SourceProperty, *TargetProperty))
			return EAngelscriptBlueprintDefaultCopyResult::PropertyRejectedByPolicy;
	}

	return EAngelscriptBlueprintDefaultCopyResult::Accepted;
}

EAngelscriptBlueprintDefaultCopyResult
FAngelscriptBlueprintDefaultCopyFilterAdapter::Copy(
	const FAngelscriptBlueprintDefaultCopyRequest& Request)
{
	if (GAngelscriptBlueprintDefaultCopyDepth > 0)
		return EAngelscriptBlueprintDefaultCopyResult::RecursiveCopy;

	const EAngelscriptBlueprintDefaultCopyResult ValidationResult = Validate(Request);
	if (ValidationResult != EAngelscriptBlueprintDefaultCopyResult::Accepted)
		return ValidationResult;

	Request.Trace.Add(EAngelscriptBlueprintDefaultCopyTracePhase::ValidationAccepted);
	++GAngelscriptBlueprintDefaultCopyDepth;
	for (const FName PropertyName : Request.PropertyNames)
	{
		const FProperty* SourceProperty = FindFProperty<FProperty>(
			Request.Source->GetClass(), PropertyName);
		const FProperty* TargetProperty = FindFProperty<FProperty>(
			Request.Target->GetClass(), PropertyName);
		TargetProperty->CopyCompleteValue(
			TargetProperty->ContainerPtrToValuePtr<void>(Request.Target),
			SourceProperty->ContainerPtrToValuePtr<void>(Request.Source));
	}
	--GAngelscriptBlueprintDefaultCopyDepth;
	Request.Trace.Add(EAngelscriptBlueprintDefaultCopyTracePhase::CopyCallReturned);
	return EAngelscriptBlueprintDefaultCopyResult::Accepted;
}

bool FAngelscriptBlueprintDefaultCopyFilterAdapter::HasCapability(
	EAngelscriptBlueprintDefaultCopyCapability Capability)
{
	return Capability == EAngelscriptBlueprintDefaultCopyCapability::ExternalPropertyCopy;
}
