// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "UMG/AngelscriptListViewEntryAdapter.h"

#include "Runtime/UMG/Public/Blueprint/UserWidget.h"
#include "UObject/Class.h"

const TCHAR* FAngelscriptListViewEntryAdapter::GetMarker()
{
	// LISTVIEW_ENTRY_COMPOSITION_MARKER identifies this explicit composition adapter.
	return TEXT("AngelscriptListViewEntryWidgetComposition");
}

EAngelscriptListViewEntryAdapterResult
FAngelscriptListViewEntryAdapter::ValidateEntryWidgetClass(
	const UClass* EntryWidgetClass
)
{
	if (EntryWidgetClass == nullptr)
	{
		return EAngelscriptListViewEntryAdapterResult::NullEntryWidgetClass;
	}

	if (!IsValid(EntryWidgetClass))
	{
		return EAngelscriptListViewEntryAdapterResult::InvalidEntryWidgetClass;
	}

	if (!EntryWidgetClass->IsChildOf(UUserWidget::StaticClass()))
	{
		return EAngelscriptListViewEntryAdapterResult::EntryWidgetClassNotUserWidget;
	}

	if (EntryWidgetClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return EAngelscriptListViewEntryAdapterResult::EntryWidgetClassAbstract;
	}

	if (EntryWidgetClass->HasAnyClassFlags(CLASS_Deprecated))
	{
		return EAngelscriptListViewEntryAdapterResult::EntryWidgetClassDeprecated;
	}

	if (EntryWidgetClass->HasAnyClassFlags(CLASS_NewerVersionExists))
	{
		return EAngelscriptListViewEntryAdapterResult::EntryWidgetClassNewerVersion;
	}

	// Script/generated entry classes use the stock compiled-from-blueprint
	// reflection marker. Native, interface, and unmarked classes are outside
	// this external composition subset.
	if (EntryWidgetClass->HasAnyClassFlags(CLASS_Native | CLASS_Interface)
		|| !EntryWidgetClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
	{
		return EAngelscriptListViewEntryAdapterResult::EntryWidgetClassUnsupported;
	}

	return EAngelscriptListViewEntryAdapterResult::Accepted;
}

EAngelscriptListViewEntryAdapterResult
FAngelscriptListViewEntryAdapter::ComposeEntryWidget(
	UObject* WorldContextObject,
	UClass* EntryWidgetClass,
	UObject* ListItemObject,
	const FCreateEntryWidget& CreateEntryWidget,
	const FOnListItemObjectSet& OnListItemObjectSet,
	UUserWidget*& OutEntryWidget
)
{
	OutEntryWidget = nullptr;

	const EAngelscriptListViewEntryAdapterResult ClassResult =
		ValidateEntryWidgetClass(EntryWidgetClass);
	if (ClassResult != EAngelscriptListViewEntryAdapterResult::Accepted)
	{
		return ClassResult;
	}

	if (!IsValid(WorldContextObject))
	{
		return EAngelscriptListViewEntryAdapterResult::NullWorldContext;
	}

	if (!IsValid(ListItemObject))
	{
		return EAngelscriptListViewEntryAdapterResult::NullListItem;
	}

	if (!CreateEntryWidget)
	{
		return EAngelscriptListViewEntryAdapterResult::MissingCreateCallback;
	}

	if (!OnListItemObjectSet)
	{
		return EAngelscriptListViewEntryAdapterResult::MissingObjectSetCallback;
	}

	UUserWidget* EntryWidget = CreateEntryWidget(WorldContextObject, EntryWidgetClass);
	if (!IsValid(EntryWidget))
	{
		return EAngelscriptListViewEntryAdapterResult::FactoryReturnedInvalidWidget;
	}

	if (EntryWidget->GetClass() != EntryWidgetClass)
	{
		return EAngelscriptListViewEntryAdapterResult::FactoryReturnedWrongClass;
	}

	OutEntryWidget = EntryWidget;
	OnListItemObjectSet(EntryWidget, ListItemObject);
	if (!IsValid(EntryWidget) || EntryWidget->GetClass() != EntryWidgetClass)
	{
		OutEntryWidget = nullptr;
		return EAngelscriptListViewEntryAdapterResult::ObjectSetCallbackInvalidatedWidget;
	}

	return EAngelscriptListViewEntryAdapterResult::Accepted;
}

EAngelscriptListViewEntryAdapterResult
FAngelscriptListViewEntryAdapter::ReleaseEntryWidget(
	UUserWidget* EntryWidget,
	const FOnEntryReleased& OnEntryReleased
)
{
	if (!IsValid(EntryWidget))
	{
		return EAngelscriptListViewEntryAdapterResult::InvalidReleasedWidget;
	}

	if (!OnEntryReleased)
	{
		return EAngelscriptListViewEntryAdapterResult::MissingReleaseCallback;
	}

	OnEntryReleased(EntryWidget);
	return EAngelscriptListViewEntryAdapterResult::Accepted;
}
