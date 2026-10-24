// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "ClassGenerator/ASClass.h"
#include "UObject/StrongObjectPtr.h"

/** Keeps superseded script CDOs out of stock's ordinary-archetype migration.
 * CDO/DSO reference mappings remain owned by the engine. This scope must wrap
 * only the synchronous ReparentHierarchies call, not the whole reload.
 */
class FAngelscriptRetiredClassDefaultScope final
{
public:
	explicit FAngelscriptRetiredClassDefaultScope(const TMap<UClass*, UClass*>& Replacements)
	{
		check(IsInGameThread());
		for (const TPair<UClass*, UClass*>& Pair : Replacements)
		{
			UASClass* OldClass = Cast<UASClass>(Pair.Key);
			UASClass* NewClass = Cast<UASClass>(Pair.Value);
			if (OldClass == nullptr || NewClass == nullptr || OldClass == NewClass
				|| !OldClass->HasValidScriptClassCarrier() || !NewClass->HasValidScriptClassCarrier())
			{
				continue;
			}
			UObject* Default = OldClass->GetDefaultObject(false);
			if (Default == nullptr || Default->GetClass() != OldClass
				|| !Default->HasAnyFlags(RF_ClassDefaultObject)
				|| Default->HasAnyFlags(RF_NewerVersionExists))
			{
				continue;
			}
			RetiredDefaults.Emplace(OldClass, NewClass, Default);
			Default->SetFlags(RF_NewerVersionExists);
		}
	}

	~FAngelscriptRetiredClassDefaultScope()
	{
		for (const FRetiredDefault& Entry : RetiredDefaults)
		{
			UObject* Default = Entry.Default.Get();
			UObject* Current = Entry.Class->GetDefaultObject(false);
			UObject* Replacement = Entry.ReplacementClass->GetDefaultObject(false);
			if (!IsValid(Default) || Default->GetClass() != Entry.Class
				|| !Default->HasAnyFlags(RF_ClassDefaultObject)
				|| Default->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				UE_LOG(LogTemp, Fatal, TEXT("ASR-RETIRED-CDO-IDENTITY: held script default is no longer valid"));
			}
			if (Current != Default)
			{
				if (Current == nullptr || Current != Replacement || Replacement->GetClass() != Entry.ReplacementClass)
				{
					UE_LOG(LogTemp, Fatal, TEXT("ASR-RETIRED-CDO-REFERENCE: unexpected retired class default replacement"));
				}
				// Stock replaces references in the old class too. Preserve its own default
				// lifetime without undoing replacements in live instances or the new class.
				Entry.Class->SetDefaultObject(Default);
			}
			Default->ClearFlags(RF_NewerVersionExists);
		}
	}

	FAngelscriptRetiredClassDefaultScope(const FAngelscriptRetiredClassDefaultScope&) = delete;
	FAngelscriptRetiredClassDefaultScope& operator=(const FAngelscriptRetiredClassDefaultScope&) = delete;

private:
	struct FRetiredDefault
	{
		UASClass* Class;
		UASClass* ReplacementClass;
		TStrongObjectPtr<UObject> Default;

		FRetiredDefault(UASClass* InClass, UASClass* InReplacementClass, UObject* InDefault)
			: Class(InClass), ReplacementClass(InReplacementClass), Default(InDefault) {}
	};
	TArray<FRetiredDefault> RetiredDefaults;
};
