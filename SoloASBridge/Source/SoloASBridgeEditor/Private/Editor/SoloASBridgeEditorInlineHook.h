// Copyright Cuviss Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Editor/SoloASEditorEntryHook.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)

namespace SoloASBridge::Editor
{
struct FEditorInlineHookHandle
{
	void* Original = nullptr;
	UPTRINT TargetModuleStart = 0;
	UPTRINT TargetModuleEnd = 0;
	bool IsCallerInTargetModule(const void* Caller) const
	{
		const UPTRINT Address = reinterpret_cast<UPTRINT>(Caller);
		return TargetModuleStart != 0 && Address >= TargetModuleStart && Address < TargetModuleEnd;
	}
#if PLATFORM_MAC
	uint8 OriginalBytes[32]{};
	SIZE_T OriginalByteCount = 0;
	void* TargetAddress = nullptr;
	void* TrampolineAddress = nullptr;
	SIZE_T TrampolineAllocationSize = 0;
#endif
};

bool InstallEditorInlineHook(
	FEditorInlineHookHandle& Handle, void* Target, void* Replacement,
	const TCHAR* HookName);
bool RemoveEditorInlineHook(
	FEditorInlineHookHandle& Handle, void* Replacement,
	const TCHAR* HookName);
}

#endif
