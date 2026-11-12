// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
                                                                                     

#include "ScriptCache/ModKitCacheKeystore.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Shared/Crypto/ModKitCacheCipher.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr int32 KeystoreSchemaVersion = 1;

	TSharedPtr<FJsonObject> ReadRoot(const FString& Path)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *Path))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Content), Root) || !Root.IsValid())
		{
			return nullptr;
		}
		return Root;
	}

	bool ReadEntryObject(const TSharedPtr<FJsonObject>& Object, FModKitKeystoreEntry& OutEntry)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		OutEntry = FModKitKeystoreEntry();
		OutEntry.KeyId = Object->GetStringField(TEXT("keyId"));
		if (OutEntry.KeyId.IsEmpty())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Fragments = nullptr;
		if (Object->TryGetArrayField(TEXT("binaryFragments"), Fragments))
		{
			for (const TSharedPtr<FJsonValue>& FragmentValue : *Fragments)
			{
				const TSharedPtr<FJsonObject> FragmentObject = FragmentValue->AsObject();
				if (!FragmentObject.IsValid())
				{
					continue;
				}
				FModKitKeystoreBinaryFragment Fragment;
				Fragment.Id = FragmentObject->GetStringField(TEXT("id"));
				Fragment.Data = FModKitCacheCipher::BytesFromHex(FragmentObject->GetStringField(TEXT("data")));
				if (Fragment.Data.Num() > 0)
				{
					OutEntry.BinaryFragments.Add(MoveTemp(Fragment));
				}
			}
		}

		OutEntry.HeaderFragment = FModKitCacheCipher::BytesFromHex(Object->GetStringField(TEXT("headerFragment")));
		OutEntry.ExpectedRuntimeHash = FModKitCacheCipher::BytesFromHex(Object->GetStringField(TEXT("expectedRuntimeHash")));
		OutEntry.RuntimeModule = Object->GetStringField(TEXT("runtimeModule"));
		OutEntry.CreatedAt = Object->GetStringField(TEXT("createdAt"));
		OutEntry.Comment = Object->GetStringField(TEXT("comment"));
		return true;
	}

	TSharedPtr<FJsonObject> WriteEntryObject(const FModKitKeystoreEntry& Entry)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("keyId"), Entry.KeyId);

		TArray<TSharedPtr<FJsonValue>> Fragments;
		for (const FModKitKeystoreBinaryFragment& Fragment : Entry.BinaryFragments)
		{
			TSharedPtr<FJsonObject> FragmentObject = MakeShared<FJsonObject>();
			FragmentObject->SetStringField(TEXT("id"), Fragment.Id);
			FragmentObject->SetStringField(TEXT("data"), FModKitCacheCipher::BytesToHex(Fragment.Data));
			Fragments.Add(MakeShared<FJsonValueObject>(FragmentObject));
		}
		Object->SetArrayField(TEXT("binaryFragments"), Fragments);
		Object->SetStringField(TEXT("headerFragment"), FModKitCacheCipher::BytesToHex(Entry.HeaderFragment));
		Object->SetStringField(TEXT("expectedRuntimeHash"), FModKitCacheCipher::BytesToHex(Entry.ExpectedRuntimeHash));
		Object->SetStringField(TEXT("runtimeModule"), Entry.RuntimeModule);
		Object->SetStringField(TEXT("createdAt"), Entry.CreatedAt);
		Object->SetStringField(TEXT("comment"), Entry.Comment);
		return Object;
	}
}

FString FModKitCacheKeystore::GetDefaultPath()
{
	const FString EnvOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("MODKIT_CACHE_KEY_PATH"));
	if (!EnvOverride.IsEmpty())
	{
		return EnvOverride;
	}
	return FPaths::Combine(FPaths::ProjectDir(), TEXT("Build"), TEXT("ScriptCacheKey.json"));
}

bool FModKitCacheKeystore::LoadEntries(const FString& Path, TArray<FModKitKeystoreEntry>& OutEntries)
{
	OutEntries.Reset();
	const TSharedPtr<FJsonObject> Root = ReadRoot(Path);
	if (!Root.IsValid())
	{
		return false;
	}

	const int32 Schema = Root->GetIntegerField(TEXT("schemaVersion"));
	if (Schema != KeystoreSchemaVersion)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Root->TryGetArrayField(TEXT("entries"), Entries))
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
	{
		FModKitKeystoreEntry Entry;
		if (ReadEntryObject(EntryValue->AsObject(), Entry))
		{
			OutEntries.Add(MoveTemp(Entry));
		}
	}
	return true;
}

bool FModKitCacheKeystore::FindEntry(const FString& Path, const FString& KeyId, FModKitKeystoreEntry& OutEntry)
{
	TArray<FModKitKeystoreEntry> Entries;
	if (!LoadEntries(Path, Entries))
	{
		return false;
	}
	for (const FModKitKeystoreEntry& Entry : Entries)
	{
		if (Entry.KeyId == KeyId)
		{
			OutEntry = Entry;
			return true;
		}
	}
	return false;
}

bool FModKitCacheKeystore::WriteEntry(const FString& Path, const FModKitKeystoreEntry& NewEntry)
{
	TArray<FModKitKeystoreEntry> Entries;
	bool bHasExisting = false;
	if (FPaths::FileExists(Path))
	{
		// Preserve other KeyId entries (A/B rotation support); replace ours.
		bHasExisting = LoadEntries(Path, Entries);
	}

	TArray<FModKitKeystoreEntry> Updated;
	Updated.Reserve(Entries.Num() + 1);
	bool bReplaced = false;
	for (const FModKitKeystoreEntry& Entry : Entries)
	{
		if (Entry.KeyId == NewEntry.KeyId)
		{
			Updated.Add(NewEntry);
			bReplaced = true;
		}
		else
		{
			Updated.Add(Entry);
		}
	}
	if (!bReplaced)
	{
		Updated.Add(NewEntry);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), KeystoreSchemaVersion);
	TArray<TSharedPtr<FJsonValue>> EntryValues;
	for (const FModKitKeystoreEntry& Entry : Updated)
	{
		EntryValues.Add(MakeShared<FJsonValueObject>(WriteEntryObject(Entry)));
	}
	Root->SetArrayField(TEXT("entries"), EntryValues);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		return false;
	}

	const FString Directory = FPaths::GetPath(Path);
	if (!FPaths::DirectoryExists(Directory))
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
	}
	return FFileHelper::SaveStringToFile(Output, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}