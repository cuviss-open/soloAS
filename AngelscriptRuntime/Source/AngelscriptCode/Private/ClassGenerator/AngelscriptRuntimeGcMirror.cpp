// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/AngelscriptRuntimeGcMirror.h"
#include "ClassGenerator/AngelscriptRetiredModules.h"
#include "AngelscriptRuntimeGcLifecycle.h"
#include "ClassGenerator/ASClass.h"
#include "AngelscriptManager.h"
#include "AngelscriptReflectionAdapter.h"
#include "Runtime/SoloASBridgeObjectLifecycle.h"
#include "Runtime/SoloASSceneComponentAttachment.h"
#include "UObject/Object.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectThreadContext.h"

#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"

#include "Containers/Set.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "StartAngelscriptHeaders.h"
#include "as_scriptobject.h"
#include "EndAngelscriptHeaders.h"

namespace AngelscriptRuntimeGcMirror
{
#if WITH_DEV_AUTOMATION_TESTS
static const void* GCrossActorOldIds[6]{};
static FCrossActorMissingMapObserver GCrossActorObserver;
static bool bCrossActorApplied = false;
bool ArmCrossActorMissingMapForTest(const void* const Identities[6], FCrossActorMissingMapObserver&& Observer)
{
    if (!IsInGameThread() || GCrossActorObserver || !Observer) return false;
    TSet<const void*> Unique;
    for (int32 Index = 0; Index != 6; ++Index)
    {
        if (Identities[Index] == nullptr || Unique.Contains(Identities[Index])) return false;
        Unique.Add(Identities[Index]); GCrossActorOldIds[Index] = Identities[Index];
    }
    GCrossActorObserver = MoveTemp(Observer); bCrossActorApplied = false;
    return true;
}
void ClearCrossActorMissingMapForTest() { bCrossActorApplied = false; GCrossActorObserver = FCrossActorMissingMapObserver(); }
#endif
namespace
{
thread_local bool bRuntimeDestroyCallbackInProgress = false;

bool IsScriptReplacementCaptureExcluded(const UObject* Object)
{
	return Cast<UASClass>(Object) != nullptr || Object->IsTemplate()
		|| Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject
			| RF_DefaultSubObject);
}

enum class EReinstancedScriptPairConsumptionState : uint8
{
	Reserved,
	Consumed,
	Failed,
};

enum class EReinstancedScriptPairFailureStage : uint8
{
	Preflight,
	BridgeForwardUpdate,
};

bool HasRetiredRuntimeType(const FRuntimeTrackedObject& Entry, const UASClass* Class)
{
    return IsRetiredRuntimeReceiverIdentity(Entry.Key.ResolveObjectPtrEvenIfUnreachable(),
        static_cast<asITypeInfo*>(Entry.SavedTypeCookie), Entry.RetainedModule, Class, Entry.TrackSequence);
}

struct FConsumedReinstancedScriptClassPair
{
	FObjectKey OldKey;
	FObjectKey NewKey;
	const void* OldObjectIdentity = nullptr;
	const void* NewObjectIdentity = nullptr;
	uint64 CaptureGeneration = 0;
	uint64 OldTrackSequence = 0;
	uint64 NewTrackSequence = 0;
	bool bOldRemovedByNaturalGc = false;
	EReinstancedScriptPairConsumptionState State =
		EReinstancedScriptPairConsumptionState::Reserved;

	FConsumedReinstancedScriptClassPair(
	const FObjectKey& InOldKey,
		const FObjectKey& InNewKey,
		const void* InOldObjectIdentity,
		const void* InNewObjectIdentity,
		uint64 InCaptureGeneration,
		uint64 InOldTrackSequence,
		uint64 InNewTrackSequence,
		EReinstancedScriptPairConsumptionState InState
	)
		: OldKey(InOldKey)
		, NewKey(InNewKey)
		, OldObjectIdentity(InOldObjectIdentity)
		, NewObjectIdentity(InNewObjectIdentity)
		, CaptureGeneration(InCaptureGeneration)
		, OldTrackSequence(InOldTrackSequence)
		, NewTrackSequence(InNewTrackSequence)
		, State(InState)
	{
	}
};

TMap<FObjectKey, FConsumedReinstancedScriptClassPair>
	ReinstancedScriptPairsByNewObject;
enum class EReloadCaptureState : uint8
{
	Captured,
	Reserved,
	Consumed,
	RetiredByGc,
	Failed,
};

enum class EReloadCaptureRole : uint8
{
	Ordinary,
};

struct FReloadCaptureRecord
{
	FObjectKey OldKey;
	const void* OldObjectIdentity = nullptr;
	asITypeInfo* OldTypeCookie = nullptr;
	const void* OldTypeCookieUserData = nullptr;
	const void* OldClassIdentity = nullptr;
	const void* NewClassIdentity = nullptr;
	uint32 OldFlags = 0;
	EReloadCaptureRole OldRole = EReloadCaptureRole::Ordinary;
	uint64 OldTrackSequence = 0;
	uint64 CaptureGeneration = 0;
	TArray<FReinstancedScriptScalarSnapshot> ScalarSnapshots;
	TArray<FReinstancedScriptClassReferenceSnapshot> ClassReferenceSnapshots;
	bool bPairFinalizationClaimed = false;
	EReloadCaptureState State = EReloadCaptureState::Captured;
};

bool IsApprovedRuntimeScalarProperty(
	const FProperty* Property,
	const UASClass* ScriptClass,
	const asITypeInfo* TypeCookie
)
{
	const UASClass* OwnerClass = Property == nullptr
		? nullptr
		: Cast<UASClass>(Property->GetOwnerClass());
	const SIZE_T PropertyOffset = Property != nullptr
		? static_cast<SIZE_T>(Property->GetOffset_ForInternal()) : 0;
	const SIZE_T PropertyEnd = PropertyOffset + (Property != nullptr
		? static_cast<SIZE_T>(Property->GetSize()) : 0);
	if (Property == nullptr
		|| ScriptClass == nullptr
		|| TypeCookie == nullptr
		|| OwnerClass == nullptr
		|| !ScriptClass->IsChildOf(OwnerClass)
		|| Property->ArrayDim != 1
		|| Property->GetSize() != Property->GetElementSize()
		|| !Property->IsInContainer(ScriptClass)
		|| PropertyOffset < static_cast<SIZE_T>(ScriptClass->ScriptPropertyOffset)
		|| PropertyEnd > static_cast<SIZE_T>(ScriptClass->GetPropertiesSize())
		|| PropertyEnd > static_cast<SIZE_T>(TypeCookie->GetSize())
		|| !HasAngelscriptPropertyTrait(
			Property,
			EAngelscriptPropertyTrait::RuntimeGenerated)
		|| (CastField<FNumericProperty>(Property) == nullptr
			&& CastField<FBoolProperty>(Property) == nullptr
			&& CastField<FEnumProperty>(Property) == nullptr)
		|| Property->HasAnyPropertyFlags(
			CPF_InstancedReference
			| CPF_ContainsInstancedReference
			| CPF_Transient
			| CPF_DuplicateTransient
			| CPF_NonPIEDuplicateTransient
			| CPF_TextExportTransient
			| CPF_SkipSerialization
			| CPF_Virtual)
		|| Property->ContainsInstancedObjectProperty()
		|| Property->HasGetter()
		|| Property->HasSetter()
		|| Property->IsA<FArrayProperty>()
		|| Property->IsA<FSetProperty>()
		|| Property->IsA<FMapProperty>()
		|| Property->IsA<FStructProperty>()
		|| Property->IsA<FDelegateProperty>()
		|| Property->IsA<FMulticastDelegateProperty>())
	{
		return false;
	}

	TArray<const FStructProperty*> EncounteredFinishDestroyProps;
	if (Property->ContainsFinishDestroy(EncounteredFinishDestroyProps))
		return false;

	TArray<const FStructProperty*> EncounteredStructProps;
	const EPropertyObjectReferenceType AllReferenceKinds =
		EPropertyObjectReferenceType::Strong
		| EPropertyObjectReferenceType::Weak
		| EPropertyObjectReferenceType::Soft
		| EPropertyObjectReferenceType::Conservative;
	if (Property->ContainsObjectReference(
		EncounteredStructProps,
		AllReferenceKinds))
	{
		return false;
	}

	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		return EnumProperty->GetEnum() != nullptr
			&& EnumProperty->GetUnderlyingProperty() != nullptr
			&& CastField<FNumericProperty>(
				EnumProperty->GetUnderlyingProperty()) != nullptr;
	}
	return true;
}

FString GetRuntimeEnumSignature(const UEnum* Enum)
{
	FString Signature;
	if (Enum == nullptr)
		return Signature;

	for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
	{
		Signature += Enum->GetNameStringByIndex(Index);
		Signature += TEXT("=");
		Signature += LexToString(Enum->GetValueByIndex(Index));
		Signature += TEXT(";");
	}
	return Signature;
}

const UEnum* GetRuntimeEnumForProperty(const FProperty* Property)
{
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		return EnumProperty->GetEnum();
	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		return ByteProperty->Enum;
	return nullptr;
}

bool CaptureRuntimeScalarSnapshots(
	UObject* Object,
	UASClass* OldClass,
	UASClass* NewClass,
	asITypeInfo* OldTypeCookie,
	const TMap<UClass*, UClass*>& ReloadClasses,
	TArray<FReinstancedScriptScalarSnapshot>& OutSnapshots
)
{
	if (Object == nullptr
		|| OldClass == nullptr
		|| NewClass == nullptr
		|| OldTypeCookie == nullptr
		|| Object->GetClass() != OldClass)
	{
		return false;
	}

	const UObject* OldArchetype = Object->GetArchetype();
	const bool bUseArchetypeDelta = OldArchetype != nullptr
		&& Object->IsA(OldArchetype->GetClass())
		&& !OldArchetype->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
	for (TFieldIterator<FProperty> It(OldClass); It; ++It)
	{
		FProperty* Property = *It;
		if (!HasAngelscriptPropertyTrait(
			Property,
			EAngelscriptPropertyTrait::RuntimeGenerated))
		{
			continue;
		}
		if (!IsApprovedRuntimeScalarProperty(Property, OldClass, OldTypeCookie))
		{
			continue;
		}

		UASClass* OldOwnerClass = Cast<UASClass>(Property->GetOwnerClass());
		UClass* const* NewOwnerValue = ReloadClasses.Find(OldOwnerClass);
		UClass* ExpectedNewOwnerClass = nullptr;
		if (NewOwnerValue != nullptr)
		{
			ExpectedNewOwnerClass = Cast<UASClass>(*NewOwnerValue);
			if (ExpectedNewOwnerClass == nullptr
				|| !NewClass->IsChildOf(ExpectedNewOwnerClass))
			{
				return false;
			}
		}
		else if (OldOwnerClass == nullptr
			|| OldOwnerClass->NewerVersion != nullptr
			|| OldOwnerClass->HasAnyClassFlags(CLASS_NewerVersionExists)
			|| !NewClass->IsChildOf(OldOwnerClass))
		{
			// An unchanged script parent remains the exact expected owner. Any
			// retired or unrelated owner must have an explicit reload mapping.
			return false;
		}
		else
		{
			ExpectedNewOwnerClass = OldOwnerClass;
		}
		if (ExpectedNewOwnerClass == nullptr)
		{
			return false;
		}

		// Match tagged delta serialization; unchanged values use the new defaults,
		// including defaults functions that stopped at a script exception.
		if (bUseArchetypeDelta && Property->Identical(
			Property->ContainerPtrToValuePtr<void>(Object),
			Property->ContainerPtrToValuePtrForDefaults<void>(OldArchetype->GetClass(), OldArchetype), PPF_None))
		{
			continue;
		}
		FReinstancedScriptScalarSnapshot& Snapshot =
			OutSnapshots.AddDefaulted_GetRef();
		Snapshot.PropertyName = Property->GetFName();
		Snapshot.ExpectedNewOwnerIdentity =
			static_cast<const void*>(ExpectedNewOwnerClass);
		Snapshot.PropertyClass = Property->GetClass();
		Snapshot.PropertyTraits = GetAngelscriptPropertyTraits(Property);
		Snapshot.Offset = Property->GetOffset_ForInternal();
		Snapshot.Size = Property->GetSize();
		Snapshot.ElementSize = Property->GetElementSize();
		Snapshot.ArrayDim = Property->ArrayDim;
		Snapshot.PropertyFlags = static_cast<uint64>(
			Property->GetPropertyFlags());

		if (const FBoolProperty* BoolProperty =
			CastField<FBoolProperty>(Property))
		{
			Snapshot.Kind = EReinstancedScriptScalarKind::Bool;
			Snapshot.bBoolValue = BoolProperty->GetPropertyValue_InContainer(Object);
			Snapshot.bBoolIsNativeBool = BoolProperty->IsNativeBool();
			Snapshot.BoolFieldMask = static_cast<uint32>(BoolProperty->GetFieldMask());
			Snapshot.BoolByteOffset = static_cast<uint32>(BoolProperty->GetByteOffset());
			Snapshot.BoolFieldSize = static_cast<uint32>(BoolProperty->GetBoolFieldSize());
			Snapshot.BoolByteMask = static_cast<uint32>(BoolProperty->GetByteMask());
		}
		else if (const UEnum* Enum = GetRuntimeEnumForProperty(Property))
		{
			if (Enum->GetPathName().IsEmpty())
				return false;
			Snapshot.Kind = EReinstancedScriptScalarKind::Enum;
			if (const FEnumProperty* EnumProperty =
				CastField<FEnumProperty>(Property))
			{
				Snapshot.UnderlyingPropertyClass =
					EnumProperty->GetUnderlyingProperty()->GetClass();
			}
			else
			{
				// FByteProperty stores byte enums directly; its own field class
				// is the complete underlying identity for this representation.
				Snapshot.UnderlyingPropertyClass = Property->GetClass();
			}
			Snapshot.EnumIdentity = Enum->GetPathName();
			Snapshot.EnumSignature = GetRuntimeEnumSignature(Enum);
			Snapshot.PrimitiveBytes.Append(
				static_cast<const uint8*>(
					Property->ContainerPtrToValuePtr<void>(Object)),
				Property->GetSize());
		}
		else
		{
			Snapshot.Kind = EReinstancedScriptScalarKind::Numeric;
			Snapshot.PrimitiveBytes.Append(
				static_cast<const uint8*>(
					Property->ContainerPtrToValuePtr<void>(Object)),
				Property->GetSize());
		}

		if (Snapshot.Kind != EReinstancedScriptScalarKind::Bool
			&& Snapshot.PrimitiveBytes.Num() != Snapshot.Size)
		{
			return false;
		}
	}
	return true;
}

bool IsCurrentClassIdentity(const UClass* Class)
{
	return Class != nullptr
		&& IsValid(Class)
		&& !Class->HasAnyClassFlags(CLASS_NewerVersionExists)
		&& Class->GetAuthoritativeClass() == Class;
}

bool IsAdmittedRuntimeClassReferenceProperty(
	const FProperty* Property,
	const UASClass* ScriptClass,
	const asITypeInfo* TypeCookie)
{
	const UASClass* OwnerClass = Property == nullptr
		? nullptr : Cast<UASClass>(Property->GetOwnerClass());
	const SIZE_T PropertyOffset = Property != nullptr
		? static_cast<SIZE_T>(Property->GetOffset_ForInternal()) : 0;
	const SIZE_T PropertyEnd = PropertyOffset + (Property != nullptr
		? static_cast<SIZE_T>(Property->GetSize()) : 0);
	const FClassProperty* ClassProperty = CastField<FClassProperty>(Property);
	return Property != nullptr
		&& ScriptClass != nullptr
		&& TypeCookie != nullptr
		&& OwnerClass != nullptr
		&& ScriptClass->IsChildOf(OwnerClass)
		&& ClassProperty != nullptr
		&& Property->GetClass() == FClassProperty::StaticClass()
		&& Property->ArrayDim == 1
		&& Property->GetSize() == Property->GetElementSize()
		&& Property->IsInContainer(ScriptClass)
		&& PropertyOffset >= static_cast<SIZE_T>(ScriptClass->ScriptPropertyOffset)
		&& PropertyEnd <= static_cast<SIZE_T>(ScriptClass->GetPropertiesSize())
		&& PropertyEnd <= static_cast<SIZE_T>(TypeCookie->GetSize())
		&& HasAngelscriptPropertyTrait(
			Property, EAngelscriptPropertyTrait::RuntimeGenerated)
		&& !Property->HasAnyPropertyFlags(
			CPF_InstancedReference | CPF_ContainsInstancedReference
			| CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient
			| CPF_TextExportTransient | CPF_SkipSerialization | CPF_Virtual)
		&& !Property->ContainsInstancedObjectProperty()
		&& !Property->HasGetter()
		&& !Property->HasSetter()
		&& ClassProperty->MetaClass != nullptr
		&& ClassProperty->PropertyClass == UClass::StaticClass()
		&& IsValid(ClassProperty->MetaClass);
}

bool CaptureRuntimeClassReferenceSnapshots(
	UObject* Object,
	UASClass* OldClass,
	UASClass* NewClass,
	asITypeInfo* OldTypeCookie,
	const TMap<UClass*, UClass*>& ReloadClasses,
	TArray<FReinstancedScriptClassReferenceSnapshot>& OutSnapshots)
{
	if (Object == nullptr || OldClass == nullptr || NewClass == nullptr
		|| OldTypeCookie == nullptr || Object->GetClass() != OldClass)
	{
		return false;
	}

	for (TFieldIterator<FProperty> It(OldClass); It; ++It)
	{
		FProperty* Property = *It;
		if (!HasAngelscriptPropertyTrait(
			Property, EAngelscriptPropertyTrait::RuntimeGenerated)
			|| !Property->IsA<FClassProperty>())
		{
			continue;
		}
		if (!IsAdmittedRuntimeClassReferenceProperty(
				Property, OldClass, OldTypeCookie))
		{
			return false;
		}

		UASClass* OldOwnerClass = Cast<UASClass>(Property->GetOwnerClass());
		UClass* const* NewOwnerValue = ReloadClasses.Find(OldOwnerClass);
		UASClass* ExpectedNewOwnerClass = NewOwnerValue != nullptr
			? Cast<UASClass>(*NewOwnerValue) : OldOwnerClass;
		if (ExpectedNewOwnerClass == nullptr || !NewClass->IsChildOf(ExpectedNewOwnerClass)
			|| (NewOwnerValue == nullptr && (OldOwnerClass->NewerVersion != nullptr
				|| OldOwnerClass->HasAnyClassFlags(CLASS_NewerVersionExists))))
		{
			return false;
		}

		const FClassProperty* ClassProperty = CastFieldChecked<FClassProperty>(Property);
		UClass* ExpectedMetaClass = ClassProperty->MetaClass;
		if (ReloadClasses.Contains(ExpectedMetaClass)
			|| ReloadClasses.FindKey(ExpectedMetaClass) != nullptr
			|| !IsCurrentClassIdentity(ExpectedMetaClass))
		{
			return false;
		}

		UClass* CapturedValue = Cast<UClass>(
			ClassProperty->GetObjectPropertyValue_InContainer(Object));
		UClass* ExpectedValue = CapturedValue;
		EReinstancedScriptClassReferenceKind Kind =
			EReinstancedScriptClassReferenceKind::Null;
		if (CapturedValue != nullptr)
		{
			if (!IsValid(CapturedValue) || !CapturedValue->IsChildOf(ExpectedMetaClass))
			{
				return false;
			}
			if (UClass* const* Replacement = ReloadClasses.Find(CapturedValue))
			{
				if (Cast<UASClass>(CapturedValue) == nullptr
					|| Cast<UASClass>(*Replacement) == nullptr)
				{
					return false;
				}
				ExpectedValue = *Replacement;
				Kind = EReinstancedScriptClassReferenceKind::ReloadedScriptOldToNew;
			}
			else if (ReloadClasses.FindKey(CapturedValue) != nullptr)
			{
				if (Cast<UASClass>(CapturedValue) == nullptr)
				{
					return false;
				}
				Kind = EReinstancedScriptClassReferenceKind::AlreadyExpectedNew;
			}
			else if (CapturedValue->HasAnyClassFlags(CLASS_Native)
				&& IsCurrentClassIdentity(CapturedValue))
			{
				Kind = EReinstancedScriptClassReferenceKind::StableNativeExact;
			}
			else
			{
				return false;
			}
			if (!IsCurrentClassIdentity(ExpectedValue)
				|| !ExpectedValue->IsChildOf(ExpectedMetaClass))
			{
				return false;
			}
		}

		FReinstancedScriptClassReferenceSnapshot& Snapshot =
			OutSnapshots.AddDefaulted_GetRef();
		Snapshot.PropertyName = Property->GetFName();
		Snapshot.ExpectedNewOwnerIdentity = static_cast<const void*>(ExpectedNewOwnerClass);
		Snapshot.PropertyClass = Property->GetClass();
		Snapshot.PropertyTraits = GetAngelscriptPropertyTraits(Property);
		Snapshot.Offset = Property->GetOffset_ForInternal();
		Snapshot.Size = Property->GetSize();
		Snapshot.ElementSize = Property->GetElementSize();
		Snapshot.ArrayDim = Property->ArrayDim;
		Snapshot.PropertyFlags = static_cast<uint64>(Property->GetPropertyFlags());
		Snapshot.MetaClassKey = FObjectKey(ExpectedMetaClass);
		Snapshot.MetaClassIdentity = static_cast<const void*>(ExpectedMetaClass);
		Snapshot.Kind = Kind;
		if (CapturedValue != nullptr)
		{
			Snapshot.ExpectedValueKey = FObjectKey(ExpectedValue);
			Snapshot.ExpectedValueIdentity = static_cast<const void*>(ExpectedValue);
		}
	}
	return true;
}

struct FScriptActorSceneComponentAttachmentSnapshot
{
	const void* OldObjectIdentity = nullptr;
	const void* OldOwnerIdentity = nullptr;
	const void* OldParentIdentity = nullptr;
	FName ComponentName = NAME_None;
	FName SocketName = NAME_None;
	FTransform RelativeTransform;
	bool bWasRegistered = false;
	bool bIsRoot = false;
	bool bIsDefaultSubobject = false;
	bool bOwnerIsActor = false;
	bool bWorldIsActorWorld = false;
	TArray<const void*> ChildIdentities;
};

struct FScriptActorOwnedComponentSnapshot
{
	const void* OldObjectIdentity = nullptr;
	const void* OldOwnerIdentity = nullptr;
	const void* OldClassIdentity = nullptr;
	const void* ExpectedNewClassIdentity = nullptr;
	FName ComponentName = NAME_None;
	bool bIsDefaultSubobject = false;
	bool bIsScriptOwnedTarget = false;
};

struct FScriptActorAttachmentSnapshot
{
	FObjectKey OldActorKey;
	const void* OldActorIdentity = nullptr;
	const void* OldWorldIdentity = nullptr;
	FName ActorName = NAME_None;
	FString ActorFullName;
	uint64 CaptureGeneration = 0;
	const void* OldRootIdentity = nullptr;
	TArray<FScriptActorOwnedComponentSnapshot> ActorComponents;
	TArray<FScriptActorSceneComponentAttachmentSnapshot> Components;
};

struct FScriptActorSceneComponentAttachmentRestore
{
	USceneComponent* NewComponent = nullptr;
	USceneComponent* NewParent = nullptr;
	FName SocketName = NAME_None;
	FTransform RelativeTransform;
	bool bWasRegistered = false;
	bool bWasListedByParent = false;
	bool bIsRoot = false;
	USceneComponent* OriginalParent = nullptr;
	FName OriginalSocketName = NAME_None;
	FTransform OriginalRelativeTransform;
	bool bWasOriginallyRegistered = false;
	bool bWasOriginallyListedByParent = false;
	TArray<USceneComponent*> ExpectedChildren;
	TArray<USceneComponent*> OriginalChildren;
	bool bNormalizeRetiredChildren = false;
	bool bParentWasRemoved = false;
	int32 AttachmentDepth = 0;
	int32 OriginalAttachmentDepth = 0;
	int32 TargetSiblingIndex = INDEX_NONE;
	int32 OriginalSiblingIndex = INDEX_NONE;
};

struct FScriptActorAttachmentRestorePlan
{
	AActor* NewActor = nullptr;
	USceneComponent* NewRoot = nullptr;
	USceneComponent* OriginalRoot = nullptr;
	TArray<FScriptActorSceneComponentAttachmentRestore> Components;
};

TMap<const void*, FReloadCaptureRecord> ReloadCapturesByOldIdentity;
TArray<FScriptActorAttachmentSnapshot> ScriptActorAttachmentSnapshots;
uint64 NextReloadCaptureGeneration = 1;
uint64 ActiveReloadCaptureGeneration = 0;
bool bRuntimeScriptReplacementCaptureActive = false;
bool bRuntimeScriptReplacementCaptureAborted = false;
uint64 NextRuntimeTrackSequence = 1;
bool bReinstancedScriptPairConsumerInProgress = false;

struct FReinstancedScriptClassPair
{
	UObject* NewObject = nullptr;
	UASClass* NewClass = nullptr;
	asITypeInfo* NewTypeCookie = nullptr;
	const FReloadCaptureRecord* Capture = nullptr;
};

struct FClassReferenceRestorePlan
{
	FClassProperty* Property = nullptr;
	UClass* OriginalValue = nullptr;
	UClass* ExpectedValue = nullptr;
};

bool ResolveSnapshotClass(
	const FObjectKey& Key,
	const void* Identity,
	UClass*& OutClass)
{
	OutClass = nullptr;
	if (Identity == nullptr)
	{
		return true;
	}
	UClass* ResolvedClass = Cast<UClass>(Key.ResolveObjectPtrEvenIfUnreachable());
	if (ResolvedClass == nullptr || !IsValid(ResolvedClass)
		|| static_cast<const void*>(ResolvedClass) != Identity
		|| !(FObjectKey(ResolvedClass) == Key)
		|| !IsCurrentClassIdentity(ResolvedClass))
	{
		return false;
	}
	OutClass = ResolvedClass;
	return true;
}

bool IsSnapshotExpectedClassConsistent(
	const FReinstancedScriptClassReferenceSnapshot& Snapshot,
	UClass* ExpectedValue)
{
	switch (Snapshot.Kind)
	{
	case EReinstancedScriptClassReferenceKind::Null:
		return ExpectedValue == nullptr
			&& Snapshot.ExpectedValueIdentity == nullptr;
	case EReinstancedScriptClassReferenceKind::StableNativeExact:
		return ExpectedValue != nullptr
			&& ExpectedValue->HasAnyClassFlags(CLASS_Native)
			&& IsCurrentClassIdentity(ExpectedValue);
	case EReinstancedScriptClassReferenceKind::ReloadedScriptOldToNew:
	case EReinstancedScriptClassReferenceKind::AlreadyExpectedNew:
		return ExpectedValue != nullptr
			&& Cast<UASClass>(ExpectedValue) != nullptr
			&& IsCurrentClassIdentity(ExpectedValue);
	default:
		return false;
	}
}

bool BuildRuntimeClassReferenceRestorePlan(
	UObject* NewObject,
	UASClass* NewClass,
	asITypeInfo* NewTypeCookie,
	const TArray<FReinstancedScriptClassReferenceSnapshot>& Snapshots,
	bool bCaptureOriginalValues,
	TArray<FClassReferenceRestorePlan>& OutPlans)
{
	if (NewObject == nullptr || NewClass == nullptr || NewTypeCookie == nullptr
		|| !IsValid(NewObject) || NewObject->GetClass() != NewClass
		|| NewTypeCookie != NewClass->ScriptTypePtr
		|| NewTypeCookie->GetUserData() != NewClass)
	{
		return false;
	}
	for (const FReinstancedScriptClassReferenceSnapshot& Snapshot : Snapshots)
	{
		FProperty* CurrentProperty = NewClass->FindPropertyByName(Snapshot.PropertyName);
		FClassProperty* ClassProperty = CastField<FClassProperty>(CurrentProperty);
		UClass* MetaClass = nullptr;
		UClass* ExpectedValue = nullptr;
		if (ClassProperty == nullptr
			|| !IsAdmittedRuntimeClassReferenceProperty(
				CurrentProperty, NewClass, NewTypeCookie)
			|| static_cast<const void*>(CurrentProperty->GetOwnerStruct())
				!= Snapshot.ExpectedNewOwnerIdentity
			|| CurrentProperty->GetClass() != Snapshot.PropertyClass
			|| GetAngelscriptPropertyTraits(CurrentProperty) != Snapshot.PropertyTraits
			|| CurrentProperty->GetOffset_ForInternal() != Snapshot.Offset
			|| CurrentProperty->GetSize() != Snapshot.Size
			|| CurrentProperty->GetElementSize() != Snapshot.ElementSize
			|| CurrentProperty->ArrayDim != Snapshot.ArrayDim
			|| static_cast<uint64>(CurrentProperty->GetPropertyFlags())
				!= Snapshot.PropertyFlags
			|| !ResolveSnapshotClass(
				Snapshot.MetaClassKey, Snapshot.MetaClassIdentity, MetaClass)
			|| MetaClass == nullptr || ClassProperty->MetaClass != MetaClass
			|| !ResolveSnapshotClass(
				Snapshot.ExpectedValueKey, Snapshot.ExpectedValueIdentity, ExpectedValue)
			|| !IsSnapshotExpectedClassConsistent(Snapshot, ExpectedValue)
			|| (ExpectedValue != nullptr && !ExpectedValue->IsChildOf(MetaClass)))
		{
			return false;
		}

		FClassReferenceRestorePlan& Plan = OutPlans.AddDefaulted_GetRef();
		Plan.Property = ClassProperty;
		Plan.ExpectedValue = ExpectedValue;
		if (bCaptureOriginalValues)
		{
			Plan.OriginalValue = Cast<UClass>(
				ClassProperty->GetObjectPropertyValue_InContainer(NewObject));
			if (Plan.OriginalValue != nullptr && (!IsValid(Plan.OriginalValue)
				|| !Plan.OriginalValue->IsChildOf(MetaClass)))
			{
				return false;
			}
		}
	}
	return true;
}

bool ValidateReinstancedScriptPairClassReferencePreflight(
	const FReinstancedScriptClassPair& Pair)
{
	if (Pair.Capture == nullptr)
	{
		return false;
	}
	TArray<FClassReferenceRestorePlan> Plans;
	return BuildRuntimeClassReferenceRestorePlan(
		Pair.NewObject,
		Pair.NewClass,
		Pair.NewTypeCookie,
		Pair.Capture->ClassReferenceSnapshots,
		false,
		Plans);
}


bool UpdateBridgeRuntimeObjectTypeCookieForReconstruction(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	asITypeInfo* NewTypeCookie,
	bool bRollback
);

const TCHAR* ReinstancedScriptPairFailureStageName(
	EReinstancedScriptPairFailureStage Stage
)
{
	return Stage == EReinstancedScriptPairFailureStage::BridgeForwardUpdate
		? TEXT("BridgeForwardUpdate")
		: TEXT("PREFLIGHT");
}

bool MarkReinstancedScriptPairFailed(
	UObject* NewObject,
	const FReinstancedScriptPairFinalizationContext* PairContext
)
{
	if (NewObject == nullptr || PairContext == nullptr)
	{
		return false;
	}

	const FObjectKey NewKey(NewObject);
	const void* NewObjectIdentity = static_cast<const void*>(NewObject);
	if (!(NewKey == PairContext->NewKey)
		|| NewObjectIdentity != PairContext->NewObjectIdentity)
	{
		return false;
	}

	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	FConsumedReinstancedScriptClassPair* Record =
		ReinstancedScriptPairsByNewObject.Find(NewKey);
	if (Record == nullptr
		|| !(Record->NewKey == PairContext->NewKey)
		|| Record->NewObjectIdentity != NewObjectIdentity
		|| Record->OldKey != PairContext->OldKey
		|| Record->OldObjectIdentity != PairContext->OldObjectIdentity
		|| Record->CaptureGeneration != PairContext->CaptureGeneration
		|| Record->OldTrackSequence != PairContext->OldTrackSequence
		|| Record->NewTrackSequence != PairContext->NewTrackSequence
		|| Record->State
			!= EReinstancedScriptPairConsumptionState::Reserved)
	{
		return false;
	}
	FReloadCaptureRecord* Capture =
		ReloadCapturesByOldIdentity.Find(PairContext->OldObjectIdentity);
	if (Capture == nullptr
		|| Capture->State != EReloadCaptureState::Reserved
		|| Capture->OldKey != PairContext->OldKey
		|| Capture->CaptureGeneration != PairContext->CaptureGeneration
		|| Capture->OldTrackSequence != PairContext->OldTrackSequence)
	{
		return false;
	}
	Record->State = EReinstancedScriptPairConsumptionState::Failed;
	Capture->State = EReloadCaptureState::Failed;
	return true;
}

bool ReserveReinstancedScriptPairWindow(
	const TArray<FReinstancedScriptClassPair>& Pairs,
	TArray<FReinstancedScriptPairFinalizationContext>& OutContexts,
	EReinstancedScriptPairFailureStage& OutFailureStage
)
{
	OutContexts.Reset();
	OutFailureStage = EReinstancedScriptPairFailureStage::Preflight;
	if (Pairs.Num() == 0)
	{
		return true;
	}

	// Schema and Bridge probes are side-effect-free and run before any ledger,
	// capture, mirror, or pair-finalization claim publication.
	for (const FReinstancedScriptClassPair& Pair : Pairs)
	{
		if (!ValidateReinstancedScriptPairClassReferencePreflight(Pair))
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning,
				TEXT("SCRIPT_REINSTANCED_PAIR_PREFLIGHT_DETAIL stage=ClassReferences class=%s"),
				*Pair.NewClass->GetPathName());
			return false;
		}
		if (!UpdateBridgeRuntimeObjectTypeCookieForReconstruction(
				Pair.NewObject,
				Pair.NewTypeCookie,
				Pair.NewTypeCookie,
				false
			))
		{
			OutFailureStage =
				EReinstancedScriptPairFailureStage::BridgeForwardUpdate;
			return false;
		}
	}

	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	if (bRuntimeLifecycleShuttingDown || !bRuntimeScriptReplacementCaptureActive)
	{
		return false;
	}

	TSet<const void*> SeenOldIdentities;
	TSet<FObjectKey> SeenNewKeys;
	TArray<FReinstancedScriptPairFinalizationContext> Contexts;
	Contexts.Reserve(Pairs.Num());

	// Keep this pass read-only. No ledger or capture state is changed until
	// every callback pair has passed the complete window preflight.
	for (const FReinstancedScriptClassPair& Pair : Pairs)
	{
		const FReloadCaptureRecord* Capture = Pair.Capture;
		if (Capture == nullptr
			|| ReloadCapturesByOldIdentity.Find(Capture->OldObjectIdentity) != Capture
			|| Capture->State != EReloadCaptureState::Captured
			|| Capture->CaptureGeneration != ActiveReloadCaptureGeneration
			|| Capture->OldObjectIdentity == nullptr
			|| SeenOldIdentities.Contains(Capture->OldObjectIdentity))
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning, TEXT("SCRIPT_REINSTANCED_PAIR_PREFLIGHT_DETAIL stage=Capture"));
			return false;
		}
		SeenOldIdentities.Add(Capture->OldObjectIdentity);

		const FObjectKey NewKey(Pair.NewObject);
		if (NewKey == Capture->OldKey || SeenNewKeys.Contains(NewKey))
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning, TEXT("SCRIPT_REINSTANCED_PAIR_PREFLIGHT_DETAIL stage=IdentityKeys"));
			return false;
		}
		SeenNewKeys.Add(NewKey);

		for (const TPair<FObjectKey, FConsumedReinstancedScriptClassPair>& ExistingPair :
			ReinstancedScriptPairsByNewObject)
		{
			const FConsumedReinstancedScriptClassPair& Existing = ExistingPair.Value;
			const bool bDirectConsumedPredecessor =
				Existing.State == EReinstancedScriptPairConsumptionState::Consumed
				&& Existing.NewKey == Capture->OldKey
				&& Existing.NewObjectIdentity == Capture->OldObjectIdentity
				&& Existing.NewTrackSequence == Capture->OldTrackSequence
				&& Existing.CaptureGeneration < Capture->CaptureGeneration;
			// Retired UObject addresses can be reused; only keys include the serial identity.
			if (Existing.NewKey == NewKey
				|| Existing.OldKey == Capture->OldKey
				|| (!bDirectConsumedPredecessor
					&& Existing.NewKey == Capture->OldKey)
				|| Existing.OldKey == NewKey)
			{
				UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning,
					TEXT("SCRIPT_REINSTANCED_PAIR_PREFLIGHT_DETAIL stage=Ledger "
						"existingGeneration=%llu currentGeneration=%llu oldRaw=%d newRaw=%d oldKey=%d newKey=%d predecessor=%d"),
					static_cast<unsigned long long>(Existing.CaptureGeneration), static_cast<unsigned long long>(Capture->CaptureGeneration),
					Existing.OldObjectIdentity == Capture->OldObjectIdentity || Existing.NewObjectIdentity == Capture->OldObjectIdentity,
					Existing.OldObjectIdentity == static_cast<const void*>(Pair.NewObject)
						|| Existing.NewObjectIdentity == static_cast<const void*>(Pair.NewObject),
					Existing.OldKey == Capture->OldKey || Existing.NewKey == Capture->OldKey,
					Existing.OldKey == NewKey || Existing.NewKey == NewKey, bDirectConsumedPredecessor);
				return false;
			}
		}

		const FRuntimeTrackedObject* OldEntry =
			RuntimeTrackedObjects.Find(Capture->OldKey);
		const FRuntimeTrackedObject* NewEntry =
			RuntimeTrackedObjects.Find(NewKey);
		if (OldEntry == nullptr
			|| !(OldEntry->Key == Capture->OldKey)
			|| static_cast<const void*>(OldEntry->ObjectIdentity)
				!= Capture->OldObjectIdentity
			|| OldEntry->State != ERuntimeMirrorState::Pending
			|| OldEntry->SavedTypeCookie != Capture->OldTypeCookie
			|| OldEntry->TrackSequence != Capture->OldTrackSequence
			|| NewEntry == nullptr
			|| NewEntry->ObjectIdentity != static_cast<const UObjectBase*>(Pair.NewObject)
			|| NewEntry->State != ERuntimeMirrorState::Pending
			|| NewEntry->SavedTypeCookie != Pair.NewTypeCookie)
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning,
				TEXT("SCRIPT_REINSTANCED_PAIR_PREFLIGHT_DETAIL stage=Tracking class=%s "
					"oldPresent=%d oldState=%d oldCookie=%d oldSequence=%d newPresent=%d newState=%d newCookie=%d"),
				*Pair.NewClass->GetPathName(), OldEntry != nullptr, OldEntry != nullptr ? int32(OldEntry->State) : -1,
				OldEntry != nullptr && OldEntry->SavedTypeCookie == Capture->OldTypeCookie,
				OldEntry != nullptr && OldEntry->TrackSequence == Capture->OldTrackSequence,
				NewEntry != nullptr, NewEntry != nullptr ? int32(NewEntry->State) : -1,
				NewEntry != nullptr && NewEntry->SavedTypeCookie == Pair.NewTypeCookie);
			return false;
		}

		FReinstancedScriptPairFinalizationContext& Context = Contexts.AddDefaulted_GetRef();
		Context.OldKey = Capture->OldKey;
		Context.NewKey = NewKey;
		Context.OldObjectIdentity = Capture->OldObjectIdentity;
		Context.NewObjectIdentity = static_cast<const void*>(Pair.NewObject);
		Context.OldTypeCookie = Capture->OldTypeCookie;
		Context.OldTypeCookieUserData = Capture->OldTypeCookieUserData;
		Context.OldClassIdentity = Capture->OldClassIdentity;
		Context.NewClassIdentity = Capture->NewClassIdentity;
		Context.CaptureGeneration = Capture->CaptureGeneration;
		Context.OldTrackSequence = Capture->OldTrackSequence;
		Context.NewTrackSequence = NewEntry->TrackSequence;
		Context.ScalarSnapshots = Capture->ScalarSnapshots;
		Context.ClassReferenceSnapshots = Capture->ClassReferenceSnapshots;
	}

	for (const FReinstancedScriptPairFinalizationContext& Context : Contexts)
	{
		if (SeenNewKeys.Contains(Context.OldKey))
		{
			return false;
		}
	}

	// Revalidate every pointer and state while the same lock is still held.
	// This pass must complete before the first ledger/capture/mirror mutation;
	// the mutation loop below therefore cannot discover a partial-batch error.
	bool bPublicationReady = Contexts.Num() == Pairs.Num();
	for (int32 Index = 0; Index < Contexts.Num(); ++Index)
	{
		if (Index >= Pairs.Num())
		{
			bPublicationReady = false;
			continue;
		}
		const FReinstancedScriptPairFinalizationContext& Context = Contexts[Index];
		const FReloadCaptureRecord* Capture =
			ReloadCapturesByOldIdentity.Find(Context.OldObjectIdentity);
		const FRuntimeTrackedObject* OldEntry =
			RuntimeTrackedObjects.Find(Context.OldKey);
		const FRuntimeTrackedObject* NewEntry =
			RuntimeTrackedObjects.Find(Context.NewKey);
		bPublicationReady = bPublicationReady
			&& Capture != nullptr
			&& Capture->State == EReloadCaptureState::Captured
			&& !Capture->bPairFinalizationClaimed
			&& Capture->OldKey == Context.OldKey
			&& Capture->OldObjectIdentity == Context.OldObjectIdentity
			&& Capture->CaptureGeneration == Context.CaptureGeneration
			&& Capture->OldTrackSequence == Context.OldTrackSequence
			&& OldEntry != nullptr
			&& OldEntry->Key == Context.OldKey
			&& static_cast<const void*>(OldEntry->ObjectIdentity)
				== Context.OldObjectIdentity
			&& OldEntry->State == ERuntimeMirrorState::Pending
			&& OldEntry->SavedTypeCookie == Context.OldTypeCookie
			&& OldEntry->TrackSequence == Context.OldTrackSequence
			&& NewEntry != nullptr
			&& NewEntry->Key == Context.NewKey
			&& NewEntry->ObjectIdentity == Context.NewObjectIdentity
			&& NewEntry->State == ERuntimeMirrorState::Pending
			&& NewEntry->SavedTypeCookie == Pairs[Index].NewTypeCookie
			&& NewEntry->TrackSequence == Context.NewTrackSequence
			&& !ReinstancedScriptPairsByNewObject.Contains(Context.NewKey);
	}
	if (!bPublicationReady)
	{
		FailRuntimeMirror(TEXT("REINSTANCED_PAIR_BATCH_PUBLISH_INVARIANT"), nullptr);
		return false;
	}

	for (int32 Index = 0; Index < Pairs.Num(); ++Index)
	{
		const FReinstancedScriptPairFinalizationContext& Context = Contexts[Index];
		ReinstancedScriptPairsByNewObject.Add(
			Context.NewKey,
			FConsumedReinstancedScriptClassPair(
				Context.OldKey,
				Context.NewKey,
				Context.OldObjectIdentity,
				Context.NewObjectIdentity,
				Context.CaptureGeneration,
				Context.OldTrackSequence,
				Context.NewTrackSequence,
				EReinstancedScriptPairConsumptionState::Reserved
			)
		);
		FReloadCaptureRecord* MutableCapture =
			ReloadCapturesByOldIdentity.Find(Context.OldObjectIdentity);
		MutableCapture->State = EReloadCaptureState::Reserved;
		MutableCapture->bPairFinalizationClaimed = true;
		FRuntimeTrackedObject* MutableNewEntry =
			RuntimeTrackedObjects.Find(Context.NewKey);
		MutableNewEntry->State = ERuntimeMirrorState::Reconstructing;
	}
	OutContexts = MoveTemp(Contexts);
	return true;
}

void FinalizeReinstancedScriptPairWindow(
	const TArray<FReinstancedScriptClassPair>& Pairs,
	const TArray<FReinstancedScriptPairFinalizationContext>& Contexts
)
{
	if (Pairs.Num() != Contexts.Num())
	{
		FailRuntimeMirror(
			TEXT("REINSTANCED_PAIR_BATCH_CONTEXT_COUNT_MISMATCH"),
			nullptr
		);
		return;
	}

	// This is the only final pair-state lock acquisition. It does not resolve
	// UObjects, call Bridge, allocate, log, or recover. Every record is checked
	// first, then all terminal transitions are applied in one mutation pass.
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	bool bFinalizationReady = true;
	for (int32 Index = 0; Index < Contexts.Num(); ++Index)
	{
		const FReinstancedScriptClassPair& Pair = Pairs[Index];
		const FReinstancedScriptPairFinalizationContext& Context = Contexts[Index];
		const FConsumedReinstancedScriptClassPair* Record =
			ReinstancedScriptPairsByNewObject.Find(Context.NewKey);
		const FRuntimeTrackedObject* NewEntry =
			RuntimeTrackedObjects.Find(Context.NewKey);
		const FReloadCaptureRecord* Capture =
			ReloadCapturesByOldIdentity.Find(Context.OldObjectIdentity);
		const FRuntimeTrackedObject* OldEntry =
			RuntimeTrackedObjects.Find(Context.OldKey);
		const bool bOldEntryValid = OldEntry != nullptr
			&& OldEntry->Key == Context.OldKey
			&& static_cast<const void*>(OldEntry->ObjectIdentity)
				== Context.OldObjectIdentity
			&& OldEntry->State == ERuntimeMirrorState::Pending
			&& OldEntry->SavedTypeCookie == Context.OldTypeCookie
			&& OldEntry->TrackSequence == Context.OldTrackSequence;
		const bool bOldNaturalGcRemovalValid = OldEntry == nullptr
			&& Record != nullptr
			&& Record->bOldRemovedByNaturalGc;
		const bool bRecordValid = Record != nullptr
			&& Record->State == EReinstancedScriptPairConsumptionState::Reserved
			&& Record->OldKey == Context.OldKey
			&& Record->NewKey == Context.NewKey
			&& Record->OldObjectIdentity == Context.OldObjectIdentity
			&& Record->NewObjectIdentity == Context.NewObjectIdentity
			&& Record->CaptureGeneration == Context.CaptureGeneration
			&& Record->OldTrackSequence == Context.OldTrackSequence
			&& Record->NewTrackSequence == Context.NewTrackSequence;
		const bool bCaptureValid = Capture != nullptr
			&& Capture->State == EReloadCaptureState::Reserved
			&& Capture->OldKey == Context.OldKey
			&& Capture->OldObjectIdentity == Context.OldObjectIdentity
			&& Capture->OldTypeCookie == Context.OldTypeCookie
			&& Capture->OldTypeCookieUserData == Context.OldTypeCookieUserData
			&& Capture->OldClassIdentity == Context.OldClassIdentity
			&& Capture->NewClassIdentity == Context.NewClassIdentity
			&& Capture->CaptureGeneration == Context.CaptureGeneration
			&& Capture->OldTrackSequence == Context.OldTrackSequence
			&& Capture->bPairFinalizationClaimed;
		const bool bNewEntryValid = NewEntry != nullptr
			&& NewEntry->ObjectIdentity
				== static_cast<const UObjectBase*>(Pair.NewObject)
			&& NewEntry->State == ERuntimeMirrorState::Reconstructing
			&& NewEntry->SavedTypeCookie == Pair.NewTypeCookie
			&& NewEntry->TrackSequence == Context.NewTrackSequence;
		bFinalizationReady = bFinalizationReady
			&& Pair.NewObject != nullptr
			&& Context.NewObjectIdentity == static_cast<const void*>(Pair.NewObject)
			&& Pair.NewTypeCookie != nullptr
			&& bRecordValid
			&& bCaptureValid
			&& bNewEntryValid
			&& (bOldEntryValid || bOldNaturalGcRemovalValid);
	}
	if (!bFinalizationReady)
	{
		FailRuntimeMirror(TEXT("REINSTANCED_PAIR_BATCH_FINALIZE_INVARIANT"), nullptr);
		return;
	}

	for (const FReinstancedScriptPairFinalizationContext& Context : Contexts)
	{
		FConsumedReinstancedScriptClassPair* Record =
			ReinstancedScriptPairsByNewObject.Find(Context.NewKey);
		FReloadCaptureRecord* Capture =
			ReloadCapturesByOldIdentity.Find(Context.OldObjectIdentity);
		FRuntimeTrackedObject* NewEntry =
			RuntimeTrackedObjects.Find(Context.NewKey);
		NewEntry->State = ERuntimeMirrorState::Pending;
		Record->State = EReinstancedScriptPairConsumptionState::Consumed;
		Capture->State = EReloadCaptureState::Consumed;
	}
}



bool IsRawReinstancedScriptPairTombstone(UObject* OldObject, UObject* NewObject)
{
	// This is deliberately address-only. The old pointer may already be retired.
	const void* OldObjectIdentity = static_cast<const void*>(OldObject);
	const void* NewObjectIdentity = static_cast<const void*>(NewObject);
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	for (const TPair<FObjectKey, FConsumedReinstancedScriptClassPair>& Pair :
		ReinstancedScriptPairsByNewObject)
	{
		if (Pair.Value.OldObjectIdentity == OldObjectIdentity
			&& Pair.Value.NewObjectIdentity == NewObjectIdentity
			&& Pair.Value.State != EReinstancedScriptPairConsumptionState::Reserved)
		{
			return true;
		}
	}
	return false;
}

enum class EReinstancedScriptPairFinalizationContextMatch : uint8
{
	Ordinary,
	Pair,
	Mismatch,
};

EReinstancedScriptPairFinalizationContextMatch GetReinstancedScriptPairFinalizationContextMatch(
	UObject* NewObject,
	const FReinstancedScriptPairFinalizationContext* PairContext
)
{
	if (PairContext == nullptr)
	{
		return EReinstancedScriptPairFinalizationContextMatch::Ordinary;
	}

	if (NewObject == nullptr
		|| !(FObjectKey(NewObject) == PairContext->NewKey)
		|| static_cast<const void*>(NewObject)
			!= PairContext->NewObjectIdentity)
	{
		return EReinstancedScriptPairFinalizationContextMatch::Mismatch;
	}

	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	const FRuntimeTrackedObject* NewEntry =
		RuntimeTrackedObjects.Find(PairContext->NewKey);
	return NewEntry != nullptr
		&& NewEntry->ObjectIdentity
			== static_cast<const UObjectBase*>(NewObject)
		&& NewEntry->TrackSequence == PairContext->NewTrackSequence
		? EReinstancedScriptPairFinalizationContextMatch::Pair
		: EReinstancedScriptPairFinalizationContextMatch::Mismatch;
}

bool FinalizeReinstancedScriptPairConsumption(
	UObject* NewObject,
	asITypeInfo* NewTypeCookie,
	const FReinstancedScriptPairFinalizationContext* PairContext
)
{
	const EReinstancedScriptPairFinalizationContextMatch ContextMatch =
		GetReinstancedScriptPairFinalizationContextMatch(NewObject, PairContext);
	if (ContextMatch
		== EReinstancedScriptPairFinalizationContextMatch::Ordinary)
	{
		// Ordinary same-object reconstruction does not finalize replacement pairs.
		// A live Consumed or Failed record remains a replay tombstone only.
		return true;
	}

	if (ContextMatch
		!= EReinstancedScriptPairFinalizationContextMatch::Pair
		|| NewObject == nullptr
		|| NewTypeCookie == nullptr)
	{
		return false;
	}

	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	FConsumedReinstancedScriptClassPair* Record =
		ReinstancedScriptPairsByNewObject.Find(FObjectKey(NewObject));
	FRuntimeTrackedObject* NewEntry =
		RuntimeTrackedObjects.Find(FObjectKey(NewObject));
	FReloadCaptureRecord* Capture = PairContext != nullptr
		? ReloadCapturesByOldIdentity.Find(PairContext->OldObjectIdentity)
		: nullptr;
	const bool bCanConsume = Record != nullptr
		&& Record->State == EReinstancedScriptPairConsumptionState::Reserved
		&& Record->NewKey == PairContext->NewKey
		&& Record->OldKey == PairContext->OldKey
		&& Record->OldObjectIdentity == PairContext->OldObjectIdentity
		&& Record->CaptureGeneration == PairContext->CaptureGeneration
		&& Record->OldTrackSequence == PairContext->OldTrackSequence
		&& Record->NewObjectIdentity
			== static_cast<const void*>(NewObject)
		&& Record->NewTrackSequence == PairContext->NewTrackSequence
		&& NewEntry != nullptr
		&& NewEntry->ObjectIdentity == Record->NewObjectIdentity
		&& NewEntry->TrackSequence == Record->NewTrackSequence
		&& NewEntry->State == ERuntimeMirrorState::Pending
		&& NewEntry->SavedTypeCookie == NewTypeCookie
		&& Capture != nullptr
		&& Capture->State == EReloadCaptureState::Reserved
		&& Capture->OldKey == PairContext->OldKey
		&& Capture->OldTypeCookie == PairContext->OldTypeCookie
		&& Capture->OldTypeCookieUserData == PairContext->OldTypeCookieUserData
		&& Capture->CaptureGeneration == PairContext->CaptureGeneration
		&& Capture->OldTrackSequence == PairContext->OldTrackSequence
		&& Capture->bPairFinalizationClaimed;
	if (!bCanConsume)
	{
		if (Record != nullptr
			&& Record->State == EReinstancedScriptPairConsumptionState::Reserved)
		{
			Record->State = EReinstancedScriptPairConsumptionState::Failed;
		}
		if (Capture != nullptr && Capture->State == EReloadCaptureState::Reserved)
		{
			Capture->State = EReloadCaptureState::Failed;
		}
		return false;
	}

	Record->State = EReinstancedScriptPairConsumptionState::Consumed;
	Capture->State = EReloadCaptureState::Consumed;
	return true;
}

bool ValidateReinstancedScriptPairFinalization(
	UObject* NewObject,
	asITypeInfo* NewTypeCookie,
	const FReinstancedScriptPairFinalizationContext* PairContext
)
{
	const EReinstancedScriptPairFinalizationContextMatch ContextMatch =
		GetReinstancedScriptPairFinalizationContextMatch(NewObject, PairContext);
	if (ContextMatch
		== EReinstancedScriptPairFinalizationContextMatch::Ordinary)
	{
		return true;
	}

	if (ContextMatch
		!= EReinstancedScriptPairFinalizationContextMatch::Pair
		|| NewObject == nullptr
		|| NewTypeCookie == nullptr)
	{
		return false;
	}

	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	const FConsumedReinstancedScriptClassPair* Record =
		ReinstancedScriptPairsByNewObject.Find(FObjectKey(NewObject));
	const FRuntimeTrackedObject* NewEntry =
		RuntimeTrackedObjects.Find(FObjectKey(NewObject));
	const FReloadCaptureRecord* Capture = PairContext != nullptr
		? ReloadCapturesByOldIdentity.Find(PairContext->OldObjectIdentity)
		: nullptr;
	return Record != nullptr
		&& Record->State
			== EReinstancedScriptPairConsumptionState::Reserved
		&& Record->NewKey == PairContext->NewKey
		&& Record->OldKey == PairContext->OldKey
		&& Record->OldObjectIdentity == PairContext->OldObjectIdentity
		&& Record->CaptureGeneration == PairContext->CaptureGeneration
		&& Record->OldTrackSequence == PairContext->OldTrackSequence
		&& Record->NewObjectIdentity
			== static_cast<const void*>(NewObject)
		&& Record->NewTrackSequence == PairContext->NewTrackSequence
		&& NewEntry != nullptr
		&& NewEntry->ObjectIdentity == Record->NewObjectIdentity
		&& NewEntry->TrackSequence == Record->NewTrackSequence
		&& NewEntry->State == ERuntimeMirrorState::Reconstructing
		&& NewEntry->SavedTypeCookie == NewTypeCookie
		&& Capture != nullptr
		&& Capture->State == EReloadCaptureState::Reserved
		&& Capture->OldKey == PairContext->OldKey
		&& Capture->OldTypeCookie == PairContext->OldTypeCookie
		&& Capture->OldTypeCookieUserData == PairContext->OldTypeCookieUserData
		&& Capture->CaptureGeneration == PairContext->CaptureGeneration
		&& Capture->OldTrackSequence == PairContext->OldTrackSequence
		&& Capture->bPairFinalizationClaimed;
}

#if WITH_DEV_AUTOMATION_TESTS


bool IsRuntimeDestructorFaultTarget(const UObject* Object)
{
	return Object != nullptr
		&& Object->GetName().StartsWith(TEXT("RuntimeGcDestructorCallbackFaultProbe"));
}
#endif

asITypeInfo* TryClaimRuntimeTrackedMirror(UObject* Object, asITypeInfo* ExpectedTypeCookie,
    ERuntimeMirrorState ExpectedState, ERuntimeMirrorState NewState, FRuntimeTrackedObject* Snapshot = nullptr)
{
    if (!Object) return nullptr;
    FScopeLock Lock(&RuntimeTrackedObjectsMutex);
    if (bRuntimeLifecycleShuttingDown) return nullptr;
    auto* Entry = RuntimeTrackedObjects.Find(ResolveRuntimeDestructorKey(Object));
    if (!Entry) return nullptr;
    if (Snapshot) *Snapshot = *Entry;
    const auto* Certificate = SoloASBridge::GetActiveRuntimeDeferredDestroyToken();
    if (Entry->State != ExpectedState
        || !MatchesRuntimeDestructorTracking(Object, Entry->ObjectIdentity, Entry->SavedTypeCookie, Entry->TrackSequence, ExpectedTypeCookie, true)
        || (ExpectedState == ERuntimeMirrorState::TerminalDeferred
            && (!Certificate || Entry->TerminalPlanId != Certificate->PlanId))) return nullptr;
    Entry->State = NewState;
    return static_cast<asITypeInfo*>(Entry->SavedTypeCookie);
}

asITypeInfo* ClaimRuntimeTrackedMirror(UObject* Object, asITypeInfo* ExpectedTypeCookie,
    ERuntimeMirrorState ExpectedState, ERuntimeMirrorState NewState)
{
    checkf(IsInGameThread(), TEXT("MIRROR_CLAIM_FAILED"));
    FRuntimeTrackedObject Snapshot;
    asITypeInfo* Cookie = TryClaimRuntimeTrackedMirror(Object, ExpectedTypeCookie, ExpectedState, NewState, &Snapshot);
    if (!Cookie)
    {
        UE_LOG(LogAngelscriptRuntimeGcLifecycle, Error,
            TEXT("MIRROR_CLAIM_DETAIL object=%s template=%d present=%d state=%d expected=%d cookie=%d"),
            Object ? *Object->GetPathName() : TEXT("<null>"), Object && Object->IsTemplate(),
            Snapshot.TrackSequence != 0, Snapshot.TrackSequence ? int32(Snapshot.State) : -1, int32(ExpectedState),
            Snapshot.TrackSequence && (!ExpectedTypeCookie || Snapshot.SavedTypeCookie == ExpectedTypeCookie));
        FailRuntimeMirror(TEXT("MIRROR_CLAIM_FAILED"), Object);
        return nullptr;
    }
    CancelRuntimeTerminalOrdinaryClaim(Object, Cookie);
    return Cookie;
}

bool RemoveRuntimeTrackedMirror(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	ERuntimeMirrorState ExpectedState,
	bool bMarkNaturalGcRemoval = false
)
{
	if (Object == nullptr)
	{
		return false;
	}

	asIScriptModule* RetainedModule = nullptr;
	uint64 RetiredCaptureGeneration = 0;
	bool bRemoved = false;
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		const FObjectKey Key = ResolveRuntimeDestructorKey(Object);
		FRuntimeTrackedObject* Entry = RuntimeTrackedObjects.Find(Key);
		if (Entry == nullptr || Entry->State != ExpectedState
			|| !MatchesRuntimeDestructorTracking(Object, Entry->ObjectIdentity, Entry->SavedTypeCookie, Entry->TrackSequence, ExpectedTypeCookie))
		{
			return false;
		}
		const void* ObjectIdentity = static_cast<const void*>(Entry->ObjectIdentity);
		const uint64 TrackSequence = Entry->TrackSequence;
		RetainedModule = Entry->RetainedModule;
		Entry->RetainedModule = nullptr;
		ForgetRemovedRuntimeReceiver(Object, static_cast<asITypeInfo*>(Entry->SavedTypeCookie));
		bRemoved = RuntimeTrackedObjects.Remove(Key) == 1;
		if (bRemoved && bMarkNaturalGcRemoval)
		{
			FReloadCaptureRecord* Capture = ReloadCapturesByOldIdentity.Find(ObjectIdentity);
			if (bRuntimeScriptReplacementCaptureActive && Capture
				&& Capture->State == EReloadCaptureState::Captured
				&& Capture->CaptureGeneration == ActiveReloadCaptureGeneration
				&& Capture->OldKey == Key && Capture->OldObjectIdentity == ObjectIdentity
				&& Capture->OldTrackSequence == TrackSequence && Capture->OldTypeCookie == ExpectedTypeCookie)
			{
				// An unreferenced source may be collected before the engine creates its
				// replacement map. This is witnessed retirement, never pair consumption.
				Capture->State = EReloadCaptureState::RetiredByGc;
				ScriptActorAttachmentSnapshots.RemoveAll([&](const FScriptActorAttachmentSnapshot& Snapshot)
				{
					return Snapshot.OldActorKey == Key && Snapshot.OldActorIdentity == ObjectIdentity
						&& Snapshot.CaptureGeneration == Capture->CaptureGeneration;
				});
				RetiredCaptureGeneration = Capture->CaptureGeneration;
			}
			for (TPair<FObjectKey, FConsumedReinstancedScriptClassPair>& Pair :
				ReinstancedScriptPairsByNewObject)
			{
				FConsumedReinstancedScriptClassPair& Record = Pair.Value;
				if (Record.State == EReinstancedScriptPairConsumptionState::Reserved
					&& Record.OldKey == Key
					&& Record.OldObjectIdentity == ObjectIdentity
					&& Record.OldTrackSequence == TrackSequence)
				{
					Record.bOldRemovedByNaturalGc = true;
				}
			}
		}
	}
	if (bRemoved) CancelRuntimeTerminalOrdinaryClaim(Object, ExpectedTypeCookie);
	if (RetainedModule) AngelscriptRetiredModules::ReleaseModule(RetainedModule);
	if (RetiredCaptureGeneration)
		UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
			TEXT("SCRIPT_REINSTANCED_CAPTURE_RETIRED_BY_GC generation=%llu"), RetiredCaptureGeneration);
	return bRemoved;
}

bool PrepareRuntimeTrackedMirrorForLifecycle(
	UObject* Object,
	asITypeInfo* TypeCookie
)
{
	if (Object == nullptr || TypeCookie == nullptr)
	{
		return false;
	}

	const FObjectKey ClassKey(Object->GetClass());
	const bool bReplacementCaptureExcluded = IsScriptReplacementCaptureExcluded(Object);
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	const FObjectKey Key(Object);
	if (bRuntimeLifecycleShuttingDown || RuntimeTrackedObjects.Contains(Key))
	{
		return false;
	}

	RuntimeTrackedObjects.Add(
		Key,
		FRuntimeTrackedObject{Key, Object, TypeCookie, ERuntimeMirrorState::Pending,
			NextRuntimeTrackSequence++, ClassKey, bReplacementCaptureExcluded}
	);
	return true;
}

bool ClaimRuntimeTrackedMirrorForUntracking(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* ExpectedTypeCookie
)
{
    if (!Object || !ExpectedTypeCookie || Object->GetClass() != ScriptClass
        || ExpectedTypeCookie->GetUserData() != ScriptClass) return false;
    return TryClaimRuntimeTrackedMirror(Object, ExpectedTypeCookie,
        ERuntimeMirrorState::Pending, ERuntimeMirrorState::Untracking) == ExpectedTypeCookie;
}

bool RestoreRuntimeTrackedMirrorAfterUntrackFailure(UObject* Object)
{
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	FRuntimeTrackedObject* Entry = RuntimeTrackedObjects.Find(FObjectKey(Object));
	if (Entry == nullptr || Entry->State != ERuntimeMirrorState::Untracking)
	{
		return false;
	}
	Entry->State = ERuntimeMirrorState::Pending;
	return true;
}

bool RestoreRuntimeTrackedMirrorAfterReconstructionFailure(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	const FReinstancedScriptPairFinalizationContext* PairContext = nullptr
)
{
	bool bRecovered = false;
	if (Object != nullptr && ExpectedTypeCookie != nullptr)
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		FRuntimeTrackedObject* Entry = RuntimeTrackedObjects.Find(FObjectKey(Object));
		if (Entry != nullptr && Entry->State == ERuntimeMirrorState::Reconstructing
			&& Entry->SavedTypeCookie == ExpectedTypeCookie)
		{
			Entry->State = ERuntimeMirrorState::Pending;
			bRecovered = true;
		}
	}
	MarkReinstancedScriptPairFailed(Object, PairContext);
	return bRecovered;
}

bool RestoreRuntimeTrackedMirrorAfterCommittedReconstructionFailure(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	asITypeInfo* NewTypeCookie,
	const FReinstancedScriptPairFinalizationContext* PairContext = nullptr
)
{
	if (Object == nullptr || ExpectedTypeCookie == nullptr || NewTypeCookie == nullptr)
	{
		return false;
	}

	if (!UpdateRetainedRuntimeCookie(Object, NewTypeCookie, ExpectedTypeCookie,
        ERuntimeMirrorState::Pending, ERuntimeMirrorState::Pending)) return false;
	MarkReinstancedScriptPairFailed(Object, PairContext);
	return true;
}

bool RestoreRuntimeTrackedMirrorAfterDestructionFailure(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie
)
{
	bool bRecovered = false;
	if (Object != nullptr && ExpectedTypeCookie != nullptr && !IsRuntimeTerminalDestructorActive())
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		FRuntimeTrackedObject* Entry = RuntimeTrackedObjects.Find(FObjectKey(Object));
		if (Entry != nullptr && Entry->State == ERuntimeMirrorState::Dispatching
			&& Entry->ObjectIdentity == static_cast<const UObjectBase*>(Object)
			&& Entry->SavedTypeCookie == ExpectedTypeCookie)
		{
			Entry->State = ERuntimeMirrorState::Pending;
			bRecovered = true;
		}
	}
	MarkReinstancedScriptPairFailed(Object, nullptr);
	return bRecovered;
}

bool CallDestructor(UObject* Object, asITypeInfo* TypeCookie)
{
	if (Object == nullptr || TypeCookie == nullptr)
	{
		return false;
	}

#if WITH_DEV_AUTOMATION_TESTS
	if (IsRuntimeDestructorFaultTarget(Object)
		&& IsRuntimeReconstructionFaultEnabled(TEXT("DestructorCallback")))
	{
		return false;
	}
#endif

	UASClass* ScriptClass = static_cast<UASClass*>(TypeCookie->GetUserData());
	return ScriptClass != nullptr && Object->IsA(ScriptClass)
		&& ScriptClass->RuntimeDestroyObject(Object, TypeCookie);
}


bool ClaimRuntimeTrackedMirrorForLifecycleDestruction(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* ExpectedTypeCookie
)
{
	if (!IsValid(Object)
		|| ScriptClass == nullptr
		|| ExpectedTypeCookie == nullptr
		|| Object->GetClass() != ScriptClass
		|| ExpectedTypeCookie->GetUserData() != ScriptClass)
	{
		return false;
	}

    return TryClaimRuntimeTrackedMirror(Object, ExpectedTypeCookie,
        ERuntimeMirrorState::Pending, ERuntimeMirrorState::Dispatching) == ExpectedTypeCookie;
}

bool ValidateReinstancedScriptClassPair(
	const FReinstancedScriptClassPair& Pair
)
{
	const FReloadCaptureRecord* Capture = Pair.Capture;
	if (!IsInGameThread()
		|| bRuntimeLifecycleShuttingDown
		|| !bRuntimeScriptReplacementCaptureActive
		|| Capture == nullptr
		|| Capture->State != EReloadCaptureState::Captured
		|| Capture->CaptureGeneration != ActiveReloadCaptureGeneration
		|| Pair.NewObject == nullptr
		|| !IsValid(Pair.NewObject)
		|| Pair.NewClass == nullptr
		|| Pair.NewObject->GetClass() != Pair.NewClass
		|| Pair.NewObject->IsUnreachable()
		|| Pair.NewObject->HasAnyInternalFlags(EInternalObjectFlags::PendingConstruction)
		|| Pair.NewObject->HasAnyFlags(
			RF_BeginDestroyed
			| RF_FinishDestroyed
			| RF_NeedInitialization
			| RF_NeedLoad
			| RF_NeedPostLoad
			| RF_NeedPostLoadSubobjects
			| RF_ClassDefaultObject
			| RF_ArchetypeObject
			| RF_DefaultSubObject
		)
		|| Pair.NewClass->HasAnyClassFlags(CLASS_NewerVersionExists)
		|| Capture->OldTypeCookie == nullptr
		|| Capture->OldObjectIdentity == nullptr
		|| Capture->OldClassIdentity == nullptr
		|| Capture->OldRole != EReloadCaptureRole::Ordinary
		|| (Capture->OldFlags & (RF_BeginDestroyed | RF_FinishDestroyed
			| RF_ClassDefaultObject | RF_ArchetypeObject | RF_DefaultSubObject)) != 0
		|| Capture->NewClassIdentity != static_cast<const void*>(Pair.NewClass)
		|| Capture->OldTypeCookieUserData != Capture->OldClassIdentity
		|| Capture->OldTypeCookie == Pair.NewTypeCookie
		|| Pair.NewTypeCookie == nullptr
		|| Pair.NewTypeCookie->GetUserData() != Pair.NewClass
		|| Pair.NewTypeCookie != Pair.NewClass->ScriptTypePtr)
	{
		return false;
	}

	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	const FRuntimeTrackedObject* NewEntry =
		RuntimeTrackedObjects.Find(FObjectKey(Pair.NewObject));
	if (NewEntry == nullptr
		|| NewEntry->ObjectIdentity != static_cast<const UObjectBase*>(Pair.NewObject)
		|| NewEntry->State != ERuntimeMirrorState::Pending
		|| NewEntry->SavedTypeCookie != Pair.NewTypeCookie)
	{
		return false;
	}

	return true;
}

bool UpdateBridgeRuntimeObjectTypeCookieForReconstruction(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	asITypeInfo* NewTypeCookie,
	bool bRollback
)
{
	if (Object == nullptr || ExpectedTypeCookie == nullptr || NewTypeCookie == nullptr
		|| (!bRollback && !IsExactRuntimeReconstructionTypeCookie(Object, NewTypeCookie)))
	{
		return false;
	}

#if WITH_DEV_AUTOMATION_TESTS
	if (IsRuntimeReconstructionFaultTarget(Object)
		&& bRollback
		&& IsRuntimeReconstructionFaultEnabled(TEXT("RollbackRecovery")))
	{
		return false;
	}
	if (IsRuntimeReconstructionFaultTarget(Object)
		&& !bRollback
		&& IsRuntimeReconstructionFaultEnabled(TEXT("BridgeForwardUpdate")))
	{
		return false;
	}
#endif
	return SoloASBridge::UpdateRuntimeObjectTypeCookie(
		Object,
		ExpectedTypeCookie,
		NewTypeCookie
	);
}

bool IsScriptActorDeclaredComponentName(
	UASClass* ScriptActorClass,
	FName ComponentName
)
{
	for (UClass* CurrentClass = ScriptActorClass;
		CurrentClass != nullptr;
		CurrentClass = CurrentClass->GetSuperClass())
	{
		UASClass* CurrentScriptClass = Cast<UASClass>(CurrentClass);
		if (CurrentScriptClass == nullptr)
		{
			continue;
		}
		for (const UASClass::FDefaultComponent& DefaultComponent :
			CurrentScriptClass->DefaultComponents)
		{
			if (DefaultComponent.ComponentName == ComponentName)
			{
				return true;
			}
		}
		for (const UASClass::FOverrideComponent& OverrideComponent :
			CurrentScriptClass->OverrideComponents)
		{
			if (OverrideComponent.OverrideComponentName == ComponentName)
			{
				return true;
			}
		}
	}
	return false;
}

bool CaptureScriptActorAttachmentSnapshot(
	UObject* Object,
	uint64 CaptureGeneration,
	const TMap<UClass*, UClass*>& ReloadClasses,
	FScriptActorAttachmentSnapshot& OutSnapshot
)
{
	AActor* Actor = Cast<AActor>(Object);
	if (Actor == nullptr
		|| !IsValid(Actor)
		|| Actor->IsUnreachable()
		|| Actor->HasAnyFlags(
			RF_BeginDestroyed
			| RF_FinishDestroyed
			| RF_ClassDefaultObject
			| RF_ArchetypeObject
			| RF_DefaultSubObject
		))
	{
		return false;
	}

	OutSnapshot = FScriptActorAttachmentSnapshot{};
	OutSnapshot.OldActorKey = FObjectKey(Actor);
	OutSnapshot.OldActorIdentity = static_cast<const void*>(Actor);
	OutSnapshot.OldWorldIdentity = static_cast<const void*>(Actor->GetWorld());
	OutSnapshot.ActorName = Actor->GetFName();
	OutSnapshot.ActorFullName = Actor->GetFullName();
	OutSnapshot.CaptureGeneration = CaptureGeneration;
	OutSnapshot.OldRootIdentity = static_cast<const void*>(Actor->GetRootComponent());

	UASClass* ScriptActorClass = UASClass::GetFirstASClass(Actor);
	TArray<USceneComponent*> SceneComponents;
	for (UActorComponent* Component : Actor->GetComponents())
	{
		if (Component == nullptr
			|| !IsValid(Component)
			|| Component->IsUnreachable()
			|| Component->HasAnyFlags(
				RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return false;
		}
		// Capture public Actor component membership and stable identity/value data.
		// Script-owned non-scene entries are validated below; scene entries retain
		// their established attachment-specific snapshot and plan.
		FScriptActorOwnedComponentSnapshot& ComponentSnapshot =
			OutSnapshot.ActorComponents.AddDefaulted_GetRef();
		ComponentSnapshot.OldObjectIdentity =
			static_cast<const void*>(Component);
		ComponentSnapshot.OldOwnerIdentity =
			static_cast<const void*>(Component->GetOwner());
		UClass* OldComponentClass = Component->GetClass();
		if (OldComponentClass == nullptr)
		{
			return false;
		}
		ComponentSnapshot.OldClassIdentity =
			static_cast<const void*>(OldComponentClass);
		ComponentSnapshot.ComponentName = Component->GetFName();
		ComponentSnapshot.bIsDefaultSubobject =
			Component->IsDefaultSubobject();
		const bool bIsSceneComponent = Cast<USceneComponent>(Component) != nullptr;
		const bool bIsScriptOwnedTarget =
			!bIsSceneComponent
			&& (Cast<UASClass>(Component->GetClass()) != nullptr
				|| (ComponentSnapshot.bIsDefaultSubobject
					&& IsScriptActorDeclaredComponentName(
						ScriptActorClass,
						ComponentSnapshot.ComponentName)));
		ComponentSnapshot.bIsScriptOwnedTarget = bIsScriptOwnedTarget;
		if (bIsScriptOwnedTarget)
		{
			UClass* const* NewComponentClassValue =
				ReloadClasses.Find(OldComponentClass);
			if (NewComponentClassValue != nullptr)
			{
				if (*NewComponentClassValue == nullptr)
				{
					return false;
				}
				ComponentSnapshot.ExpectedNewClassIdentity =
					static_cast<const void*>(*NewComponentClassValue);
			}
			else
			{
				if (OldComponentClass->HasAnyClassFlags(
					CLASS_NewerVersionExists)
					|| OldComponentClass->GetAuthoritativeClass()
						!= OldComponentClass)
				{
					return false;
				}
				ComponentSnapshot.ExpectedNewClassIdentity =
					static_cast<const void*>(OldComponentClass);
			}
		}
		if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
		{
			if (!IsValid(SceneComponent)
				|| SceneComponent->IsUnreachable()
				|| SceneComponent->HasAnyFlags(
					RF_BeginDestroyed | RF_FinishDestroyed))
			{
				return false;
			}
			SceneComponents.Add(SceneComponent);
		}
	}
	SceneComponents.Sort([](const USceneComponent& Left, const USceneComponent& Right)
	{
		const int32 NameOrder = Left.GetFName().LexicalLess(Right.GetFName())
			? -1
			: (Right.GetFName().LexicalLess(Left.GetFName()) ? 1 : 0);
		return NameOrder != 0
			? NameOrder < 0
			: Left.GetFullName().Compare(Right.GetFullName()) < 0;
	});

	OutSnapshot.Components.Reserve(SceneComponents.Num());
	bool bFoundRoot = OutSnapshot.OldRootIdentity == nullptr;
	for (USceneComponent* SceneComponent : SceneComponents)
	{
		if (SceneComponent == nullptr
			|| !IsValid(SceneComponent)
			|| SceneComponent->IsUnreachable()
			|| SceneComponent->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return false;
		}

		FScriptActorSceneComponentAttachmentSnapshot& ComponentSnapshot =
			OutSnapshot.Components.AddDefaulted_GetRef();
		ComponentSnapshot.OldObjectIdentity =
			static_cast<const void*>(SceneComponent);
		ComponentSnapshot.OldOwnerIdentity =
			static_cast<const void*>(SceneComponent->GetOwner());
		ComponentSnapshot.OldParentIdentity =
			static_cast<const void*>(SceneComponent->GetAttachParent());
		ComponentSnapshot.ComponentName = SceneComponent->GetFName();
		ComponentSnapshot.bIsDefaultSubobject = SceneComponent->IsDefaultSubobject();
		ComponentSnapshot.SocketName = SceneComponent->GetAttachSocketName();
		ComponentSnapshot.RelativeTransform = SceneComponent->GetRelativeTransform();
		ComponentSnapshot.bWasRegistered = SceneComponent->IsRegistered();
		ComponentSnapshot.bIsRoot =
			SceneComponent == Actor->GetRootComponent();
		ComponentSnapshot.bOwnerIsActor = SceneComponent->GetOwner() == Actor;
		ComponentSnapshot.bWorldIsActorWorld =
			SceneComponent->GetWorld() == Actor->GetWorld();
		ComponentSnapshot.ChildIdentities.Reserve(
			SceneComponent->GetAttachChildren().Num());
		for (const TObjectPtr<USceneComponent>& Child : SceneComponent->GetAttachChildren())
		{
			if (Child == nullptr)
			{
				return false;
			}
			ComponentSnapshot.ChildIdentities.Add(
				static_cast<const void*>(Child.Get())
			);
		}
		bFoundRoot = bFoundRoot || ComponentSnapshot.bIsRoot;
	}

	return bFoundRoot;
}

UObject* FindReplacementByObjectIdentity(
	const TMap<UObject*, UObject*>& ReinstancedObjects,
	const void* OldObjectIdentity
)
{
	if (OldObjectIdentity == nullptr)
	{
		return nullptr;
	}
	for (const TPair<UObject*, UObject*>& Element : ReinstancedObjects)
	{
		if (static_cast<const void*>(Element.Key) == OldObjectIdentity)
		{
			return Element.Value;
		}
	}
	return nullptr;
}

bool HasReplacementObjectIdentity(
	const TMap<UObject*, UObject*>& ReinstancedObjects,
	const void* OldObjectIdentity
)
{
	if (OldObjectIdentity == nullptr)
	{
		return false;
	}
	for (const TPair<UObject*, UObject*>& Element : ReinstancedObjects)
	{
		if (static_cast<const void*>(Element.Key) == OldObjectIdentity)
		{
			return true;
		}
	}
	return false;
}

bool BuildScriptActorAttachmentRestorePlans(
	const TMap<UObject*, UObject*>& ReinstancedObjects,
	TArray<FScriptActorAttachmentRestorePlan>& OutPlans
)
{
	const auto RejectPlan = [](int32 SourceLine)
	{
		UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning,
			TEXT("SCRIPT_REINSTANCED_ATTACHMENT_PLAN_REJECTED sourceLine=%d"), SourceLine);
		return false;
	};
	OutPlans.Reset();
	TSet<const void*> CallbackOldActorIdentities;
	for (const TPair<UObject*, UObject*>& ReinstancedObject : ReinstancedObjects)
	{
		if (ReinstancedObject.Key != nullptr)
		{
			CallbackOldActorIdentities.Add(
				static_cast<const void*>(ReinstancedObject.Key));
		}
	}
	TArray<FScriptActorAttachmentSnapshot> Snapshots;
	uint64 CaptureGeneration = 0;
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		if (!bRuntimeScriptReplacementCaptureActive
			|| ActiveReloadCaptureGeneration == 0)
		{
			return RejectPlan(__LINE__);
		}
		CaptureGeneration = ActiveReloadCaptureGeneration;
		for (const FScriptActorAttachmentSnapshot& Snapshot :
			ScriptActorAttachmentSnapshots)
		{
			if (!CallbackOldActorIdentities.Contains(Snapshot.OldActorIdentity))
			{
				continue;
			}
			const FReloadCaptureRecord* Capture =
				ReloadCapturesByOldIdentity.Find(Snapshot.OldActorIdentity);
			if (Snapshot.CaptureGeneration != CaptureGeneration
				|| Capture == nullptr
				|| Capture->OldKey != Snapshot.OldActorKey
				|| Capture->OldObjectIdentity != Snapshot.OldActorIdentity
				|| Capture->CaptureGeneration != CaptureGeneration
				|| Capture->State != EReloadCaptureState::Captured)
			{
				return RejectPlan(__LINE__);
			}
			Snapshots.Add(Snapshot);
		}
	}
	if (Snapshots.Num() == 0)
	{
		return true;
	}
	Snapshots.Sort([](const FScriptActorAttachmentSnapshot& Left,
		const FScriptActorAttachmentSnapshot& Right)
	{
		const int32 FullNameOrder = Left.ActorFullName.Compare(Right.ActorFullName);
		if (FullNameOrder != 0)
		{
			return FullNameOrder < 0;
		}
		const int32 NameOrder = Left.ActorName.LexicalLess(Right.ActorName)
			? -1
			: (Right.ActorName.LexicalLess(Left.ActorName) ? 1 : 0);
		if (NameOrder != 0)
		{
			return NameOrder < 0;
		}
		return false;
	});

	// Build one callback-global scene-component map before constructing any
	// plan.  The reinstancing callback is the only authority for cross-Actor
	// parentage; no parent outside this captured batch may be accepted.
	TMap<const void*, USceneComponent*> BatchNewComponentsByOldIdentity;
	TMap<const void*, AActor*> BatchNewComponentActors;
	TSet<const void*> BatchNewActorIdentities;
	TSet<FString> BatchActorFullNames;
	TMap<const void*, const FScriptActorSceneComponentAttachmentSnapshot*>
		BatchSceneSnapshotsByOldIdentity;
	TSet<USceneComponent*> BatchMappedNewComponents;
	TSet<const void*> BatchOldActorIdentities;
	BatchNewComponentsByOldIdentity.Reserve(32);
	BatchNewComponentActors.Reserve(32);
	for (const FScriptActorAttachmentSnapshot& Snapshot : Snapshots)
	{
		if (Snapshot.OldActorIdentity == nullptr
			|| BatchOldActorIdentities.Contains(Snapshot.OldActorIdentity))
		{
			return RejectPlan(__LINE__);
		}
		BatchOldActorIdentities.Add(Snapshot.OldActorIdentity);
		AActor* MappedActor = Cast<AActor>(FindReplacementByObjectIdentity(
			ReinstancedObjects,
			Snapshot.OldActorIdentity));
		if (MappedActor == nullptr || !IsValid(MappedActor)
			|| MappedActor->IsUnreachable())
		{
			return RejectPlan(__LINE__);
		}
		if (BatchNewActorIdentities.Contains(static_cast<const void*>(MappedActor)))
		{
			return RejectPlan(__LINE__);
		}
		if (BatchActorFullNames.Contains(Snapshot.ActorFullName))
		{
			return RejectPlan(__LINE__);
		}
		BatchNewActorIdentities.Add(static_cast<const void*>(MappedActor));
		BatchActorFullNames.Add(Snapshot.ActorFullName);
		for (const FScriptActorSceneComponentAttachmentSnapshot& ComponentSnapshot :
			Snapshot.Components)
		{
			if (ComponentSnapshot.OldObjectIdentity == nullptr
				|| BatchSceneSnapshotsByOldIdentity.Contains(
					ComponentSnapshot.OldObjectIdentity))
			{
				return RejectPlan(__LINE__);
			}
			USceneComponent* MappedComponent = Cast<USceneComponent>(
				FindReplacementByObjectIdentity(
					ReinstancedObjects,
					ComponentSnapshot.OldObjectIdentity));
			// An explicit null replacement retires a removed default component.
			// Keep its snapshot for old-graph validation, but never restore it.
			const bool bRemovedDefault = MappedComponent == nullptr
				&& HasReplacementObjectIdentity(ReinstancedObjects, ComponentSnapshot.OldObjectIdentity)
				&& FindReplacementByObjectIdentity(ReinstancedObjects, ComponentSnapshot.OldObjectIdentity) == nullptr
				&& ComponentSnapshot.bIsDefaultSubobject
				&& FindFProperty<FProperty>(MappedActor->GetClass(), ComponentSnapshot.ComponentName) == nullptr
				&& MappedActor->GetClass()->GetDefaultObject()->GetDefaultSubobjectByName(ComponentSnapshot.ComponentName) == nullptr;
			if (bRemovedDefault)
			{
				BatchSceneSnapshotsByOldIdentity.Add(ComponentSnapshot.OldObjectIdentity, &ComponentSnapshot);
				BatchNewComponentsByOldIdentity.Add(ComponentSnapshot.OldObjectIdentity, nullptr);
				continue;
			}
			if (MappedComponent == nullptr || !IsValid(MappedComponent)
				|| MappedComponent->IsUnreachable()
				|| MappedComponent->GetOwner() != MappedActor
					|| MappedComponent->GetWorld() != MappedActor->GetWorld()
					|| BatchMappedNewComponents.Contains(MappedComponent))
				{
					const bool bMappedLive = MappedComponent != nullptr && IsValid(MappedComponent);
					UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning,
						TEXT("SCRIPT_REINSTANCED_SCENE_MAP_REJECTED actor=%s component=%s root=%d "
							"mapPresent=%d mapped=%d live=%d unreachable=%d owner=%d world=%d duplicate=%d"),
						*Snapshot.ActorName.ToString(), *ComponentSnapshot.ComponentName.ToString(),
						ComponentSnapshot.bIsRoot,
						HasReplacementObjectIdentity(ReinstancedObjects, ComponentSnapshot.OldObjectIdentity),
						MappedComponent != nullptr,
						bMappedLive, bMappedLive && MappedComponent->IsUnreachable(),
						bMappedLive && MappedComponent->GetOwner() == MappedActor,
						bMappedLive && MappedComponent->GetWorld() == MappedActor->GetWorld(),
						BatchMappedNewComponents.Contains(MappedComponent));
					return RejectPlan(__LINE__);
			}
			BatchSceneSnapshotsByOldIdentity.Add(
				ComponentSnapshot.OldObjectIdentity,
				&ComponentSnapshot);
			BatchNewComponentsByOldIdentity.Add(
				ComponentSnapshot.OldObjectIdentity,
				MappedComponent);
			BatchNewComponentActors.Add(
				ComponentSnapshot.OldObjectIdentity,
				MappedActor);
			BatchMappedNewComponents.Add(MappedComponent);
		}
	}
	TMap<const void*, const void*> BatchOldChildParentByIdentity;
	for (const FScriptActorAttachmentSnapshot& Snapshot : Snapshots)
	{
		for (const FScriptActorSceneComponentAttachmentSnapshot& ComponentSnapshot :
			Snapshot.Components)
		{
			for (const void* ChildIdentity : ComponentSnapshot.ChildIdentities)
			{
				if (!BatchSceneSnapshotsByOldIdentity.Contains(ChildIdentity)
					|| BatchOldChildParentByIdentity.Contains(ChildIdentity))
				{
					return RejectPlan(__LINE__);
				}
				BatchOldChildParentByIdentity.Add(
					ChildIdentity,
					ComponentSnapshot.OldObjectIdentity);
			}
		}
	}

	auto IsRetiredChild = [&](USceneComponent* Child, USceneComponent* Parent)
	{
		const void* Identity = static_cast<const void*>(Child);
		return Child == nullptr || (BatchNewComponentsByOldIdentity.Contains(Identity)
			&& BatchNewComponentsByOldIdentity.FindRef(Identity) == nullptr
			&& (!IsValid(Child) || Child->IsBeingDestroyed() || Child->GetAttachParent() != Parent));
	};

	TSet<const void*> SeenNewActorComponents;
	TSet<const void*> SeenNewComponents;
	TSet<const void*> SeenOldActorComponents;
	TSet<USceneComponent*> BatchCurrentChildOwners;
	TSet<USceneComponent*> BatchClosedCurrentChildren;
	for (const FScriptActorAttachmentSnapshot& Snapshot : Snapshots)
	{
		AActor* NewActor = Cast<AActor>(FindReplacementByObjectIdentity(
			ReinstancedObjects,
			Snapshot.OldActorIdentity
		));
		if (NewActor == nullptr
			|| !IsValid(NewActor)
			|| static_cast<const void*>(NewActor) == Snapshot.OldActorIdentity
			|| NewActor->IsUnreachable()
			|| NewActor->HasAnyFlags(
				RF_BeginDestroyed
				| RF_FinishDestroyed
				| RF_ClassDefaultObject
				| RF_ArchetypeObject
				| RF_DefaultSubObject
				)
			|| !BatchNewActorIdentities.Contains(static_cast<const void*>(NewActor)))
		{
			return RejectPlan(__LINE__);
		}

		UWorld* NewWorld = NewActor->GetWorld();
		if ((Snapshot.OldWorldIdentity == nullptr) != (NewWorld == nullptr)
			|| (NewWorld != nullptr
				&& static_cast<const void*>(NewWorld) != Snapshot.OldWorldIdentity))
		{
			return RejectPlan(__LINE__);
		}

		FScriptActorAttachmentRestorePlan& Plan = OutPlans.AddDefaulted_GetRef();
		Plan.NewActor = NewActor;
		Plan.NewRoot = Cast<USceneComponent>(FindReplacementByObjectIdentity(
			ReinstancedObjects,
			Snapshot.OldRootIdentity
		));
		const bool bRootWasRemoved = Snapshot.OldRootIdentity != nullptr
			&& BatchNewComponentsByOldIdentity.Contains(Snapshot.OldRootIdentity)
			&& BatchNewComponentsByOldIdentity.FindRef(Snapshot.OldRootIdentity) == nullptr;
		if (bRootWasRemoved)
		{
			USceneComponent* DefaultRoot = NewActor->GetClass()->GetDefaultObject<AActor>()->GetRootComponent();
			Plan.NewRoot = DefaultRoot ? Cast<USceneComponent>(NewActor->GetDefaultSubobjectByName(DefaultRoot->GetFName())) : nullptr;
			if (!DefaultRoot || !DefaultRoot->IsDefaultSubobject() || !Plan.NewRoot
				|| !BatchMappedNewComponents.Contains(Plan.NewRoot)) return RejectPlan(__LINE__);
		}
		if (Snapshot.OldRootIdentity != nullptr
			&& (Plan.NewRoot == nullptr || !IsValid(Plan.NewRoot)))
		{
			return RejectPlan(__LINE__);
		}

		TMap<const void*, int32> OldComponentIndices;
		TMap<const void*, USceneComponent*> NewComponentsByOldIdentity;
		TSet<USceneComponent*> MappedNewComponents;
		TSet<FName> SeenComponentNames;
		OldComponentIndices.Reserve(Snapshot.Components.Num());
		NewComponentsByOldIdentity.Reserve(Snapshot.Components.Num());
		MappedNewComponents.Reserve(Snapshot.Components.Num());
		TSet<UActorComponent*> CurrentActorComponents;
		for (UActorComponent* ActorComponent : NewActor->GetComponents())
		{
			if (ActorComponent != nullptr)
			{
				CurrentActorComponents.Add(ActorComponent);
			}
		}
		auto LogActorComponentPreflightFailure =
			[&](const FScriptActorOwnedComponentSnapshot& ComponentSnapshot,
				UActorComponent* NewComponent,
				const void* ExpectedComponentClassIdentity,
				bool bComponentMapPresent,
				bool bClassMapPresent,
				bool bClassMapMatchesExpected)
		{
			const bool bNewComponentLive = NewComponent != nullptr
				&& IsValid(NewComponent);
			const bool bNewComponentIdentityReplaced = NewComponent != nullptr
				&& static_cast<const void*>(NewComponent)
					!= ComponentSnapshot.OldObjectIdentity;
			const uint32 NewComponentFlags = bNewComponentLive
				? static_cast<uint32>(NewComponent->GetFlags())
				: 0u;
			const bool bNewComponentUnreachable = bNewComponentLive
				&& NewComponent->IsUnreachable();
			const bool bNewComponentNonScene = bNewComponentLive
				&& Cast<USceneComponent>(NewComponent) == nullptr;
			const bool bNewComponentOwnerMatches = bNewComponentLive
				&& NewComponent->GetOwner() == NewActor;
			const bool bNewComponentMember = bNewComponentLive
				&& CurrentActorComponents.Contains(NewComponent);
			const bool bNewComponentClassMatches = bNewComponentLive
				&& static_cast<const void*>(NewComponent->GetClass())
					== ExpectedComponentClassIdentity;
			const bool bNewComponentNameMatches = bNewComponentLive
				&& NewComponent->GetFName() == ComponentSnapshot.ComponentName;
			const bool bNewComponentDsoMatches = bNewComponentLive
				&& NewComponent->IsDefaultSubobject()
					== ComponentSnapshot.bIsDefaultSubobject;
			const bool bNewComponentDuplicate = bNewComponentLive
				&& SeenNewActorComponents.Contains(
					static_cast<const void*>(NewComponent));
			UE_LOG(
				LogAngelscriptRuntimeGcLifecycle,
				Error,
				TEXT("SCRIPT_REINSTANCED_ACTOR_COMPONENT_PREFLIGHT_DIAGNOSTIC "
					"callbackMapSize=%d componentIdentity=%p componentMapPresent=%d "
					"classIdentity=%p expectedClassIdentity=%p classMapPresent=%d "
					"classMapMatchesExpected=%d newComponentLive=%d "
					"newComponentIdentityReplaced=%d newComponentUnreachable=%d "
					"newComponentFlags=0x%08x newComponentNonScene=%d "
					"newComponentOwnerMatches=%d newComponentMember=%d "
					"newComponentClassMatches=%d newComponentNameMatches=%d "
					"newComponentDsoMatches=%d newComponentDuplicate=%d "
					"generation=%llu snapshotGeneration=%llu componentName=%s"),
				ReinstancedObjects.Num(), ComponentSnapshot.OldObjectIdentity,
				bComponentMapPresent ? 1 : 0,
				ComponentSnapshot.OldClassIdentity,
				ExpectedComponentClassIdentity,
				bClassMapPresent ? 1 : 0,
				bClassMapMatchesExpected ? 1 : 0,
				bNewComponentLive ? 1 : 0,
				bNewComponentIdentityReplaced ? 1 : 0,
				bNewComponentUnreachable ? 1 : 0, NewComponentFlags,
				bNewComponentNonScene ? 1 : 0,
				bNewComponentOwnerMatches ? 1 : 0,
				bNewComponentMember ? 1 : 0,
				bNewComponentClassMatches ? 1 : 0,
				bNewComponentNameMatches ? 1 : 0,
				bNewComponentDsoMatches ? 1 : 0,
				bNewComponentDuplicate ? 1 : 0,
				static_cast<unsigned long long>(CaptureGeneration),
				static_cast<unsigned long long>(Snapshot.CaptureGeneration),
				*ComponentSnapshot.ComponentName.ToString());
		};

		for (const FScriptActorOwnedComponentSnapshot& ComponentSnapshot :
			Snapshot.ActorComponents)
		{
			// This unit covers only script-owned non-scene ActorComponents.
			// Scene and native/unrelated components remain outside this target set.
			if (!ComponentSnapshot.bIsScriptOwnedTarget)
			{
				continue;
			}
			const bool bComponentMapPresent = HasReplacementObjectIdentity(
				ReinstancedObjects,
				ComponentSnapshot.OldObjectIdentity);
			const UObject* MappedComponentClass =
				FindReplacementByObjectIdentity(
					ReinstancedObjects,
					ComponentSnapshot.OldClassIdentity
				);
			const bool bClassMapPresent = HasReplacementObjectIdentity(
				ReinstancedObjects,
				ComponentSnapshot.OldClassIdentity);
			const bool bClassMapMatchesExpected =
				ComponentSnapshot.ExpectedNewClassIdentity != nullptr
				&& (!bClassMapPresent
					|| (MappedComponentClass != nullptr
						&& static_cast<const void*>(MappedComponentClass)
							== ComponentSnapshot.ExpectedNewClassIdentity));
			if (ComponentSnapshot.OldObjectIdentity == nullptr
				|| ComponentSnapshot.OldOwnerIdentity
					!= Snapshot.OldActorIdentity
				|| ComponentSnapshot.OldClassIdentity == nullptr
				|| ComponentSnapshot.ExpectedNewClassIdentity == nullptr
				|| SeenOldActorComponents.Contains(
					ComponentSnapshot.OldObjectIdentity)
				|| SeenComponentNames.Contains(ComponentSnapshot.ComponentName))
			{
				LogActorComponentPreflightFailure(
					ComponentSnapshot,
					nullptr,
					ComponentSnapshot.ExpectedNewClassIdentity,
					bComponentMapPresent, bClassMapPresent,
					bClassMapMatchesExpected);
				return RejectPlan(__LINE__);
			}
			SeenOldActorComponents.Add(ComponentSnapshot.OldObjectIdentity);
			SeenComponentNames.Add(ComponentSnapshot.ComponentName);

			UActorComponent* NewComponent = Cast<UActorComponent>(
				FindReplacementByObjectIdentity(
					ReinstancedObjects,
					ComponentSnapshot.OldObjectIdentity
				)
			);
			const void* ExpectedComponentClassIdentity =
				ComponentSnapshot.ExpectedNewClassIdentity;
			if (NewComponent == nullptr
				|| !IsValid(NewComponent)
				|| static_cast<const void*>(NewComponent)
					== ComponentSnapshot.OldObjectIdentity
				|| NewComponent->IsUnreachable()
				|| NewComponent->HasAnyFlags(
					RF_BeginDestroyed
					| RF_FinishDestroyed
					| RF_ClassDefaultObject
					| RF_ArchetypeObject
					)
				|| Cast<USceneComponent>(NewComponent) != nullptr
				|| NewComponent->GetOwner() != NewActor
				|| !CurrentActorComponents.Contains(NewComponent)
				|| !bClassMapMatchesExpected
				|| static_cast<const void*>(NewComponent->GetClass())
					!= ExpectedComponentClassIdentity
				|| NewComponent->GetFName() != ComponentSnapshot.ComponentName
				|| NewComponent->IsDefaultSubobject()
					!= ComponentSnapshot.bIsDefaultSubobject
				|| SeenNewActorComponents.Contains(
					static_cast<const void*>(NewComponent)))
			{
				LogActorComponentPreflightFailure(
					ComponentSnapshot,
					NewComponent,
					ExpectedComponentClassIdentity,
					bComponentMapPresent, bClassMapPresent,
					bClassMapMatchesExpected);
				return RejectPlan(__LINE__);
			}
			SeenNewActorComponents.Add(static_cast<const void*>(NewComponent));
		}
		for (int32 Index = 0; Index < Snapshot.Components.Num(); ++Index)
		{
			const FScriptActorSceneComponentAttachmentSnapshot& ComponentSnapshot =
				Snapshot.Components[Index];
			if (ComponentSnapshot.bIsRoot)
			{
				if (ComponentSnapshot.OldParentIdentity == nullptr
					&& ComponentSnapshot.SocketName != NAME_None)
				{
					return RejectPlan(__LINE__);
				}
				if (ComponentSnapshot.OldParentIdentity != nullptr)
				{
					const FScriptActorSceneComponentAttachmentSnapshot* ParentSnapshot =
						BatchSceneSnapshotsByOldIdentity.FindRef(
							ComponentSnapshot.OldParentIdentity);
					if (ParentSnapshot == nullptr
						|| ParentSnapshot->OldOwnerIdentity
							== Snapshot.OldActorIdentity)
					{
						return RejectPlan(__LINE__);
					}
				}
			}
			if (ComponentSnapshot.OldObjectIdentity == nullptr
				|| OldComponentIndices.Contains(ComponentSnapshot.OldObjectIdentity)
				|| SeenComponentNames.Contains(ComponentSnapshot.ComponentName)
				|| ComponentSnapshot.OldOwnerIdentity != Snapshot.OldActorIdentity
				|| !ComponentSnapshot.bOwnerIsActor
				|| !ComponentSnapshot.bWorldIsActorWorld)
			{
				return RejectPlan(__LINE__);
			}
			SeenComponentNames.Add(ComponentSnapshot.ComponentName);

			if (BatchNewComponentsByOldIdentity.Contains(ComponentSnapshot.OldObjectIdentity)
				&& BatchNewComponentsByOldIdentity.FindRef(ComponentSnapshot.OldObjectIdentity) == nullptr)
			{
				continue;
			}

			USceneComponent* NewComponent = Cast<USceneComponent>(
				FindReplacementByObjectIdentity(
					ReinstancedObjects,
					ComponentSnapshot.OldObjectIdentity
				)
			);
			if (NewComponent == nullptr
				|| !IsValid(NewComponent)
				|| static_cast<const void*>(NewComponent)
					== ComponentSnapshot.OldObjectIdentity
				|| NewComponent->IsUnreachable()
				|| NewComponent->HasAnyFlags(
					RF_BeginDestroyed
					| RF_FinishDestroyed
					| RF_ClassDefaultObject
					| RF_ArchetypeObject
					)
				|| NewComponent->GetOwner() != NewActor
				|| NewComponent->GetWorld() != NewWorld
				|| SeenNewComponents.Contains(static_cast<const void*>(NewComponent)))
			{
				return RejectPlan(__LINE__);
			}
			TSet<USceneComponent*> CurrentParentChain;
			for (USceneComponent* Current = NewComponent;
				Current != nullptr;
				Current = Current->GetAttachParent())
			{
				if (!IsValid(Current)
					|| Current->IsUnreachable()
					|| Current->HasAnyFlags(
						RF_BeginDestroyed | RF_FinishDestroyed
						| RF_ClassDefaultObject | RF_ArchetypeObject
						)
					|| !BatchMappedNewComponents.Contains(Current)
					|| !BatchNewActorIdentities.Contains(
						static_cast<const void*>(Current->GetOwner()))
					|| Current->GetWorld() != NewWorld
					|| CurrentParentChain.Contains(Current))
				{
					return RejectPlan(__LINE__);
				}
				CurrentParentChain.Add(Current);
			}
			int32 RemovedChildBudget = 0;
			for (const void* ChildIdentity : ComponentSnapshot.ChildIdentities)
			{
				RemovedChildBudget += BatchNewComponentsByOldIdentity.Contains(ChildIdentity)
					&& BatchNewComponentsByOldIdentity.FindRef(ChildIdentity) == nullptr ? 1 : 0;
			}
			TSet<USceneComponent*> CurrentChildren;
			for (const TObjectPtr<USceneComponent>& Child :
				NewComponent->GetAttachChildren())
			{
				USceneComponent* ChildObject = Child.Get();
				if (IsRetiredChild(ChildObject, NewComponent))
				{
					// Reference replacement leaves null slots in AttachChildren.
					// Accept only as many retired slots as this captured parent lost.
					if (--RemovedChildBudget < 0) return RejectPlan(__LINE__);
					continue;
				}
				if (ChildObject == nullptr
					|| !IsValid(ChildObject)
					|| ChildObject->IsUnreachable()
					|| ChildObject->HasAnyFlags(
						RF_BeginDestroyed | RF_FinishDestroyed
						| RF_ClassDefaultObject | RF_ArchetypeObject
						)
					|| !BatchNewActorIdentities.Contains(
						static_cast<const void*>(ChildObject->GetOwner()))
					|| ChildObject->GetWorld() != NewWorld
					|| CurrentChildren.Contains(ChildObject)
					|| !BatchMappedNewComponents.Contains(ChildObject)
					|| ChildObject->GetAttachParent() != NewComponent
					|| BatchCurrentChildOwners.Contains(ChildObject))
				{
					return RejectPlan(__LINE__);
				}
				CurrentChildren.Add(ChildObject);
				BatchCurrentChildOwners.Add(ChildObject);
			}

			OldComponentIndices.Add(ComponentSnapshot.OldObjectIdentity, Index);
			NewComponentsByOldIdentity.Add(
				ComponentSnapshot.OldObjectIdentity,
				NewComponent
			);
			MappedNewComponents.Add(NewComponent);
			SeenNewComponents.Add(static_cast<const void*>(NewComponent));
		}

		if (Snapshot.OldRootIdentity != nullptr && !bRootWasRemoved
			&& (!OldComponentIndices.Contains(Snapshot.OldRootIdentity)
				|| Plan.NewRoot
					!= NewComponentsByOldIdentity.FindRef(Snapshot.OldRootIdentity)))
		{
			return RejectPlan(__LINE__);
		}
		Plan.OriginalRoot = NewActor->GetRootComponent();
		if (Plan.OriginalRoot != nullptr
			&& (!IsValid(Plan.OriginalRoot)
				|| Plan.OriginalRoot->IsUnreachable()
				|| Plan.OriginalRoot->HasAnyFlags(
					RF_BeginDestroyed | RF_FinishDestroyed
					| RF_ClassDefaultObject | RF_ArchetypeObject
					)
				|| Plan.OriginalRoot->GetOwner() != NewActor
				|| Plan.OriginalRoot->GetWorld() != NewWorld
				|| !MappedNewComponents.Contains(Plan.OriginalRoot)))
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning,
				TEXT("SCRIPT_REINSTANCED_PAIR_PREFLIGHT_DETAIL stage=ActorRoot actorClass=%s "
					"oldRoot=%d newRoot=%s capturedScene=%d mappedScene=%d"),
				*NewActor->GetClass()->GetPathName(), Snapshot.OldRootIdentity != nullptr,
				*GetNameSafe(Plan.OriginalRoot), Snapshot.Components.Num(), MappedNewComponents.Num());
			return RejectPlan(__LINE__);
		}
		for (UActorComponent* ActorComponent : NewActor->GetComponents())
		{
			if (USceneComponent* SceneComponent =
				Cast<USceneComponent>(ActorComponent))
			{
				if (!MappedNewComponents.Contains(SceneComponent))
				{
					return RejectPlan(__LINE__);
				}
			}
		}
		for (USceneComponent* NewComponent : MappedNewComponents)
		{
			for (USceneComponent* Current = NewComponent->GetAttachParent();
				Current != nullptr;
				Current = Current->GetAttachParent())
			{
				if (!BatchMappedNewComponents.Contains(Current))
				{
					return RejectPlan(__LINE__);
				}
			}
			for (const TObjectPtr<USceneComponent>& Child :
				NewComponent->GetAttachChildren())
			{
				USceneComponent* ChildObject = Child.Get();
				if (IsRetiredChild(ChildObject, NewComponent)) continue;
				if (!BatchMappedNewComponents.Contains(ChildObject)
					|| BatchClosedCurrentChildren.Contains(ChildObject))
				{
					return RejectPlan(__LINE__);
				}
				BatchClosedCurrentChildren.Add(ChildObject);
			}
		}
		for (const FScriptActorSceneComponentAttachmentSnapshot& ComponentSnapshot :
			Snapshot.Components)
		{
			for (const void* ChildIdentity : ComponentSnapshot.ChildIdentities)
			{
				const FScriptActorSceneComponentAttachmentSnapshot* ChildSnapshot =
					BatchSceneSnapshotsByOldIdentity.FindRef(ChildIdentity);
				if (ChildSnapshot == nullptr
					|| BatchOldChildParentByIdentity.FindRef(ChildIdentity)
						!= ComponentSnapshot.OldObjectIdentity
					|| ChildSnapshot->OldParentIdentity
						!= ComponentSnapshot.OldObjectIdentity)
				{
					return RejectPlan(__LINE__);
				}
			}
		}
		for (const FScriptActorSceneComponentAttachmentSnapshot& ComponentSnapshot :
			Snapshot.Components)
		{
			// The public API cannot reliably reproduce an unregistered attachment
			// whose child-side parent pointer lacks parent-side membership.
			const void* ListedParentIdentity =
				BatchOldChildParentByIdentity.FindRef(
					ComponentSnapshot.OldObjectIdentity);
			if (ListedParentIdentity != ComponentSnapshot.OldParentIdentity)
			{
				return RejectPlan(__LINE__);
			}
			if (ComponentSnapshot.bWasRegistered
				&& ComponentSnapshot.OldParentIdentity != nullptr)
			{
				const FScriptActorSceneComponentAttachmentSnapshot* ParentSnapshot =
					BatchSceneSnapshotsByOldIdentity.FindRef(
						ComponentSnapshot.OldParentIdentity);
				if (ParentSnapshot == nullptr || !ParentSnapshot->bWasRegistered)
				{
					return RejectPlan(__LINE__);
				}
			}
		}

		Plan.Components.Reserve(Snapshot.Components.Num());
		for (const FScriptActorSceneComponentAttachmentSnapshot& ComponentSnapshot :
			Snapshot.Components)
		{
			if (BatchNewComponentsByOldIdentity.FindRef(ComponentSnapshot.OldObjectIdentity) == nullptr)
			{
				continue;
			}
			const bool bParentWasRemoved = ComponentSnapshot.OldParentIdentity != nullptr
				&& BatchNewComponentsByOldIdentity.Contains(ComponentSnapshot.OldParentIdentity)
				&& BatchNewComponentsByOldIdentity.FindRef(ComponentSnapshot.OldParentIdentity) == nullptr;
			USceneComponent* NewParent = nullptr;
			if (ComponentSnapshot.OldParentIdentity != nullptr)
			{
				if (!BatchNewComponentsByOldIdentity.Contains(
						ComponentSnapshot.OldParentIdentity))
				{
					// Parentage is accepted only when both endpoints were captured
					// by this callback and mapped in the batch-global table.
					return RejectPlan(__LINE__);
				}
				NewParent = BatchNewComponentsByOldIdentity.FindRef(
						ComponentSnapshot.OldParentIdentity
					);
				if (bParentWasRemoved)
				{
					// Haze reattaches surviving defaults according to their new template.
					auto* Template = Cast<USceneComponent>(NewComponentsByOldIdentity.FindRef(
						ComponentSnapshot.OldObjectIdentity)->GetArchetype());
					if (!Template || !Template->IsTemplate()) return RejectPlan(__LINE__);
					USceneComponent* TemplateParent = Template->GetAttachParent();
					NewParent = TemplateParent ? Cast<USceneComponent>(NewActor->GetDefaultSubobjectByName(
						TemplateParent->GetFName())) : nullptr;
					if (TemplateParent && (!NewParent || !BatchMappedNewComponents.Contains(NewParent)))
						return RejectPlan(__LINE__);
				}
				if (!bParentWasRemoved && (NewParent == nullptr
						|| NewParent == BatchNewComponentsByOldIdentity.FindRef(
							ComponentSnapshot.OldObjectIdentity)
						|| BatchNewComponentActors.FindRef(
							ComponentSnapshot.OldParentIdentity) == nullptr
						|| !BatchNewActorIdentities.Contains(static_cast<const void*>(
							NewParent->GetOwner()))
						|| NewParent->GetWorld() != NewActor->GetWorld()))
				{
					return RejectPlan(__LINE__);
				}
			}

			FScriptActorSceneComponentAttachmentRestore& Restore =
				Plan.Components.AddDefaulted_GetRef();
			Restore.NewComponent = NewComponentsByOldIdentity.FindRef(
				ComponentSnapshot.OldObjectIdentity
			);
			Restore.NewParent = NewParent;
			Restore.bParentWasRemoved = bParentWasRemoved;
			Restore.SocketName = ComponentSnapshot.SocketName;
			Restore.RelativeTransform = ComponentSnapshot.RelativeTransform;
			Restore.bWasRegistered = ComponentSnapshot.bWasRegistered;
			Restore.bWasListedByParent = NewParent != nullptr && (bParentWasRemoved || (
				ComponentSnapshot.OldParentIdentity != nullptr
				&& BatchOldChildParentByIdentity.FindRef(
					ComponentSnapshot.OldObjectIdentity
				) == ComponentSnapshot.OldParentIdentity));
			if (ComponentSnapshot.OldParentIdentity != nullptr && !bParentWasRemoved)
			{
				const FScriptActorSceneComponentAttachmentSnapshot* ParentSnapshot =
					BatchSceneSnapshotsByOldIdentity.FindRef(
						ComponentSnapshot.OldParentIdentity);
				if (ParentSnapshot == nullptr)
				{
					return RejectPlan(__LINE__);
				}
				for (int32 ChildIndex = 0;
					ChildIndex < ParentSnapshot->ChildIdentities.Num();
					++ChildIndex)
				{
					if (ParentSnapshot->ChildIdentities[ChildIndex]
						== ComponentSnapshot.OldObjectIdentity)
					{
						Restore.TargetSiblingIndex = 0;
						for (int32 SiblingIndex = 0; SiblingIndex < ChildIndex; ++SiblingIndex)
						{
							Restore.TargetSiblingIndex += BatchNewComponentsByOldIdentity.FindRef(
								ParentSnapshot->ChildIdentities[SiblingIndex]) != nullptr ? 1 : 0;
						}
						break;
					}
				}
				if (Restore.TargetSiblingIndex == INDEX_NONE)
				{
					return RejectPlan(__LINE__);
				}
			}
			Restore.bIsRoot = bRootWasRemoved ? Restore.NewComponent == Plan.NewRoot : ComponentSnapshot.bIsRoot;
			if (bRootWasRemoved && Restore.bIsRoot)
			{
				// Native replacement places the new root at the Actor's world transform.
				Restore.RelativeTransform = Restore.NewComponent->GetRelativeTransform();
				Restore.SocketName = NAME_None;
			}
			TSet<const void*> AttachmentChain;
			const void* AttachmentIdentity = ComponentSnapshot.OldObjectIdentity;
			while (AttachmentIdentity != nullptr)
			{
				if (AttachmentChain.Contains(AttachmentIdentity))
				{
					return RejectPlan(__LINE__);
				}
				AttachmentChain.Add(AttachmentIdentity);
				const FScriptActorSceneComponentAttachmentSnapshot* CurrentSnapshot =
					BatchSceneSnapshotsByOldIdentity.FindRef(AttachmentIdentity);
				if (CurrentSnapshot == nullptr)
				{
					return RejectPlan(__LINE__);
				}
				if (AttachmentIdentity != ComponentSnapshot.OldObjectIdentity)
				{
					++Restore.AttachmentDepth;
				}
				AttachmentIdentity = CurrentSnapshot->OldParentIdentity;
			}
			Restore.OriginalParent = Restore.NewComponent->GetAttachParent();
			if (Restore.OriginalParent != nullptr
				&& (!IsValid(Restore.OriginalParent)
					|| Restore.OriginalParent->IsUnreachable()
					|| Restore.OriginalParent->HasAnyFlags(
						RF_BeginDestroyed | RF_FinishDestroyed
						| RF_ClassDefaultObject | RF_ArchetypeObject
						)
						|| !BatchNewActorIdentities.Contains(static_cast<const void*>(
							Restore.OriginalParent->GetOwner()))
					|| Restore.OriginalParent->GetWorld() != NewWorld))
			{
				return RejectPlan(__LINE__);
			}
			Restore.OriginalSocketName =
				Restore.NewComponent->GetAttachSocketName();
			Restore.OriginalRelativeTransform =
				Restore.NewComponent->GetRelativeTransform();
			Restore.bWasOriginallyRegistered = Restore.NewComponent->IsRegistered();
			if (Restore.OriginalParent != nullptr)
			{
				int32 OriginalChildIndex = 0;
				for (const TObjectPtr<USceneComponent>& OriginalSibling :
					Restore.OriginalParent->GetAttachChildren())
				{
					if (OriginalSibling.Get() == Restore.NewComponent)
					{
						Restore.bWasOriginallyListedByParent = true;
						Restore.OriginalSiblingIndex = OriginalChildIndex;
						break;
					}
					++OriginalChildIndex;
				}
			}
			if (Restore.bWasOriginallyRegistered
				&& Restore.OriginalParent != nullptr
				&& (!Restore.bWasOriginallyListedByParent
					|| !Restore.OriginalParent->IsRegistered()))
			{
				return RejectPlan(__LINE__);
			}
			if (!Restore.bWasOriginallyRegistered
				&& Restore.OriginalParent != nullptr
				&& !Restore.bWasOriginallyListedByParent)
			{
				return RejectPlan(__LINE__);
			}
			Restore.ExpectedChildren.Reserve(ComponentSnapshot.ChildIdentities.Num());
			for (const void* ChildIdentity : ComponentSnapshot.ChildIdentities)
			{
				USceneComponent* NewChild =
					BatchNewComponentsByOldIdentity.FindRef(ChildIdentity);
				if (NewChild == nullptr)
				{
					if (BatchNewComponentsByOldIdentity.Contains(ChildIdentity)) continue;
					return RejectPlan(__LINE__);
				}
				Restore.ExpectedChildren.Add(NewChild);
			}
			for (const TObjectPtr<USceneComponent>& Child :
				Restore.NewComponent->GetAttachChildren())
			{
				if (IsRetiredChild(Child.Get(), Restore.NewComponent))
				{
					Restore.bNormalizeRetiredChildren = true;
					continue;
				}
				if (Child != nullptr)
				{
					Restore.OriginalChildren.Add(Child.Get());
				}
			}
		}

		for (int32 Index = 0; Index < Snapshot.Components.Num(); ++Index)
		{
			const void* CurrentIdentity =
				Snapshot.Components[Index].OldObjectIdentity;
			TSet<const void*> SeenInChain;
			while (CurrentIdentity != nullptr)
			{
				if (SeenInChain.Contains(CurrentIdentity))
				{
					return RejectPlan(__LINE__);
				}
				SeenInChain.Add(CurrentIdentity);
				const FScriptActorSceneComponentAttachmentSnapshot* CurrentSnapshot =
					BatchSceneSnapshotsByOldIdentity.FindRef(CurrentIdentity);
				if (CurrentSnapshot == nullptr)
				{
					return RejectPlan(__LINE__);
				}
				CurrentIdentity = CurrentSnapshot->OldParentIdentity;
			}
		}
	}
	for (FScriptActorAttachmentRestorePlan& Plan : OutPlans)
		for (FScriptActorSceneComponentAttachmentRestore& Restore : Plan.Components)
			if (Restore.bParentWasRemoved && Restore.NewParent)
			{
				bool bFoundParent = false;
				for (FScriptActorAttachmentRestorePlan& ParentPlan : OutPlans)
					for (FScriptActorSceneComponentAttachmentRestore& Parent : ParentPlan.Components)
						if (Parent.NewComponent == Restore.NewParent)
						{
							Restore.TargetSiblingIndex = Parent.ExpectedChildren.Num();
							Parent.ExpectedChildren.Add(Restore.NewComponent);
							bFoundParent = true;
						}
				if (!bFoundParent) return RejectPlan(__LINE__);
			}
	for (FScriptActorAttachmentRestorePlan& Plan : OutPlans)
	{
		for (FScriptActorSceneComponentAttachmentRestore& Restore : Plan.Components)
		{
			TSet<USceneComponent*> OriginalChain;
			for (USceneComponent* Current = Restore.NewComponent;
				Current != nullptr && Current->GetAttachParent() != nullptr;
				Current = Current->GetAttachParent())
			{
				USceneComponent* Parent = Current->GetAttachParent();
				if (OriginalChain.Contains(Parent)
					|| !BatchMappedNewComponents.Contains(Parent)
					|| !BatchNewActorIdentities.Contains(static_cast<const void*>(
						Parent->GetOwner()))
					|| Parent->GetWorld() != Restore.NewComponent->GetWorld())
				{
					return RejectPlan(__LINE__);
				}
				OriginalChain.Add(Parent);
				++Restore.OriginalAttachmentDepth;
			}
		}
	}
	return true;
}

bool HasExactScriptActorAttachmentChildren(
	USceneComponent* Component,
	const TArray<USceneComponent*>& ExpectedChildren
)
{
	if (Component == nullptr
		|| Component->GetAttachChildren().Num() != ExpectedChildren.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < ExpectedChildren.Num(); ++Index)
	{
		USceneComponent* ChildObject = Component->GetAttachChildren()[Index].Get();
		if (ChildObject == nullptr || ChildObject != ExpectedChildren[Index])
		{
			return false;
		}
	}
	return true;
}

bool ApplyScriptActorAttachmentComponentState(
	const FScriptActorSceneComponentAttachmentRestore& Restore,
	USceneComponent* Parent,
	FName SocketName,
	const FTransform& RelativeTransform,
	bool bShouldBeRegistered,
	bool bShouldBeListedByParent
)
{
	if (Restore.NewComponent == nullptr
		|| (Parent == nullptr && bShouldBeListedByParent)
		|| (bShouldBeRegistered && Parent != nullptr
			&& (!bShouldBeListedByParent || !Parent->IsRegistered()))
		|| (!bShouldBeRegistered && Parent != nullptr
			&& !bShouldBeListedByParent))
	{
		return false;
	}
	if (!bShouldBeRegistered && Restore.NewComponent->IsRegistered())
	{
		Restore.NewComponent->UnregisterComponent();
	}
	if (Parent != nullptr
		&& (bShouldBeRegistered || bShouldBeListedByParent))
	{
		if (!Restore.NewComponent->AttachToComponent(
			Parent,
			FAttachmentTransformRules::KeepRelativeTransform,
			SocketName))
		{
			return false;
		}
	}
	else if (!Restore.NewComponent->IsRegistered())
	{
		Restore.NewComponent->SetupAttachment(Parent, SocketName);
	}
	else if (Restore.NewComponent->GetAttachParent() != nullptr)
	{
		Restore.NewComponent->DetachFromComponent(
			FDetachmentTransformRules::KeepRelativeTransform);
	}
	Restore.NewComponent->SetRelativeTransform(RelativeTransform);
	if (bShouldBeRegistered && !Restore.NewComponent->IsRegistered())
	{
		Restore.NewComponent->RegisterComponent();
	}
	bool bIsListedByParent = false;
	if (Parent != nullptr)
	{
		for (const TObjectPtr<USceneComponent>& Child : Parent->GetAttachChildren())
		{
			if (Child.Get() == Restore.NewComponent)
			{
				bIsListedByParent = true;
				break;
			}
		}
	}
	return Restore.NewComponent->IsRegistered() == bShouldBeRegistered
		&& Restore.NewComponent->GetAttachParent() == Parent
		&& Restore.NewComponent->GetAttachSocketName() == SocketName
		&& bIsListedByParent == bShouldBeListedByParent
		&& Restore.NewComponent->GetRelativeTransform().Equals(RelativeTransform);
}

bool VerifyScriptActorAttachmentPlanState(
	const TArray<FScriptActorAttachmentRestorePlan>& Plans,
	bool bOriginalState
)
{
	for (const FScriptActorAttachmentRestorePlan& Plan : Plans)
	{
		USceneComponent* ExpectedRoot =
			bOriginalState ? Plan.OriginalRoot : Plan.NewRoot;
		if (Plan.NewActor == nullptr
			|| Plan.NewActor->GetRootComponent() != ExpectedRoot)
		{
			return false;
		}
		for (const FScriptActorSceneComponentAttachmentRestore& Restore :
			Plan.Components)
		{
			USceneComponent* ExpectedParent = bOriginalState
				? Restore.OriginalParent
				: Restore.NewParent;
			const FName ExpectedSocket = bOriginalState
				? Restore.OriginalSocketName
				: Restore.SocketName;
			const FTransform& ExpectedTransform = bOriginalState
				? Restore.OriginalRelativeTransform
				: Restore.RelativeTransform;
			const bool bExpectedRegistered = bOriginalState
				? Restore.bWasOriginallyRegistered
				: Restore.bWasRegistered;
			const TArray<USceneComponent*>& ExpectedChildren = bOriginalState
				? Restore.OriginalChildren
				: Restore.ExpectedChildren;
			if (Restore.NewComponent == nullptr
				|| Restore.NewComponent->GetOwner() != Plan.NewActor
				|| Restore.NewComponent->IsRegistered() != bExpectedRegistered
				|| Restore.NewComponent->GetAttachParent() != ExpectedParent
				|| Restore.NewComponent->GetAttachSocketName() != ExpectedSocket
				|| !Restore.NewComponent->GetRelativeTransform().Equals(
					ExpectedTransform)
				|| !HasExactScriptActorAttachmentChildren(
					Restore.NewComponent,
					ExpectedChildren))
			{
				return false;
			}
		}
	}
	return true;
}

bool RestoreScriptActorAttachmentPlans(
	const TArray<FScriptActorAttachmentRestorePlan>& Plans
)
{
	TArray<const FScriptActorSceneComponentAttachmentRestore*> OrderedRestores;
	for (const FScriptActorAttachmentRestorePlan& Plan : Plans)
	{
		if (Plan.NewActor == nullptr || !IsValid(Plan.NewActor))
		{
			return false;
		}
		const FScriptActorSceneComponentAttachmentRestore* RootRestore = nullptr;
		for (const FScriptActorSceneComponentAttachmentRestore& Restore : Plan.Components)
		{
			if (Restore.NewComponent == nullptr
				|| !IsValid(Restore.NewComponent)
				|| Restore.NewComponent->GetOwner() != Plan.NewActor
				|| (Restore.NewParent != nullptr
					&& (Restore.NewParent == Restore.NewComponent
						|| !IsValid(Restore.NewParent)
						|| Restore.NewParent->GetWorld()
							!= Restore.NewComponent->GetWorld()))
				|| (!Restore.bWasRegistered
					&& Restore.NewParent != nullptr
					&& !Restore.bWasListedByParent))
			{
				return false;
			}
			// Retired edges are outside the live graph and must not be restored on rollback.
			if (Restore.bNormalizeRetiredChildren
				&& !SoloASBridge::NormalizeSceneComponentChildren(Restore.NewComponent)) return false;
			OrderedRestores.Add(&Restore);
			if (Restore.bIsRoot)
			{
				if (RootRestore != nullptr
					|| (Restore.NewParent != nullptr
						&& Restore.NewParent->GetOwner() == Plan.NewActor))
				{
					return false;
				}
				RootRestore = &Restore;
			}
		}
		if ((Plan.NewRoot == nullptr) != (RootRestore == nullptr)
			|| (Plan.NewRoot != nullptr && RootRestore->NewComponent != Plan.NewRoot))
		{
			return false;
		}
	}
	OrderedRestores.Sort([](
		const FScriptActorSceneComponentAttachmentRestore& Left,
		const FScriptActorSceneComponentAttachmentRestore& Right)
	{
		if (Left.AttachmentDepth != Right.AttachmentDepth)
		{
			return Left.AttachmentDepth < Right.AttachmentDepth;
		}
		const FString LeftParent = Left.NewParent != nullptr
			? Left.NewParent->GetFullName()
			: FString();
		const FString RightParent = Right.NewParent != nullptr
			? Right.NewParent->GetFullName()
			: FString();
		const int32 ParentComparison = LeftParent.Compare(RightParent);
		if (ParentComparison != 0)
		{
			return ParentComparison < 0;
		}
		if (Left.TargetSiblingIndex != Right.TargetSiblingIndex)
		{
			return Left.TargetSiblingIndex < Right.TargetSiblingIndex;
		}
		const FString LeftOwner = Left.NewComponent->GetOwner()->GetFullName();
		const FString RightOwner = Right.NewComponent->GetOwner()->GetFullName();
		const int32 OwnerComparison = LeftOwner.Compare(RightOwner);
		if (OwnerComparison != 0)
		{
			return OwnerComparison < 0;
		}
		return Left.NewComponent->GetFullName().Compare(
			Right.NewComponent->GetFullName()) < 0;
	});

	// Detach/unregister the complete batch before attaching any new parent.
	for (int32 Index = OrderedRestores.Num() - 1; Index >= 0; --Index)
	{
		const FScriptActorSceneComponentAttachmentRestore& Restore =
			*OrderedRestores[Index];
		if (!Restore.bWasRegistered && Restore.NewComponent->IsRegistered())
		{
			Restore.NewComponent->UnregisterComponent();
		}
		if (Restore.NewComponent->GetAttachParent() != nullptr)
		{
			Restore.NewComponent->DetachFromComponent(
				FDetachmentTransformRules::KeepRelativeTransform);
		}
		else if (!Restore.NewComponent->IsRegistered())
		{
			Restore.NewComponent->SetupAttachment(nullptr);
		}
	}

	for (const FScriptActorAttachmentRestorePlan& Plan : Plans)
	{
		if (Plan.NewRoot != nullptr)
		{
			const FScriptActorSceneComponentAttachmentRestore* RootRestore = nullptr;
			for (const FScriptActorSceneComponentAttachmentRestore& Restore : Plan.Components)
			{
				if (Restore.bIsRoot)
				{
					RootRestore = &Restore;
					break;
				}
			}
			if (RootRestore == nullptr)
			{
				return false;
			}
			Plan.NewRoot->SetRelativeTransform(RootRestore->RelativeTransform);
			if (!Plan.NewActor->SetRootComponent(Plan.NewRoot))
			{
				return false;
			}
		}
		else
		{
			if (!Plan.NewActor->SetRootComponent(nullptr))
			{
				return false;
			}
		}
	}
	for (const FScriptActorSceneComponentAttachmentRestore* Restore : OrderedRestores)
	{
		if (!ApplyScriptActorAttachmentComponentState(
				*Restore,
				Restore->NewParent,
				Restore->SocketName,
				Restore->RelativeTransform,
				Restore->bWasRegistered,
				Restore->bWasListedByParent))
		{
			return false;
		}
	}
	return VerifyScriptActorAttachmentPlanState(Plans, false);
}

bool RollbackScriptActorAttachmentPlans(
	const TArray<FScriptActorAttachmentRestorePlan>& Plans
)
{
	TArray<const FScriptActorSceneComponentAttachmentRestore*> OrderedRestores;
	bool bRollbackSucceeded = true;
	for (const FScriptActorAttachmentRestorePlan& Plan : Plans)
	{
		if (Plan.NewActor == nullptr || !IsValid(Plan.NewActor))
		{
			return false;
		}
		const FScriptActorSceneComponentAttachmentRestore* RootRestore = nullptr;
		bool bOriginalRootFound = Plan.OriginalRoot == nullptr;
		for (const FScriptActorSceneComponentAttachmentRestore& Restore : Plan.Components)
		{
			if (Restore.NewComponent == nullptr
				|| !IsValid(Restore.NewComponent)
				|| Restore.NewComponent->GetOwner() != Plan.NewActor
				|| (Restore.OriginalParent != nullptr
					&& (Restore.OriginalParent == Restore.NewComponent
						|| !IsValid(Restore.OriginalParent)
						|| Restore.OriginalParent->GetWorld()
							!= Restore.NewComponent->GetWorld()))
				|| (!Restore.bWasOriginallyRegistered
					&& Restore.OriginalParent != nullptr
					&& !Restore.bWasOriginallyListedByParent))
			{
				return false;
			}
			OrderedRestores.Add(&Restore);
			if (Restore.bIsRoot)
			{
				if (RootRestore != nullptr)
				{
					return false;
				}
				RootRestore = &Restore;
			}
			if (Restore.NewComponent == Plan.OriginalRoot)
			{
				if (bOriginalRootFound)
				{
					return false;
				}
				bOriginalRootFound = true;
			}
		}
		if (!bOriginalRootFound)
		{
			return false;
		}
	}
	OrderedRestores.Sort([](
		const FScriptActorSceneComponentAttachmentRestore& Left,
		const FScriptActorSceneComponentAttachmentRestore& Right)
	{
		if (Left.OriginalAttachmentDepth != Right.OriginalAttachmentDepth)
		{
			return Left.OriginalAttachmentDepth < Right.OriginalAttachmentDepth;
		}
		const FString LeftParent = Left.OriginalParent != nullptr
			? Left.OriginalParent->GetFullName()
			: FString();
		const FString RightParent = Right.OriginalParent != nullptr
			? Right.OriginalParent->GetFullName()
			: FString();
		const int32 ParentComparison = LeftParent.Compare(RightParent);
		if (ParentComparison != 0)
		{
			return ParentComparison < 0;
		}
		if (Left.OriginalSiblingIndex != Right.OriginalSiblingIndex)
		{
			return Left.OriginalSiblingIndex < Right.OriginalSiblingIndex;
		}
		const FString LeftOwner = Left.NewComponent->GetOwner()->GetFullName();
		const FString RightOwner = Right.NewComponent->GetOwner()->GetFullName();
		const int32 OwnerComparison = LeftOwner.Compare(RightOwner);
		if (OwnerComparison != 0)
		{
			return OwnerComparison < 0;
		}
		return Left.NewComponent->GetFullName().Compare(
			Right.NewComponent->GetFullName()) < 0;
	});

	for (int32 Index = OrderedRestores.Num() - 1; Index >= 0; --Index)
	{
		const FScriptActorSceneComponentAttachmentRestore& Restore =
			*OrderedRestores[Index];
		if (!Restore.bWasOriginallyRegistered && Restore.NewComponent->IsRegistered())
		{
			Restore.NewComponent->UnregisterComponent();
		}
		if (Restore.NewComponent->GetAttachParent() != nullptr)
		{
			Restore.NewComponent->DetachFromComponent(
				FDetachmentTransformRules::KeepRelativeTransform);
		}
		else if (!Restore.NewComponent->IsRegistered())
		{
			Restore.NewComponent->SetupAttachment(nullptr);
		}
	}
	for (const FScriptActorAttachmentRestorePlan& Plan : Plans)
	{
		bRollbackSucceeded = Plan.NewActor->SetRootComponent(Plan.OriginalRoot)
			&& bRollbackSucceeded;
	}
	for (const FScriptActorSceneComponentAttachmentRestore* Restore : OrderedRestores)
	{
		bRollbackSucceeded = ApplyScriptActorAttachmentComponentState(
				*Restore,
				Restore->OriginalParent,
				Restore->OriginalSocketName,
				Restore->OriginalRelativeTransform,
				Restore->bWasOriginallyRegistered,
				Restore->bWasOriginallyListedByParent)
			&& bRollbackSucceeded;
	}
	return VerifyScriptActorAttachmentPlanState(Plans, true)
		&& bRollbackSucceeded;
}

void RemoveProcessedScriptActorAttachmentSnapshots(
	const TMap<UObject*, UObject*>& ReinstancedObjects
)
{
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	for (int32 Index = ScriptActorAttachmentSnapshots.Num() - 1;
		Index >= 0;
		--Index)
	{
		const void* SnapshotIdentity =
			ScriptActorAttachmentSnapshots[Index].OldActorIdentity;
		bool bPresentInCallback = false;
		for (const TPair<UObject*, UObject*>& ReinstancedObject :
			ReinstancedObjects)
		{
			if (ReinstancedObject.Key != nullptr
				&& static_cast<const void*>(ReinstancedObject.Key)
					== SnapshotIdentity)
			{
				bPresentInCallback = true;
				break;
			}
		}
		if (bPresentInCallback)
		{
			ScriptActorAttachmentSnapshots.RemoveAt(
				Index,
				1,
				EAllowShrinking::No);
		}
	}
}
}

bool RestoreScriptRuntimeClassReferenceState(
	UObject* NewObject,
	UASClass* NewClass,
	asITypeInfo* NewTypeCookie,
	const TArray<FReinstancedScriptClassReferenceSnapshot>& Snapshots)
{
	TArray<FClassReferenceRestorePlan> Plans;
	if (!BuildRuntimeClassReferenceRestorePlan(
			NewObject,
			NewClass,
			NewTypeCookie,
			Snapshots,
			true,
			Plans))
	{
		return false;
	}

	for (int32 Index = 0; Index < Plans.Num(); ++Index)
	{
		const FClassReferenceRestorePlan& Plan = Plans[Index];
		Plan.Property->SetObjectPropertyValue_InContainer(
			NewObject,
			Plan.ExpectedValue);
		if (Cast<UClass>(
				Plan.Property->GetObjectPropertyValue_InContainer(NewObject))
			!= Plan.ExpectedValue)
		{
			bool bRollbackVerified = true;
			for (int32 RollbackIndex = Index; RollbackIndex >= 0; --RollbackIndex)
			{
				const FClassReferenceRestorePlan& Rollback = Plans[RollbackIndex];
				Rollback.Property->SetObjectPropertyValue_InContainer(
					NewObject,
					Rollback.OriginalValue);
				bRollbackVerified = bRollbackVerified
					&& Cast<UClass>(
						Rollback.Property->GetObjectPropertyValue_InContainer(NewObject))
						== Rollback.OriginalValue;
			}
			if (!bRollbackVerified)
			{
				return false;
			}
			return false;
		}
	}
	return true;
}

bool GetReinstancedScriptPairReplayToken(
	UObject* LiveNewObject,
	FReinstancedScriptPairReplayToken& OutToken
)
{
	if (LiveNewObject == nullptr)
	{
		return false;
	}
	const FObjectKey NewKey(LiveNewObject);
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	const FConsumedReinstancedScriptClassPair* Record =
		ReinstancedScriptPairsByNewObject.Find(NewKey);
	if (Record == nullptr
		|| Record->NewObjectIdentity != static_cast<const void*>(LiveNewObject))
	{
		return false;
	}
	OutToken.OldKey = Record->OldKey;
	OutToken.NewKey = Record->NewKey;
	OutToken.CaptureGeneration = Record->CaptureGeneration;
	OutToken.NewTrackSequence = Record->NewTrackSequence;
	return true;
}

EReinstancedScriptPairReplayResult ReplayReinstancedScriptPairToken(
	const FReinstancedScriptPairReplayToken& Token
)
{
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	const FConsumedReinstancedScriptClassPair* Record =
		ReinstancedScriptPairsByNewObject.Find(Token.NewKey);
	if (Record == nullptr)
	{
		return EReinstancedScriptPairReplayResult::Missing;
	}
	if (Record->OldKey != Token.OldKey
		|| Record->NewKey != Token.NewKey
		|| Record->CaptureGeneration != Token.CaptureGeneration
		|| Record->NewTrackSequence != Token.NewTrackSequence)
	{
		return EReinstancedScriptPairReplayResult::Conflict;
	}
	switch (Record->State)
	{
	case EReinstancedScriptPairConsumptionState::Reserved:
		return EReinstancedScriptPairReplayResult::Reserved;
	case EReinstancedScriptPairConsumptionState::Consumed:
		return EReinstancedScriptPairReplayResult::Consumed;
	case EReinstancedScriptPairConsumptionState::Failed:
		return EReinstancedScriptPairReplayResult::Failed;
	default:
		return EReinstancedScriptPairReplayResult::Missing;
	}
}

void FailRuntimeMirror(const TCHAR* Sentinel, const UObject* Object)
{
	UE_LOG(
		LogAngelscriptRuntimeGcLifecycle,
		Fatal,
		TEXT("%s: Runtime object mirror rejected object %p."),
		Sentinel,
		Object
	);
}

bool HasRuntimeObjectsPendingFinishDestroy()
{
    FScopeLock Lock(&RuntimeTrackedObjectsMutex);
    for (const auto& Pair : RuntimeTrackedObjects)
        if (Pair.Value.Key.ResolveObjectPtrEvenIfUnreachable()) return true;
    return false;
}

bool HasRuntimeObjectsPendingFinishDestroyForClass(const UASClass* Class)
{
    FScopeLock Lock(&RuntimeTrackedObjectsMutex);
    for (const auto& Pair : RuntimeTrackedObjects)
    {
        const auto& Entry = Pair.Value;
        UObject* Object = Entry.Key.ResolveObjectPtrEvenIfUnreachable();
        if (!Object) continue;
        asITypeInfo* Type = static_cast<asITypeInfo*>(Entry.SavedTypeCookie);
        const UASClass* Owner = Type ? static_cast<UASClass*>(Type->GetUserData()) : nullptr;
        if (Object->GetClass()->IsChildOf(Class) || (Owner && Owner->IsChildOf(Class)))
            return true;
    }
    return false;
}

void ReleasePublicDestroyReloadTracking(UObject* Object)
{
#if !SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    // Tracking serves reload identity only. Destruction never queries a test
    // observer or claims the old terminal lifecycle protocol.
    asITypeInfo* Cookie = nullptr;
    {
        FScopeLock Lock(&RuntimeTrackedObjectsMutex);
        const auto* Entry = RuntimeTrackedObjects.Find(FObjectKey(Object));
        if (!Entry) return;
        check(Entry->State == ERuntimeMirrorState::Pending);
        Cookie = static_cast<asITypeInfo*>(Entry->SavedTypeCookie);
    }
    verify(SoloASBridge::UntrackRuntimeObject(Object));
    verify(RemoveRuntimeTrackedMirror(Object, Cookie, ERuntimeMirrorState::Pending, true));
#endif
}

bool TrackRuntimeObjectWithMirror(UObject* Object, asITypeInfo* TypeCookie)
{
	const bool bSupportedTrackingThread = IsInGameThread()
		|| (IsInAsyncLoadingThread()
			&& Object != nullptr
			&& Object->HasAnyInternalFlags(
				EInternalObjectFlags_AsyncLoading
			));
	checkf(bSupportedTrackingThread, TEXT("unsupported tracking thread"));
	if (!bSupportedTrackingThread)
	{
		return false;
	}
	FAngelscriptExecutionReadScope TypeLifetimeScope;
	if (Object == nullptr || TypeCookie == nullptr)
	{
		FailRuntimeMirror(TEXT("REENTRANT_CONSTRUCTION"), Object);
		return false;
	}

	const FObjectKey Key(Object);
	const FObjectKey ClassKey(Object->GetClass());
	const bool bReplacementCaptureExcluded = IsScriptReplacementCaptureExcluded(Object);
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	checkf(!bRuntimeLifecycleShuttingDown, TEXT("TRACK_DURING_SHUTDOWN"));
	checkf(!RuntimeTrackedObjects.Contains(Key), TEXT("REENTRANT_CONSTRUCTION"));
	if (bRuntimeLifecycleShuttingDown || RuntimeTrackedObjects.Contains(Key))
	{
		FailRuntimeMirror(
			bRuntimeLifecycleShuttingDown ? TEXT("TRACK_DURING_SHUTDOWN")
				: TEXT("REENTRANT_CONSTRUCTION"),
			Object
		);
		return false;
	}

	if (!SoloASBridge::TrackRuntimeObject(Object, TypeCookie))
	{
		FailRuntimeMirror(TEXT("REENTRANT_CONSTRUCTION"), Object);
		return false;
	}

	const uint64 TrackSequence = NextRuntimeTrackSequence++;
	RuntimeTrackedObjects.Add(
		Key,
		FRuntimeTrackedObject{Key, Object, TypeCookie, ERuntimeMirrorState::Pending,
			TrackSequence, ClassKey, bReplacementCaptureExcluded}
	);
	Lock.Unlock();
	UE_LOG(LogAngelscriptRuntimeGcLifecycle, VeryVerbose,
		TEXT("ASR_MIRROR_REGISTER sequence=%llu object=%s class=%s flags=%08x ")
		TEXT("source=TrackRuntimeObjectWithMirror source_type=%s source_module=%s carrier=%s parent=%s"),
		TrackSequence, *Object->GetPathName(), *Object->GetClass()->GetPathName(), uint32(Object->GetFlags()),
		ANSI_TO_TCHAR(TypeCookie->GetName()), TypeCookie->GetModule() ? ANSI_TO_TCHAR(TypeCookie->GetModule()->GetName()) : TEXT("<none>"),
		*Object->GetClass()->GetClass()->GetPathName(), *GetPathNameSafe(Object->GetClass()->GetSuperClass()));
	return true;
}

bool TrackRuntimeObjectWithMirrorForLifecycle(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie
)
{
	if (!IsInGameThread()
		|| !IsAngelscriptExecutionWriterHeldByCurrentThread()
		|| !IsValid(Object)
		|| ScriptClass == nullptr
		|| TypeCookie == nullptr
		|| Object->GetClass() != ScriptClass
		|| TypeCookie->GetUserData() != ScriptClass
		|| TypeCookie != ScriptClass->ScriptTypePtr
		|| Object->IsUnreachable()
		|| Object->HasAnyFlags(
			RF_BeginDestroyed
			| RF_FinishDestroyed
			| RF_ClassDefaultObject
			| RF_ArchetypeObject
		))
	{
		return false;
	}

	if (!PrepareRuntimeTrackedMirrorForLifecycle(Object, TypeCookie))
	{
		return false;
	}

	if (!SoloASBridge::TrackRuntimeObject(Object, TypeCookie))
	{
		if (!RemoveRuntimeTrackedMirror(
			Object,
			TypeCookie,
			ERuntimeMirrorState::Pending
		))
		{
			FailRuntimeMirror(TEXT("MIRROR_TRACK_ROLLBACK_FAILED"), Object);
		}
		return false;
	}
	return true;
}

bool UntrackRuntimeObjectWithMirrorForLifecycle(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* ExpectedTypeCookie
)
{
	if (!IsInGameThread()
		|| !IsAngelscriptExecutionWriterHeldByCurrentThread()
		|| !IsValid(Object)
		|| ScriptClass == nullptr
		|| ExpectedTypeCookie == nullptr
		|| Object->GetClass() != ScriptClass
		|| ExpectedTypeCookie->GetUserData() != ScriptClass)
	{
		return false;
	}

	if (!ClaimRuntimeTrackedMirrorForUntracking(
		Object,
		ScriptClass,
		ExpectedTypeCookie
	))
	{
		return false;
	}
	CancelRuntimeTerminalOrdinaryClaim(Object, ExpectedTypeCookie);

	if (!SoloASBridge::UntrackRuntimeObject(Object))
	{
		if (!RestoreRuntimeTrackedMirrorAfterUntrackFailure(Object))
		{
			FailRuntimeMirror(TEXT("UNTRACK_RECOVERY_FAILED"), Object);
		}
		return false;
	}

	if (!RemoveRuntimeTrackedMirror(
		Object,
		ExpectedTypeCookie,
		ERuntimeMirrorState::Untracking
	))
	{
		const bool bBridgeRollback = SoloASBridge::TrackRuntimeObject(
			Object,
			ExpectedTypeCookie
		);
		const bool bMirrorRecovery = RestoreRuntimeTrackedMirrorAfterUntrackFailure(Object);
		if (!bBridgeRollback || !bMirrorRecovery)
		{
			FailRuntimeMirror(TEXT("UNTRACK_ROLLBACK_FAILED"), Object);
		}
		return false;
	}
	return true;
}

void ConsumeEngineReinstancedScriptClassPairs(
	const TMap<UObject*, UObject*>& ReinstancedObjects
)
{
	if (!IsInGameThread()
		|| bRuntimeLifecycleShuttingDown
		|| !bRuntimeScriptReplacementCaptureActive
		|| bRuntimeScriptReplacementCaptureAborted
		|| bReinstancedScriptPairConsumerInProgress)
	{
		return;
	}

	TGuardValue<bool> PairConsumerGuard(
		bReinstancedScriptPairConsumerInProgress,
		true
	);
	FAngelscriptExecutionWriteScope TypeLifetimeScope;
	TSet<const void*> CapturedSeedIdentities;
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		for (const TPair<UObject*, UObject*>& Element : ReinstancedObjects)
		{
			const void* OldIdentity = static_cast<const void*>(Element.Key);
			const FReloadCaptureRecord* Capture =
				ReloadCapturesByOldIdentity.Find(OldIdentity);
			if (Capture != nullptr
				&& Capture->State == EReloadCaptureState::Captured
				&& Capture->CaptureGeneration == ActiveReloadCaptureGeneration)
			{
				CapturedSeedIdentities.Add(OldIdentity);
			}
		}
	}

	TMap<const void*, int32> IncomingEdgeCounts;
	for (const TPair<UObject*, UObject*>& Element : ReinstancedObjects)
	{
		if (Element.Value != nullptr)
		{
			++IncomingEdgeCounts.FindOrAdd(
				static_cast<const void*>(Element.Value));
		}
	}
	TMap<const void*, UObject*> TerminalReplacementsBySeed;
	TMap<const void*, const void*> IntermediateOwners;
	TSet<const void*> CapturedIntermediateIdentities;
	TSet<const void*> TerminalIdentities;
	bool bReplacementClosureValid = true;
	for (const TPair<UObject*, UObject*>& Element : ReinstancedObjects)
	{
		const void* SeedIdentity = static_cast<const void*>(Element.Key);
		if (!CapturedSeedIdentities.Contains(SeedIdentity))
		{
			continue;
		}

		TSet<const void*> SeenIdentities;
		SeenIdentities.Add(SeedIdentity);
		UObject* Cursor = Element.Value;
		while (Cursor != nullptr)
		{
			const void* CursorIdentity = static_cast<const void*>(Cursor);
			if (SeenIdentities.Contains(CursorIdentity))
			{
				Cursor = nullptr;
				break;
			}
			SeenIdentities.Add(CursorIdentity);
			UObject* const* Next = ReinstancedObjects.Find(Cursor);
			if (Next == nullptr)
			{
				break;
			}
			const void* const* ExistingOwner = IntermediateOwners.Find(CursorIdentity);
			if (CapturedSeedIdentities.Contains(CursorIdentity)
				|| IncomingEdgeCounts.FindRef(CursorIdentity) != 1
				|| (ExistingOwner != nullptr && *ExistingOwner != SeedIdentity))
			{
				Cursor = nullptr;
				break;
			}
			IntermediateOwners.Add(CursorIdentity, SeedIdentity);
			CapturedIntermediateIdentities.Add(CursorIdentity);
			Cursor = *Next;
		}
		const void* TerminalIdentity = static_cast<const void*>(Cursor);
		if (Cursor == nullptr || TerminalIdentity == SeedIdentity
			|| TerminalIdentities.Contains(TerminalIdentity))
		{
			bReplacementClosureValid = false;
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Error,
				TEXT("SCRIPT_REINSTANCED_PAIR_CHAIN_REJECTED old=%p terminal=%p"),
				Element.Key, Cursor);
			continue;
		}
		TerminalIdentities.Add(TerminalIdentity);
		TerminalReplacementsBySeed.Add(SeedIdentity, Cursor);
	}
	TMap<UObject*, UObject*> ReplacementView = ReinstancedObjects;
	for (TPair<UObject*, UObject*>& Element : ReplacementView)
	{
		if (UObject* const* Terminal = TerminalReplacementsBySeed.Find(
			static_cast<const void*>(Element.Key)))
		{
			Element.Value = *Terminal;
		}
	}

	bool bSawScriptCandidate = CapturedSeedIdentities.Num() != 0;
	bool bAllScriptCandidatesValid = bReplacementClosureValid;
	TArray<FReinstancedScriptClassPair> CandidatePairs;
	TSet<const void*> CallbackOldActorIdentities;
	for (const TPair<UObject*, UObject*>& ReinstancedObject : ReplacementView)
	{
		if (ReinstancedObject.Key != nullptr)
		{
			CallbackOldActorIdentities.Add(
				static_cast<const void*>(ReinstancedObject.Key));
		}
	}
	struct FScriptActorOwnedComponentBypass { const void* OldObjectIdentity; const void* OldOwnerIdentity; const void* OldClassIdentity; };
	TArray<FScriptActorOwnedComponentBypass> OwnedComponentBypassCandidates;
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		for (const FScriptActorAttachmentSnapshot& Snapshot : ScriptActorAttachmentSnapshots)
		{
			if (Snapshot.CaptureGeneration != ActiveReloadCaptureGeneration
				|| !CallbackOldActorIdentities.Contains(Snapshot.OldActorIdentity))
			{
				continue;
			}
			for (const FScriptActorOwnedComponentSnapshot& Component : Snapshot.ActorComponents)
			{
				if (!Component.bIsScriptOwnedTarget
					|| Component.OldObjectIdentity == nullptr
					|| Component.OldOwnerIdentity == nullptr
					|| Component.OldClassIdentity == nullptr
					|| Snapshot.OldActorIdentity == nullptr
					|| Component.OldOwnerIdentity != Snapshot.OldActorIdentity) continue;
				FScriptActorOwnedComponentBypass& Candidate = OwnedComponentBypassCandidates.AddDefaulted_GetRef();
				Candidate.OldObjectIdentity = Component.OldObjectIdentity;
				Candidate.OldOwnerIdentity = Component.OldOwnerIdentity;
				Candidate.OldClassIdentity = Component.OldClassIdentity;
			}
		}
	}

	for (const TPair<UObject*, UObject*>& Element : ReinstancedObjects)
	{
		UObject* OldObject = Element.Key;
		const void* OldObjectIdentity = static_cast<const void*>(OldObject);
		const FReloadCaptureRecord* Capture = nullptr;
		{
			FScopeLock Lock(&RuntimeTrackedObjectsMutex);
			Capture = ReloadCapturesByOldIdentity.Find(OldObjectIdentity);
		}
		if (Capture == nullptr
			&& CapturedIntermediateIdentities.Contains(OldObjectIdentity))
		{
			continue;
		}
		UObject* NewObject = TerminalReplacementsBySeed.Contains(OldObjectIdentity)
			? TerminalReplacementsBySeed.FindRef(OldObjectIdentity)
			: Element.Value;
		if (IsRawReinstancedScriptPairTombstone(OldObject, NewObject))
		{
			bSawScriptCandidate = true;
			bAllScriptCandidatesValid = false;
			UE_LOG(
				LogAngelscriptRuntimeGcLifecycle,
				Error,
				TEXT("SCRIPT_REINSTANCED_PAIR_REJECTED raw-tombstone old=%p new=%p"),
				OldObject,
				NewObject
			);
			continue;
		}
		if (Capture != nullptr && Capture->State != EReloadCaptureState::Captured)
		{
			bSawScriptCandidate = true;
			bAllScriptCandidatesValid = false;
			UE_LOG(
				LogAngelscriptRuntimeGcLifecycle,
				Error,
				TEXT("SCRIPT_REINSTANCED_PAIR_REJECTED raw-tombstone old=%p new=%p"),
				OldObject,
				NewObject
			);
			continue;
		}

		const bool bNewLive = NewObject != nullptr && IsValid(NewObject);
		const bool bNewScriptObject = bNewLive
			&& Cast<UASClass>(NewObject->GetClass()) != nullptr;
		bool bUnchangedScriptActorOwnedComponent = false;
		if (Capture == nullptr && bNewScriptObject)
		{
			const FScriptActorOwnedComponentBypass* Bypass = nullptr;
			int32 BypassMatchCount = 0;
			for (const FScriptActorOwnedComponentBypass& Candidate : OwnedComponentBypassCandidates)
			{
				if (Candidate.OldObjectIdentity == OldObjectIdentity)
				{
					Bypass = &Candidate; ++BypassMatchCount;
				}
			}
			UActorComponent* NewComponent = Cast<UActorComponent>(NewObject);
			AActor* NewOwner = Bypass != nullptr ? Cast<AActor>(FindReplacementByObjectIdentity(ReplacementView, Bypass->OldOwnerIdentity)) : nullptr;
			bUnchangedScriptActorOwnedComponent = BypassMatchCount == 1 && NewComponent != nullptr
				&& !NewComponent->IsUnreachable()
				&& !NewComponent->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
				&& Cast<USceneComponent>(NewComponent) == nullptr
				&& NewOwner != nullptr && IsValid(NewOwner) && !NewOwner->IsUnreachable() && !NewOwner->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
				&& static_cast<const void*>(NewOwner) != Bypass->OldOwnerIdentity
				&& NewComponent->GetOwner() == NewOwner
				&& static_cast<const void*>(NewComponent->GetClass()) == Bypass->OldClassIdentity;
		}
		if (Capture == nullptr)
		{
			if (!bNewScriptObject)
			{
				continue;
			}
			if (bUnchangedScriptActorOwnedComponent)
			{
				continue;
			}
			bool bChainContinued = false;
			{
				FScopeLock Lock(&RuntimeTrackedObjectsMutex);
				const FObjectKey ContinuationOldKey(OldObject);
				const FObjectKey ContinuationTerminalKey(NewObject);
				const FConsumedReinstancedScriptClassPair* Predecessor =
					ReinstancedScriptPairsByNewObject.Find(ContinuationOldKey);
				const FRuntimeTrackedObject* TerminalEntry =
					RuntimeTrackedObjects.Find(ContinuationTerminalKey);
				UASClass* TerminalClass = Cast<UASClass>(NewObject->GetClass());
				if (Predecessor != nullptr
					&& Predecessor->State == EReinstancedScriptPairConsumptionState::Consumed
					&& Predecessor->NewKey == ContinuationOldKey
					&& Predecessor->NewObjectIdentity == OldObjectIdentity
					&& Predecessor->CaptureGeneration == ActiveReloadCaptureGeneration
					&& TerminalEntry != nullptr
					&& TerminalEntry->Key == ContinuationTerminalKey
					&& TerminalEntry->ObjectIdentity == static_cast<const UObjectBase*>(NewObject)
					&& TerminalEntry->State == ERuntimeMirrorState::Pending
					&& TerminalClass != nullptr
					&& TerminalEntry->SavedTypeCookie == TerminalClass->ScriptTypePtr
					&& !ReinstancedScriptPairsByNewObject.Contains(ContinuationTerminalKey))
				{
					ReinstancedScriptPairsByNewObject.Add(
						ContinuationTerminalKey,
						FConsumedReinstancedScriptClassPair(
							ContinuationOldKey,
							ContinuationTerminalKey,
							OldObjectIdentity,
							static_cast<const void*>(NewObject),
							ActiveReloadCaptureGeneration,
							Predecessor->NewTrackSequence,
							TerminalEntry->TrackSequence,
							EReinstancedScriptPairConsumptionState::Consumed));
					bChainContinued = true;
				}
			}
			if (bChainContinued)
			{
				UE_LOG(
					LogAngelscriptRuntimeGcLifecycle,
					Log,
					TEXT("SCRIPT_REINSTANCED_PAIR_CHAIN_CONTINUED old=%p new=%p"),
					OldObject,
					NewObject
				);
				continue;
			}
			const bool bSpecialNewObject = NewObject->IsUnreachable()
				|| NewObject->HasAnyFlags(
					RF_ClassDefaultObject | RF_ArchetypeObject | RF_DefaultSubObject)
				|| Cast<UASClass>(NewObject) != nullptr;
			if (bSpecialNewObject)
			{
				continue;
			}
			bSawScriptCandidate = true;
			bAllScriptCandidatesValid = false;
			UE_LOG(
				LogAngelscriptRuntimeGcLifecycle,
				Error,
				TEXT("SCRIPT_REINSTANCED_PAIR_REJECTED uncaptured-ordinary old=%p new=%p"),
				OldObject,
				NewObject
			);
			continue;
		}

		bSawScriptCandidate = true;
		FReinstancedScriptClassPair Pair;
		Pair.NewObject = NewObject;
		Pair.NewClass = bNewLive
			? Cast<UASClass>(NewObject->GetClass())
			: nullptr;
		Pair.NewTypeCookie = Pair.NewClass != nullptr
			? Pair.NewClass->ScriptTypePtr
			: nullptr;
		Pair.Capture = Capture;
		if (!ValidateReinstancedScriptClassPair(Pair))
		{
			bAllScriptCandidatesValid = false;
			UE_LOG(
				LogAngelscriptRuntimeGcLifecycle,
				Error,
				TEXT("SCRIPT_REINSTANCED_PAIR_REJECTED old=%p new=%p new_class=%p"),
				OldObject,
				NewObject,
				Pair.NewClass
			);
			continue;
		}
		CandidatePairs.Add(Pair);
	}
	if (bSawScriptCandidate)
	{
		UE_LOG(
			LogAngelscriptRuntimeGcLifecycle,
			Log,
			TEXT("SCRIPT_REINSTANCED_PAIR_BATCH_PREFLIGHT candidateCount=%d generation=%llu"),
			CandidatePairs.Num(),
			static_cast<unsigned long long>(ActiveReloadCaptureGeneration)
		);
	}

#if WITH_DEV_AUTOMATION_TESTS
    if (GCrossActorObserver && !bCrossActorApplied && bAllScriptCandidatesValid
        && CandidatePairs.Num() == 2 && ReinstancedScriptPairsByNewObject.IsEmpty())
    {
        TSet<UObject*> NewObjects;
        for (const void* Identity : GCrossActorOldIds)
        {
            UObject* NewObject = FindReplacementByObjectIdentity(ReplacementView, Identity);
            if (IsValid(NewObject) && static_cast<const void*>(NewObject) != Identity
                && ReinstancedObjects.FindRef(static_cast<UObject*>(const_cast<void*>(Identity))) == NewObject)
                NewObjects.Add(NewObject);
        }
        if (NewObjects.Num() == 6)
        {
            GCrossActorObserver(ReinstancedObjects, false);
            bCrossActorApplied = ReplacementView.Remove(
                static_cast<UObject*>(const_cast<void*>(GCrossActorOldIds[4]))) == 1;
        }
    }
#endif
	TArray<FReinstancedScriptPairFinalizationContext> PairContexts;
	TArray<FScriptActorAttachmentRestorePlan> AttachmentPlans;
	EReinstancedScriptPairFailureStage FailureStage =
		EReinstancedScriptPairFailureStage::Preflight;
	if (bSawScriptCandidate
		&& bAllScriptCandidatesValid
		&& !BuildScriptActorAttachmentRestorePlans(
			ReplacementView,
			AttachmentPlans
		))
	{
		UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning,
			TEXT("SCRIPT_REINSTANCED_PAIR_PREFLIGHT_DETAIL stage=AttachmentPlan"));
#if WITH_DEV_AUTOMATION_TESTS
        if (bCrossActorApplied) GCrossActorObserver(ReinstancedObjects, true);
#endif
		bAllScriptCandidatesValid = false;
	}
	bool bAttachmentMutationApplied = false;
	if (bSawScriptCandidate && bAllScriptCandidatesValid)
	{
		if (!RestoreScriptActorAttachmentPlans(AttachmentPlans))
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning,
				TEXT("SCRIPT_REINSTANCED_PAIR_PREFLIGHT_DETAIL stage=AttachmentRestore"));
			if (!RollbackScriptActorAttachmentPlans(AttachmentPlans))
			{
				FailRuntimeMirror(
					TEXT("SCRIPT_REINSTANCED_ACTOR_ATTACHMENT_ROLLBACK_FAILED"),
					nullptr);
				return;
			}
			bAllScriptCandidatesValid = false;
		}
		else
		{
			bAttachmentMutationApplied = true;
		}
	}
	if (bSawScriptCandidate
		&& bAllScriptCandidatesValid
		&& !ReserveReinstancedScriptPairWindow(
			CandidatePairs,
			PairContexts,
			FailureStage
		))
	{
		UE_LOG(LogAngelscriptRuntimeGcLifecycle, Warning,
			TEXT("SCRIPT_REINSTANCED_PAIR_PREFLIGHT_DETAIL stage=PairWindow"));
		if (bAttachmentMutationApplied)
		{
			if (!RollbackScriptActorAttachmentPlans(AttachmentPlans))
			{
				FailRuntimeMirror(
					TEXT("SCRIPT_REINSTANCED_ACTOR_ATTACHMENT_ROLLBACK_FAILED"),
					nullptr);
				return;
			}
			bAttachmentMutationApplied = false;
		}
		bAllScriptCandidatesValid = false;
	}

	if (!bAllScriptCandidatesValid || CandidatePairs.Num() == 0)
	{
		if (bAttachmentMutationApplied)
		{
			if (!RollbackScriptActorAttachmentPlans(AttachmentPlans))
			{
				FailRuntimeMirror(
					TEXT("SCRIPT_REINSTANCED_ACTOR_ATTACHMENT_ROLLBACK_FAILED"),
					nullptr);
				return;
			}
			bAttachmentMutationApplied = false;
		}
		if (bSawScriptCandidate)
		{
			bRuntimeScriptReplacementCaptureAborted = true;
			UE_LOG(
				LogAngelscriptRuntimeGcLifecycle,
				Error,
				TEXT("SCRIPT_REINSTANCED_PAIR_BATCH_REJECTED candidateCount=%d stage=%s published=0 consumed=0 generation=%llu"),
				CandidatePairs.Num(),
				ReinstancedScriptPairFailureStageName(FailureStage),
				static_cast<unsigned long long>(ActiveReloadCaptureGeneration)
			);
			UE_LOG(
				LogAngelscriptRuntimeGcLifecycle,
				Error,
				TEXT("SCRIPT_REINSTANCED_PAIR_WINDOW_REJECTED"));
		}
		return;
	}

	UE_LOG(
		LogAngelscriptRuntimeGcLifecycle,
		Log,
		TEXT("SCRIPT_REINSTANCED_PAIR_BATCH_PUBLISHED candidateCount=%d published=1 consumed=0 generation=%llu"),
		CandidatePairs.Num(),
		static_cast<unsigned long long>(ActiveReloadCaptureGeneration)
	);

	for (int32 Index = 0; Index < CandidatePairs.Num(); ++Index)
	{
		const FReinstancedScriptClassPair& Pair = CandidatePairs[Index];
		const FReinstancedScriptPairFinalizationContext& PairContext =
			PairContexts[Index];
		ReconstructReinstancedScriptClassPair(
			Pair.NewObject,
			Pair.NewClass,
			Pair.NewTypeCookie,
			PairContext
		);
	}

	FinalizeReinstancedScriptPairWindow(CandidatePairs, PairContexts);
	RemoveProcessedScriptActorAttachmentSnapshots(ReplacementView);
	// Preserve the established single-pair event for existing consumers; the
	// batch marker below carries the cardinality and publication state.
	UE_LOG(
		LogAngelscriptRuntimeGcLifecycle,
		Log,
		TEXT("SCRIPT_REINSTANCED_PAIR_CONSUMED"));
	UE_LOG(
		LogAngelscriptRuntimeGcLifecycle,
		Log,
		TEXT("SCRIPT_REINSTANCED_PAIR_BATCH_CONSUMED candidateCount=%d published=1 consumed=%d generation=%llu"),
		CandidatePairs.Num(),
		CandidatePairs.Num(),
		static_cast<unsigned long long>(ActiveReloadCaptureGeneration)
	);
}

bool BeginRuntimeScriptReplacementCapture(
	const TMap<UClass*, UClass*>& ReloadClasses
)
{
	return BeginRuntimeScriptReplacementCaptureWithWitnesses(ReloadClasses, nullptr);
}

bool BeginRuntimeScriptReplacementCaptureWithWitnesses(
	const TMap<UClass*, UClass*>& ReloadClasses,
	TArray<FRuntimeScriptReplacementWitness>* OutWitnesses
)
{
	if (!IsInGameThread())
	{
		return false;
	}

	FAngelscriptExecutionWriteScope TypeLifetimeScope;
	TArray<FRuntimeTrackedObject> TrackedEntries;
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		if (bRuntimeLifecycleShuttingDown
			|| bRuntimeScriptReplacementCaptureActive
			|| bRuntimeScriptReplacementCaptureAborted
			|| ReloadCapturesByOldIdentity.Num() != 0
			|| ScriptActorAttachmentSnapshots.Num() != 0)
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Error,
				TEXT("SCRIPT_RELOAD_CAPTURE_REJECT reason=active_window shutting_down=%d active=%d aborted=%d captures=%d attachments=%d"),
				bRuntimeLifecycleShuttingDown, bRuntimeScriptReplacementCaptureActive,
				bRuntimeScriptReplacementCaptureAborted, ReloadCapturesByOldIdentity.Num(), ScriptActorAttachmentSnapshots.Num());
			return false;
		}
		for (const TPair<FObjectKey, FConsumedReinstancedScriptClassPair>& Pair :
			ReinstancedScriptPairsByNewObject)
		{
			if (Pair.Value.State == EReinstancedScriptPairConsumptionState::Reserved)
			{
				return false;
			}
		}
		TrackedEntries.Reserve(RuntimeTrackedObjects.Num());
		for (const TPair<FObjectKey, FRuntimeTrackedObject>& Tracked : RuntimeTrackedObjects)
		{
			TrackedEntries.Add(Tracked.Value);
		}
	}

	for (const TPair<UClass*, UClass*>& Replacement : ReloadClasses)
	{
		UASClass* OldClass = Cast<UASClass>(Replacement.Key);
		UASClass* NewClass = Cast<UASClass>(Replacement.Value);
		if ((OldClass != nullptr || NewClass != nullptr)
			&& (OldClass == nullptr || NewClass == nullptr
				|| OldClass == NewClass
				|| OldClass->NewerVersion != NewClass
				|| !OldClass->HasAnyClassFlags(CLASS_NewerVersionExists)
				|| NewClass->HasAnyClassFlags(CLASS_NewerVersionExists)))
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Error,
				TEXT("SCRIPT_RELOAD_CAPTURE_REJECT reason=class_generation old=%s new=%s"),
				*GetPathNameSafe(Replacement.Key), *GetPathNameSafe(Replacement.Value));
			return false;
		}
	}

	uint64 Generation = 0;
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		// Reserve no global state yet. Scalar values must be materialized while
		// the old object/class/property graph is still live and before this
		// capture can make the replacement window visible.
		Generation = NextReloadCaptureGeneration;
	}
	TArray<FReloadCaptureRecord> Captures;
	TArray<FScriptActorAttachmentSnapshot> AttachmentSnapshots;
	TSet<const void*> CaptureIdentities;
	TSet<FObjectKey> ExpiredUnrelatedKeys;
	for (const FRuntimeTrackedObject& Entry : TrackedEntries)
	{
		UObject* Object = Entry.Key.ResolveObjectPtrEvenIfUnreachable();
		if (Object == nullptr)
		{
			bool bClassInReload = false;
			for (const TPair<UClass*, UClass*>& Replacement : ReloadClasses)
			{
				if (FObjectKey(Replacement.Key) == Entry.SavedClassKey)
				{
					bClassInReload = true;
					break;
				}
			}
			if ((!bClassInReload || Entry.bReplacementCaptureExcluded)
				&& Entry.State == ERuntimeMirrorState::Pending && Entry.RetainedModule == nullptr)
			{
				ExpiredUnrelatedKeys.Add(Entry.Key);
				continue;
			}
		}
		if (Object == nullptr
			|| static_cast<const UObjectBase*>(Object) != Entry.ObjectIdentity)
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Error,
				TEXT("SCRIPT_RELOAD_CAPTURE_REJECT reason=tracked_identity expired=%d state=%d sequence=%llu module_retained=%d"),
				Object == nullptr, int32(Entry.State), static_cast<unsigned long long>(Entry.TrackSequence), Entry.RetainedModule != nullptr);
			return false;
		}
		UASClass* OldClass = Cast<UASClass>(Object->GetClass());
		UClass* const* NewClassValue = ReloadClasses.Find(OldClass);
		if (NewClassValue == nullptr)
		{
			continue;
		}
		UASClass* NewClass = Cast<UASClass>(*NewClassValue);
		if (IsScriptReplacementCaptureExcluded(Object))
		{
			continue;
		}
		if (Entry.State != ERuntimeMirrorState::Pending
			|| Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			|| Entry.SavedTypeCookie == nullptr
			|| static_cast<asITypeInfo*>(Entry.SavedTypeCookie)->GetUserData()
				!= OldClass
			|| (Entry.SavedTypeCookie != OldClass->ScriptTypePtr && !HasRetiredRuntimeType(Entry, OldClass))
			|| NewClass == nullptr)
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Error,
				TEXT("SCRIPT_RELOAD_CAPTURE_REJECT object=%s state=%d retiring=%d saved_type=%d current_type=%d type_matches=%d module_retained=%d"),
				*Object->GetName(), static_cast<int32>(Entry.State), Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed) ? 1 : 0,
				Entry.SavedTypeCookie != nullptr ? 1 : 0, OldClass->ScriptTypePtr != nullptr ? 1 : 0,
				Entry.SavedTypeCookie == OldClass->ScriptTypePtr ? 1 : 0, Entry.RetainedModule != nullptr ? 1 : 0);
			return false;
		}
		if (CaptureIdentities.Contains(static_cast<const void*>(Object)))
		{
			return false;
		}
		CaptureIdentities.Add(static_cast<const void*>(Object));

		FReloadCaptureRecord& Capture = Captures.AddDefaulted_GetRef();
		Capture.OldKey = Entry.Key;
		Capture.OldObjectIdentity = static_cast<const void*>(Object);
		Capture.OldTypeCookie = static_cast<asITypeInfo*>(Entry.SavedTypeCookie);
		Capture.OldTypeCookieUserData =
			static_cast<asITypeInfo*>(Entry.SavedTypeCookie)->GetUserData();
		Capture.OldClassIdentity = static_cast<const void*>(OldClass);
		Capture.NewClassIdentity = static_cast<const void*>(NewClass);
		Capture.OldFlags = static_cast<uint32>(Object->GetFlags());
		Capture.OldTrackSequence = Entry.TrackSequence;
		Capture.CaptureGeneration = Generation;
		if (!CaptureRuntimeScalarSnapshots(
			Object,
			OldClass,
			NewClass,
			static_cast<asITypeInfo*>(Entry.SavedTypeCookie),
			ReloadClasses,
			Capture.ScalarSnapshots))
		{
			return false;
		}
		if (!CaptureRuntimeClassReferenceSnapshots(
			Object,
			OldClass,
			NewClass,
			static_cast<asITypeInfo*>(Entry.SavedTypeCookie),
			ReloadClasses,
			Capture.ClassReferenceSnapshots))
		{
			return false;
		}

		if (AActor* Actor = Cast<AActor>(Object))
		{
			FScriptActorAttachmentSnapshot& Snapshot =
				AttachmentSnapshots.AddDefaulted_GetRef();
			if (!CaptureScriptActorAttachmentSnapshot(
					Actor,
					Generation,
					ReloadClasses,
					Snapshot
				))
			{
				return false;
			}
		}
	}
	AttachmentSnapshots.Sort([](const FScriptActorAttachmentSnapshot& Left,
		const FScriptActorAttachmentSnapshot& Right)
	{
		const int32 NameOrder = Left.ActorName.LexicalLess(Right.ActorName)
			? -1
			: (Right.ActorName.LexicalLess(Left.ActorName) ? 1 : 0);
		return NameOrder != 0
			? NameOrder < 0
			: Left.ActorFullName.Compare(Right.ActorFullName) < 0;
	});

	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		if (bRuntimeLifecycleShuttingDown
			|| bRuntimeScriptReplacementCaptureActive
			|| bRuntimeScriptReplacementCaptureAborted
			|| ReloadCapturesByOldIdentity.Num() != 0
			|| ScriptActorAttachmentSnapshots.Num() != 0)
		{
			return false;
		}
		for (const FRuntimeTrackedObject& CapturedEntry : TrackedEntries)
		{
			const FRuntimeTrackedObject* CurrentEntry =
				RuntimeTrackedObjects.Find(CapturedEntry.Key);
			if (ExpiredUnrelatedKeys.Contains(CapturedEntry.Key))
			{
				if (CurrentEntry == nullptr
					|| CurrentEntry->ObjectIdentity != CapturedEntry.ObjectIdentity
					|| CurrentEntry->State != CapturedEntry.State
					|| CurrentEntry->SavedTypeCookie != CapturedEntry.SavedTypeCookie
					|| CurrentEntry->TrackSequence != CapturedEntry.TrackSequence
					|| CurrentEntry->SavedClassKey != CapturedEntry.SavedClassKey
					|| CurrentEntry->bReplacementCaptureExcluded != CapturedEntry.bReplacementCaptureExcluded
					|| CurrentEntry->RetainedModule != CapturedEntry.RetainedModule)
				{
					return false;
				}
				continue;
			}
			if (CurrentEntry == nullptr
				|| CurrentEntry->ObjectIdentity != CapturedEntry.ObjectIdentity
				|| CurrentEntry->State != CapturedEntry.State
				|| CurrentEntry->SavedTypeCookie != CapturedEntry.SavedTypeCookie
				|| CurrentEntry->TrackSequence != CapturedEntry.TrackSequence
				|| CurrentEntry->SavedClassKey != CapturedEntry.SavedClassKey
				|| CurrentEntry->bReplacementCaptureExcluded != CapturedEntry.bReplacementCaptureExcluded)
			{
				return false;
			}
		}
		if (NextReloadCaptureGeneration != Generation)
		{
			return false;
		}
		for (const FObjectKey& ExpiredKey : ExpiredUnrelatedKeys)
		{
			if (RuntimeTrackedObjects.Remove(ExpiredKey) != 1)
			{
				return false;
			}
		}
		for (const FReloadCaptureRecord& Capture : Captures)
		{
			ReloadCapturesByOldIdentity.Add(Capture.OldObjectIdentity, Capture);
		}
		++NextReloadCaptureGeneration;
		ScriptActorAttachmentSnapshots = MoveTemp(AttachmentSnapshots);
		ActiveReloadCaptureGeneration = Generation;
		bRuntimeScriptReplacementCaptureAborted = false;
		bRuntimeScriptReplacementCaptureActive = true;
		if (OutWitnesses != nullptr)
		{
			OutWitnesses->Reset();
			for (const TPair<const void*, FReloadCaptureRecord>& Entry : ReloadCapturesByOldIdentity)
			{
				const FReloadCaptureRecord& Capture = Entry.Value;
				OutWitnesses->Add({Capture.OldKey, Capture.OldObjectIdentity,
					Capture.NewClassIdentity, Capture.CaptureGeneration});
			}
		}
	}
	return true;
}

void EndRuntimeScriptReplacementCapture()
{
	if (!IsInGameThread())
	{
		return;
	}
	checkf(!bReinstancedScriptPairConsumerInProgress,
		TEXT("REINSTANCED_PAIR_CONSUMER_ACTIVE"));
	if (bReinstancedScriptPairConsumerInProgress)
	{
		return;
	}
	FAngelscriptExecutionWriteScope TypeLifetimeScope;
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	const int32 PendingAttachmentSnapshotCount =
		ScriptActorAttachmentSnapshots.Num();
	int32 PendingCaptureCount = 0;
	for (const TPair<const void*, FReloadCaptureRecord>& Capture :
		ReloadCapturesByOldIdentity)
	{
		if (Capture.Value.CaptureGeneration == ActiveReloadCaptureGeneration
			&& Capture.Value.State != EReloadCaptureState::Consumed
			&& Capture.Value.State != EReloadCaptureState::RetiredByGc)
		{
			++PendingCaptureCount;
			UObject* PendingObject = Capture.Value.OldKey.ResolveObjectPtrEvenIfUnreachable();
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Display,
				TEXT("SCRIPT_REINSTANCED_PENDING_CAPTURE object=%s allocated=%d valid=%d state=%d"),
				*GetPathNameSafe(PendingObject), PendingObject != nullptr ? 1 : 0,
				IsValid(PendingObject) ? 1 : 0, int32(Capture.Value.State));
		}
	}
	if (bRuntimeScriptReplacementCaptureAborted
		|| PendingAttachmentSnapshotCount != 0
		|| PendingCaptureCount != 0)
	{
		UE_LOG(
			LogAngelscriptRuntimeGcLifecycle,
			Warning,
			TEXT("SCRIPT_REINSTANCED_PAIR_WINDOW_INCOMPLETE aborted=%d pendingActorSnapshots=%d pendingCaptures=%d generation=%llu"),
			bRuntimeScriptReplacementCaptureAborted ? 1 : 0,
			PendingAttachmentSnapshotCount,
			PendingCaptureCount,
			static_cast<unsigned long long>(ActiveReloadCaptureGeneration)
		);
	}
	for (TPair<FObjectKey, FConsumedReinstancedScriptClassPair>& Pair :
		ReinstancedScriptPairsByNewObject)
	{
		if (Pair.Value.State == EReinstancedScriptPairConsumptionState::Reserved
			&& Pair.Value.CaptureGeneration == ActiveReloadCaptureGeneration)
		{
			Pair.Value.State = EReinstancedScriptPairConsumptionState::Failed;
		}
	}
	for (TPair<const void*, FReloadCaptureRecord>& Capture : ReloadCapturesByOldIdentity)
	{
		if (Capture.Value.CaptureGeneration == ActiveReloadCaptureGeneration
			&& Capture.Value.State != EReloadCaptureState::Consumed)
		{
			Capture.Value.State = EReloadCaptureState::Failed;
		}
	}
	ReloadCapturesByOldIdentity.Empty();
	ScriptActorAttachmentSnapshots.Empty();
	ActiveReloadCaptureGeneration = 0;
	bRuntimeScriptReplacementCaptureAborted = false;
	bRuntimeScriptReplacementCaptureActive = false;
}




#if WITH_DEV_AUTOMATION_TESTS
bool ObserveRuntimeTrackedMirrorCookie(UObject* Object, asITypeInfo*& OutTypeCookie)
{
    void* Cookie = nullptr;
    uint64 Sequence = 0;
    OutTypeCookie = Object && ObserveBridgeRuntimeCookieForMirror(Object, Cookie)
        && ReadRuntimeDestroyTracking({FObjectKey(Object), Object, Cookie, 0, 0}, Sequence) ? static_cast<asITypeInfo*>(Cookie) : nullptr;
    return OutTypeCookie != nullptr;
}
#endif

bool CommitRuntimeTrackedObjectReconstruction(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	asITypeInfo* NewTypeCookie,
	const FReinstancedScriptPairFinalizationContext* PairContext
)
{
	checkf(IsInGameThread(),
		TEXT("Runtime reconstruction commit requires the game thread."));
	checkf(IsAngelscriptExecutionWriterHeldByCurrentThread(),
		TEXT("Runtime reconstruction commit requires the execution writer."));
	if (!IsInGameThread()
		|| !IsAngelscriptExecutionWriterHeldByCurrentThread())
	{
		return false;
	}

	if (!IsExactRuntimeReconstructionTypeCookie(Object, NewTypeCookie))
	{
			if (!RestoreRuntimeTrackedMirrorAfterReconstructionFailure(
				Object,
				ExpectedTypeCookie,
				PairContext
		))
		{
			FailRuntimeMirror(TEXT("RECONSTRUCT_ROLLBACK_FAILED"), Object);
		}
		return false;
	}

	if (!ValidateReinstancedScriptPairFinalization(
		Object,
		NewTypeCookie,
		PairContext
	))
	{
		if (!RestoreRuntimeTrackedMirrorAfterReconstructionFailure(
				Object,
				ExpectedTypeCookie,
				PairContext
			))
		{
			FailRuntimeMirror(TEXT("RECONSTRUCT_ROLLBACK_FAILED"), Object);
		}
		return false;
	}

	if (!UpdateBridgeRuntimeObjectTypeCookieForReconstruction(
			Object,
			ExpectedTypeCookie,
			NewTypeCookie,
			false
		))
	{
		if (!RestoreRuntimeTrackedMirrorAfterReconstructionFailure(
			Object,
			ExpectedTypeCookie,
			PairContext
		))
		{
			FailRuntimeMirror(TEXT("RECONSTRUCT_ROLLBACK_FAILED"), Object);
		}
		return false;
	}

	if (UpdateRuntimeTrackedMirrorCookie(Object, ExpectedTypeCookie, NewTypeCookie))
	{
		if (FinalizeReinstancedScriptPairConsumption(
				Object,
				NewTypeCookie,
				PairContext
			))
		{
			if (PairContext == nullptr && !CommitRuntimeReconstructionRevision(Object, ExpectedTypeCookie, NewTypeCookie))
				FailRuntimeMirror(TEXT("RECONSTRUCT_REVISION_COMMIT_FAILED"), Object);
			return true;
		}

		const bool bBridgeRollback =
			UpdateBridgeRuntimeObjectTypeCookieForReconstruction(
				Object,
				NewTypeCookie,
				ExpectedTypeCookie,
				true
			);
		const bool bMirrorRollback =
			RestoreRuntimeTrackedMirrorAfterCommittedReconstructionFailure(
				Object,
				ExpectedTypeCookie,
				NewTypeCookie,
				PairContext
			);
		if (!bBridgeRollback || !bMirrorRollback)
		{
			FailRuntimeMirror(TEXT("RECONSTRUCT_ROLLBACK_FAILED"), Object);
		}
		return false;
	}

	const bool bBridgeRollback =
		UpdateBridgeRuntimeObjectTypeCookieForReconstruction(
			Object,
			NewTypeCookie,
			ExpectedTypeCookie,
			true
		);
	const bool bMirrorRecovery = RestoreRuntimeTrackedMirrorAfterReconstructionFailure(
		Object,
		ExpectedTypeCookie,
		PairContext
	);
	if (!bBridgeRollback || !bMirrorRecovery)
	{
		FailRuntimeMirror(TEXT("RECONSTRUCT_ROLLBACK_FAILED"), Object);
	}
	return false;
}

void DestroyTrackedRuntimeObjectExplicitly(UObject* Object)
{
	checkf(IsAngelscriptExecutionWriterHeldByCurrentThread(),
		TEXT("Explicit Runtime destruction requires the execution writer."));
	const bool bGarbageCleanup = Object && Object->HasAnyInternalFlags(EInternalObjectFlags::Garbage);
	const FObjectKey Key(Object);
	if (bGarbageCleanup)
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		// Weak serial identities survive until physical collection, without retaining
		// objects or type cookies. Missing mirror entries alone never grant a skip.
		for (auto It = ExplicitlyDestroyedGarbageObjects.CreateIterator(); It; ++It)
			if (It->ResolveObjectPtrEvenIfUnreachable() == nullptr) It.RemoveCurrent();
		if (ExplicitlyDestroyedGarbageObjects.Contains(Key))
		{
			if (RuntimeTrackedObjects.Contains(Key))
				FailRuntimeMirror(TEXT("GARBAGE_DESTRUCTION_IDENTITY_REUSED"), Object);
			return;
		}
	}
	asITypeInfo* SavedTypeCookie = ClaimRuntimeTrackedMirror(
		Object, nullptr, ERuntimeMirrorState::Pending, ERuntimeMirrorState::Dispatching);
	if (!CallDestructor(Object, SavedTypeCookie))
	{
		FailRuntimeMirror(TEXT("NON_GC_OVERWRITE_UNSUPPORTED"), Object);
		return;
	}
	if (!SoloASBridge::UntrackRuntimeObject(Object))
	{
		FailRuntimeMirror(TEXT("SHUTDOWN_UNTRACK_FAILED"), Object);
		return;
	}
	if (!RemoveRuntimeTrackedMirror(
		Object, SavedTypeCookie, ERuntimeMirrorState::Dispatching))
	{
		FailRuntimeMirror(TEXT("MIRROR_REMOVE_FAILED"), Object);
		return;
	}
	if (bGarbageCleanup)
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		ExplicitlyDestroyedGarbageObjects.Add(Key);
	}
}

bool DestroyTrackedRuntimeObjectExplicitlyForLifecycle(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* ExpectedTypeCookie
)
{
	checkf(IsInGameThread(),
		TEXT("Lifecycle Runtime destruction requires the game thread."));
	checkf(IsAngelscriptExecutionWriterHeldByCurrentThread(),
		TEXT("Lifecycle Runtime destruction requires the execution writer."));
	if (!IsInGameThread()
		|| !IsAngelscriptExecutionWriterHeldByCurrentThread()
		|| !ClaimRuntimeTrackedMirrorForLifecycleDestruction(
			Object,
			ScriptClass,
			ExpectedTypeCookie
		))
	{
		return false;
	}

	CancelRuntimeTerminalOrdinaryClaim(Object, ExpectedTypeCookie);
	if (!CallDestructor(Object, ExpectedTypeCookie))
	{
		if (!RestoreRuntimeTrackedMirrorAfterDestructionFailure(
			Object,
			ExpectedTypeCookie
		))
		{
			FailRuntimeMirror(TEXT("MIRROR_DESTROY_RECOVERY_FAILED"), Object);
		}
		FailRuntimeMirror(TEXT("NON_GC_OVERWRITE_UNSUPPORTED"), Object);
		return false;
	}
	if (!SoloASBridge::UntrackRuntimeObject(Object))
	{
		// The script destructor has already run, so this path cannot safely
		// re-track the object. Fail closed after restoring the diagnostic state;
		// continuing would permit a second destructor dispatch.
		if (!RestoreRuntimeTrackedMirrorAfterDestructionFailure(
			Object,
			ExpectedTypeCookie
		))
		{
			FailRuntimeMirror(TEXT("MIRROR_DESTROY_RECOVERY_FAILED"), Object);
		}
		FailRuntimeMirror(TEXT("SHUTDOWN_UNTRACK_FAILED"), Object);
		return false;
	}
	if (!RemoveRuntimeTrackedMirror(
		Object,
		ExpectedTypeCookie,
		ERuntimeMirrorState::Dispatching
	))
	{
		if (!RestoreRuntimeTrackedMirrorAfterDestructionFailure(
			Object,
			ExpectedTypeCookie
		))
		{
			FailRuntimeMirror(TEXT("MIRROR_DESTROY_RECOVERY_FAILED"), Object);
		}
		FailRuntimeMirror(TEXT("MIRROR_REMOVE_FAILED"), Object);
		return false;
	}
	return true;
}

void DestroyRuntimeTrackedObjectsForClass(UClass* RemovedClass, bool bPreserveLiveObjects)
{
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
	checkf(IsAngelscriptExecutionWriterHeldByCurrentThread(),
		TEXT("Removed-class mirror retirement requires the execution writer."));
	if (RemovedClass == nullptr)
	{
		return;
	}

	TArray<FRuntimeTrackedObject> RetiredEntries;
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		for (const TPair<FObjectKey, FRuntimeTrackedObject>& Pair : RuntimeTrackedObjects)
		{
			if (Pair.Value.State == ERuntimeMirrorState::Pending)
			{
				RetiredEntries.Add(Pair.Value);
			}
		}
	}

	RetiredEntries.Sort([](const FRuntimeTrackedObject& Left, const FRuntimeTrackedObject& Right)
	{
		return Left.TrackSequence > Right.TrackSequence;
	});
	for (const FRuntimeTrackedObject& Entry : RetiredEntries)
	{
		UObject* Object = Entry.Key.ResolveObjectPtrEvenIfUnreachable();
		const bool bResolved = Object != nullptr
			&& static_cast<const UObjectBase*>(Object) == Entry.ObjectIdentity;
		checkf(bResolved, TEXT("REMOVED_CLASS_RESOLVE_FAILED"));
		if (!bResolved)
		{
			FailRuntimeMirror(TEXT("REMOVED_CLASS_RESOLVE_FAILED"), Object);
			return;
		}
		// Blueprint instances share their nearest script class's type lifetime.
		// A distinct AS subclass owns a different cookie and is retired separately.
		if (UASClass::GetFirstASClass(Object->GetClass()) == RemovedClass)
		{
			// Direct AS classes keep their CDOs rooted after type replacement.
			// Discard the obsolete mirror cookie without ending the lifetime of
			// those still-live defaults or their owned archetype subobjects.
			UObject* DefaultOwner = Object;
			while (DefaultOwner != nullptr && !DefaultOwner->HasAnyFlags(RF_ClassDefaultObject))
			{
				DefaultOwner = DefaultOwner->GetOuter();
			}
			UASClass* ExactClass = Cast<UASClass>(Object->GetClass());
			UASClass* DefaultClass = DefaultOwner != nullptr ? Cast<UASClass>(DefaultOwner->GetClass()) : nullptr;
			if (ExactClass == RemovedClass && DefaultClass != nullptr && DefaultClass->IsRooted()
				&& DefaultClass->GetDefaultObject(false) == DefaultOwner
				&& Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
				&& !Object->IsUnreachable() && !Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				if (!UntrackRuntimeObjectWithMirrorForLifecycle(Object, ExactClass,
					static_cast<asITypeInfo*>(Entry.SavedTypeCookie)))
				{
					FailRuntimeMirror(TEXT("RETIRED_DEFAULT_UNTRACK_FAILED"), Object);
					return;
				}
			}
			// A same-name declaration can replace an already removed class while
			// external holders still own its old objects and module generation.
			// Native Blueprint subclasses borrow the nearest script owner's type.
			else if ((bPreserveLiveObjects || HasRetiredRuntimeType(Entry, Cast<UASClass>(RemovedClass)))
				&& !Object->IsTemplate() && !Object->IsUnreachable()
				&& !Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				FScopeLock Lock(&RuntimeTrackedObjectsMutex);
				FRuntimeTrackedObject& Current = RuntimeTrackedObjects.FindChecked(Entry.Key);
				if (!Current.RetainedModule)
				{
					Current.RetainedModule = AngelscriptRetiredModules::RetainType(static_cast<asITypeInfo*>(Current.SavedTypeCookie));
					if (!Current.RetainedModule)
					{
						FailRuntimeMirror(TEXT("RETIRED_OBJECT_MODULE_MISSING"), Object);
						return;
					}
				}
                RecordRemovedRuntimeReceiver(Object, static_cast<asITypeInfo*>(Current.SavedTypeCookie),
                    Current.RetainedModule, Current.TrackSequence);
			}
			else
			{
				DestroyTrackedRuntimeObjectExplicitly(Object);
			}
		}
	}
#endif
}

bool RuntimeDestroyObjectAdapter(UObject* Object, void* TypeCookie, uint64 Generation)
{
	checkf(IsInGameThread(), TEXT("Runtime destroy callback requires the game thread."));
	checkf(Generation == UASClass::RuntimeLifecycleGeneration,
		TEXT("Runtime destroy callback generation drifted."));
	checkf(!bRuntimeDestroyCallbackInProgress,
		TEXT("Runtime destroy callback re-entered."));
	if (Object == nullptr || TypeCookie == nullptr
		|| Generation != UASClass::RuntimeLifecycleGeneration
		|| bRuntimeDestroyCallbackInProgress || !IsRuntimeDestructorContextValid(Object, TypeCookie))
	{
		return false;
	}

	TGuardValue<bool> CallbackGuard(bRuntimeDestroyCallbackInProgress, true);
	if (IsRuntimeGcTraceEnabled())
	{
		UE_LOG(LogAngelscriptRuntimeGcLifecycle, Log,
			TEXT("SOLOAS_GC_CALLBACK_BEGIN class=%s object=%p"),
			*Object->GetClass()->GetName(), Object);
	}
	TOptional<FAngelscriptExecutionWriteScope> TypeLifetimeScope;
	if (!IsAngelscriptExecutionWriterHeldByCurrentThread())
	{
		TypeLifetimeScope.Emplace();
	}
	asITypeInfo* ClaimedTypeCookie = ClaimRuntimeTrackedMirror(
		Object, static_cast<asITypeInfo*>(TypeCookie),
		SoloASBridge::GetActiveRuntimeDeferredDestroyToken() ? ERuntimeMirrorState::TerminalDeferred : ERuntimeMirrorState::Pending,
        ERuntimeMirrorState::Dispatching);
	checkf(ClaimedTypeCookie != nullptr, TEXT("MIRROR_CLAIM_FAILED"));
	if (!CallDestructor(Object, ClaimedTypeCookie))
	{
		const bool bMirrorRecovered = RestoreRuntimeTrackedMirrorAfterDestructionFailure(
			Object,
			ClaimedTypeCookie
		);
		if (!bMirrorRecovered)
		{
			FailRuntimeMirror(TEXT("MIRROR_DESTROY_RECOVERY_FAILED"), Object);
		}
		if (IsRuntimeGcTraceEnabled())
		{
			UE_LOG(LogAngelscriptRuntimeGcLifecycle, Log,
				TEXT("SOLOAS_GC_CALLBACK_CALL_FAILED class=%s object=%p mirrorRecovered=%s"),
				*Object->GetClass()->GetName(), Object,
				bMirrorRecovered ? TEXT("true") : TEXT("false"));
		}
		return false;
	}
	if (IsRuntimeGcTraceEnabled())
	{
		UE_LOG(LogAngelscriptRuntimeGcLifecycle, Log,
			TEXT("SOLOAS_GC_CALLBACK_AS_RETURNED class=%s object=%p"),
			*Object->GetClass()->GetName(), Object);
	}
	const bool bRemoved = RemoveRuntimeTrackedMirror(
		Object,
		ClaimedTypeCookie,
		ERuntimeMirrorState::Dispatching,
		true
	);
	verifyf(bRemoved, TEXT("MIRROR_REMOVE_FAILED"));
	if (IsRuntimeGcTraceEnabled())
	{
		UE_LOG(LogAngelscriptRuntimeGcLifecycle, Log,
			TEXT("SOLOAS_GC_CALLBACK_END class=%s object=%p removed=%s"),
			*Object->GetClass()->GetName(), Object, bRemoved ? TEXT("true") : TEXT("false"));
	}
	return bRemoved;
}

void VerifyRuntimeGcLifecycleStartup()
{
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	checkf(RuntimeTrackedObjects.Num() == 0
		&& ReinstancedScriptPairsByNewObject.Num() == 0
		&& ReloadCapturesByOldIdentity.Num() == 0
		&& ScriptActorAttachmentSnapshots.Num() == 0
		&& !bRuntimeScriptReplacementCaptureActive
		&& !bRuntimeScriptReplacementCaptureAborted
		&& !bRuntimeLifecycleShuttingDown,
		TEXT("Runtime lifecycle mirror startup state is not empty."));
}

void BeginRuntimeLifecycleShutdown()
{
	checkf(IsInGameThread(), TEXT("Runtime lifecycle shutdown requires the game thread."));
	checkf(!bReinstancedScriptPairConsumerInProgress,
		TEXT("REINSTANCED_PAIR_CONSUMER_ACTIVE"));
	if (bReinstancedScriptPairConsumerInProgress)
	{
		return;
	}
	FScopeLock Lock(&RuntimeTrackedObjectsMutex);
	checkf(!bRuntimeLifecycleShuttingDown, TEXT("Runtime lifecycle shutdown started twice."));
	if (bRuntimeLifecycleShuttingDown)
	{
		FailRuntimeMirror(TEXT("TRACK_DURING_SHUTDOWN"), nullptr);
		return;
	}
	bRuntimeLifecycleShuttingDown = true;
	for (TPair<FObjectKey, FConsumedReinstancedScriptClassPair>& Pair :
		ReinstancedScriptPairsByNewObject)
	{
		if (Pair.Value.State == EReinstancedScriptPairConsumptionState::Reserved)
		{
			Pair.Value.State = EReinstancedScriptPairConsumptionState::Failed;
		}
	}
	ReloadCapturesByOldIdentity.Empty();
	ScriptActorAttachmentSnapshots.Empty();
	ActiveReloadCaptureGeneration = 0;
	bRuntimeScriptReplacementCaptureAborted = false;
	bRuntimeScriptReplacementCaptureActive = false;
}

void AbandonRuntimeTrackedObjects()
{
	checkf(IsAngelscriptExecutionWriterHeldByCurrentThread(), TEXT("Runtime mirror abandonment requires the execution writer."));
	checkf(!bReinstancedScriptPairConsumerInProgress, TEXT("REINSTANCED_PAIR_CONSUMER_ACTIVE"));
	if (bReinstancedScriptPairConsumerInProgress)
	{
		return;
	}
	TArray<FRuntimeTrackedObject> AbandonedEntries;
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		checkf(bRuntimeLifecycleShuttingDown, TEXT("Runtime mirror abandonment requires shutdown state."));
		for (const TPair<FObjectKey, FConsumedReinstancedScriptClassPair>& Pair : ReinstancedScriptPairsByNewObject)
		{
			checkf(Pair.Value.State != EReinstancedScriptPairConsumptionState::Reserved, TEXT("SHUTDOWN_REINSTANCED_PAIR_RESERVED"));
		}
		for (auto It = RuntimeTrackedObjects.CreateIterator(); It; ++It)
		{
			const FRuntimeTrackedObject& Entry = It.Value();
			checkf(Entry.State == ERuntimeMirrorState::Pending,
				TEXT("SHUTDOWN_DISPATCHING"));
			if (Entry.State == ERuntimeMirrorState::Pending
				&& Entry.RetainedModule == nullptr
				&& Entry.Key.ResolveObjectPtrEvenIfUnreachable() == nullptr)
			{
				It.RemoveCurrent();
				continue;
			}
			AbandonedEntries.Add(Entry);
		}
	}
	AbandonedEntries.Sort([](const FRuntimeTrackedObject& Left,
		const FRuntimeTrackedObject& Right) { return Left.TrackSequence > Right.TrackSequence; });
	for (const FRuntimeTrackedObject& Entry : AbandonedEntries)
	{
		UObject* Object = Entry.Key.ResolveObjectPtrEvenIfUnreachable();
		const bool bResolved = Object != nullptr && static_cast<const UObjectBase*>(Object) == Entry.ObjectIdentity;
		checkf(bResolved, TEXT("SHUTDOWN_RESOLVE_FAILED"));
		if (!bResolved)
		{
			FailRuntimeMirror(TEXT("SHUTDOWN_RESOLVE_FAILED"), Object);
			return;
		}
		checkf(!Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed), TEXT("SHUTDOWN_ALREADY_DESTROYED"));
		asITypeInfo* SavedTypeCookie = static_cast<asITypeInfo*>(Entry.SavedTypeCookie);
		verifyf(SoloASBridge::UntrackRuntimeObject(Object), TEXT("SHUTDOWN_UNTRACK_FAILED"));
		checkf(RemoveRuntimeTrackedMirror(
			Object, SavedTypeCookie, ERuntimeMirrorState::Pending),
			TEXT("MIRROR_REMOVE_FAILED"));
	}
	{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		ReinstancedScriptPairsByNewObject.Empty();
		ExplicitlyDestroyedGarbageObjects.Empty();
		ScriptActorAttachmentSnapshots.Empty();
		bRuntimeScriptReplacementCaptureAborted = false;
	}
	VerifyRuntimeTrackedObjectsAbandoned();
}

void VerifyRuntimeTrackedObjectsAbandoned()
{
		FScopeLock Lock(&RuntimeTrackedObjectsMutex);
		checkf(!bRuntimeScriptReplacementCaptureActive
			&& !bRuntimeScriptReplacementCaptureAborted
			&& ReloadCapturesByOldIdentity.Num() == 0,
			TEXT("SHUTDOWN_RELOAD_CAPTURE_ACTIVE"));
		checkf(ScriptActorAttachmentSnapshots.Num() == 0,
			TEXT("SHUTDOWN_ATTACHMENT_SNAPSHOT_ACTIVE"));
		checkf(RuntimeTrackedObjects.Num() == 0
		&& ReinstancedScriptPairsByNewObject.Num() == 0,
		TEXT("MIRROR_NOT_EMPTY"));
}
}
