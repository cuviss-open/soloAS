#include "ClassGenerator/AngelscriptClassGenerator.h"
                                                                                                  
#include "Extensions/AngelscriptExtensionHooks.h"
                                                                                                
                                                                                                                              
#include "Runtime/Launch/Resources/Version.h"
                                                                                                                            
                                                                                                                 
#include "AngelscriptReloadSafety.h"
                                                                                                               
                                                                                                    
#include "ClassGenerator/AngelscriptFieldDestruction.h"
                                                                                                  
#include "ClassGenerator/ASClass.h"
#include "ClassGenerator/ASStruct.h"
                                                                                              
#include "ClassGenerator/ASEnum.h"
                                                                                            

                                                                                                    
#include "UObject/CoreRedirects.h"
                                                                                                  
#include "UObject/LinkerLoad.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/GarbageCollection.h"
#include "UObject/GarbageCollectionSchema.h"
#include "StructUtils/InstancedStruct.h"

#include "Misc/ScopedSlowTask.h"

#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Subsystems/SubsystemCollection.h"
#include "Subsystems/WorldSubsystem.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimSubsystem_PropertyAccess.h"

#include "Engine/Engine.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/UserDefinedEnum.h"

#include "AngelscriptType.h"
#include "AngelscriptInclude.h"
#include "AngelscriptSettings.h"
                                                                                                                  
#include "ClassGenerator/AngelscriptRuntimeGcMirror.h"
                                                                                                                
                                                                                                    
#include "Shared/SoloASScriptBlueprint.h"
                                                                                                  
                                                                                                               
#include "AngelscriptReflectionAdapter.h"
                                                                                                             
#include "Binds/Helper_FunctionSignature.h"

#include "StartAngelscriptHeaders.h"
#include "as_config.h"
#include "as_scriptengine.h"
#include "as_scriptfunction.h"
#include "as_objecttype.h"
#include "as_scriptobject.h"
#include "as_context.h"
#include "EndAngelscriptHeaders.h"

FOnAngelscriptClassReload FAngelscriptClassGenerator::OnClassReload;
FOnAngelscriptEnumChanged FAngelscriptClassGenerator::OnEnumChanged;
FOnAngelscriptEnumCreated FAngelscriptClassGenerator::OnEnumCreated;
FOnAngelscriptStructReload FAngelscriptClassGenerator::OnStructReload;
FOnAngelscriptDelegateReload FAngelscriptClassGenerator::OnDelegateReload;
FOnAngelscriptPostReload FAngelscriptClassGenerator::OnPostReload;
FOnAngelscriptFullReload FAngelscriptClassGenerator::OnFullReload;
                                                                                                                        
FOnAngelscriptPreFullReload FAngelscriptClassGenerator::OnPreFullReload;
                                                                                                                      
FOnAngelscriptLiteralAssetReload FAngelscriptClassGenerator::OnLiteralAssetReload;

static int32 UniqueGeneratedCounter = 1;
static FORCEINLINE int32 UniqueCounter()
{
	return UniqueGeneratedCounter++;
}
                                                                                                             
static bool IsAngelscriptRuntimeGeneratedProperty(const FProperty* Property)
{
	return HasAngelscriptPropertyTrait(
		Property,
		EAngelscriptPropertyTrait::RuntimeGenerated
	);
}
                                                                                                           
                                                                                                     
static void CommitHotReloadRuntimeObjectTypeCookie(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie,
	asITypeInfo* NewTypeCookie)
{
	if (!IsAngelscriptExecutionWriterHeldByCurrentThread())
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT(
				"ASR-P6-HOT-RELOAD-COOKIE-WRITER-MISSING: runtime object "
				"type-cookie mutation requires the execution writer."
			));
	}

	if (Object == nullptr || ExpectedTypeCookie == nullptr || NewTypeCookie == nullptr)
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT(
				"ASR-P6-HOT-RELOAD-COOKIE-IDENTITY-INVALID: runtime object "
				"reconstruction requires non-null object and type-cookie identities."
			));
	}

	if (!AngelscriptRuntimeGcMirror::CommitRuntimeTrackedObjectReconstruction(
		Object,
		ExpectedTypeCookie,
		NewTypeCookie,
		nullptr))
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT("MIRROR_COOKIE_UPDATE_FAILED"));
	}
}
                                                                                                   
                                                                                                                    
static bool RejectUnsupportedDynamicReplication(
	const FAngelscriptClassDesc& ClassDesc,
	const UClass* GeneratedClass,
	FString& OutPropertyName)
{
	if (GeneratedClass == nullptr
		|| (!GeneratedClass->IsChildOf(AActor::StaticClass())
			&& !GeneratedClass->IsChildOf(UActorComponent::StaticClass())))
	{
		return true;
	}

	for (const TSharedRef<FAngelscriptPropertyDesc>& PropertyDesc : ClassDesc.Properties)
	{
		if (PropertyDesc->bReplicated)
		{
			const UASClass* ScriptClass = Cast<UASClass>(GeneratedClass);
			if (ScriptClass != nullptr && ScriptClass->HasValidScriptClassCarrier())
			{
				continue;
			}
			OutPropertyName = PropertyDesc->GetUnrealPropertyName();
			return false;
		}
	}

	return true;
}
                                                                                                                  
                                                                                                            
static void ClaimHotReloadRuntimeObjectTypeCookie(
	UObject* Object,
	asITypeInfo* ExpectedTypeCookie)
{
	if (!IsAngelscriptExecutionWriterHeldByCurrentThread())
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT(
				"ASR-P6-RECONSTRUCT-COOKIE-WRITER-MISSING: runtime object "
				"reconstruction claim requires the execution writer."
			));
	}

	if (!AngelscriptRuntimeGcMirror::ClaimRuntimeTrackedMirrorForSoftReload(
		Object,
		ExpectedTypeCookie))
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT("RECONSTRUCT_REENTRANT"));
	}
}
                                                                                                          

void FAngelscriptClassGenerator::AddModule(TSharedRef<FAngelscriptModuleDesc> Module)
{
	FModuleData Data;
	Data.OldModule = FAngelscriptManager::Get().GetModule(Module->ModuleName);
	Data.NewModule = Module;
	Data.ModuleIndex = Modules.Num();

	Modules.Add(Data);

	ModuleIndexByName.Add(Module->ModuleName, Data.ModuleIndex);
	ModuleIndexByNewScriptModule.Add(Module->ScriptModule, Data.ModuleIndex);
}

// From Bind_BlueprintEvent.cpp
extern UFunction* GetBlueprintEventByScriptName(UClass* Class, const FString& ScriptName);

static const FName NAME_ExposeOnSpawn(TEXT("ExposeOnSpawn"));
static const FName NAME_EditFixedSize(TEXT("EditFixedSize"));
static const FName NAME_DisplayName(TEXT("DisplayName"));
static const FName NAME_NoBlueprintsOfChildren(TEXT("NoBlueprintsOfChildren"));
static const FName NAME_Evt_ScriptName(TEXT("ScriptName"));
static const FName NAME_AllowPrivateAccess(TEXT("AllowPrivateAccess"));
static const FName NAME_Meta_EditorOnly(TEXT("EditorOnly"));

static const FName NAME_Class_DefaultConfig(TEXT("DefaultConfig"));
static const FName NAME_Actor_DefaultComponent(TEXT("DefaultComponent"));
static const FName NAME_Actor_OverrideComponent(TEXT("OverrideComponent"));
static const FName NAME_Actor_BindComponent(TEXT("BindComponent"));
static const FName NAME_Actor_RootComponent(TEXT("RootComponent"));
static const FName NAME_Actor_Attach(TEXT("Attach"));
static const FName NAME_Actor_AttachSocket(TEXT("AttachSocket"));
static const FName NAME_AnyStructRef(TEXT("__ANY_STRUCT_REF"));
static const FName NAME_Function_MixinArgument(TEXT("MixinArgument"));
static const FName NAME_Function_DefaultToSelf(TEXT("DefaultToSelf"));

                                                                                                                       
static bool IsValidExternalConstructionAttachment(
	const UASClass* ScriptClass,
	const UASClass::FDefaultComponent& DefaultComponent,
	const TSet<FName>& ValidScriptAttachmentTargetNames)
{
	if (ScriptClass == nullptr || DefaultComponent.ComponentClass == nullptr)
		return false;

	// A root is detached by definition.  Do not silently discard a conflicting
	// parent or socket while the stock initializer is still constructing it.
	if (DefaultComponent.bIsRoot
		&& (DefaultComponent.Attach != NAME_None
			|| DefaultComponent.AttachSocket != NAME_None))
		return false;

	// A socket without an explicit parent is valid: the stock construction
	// adapter attaches it to the actor root after all defaults are created.
	if (DefaultComponent.Attach == NAME_None)
		return DefaultComponent.AttachSocket == NAME_None
			|| DefaultComponent.ComponentClass->IsChildOf(USceneComponent::StaticClass());

	if (DefaultComponent.ComponentName == DefaultComponent.Attach
		|| !DefaultComponent.ComponentClass->IsChildOf(USceneComponent::StaticClass()))
		return false;

	const FProperty* ParentProperty = ScriptClass->FindPropertyByName(
		DefaultComponent.Attach);
	const FObjectPropertyBase* ParentObjectProperty =
		CastField<FObjectPropertyBase>(ParentProperty);
	if (ParentObjectProperty == nullptr
		|| ParentObjectProperty->PropertyClass == nullptr
		|| !ParentObjectProperty->PropertyClass->IsChildOf(USceneComponent::StaticClass())
		|| !ParentObjectProperty->HasAnyPropertyFlags(CPF_InstancedReference))
	{
		return false;
	}

	// Script-generated targets must be registered default components.  The
	// registration set is built from descriptors and finalized parents so this
	// remains valid in cooked/runtime builds and is independent of property order.
	if (IsAngelscriptRuntimeGeneratedProperty(ParentProperty))
		return ValidScriptAttachmentTargetNames.Contains(DefaultComponent.Attach);

	// Native inherited component properties have no script descriptor.  Resolve
	// them through the owner CDO using the same public component enumeration used
	// by the construction path, and require an actual SceneComponent instance.
	UClass* OwnerClass = ParentObjectProperty->GetOwnerClass();
	AActor* OwnerCDO = OwnerClass != nullptr
		? Cast<AActor>(OwnerClass->GetDefaultObject())
		: nullptr;
	if (OwnerCDO == nullptr)
		return false;

	for (auto* OwnerComponent : OwnerCDO->GetComponents())
	{
		if (OwnerComponent != nullptr
			&& OwnerComponent->GetFName() == DefaultComponent.Attach
			&& OwnerComponent->IsA<USceneComponent>())
		{
			return true;
		}
	}

	return false;
}
                                                                                                                     
const static FName FUNCMETA_BlueprintThreadSafe("BlueprintThreadSafe");
const static FName FUNCMETA_NotBlueprintThreadSafe("NotBlueprintThreadSafe");
const static FName FUNCMETA_BlueprintProtected("BlueprintProtected");
const static FName FUNCMETA_CrumbFunction("CrumbFunction");
const static FName FUNCMETA_ScriptNoOp("ScriptNoOp");

const static FName CLASSMETA_NotAngelscriptSpawnable("NotAngelscriptSpawnable");

TSharedPtr<FAngelscriptClassDesc> FAngelscriptClassGenerator::EnsureClassAnalyzed(const FString& ClassName)
{
	FDataRef* Ref = DataRefByName.Find(ClassName);
	if (Ref != nullptr && Ref->bIsClass)
	{
		auto& ModuleData = Modules[Ref->ModuleIndex];
		auto& ClassData = ModuleData.Classes[Ref->DataIndex];
		if (ClassData.NewClass.IsValid())
		{
			// Class is pending analysis, analyze it now and return it
			Analyze(ModuleData, ClassData);
			return ClassData.NewClass;
		}
	}

	// Class isn't pending analysis, look it up from the manager from previous compile
	return FAngelscriptManager::Get().GetClass(ClassName);
}

TSharedPtr<FAngelscriptClassDesc> FAngelscriptClassGenerator::GetClassDesc(const FString& ClassName)
{
	FDataRef* Ref = DataRefByName.Find(ClassName);
	if (Ref != nullptr && Ref->bIsClass)
	{
		auto& ModuleData = Modules[Ref->ModuleIndex];
		auto& ClassData = ModuleData.Classes[Ref->DataIndex];
		if (ClassData.NewClass.IsValid())
			return ClassData.NewClass;
	}

	return FAngelscriptManager::Get().GetClass(ClassName);
}

FString FAngelscriptClassGenerator::GetUnrealName(bool bIsStruct, const FString& ClassName)
{
	FString UnrealName = ClassName;
	if (UnrealName.Len() >= 2)
	{
		if (bIsStruct ? (UnrealName[0] == 'F') : (UnrealName[0] == 'U' || UnrealName[0] == 'A'))
		{
			if (FChar::IsUpper(UnrealName[1]))
				UnrealName = UnrealName.Mid(1);
		}
	}
	return UnrealName;
}

static const FString STR_Arg_WorldContext(TEXT("WorldContext"));
static const FName NAME_Arg_WorldContext("WorldContext");
static const FName NAME_Arg_CallableWithoutWorldContext("CallableWithoutWorldContext");
static const FName NAME_Arg_OptionalWorldContext("OptionalWorldContext");
static const FName NAME_Arg_NoWorldContext("NoWorldContext");
static const FName NAME_Arg_AdvancedDisplay("AdvancedDisplay");
static const FName NAME_Arg_RequiredParameters("RequiredParameters");
void FAngelscriptClassGenerator::Analyze(FModuleData& ModuleData, FClassData& ClassData)
{
	// Ignore if we've already analyzed this class
	if (ClassData.bAnalyzed)
		return;

	const bool bLoadedPrecompiledCode = ModuleData.NewModule->bLoadedPrecompiledCode;

	// Modules that previously had swap-in errors should always full reload
	if (ModuleData.OldModule.IsValid() && ModuleData.OldModule->bModuleSwapInError && ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
		ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;

	// Classes that suggested a full reload before should continue to suggest them now
	if (ClassData.OldClass.IsValid() && ClassData.OldClass->bPendingFullReload && ClassData.ReloadReq < EReloadRequirement::FullReloadSuggested)
		ClassData.ReloadReq = EReloadRequirement::FullReloadSuggested;

	// Resolve the compiled script type for the class
	auto* ScriptType = ClassData.NewClass->ScriptType;

	if (ClassData.OldClass.IsValid() && ClassData.OldClass->ScriptType)
		UpdatedScriptTypeMap.Add(ClassData.OldClass->ScriptType, ScriptType);

	// Make sure the superclass has been analyzed
	TSharedPtr<FAngelscriptClassDesc> AngelscriptSuperClass;
	if (!ClassData.NewClass->bSuperIsCodeClass)
		AngelscriptSuperClass = EnsureClassAnalyzed(ClassData.NewClass->SuperClass);

	// Analyze all properties in the class
	TMap<FString, int32> PropertyIndexMap;
	TArray<FAngelscriptTypeUsage> PropertyTypes;
	if (ScriptType != nullptr)
	{
		int32 PropertyCount = ScriptType->GetPropertyCount();
		PropertyTypes.SetNum(PropertyCount);
		for (int32 i = 0; i < PropertyCount; ++i)
		{
			// Skip inherited properties here, they will be handled by the parent class
			if (ScriptType->IsPropertyInherited(i))
				continue;

			const char* Name;
			ScriptType->GetProperty(i, &Name);

			FString ScriptPropertyName = ANSI_TO_TCHAR(Name);
			PropertyIndexMap.Add(ScriptPropertyName, i);

			auto PropertyType = FAngelscriptTypeUsage::FromProperty(ScriptType, i);
			PropertyTypes[i] = PropertyType;
		}
	}

	// Make sure the previous class is actually replacable
	FString UnrealName = GetUnrealName(ClassData.NewClass->bIsStruct, ClassData.NewClass->ClassName);
                                                                                                       
	if (ModuleData.NewModule->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf) UnrealName += TEXT("__") + ModuleData.NewModule->ScriptUnit.Id;
                                                                                                     
	UObject* ReplacedObj = FindObject<UObject>(FAngelscriptManager::GetPackage(), *UnrealName);
	if (ReplacedObj != nullptr)
	{
		if (ClassData.NewClass->bIsStruct)
		{
			if (!ReplacedObj->IsA<UASStruct>())
			{
				FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, ClassData.NewClass->LineNumber, FString::Printf(
					TEXT("Struct %s has a name conflict with non-struct unreal object %s."),
					*ClassData.NewClass->ClassName, *ReplacedObj->GetPathName()));
				ClassData.ReloadReq = EReloadRequirement::Error;
			}
			else
			{
				auto* ReplacedScriptType = (asITypeInfo*)((UASStruct*)ReplacedObj)->ScriptType;
				if (ReplacedScriptType != nullptr)
				{
					auto Module = FAngelscriptManager::Get().GetModule(ReplacedScriptType->GetModule());
					if (Module.IsValid() && !IsReloadingModule(Module))
					{
						FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, ClassData.NewClass->LineNumber, FString::Printf(
							TEXT("Name conflict: unreal name %s for script type %s is already in use in module %s."),
							*UnrealName, *ClassData.NewClass->ClassName, *Module->ModuleName));
						ClassData.ReloadReq = EReloadRequirement::Error;
					}
				}
			}
		}
		else
		{
			if (!ReplacedObj->IsA<UASClass>())
			{
				FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, ClassData.NewClass->LineNumber, FString::Printf(
					TEXT("Class %s has a name conflict with non-class unreal object %s."),
					*ClassData.NewClass->ClassName, *ReplacedObj->GetPathName()));
				ClassData.ReloadReq = EReloadRequirement::Error;
			}
			else
			{
				auto* ReplacedScriptType = (asITypeInfo*)((UASClass*)ReplacedObj)->ScriptTypePtr;
				if (ReplacedScriptType != nullptr)
				{
					auto Module = FAngelscriptManager::Get().GetModule(ReplacedScriptType->GetModule());
					if (Module.IsValid() && !IsReloadingModule(Module))
					{
						FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, ClassData.NewClass->LineNumber, FString::Printf(
							TEXT("Name conflict: unreal name %s for script type %s is already in use in module %s."),
							*UnrealName, *ClassData.NewClass->ClassName, *Module->ModuleName));
						ClassData.ReloadReq = EReloadRequirement::Error;
					}
				}
			}
		}
	}

	if (UsedUnrealNames.Contains(UnrealName))
	{
		FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, ClassData.NewClass->LineNumber, FString::Printf(
			TEXT("Name conflict: unreal name %s for script type %s is already in use."),
			*UnrealName, *ClassData.NewClass->ClassName));
		ClassData.ReloadReq = EReloadRequirement::Error;
	}
	UsedUnrealNames.Add(UnrealName);

	auto AngelscriptSettings = UAngelscriptSettings::StaticClass()->GetDefaultObject<UAngelscriptSettings>();

	// Some properties without a UPROPERTY() should be added as 
	// hidden properties, for garbage collection purposes.
	//  This will be the case for all properties in structs,
	//  as well as any properties whose type is RequiresProperty()
	for (auto& Elem : PropertyIndexMap)
	{
		bool bShouldMakeProperty = false;

		FAngelscriptTypeUsage PropertyType = PropertyTypes[Elem.Value];

		if (ClassData.NewClass->bIsStruct)
			bShouldMakeProperty = !PropertyType.NeverRequiresGC();

		if (PropertyType.RequiresProperty())
			bShouldMakeProperty = true;

		if (!bShouldMakeProperty)
			continue;

		// Skip properties that are already added
		if (ClassData.NewClass->GetProperty(Elem.Key).IsValid())
			continue;

		// Show an error if we can't create a UPROPERTY for this type
		if (!PropertyType.CanCreateProperty())
		{
			FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, ClassData.NewClass->LineNumber, FString::Printf(
				TEXT("Property %s with type %s is in a context where a UPROPERTY must be generated for GC reasons, but the property type is not supported by UPROPERTY."),
				*Elem.Key, *PropertyType.GetAngelscriptDeclaration()));
			ClassData.ReloadReq = EReloadRequirement::Error;
			continue;
		}

		// Add new property
		auto PropDesc = MakeShared<FAngelscriptPropertyDesc>();
		PropDesc->PropertyName = Elem.Key;
		PropDesc->bBlueprintReadable = false;
		PropDesc->bBlueprintWritable = false;
		PropDesc->bEditableOnDefaults = false;
		PropDesc->bEditableOnInstance = false;

		if (AngelscriptSettings->bMarkNonUpropertyPropertiesAsTransient || !ClassData.NewClass->bIsStruct)
		{
			// Properties without a UPROPERTY macro are marked as Transient by default to avoid unintentional serialization
			PropDesc->bTransient = true;
		}

		if (AngelscriptSettings->bMarkNonUpropertyPropertiesAsNotReplicated && ClassData.NewClass->bIsStruct)
		{
			// Properties without a UPROPERTY macro are marked as NotReplicated by default to avoid unintentional replication
			PropDesc->bSkipReplication = true;
		}

		ClassData.NewClass->Properties.Add(PropDesc);
	}

	for (auto PropertyDesc : ClassData.NewClass->Properties)
	{
		// Check if this property should be added as a FProperty

		int32* ScriptIndexPtr = PropertyIndexMap.Find(PropertyDesc->PropertyName);
		if (ScriptIndexPtr != nullptr)
		{
			int32 ScriptIndex = *ScriptIndexPtr;
			const char* Name;
			int PropertyOffset;
			int TypeId;

			bool bIsPrivate;
			bool bIsProtected;

			ScriptType->GetProperty(ScriptIndex, &Name, &TypeId, &bIsPrivate, &bIsProtected, &PropertyOffset);

			auto PropertyType = PropertyTypes[ScriptIndex];
			if (!PropertyType.IsValid() || !PropertyType.CanCreateProperty())
			{
				// Emit an error, this type is not valid for usage as FProperty
				FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, PropertyDesc->LineNumber, FString::Printf(
					TEXT("Property %s %s in class %s has a type that cannot be a UPROPERTY."),
					*PropertyType.GetAngelscriptDeclaration(), *PropertyDesc->PropertyName, *ClassData.NewClass->ClassName));
				ModuleData.ReloadReq = EReloadRequirement::Error;
				break;
			}

			PropertyDesc->PropertyType = PropertyType;
			PropertyDesc->ScriptPropertyIndex = ScriptIndex;
			PropertyDesc->ScriptPropertyOffset = (SIZE_T)PropertyOffset;

			PropertyDesc->bIsPrivate = bIsPrivate;
			PropertyDesc->bIsProtected = bIsProtected;

#if WITH_EDITOR
			if (PropertyDesc->Meta.Contains(NAME_Actor_DefaultComponent))
			{
				// If the property is a default component and a subobject of that name exists in the code parent, error
				UClass* CodeSuperClass = ClassData.NewClass->CodeSuperClass;
				UObject* CodeCDO = CodeSuperClass->GetDefaultObject();
				if (CodeCDO->GetDefaultSubobjectByName(*PropertyDesc->PropertyName) != nullptr)
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, PropertyDesc->LineNumber, FString::Printf(
						TEXT("Component with name %s in class %s already exists in parent class."), *PropertyDesc->PropertyName, *ClassData.NewClass->ClassName));
					ClassData.ReloadReq = EReloadRequirement::Error;
				}

				// Default component properties can only be subclasses of UActorComponent
				UClass* PropertyCodeSuper = ResolveCodeSuperForProperty(PropertyType);
				if (PropertyCodeSuper == nullptr || !PropertyCodeSuper->IsChildOf(UActorComponent::StaticClass()))
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, PropertyDesc->LineNumber, FString::Printf(
						TEXT("DefaultComponent with name %s and type %s in class %s does not derive from UActorComponent."),
						*PropertyDesc->PropertyName, *PropertyType.GetAngelscriptDeclaration(), *ClassData.NewClass->ClassName));
					ClassData.ReloadReq = EReloadRequirement::Error;
				}
			}

			if(PropertyDesc->Meta.Contains(NAME_Actor_BindComponent))
			{
				// Default component properties can only be subclasses of UActorComponent
				UClass* PropertyCodeSuper = ResolveCodeSuperForProperty(PropertyType);
				if (PropertyCodeSuper == nullptr || !PropertyCodeSuper->IsChildOf(UActorComponent::StaticClass()))
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, PropertyDesc->LineNumber, FString::Printf(
						TEXT("BindComponent with name %s and type %s in class %s does not derive from UActorComponent."),
						*PropertyDesc->PropertyName, *PropertyType.GetAngelscriptDeclaration(), *ClassData.NewClass->ClassName));
					ClassData.ReloadReq = EReloadRequirement::Error;
				}
			}
#endif
		}
		else
		{
			FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, PropertyDesc->LineNumber, FString::Printf(
				TEXT("Could not find property %s in class %s to be a UPROPERTY()."), *PropertyDesc->PropertyName, *ClassData.NewClass->ClassName));
			ClassData.ReloadReq = EReloadRequirement::Error;
		}
	}

	// Collect all functions in the script class for later lookup
	TMap<FString, TArray<asCScriptFunction*>> FunctionMap;
	if (ScriptType != nullptr)
	{
		for (int32 i = 0, MethodCount = ScriptType->GetMethodCount(); i < MethodCount; ++i)
		{
			asCScriptFunction* ScriptFunction = (asCScriptFunction*)ScriptType->GetMethodByIndex(i);

			// Don't consider functions declared in superclasses here, they will be bound by
			// the superclass when we analyze that.
			if (ScriptFunction->GetObjectType() != ScriptType)
				continue;

			FString FunctionName = ANSI_TO_TCHAR(ScriptFunction->GetName());
			FunctionMap.FindOrAdd(FunctionName).Add(ScriptFunction);
		}
	}
	else
	{
		asCModule* ScriptModule = (asCModule*)ModuleData.NewModule->ScriptModule;
		for (int32 i = 0, FunctionCount = ScriptModule->GetFunctionCount(); i < FunctionCount; ++i)
		{
			asCScriptFunction* ScriptFunction = (asCScriptFunction*)ScriptModule->GetFunctionByIndex(i);

			FString FunctionName = ANSI_TO_TCHAR(ScriptFunction->GetName());
			FunctionMap.FindOrAdd(FunctionName).Add(ScriptFunction);
		}
	}

#if WITH_EDITOR
	// Expensive check, so editor only. Check to make sure that 
	// functions that we expect to be UFUNCTION(BlueprintOverride) are
	// actually tagged as such.
	{
		UClass* CodeSuperClass = ClassData.NewClass->CodeSuperClass;

		for (auto& Elem : FunctionMap)
		{
			auto FunctionDesc = ClassData.NewClass->GetMethod(Elem.Key);
			if (FunctionDesc.IsValid() && FunctionDesc->bBlueprintOverride)
				continue;

			bool bHaveParentFunction = false;
			bool bParentIsEvent = false;
			bool bParentIsCpp = false;
			FString ParentName;

			auto* UnrealParentFunction = GetBlueprintEventByScriptName(CodeSuperClass, Elem.Key);
			if (UnrealParentFunction != nullptr)
			{
				bHaveParentFunction = true;
				bParentIsCpp = true;
				bParentIsEvent = UnrealParentFunction->HasAnyFunctionFlags(FUNC_BlueprintEvent);
				ParentName = UnrealParentFunction->GetOuterUClass()->GetName();
			}

			if (!bHaveParentFunction)
			{
				auto CheckSuperClass = AngelscriptSuperClass;
				while (CheckSuperClass.IsValid())
				{
					auto ScriptParentFunction = CheckSuperClass->GetMethod(Elem.Key);
					if (ScriptParentFunction.IsValid())
					{
						bHaveParentFunction = true;
						ParentName = CheckSuperClass->ClassName;

						if (ScriptParentFunction->bBlueprintOverride)
							bParentIsEvent = true;
						else if (ScriptParentFunction->bBlueprintEvent)
							bParentIsEvent = true;
						break;
					}

					if (CheckSuperClass->bSuperIsCodeClass)
						break;

					CheckSuperClass = EnsureClassAnalyzed(CheckSuperClass->SuperClass);
				}
			}

			if (bHaveParentFunction && (FunctionDesc.IsValid() || bParentIsEvent))
			{
				int32 LineNumber = 0;
				auto* ScriptFunction = (asCScriptFunction*)Elem.Value[0];
				if (ScriptFunction != nullptr && ScriptFunction->scriptData != nullptr)
					LineNumber = ScriptFunction->scriptData->declaredAt & 0xFFFFF;

				if (bParentIsEvent)
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, LineNumber, FString::Printf(
						TEXT("Method %s is a BlueprintEvent in parent class %s, override requires the BlueprintOverride function specifier."),
						*Elem.Key, *ParentName));
				}
				else if (bParentIsCpp)
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, LineNumber, FString::Printf(
						TEXT("Method %s already exists in parent class %s, but is not a BlueprintEvent and cannot be overridden."),
						*Elem.Key, *ParentName));
				}
				else
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, LineNumber, FString::Printf(
						TEXT("Method %s is already a UFUNCTION in parent class %s. Use the BlueprintEvent/BlueprintOverride specifiers, or remove UFUNCTION and use the angelscript `override` keyword."),
						*Elem.Key, *ParentName));
				}

				ClassData.ReloadReq = EReloadRequirement::Error;
			}
		}
	}
#endif

	// Determine whether the class is threadsafe
	// Determine if the function is thread safe or in a thread safe class
	const bool bClassThreadSafe = ClassData.NewClass->Meta.Contains(FUNCMETA_BlueprintThreadSafe);

	// Analyze all the functions we want to bind
	for (auto FunctionDesc : ClassData.NewClass->Methods)
	{
		// If there are multiple script functions with this name,
		// we can't bind.
		auto* FunctionList = FunctionMap.Find(FunctionDesc->ScriptFunctionName);
		if (FunctionList == nullptr)
		{
			FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
				TEXT("Could not find function %s in class %s to be a UFUNCTION()."), *FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
			ClassData.ReloadReq = EReloadRequirement::Error;
			continue;
		}
		if (FunctionList->Num() != 1)
		{
			FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
				TEXT("Multiple methods with name %s in class %s found. UFUNCTION()s must have unique names."), *FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
			ClassData.ReloadReq = EReloadRequirement::Error;
			continue;
		}

		asCScriptFunction* ScriptFunction = (asCScriptFunction*)(*FunctionList)[0];

		FunctionDesc->ScriptFunction = ScriptFunction;
		FunctionDesc->bIsPrivate = ScriptFunction->IsPrivate();
		FunctionDesc->bIsProtected = ScriptFunction->IsProtected();

		if (!bLoadedPrecompiledCode)
		{
			if (bClassThreadSafe)
				FunctionDesc->bThreadSafe = !FunctionDesc->Meta.Contains(FUNCMETA_NotBlueprintThreadSafe);
			else
				FunctionDesc->bThreadSafe = FunctionDesc->Meta.Contains(FUNCMETA_BlueprintThreadSafe);

			if (FunctionDesc->bBlueprintEvent || FunctionDesc->bBlueprintOverride)
				FunctionDesc->bIsNoOp = ScriptFunction->IsNoOp();
		}

		if (ScriptFunction->IsReadOnly())
			FunctionDesc->bIsConstMethod = true;

		int32 ReturnTypeId = ScriptFunction->GetReturnTypeId();
		if (ReturnTypeId != asTYPEID_VOID)
		{
			FunctionDesc->ReturnType = FAngelscriptTypeUsage::FromReturn(ScriptFunction);
			if (!FunctionDesc->ReturnType.IsValid() || !FunctionDesc->ReturnType.CanCreateProperty() || !FunctionDesc->ReturnType.CanBeReturned() || FunctionDesc->ReturnType.bIsReference)
			{
				if (FunctionDesc->ReturnType.bIsReference)
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
						TEXT("UFUNCTIONs cannot return references, function %s in class %s"), *FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
				}
				else
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
						TEXT("Unknown or invalid return type to function %s in class %s"), *FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
				}

				ClassData.ReloadReq = EReloadRequirement::Error;
				continue;
			}
		}

		int32 ArgCount = ScriptFunction->GetParamCount();
		for (int32 i = 0; i < ArgCount; ++i)
		{
			const char* ParamName = nullptr;
			const char* ParamDefaultValue = nullptr;
			asDWORD RefFlags = 0;
			ScriptFunction->GetParam(i, nullptr, &RefFlags, &ParamName, &ParamDefaultValue);

			auto Type = FAngelscriptTypeUsage::FromParam(ScriptFunction, i);
			if (!Type.IsValid() || !Type.CanBeArgument() || !Type.CanCreateProperty())
			{
				FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
					TEXT("Unknown or invalid parameter type for parameter %s to function %s in class %s"), 
					ANSI_TO_TCHAR(ParamName), *FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));

				ClassData.ReloadReq = EReloadRequirement::Error;
				break;
			}

			FAngelscriptArgumentDesc ArgDesc;
			ArgDesc.Type = Type;
			ArgDesc.ArgumentName = ANSI_TO_TCHAR(ParamName);
			ArgDesc.DefaultValue = ANSI_TO_TCHAR(ParamDefaultValue);

			// Some types of arguments are forced to be outparam refs
			if (Type.IsValid() && Type.Type->IsParamForcedOutParam() && Type.bIsConst)
			{
				ArgDesc.bInRefForceCopyOut = true;
				ArgDesc.bBlueprintInRef = true;
			}
			else if (Type.bIsReference)
			{
				if ((RefFlags & asTM_INOUTREF) == asTM_INOUTREF)
				{
					if (Type.bIsConst)
					{
						ArgDesc.bBlueprintByValue = true;
					}
					else
					{
						ArgDesc.bBlueprintInRef = true;
					}
				}
				else if ((RefFlags & asTM_OUTREF) != 0)
				{
					ArgDesc.bBlueprintOutRef = true;
				}
				else
				{
					ArgDesc.bBlueprintInRef = true;
				}
			}
			else
			{
				ArgDesc.bBlueprintByValue = true;
			}

			// Object arguments named WorldContext are automatically marked
			// as world context pins, if we don't have one yet.
			if (Type.Type->IsObjectPointer() && ArgDesc.ArgumentName.Equals(STR_Arg_WorldContext, ESearchCase::IgnoreCase))
			{
				if (!FunctionDesc->Meta.Contains(NAME_Arg_WorldContext))
					FunctionDesc->Meta.Add(NAME_Arg_WorldContext, ArgDesc.ArgumentName);
			}

			// When used in BP, certain names are reserved and illegal to use for parameters
			if (FunctionDesc->bBlueprintCallable || FunctionDesc->bBlueprintEvent)
			{
				if (ArgDesc.ArgumentName == TEXT("self"))
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
						TEXT("Parameter name '%s' is reserved and invalid for parameters on BlueprintCallable functions."), 
						*ArgDesc.ArgumentName));
					ClassData.ReloadReq = EReloadRequirement::Error;
				}
			}

			FunctionDesc->Arguments.Add(ArgDesc);
		}

		// Check that BlueprintPure has a return value
		if (FunctionDesc->bBlueprintPure)
		{
			bool bHasOutParams = false;

			if (FunctionDesc->ReturnType.IsValid())
				bHasOutParams = true;

			for (auto& Param : FunctionDesc->Arguments)
			{
				if (Param.Type.bIsReference && !Param.Type.bIsConst)
					bHasOutParams = true;
			}

			if (!bHasOutParams)
			{
				FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
					TEXT("BlueprintPure method %s in class %s must have return value."), *FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
				ClassData.ReloadReq = EReloadRequirement::Error;
			}
		}

		UClass* CodeSuperClass = ClassData.NewClass->CodeSuperClass;

		// Check that BlueprintCallable/BlueprintEvent doesn't bump into a superclass function
		if ((FunctionDesc->bBlueprintCallable || FunctionDesc->bBlueprintEvent) && !FunctionDesc->bBlueprintOverride)
		{
			UFunction* ParentFunction = CodeSuperClass->FindFunctionByName(*FunctionDesc->FunctionName);
			if (ParentFunction != nullptr)
			{
				FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
					TEXT("%s method %s in class %s already specified in superclass %s."),
					FunctionDesc->bBlueprintEvent ? TEXT("BlueprintEvent") : TEXT("BlueprintCallable"),
					*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName, *ClassData.NewClass->SuperClass));
				ClassData.ReloadReq = EReloadRequirement::Error;
			}
			else
			{
				if (AngelscriptSuperClass.IsValid())
				{
					TSharedPtr<FAngelscriptFunctionDesc> ParentScriptFunction = AngelscriptSuperClass->GetMethod(FunctionDesc->FunctionName);
					if (!FunctionDesc->bBlueprintEvent)
					{
						if (ParentScriptFunction.IsValid() && 
							((!ParentScriptFunction->SignatureMatches(FunctionDesc) && ParentScriptFunction->bBlueprintCallable)
								 || ParentScriptFunction->bBlueprintEvent))
						{
							// Function exists in parent script class, but with a different signature
							FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
								TEXT("BlueprintCallable method %s in class %s is specified in superclass %s with a different signature."),
								*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName, *ClassData.NewClass->SuperClass));
							ClassData.ReloadReq = EReloadRequirement::Error;
						}
					}
					else
					{
						if (ParentScriptFunction.IsValid() && (ParentScriptFunction->bBlueprintCallable || ParentScriptFunction->bBlueprintEvent))
						{
							// Function exists in parent script class, but with a different signature
							FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
								TEXT("BlueprintEvent method %s in class %s is already specified in superclass %s."),
								*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName, *ClassData.NewClass->SuperClass));
							ClassData.ReloadReq = EReloadRequirement::Error;
						}
					}

				}
			}
		}

		// Check that BlueprintOverride actually overrides something from a superclass
		if (FunctionDesc->bBlueprintOverride)
		{
			// Check if we should use a displayname override for this function
			FunctionDesc->OriginalFunctionName = FunctionDesc->FunctionName;
			auto* ParentFunction = GetBlueprintEventByScriptName(CodeSuperClass, FunctionDesc->FunctionName);
                                                                                                 
            if (ParentFunction == nullptr)
            {
                FAngelscriptTypeGenerationContext Context;
                Context.Point = EAngelscriptTypeGenerationPoint::ResolveFunction;
                Context.Class = ClassData.NewClass.Get();
                Context.Function = &FunctionDesc.Get();
                FString Error;
                if (!DispatchAngelscriptTypeGeneration(Context, Error))
                {
                    FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, Error);
                    ClassData.ReloadReq = EReloadRequirement::Error;
                }
                ParentFunction = Context.ResolvedFunction;
            }
                                                                                               

			if (ParentFunction != nullptr)
				FunctionDesc->FunctionName = ParentFunction->GetName();

			if (ParentFunction == nullptr)
			{
				if (AngelscriptSuperClass.IsValid())
				{
					// Check if our angelscript superclass will create this event before we override it
					TSharedPtr<FAngelscriptClassDesc> CheckSuperClass = AngelscriptSuperClass;
					TSharedPtr<FAngelscriptFunctionDesc> SuperFunctionDesc;

					while (CheckSuperClass.IsValid())
					{
						SuperFunctionDesc = CheckSuperClass->GetMethod(FunctionDesc->FunctionName);
						if (SuperFunctionDesc.IsValid())
							break;

						if (!CheckSuperClass->bSuperIsCodeClass)
							CheckSuperClass = EnsureClassAnalyzed(CheckSuperClass->SuperClass);
						else
							break;
					}

					if (!SuperFunctionDesc.IsValid())
					{
						FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
							TEXT("BlueprintOverride method %s in class %s does not exist in superclass %s."),
							*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName, *AngelscriptSuperClass->ClassName));
						ClassData.ReloadReq = EReloadRequirement::Error;
					}
					else if (!SuperFunctionDesc->bBlueprintEvent && !SuperFunctionDesc->bBlueprintOverride)
					{
						FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
							TEXT("BlueprintOverride method %s in class %s is not marked BlueprintEvent in superclass %s."),
							*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName, *AngelscriptSuperClass->ClassName));
						ClassData.ReloadReq = EReloadRequirement::Error;
					}
					else if (!SuperFunctionDesc->SignatureMatches(FunctionDesc))
					{
						FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
							TEXT("BlueprintOverride method %s in class %s does not match signature of event declared in superclass %s."),
							*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName, *AngelscriptSuperClass->ClassName));
						ClassData.ReloadReq = EReloadRequirement::Error;
					}
					else if (SuperFunctionDesc->Meta.Contains(NAME_Meta_EditorOnly) && !FunctionDesc->Meta.Contains(NAME_Meta_EditorOnly))
					{
						FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
							TEXT("BlueprintOverride method %s in class %s overrides an editor-only parent function, but is not in editor-only code."),
							*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
						ClassData.ReloadReq = EReloadRequirement::Error;
					}
				}
				else
				{
					if (auto* NonEvent = CodeSuperClass->FindFunctionByName(*FunctionDesc->FunctionName))
					{
#if WITH_EDITOR
						bool bShouldUseScriptName = false;
						FString ScriptName = FAngelscriptFunctionSignature::GetScriptNameForFunction(NonEvent);

						if (NonEvent->HasAnyFunctionFlags(FUNC_BlueprintEvent))
						{
							if (ScriptName != FunctionDesc->FunctionName
								&& GetBlueprintEventByScriptName(CodeSuperClass, ScriptName) != nullptr)
							{
								bShouldUseScriptName = true;
							}
						}

						if (bShouldUseScriptName)
						{
							FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
								TEXT("Use name `%s` instead to override C++ event %s in parent class %s, as it has a ScriptName or stripped prefix."),
								*ScriptName, *NonEvent->GetName(), *NonEvent->GetOwnerClass()->GetName()));
							ClassData.ReloadReq = EReloadRequirement::Error;
						}
						else
#endif
						{
							FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
								TEXT("Method %s in parent class %s is not a BlueprintImplementableEvent or BlueprintNativeEvent in C++ and cannot be overridden."),
								*NonEvent->GetName(), *NonEvent->GetOwnerClass()->GetName()));
							ClassData.ReloadReq = EReloadRequirement::Error;
						}
					}
					else
					{
						FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
							TEXT("BlueprintOverride method %s in class %s does not exist in superclass %s, or is not a BlueprintImplementableEvent or BlueprintNativeEvent in C++."),
							*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName, *CodeSuperClass->GetName()));
						ClassData.ReloadReq = EReloadRequirement::Error;
					}
				}
			}
			else
			{
				// Make sure our function signature is the same as the event we're overriding
				bool bASReturnsVoid = !FunctionDesc->ReturnType.IsValid();
				bool bUEReturnsVoid = ParentFunction->GetReturnProperty() == nullptr;

				bool bArgCountMismatch = false;
				bool bTypeMismatch = false;
				if (bASReturnsVoid != bUEReturnsVoid)
				{
					bTypeMismatch = true;
				}

				if (!bASReturnsVoid && !bUEReturnsVoid)
				{
					// If the UE return value is a float, but the script one is a double, we need to use our special extendo type for that
					if (FunctionDesc->ReturnType.Type == FAngelscriptType::ScriptDoubleType())
					{
						if (ParentFunction->GetReturnProperty()->IsA<FFloatProperty>())
						{
							FunctionDesc->ReturnType.Type = FAngelscriptType::ScriptFloatParamExtendedToDoubleType();
							ScriptFunction->returnType.SetFloatExtendedToDouble(true);
						}
					}

					if (!FunctionDesc->ReturnType.MatchesProperty(ParentFunction->GetReturnProperty(), FAngelscriptType::EPropertyMatchType::OverrideReturnValue))
						bTypeMismatch = true;
				}


				int32 UEParmCount = ParentFunction->NumParms;
				if (!bUEReturnsVoid)
					UEParmCount -= 1;

				if (FunctionDesc->Arguments.Num() != UEParmCount)
					bArgCountMismatch = true;

				int32 ArgumentIndex = 0;
				for (TFieldIterator<FProperty> It(ParentFunction); It && (It->PropertyFlags & CPF_Parm); ++It)
				{
					FProperty* Property = *It;
					if (Property->PropertyFlags & CPF_ReturnParm)
						continue;

					// Dummy arguments can match any reference to a struct
					if (Property->GetFName() == NAME_AnyStructRef)
					{
						if (FunctionDesc->Arguments.Num() == 0)
						{
							bArgCountMismatch = false;
							continue;
						}

						auto& OverrideArg = FunctionDesc->Arguments[ArgumentIndex];
						if (!OverrideArg.Type.bIsReference
							|| OverrideArg.Type.bIsConst != Property->HasAnyPropertyFlags(CPF_ConstParm)
							|| !OverrideArg.Type.Type.IsValid()
							|| !OverrideArg.Type.Type->IsUnrealStruct())
						{
							bTypeMismatch = true;
						}
						else
						{
							OverrideArg.bBlueprintByValue = false;
							OverrideArg.bBlueprintInRef = true;
							OverrideArg.bBlueprintOutRef = false;
						}

						continue;
					}

					// Check regular argument signatures
					if (ArgumentIndex < FunctionDesc->Arguments.Num())
					{
						auto& OverrideArg = FunctionDesc->Arguments[ArgumentIndex];
						OverrideArg.ArgumentName = Property->GetName();

						// If the UE parameter is a float, but the script one is a double, we need to use our special extendo type for that
						if (OverrideArg.Type.Type == FAngelscriptType::ScriptDoubleType() && Property->IsA<FFloatProperty>())
						{
							OverrideArg.Type.Type = FAngelscriptType::ScriptFloatParamExtendedToDoubleType();
							ScriptFunction->parameterTypes[ArgumentIndex].SetFloatExtendedToDouble(true);
						}

						if (!FunctionDesc->Arguments[ArgumentIndex].Type.MatchesProperty(Property, FAngelscriptType::EPropertyMatchType::OverrideArgument))
							bTypeMismatch = true;

						if (OverrideArg.Type.bIsReference)
						{
							if (Property->HasAnyPropertyFlags(CPF_ReferenceParm))
							{
								OverrideArg.bBlueprintByValue = false;
								OverrideArg.bBlueprintInRef = true;
								OverrideArg.bBlueprintOutRef = false;

								if (Property->HasAnyPropertyFlags(CPF_OutParm))
									OverrideArg.bInRefForceCopyOut = true;
								else
									ensure(false);

								if (Property->HasAnyPropertyFlags(CPF_ConstParm) != OverrideArg.Type.bIsConst)
									bTypeMismatch = true;
							}
							else if (Property->HasAnyPropertyFlags(CPF_OutParm))
							{
								OverrideArg.bBlueprintByValue = false;
								OverrideArg.bBlueprintInRef = false;
								OverrideArg.bBlueprintOutRef = true;

								if (OverrideArg.Type.bIsConst)
									bTypeMismatch = true;
							}
							else
							{
								OverrideArg.bBlueprintByValue = true;
								OverrideArg.bBlueprintInRef = false;
								OverrideArg.bBlueprintOutRef = false;

								if (!OverrideArg.Type.bIsConst)
									bTypeMismatch = true;
							}
						}
						else
						{
							OverrideArg.bBlueprintByValue = true;
							OverrideArg.bBlueprintInRef = false;
							OverrideArg.bBlueprintOutRef = false;

							if (Property->HasAnyPropertyFlags(CPF_ReferenceParm) || Property->HasAnyPropertyFlags(CPF_OutParm))
								bTypeMismatch = true;
						}
					}

					ArgumentIndex += 1;
				}

				if (bTypeMismatch || bArgCountMismatch)
				{
					FString ExpectedSignature;
					if (ScriptType != nullptr)
					{
						asIScriptFunction* ScriptParentFunction = ScriptType->GetMethodByName(TCHAR_TO_ANSI(*FunctionDesc->OriginalFunctionName));
						if (ScriptParentFunction != nullptr)
						{
							ExpectedSignature = ANSI_TO_TCHAR(ScriptParentFunction->GetDeclaration(false, false, true, true));
						}
					}

					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
						TEXT("BlueprintOverride method %s in class %s does not match function signature of event in superclass %s.\nExpected Signature: %s"),
						*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName, *CodeSuperClass->GetName(), *ExpectedSignature));
					ClassData.ReloadReq = EReloadRequirement::Error;
				}

				// Check const correctness separately so we can emit a more specific error
				if (ParentFunction->HasAnyFunctionFlags(FUNC_Const))
				{
					if (!ScriptFunction->IsReadOnly())
					{
						FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
							TEXT("BlueprintOverride method %s in class %s is not const, but is overriding a const method. Please add 'const' to the end of the function declaration."),
							*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
						ClassData.ReloadReq = EReloadRequirement::Error;
					}
				}
				else
				{
					if (ScriptFunction->IsReadOnly())
					{
						FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
							TEXT("BlueprintOverride method %s in class %s is specified as const, but is overriding a non const method. Please remove 'const' from the end of the function declaration."),
							*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
						ClassData.ReloadReq = EReloadRequirement::Error;
					}
				}

				if (ParentFunction->HasAnyFunctionFlags(FUNC_EditorOnly) && !FunctionDesc->Meta.Contains(NAME_Meta_EditorOnly))
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
						TEXT("BlueprintOverride method %s in class %s overrides an editor-only parent function, but is not in editor-only code."),
						*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
					ClassData.ReloadReq = EReloadRequirement::Error;
				}
			}
		}
	}

	// Analyze if any function needs validation (we do this after the loop above to ensure all functions have their Return type and other values initialized)
	for (auto FunctionDesc : ClassData.NewClass->Methods)
	{
		if (FunctionDesc->bNetValidate)
		{
			auto ValidateFunction = ClassData.NewClass->GetMethod(FunctionDesc->FunctionName + "_Validate");
			if (ValidateFunction)
			{
				if (ValidateFunction->ScriptFunction->GetReturnTypeId() != asTYPEID_BOOL)
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, ValidateFunction->LineNumber, FString::Printf(
						TEXT("UFUNCTION() %s in class %s has a _Validate function that is returning a non-bool!"),
						*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
					ClassData.ReloadReq = EReloadRequirement::Error;
				}
				else if (!FunctionDesc->ParametersMatches(ValidateFunction))
				{
					FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, ValidateFunction->LineNumber, FString::Printf(
						TEXT("UFUNCTION() %s in class %s has a _Validate function but the parameters don't match!"),
						*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
					ClassData.ReloadReq = EReloadRequirement::Error;
				}
			}
			else
			{
				FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, FunctionDesc->LineNumber, FString::Printf(
					TEXT("UFUNCTION() %s in class %s is marked as WithValidate but no _Validate function provided! Is it marked as UFUNCTION()?"),
					*FunctionDesc->FunctionName, *ClassData.NewClass->ClassName));
				ClassData.ReloadReq = EReloadRequirement::Error;
			}
		}
	}

	

	UStruct* PrevStruct = nullptr;
	if (ClassData.OldClass.IsValid())
	{
		if (ClassData.OldClass->Class != nullptr)
			PrevStruct = ClassData.OldClass->Class;
		else if (ClassData.OldClass->Struct != nullptr)
			PrevStruct = ClassData.OldClass->Struct;
	}
#if AS_CAN_HOTRELOAD
	else if (FAngelscriptManager::Get().bIsInitialCompileFinished)
	{
		// It's possible we're replacing a class that was previously removed
		UASClass* ReplacedClass = FindObject<UASClass>(FAngelscriptManager::GetPackage(), *UnrealName);
		if (ReplacedClass != nullptr)
		{
			if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
				ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
		}
	}
#endif

	if (PrevStruct != nullptr)
	{
		// If our superclass changed, we need a full reload
		if (ClassData.OldClass->SuperClass != ClassData.NewClass->SuperClass)
		{
			if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
				ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
		}

		// Check if any properties from the old class have been
		// removed or changed type.
		for (auto OldPropertyDesc : ClassData.OldClass->Properties)
		{
			bool bFound = false;
			for (auto PropertyDesc : ClassData.NewClass->Properties)
			{
				if (PropertyDesc->PropertyName == OldPropertyDesc->PropertyName)
				{
					bFound = true;

					// If the property type has changes, we must do a full reload
					if (PropertyDesc->PropertyType != OldPropertyDesc->PropertyType)
					{
						if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
						{
							ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
							ClassData.ReloadReqLines.AddUnique(PropertyDesc->LineNumber);
						}
					}

					// If the definition has changed, we must do a full reload
					if (!PropertyDesc->IsDefinitionEquivalent(*OldPropertyDesc))
					{
						if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
						{
							ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
							ClassData.ReloadReqLines.AddUnique(PropertyDesc->LineNumber);
						}
					}

					// If the metadata changed suggest a full reload
					if (!PropertyDesc->Meta.OrderIndependentCompareEqual(OldPropertyDesc->Meta))
					{
						if (ClassData.ReloadReq < EReloadRequirement::FullReloadSuggested)
						{
							ClassData.ReloadReq = EReloadRequirement::FullReloadSuggested;
							ClassData.ReloadReqLines.AddUnique(PropertyDesc->LineNumber);
						}
					}
				}
			}

			// If any properties were removed, we must do a full reload
			if (!bFound)
			{
				if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
				{
					ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
					ClassData.ReloadReqLines.AddUnique(OldPropertyDesc->LineNumber);
				}
			}
		}

		// Check if we added any new properties that weren't in the old class.
		for (auto PropertyDesc : ClassData.NewClass->Properties)
		{
			TSharedPtr<FAngelscriptPropertyDesc> PreviousProperty;
			bool bFound = false;
			for (auto OldPropertyDesc : ClassData.OldClass->Properties)
			{
				if (PropertyDesc->PropertyName == OldPropertyDesc->PropertyName
					&& OldPropertyDesc->bHasUnrealProperty)
				{
					PreviousProperty = OldPropertyDesc;
					bFound = true;
					break;
				}
			}

			// If we added a new property, we should suggest a full reload so we can use it
			if (!bFound)
			{
				if (ClassData.NewClass->bIsStruct || PropertyDesc->PropertyType.RequiresProperty())
				{
					// If the property was required to be added, we need to do a full
					// reload and can't just suggest one.
					if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
					{
						ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
						ClassData.ReloadReqLines.AddUnique(PropertyDesc->LineNumber);
					}
				}
				else
				{
					if (ClassData.ReloadReq < EReloadRequirement::FullReloadSuggested)
					{
						ClassData.ReloadReq = EReloadRequirement::FullReloadSuggested;
						ClassData.ReloadReqLines.AddUnique(PropertyDesc->LineNumber);
					}
				}
			}
			else
			{
				// In structs, changing the offset of a property also requires a full reload,
				// since we can't loop over all instances of a struct to change them.
				if (ClassData.NewClass->bIsStruct)
				{
					if (PreviousProperty->ScriptPropertyOffset != PropertyDesc->ScriptPropertyOffset)
					{
						if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
						{
							ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
							ClassData.ReloadReqLines.AddUnique(PropertyDesc->LineNumber);
						}
					}
				}
			}
		}

		// Check if any bound methods from the old class have been removed or changed signature
		for (auto OldFunctionDesc : ClassData.OldClass->Methods)
		{
			auto NewFunctionDesc = ClassData.NewClass->GetMethod(OldFunctionDesc->FunctionName);
			if (!NewFunctionDesc.IsValid())
			{
				// Method was removed, need full reload
				if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
				{
					ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
					ClassData.ReloadReqLines.AddUnique(OldFunctionDesc->LineNumber);
				}
			}
			else
			{
				if (!OldFunctionDesc->SignatureMatches(NewFunctionDesc))
				{
					// Method changed signature, need full reload
					if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
					{
						ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
						ClassData.ReloadReqLines.AddUnique(NewFunctionDesc->LineNumber);
					}
				}
				else
				{
					// Check if any default values have changed
					for (int32 ArgIndex = 0, ArgCount = NewFunctionDesc->Arguments.Num(); ArgIndex < ArgCount; ++ArgIndex)
					{
						if (!ensure(OldFunctionDesc->Arguments.IsValidIndex(ArgIndex)))
							continue;
						auto& NewArgument = NewFunctionDesc->Arguments[ArgIndex];
						auto& OldArgument = OldFunctionDesc->Arguments[ArgIndex];
						if (NewArgument.DefaultValue != OldArgument.DefaultValue
							|| NewArgument.ArgumentName != OldArgument.ArgumentName)
						{
							// We should suggest a full reload to propagate this change
							if (ClassData.ReloadReq < EReloadRequirement::FullReloadSuggested)
							{
								ClassData.ReloadReq = EReloadRequirement::FullReloadSuggested;
								ClassData.ReloadReqLines.AddUnique(NewFunctionDesc->LineNumber);
							}
						}
					}
				}

				// If the definition has changed, we must do a full reload
				if (!OldFunctionDesc->IsDefinitionEquivalent(*NewFunctionDesc))
				{
					if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
					{
						ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
						ClassData.ReloadReqLines.AddUnique(NewFunctionDesc->LineNumber);
					}
				}

				// If the metadata changed suggest a full reload
				if (!NewFunctionDesc->Meta.OrderIndependentCompareEqual(OldFunctionDesc->Meta))
				{
					if (ClassData.ReloadReq < EReloadRequirement::FullReloadSuggested)
					{
						ClassData.ReloadReq = EReloadRequirement::FullReloadSuggested;
						ClassData.ReloadReqLines.AddUnique(NewFunctionDesc->LineNumber);
					}
				}
			}
		}

		// Check if we added any bound methods that weren't in the old class
		for (auto NewFunctionDesc : ClassData.NewClass->Methods)
		{
			auto OldFunctionDesc = ClassData.OldClass->GetMethod(NewFunctionDesc->FunctionName);
			if (!OldFunctionDesc.IsValid() || OldFunctionDesc->Function == nullptr)
			{
				// We added a new function, we should suggest a full reload but not require it
				if (ClassData.ReloadReq < EReloadRequirement::FullReloadSuggested)
				{
					ClassData.ReloadReq = EReloadRequirement::FullReloadSuggested;
					ClassData.ReloadReqLines.AddUnique(NewFunctionDesc->LineNumber);
				}

				// Unless this is a BlueprintEvent, which requires a full reload to be added
				// since the event thunk calls back into the blueprint vm to handle the virtualness
				if (NewFunctionDesc->bBlueprintEvent)
				{
					if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
					{
						ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
						ClassData.ReloadReqLines.AddUnique(NewFunctionDesc->LineNumber);
					}
				}
			}
		}

		// If we changed code in 'default' statements, we need to suggest a full reload
		// to propagate the changes to properties properly.
		if (ClassData.OldClass->DefaultsCode != ClassData.NewClass->DefaultsCode)
		{
			if (ClassData.ReloadReq < EReloadRequirement::FullReloadSuggested)
				ClassData.ReloadReq = EReloadRequirement::FullReloadSuggested;
		}

		// If the size of the new script type is larger than the old one (+debug slack),
		// then we can't replace the script objects in-place, so we need a full reload
		UASClass* OldClass = (UASClass*)ClassData.OldClass->Class;
		if (OldClass != nullptr && ScriptType != nullptr)
		{
			int32 ScriptSize = ScriptType->GetSize();
			if (ScriptSize > OldClass->GetPropertiesSize())
			{
				if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
					ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
			}
		}

		// If the metadata changed suggest a full reload
		if (!ClassData.NewClass->Meta.OrderIndependentCompareEqual(ClassData.OldClass->Meta))
		{
			if (ClassData.ReloadReq < EReloadRequirement::FullReloadSuggested)
				ClassData.ReloadReq = EReloadRequirement::FullReloadSuggested;
		}

		// If one of the class' flags have changed, we should try to do a full reload
		if (!ClassData.NewClass->AreFlagsEqual(*ClassData.OldClass.Get()))
		{
			if (ClassData.ReloadReq < EReloadRequirement::FullReloadSuggested)
				ClassData.ReloadReq = EReloadRequirement::FullReloadSuggested;
		}
	}
	else
	{
		// This is a new class, if we don't do a full reload things will get confused
		if (ClassData.ReloadReq < EReloadRequirement::FullReloadRequired)
			ClassData.ReloadReq = EReloadRequirement::FullReloadRequired;
	}

	// Make sure any composed structs we're composing have the ComposedStruct metatag on their property
	// as well, or weird stuff will happen.
#if WITH_EDITOR
	if ((ClassData.NewClass->bIsStruct && ClassData.NewClass->Meta.Contains("ComposedStruct"))
		|| !ClassData.NewClass->ComposeOntoClass.IsEmpty())
	{
		for (auto Property : ClassData.NewClass->Properties)
		{
			if (Property->Meta.Contains("NoCompose"))
				continue;
			if (Property->Meta.Contains("CustomCompose"))
				continue;
			if (Property->Meta.Contains("ComposedStruct"))
				continue;

			if (!Property->PropertyType.Type.IsValid())
				continue;

			if (Property->PropertyType.Type != FAngelscriptType::GetScriptStruct())
				continue;

			auto StructData = GetClassDesc(ANSI_TO_TCHAR(Property->PropertyType.ScriptClass->GetName()));
			if (!StructData.IsValid())
				continue;

			if (StructData->Meta.Contains("ComposedStruct"))
			{
				FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, Property->LineNumber, FString::Printf(
					TEXT("Composed struct property %s does not have ComposedStruct meta in its UPROPERTY()."),
					*Property->PropertyName));
				ClassData.ReloadReq = EReloadRequirement::Error;
			}
		}
	}
#endif

#if WITH_EDITOR
	// Apply additional compile errors that the code class wants if applicable
	if (ClassData.NewClass.IsValid())
	{
		auto& AdditionalCompileChecks = FAngelscriptManager::Get().AdditionalCompileChecks;
		UClass* CodeParent = ClassData.NewClass->CodeSuperClass;
		while (CodeParent != nullptr)
		{
			auto* CheckBind = AdditionalCompileChecks.Find(CodeParent);
			if (CheckBind != nullptr && (*CheckBind).IsValid())
			{
				if (!(*CheckBind)->ScriptCompileAdditionalChecks(ModuleData.NewModule, ClassData.NewClass))
				{
					ClassData.ReloadReq = EReloadRequirement::Error;
				}
			}

			CodeParent = CodeParent->GetSuperClass();
		}
	}
#endif

	ClassData.bAnalyzed = true;
}

void FAngelscriptClassGenerator::Analyze(FModuleData& ModuleData, FDelegateData& DelegateData)
{
	// Ignore if we've already analyzed this delegate
	if (DelegateData.bAnalyzed)
		return;
	DelegateData.bAnalyzed = true;

	auto DelegateDesc = DelegateData.NewDelegate;

	// Check already compiled modules for conflicting delegates
	TSharedPtr<FAngelscriptModuleDesc> FoundInModule;
                                                                                                              
	auto ExistingDelegate = FAngelscriptManager::Get().GetDelegate(ModuleData.NewModule->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf
		? ModuleData.NewModule->ScriptUnit.Id + TEXT("::") + DelegateDesc->DelegateName : DelegateDesc->DelegateName, &FoundInModule);
                                                                                                            
	if (ExistingDelegate.IsValid() && FoundInModule.IsValid())
	{
		// Only allow this if the module it was in is being reloaded
		if (!IsReloadingModule(FoundInModule))
		{
			FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, 1, FString::Printf(
				TEXT("Delegate/Event %s in module %s already exists in module %s."),
				*DelegateDesc->DelegateName, *ModuleData.NewModule->ModuleName, *FoundInModule->ModuleName));
			ModuleData.ReloadReq = EReloadRequirement::Error;
		}
	}

	// Check other modules we're reloading for conflicting delegates
	for (auto& OtherModule : Modules)
	{
		for (auto& OtherDelegate : OtherModule.Delegates)
		{
			if (OtherDelegate.NewDelegate->DelegateName == DelegateDesc->DelegateName
				&& DelegateDesc != OtherDelegate.NewDelegate)
			{
				FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, 1, FString::Printf(
					TEXT("Delegate/Event %s in module %s already exists in module %s."),
					*DelegateDesc->DelegateName, *ModuleData.NewModule->ModuleName, *OtherModule.NewModule->ModuleName));
				ModuleData.ReloadReq = EReloadRequirement::Error;
			}
		}
	}

	// Resolve the compiled script type for the delegate
	auto* ScriptType = DelegateDesc->ScriptType;

	if (ScriptType == nullptr)
	{
		FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, DelegateData.NewDelegate->LineNumber, FString::Printf(
			TEXT("Could not find generated struct for delegate type %s"), *DelegateDesc->DelegateName));
		DelegateData.ReloadReq = EReloadRequirement::Error;
		return;
	}

	if (DelegateData.OldDelegate.IsValid() && DelegateData.OldDelegate->ScriptType)
		UpdatedScriptTypeMap.Add(DelegateData.OldDelegate->ScriptType, ScriptType);

	// Find the signature function in the delegate's struct class
	asIScriptFunction* ScriptSignature = nullptr;

	if (DelegateDesc->bIsMulticast)
		ScriptSignature = ScriptType->GetMethodByName("Broadcast");
	else
		ScriptSignature = ScriptType->GetMethodByName("Execute");

	if (ScriptSignature == nullptr)
	{
		FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, DelegateData.NewDelegate->LineNumber, FString::Printf(
			TEXT("Could not find generated execute method for delegate type %s"), *DelegateDesc->DelegateName));
		DelegateData.ReloadReq = EReloadRequirement::Error;
		return;
	}

	auto FunctionDesc = MakeShared<FAngelscriptFunctionDesc>();
	DelegateDesc->Signature = FunctionDesc;

	int32 ReturnTypeId = ScriptSignature->GetReturnTypeId();
	if (ReturnTypeId != asTYPEID_VOID)
	{
		FunctionDesc->ReturnType = FAngelscriptTypeUsage::FromReturn(ScriptSignature);
		if (!FunctionDesc->ReturnType.IsValid() || !FunctionDesc->ReturnType.CanCreateProperty() || !FunctionDesc->ReturnType.CanBeReturned())
		{
			FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, DelegateData.NewDelegate->LineNumber, FString::Printf(
				TEXT("Unknown or invalid return type to function %s in delegate %s."), *FunctionDesc->FunctionName, *DelegateDesc->DelegateName));
			DelegateData.ReloadReq = EReloadRequirement::Error;
		}
	}

	int32 ArgCount = ScriptSignature->GetParamCount();
	for (int32 i = 0; i < ArgCount; ++i)
	{
		const char* ParamName = nullptr;
		const char* ParamDefaultValue = nullptr;
		asDWORD RefFlags = 0;
		ScriptSignature->GetParam(i, nullptr, &RefFlags, &ParamName, &ParamDefaultValue);

		auto Type = FAngelscriptTypeUsage::FromParam(ScriptSignature, i);
		if (!Type.IsValid() || !Type.CanBeArgument() || !Type.CanCreateProperty())
		{
			FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, DelegateData.NewDelegate->LineNumber, FString::Printf(
				TEXT("Unknown or invalid parameter type for parameter %s to delegate %s."), 
				ANSI_TO_TCHAR(ParamName), *DelegateDesc->DelegateName));
			DelegateData.ReloadReq = EReloadRequirement::Error;
			break;
		}

		FAngelscriptArgumentDesc ArgDesc;
		ArgDesc.Type = Type;
		ArgDesc.ArgumentName = ANSI_TO_TCHAR(ParamName);
		ArgDesc.DefaultValue = ANSI_TO_TCHAR(ParamDefaultValue);

		// Some types of arguments are forced to be outparam refs
		if (Type.IsValid() && Type.Type->IsParamForcedOutParam() && Type.bIsConst)
		{
			ArgDesc.bInRefForceCopyOut = true;
			ArgDesc.bBlueprintInRef = true;
		}
		else if (Type.bIsReference)
		{
			if ((RefFlags & asTM_INOUTREF) == asTM_INOUTREF)
			{
				if (Type.bIsConst)
				{
					ArgDesc.bBlueprintByValue = true;
				}
				else
				{
					ArgDesc.bBlueprintInRef = true;
				}
			}
			else if ((RefFlags & asTM_OUTREF) != 0)
			{
				ArgDesc.bBlueprintOutRef = true;
			}
			else
			{
				ArgDesc.bBlueprintInRef = true;
			}
		}
		else
		{
			ArgDesc.bBlueprintByValue = true;
		}

		FunctionDesc->Arguments.Add(ArgDesc);
	}

	if (DelegateData.OldDelegate.IsValid())
	{
		if (!DelegateData.OldDelegate->Signature.IsValid()
			|| !DelegateData.OldDelegate->Signature->SignatureMatches(FunctionDesc, true)
			|| !DelegateData.OldDelegate->Signature->IsDefinitionEquivalent(*FunctionDesc))
		{
			// Signature changed, need full reload
			if (DelegateData.ReloadReq < EReloadRequirement::FullReloadRequired)
			{
				DelegateData.ReloadReq = EReloadRequirement::FullReloadRequired;
				DelegateData.ReloadReqLines.AddUnique(DelegateDesc->LineNumber);
			}
		}
	}
	else
	{
		// New delegate, suggest a full reload
		if (DelegateData.ReloadReq < EReloadRequirement::FullReloadRequired)
		{
			DelegateData.ReloadReq = EReloadRequirement::FullReloadRequired;
			DelegateData.ReloadReqLines.AddUnique(DelegateDesc->LineNumber);
		}
	}
}

void FAngelscriptClassGenerator::InitEnums(FModuleData& ModuleData)
{
	auto ModuleDesc = ModuleData.NewModule;
	asIScriptModule* ScriptModule = ModuleData.NewModule->ScriptModule;
	if (ScriptModule == nullptr)
	{
		ensure(false);
		return;
	}

	// Create new enum descriptors for all enums found in the script module
	int32 EnumCount = ScriptModule->GetEnumCount();
	for (int32 i = 0; i < EnumCount; ++i)
	{
		asITypeInfo* EnumType = ScriptModule->GetEnumByIndex(i);
		FString EnumName = ANSI_TO_TCHAR(EnumType->GetName());

		// The preprocessor might have already created an enum for this
		auto EnumDesc = ModuleData.NewModule->GetEnum(EnumName);
		if (!EnumDesc.IsValid())
		{
			EnumDesc = MakeShared<FAngelscriptEnumDesc>();
			EnumDesc->EnumName = MoveTemp(EnumName);
			ModuleData.NewModule->Enums.Add(EnumDesc.ToSharedRef());

#if WITH_EDITOR
			EnumDesc->LineNumber = ((asCTypeInfo*)EnumType)->declaredAt & 0xFFFFF;
#endif
		}

		EnumDesc->ScriptType = EnumType;

		FEnumData EnumData;
		EnumData.NewEnum = EnumDesc;
		EnumData.DataIndex = ModuleData.Enums.Num();

		// Add all values from script into the enum
		int32 ValueCount = EnumType->GetEnumValueCount();
		for (int32 v = 0; v < ValueCount; ++v)
		{
			int32 Value;
			FString Name = ANSI_TO_TCHAR(EnumType->GetEnumValueByIndex(v, &Value));
			EnumDesc->ValueNames.Add(*Name);
			EnumDesc->EnumValues.Add(Value);
		}

		// Look up the previous descriptor for the enum
		if (ModuleData.OldModule.IsValid())
		{
			EnumData.OldEnum = ModuleData.OldModule->GetEnum(EnumDesc->EnumName);
			if (EnumData.OldEnum.IsValid())
			{
				EnumData.NewEnum->Enum = EnumData.OldEnum->Enum;
			}
		}

		ModuleData.Enums.Add(EnumData);

		check(!DataRefByNewScriptType.Contains(EnumType));

		DataRefByNewScriptType.Add(EnumType, FDataRef(ModuleData, EnumData));
		DataRefByName.Add(EnumDesc->EnumName, FDataRef(ModuleData, EnumData));
	}
}

bool FAngelscriptClassGenerator::IsReloadingModule(TSharedPtr<FAngelscriptModuleDesc> Module)
{
	for (auto& ModuleData : Modules)
	{
		if (ModuleData.OldModule == Module)
			return true;
		if (ModuleData.NewModule == Module)
			return true;
	}
	return false;
}

void FAngelscriptClassGenerator::AnalyzeEnums(FModuleData& ModuleData)
{
	// Make sure our enums don't collide with any other existing enums
	for (auto& EnumData : ModuleData.Enums)
	{
		auto EnumDesc = EnumData.NewEnum;

		// Check already compiled modules
		TSharedPtr<FAngelscriptModuleDesc> FoundInModule;
                                                                                                          
	auto ExistingEnum = FAngelscriptManager::Get().GetEnum(ModuleData.NewModule->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf
		? ModuleData.NewModule->ScriptUnit.Id + TEXT("::") + EnumDesc->EnumName : EnumDesc->EnumName, &FoundInModule);
                                                                                                        
		if (ExistingEnum.IsValid() && FoundInModule.IsValid())
		{
			// Only allow this if the module it was in is being reloaded
			if (!IsReloadingModule(FoundInModule))
			{
				FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, 1, FString::Printf(
					TEXT("Enum %s in module %s already exists in module %s."),
					*EnumDesc->EnumName, *ModuleData.NewModule->ModuleName, *FoundInModule->ModuleName));
				ModuleData.ReloadReq = EReloadRequirement::Error;
			}
		}

		// Make sure the unreal name is not being used
		// NB: Enums here actually _do_ have the `E` prefix in their unreal name!
		FString UnrealName = EnumDesc->EnumName;
                                                                                                           
	if (ModuleData.NewModule->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf) UnrealName += TEXT("__") + ModuleData.NewModule->ScriptUnit.Id;
                                                                                                         
		if (UsedUnrealNames.Contains(UnrealName))
		{
			FAngelscriptManager::Get().ScriptCompileError(ModuleData.NewModule, EnumDesc->LineNumber, FString::Printf(
				TEXT("Name conflict: unreal name %s for script enum %s is already in use."),
				*UnrealName, *EnumDesc->EnumName));
			ModuleData.ReloadReq = EReloadRequirement::Error;
		}
		UsedUnrealNames.Add(UnrealName);

		// Enums that suggested a full reload before should continue to suggest them now
		if (EnumData.OldEnum.IsValid() && EnumData.OldEnum->bPendingFullReload && ModuleData.ReloadReq < EReloadRequirement::FullReloadSuggested)
		{
			ModuleData.ReloadReq = EReloadRequirement::FullReloadSuggested;
			EnumData.bNeedReload = true;
		}

		// Check if we've changed the enum
		if (EnumData.OldEnum.IsValid())
		{
			if (EnumData.NewEnum->ValueNames != EnumData.OldEnum->ValueNames
				|| EnumData.NewEnum->EnumValues != EnumData.OldEnum->EnumValues)
			{
				if (ModuleData.ReloadReq < EReloadRequirement::FullReloadSuggested)
				{
					ModuleData.ReloadReq = EReloadRequirement::FullReloadSuggested;
					ModuleData.ReloadReqLines.AddUnique(EnumDesc->LineNumber);
				}
				EnumData.bNeedReload = true;
			}

#if WITH_EDITOR
			if (EnumData.NewEnum->Documentation != EnumData.OldEnum->Documentation)
			{
				if (ModuleData.ReloadReq < EReloadRequirement::FullReloadSuggested)
				{
					ModuleData.ReloadReq = EReloadRequirement::FullReloadSuggested;
					ModuleData.ReloadReqLines.AddUnique(EnumDesc->LineNumber);
				}
				EnumData.bNeedReload = true;
			}
#endif

			// Check if the metadata has changed
			if (!EnumData.NewEnum->Meta.OrderIndependentCompareEqual(EnumData.OldEnum->Meta))
			{
				if (ModuleData.ReloadReq < EReloadRequirement::FullReloadSuggested)
				{
					ModuleData.ReloadReq = EReloadRequirement::FullReloadSuggested;
					ModuleData.ReloadReqLines.AddUnique(EnumDesc->LineNumber);
				}
				EnumData.bNeedReload = true;
			}
		}

		// Check if we've added the enum
		if (!EnumData.OldEnum.IsValid() || EnumData.OldEnum->Enum == nullptr)
		{
			if (ModuleData.ReloadReq < EReloadRequirement::FullReloadSuggested)
			{
				ModuleData.ReloadReq = EReloadRequirement::FullReloadSuggested;
				ModuleData.ReloadReqLines.AddUnique(EnumDesc->LineNumber);
			}
			EnumData.bNeedReload = true;
		}
	}
}

void FAngelscriptClassGenerator::SetupModule(FModuleData& ModuleData)
{
	// Create internal class data for each class
	for (auto ClassDesc : ModuleData.NewModule->Classes)
	{	
		FClassData ClassData;
		ClassData.NewClass = ClassDesc;
		ClassData.DataIndex = ModuleData.Classes.Num();

		// Find the old class we're replacing
		if (ModuleData.OldModule.IsValid())
		{
			for (auto OldClassDesc : ModuleData.OldModule->Classes)
			{
				if (OldClassDesc->ClassName == ClassDesc->ClassName
					&& OldClassDesc->bIsStruct == ClassDesc->bIsStruct)
				{
					ClassData.OldClass = OldClassDesc;
					break;
				}
			}
		}
		
		ModuleData.Classes.Add(ClassData);

		// Store lookup for this class
		if (!ClassDesc->bIsStaticsClass)
		{
			auto* ScriptType = GetNamespacedTypeInfoForClass(ClassData.NewClass, ModuleData.NewModule);
			ClassData.NewClass->ScriptType = ScriptType;

			check(!DataRefByNewScriptType.Contains(ScriptType));

			DataRefByNewScriptType.Add(ClassData.NewClass->ScriptType, FDataRef(ModuleData, ClassData));
			DataRefByName.Add(ClassData.NewClass->ClassName, FDataRef(ModuleData, ClassData));
		}
	}

	// Create internal delegate data
	for (auto DelegateDesc : ModuleData.NewModule->Delegates)
	{
		FDelegateData DelegateData;
		DelegateData.NewDelegate = DelegateDesc;
		DelegateData.DataIndex = ModuleData.Delegates.Num();

		// Find the old delegate we're replacing
		if (ModuleData.OldModule.IsValid())
		{
			for (auto OldDelegateDesc : ModuleData.OldModule->Delegates)
			{
				if (OldDelegateDesc->DelegateName == DelegateDesc->DelegateName)
				{
					DelegateData.OldDelegate = OldDelegateDesc;
					break;
				}
			}
		}

		ModuleData.Delegates.Add(DelegateData);

		// Store lookup for this delegate
		auto* ScriptType = ModuleData.NewModule->ScriptModule->GetTypeInfoByName(TCHAR_TO_ANSI(*DelegateDesc->DelegateName));
		DelegateData.NewDelegate->ScriptType = ScriptType;

		// Tag the script type as a delegate so we can classify it before the
		// actual delegate signature function is generated.
		if (DelegateDesc->bIsMulticast)
			ScriptType->SetUserData(FAngelscriptType::TAG_UserData_Multicast_Delegate);
		else
			ScriptType->SetUserData(FAngelscriptType::TAG_UserData_Delegate);

		check(!DataRefByNewScriptType.Contains(ScriptType));

		DataRefByNewScriptType.Add(DelegateData.NewDelegate->ScriptType, FDataRef(ModuleData, DelegateData));
		DataRefByName.Add(DelegateData.NewDelegate->DelegateName, FDataRef(ModuleData, DelegateData));
	}
}

void FAngelscriptClassGenerator::Analyze(FModuleData& ModuleData)
{
	// Analyze each delegate in the module
	for (auto& DelegateData : ModuleData.Delegates)
	{
		Analyze(ModuleData, DelegateData);

		// The delegate that requires the highest reload type should determine it
		if (DelegateData.ReloadReq > ModuleData.ReloadReq)
		{
			ModuleData.ReloadReq = DelegateData.ReloadReq;
#if WITH_EDITOR
			ModuleData.ReloadReqLines.AddUnique(DelegateData.NewDelegate->LineNumber);
#endif
		}

#if WITH_EDITOR
		for (int32 ReloadLine : DelegateData.ReloadReqLines)
			ModuleData.ReloadReqLines.AddUnique(ReloadLine);
#endif
	}

	// Analyze each class in the module
	for (auto& ClassData : ModuleData.Classes)
	{
		Analyze(ModuleData, ClassData);

		// The class that requires the highest reload type should determine it
		if (ClassData.ReloadReq > ModuleData.ReloadReq)
		{
			ModuleData.ReloadReq = ClassData.ReloadReq;
#if WITH_EDITOR
			ModuleData.ReloadReqLines.AddUnique(ClassData.NewClass->LineNumber);
#endif
		}

#if WITH_EDITOR
		for (int32 ReloadLine : ClassData.ReloadReqLines)
			ModuleData.ReloadReqLines.AddUnique(ReloadLine);
#endif
	}

	// Make sure enums from the file are analyzed
	InitEnums(ModuleData);
	AnalyzeEnums(ModuleData);

	// If any classes from the old module aren't in the new module,
	// immediately require a full reload.
	if (ModuleData.OldModule.IsValid())
	{
		for (auto OldClassDesc : ModuleData.OldModule->Classes)
		{
			if (!ModuleData.NewModule->GetClass(OldClassDesc->ClassName).IsValid())
			{
				ModuleData.RemovedClasses.Add(OldClassDesc);
				if (ModuleData.ReloadReq < EReloadRequirement::FullReloadRequired)
					ModuleData.ReloadReq = EReloadRequirement::FullReloadRequired;
			}
		}
	}
}

                                                                                                                          
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
bool FAngelscriptClassGenerator::ValidateExplicitFullReloadBindingShape(
	const FString& CanonicalTarget,
	const TArray<TArray<FString>>& CanonicalModuleCodePaths,
	int32& OutTargetModuleIndex,
	int32 QualifyingWitnessCount)
{
	OutTargetModuleIndex = INDEX_NONE;
	int32 MatchingTargetModules = 0;
	for (int32 ModuleIndex = 0; ModuleIndex < CanonicalModuleCodePaths.Num(); ++ModuleIndex)
	{
		int32 MatchingTargetSections = 0;
		for (const FString& CanonicalSection : CanonicalModuleCodePaths[ModuleIndex])
		{
#if PLATFORM_WINDOWS
			if (CanonicalTarget.Equals(CanonicalSection, ESearchCase::IgnoreCase))
#else
			if (CanonicalTarget.Equals(CanonicalSection, ESearchCase::CaseSensitive))
#endif
			{
				++MatchingTargetSections;
			}
		}

		if (MatchingTargetSections == 0)
			continue;

		++MatchingTargetModules;
		if (MatchingTargetModules > 1
			|| MatchingTargetSections != 1
			|| CanonicalModuleCodePaths[ModuleIndex].Num() != 1)
		{
			return false;
		}
		OutTargetModuleIndex = ModuleIndex;
	}

	return MatchingTargetModules == 1
		&& OutTargetModuleIndex != INDEX_NONE
		&& (QualifyingWitnessCount == INDEX_NONE || QualifyingWitnessCount > 0);
}

bool FAngelscriptClassGenerator::HasDistinctExplicitFullReloadMarkerPair(
	const UASClass* OldClass,
	const UASClass* NewClass)
{
	return OldClass != nullptr && NewClass != nullptr && OldClass != NewClass;
}

#endif
                                                                                                                        
                                                                                                                               
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING

bool FAngelscriptClassGenerator::ValidateAndMarkExplicitFullReloadTarget()
{
	FAngelscriptManager& Manager = FAngelscriptManager::Get();
	if (!Manager.bForcedFullReloadExecution)
		return true;

	Manager.bForcedFullReloadBindingValidated = false;
	Manager.ForcedFullReloadMarkerOldClass = nullptr;
	Manager.ForcedFullReloadMarkerNewClass = nullptr;

	FString CanonicalTarget;
	if (!Manager.CanonicalizeExplicitFullReloadFilename(
		Manager.ForcedFullReloadFile.AbsolutePath,
		CanonicalTarget))
	{
		return false;
	}

	TArray<TArray<FString>> CanonicalModuleCodePaths;
	CanonicalModuleCodePaths.Reserve(Modules.Num());

                                                                                                                             
                                                                                                                                   
	for (FModuleData& ModuleData : Modules)
	{
		TArray<FString>& CanonicalCodePaths = CanonicalModuleCodePaths.AddDefaulted_GetRef();
		if (!ModuleData.NewModule.IsValid())
			continue;

		for (const FAngelscriptModuleDesc::FCodeSection& Section : ModuleData.NewModule->Code)
		{
			FString CanonicalSection;
			if (!Manager.CanonicalizeExplicitFullReloadFilename(Section.AbsoluteFilename, CanonicalSection))
				return false;
			CanonicalCodePaths.Add(MoveTemp(CanonicalSection));
		}
	}

	int32 TargetModuleIndex = INDEX_NONE;
	if (!ValidateExplicitFullReloadBindingShape(
		CanonicalTarget,
		CanonicalModuleCodePaths,
		TargetModuleIndex))
	{
		return false;
	}
	FModuleData& TargetModule = Modules[TargetModuleIndex];
	if (!Manager.IsExplicitFullReloadTarget(
		CanonicalTarget,
		CanonicalModuleCodePaths[TargetModuleIndex][0]))
	{
		return false;
	}

	UASClass* WitnessOldClass = nullptr;
	for (FClassData& ClassData : TargetModule.Classes)
	{
		if (!ClassData.NewClass.IsValid()
			|| ClassData.NewClass->bIsStaticsClass
			|| ClassData.NewClass->bIsStruct
			|| !ClassData.OldClass.IsValid())
		{
			continue;
		}

		WitnessOldClass = Cast<UASClass>(ClassData.OldClass->Class);
		if (WitnessOldClass != nullptr)
			break;
	}

	// Setup runs before PerformReload creates new UASClass objects.  The old side
	// is therefore the available witness here; the callback records its new pair.
	if (WitnessOldClass == nullptr)
		return false;
	if (!ValidateExplicitFullReloadBindingShape(
		CanonicalTarget,
		CanonicalModuleCodePaths,
		TargetModuleIndex,
		1))
	{
		return false;
	}

	for (FClassData& ClassData : TargetModule.Classes)
	{
		if (ClassData.NewClass.IsValid() && !ClassData.NewClass->bIsStaticsClass)
			ClassData.bExplicitFullReloadTarget = true;
	}

	Manager.ForcedFullReloadMarkerOldClass = WitnessOldClass;
	Manager.ForcedFullReloadMarkerNewClass = nullptr;
	Manager.bForcedFullReloadBindingValidated = true;
	return true;
}
#endif
                                                                                                                                 
FAngelscriptClassGenerator::EReloadRequirement FAngelscriptClassGenerator::Setup()
{
	FAngelscriptScopeTimer Timer(TEXT("class generator analysis"));

	// Create data structures for each module we're generating for to use during analysis
	for (auto& ModuleData : Modules)
		SetupModule(ModuleData);

	// Analyze all modules we're generating classes for
	for (auto& ModuleData : Modules)
		Analyze(ModuleData);

	// Make sure all classes have the reload requirements of their
	// dependencies propagated to them.
	for (auto& ModuleData : Modules)
	{
		for (auto& ClassData : ModuleData.Classes)
		{
			PropagateReloadRequirements(ModuleData, ClassData);
		}
		for (auto& DelegateData : ModuleData.Delegates)
		{
			PropagateReloadRequirements(ModuleData, DelegateData);
		}
	}
	                                                                                                             
	for (auto& ModuleData : Modules)
	{
		for (const auto& ClassData : ModuleData.Classes)
		{
			const auto& ClassDesc = ClassData.NewClass;
			if (ClassDesc->bIsStruct)
				continue;

			const UClass* InvalidBlueprintAncestor = nullptr;
			if (UASClass::HasValidScriptClassCarrierAncestry(
				ClassDesc->CodeSuperClass, &InvalidBlueprintAncestor))
			{
				continue;
			}

			const FString Reason = InvalidBlueprintAncestor != nullptr
				? FString::Printf(
					TEXT("Blueprint ancestor '%s' is not registered as a BlueprintDependencyClass."),
					*InvalidBlueprintAncestor->GetPathName())
				: TEXT("The script class ancestry contains a cycle.");
			FAngelscriptManager::Get().ScriptCompileError(
				ModuleData.NewModule,
				ClassDesc->LineNumber,
				FString::Printf(
					TEXT("ASR-CLASS-CARRIER-INVALID: script class '%s' cannot use this carrier: %s"),
					*ClassDesc->ClassName,
					*Reason));
			ModuleData.NewModule->bModuleSwapInError = true;
			ModuleData.ReloadReq = EReloadRequirement::Error;
			return EReloadRequirement::Error;
		}
	}
	                                                                                                           

                                                                                                                              
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
	if (FAngelscriptManager::Get().bForcedFullReloadExecution
		&& !ValidateAndMarkExplicitFullReloadTarget())
	{
		return EReloadRequirement::Error;
	}
#endif
                                                                                                                            
	// Determine what kind of reload we require
	EReloadRequirement ReloadReq = EReloadRequirement::SoftReload;
	for (auto& ModuleData : Modules)
	{
		if (ModuleData.ReloadReq > ReloadReq)
			ReloadReq = ModuleData.ReloadReq;
	}
	return ReloadReq;
}

void FAngelscriptClassGenerator::AddReloadDependency(FReloadPropagation* Source, const FAngelscriptTypeUsage& Type)
{
	EReloadRequirement Req = EReloadRequirement::SoftReload;

	// Recursively propagate for any subtypes in this type (ex. array element type)
	for (const FAngelscriptTypeUsage& SubType : Type.SubTypes)
		AddReloadDependency(Source, SubType);

	// Types that don't have a script class will never reload
	if (Type.ScriptClass == nullptr || !(Type.ScriptClass->GetFlags() & asOBJ_SCRIPT_OBJECT))
		return;

	AddReloadDependency(Source, Type.ScriptClass);
}

void FAngelscriptClassGenerator::AddReloadDependency(FReloadPropagation* Source, asITypeInfo* TypeInfo)
{
	if (TypeInfo == nullptr)
		return;

	FDataRef* Ref = DataRefByNewScriptType.Find(TypeInfo);
	if (Ref != nullptr)
	{
		FModuleData& ModuleData = Modules[Ref->ModuleIndex];
		if (Ref->bIsClass)
		{
			FClassData& ClassData = ModuleData.Classes[Ref->DataIndex];
			check(ClassData.NewClass->ScriptType == TypeInfo);

			PropagateReloadRequirements(ModuleData, ClassData);
			if (!ClassData.bFinishedPropagating || ClassData.bHasOutstandingDependencies)
			{
				ClassData.PendingDependees.AddUnique(Source);
				Source->bHasOutstandingDependencies = true;
			}

			if (ClassData.ReloadReq > Source->ReloadReq)
				Source->ReloadReq = ClassData.ReloadReq;
		}
		else if (Ref->bIsDelegate)
		{
			FDelegateData& DelegateData = ModuleData.Delegates[Ref->DataIndex];
			check(DelegateData.NewDelegate->ScriptType == TypeInfo);

			PropagateReloadRequirements(ModuleData, DelegateData);
			if (!DelegateData.bFinishedPropagating || DelegateData.bHasOutstandingDependencies)
			{
				DelegateData.PendingDependees.AddUnique(Source);
				Source->bHasOutstandingDependencies = true;
			}

			if (DelegateData.ReloadReq > Source->ReloadReq)
				Source->ReloadReq = DelegateData.ReloadReq;
		}
	}
	else
	{
		// If there's any subtypes, we should depend on those as well
		int32 SubTypeCount = TypeInfo->GetSubTypeCount();
		if (SubTypeCount != 0)
		{
			for (int32 i = 0; i < SubTypeCount; ++i)
				AddReloadDependency(Source, TypeInfo->GetSubType(i));
		}
	}
}

void FAngelscriptClassGenerator::PropagateReloadRequirements(FModuleData& ModuleData, FClassData& ClassData)
{
	if (ClassData.bStartedPropagating)
		return;
	ClassData.bStartedPropagating = true;

	// Don't need to propagate if we're already forcing a full reload
	if (ClassData.ReloadReq >= EReloadRequirement::FullReloadRequired)
		return;

	auto ClassDesc = ClassData.NewClass;

	if (!ClassDesc->bSuperIsCodeClass)
	{
		FModuleData* OtherModule = nullptr;
		FClassData* OtherClass = nullptr;
		FDelegateData* OtherDelegate = nullptr;
		asITypeInfo* SuperScriptType = ClassDesc->ScriptType->GetBaseType();

		// Check if it's a class we're reloading
		if (SuperScriptType != nullptr)
			AddReloadDependency(&ClassData, SuperScriptType);
	}

	if (ClassData.NewClass->ScriptType != nullptr)
	{
		asCObjectType* ObjType = (asCObjectType*)ClassData.NewClass->ScriptType;
		int PropCount = ObjType->localProperties.GetLength();
		for (int PropIndex = 0; PropIndex < PropCount; ++PropIndex)
		{
			asCObjectProperty* Prop = ObjType->localProperties[PropIndex];
			if (Prop->type.IsObject())
				AddReloadDependency(&ClassData, Prop->type.GetTypeInfo());
		}

		int MethodCount = ObjType->methods.GetLength();
		for (int MethodIndex = 0; MethodIndex < MethodCount; ++MethodIndex)
		{
			asCScriptFunction* Func = (asCScriptFunction*)ObjType->engine->GetFunctionById(ObjType->methods[MethodIndex]);
			if (Func->returnType.IsObject())
				AddReloadDependency(&ClassData, Func->returnType.GetTypeInfo());

			int ParamCount = Func->parameterTypes.GetLength();
			for (int ParamIndex = 0; ParamIndex < ParamCount; ++ParamIndex)
			{
				if (Func->parameterTypes[ParamIndex].IsObject())
					AddReloadDependency(&ClassData, Func->parameterTypes[ParamIndex].GetTypeInfo());
			}
		}
	}
	else
	{
		for (auto Property : ClassDesc->Properties)
		{
			AddReloadDependency(&ClassData, Property->PropertyType);
		}

		for (auto Function : ClassDesc->Methods)
		{
			AddReloadDependency(&ClassData, Function->ReturnType);
			for (auto Argument : Function->Arguments)
				AddReloadDependency(&ClassData, Argument.Type);
		}
	}

	ClassData.bFinishedPropagating = true;
	ResolvePendingReloadDependees(&ClassData);
}

void FAngelscriptClassGenerator::PropagateReloadRequirements(FModuleData& ModuleData, FDelegateData& DelegateData)
{
	if (DelegateData.bStartedPropagating)
		return;
	DelegateData.bStartedPropagating = true;

	// Don't need to propagate if we're already forcing a full reload
	if (DelegateData.ReloadReq >= EReloadRequirement::FullReloadRequired)
		return;

	auto Function = DelegateData.NewDelegate->Signature;
	AddReloadDependency(&DelegateData, Function->ReturnType);
	for (auto Argument : Function->Arguments)
		AddReloadDependency(&DelegateData, Argument.Type);

	DelegateData.bFinishedPropagating = true;
	ResolvePendingReloadDependees(&DelegateData);
}

void FAngelscriptClassGenerator::ResolvePendingReloadDependees(FReloadPropagation* Source)
{
	check(Source->bFinishedPropagating);

	// Anything that was marked dependent on us before we finished propagation should
	// receive our latest reload requirement via recursive push.
	for (FReloadPropagation* Dependee : Source->PendingDependees)
	{
		if (Source->ReloadReq > Dependee->ReloadReq)
		{
			Dependee->ReloadReq = Source->ReloadReq;

			// Need to recurse so we apply the same reload requirement forward
			ResolvePendingReloadDependees(Dependee);
		}
	}
}

bool FAngelscriptClassGenerator::ShouldFullReload(FClassData& Class)
{
                                                                                                                                     
	if (bIsDoingFullReload && Class.bExplicitFullReloadTarget)
		return true;
                                                                                                                                   
	return bIsDoingFullReload && Class.ReloadReq >= EReloadRequirement::FullReloadSuggested;
}

bool FAngelscriptClassGenerator::ShouldFullReload(FEnumData& Enum)
{
	return bIsDoingFullReload && Enum.bNeedReload;
}

bool FAngelscriptClassGenerator::ShouldFullReload(FDelegateData& Delegate)
{
	return bIsDoingFullReload && Delegate.ReloadReq >= EReloadRequirement::FullReloadSuggested;
}

                                                                                                                     
bool FAngelscriptClassGenerator::PrepareModuleTransaction(FString& Error)
{
	check(!bModuleTransactionPrepared && !bPreparingModuleTransaction);
	for (auto& Module : Modules)
	{
		auto Reject = [&](const FString& Identity)
		{
			Error = FString::Printf(TEXT("Isolated class generation cannot prepare shared or lifecycle identity '%s::%s'; the previous generation is unchanged."),
				*Module.NewModule->ModuleName, *Identity);
			return false;
		};
		for (const auto& Enum : Module.Enums) return Reject(Enum.NewEnum->EnumName);
		for (const auto& Delegate : Module.Delegates) return Reject(Delegate.NewDelegate->DelegateName);
		for (const auto& Removed : Module.RemovedClasses) return Reject(Removed->ClassName);
		for (auto& Class : Module.Classes)
		{
			const auto& Desc = Class.NewClass;
			if (Desc->bIsStruct || Desc->CodeSuperClass->IsChildOf(UDynamicSubsystem::StaticClass())
				|| Desc->CodeSuperClass->IsChildOf(UWorldSubsystem::StaticClass())) return Reject(Desc->ClassName);
		}
	}
	// An otherwise-soft class would mutate its old UClass and instances in place.
	// Give every class in this admitted closure a separate candidate instead.
	for (auto& Module : Modules)
		for (auto& Class : Module.Classes) Class.ReloadReq = EReloadRequirement::FullReloadRequired;
	TGuardValue<bool> Preparing(bPreparingModuleTransaction, true);
	PerformReload(true);
	for (const auto& Module : Modules)
		if (Module.NewModule->bModuleSwapInError || Module.NewModule->bCompileError)
		{
			Error = FString::Printf(TEXT("Candidate class generation failed for module '%s'; the previous generation is unchanged."), *Module.NewModule->ModuleName);
			return false;
		}
	for (const auto& Module : Modules)
		for (const auto& Class : Module.Classes)
			if (Class.ReplacedClass)
			{
				const FString Retired = FString::Printf(TEXT("%s_REPLACED_%d"), *Class.ReplacedClass->GetName(), UniqueCounter());
				if (!Class.ReplacedClass->Rename(*Retired, nullptr, REN_DontCreateRedirectors | REN_Test))
				{ Error = TEXT("The previous class cannot enter its retained namespace; the previous generation is unchanged."); return false; }
			}
	return true;
}

bool FAngelscriptClassGenerator::CommitModuleTransaction(FString& Error)
{
	check(bModuleTransactionPrepared && !bPreparingModuleTransaction);
#if WITH_EDITOR
	if (FAngelscriptManager::bIsInitialCompileFinished)
	{
		TArray<UObject*> PreviousTypes;
		for (const auto& Module : Modules)
			for (const auto& Class : Module.Classes)
				if (Class.ReplacedClass) PreviousTypes.Add(Class.ReplacedClass);
		OnPreFullReload.Broadcast(PreviousTypes);
	}
#endif
	for (auto& Module : Modules)
		for (auto& Class : Module.Classes)
		{
			UASClass* Candidate = CastChecked<UASClass>(Class.NewClass->Class);
			FString Canonical = GetUnrealName(false, Class.NewClass->ClassName);
			if (Module.NewModule->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf)
				Canonical += TEXT("__") + Module.NewModule->ScriptUnit.Id;
			if (Class.ReplacedClass)
			{
				const FString Retired = FString::Printf(TEXT("%s_REPLACED_%d"), *Canonical, UniqueCounter());
				if (!Class.ReplacedClass->Rename(*Retired, nullptr, REN_DontCreateRedirectors))
				{ Error = TEXT("Previous class retirement failed at the generation commit boundary."); return false; }
				Class.ReplacedClass->ClassFlags |= CLASS_NewerVersionExists;
				Class.ReplacedClass->NewerVersion = Candidate;
			}
			if (!Candidate->Rename(*Canonical, nullptr, REN_DontCreateRedirectors))
			{ Error = TEXT("Candidate class publication failed at the generation commit boundary."); return false; }
			PublishClassRegistration(Candidate);
		}
	bModuleTransactionPrepared = false;
	FScopedSlowTask SlowTask(1.f);
	CompleteReload(SlowTask);
	return true;
}

void FAngelscriptClassGenerator::AbortModuleTransaction()
{
	if (!bModuleTransactionPrepared) return;
	// Candidate receivers are already released by the provider, while their VM
	// types are still valid. Retain the module for remaining CDO/metadata GC owners.
	for (auto& Module : Modules)
		for (auto& Class : Module.Classes)
		{
			UASClass* Candidate = Cast<UASClass>(Class.NewClass->Class);
			if (!Candidate) continue;
			AngelscriptRuntimeGcMirror::DestroyRuntimeTrackedObjectsForClass(Candidate);
			CleanupRemovedClass(Class.NewClass);
		}
	bModuleTransactionPrepared = false;
}
                                                                                                                   

void FAngelscriptClassGenerator::PerformFullReload()
{
	PerformReload(true);
}

void FAngelscriptClassGenerator::PerformSoftReload()
{
                                                                                                                     
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
	if (FAngelscriptManager::Get().bForcedFullReloadExecution)
	{
		PerformReload(true);
		return;
	}
#endif
                                                                                                                   
	PerformReload(false);
}

void FAngelscriptClassGenerator::PerformReload(bool bFullReload)
{
	// Create progress indicator
	FScopedSlowTask SlowTask(1.8f);
	if (bFullReload && FAngelscriptManager::Get().bIsInitialCompileFinished)
		SlowTask.MakeDialogDelayed(0.5f);

	bIsDoingFullReload = bFullReload;
                                                                                                                     
#if WITH_EDITOR
	if (!bPreparingModuleTransaction && bFullReload && FAngelscriptManager::bIsInitialCompileFinished)
	{
		TArray<UObject*> PreviousTypes;
		for (FModuleData& Module : Modules)
			for (FClassData& Class : Module.Classes)
				if (ShouldFullReload(Class) && Class.OldClass.IsValid())
				{
					UObject* Previous = Class.OldClass->bIsStruct
						? static_cast<UObject*>(Class.OldClass->Struct)
						: static_cast<UObject*>(Class.OldClass->Class);
					if (Previous) PreviousTypes.AddUnique(Previous);
				}
		OnPreFullReload.Broadcast(PreviousTypes);
	}
#endif
                                                                                                                   

                                                                                                                       
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
	if (FAngelscriptManager::Get().bForcedFullReloadExecution)
	{
		// Setup validates the exact target module and marks only its actual classes
		// before this method is entered; no reload requirement is promoted here.
		check(FAngelscriptManager::Get().bForcedFullReloadBindingValidated);
	}
#endif
                                                                                                                     
	{
		FAngelscriptScopeTimer Timer(TEXT("class generator reload"));

		// Update progress indicator
		SlowTask.EnterProgressFrame(0.1f);

		// Update progress indicator
		SlowTask.EnterProgressFrame(0.1f);

		// Link or create classes to the new script types
		for (auto& ModuleData : Modules)
		{
			for (auto& ClassData : ModuleData.Classes)
			{
				if (ShouldFullReload(ClassData))
				{
					if (ClassData.NewClass->bIsStruct)
						CreateFullReloadStruct(ModuleData, ClassData);
					else
						CreateFullReloadClass(ModuleData, ClassData);
				}
				else
				{
					LinkSoftReloadClasses(ModuleData, ClassData);
				}
			}

			for (auto& EnumData : ModuleData.Enums)
			{
				if (!ShouldFullReload(EnumData))
				{
					LinkSoftReloadClasses(ModuleData, EnumData);
				}
			}

			for (auto& DelegateData : ModuleData.Delegates)
			{
				if (ShouldFullReload(DelegateData))
				{
					CreateFullReloadDelegate(ModuleData, DelegateData);
				}
				else
				{
					LinkSoftReloadClasses(ModuleData, DelegateData);
				}
			}

			for (auto RemovedClass : ModuleData.RemovedClasses)
			{
				FullReloadRemoveClass(ModuleData, RemovedClass);
			}
		}

		// Update progress indicator
		SlowTask.EnterProgressFrame(0.1f);

		// Reload any enums that have changed
		for (auto& ModuleData : Modules)
		{
			for (auto& EnumData : ModuleData.Enums)
			{
				if (ShouldFullReload(EnumData))
				{
					DoFullReload(ModuleData, EnumData);
				}
			}
		}

		// Update progress indicator
		SlowTask.EnterProgressFrame(0.1f);

		// Structs should reload first
		for (auto& ModuleData : Modules)
		{
			for (auto& ClassData : ModuleData.Classes)
			{
				if (ShouldFullReload(ClassData) && ClassData.NewClass->bIsStruct)
				{
					DoFullReload(ModuleData, ClassData);
				}
			}
		}

		// Update progress indicator
		SlowTask.EnterProgressFrame(0.1f);

		// Reload any delegates that have changed
		for (auto& ModuleData : Modules)
		{
			for (auto& DelegateData : ModuleData.Delegates)
			{
				if (ShouldFullReload(DelegateData))
				{
					DoFullReload(ModuleData, DelegateData);
				}
			}
		}

		// Update progress indicator
		SlowTask.EnterProgressFrame(0.1f);

		// Do preparatory work for soft reloads to happen after the full reloads are done
		for (auto& ModuleData : Modules)
		{
			for (auto& ClassData : ModuleData.Classes)
			{
				if (!ClassData.NewClass->bIsStruct)
				{
					if (!ShouldFullReload(ClassData))
					{
						PrepareSoftReload(ModuleData, ClassData);
					}
				}
			}
		}

		// Reload all full reload classes, now that we have all structs
		for (auto& ModuleData : Modules)
		{
			for (auto& ClassData : ModuleData.Classes)
			{
				if (!ClassData.NewClass->bIsStruct)
				{
					if (ShouldFullReload(ClassData))
					{
						DoFullReload(ModuleData, ClassData);
					}
				}
			}
		}

		// Soft reloads need to happen after all the other reloads and prepare reloads are done
		for (auto& ModuleData : Modules)
		{
			for (auto& ClassData : ModuleData.Classes)
			{
				if (!ClassData.NewClass->bIsStruct)
				{
					if (!ShouldFullReload(ClassData))
					{
						DoSoftReload(ModuleData, ClassData);
					}
				}
			}
		}

		// Update progress indicator
		SlowTask.EnterProgressFrame(0.1f);

		// Finalize all classes after reload
		for (auto& ModuleData : Modules)
		{
			for (auto& ClassData : ModuleData.Classes)
			{
				if (!ClassData.NewClass->bIsStruct)
				{
					if (ShouldFullReload(ClassData))
					{
						FinalizeClass(ModuleData, ClassData);
					}
				}
			}
		}

		// Update progress indicator
		SlowTask.EnterProgressFrame(0.1f);

		// Initialize default objects for all classes after reload
		CallPostInitFunctions();
		InitDefaultObjects();

		// Very last verification step after all default objects are created
		for (auto& ModuleData : Modules)
		{
			for (auto& ClassData : ModuleData.Classes)
			{
				if (!ClassData.NewClass->bIsStruct)
					VerifyClass(ModuleData, ClassData.NewClass);
			}
		}
	}

                                                                                                                        
	if (bPreparingModuleTransaction)
	{
		bModuleTransactionPrepared = true;
		return;
	}
	CompleteReload(SlowTask);
}

void FAngelscriptClassGenerator::CompleteReload(FScopedSlowTask& SlowTask)
{
                                                                                                                      

	if (bIsDoingFullReload)
	{
		// Update progress indicator
		if (bReinstancingAny && FAngelscriptManager::Get().bIsInitialCompileFinished)
			SlowTask.MakeDialog();
		SlowTask.EnterProgressFrame(0.5f, FText::FromString(TEXT("Unreal Hot Reload")));

		FAngelscriptScopeTimer PostTimer(TEXT("post full reload"));

		// Inform about all changed classes and structs
		for (auto& ModuleData : Modules)
		{
			for (auto& ClassData : ModuleData.Classes)
			{
				if (ClassData.NewClass->bIsStruct)
				{
					UScriptStruct* OldStruct = nullptr;
					UScriptStruct* NewStruct = nullptr;

					if (ClassData.OldClass.IsValid())
						OldStruct = (UScriptStruct*)ClassData.OldClass->Struct;
					else
						OldStruct = (UScriptStruct*)ClassData.ReplacedStruct;

					if (ClassData.NewClass.IsValid())
					{
						NewStruct = (UScriptStruct*)ClassData.NewClass->Struct;
						check(ClassData.ReplacedStruct == nullptr || OldStruct == (UScriptStruct*)ClassData.ReplacedStruct);
					}

					if ((OldStruct != nullptr || NewStruct != nullptr) && OldStruct != NewStruct)
						OnStructReload.Broadcast(OldStruct, NewStruct);
				}
				else
				{
					UClass* OldClass = nullptr;
					UClass* NewClass = nullptr;

					if (ClassData.OldClass.IsValid())
						OldClass = ClassData.OldClass->Class;
					else
						OldClass = ClassData.ReplacedClass;

					if (ClassData.NewClass.IsValid())
					{
						NewClass = ClassData.NewClass->Class;
						check(ClassData.ReplacedClass == nullptr || OldClass == ClassData.ReplacedClass);
					}

					if ((OldClass != nullptr || NewClass != nullptr) && OldClass != NewClass)
						OnClassReload.Broadcast(OldClass, NewClass);
				}
			}
		}

		// Call new reinstancing if needed
		OnFullReload.Broadcast();

		// Update progress indicator
		SlowTask.EnterProgressFrame(0.5f, FText::FromString(TEXT("Unreal Editor Refresh")));

		// Classes that are no longer present in script should be marked as such
		TSet<void*> RemovedScriptTypes;
		for (auto& ModuleData : Modules)
		{
			for (auto RemovedClass : ModuleData.RemovedClasses)
			{
				UASClass* Class = (UASClass*)RemovedClass->Class;
				if (Class && Class->ScriptTypePtr != nullptr)
					RemovedScriptTypes.Add(Class->ScriptTypePtr);

				CleanupRemovedClass(RemovedClass);
			}
		}

		// Notify that a reload has just been performed
		{
			FAngelscriptScopeTimer Timer(TEXT("new class propagation"));
			OnPostReload.Broadcast(bIsDoingFullReload);
		}

		// Null out script types on old classes, we're
		// going to be deleting the module soon.
		if (bReinstancingAny)
		{
			for (auto& ModuleData : Modules)
			{
				for (auto& ClassData : ModuleData.Classes)
				{
					if (ClassData.ReplacedClass != nullptr)
					{
                                                                                                                
						// Retire pending runtime mirrors of the replaced class while the old
						// module is still alive; otherwise their stale type cookies crash the
						// next garbage collection after the old module is discarded.
						AngelscriptRuntimeGcMirror::DestroyRuntimeTrackedObjectsForClass(ClassData.ReplacedClass);
						ClassData.ReplacedClass->RetainRetiredScriptTypeForGc();
#if !SOLOAS_LEGACY_TERMINAL_LIFECYCLE
                        if (ClassData.ReplacedClass->IsRooted())
                            ClassData.ReplacedClass->RemoveFromRoot();
                        ClassData.ReplacedClass->ClearFlags(RF_Standalone);
#endif
                                                                                                              
						if (ClassData.ReplacedClass->ScriptTypePtr != nullptr)
							RemovedScriptTypes.Add(ClassData.ReplacedClass->ScriptTypePtr);
						ClassData.ReplacedClass->ScriptTypePtr = nullptr;
						ClassData.ReplacedClass->ConstructFunction = nullptr;
						ClassData.ReplacedClass->DefaultsFunction = nullptr;

						// The old UFunction objects should have their ScriptFunction nulled out,
						// so if something accidentally calls something on an old class the editor won't crash
                                                                                                                                     
#if 0
                                                                                                                                   
						for (auto& Elem : ClassData.ReplacedClass->GetFunctionMap())
						{
							UASFunction* Function = Cast<UASFunction>(Elem.Value);
							if (Function == nullptr)
								continue;

							Function->ScriptFunction = nullptr;
						}
                                                                                                                                  
#endif
						for (TFieldIterator<UFunction> It(
							ClassData.ReplacedClass,
							EFieldIteratorFlags::ExcludeSuper); It; ++It)
						{
							if (UASFunction* Function = Cast<UASFunction>(*It))
								Function->ScriptFunction = nullptr;
						}
                                                                                                                                
					}
					else if (ClassData.ReplacedStruct != nullptr)
					{
						UASStruct* ReplacedStruct = (UASStruct*)ClassData.ReplacedStruct;
						ReplacedStruct->ScriptType = nullptr;
						ReplacedStruct->UpdateScriptType();
					}
				}
			}
		}

		// Delete script types from all classes that were refering to an old one
		if (RemovedScriptTypes.Num() != 0)
		{
			for (UClass* Class : TObjectRange<UClass>())
			{
                                                                                                            
#if 0
                                                                                                          
				if (Class->ScriptTypePtr == nullptr)
					continue;
				if (RemovedScriptTypes.Contains(Class->ScriptTypePtr))
					Class->ScriptTypePtr = nullptr;
                                                                                                        
#endif
				asITypeInfo* ClassScriptType = GetDirectAngelscriptClassScriptType(Class);
				if (ClassScriptType == nullptr)
					continue;
				if (RemovedScriptTypes.Contains(ClassScriptType))
					verify(ClearAngelscriptClassScriptType(Class, ClassScriptType));
                                                                                                      
			}
		}

		// Force a garbage collection step if we reinstanced any classes, so we don't litter with old instances
		if (bReinstancingAny)
			GEngine->ForceGarbageCollection(true);

		// If we've created any dynamic subsystem classes, inform subsystem collections
		if (ReinstancedSubsystems.Num() != 0)
		{
			if (GEngine != nullptr)
			{
				// The engine is initialized so we can activate our subsystems now
				for (UClass* NewSubsystem : ReinstancedSubsystems)
					FSubsystemCollectionBase::ActivateExternalSubsystem(NewSubsystem);
			}
			else
			{
				// This is likely an initial compile, we should wait with activating subsystems until the engine is inited
                                                                                                            
				TArray<TWeakObjectPtr<UClass>> AddedSubsystems;
				for (UClass* Subsystem : ReinstancedSubsystems) AddedSubsystems.Add(Subsystem);
				const TSharedRef<FDelegateHandle> Handle = MakeShared<FDelegateHandle>();
				*Handle = FCoreDelegates::GetOnPostEngineInit().AddLambda([AddedSubsystems = MoveTemp(AddedSubsystems), Handle]()
					{
						for (const TWeakObjectPtr<UClass>& Subsystem : AddedSubsystems)
							if (UClass* Class = Subsystem.Get())
								if (!Class->HasAnyClassFlags(CLASS_NewerVersionExists))
									FSubsystemCollectionBase::ActivateExternalSubsystem(Class);
						FCoreDelegates::GetOnPostEngineInit().Remove(*Handle);
					});
                                                                                                          
			}
		}
	}
	else
	{
		FAngelscriptScopeTimer PostTimer(TEXT("post soft reload"));
		OnPostReload.Broadcast(bIsDoingFullReload);
	}

#if WITH_EDITOR
	// Apply additional compile errors that the code classes want if applicable
	auto& AdditionalCompileChecks = FAngelscriptManager::Get().AdditionalCompileChecks;
	for (auto& ModuleData : Modules)
	{
		for (auto& ClassData : ModuleData.Classes)
		{
			if (ClassData.NewClass->bIsStruct)
				continue;

			UClass* CodeParent = ClassData.NewClass->CodeSuperClass;
			while (CodeParent != nullptr)
			{
				auto* CheckBind = AdditionalCompileChecks.Find(CodeParent);
				if (CheckBind != nullptr && (*CheckBind).IsValid())
					(*CheckBind)->PostReloadAdditionalChecks(bIsDoingFullReload, ModuleData.NewModule, ClassData.NewClass);

				CodeParent = CodeParent->GetSuperClass();
			}
		}
	}
#endif
}

void FAngelscriptClassGenerator::EnsureReloaded(FModuleData& Module, FClassData& Class)
{
	if (Class.bReloaded)
		return;

	if (ShouldFullReload(Class))
		DoFullReload(Module, Class);
	else if (!Class.NewClass->bIsStruct)
		DoSoftReload(Module, Class);
}

void FAngelscriptClassGenerator::EnsureReloaded(UASClass* Class)
{
	for (auto& ModuleData : Modules)
	{
		for (auto& ClassData : ModuleData.Classes)
		{
			if (!ClassData.OldClass.IsValid())
				continue;

			if (ClassData.OldClass->Class == Class)
			{
				EnsureReloaded(ModuleData, ClassData);
				return;
			}
		}
	}
}

void FAngelscriptClassGenerator::EnsureReloaded(int TypeId)
{
	FModuleData* ModuleDataPtr = nullptr;
	FClassData* ClassDataPtr = nullptr;
	FDelegateData* DelegateDataPtr = nullptr;

	asITypeInfo* ScriptType = FAngelscriptManager::Get().Engine->GetTypeInfoById(TypeId);
	if (ScriptType == nullptr)
		return;

	// Also ensure reloads on subtypes
	for (int32 i = 0, SubCount = ScriptType->GetSubTypeCount(); i < SubCount; ++i)
	{
		int32 SubTypeId = ScriptType->GetSubTypeId(i);
		if ((SubTypeId & asTYPEID_OBJHANDLE) == 0)
			EnsureReloaded(SubTypeId);
	}

	// Ensure that the type we're acting on is actually reloaded
	if (GetDataFor(ScriptType, ModuleDataPtr, ClassDataPtr, DelegateDataPtr))
	{
		if (ClassDataPtr != nullptr)
			EnsureReloaded(*ModuleDataPtr, *ClassDataPtr);
	}
}

void FAngelscriptClassGenerator::EnsureClassFinalized(UASClass* Class)
{
	FModuleData* ModuleData = nullptr;
	FClassData* ClassData = nullptr;
	FDelegateData* DelegateData = nullptr;
	if (GetDataFor((asITypeInfo*)Class->ScriptTypePtr, ModuleData, ClassData, DelegateData))
	{
		if (ClassData != nullptr && !ClassData->bFinalized && ShouldFullReload(*ClassData))
			FinalizeClass(*ModuleData, *ClassData);
	}
}

void FAngelscriptClassGenerator::CreateFullReloadClass(FModuleData& ModuleData, FClassData& ClassData)
{
	auto ClassDesc = ClassData.NewClass;
	auto ModuleDesc = ModuleData.NewModule;

	FString ClassName = ClassDesc->ClassName;
	FString UnrealName = GetUnrealName(false, ClassName);
                                                                                                   
	if (ModuleDesc->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf) UnrealName += TEXT("__") + ModuleDesc->ScriptUnit.Id;
                                                                                                 

	// Check if we're replacing a class
	UASClass* ReplacedClass = FindObject<UASClass>(FAngelscriptManager::GetPackage(), *UnrealName);
                                                                                                                      
	if (ReplacedClass && !bPreparingModuleTransaction)
	{
		FString OldClassName = FString::Printf(TEXT("%s_REPLACED_%d"), *ReplacedClass->GetName(), UniqueCounter());
		ReplacedClass->Rename(*OldClassName, nullptr, REN_DontCreateRedirectors);
		ReplacedClass->ClassFlags |= CLASS_NewerVersionExists;
	}
                                                                                                                    

                                                                                                                      
	if (bPreparingModuleTransaction)
		UnrealName += FString::Printf(TEXT("_CANDIDATE_%d"), UniqueCounter());
                                                                                                                    
	UASClass* NewClass = NewObject<UASClass>(
		FAngelscriptManager::GetPackage(),
		UASClass::StaticClass(),
		FName(*UnrealName),
		RF_Public | RF_Standalone | RF_MarkAsRootSet
	);

	asITypeInfo* ScriptType = ClassDesc->ScriptType;
	if (ScriptType != nullptr)
		ScriptType->SetUserData(NewClass);

	NewClass->bIsEditorOnlyClass = ClassDesc->Meta.Contains(NAME_Meta_EditorOnly);

#if WITH_EDITOR
	NewClass->AngelscriptTypename = ClassName;
#endif

	ClassDesc->Class = NewClass;
	ClassData.ReplacedClass = ReplacedClass;
	SetScriptStaticClass(ClassDesc, NewClass);

	// If we're creating a new dynamic subsystem class, mark it
	if (ClassDesc->CodeSuperClass->IsChildOf<UDynamicSubsystem>() || ClassDesc->CodeSuperClass->IsChildOf<UWorldSubsystem>())
	{
		if (ReplacedClass != nullptr)
			FSubsystemCollectionBase::DeactivateExternalSubsystem(ReplacedClass);
		ReinstancedSubsystems.Add(NewClass);
	}
}

void FAngelscriptClassGenerator::FullReloadRemoveClass(FModuleData& ModuleData, TSharedPtr<FAngelscriptClassDesc> RemovedClass)
{
	// If we're removing a new dynamic subsystem class, deactivate it
	if (RemovedClass->Class != nullptr && (RemovedClass->Class->IsChildOf<UDynamicSubsystem>() || RemovedClass->Class->IsChildOf<UWorldSubsystem>()))
		FSubsystemCollectionBase::DeactivateExternalSubsystem(RemovedClass->Class);
                                                                                                               
	extern void RetireRemovedRuntimeClass(UClass* Class);
	RetireRemovedRuntimeClass(RemovedClass->Class);
                                                                                                             
}

void FAngelscriptClassGenerator::CreateFullReloadStruct(FModuleData& ModuleData, FClassData& ClassData)
{
	auto ClassDesc = ClassData.NewClass;
	auto ModuleDesc = ModuleData.NewModule;

	FString ClassName = ClassDesc->ClassName;
	FString UnrealName = GetUnrealName(true, ClassName);
                                                                                                    
	if (ModuleDesc->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf) UnrealName += TEXT("__") + ModuleDesc->ScriptUnit.Id;
                                                                                                  

	// Check if we're replacing a struct
	UASStruct* ReplacedStruct = FindObject<UASStruct>(FAngelscriptManager::GetPackage(), *UnrealName);
	if (ReplacedStruct)
	{
		FString OldClassName = FString::Printf(TEXT("%s_REPLACED_%d"), *ReplacedStruct->GetName(), UniqueCounter());

		// Move the old struct to the transient package, which blueprint structs do to indicate they are old
		ReplacedStruct->Rename(*OldClassName, GetTransientPackage(), REN_DontCreateRedirectors);
	}

	UASStruct* NewStruct = NewObject<UASStruct>(
		FAngelscriptManager::GetPackage(),
		UASStruct::StaticClass(),
		FName(*UnrealName),
		RF_Public | RF_Standalone | RF_MarkAsRootSet
	);

                                                                                                                                  
#if 0
                                                                                                                                
	NewStruct->bIsScriptStruct = true;
                                                                                                                                
#endif
	check(NewStruct->IsA<UASStruct>());
                                                                                                                              
	NewStruct->SetSuperStruct(nullptr);

	if (ReplacedStruct != nullptr)
		NewStruct->Guid = ReplacedStruct->Guid;
	else
		NewStruct->SetGuid(NewStruct->GetFName());

                                                                                                     
	FString DisplayString = ModuleDesc->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf ? GetUnrealName(true, ClassName) : NewStruct->GetName();
                                                                                                   
	DisplayString = FName::NameToDisplayString(DisplayString, false);

#if WITH_EDITOR
	NewStruct->SetMetaData(NAME_DisplayName, *DisplayString);
	NewStruct->AngelscriptTypename = ClassName;

	for (auto& Elem : ClassDesc->Meta)
		NewStruct->SetMetaData(Elem.Key, *Elem.Value);
#endif

	asITypeInfo* ScriptType = ClassDesc->ScriptType;
	ScriptType->SetUserData(NewStruct);
	NewStruct->SetPropertiesSize(ScriptType->GetSize());

	NewStruct->bIsEditorOnlyStruct = ClassDesc->Meta.Contains(NAME_Meta_EditorOnly);

	// Tell the loading system the struct exists
	NotifyRegistrationEvent(TEXT("/Script/Angelscript"), *UnrealName, ENotifyRegistrationType::NRT_Struct,
		ENotifyRegistrationPhase::NRP_Finished, nullptr, false, NewStruct);

	ClassDesc->Struct = NewStruct;
	ClassData.ReplacedStruct = ReplacedStruct;
	SetScriptStaticStruct(ClassDesc, NewStruct);
}

void FAngelscriptClassGenerator::CreateFullReloadDelegate(FModuleData& Module, FDelegateData& Delegate)
{
	auto DelegateDesc = Delegate.NewDelegate;

	FName FunctionName = *(FString::Printf(TEXT("%s"),
			*DelegateDesc->DelegateName
		) + HEADER_GENERATED_DELEGATE_SIGNATURE_SUFFIX);

                                                                                                      
	if (Module.NewModule->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf)
		FunctionName = FName(*(FunctionName.ToString() + TEXT("__") + Module.NewModule->ScriptUnit.Id));
                                                                                                    
	UDelegateFunction* ReplacedFunction = FindObject<UDelegateFunction>(FAngelscriptManager::GetPackage(), *FunctionName.ToString());
	if (ReplacedFunction)
	{
		FString OldFunctionName = FString::Printf(TEXT("%s_REPLACED_%d"), *ReplacedFunction->GetName(), UniqueCounter());
		ReplacedFunction->Rename(*OldFunctionName, nullptr, REN_DontCreateRedirectors);
	}

	UDelegateFunction* Function = NewObject<UDelegateFunction>(
		FAngelscriptManager::GetPackage(),
		UDelegateFunction::StaticClass(),
		FunctionName,
		RF_Public | RF_Standalone | RF_MarkAsRootSet
	);

	DelegateDesc->Function = Function;
	if (DelegateDesc->ScriptType)
		DelegateDesc->ScriptType->SetUserData(Function);

	// Tell the loading system the delegate exists
                                                                                                                                    
#if 0
                                                                                                                                  
	NotifyRegistrationEvent(TEXT("/Script/Angelscript"), *FunctionName.ToString(), ENotifyRegistrationType::NRT_Struct,
		ENotifyRegistrationPhase::NRP_Finished, nullptr, false, Function);
                                                                                                                                     
#endif
	check(Function != nullptr);
                                                                                                                                   
}

void FAngelscriptClassGenerator::DoFullReload(FModuleData& ModuleData, FClassData& ClassData)
{
	auto ClassDesc = ClassData.NewClass;
	auto ModuleDesc = ModuleData.NewModule;

	// Check if we've already performed the reload on this class
	if (ClassData.bReloaded)
		return;
	ClassData.bReloaded = true;

	// Log the reload if needed
	if (ClassData.OldClass.IsValid())
		UE_LOG(Angelscript, Log, TEXT("Full Reload: %s"), *ClassData.NewClass->ClassName);

	// Structs and classes should be handled slightly differently
	if (ClassData.NewClass->bIsStruct)
	{
		DoFullReloadStruct(ModuleData, ClassData);
	}
	else
	{
		DoFullReloadClass(ModuleData, ClassData);
	}

}

int32 FAngelscriptClassGenerator::AddClassProperties(TSharedPtr<FAngelscriptClassDesc> ClassDesc)
{
	auto* ScriptType = (asCObjectType*)ClassDesc->ScriptType;
	if (ScriptType == nullptr)
		return ClassDesc->CodeSuperClass->GetPropertiesSize();

	int32 PropertiesSize = ScriptType->GetSize();
	FArchive ArDummy;

	UStruct* InStruct = ClassDesc->Class ? ClassDesc->Class : ClassDesc->Struct;

	auto MarkUStructContainsReference = [InStruct]() {
		if (UClass* ObjClass = Cast<UClass>(InStruct))
		{
			ObjClass->ClassFlags |= CLASS_HasInstancedReference;
		}
		else if (UScriptStruct* Struct = Cast<UScriptStruct>(InStruct))
		{
			Struct->StructFlags = EStructFlags(Struct->StructFlags | STRUCT_HasInstancedReference);
		}
	};

	auto BubbleUpInstanceReferenceFlags = [MarkUStructContainsReference](FProperty* Property, FProperty* InnerProperty) {
		// If a struct is marked as STRUCT_HasInstancedReference then the struct property must be marked with
		// CPF_ContainsInstancedReference and our owning UStruct (Class or Struct) must be marked appropriately as well
		if (FStructProperty* StructProperty = CastField<FStructProperty>(InnerProperty))
		{
			if (StructProperty->Struct->StructFlags & STRUCT_HasInstancedReference)
			{
				// If the struct was contained within a container, that container must be marked as containing references as well
				if (Property)
				{
					Property->SetPropertyFlags(CPF_ContainsInstancedReference);
				}
				StructProperty->SetPropertyFlags(CPF_ContainsInstancedReference);
				MarkUStructContainsReference();
			}
		}
	};

	auto ApplyInstancedPropertyFlags = [](FProperty* Property, FProperty* InnerProperty) {
		const FName NAME_EditInline(TEXT("EditInline"));

		if (Property)
		{
			Property->SetPropertyFlags(CPF_ContainsInstancedReference);
#if WITH_EDITOR
			Property->SetMetaData(NAME_EditInline, FString("true"));
#endif
		}

		if (InnerProperty)
		{
			InnerProperty->SetPropertyFlags(CPF_InstancedReference | CPF_ExportObject | CPF_PersistentInstance);
#if WITH_EDITOR
			InnerProperty->SetMetaData(NAME_EditInline, FString("true"));
#endif
		}
	};

	// Add any properties from angelscript as FProperty to the class
	for (int32 i = ScriptType->properties.GetLength() - 1; i >= 0; --i)
	{
		asCObjectProperty* ScriptProp = ScriptType->properties[i];
		int PropertyOffset = ScriptProp->byteOffset;

		// Don't create new UProperties for inherited properties, our superclass will have them
		if (ScriptType->IsPropertyInherited(i))
			continue;

		// Make sure the reload for the property type has completed
		if (ScriptProp->type.IsObject() && !ScriptProp->type.IsReferenceType())
			EnsureReloaded(ScriptType->engine->GetTypeIdFromDataType(ScriptProp->type));

		TSharedPtr<FAngelscriptPropertyDesc> PropDesc = ClassDesc->GetProperty(ScriptProp->name);
		if (PropDesc.IsValid())
		{
			// Add the property as an exported UPROPERTY() on the class
			FAngelscriptTypeUsage PropertyType = PropDesc->PropertyType;

			FAngelscriptType::FPropertyParams Params;
			Params.Struct = InStruct;
			Params.Outer = InStruct;
			Params.PropertyName = FName(PropDesc->GetUnrealPropertyName());

			FProperty* NewProperty = PropertyType.CreateProperty(Params);

			PropDesc->bHasUnrealProperty = true;

#if WITH_EDITOR
			for (auto& Elem : PropDesc->Meta)
				NewProperty->SetMetaData(Elem.Key, *Elem.Value);

			if (PropDesc->bIsProtected)
				NewProperty->SetMetaData(FUNCMETA_BlueprintProtected, TEXT("true"));
#endif

                                                                                                                                   
#if 0
                                                                                                                                 
			NewProperty->AngelscriptPropertyFlags |= APF_RuntimeGenerated;
                                                                                                                            
#endif
			check(IsAngelscriptRuntimeGeneratedProperty(NewProperty));
                                                                                                                          

			if (PropDesc->bReplicated)
			{
				NewProperty->SetPropertyFlags(CPF_Net);
				NewProperty->SetBlueprintReplicationCondition(PropDesc->ReplicationCondition);

				if (PropDesc->bRepNotify)
				{
					FString* RepNotifyFunc = PropDesc->Meta.Find(TEXT("ReplicatedUsing"));
					if (RepNotifyFunc != nullptr)
					{
						NewProperty->SetPropertyFlags(CPF_RepNotify);
						NewProperty->RepNotifyFunc = FName(**RepNotifyFunc);
					}
				}

				if (PropDesc->Meta.Contains(TEXT("ReplicationPushModel")))
				{
					UASClass* ASClass = Cast<UASClass>(ClassDesc->Class);
					if (ASClass != nullptr)
					{
						ASClass->PushModelReplicatedProperties.AddUnique(NewProperty->GetFName());
					}
				}
			}
			else
			{
				if (PropDesc->bSkipReplication)
				{
					NewProperty->SetPropertyFlags(CPF_RepSkip);
				}
			}

			if (PropDesc->bSkipSerialization)
			{
				NewProperty->SetPropertyFlags(CPF_SkipSerialization);
			}

			if (PropDesc->bSaveGame)
			{
				NewProperty->SetPropertyFlags(CPF_SaveGame);
			}

			// Read property specifiers from descriptor
			if ((PropDesc->bBlueprintReadable || PropDesc->bBlueprintWritable) && (!PropDesc->bIsPrivate || PropDesc->Meta.Find(NAME_AllowPrivateAccess)))
			{
				NewProperty->SetPropertyFlags(CPF_BlueprintVisible);
				if (!PropDesc->bBlueprintWritable)
					NewProperty->SetPropertyFlags(CPF_BlueprintReadOnly);
			}

			if (!NewProperty->HasAnyPropertyFlags(CPF_BlueprintAssignable))
			{
				if (PropDesc->bEditableOnInstance || PropDesc->bEditableOnDefaults || PropDesc->bEditConst)
				{
					NewProperty->SetPropertyFlags(CPF_Edit);
					if (!PropDesc->bEditableOnInstance)
						NewProperty->SetPropertyFlags(CPF_DisableEditOnInstance);
					if (!PropDesc->bEditableOnDefaults)
						NewProperty->SetPropertyFlags(CPF_DisableEditOnTemplate);
					if (PropDesc->bEditConst)
						NewProperty->SetPropertyFlags(CPF_EditConst);
				}
			}
			else if (PropDesc->Meta.Find(TEXT("BPCannotCallEvent")) || PropDesc->bIsPrivate || PropDesc->bIsProtected)
			{
				NewProperty->ClearPropertyFlags(CPF_BlueprintCallable);
			}

			if (PropDesc->bInstancedReference)
			{
				NewProperty->SetPropertyFlags(CPF_InstancedReference | CPF_ExportObject | CPF_EditConst);
			}

			if (PropDesc->bPersistentInstance)
			{
				MarkUStructContainsReference();
			}

			if (PropDesc->bAdvancedDisplay)
				NewProperty->SetPropertyFlags(CPF_AdvancedDisplay);

			if (PropDesc->bTransient)
				NewProperty->SetPropertyFlags(CPF_Transient);

			if (PropDesc->bConfig)
				NewProperty->SetPropertyFlags(CPF_Config);

			if (PropDesc->bInterp)
				NewProperty->SetPropertyFlags(CPF_Interp);

			if (PropDesc->bAssetRegistrySearchable)
				NewProperty->SetPropertyFlags(CPF_AssetRegistrySearchable);

			if (PropDesc->bNoClear)
				NewProperty->SetPropertyFlags(CPF_NoClear);

			if (PropDesc->Meta.Contains(NAME_ExposeOnSpawn))
				NewProperty->SetPropertyFlags(CPF_ExposeOnSpawn);

			if (PropDesc->Meta.Contains(NAME_EditFixedSize))
				NewProperty->SetPropertyFlags(CPF_EditFixedSize);

			if (PropDesc->Meta.Contains(NAME_Meta_EditorOnly))
				NewProperty->SetPropertyFlags(CPF_EditorOnly);

			// If any containers contain instanced references, make sure to bubble up their instance reference flags
			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(NewProperty))
			{
				BubbleUpInstanceReferenceFlags(ArrayProp, ArrayProp->Inner);

				ArrayProp->Inner->ClearPropertyFlags(CPF_PropagateToArrayInner);
				if(PropDesc->bPersistentInstance)
				{
					ApplyInstancedPropertyFlags(ArrayProp, ArrayProp->Inner);
				}
				ArrayProp->Inner->SetPropertyFlags( ArrayProp->GetPropertyFlags() & CPF_PropagateToArrayInner );
			}
			else if (FMapProperty* MapProp = CastField<FMapProperty>(NewProperty))
			{
				BubbleUpInstanceReferenceFlags(MapProp, MapProp->ValueProp);
				BubbleUpInstanceReferenceFlags(MapProp, MapProp->KeyProp);

				MapProp->ValueProp->ClearPropertyFlags(CPF_PropagateToMapValue);
					
				if(PropDesc->bPersistentInstance)
				{
					ApplyInstancedPropertyFlags(MapProp, MapProp->ValueProp);
				}
				MapProp->ValueProp->SetPropertyFlags( MapProp->GetPropertyFlags() & CPF_PropagateToMapValue );

				MapProp->KeyProp->ClearPropertyFlags(CPF_PropagateToMapKey);
				if(PropDesc->bPersistentInstance)
				{
					ApplyInstancedPropertyFlags(MapProp, MapProp->KeyProp);
				}
				MapProp->KeyProp->SetPropertyFlags( MapProp->GetPropertyFlags() & CPF_PropagateToMapKey );
			}
			else if (FSetProperty* SetProp = CastField<FSetProperty>(NewProperty))
			{
				BubbleUpInstanceReferenceFlags(SetProp, SetProp->ElementProp);

				SetProp->ElementProp->ClearPropertyFlags(CPF_PropagateToSetElement);
				if(PropDesc->bPersistentInstance)
				{
					ApplyInstancedPropertyFlags(SetProp, SetProp->ElementProp);
				}
				SetProp->ElementProp->SetPropertyFlags( SetProp->GetPropertyFlags() & CPF_PropagateToSetElement );
			}
			else if (FStructProperty* StructProperty = CastField<FStructProperty>(NewProperty))
			{
				if(PropDesc->bPersistentInstance)
				{
					ApplyInstancedPropertyFlags(StructProperty, nullptr);
				}
			}
			else
			{
				BubbleUpInstanceReferenceFlags(nullptr, NewProperty);
				if (PropDesc->bPersistentInstance)
				{
					ApplyInstancedPropertyFlags(nullptr, NewProperty);
				}
			}

			// mark as hashable
			if (PropDesc->PropertyType.CanHashValue())
			{
				NewProperty->SetPropertyFlags(CPF_HasGetValueTypeHash);
			}

			// Add property to class children
			NewProperty->Next = InStruct->ChildProperties;
			InStruct->ChildProperties = NewProperty;

			// Link at the right place in the script 
			// We set the properties size so the property links correctly,
			// then override it with the proper one later. Note that none
			// of these properties take up any 'size' in unreal, because
			// they're all included in the ScriptSize of the angelscript object.
			InStruct->SetPropertiesSize(PropertyOffset);
			NewProperty->Link(ArDummy);

			// If this check fails, there is most likely an alignment difference
			// between the CPP type and the angelscript type. Set the 'alignment'
			// member of the appropriate asITypeInfo to get angelscript to align
			// its properties in accordance with unreal alignment rules.
			check(NewProperty->GetOffset_ForUFunction() == PropertyOffset);
		}
	}

	return PropertiesSize;
}

bool FAngelscriptClassGenerator::GetDataFor(int TypeId, FModuleData*& OutModule, FClassData*& OutClass, FDelegateData*& OutDelegate)
{
	asITypeInfo* ScriptType = FAngelscriptManager::Get().Engine->GetTypeInfoById(TypeId);
	if (ScriptType == nullptr)
		return false;
	return GetDataFor(ScriptType, OutModule, OutClass, OutDelegate);
}

bool FAngelscriptClassGenerator::GetDataFor(class asITypeInfo* ScriptType, FModuleData*& OutModule, FClassData*& OutClass, FDelegateData*& OutDelegate)
{
	FDataRef* Ref = DataRefByNewScriptType.Find(ScriptType);

	if (Ref != nullptr)
	{
		FModuleData& ModuleData = Modules[Ref->ModuleIndex];
		if (Ref->bIsClass)
		{
			FClassData& ClassData = ModuleData.Classes[Ref->DataIndex];
			check(ClassData.NewClass->ScriptType == ScriptType);

			OutModule = &ModuleData;
			OutClass = &ClassData;
			OutDelegate = nullptr;

			return true;
		}
		else if (Ref->bIsDelegate)
		{
			FDelegateData& DelegateData = ModuleData.Delegates[Ref->DataIndex];
			check(DelegateData.NewDelegate->ScriptType == ScriptType);

			OutModule = &ModuleData;
			OutClass = nullptr;
			OutDelegate = &DelegateData;

			return true;
		}
	}

	return false;
}

UClass* FAngelscriptClassGenerator::ResolveCodeSuperForProperty(const FAngelscriptTypeUsage& Usage)
{
	UClass* ClassOfProperty = Usage.GetClass();
	if (ClassOfProperty != nullptr)
	{
                                                                                                                  
#if 0
                                                                                                                
		while (ClassOfProperty != nullptr && ClassOfProperty->bIsScriptClass)
                                                                                                             
#endif
		while (ClassOfProperty != nullptr
			&& GetAngelscriptClassScriptType(ClassOfProperty) != nullptr)
                                                                                                           
			ClassOfProperty = ClassOfProperty->GetSuperClass();
		return ClassOfProperty;
	}

	if (Usage.ScriptClass != nullptr)
	{
		FModuleData* ModuleData = nullptr;
		FClassData* ClassData = nullptr;
		FDelegateData* DelegateData = nullptr;
		GetDataFor(Usage.ScriptClass, ModuleData, ClassData, DelegateData);

		if (ClassData != nullptr)
			return ClassData->NewClass->CodeSuperClass;
	}

	return nullptr;
}

void FAngelscriptClassGenerator::DoFullReloadStruct(FModuleData& ModuleData, FClassData& ClassData)
{
	auto ClassDesc = ClassData.NewClass;
	auto ModuleDesc = ModuleData.NewModule;

	FString ClassName = ClassDesc->ClassName;
	auto* ScriptType = ClassDesc->ScriptType;

	UASStruct* NewStruct = (UASStruct*)ClassDesc->Struct;
	UASStruct* ReplacedStruct = (UASStruct*)ClassData.ReplacedStruct;

	// Set up the class' base data
#if WITH_EDITOR
	NewStruct->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
#endif

	NewStruct->PropertyLink = nullptr;
	NewStruct->MinAlignment = ScriptType->alignment;
	NewStruct->Bind();

	// Record data about the class in the descriptor for further pipeline use
	ClassDesc->Struct = NewStruct;
	NewStruct->ScriptType = ScriptType;
	NewStruct->SetPropertiesSize(ScriptType->GetSize());

                                                                                                                     
#if 0
                                                                                                                   
	NewStruct->SetCppStructOps(NewStruct->CreateCppStructOps());
                                                                                                                    
#endif
	NewStruct->SetAngelscriptCppStructOps(NewStruct->CreateCppStructOps());
                                                                                                                  
	NewStruct->PrepareCppStructOps();

	// Add all properties from angelscript as FProperty to the class
	int32 PropertiesSize = AddClassProperties(ClassDesc);
	NewStruct->SetPropertiesSize(PropertiesSize);

	NewStruct->StaticLink();
	NewStruct->DestructorLink = nullptr;

	// Tell the hot-reloader to replace the old class
	if (ReplacedStruct != nullptr)
	{
		bReinstancingAny = true;
		ReplacedStruct->NewerVersion = NewStruct;
	}
}

void FAngelscriptClassGenerator::DoFullReloadClass(FModuleData& ModuleData, FClassData& ClassData)
{
	auto ClassDesc = ClassData.NewClass;
	auto ModuleDesc = ModuleData.NewModule;

	FString ClassName = ClassDesc->ClassName;
	asITypeInfo* ScriptType = ClassDesc->ScriptType;

	UClass* ParentCodeClass = ClassDesc->CodeSuperClass;
	UASClass* ParentASClass = nullptr;

	if (!ClassDesc->bSuperIsCodeClass)
	{
		// If the type's super class is in a module we're currently compiling,
		// force that to do its full reload first.
		asITypeInfo* ScriptSuperType = ScriptType->GetBaseType();
		asIScriptModule* ScriptSuperModule = ScriptSuperType->GetModule();

		bool bFoundInCompilingModules = false;
		for (auto& CheckModuleData : Modules)
		{
			if (CheckModuleData.NewModule->ScriptModule == ScriptSuperModule)
			{
				// Make sure class is actually reloaded
				for (auto& CheckClassData : CheckModuleData.Classes)
				{
					if (CheckClassData.NewClass->ClassName == ClassDesc->SuperClass)
					{
						EnsureReloaded(CheckModuleData, CheckClassData);

						ParentASClass = (UASClass*)CheckClassData.NewClass->Class;
						bFoundInCompilingModules = true;
						break;
					}
				}
				break;
			}
		}

		if (!bFoundInCompilingModules)
		{
			ParentASClass = Cast<UASClass>(FAngelscriptManager::Get().GetClass(ClassDesc->SuperClass)->Class);
			check(ParentASClass);
		}
	}

	UClass* SuperClass = ParentASClass ? ParentASClass : ParentCodeClass;
	UASClass* NewClass = (UASClass*)ClassDesc->Class;
	UASClass* ReplacedClass = ClassData.ReplacedClass;

	// Need to make sure all instances we're going to full reload are fully constructed before we can do anything
	TArray<UObject*> Instances;
	TArray<UObject*> CDOInstances;
	GetObjectsOfClass(ReplacedClass, Instances, true, RF_NoFlags);

	// Set up the class' base data
	NewClass->ClassFlags = CLASS_CompiledFromBlueprint;
                                                                                                                                 
#if 0
                                                                                                                               
	NewClass->bIsScriptClass = true;
                                                                                                                               
#endif
	check(Cast<UASClass>(NewClass) == NewClass);
                                                                                                                             
	NewClass->ClassFlags |= (SuperClass->ClassFlags & CLASS_ScriptInherit);

	if (ClassDesc->ConfigName.Len() != 0)
	{
		NewClass->ClassFlags |= CLASS_Config;
		NewClass->ClassConfigName = FName(*ClassDesc->ConfigName);
	}
	else
	{
		NewClass->ClassConfigName = SuperClass->ClassConfigName;
	}

	if ((NewClass->ClassFlags & CLASS_Config) != 0)
	{
		if (ClassDesc->Meta.Contains(NAME_Class_DefaultConfig))
			NewClass->ClassFlags |= CLASS_DefaultConfig;
	}

	NewClass->PropertyLink = SuperClass->PropertyLink;
	NewClass->SetSuperStruct(SuperClass);
                                                                                                
    {
        FAngelscriptTypeGenerationContext Context;
        Context.Point = EAngelscriptTypeGenerationPoint::ConfigureClass;
        Context.Class = ClassDesc.Get();
        FString Error;
        if (!DispatchAngelscriptTypeGeneration(Context, Error))
            FAngelscriptManager::Get().ScriptCompileError(ModuleDesc, ClassDesc->LineNumber, Error);
    }
                                                                                              


#if WITH_EDITOR
	CopyClassInheritedMetaData(SuperClass, NewClass);

	if (!ClassDesc->bIsStaticsClass)
	{
		if (!SuperClass->HasMetaDataHierarchical(NAME_NoBlueprintsOfChildren))
		{
			NewClass->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
			NewClass->SetMetaData(TEXT("IsBlueprintBase"), TEXT("true"));
		}
		else
		{
			NewClass->SetMetaData(TEXT("IsBlueprintBase"), TEXT("false"));
			NewClass->RemoveMetaData(TEXT("Blueprintable"));
		}
	}

	if (!ClassDesc->Meta.Contains("DisplayName"))
	{
                                                                                                    
		FString DisplayString = ModuleData.NewModule->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf
			? GetUnrealName(false, ClassDesc->ClassName)
			: NewClass->GetName();
                                                                                                  
		DisplayString = FName::NameToDisplayString(DisplayString, false);
		NewClass->SetMetaData(NAME_DisplayName, *DisplayString);
	}

	for (auto& Elem : ClassDesc->Meta)
		NewClass->SetMetaData(Elem.Key, *Elem.Value);

	if (ClassDesc->Meta.Contains(TEXT("NotBlueprintable")))
	{
		NewClass->SetMetaData(TEXT("IsBlueprintBase"), TEXT("false"));
		NewClass->RemoveMetaData(TEXT("Blueprintable"));
	}
	else if (ClassDesc->Meta.Contains(TEXT("Blueprintable")))
	{
		NewClass->SetMetaData(TEXT("IsBlueprintBase"), TEXT("true"));
		NewClass->RemoveMetaData(TEXT("NotBlueprintable"));
	}

	// Don't inherit BlueprintThreadSafe ever
	if (!ClassDesc->Meta.Contains(FUNCMETA_BlueprintThreadSafe))
		NewClass->RemoveMetaData(FUNCMETA_BlueprintThreadSafe);
#endif

	NewClass->ClassWithin = UObject::StaticClass();
	NewClass->Bind();

	if (ClassDesc->bPlaceable.IsSet())
	{
		if(ClassDesc->bPlaceable.GetValue())
			NewClass->ClassFlags &= ~CLASS_NotPlaceable; 
		else
			NewClass->ClassFlags |= CLASS_NotPlaceable; 
	}

	if (ClassDesc->bAbstract)
		NewClass->ClassFlags |= CLASS_Abstract;

	if (ClassDesc->bTransient)
		NewClass->ClassFlags |= CLASS_Transient;

	if (ClassDesc->bHideDropdown)
		NewClass->ClassFlags |= CLASS_HideDropDown;

	if (ClassDesc->bDefaultToInstanced)
		NewClass->ClassFlags |= CLASS_DefaultToInstanced;

	if (ClassDesc->bEditInlineNew)
		NewClass->ClassFlags |= CLASS_EditInlineNew;

	if (ClassDesc->bIsDeprecatedClass)
		NewClass->ClassFlags |= CLASS_Deprecated;

#if WITH_EDITOR
	FString HideCategories = NewClass->GetMetaData(TEXT("HideCategories"));
	if (!HideCategories.Contains(TEXT(" DefaultComponents")) && SuperClass->IsChildOf(AActor::StaticClass()))
		NewClass->SetMetaData(TEXT("HideCategories"), *(HideCategories + TEXT(" DefaultComponents")));
#endif

	NewClass->bHasASClassParent = ParentASClass != nullptr;
                                                                                                                        
	if (!NewClass->HasValidScriptClassCarrier())
	{
		FAngelscriptManager::Get().ScriptCompileError(
			ModuleData.NewModule, ClassDesc->LineNumber,
			TEXT("ASR-CLASS-CARRIER-INVALID: script classes require a direct-born carrier "
				"and native or script ancestry."));
		ModuleData.NewModule->bModuleSwapInError = true;
		return;
	}
	FString UnsupportedReplicatedProperty;
	if (!RejectUnsupportedDynamicReplication(*ClassDesc, NewClass, UnsupportedReplicatedProperty))
	{
		FAngelscriptManager::Get().ScriptCompileError(
			ModuleData.NewModule,
			ClassDesc->LineNumber,
			FString::Printf(
				TEXT(
					"ASR-P6-UNSUPPORTED-DYNAMIC-REPLICATION: dynamic AngelScript "
					"Actor/Component class %s cannot declare replicated property %s "
					"on stock Unreal Engine."),
				*ClassDesc->ClassName,
				*UnsupportedReplicatedProperty));
		ModuleData.NewModule->bModuleSwapInError = true;
		return;
	}
                                                                                                                      

	// Add all properties from angelscript as FProperty to the class
	int32 PropertiesSize = AddClassProperties(ClassDesc);
                                                                                                                     
#if 0
                                                                                                                   
	check(PropertiesSize >= SuperClass->GetContainerSize());
                                                                                                                  
#endif
	PropertiesSize = FMath::Max(
		PropertiesSize, SuperClass->GetPropertiesSize());
	check(PropertiesSize >= SuperClass->GetPropertiesSize());
                                                                                                                

	// Get the script type's alignment if available, otherwise default to super alignment
	int32 MinAlignment = (ScriptType ? ScriptType->alignment : SuperClass->MinAlignment);
	check(MinAlignment >= SuperClass->MinAlignment);

	TArray<UASFunction*> FunctionsWithValidate;

	// Add any functions from angelscript as UFunction to the class
	for (auto FunctionDesc : ClassDesc->Methods)
	{
		FName FunctionName(*FunctionDesc->FunctionName);
		UFunction* ParentFunction = SuperClass->FindFunctionByName(FunctionName);

		asCScriptFunction* ScriptFunction = (asCScriptFunction*)FunctionDesc->ScriptFunction;
		ScriptFunction->isInUse = true;

		// Initialize UFunction object
		auto* NewFunction = UASFunction::AllocateFunctionFor(NewClass, FunctionName, FunctionDesc);
		NewFunction->SetSuperStruct(ParentFunction);
		NewFunction->FirstPropertyToInit = NULL;
                                                                                                                          
#if 0
                                                                                                                        
		NewFunction->FunctionFlags |= FUNC_RuntimeGenerated;
                                                                                                                         
#endif
		NewFunction->FunctionFlags |= FUNC_Native;
		NewFunction->SetNativeFunc(&UASFunction::execAngelscriptRuntimeThunk);
                                                                                                                       
		NewFunction->ScriptFunction = (asCScriptFunction*)FunctionDesc->ScriptFunction;
		NewFunction->bIsNoOp = FunctionDesc->bIsNoOp;

		if (ScriptFunction->traits.GetTrait(asTRAIT_FINAL))
		{
			NewFunction->JitFunction = ScriptFunction->jitFunction;
			NewFunction->JitFunction_Raw = ScriptFunction->jitFunction_Raw;
			NewFunction->JitFunction_ParmsEntry = ScriptFunction->jitFunction_ParmsEntry;
		}

#if WITH_EDITOR
		for (auto& Elem : FunctionDesc->Meta)
			NewFunction->SetMetaData(Elem.Key, *Elem.Value);

		// Record which argument was the mixin argument
		if (ScriptFunction->traits.GetTrait(asTRAIT_MIXIN)
			&& ScriptFunction->parameterNames.GetLength() >= 1)
		{
			FString MixinArgumentName =  ANSI_TO_TCHAR(ScriptFunction->parameterNames[0].AddressOf());
			NewFunction->SetMetaData(NAME_Function_MixinArgument, *MixinArgumentName);
			NewFunction->SetMetaData(NAME_Function_DefaultToSelf, *MixinArgumentName);
		}

		if (NewFunction->bIsNoOp)
			NewFunction->SetMetaData(FUNCMETA_ScriptNoOp, TEXT("true"));
#endif

		FunctionDesc->Function = NewFunction;

#if WITH_EDITOR
		if (FunctionDesc->bIsProtected && FunctionDesc->bBlueprintCallable)
			NewFunction->SetMetaData(FUNCMETA_BlueprintProtected, TEXT("true"));
#endif

		if (FunctionDesc->bBlueprintCallable && !FunctionDesc->bIsPrivate)
			NewFunction->FunctionFlags |= FUNC_BlueprintCallable;
		if ((FunctionDesc->bBlueprintEvent && FunctionDesc->bCanOverrideEvent) || FunctionDesc->bBlueprintOverride)
			NewFunction->FunctionFlags |= FUNC_BlueprintEvent;
                                                                                                                
		if (((FunctionDesc->bBlueprintEvent && FunctionDesc->bCanOverrideEvent) || FunctionDesc->bBlueprintOverride)
			&& !FunctionDesc->bIsPrivate)
			NewFunction->FunctionFlags |= FUNC_BlueprintCallable;
                                                                                                              
		if (FunctionDesc->bBlueprintPure && !FunctionDesc->bIsPrivate)
			NewFunction->FunctionFlags |= FUNC_BlueprintPure;
		if (FunctionDesc->bIsStatic)
			NewFunction->FunctionFlags |= FUNC_Static;
		if (FunctionDesc->bNetMulticast)
			NewFunction->FunctionFlags |= FUNC_NetMulticast;
		if (FunctionDesc->bNetClient)
			NewFunction->FunctionFlags |= FUNC_NetClient;
		if (FunctionDesc->bNetServer)
			NewFunction->FunctionFlags |= FUNC_NetServer;
		if (FunctionDesc->bNetValidate)
		{
			NewFunction->FunctionFlags |= FUNC_NetValidate;
			FunctionsWithValidate.Add(NewFunction);
		}
		if (FunctionDesc->bBlueprintAuthorityOnly)
			NewFunction->FunctionFlags |= FUNC_BlueprintAuthorityOnly;
		if (FunctionDesc->bExec)
			NewFunction->FunctionFlags |= FUNC_Exec;
		if ((NewFunction->FunctionFlags & FUNC_NetFuncFlags) != 0)
		{
			NewFunction->FunctionFlags |= FUNC_Net;
			if (!FunctionDesc->bUnreliable)
				NewFunction->FunctionFlags |= FUNC_NetReliable;
		}
#if WITH_ANGELSCRIPT_HAZE
		if (FunctionDesc->bNetFunction)
		{
			NewFunction->FunctionFlags |= FUNC_NetFunction;
			if (!FunctionDesc->bUnreliable)
				NewFunction->FunctionFlags |= FUNC_NetReliable;
			if (FunctionDesc->Meta.Contains(FUNCMETA_CrumbFunction))
				NewFunction->HazeFunctionFlags = (EHazeFunctionFlags)((uint32)NewFunction->HazeFunctionFlags | (uint32)HAZEFUNC_CrumbFunction);
		}
		if (FunctionDesc->bDevFunction)
			NewFunction->FunctionFlags |= FUNC_DevFunction;
#endif
		if (FunctionDesc->bIsConstMethod)
			NewFunction->FunctionFlags |= FUNC_Const;
		if (FunctionDesc->Meta.Contains(NAME_Meta_EditorOnly))
			NewFunction->FunctionFlags |= FUNC_EditorOnly;

		if (ParentFunction)
		{
			// Copy some flags we need from the parent
			NewFunction->FunctionFlags |= (ParentFunction->FunctionFlags & (FUNC_FuncInherit | FUNC_Public | FUNC_Protected | FUNC_Private | FUNC_BlueprintPure | FUNC_HasOutParms));
#if WITH_EDITOR
			// Copy the source values before adding metadata for NewFunction. Creating the
			// destination entry can reallocate the package map and invalidate a same-package
			// source-map pointer held by FMetaData::CopyMetadata.
			                                                                                                             
			TMap<FName, FString> ParentMetadataSnapshot;
			if (const TMap<FName, FString>* ParentMetadata = FMetaData::GetMapForObject(ParentFunction))
				ParentMetadataSnapshot = *ParentMetadata;

			for (const TPair<FName, FString>& MetadataEntry : ParentMetadataSnapshot)
				NewFunction->SetMetaData(MetadataEntry.Key, *MetadataEntry.Value);
			                                                                                                           

			if (!NewFunction->bIsNoOp)
				NewFunction->RemoveMetaData(FUNCMETA_ScriptNoOp);
#endif
		}

		AddFunctionReturnType(NewFunction, FunctionDesc->ReturnType);

		FProperty* WorldContextProperty = nullptr;

		// Generate a hidden world context argument for all static functions by default
		if (FunctionDesc->bIsStatic)
		{
			FString* WorldContextParam = FunctionDesc->Meta.Find(NAME_Arg_WorldContext);

			int32 ParamIndex = -1;
			if (WorldContextParam != nullptr && WorldContextParam->Len() != 0)
			{
				for (int32 ArgIndex = 0, ArgCount = FunctionDesc->Arguments.Num(); ArgIndex < ArgCount; ++ArgIndex)
				{
					if (FunctionDesc->Arguments[ArgIndex].ArgumentName == *WorldContextParam)
					{
						ParamIndex = ArgIndex;
						break;
					}
				}
			}

			if (ParamIndex == -1 && FunctionDesc->Meta.Contains(NAME_Arg_NoWorldContext))
			{
				NewFunction->WorldContextIndex = -1;
				NewFunction->bIsWorldContextGenerated = false;
				NewFunction->bHasWorldContext = false;
			}
			else if (ParamIndex == -1)
			{
				// No existing world context, generate a fake one
				FAngelscriptArgumentDesc ArgDesc;
				ArgDesc.ArgumentName = TEXT("_World_Context");
				ArgDesc.Type.Type = FAngelscriptType::GetByClass(UObject::StaticClass());

				FProperty* Prop = AddFunctionArgument(NewFunction, ArgDesc, false);
                                                                                                                                  
#if 0
                                                                                                                                
				Prop->AngelscriptPropertyFlags |= APF_WorldContext;
                                                                                                                                   
#endif
				check(IsAngelscriptRuntimeGeneratedProperty(Prop));
                                                                                                                                 
				WorldContextProperty = Prop;

#if WITH_EDITOR
				NewFunction->SetMetaData(NAME_Arg_WorldContext, *Prop->GetName());
#endif

				NewFunction->WorldContextIndex = FunctionDesc->Arguments.Num();
				NewFunction->bIsWorldContextGenerated = true;
				NewFunction->bHasWorldContext = true;
			}
			else
			{
				NewFunction->WorldContextIndex = ParamIndex;
				NewFunction->bIsWorldContextGenerated = false;
				NewFunction->bHasWorldContext = true;
			}

			if (FunctionDesc->Meta.Contains(NAME_Arg_CallableWithoutWorldContext) || FunctionDesc->Meta.Contains(NAME_Arg_OptionalWorldContext))
				NewFunction->bIsWorldContextOptional = true;
		}

		// Add properties to the function for all arguments
		TArray<FProperty*> ArgumentProperties;
		for (auto& ArgDesc : FunctionDesc->Arguments)
		{
			FProperty* Prop = AddFunctionArgument(NewFunction, ArgDesc);
			ArgumentProperties.Add(Prop);

			if (Prop->HasAnyPropertyFlags(CPF_OutParm))
				NewFunction->FunctionFlags |= FUNC_HasOutParms;
		}

		if (WorldContextProperty != nullptr)
			ArgumentProperties.Add(WorldContextProperty);

		// Link arguments in the correct order
		for (int32 i = ArgumentProperties.Num() - 1; i >= 0; --i)
		{
			auto* NewProperty = ArgumentProperties[i];

			// If we were doing a world context, flag it
			if (i == NewFunction->WorldContextIndex)
                                                                                                                                 
#if 0
                                                                                                                               
				NewProperty->AngelscriptPropertyFlags |= APF_WorldContext;
                                                                                                                                  
#endif
				check(IsAngelscriptRuntimeGeneratedProperty(NewProperty));
                                                                                                                                

			NewProperty->Next = NewFunction->ChildProperties;
			NewFunction->ChildProperties = NewProperty;
		}

#if WITH_EDITOR
		// Set advanced display flag on any properties marked that way
		const FString& AdvancedMeta = NewFunction->GetMetaData(NAME_Arg_AdvancedDisplay);
		if (AdvancedMeta.Len() != 0)
		{
			TArray<FString> AdvancedParams;
			AdvancedMeta.ParseIntoArrayWS(AdvancedParams, TEXT(","), true);

			for (FString& AdvParam : AdvancedParams)
			{
				AdvParam.TrimStartAndEndInline();

				for (FProperty* ArgProperty : ArgumentProperties)
				{
					if (ArgProperty->GetName() == AdvParam)
					{
						ArgProperty->PropertyFlags |= CPF_AdvancedDisplay;
						break;
					}
				}
			}
		}
#endif

		// Set required parameter display flag on any properties marked that way
		const FString* RequiredParams = FunctionDesc->Meta.Find(NAME_Arg_RequiredParameters);
		if (RequiredParams != nullptr && !RequiredParams->IsEmpty())
		{
			TArray<FString> ParamNames;
			RequiredParams->ParseIntoArrayWS(ParamNames, TEXT(","), true);

			for (FString& ParamName : ParamNames)
			{
				ParamName.TrimStartAndEndInline();

				for (FProperty* ArgProperty : ArgumentProperties)
				{
					if (ArgProperty->GetName() == ParamName)
					{
						ArgProperty->PropertyFlags |= CPF_RequiredParm;
						break;
					}
				}
			}
		}

		// Link into class
		NewFunction->Next = NewClass->Children;
		NewClass->Children = NewFunction;

		// Add the function to it's owner class function name -> function map
		NewClass->AddFunctionToFunctionMap(NewFunction, NewFunction->GetFName());

		// Link function
		NewFunction->StaticLink(true);
		NewFunction->FinalizeArguments();

		for (int32 i = ArgumentProperties.Num() - 1; i >= 0; --i)
		{
			auto* Prop = ArgumentProperties[i];
                                                                                                                               
#if 0
                                                                                                                             
			if (Prop->AngelscriptPropertyFlags & APF_WorldContext)
                                                                                                                                
#endif
			if (i == NewFunction->WorldContextIndex)
                                                                                                                              
			{
				NewFunction->WorldContextOffsetInParms = Prop->GetOffset_ForUFunction();
				break;
			}
		}
	}

	// Cache _Validate functions for all functions requiring validation
	for (auto* Function : FunctionsWithValidate)
	{
		Function->ValidateFunction = NewClass->FindFunctionByName(FName(*(Function->GetName() + TEXT("_Validate"))));
	}

                                                                                                                                
#if 0
                                                                                                                              
	NewClass->ContainerSize = PropertiesSize;
                                                                                                                             
#endif
	const int32 ScriptPropertiesSize = PropertiesSize;
                                                                                                                           

#if AS_CAN_HOTRELOAD && (WITH_EDITOR || (!UE_BUILD_TEST && !UE_BUILD_SHIPPING))
	// Add some slack for new properties later
	PropertiesSize += 128;
#endif

                                                                                                                
	const int32 ContainerSize = PropertiesSize;
	check(ContainerSize >= ScriptPropertiesSize);
                                                                                                              
                                                                                                                              
#if 0
                                                                                                                            
	NewClass->SetPropertiesSize(PropertiesSize);
                                                                                                                                
#endif
	NewClass->SetPropertiesSize(ContainerSize);
	check(ContainerSize >= NewClass->GetPropertiesSize());
                                                                                                                              
                                                                                                     
	ValidateAngelscriptFieldDestruction(NewClass, ScriptType);
                                                                                                   
	NewClass->StaticLink(false);
                                                                                                        
	VerifyAngelscriptFieldDestructorLinks(NewClass);
                                                                                                      
	NewClass->AssembleReferenceTokenStream(true);

	NewClass->MinAlignment = MinAlignment;
	NewClass->ScriptPropertyOffset = ParentCodeClass->GetPropertiesSize();
                                                                                                                 
	check(NewClass->ScriptPropertyOffset <= ContainerSize);
	extern void PrepareRuntimeScriptTypeIdentity(UASClass*, asITypeInfo*);
	NewClass->bIsModuleStaticCarrier = ClassDesc->bIsStaticsClass;
	// Module statics carry reflected global functions, not an AS instance type.
	if (!ClassDesc->bIsStaticsClass)
		PrepareRuntimeScriptTypeIdentity(NewClass, ScriptType);
                                                                                                               
	NewClass->ScriptTypePtr = ScriptType;
	NewClass->CodeSuperClass = ParentCodeClass;

	// Record data about the class in the descriptor for further pipeline use
	ClassDesc->ScriptType = ScriptType;
	ClassDesc->Class = NewClass;

	UpdateConstructAndDefaultsFunctions(ClassDesc, NewClass);

	// Some properties should be refcounted but are not UProperties, detect them now
	DetectAngelscriptReferences(ClassDesc);
                                                                                                           
	extern void RebuildRuntimeTerminalClassChain(UASClass*);
	RebuildRuntimeTerminalClassChain(NewClass);
                                                                                                         

	// Tell the hot-reloader to replace the old class
	if (ReplacedClass != nullptr)
	{
		bReinstancingAny = true;
                                                                                                                   
		if (!bPreparingModuleTransaction)
			ReplacedClass->NewerVersion = NewClass;
                                                                                                                 
	}
	else
	{
		AddedClasses.Add(NewClass);
	}
}

#if WITH_EDITOR
static const TArray<FName> InheritedCategoryKeywords = {
	"ShowCategories",
	"AutoExpandCategories",
	"AutoCollapseCategories",
	"PrioritizeCategories",
	"HideCategories"
};
static const TArray<FName> InheritedMetaData = {
	"HideFunctions",
	"SparseClassDataTypes"
};
static const FName NAME_IgnoreCategoryKeywords("IgnoreCategoryKeywordsInSubclasses");
void FAngelscriptClassGenerator::CopyClassInheritedMetaData(UClass* SuperClass, UClass* NewClass)
{
	auto* SuperMeta = FMetaData::GetMapForObject(SuperClass);
	if (SuperMeta != nullptr)
	{
		// Need to copy, because calling SetMetaData could invalidate the SuperMeta pointer,
		// because it adds a new entry into the metadata map for the new class object.
		TArray<TPair<FName, FString>, TInlineAllocator<8>> CopiedMetaData;

		if (!SuperClass->HasMetaData(NAME_IgnoreCategoryKeywords))
		{
			for (FName MetaName : InheritedCategoryKeywords)
			{
				FString* MetaValue = SuperMeta->Find(MetaName);
				if (MetaValue != nullptr)
					CopiedMetaData.Add(TPair<FName, FString>{MetaName, *MetaValue});
			}
		}

		for (FName MetaName : InheritedMetaData)
		{
			FString* MetaValue = SuperMeta->Find(MetaName);
			if (MetaValue != nullptr)
				CopiedMetaData.Add(TPair<FName, FString>{MetaName, *MetaValue});
		}

		for (auto& MetaPair : CopiedMetaData)
			NewClass->SetMetaData(MetaPair.Key, *MetaPair.Value);
	}
}
#endif

void FAngelscriptClassGenerator::DoFullReload(FModuleData& ModuleData, FEnumData& EnumData)
{
                                                                                                                     
    const auto* EnumMutationScope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
    AngelscriptReloadSafety::NotifyPreparedScriptEnumReload({EnumMutationScope ? EnumMutationScope->Epoch : 0,
        ModuleData.OldModule.Get(), ModuleData.NewModule.Get(),
        EnumData.OldEnum.Get(), EnumData.NewEnum.Get(), EnumData.OldEnum.IsValid() ? EnumData.OldEnum->Enum : nullptr, true,
        AngelscriptReloadSafety::EPreparedScriptEnumReloadPhase::Begin});
                                                                                                                   
	// Log the reload if needed
	if (EnumData.OldEnum.IsValid())
		UE_LOG(Angelscript, Log, TEXT("Full Reload: %s"), *EnumData.NewEnum->EnumName);

	auto EnumDesc = EnumData.NewEnum;
                                                                                                  
	FString UnrealName = EnumDesc->EnumName;
	if (ModuleData.NewModule->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf) UnrealName += TEXT("__") + ModuleData.NewModule->ScriptUnit.Id;
                                                                                                

	UUserDefinedEnum* Enum = nullptr;
	TArray<TPair<FName, int64>> OldNames;

	bool bExistingEnum = EnumData.OldEnum.IsValid() && EnumData.OldEnum->Enum != nullptr;
	bool bHasChanged = true;
	if (bExistingEnum)
	{
		Enum = (UUserDefinedEnum*)EnumData.OldEnum->Enum;

		const int32 EnumeratorsToCopy = Enum->NumEnums() - 1;
		for (int32 Index = 0; Index < EnumeratorsToCopy; Index++)
		{
			FName Name = Enum->GetNameByIndex(Index);
			int64 Value = Enum->GetValueByIndex(Index);
			OldNames.Emplace(Name, Value);

			if (Index >= EnumDesc->EnumValues.Num()
				|| EnumDesc->EnumValues[Index] != Value
				|| EnumDesc->ValueNames[Index] != Name)
			{
				bHasChanged = true;
			}
		}

		if (EnumDesc->EnumValues.Num() != EnumeratorsToCopy)
			bHasChanged = true;

#if WITH_EDITOR
		// Remove old metadata
		for (auto& MetaElement : EnumData.OldEnum->Meta)
		{
			if (!EnumData.NewEnum->Meta.Contains(MetaElement.Key))
			{
				if (MetaElement.Key.Value < Enum->NumEnums())
					Enum->RemoveMetaData(*MetaElement.Key.Key.ToString(), MetaElement.Key.Value);
			}
		}
#endif
	}
	else
	{
                                                                                              
		Enum = NewObject<UASEnum>(
			FAngelscriptManager::GetPackage(),
			UASEnum::StaticClass(),
			FName(*UnrealName),
			RF_Public | RF_Standalone | RF_MarkAsRootSet
		);
                                                                                            

		TArray<TPair<FName, int64>> EmptyNames;
		Enum->SetEnums(
			EmptyNames,
			UEnum::ECppForm::EnumClass,
			UEnum::EUnderlyingType::int8,
			EEnumFlags::None,
			UEnum::EAddMaxKeyIfMissing::Yes
		);

#if WITH_EDITOR
		Enum->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
#endif

		// Tell the loading system the enum exists
                                                                                                              
		NotifyRegistrationEvent(TEXT("/Script/Angelscript"), *UnrealName, ENotifyRegistrationType::NRT_Enum,
			ENotifyRegistrationPhase::NRP_Finished, nullptr, false, Enum);
                                                                                                            
	}

	if (bHasChanged)
	{
		EnumDesc->Enum = Enum;

		TArray<TPair<FName, int64>> Values;
		for (int32 i = 0, Count = EnumDesc->ValueNames.Num(); i < Count; ++i)
		{
			const FString FullNameStr = Enum->GenerateFullEnumName(*EnumDesc->ValueNames[i].ToString());
			Values.Emplace(*FullNameStr, EnumDesc->EnumValues[i]);
		}

		Enum->SetEnums(
			Values,
			UEnum::ECppForm::EnumClass,
			UEnum::EUnderlyingType::int8,
			EEnumFlags::None,
			UEnum::EAddMaxKeyIfMissing::Yes
		);

		for (int32 i = 0, Count = Values.Num(); i < Count; ++i)
		{
			FText DisplayName;

#if WITH_EDITOR
			auto* DisplayNameMeta = EnumDesc->Meta.Find(TPair<FName,int32>(NAME_DisplayName, i));
			if (DisplayNameMeta != nullptr)
				DisplayName = FText::FromString(*DisplayNameMeta);
			else
#endif
				DisplayName = FText::FromName(Values[i].Key);

			Enum->DisplayNameMap.Add(Values[i].Key, DisplayName);
		}
	}

#if WITH_EDITOR
	// Add specified metadata
	for (auto& MetaElement : EnumDesc->Meta)
	{
		if (MetaElement.Key.Value < Enum->NumEnums())
			Enum->SetMetaData(*MetaElement.Key.Key.ToString(), *MetaElement.Value, MetaElement.Key.Value);
	}
#endif

#if WITH_EDITOR
	if (EnumDesc->Documentation.Len() != 0)
	{
		// Add documentation about the enum itself
		Enum->SetMetaData(TEXT("ToolTip"), *EnumDesc->Documentation);
	}
#endif

	// Set the enum on the script type
	EnumDesc->ScriptType->SetUserData(Enum);

	// Need to inform editor if we changed an existing enum
	if (!bExistingEnum)
	{
		if (FAngelscriptManager::bIsInitialCompileFinished)
			OnEnumCreated.Broadcast(Enum);
	}
	else if (bHasChanged)
	{
		OnEnumChanged.Broadcast(Enum, OldNames);
	}
}

void FAngelscriptClassGenerator::LinkSoftReloadClasses(FModuleData& Module, FEnumData& Enum)
{
	if (!Enum.OldEnum.IsValid())
		return;
                                                                                                                     
    const auto* EnumClaimScope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
    AngelscriptReloadSafety::NotifyPreparedScriptEnumReload({EnumClaimScope ? EnumClaimScope->Epoch : 0,
        Module.OldModule.Get(), Module.NewModule.Get(),
        Enum.OldEnum.Get(), Enum.NewEnum.Get(), Enum.OldEnum->Enum, false,
        AngelscriptReloadSafety::EPreparedScriptEnumReloadPhase::Begin});
                                                                                                                   
	Enum.NewEnum->Enum = Enum.OldEnum->Enum;
	Enum.NewEnum->ScriptType->SetUserData(Enum.NewEnum->Enum);
	Enum.NewEnum->bPendingFullReload = Enum.bNeedReload;
                                                                                                                      
    const auto* EnumCommitScope = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
    AngelscriptReloadSafety::NotifyPreparedScriptEnumReload({EnumCommitScope ? EnumCommitScope->Epoch : 0,
        Module.OldModule.Get(), Module.NewModule.Get(),
        Enum.OldEnum.Get(), Enum.NewEnum.Get(), Enum.OldEnum->Enum, false,
        AngelscriptReloadSafety::EPreparedScriptEnumReloadPhase::Commit});
                                                                                                                    
}

void FAngelscriptClassGenerator::DoFullReload(FModuleData& Module, FDelegateData& DelegateData)
{
	auto FunctionDesc = DelegateData.NewDelegate->Signature;
	UDelegateFunction* NewFunction = DelegateData.NewDelegate->Function;
	NewFunction->FirstPropertyToInit = NULL;
                                                                                                                         
#if 0
                                                                                                                       
	NewFunction->FunctionFlags |= FUNC_RuntimeGenerated;
                                                                                                                       
#endif
	check(!NewFunction->HasAnyFunctionFlags(FUNC_Native));
                                                                                                                     

	NewFunction->FunctionFlags |= FUNC_Delegate;
	if (DelegateData.NewDelegate->bIsMulticast)
		NewFunction->FunctionFlags |= FUNC_MulticastDelegate;
                                                                                                                        
	// The generated provenance identity is stable once the object, outer, and
	// delegate flags exist. Loading-system exposure remains deferred until the
	// complete signature has been linked.
	if (!RegisterAngelscriptRuntimeGeneratedFunction(NewFunction))
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT(
				"ASR-P6-DELEGATE-TRAIT-REGISTRATION-FAILED: Runtime-created "
				"delegate provenance could not be registered."
			));
	}
                                                                                                                      

	auto* ReturnProp = AddFunctionReturnType(NewFunction, FunctionDesc->ReturnType);

	// Add properties to the function for all arguments
	TArray<FProperty*> ArgumentProperties;
	for (auto& ArgDesc : FunctionDesc->Arguments)
	{
		FProperty* Prop = AddFunctionArgument(NewFunction, ArgDesc);
		ArgumentProperties.Add(Prop);
	}

	// Link arguments in the correct order
	for (int32 i = ArgumentProperties.Num() - 1; i >= 0; --i)
	{
		auto* NewProperty = ArgumentProperties[i];
		NewProperty->Next = NewFunction->ChildProperties;
		NewFunction->ChildProperties = NewProperty;
	}

	// Link function
	NewFunction->StaticLink(true);

                                                                                                                                
	NotifyRegistrationEvent(
		TEXT("/Script/Angelscript"),
		*NewFunction->GetName(),
		ENotifyRegistrationType::NRT_Struct,
		ENotifyRegistrationPhase::NRP_Finished,
		nullptr,
		false,
		NewFunction);
                                                                                                                              
	// Tell unreal about the change
	if (DelegateData.OldDelegate.IsValid() && DelegateData.OldDelegate->Function != nullptr)
		OnDelegateReload.Broadcast(DelegateData.OldDelegate->Function, NewFunction);
}

FProperty* FAngelscriptClassGenerator::AddFunctionReturnType(UFunction* NewFunction, const FAngelscriptTypeUsage& ReturnType)
{
	auto* ASFunction = Cast<UASFunction>(NewFunction);

	// Add return property to the function if it doesn't return void
	if (ReturnType.IsValid())
	{
		FAngelscriptType::FPropertyParams Params;
		Params.Struct = nullptr;
		Params.Outer = NewFunction;
		Params.PropertyName = TEXT("ReturnValue");

		// Create FProperty for argument
		FProperty* NewProperty = ReturnType.CreateProperty(Params);
		NewProperty->SetPropertyFlags(CPF_Parm | CPF_OutParm | CPF_ReturnParm);
                                                                                                                                   
#if 0
                                                                                                                                 
		NewProperty->AngelscriptPropertyFlags |= APF_RuntimeGenerated;
                                                                                                                            
#endif
		check(IsAngelscriptRuntimeGeneratedProperty(NewProperty));
                                                                                                                          

		// Add property into function
		NewProperty->Next = NewFunction->ChildProperties;
		NewFunction->ChildProperties = NewProperty;

		// Store it in the function for easy lookup
		if(ASFunction != nullptr)
			ASFunction->ReturnArgument = UASFunction::FArgument{NewProperty, ReturnType};
		return NewProperty;
	}

	return nullptr;
}

FProperty* FAngelscriptClassGenerator::AddFunctionArgument(UFunction* NewFunction, const FAngelscriptArgumentDesc& ArgDesc, bool bAddToArgList)
{
	auto* ASFunction = Cast<UASFunction>(NewFunction);

	FAngelscriptType::FPropertyParams Params;
	Params.Struct = nullptr;
	Params.Outer = NewFunction;
	Params.PropertyName = *ArgDesc.ArgumentName;

	// Create FProperty for argument
	FProperty* NewProperty = ArgDesc.Type.CreateProperty(Params);
	NewProperty->SetPropertyFlags(CPF_Parm);
                                                                                                                                     
#if 0
                                                                                                                                   
	NewProperty->AngelscriptPropertyFlags |= APF_RuntimeGenerated;
                                                                                                                              
#endif
	check(IsAngelscriptRuntimeGeneratedProperty(NewProperty));
                                                                                                                            

	if (ArgDesc.bBlueprintOutRef)
	{
		NewProperty->SetPropertyFlags(CPF_OutParm);
		if (ArgDesc.Type.bIsConst)
			NewProperty->SetPropertyFlags(CPF_ConstParm);
	}
	else if (ArgDesc.bBlueprintInRef)
	{
		NewProperty->SetPropertyFlags(CPF_ReferenceParm);
		NewProperty->SetPropertyFlags(CPF_OutParm);
		if (ArgDesc.Type.bIsConst)
			NewProperty->SetPropertyFlags(CPF_ConstParm);
	}

#if WITH_EDITOR
	// Handle default values from angelscript
	if (ArgDesc.DefaultValue.Len() != 0)
	{
		FString UnrealDefaultValue;
		if (ArgDesc.Type.DefaultValue_AngelscriptToUnreal(ArgDesc.DefaultValue, UnrealDefaultValue))
		{
			FString DefaultValueMeta = TEXT("CPP_Default_");
			DefaultValueMeta += ArgDesc.ArgumentName;
			NewFunction->SetMetaData(*DefaultValueMeta, *UnrealDefaultValue);
		}
	}
#endif

	// Store it in the function for easy lookup
	if (ASFunction != nullptr && bAddToArgList)
		ASFunction->Arguments.Add(UASFunction::FArgument{NewProperty, ArgDesc.Type});

	return NewProperty;
}

void FAngelscriptClassGenerator::LinkSoftReloadClasses(FModuleData& Module, FDelegateData& Delegate)
{
	if (!Delegate.OldDelegate.IsValid())
		return;
	Delegate.NewDelegate->Function = Delegate.OldDelegate->Function;
	Delegate.NewDelegate->ScriptType->SetUserData(Delegate.NewDelegate->Function);
}

void FAngelscriptClassGenerator::LinkSoftReloadClasses(FModuleData& ModuleData, FClassData& ClassData)
{
	auto ClassDesc = ClassData.NewClass;
	auto ModuleDesc = ModuleData.NewModule;

	asITypeInfo* ScriptType = GetNamespacedTypeInfoForClass(ClassData.NewClass, ModuleData.NewModule);

	if (!ClassData.NewClass->bIsStruct)
	{
		UASClass* Class = (UASClass*)ClassData.OldClass->Class;
		if (ScriptType != nullptr)
			ScriptType->SetUserData(Class);
		SetScriptStaticClass(ClassDesc, Class);
	}
	else
	{
		UASStruct* Struct = (UASStruct*)ClassData.OldClass->Struct;
		if (ScriptType != nullptr)
			ScriptType->SetUserData(Struct);
		if (Struct != nullptr)
		{
			ClassData.NewClass->Struct = Struct;

			Struct->ScriptType = ScriptType;
			Struct->UpdateScriptType();
		}

		for (auto PropDesc : ClassDesc->Properties)
		{
			// Look up the old property so we know if we have a FProperty associated
			auto OldProperty = ClassData.OldClass->GetProperty(PropDesc->PropertyName);
			PropDesc->bHasUnrealProperty = OldProperty.IsValid() && OldProperty->bHasUnrealProperty;
		}

		SetScriptStaticStruct(ClassDesc, Struct);
	}
}

void FAngelscriptClassGenerator::PrepareSoftReload(FModuleData& ModuleData, FClassData& ClassData)
{
	// Ignore any classes that don't exist yet. A queued full reload will create them later.
	if (!ClassData.OldClass.IsValid())
		return;

	UASClass* Class = (UASClass*)ClassData.OldClass->Class;

	// For soft reloads we need to allocate a CDO without any defaults, to diff to later
	extern thread_local bool GConstructASObjectWithoutDefaults;
	GConstructASObjectWithoutDefaults = true;

	UObject* CDONoDefaults;
	{
		FScopedAllowAbstractClassAllocation AllowAbstract;
		CDONoDefaults = NewObject<UObject>(GetTransientPackage(), Class,
			MakeUniqueObjectName(GetTransientPackage(), Class, *(Class->GetDefaultObjectName().ToString() + TEXT("_NoDefaults"))),
			RF_ArchetypeObject);
	}

	// Destroying the CDONoDefaults and reinitializing it makes sure we unload any Config properties in here,
	// which should be treated the same as properties modified by 'default'.
                                                                                                                
	asITypeInfo* const PreviousNoDefaultsTypeCookie = ClassData.OldClass->ScriptType;
	if (!Class->bIsModuleStaticCarrier)
		ClaimHotReloadRuntimeObjectTypeCookie(
			CDONoDefaults,
			PreviousNoDefaultsTypeCookie);
                                                                                                              
	DestructScriptObject((asCScriptObject*)CDONoDefaults, Class, (asCObjectType*)ClassData.OldClass->ScriptType);
	ReinitializeScriptObject((asCScriptObject*)CDONoDefaults, Class, (asCObjectType*)ClassData.OldClass->ScriptType);
                                                                                                               
	if (!Class->bIsModuleStaticCarrier)
		CommitHotReloadRuntimeObjectTypeCookie(
			CDONoDefaults,
			PreviousNoDefaultsTypeCookie,
			ClassData.OldClass->ScriptType);
                                                                                                             

	ClassData.CDONoDefaults = CDONoDefaults;
}

void FAngelscriptClassGenerator::DoSoftReload(FModuleData& ModuleData, FClassData& ClassData)
{
	// Ignore any classes that don't exist yet. A queued full reload will create them later.
	if (!ClassData.OldClass.IsValid())
		return;

	// Check if we've already performed the reload on this class
	if (ClassData.bReloaded)
		return;
	ClassData.bReloaded = true;

	// Log the reload if needed
	if (ClassData.OldClass.IsValid())
		UE_LOG(Angelscript, Log, TEXT("Soft Reload: %s"), *ClassData.NewClass->ClassName);

	auto ClassDesc = ClassData.NewClass;
	auto ModuleDesc = ModuleData.NewModule;

	// If we suggested a full reload but can't do one right now, mark that on the class for future use
	if (ClassData.ReloadReq >= EReloadRequirement::FullReloadSuggested)
		ClassDesc->bPendingFullReload = true;

	UASClass* Class = (UASClass*)ClassData.OldClass->Class;
	UObject* CDONoDefaults = ClassData.CDONoDefaults;
	check(Class);

	// Re-create angelscript objects for all direct instances of the class
	TArray<UObject*> Instances;
	TArray<UObject*> CDOInstances;
	GetObjectsOfClass(Class, Instances, true, RF_NoFlags);

	// Soft reloads preserve defaults-code, so we shouldn't take the new defaults code yet
	ClassDesc->DefaultsCode = ClassData.OldClass->DefaultsCode;

	// Make sure our parent is soft reloaded so we don't have to re-link properties multiple times
	UClass* SuperClass = Class->GetSuperClass();
	if (auto* ParentASClass = Cast<UASClass>(SuperClass))
	{
		EnsureReloaded(ParentASClass);
	}

	// Re-link all the class' properties into the right place
	int32 PropertiesSize = Class->GetPropertiesSize();
	FArchive ArDummy;
	for (auto PropDesc : ClassDesc->Properties)
	{
		// Look up the old property so we know if we have a FProperty associated
		auto OldProperty = ClassData.OldClass->GetProperty(PropDesc->PropertyName);
		PropDesc->bHasUnrealProperty = OldProperty.IsValid() && OldProperty->bHasUnrealProperty;

		// Could be a new property that's not in the class. No big deal.
		if (!PropDesc->bHasUnrealProperty)
			continue;

		FProperty* Property = Class->FindPropertyByName(FName(*PropDesc->GetUnrealPropertyName()));

		// Only re-link properties directly in this class
		if (Property->GetOwnerClass() != Class)
			continue;

		Class->SetPropertiesSize(PropDesc->ScriptPropertyOffset);
		Property->Link(ArDummy);
	}

	// After linking the properties, destroy the unversioned schema on the class so it refreshes
	// Properties may have changed offsets so the schema could be out of date
                                                                                                                              
#if 0
                                                                                                                            
	COREUOBJECT_API extern void DestroyUnversionedSchema(const UStruct* Struct);
	DestroyUnversionedSchema(Class);
                                                                                                                            
#endif
#if !SOLOASBRIDGE_ENGINE_PROFILE_MATCHED
#error Class generator requires the configured engine schema layout.
#endif
	const FUnversionedStructSchema* PreviousGameSchema =
		Class->UnversionedGameSchema.exchange(nullptr, std::memory_order_acq_rel);
	FMemory::Free(const_cast<FUnversionedStructSchema*>(PreviousGameSchema));
#if WITH_EDITORONLY_DATA
	const FUnversionedStructSchema* PreviousEditorSchema =
		Class->UnversionedEditorSchema.exchange(nullptr, std::memory_order_acq_rel);
	FMemory::Free(const_cast<FUnversionedStructSchema*>(PreviousEditorSchema));
#endif
                                                                                                                          

	// If the class has default components, update the offsets to match the new property offsets
	for (auto& DefaultComp : Class->DefaultComponents)
	{
		FProperty* Property = Class->FindPropertyByName(DefaultComp.ComponentName);
		check(Property != nullptr);

		DefaultComp.VariableOffset = Property->GetOffset_ForUFunction();
	}

	// If the class has override components, update the offsets to match the new property offsets
	for (auto& OverrideComp : Class->OverrideComponents)
	{
		FProperty* Property = Class->FindPropertyByName(OverrideComp.VariableName);
		check(Property != nullptr);

		OverrideComp.VariableOffset = Property->GetOffset_ForUFunction();
	}

	// The associated angelscript type should know which class it is so it can create objects
	asITypeInfo* OldScriptType = ClassData.OldClass->ScriptType;
	asITypeInfo* ScriptType = GetNamespacedTypeInfoForClass(ClassData.NewClass, ModuleData.NewModule);
                                                                                                                         
	extern void RetireRuntimeTerminalClassChain(UASClass*);
	RetireRuntimeTerminalClassChain(Class);
                                                                                                                       
	Class->ScriptTypePtr = ScriptType;

	Class->SetPropertiesSize(PropertiesSize);
	if (ScriptType != nullptr)
                                                                                                                                 
#if 0
                                                                                                                               
		Class->ContainerSize = ScriptType->GetSize();
                                                                                                                               
#endif
	{
		const int32 ContainerSize = FMath::Max(Class->GetPropertiesSize(), static_cast<int32>(ScriptType->GetSize()));
		Class->SetPropertiesSize(ContainerSize);
		check(ContainerSize >= Class->GetPropertiesSize());
		check(Class->ScriptPropertyOffset <= ContainerSize);
		extern void PrepareRuntimeScriptTypeIdentity(UASClass*, asITypeInfo*);
		PrepareRuntimeScriptTypeIdentity(Class, ScriptType);
	}
                                                                                                                             

	// Update class flags so we catch when the specifiers have changed
	if (ClassDesc->bPlaceable.IsSet())
	{
		if(ClassDesc->bPlaceable.GetValue())
			Class->ClassFlags &= ~CLASS_NotPlaceable; 
		else
			Class->ClassFlags |= CLASS_NotPlaceable; 
	}
	else
	{
		if (SuperClass->HasAnyClassFlags(CLASS_NotPlaceable))
			Class->ClassFlags |= CLASS_NotPlaceable; 
		else
			Class->ClassFlags &= ~CLASS_NotPlaceable; 
	}

	if (ClassDesc->bAbstract)
		Class->ClassFlags |= CLASS_Abstract;
	else
		Class->ClassFlags &= ~CLASS_Abstract;

	if (ClassDesc->bTransient)
		Class->ClassFlags |= CLASS_Transient;
	else if (!SuperClass->HasAnyClassFlags(CLASS_Transient))
		Class->ClassFlags &= ~CLASS_Transient;

	if (ClassDesc->bHideDropdown)
		Class->ClassFlags |= CLASS_HideDropDown;
	else
		Class->ClassFlags &= ~CLASS_HideDropDown;

	if (ClassDesc->bDefaultToInstanced)
		Class->ClassFlags |= CLASS_DefaultToInstanced;
	else if (!SuperClass->HasAnyClassFlags(CLASS_DefaultToInstanced))
		Class->ClassFlags &= ~CLASS_DefaultToInstanced;

	if (ClassDesc->bEditInlineNew)
		Class->ClassFlags |= CLASS_EditInlineNew;
	else if (!SuperClass->HasAnyClassFlags(CLASS_EditInlineNew))
		Class->ClassFlags &= ~CLASS_EditInlineNew;

	if (ClassDesc->bIsDeprecatedClass)
		Class->ClassFlags |= CLASS_Deprecated;
	else if (!SuperClass->HasAnyClassFlags(CLASS_Deprecated))
		Class->ClassFlags &= ~CLASS_Deprecated;

	// Re-link all the class' functions so they point to the right script function
	for (auto FuncDesc : ClassDesc->Methods)
	{
		auto OldFuncDesc = ClassData.OldClass->GetMethod(FuncDesc->FunctionName);

		// New function, nothing to soft reload
		if (!OldFuncDesc.IsValid())
			continue;

		if (OldFuncDesc->Function != nullptr && OldFuncDesc->ScriptFunction != nullptr)
		{
			FuncDesc->Function = OldFuncDesc->Function;
			((UASFunction*)FuncDesc->Function)->ScriptFunction = (asCScriptFunction*)FuncDesc->ScriptFunction;

			// We need to check the function's arguments and update the script types in them
			SoftReloadFunction(OldFuncDesc->Function);

#if WITH_EDITOR
			// Update the no-op flag
			if (FuncDesc->bIsNoOp != OldFuncDesc->bIsNoOp)
			{
				if (FuncDesc->bIsNoOp)
					OldFuncDesc->Function->SetMetaData(FUNCMETA_ScriptNoOp, TEXT("true"));
				else
					OldFuncDesc->Function->RemoveMetaData(FUNCMETA_ScriptNoOp);
			}
#endif
		}
	}

	// Re-assemble the class' tokens for garbage collection
	Class->AssembleReferenceTokenStream(true);

	// Record the old class in the new module
	ClassDesc->ScriptType = ScriptType;
	ClassDesc->Class = Class;

	UpdateConstructAndDefaultsFunctions(ClassDesc, Class);

	// Re-detect all angelscript properties that should be reference collected
	DetectAngelscriptReferences(ClassDesc);
                                                                                                               
	extern void RebuildRuntimeTerminalClassChain(UASClass*);
	RebuildRuntimeTerminalClassChain(Class);
                                                                                                             

	// We also need to update the script object type for all derived blueprint classes
	ForEachObjectOfClass(UBlueprintGeneratedClass::StaticClass(), [&](UObject* Obj)
	{
		UClass* CheckClass = (UClass*)Obj;
		if (!CheckClass->IsChildOf(Class))
			return;

		// Don't touch classes that have been replaced through a full reload
                                                                                                                     
#if 0
                                                                                                                   
		if (CheckClass->ScriptTypePtr == nullptr)
			return;
                                                                                                                 
#endif
		// Script carriers are updated by their own reload pass, not the Blueprint-child pass.
		if (Cast<UASClass>(CheckClass) != nullptr)
			return;
		if (CheckClass->HasAnyClassFlags(CLASS_NewerVersionExists))
			return;
		// Ordinary Blueprint descendants resolve their type from AS ancestry.
		// A missing direct sidecar must not skip schema and reference refresh.
		asITypeInfo* CheckScriptType = GetDirectAngelscriptClassScriptType(CheckClass);
                                                                                                               

		UASClass* ASClass = UASClass::GetFirstASClass(CheckClass);
		if (ASClass == Class)
		{
                                                                                                                  
#if 0
                                                                                                                
			ensure(CheckClass->ScriptTypePtr == OldScriptType);
                                                                                                              
#endif
			ensure(CheckScriptType == nullptr || CheckScriptType == OldScriptType);
                                                                                                            

			// Update the actual angelscript type we're using
                                                                                                                
#if 0
                                                                                                              
			CheckClass->ScriptTypePtr = Class->ScriptTypePtr;
                                                                                                            
#endif
			// Preserve dynamic ancestry lookup instead of creating a reparent-stale cache.
			if (CheckScriptType != nullptr)
				verify(SetAngelscriptClassScriptType(
					CheckClass,
					GetAngelscriptClassScriptType(Class)
				));
                                                                                                          

			// Refresh the serialization schema
                                                                                                                                         
#if 0
                                                                                                                                       
			DestroyUnversionedSchema(CheckClass);
                                                                                                                                       
#endif
#if !SOLOASBRIDGE_ENGINE_PROFILE_MATCHED
#error Class generator requires the configured engine schema layout.
#endif
			const FUnversionedStructSchema* PreviousGameSchema =
				CheckClass->UnversionedGameSchema.exchange(
					nullptr,
					std::memory_order_acq_rel
				);
			FMemory::Free(
				const_cast<FUnversionedStructSchema*>(PreviousGameSchema)
			);
#if WITH_EDITORONLY_DATA
			const FUnversionedStructSchema* PreviousEditorSchema =
				CheckClass->UnversionedEditorSchema.exchange(
					nullptr,
					std::memory_order_acq_rel
				);
			FMemory::Free(
				const_cast<FUnversionedStructSchema*>(PreviousEditorSchema)
			);
#endif
                                                                                                                                     

			// Poke the reference token stream so we update our property offsets
			CheckClass->AssembleReferenceTokenStream(true);

#if WITH_EDITOR
			// If this is an Animation Blueprint, we need to re-link its PropertyAccesses.
			// Properties might have changed offsets when we Link()ed them above, and PropertyAccess
			// stores offsets to properties for use in ABP evaluation, so we need them to be updated.
			if (UAnimBlueprintGeneratedClass* ABPGC = Cast<UAnimBlueprintGeneratedClass>(CheckClass))
			{
				if (const FAnimSubsystem_PropertyAccess* PropertyAccess = IAnimClassInterface::GetFromClass(CheckClass)->FindSubsystem<FAnimSubsystem_PropertyAccess>())
				{
					FAnimSubsystemContext SubsystemContext(*PropertyAccess, FAnimSubsystem_PropertyAccess::StaticStruct());
					FAnimSubsystemLinkContext LinkContext(SubsystemContext, *ABPGC);
					((FAnimSubsystem_PropertyAccess*)PropertyAccess)->OnLink(LinkContext);
				}
			}
#endif
		}
	}, true);

	struct FRawUnrealPropertyType : public FAngelscriptType
	{
		virtual bool CanCopy(const FAngelscriptTypeUsage& Usage) const override { return true; }
		virtual bool NeedCopy(const FAngelscriptTypeUsage& Usage) const override { return true; }
		virtual void CopyValue(const FAngelscriptTypeUsage& Usage, void* SourcePtr, void* DestinationPtr) const
		{
			Usage.UnrealProperty->CopyCompleteValue(DestinationPtr, SourcePtr);
		}

		virtual bool CanCompareForHotReload(const FAngelscriptTypeUsage& Usage) const override { return true; }
		virtual bool IsValueEqualForHotReload(const FAngelscriptTypeUsage& Usage, void* SourcePtr, void* DestinationPtr) const override
		{
			return Usage.UnrealProperty->Identical(SourcePtr, DestinationPtr);
		}

		virtual bool CanConstruct(const FAngelscriptTypeUsage& Usage) const override { return true; }
		virtual bool NeedConstruct(const FAngelscriptTypeUsage& Usage) const override { return true; }
		virtual void ConstructValue(const FAngelscriptTypeUsage& Usage, void* DestinationPtr) const override
		{
			Usage.UnrealProperty->InitializeValue(DestinationPtr);
		}

		virtual int32 GetValueSize(const FAngelscriptTypeUsage& Usage) const override
		{
			return Usage.UnrealProperty->GetSize();
		}

		virtual bool CanDestruct(const FAngelscriptTypeUsage& Usage) const override { return true; }
		virtual bool NeedDestruct(const FAngelscriptTypeUsage& Usage) const override { return true; }
		virtual void DestructValue(const FAngelscriptTypeUsage& Usage, void* DestinationPtr) const override
		{
			Usage.UnrealProperty->DestroyValue(DestinationPtr);
		}
	};

	// Helpers for flattening properties local to a class and expanding structs
	struct FLocalProperty
	{
		FString Name;
		FAngelscriptTypeUsage Type;
		int Offset = -1;
	};

	struct FLocalPropertyContext
	{
		int BaseOffset = 0;
		asITypeInfo* ScriptType = nullptr;
		UStruct* UnrealStruct = nullptr;
		int IgnoreBeforeOffset = 0;
		FString NamePrefix;

		static TSharedPtr<FAngelscriptType> GetRawUnrealPropertyType()
		{
			static TSharedPtr<FAngelscriptType> Type = MakeShared<FRawUnrealPropertyType>();
			return Type;
		}

		void Append(TArray<FLocalProperty>& Properties, const FString& Name, int Offset, const FAngelscriptTypeUsage& Type)
		{
			if (UStruct* InnerStruct = Type.Type->GetUnrealStruct(Type))
			{
				UScriptStruct* ScriptStruct = Cast<UScriptStruct>(InnerStruct);
				if (ScriptStruct != FInstancedStruct::StaticStruct())
				{
					if (ScriptStruct == nullptr || (ScriptStruct->StructFlags & STRUCT_Atomic) == 0)
					{
						FLocalPropertyContext InnerContext;
						InnerContext.BaseOffset = Offset + BaseOffset;
						InnerContext.UnrealStruct = InnerStruct;
						InnerContext.NamePrefix = NamePrefix + Name + TEXT(";");

						InnerContext.Resolve(Properties);
						return;
					}
				}
			}

			FLocalProperty LocalProp;
			LocalProp.Name = NamePrefix + Name;
			LocalProp.Type = Type;
			LocalProp.Offset = Offset + BaseOffset;
			Properties.Add(LocalProp);
		}

		void Resolve(TArray<FLocalProperty>& Properties)
		{
			if (ScriptType != nullptr)
			{
				for (int32 i = 0, PropertyCount = ScriptType->GetPropertyCount(); i < PropertyCount; ++i)
				{
					const char* Name;
					int PropertyOffset;
					int TypeId;

					ScriptType->GetProperty(i, &Name, &TypeId, nullptr, nullptr, &PropertyOffset);

					if (PropertyOffset < IgnoreBeforeOffset)
						continue;

					auto PropertyType = FAngelscriptTypeUsage::FromProperty(ScriptType, i);
					if (PropertyType.IsValid())
						Append(Properties, ANSI_TO_TCHAR(Name), PropertyOffset, PropertyType);
				}
			}

			if (UnrealStruct != nullptr)
			{
				for (TFieldIterator<FProperty> It(UnrealStruct); It; ++It)
				{
					FProperty* Property = *It;

					FAngelscriptTypeUsage Type = FAngelscriptTypeUsage::FromProperty(Property);
					if (!Type.IsValid())
					{
						Type.Type = GetRawUnrealPropertyType();
						Type.UnrealProperty = Property;
					}

					if (Property->ArrayDim == 1)
					{
						Append(Properties, Property->GetName(), Property->GetOffset_ForUFunction(), Type);
					}
					else
					{
						int32 ElementSize = Property->GetElementSize();
						for (int32 i = 0; i < Property->ArrayDim; ++i)
						{
							FString IndexedPropertyName = FString::Printf(TEXT("%s[%d]"), *Property->GetName(), i);
							Append(Properties, IndexedPropertyName, Property->GetOffset_ForUFunction() + ElementSize*i, Type);
						}
					}
				}
			}
		}
	};


	// Detect which properties can be copied from the old instance to the new 
	struct FPropertyCopy
	{
		FString Name;
		FAngelscriptTypeUsage Type;
		SIZE_T OldOffset = 0;
		SIZE_T NewOffset = 0;
		bool bCanCompare = false;
		bool bNeedConstruct = false;
		bool bNeedDestruct = false;
		bool bIsInstanced = false;
		bool bModifiedByDefaults = false;
	};

	TMap<FString, FPropertyCopy> OldProperties;
	TArray<FPropertyCopy> PropertiesToCopy;

	if (OldScriptType != nullptr)
	{
		auto* BaseCDO = Class->GetDefaultObject();
		asCScriptObject* BaseCDOScriptObject = (asCScriptObject*)FAngelscriptManager::UObjectToAngelscript(BaseCDO);

		FLocalPropertyContext Lookup;
		Lookup.ScriptType = OldScriptType;
		// Never copy c++ properties
		Lookup.IgnoreBeforeOffset = ClassData.OldClass->CodeSuperClass->GetPropertiesSize();

		TArray<FLocalProperty> LocalProperties;
		Lookup.Resolve(LocalProperties);

		for (const FLocalProperty& LocalProp : LocalProperties)
		{
			FAngelscriptTypeUsage PropertyType = LocalProp.Type;
			int PropertyOffset = LocalProp.Offset;

			if (PropertyType.CanCopy() && PropertyType.CanConstruct() && PropertyType.CanDestruct())
			{
				FPropertyCopy Copy;
				Copy.Name = LocalProp.Name;
				Copy.Type = PropertyType;
				Copy.OldOffset = PropertyOffset;
				Copy.bCanCompare = PropertyType.CanCompareForHotReload();
				Copy.bNeedConstruct = PropertyType.NeedConstruct();
				Copy.bNeedDestruct = PropertyType.NeedDestruct();
				Copy.bIsInstanced = PropertyType.IsObjectPointer();

				// We need to determine whether the Base CDO has a different value from the
				// Base CDO without defaults. That tells us whether a default statement changes
				// this property meaning we need to copy it over regardless of the value.
				if (Copy.bCanCompare)
				{
					void* BaseCDOPtr = (void*)((SIZE_T)BaseCDO + Copy.OldOffset);
					void* CDONoDefaultsPtr = (void*)((SIZE_T)CDONoDefaults + Copy.OldOffset);
					if (!Copy.Type.IsValueEqualForHotReload(BaseCDOPtr, CDONoDefaultsPtr))
					{
						Copy.bModifiedByDefaults = true;
					}
				}

				OldProperties.Add(LocalProp.Name, Copy);
			}
		}
	}

	if (ScriptType != nullptr)
	{
		FLocalPropertyContext Lookup;
		Lookup.ScriptType = ScriptType;
		// Never copy c++ properties
		Lookup.IgnoreBeforeOffset = ClassData.NewClass->CodeSuperClass->GetPropertiesSize();

		TArray<FLocalProperty> LocalProperties;
		Lookup.Resolve(LocalProperties);

		for (const FLocalProperty& LocalProp : LocalProperties)
		{
			FAngelscriptTypeUsage PropertyType = LocalProp.Type;
			int PropertyOffset = LocalProp.Offset;

			if (PropertyType.CanCopy() && PropertyType.CanConstruct() && PropertyType.CanDestruct())
			{
				// See if this property was in the old class
				FPropertyCopy* Copy = OldProperties.Find(LocalProp.Name);
				if (Copy != nullptr && Copy->Type == PropertyType)
				{
					Copy->NewOffset = PropertyOffset;
					PropertiesToCopy.Add(*Copy);
				}
			}
		}

		// Temp buffer is used to copy values to from the old script object
		// so we can copy them back in after constructing the new script object
		// in the same place in memory.
		TArray<uint8> TempBuffer;
		if (ScriptType != nullptr)
			TempBuffer.AddUninitialized(ScriptType->GetSize() + 32);

		uint8* TempData = Align(TempBuffer.GetData(), 16);

		// Temp flags whether values were stored for each property we can copy
		TArray<bool> TempShouldCopy;
		TempShouldCopy.AddUninitialized(PropertiesToCopy.Num());

		auto* BaseCDO = Class->GetDefaultObject();
		asCScriptObject* BaseCDOScriptObject = (asCScriptObject*)FAngelscriptManager::UObjectToAngelscript(BaseCDO);

		for (UObject* Instance : Instances)
		{
                                                                                                             
			if (!IsValid(Instance))
			{
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
				// Derived script classes own explicit retirement in the legacy protocol.
				if (UASClass::GetFirstASClass(Instance->GetClass()) == Class)
					UASClass::DestroyTrackedRuntimeObjectExplicitly(Instance);
#endif
				// The public FinishDestroy path owns finalization. A garbage template
				// must not be reconstructed or explicitly destroyed by later soft reloads.
				continue;
			}
                                                                                                           
			asCScriptObject* ScriptObject = (asCScriptObject*)FAngelscriptManager::UObjectToAngelscript(Instance);

			// Re-create this only if the script object is directly our script type,
			// descendant types will be handled by that type's soft reload.
			if (ScriptObject->GetObjectType() != ScriptType)
				continue;
			if (Instance->HasAnyFlags(RF_FinishDestroyed))
				continue;

			// CDOs will be reinstanced later, so we can still check their property values
			if (Instance->HasAnyFlags(RF_ClassDefaultObject))
			{
				CDOInstances.Add(Instance);
				continue;
			}

			auto* AssociatedCDO = Instance->GetClass()->GetDefaultObject();
			asCScriptObject* CDOScriptObject = (asCScriptObject*)FAngelscriptManager::UObjectToAngelscript(AssociatedCDO);

			// Save properties from old script object to temporary buffer where appropriate
			for (int32 i = 0, PropNum = PropertiesToCopy.Num(); i < PropNum; ++i)
			{
				auto& Copy = PropertiesToCopy[i];
				void* OriginalPtr = (void*)((SIZE_T)ScriptObject + Copy.OldOffset);
				void* CDOPtr = (void*)((SIZE_T)CDOScriptObject + Copy.OldOffset);
				void* NewPtr = (void*)((SIZE_T)TempData + Copy.NewOffset);

				// Only copy values that differ from the original CDO value, or are modified by default statements
				bool bShouldCopy = Copy.bModifiedByDefaults || !Copy.bCanCompare || !Copy.Type.IsValueEqualForHotReload(CDOPtr, OriginalPtr);

				if (!bShouldCopy && AssociatedCDO != BaseCDO)
				{
					// If our CDO's value was different from the base CDO, we need
					// to copy the CDO's old value to our new instance.
					void* BaseCDOPtr = (void*)((SIZE_T)BaseCDOScriptObject + Copy.OldOffset);
					if (!Copy.Type.IsValueEqualForHotReload(BaseCDOPtr, CDOPtr))
					{
						OriginalPtr = CDOPtr; // Copy CDO value
						bShouldCopy = true;
					}
				}

				TempShouldCopy[i] = bShouldCopy;
				if (bShouldCopy)
				{
					if (Copy.bNeedConstruct)
						Copy.Type.ConstructValue(NewPtr);
					Copy.Type.CopyValue(OriginalPtr, NewPtr);
				}
			}

			// Recreate the script instance
                                                                                                              
			asITypeInfo* const PreviousInstanceTypeCookie = OldScriptType;
			ClaimHotReloadRuntimeObjectTypeCookie(
				FAngelscriptManager::AngelscriptToUObject(ScriptObject),
				PreviousInstanceTypeCookie);
                                                                                                            
			DestructScriptObject(ScriptObject, Class, (asCObjectType*)OldScriptType);

			// Copy properties from temporary buffer to new script object at the right place
			for (int32 i = 0, PropNum = PropertiesToCopy.Num(); i < PropNum; ++i)
			{
				if (TempShouldCopy[i])
				{
					auto& Copy = PropertiesToCopy[i];
					if (!Copy.bIsInstanced)
						continue;

					void* OriginalPtr = (void*)((SIZE_T)TempData + Copy.NewOffset);
					void* NewPtr = (void*)((SIZE_T)ScriptObject + Copy.NewOffset);

					Copy.Type.CopyValue(OriginalPtr, NewPtr);
				}
			}

			ReinitializeScriptObject(ScriptObject, Class, (asCObjectType*)ScriptType);

			// Copy properties from temporary buffer to new script object at the right place
			for (int32 i = 0, PropNum = PropertiesToCopy.Num(); i < PropNum; ++i)
			{
				if (TempShouldCopy[i])
				{
					auto& Copy = PropertiesToCopy[i];

					void* OriginalPtr = (void*)((SIZE_T)TempData + Copy.NewOffset);
					void* NewPtr = (void*)((SIZE_T)ScriptObject + Copy.NewOffset);

					Copy.Type.CopyValue(OriginalPtr, NewPtr);
					if (Copy.bNeedDestruct)
						Copy.Type.DestructValue(OriginalPtr);
				}
			}
                                                                                                             
			// Commit only after construction and restored/default property state are complete.
			CommitHotReloadRuntimeObjectTypeCookie(
				FAngelscriptManager::AngelscriptToUObject(ScriptObject),
				PreviousInstanceTypeCookie,
				ScriptType);
                                                                                                           
		}

		// Recreate all CDO script objects
		struct FCDOReinstanceData
		{
			TArray<uint8> TempBuffer;
			TArray<bool> TempShouldCopy;
			uint8* TempData;
		};

		TArray<FCDOReinstanceData> CDOReinstanceData;
		CDOReinstanceData.AddDefaulted(CDOInstances.Num());

		for (int32 i = 0, CDONum = CDOInstances.Num(); i < CDONum; ++i)
		{
			UObject* CDO = CDOInstances[i];
			asCScriptObject* CDOScriptObject = (asCScriptObject*)FAngelscriptManager::UObjectToAngelscript(CDO);

			FCDOReinstanceData& Data = CDOReinstanceData[i];
			Data.TempBuffer.AddUninitialized(ScriptType->GetSize() + 32);
			Data.TempShouldCopy.AddUninitialized(PropertiesToCopy.Num());
			Data.TempData = Align(Data.TempBuffer.GetData(), 16);

			// If this is not the Base CDO of this script class, we have a parent CDO
			UObject* ParentCDO = nullptr;
			asCScriptObject* ParentCDOScriptObject = nullptr;
			if (CDO != BaseCDO)
			{
				ParentCDO = CDO->GetClass()->GetSuperClass()->GetDefaultObject();
				ParentCDOScriptObject = (asCScriptObject*)FAngelscriptManager::UObjectToAngelscript(ParentCDO);
			}

			// Save properties from old script object to temporary buffer where appropriate
			for (int32 PropIndex = 0, PropNum = PropertiesToCopy.Num(); PropIndex < PropNum; ++PropIndex)
			{
				auto& Copy = PropertiesToCopy[PropIndex];
				void* CDOPtr = (void*)((SIZE_T)CDOScriptObject + Copy.OldOffset);
				void* SourcePtr = nullptr;

				bool bShouldCopy = false;

				if (Copy.bModifiedByDefaults)
				{
					bShouldCopy = true;
					SourcePtr = CDOPtr;
				}
				else if (ParentCDO != nullptr)
				{
					// CDO Values should be copied if this CDO is not the base CDO,
					// and then they should be copied if they differ from the parent CDO.
					void* ParentPtr = (void*)((SIZE_T)ParentCDOScriptObject + Copy.OldOffset);
					if (!Copy.bCanCompare || !Copy.Type.IsValueEqualForHotReload(ParentPtr, CDOPtr))
					{
						SourcePtr = CDOPtr;
						bShouldCopy = true;
					}
					else
					{
						// If our parent's value is different from the base value,
						// we should copy it as well.
						if (ParentCDO != BaseCDO)
						{
							void* BasePtr = (void*)((SIZE_T)BaseCDOScriptObject + Copy.OldOffset);
							if (!Copy.Type.IsValueEqualForHotReload(ParentPtr, BasePtr))
							{
								SourcePtr = ParentPtr;
								bShouldCopy = true;
							}
						}
					}
				}

				// Instanced properties should always be copied over to the new place first
				if (!bShouldCopy && Copy.bIsInstanced)
				{
					SourcePtr = CDOPtr;
					bShouldCopy = true;
				}

				Data.TempShouldCopy[PropIndex] = bShouldCopy;
				if (bShouldCopy)
				{
					void* DestPtr = (void*)((SIZE_T)Data.TempData + Copy.NewOffset);
					if (Copy.bNeedConstruct)
						Copy.Type.ConstructValue(DestPtr);
					Copy.Type.CopyValue(SourcePtr, DestPtr);
				}
			}
		}

		for (int32 i = 0, CDONum = CDOInstances.Num(); i < CDONum; ++i)
		{
			UObject* CDO = CDOInstances[i];
			FCDOReinstanceData& Data = CDOReinstanceData[i];

			// Actually reinstance the CDO script object
			asCScriptObject* ScriptObject = (asCScriptObject*)FAngelscriptManager::UObjectToAngelscript(CDO);
                                                                                                         
			asITypeInfo* const PreviousCdoTypeCookie = OldScriptType;
			ClaimHotReloadRuntimeObjectTypeCookie(
				CDO,
				PreviousCdoTypeCookie);
                                                                                                       
			DestructScriptObject(ScriptObject, Class, (asCObjectType*)OldScriptType);

			// Copy properties from temporary buffer to new script object at the right place
			for (int32 PropIndex = 0, PropNum = PropertiesToCopy.Num(); PropIndex < PropNum; ++PropIndex)
			{
				if (Data.TempShouldCopy[PropIndex])
				{
					auto& Copy = PropertiesToCopy[PropIndex];
					if (!Copy.bIsInstanced)
						continue;

					void* OriginalPtr = (void*)((SIZE_T)Data.TempData + Copy.NewOffset);
					void* NewPtr = (void*)((SIZE_T)ScriptObject + Copy.NewOffset);

					Copy.Type.CopyValue(OriginalPtr, NewPtr);
				}
			}

			ReinitializeScriptObject(ScriptObject, Class, (asCObjectType*)ScriptType);

			// Copy properties from temporary buffer to new script object at the right place
			for (int32 PropIndex = 0, PropNum = PropertiesToCopy.Num(); PropIndex < PropNum; ++PropIndex)
			{
				if (Data.TempShouldCopy[PropIndex])
				{
					auto& Copy = PropertiesToCopy[PropIndex];

					void* OriginalPtr = (void*)((SIZE_T)Data.TempData + Copy.NewOffset);
					void* NewPtr = (void*)((SIZE_T)ScriptObject + Copy.NewOffset);

					Copy.Type.CopyValue(OriginalPtr, NewPtr);
					if (Copy.bNeedDestruct)
						Copy.Type.DestructValue(OriginalPtr);
				}
			}
                                                                                                        
			// Commit only after construction and restored/default property state are complete.
			CommitHotReloadRuntimeObjectTypeCookie(
				CDO,
				PreviousCdoTypeCookie,
				ScriptType);
                                                                                                      
		}
	}

	// Clean up the temporary CDO we created
	CDONoDefaults->MarkAsGarbage();
}

void FAngelscriptClassGenerator::SoftReloadFunction(UFunction* Function)
{
	auto* ASFunction = Cast<UASFunction>(Function);
	if (ASFunction == nullptr)
		return;

	for (int32 i = 0, Num = ASFunction->Arguments.Num(); i < Num; ++i)
		SoftReloadType(ASFunction->Arguments[i].Type);
	for (int32 i = 0, Num = ASFunction->DestroyArguments.Num(); i < Num; ++i)
		SoftReloadType(ASFunction->DestroyArguments[i].Type);
	
	SoftReloadType(ASFunction->ReturnArgument.Type);
}

void FAngelscriptClassGenerator::SoftReloadType(FAngelscriptTypeUsage& Usage)
{
	if (Usage.ScriptClass != nullptr)
	{
		asITypeInfo** NewType = UpdatedScriptTypeMap.Find(Usage.ScriptClass);
		if (NewType != nullptr)
			Usage.ScriptClass = *NewType;
	}

	for (auto& SubType : Usage.SubTypes)
	{
		SoftReloadType(SubType);
	}
}

void FAngelscriptClassGenerator::DestructScriptObject(class asCScriptObject* ScriptObject, UASClass* NewClass, class asCObjectType* ObjectTypeToDestruct)
{
	// Pretend-destroy old object
	if (ObjectTypeToDestruct != nullptr)
                                                                                                        
    if (AngelscriptRuntimeGcMirror::ClaimRuntimeScriptDestructor(
            FAngelscriptManager::AngelscriptToUObject(ScriptObject)))
                                                                                                      
		ScriptObject->CallDestructor(ObjectTypeToDestruct);

	auto* NewObjectType = (asCObjectType*)NewClass->ScriptTypePtr;
	if (NewObjectType == nullptr)
		return;

	// Zero out the memory used for the script object, we don't want to have trash data in here still
	UObject* Object = FAngelscriptManager::AngelscriptToUObject(ScriptObject);
	UASClass* ASClass = UASClass::GetFirstASClass(Object);
	FMemory::Memzero((void*)((SIZE_T)Object + ASClass->ScriptPropertyOffset),
		ASClass->GetPropertiesSize() - ASClass->ScriptPropertyOffset);
}

void FAngelscriptClassGenerator::ReinitializeScriptObject(class asCScriptObject* ScriptObject, UASClass* NewClass, class asCObjectType* ObjectTypeToConstruct)
{
	if (ObjectTypeToConstruct == nullptr)
		return;

                                                                                                        
    AngelscriptRuntimeGcMirror::ResetRuntimeScriptDestructor(
        FAngelscriptManager::AngelscriptToUObject(ScriptObject));
                                                                                                      
	// Construct the C++ part of the angelscript scriptobject
	new(ScriptObject) asCScriptObject(ObjectTypeToConstruct);

	if (ObjectTypeToConstruct->beh.construct != 0)
	{
		// Call the angelscript constructor of the scriptobject
		auto& Manager = FAngelscriptManager::Get();
		FAngelscriptContext Context((UObject*)ScriptObject);
		Context->Prepare(Manager.Engine->GetFunctionById(ObjectTypeToConstruct->beh.construct));
		Context->SetObject(ScriptObject);
		Context->Execute();
	}
	else
	{
		ensureMsgf(false, TEXT("Angelscript implemented class does not have a constructor with no arguments. This will crash soon."));
	}
}

static UE::GC::ObjectAROFn GetARO(UClass* Class)
{
	UE::GC::ObjectAROFn ARO = Class->CppClassStaticFunctions.GetAddReferencedObjects();
	check(ARO != nullptr);
	return ARO != &UObject::AddReferencedObjects ? ARO : nullptr;
}

void FAngelscriptClassGenerator::DetectAngelscriptReferences(TSharedPtr<FAngelscriptClassDesc> ClassDesc)
{
	UASClass* Class = (UASClass*)ClassDesc->Class;
	asITypeInfo* ScriptType = (asITypeInfo*)Class->ScriptTypePtr;
	if (ScriptType == nullptr)
		return;

	// Pop the End-Of-Stream token from the end of the class' existing stream
	UE::GC::FSchemaBuilder Schema(0);
	UE::GC::FPropertyStack PropertyStack;

	FAngelscriptType::FGCReferenceParams RefParams;
	RefParams.Class = Class;
	RefParams.DebugPath = &PropertyStack;
	RefParams.Schema = &Schema;

	Schema.Append(Class->ReferenceSchema.Get());
	const int32 NumPreviousMembers = Schema.NumMembers();

	for (int32 i = 0, PropertyCount = ScriptType->GetPropertyCount(); i < PropertyCount; ++i)
	{
		const char* Name;
		int PropertyOffset;
		int TypeId;

		ScriptType->GetProperty(i, &Name, &TypeId, nullptr, nullptr, &PropertyOffset);

		// We don't care about primitives for this
		if (TypeId <= asTYPEID_LAST_PRIMITIVE)
			continue;

		// Our super class will have dealt with inherited properties
		if (ScriptType->IsPropertyInherited(i))
			continue;

		bool bAddedAsUnrealProperty = false;
		FAngelscriptTypeUsage PropertyType;

		auto PropDesc = ClassDesc->GetProperty(ANSI_TO_TCHAR(Name));
		if (PropDesc.IsValid())
		{
			PropertyType = PropDesc->PropertyType;
			bAddedAsUnrealProperty = PropDesc->bHasUnrealProperty;
		}
		else
		{
			PropertyType = FAngelscriptTypeUsage::FromProperty(ScriptType, i);
		}

		if (!bAddedAsUnrealProperty)
		{
			if(PropertyType.HasReferences())
			{
				RefParams.AtOffset = PropertyOffset;
				RefParams.Names.Push(Name);
				PropertyType.EmitReferenceInfo(RefParams);
				RefParams.Names.Pop();
			}
		}
	}

	const bool bOverrideReferenceSchema = Schema.NumMembers() != NumPreviousMembers || NumPreviousMembers == 0;
	if (bOverrideReferenceSchema)
	{
		UE::GC::FSchemaView View(Schema.Build(GetARO(Class)), UE::GC::EOrigin::Other);
		Class->ReferenceSchema.Set(View);
	}
}

void FAngelscriptClassGenerator::SetScriptStaticClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc, UClass* Class)
{
	if (ClassDesc->ScriptType == nullptr)
		return; // Statics classes don't exist in angelscript

	// AngelscriptPreprocessor added a global UClass variable in script
	// that it expects us to fill in with the class we generate,
	// this way, ::StaticClass() works on script classes.
	asCModule* ScriptModule = (asCModule*)ClassDesc->ScriptType->GetModule();

	asSNameSpace* ScriptNamespace = nullptr;
	if (ClassDesc->Namespace.IsSet())
		ScriptNamespace = ScriptModule->engine->FindNameSpace(TCHAR_TO_ANSI(*ClassDesc->Namespace.GetValue()));
	else
		ScriptNamespace = ScriptModule->defaultNamespace;

	asCGlobalProperty* Property = ScriptModule->scriptGlobals.FindFirst(
		TCHAR_TO_ANSI(*ClassDesc->StaticClassGlobalVariableName),
		ScriptNamespace
	);
	if (Property == nullptr)
	{
		ensure(false);
		return;
	}

	void* VarAddr = Property->GetAddressOfValue();
	**(TSubclassOf<UObject>**)VarAddr = Class;
}

void FAngelscriptClassGenerator::SetScriptStaticStruct(TSharedPtr<FAngelscriptClassDesc> ClassDesc, UScriptStruct* Struct)
{
	if (ClassDesc->ScriptType == nullptr)
		return;

	// AngelscriptPreprocessor added a global UScriptStruct variable in script
	// that it expects us to fill in with the struct we generate
	asCModule* ScriptModule = (asCModule*)ClassDesc->ScriptType->GetModule();

	asSNameSpace* ScriptNamespace = nullptr;
	if (ClassDesc->Namespace.IsSet())
		ScriptNamespace = ScriptModule->engine->FindNameSpace(TCHAR_TO_ANSI(*ClassDesc->Namespace.GetValue()));
	else
		ScriptNamespace = ScriptModule->defaultNamespace;

	asCGlobalProperty* Property = ScriptModule->scriptGlobals.FindFirst(
		TCHAR_TO_ANSI(*ClassDesc->StaticClassGlobalVariableName),
		ScriptNamespace
	);
	if (Property == nullptr)
	{
		ensure(false);
		return;
	}

	void* VarAddr = Property->GetAddressOfValue();
	**(UScriptStruct***)VarAddr = Struct;
}

void FAngelscriptClassGenerator::CleanupRemovedClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc)
{
	UASClass* Class = (UASClass*)ClassDesc->Class;
	if (Class != nullptr)
	{
                                                                                                           
        Class->RetainRetiredScriptTypeForGc();
#if !SOLOAS_LEGACY_TERMINAL_LIFECYCLE
        if (Class->IsRooted())
            Class->RemoveFromRoot();
        Class->ClearFlags(RF_Standalone);
#endif
                                                                                                         
		Class->ScriptTypePtr = nullptr;
		Class->ConstructFunction = nullptr;
		Class->DefaultsFunction = nullptr;

		// Classes that no longer exist should be removed from placement and menus
		Class->ClassFlags |= CLASS_NotPlaceable;
		Class->ClassFlags |= CLASS_HideDropDown;
		Class->ClassFlags |= CLASS_Hidden;

                                                                                                                                    
#if 0
                                                                                                                                  
		for (auto& Elem : Class->GetFunctionMap())
		{
			UASFunction* Function = Cast<UASFunction>(Elem.Value);
			if (Function == nullptr)
				continue;

			Function->ScriptFunction = nullptr;
		}
                                                                                                                                 
#endif
		for (TFieldIterator<UFunction> It(
			Class,
			EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (UASFunction* Function = Cast<UASFunction>(*It))
				Function->ScriptFunction = nullptr;
		}
                                                                                                                               
	}

	UASStruct* Struct = (UASStruct*)ClassDesc->Struct;
	if (Struct != nullptr)
	{
		Struct->ScriptType = nullptr;
		Struct->UpdateScriptType();
	}
}

void FAngelscriptClassGenerator::FinalizeClass(FModuleData& ModuleData, FClassData& ClassData)
{
	auto ClassDesc = ClassData.NewClass;
	UClass* NewClass = ClassDesc->Class;
	if (NewClass == nullptr)
		return;
	if (ClassData.bFinalized)
		return;

	ClassData.bFinalized = true;
	NewClass->SetUpRuntimeReplicationData();

	// Check if we have anything marked composable-wise
	if (!ClassDesc->ComposeOntoClass.IsEmpty())
	{
		auto ComposeOntoClassDesc = GetClassDesc(ClassDesc->ComposeOntoClass);
		if (ComposeOntoClassDesc.IsValid())
			((UASClass*)NewClass)->ComposeOntoClass = ComposeOntoClassDesc->Class;
	}

	// Actors and components can have some special stuff added to them
	if (NewClass->IsChildOf(AActor::StaticClass()))
		FinalizeActorClass(ModuleData, ClassDesc);
	else if (NewClass->IsChildOf(UActorComponent::StaticClass()))
		FinalizeComponentClass(ClassDesc);
	else
		FinalizeObjectClass(ClassDesc);

                                                                                                                        
	UASClass* Candidate = CastChecked<UASClass>(NewClass);
	if (bPreparingModuleTransaction)
	{
		if (!Candidate->HasValidScriptClassCarrier()
			|| !SoloASBridge::PrepareScriptClassRegistration(Candidate, UASClass::StaticClass()))
			UE_LOG(Angelscript, Fatal, TEXT("ASR-CLASS-CARRIER-REGISTRATION-FAILED: %s"), *Candidate->GetPathName());
	}
	else PublishClassRegistration(Candidate);
}

void FAngelscriptClassGenerator::PublishClassRegistration(UASClass* NewClass)
{
                                                                                                                      
                                                                                                          
	UASClass* ScriptClass = CastChecked<UASClass>(NewClass);
	check(ScriptClass->HasValidScriptClassCarrier());
	const bool bRegistered = SoloASBridge::RegisterScriptClass(ScriptClass, UASClass::StaticClass());
	if (!bRegistered)
	{
		UE_LOG(Angelscript, Fatal, TEXT("ASR-CLASS-CARRIER-REGISTRATION-FAILED: %s"), *NewClass->GetPathName());
	}
                                                                                                        
                                                                                                    
	// Stock classifies ASClass imports as objects. Preserve the user's ClassRedirect
	// and mirror only aliases that resolve to this actual script class before loading resumes.
	const FCoreRedirectObjectName CurrentClassName(NewClass->GetPathName());
	TArray<FCoreRedirectObjectName> PreviousClassNames;
	FCoreRedirects::FindPreviousNames(ECoreRedirectFlags::Type_Class, CurrentClassName, PreviousClassNames);
	TArray<FCoreRedirect> ClassObjectAliases;
	for (const FCoreRedirectObjectName& PreviousName : PreviousClassNames)
	{
		if (FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Class, PreviousName) != CurrentClassName
			|| FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Object, PreviousName) != PreviousName)
			continue;
		ClassObjectAliases.Emplace(ECoreRedirectFlags::Type_Object, PreviousName.ToString(), CurrentClassName.ToString());
	}
	if (!ClassObjectAliases.IsEmpty()
		&& !FCoreRedirects::AddRedirectList(ClassObjectAliases, TEXT("Angelscript.ClassObjectImports")))
	{
		UE_LOG(Angelscript, Error, TEXT("Could not register script class import redirects for %s"), *NewClass->GetPathName());
	}
                                                                                                  
	// Tell the loading system the class exists
	NotifyRegistrationEvent(TEXT("/Script/Angelscript"), *NewClass->GetName(), ENotifyRegistrationType::NRT_Class,
		ENotifyRegistrationPhase::NRP_Finished, nullptr, false, NewClass);
}

void FAngelscriptClassGenerator::FinalizeActorClass(FModuleData& ModuleData, TSharedPtr<FAngelscriptClassDesc> ClassDesc)
{
	UASClass* ASClass = (UASClass*)ClassDesc->Class;
	check(ASClass->DefaultComponents.Num() == 0);

	// Actors use a special constructor
	ClassDesc->Class->ClassConstructor = &UASClass::StaticActorConstructor;

                                                                                                                   
	TSet<FName> ValidScriptAttachmentTargetNames;
	for (const TSharedRef<FAngelscriptPropertyDesc>& Property : ClassDesc->Properties)
	{
		if (Property->Meta.Contains(NAME_Actor_DefaultComponent))
			ValidScriptAttachmentTargetNames.Add(*Property->PropertyName);
	}

	for (UClass* ParentClass = ClassDesc->Class->GetSuperClass();
		ParentClass != nullptr;
		ParentClass = ParentClass->GetSuperClass())
	{
		if (UASClass* ParentASClass = Cast<UASClass>(ParentClass))
		{
			EnsureClassFinalized(ParentASClass);
			for (const auto& ParentComponent : ParentASClass->DefaultComponents)
				ValidScriptAttachmentTargetNames.Add(ParentComponent.ComponentName);
		}
	}
                                                                                                                 
	// Look for any DefaultComponent properties in the class so we
	// can record them and construct them on the fly.
	for (auto Property : ClassDesc->Properties)
	{
		if (Property->Meta.Contains(NAME_Actor_DefaultComponent))
		{
			UASClass::FDefaultComponent Comp;
			Comp.ComponentClass = Property->PropertyType.GetClass();
			Comp.ComponentName = *Property->PropertyName;
			Comp.VariableOffset = Property->ScriptPropertyOffset;
			Comp.bIsRoot = Property->Meta.Contains(NAME_Actor_RootComponent);
#if WITH_EDITOR
			Comp.bEditorOnly = Property->Meta.Contains(NAME_Meta_EditorOnly);
#else
			Comp.bEditorOnly = false;
			ensure(!Property->Meta.Contains(NAME_Meta_EditorOnly));
#endif

			FString* FoundAttach = Property->Meta.Find(NAME_Actor_Attach);
			if (FoundAttach != nullptr)
				Comp.Attach = **FoundAttach;
			else
				Comp.Attach = NAME_None;

			FString* FoundSocket = Property->Meta.Find(NAME_Actor_AttachSocket);
			if (FoundSocket != nullptr)
				Comp.AttachSocket = **FoundSocket;
			else
				Comp.AttachSocket = NAME_None;

			if (Comp.ComponentClass == nullptr || !Comp.ComponentClass->IsChildOf(UActorComponent::StaticClass()))
			{
				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, Property->LineNumber,
					FString::Printf(TEXT("Property %s::%s was marked as DefaultComponent, but is not a type of component."),
					*ClassDesc->ClassName, *Property->PropertyName));
				ModuleData.NewModule->bModuleSwapInError = true;
				continue;
			}

			if (Comp.ComponentClass->HasAnyClassFlags(CLASS_Abstract) && !ClassDesc->bAbstract)
			{
				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, Property->LineNumber,
					FString::Printf(TEXT("Property %s::%s was marked as DefaultComponent, but the component class is abstract and cannot be added."),
					*ClassDesc->ClassName, *Property->PropertyName));
				ModuleData.NewModule->bModuleSwapInError = true;
				continue;
			}

			if (Comp.Attach != NAME_None && !Comp.ComponentClass->IsChildOf(USceneComponent::StaticClass()))
			{
				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, Property->LineNumber,
					FString::Printf(TEXT("Property %s::%s has a component attach set, but is not a type of scene component."),
					*ClassDesc->ClassName, *Property->PropertyName));
				ModuleData.NewModule->bModuleSwapInError = true;
			}

			if (Comp.bIsRoot && !Comp.ComponentClass->IsChildOf(USceneComponent::StaticClass()))
			{
				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, Property->LineNumber,
					FString::Printf(TEXT("Property %s::%s has RootComponent set, but is not a type of scene component."),
					*ClassDesc->ClassName, *Property->PropertyName));
				ModuleData.NewModule->bModuleSwapInError = true;
			}

                                                                                                                       
			if (!IsValidExternalConstructionAttachment(
				ASClass, Comp, ValidScriptAttachmentTargetNames))
			{
				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, Property->LineNumber,
					FString::Printf(
						TEXT("Property %s::%s has an invalid construction-time attachment contract; "
							"roots cannot have a parent/socket, and attached components require a SceneComponent parent."),
						*ClassDesc->ClassName, *Property->PropertyName));
				ModuleData.NewModule->bModuleSwapInError = true;
				continue;
			}
                                                                                                                     
#if WITH_EDITOR
			// For root components, make sure no other component is marked as root component
			if (Comp.bIsRoot)
			{
				FString OtherRoot;
				auto* CheckClass = ASClass;
				while (CheckClass != nullptr && OtherRoot.IsEmpty())
				{
					for (auto& OtherComponent : CheckClass->DefaultComponents)
					{
						if (OtherComponent.bIsRoot)
						{
							OtherRoot = OtherComponent.ComponentName.ToString();
							break;
						}
					}

					CheckClass = Cast<UASClass>(CheckClass->GetSuperClass());
					if (CheckClass != nullptr)
						EnsureClassFinalized(CheckClass);
				}

				if (OtherRoot.IsEmpty())
				{
					UActorComponent* CppRootComp = ClassDesc->CodeSuperClass->GetDefaultObject<AActor>()->GetRootComponent();
					if (CppRootComp != nullptr)
						OtherRoot = CppRootComp->GetName();
				}

				if (!OtherRoot.IsEmpty())
				{
					FAngelscriptManager::Get().ScriptCompileError(
						ModuleData.NewModule, Property->LineNumber,
						FString::Printf(TEXT("Property %s is RootComponent, but the actor already has root component %s."),
						*Property->PropertyName, *OtherRoot));
					ModuleData.NewModule->bModuleSwapInError = true;
				}
			}
#endif

			ASClass->DefaultComponents.Add(Comp);
			ASClass->ClassFlags |= CLASS_HasInstancedReference;
		}
		else if (Property->Meta.Contains(NAME_Actor_OverrideComponent))
		{
			UASClass::FOverrideComponent Comp;
			Comp.ComponentClass = Property->PropertyType.GetClass();
			Comp.OverrideComponentName = *Property->Meta[NAME_Actor_OverrideComponent];
			Comp.VariableName = *Property->PropertyName;
			Comp.VariableOffset = Property->ScriptPropertyOffset;

			if (Comp.ComponentClass == nullptr || !Comp.ComponentClass->IsChildOf(UActorComponent::StaticClass()))
			{
				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, Property->LineNumber,
					FString::Printf(TEXT("Property %s::%s was marked as OverrideComponent, but is not a type of component."),
					*ClassDesc->ClassName, *Property->PropertyName));
				ModuleData.NewModule->bModuleSwapInError = true;
				continue;
			}

			if (Comp.ComponentClass->HasAnyClassFlags(CLASS_Abstract) && !ClassDesc->bAbstract)
			{
				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, Property->LineNumber,
					FString::Printf(TEXT("Property %s::%s was marked as OverrideComponent, but the component class is abstract and cannot be used."),
					*ClassDesc->ClassName, *Property->PropertyName));
				ModuleData.NewModule->bModuleSwapInError = true;
				continue;
			}

#if WITH_EDITOR
			UClass* ClassOfOverrideComponent = nullptr;
			auto* ParentClass = ClassDesc->Class->GetSuperClass();
			while (ParentClass != nullptr)
			{
				// Check if this component lives in a parent script class
				auto* ParentASClass = Cast<UASClass>(ParentClass);
				if (ParentASClass != nullptr)
				{
					EnsureClassFinalized(ParentASClass);

					for (const auto& DefComp : ParentASClass->DefaultComponents)
					{
						if (DefComp.ComponentName == Comp.OverrideComponentName)
						{
							ClassOfOverrideComponent = DefComp.ComponentClass;
							break;
						}
					}

					if (ClassOfOverrideComponent != nullptr)
						break;
					ParentClass = ParentClass->GetSuperClass();
					continue;
				}

				// Check if this component lives in a parent C++ class
				auto* CppCDO = Cast<AActor>(ParentClass->GetDefaultObject());
				if (CppCDO != nullptr)
				{
					for (auto* ParentComponent : CppCDO->GetComponents())
					{
						if (ParentComponent != nullptr && ParentComponent->GetFName() == Comp.OverrideComponentName)
						{
							ClassOfOverrideComponent = ParentComponent->GetClass();
							break;
						}
					}

					if (ClassOfOverrideComponent == nullptr)
					{
						// if the parent component is an abstract class it won't show up in GetComponents, 
						// so iterate over all the fields and use the property name.
						for (TFieldIterator<FProperty> It(ParentClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
						{
							FProperty* Prop = *It;
							FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Prop);

							if (ObjectProperty && ObjectProperty->HasAnyPropertyFlags(CPF_InstancedReference))
							{
								if (ObjectProperty != nullptr && ObjectProperty->GetFName() == Comp.OverrideComponentName)
								{
									if (ObjectProperty->PropertyClass != nullptr && ObjectProperty->PropertyClass->HasAllClassFlags(CLASS_Abstract))
									{
										ClassOfOverrideComponent = ObjectProperty->PropertyClass;
										break;
									}
								}
							}
						}
					}

					if (ClassOfOverrideComponent != nullptr)
						break;
				}
				ParentClass = ParentClass->GetSuperClass();
			}

			if (ClassOfOverrideComponent == nullptr)
			{
				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, Property->LineNumber,
					FString::Printf(TEXT("OverrideComponent %s::%s could not find component %s in base class to override."),
					*ClassDesc->ClassName, *Property->PropertyName, *Comp.OverrideComponentName.ToString()));
				ModuleData.NewModule->bModuleSwapInError = true;
				continue;
			}
                                                                                                                                  
#if 0
                                                                                                                                
			else if (!Comp.ComponentClass->IsChildOf(ClassOfOverrideComponent->GetMostUpToDateClass()))
                                                                                                                               
#endif
			else if (!Comp.ComponentClass->IsChildOf(ClassOfOverrideComponent->GetAuthoritativeClass()))
                                                                                                                             
			{
				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, Property->LineNumber,
					FString::Printf(TEXT("OverrideComponent %s::%s type does not inherit from the base class's %s"),
					*ClassDesc->ClassName, *Property->PropertyName, *ClassOfOverrideComponent->GetName()));
				ModuleData.NewModule->bModuleSwapInError = true;
				continue;
			}
#endif

			ASClass->OverrideComponents.Add(Comp);
			ASClass->ClassFlags |= CLASS_HasInstancedReference;
		}
		else if (Property->Meta.Contains(NAME_Actor_BindComponent))
		{
			UASClass::FBindComponent Comp;
			Comp.ComponentClass = Property->PropertyType.GetClass();
			Comp.ComponentName = *Property->PropertyName;
			Comp.VariableName = *Property->GetUnrealPropertyName();
			Comp.VariableOffset = Property->ScriptPropertyOffset;

			if (Comp.ComponentClass == nullptr || !Comp.ComponentClass->IsChildOf(UActorComponent::StaticClass()))
			{
				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, Property->LineNumber,
					FString::Printf(TEXT("Property %s::%s was marked as BindComponent, but is not a type of component."),
					*ClassDesc->ClassName, *Property->PropertyName));
				ModuleData.NewModule->bModuleSwapInError = true;
				continue;
			}

			ASClass->BindComponents.Add(Comp);
			ASClass->ClassFlags |= CLASS_HasInstancedReference;
		}
	}
}

static const FName NAME_Component_Spawnable("BlueprintSpawnableComponent");
void FAngelscriptClassGenerator::FinalizeComponentClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc)
{
	// Components use a special constructor
	ClassDesc->Class->ClassConstructor = &UASClass::StaticComponentConstructor;
	
#if WITH_EDITOR
	// All components are blueprint spawnable
	if(!ClassDesc->bPlaceable.IsSet() || ClassDesc->bPlaceable.GetValue())
		ClassDesc->Class->SetMetaData(NAME_Component_Spawnable, TEXT(""));
#endif
}

void FAngelscriptClassGenerator::FinalizeObjectClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc)
{
	// Objects use a generic static constructor
	ClassDesc->Class->ClassConstructor = &UASClass::StaticObjectConstructor;
}

void FAngelscriptClassGenerator::VerifyClass(FModuleData& ModuleData, TSharedPtr<FAngelscriptClassDesc> ClassDesc)
{
#if WITH_EDITOR
	auto* ASClass = (UASClass*)ClassDesc->Class;
	if (ASClass == nullptr)
		return;

	if (ASClass->IsChildOf(AActor::StaticClass()))
	{
		// if this class isn't abstract, verify that we've provided overrides for any components that are abstract in any abstract super classes
		if (!ClassDesc->bAbstract)
		{
			// We'll need to collect all AS override components to check against as we go up the class chain
			TArray<UASClass::FOverrideComponent> OverrideComponentsInHierarchy;
			OverrideComponentsInHierarchy.Append(ASClass->OverrideComponents);

			auto* ParentClass = ClassDesc->Class->GetSuperClass();
			while (ParentClass != nullptr)
			{
				// if we've hit a non-abstract parent class, we can assume that all abstract component classes have been dealt with.
				if (!ParentClass->HasAnyClassFlags(CLASS_Abstract))
				{
					break;
				}

				auto* ParentASClass = Cast<UASClass>(ParentClass);
				if (ParentASClass)
				{
					EnsureClassFinalized(ParentASClass);
				}

				for (TFieldIterator<FProperty> It(ParentClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				{
					FProperty* Property = *It;
					FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);

					if (ObjectProperty && ObjectProperty->HasAnyPropertyFlags(CPF_InstancedReference))
					{
						UClass* PropertyClass = ObjectProperty->PropertyClass;

						if (PropertyClass && PropertyClass->HasAnyClassFlags(CLASS_Abstract))
						{
							bool bFoundOverride = false;

							for (UASClass::FOverrideComponent& OverrideComponent : OverrideComponentsInHierarchy)
							{
								if (Property->GetFName() == OverrideComponent.OverrideComponentName)
								{
									bFoundOverride = true;
									break;
								}
							}

							if (!bFoundOverride)
							{
								FAngelscriptManager::Get().ScriptCompileError(
									ModuleData.NewModule, ClassDesc->LineNumber,
									FString::Printf(TEXT("OverrideComponent for %s::%s missing that's specified in base class %s. Component override is needed because component class %s is Abstract."),
										*ClassDesc->ClassName, *Property->GetName(), *ParentClass->GetName(), *PropertyClass->GetName()));
								ModuleData.NewModule->bModuleSwapInError = true;
							}
						}
					}
				}

				if (ParentASClass)
				{
					// add the override components from the parent AS class to the list for us to query as we go up the chain. 
					OverrideComponentsInHierarchy.Append(ParentASClass->OverrideComponents);
				}

				ParentClass = ParentClass->GetSuperClass();
			}
		}

		// Verify that the specified attachments exist for each component that is being attached
		AActor* ActorDefaultObject = CastChecked<AActor>(ASClass->GetDefaultObject(false));
		check(ActorDefaultObject != nullptr);

		for (auto& DefComp : ASClass->DefaultComponents)
		{
			if (DefComp.Attach != NAME_None)
			{
				FObjectPropertyBase* ParentComponentProperty = nullptr;
				UActorComponent* ParentComponent = nullptr;

				UActorComponent* ChildComponent = *(UActorComponent**)((SIZE_T)ActorDefaultObject + DefComp.VariableOffset);
				if (ChildComponent == nullptr)
					continue;

				for (TFieldIterator<FProperty> It(ASClass); It; ++It)
				{
					FObjectPropertyBase* Property = CastField<FObjectPropertyBase>(*It);
					if (Property == nullptr)
						continue;
					if (!Property->HasAnyPropertyFlags(CPF_InstancedReference))
						continue;

					UActorComponent* Component = Cast<UActorComponent>(Property->GetObjectPropertyValue_InContainer(ActorDefaultObject));
                                                                                                                                      
#if 0
                                                                                                                                    
					if ((Property->AngelscriptPropertyFlags & APF_RuntimeGenerated) != 0)
                                                                                                                               
#endif
					if (IsAngelscriptRuntimeGeneratedProperty(Property))
                                                                                                                             
					{
						if (Property->GetFName() == DefComp.Attach)
						{
							ParentComponentProperty = Property;
							ParentComponent = Component;
							break;
						}
					}
					else
					{
						if (Component != nullptr && Component->GetFName() == DefComp.Attach)
						{
							ParentComponentProperty = Property;
							ParentComponent = Component;
							break;
						}
					}
				}

				if (ParentComponentProperty != nullptr && ParentComponent != nullptr)
				{
					if (!ParentComponent->IsA<USceneComponent>())
					{
						int LineNumber = ClassDesc->LineNumber;
						auto PropDesc = ClassDesc->GetProperty(DefComp.ComponentName.ToString());
						if (PropDesc.IsValid())
							LineNumber = PropDesc->LineNumber;

						FAngelscriptManager::Get().ScriptCompileError(
							ModuleData.NewModule, LineNumber,
							FString::Printf(TEXT("Attach parent %s is not a SceneComponent for DefaultComponent %s."),
								*DefComp.Attach.ToString(), *DefComp.ComponentName.ToString()), true);
						ModuleData.NewModule->bModuleSwapInError = true;
					}
					else if (ParentComponentProperty->HasAnyPropertyFlags(CPF_EditorOnly) || ParentComponent->bIsEditorOnly)
					{
						auto* ChildComponentProperty = ASClass->FindPropertyByName(DefComp.ComponentName);
						if (ChildComponentProperty != nullptr && !ChildComponentProperty->HasAnyPropertyFlags(CPF_EditorOnly)
							&& !ChildComponent->bIsEditorOnly)
						{
							bool bActorIsEditorOnly = ActorDefaultObject->bIsEditorOnlyActor || ASClass->IsDeveloperOnly();
							if (!bActorIsEditorOnly)
							{
								int LineNumber = ClassDesc->LineNumber;
								auto PropDesc = ClassDesc->GetProperty(DefComp.ComponentName.ToString());
								if (PropDesc.IsValid())
									LineNumber = PropDesc->LineNumber;

								FAngelscriptManager::Get().ScriptCompileError(
									ModuleData.NewModule, LineNumber,
									FString::Printf(TEXT("Non-Editor DefaultComponent %s cannot be attached to Editor-Only attach parent %s."),
										*DefComp.ComponentName.ToString(), *DefComp.Attach.ToString()), true);
								ModuleData.NewModule->bModuleSwapInError = true;
							}
						}
					}
				}
				else
				{
					int LineNumber = ClassDesc->LineNumber;
					auto PropDesc = ClassDesc->GetProperty(DefComp.ComponentName.ToString());
					if (PropDesc.IsValid())
						LineNumber = PropDesc->LineNumber;

					FString SuggestionText;
					if (FObjectProperty* Prop = CastField<FObjectProperty>(ASClass->FindPropertyByName(DefComp.Attach)))
					{
						UObject* CDOObject = Prop->GetObjectPropertyValue_InContainer(ActorDefaultObject);
						if (CDOObject != nullptr && CDOObject->IsA<USceneComponent>())
							SuggestionText = FString::Printf(TEXT("\n\nDid you mean the component named: %s"), *CDOObject->GetName());
					}

					FAngelscriptManager::Get().ScriptCompileError(
						ModuleData.NewModule, LineNumber,
						FString::Printf(TEXT("Attach parent %s does not exist for DefaultComponent %s.%s"),
							*DefComp.Attach.ToString(), *DefComp.ComponentName.ToString(), *SuggestionText), true);
					ModuleData.NewModule->bModuleSwapInError = true;
				}
			}
			else if (DefComp.bIsRoot)
			{
				auto* ChildComponentProperty = ASClass->FindPropertyByName(DefComp.ComponentName);
				UActorComponent* ChildComponent = *(UActorComponent**)((SIZE_T)ActorDefaultObject + DefComp.VariableOffset);

				if (ChildComponentProperty != nullptr && ChildComponent != nullptr && (ChildComponentProperty->HasAnyPropertyFlags(CPF_EditorOnly) || ChildComponent->bIsEditorOnly))
				{
					bool bActorIsEditorOnly = ActorDefaultObject->bIsEditorOnlyActor || ASClass->IsDeveloperOnly();
					if (!bActorIsEditorOnly)
					{
						int LineNumber = ClassDesc->LineNumber;
						auto PropDesc = ClassDesc->GetProperty(DefComp.ComponentName.ToString());
						if (PropDesc.IsValid())
							LineNumber = PropDesc->LineNumber;

						FAngelscriptManager::Get().ScriptCompileError(
							ModuleData.NewModule, LineNumber,
							FString::Printf(TEXT("Editor-Only DefaultComponent %s cannot be the RootComponent of non-editor actor %s."),
								*DefComp.ComponentName.ToString(), *ClassDesc->ClassName), true);
						ModuleData.NewModule->bModuleSwapInError = true;
					}
				}
			}

			// Ensure that the component isn't marked so it can't be added in angelscript
			if (DefComp.ComponentClass != nullptr && DefComp.ComponentClass->HasMetaData(CLASSMETA_NotAngelscriptSpawnable))
			{
				int LineNumber = ClassDesc->LineNumber;
				auto PropDesc = ClassDesc->GetProperty(DefComp.ComponentName.ToString());
				if (PropDesc.IsValid())
					LineNumber = PropDesc->LineNumber;

				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, LineNumber,
					FString::Printf(TEXT("Component type %s has NotAngelscriptSpawnable meta tag and cannot be a DefaultComponent"),
						*DefComp.ComponentClass->GetName()), true);
				ModuleData.NewModule->bModuleSwapInError = true;
			}

			// Show a warning if the component type is marked as deprecated
			if (DefComp.ComponentClass != nullptr && DefComp.ComponentClass->HasAnyClassFlags(CLASS_Deprecated))
			{
				int LineNumber = ClassDesc->LineNumber;
				auto PropDesc = ClassDesc->GetProperty(DefComp.ComponentName.ToString());
				if (PropDesc.IsValid())
					LineNumber = PropDesc->LineNumber;

				FAngelscriptManager::Get().ScriptCompileError(
					ModuleData.NewModule, LineNumber,
					FString::Printf(TEXT("%s is deprecated"),
						*DefComp.ComponentClass->GetName()), false);
			}
		}
	}
#endif
}

void FAngelscriptClassGenerator::InitClassTickSettings(FClassData& ClassData)
{
	auto& ClassDesc = ClassData.NewClass;
	UASClass* NewClass = (UASClass*)ClassDesc->Class;
	if (NewClass == nullptr)
		return;

	if (ClassData.bHasEvalTick)
	{
		return;
	}

	if (!NewClass->IsChildOf(AActor::StaticClass()) && !NewClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return;
	}

	UClass* ParentClass = NewClass->GetSuperClass();

	bool bCanEverTick = false;
	bool bStartWithTickEnabled = false;

	if (UASClass* ParentASClass = Cast<UASClass>(ParentClass))
	{
		FModuleData* ParentModuleData;
		FClassData* ParentClassData;
		FDelegateData* ParentDelegateData;
		if (GetDataFor((asITypeInfo*)ParentASClass->ScriptTypePtr, ParentModuleData, ParentClassData, ParentDelegateData))
		{
			check(ParentClassData);

			if (!ParentClassData->bHasEvalTick)
			{
				// Make sure to update parents before this
				InitClassTickSettings(*ParentClassData);
			}
		}

		bCanEverTick = ParentASClass->bCanEverTick;
		bStartWithTickEnabled = ParentASClass->bStartWithTickEnabled;
	}
	else // Parent must be C++ class
	{
		FTickFunction* ParentTickFunction;
		if (NewClass->IsChildOf(AActor::StaticClass()))
		{
			ParentTickFunction = &ParentClass->GetDefaultObject<AActor>()->PrimaryActorTick;
		}
		else
		{
			check(NewClass->IsChildOf(UActorComponent::StaticClass()));
			ParentTickFunction = &ParentClass->GetDefaultObject<UActorComponent>()->PrimaryComponentTick;
		}

		bCanEverTick = ParentTickFunction->bCanEverTick;
		bStartWithTickEnabled = ParentTickFunction->bStartWithTickEnabled;
	}

	if (!bCanEverTick)
	{
		// If the class has a ReceiveTick or a Tick function, it can tick
		auto TickDesc = ClassDesc->GetMethod(TEXT("Tick"));
		if (!TickDesc.IsValid())
			TickDesc = ClassDesc->GetMethod(TEXT("ReceiveTick"));

		if (TickDesc.IsValid() && TickDesc->ScriptFunction != nullptr && (!TickDesc->bIsNoOp || (GIsEditor && !IsRunningCommandlet())))
		{
			bCanEverTick = true;
			bStartWithTickEnabled = true;
		}
	}

	NewClass->bCanEverTick = bCanEverTick;
	NewClass->bStartWithTickEnabled = bStartWithTickEnabled;

	ClassData.bHasEvalTick = true;
}

void FAngelscriptClassGenerator::CallPostInitFunctions()
{
	// Ensure that all literal assets have been created now that we can
	for (auto& ModuleData : Modules)
	{
		if (ModuleData.NewModule->ScriptModule == nullptr)
			continue;

		for (const FString& InitFunctionName : ModuleData.NewModule->PostInitFunctions)
		{
			auto AnsiFunctionName = StringCast<ANSICHAR>(*InitFunctionName);
			bool bFound = false;
			for (int i = 0, Count = ModuleData.NewModule->ScriptModule->globalFunctionList.GetLength(); i < Count; ++i)
			{
				asCScriptFunction* ScriptFunction = ModuleData.NewModule->ScriptModule->globalFunctionList[i];
				if (ScriptFunction->name != AnsiFunctionName.Get())
					continue;

				FAngelscriptContext Context;
				Context->Prepare(ScriptFunction);
				Context->Execute();
				bFound = true;
				break;
			}
		}
	}
}

void FAngelscriptClassGenerator::InitDefaultObjects()
{
	// First do a prepass in which we figure out what the tick settings should be on each ASClass.
	// This is needed to be done before we create CDOs based on AS classes, as otherwise BP CDOs can trigger CDO creation for other classes
	//		, in which the tick settings haven't been figure out yet.
	// Also need to make sure we figure it out in the correct order (class parents need to be figured out first), as class children needs to
	//		enable tick if its parent has it enabled.
	{
		for (auto& ModuleData : Modules)
		{
			for (auto& ClassData : ModuleData.Classes)
			{
				if (!ClassData.NewClass->bIsStruct)
				{
					if (ShouldFullReload(ClassData))
					{
						InitClassTickSettings(ClassData);
					}
				}
			}
		}
	}

	// Initialize default objects for all classes after reload
	for (auto& ModuleData : Modules)
	{
		for (auto& ClassData : ModuleData.Classes)
		{
			if (!ClassData.NewClass->bIsStruct)
			{
				if (ShouldFullReload(ClassData))
				{
					InitDefaultObject(ModuleData, ClassData);
				}
			}
		}
	}
}

void FAngelscriptClassGenerator::InitDefaultObject(FModuleData& ModuleData, FClassData& ClassData)
{
	auto ClassDesc = ClassData.NewClass;
	UASClass* NewClass = (UASClass*)ClassDesc->Class;
	if (NewClass == nullptr)
		return;

	NewClass->GetDefaultObject(true);
}

void FAngelscriptClassGenerator::GetFullReloadLines(TSharedRef<FAngelscriptModuleDesc> Module, TArray<int32>& OutLines)
{
	for (auto& ModuleData : Modules)
	{
		if (ModuleData.NewModule == Module)
			OutLines.Append(ModuleData.ReloadReqLines);
	}
}

bool FAngelscriptClassGenerator::WantsFullReload(TSharedRef<FAngelscriptModuleDesc> Module)
{
	for (auto& ModuleData : Modules)
	{
		if (ModuleData.NewModule == Module)
		{
			return ModuleData.ReloadReq >= EReloadRequirement::FullReloadSuggested;
		}
	}
	return false;
}

bool FAngelscriptClassGenerator::NeedsFullReload(TSharedRef<FAngelscriptModuleDesc> Module)
{
	for (auto& ModuleData : Modules)
	{
		if (ModuleData.NewModule == Module)
		{
			return ModuleData.ReloadReq >= EReloadRequirement::FullReloadRequired;
		}
	}
	return false;
}

void FAngelscriptClassGenerator::UpdateConstructAndDefaultsFunctions(TSharedPtr<FAngelscriptClassDesc> ClassDesc, UASClass* Class)
{
	asCObjectType* ObjType = (asCObjectType*)Class->ScriptTypePtr;
	if (ObjType != nullptr)
	{
		Class->ConstructFunction = ObjType->GetEngine()->GetFunctionById(ObjType->beh.construct);
		
		// Only take the defaults function if it was overridden by our class, otherwise we're going to call the parent manually anyway
		auto* DefaultsFunction = (asCScriptFunction*)ObjType->GetMethodByDecl("void __InitDefaults()");
		if (DefaultsFunction != nullptr && DefaultsFunction->objectType == ObjType)
			Class->DefaultsFunction = DefaultsFunction;

		((asCScriptFunction*)Class->ConstructFunction)->isInUse = true;
		if (Class->DefaultsFunction != nullptr)
			((asCScriptFunction*)Class->DefaultsFunction)->isInUse = true;
	}
	else
	{
		Class->ConstructFunction = nullptr;
		Class->DefaultsFunction = nullptr;
	}
}

asITypeInfo* FAngelscriptClassGenerator::GetNamespacedTypeInfoForClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc, TSharedPtr<FAngelscriptModuleDesc> ModuleDesc) const
{
	asCScriptEngine* Engine = (asCScriptEngine*) FAngelscriptManager::Get().Engine;
	asCModule* Module = (asCModule*) ModuleDesc->ScriptModule;

	check(Engine != nullptr);
	check(Module != nullptr);

	asSNameSpace* NameSpace = nullptr;
	if (ClassDesc->Namespace.IsSet())
	{
		NameSpace = Engine->FindNameSpace(TCHAR_TO_ANSI(*ClassDesc->Namespace.GetValue()));
	}

	// Default to the modules default namespace if we couldn't find the overridden namespace.
	if (NameSpace == nullptr)
	{
		NameSpace = Module->defaultNamespace;
	}

	check(NameSpace != nullptr);

	return Module->GetType(TCHAR_TO_ANSI(*ClassDesc->ClassName), NameSpace);
}
                                                                                                                        
bool FAngelscriptClassGenerator::ValidatePayloadReloads(FString& Error) const
{
    TArray<FAngelscriptPayloadReloadType> Types;
    bool bHasReplacedStruct = false;
    for (const FModuleData& Module : Modules)
    {
        for (const FClassData& Class : Module.Classes)
        {
            if (!Class.NewClass.IsValid()) continue;
            FAngelscriptPayloadReloadType& Type = Types.AddDefaulted_GetRef();
            Type.Candidate = Class.NewClass;
            if (Class.OldClass.IsValid() && Class.OldClass->bIsStruct)
                Type.Previous = Cast<UScriptStruct>(Class.OldClass->Struct);
            if (Class.OldClass.IsValid() && !Class.OldClass->bIsStruct)
                Type.PreviousObjectClass = Class.OldClass->Class;
            Type.bWillReplace = Type.Previous && Class.NewClass->bIsStruct
                && (Class.ReloadReq >= EReloadRequirement::FullReloadSuggested || Class.bExplicitFullReloadTarget);
            Type.bWillReplaceObjectClass = Type.PreviousObjectClass && !Class.NewClass->bIsStruct
                && (Class.ReloadReq >= EReloadRequirement::FullReloadSuggested || Class.bExplicitFullReloadTarget);
            bHasReplacedStruct |= Type.bWillReplace;
        }
    }
    if (!bHasReplacedStruct) return true;
    auto* Prepared = AngelscriptReloadSafety::GetPreparedPayloadReloadScope();
    if (!Prepared) { Error = TEXT("Payload reload has no prepared transaction owner"); return false; }
    Prepared->Plans.Reset();
    for (const FModuleData& Module : Modules)
        for (const FClassData& Class : Module.Classes)
        {
            if (!Class.OldClass.IsValid() || !Class.NewClass.IsValid() || Class.NewClass->bIsStruct
                || !Class.OldClass->Class || !Class.OldClass->ScriptType) continue;
            const bool bReplace = Class.ReloadReq >= EReloadRequirement::FullReloadSuggested || Class.bExplicitFullReloadTarget;
            Prepared->Plans.Add({Class.OldClass->Class, FObjectKey(Class.OldClass->Class), Class.OldClass->ScriptType,
                Class.NewClass->ScriptType, bReplace ? AngelscriptRuntimeGcMirror::EScriptObjectReloadPlan::RequiresReplacement
                    : AngelscriptRuntimeGcMirror::EScriptObjectReloadPlan::SoftReconstruction});
        }
    for (TObjectIterator<UASClass> It; It; ++It)
    {
        UASClass* Class = *It;
        if (!Class->ScriptTypePtr || Class->NewerVersion || Class->HasAnyClassFlags(CLASS_NewerVersionExists)
            || Prepared->Plans.ContainsByPredicate([Class](const auto& Plan) { return Plan.Class == Class; })) continue;
        if (Modules.ContainsByPredicate([Class](const FModuleData& Data)
            { return Data.OldModule.IsValid() && Data.OldModule->ScriptModule == Class->ScriptTypePtr->GetModule(); })) continue;
        const auto Module = FAngelscriptManager::Get().GetModule(Class->ScriptTypePtr->GetModule());
        if (!Module.IsValid()) continue;
        const bool bCurrent = Module->Classes.ContainsByPredicate([Class](const auto& Desc)
            { return Desc->Class == Class && Desc->ScriptType == Class->ScriptTypePtr; });
        if (bCurrent) Prepared->Plans.Add({Class, FObjectKey(Class), Class->ScriptTypePtr, Class->ScriptTypePtr,
            AngelscriptRuntimeGcMirror::EScriptObjectReloadPlan::Unchanged});
    }
    auto& Admission = AngelscriptReloadSafety::GetPayloadReloadAdmission();
    if (!Admission.IsBound())
    {
        Error = TEXT("The editor payload admission owner is unavailable");
        return false;
    }
    TArray<AngelscriptReloadSafety::FPreparedScriptEnumReloadPlan> EnumPlans;
    for (const FModuleData& Module : Modules)
        for (const FEnumData& Enum : Module.Enums)
        {
            auto& Plan = EnumPlans.AddDefaulted_GetRef();
            Plan.PreviousModule = Module.OldModule;
            Plan.CandidateModule = Module.NewModule;
            Plan.Previous = Enum.OldEnum;
            Plan.Candidate = Enum.NewEnum;
            Plan.PreviousCookie = Enum.OldEnum.IsValid() ? Enum.OldEnum->ScriptType : nullptr;
            Plan.CandidateCookie = Enum.NewEnum.IsValid() ? Enum.NewEnum->ScriptType : nullptr;
            Plan.bOwnerRecompiled = true;
            Plan.bEligibleFixedDefinition = Enum.OldEnum.IsValid() && Enum.NewEnum.IsValid()
                && Enum.OldEnum->Enum && Enum.NewEnum->Enum == Enum.OldEnum->Enum && !Enum.bNeedReload
                && !Enum.OldEnum->bPendingFullReload
                && Enum.OldEnum->ValueNames == Enum.NewEnum->ValueNames
                && Enum.OldEnum->EnumValues == Enum.NewEnum->EnumValues
                && Enum.OldEnum->Documentation == Enum.NewEnum->Documentation
                && Enum.OldEnum->Meta.OrderIndependentCompareEqual(Enum.NewEnum->Meta);
        }
    for (const auto& Module : FAngelscriptManager::Get().GetActiveModules())
    {
        if (Modules.ContainsByPredicate([&](const FModuleData& Data) { return Data.OldModule == Module; })) continue;
        for (const auto& Enum : Module->Enums)
        {
            auto& Plan = EnumPlans.AddDefaulted_GetRef();
            Plan.PreviousModule = Module;
            Plan.CandidateModule = Module;
            Plan.Previous = Enum;
            Plan.Candidate = Enum;
            Plan.PreviousCookie = Enum->ScriptType;
            Plan.CandidateCookie = Enum->ScriptType;
            Plan.bEligibleFixedDefinition = Enum->Enum && !Enum->bPendingFullReload;
        }
    }
    return AngelscriptReloadSafety::ExecutePayloadAdmissionWithScriptEnums(Types, EnumPlans, Error);
}
                                                                                                                      
