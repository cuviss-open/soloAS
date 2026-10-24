// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptExtensionPipelineInternal.h"
#include "AngelscriptExecutionQuiescence.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Shared/SoloASBridgeExtensionIdentity.h"

#include <array>
#include <mutex>
#include <memory>
#include <utility>

namespace AngelscriptExtensionPipelineInternal
{
bool ValidateGameThread(FString& OutError)
{
	if (IsInGameThread())
		return true;
	OutError = TEXT("AngelScript extension pipeline access requires the game thread.");
	return false;
}

const Anchor::ContextKey<FAngelscriptPreParseContext>&
GetAngelscriptPreParseAnchor()
{
	static const Anchor::ContextKey<FAngelscriptPreParseContext> Key =
		Anchor::MakeContextKey<FAngelscriptPreParseContext>(
			"cuviss.angelscript.extension/pre-parse/v1",
			0x4353415350524501ULL,
			1);
	return Key;
}

const Anchor::ContextKey<FAngelscriptBlueprintEventBoundContext>&
GetAngelscriptBlueprintEventBoundAnchor()
{
	static const Anchor::ContextKey<FAngelscriptBlueprintEventBoundContext> Key =
		Anchor::MakeContextKey<FAngelscriptBlueprintEventBoundContext>(
			"cuviss.angelscript.extension/blueprint-event-bound/v1",
			0x4353415342565401ULL,
			1);
	return Key;
}

const Anchor::ContextKey<FAngelscriptNativeInterfaceSelectionContext>&
GetAngelscriptNativeInterfaceSelectionAnchor()
{
	static const Anchor::ContextKey<FAngelscriptNativeInterfaceSelectionContext>
		Key = Anchor::MakeContextKey<
			FAngelscriptNativeInterfaceSelectionContext>(
			"cuviss.angelscript.extension/native-interface-selection/v1",
			0x435341534E494601ULL,
			1);
	return Key;
}

const Anchor::ContextKey<FAngelscriptTypeGenerationContext>&
GetAngelscriptTypeGenerationAnchor()
{
	static const auto Key = Anchor::MakeContextKey<FAngelscriptTypeGenerationContext>(
		"cuviss.angelscript.extension/type-generation/v1", 0x4353415354595001ULL, 1);
	return Key;
}

}

namespace
{
using AngelscriptExtensionPipelineInternal::BlueprintEventBoundStage;
using AngelscriptExtensionPipelineInternal::GetAngelscriptBlueprintEventBoundAnchor;
using AngelscriptExtensionPipelineInternal::GetAngelscriptNativeInterfaceSelectionAnchor;
using AngelscriptExtensionPipelineInternal::GetAngelscriptPreParseAnchor;
using AngelscriptExtensionPipelineInternal::NativeInterfaceSelectionStage;
using AngelscriptExtensionPipelineInternal::TypeGenerationStage;
using AngelscriptExtensionPipelineInternal::GetAngelscriptTypeGenerationAnchor;
using AngelscriptExtensionPipelineInternal::PreParseStage;
using AngelscriptExtensionPipelineInternal::ValidateGameThread;

struct FStageRegistrationSlot
{
	uint64 Serial = 0;
	uint8 Stage = 0;
	std::unique_ptr<Anchor::Token> Token;

	bool IsValid() const
	{
		return Serial != 0 && Stage != 0 && Token && static_cast<bool>(*Token);
	}
};

struct FAngelscriptExtensionPipelineState
{
	FCriticalSection Mutex;
	uint64 NextSerial = 1;
	uint64 ProviderSerial = 0;
	std::unique_ptr<SoloASBridge::Extensions::FProviderIdentityRegistration>
		ProviderIdentity;
	std::array<FStageRegistrationSlot, 4> Stages;
	bool bCompilationObserved = false;
	bool bDispatchActive = false;
};

FAngelscriptExtensionPipelineState& GetPipelineState()
{
	static FAngelscriptExtensionPipelineState State;
	return State;
}

bool ValidateCallbackResult(
	EAngelscriptExtensionResult Result,
	const FString& CallbackError,
	const TCHAR* Stage,
	FString& OutError)
{
	switch (Result)
	{
	case EAngelscriptExtensionResult::NotHandled:
	case EAngelscriptExtensionResult::Handled:
		return true;
	case EAngelscriptExtensionResult::Error:
		if (CallbackError.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("AngelScript extension provider returned Error without a message during %s."),
				Stage);
			return false;
		}
		OutError = CallbackError;
		return false;
	default:
		OutError = FString::Printf(
			TEXT("AngelScript extension provider returned an invalid result during %s."),
			Stage);
		return false;
	}
}

FStageRegistrationSlot* FindStageSlot(
	FAngelscriptExtensionPipelineState& State,
	uint8 Stage)
{
	if (Stage < PreParseStage || Stage > TypeGenerationStage)
		return nullptr;
	return &State.Stages[Stage - 1];
}

const FStageRegistrationSlot* FindStageSlot(
	const FAngelscriptExtensionPipelineState& State,
	uint8 Stage)
{
	if (Stage < PreParseStage || Stage > TypeGenerationStage)
		return nullptr;
	return &State.Stages[Stage - 1];
}

bool BeginDispatch(
	FAngelscriptExtensionPipelineState& State,
	FString& OutError)
{
	OutError.Reset();
	if (!ValidateGameThread(OutError))
		return false;

	FScopeLock Lock(&State.Mutex);
	if (State.bDispatchActive)
	{
		OutError = TEXT("Recursive AngelScript extension dispatch is forbidden.");
		return false;
	}
	State.bDispatchActive = true;
	State.bCompilationObserved = true;
	return true;
}

class FScopedExtensionDispatch final
{
public:
	explicit FScopedExtensionDispatch(FAngelscriptExtensionPipelineState& InState)
		: State(InState)
	{
	}
	~FScopedExtensionDispatch()
	{
		FScopeLock Lock(&State.Mutex);
		State.bDispatchActive = false;
	}
private:
	FAngelscriptExtensionPipelineState& State;
};
void WarmAnchorKeys()
{
	static std::once_flag WarmOnce;
	std::call_once(WarmOnce, [] {
		(void)GetAngelscriptPreParseAnchor();
		(void)GetAngelscriptTypeGenerationAnchor();
		(void)GetAngelscriptBlueprintEventBoundAnchor();
		(void)GetAngelscriptNativeInterfaceSelectionAnchor();
	});
}
bool IsExecutionQuiescentIfNeeded(
	const FAngelscriptExtensionPipelineState& State,
	FString& OutError,
	const TCHAR* Operation)
{
	if (!State.bCompilationObserved
		|| IsAngelscriptExecutionWriterHeldByCurrentThread())
	{
		return true;
	}
	OutError = Operation;
	return false;
}
bool RegisterStageToken(
	uint8 Stage,
	Anchor::Token&& Token,
	const FAngelscriptExtensionCallbackRegistration& Registration,
	uint64& OutSerial,
	FString& OutError)
{
	if (Registration.IsValid())
	{
		OutError = TEXT("AngelScript extension callback output is already active.");
		return false;
	}
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	FScopeLock Lock(&State.Mutex);
	FStageRegistrationSlot* Slot = FindStageSlot(State, Stage);
	if (Slot == nullptr)
	{
		OutError = TEXT("AngelScript extension callback stage is invalid.");
		return false;
	}
	if (Slot->IsValid())
	{
		OutError = TEXT("AngelScript extension callback stage is already registered.");
		return false;
	}
	if (State.NextSerial == 0)
		++State.NextSerial;
	Slot->Serial = State.NextSerial++;
	Slot->Stage = Stage;
	Slot->Token.reset(new Anchor::Token(std::move(Token)));
	if (!Slot->IsValid())
	{
		Slot->Token.reset();
		Slot->Serial = 0;
		Slot->Stage = 0;
		OutError = TEXT("AngelScript extension callback stage is already registered.");
		return false;
	}
	OutSerial = Slot->Serial;
	return true;
}
bool BeginStageRegistrationImpl(
	FString& OutError)
{
	OutError.Reset();
	if (!ValidateGameThread(OutError))
		return false;
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	FScopeLock Lock(&State.Mutex);
	if (State.bDispatchActive)
	{
		OutError = TEXT("AngelScript extension callback cannot register during dispatch.");
		return false;
	}
	if (State.ProviderSerial == 0 || State.ProviderIdentity == nullptr)
	{
		OutError = TEXT(
			"AngelScript extension callback requires an active provider.");
		return false;
	}
	if (!IsExecutionQuiescentIfNeeded(
			State,
			OutError,
			TEXT("Late AngelScript extension callback registration requires execution quiescence.")))
	{
		return false;
	}
	return true;
}
bool RegisterStageCallbackTokenImpl(
	uint8 Stage,
	Anchor::Token&& Token,
	const FAngelscriptExtensionCallbackRegistration& Registration,
	uint64& OutSerial,
	FString& OutError)
{
	return RegisterStageToken(
		Stage, std::move(Token), Registration, OutSerial, OutError);
}
}
namespace AngelscriptExtensionPipelineInternal
{
bool IsStageRegistered(uint8 Stage)
{
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	FScopeLock Lock(&State.Mutex);
	const FStageRegistrationSlot* Slot = FindStageSlot(State, Stage);
	return Slot != nullptr && Slot->IsValid();
}
bool IsProviderRegistered()
{
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	FScopeLock Lock(&State.Mutex);
	return State.ProviderSerial != 0 && State.ProviderIdentity != nullptr;
}
bool BeginProviderRegistration(FString& OutError)
{
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	FScopeLock Lock(&State.Mutex);
	if (State.bDispatchActive)
	{
		OutError = TEXT("AngelScript extension provider cannot register during dispatch.");
		return false;
	}
	if (!IsExecutionQuiescentIfNeeded(
			State,
			OutError,
			TEXT("Late AngelScript extension registration requires execution quiescence.")))
	{
		return false;
	}
	if (State.ProviderIdentity != nullptr)
	{
		OutError = TEXT("AngelScript extension provider is already registered.");
		return false;
	}
	return true;
}
bool CommitProviderRegistration(
	std::unique_ptr<
		SoloASBridge::Extensions::FProviderIdentityRegistration>&& Identity,
	uint64& OutSerial,
	FString& OutError)
{
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	FScopeLock Lock(&State.Mutex);
	if (State.ProviderIdentity != nullptr)
	{
		FString IgnoredError;
		SoloASBridge::Extensions::UnregisterProviderIdentity(
			*Identity, IgnoredError);
		OutError = TEXT("AngelScript extension provider is already registered.");
		return false;
	}
	if (State.NextSerial == 0)
		++State.NextSerial;
	State.ProviderSerial = State.NextSerial++;
	State.ProviderIdentity = std::move(Identity);
	OutSerial = State.ProviderSerial;
	return true;
}
bool UnregisterProvider(uint64& Serial, FString& OutError)
{
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	FScopeLock Lock(&State.Mutex);
	if (State.bDispatchActive)
	{
		OutError = TEXT("AngelScript extension provider cannot unregister during dispatch.");
		return false;
	}
	if (!IsExecutionQuiescentIfNeeded(
			State,
			OutError,
			TEXT("AngelScript extension provider cannot unload after compilation without execution quiescence.")))
	{
		return false;
	}
	if (State.ProviderSerial != Serial || State.ProviderIdentity == nullptr)
	{
		OutError = TEXT("AngelScript extension registration does not own the active provider.");
		return false;
	}
	if (!SoloASBridge::Extensions::UnregisterProviderIdentity(
			*State.ProviderIdentity, OutError))
	{
		return false;
	}
	State.ProviderIdentity.reset();
	State.ProviderSerial = 0;
	Serial = 0;
	return true;
}
bool BeginStageRegistration(FString& OutError)
{
	return BeginStageRegistrationImpl(OutError);
}
bool RegisterStageCallbackToken(
	uint8 Stage,
	Anchor::Token&& Token,
	const FAngelscriptExtensionCallbackRegistration& Registration,
	uint64& OutSerial,
	FString& OutError)
{
	return RegisterStageCallbackTokenImpl(
		Stage, std::move(Token), Registration, OutSerial, OutError);
}
bool UnregisterStageCallback(
	uint64& Serial,
	uint8& Stage,
	FString& OutError)
{
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	FScopeLock Lock(&State.Mutex);
	if (State.bDispatchActive)
	{
		OutError = TEXT("AngelScript extension callback cannot unregister during dispatch.");
		return false;
	}
	if (!IsExecutionQuiescentIfNeeded(
			State,
			OutError,
			TEXT("AngelScript extension callback cannot unload after compilation without execution quiescence.")))
	{
		return false;
	}
	FStageRegistrationSlot* Slot = FindStageSlot(State, Stage);
	if (Slot == nullptr || Slot->Serial != Serial || !Slot->IsValid())
	{
		OutError = TEXT("AngelScript extension callback registration does not own the active callback.");
		return false;
	}
	Anchor::Unregister(*Slot->Token);
	Slot->Token.reset();
	Slot->Serial = 0;
	Slot->Stage = 0;
	Serial = 0;
	Stage = 0;
	return true;
}
}
bool DispatchAngelscriptPreParse(
	FAngelscriptPreprocessor& Preprocessor,
	FString& OutError)
{
	const bool bPreParseRegistered =
		IsAngelscriptExtensionStageRegistered(
			EAngelscriptExtensionStage::PreParse);
	if (!bPreParseRegistered)
	{
		OutError.Reset();
		return true;
	}
	WarmAnchorKeys();
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	if (!BeginDispatch(State, OutError))
		return false;
	FScopedExtensionDispatch DispatchScope(State);
	FAngelscriptPreParseContext Context;
	Context.Preprocessor = &Preprocessor;
	Anchor::Execute(GetAngelscriptPreParseAnchor(), Context);
	return ValidateCallbackResult( Context.Result, Context.Error, TEXT("pre-parse"), OutError);
}
bool DispatchAngelscriptBlueprintEventBound(
	const FAngelscriptBlueprintEventBindingSnapshot& Binding,
	FString& OutError)
{
	const bool bBlueprintEventBoundRegistered =
		IsAngelscriptExtensionStageRegistered(
			EAngelscriptExtensionStage::BlueprintEventBound);
	if (!bBlueprintEventBoundRegistered)
	{
		OutError.Reset();
		return true;
	}
	WarmAnchorKeys();
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	if (!BeginDispatch(State, OutError))
		return false;
	FScopedExtensionDispatch DispatchScope(State);
	FAngelscriptBlueprintEventBoundContext Context;
	Context.Binding = &Binding;
	Anchor::Execute(GetAngelscriptBlueprintEventBoundAnchor(), Context);
	return ValidateCallbackResult( Context.Result, Context.Error, TEXT("Blueprint-event-bound"), OutError);
}
bool DispatchAngelscriptNativeInterfaceSelection(
	UClass& NativeClass,
	bool& OutBindAsInterface,
	FString& OutError)
{
	OutBindAsInterface = false;
	const bool bNativeInterfaceSelectionRegistered =
		IsAngelscriptExtensionStageRegistered(
			EAngelscriptExtensionStage::NativeInterfaceSelection);
	if (!bNativeInterfaceSelectionRegistered)
	{
		OutError.Reset();
		return true;
	}
	WarmAnchorKeys();
	FAngelscriptExtensionPipelineState& State = GetPipelineState();
	if (!BeginDispatch(State, OutError))
		return false;
	FScopedExtensionDispatch DispatchScope(State);
	FAngelscriptNativeInterfaceSelectionContext Context;
	Context.NativeClass = &NativeClass;
	Anchor::Execute(GetAngelscriptNativeInterfaceSelectionAnchor(), Context);
	if (!ValidateCallbackResult( Context.Result, Context.Error, TEXT("native-interface-selection"), OutError))
		return false;
	if (Context.Result == EAngelscriptExtensionResult::NotHandled
		&& Context.bBindAsInterface)
	{
		OutError = TEXT(
			"Unhandled native-interface selection attempted to change the default.");
		return false;
	}
	OutBindAsInterface =
		Context.Result == EAngelscriptExtensionResult::Handled
		&& Context.bBindAsInterface;
	return true;
}

bool DispatchAngelscriptTypeGeneration(
	FAngelscriptTypeGenerationContext& Context, FString& OutError)
{
	OutError.Reset();
	if (!IsAngelscriptExtensionStageRegistered(EAngelscriptExtensionStage::TypeGeneration))
		return true;
	WarmAnchorKeys();
	auto& State = GetPipelineState();
	if (!BeginDispatch(State, OutError)) return false;
	FScopedExtensionDispatch DispatchScope(State);
	Anchor::Execute(GetAngelscriptTypeGenerationAnchor(), Context);
	return ValidateCallbackResult(Context.Result, Context.Error, TEXT("type-generation"), OutError);
}
