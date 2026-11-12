// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "FeatureMods/ModKitConflictCheck.h"

#include "FeatureMods/ModKitCompilationManager.h"

bool FModKitConflictCheck::CheckModuleSet(const TArray<TSharedRef<FAngelscriptModuleDesc>>& Modules, FString& OutReason)
{
	FAngelscriptManager& Mgr = FAngelscriptManager::Get();

	for (const TSharedRef<FAngelscriptModuleDesc>& Module : Modules)
	{
		// Module-name conflict: a content-bearing active module with the same
		// name is already live. An empty placeholder is absent (reload) and
		// allowed to recompile (v2 §2.5).
		TSharedPtr<FAngelscriptModuleDesc> Active = Mgr.GetModule(Module->ModuleName);
		if (Active.IsValid() && !FModKitCompilationManager::IsEmptyPlaceholderModule(Module->ModuleName))
		{
			OutReason = FString::Printf(TEXT("module-conflict name=%s"), *Module->ModuleName);
			return true;
		}

		// Class-name conflict: AS type names are globally unique, so a class
		// that already resolves is a collision with an activated mod or the
		// game module surface.
		for (const TSharedRef<FAngelscriptClassDesc>& ClassDesc : Module->Classes)
		{
			if (ClassDesc->bIsStaticsClass)
			{
				continue;
			}
			if (Mgr.GetClass(ClassDesc->GetTypeIdentity()).IsValid())
			{
				OutReason = FString::Printf(TEXT("class-conflict name=%s"), *ClassDesc->ClassName);
				return true;
			}
		}
	}
	return false;
}