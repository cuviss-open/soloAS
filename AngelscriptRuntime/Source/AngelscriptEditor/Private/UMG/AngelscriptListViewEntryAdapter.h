// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UClass;
class UObject;
class UUserWidget;

enum class EAngelscriptListViewEntryAdapterResult : uint8
{
	Accepted,
	NullEntryWidgetClass,
	EntryWidgetClassNotUserWidget,
	EntryWidgetClassAbstract,
	EntryWidgetClassDeprecated,
	EntryWidgetClassNewerVersion,
	EntryWidgetClassUnsupported,
	NullWorldContext,
	NullListItem,
	MissingCreateCallback,
	MissingObjectSetCallback,
	FactoryReturnedInvalidWidget,
	FactoryReturnedWrongClass,
	ObjectSetCallbackInvalidatedWidget,
	MissingReleaseCallback,
	InvalidReleasedWidget,
	InvalidEntryWidgetClass,
};

/**
 * Explicit external composition for a script-backed ListView entry widget.
 *
 * The caller owns ListView integration and widget creation. This contract only
 * validates the class, validates the factory result, and forwards the two
 * public list-entry lifecycle callbacks in a fail-closed order. Composition
 * revalidates the widget after object binding before publishing success. It
 * does not replace the stock final list rebuild path or claim Haze lifecycle
 * parity.
 */
class ANGELSCRIPTEDITOR_API FAngelscriptListViewEntryAdapter
{
public:
	using FCreateEntryWidget = TFunction<UUserWidget*(UObject* WorldContextObject, UClass* EntryWidgetClass)>;
	using FOnListItemObjectSet = TFunction<void(UUserWidget* EntryWidget, UObject* ListItemObject)>;
	using FOnEntryReleased = TFunction<void(UUserWidget* EntryWidget)>;

	static const TCHAR* GetMarker();

	static EAngelscriptListViewEntryAdapterResult ValidateEntryWidgetClass(
		const UClass* EntryWidgetClass
	);

	static EAngelscriptListViewEntryAdapterResult ComposeEntryWidget(
		UObject* WorldContextObject,
		UClass* EntryWidgetClass,
		UObject* ListItemObject,
		const FCreateEntryWidget& CreateEntryWidget,
		const FOnListItemObjectSet& OnListItemObjectSet,
		UUserWidget*& OutEntryWidget
	);

	static EAngelscriptListViewEntryAdapterResult ReleaseEntryWidget(
		UUserWidget* EntryWidget,
		const FOnEntryReleased& OnEntryReleased
	);
};
