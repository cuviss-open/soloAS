// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UClass;
class UObject;
struct FImportObjectParams;

enum class EAngelscriptEditorObjectImportResult : uint8
{
	Completed,
	NullClass,
	NullTemplate,
	NullOuter,
	EmptyName,
	InvalidClass,
	InvalidOuter,
	DuplicateName,
	InvalidTemplate,
	TransientTemplate,
	MismatchedTemplateClass,
	CreationFailed,
	NullTarget,
	NullReplacement,
	SameTargetAndReplacement,
	InvalidTarget,
	TransientTarget,
	UnsupportedTarget,
	InvalidReplacement,
	TransientReplacement,
	UnsupportedReplacement,
	MismatchedReplacementClass,
	EmptyPropertySteps,
	DuplicatePropertyTarget,
	MissingSourceProperty,
	MissingTargetProperty,
	UnsupportedProperty,
	MismatchedPropertyType,
};

struct FAngelscriptEditorObjectPreCreateRequest
{
	UClass* Class = nullptr;
	UObject* Template = nullptr;
	UObject* Outer = nullptr;
	FName Name = NAME_None;
};

struct FAngelscriptEditorObjectPropertyImportStep
{
	FName SourceProperty = NAME_None;
	FName TargetProperty = NAME_None;
};

struct FAngelscriptEditorObjectImportRequest
{
	UObject* Target = nullptr;
	UObject* Replacement = nullptr;
	TArray<FAngelscriptEditorObjectPropertyImportStep> Steps;
};

/** Public-API-only pre-create/import contract for the EditorObject seam. */
class ANGELSCRIPTEDITOR_API FAngelscriptEditorObjectPrecreateImportAdapter
{
public:
	/** Import property/subobject text into an existing editor object, including a CDO. */
	static bool ImportObjectText(UObject* Object, const FString& Text);

	static const TCHAR* GetMarker();

	static EAngelscriptEditorObjectImportResult ValidatePreCreate(
		const FAngelscriptEditorObjectPreCreateRequest& Request);

	static UObject* PreCreate(
		const FAngelscriptEditorObjectPreCreateRequest& Request,
		EAngelscriptEditorObjectImportResult& OutResult);

	static EAngelscriptEditorObjectImportResult ValidateImport(
		const FAngelscriptEditorObjectImportRequest& Request);

	static EAngelscriptEditorObjectImportResult ImportProperties(
		const FAngelscriptEditorObjectImportRequest& Request);

	/**
	 * Import the original object text with definition-time identity reuse by actual
	 * class, outer and name. Uses native public property import and finalization.
	 * Returns the consumed source cursor, or null after a partial import failure.
	 * Brush blocks use the native model factory and final static/moving preparation.
	 * Foliage blocks use public level routing, type resolution and instance insertion.
	 * Whole-record equivalence still requires validation of the specialized paths.
	 */
	static const TCHAR* ImportWholeText(FImportObjectParams& Params);

	/** Route external native text imports only when an AS object is involved. */
	static bool ShouldUseImportWholeText(const FImportObjectParams& Params);
};
