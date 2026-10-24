#include "AngelscriptBindDatabase.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "StaticJIT/StaticJITConfig.h"
#include "AngelscriptManager.h"
#include "UObject/Class.h"

#if WITH_EDITOR
#include "SourceCodeNavigation.h"
#endif

FAngelscriptBindDatabase FAngelscriptBindDatabase::Instance;

void FAngelscriptBindDatabase::Serialize(FArchive& Archive)
{
	Archive << Structs;
	Archive << Classes;
	Archive << BindDatabaseNamingData;
}

void FAngelscriptBindDatabase::Clear()
{
	Structs.Empty();
	Classes.Empty();
	BindingNameUnrealPaths.Empty();
	BindDatabaseNamingData.Empty();
}

void FAngelscriptBindDatabase::Save(const FString& Path)
{
	{
		TArray<uint8> Data;
		FMemoryWriter Writer(Data);

		Serialize(Writer);

		bool bSaveSuccess = FFileHelper::SaveArrayToFile(Data, *Path);
		if (IsRunningCookCommandlet())
		{
			if (!bSaveSuccess)
			{
				UE_LOG(Angelscript, Error, TEXT("Unable to write the Script/Binds.Cache file during cook"));
			}
		}
	}

#if WITH_EDITOR
	{
		TArray<uint8> HeaderData;
		FMemoryWriter Writer(HeaderData);

		TArray<FAngelscriptClassHeader> Headers;
		for (auto& Bind : Classes)
		{
			UClass* Class = FindObject<UClass>(nullptr, *Bind.UnrealPath);
			FString HeaderPath;
			if (FSourceCodeNavigation::FindClassHeaderPath(Class, HeaderPath) && IFileManager::Get().FileSize(*HeaderPath) != INDEX_NONE)
				Headers.Add(FAngelscriptClassHeader{Bind.UnrealPath, HeaderPath});
		}

		for (auto& Bind : Structs)
		{
			UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *Bind.UnrealPath);
			FString HeaderPath;
			if (FSourceCodeNavigation::FindClassHeaderPath(Struct, HeaderPath) && IFileManager::Get().FileSize(*HeaderPath) != INDEX_NONE)
				Headers.Add(FAngelscriptClassHeader{Bind.UnrealPath, HeaderPath});
		}

		for (UEnum* Enum : BoundEnums)
		{
			FString HeaderPath;
			if (FSourceCodeNavigation::FindClassHeaderPath(Enum, HeaderPath) && IFileManager::Get().FileSize(*HeaderPath) != INDEX_NONE)
				Headers.Add(FAngelscriptClassHeader{Enum->GetPathName(), HeaderPath});
		}

		for (UDelegateFunction* DelegateFunction : BoundDelegateFunctions)
		{
			FString HeaderPath;
			if (FSourceCodeNavigation::FindClassHeaderPath(DelegateFunction, HeaderPath) && IFileManager::Get().FileSize(*HeaderPath) != INDEX_NONE)
				Headers.Add(FAngelscriptClassHeader{ DelegateFunction->GetPathName(), HeaderPath });
		}

		Headers.StableSort([](const FAngelscriptClassHeader& a, const FAngelscriptClassHeader& b)
		{
			return a.UnrealPath < b.UnrealPath;
		});
		
		Writer << Headers;

		bool bSaveSuccess = FFileHelper::SaveArrayToFile(HeaderData, *(Path + TEXT(".Headers")));
		if (IsRunningCookCommandlet())
		{
			if (!bSaveSuccess)
			{
				UE_LOG(Angelscript, Error, TEXT("Unable to write the Script/Binds.Cache.Headers file during cook"));
			}
		}
	}
#endif
}

void FAngelscriptBindDatabase::Load(const FString& Path, bool bGeneratingPrecompiledData)
{
	{
		TArray<uint8> Data;
		FFileHelper::LoadFileToArray(Data, *Path);

		FMemoryReader Reader(Data);
		Serialize(Reader);

		if (Classes.Num() == 0 && Structs.Num() == 0)
		{
			UE_LOG(Angelscript, Fatal, TEXT("Unable to load script bind database, Script/Binds.Cache file is missing or old. This will cause script compilation and execution to fail."));
		}

		// Cache binding names for each unreal path
		for (auto& DBStruct : Structs)
		{
			BindingNameUnrealPaths.Add(DBStruct.UnrealPath, DBStruct.TypeName);

			for (auto& Property : DBStruct.Properties)
				BindingNameUnrealPaths.Add(Property.UnrealPath, Property.PropertyName);
		}

		for (auto& DBClass : Classes)
		{
			BindingNameUnrealPaths.Add(DBClass.UnrealPath, DBClass.TypeName);

			for (auto& Property : DBClass.Properties)
				BindingNameUnrealPaths.Add(Property.UnrealPath, Property.PropertyName);
		}

	}

#if AS_CAN_GENERATE_JIT
	if (bGeneratingPrecompiledData)
	{
		TArray<uint8> HeaderData;
		FFileHelper::LoadFileToArray(HeaderData, *(Path + TEXT(".Headers")));

		FMemoryReader Reader(HeaderData);

		TArray<FAngelscriptClassHeader> Headers;
		Reader << Headers;

		for (const auto& Header : Headers)
		{
			UObject* Field = FindObject<UObject>(nullptr, *Header.UnrealPath);
			if (Field == nullptr)
				continue;
			HeaderLinks.Add(Field, Header.Header);
		}
	}
#endif
}

FString FAngelscriptBindDatabase::GetSourceHeader(UField* Field)
{
#if WITH_EDITOR
	FString HeaderPath;
	if (FSourceCodeNavigation::FindClassHeaderPath(Field, HeaderPath) && IFileManager::Get().FileSize(*HeaderPath) != INDEX_NONE)
		return HeaderPath;
#else
	FString* Found = FAngelscriptBindDatabase::Get().HeaderLinks.Find(Field);
	if (Found != nullptr)
		return *Found;
#endif

	return TEXT("");
}

FString FAngelscriptBindDatabase::GetPropertyBoundName(FProperty* Property)
{
#if WITH_EDITOR
	return Property->GetName();
#else
	FStringView* BoundName = FAngelscriptBindDatabase::Get().BindingNameUnrealPaths.Find(Property->GetPathName());
	if (ensure(BoundName != nullptr))
		return FString(*BoundName);
	else
		return TEXT("");
#endif
}

FString FAngelscriptBindDatabase::GetClassBoundName(UClass* Class)
{
#if WITH_EDITOR
	return Class->GetPrefixCPP() + Class->GetName();
#else
	FStringView* BoundName = FAngelscriptBindDatabase::Get().BindingNameUnrealPaths.Find(Class->GetPathName());
	if (ensure(BoundName != nullptr))
	{
		return FString(*BoundName);
	}
	else
	{
		return TEXT("");
	}
#endif
}

FString FAngelscriptBindDatabase::GetNamingData(FString Key, FString EditorName)
{
#if WITH_EDITOR
	FAngelscriptBindDatabase::Get().BindDatabaseNamingData.Add(Key, EditorName);
	return EditorName;
#else
	if (FString* Value = FAngelscriptBindDatabase::Get().BindDatabaseNamingData.Find(Key))
		return *Value;

	ensure(false);
	return EditorName;
#endif
}
