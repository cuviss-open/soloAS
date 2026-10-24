// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "EditorObject/AngelscriptEditorObjectPrecreateImportAdapter.h"

#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "ClassGenerator/ASClass.h"

#include "BSPOps.h"
#include "Components/ActorComponent.h"
#include "Components/BrushComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "Factories/ModelFactory.h"
#include "FoliageType.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "GameFramework/Volume.h"
#include "Model.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/CoreMisc.h"
#include "Misc/FeedbackContext.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/Class.h"
#include "UObject/OverridableManager.h"
#include "UObject/OverriddenPropertySet.h"
#include "UObject/Package.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace
{
const EPropertyFlags UnsupportedPropertyFlags =
	CPF_Transient
	| CPF_EditConst
	| CPF_DisableEditOnInstance
	| CPF_DisableEditOnTemplate
	| CPF_InstancedReference
	| CPF_ContainsInstancedReference
	| CPF_ExportObject
	| CPF_PersistentInstance;
}

const TCHAR* FAngelscriptEditorObjectPrecreateImportAdapter::GetMarker()
{
	// EDITOR_OBJECT_PRECREATE_IMPORT_ADAPTER_MARKER identifies this explicit adapter.
	return TEXT("AngelscriptEditorObjectPrecreateImportAdapter");
}

EAngelscriptEditorObjectImportResult
FAngelscriptEditorObjectPrecreateImportAdapter::ValidatePreCreate(
	const FAngelscriptEditorObjectPreCreateRequest& Request)
{
	if (Request.Class == nullptr)
		return EAngelscriptEditorObjectImportResult::NullClass;
	if (Request.Template == nullptr)
		return EAngelscriptEditorObjectImportResult::NullTemplate;
	if (Request.Outer == nullptr)
		return EAngelscriptEditorObjectImportResult::NullOuter;
	if (Request.Name.IsNone())
		return EAngelscriptEditorObjectImportResult::EmptyName;
	if (!IsValid(Request.Class))
		return EAngelscriptEditorObjectImportResult::InvalidClass;
	if (Request.Class->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
		return EAngelscriptEditorObjectImportResult::InvalidClass;
	if (!IsValid(Request.Outer) || Request.Outer->HasAnyFlags(RF_Transient))
		return EAngelscriptEditorObjectImportResult::InvalidOuter;
	if (!Request.Outer->IsA(Request.Class->ClassWithin))
		return EAngelscriptEditorObjectImportResult::InvalidOuter;
	if (!IsValid(Request.Template))
		return EAngelscriptEditorObjectImportResult::InvalidTemplate;
	if (Request.Template->HasAnyFlags(RF_Transient))
		return EAngelscriptEditorObjectImportResult::TransientTemplate;
	if (Request.Template->GetClass() != Request.Class)
		return EAngelscriptEditorObjectImportResult::MismatchedTemplateClass;
	if (StaticFindObjectFast(UObject::StaticClass(), Request.Outer, Request.Name) != nullptr)
		return EAngelscriptEditorObjectImportResult::DuplicateName;
	return EAngelscriptEditorObjectImportResult::Completed;
}

namespace
{
// The unexported editor token helpers use two public Command calls and restore
// the cursor on a failed pair. Preserve that behavior for every dispatch probe.
bool ConsumeImportToken(const TCHAR*& Text, const TCHAR* Verb, const TCHAR* Kind)
{
	const TCHAR* Probe = Text;
	if (!FParse::Command(&Probe, Verb) || !FParse::Command(&Probe, Kind))
		return false;
	Text = Probe;
	return true;
}

// UnrealEd ParamParser's vector/rotator helpers are not exported. Match their
// reset, named-field and comma-field behavior using public FParse operations.
void ParseFoliageVector(const TCHAR* Text, FVector& Value)
{
	Value = FVector::ZeroVector;
	int32 Fields = 0;
	Fields += FParse::Value(Text, TEXT("X="), Value.X);
	Fields += FParse::Value(Text, TEXT("Y="), Value.Y);
	Fields += FParse::Value(Text, TEXT("Z="), Value.Z);
	if (Fields == 0)
	{
		Value.X = FCString::Atof(Text);
		Text = FCString::Strchr(Text, TEXT(','));
		if (!Text)
			return;
		Value.Y = FCString::Atof(++Text);
		Text = FCString::Strchr(Text, TEXT(','));
		if (Text)
			Value.Z = FCString::Atof(++Text);
	}
}

void ParseFoliageRotation(const TCHAR* Text, FRotator& Value)
{
	Value = FRotator::ZeroRotator;
	float Field = 0.0f;
	int32 Fields = 0;
	if (FParse::Value(Text, TEXT("PITCH="), Field)) { Value.Pitch = Field; ++Fields; }
	if (FParse::Value(Text, TEXT("YAW="), Field)) { Value.Yaw = Field; ++Fields; }
	if (FParse::Value(Text, TEXT("ROLL="), Field)) { Value.Roll = Field; ++Fields; }
	if (Fields == 0)
	{
		Value.Pitch = FCString::Atof(Text);
		Text = FCString::Strchr(Text, TEXT(','));
		if (!Text)
			return;
		Value.Yaw = FCString::Atof(++Text);
		Text = FCString::Strchr(Text, TEXT(','));
		if (Text)
			Value.Roll = FCString::Atof(++Text);
	}
}

bool IsImportFrameEnd(const TCHAR* Text, int32 Depth)
{
	return ConsumeImportToken(Text, TEXT("END"), TEXT("Actor"))
		|| ConsumeImportToken(Text, TEXT("END"), TEXT("DefaultProperties"))
		|| ConsumeImportToken(Text, TEXT("END"), TEXT("structdefaultproperties"))
		|| (ConsumeImportToken(Text, TEXT("END"), TEXT("Object")) && Depth != 0);
}

class FTextImportContext final : public FContextSupplier
{
public:
	explicit FTextImportContext(const FImportObjectParams& Params)
		: CurrentLine(Params.LineNumber)
	{
		UClass* Owner = Params.ObjectStruct->GetOwnerClass();
		PackageName = (Owner ? Owner : Params.ObjectStruct)->GetOutermost()->GetName();
		ClassName = Owner ? Owner->GetName() : FName(NAME_None).ToString();
	}

	FString GetContext() override
	{
		return FString::Printf(TEXT("%sDevelopment/Src/%s/Classes/%s.h(%i)"),
			*FPaths::RootDir(), *PackageName, *ClassName, CurrentLine);
	}

	int32 CurrentLine;
private:
	FString PackageName;
	FString ClassName;
};

// Keep diagnostic context local to this call tree, including reentrant imports.
class FTextImportFeedbackScope
{
public:
	FTextImportFeedbackScope(FImportObjectParams& Params, FTextImportContext* Context)
		: Warn(Params.Warn), bChanged(Params.LineNumber != INDEX_NONE)
	{
		if (bChanged)
		{
			Previous = Warn->GetContext();
			if (Context)
				Context->CurrentLine = Params.LineNumber;
			Warn->SetContext(Context);
		}
	}

	~FTextImportFeedbackScope()
	{
		if (bChanged)
			Warn->SetContext(Previous);
	}
private:
	FFeedbackContext* Warn;
	FContextSupplier* Previous = nullptr;
	bool bChanged;
};

// Traverse only the properties recorded by native ImportSingleProperty. Native
// final remapping has no map/set traversal; their parsing still uses native code.
void RemapImportedValue(FProperty* Property, int32 Index,
	uint8* Container, const TMap<FSoftObjectPath, UObject*>& Remapper)
{
	if (FArrayProperty* Array = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Container));
		const int32 First = Index == INDEX_NONE ? 0 : Index;
		const int32 Limit = Index == INDEX_NONE ? Values.Num() : Index + 1;
		for (int32 Element = First; Element < Limit; ++Element)
			RemapImportedValue(Array->Inner, INDEX_NONE, Values.GetRawPtr(Element), Remapper);
		return;
	}

	const int32 First = Index == INDEX_NONE ? 0 : Index;
	const int32 Limit = Index == INDEX_NONE ? Property->ArrayDim : Index + 1;
	if (FObjectPropertyBase* Reference = CastField<FObjectPropertyBase>(Property))
	{
		for (int32 Element = First; Element < Limit; ++Element)
		{
			uint8* Address = Reference->ContainerPtrToValuePtr<uint8>(Container, Element);
			UObject* Value = Reference->GetObjectPropertyValue(Address);
			UObject* const* Replacement = Value ? Remapper.Find(FSoftObjectPath(Value)) : nullptr;
			if (!Value)
			{
				if (FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(Reference))
					Replacement = Remapper.Find(Soft->GetPropertyValue(Address).ToSoftObjectPath());
			}
			if (Replacement && (*Replacement)->GetClass()->IsChildOf(Reference->PropertyClass))
				Reference->SetObjectPropertyValue(Address, *Replacement);
		}
	}
	else if (FStructProperty* Struct = CastField<FStructProperty>(Property))
	{
		for (int32 Element = First; Element < Limit; ++Element)
		{
			if (Struct->Struct == TBaseStructure<FSoftObjectPath>::Get())
			{
				FSoftObjectPath* Path = Struct->ContainerPtrToValuePtr<FSoftObjectPath>(Container, Element);
				if (UObject* const* Replacement = Remapper.Find(*Path))
					*Path = FSoftObjectPath(*Replacement);
			}
			else
			{
				uint8* StructData = Struct->ContainerPtrToValuePtr<uint8>(Container, Element);
				for (TFieldIterator<FProperty> Field(Struct->Struct); Field; ++Field)
					RemapImportedValue(*Field, INDEX_NONE, StructData, Remapper);
			}
		}
	}
}

UObject* ResolveImportArchetype(const TCHAR* Header, UObject* Outer)
{
	if (FBlueprintEditorUtils::IsAnonymousBlueprintClass(Outer->GetClass()))
		return nullptr;
	FString ExportText;
	FString ClassPath;
	FString ObjectPath;
	if (!FParse::Value(Header, TEXT("Archetype="), ExportText)
		|| !FPackageName::ParseExportTextPath(ExportText, &ClassPath, &ObjectPath))
		return nullptr;

	UClass* Class = FPackageName::IsValidObjectPath(ClassPath)
		? Cast<UClass>(StaticFindObject(UClass::StaticClass(), nullptr, *ClassPath))
		: Cast<UClass>(StaticFindFirstObject(UClass::StaticClass(), *ClassPath,
			EFindFirstObjectOptions::NativeFirst | EFindFirstObjectOptions::EnsureIfAmbiguous,
			ELogVerbosity::Warning));
	if (!Class)
		return nullptr;
	ObjectPath = ObjectPath.TrimQuotes();
	return FPackageName::IsShortPackageName(ObjectPath)
		? StaticFindFirstObject(Class, *ObjectPath,
			EFindFirstObjectOptions::NativeFirst | EFindFirstObjectOptions::EnsureIfAmbiguous)
		: StaticFindObject(Class, nullptr, *ObjectPath);
}

const TCHAR* ExecuteTextImportFrame(FImportObjectParams& Params, FTextImportContext* Context);

// All parser state is scoped to one frame. The cursor continues to point into
// the caller's original buffer; children consume it synchronously at definition.
struct FTextImportFrame
{
	FImportObjectParams& Params;
	FObjectInstancingGraph& Graph;
	FTextImportContext* Context;
	const TCHAR* Cursor;
	UObject* Root;
	UClass* OwnerClass = nullptr;
	bool bSubobjectsAllowed;
	bool bImportedBrush = false;
	TMap<FSoftObjectPath, UObject*>* Remapper;
	TOptional<TMap<FSoftObjectPath, UObject*>> LocalRemapper;
	TArray<FDefinedProperty> DefinedProperties;
	TMap<UObject*, FObjectInstancingGraph> DedicatedGraphs;

	FTextImportFrame(FImportObjectParams& InParams, FObjectInstancingGraph& InGraph,
		FTextImportContext* InContext)
		: Params(InParams), Graph(InGraph), Context(InContext), Cursor(Params.SourceText),
		  Root(Params.SubobjectRoot),
		  bSubobjectsAllowed(!Params.ObjectStruct->IsA(UScriptStruct::StaticClass()) && Params.SubobjectOuter),
		  Remapper(Params.ObjectRemapper)
	{
		if (bSubobjectsAllowed)
		{
			if (!Root)
				Root = Params.SubobjectOuter;
			OwnerClass = Params.SubobjectOuter->IsA(UClass::StaticClass())
				? CastChecked<UClass>(Params.SubobjectOuter) : Params.SubobjectOuter->GetClass();
		}
	}

	FObjectInstancingGraph* SelectExistingGraph(UObject* Object, TOptional<bool> Overrides = {})
	{
		if (FObjectInstancingGraph* Existing = DedicatedGraphs.Find(Object))
			return Existing;
		if (FOverridableSerializationLogic::ShouldUseDedicatedInstancingGraph(Object, Overrides))
			return &DedicatedGraphs.Emplace(Object, FObjectInstancingGraph(Object));
		return &Graph;
	}

	bool EnterChild(UObject* Child, UClass* Class, FObjectInstancingGraph* ChildGraph)
	{
		FImportObjectParams ChildParams;
		ChildParams.DestData = reinterpret_cast<uint8*>(Child);
		ChildParams.SourceText = Cursor;
		ChildParams.ObjectStruct = Class;
		ChildParams.SubobjectRoot = Root;
		ChildParams.SubobjectOuter = Child;
		ChildParams.Warn = Params.Warn;
		ChildParams.Depth = Params.Depth + 1;
		ChildParams.LineNumber = Context ? Context->CurrentLine : 0;
		ChildParams.InInstanceGraph = ChildGraph;
		ChildParams.ObjectRemapper = Remapper;
		ChildParams.bShouldCallEditChange = true;
		Cursor = ExecuteTextImportFrame(ChildParams, Context);
		if (!Cursor)
		{
			Params.Warn->Logf(ELogVerbosity::Error,
				TEXT("BEGIN OBJECT: Could not import properties from sub-object %s."), *Root->GetName());
			return false;
		}
		return true;
	}

	bool ImportObject(const TCHAR* Header, const FString& Statement)
	{
		if (!bSubobjectsAllowed)
		{
			Params.Warn->Logf(ELogVerbosity::Error, TEXT("BEGIN OBJECT: Subobjects are not allowed in this context"));
			return false;
		}
		UClass* Class = nullptr;
		bool bInvalidClass = false;
		ParseObject<UClass>(Header, TEXT("Class="), Class, nullptr, &bInvalidClass);
		if (bInvalidClass)
		{
			Params.Warn->Logf(ELogVerbosity::Error, TEXT("BEGIN OBJECT: Invalid class specified: %s"), *Statement);
			return false;
		}
		FName Name = NAME_None;
		FParse::Value(Header, TEXT("Name="), Name);
		if (Name.IsNone())
		{
			Params.Warn->Logf(ELogVerbosity::Error,
				TEXT("BEGIN OBJECT: Must specify valid name for subobject/component: %s"), *Statement);
			return false;
		}
		FString ExportPath;
		const bool bExportPath = FParse::Value(Header, TEXT("ExportPath="), ExportPath);
		UObject* BaseTemplate = nullptr;
		bool bRedefinition = false;
		if (!Class)
		{
			UClass* Parent = OwnerClass->GetSuperClass();
			check(Parent);
			UObject* ParentDefault = Parent->GetDefaultObject();
			check(ParentDefault);
			BaseTemplate = StaticFindObjectFast(UObject::StaticClass(), Params.SubobjectOuter, Name);
			bRedefinition = BaseTemplate != nullptr;
			if (!BaseTemplate)
				BaseTemplate = StaticFindObjectFast(UObject::StaticClass(), ParentDefault, Name);
			if (!BaseTemplate)
			{
				Params.Warn->Logf(ELogVerbosity::Error,
					TEXT("BEGIN OBJECT: No base template named %s found in parent class %s: %s"),
					*Name.ToString(), *Parent->GetName(), *Statement);
				return false;
			}
			Class = BaseTemplate->GetClass();
		}

		const bool bT3D = FOverridableSerializationLogic::HasCapabilities(
			FOverridableSerializationLogic::ECapabilities::T3DSerialization);
		const bool bShadow = bT3D && FOverridableSerializationLogic::HasCapabilities(
			FOverridableSerializationLogic::ECapabilities::SubObjectsShadowSerialization);
		checkSlow(Class);
		if (bRedefinition)
		{
			if (bShadow && FOverridableManager::Get().IsEnabled(BaseTemplate))
			{
				if (!Remapper)
					Remapper = &LocalRemapper.Emplace();
				FSoftObjectPath Path(ExportPath);
				if (!ensureMsgf(bExportPath, TEXT("Subobject redefinition is missing the object's export path while importing it.")))
					Path = FSoftObjectPath(BaseTemplate);
				if (UObject* const* Replacement = Remapper->Find(Path))
					BaseTemplate = *Replacement;
			}
			// A classless redefinition deliberately has none of the definition effects.
			return EnterChild(BaseTemplate, Class, SelectExistingGraph(BaseTemplate));
		}

		FName OperationName = NAME_None;
		const bool bOperation = FParse::Value(Header, TEXT("OverriddenOperation="), OperationName);
		const TOptional<EOverriddenPropertyOperation> Operation = bOperation && bT3D
			? GetOverriddenOperationFromName(OperationName) : TOptional<EOverriddenPropertyOperation>();
		UObject* RequestedArchetype = ResolveImportArchetype(Header, Params.SubobjectOuter);
		UObject* Child = nullptr;
		if (Params.SubobjectOuter->HasAnyFlags(RF_ClassDefaultObject))
		{
			if (!RequestedArchetype)
			{
				RequestedArchetype = OwnerClass->GetDefaultSubobjectByName(Name);
				if (RequestedArchetype)
				{
					if (!BaseTemplate)
					{
						Params.Warn->Logf(ELogVerbosity::Error,
							TEXT("BEGIN OBJECT: The component name %s is already used (if you want to override the component, don't specify a class): %s"),
							*Name.ToString(), *Statement);
						return false;
					}
					checkf(RequestedArchetype == BaseTemplate, TEXT("OverrideComponent: '%s'   BaseTemplate: '%s'"),
						*RequestedArchetype->GetFullName(), *BaseTemplate->GetFullName());
				}
			}
		}
		else
		{
			Child = FindObject<UObject>(Params.SubobjectOuter, *Name.ToString());
			if (Child && Child->GetArchetype() == BaseTemplate && !BaseTemplate)
			{
				Params.Warn->Logf(ELogVerbosity::Error,
					TEXT("BEGIN OBJECT: A subobject named %s is already declared in a parent class.  If you intended to override that subobject, don't specify a class in the derived subobject definition: %s"),
					*Name.ToString(), *Statement);
				return false;
			}
		}

		EObjectFlags Flags = Params.SubobjectOuter->GetMaskedFlags(RF_PropagateToSubObjects);
		if (!RequestedArchetype)
			RequestedArchetype = BaseTemplate;
		UObject* Abandoned = nullptr;
		if (Child && (Child->GetClass() != Class || Child->GetOuter() != Params.SubobjectOuter || Child->GetFName() != Name))
		{
			if (bShadow && FOverridableManager::Get().IsEnabled(Child) && !Child->GetClass()->IsChildOf(Class))
				Name = MakeUniqueObjectName(Params.SubobjectOuter, Class, Name);
			else
			{
				UE_LOG(LogTemp, Log, TEXT("Could not reuse component instance %s, name clash?"), *Child->GetFullName());
				Child->Rename(nullptr, nullptr, REN_DontCreateRedirectors);
				Abandoned = Child;
			}
			Child = nullptr;
		}

		FObjectInstancingGraph* SelectedGraph = &Graph;
		if (!Child)
		{
			const bool bDedicated = FOverridableSerializationLogic::ShouldUseDedicatedInstancingGraphForNewObject(
				Class, Flags, RequestedArchetype, bT3D && Operation.IsSet());
			FObjectInstancingGraph ConstructionGraph;
			Child = NewObject<UObject>(Params.SubobjectOuter, Class, Name, Flags, RequestedArchetype,
				!!Params.SubobjectOuter, bDedicated ? &ConstructionGraph : &Graph);
			if (bDedicated)
			{
				if (!ConstructionGraph.HasDestinationRoot())
					ConstructionGraph.SetDestinationRoot(Child);
				SelectedGraph = &DedicatedGraphs.Emplace(Child, MoveTemp(ConstructionGraph));
			}
		}
		else
		{
			SelectedGraph = SelectExistingGraph(Child, bT3D && Operation.IsSet());
			if (Flags & RF_Transactional)
			{
				if (UActorComponent* Component = Cast<UActorComponent>(Child))
				{
					if (Component->IsCreatedByConstructionScript())
						Flags &= ~RF_Transactional;
				}
			}
			if (Child->HasAllFlags(RF_DefaultSubObject))
				Flags |= RF_DefaultSubObject;
			Child->ClearGarbage();
			Child->ClearGarbage();
			Child->ClearFlags(RF_AllFlags);
			Child->ClearInternalFlags(EInternalObjectFlags_AllFlags);
			Child->SetFlags(Flags);
		}

		// Publish the requested template mapping before serialization or child entry.
		// Identity reuse intentionally does not compare or change the real archetype.
		SelectedGraph->AddNewInstance(Child, RequestedArchetype);
		TMap<UObject*, UObject*> Replacements;
		if (RequestedArchetype)
			Replacements.Add(RequestedArchetype, Child);
		if (Abandoned)
			Replacements.Add(Abandoned, Child);
		FArchiveReplaceObjectRef<UObject> Replace(Params.SubobjectOuter, Replacements,
			EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
		if (bExportPath && Remapper)
			Remapper->Add(FSoftObjectPath(ExportPath), Child);
		if (bT3D)
		{
			if (Operation.IsSet())
				FOverridableManager::Get().RestoreOverrideOperation(Child, Operation.GetValue(), true);
			else
				FOverridableManager::Get().Disable(Child);
		}
		return EnterChild(Child, Class, SelectedGraph);
	}

	bool ImportBrush(const TCHAR* Header, const FString& Statement)
	{
		if (!bSubobjectsAllowed)
		{
			Params.Warn->Logf(ELogVerbosity::Error, TEXT("BEGIN BRUSH: Subobjects are not allowed in this context"));
			return false;
		}
		TCHAR BrushName[NAME_SIZE];
		if (!FParse::Value(Header, TEXT("Name="), BrushName, NAME_SIZE))
			return true;
		UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.ModelFactory"));
		if (!FactoryClass)
			FactoryClass = LoadObject<UClass>(nullptr, TEXT("/Script/UnrealEd.ModelFactory"));
		if (!FactoryClass || FactoryClass->GetPathName() != TEXT("/Script/UnrealEd.ModelFactory")
			|| FactoryClass->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
		{
			Params.Warn->Logf(ELogVerbosity::Error, TEXT("BEGIN BRUSH: Actual UnrealEd.ModelFactory class is unavailable"));
			return false;
		}
		TStrongObjectPtr<UObject> FactoryObject(NewObject<UObject>(GetTransientPackage(), FactoryClass));
		if (!FactoryObject.IsValid() || FactoryObject->GetClass() != FactoryClass)
		{
			Params.Warn->Logf(ELogVerbosity::Error, TEXT("BEGIN BRUSH: Could not construct actual UnrealEd.ModelFactory"));
			return false;
		}
		UModel* ExistingBrush = FindObject<UModel>(Root, BrushName);
		if (ExistingBrush && ExistingBrush->Polys && ExistingBrush->Polys->Element.Num() > 0)
			ExistingBrush->Rename();

		const TCHAR* const FactoryStart = Cursor;
		const TCHAR* const BufferEnd = Cursor + FCString::Strlen(Cursor);
		UModelFactory* Factory = static_cast<UModelFactory*>(FactoryObject.Get());
		UObject* ImportedModel = Factory->FactoryCreateText(UModel::StaticClass(), Root,
			FName(BrushName, FNAME_Add), RF_NoFlags, nullptr, TEXT("t3d"), Cursor, BufferEnd, Params.Warn);
		const UPTRINT StartAddress = reinterpret_cast<UPTRINT>(FactoryStart);
		const UPTRINT EndAddress = reinterpret_cast<UPTRINT>(BufferEnd);
		const UPTRINT CursorAddress = reinterpret_cast<UPTRINT>(Cursor);
		if (!ImportedModel || !ImportedModel->IsA(UModel::StaticClass())
			|| CursorAddress <= StartAddress || CursorAddress > EndAddress
			|| (CursorAddress - StartAddress) % sizeof(TCHAR) != 0)
		{
			Params.Warn->Logf(ELogVerbosity::Error, TEXT("BEGIN BRUSH: Model factory failed or returned an invalid source cursor"));
			return false;
		}
		bImportedBrush = true;
		return true;
	}

	const TCHAR* Parse()
	{
		const int32 PortFlags = PPF_Delimited | PPF_CheckReferences
			| (GIsImportingT3D ? PPF_AttemptNonQualifiedSearch : 0);
		FString Statement;
		int32 LinesConsumed = 0;
		while (FParse::LineExtended(&Cursor, Statement, LinesConsumed, FParse::ELineExtendedFlags::OldExactMode))
		{
			int32 Length = Statement.Len();
			while (Length > 0 && (Statement[Length - 1] == TEXT(';')
				|| Statement[Length - 1] == TEXT(' ') || Statement[Length - 1] == TEXT('\t')))
				--Length;
			Statement.LeftInline(Length, EAllowShrinking::No);
			if (Context)
				Context->CurrentLine += LinesConsumed;
			if (Statement.IsEmpty())
				continue;
			const TCHAR* Text = *Statement;
			int32 ExplicitLine;
			if (FParse::Value(Text, TEXT("linenumber="), ExplicitLine))
			{
				if (Context)
					Context->CurrentLine = ExplicitLine;
				continue;
			}
			if (ConsumeImportToken(Text, TEXT("BEGIN"), TEXT("Brush"))
				&& Params.ObjectStruct->IsChildOf(ABrush::StaticClass()))
			{
				if (!ImportBrush(Text, Statement))
					return nullptr;
			}
			else if (ConsumeImportToken(Text, TEXT("BEGIN"), TEXT("Foliage")))
			{
				UFoliageType* SourceType;
				FName ComponentName;
				if (Root && ParseObject<UFoliageType>(Text, TEXT("FoliageType="), SourceType,
					nullptr, EParseObjectLoadingPolicy::Find)
					&& FParse::Value(Text, TEXT("Component="), ComponentName))
				{
					UPrimitiveComponent* Component = FindObjectFast<UPrimitiveComponent>(Root, ComponentName);
					if (Component && Component->GetComponentLevel())
					{
						TMap<AInstancedFoliageActor*, TArray<FFoliageInstance>> InstancesByActor;
						FString Line;
						while (FParse::Line(&Cursor, Line))
						{
							const TCHAR* InstanceText = *Line;
							if (ConsumeImportToken(InstanceText, TEXT("END"), TEXT("Foliage")))
								break;
							FFoliageInstance Instance;
							FString Value;
							if (FParse::Value(InstanceText, TEXT("Location="), Value, false))
								ParseFoliageVector(*Value, Instance.Location);
							if (FParse::Value(InstanceText, TEXT("Rotation="), Value, false))
								ParseFoliageRotation(*Value, Instance.Rotation);
							if (FParse::Value(InstanceText, TEXT("PreAlignRotation="), Value, false))
								ParseFoliageRotation(*Value, Instance.PreAlignRotation);
							if (FParse::Value(InstanceText, TEXT("DrawScale3D="), Value, false))
							{
								FVector Scale;
								ParseFoliageVector(*Value, Scale);
								Instance.DrawScale3D = FVector3f(Scale);
							}
							FParse::Value(InstanceText, TEXT("Flags="), Instance.Flags);
							Instance.BaseComponent = Component;
							if (AInstancedFoliageActor* Actor = AInstancedFoliageActor::Get(
								Component->GetWorld(), true, Component->GetComponentLevel(), Instance.Location))
								InstancesByActor.FindOrAdd(Actor).Add(MoveTemp(Instance));
						}
						for (const auto& Group : InstancesByActor)
						{
							FFoliageInfo* Info = nullptr;
							UFoliageType* LocalType = Group.Key->AddFoliageType(SourceType, &Info);
							TArray<const FFoliageInstance*> Instances;
							Instances.Reserve(Group.Value.Num());
							for (const FFoliageInstance& Instance : Group.Value)
								Instances.Add(&Instance);
							if (Info)
								Info->AddInstances(LocalType, Instances);
						}
					}
				}
			}
			else if (ConsumeImportToken(Text, TEXT("BEGIN"), TEXT("Object")))
			{
				if (!ImportObject(Text, Statement))
					return nullptr;
			}
			else if (FParse::Command(&Text, TEXT("CustomProperties")))
			{
				check(Params.SubobjectOuter);
				Params.SubobjectOuter->ImportCustomProperties(Text, Params.Warn);
			}
			else if (IsImportFrameEnd(Text, Params.Depth))
				break;
			else if (ConsumeImportToken(Text, TEXT("REMOVE"), TEXT("Component")))
			{
				checkf(false, TEXT("Remove component is illegal in pasted text"));
			}
			else
				FProperty::ImportSingleProperty(Text, Params.DestData, Params.ObjectStruct,
					Params.SubobjectOuter, PortFlags, Params.Warn, DefinedProperties);
		}
		if (Remapper)
		{
			for (const FDefinedProperty& Defined : DefinedProperties)
				RemapImportedValue(Defined.Property, Defined.Index, Params.DestData, *Remapper);
		}
		if (bImportedBrush && Params.ObjectStruct->IsChildOf(ABrush::StaticClass())
			&& !Params.ObjectStruct->IsChildOf(AVolume::StaticClass()))
		{
			ABrush* Actor = reinterpret_cast<ABrush*>(Params.DestData);
			if (GIsEditor && Actor->GetBrushComponent())
			{
				// Select preparation after all properties and reference remapping.
				if (Actor->GetBrushComponent()->Mobility == EComponentMobility::Static)
					Actor->SetNotForClientOrServer();
				else
					FBSPOps::csgPrepMovingBrush(Actor);
			}
		}
		return Cursor;
	}
};

const TCHAR* ExecuteTextImportFrame(FImportObjectParams& Params, FTextImportContext* Context)
{
	FTextImportFeedbackScope FeedbackScope(Params, Context);
	if (Params.bShouldCallEditChange && Params.SubobjectOuter)
		Params.SubobjectOuter->PreEditChange(nullptr);
	FObjectInstancingGraph DefaultGraph;
	FObjectInstancingGraph& Graph = Params.InInstanceGraph ? *Params.InInstanceGraph : DefaultGraph;
	if (Params.SubobjectRoot && Params.SubobjectRoot != UObject::StaticClass()->GetDefaultObject())
		Graph.SetDestinationRoot(Params.SubobjectRoot);
	TOptional<TMap<FSoftObjectPath, UObject*>> WrapperRemapper;
	if (!Params.ObjectRemapper && FOverridableManager::Get().IsEnabled(Params.SubobjectOuter))
		Params.ObjectRemapper = &WrapperRemapper.Emplace();

	check(Params.ObjectStruct);
	check(Params.DestData);
	const TCHAR* Result = nullptr;
	if (Params.SourceText)
	{
		FOverriddenPropertySet* Overrides = nullptr;
		if (Params.SubobjectOuter && FOverridableSerializationLogic::HasCapabilities(
			FOverridableSerializationLogic::ECapabilities::T3DSerialization))
			Overrides = FOverridableManager::Get().GetOverriddenProperties(Params.SubobjectOuter);
		FEnableOverridableSerializationScope OverrideScope(Overrides != nullptr, Overrides);
		FTextImportFrame Frame(Params, Graph, Context);
		Result = Frame.Parse();
	}
	// The parser scope has ended. Native wrappers finalize even on a null result.
	if (Params.SubobjectOuter)
	{
		check(Params.SubobjectRoot);
		if (!Params.SubobjectRoot->HasAnyFlags(RF_ClassDefaultObject))
		{
			UObject* ActualArchetype = Params.SubobjectOuter->GetArchetype();
			Params.ObjectStruct->InstanceSubobjectTemplates(Params.DestData, ActualArchetype,
				ActualArchetype->GetClass(), Params.SubobjectOuter, &Graph);
		}
		if (Params.bShouldCallEditChange)
		{
			Params.SubobjectOuter->PostEditImport();
			Params.SubobjectOuter->PostEditChange();
		}
		Params.SubobjectRoot->CheckDefaultSubobjects();
	}
	if (WrapperRemapper.IsSet())
		Params.ObjectRemapper = nullptr;
	return Result;
}
}

const TCHAR* FAngelscriptEditorObjectPrecreateImportAdapter::ImportWholeText(FImportObjectParams& Params)
{
	TOptional<FTextImportContext> Context;
	if (Params.LineNumber != INDEX_NONE && !Params.SubobjectRoot)
		Context.Emplace(Params);
	return ExecuteTextImportFrame(Params, Context.IsSet() ? &Context.GetValue() : nullptr);
}

bool FAngelscriptEditorObjectPrecreateImportAdapter::ShouldUseImportWholeText(
	const FImportObjectParams& Params)
{
	if (!IsInGameThread())
		return false;

	auto IsScriptClassOrSubclass = [](const UClass* Class)
	{
		for (const UClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
			if (Cast<UASClass>(Current) != nullptr)
				return true;
		return false;
	};
	auto IsScriptObject = [&IsScriptClassOrSubclass](const UObject* Object)
	{
		return Object != nullptr && IsScriptClassOrSubclass(Object->GetClass());
	};
	auto HasScriptSubobjects = [&IsScriptObject](UObject* Outer)
	{
		if (Outer == nullptr)
			return false;
		TArray<UObject*> Subobjects;
		GetObjectsWithOuter(Outer, Subobjects, EGetObjectsFlags::IncludeNestedObjects);
		for (const UObject* Subobject : Subobjects)
			if (IsScriptObject(Subobject))
				return true;
		return false;
	};

	if (IsScriptClassOrSubclass(Cast<UClass>(Params.ObjectStruct))
		|| IsScriptObject(Params.SubobjectRoot) || IsScriptObject(Params.SubobjectOuter))
		return true;

	if (HasScriptSubobjects(Params.SubobjectRoot)
		|| (Params.SubobjectRoot && HasScriptSubobjects(Params.SubobjectRoot->GetArchetype()))
		|| (Params.SubobjectOuter && IsScriptObject(Params.SubobjectOuter->GetArchetype()))
		|| (Params.SubobjectOuter && HasScriptSubobjects(Params.SubobjectOuter->GetArchetype())))
		return true;

	if (Params.SourceText != nullptr)
	{
		static const TCHAR* const ScriptClassMarkers[] = {
			TEXT("Class=/Script/Angelscript."),
			TEXT("Archetype=/Script/Angelscript."),
		};
		for (const TCHAR* Marker : ScriptClassMarkers)
			if (FCString::Strstr(Params.SourceText, Marker) != nullptr)
				return true;
	}
	return false;
}

bool FAngelscriptEditorObjectPrecreateImportAdapter::ImportObjectText(UObject* Object, const FString& Text)
{
	if (!IsInGameThread() || !IsValid(Object)
		|| Object->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_BeginDestroyed | RF_FinishDestroyed)
		|| Object->GetClass()->HasAnyClassFlags(CLASS_NewerVersionExists))
		return false;

	// Editor callbacks can reenter script and change the caller's string or roots.
	const FString SourceText(Text);
	TStrongObjectPtr<UObject> RetainedObject(Object);
	TStrongObjectPtr<UClass> RetainedClass(Object->GetClass());
	FImportObjectParams Params;
	Params.DestData = reinterpret_cast<uint8*>(Object);
	Params.ObjectStruct = RetainedClass.Get();
	Params.SubobjectRoot = Object;
	Params.SubobjectOuter = Object;
	Params.SourceText = *SourceText;
	Params.Warn = GWarn;
	Params.bShouldCallEditChange = true;
	const TCHAR* End = ImportWholeText(Params);
	return End == *SourceText + SourceText.Len();
}

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_EditorObjectTextImport(FAngelscriptBinds::EOrder::Late, []()
{
	if (!FAngelscriptManager::bUseEditorScripts)
		return;
	FAngelscriptBinds::FNamespace Namespace("Editor");
	FAngelscriptBinds::BindGlobalFunction("bool ImportObjectText(UObject Object, const FString& Text)",
		[](UObject* Object, const FString& Text)
		{
			return FAngelscriptEditorObjectPrecreateImportAdapter::ImportObjectText(Object, Text);
		});
});

UObject* FAngelscriptEditorObjectPrecreateImportAdapter::PreCreate(
	const FAngelscriptEditorObjectPreCreateRequest& Request,
	EAngelscriptEditorObjectImportResult& OutResult)
{
	OutResult = ValidatePreCreate(Request);
	if (OutResult != EAngelscriptEditorObjectImportResult::Completed)
		return nullptr;

	UObject* CreatedObject = NewObject<UObject>(
		Request.Outer, Request.Class, Request.Name, RF_Transactional, Request.Template);
	if (CreatedObject == nullptr)
	{
		OutResult = EAngelscriptEditorObjectImportResult::CreationFailed;
	}
	return CreatedObject;
}

EAngelscriptEditorObjectImportResult
FAngelscriptEditorObjectPrecreateImportAdapter::ValidateImport(
	const FAngelscriptEditorObjectImportRequest& Request)
{
	if (Request.Target == nullptr)
		return EAngelscriptEditorObjectImportResult::NullTarget;
	if (Request.Replacement == nullptr)
		return EAngelscriptEditorObjectImportResult::NullReplacement;
	if (Request.Target == Request.Replacement)
		return EAngelscriptEditorObjectImportResult::SameTargetAndReplacement;
	if (!IsValid(Request.Target))
		return EAngelscriptEditorObjectImportResult::InvalidTarget;
	if (Request.Target->HasAnyFlags(RF_Transient))
		return EAngelscriptEditorObjectImportResult::TransientTarget;
	if (Request.Target->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		return EAngelscriptEditorObjectImportResult::UnsupportedTarget;
	if (!IsValid(Request.Replacement))
		return EAngelscriptEditorObjectImportResult::InvalidReplacement;
	if (Request.Replacement->HasAnyFlags(RF_Transient))
		return EAngelscriptEditorObjectImportResult::TransientReplacement;
	if (Request.Replacement->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		return EAngelscriptEditorObjectImportResult::UnsupportedReplacement;
	if (Request.Target->GetClass() != Request.Replacement->GetClass())
		return EAngelscriptEditorObjectImportResult::MismatchedReplacementClass;
	if (Request.Steps.IsEmpty())
		return EAngelscriptEditorObjectImportResult::EmptyPropertySteps;

	TSet<FName> ImportedTargets;
	for (const FAngelscriptEditorObjectPropertyImportStep& Step : Request.Steps)
	{
		if (Step.SourceProperty.IsNone() || Step.TargetProperty.IsNone())
			return EAngelscriptEditorObjectImportResult::UnsupportedProperty;
		if (ImportedTargets.Contains(Step.TargetProperty))
			return EAngelscriptEditorObjectImportResult::DuplicatePropertyTarget;
		ImportedTargets.Add(Step.TargetProperty);

		const FProperty* SourceProperty = FindFProperty<FProperty>(
			Request.Replacement->GetClass(), Step.SourceProperty);
		if (SourceProperty == nullptr)
			return EAngelscriptEditorObjectImportResult::MissingSourceProperty;
		const FProperty* TargetProperty = FindFProperty<FProperty>(
			Request.Target->GetClass(), Step.TargetProperty);
		if (TargetProperty == nullptr)
			return EAngelscriptEditorObjectImportResult::MissingTargetProperty;
		if (SourceProperty->HasAnyPropertyFlags(UnsupportedPropertyFlags)
			|| TargetProperty->HasAnyPropertyFlags(UnsupportedPropertyFlags))
			return EAngelscriptEditorObjectImportResult::UnsupportedProperty;
		if (SourceProperty->GetClass() != TargetProperty->GetClass()
			|| SourceProperty->GetSize() != TargetProperty->GetSize()
			|| !SourceProperty->SameType(TargetProperty))
			return EAngelscriptEditorObjectImportResult::MismatchedPropertyType;
	}
	return EAngelscriptEditorObjectImportResult::Completed;
}

EAngelscriptEditorObjectImportResult
FAngelscriptEditorObjectPrecreateImportAdapter::ImportProperties(
	const FAngelscriptEditorObjectImportRequest& Request)
{
	const EAngelscriptEditorObjectImportResult ValidationResult = ValidateImport(Request);
	if (ValidationResult != EAngelscriptEditorObjectImportResult::Completed)
		return ValidationResult;

	for (const FAngelscriptEditorObjectPropertyImportStep& Step : Request.Steps)
	{
		const FProperty* SourceProperty = FindFProperty<FProperty>(
			Request.Replacement->GetClass(), Step.SourceProperty);
		const FProperty* TargetProperty = FindFProperty<FProperty>(
			Request.Target->GetClass(), Step.TargetProperty);
		TargetProperty->CopyCompleteValue(
			TargetProperty->ContainerPtrToValuePtr<void>(Request.Target),
			SourceProperty->ContainerPtrToValuePtr<void>(Request.Replacement));
	}
	return EAngelscriptEditorObjectImportResult::Completed;
}
