// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Extensions/NativeEventSuper/AngelscriptNativeEventSuper.h"
#include "Extensions/NativeEventSuper/AngelscriptNativeEventSuperInternal.h"

#include "AngelscriptManager.h"

#include <limits>

#include "StartAngelscriptHeaders.h"
#include "as_context.h"
#include "angelscript.h"
#include "as_scriptfunction.h"
#include "EndAngelscriptHeaders.h"

namespace
{
constexpr int32 NativeEventSuperMaxArgs = 16;
constexpr SIZE_T NativeEventSuperMaxSize = 1024;
constexpr int32 NativeEventSuperArgumentBufferAlignment = 64;
constexpr int32 NativeEventSuperMaxCallstackFrames = 64;

struct FNativeEventSuperRegistry
{
	TMap<const asIScriptFunction*,
		TUniquePtr<FAngelscriptNativeEventSuperAlias>> Aliases;
};

uint8 GNativeEventSuperRegistryType;

asPWORD GetRegistryType()
{
	return reinterpret_cast<asPWORD>(&GNativeEventSuperRegistryType);
}

FNativeEventSuperRegistry* GetRegistry(asIScriptEngine* Engine)
{
	return Engine == nullptr
		? nullptr
		: static_cast<FNativeEventSuperRegistry*>(
			Engine->GetUserData(GetRegistryType()));
}

void CleanupRegistry(asIScriptEngine* Engine)
{
	delete GetRegistry(Engine);
}

FNativeEventSuperRegistry* InstallRegistry(asIScriptEngine* Engine)
{
	if (Engine == nullptr)
		return nullptr;

	auto* Registry = new FNativeEventSuperRegistry;
	delete static_cast<FNativeEventSuperRegistry*>(
		Engine->SetUserData(Registry, GetRegistryType()));
	Engine->SetEngineUserDataCleanupCallback(
		&CleanupRegistry, GetRegistryType());
	return Registry;
}
}

bool RegisterAngelscriptNativeEventSuperAlias(
	asIScriptEngine* Engine,
	TUniquePtr<FAngelscriptNativeEventSuperAlias> Alias
)
{
	if (Engine == nullptr || !Alias.IsValid()
		|| Alias->FunctionIdentity == nullptr
		|| Alias->FunctionIdentity->GetEngine() != Engine
		|| Alias->CanonicalScriptName.IsEmpty()
		|| Alias->NativeFunctionName.IsEmpty()
		|| Alias->NativeDeclaringClassPath.IsEmpty()
		|| !ValidateNativeEventSuperArgumentShape(*Alias))
	{
		return false;
	}

	FNativeEventSuperRegistry* Registry = GetRegistry(Engine);
	if (Registry == nullptr)
		Registry = InstallRegistry(Engine);
	if (Registry == nullptr
		|| Registry->Aliases.Contains(Alias->FunctionIdentity))
	{
		return false;
	}

	const asIScriptFunction* Function = Alias->FunctionIdentity;
	Registry->Aliases.Add(Function, MoveTemp(Alias));
	return true;
}

const FAngelscriptNativeEventSuperAlias*
ResolveAngelscriptNativeEventSuperAlias(const asIScriptFunction* Function)
{
	if (Function == nullptr)
		return nullptr;

	FNativeEventSuperRegistry* Registry = GetRegistry(Function->GetEngine());
	if (Registry == nullptr)
		return nullptr;

	const TUniquePtr<FAngelscriptNativeEventSuperAlias>* Alias =
		Registry->Aliases.Find(Function);
	if (Alias == nullptr || !Alias->IsValid())
		return nullptr;
	if ((*Alias)->FunctionIdentity != Function
		|| Function->GetUserData() != Alias->Get())
	{
		return nullptr;
	}
	return Alias->Get();
}

bool IsAngelscriptNativeEventSuperAlias(const asIScriptFunction* Function)
{
	return ResolveAngelscriptNativeEventSuperAlias(Function) != nullptr;
}

bool ValidateNativeEventSuperArgumentShape(
	const TArray<FAngelscriptTypeUsage>& ArgumentTypes,
	const FAngelscriptTypeUsage& ReturnType,
	int32 EffectiveArgumentCount
)
{
	if (EffectiveArgumentCount < 0
		|| EffectiveArgumentCount > NativeEventSuperMaxArgs)
	{
		return false;
	}

	SIZE_T PackedBytes = 0;
	auto AccumulateType = [&PackedBytes](
		const FAngelscriptTypeUsage& Type) -> bool
	{
		if (!Type.IsValid())
			return false;

		const int32 ArgumentAlign = Type.GetValueAlignment();
		const int32 ArgumentSizeValue = Type.GetValueSize();
		if (ArgumentAlign <= 0 || ArgumentSizeValue <= 0
			|| (ArgumentAlign & (ArgumentAlign - 1)) != 0
			|| ArgumentAlign > NativeEventSuperArgumentBufferAlignment)
		{
			return false;
		}

		const SIZE_T ArgumentAlignment =
			static_cast<SIZE_T>(ArgumentAlign);
		SIZE_T ArgumentOffset = PackedBytes;
		if (ArgumentOffset > std::numeric_limits<SIZE_T>::max()
			- (ArgumentAlignment - 1))
		{
			return false;
		}
		ArgumentOffset = Align(ArgumentOffset, ArgumentAlignment);

		const SIZE_T ArgumentSize = static_cast<SIZE_T>(ArgumentSizeValue);
		if (ArgumentOffset > std::numeric_limits<SIZE_T>::max() - ArgumentSize
			|| ArgumentOffset + ArgumentSize >= NativeEventSuperMaxSize)
		{
			return false;
		}
		PackedBytes = ArgumentOffset + ArgumentSize;
		return true;
	};

	for (const FAngelscriptTypeUsage& ArgumentType : ArgumentTypes)
	{
		if (!AccumulateType(ArgumentType))
			return false;
	}
	return !ReturnType.IsValid() || AccumulateType(ReturnType);
}

bool ValidateNativeEventSuperArgumentShape(
	const FAngelscriptNativeEventSuperAlias& Alias
)
{
	const int32 ReturnSlotCount = Alias.ReturnType.IsValid() ? 1 : 0;
	if (Alias.ArgumentTypes.Num()
		> NativeEventSuperMaxArgs - ReturnSlotCount)
	{
		return false;
	}
	return ValidateNativeEventSuperArgumentShape(
		Alias.ArgumentTypes,
		Alias.ReturnType,
		Alias.ArgumentTypes.Num() + ReturnSlotCount);
}

bool ResolveNativeEventSuperCaller(
	const FAngelscriptNativeEventSuperAlias& Alias,
	FString& OutCallerClassPath
)
{
	OutCallerClassPath.Reset();
	asCContext* Context = FAngelscriptManager::GetCurrentScriptContext();
	if (Context == nullptr)
		return false;

	const asUINT CallstackSize = FMath::Min(
		Context->GetCallstackSize(),
		static_cast<asUINT>(NativeEventSuperMaxCallstackFrames));
	FAngelscriptManager& Manager = FAngelscriptManager::Get();
	const FString ExpectedScriptFunctionName =
		Alias.CanonicalScriptName + TEXT("_Implementation");
	for (asUINT Frame = 0; Frame < CallstackSize; ++Frame)
	{
		asIScriptFunction* CallerFunction = Context->GetFunction(Frame);
		if (CallerFunction == nullptr
			|| IsAngelscriptNativeEventSuperAlias(CallerFunction)
			|| CallerFunction->GetFuncType() == asEFuncType::asFUNC_SYSTEM)
		{
			continue;
		}
		if (CallerFunction->GetModule() == nullptr)
			continue;

		TSharedPtr<FAngelscriptModuleDesc> Module =
			Manager.GetModule(CallerFunction->GetModule());
		if (!Module.IsValid())
			continue;

		TSharedPtr<FAngelscriptClassDesc> Class =
			Module->GetClass(CallerFunction->GetObjectType());
		if (!Class.IsValid() || Class->Class == nullptr)
			continue;

		for (const TSharedRef<FAngelscriptFunctionDesc>& Method : Class->Methods)
		{
			if (Method->ScriptFunction != CallerFunction)
				continue;
			if (!Method->bBlueprintOverride
				|| Method->OriginalFunctionName != Alias.CanonicalScriptName
				|| Method->FunctionName != Alias.NativeFunctionName
				|| Method->ScriptFunctionName != ExpectedScriptFunctionName
				|| ExpectedScriptFunctionName
					!= ANSI_TO_TCHAR(CallerFunction->GetName())
				|| Method->Function == nullptr
				|| Method->Function->GetOuterUClass() != Class->Class
				|| Method->Function->GetName() != Alias.NativeFunctionName)
			{
				continue;
			}

			OutCallerClassPath = Class->Class->GetPathName();
			return !OutCallerClassPath.IsEmpty();
		}
	}
	return false;
}
