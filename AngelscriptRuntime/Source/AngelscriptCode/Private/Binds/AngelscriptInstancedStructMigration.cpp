// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "Binds/AngelscriptInstancedStructMigration.h"

#include "ClassGenerator/ASStruct.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/Class.h"

namespace
{
class FAngelscriptObjectReplacementArchive final : public FObjectAndNameAsStringProxyArchive
{
public:
	FAngelscriptObjectReplacementArchive(
		FArchive& InnerArchive,
		const TMap<UObject*, UObject*>& InReplacementMap)
		: FObjectAndNameAsStringProxyArchive(InnerArchive, true)
		, ReplacementMap(InReplacementMap)
	{
	}

	virtual FArchive& operator<<(UObject*& Object) override
	{
		FObjectAndNameAsStringProxyArchive::operator<<(Object);
		ApplyReplacement(Object);
		return *this;
	}

	virtual FArchive& operator<<(FObjectPtr& Object) override
	{
		FObjectAndNameAsStringProxyArchive::operator<<(Object);
		if (IsLoading())
		{
			UObject* ResolvedObject = Object.Get();
			ApplyReplacement(ResolvedObject);
			Object = ResolvedObject;
		}
		return *this;
	}

	bool HasMissingReplacement() const
	{
		return bHasMissingReplacement;
	}

private:
	void ApplyReplacement(UObject*& Object)
	{
		if (!IsLoading() || Object == nullptr)
		{
			return;
		}

		if (UObject* const* Replacement = ReplacementMap.Find(Object))
		{
			Object = *Replacement;
		}
		else
		{
			bHasMissingReplacement = true;
		}
	}

	const TMap<UObject*, UObject*>& ReplacementMap;
	bool bHasMissingReplacement = false;
};

bool HasObjectReference(const UScriptStruct* Struct)
{
	TArray<const FStructProperty*> EncounteredStructProps;
	for (const FProperty* Property = Struct->PropertyLink; Property; Property = Property->PropertyLinkNext)
	{
		if (Property->ContainsObjectReference(EncounteredStructProps, EPropertyObjectReferenceType_Any))
		{
			return true;
		}
	}

	return false;
}
}

EAngelscriptInstancedStructMigrationResult MigratePayloadInternal(
	FInstancedStruct* Payload,
	const UScriptStruct* OldStruct,
	const UScriptStruct* NewStruct,
	const UScriptStruct* NewestStruct,
	const TMap<UObject*, UObject*>* ReplacementMap);

EAngelscriptInstancedStructMigrationResult FAngelscriptInstancedStructMigration::MigratePayload(
	FInstancedStruct* Payload,
	const UScriptStruct* OldStruct,
	const UScriptStruct* NewStruct,
	const UScriptStruct* NewestStruct)

{
	return MigratePayloadInternal(Payload, OldStruct, NewStruct, NewestStruct, nullptr);
}

EAngelscriptInstancedStructMigrationResult FAngelscriptInstancedStructMigration::MigratePayloadWithObjectReplacements(
	FInstancedStruct* Payload,
	const UScriptStruct* OldStruct,
	const UScriptStruct* NewStruct,
	const UScriptStruct* NewestStruct,
	const TMap<UObject*, UObject*>& ReplacementMap)

{
	return MigratePayloadInternal(Payload, OldStruct, NewStruct, NewestStruct, &ReplacementMap);
}

EAngelscriptInstancedStructMigrationResult MigratePayloadInternal(
	FInstancedStruct* Payload,
	const UScriptStruct* OldStruct,
	const UScriptStruct* NewStruct,
	const UScriptStruct* NewestStruct,
	const TMap<UObject*, UObject*>* ReplacementMap)
{
	if (Payload == nullptr || OldStruct == nullptr || NewStruct == nullptr || NewestStruct == nullptr)
	{
		return EAngelscriptInstancedStructMigrationResult::NullArgument;
	}

	if (OldStruct == NewStruct || NewStruct == NewestStruct || OldStruct == NewestStruct)
	{
		return EAngelscriptInstancedStructMigrationResult::InvalidStructIdentity;
	}

	UASStruct* OldScriptStruct = Cast<UASStruct>(const_cast<UScriptStruct*>(OldStruct));
	UASStruct* NewScriptStruct = Cast<UASStruct>(const_cast<UScriptStruct*>(NewStruct));
	UASStruct* NewestScriptStruct = Cast<UASStruct>(const_cast<UScriptStruct*>(NewestStruct));
	if (OldScriptStruct == nullptr || NewScriptStruct == nullptr || NewestScriptStruct == nullptr)
	{
		return EAngelscriptInstancedStructMigrationResult::InvalidStructIdentity;
	}

	if (!IsValid(OldScriptStruct)
		|| !IsValid(NewScriptStruct)
		|| !IsValid(NewestScriptStruct))
	{
		return EAngelscriptInstancedStructMigrationResult::InvalidStructIdentity;
	}

	if (OldScriptStruct->NewerVersion != NewScriptStruct
		|| OldScriptStruct->ResolveNewestStruct() != NewestScriptStruct
		|| NewScriptStruct->ResolveNewestStruct() != NewestScriptStruct
		|| NewestScriptStruct->ResolveNewestStruct() != NewestScriptStruct)
	{
		return EAngelscriptInstancedStructMigrationResult::StaleStructChain;
	}

	if (!FStructUtils::TheSameLayout(OldStruct, NewStruct, true)
		|| !FStructUtils::TheSameLayout(NewStruct, NewestStruct, true))
	{
		return EAngelscriptInstancedStructMigrationResult::SchemaDrift;
	}

	const bool bHasObjectReference = HasObjectReference(OldStruct)
		|| HasObjectReference(NewStruct)
		|| HasObjectReference(NewestStruct);
	if (bHasObjectReference && ReplacementMap == nullptr)
	{
		return EAngelscriptInstancedStructMigrationResult::UnsupportedReferencePayload;
	}

	if (!Payload->IsValid()
		|| Payload->GetScriptStruct() != OldStruct
		|| Payload->GetMutableMemory() == nullptr)
	{
		return EAngelscriptInstancedStructMigrationResult::InvalidPayload;
	}

	TArray<uint8> SerializedPayload;
	FMemoryWriter Writer(SerializedPayload, true);
	FObjectAndNameAsStringProxyArchive SaveArchive(Writer, true);
	// UE 5.8.3 keeps SerializeItem non-const although this path serializes value storage, not struct metadata.
	const_cast<UScriptStruct*>(OldStruct)->SerializeItem(SaveArchive, Payload->GetMutableMemory(), nullptr);
	if (SaveArchive.IsError())
	{
		return EAngelscriptInstancedStructMigrationResult::ArchiveWriteFailed;
	}

	if (SerializedPayload.IsEmpty())
	{
		return EAngelscriptInstancedStructMigrationResult::EmptyPayload;
	}

	FInstancedStruct MigratedPayload;
	MigratedPayload.InitializeAs(NewestStruct);
	if (!MigratedPayload.IsValid()
		|| MigratedPayload.GetScriptStruct() != NewestStruct
		|| MigratedPayload.GetMutableMemory() == nullptr)
	{
		return EAngelscriptInstancedStructMigrationResult::InvalidTarget;
	}

	FMemoryReader Reader(SerializedPayload, true);
	if (ReplacementMap != nullptr)
	{
		FAngelscriptObjectReplacementArchive LoadArchive(Reader, *ReplacementMap);
		const_cast<UScriptStruct*>(NewestStruct)->SerializeItem(
			LoadArchive, MigratedPayload.GetMutableMemory(), nullptr);
		if (LoadArchive.IsError())
		{
			return EAngelscriptInstancedStructMigrationResult::ArchiveReadFailed;
		}
		if (LoadArchive.HasMissingReplacement())
		{
			return EAngelscriptInstancedStructMigrationResult::MissingReferenceReplacement;
		}
	}
	else
	{
		FObjectAndNameAsStringProxyArchive LoadArchive(Reader, true);
		const_cast<UScriptStruct*>(NewestStruct)->SerializeItem(
			LoadArchive, MigratedPayload.GetMutableMemory(), nullptr);
		if (LoadArchive.IsError())
		{
			return EAngelscriptInstancedStructMigrationResult::ArchiveReadFailed;
		}
	}

	*Payload = MoveTemp(MigratedPayload);
	return EAngelscriptInstancedStructMigrationResult::Migrated;
}
