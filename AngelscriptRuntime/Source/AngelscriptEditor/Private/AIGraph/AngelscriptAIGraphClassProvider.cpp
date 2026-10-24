// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptAIGraphClassProvider.h"

#include "ClassGenerator/AngelscriptClassState.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

namespace
{
bool IsScriptNodeClass(const UClass* Class)
{
	return Class != nullptr
		&& IsValid(Class)
		&& Class->GetClassPathName().IsValid()
		&& Class->GetFName() != NAME_None
		&& !Class->HasAnyClassFlags(CLASS_NewerVersionExists)
		&& GetAngelscriptClassScriptType(Class) != nullptr;
}

}

const TCHAR* FAngelscriptAIGraphClassProvider::GetMarker()
{
	// AIGRAPH_CLASS_DISCOVERY_MARKER identifies this explicit provider.
	return TEXT("AngelscriptAIGraphScriptNodeClassProvider");
}

void FAngelscriptAIGraphClassProvider::GetScriptNodeClasses(
	TArray<UClass*>& OutClasses
)
{
	OutClasses.Reset();
	TSet<UClass*> SeenClasses;

	for (TObjectIterator<UClass> ClassIterator; ClassIterator; ++ClassIterator)
	{
		UClass* Class = *ClassIterator;
		if (!IsScriptNodeClass(Class))
		{
			continue;
		}

		if (!SeenClasses.Contains(Class))
		{
			SeenClasses.Add(Class);
			OutClasses.Add(Class);
		}
	}

	OutClasses.StableSort([](const UClass& Left, const UClass& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});
}

void FAngelscriptAIGraphClassProvider::GetScriptNodeClassDataForBaseClass(
	const UClass* BaseClass,
	TArray<FGraphNodeClassData>& OutClassData
)
{
	OutClassData.Reset();

	if (BaseClass == nullptr
		|| !IsValid(BaseClass))
	{
		return;
	}

	TArray<UClass*> ScriptClasses;
	GetScriptNodeClasses(ScriptClasses);
	TSet<FString> SeenClassPaths;

	for (UClass* Class : ScriptClasses)
	{
		if (!IsScriptNodeClass(Class)
			|| !Class->IsChildOf(BaseClass))
		{
			continue;
		}

		const FString ClassPath = Class->GetPathName();
		if (ClassPath.IsEmpty() || SeenClassPaths.Contains(ClassPath))
		{
			continue;
		}

		SeenClassPaths.Add(ClassPath);
		OutClassData.Add(FGraphNodeClassData(Class, FString()));
	}

}
