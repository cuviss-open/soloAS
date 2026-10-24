#include "ClassReloadHelper.h"
                                                                                                            
#include "EdGraph/EdGraphPin.h"
#include "UObject/FieldPathProperty.h"
#include "UObject/PropertyOptional.h"
#include "UObject/NoExportTypes.h"
#include "Serialization/ArchiveUObject.h"
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "Editor/SoloASEditorFieldPathReload.h"
#endif
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
                                                                                                          
                                                                                                                  
#include "Editor/SoloASRetiredLoadingExportRepair.h"
                                                                                                                
                                                                                                                  
#include "AngelscriptRuntimeGcLifecycle.h"
                                                                                                                
                                                                                                              
#include "InstancedStructReload.h"
#include "AngelscriptReloadSafety.h"
                                                                                                            
                                                                                                                                     
#include "ClassReloadEventSignatureResolver.h"
                                                                                                                                   
#include "AngelscriptManager.h"

#include "Serialization/ArchiveReplaceObjectRef.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "BlueprintCompilationManager.h"
                                                                                                                                       
#include "Blueprint/AngelscriptRetiredClassDefaultScope.h"
#include "Blueprint/BlueprintSupport.h"
                                                                                                                                     
                                                                                                             
#include "Blueprint/AngelscriptActorReplacementTransaction.h"
                                                                                                           
#include "Elements/Framework/EngineElementsLibrary.h"
#include "DataStorage/Features.h"
#include "Elements/Columns/TypedElementHandleColumn.h"
#include "Elements/Interfaces/TypedElementDataStorageCompatibilityInterface.h"
#include "Elements/Interfaces/TypedElementDataStorageInterface.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"
#include "Kismet2/ReloadUtilities.h"
#include "Selection.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "LevelEditorSubsystem.h"
                                                                                                          
#include "Kismet2/KismetReinstanceUtilities.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectHash.h"
#include "Editor/SoloASBridgeReload.h"
#include "AngelscriptReflectionAdapter.h"
#include "UObject/ReferencerFinder.h"
#include "UObject/LinkerLoad.h"
#include "Misc/ScopeExit.h"
#include "ClassGenerator/ASClass.h"
#include "AngelscriptRuntimeGcMirrorAPI.h"
                                                                                                        
                                                                                                      
#include "Editor/SoloASScriptBlueprintDependencies.h"
#include "Extensions/AngelscriptExtensionHooks.h"
                                                                                                    
                                                                                                                    
#include "Blueprint/AngelscriptInheritedComponentAttachmentRepair.h"
#include "Elements/Framework/TypedElementRegistry.h"
                                                                                                                  
#include "IPlacementModeModule.h"

#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "ActorFactories/ActorFactoryVolume.h"
                                                                                                                              
#include "Engine/DataAsset.h"
#include "UObject/TextProperty.h"
#include "UObject/StrProperty.h"
#include "UObject/Utf8StrProperty.h"
                                                                                                                            

                                                                                                                                          
#if 0
                                                                                                                                        
// The new unreal reload system is not yet up to providing for AS reloads (for example, it does not deal well with changing UFunction definitions)
// However, you can try it out with this CVar and hopefully at some point it can replace the hacked-together mess using the old reloader.
static int32 GAngelscriptUseUnrealReload = 0;
static FAutoConsoleVariableRef CVar_AngelscriptUseUnrealReload(TEXT("angelscript.UseUnrealReload"), GAngelscriptUseUnrealReload, TEXT(""));
                                                                                                                                        
#endif
static_assert(true, "Bridge stock reload mode is unconditional.");
                                                                                                                                      

                                                                                                                   
namespace
{
int32 GReloadCaptureProbe = 1;
FAutoConsoleVariableRef CVar_ReloadCaptureProbe(
	TEXT("soloas.ReloadCaptureProbe"), GReloadCaptureProbe,
	TEXT("Use the lightweight hidden type reference probe during FullReload capture."));

bool AddDefaultSubobjectMappings(UObject* OldOuter, UObject* NewOuter,
	const TMap<UClass*, UClass*>& Classes, TMap<UObject*, UObject*>& Mappings)
{
	TArray<UObject*> OldChildren;
	GetObjectsWithOuter(OldOuter, OldChildren, EGetObjectsFlags::None);
	for (UObject* OldChild : OldChildren)
	{
		if (!OldChild->HasAnyFlags(RF_DefaultSubObject)) continue;
		UObject* NewChild = NewOuter->GetDefaultSubobjectByName(OldChild->GetFName());
		UClass* ExpectedClass = Classes.FindRef(OldChild->GetClass());
		if (ExpectedClass == nullptr) ExpectedClass = OldChild->GetClass();
		if (NewChild == nullptr || NewChild->GetOuter() != NewOuter
			|| !NewChild->HasAnyFlags(RF_DefaultSubObject) || NewChild->GetClass() != ExpectedClass)
		{
			return false;
		}
		Mappings.Add(OldChild, NewChild);
		if (!AddDefaultSubobjectMappings(OldChild, NewChild, Classes, Mappings)) return false;
	}
	return true;
}

struct FSerializedObjectState
{
	TArray<uint8> FullObject;
	FCustomVersionContainer FullObjectVersions;
	TArray<uint8> ReferenceCollector;
	FCustomVersionContainer ReferenceCollectorVersions;
};

struct FSerializedObjectReferenceSnapshot
{
	TWeakObjectPtr<UObject> Target;
	FObjectKey TargetKey;
	TArray<FName> SerializeClassNames;
	uint32 SerializeLayoutHash = 0;
	FSerializedObjectState State;
};

void HashSerializedPropertyLayout(uint32& Hash, const FProperty* Property)
{
	Hash = HashCombine(Hash, GetTypeHash(Property->GetFName()));
	Hash = HashCombine(Hash, GetTypeHash(Property->GetClass()->GetFName()));
	Hash = HashCombine(Hash, GetTypeHash(Property->GetOffset_ForInternal()));
	Hash = HashCombine(Hash, GetTypeHash(Property->GetElementSize()));
	Hash = HashCombine(Hash, GetTypeHash(Property->ArrayDim));
	if (const auto* Struct = CastField<FStructProperty>(Property))
	{
		Hash = HashCombine(Hash, GetTypeHash(Struct->Struct->GetPathName()));
		Hash = HashCombine(Hash, GetTypeHash(Struct->Struct->GetStructureSize()));
		for (TFieldIterator<FProperty> PropertyIt(Struct->Struct, EFieldIteratorFlags::ExcludeSuper);
			PropertyIt;
			++PropertyIt)
		{
			HashSerializedPropertyLayout(Hash, *PropertyIt);
		}
	}
	else if (const auto* Object = CastField<FObjectPropertyBase>(Property))
	{
		Hash = HashCombine(Hash, GetTypeHash(Object->PropertyClass->GetPathName()));
	}
	else if (const auto* Array = CastField<FArrayProperty>(Property))
	{
		HashSerializedPropertyLayout(Hash, Array->Inner);
	}
	else if (const auto* Set = CastField<FSetProperty>(Property))
	{
		HashSerializedPropertyLayout(Hash, Set->ElementProp);
	}
	else if (const auto* Map = CastField<FMapProperty>(Property))
	{
		HashSerializedPropertyLayout(Hash, Map->KeyProp);
		HashSerializedPropertyLayout(Hash, Map->ValueProp);
	}
}

TArray<FName> GetSerializedClassChain(const UClass* Class)
{
	TArray<FName> Result;
	for (const UClass* Cursor = Class; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
		Result.Add(Cursor->GetFName());
	return Result;
}

uint32 GetSerializedObjectLayoutHash(const UClass* Class)
{
	uint32 Hash = 0;
	for (const UClass* Cursor = Class; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
	{
		Hash = HashCombine(Hash, GetTypeHash(Cursor->GetPathName()));
		Hash = HashCombine(Hash, GetTypeHash(Cursor->GetPropertiesSize()));
		for (TFieldIterator<FProperty> PropertyIt(Cursor, EFieldIteratorFlags::ExcludeSuper);
			PropertyIt;
			++PropertyIt)
		{
			HashSerializedPropertyLayout(Hash, *PropertyIt);
		}
	}
	return Hash;
}

bool HasReflectedReloadReference(
	UObject* Object, const TMap<UObject*, UObject*>& Replacements)
{
	for (FPropertyValueIterator Value(FProperty::StaticClass(), Object->GetClass(), Object);
		Value;
		++Value)
	{
		if (const auto* ObjectProperty = CastField<FObjectPropertyBase>(Value.Key()))
		{
			UObject* Referenced = ObjectProperty->GetObjectPropertyValue(Value.Value());
			UObject* const* Replacement = Replacements.Find(Referenced);
			if (Replacement && *Replacement != Referenced)
			{
				return true;
			}
		}
		else if (const auto* PathProperty = CastField<FFieldPathProperty>(Value.Key()))
		{
			const FFieldPath* StoredPath = PathProperty->GetPropertyValuePtr(Value.Value());
			if (StoredPath == nullptr) continue;
			FFieldPath PathCopy(*StoredPath);
			if (FField* Field = PathCopy.GetTyped(FField::StaticClass()))
			{
				UObject* const* Replacement = Replacements.Find(Field->GetOwnerStruct());
				if (Replacement && *Replacement != Field->GetOwnerStruct()) return true;
			}
			const FString PathName = PathCopy.ToString();
			for (const auto& Pair : Replacements)
				if (Pair.Key != nullptr && Pair.Key != Pair.Value
					&& PathName.StartsWith(Pair.Key->GetPathName() + TEXT(":"))) return true;
		}
	}
	return false;
}

class FSerializedTypeReferenceWriter : public FObjectAndNameAsStringProxyArchive
{
public:
	FSerializedTypeReferenceWriter(FArchive& Inner, const TSet<UObject*>* Types)
		: FObjectAndNameAsStringProxyArchive(Inner, false), PreviousTypes(Types) {}
	using FObjectAndNameAsStringProxyArchive::operator<<;
	FArchive& operator<<(UObject*& Object) override
	{
		bContainsPreviousType |= PreviousTypes && PreviousTypes->Contains(Object);
		return FObjectAndNameAsStringProxyArchive::operator<<(Object);
	}
	FArchive& operator<<(FObjectPtr& Object) override
	{
		bContainsPreviousType |= PreviousTypes && PreviousTypes->Contains(Object.Get());
		return FObjectAndNameAsStringProxyArchive::operator<<(Object);
	}
	bool bContainsPreviousType = false;
private:
	const TSet<UObject*>* PreviousTypes;
};

bool CanSkipProbeProperty(const FProperty* Property, TSet<const FProperty*>& VisitedProperties)
{
	if (CastField<FNumericProperty>(Property) || CastField<FBoolProperty>(Property)
		|| CastField<FEnumProperty>(Property) || CastField<FNameProperty>(Property)
		|| CastField<FStrProperty>(Property) || CastField<FUtf8StrProperty>(Property)
		|| CastField<FTextProperty>(Property)
		|| CastField<FFieldPathProperty>(Property))
	{
		return true;
	}
	if (CastField<FObjectPropertyBase>(Property))
		return true;
	// The weak-inclusive native replacement pass visits these reflected references too.
	if (CastField<FDelegateProperty>(Property) || CastField<FMulticastDelegateProperty>(Property)
		|| CastField<FInterfaceProperty>(Property)) return true;
	// A property-type backedge adds no new leaf; unknown leaves still reject the closure.
	if (VisitedProperties.Contains(Property))
		return true;
	VisitedProperties.Add(Property);
	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		const auto* StructOps = (StructProperty->Struct->StructFlags & STRUCT_SerializeNative)
			? StructProperty->Struct->GetCppStructOps() : nullptr;
		// A soft-only native serializer cannot add the hidden hard/weak references this probe seeks.
		// Unknown serializers default to Conservative and keep the existing native probe.
		if ((StructProperty->Struct->StructFlags & STRUCT_SerializeNative)
			&& (StructOps == nullptr || StructOps->HasSerializerObjectReferences(
				EPropertyObjectReferenceType::Strong | EPropertyObjectReferenceType::Weak
				| EPropertyObjectReferenceType::Conservative)))
		{
			// PinType's native serializer exposes the same weak references to SerializeBin.
			// The normal weak-reference replacement pass owns those reflected values.
			if (StructProperty->Struct == FEdGraphPinType::StaticStruct()) return true;
			// These audited native serializers contain only scalar values, never hidden type references.
			const UScriptStruct* Struct = StructProperty->Struct;
			return Struct == TBaseStructure<FVector>::Get() || Struct == TBaseStructure<FVector2D>::Get()
				|| Struct == TBaseStructure<FRotator>::Get() || Struct == TBaseStructure<FQuat>::Get()
				|| Struct == TBaseStructure<FTransform>::Get() || Struct == TBaseStructure<FGuid>::Get()
				|| Struct == TBaseStructure<FColor>::Get() || Struct == TBaseStructure<FLinearColor>::Get();
		}
		for (FProperty* Field = StructProperty->Struct->PropertyLink; Field; Field = Field->PropertyLinkNext)
			if (!CanSkipProbeProperty(Field, VisitedProperties)) return false;
		return true;
	}
	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		return CanSkipProbeProperty(ArrayProperty->Inner, VisitedProperties);
	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		return CanSkipProbeProperty(SetProperty->ElementProp, VisitedProperties);
	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		return CanSkipProbeProperty(MapProperty->KeyProp, VisitedProperties)
			&& CanSkipProbeProperty(MapProperty->ValueProp, VisitedProperties);
	if (const FOptionalProperty* OptionalProperty = CastField<FOptionalProperty>(Property))
		return CanSkipProbeProperty(OptionalProperty->GetValueProperty(), VisitedProperties);
	return false;
}

bool CanSkipProbeProperty(const FProperty* Property)
{
	TSet<const FProperty*> VisitedProperties;
	return CanSkipProbeProperty(Property, VisitedProperties);
}

void AddProbeSkippableProperties(const UClass* Class,
	TSet<const UClass*>& ProcessedClasses, TSet<const FProperty*>& SkippableProperties)
{
	for (const UClass* Cursor = Class; Cursor; Cursor = Cursor->GetSuperClass())
	{
		// A processed class already completed its entire ancestor chain.
		if (ProcessedClasses.Contains(Cursor))
			break;
		ProcessedClasses.Add(Cursor);
		for (TFieldIterator<FProperty> It(Cursor, EFieldIteratorFlags::ExcludeSuper); It; ++It)
			if (CanSkipProbeProperty(*It)) SkippableProperties.Add(*It);
	}
}

class FHiddenTypeReferenceProbe : public FArchiveUObject
{
public:
	FHiddenTypeReferenceProbe(const TSet<UObject*>& InTypes,
		const TSet<const FProperty*>& InSkippableProperties, TMap<const UClass*, bool>& InClassNeedsProperties,
		bool bObjectReferenceCollector)
		: Types(InTypes), SkippableProperties(InSkippableProperties), ClassNeedsProperties(InClassNeedsProperties)
	{
		SetIsSaving(true);
		SetIsPersistent(false);
		ArNoDelta = true;
		ArIsObjectReferenceCollector = bObjectReferenceCollector;
		ArIsModifyingWeakAndStrongReferences = bObjectReferenceCollector;
	}

	virtual void Serialize(void*, int64 Length) override { Position += Length; }
	virtual int64 Tell() override { return Position; }
	virtual int64 TotalSize() override { return Position; }
	virtual void Seek(int64 InPosition) override { Position = InPosition; }
	virtual FArchive& operator<<(FName&) override { return *this; }
	virtual FArchive& operator<<(FText&) override { return *this; }
	virtual FArchive& operator<<(FField*&) override { return *this; }
	virtual FArchive& operator<<(FLazyObjectPtr&) override { return *this; }
	virtual FArchive& operator<<(FSoftObjectPath&) override { return *this; }
	virtual FArchive& operator<<(FSoftObjectPtr&) override { return *this; }
	virtual FArchive& operator<<(FWeakObjectPtr& Value) override
	{
		return FArchiveUObject::SerializeWeakObjectPtr(*this, Value);
	}
	virtual FArchive& operator<<(UObject*& Object) override
	{
		bContainsPreviousType |= Types.Contains(Object);
		return *this;
	}
	virtual FArchive& operator<<(FObjectPtr& Object) override
	{
		if (Object.IsResolved())
			bContainsPreviousType |= Types.Contains(Object.Get());
		return *this;
	}
	virtual void MarkScriptSerializationStart(const UObject* Object) override
	{
		SerializationStack.Add({Object, WantBinaryPropertySerialization(), bool(ArUseCustomPropertyList), ArCustomPropertyList});
		const UClass* Class = Object->GetClass();
		bool* NeedsProperties = ClassNeedsProperties.Find(Class);
		if (NeedsProperties == nullptr)
		{
			bool bNeedsProperties = false;
			for (TFieldIterator<FProperty> It(Class); It; ++It)
				if (!SkippableProperties.Contains(*It)) { bNeedsProperties = true; break; }
			NeedsProperties = &ClassNeedsProperties.Add(Class, bNeedsProperties);
		}
		if (*NeedsProperties) return;
		// The existing filter skips every reflected field of this class.
		// Limit the empty binary list to UObject's property phase; native custom
		// serialization before and after Super::Serialize keeps its original flags.
		SetWantBinaryPropertySerialization(true);
		ArUseCustomPropertyList = true;
		ArCustomPropertyList = nullptr;
	}
	virtual void MarkScriptSerializationEnd(const UObject* Object) override
	{
		check(SerializationStack.Num() != 0 && SerializationStack.Last().Object == Object);
		const FSerializationFrame Frame = SerializationStack.Pop();
		SetWantBinaryPropertySerialization(Frame.bBinary);
		ArUseCustomPropertyList = Frame.bCustomList;
		ArCustomPropertyList = Frame.CustomList;
	}
	virtual bool ShouldSkipProperty(const FProperty* Property) const override
	{
		if (SerializationStack.IsEmpty() || !SkippableProperties.Contains(Property))
			return false;
		const UClass* OwnerClass = Cast<UClass>(Property->GetOwnerStruct());
		return OwnerClass != nullptr
			&& SerializationStack.Last().Object->GetClass()->IsChildOf(OwnerClass);
	}

	bool ContainsPreviousType() const { return bContainsPreviousType; }

private:
	const TSet<UObject*>& Types;
	const TSet<const FProperty*>& SkippableProperties;
	TMap<const UClass*, bool>& ClassNeedsProperties;
	struct FSerializationFrame
	{
		const UObject* Object;
		bool bBinary;
		bool bCustomList;
		const FCustomPropertyListNode* CustomList;
	};
	TArray<FSerializationFrame> SerializationStack;
	int64 Position = 0;
	bool bContainsPreviousType = false;
};

bool ProbeSerializedObjectForTypeReference(UObject* Object,
	const TSet<UObject*>& PreviousTypes, const TSet<const FProperty*>& SkippableProperties,
	TMap<const UClass*, bool>& ClassNeedsProperties)
{
	FHiddenTypeReferenceProbe FullProbe(PreviousTypes, SkippableProperties, ClassNeedsProperties, false);
	Object->Serialize(FullProbe);
	if (FullProbe.ContainsPreviousType())
		return true;

	FHiddenTypeReferenceProbe CollectorProbe(PreviousTypes, SkippableProperties, ClassNeedsProperties, true);
	Object->Serialize(CollectorProbe);
	return CollectorProbe.ContainsPreviousType();
}

bool CaptureSerializedObjectState(UObject* Object, FSerializedObjectState& OutState,
	const TSet<UObject*>* PreviousTypes = nullptr, bool* ContainsPreviousType = nullptr)
{
	FMemoryWriter FullMemory(OutState.FullObject);
	FSerializedTypeReferenceWriter FullWriter(FullMemory, PreviousTypes);
	FullWriter.ArNoDelta = true;
	Object->Serialize(FullWriter);
	if (FullWriter.IsError())
	{
		return false;
	}
	OutState.FullObjectVersions = FullWriter.GetCustomVersions();

	FMemoryWriter ReferenceMemory(OutState.ReferenceCollector);
	FSerializedTypeReferenceWriter ReferenceWriter(ReferenceMemory, PreviousTypes);
	ReferenceWriter.ArNoDelta = true;
	ReferenceWriter.ArIsObjectReferenceCollector = true;
	ReferenceWriter.ArIsModifyingWeakAndStrongReferences = true;
	Object->Serialize(ReferenceWriter);
	if (ReferenceWriter.IsError())
	{
		return false;
	}
	OutState.ReferenceCollectorVersions = ReferenceWriter.GetCustomVersions();
	if (ContainsPreviousType)
		*ContainsPreviousType = FullWriter.bContainsPreviousType || ReferenceWriter.bContainsPreviousType;
	return true;
}

bool RestoreSerializedObjectStream(
	UObject* Object,
	const TArray<uint8>& Bytes,
	const FCustomVersionContainer& CustomVersions,
	bool bObjectReferenceCollector)
{
	FMemoryReader MemoryReader(Bytes);
	FObjectAndNameAsStringProxyArchive Reader(MemoryReader, false);
	Reader.ArNoDelta = true;
	Reader.ArIsObjectReferenceCollector = bObjectReferenceCollector;
	Reader.ArIsModifyingWeakAndStrongReferences = bObjectReferenceCollector;
	Reader.SetCustomVersions(CustomVersions);
	Object->Serialize(Reader);
	return !Reader.IsError() && MemoryReader.Tell() == Bytes.Num();
}

bool RestoreSerializedObjectState(UObject* Object, const FSerializedObjectState& State)
{
	return RestoreSerializedObjectStream(
		Object, State.FullObject, State.FullObjectVersions, false)
		&& RestoreSerializedObjectStream(
			Object, State.ReferenceCollector, State.ReferenceCollectorVersions, true);
}

TMap<UObject*, UObject*> MakeTypeReplacementMap(
	const TMap<UClass*, UClass*>& Classes,
	const TMap<UScriptStruct*, UScriptStruct*>& Structs,
	const TMap<UObject*, UObject*>& Objects)
{
	TMap<UObject*, UObject*> Replacements;
	for (const TPair<UClass*, UClass*>& Pair : Classes)
		if (Pair.Key && Pair.Value) Replacements.Add(Pair.Key, Pair.Value);
	for (const TPair<UScriptStruct*, UScriptStruct*>& Pair : Structs)
		if (Pair.Key && Pair.Value) Replacements.Add(Pair.Key, Pair.Value);
	for (const TPair<UObject*, UObject*>& Pair : Objects)
		if (Pair.Key && Pair.Value) Replacements.Add(Pair.Key, Pair.Value);
	return Replacements;
}

class FCustomSerializedReferenceSnapshots
{
public:
	void Capture(const TArray<UObject*>& PreviousTypes)
	{
		TSet<UObject*> RetiredObjects;
		for (UObject* Type : PreviousTypes)
			if (Type) RetiredObjects.Add(Type);
		if (RetiredObjects.IsEmpty()) return;
		TArray<FString> PreviousTypePackages;
		for (UObject* Type : RetiredObjects)
			PreviousTypePackages.AddUnique(Type->GetOutermost()->GetName());
		struct FTypePackagePaths
		{
			FString Package;
			FString ObjectPrefix;
			FString SubobjectPrefix;
		};
		TArray<FTypePackagePaths> PreviousTypePackagePaths;
		for (const FString& TypePackage : PreviousTypePackages)
			PreviousTypePackagePaths.Add({TypePackage, TypePackage + TEXT("."), TypePackage + TEXT(":")});
		TMap<UPackage*, bool> PackageMayReferencePreviousType;
		auto MayPackageReferencePreviousType = [&](UPackage* Package)
		{
			// Dirty or unlinked packages have no stable import evidence, so keep probing them.
			if (Package == nullptr || Package->IsDirty()) return true;
			if (bool* Cached = PackageMayReferencePreviousType.Find(Package)) return *Cached;
			FLinkerLoad* Linker = Package->GetLinker();
			if (Linker == nullptr) return true;
			bool bMayReferencePreviousType = false;
			for (int32 ImportIndex = 0; ImportIndex < Linker->ImportMap.Num() && !bMayReferencePreviousType; ++ImportIndex)
			{
				const FString ImportPath = Linker->GetImportPathName(ImportIndex);
				for (const FTypePackagePaths& TypePackage : PreviousTypePackagePaths)
					if (ImportPath == TypePackage.Package || ImportPath.StartsWith(TypePackage.ObjectPrefix)
						|| ImportPath.StartsWith(TypePackage.SubobjectPrefix))
					{
						bMayReferencePreviousType = true;
						break;
					}
			}
			PackageMayReferencePreviousType.Add(Package, bMayReferencePreviousType);
			return bMayReferencePreviousType;
		};
		TSet<const UClass*> ProbeClassesWithPrecomputedProperties;
		TSet<const FProperty*> ProbeSkippableProperties;
		TMap<const UClass*, bool> ProbeClassNeedsProperties;

		for (TObjectIterator<UObject> It; It; ++It)
		{
			UObject* Object = *It;
			if (!IsValid(Object) || Object->IsA<UPackage>() || Object->IsA<UStruct>()
				|| Object->HasAnyFlags(RF_NeedInitialization | RF_NeedLoad | RF_NeedPostLoad
					| RF_BeginDestroyed | RF_FinishDestroyed)
				|| Object->GetClass()->HasAnyClassFlags(CLASS_NewerVersionExists)) continue;
			if (GReloadCaptureProbe != 0)
			{
				if (!MayPackageReferencePreviousType(Object->GetOutermost()))
					continue;
				AddProbeSkippableProperties(Object->GetClass(), ProbeClassesWithPrecomputedProperties,
					ProbeSkippableProperties);
				const UClass* Class = Object->GetClass();
				if (Class->GetClass() == UClass::StaticClass()
					&& SoloASBridge::HasOnlyReflectedReferences(Object))
				{
					bool* NeedsProperties = ProbeClassNeedsProperties.Find(Class);
					if (NeedsProperties == nullptr)
					{
						bool bNeedsProperties = false;
						for (TFieldIterator<FProperty> Property(Class); Property; ++Property)
							bNeedsProperties |= !ProbeSkippableProperties.Contains(*Property);
						NeedsProperties = &ProbeClassNeedsProperties.Add(Class, bNeedsProperties);
					}
					// Audited native serializers expose all type references through reflected properties.
					// The per-object probe uses the same property filter for every omitted field.
					if (!*NeedsProperties) continue;
				}
				if (!ProbeSerializedObjectForTypeReference(Object, RetiredObjects, ProbeSkippableProperties,
					ProbeClassNeedsProperties))
					continue;
			}
			FSerializedObjectState State;
			bool ContainsPreviousType = false;
			if (!CaptureSerializedObjectState(Object, State, &RetiredObjects, &ContainsPreviousType)
				|| !ContainsPreviousType) continue;
			FSerializedObjectReferenceSnapshot& Snapshot = Snapshots.AddDefaulted_GetRef();
			Snapshot.Target = Object;
			Snapshot.TargetKey = FObjectKey(Object);
			Snapshot.SerializeClassNames = GetSerializedClassChain(Object->GetClass());
			Snapshot.SerializeLayoutHash = GetSerializedObjectLayoutHash(Object->GetClass());
			Snapshot.State = MoveTemp(State);
		}
	}

	void SelectHiddenReferences(
		const TMap<UClass*, UClass*>& Classes,
		const TMap<UScriptStruct*, UScriptStruct*>& Structs,
		const TMap<UObject*, UObject*>& Objects)
	{
		if (Snapshots.IsEmpty()) return;
		Replacements = MakeTypeReplacementMap(Classes, Structs, Objects);
		TSet<UObject*> RetiredObjects;
		for (const auto& Pair : Replacements)
			if (Pair.Key) RetiredObjects.Add(Pair.Key);
		// SerializeBin includes reflected native struct serializers (e.g. InstancedStruct).
		// Their payload migration belongs to the existing reinstancer, not state replay.
		TSet<UObject*> ReflectedReferencers;
		for (UObject* Referencer : FReferencerFinder::GetAllReferencers(RetiredObjects, &RetiredObjects))
			ReflectedReferencers.Add(Referencer);
		for (const auto& Pair : Classes)
			if (Pair.Key && Pair.Value)
			{
				AuthoritativeTypeReplacements.Add(Pair.Key);
				AuthoritativeTypeReplacements.Add(Pair.Value);
				Replacements.Add(Pair.Value, Pair.Value);
			}
		for (const auto& Pair : Structs)
			if (Pair.Key && Pair.Value)
			{
				AuthoritativeTypeReplacements.Add(Pair.Key);
				AuthoritativeTypeReplacements.Add(Pair.Value);
				Replacements.Add(Pair.Value, Pair.Value);
			}
		Snapshots.RemoveAll([&](const FSerializedObjectReferenceSnapshot& Snapshot)
		{
			UObject* Object = Snapshot.Target.Get();
			if (!IsValid(Object) || ReflectedReferencers.Contains(Object)
				|| HasReflectedReloadReference(Object, Replacements)) return true;
			FSerializedObjectState CurrentState;
			bool ContainsPreviousType = false;
			return !CaptureSerializedObjectState(Object, CurrentState, &RetiredObjects, &ContainsPreviousType)
				|| !ContainsPreviousType;
		});
	}

	void RemapTargets(const TMap<UObject*, UObject*>& ObjectReplacements)
	{
		if (ObjectReplacements.IsEmpty()) return;
		for (FSerializedObjectReferenceSnapshot& Snapshot : Snapshots)
		{
			if (UObject* Current = Snapshot.Target.Get())
			{
				if (UObject* const* Replacement = ObjectReplacements.Find(Current))
				{
					Snapshot.Target = *Replacement;
					Snapshot.TargetKey = FObjectKey(*Replacement);
				}
			}
		}
		for (const TPair<UObject*, UObject*>& Pair : ObjectReplacements)
		{
			if (Pair.Key == nullptr || Pair.Value == nullptr)
			{
				continue;
			}
			if ((Pair.Key->IsA<UClass>() || Pair.Key->IsA<UScriptStruct>())
				&& AuthoritativeTypeReplacements.Contains(Pair.Key))
			{
				continue;
			}
			Replacements.Add(Pair.Key, Pair.Value);
		}
	}

	void AddCurrentTargets(TSet<TWeakObjectPtr<UObject>>& Targets) const
	{
		for (const FSerializedObjectReferenceSnapshot& Snapshot : Snapshots)
			if (UObject* Target = Snapshot.Target.Get()) Targets.Add(TWeakObjectPtr<UObject>(Target));
	}

	void Restore()
	{
		TMap<UObject*, UObject*> ResolvedReplacements;
		for (const TPair<UObject*, UObject*>& Pair : Replacements)
		{
			// Resolve Stock's transient REPLACED classes through the final script class.
			UObject* Replacement = Pair.Value;
			TSet<UObject*> Visited;
			while (Replacement != nullptr
				&& !Visited.Contains(Replacement))
			{
				Visited.Add(Replacement);
				UObject* const* Next = Replacements.Find(Replacement);
				if (Next == nullptr || *Next == Replacement) break;
				Replacement = *Next;
			}
			if (Pair.Key != nullptr && Replacement != nullptr)
				ResolvedReplacements.Add(Pair.Key, Replacement);
		}

		const EArchiveReplaceObjectFlags Flags = EArchiveReplaceObjectFlags::DelayStart
			| EArchiveReplaceObjectFlags::IgnoreOuterRef
			| EArchiveReplaceObjectFlags::IgnoreArchetypeRef;
		for (FSerializedObjectReferenceSnapshot& Snapshot : Snapshots)
		{
			UObject* Object = Snapshot.Target.Get();
			if (!IsValid(Object) || FObjectKey(Object) != Snapshot.TargetKey
				|| Object->HasAnyFlags(RF_NeedInitialization | RF_NeedLoad | RF_NeedPostLoad
					| RF_BeginDestroyed | RF_FinishDestroyed))
			{
				continue;
			}
			const TArray<FName> CurrentClassNames = GetSerializedClassChain(Object->GetClass());
			if (CurrentClassNames != Snapshot.SerializeClassNames
				|| GetSerializedObjectLayoutHash(Object->GetClass())
					!= Snapshot.SerializeLayoutHash)
			{
				continue;
			}

			FSerializedObjectState CurrentState;
			if (!CaptureSerializedObjectState(Object, CurrentState)) continue;
			if (!RestoreSerializedObjectState(Object, Snapshot.State))
			{
				RestoreSerializedObjectState(Object, CurrentState);
				continue;
			}

			FArchiveReplaceObjectRef<UObject> SerializedReferences(Object,
				ResolvedReplacements, Flags);
			SerializedReferences.ArIsObjectReferenceCollector = false;
			SerializedReferences.SerializeSearchObject();
			FArchiveReplaceObjectRef<UObject> CollectorReferences(Object,
				ResolvedReplacements, Flags);
			CollectorReferences.ArIsObjectReferenceCollector = true;
			CollectorReferences.SerializeSearchObject();
			// Replaying and remapping serialized keys can change their cached identity.
			for (FPropertyValueIterator Value(FProperty::StaticClass(), Object->GetClass(), Object);
				Value; ++Value)
			{
				if (const auto* Map = CastField<FMapProperty>(Value.Key()))
					FScriptMapHelper(Map, const_cast<void*>(Value.Value())).Rehash();
				else if (const auto* Set = CastField<FSetProperty>(Value.Key()))
					FScriptSetHelper(Set, const_cast<void*>(Value.Value())).Rehash();
			}
		}
	}

private:
	TArray<FSerializedObjectReferenceSnapshot> Snapshots;
	TMap<UObject*, UObject*> Replacements;
	TSet<UObject*> AuthoritativeTypeReplacements;
};

TUniquePtr<FCustomSerializedReferenceSnapshots> PendingCustomSerializedReferences;

void RefreshRetainedScriptTypeReferences(const TMap<UClass*, UClass*>& Classes,
    const TMap<UScriptStruct*, UScriptStruct*>& Structs,
    const TMap<UObject*, UObject*>& Objects,
    const TSet<TWeakObjectPtr<UObject>>* CandidateObjects = nullptr)
{
    SoloASBridge::RefreshRetainedScriptTypeReferences(Classes, Structs, Objects, CandidateObjects);
}
}
                                                                                                                 
                                                                                                                     
void FClassReloadHelper::FReloadState::CaptureCustomSerializedReferences(const TArray<UObject*>& PreviousTypes)
{
	SoloASBridge::Editor::RelinkBlueprintParentScriptClasses(PreviousTypes);
	PendingCustomSerializedReferences = MakeUnique<FCustomSerializedReferenceSnapshots>();
	PendingCustomSerializedReferences->Capture(PreviousTypes);
}

void FClassReloadHelper::FReloadState::ClearCustomSerializedReferences()
{
	PendingCustomSerializedReferences.Reset();
}
                                                                                                                   

void FClassReloadHelper::FReloadState::PerformReinstance()
{
	// We never go into the reinstance path in an initial compile, there's no point
	if (!FAngelscriptManager::Get().bIsInitialCompileFinished)
		return;
                                                                                                       
	bool bNativeReinstanceCompleted = false;
	ON_SCOPE_EXIT
	{
		if (!bNativeReinstanceCompleted)
			FAngelscriptManager::Get().ReportNativeReloadFailure();
	};
                                                                                                     

                                                                                                                                    
#if 0
                                                                                                                                  
	if (GAngelscriptUseUnrealReload == 0)
	{
                                                                                                                                  
#endif
static_assert(true, "Bridge stock reload mode is unconditional.");
                                                                                                                                
                                                                                                                 
		// Script signatures use the public reference-replacement route below.
		const TMap<UDelegateFunction*, UDelegateFunction*> StockReloadDelegates;
		const ESoloASBridgeReloadResult StockReloadInputResult =
			SoloASBridge::ValidateStockReloadInputs(StockReloadDelegates, ReloadAssets);
		if (StockReloadInputResult != ESoloASBridgeReloadResult::Completed)
		{
			UE_LOG(Angelscript, Error, TEXT("Native reload input rejected: %s."),
				SoloASBridge::DescribeStockReloadResult(StockReloadInputResult));
			return;
		}
        TMap<UObject*, UObject*> DelegateReplacements;
        TSet<UObject*> RetiredSignatures;
        for (const auto& Change : ReloadDelegates)
        {
            if (!Change.Key || !Change.Value || Change.Key == Change.Value
                || Change.Key->GetOuter() != FAngelscriptManager::GetPackage()
                || Change.Value->GetOuter() != FAngelscriptManager::GetPackage()
                || !IsAngelscriptRuntimeGeneratedFunction(Change.Key)
                || !IsAngelscriptRuntimeGeneratedFunction(Change.Value)
                || !Change.Key->HasAnyFunctionFlags(FUNC_Delegate)
                || !Change.Value->HasAnyFunctionFlags(FUNC_Delegate))
            {
                UE_LOG(Angelscript, Error, TEXT("Native reload rejected an unowned delegate signature pair."));
                return;
            }
            DelegateReplacements.Add(Change.Key, Change.Value);
            RetiredSignatures.Add(Change.Key);
        }
        if (!RetiredSignatures.IsEmpty())
        {
            // Match the public engine referencer/replacement route. Only exact old
            // signatures in this batch are rewritten; ownership is never replaced.
            const TArray<UObject*> Referencers = FReferencerFinder::GetAllReferencers(RetiredSignatures, &RetiredSignatures);
            for (UObject* Referencer : Referencers)
            {
                FArchiveReplaceObjectRef<UObject> ReplaceSignatures(Referencer, DelegateReplacements,
                    EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
            }
        }
                                                                                                               
		TArray<UBlueprint*> DependencyBPs;
                                                                                                       
		TUniquePtr<SoloASBridge::Editor::FScriptBlueprintDependencyScope>
			ScriptDependencyScope =
				SoloASBridge::Editor::PinScriptBlueprintReplacements(ReloadClasses);
		if (!ScriptDependencyScope) return;
        TMap<UClass*, UClass*> PreparedBlueprintClasses;
        TMap<UObject*, UObject*> PreparedBlueprintObjects;
        TSet<UObject*> OwnedBlueprintMetadata;
        TSet<UObject*> PreparedPreviousInstances;
        if (IsAngelscriptExtensionStageRegistered(EAngelscriptExtensionStage::TypeGeneration))
        {
            FAngelscriptTypeGenerationContext Context;
            Context.Point = EAngelscriptTypeGenerationPoint::RetainPreviousInstances;
            Context.bModulesSwapped = true;
            Context.PreviousInstancesToRetain = &PreparedPreviousInstances;
            Context.PreparedBlueprintClasses = &PreparedBlueprintClasses;
            Context.PreparedBlueprintObjects = &PreparedBlueprintObjects;
            Context.OwnedBlueprintMetadata = &OwnedBlueprintMetadata;
            FString Error;
            if (!DispatchAngelscriptTypeGeneration(Context, Error))
                AngelscriptReloadSafety::StopCommittedReload(TEXT("prepared-blueprint-query"), *Error);
        }
        ReloadClasses.Append(PreparedBlueprintClasses);
        TMap<UObject*, UObject*> CombinedPreparedReferenceObjects;
        if (!PreparedBlueprintObjects.IsEmpty())
        {
            CombinedPreparedReferenceObjects = ReloadAssets;
            CombinedPreparedReferenceObjects.Append(PreparedBlueprintObjects);
        }
        const TMap<UObject*, UObject*>& ReloadReferenceObjects = PreparedBlueprintObjects.IsEmpty()
            ? ReloadAssets : CombinedPreparedReferenceObjects;
		auto HasOldScriptAncestor = [&](UBlueprint* BP)
		{
			TSet<const UClass*> Visited;
			for (const UClass* Cursor = BP != nullptr
				? BP->GeneratedClass.Get()
				: nullptr;
				Cursor != nullptr && !Visited.Contains(Cursor);
				Cursor = Cursor->GetSuperClass())
			{
				Visited.Add(Cursor);
				if (ScriptDependencyScope->IsOldScriptClass(Cursor))
					return true;
			}
			return false;
		};

                                                                                                     
                                                                                                          
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
        SoloASBridge::FEditorFieldPathReloadScope FieldPathReloadScope;
#endif
		RefreshRetainedScriptTypeReferences(ReloadClasses, ReloadStructs, ReloadReferenceObjects);
		TUniquePtr<FCustomSerializedReferenceSnapshots> CustomSerializedReferences =
			MoveTemp(PendingCustomSerializedReferences);
		if (!CustomSerializedReferences) CustomSerializedReferences = MakeUnique<FCustomSerializedReferenceSnapshots>();
		CustomSerializedReferences->SelectHiddenReferences(ReloadClasses, ReloadStructs, ReloadReferenceObjects);
		TSet<TWeakObjectPtr<UObject>> ReinstancedObjectsForPathRefresh;
		const FDelegateHandle CustomSerializeReinstanceHandle =
			FCoreUObjectDelegates::OnObjectsReinstanced.AddLambda(
				[&CustomSerializedReferences, &ReinstancedObjectsForPathRefresh](const TMap<UObject*, UObject*>& Replacements)
				{
					CustomSerializedReferences->RemapTargets(Replacements);
					for (const TPair<UObject*, UObject*>& Pair : Replacements)
						if (Pair.Value != nullptr) ReinstancedObjectsForPathRefresh.Add(TWeakObjectPtr<UObject>(Pair.Value));
				});
		const FDelegateHandle CustomSerializeReplacedHandle =
			FCoreUObjectDelegates::OnObjectsReplaced.AddLambda(
				[&CustomSerializedReferences, &ReinstancedObjectsForPathRefresh](const TMap<UObject*, UObject*>& Replacements)
				{
					CustomSerializedReferences->RemapTargets(Replacements);
					for (const TPair<UObject*, UObject*>& Pair : Replacements)
						if (Pair.Value != nullptr) ReinstancedObjectsForPathRefresh.Add(TWeakObjectPtr<UObject>(Pair.Value));
				});
		ON_SCOPE_EXIT
		{
			FCoreUObjectDelegates::OnObjectsReinstanced.Remove(CustomSerializeReinstanceHandle);
			FCoreUObjectDelegates::OnObjectsReplaced.Remove(CustomSerializeReplacedHandle);
		};
                                                                                                        
                                                                                                             
		{
			FAngelscriptRuntimeReinstanceGcScope RuntimeGcScope(ReloadClasses, true);
                                                                                                           
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
                                                                                                           
		}
                                                                                                         
                                                                                                              
        TArray<AngelscriptRuntimeGcMirror::FRuntimeScriptReplacementWitness> PayloadWitnesses;
        if (!AngelscriptRuntimeGcMirror::BeginRuntimeScriptReplacementCaptureWithWitnesses(ReloadClasses, &PayloadWitnesses))
            AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-capture"), TEXT("Replacement capture admission failed"));
        ON_SCOPE_EXIT
        {
            AngelscriptRuntimeGcMirror::EndRuntimeScriptReplacementCapture();
        };
        FInstancedStructReloadScope PayloadReloadScope(ReloadStructs, ReloadClasses, PayloadWitnesses);
        FString PayloadMigrationError;
        if (!PayloadReloadScope.Prepare(PayloadMigrationError)
            || !MigrateInstancedStructsForReload(ReloadStructs, PayloadMigrationError))
        {
            AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-prepare"), *PayloadMigrationError);
        }
                                                                                                            
		TArray<UK2Node*> AllNodes;

		// Go through all blueprints and find any that are using a struct
		// or delegate that we have replaced, and change their pins
		// to point to the new ones instead.
		{
			TMap<UObject*, UObject*> ClassReplaceList;
			for (auto& Elem : ReloadClasses)
				ClassReplaceList.Add(Elem.Key, Elem.Value);
			for (auto& Elem : ReloadStructs)
				ClassReplaceList.Add(Elem.Key, Elem.Value);

			auto ReplacePinType = [&](FEdGraphPinType& PinType) -> bool
			{
				if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
				{
					UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get());
					if (Struct == nullptr)
						return false;

					UScriptStruct** NewStruct = ReloadStructs.Find(Struct);
					if (NewStruct == nullptr)
						return false;

					PinType.PinSubCategoryObject = *NewStruct;
					return true;
				}
				else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Enum || PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
				{
					if (ReloadEnums.Contains((UEnum*)PinType.PinSubCategoryObject.Get()))
						return true;
					else
						return false;
				}
				else
				{
					return false;
				}
			};

			for (TObjectIterator<UBlueprint> BlueprintIt; BlueprintIt; ++BlueprintIt)
			{
				UBlueprint* BP = *BlueprintIt;
                                                                                                                    
                if (OwnedBlueprintMetadata.Contains(BP)) continue;
                                                                                                                  

				AllNodes.Reset();
				FBlueprintEditorUtils::GetAllNodesOfClass(BP, AllNodes);

				bool bHasDependency = false;
				for (UK2Node* Node : AllNodes)
				{
					TArray<UStruct*> Dependencies;
					if (Node->HasExternalDependencies(&Dependencies))
					{
						for (UStruct* Struct : Dependencies)
						{
                                                                                                                              
							#if 0
                                                                                                                            
							if (ReloadClasses.Contains((UClass*)Struct))
								bHasDependency = true;
                                                                                                                                    
							#endif
							if (ScriptDependencyScope->IsOldScriptClass(Struct))
								bHasDependency = true;
                                                                                                                                  
							if (ReloadStructs.Contains((UScriptStruct*)Struct))
								bHasDependency = true;

							if (bHasDependency)
								break;
						}
					}

					for (auto* Pin : Node->Pins)
					{
						bHasDependency |= ReplacePinType(Pin->PinType);
					}

					if (auto* EditableBase = Cast<UK2Node_EditablePinBase>(Node))
					{
						for (auto Desc : EditableBase->UserDefinedPins)
						{
							bHasDependency |= ReplacePinType(Desc->PinType);
						}
					}

					if (auto* Event = Cast<UK2Node_Event>(Node))
					{
                                                                                                                                            
#if 0
                                                                                                                                          
						if (auto* Function = Event->GetTiedSignatureFunction())
						{
							if (auto* DelegateFunction = Cast<UDelegateFunction>(Function))
							{
								if (NewDelegates.Contains(DelegateFunction) || ReloadDelegates.Contains(DelegateFunction))
								{
									bHasDependency = true;
								}
							}
							else if (ReloadClasses.Contains(Function->GetOwnerClass()))
							{
								bHasDependency = true;
							}
						}
                                                                                                                                                     
#endif
						if (auto* Function = ResolveClassReloadEventSignature(Event))
						{
							if (auto* DelegateFunction = Cast<UDelegateFunction>(Function))
							{
								if (NewDelegates.Contains(DelegateFunction)
									|| ReloadDelegates.Contains(DelegateFunction))
								{
									bHasDependency = true;
								}
							}
							else if (ReloadClasses.Contains(Function->GetOwnerClass()))
							{
								bHasDependency = true;
							}
						}
                                                                                                                                                   
					}

					if (auto* MacroInst = Cast<UK2Node_MacroInstance>(Node))
					{
						bHasDependency |= ReplacePinType(MacroInst->ResolvedWildcardType);
					}
				}

				for (auto& Variable : BP->NewVariables)
				{
					bHasDependency |= ReplacePinType(Variable.VarType);
				}

				// Check if the blueprint references any of our replacing classes at all
				FArchiveReplaceObjectRef<UObject> ReplaceObjectArch(
					BP, ClassReplaceList,
					EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
				if (ReplaceObjectArch.GetCount())
					bHasDependency = true;

                                                                                                                          
#if 0
                                                                                                                        
				if (bHasDependency)
					DependencyBPs.Add(BP);
                                                                                                                             
#endif
				bHasDependency |= HasOldScriptAncestor(BP);
				if (bHasDependency)
				{
					if (!ScriptDependencyScope->PinAffectedBlueprint(BP))
						continue;
					DependencyBPs.Add(BP);
				}
                                                                                                                           
			}

                                                                                                                   
            for (int32 Index = 0; Index < DependencyBPs.Num(); ++Index)
            {
                TArray<UBlueprint*> Dependents;
                FBlueprintEditorUtils::GetDependentBlueprints(DependencyBPs[Index], Dependents);
                for (UBlueprint* Dependent : Dependents)
                    if (!OwnedBlueprintMetadata.Contains(Dependent)
                        && !DependencyBPs.Contains(Dependent) && ScriptDependencyScope->PinAffectedBlueprint(Dependent))
                        DependencyBPs.Add(Dependent);
            }
                                                                                                                 

			for (auto& Struct : ReloadStructs)
			{
                                                                                                                               
#if 0
                                                                                                                             
				FStructureEditorUtils::BroadcastPreChange(Struct.Key);
                                                                                                                           
#endif
				if (UUserDefinedStruct* UserDefinedStruct = Cast<UUserDefinedStruct>(Struct.Key))
				{
					FStructureEditorUtils::BroadcastPreChange(UserDefinedStruct);
				}
				else
				{
					for (UDataTable* Table : GetTablesDependentOnStruct(Struct.Key))
					{
						Table->CleanBeforeStructChange();
					}
				}
                                                                                                                         

				// Update struct pointers in DataTable with newly generated replacements.
				TArray<UDataTable*> Tables = GetTablesDependentOnStruct(Struct.Key);
				for (UDataTable* Table : Tables)
				{
					Table->RowStruct = Struct.Value;
				}
			}
		}

                                                                                                                  
	TArray<TWeakObjectPtr<AActor>> ReloadSelectedActors;
	TArray<TWeakObjectPtr<UActorComponent>> ReloadSelectedComponents;
	auto RemoveUnregisteredSelectionHandles = [](UTypedElementSelectionSet* SelectionSet, UTypedElementRegistry* Registry)
	{
		if (!SelectionSet || !Registry) return;
		TArray<FTypedElementHandle> SelectedHandles = SelectionSet->GetSelectedElementHandles();
		TArray<FTypedElementHandle> ValidHandles;
		ValidHandles.Reserve(SelectedHandles.Num());
		bool bHasUnregisteredHandle = false;
		for (const FTypedElementHandle& Handle : SelectedHandles)
		{
			if (!Handle.IsSet() || Registry->GetRegisteredElementTypeName(Handle.GetId().GetTypeId()).IsNone())
			{
				bHasUnregisteredHandle = true;
				continue;
			}
			ValidHandles.Add(Handle);
		}
		if (!bHasUnregisteredHandle) return;

		// Selection APIs resolve handles while clearing, so rebuild the list without stale entries first.
		FTypedElementList& MutableElementList = const_cast<FTypedElementList&>(SelectionSet->GetElementList().Get());
		MutableElementList.Reset();
		const FTypedElementSelectionOptions SelectionOptions = FTypedElementSelectionOptions()
			.SetAllowHidden(true).SetAllowGroups(true).SetAllowLegacyNotifications(false).SetWarnIfLocked(false);
		SelectionSet->SetSelection(ValidHandles, SelectionOptions);
		SelectionSet->NotifyPendingChanges();
	};
	if (GEditor)
	{
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
			if (AActor* Actor = Cast<AActor>(*It)) ReloadSelectedActors.Add(Actor);
		for (FSelectionIterator It(*GEditor->GetSelectedComponents()); It; ++It)
			if (UActorComponent* Component = Cast<UActorComponent>(*It)) ReloadSelectedComponents.Add(Component);
		if (UTypedElementRegistry* Registry = UTypedElementRegistry::GetInstance())
		{
			if (USelection* SelectedActors = GEditor->GetSelectedActors())
				RemoveUnregisteredSelectionHandles(SelectedActors->GetElementSelectionSet(), Registry);
			if (USelection* SelectedComponents = GEditor->GetSelectedComponents())
				RemoveUnregisteredSelectionHandles(SelectedComponents->GetElementSelectionSet(), Registry);
		}
		if (ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
		{
			if (UTypedElementSelectionSet* SelectionSet = LevelEditorSubsystem->GetSelectionSet())
			{
				if (UTypedElementRegistry* Registry = UTypedElementRegistry::GetInstance())
				{
					RemoveUnregisteredSelectionHandles(SelectionSet, Registry);
					for (AActor* Actor : SelectionSet->GetSelectedObjects<AActor>())
						ReloadSelectedActors.AddUnique(TWeakObjectPtr<AActor>(Actor));
					for (UActorComponent* Component : SelectionSet->GetSelectedObjects<UActorComponent>())
						ReloadSelectedComponents.AddUnique(TWeakObjectPtr<UActorComponent>(Component));
				}
			}
		}
	}
	auto ClearReloadSelection = []()
	{
		if (!GEditor) return;
		const FTypedElementSelectionOptions SelectionOptions = FTypedElementSelectionOptions()
			.SetAllowHidden(true).SetAllowGroups(true).SetAllowLegacyNotifications(false).SetWarnIfLocked(false);
		if (USelection* SelectedActors = GEditor->GetSelectedActors())
			if (UTypedElementSelectionSet* ElementSet = SelectedActors->GetElementSelectionSet())
				if (ElementSet->ClearSelection(SelectionOptions)) ElementSet->NotifyPendingChanges();
		if (USelection* SelectedComponents = GEditor->GetSelectedComponents())
			if (UTypedElementSelectionSet* ElementSet = SelectedComponents->GetElementSelectionSet())
				if (ElementSet->ClearSelection(SelectionOptions)) ElementSet->NotifyPendingChanges();
		if (ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
			if (UTypedElementSelectionSet* SelectionSet = LevelEditorSubsystem->GetSelectionSet())
				if (SelectionSet->ClearSelection(SelectionOptions)) SelectionSet->NotifyPendingChanges();
		GEditor->SelectNone(false, true, false);
		GEditor->NoteSelectionChange();
	};
	ON_SCOPE_EXIT
	{
		if (!GEditor) return;
		TArray<FTypedElementHandle> SelectionHandles;
		for (const TWeakObjectPtr<AActor>& SelectedActor : ReloadSelectedActors)
			if (AActor* Actor = SelectedActor.Get()) SelectionHandles.Add(UEngineElementsLibrary::AcquireEditorActorElementHandle(Actor));
		for (const TWeakObjectPtr<UActorComponent>& SelectedComponent : ReloadSelectedComponents)
			if (UActorComponent* Component = SelectedComponent.Get())
				SelectionHandles.Add(UEngineElementsLibrary::AcquireEditorComponentElementHandle(Component));
		if (ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>())
		{
			if (UTypedElementSelectionSet* SelectionSet = LevelEditorSubsystem->GetSelectionSet())
			{
				SelectionSet->SetSelection(SelectionHandles, FTypedElementSelectionOptions());
				SelectionSet->NotifyPendingChanges();
				GEditor->NoteSelectionChange();
				return;
			}
		}
		GEditor->SelectNone(false, true, false);
		for (const TWeakObjectPtr<AActor>& SelectedActor : ReloadSelectedActors)
			if (AActor* Actor = SelectedActor.Get()) GEditor->SelectActor(Actor, true, false);
		for (const TWeakObjectPtr<UActorComponent>& SelectedComponent : ReloadSelectedComponents)
			if (UActorComponent* Component = SelectedComponent.Get()) GEditor->SelectComponent(Component, true, false);
		GEditor->NoteSelectionChange();
	};
	ClearReloadSelection();

		FAngelscriptInheritedComponentAttachmentRepairScope AttachmentRepairScope(ReloadClasses);

                                                                                                                
		// Retiring rows must release their handles before the stock replacement listener.
		// The delegate broadcasts in reverse registration order.
		const FDelegateHandle TypedElementReplacementHandle = FCoreUObjectDelegates::OnObjectsReplaced.AddLambda(
			[&ReloadSelectedActors, &ReloadSelectedComponents, &ClearReloadSelection](const TMap<UObject*, UObject*>& ReplacedObjects)
			{
				for (const TPair<UObject*, UObject*>& Pair : ReplacedObjects)
				{
					if (AActor* OldActor = Cast<AActor>(Pair.Key))
						for (TWeakObjectPtr<AActor>& SelectedActor : ReloadSelectedActors)
							if (SelectedActor.Get() == OldActor) SelectedActor = Cast<AActor>(Pair.Value);
					if (UActorComponent* OldComponent = Cast<UActorComponent>(Pair.Key))
						for (TWeakObjectPtr<UActorComponent>& SelectedComponent : ReloadSelectedComponents)
							if (SelectedComponent.Get() == OldComponent) SelectedComponent = Cast<UActorComponent>(Pair.Value);
				}
				ClearReloadSelection();
				using namespace UE::Editor::DataStorage;
				ICoreProvider* Storage = GetMutableDataStorageFeature<ICoreProvider>(StorageFeatureName);
				ICompatibilityProvider* CompatibilityProvider = GetMutableDataStorageFeature<ICompatibilityProvider>(CompatibilityFeatureName);
				if (!Storage || !CompatibilityProvider) return;
				for (const TPair<UObject*, UObject*>& Pair : ReplacedObjects)
				{
					if (!Pair.Key || Pair.Key == Pair.Value
						|| (!Pair.Key->IsA<AActor>() && !Pair.Key->IsA<UActorComponent>())) continue;
					const RowHandle Row = CompatibilityProvider->FindRowWithCompatibleObject(Pair.Key);
					if (!Storage->IsRowAssigned(Row) || !Storage->HasColumns<Compatibility::FTypedElementColumn>(Row)) continue;
					Storage->RemoveColumn<Compatibility::FTypedElementColumn>(Row);
					checkf(!Storage->HasColumns<Compatibility::FTypedElementColumn>(Row),
						TEXT("Retiring typed-element column removal must complete before object replacement"));
				}
			});
		ON_SCOPE_EXIT
		{
			FCoreUObjectDelegates::OnObjectsReplaced.Remove(TypedElementReplacementHandle);
		};

                                                                                                                     
#if 0
                                                                                                                   
		extern UNREALED_API TMap<UObject*, UObject*> GAngelscriptAdditionalReplacementObjects;
		for (auto& Elem : ReloadStructs)
		{
			if (Elem.Key != nullptr)
				GAngelscriptAdditionalReplacementObjects.Add(Elem.Key, Elem.Value);
		}
		for (auto& Elem : ReloadDelegates)
		{
			if (Elem.Key != nullptr)
				GAngelscriptAdditionalReplacementObjects.Add(Elem.Key, Elem.Value);
		}
		for (auto& Elem : ReloadAssets)
		{
			if (Elem.Key != nullptr)
				GAngelscriptAdditionalReplacementObjects.Add(Elem.Key, Elem.Value);
		}

		// Call into unreal's standard reinstancing system to
		// actually recreate objects using the old classes.
		FDelegateHandle ReinstanceHandle = FCoreUObjectDelegates::OnObjectsReinstanced.AddStatic(&FClassReloadHelper::HandleObjectReinstancing);
		FBlueprintCompilationManager::ReparentHierarchies(ReloadClasses);
		FCoreUObjectDelegates::OnObjectsReinstanced.Remove(ReinstanceHandle);

		GAngelscriptAdditionalReplacementObjects.Reset();
                                                                                                                              
#endif
        if (!PreparedBlueprintObjects.IsEmpty())
            FCoreUObjectDelegates::OnObjectsReplaced.Broadcast(PreparedBlueprintObjects);
		TSet<UClass*> PreReloadReinstancedScriptClasses;
		FDelegateHandle ReinstanceHandle =
			FCoreUObjectDelegates::OnObjectsReinstanced.AddStatic(&FClassReloadHelper::HandleObjectReinstancing);
		auto GetReloadClassDepth = [](const UClass* Class)
		{
			int32 Depth = 0;
			for (const UStruct* SuperStruct = Class != nullptr
				? Class->GetSuperStruct()
				: nullptr;
				SuperStruct != nullptr;
				SuperStruct = SuperStruct->GetSuperStruct())
			{
				++Depth;
			}
			return Depth;
		};

		bool bQueuedOldASAncestorBPs = false;
		for (UBlueprint* BP : DependencyBPs)
		{
			if (HasOldScriptAncestor(BP))
			{
				FBlueprintCompilationManager::QueueForCompilation(BP);
				bQueuedOldASAncestorBPs = true;
			}
		}
		if (bQueuedOldASAncestorBPs)
		{
			FBlueprintCompilationManager::FlushCompilationQueueAndReinstance();
		}

		TMap<UClass*, UClass*> NonActorReloadClasses;
		TMap<UClass*, TMap<UObject*, UObject*>> AssetTemplateMappings;
		TSet<UObject*> AssetInstancesAlreadyCreated;
		for (const TPair<UObject*, UObject*>& Asset : ReloadAssets)
		{
			if (Asset.Key == nullptr || Asset.Value == nullptr) continue;
			UClass* OldAssetClass = Asset.Key->GetClass();
			UClass* NewAssetClass = Asset.Value->GetClass();
			if (UClass** ReloadedAssetClass = ReloadClasses.Find(OldAssetClass);
				ReloadedAssetClass != nullptr && *ReloadedAssetClass == NewAssetClass)
			{
				NonActorReloadClasses.Add(OldAssetClass, NewAssetClass);
				AssetInstancesAlreadyCreated.Add(Asset.Key);
				TMap<UObject*, UObject*>& TemplateMappings = AssetTemplateMappings.FindOrAdd(OldAssetClass);
				if (TemplateMappings.IsEmpty())
				{
					UObject* OldCDO = OldAssetClass->GetDefaultObject(false);
					UObject* NewCDO = NewAssetClass->GetDefaultObject(false);
					if (OldCDO == nullptr || NewCDO == nullptr
						|| !OldCDO->HasAnyFlags(RF_ClassDefaultObject)
						|| !NewCDO->HasAnyFlags(RF_ClassDefaultObject))
					{
						AngelscriptReloadSafety::StopCommittedReload(TEXT("asset-default-map"), TEXT("Asset class defaults are unavailable"));
					}
					TemplateMappings.Add(OldCDO, NewCDO);
					if (!AddDefaultSubobjectMappings(OldCDO, NewCDO, ReloadClasses, TemplateMappings))
					{
						AngelscriptReloadSafety::StopCommittedReload(TEXT("asset-default-map"), TEXT("Asset class default subobjects do not match"));
					}
				}
				TemplateMappings.Add(Asset.Key, Asset.Value);
			}
		}

		TSet<UObject*> PreviousInstancesToRetain = PreparedPreviousInstances;
		if (IsAngelscriptExtensionStageRegistered(EAngelscriptExtensionStage::TypeGeneration))
		{
			FAngelscriptTypeGenerationContext Context;
			Context.Point = EAngelscriptTypeGenerationPoint::RetainPreviousInstances;
			Context.PreviousInstancesToRetain = &PreviousInstancesToRetain;
			FString Error;
			if (!DispatchAngelscriptTypeGeneration(Context, Error))
				AngelscriptReloadSafety::StopCommittedReload(TEXT("retained-instance-query"), *Error);
		}

		TArray<TPair<UClass*, UClass*>> ReloadPairs;
		for (const TPair<UClass*, UClass*>& ReloadClass : ReloadClasses)
		{
			if (ReloadClass.Key != nullptr && ReloadClass.Value != nullptr
				&& !NonActorReloadClasses.Contains(ReloadClass.Key)
				&& !PreparedBlueprintClasses.Contains(ReloadClass.Key))
			{
				// Preserve the established hierarchy migration for ordinary classes.
				// Only classes with literal asset replacements use the batch path below.
				ReloadPairs.Add(ReloadClass);
			}
		}
		ReloadPairs.Sort([&GetReloadClassDepth](
			const TPair<UClass*, UClass*>& Left,
			const TPair<UClass*, UClass*>& Right)
		{
			const int32 LeftDepth = GetReloadClassDepth(Left.Key);
			const int32 RightDepth = GetReloadClassDepth(Right.Key);
			if (LeftDepth != RightDepth)
			{
				return LeftDepth > RightDepth;
			}

			const int32 NameOrder = Left.Key->GetFName().Compare(
				Right.Key->GetFName());
			if (NameOrder != 0)
			{
				return NameOrder < 0;
			}
			return Left.Key->GetPathName().Compare(
				Right.Key->GetPathName()) < 0;
		});
		TSet<UObject*> ReinstancedLiteralAssets;
		FDelegateHandle LiteralAssetMapHandle;
		if (!ReloadAssets.IsEmpty())
		{
			LiteralAssetMapHandle = FCoreUObjectDelegates::OnObjectsReinstanced.AddLambda(
				[&](const TMap<UObject*, UObject*>& ReinstancedObjects)
				{
					for (const TPair<UObject*, UObject*>& Asset : ReloadAssets)
					{
						if (UObject* const* NewAsset = ReinstancedObjects.Find(Asset.Key);
							NewAsset != nullptr && *NewAsset == Asset.Value)
						{
							ReinstancedLiteralAssets.Add(Asset.Key);
						}
					}
				});
		}
		ON_SCOPE_EXIT
		{
			if (LiteralAssetMapHandle.IsValid())
			{
				FCoreUObjectDelegates::OnObjectsReinstanced.Remove(LiteralAssetMapHandle);
			}
		};

		while (ReloadPairs.Num() != 0)
		{
			TMap<UClass*, UClass*> ReloadBatch;
			TArray<TPair<UClass*, UClass*>> RemainingPairs;
			for (const TPair<UClass*, UClass*>& ReloadPair : ReloadPairs)
			{
				bool bConflictsWithBatchHierarchy = false;
				for (const TPair<UClass*, UClass*>& BatchPair : ReloadBatch)
				{
					if (ReloadPair.Key->IsChildOf(BatchPair.Key)
						|| BatchPair.Key->IsChildOf(ReloadPair.Key))
					{
						bConflictsWithBatchHierarchy = true;
						break;
					}
				}

				if (bConflictsWithBatchHierarchy)
				{
					RemainingPairs.Add(ReloadPair);
				}
				else
				{
					ReloadBatch.Add(ReloadPair.Key, ReloadPair.Value);
				}
			}

			if (ReloadBatch.Num() != 0)
			{
				TArray<UClass*> ScriptCarriersInBatch;
				for (const TPair<UClass*, UClass*>& ReloadPair : ReloadBatch)
				{
					const UASClass* NewScriptClass = Cast<UASClass>(ReloadPair.Value);
					if (Cast<UASClass>(ReloadPair.Key) != nullptr
						&& ReloadPair.Key->GetDefaultObject(false) != nullptr
						&& NewScriptClass != nullptr && NewScriptClass->HasValidScriptClassCarrier())
					{
						ScriptCarriersInBatch.Add(ReloadPair.Key);
					}
				}
				{
					FAngelscriptRetiredClassDefaultScope RetiredDefaults(ReloadBatch);
					int32 HierarchyFailureDetail = 0;
					const ESoloASBridgeReloadResult HierarchyResult = SoloASBridge::RunWithStockCopyPropertiesScope([&]()
					{
						FSoloASRetiredLoadingExportRepair LoadingExports;
						FString TransactionError;
						bool bRetainsActorInBatch = false;
						for (UObject* Previous : PreviousInstancesToRetain)
							if (Previous->IsA<AActor>() && ReloadBatch.Contains(Previous->GetClass()))
								bRetainsActorInBatch = true;
						// The extension transaction owns retained Actors through its commit.
						// Stock hierarchy preparation honors the temporary instance exclusion.
						if (bRetainsActorInBatch || !AngelscriptActorReplacementTransaction::Run(ReloadBatch, TransactionError))
						{
							// Stock collects superseded instances into both exclusion sets.
							TArray<UObject*> MarkedPreviousInstances;
							for (UObject* Previous : PreviousInstancesToRetain)
								if (!Previous->HasAnyFlags(RF_NewerVersionExists))
								{
									Previous->SetFlags(RF_NewerVersionExists);
									MarkedPreviousInstances.Add(Previous);
								}
							ON_SCOPE_EXIT
							{
								for (UObject* Previous : MarkedPreviousInstances)
									Previous->ClearFlags(RF_NewerVersionExists);
							};
							FBlueprintSupport::ReparentHierarchies(ReloadBatch);
						}
					}, &HierarchyFailureDetail);
					if (HierarchyResult != ESoloASBridgeReloadResult::Completed)
					{
						// Earlier hierarchy preparation has changed live objects; no rollback is available.
						UE_LOG(Angelscript, Fatal, TEXT("Script hierarchy guard failed: %s (%d)"),
							SoloASBridge::DescribeStockReloadResult(HierarchyResult), HierarchyFailureDetail);
					}
				}
				for (UClass* ScriptClass : ScriptCarriersInBatch)
				{
				PreReloadReinstancedScriptClasses.Add(ScriptClass);
				}
			}
			ReloadPairs = MoveTemp(RemainingPairs);
		}
		if (!NonActorReloadClasses.IsEmpty())
		{
			FAngelscriptRetiredClassDefaultScope RetiredDefaults(NonActorReloadClasses);
			FSoloASRetiredLoadingExportRepair LoadingExports;
			FReplaceInstancesOfClassParameters Parameters;
			TSet<UObject*> ObjectsThatShouldUseOldStuff;
			TSet<UObject*> InstancesThatShouldUseOldClass = AssetInstancesAlreadyCreated;
			ObjectsThatShouldUseOldStuff.Append(PreviousInstancesToRetain);
			InstancesThatShouldUseOldClass.Append(PreviousInstancesToRetain);
			Parameters.OldToNewTemplates = &AssetTemplateMappings;
			Parameters.ObjectsThatShouldUseOldStuff = &ObjectsThatShouldUseOldStuff;
			Parameters.InstancesThatShouldUseOldClass = &InstancesThatShouldUseOldClass;
			Parameters.bReplaceReferencesToOldClasses = true;
			Parameters.bReplaceReferencesToOldCDOs = true;
			int32 ReinstanceFailureDetail = 0;
			const ESoloASBridgeReloadResult ReinstanceResult = SoloASBridge::RunWithStockCopyPropertiesScope([&]()
			{
				FBlueprintCompileReinstancer::BatchReplaceInstancesOfClass(NonActorReloadClasses, Parameters);
			}, &ReinstanceFailureDetail);
			if (ReinstanceResult != ESoloASBridgeReloadResult::Completed)
			{
				UE_LOG(Angelscript, Fatal, TEXT("Script asset reinstance guard failed: %s (%d)"),
					SoloASBridge::DescribeStockReloadResult(ReinstanceResult), ReinstanceFailureDetail);
			}
			for (const TPair<UClass*, UClass*>& ReloadClass : NonActorReloadClasses)
			{
				PreReloadReinstancedScriptClasses.Add(ReloadClass.Key);
			}
		}
		ScriptDependencyScope->RefreshTransientBlueprintDefaults();
		FCoreUObjectDelegates::OnObjectsReinstanced.Remove(ReinstanceHandle);
                                                                                                                            

		// Do a full-on garbage collection step to make sure old stuff is gone
		// by the time we recompile blueprints below.
                                                                                                                
		{
			FAngelscriptRuntimeReinstanceGcScope RuntimeGcScope(ReloadClasses);
                                                                                                              
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
                                                                                                              
		}
                                                                                                            

		// Make sure all blueprints that had dependencies to structs or delegates
		// are now properly recompiled.
		if (DependencyBPs.Num() != 0)
		{
			TSet<UClass*> NewlyCreatedClasses;
			for (auto& Elem : ReloadClasses)
				NewlyCreatedClasses.Add(Elem.Value);
			TSet<UScriptStruct*> NewlyCreatedStructs;
			for (auto& Elem : ReloadStructs)
				NewlyCreatedStructs.Add(Elem.Value);

			// Refresh nodes in blueprint graphs that depend on stuff we've reloaded.
			// If we don't do this then we will get errors until the nodes are manually refreshed!
			auto RefreshRelevantNodesInBP = [&](UBlueprint* BP)
			{
				AllNodes.Reset();
				FBlueprintEditorUtils::GetAllNodesOfClass(BP, AllNodes);

				auto CheckRefresh = [&](FEdGraphPinType& PinType) -> bool
				{
					if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
					{
						UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get());
						return NewlyCreatedStructs.Contains(Struct);
					}
					else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Enum || PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
					{
						if (ReloadEnums.Contains((UEnum*)PinType.PinSubCategoryObject.Get()))
							return true;
						else
							return false;
					}
					else
					{
						return false;
					}
				};

				for (UK2Node* Node : AllNodes)
				{
					TArray<UStruct*> Dependencies;
					bool bShouldRefresh = false;

					if (Node->HasExternalDependencies(&Dependencies))
					{
						for (UStruct* Struct : Dependencies)
						{
							if (NewlyCreatedClasses.Contains((UClass*)Struct))
							{
								bShouldRefresh = true;
								break;
							}
							if (NewlyCreatedStructs.Contains((UScriptStruct*)Struct))
							{
								bShouldRefresh = true;
								break;
							}
						}
					}

					if (NewlyCreatedStructs.Num() != 0 && !bShouldRefresh)
					{
						for (auto* Pin : Node->Pins)
						{
							bShouldRefresh |= CheckRefresh(Pin->PinType);
						}
					}

					if (auto* EditableBase = Cast<UK2Node_EditablePinBase>(Node))
					{
						for (auto Desc : EditableBase->UserDefinedPins)
						{
							bShouldRefresh |= CheckRefresh(Desc->PinType);
						}
					}

					if (auto* Event = Cast<UK2Node_Event>(Node))
					{
                                                                                                                                    
#if 0
                                                                                                                                  
						if (auto* Function = Event->GetTiedSignatureFunction())
						{
							if (auto* DelegateFunction = Cast<UDelegateFunction>(Function))
							{
								if (NewDelegates.Contains(DelegateFunction) || ReloadDelegates.Contains(DelegateFunction))
								{
									bShouldRefresh = true;
								}
							}
							else if (NewlyCreatedClasses.Contains(Function->GetOwnerClass()))
							{
								bShouldRefresh = true;
							}
						}
                                                                                                                                             
#endif
						if (auto* Function = ResolveClassReloadEventSignature(Event))
						{
							if (auto* DelegateFunction = Cast<UDelegateFunction>(Function))
							{
								if (NewDelegates.Contains(DelegateFunction)
									|| ReloadDelegates.Contains(DelegateFunction))
								{
									bShouldRefresh = true;
								}
							}
							else if (NewlyCreatedClasses.Contains(Function->GetOwnerClass()))
							{
								bShouldRefresh = true;
							}
						}
                                                                                                                                           
					}

					if (auto* MacroInst = Cast<UK2Node_MacroInstance>(Node))
					{
						bShouldRefresh |= CheckRefresh(MacroInst->ResolvedWildcardType);
					}

					if (bShouldRefresh)
					{
						const UEdGraphSchema* Schema = Node->GetGraph()->GetSchema();
						Schema->ReconstructNode(*Node, true);
					}
				}
			};

			// Trigger a compile of all blueprints that we detected dependencies to our class in
			for (UBlueprint* BP : DependencyBPs)
			{
				RefreshRelevantNodesInBP(BP);
				FBlueprintCompilationManager::QueueForCompilation(BP);
			}

			FBlueprintCompilationManager::FlushCompilationQueueAndReinstance();
		}

		for (auto& Struct : ReloadStructs)
		{
                                                                                                                                
#if 0
                                                                                                                              
			FStructureEditorUtils::BroadcastPostChange(Struct.Value);
                                                                                                                            
#endif
			if (UUserDefinedStruct* UserDefinedStruct = Cast<UUserDefinedStruct>(Struct.Value))
			{
				FStructureEditorUtils::BroadcastPostChange(UserDefinedStruct);
			}
			else
			{
				for (UDataTable* Table : GetTablesDependentOnStruct(Struct.Value))
				{
					Table->RestoreAfterStructChange();
				}
			}
                                                                                                                          
		}
                                                                                                                                         
#if 0
                                                                                                                                       
	}
	else
	{
                                                                                                                                       
#endif
static_assert(true, "Bridge stock reload mode is unconditional.");
                                                                                                                                     
                                                                                                              
        if (!PayloadReloadScope.Publish(PayloadMigrationError))
            AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-publish"), *PayloadMigrationError);
                                                                                                            
		TMap<UClass*, UClass*> StockReloadClasses = ReloadClasses;
		for (const auto& PreparedClass : PreparedBlueprintClasses)
			StockReloadClasses.Remove(PreparedClass.Key);
		for (UClass* ScriptClass : PreReloadReinstancedScriptClasses)
		{
			StockReloadClasses.Remove(ScriptClass);
		}
		const bool bRunStockReloadGC = !StockReloadClasses.IsEmpty()
			|| !ReloadStructs.IsEmpty() || !ReloadEnums.IsEmpty();
		// Do a full-on garbage collection step to make sure old stuff is gone
		// before we start reinstancing things we no longer need.
		if (bRunStockReloadGC)
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);

		// Call into unreal's standard reinstancing system to
		// actually recreate objects using the old classes.
                                                                                                                  
#if 0
                                                                                                                
		auto Reload = new FReload(EActiveReloadType::Reinstancing, TEXT("Angelscript"), *GLog);

		for (auto& ReloadInfo : ReloadStructs)
			Reload->NotifyChange(ReloadInfo.Value, ReloadInfo.Key);
		for (auto& ReloadInfo : ReloadClasses)
			Reload->NotifyChange(ReloadInfo.Value, ReloadInfo.Key);
		for (UEnum* ReloadEnum : ReloadEnums)
			Reload->NotifyChange(ReloadEnum, ReloadEnum);

		Reload->Reinstance();
		Reload->Finalize();
		delete Reload;
                                                                                                                   
#endif
		// Generated script instances were already replaced by the hierarchy pass.
		// Keep other replacement domains on their existing stock reload route.
		int32 OwnerFailureDetail = 0;
		FDelegateHandle StockReloadReinstanceHandle = FCoreUObjectDelegates::OnObjectsReinstanced.AddStatic(
			&FClassReloadHelper::HandleObjectReinstancing);
		const ESoloASBridgeReloadResult ReloadResult =
			[&]()
			{
				return SoloASBridge::ReinstanceWithStockReload(
					StockReloadClasses,
					ReloadStructs,
					ReloadEnums,
					StockReloadDelegates,
					ReloadAssets,
					&OwnerFailureDetail,
					bRunStockReloadGC
				);
			}();
		if (ReloadResult != ESoloASBridgeReloadResult::Completed)
		{
			FCoreUObjectDelegates::OnObjectsReinstanced.Remove(StockReloadReinstanceHandle);
			UE_LOG(
				Angelscript,
				Error,
				TEXT("Reload stopped at the stock reinstance stage: %s (guard code %d)."),
				SoloASBridge::DescribeStockReloadResult(ReloadResult), OwnerFailureDetail
			);
			AngelscriptReloadSafety::StopCommittedReload(TEXT("payload-stock-reload"), TEXT("Final stock reinstance failed after payload publication"));
		}
                                                                                                                 
                                                                                                                                         
#if 0
                                                                                                                                       
	}
                                                                                                                                       
#endif
static_assert(true, "Bridge stock reload mode is unconditional.");
                                                                                                                                     

                                                                                                             
    // Replay only stable owners that still had non-reflected references after the reflected path pass.
    CustomSerializedReferences->Restore();
    // Reinstancing and custom-state replay may have introduced paths only on these changed objects.
	CustomSerializedReferences->AddCurrentTargets(ReinstancedObjectsForPathRefresh);
	for (const TPair<UObject*, UObject*>& Asset : ReloadAssets)
		if (Asset.Value != nullptr) ReinstancedObjectsForPathRefresh.Add(TWeakObjectPtr<UObject>(Asset.Value));
    RefreshRetainedScriptTypeReferences(ReloadClasses, ReloadStructs, ReloadReferenceObjects,
		&ReinstancedObjectsForPathRefresh);
    TMap<UObject*, UObject*> PendingAssetNotifications;
    for (const TPair<UObject*, UObject*>& Asset : ReloadAssets)
    {
        if (Asset.Key != nullptr && !ReinstancedLiteralAssets.Contains(Asset.Key))
        {
            PendingAssetNotifications.Add(Asset.Key, Asset.Value);
        }
    }
    if (!PendingAssetNotifications.IsEmpty())
    {
        FCoreUObjectDelegates::OnObjectsReinstanced.Broadcast(PendingAssetNotifications);
    }
    if (StockReloadReinstanceHandle.IsValid())
    {
        FCoreUObjectDelegates::OnObjectsReinstanced.Remove(StockReloadReinstanceHandle);
    }
                                                                                                           

	// We want to force-update all the property editing UI now that we've done this reload.
	//  The easiest way to do that is to send this NotifyCustomizationModuleChanged, since
	//  all this does is a refresh on the UI, but there's no separate 'force refresh'.
	FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
	if (PropertyModule)
		PropertyModule->NotifyCustomizationModuleChanged();

	// If we've created any new volumes, we want to make sure they have actor factories
	TArray<UClass*> NewVolumeClasses;
	for (UClass* NewClass : NewClasses)
	{
		if (NewClass != nullptr && NewClass->IsChildOf(AVolume::StaticClass()))
			NewVolumeClasses.Add(NewClass);
	}

	// If we reparented an existing class to be a volume now, we also need to create a volume actor factory for it
	for (auto ReloadElem : ReloadClasses)
	{
		UClass* OldClass = ReloadElem.Key;
		const bool bOldClassIsVolume = OldClass->IsChildOf(AVolume::StaticClass());
		UClass* NewClass = ReloadElem.Value;
		const bool bNewClassIsVolume = NewClass->IsChildOf(AVolume::StaticClass());

		if (bNewClassIsVolume && !bOldClassIsVolume)
		{
			// Create a volume actor factory for the new class
			NewVolumeClasses.Add(NewClass);
		}
		else if (!bNewClassIsVolume && bOldClassIsVolume)
		{
			// Delete the volume actor factory for the class that is no longer a volume
			for (int i = 0, Count = GEditor->ActorFactories.Num(); i < Count; ++i)
			{
				if (GEditor->ActorFactories[i]->NewActorClass == OldClass || GEditor->ActorFactories[i]->NewActorClass == NewClass)
				{
					GEditor->ActorFactories.RemoveAt(i);
					--i; --Count;
				}
			}
		}
	}

	if (NewVolumeClasses.Num() != 0)
	{
		TArray<UClass*> VolumeFactoryClasses;

		// Find all actor factories we use for volumes
		for (TObjectIterator<UClass> ObjectIt; ObjectIt; ++ObjectIt)
		{
			UClass* TestClass = *ObjectIt;
			if (TestClass->IsChildOf(UActorFactoryVolume::StaticClass()) && !TestClass->HasAnyClassFlags(CLASS_Abstract))
				VolumeFactoryClasses.Add(TestClass);
		}

		// Generate factories for the volume actor we just created
		for (UClass* VolumeFactoryClass : VolumeFactoryClasses)
		{
			for (UClass* VolumeClass : NewVolumeClasses)
			{
				UActorFactory* NewFactory = NewObject<UActorFactory>(GetTransientPackage(), VolumeFactoryClass);
				check(NewFactory);
				NewFactory->NewActorClass = VolumeClass;
				GEditor->ActorFactories.Add(NewFactory);
			}
		}
	}

	// Refresh actions for enums
	auto& Database = FBlueprintActionDatabase::Get();
	for (UEnum* ChangedEnum : ReloadEnums)
		Database.RefreshAssetActions(ChangedEnum);
	for (UEnum* ChangedEnum : NewEnums)
		Database.RefreshAssetActions(ChangedEnum);

	// If we added any new classes, reflesh the placement mode list so the actors are added right away
	if (NewClasses.Num() != 0)
	{
		IPlacementModeModule::Get().OnAllPlaceableAssetsChanged().Broadcast();
		IPlacementModeModule::Get().OnPlaceableItemFilteringChanged().Broadcast();
	}

	// Refresh struct hierarchy dropdowns if any structs were reloaded
                                                                                                                                    
#if 0
                                                                                                                                  
	if (!ReloadStructs.IsEmpty())
	{
		extern STRUCTVIEWER_API void AS_RefreshStructHierarchy();
		AS_RefreshStructHierarchy();
	}
                                                                                                                            
#endif
static_assert(true, "SoloASBridge FReload publishes the stock ReloadCompleteDelegate.");
                                                                                                                          
                                                                                                           
    // Only retire the checked AS signature pairs after all native consumers migrate.
    for (const auto& Change : ReloadDelegates)
    {
        if (Change.Key && Change.Key != Change.Value
            && Change.Key->GetOuter() == FAngelscriptManager::GetPackage()
            && IsAngelscriptRuntimeGeneratedFunction(Change.Key))
        {
            if (Change.Key->IsRooted()) Change.Key->RemoveFromRoot();
            Change.Key->ClearFlags(RF_Public | RF_Standalone);
        }
    }
	bNativeReinstanceCompleted = true;
                                                                                                         
}

void FClassReloadHelper::HandleObjectReinstancing(const TMap<UObject*, UObject*>& ReinstancedObjects)
{
	// Workaround to replace references to open assets in the asset editor subsystem.
	// Without this the asset editor subsystem loses track of the asset<->editor association,
	// which can cause duplicate editor windows for the same asset.
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	for (auto Elem : ReinstancedObjects)
	{
		if (Elem.Value == nullptr)
			continue;
		if (Elem.Value->IsTemplate())
			continue;

		if (AssetEditorSubsystem != nullptr && Elem.Value->IsA<UDataAsset>())
		{
			auto Editors = AssetEditorSubsystem->FindEditorsForAsset(Elem.Key);
			for (auto* EditorInstance : Editors)
			{
				AssetEditorSubsystem->NotifyAssetClosed(Elem.Key, EditorInstance);
				AssetEditorSubsystem->NotifyAssetOpened(Elem.Value, EditorInstance);
			}
		}
	}
}
