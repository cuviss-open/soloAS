// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "Extensions/AngelscriptExtensionHooks.h"

#include <Shared/cuviss/hooks/anchor.h>

#include <memory>

namespace SoloASBridge::Extensions
{
class FProviderIdentityRegistration;
const Anchor::ContextKey<FAngelscriptTypeGenerationContext>&
GetAngelscriptTypeGenerationAnchor();
}

namespace AngelscriptExtensionPipelineInternal
{
constexpr uint8 PreParseStage = 1;
constexpr uint8 BlueprintEventBoundStage = 2;
constexpr uint8 NativeInterfaceSelectionStage = 3;
constexpr uint8 TypeGenerationStage = 4;

bool ValidateGameThread(FString& OutError);
bool IsStageRegistered(uint8 Stage);
bool IsProviderRegistered();

bool BeginProviderRegistration(FString& OutError);
bool CommitProviderRegistration(
	std::unique_ptr<
		SoloASBridge::Extensions::FProviderIdentityRegistration>&& Identity,
	uint64& OutSerial,
	FString& OutError);
bool UnregisterProvider(uint64& Serial, FString& OutError);

bool BeginStageRegistration(FString& OutError);
bool RegisterStageCallbackToken(
	uint8 Stage,
	Anchor::Token&& Token,
	const FAngelscriptExtensionCallbackRegistration& Registration,
	uint64& OutSerial,
	FString& OutError);
bool UnregisterStageCallback(
	uint64& Serial,
	uint8& Stage,
	FString& OutError);

const Anchor::ContextKey<FAngelscriptPreParseContext>&
GetAngelscriptPreParseAnchor();
const Anchor::ContextKey<FAngelscriptBlueprintEventBoundContext>&
GetAngelscriptBlueprintEventBoundAnchor();
const Anchor::ContextKey<FAngelscriptNativeInterfaceSelectionContext>&
GetAngelscriptNativeInterfaceSelectionAnchor();
const Anchor::ContextKey<FAngelscriptTypeGenerationContext>&
GetAngelscriptTypeGenerationAnchor();
}

// Runtime-only dispatch and cache identity; not part of the provider API.
bool GetAngelscriptExtensionCacheFingerprint(
	FString& OutFingerprint,
	FString& OutError
);
bool DispatchAngelscriptPreParse(
	FAngelscriptPreprocessor& Preprocessor,
	FString& OutError
);
bool DispatchAngelscriptNativeInterfaceSelection(
	UClass& NativeClass,
	bool& OutBindAsInterface,
	FString& OutError
);
bool DispatchAngelscriptNativeInterfaceBinding(FAngelscriptNativeInterfaceSelectionContext& Context);

/** Describe/bind a reflected event using Runtime's normal signature and bind database rules. */
bool DescribeAngelscriptBlueprintEvent(TSharedRef<FAngelscriptType> Type, UFunction* Function,
    FAngelscriptMethodBind& MethodBind, FAngelscriptBlueprintEventBindingSnapshot& Snapshot, bool bFromDatabase);
void BindAngelscriptBlueprintEvent(TSharedRef<FAngelscriptType> Type, UFunction* Function,
    FAngelscriptMethodBind& MethodBind);
