// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptApiMetadata.h"
#include "Misc/ScopeLock.h"

namespace
{
	struct FRecord
	{
		const void* Producer;
		FAngelscriptApiMetadata Metadata;
	};
	struct FRegistry
	{
		FCriticalSection Lock;
		TMap<FString, TArray<FRecord>> Records;
	};
	FRegistry& Registry()
	{
		static FRegistry Value;
		return Value;
	}
	void RemoveProducer(FRegistry& Value, const void* Producer)
	{
		for (auto It = Value.Records.CreateIterator(); It; ++It)
		{
			It.Value().RemoveAll([Producer](const FRecord& Record) { return Record.Producer == Producer; });
			if (It.Value().IsEmpty()) It.RemoveCurrent();
		}
	}
}

void FAngelscriptApiMetadataRegistry::Register(const void* Producer, const TArray<FAngelscriptApiMetadataEntry>& Entries)
{
	check(Producer);
	auto& Value = Registry();
	FScopeLock Scope(&Value.Lock);
	RemoveProducer(Value, Producer);
	for (const auto& Entry : Entries)
	{
		FRecord Record{Producer, Entry.Metadata};
		Record.Metadata.Tags.Sort();
		for (int32 Index = Record.Metadata.Tags.Num() - 1; Index > 0; --Index)
			if (Record.Metadata.Tags[Index] == Record.Metadata.Tags[Index - 1]) Record.Metadata.Tags.RemoveAt(Index);
		Value.Records.FindOrAdd(Entry.Key).Add(MoveTemp(Record));
	}
}

void FAngelscriptApiMetadataRegistry::Unregister(const void* Producer)
{
	auto& Value = Registry();
	FScopeLock Scope(&Value.Lock);
	RemoveProducer(Value, Producer);
}

EAngelscriptApiMetadataResult FAngelscriptApiMetadataRegistry::Query(const FString& Key, FAngelscriptApiMetadata& OutMetadata)
{
	OutMetadata = {};
	auto& Value = Registry();
	FScopeLock Scope(&Value.Lock);
	const auto* Records = Value.Records.Find(Key);
	if (!Records) return EAngelscriptApiMetadataResult::Missing;
	const auto& First = (*Records)[0].Metadata;
	for (const auto& Record : *Records)
		if (Record.Metadata.Tier != First.Tier || Record.Metadata.Module != First.Module || Record.Metadata.Tags != First.Tags)
			return EAngelscriptApiMetadataResult::Conflict;
	OutMetadata = First;
	return EAngelscriptApiMetadataResult::Found;
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAngelscriptApiMetadataTest,
	"Angelscript.Core.ApiMetadata.Registry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FAngelscriptApiMetadataTest::RunTest(const FString& Parameters)
{
	int32 ProducerA = 0, ProducerB = 0;
	const FString Key = TEXT("void __ApiMetadataRegistryTest::Call()");
	FAngelscriptApiMetadata Out;
	using EResult = EAngelscriptApiMetadataResult;
	TestTrue(TEXT("missing"), FAngelscriptApiMetadataRegistry::Query(Key, Out) == EResult::Missing);
	TArray<FAngelscriptApiMetadataEntry> Entries = {{Key, {TEXT("core"), TEXT("Core"), {TEXT("B"), TEXT("A"), TEXT("A")}}}};
	FAngelscriptApiMetadataRegistry::Register(&ProducerA, Entries);
	TestTrue(TEXT("found"), FAngelscriptApiMetadataRegistry::Query(Key, Out) == EResult::Found);
	TestEqual(TEXT("tags normalized"), Out.Tags.Num(), 2);
	Entries[0].Metadata.Tags = {TEXT("A"), TEXT("B")};
	FAngelscriptApiMetadataRegistry::Register(&ProducerB, Entries);
	TestTrue(TEXT("matching producers agree"), FAngelscriptApiMetadataRegistry::Query(Key, Out) == EResult::Found);
	Entries[0].Metadata.Module = TEXT("Engine");
	FAngelscriptApiMetadataRegistry::Register(&ProducerB, Entries);
	TestTrue(TEXT("module disagreement conflicts"), FAngelscriptApiMetadataRegistry::Query(Key, Out) == EResult::Conflict);
	TestTrue(TEXT("conflict clears output"), Out.Tier.IsEmpty());
	FAngelscriptApiMetadataRegistry::Unregister(&ProducerB);
	TestTrue(TEXT("unregister resolves conflict"), FAngelscriptApiMetadataRegistry::Query(Key, Out) == EResult::Found);
	Entries[0].Metadata.Module = TEXT("Core");
	Entries[0].Metadata.Tier = TEXT("extended");
	Entries.Add({Key, {TEXT("internal"), TEXT("Core"), {TEXT("A"), TEXT("B")}}});
	FAngelscriptApiMetadataRegistry::Register(&ProducerA, Entries);
	TestTrue(TEXT("duplicate conflicting keys reject"), FAngelscriptApiMetadataRegistry::Query(Key, Out) == EResult::Conflict);
	FAngelscriptApiMetadataRegistry::Unregister(&ProducerA);
	TestTrue(TEXT("unregistered is missing"), FAngelscriptApiMetadataRegistry::Query(Key, Out) == EResult::Missing);
	return true;
}
#endif
