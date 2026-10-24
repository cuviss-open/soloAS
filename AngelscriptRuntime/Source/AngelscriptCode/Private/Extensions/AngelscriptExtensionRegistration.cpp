// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptExtensionPipelineInternal.h"

#include "Shared/SoloASBridgeExtensionIdentity.h"

#include <utility>

namespace
{
FAngelscriptNativeInterfaceSelectionExtensionCallback NativeInterfaceBindingCallback = nullptr;
using AngelscriptExtensionPipelineInternal::BeginProviderRegistration;
using AngelscriptExtensionPipelineInternal::BeginStageRegistration;
using AngelscriptExtensionPipelineInternal::BlueprintEventBoundStage;
using AngelscriptExtensionPipelineInternal::CommitProviderRegistration;
using AngelscriptExtensionPipelineInternal::GetAngelscriptBlueprintEventBoundAnchor;
using AngelscriptExtensionPipelineInternal::GetAngelscriptNativeInterfaceSelectionAnchor;
using AngelscriptExtensionPipelineInternal::GetAngelscriptPreParseAnchor;
using AngelscriptExtensionPipelineInternal::NativeInterfaceSelectionStage;
using AngelscriptExtensionPipelineInternal::TypeGenerationStage;
using AngelscriptExtensionPipelineInternal::GetAngelscriptTypeGenerationAnchor;
using AngelscriptExtensionPipelineInternal::PreParseStage;
using AngelscriptExtensionPipelineInternal::RegisterStageCallbackToken;
using AngelscriptExtensionPipelineInternal::UnregisterProvider;
using AngelscriptExtensionPipelineInternal::UnregisterStageCallback;
using AngelscriptExtensionPipelineInternal::ValidateGameThread;
}

FAngelscriptExtensionProviderRegistration::FAngelscriptExtensionProviderRegistration(
	FAngelscriptExtensionProviderRegistration&& Other) noexcept
	: Serial(Other.Serial)
{
	Other.Serial = 0;
}

FAngelscriptExtensionProviderRegistration&
FAngelscriptExtensionProviderRegistration::operator=(
	FAngelscriptExtensionProviderRegistration&& Other) noexcept
{
	if (this == &Other)
		return *this;
	if (!ensureMsgf(
			!IsValid(),
			TEXT("An active AngelScript extension registration must be explicitly unregistered before replacement.")))
	{
		return *this;
	}
	Serial = Other.Serial;
	Other.Serial = 0;
	return *this;
}

bool FAngelscriptExtensionProviderRegistration::IsValid() const
{
	return Serial != 0;
}

FAngelscriptExtensionCallbackRegistration::FAngelscriptExtensionCallbackRegistration(
	FAngelscriptExtensionCallbackRegistration&& Other) noexcept
	: Serial(Other.Serial)
	, Stage(Other.Stage)
{
	Other.Serial = 0;
	Other.Stage = 0;
}

FAngelscriptExtensionCallbackRegistration&
FAngelscriptExtensionCallbackRegistration::operator=(
	FAngelscriptExtensionCallbackRegistration&& Other) noexcept
{
	if (this == &Other)
		return *this;
	if (!ensureMsgf(
			!IsValid(),
			TEXT("An active AngelScript extension callback registration must be ")
			TEXT("explicitly unregistered before replacement.")))
	{
		return *this;
	}
	Serial = Other.Serial;
	Stage = Other.Stage;
	Other.Serial = 0;
	Other.Stage = 0;
	return *this;
}

bool FAngelscriptExtensionCallbackRegistration::IsValid() const
{
	return Serial != 0 && Stage != 0;
}

bool IsAngelscriptExtensionStageRegistered(
	EAngelscriptExtensionStage Stage)
{
	return AngelscriptExtensionPipelineInternal::IsStageRegistered(
		static_cast<uint8>(Stage));
}

bool IsAngelscriptExtensionProviderRegistered()
{
	return AngelscriptExtensionPipelineInternal::IsProviderRegistered();
}

bool RegisterAngelscriptExtensionProvider(
	const FAngelscriptExtensionProvider& Provider,
	FAngelscriptExtensionProviderRegistration& OutRegistration,
	FString& OutError)
{
	OutError.Reset();
	if (!ValidateGameThread(OutError))
		return false;
	if (Provider.ProviderId.IsNone())
	{
		OutError = TEXT("AngelScript extension provider ID must be stable and non-empty.");
		return false;
	}
	if (OutRegistration.IsValid())
	{
		OutError = TEXT("AngelScript extension registration output is already active.");
		return false;
	}
	if (!BeginProviderRegistration(OutError))
		return false;

	std::unique_ptr<SoloASBridge::Extensions::FProviderIdentityRegistration>
		BridgeRegistration(
			new SoloASBridge::Extensions::FProviderIdentityRegistration());
	SoloASBridge::Extensions::FProviderIdentity Identity;
	Identity.ProviderId = Provider.ProviderId;
	Identity.CompatibilityTag = Provider.CompatibilityTag;
	if (!SoloASBridge::Extensions::RegisterProviderIdentity(
			Identity, *BridgeRegistration, OutError))
	{
		return false;
	}

	if (!CommitProviderRegistration(
			std::move(BridgeRegistration), OutRegistration.Serial, OutError))
	{
		return false;
	}
	return true;
}

bool UnregisterAngelscriptExtensionProvider(
	FAngelscriptExtensionProviderRegistration& Registration,
	FString& OutError)
{
	OutError.Reset();
	if (!ValidateGameThread(OutError))
		return false;
	if (!Registration.IsValid())
	{
		OutError = TEXT("AngelScript extension registration is invalid.");
		return false;
	}
	return UnregisterProvider(Registration.Serial, OutError);
}

bool RegisterAngelscriptPreParseExtension(
	FAngelscriptPreParseExtensionCallback Callback,
	FAngelscriptExtensionCallbackRegistration& OutRegistration,
	FString& OutError)
{
	OutError.Reset();
	if (Callback == nullptr)
	{
		OutError = TEXT("AngelScript extension callback must not be null.");
		return false;
	}
	if (!BeginStageRegistration(OutError))
		return false;
	Anchor::Token Token = Anchor::Register(
		GetAngelscriptPreParseAnchor(),
		[Callback](FAngelscriptPreParseContext& Context)
		{
			Context.Result = Callback(Context);
		});
	uint64 Serial = 0;
	if (!RegisterStageCallbackToken(
			PreParseStage, std::move(Token), OutRegistration, Serial, OutError))
	{
		return false;
	}
	OutRegistration.Serial = Serial;
	OutRegistration.Stage = PreParseStage;
	return true;
}

bool RegisterAngelscriptBlueprintEventBoundExtension(
	FAngelscriptBlueprintEventBoundExtensionCallback Callback,
	FAngelscriptExtensionCallbackRegistration& OutRegistration,
	FString& OutError)
{
	OutError.Reset();
	if (Callback == nullptr)
	{
		OutError = TEXT("AngelScript extension callback must not be null.");
		return false;
	}
	if (!BeginStageRegistration(OutError))
		return false;
	Anchor::Token Token = Anchor::Register(
		GetAngelscriptBlueprintEventBoundAnchor(),
		[Callback](FAngelscriptBlueprintEventBoundContext& Context)
		{
			Context.Result = Callback(Context);
		});
	uint64 Serial = 0;
	if (!RegisterStageCallbackToken(
			BlueprintEventBoundStage,
			std::move(Token),
			OutRegistration,
			Serial,
			OutError))
	{
		return false;
	}
	OutRegistration.Serial = Serial;
	OutRegistration.Stage = BlueprintEventBoundStage;
	return true;
}

bool RegisterAngelscriptNativeInterfaceSelectionExtension(
	FAngelscriptNativeInterfaceSelectionExtensionCallback Callback,
	FAngelscriptExtensionCallbackRegistration& OutRegistration,
	FString& OutError,
    FAngelscriptNativeInterfaceSelectionExtensionCallback BindingCallback)
{
	OutError.Reset();
	if (Callback == nullptr)
	{
		OutError = TEXT("AngelScript extension callback must not be null.");
		return false;
	}
	if (!BeginStageRegistration(OutError))
		return false;
	Anchor::Token Token = Anchor::Register(
		GetAngelscriptNativeInterfaceSelectionAnchor(),
		[Callback](FAngelscriptNativeInterfaceSelectionContext& Context)
		{
			Context.Result = Callback(Context);
		});
	uint64 Serial = 0;
	if (!RegisterStageCallbackToken(
			NativeInterfaceSelectionStage,
			std::move(Token),
			OutRegistration,
			Serial,
			OutError))
	{
		return false;
	}
	OutRegistration.Serial = Serial;
	OutRegistration.Stage = NativeInterfaceSelectionStage;
    NativeInterfaceBindingCallback = BindingCallback;
	return true;
}
bool RegisterAngelscriptTypeGenerationExtension(
	FAngelscriptTypeGenerationExtensionCallback Callback,
	FAngelscriptExtensionCallbackRegistration& OutRegistration,
	FString& OutError)
{
	OutError.Reset();
	if (Callback == nullptr)
	{
		OutError = TEXT("AngelScript extension callback must not be null.");
		return false;
	}
	if (!BeginStageRegistration(OutError))
		return false;
	Anchor::Token Token = Anchor::Register(
		GetAngelscriptTypeGenerationAnchor(),
		[Callback](FAngelscriptTypeGenerationContext& Context)
		{
			Context.Result = Callback(Context);
		});
	uint64 Serial = 0;
	if (!RegisterStageCallbackToken(
			TypeGenerationStage,
			std::move(Token),
			OutRegistration,
			Serial,
			OutError))
	{
		return false;
	}
	OutRegistration.Serial = Serial;
	OutRegistration.Stage = TypeGenerationStage;
	return true;
}

bool UnregisterAngelscriptExtensionCallback(
	FAngelscriptExtensionCallbackRegistration& Registration,
	FString& OutError)
{
	OutError.Reset();
	if (!ValidateGameThread(OutError))
		return false;
	if (!Registration.IsValid())
	{
		OutError = TEXT("AngelScript extension callback registration is invalid.");
		return false;
	}
    const bool bNativeInterface = Registration.Stage == NativeInterfaceSelectionStage;
    if (!UnregisterStageCallback(Registration.Serial, Registration.Stage, OutError)) return false;
    if (bNativeInterface) NativeInterfaceBindingCallback = nullptr;
    return true;
}

bool GetAngelscriptExtensionCacheFingerprint(
	FString& OutFingerprint,
	FString& OutError)
{
	return SoloASBridge::Extensions::GetSealedProviderIdentityFingerprint(
		OutFingerprint, OutError);
}

bool DispatchAngelscriptNativeInterfaceBinding(FAngelscriptNativeInterfaceSelectionContext& Context)
{
    // Binding may emit the ordinary type/event callbacks. Its pointer shares the selection token's lifetime.
    if (!NativeInterfaceBindingCallback) return false;
    check(IsInGameThread());
    Context.Result = NativeInterfaceBindingCallback(Context);
    if (Context.Result == EAngelscriptExtensionResult::Error)
        UE_LOG(LogTemp, Fatal, TEXT("Native interface binding failed: %s"), *Context.Error);
    return Context.Result == EAngelscriptExtensionResult::Handled;
}
