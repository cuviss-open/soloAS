// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.

#include "EditorBinary/AngelscriptDataTableRowValidationHook.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)

#include "ClassGenerator/ASStruct.h"
#include "EngineGlobals.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "DataTableEditorUtils.h"
#include "UObject/Class.h"

#include "Editor/SoloASEditorEntryHook.h"

namespace
{
	using FIsValidTableStruct = bool (*)(const UScriptStruct*);
	static FSoloASEditorEntryHook EntryHook;
	static TAutoConsoleVariable<int32> CVarEditorDataTableRowValidation(
		TEXT("soloas.EditorDataTableRowValidation"), 1,
		TEXT("Allow AngelScript structs in the editor DataTable row selector on Stock UE."),
		ECVF_Default);

	bool IsValidTableStructHook(const UScriptStruct* Struct)
	{
		const FIsValidTableStruct Original = reinterpret_cast<FIsValidTableStruct>(EntryHook.Original());
		if (!EntryHook.IsCallerInTargetModule(SoloASBridge::Editor::GetEditorHookCallerAddress()))
			return Original != nullptr ? Original(Struct) : false;
		if (Struct != nullptr
			&& CVarEditorDataTableRowValidation.GetValueOnAnyThread() != 0
			&& Cast<UASStruct>(Struct) != nullptr
			&& !Struct->HasAnyFlags(RF_Transient))
		{
			return true;
		}

		return Original != nullptr ? Original(Struct) : false;
	}

	template<typename T>
	constexpr bool HasNativeScriptStructMarker()
	{
		return requires(const T& Struct) { Struct.bIsScriptStruct; };
	}


}

namespace AngelscriptDataTableRowValidationHook
{
	void Initialize()
	{
		if (!GIsEditor || IsRunningCommandlet())
			return;

		if (HasNativeScriptStructMarker<UScriptStruct>())
			return;

		if (!EntryHook.Install(TEXT("?IsValidTableStruct@FDataTableEditorUtils@@SA_NPEBVUScriptStruct@@@Z"),
            "_ZN21FDataTableEditorUtils18IsValidTableStructEPK13UScriptStruct", reinterpret_cast<void*>(&IsValidTableStructHook)))
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Stock DataTable row validation hook unavailable; AS struct selection keeps native behavior."));
	}

	void Shutdown()
	{
		EntryHook.Remove();
	}
}

#endif
