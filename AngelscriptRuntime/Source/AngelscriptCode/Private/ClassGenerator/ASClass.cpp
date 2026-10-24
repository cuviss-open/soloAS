#include "ClassGenerator/ASClass.h"
                                                                                                                              
#include "Runtime/Launch/Resources/Version.h"
                                                                                                                            

                                                                                                              
#include "Shared/SoloASScriptBlueprint.h"
#include "ClassGenerator/AngelscriptReplicationShadow.h"
#include "Serialization/AsyncLoadingEvents.h"
#include "UObject/SparseDelegate.h"
#include <atomic>
                                                                                                            
                                                                                            
#include "ClassGenerator/AngelscriptRuntimeGcMirror.h"
#include "AngelscriptRuntimeGcLifecycle.h"
#include "AngelscriptExecutionQuiescence.h"
#include "ClassGenerator/AngelscriptFieldDestruction.h"
#include "ClassGenerator/AngelscriptRetiredModules.h"
#include "Runtime/SoloASBridgeCallableDispatch.h"
#include "AngelscriptReflectionAdapter.h"
#include "UObject/EnumProperty.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectHash.h"
                                                                                          
                                                                                                  
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/UnrealType.h"
                                                                                                
                                                                                                                             
#include "AngelscriptSettings.h"
#include "Engine/BlueprintGeneratedClass.h"
                                                                                                                           
#include "AngelscriptManager.h"

#include "Runtime/Launch/Resources/Version.h"
#include "UObject/Package.h"
#include "UObject/ScriptMacros.h"
#include "UObject/UObjectThreadContext.h"

#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"

#include "StartAngelscriptHeaders.h"
#include "as_config.h"
#include "as_scriptengine.h"
#include "as_scriptobject.h"
#include "as_objecttype.h"
#include "as_context.h"
#include "EndAngelscriptHeaders.h"

#ifdef _MSC_VER
#pragma warning(disable : 4191)
#endif

#if !WITH_ANGELSCRIPT_HAZE
#define AS_ENSURE ensureMsgf
#else
#define AS_ENSURE devEnsure
#endif

                                                                                                         
static_assert(STRUCT_OFFSET(UASClass, StockUberGraphFramePointerProperty)
	== STRUCT_OFFSET(UBlueprintGeneratedClass, UberGraphFramePointerProperty),
	"Stock BPGC ancestor collector requires a null frame-property slot in UASClass");
                                                                                                       

UObject* UASClass::OverrideConstructingObject = nullptr;

                                                                                                       
#if !UE_BUILD_SHIPPING && !UE_BUILD_TEST
static void TraceScriptConstructionProperty(UObject* Object, const TCHAR* Phase)
{
	FString PropertyName;
	if (Object == nullptr
		|| !FParse::Value(
			FCommandLine::Get(),
			TEXT("-SoloASConstructionTrace="),
			PropertyName)
		|| PropertyName.IsEmpty())
	{
		return;
	}

	FProperty* Property = Object->GetClass()->FindPropertyByName(
		FName(*PropertyName));
	if (Property == nullptr)
	{
		UE_LOG(
			Angelscript,
			Display,
			TEXT("SOLOAS_CONSTRUCTION_TRACE phase=%s object=%s class=%s property=%s missing=1"),
			Phase,
			*Object->GetPathName(),
			*Object->GetClass()->GetPathName(),
			*PropertyName);
		return;
	}

	FString Value;
	const bool bExported = Property->ExportText_InContainer(
		0,
		Value,
		Object,
		nullptr,
		Object,
		PPF_None);
	Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	const UStruct* Owner = Property->GetOwnerStruct();
	UE_LOG(
		Angelscript,
		Display,
		TEXT("SOLOAS_CONSTRUCTION_TRACE phase=%s object=%s class=%s property=%s owner=%s "
			"flags=0x%llx exported=%d value=%s"),
		Phase,
		*Object->GetPathName(),
		*Object->GetClass()->GetPathName(),
		*Property->GetName(),
		Owner != nullptr ? *Owner->GetPathName() : TEXT("<none>"),
		static_cast<unsigned long long>(Property->GetPropertyFlags()),
		bExported ? 1 : 0,
		*Value);
}
#else
#define TraceScriptConstructionProperty(Object, Phase) do { } while (false)
#endif
                                                                                                     
                                                                                                                         
struct FScriptStockPropertySnapshot
{
	FProperty* Property = nullptr;
	const UStruct* Owner = nullptr;
	const UClass* ObjectClass = nullptr;
	const UClass* DefaultsClass = nullptr;
	const FFieldClass* PropertyClass = nullptr;
	int32 Offset = INDEX_NONE;
	int32 Size = 0;
	int32 ElementSize = 0;
	uint64 PropertyFlags = 0;
	void* Value = nullptr;

	FScriptStockPropertySnapshot() = default;
	FScriptStockPropertySnapshot(const FScriptStockPropertySnapshot&) = delete;
	FScriptStockPropertySnapshot& operator=(
		const FScriptStockPropertySnapshot&) = delete;

	FScriptStockPropertySnapshot(FScriptStockPropertySnapshot&& Other) noexcept
		: Property(Other.Property)
		, Owner(Other.Owner)
		, ObjectClass(Other.ObjectClass)
		, DefaultsClass(Other.DefaultsClass)
		, PropertyClass(Other.PropertyClass)
		, Offset(Other.Offset)
		, Size(Other.Size)
		, ElementSize(Other.ElementSize)
		, PropertyFlags(Other.PropertyFlags)
		, Value(Other.Value)
	{
		Other.Property = nullptr;
		Other.Value = nullptr;
	}

	FScriptStockPropertySnapshot& operator=(
		FScriptStockPropertySnapshot&& Other) noexcept
	{
		if (this != &Other)
		{
			Reset();
			Property = Other.Property;
			Owner = Other.Owner;
			ObjectClass = Other.ObjectClass;
			DefaultsClass = Other.DefaultsClass;
			PropertyClass = Other.PropertyClass;
			Offset = Other.Offset;
			Size = Other.Size;
			ElementSize = Other.ElementSize;
			PropertyFlags = Other.PropertyFlags;
			Value = Other.Value;
			Other.Property = nullptr;
			Other.Value = nullptr;
		}
		return *this;
	}

	~FScriptStockPropertySnapshot()
	{
		Reset();
	}

	void Reset()
	{
		if (Property != nullptr && Value != nullptr)
			Property->DestroyAndFreeValue(Value);
		Property = nullptr;
		Value = nullptr;
	}
};

                                                                                                                       
                                                                                                                           
static bool IsSafeScriptStockProperty(const FProperty* Property)
{
	if (Property == nullptr)
		return false;

	if (Cast<UASClass>(Property->GetOwnerStruct()) == nullptr)
		return false;

	if (Property->GetSize() != Property->GetElementSize()
		|| Property->HasAnyPropertyFlags(
			CPF_Transient
			| CPF_DuplicateTransient
			| CPF_NonPIEDuplicateTransient
			| CPF_TextExportTransient
			| CPF_SkipSerialization
			| CPF_Virtual)
		|| Property->ContainsInstancedObjectProperty()
		|| Property->HasGetter()
		|| Property->HasSetter())
	{
		return false;
	}

	if (Property->IsA<FDelegateProperty>()
		|| Property->IsA<FMulticastDelegateProperty>())
	{
		return false;
	}

	TArray<const FStructProperty*> EncounteredStructProps;
	const EPropertyObjectReferenceType AllReferenceKinds =
		EPropertyObjectReferenceType::Strong
		| EPropertyObjectReferenceType::Weak
		| EPropertyObjectReferenceType::Soft
		| EPropertyObjectReferenceType::Conservative;
	return !Property->ContainsObjectReference(
		EncounteredStructProps,
		AllReferenceKinds);
}

static const UClass* GetScriptStockDefaultsClass(UObject* Object)
{
	if (Object == nullptr)
		return nullptr;

	const UClass* ObjectClass = Object->GetClass();
	if (Object->HasAnyFlags(RF_ClassDefaultObject)
		&& ObjectClass->GetSuperClass() != nullptr)
	{
		return ObjectClass->GetSuperClass();
	}
	return ObjectClass;
}

                                                                                                                         
                                                                                                                    
static bool IsAllowlistedScriptStockProperty(
	const UAngelscriptSettings* Settings,
	const FProperty* Property)
{
	if (Settings == nullptr || Property == nullptr)
		return false;

	const UStruct* Owner = Property->GetOwnerStruct();
	if (Owner == nullptr)
		return false;

	const FString Key = Owner->GetPathName() + TEXT(".") + Property->GetName();
	return Settings->StockPurePropertyPreservationAllowlist.Contains(Key);
}

static TArray<FScriptStockPropertySnapshot> CaptureScriptStockProperties(UObject* Object)
{
	TArray<FScriptStockPropertySnapshot> Snapshots;
	const bool bForceOff = FParse::Param(
		FCommandLine::Get(),
		TEXT("SoloASDisableStockPureProperties"));
	const bool bCommandLineOptIn = FParse::Param(
		FCommandLine::Get(),
		TEXT("SoloASPreserveStockPureProperties"));
	const UAngelscriptSettings* Settings = FAngelscriptManager::ConfigSettings;
	const bool bConfiguredOptIn = Settings != nullptr
		&& Settings->bEnableStockPurePropertyPreservation;
	const bool bOptIn = bCommandLineOptIn || bConfiguredOptIn;
	if (Object == nullptr || bForceOff || !bOptIn)
	{
		return Snapshots;
	}

	if (bOptIn
		&& (Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
			|| Cast<UBlueprintGeneratedClass>(Object->GetClass()) == nullptr))
		return Snapshots;

	const UClass* ObjectClass = Object->GetClass();
	const UClass* DefaultsClass = GetScriptStockDefaultsClass(Object);
	if (DefaultsClass == nullptr)
		return Snapshots;

	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsSafeScriptStockProperty(Property)
			|| !Property->IsInContainer(DefaultsClass)
			|| (bOptIn
				&& !IsAllowlistedScriptStockProperty(Settings, Property)))
			continue;

		FScriptStockPropertySnapshot& Snapshot = Snapshots.AddDefaulted_GetRef();
		Snapshot.Property = Property;
		Snapshot.Owner = Property->GetOwnerStruct();
		Snapshot.ObjectClass = ObjectClass;
		Snapshot.DefaultsClass = DefaultsClass;
		Snapshot.PropertyClass = Property->GetClass();
		Snapshot.Offset = Property->GetOffset_ForInternal();
		Snapshot.Size = Property->GetSize();
		Snapshot.ElementSize = Property->GetElementSize();
		Snapshot.PropertyFlags = static_cast<uint64>(
			Property->GetPropertyFlags());
		Snapshot.Value = Property->AllocateAndInitializeValue();
		if (Snapshot.Value == nullptr)
		{
			Snapshots.RemoveAt(Snapshots.Num() - 1, 1, EAllowShrinking::No);
			continue;
		}
		Property->CopyCompleteValue(
			Snapshot.Value,
			Property->ContainerPtrToValuePtr<void>(Object));
	}

	return Snapshots;
}

                                                                                                                  
                                                                                                                    
static int32 RestoreScriptStockProperties(
	UObject* Object,
	const TArray<FScriptStockPropertySnapshot>& Snapshots)
{
	if (Object == nullptr || Snapshots.Num() == 0)
		return 0;

	int32 RestoredCount = 0;
	for (const FScriptStockPropertySnapshot& Snapshot : Snapshots)
	{
		FProperty* CurrentProperty = Snapshot.Property == nullptr
			? nullptr
			: Object->GetClass()->FindPropertyByName(
				Snapshot.Property->GetFName());
		if (CurrentProperty != Snapshot.Property
			|| Snapshot.Owner == nullptr
			|| Object->GetClass() != Snapshot.ObjectClass
			|| Snapshot.DefaultsClass == nullptr
			|| CurrentProperty->GetClass() != Snapshot.PropertyClass
			|| CurrentProperty->GetOwnerStruct() != Snapshot.Owner
			|| CurrentProperty->GetOffset_ForInternal() != Snapshot.Offset
			|| CurrentProperty->GetSize() != Snapshot.Size
			|| CurrentProperty->GetElementSize() != Snapshot.ElementSize
			|| static_cast<uint64>(CurrentProperty->GetPropertyFlags())
				!= Snapshot.PropertyFlags
			|| !CurrentProperty->IsInContainer(Snapshot.DefaultsClass)
			|| !IsSafeScriptStockProperty(CurrentProperty))
		{
			continue;
		}

		CurrentProperty->CopyCompleteValue(
			CurrentProperty->ContainerPtrToValuePtr<void>(Object),
			Snapshot.Value);
		++RestoredCount;
	}

	return RestoredCount;
}
                                                                                                                  
                                                                                                                           
static void RestoreScriptBlueprintDefaultProperties(
	UObject* Object,
	UASClass* Anchor,
	UBlueprintGeneratedClass* BlueprintClass,
	UObject* BlueprintDefault)
{
	if (Object == nullptr || Anchor == nullptr || BlueprintClass == nullptr
		|| BlueprintDefault == nullptr || !BlueprintClass->IsChildOf(Anchor)
		|| BlueprintClass->HasAnyClassFlags(CLASS_NewerVersionExists)
		|| BlueprintDefault->GetClass() != BlueprintClass
		|| !BlueprintDefault->HasAnyFlags(RF_ClassDefaultObject)
		|| BlueprintDefault->HasAnyFlags(RF_NeedLoad | RF_NeedInitialization))
	{
		return;
	}
	// Stock InitProperties has already copied this initialized template before
	// the script constructor runs. Pending PostLoad alone does not invalidate
	// those values; rejecting them here loses Blueprint container defaults.
	UObject* ParentDefault = Anchor->GetDefaultObject(false);
	if (ParentDefault == nullptr
		|| ParentDefault->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedInitialization))
	{
		return;
	}
	for (FProperty* Property = Anchor->PropertyLink; Property; Property = Property->PropertyLinkNext)
	{
		const UClass* OwnerClass = Property->GetOwnerClass();
		const bool bRuntimeTransient = Property->HasAnyPropertyFlags(
			CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient
			| CPF_Config | CPF_GlobalConfig);
		if (OwnerClass == nullptr || !SoloASBridge::IsRegisteredScriptClass(OwnerClass)
			|| !Property->IsInContainer(Anchor) || bRuntimeTransient
			|| Property->Identical_InContainer(BlueprintDefault, ParentDefault)
			|| Property->ContainsInstancedObjectProperty())
		{
			continue;
		}
		Property->CopyCompleteValue_InContainer(Object, BlueprintDefault);
	}
}
                                                                                                                         
#if WITH_EDITOR
thread_local bool GIsInAngelscriptThreadSafeFunction = false;
thread_local bool GIsAngelscriptWorldContextAvailable = false;

ANGELSCRIPTCODE_API void SetAngelscriptWorldContextAvailable(bool bAvailable)
{
	GIsAngelscriptWorldContextAvailable = bAvailable;
}
#endif

UASClass::UASClass(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
                                                                                                                           
#if ENGINE_MAJOR_VERSION != 5 || ENGINE_MINOR_VERSION != 8 || ENGINE_PATCH_VERSION != 3
#error CS.Runtime.ClassConstruction.ScriptClassIdentity.EnginePrivateRoute requires the locked UE 5.8.3 Haze seam.
#endif
	// UObjectGlobals consumes this identity during object construction, before
	// the generator can publish ScriptTypePtr on the completed UASClass.
#if WITH_ANGELSCRIPT_HAZE
	bIsScriptClass = true;
#else
	// The locked Haze fork exposes this field even when its legacy product macro
	// defaults to zero. Stock UE has no field; the dependent branch therefore
	// disappears without an offset, ABI assumption, or engine source change.
	[]<typename T>(T* Class)
	{
		if constexpr (requires(T& Candidate) { Candidate.bIsScriptClass; })
		Class->bIsScriptClass = true;
	}(this);
#endif
                                                                                                                         
}

FORCEINLINE bool CheckGameThreadExecution(UASFunction* Function)
{
#if !UE_BUILD_TEST && !UE_BUILD_SHIPPING
	// During initial compile we are allowed to do gamethread stuff in other threads
	if (!FAngelscriptManager::bIsInitialCompileFinished)
		return true;

#if WITH_EDITOR
	auto* ConstructingObject = UASClass::GetConstructingASObject();
	if (!IsInGameThread() || ConstructingObject != nullptr)
#else
	if (!IsInGameThread())
#endif
	{
		AS_ENSURE(false,
			TEXT("BlueprintEvent/BlueprintOverride %s is being called from a `default` statement or on a different thread.\n")
			TEXT("This is not allowed unless declared as thread safe. (default statements can run in the async loading thread)"),
			*Function->GetPathName());
		return false;
	}
#endif

	return true;
}

FORCEINLINE static void VerifyScriptVirtualResolved(UASFunction* Function, UObject* Object)
{
#if !UE_BUILD_SHIPPING && !UE_BUILD_TEST
	asCScriptFunction* VirtualScriptFunction = (asCScriptFunction*)Function->ScriptFunction;
	if (VirtualScriptFunction->vfTableIdx == -1)
		return;

                                                                                                           
#if 0
                                                                                                         
	asCObjectType* ObjectType = (asCObjectType*)Object->GetClass()->ScriptTypePtr;
                                                                                                       
#endif
	asCObjectType* ObjectType = (asCObjectType*)GetAngelscriptClassScriptType(Object->GetClass());
                                                                                                     
	checkSlow(ObjectType != nullptr);
	checkSlow(VirtualScriptFunction->vfTableIdx >= 0);
	checkSlow(VirtualScriptFunction->vfTableIdx < (int)ObjectType->virtualFunctionTable.GetLength());

	asCScriptFunction* RealScriptFunction = ObjectType->virtualFunctionTable[VirtualScriptFunction->vfTableIdx];
	check(RealScriptFunction == VirtualScriptFunction);
#endif
}

FORCEINLINE static asCScriptFunction* ResolveScriptVirtual(UASFunction* Function, UObject* Object)
{
	asCScriptFunction* VirtualScriptFunction = (asCScriptFunction*)Function->ScriptFunction;
	if (VirtualScriptFunction->vfTableIdx == -1)
		return VirtualScriptFunction;

                                                                                                            
#if 0
                                                                                                          
	asCObjectType* ObjectType = (asCObjectType*)Object->GetClass()->ScriptTypePtr;
                                                                                                        
#endif
	asCObjectType* ObjectType = (asCObjectType*)GetAngelscriptClassScriptType(Object->GetClass());
                                                                                                      
	checkSlow(ObjectType != nullptr);
	checkSlow(VirtualScriptFunction->vfTableIdx >= 0);
	checkSlow(VirtualScriptFunction->vfTableIdx < (int)ObjectType->virtualFunctionTable.GetLength());

	asCScriptFunction* RealScriptFunction = ObjectType->virtualFunctionTable[VirtualScriptFunction->vfTableIdx];
	return RealScriptFunction;
}

template<bool TThreadSafe, bool TNonVirtual>
static FORCEINLINE_DEBUGGABLE void AngelscriptCallFromBPVM(UASFunction* ASFunction, UObject* Object, FFrame& Stack, RESULT_DECL)
{
	AS_LLM_SCOPE
                                                                                                     

	TOptional<FAngelscriptExecutionReadScope> ExecutionReadScope;
	if constexpr (TThreadSafe)
	{
		ExecutionReadScope.Emplace();
	}
                                                                                                   

#if AS_CAN_HOTRELOAD
	if (ASFunction->ScriptFunction == nullptr)
		return;
#endif

	if constexpr (!TThreadSafe)
	{
		if (!CheckGameThreadExecution(ASFunction))
			return;
	}

	asCScriptFunction* ScriptFunction = (asCScriptFunction*)ASFunction->ScriptFunction;
	asJITFunction JitFunction = nullptr;
	if constexpr (TNonVirtual)
	{
		JitFunction = ASFunction->JitFunction;
	}
	else
	{
		ScriptFunction = ResolveScriptVirtual(ASFunction, Object);
		JitFunction = ScriptFunction->jitFunction;
	}

	if (!TThreadSafe && JitFunction != nullptr)
	{
		UObject* NewWorldContext = nullptr;
		FScriptExecution Execution(FAngelscriptManager::GameThreadTLD);

		if constexpr (TNonVirtual)
			Execution.EntryJitFunction = (void*)ASFunction->JitFunction_Raw;
		else
			Execution.EntryJitFunction = (void*)ScriptFunction->jitFunction_Raw;

		uint8* ArgStack = (uint8*)FMemory_Alloca(ASFunction->ArgStackSize);

		int32 ArgumentCount = ASFunction->Arguments.Num();
		asDWORD* VMArgs = (asDWORD*)FMemory_Alloca(8 * ArgumentCount + 16);
		asDWORD* VMArgStart = VMArgs;

		if (!ASFunction->HasAnyFunctionFlags(FUNC_Static))
		{
			NewWorldContext = Object;
			*(void**)VMArgs = Object;
			VMArgs += 2;
		}

		if (ASFunction->ReturnArgument.VMBehavior == UASFunction::EArgumentVMBehavior::ReturnObjectPOD)
		{
			checkSlow(ScriptFunction->DoesReturnOnStack());

			*(void**)VMArgs = RESULT_PARAM;
			VMArgs += 2;
		}
		else if (ASFunction->ReturnArgument.VMBehavior == UASFunction::EArgumentVMBehavior::ReturnObjectValue)
		{
			checkSlow(ScriptFunction->DoesReturnOnStack());

			// The BP VM already initialized the return value, so we need to destruct it,
			// because it will be re-initialized by the AS VM
			ASFunction->ReturnArgument.Type.DestructValue(RESULT_PARAM);

			*(void**)VMArgs = RESULT_PARAM;
			VMArgs += 2;
		}

		for (int32 i = 0; i < ArgumentCount; ++i)
		{
			auto& Arg = ASFunction->Arguments[i];
			switch (Arg.VMBehavior)
			{
				case UASFunction::EArgumentVMBehavior::FloatExtendedToDouble:
				{
					float Value = 0;
					Stack.StepCompiledIn<FProperty>(&Value);

					*(double*)VMArgs = (double)Value;
					VMArgs += 2;
				}
				break;
				case UASFunction::EArgumentVMBehavior::WorldContextObject:
				{
					void* Ptr = nullptr;
					Stack.StepCompiledIn<FProperty>(&Ptr);

					NewWorldContext = (UObject*)Ptr;
					*(void**)VMArgs = Ptr;
					VMArgs += 2;

					AS_ENSURE(NewWorldContext != nullptr || ASFunction->bIsWorldContextOptional, TEXT("Null WorldContext passed into static function call '%s'"), *ASFunction->GetName());
				}
				break;
				case UASFunction::EArgumentVMBehavior::ObjectPointer:
				{
					void* Ptr = nullptr;
					Stack.StepCompiledIn<FProperty>(&Ptr);

					*(void**)VMArgs = Ptr;
					VMArgs += 2;
				}
				break;
				case UASFunction::EArgumentVMBehavior::ReferencePOD:
				{
					void* StackPtr = ArgStack + Arg.StackOffset;

					uint8& RefValue = Stack.StepCompiledInRef<FProperty, uint8>(StackPtr);

					*(void**)VMArgs = &RefValue;
					VMArgs += 2;
				}
				break;
				case UASFunction::EArgumentVMBehavior::Reference:
				{
					void* StackPtr = ArgStack + Arg.StackOffset;
					Arg.Type.ConstructValue(StackPtr);

					uint8& RefValue = Stack.StepCompiledInRef<FProperty, uint8>(StackPtr);
					*(void**)VMArgs = &RefValue;
					VMArgs += 2;
				}
				break;
				case UASFunction::EArgumentVMBehavior::Value1Byte:
				case UASFunction::EArgumentVMBehavior::Value2Byte:
				case UASFunction::EArgumentVMBehavior::Value4Byte:
				{
					asDWORD Value = 0;
					Stack.StepCompiledIn<FProperty>(&Value);

					*(asDWORD*)VMArgs = Value;
					VMArgs += 1;
				}
				break;
				case UASFunction::EArgumentVMBehavior::Value8Byte:
				{
					asQWORD Value = 0;
					Stack.StepCompiledIn<FProperty>(&Value);

					*(asQWORD*)VMArgs = Value;
					VMArgs += 2;
				}
				break;
				default:
					UE_ASSUME(false);
				break;
			}

		}

		if (ASFunction->bIsWorldContextGenerated)
		{
			P_GET_OBJECT(UObject, ArgWorldContext);
			NewWorldContext = ArgWorldContext;

			AS_ENSURE(NewWorldContext != nullptr || ASFunction->bIsWorldContextOptional, TEXT("Null WorldContext passed into static function call '%s'"), *ASFunction->GetName());
		}

		P_FINISH;

		asQWORD OutValue = 0;
		FAngelscriptGameThreadScopeWorldContext WorldContext(NewWorldContext);
		(JitFunction)(Execution, VMArgStart, &OutValue);

		switch (ASFunction->ReturnArgument.VMBehavior)
		{
		case UASFunction::EArgumentVMBehavior::ReferencePOD:
		{
			// Special case for value types, we need to actually copy these back into the parm struct
			void* RetValue = (void*&)OutValue;

			// We may not have a return address, if the angelscript function threw an
			// exception for example.
			if (RetValue != nullptr)
				FMemory::Memcpy(RESULT_PARAM, RetValue, ASFunction->ReturnArgument.ValueBytes);
		}
		break;
		case UASFunction::EArgumentVMBehavior::Reference:
		{
			// Special case for value types, we need to actually copy these back into the parm struct
			void* RetValue = (void*&)OutValue;

			// We may not have a return address, if the angelscript function threw an
			// exception for example.
			if (RetValue != nullptr)
				ASFunction->ReturnArgument.Type.CopyValue(RetValue, RESULT_PARAM);
		}
		break;
		case UASFunction::EArgumentVMBehavior::FloatExtendedToDouble:
			*(float*)RESULT_PARAM = (float)(double&)OutValue;
		break;
		case UASFunction::EArgumentVMBehavior::ReturnObjectPOD:
		case UASFunction::EArgumentVMBehavior::ReturnObjectValue:
		break;
		case UASFunction::EArgumentVMBehavior::Value1Byte:
			*(asBYTE*)RESULT_PARAM = (asBYTE)(asDWORD&)OutValue;
		break;
		case UASFunction::EArgumentVMBehavior::Value2Byte:
			*(asWORD*)RESULT_PARAM = (asWORD)(asDWORD&)OutValue;
		break;
		case UASFunction::EArgumentVMBehavior::Value4Byte:
			*(asDWORD*)RESULT_PARAM = (asDWORD&)OutValue;
		break;
		case UASFunction::EArgumentVMBehavior::Value8Byte:
			*(asQWORD*)RESULT_PARAM = OutValue;
		break;
		case UASFunction::EArgumentVMBehavior::None:
		break;
		default:
			UE_ASSUME(false);
		break;
		}

		for (int32 i = 0, Num = ASFunction->DestroyArguments.Num(); i < Num; ++i)
		{
			auto& Arg = ASFunction->DestroyArguments[i];
			Arg.Type.DestructValue(ArgStack + Arg.StackOffset);
		}
	}
	else
	{
		uint8* ArgStack = (uint8*)FMemory_Alloca(ASFunction->ArgStackSize);

		const bool bInGameThread = !TThreadSafe || IsInGameThread();
		bool bChangedWorldContext = false;
		UObject* PrevWorldContext = nullptr;

		// Scope because FAngelscriptPooledContextBase needs to be destructed before we reset the world context
		{
			FAngelscriptPooledContextBase Context;
			Context->Prepare(ScriptFunction);

			for (int32 i = 0, Num = ASFunction->Arguments.Num(); i < Num; ++i)
			{
				auto& Arg = ASFunction->Arguments[i];

				FAngelscriptType::FArgData Data;
				Data.StackPtr = ArgStack + Arg.StackOffset;

				Arg.Type.SetArgument(i, Context, Stack, Data);
			}

			if (!ASFunction->HasAnyFunctionFlags(FUNC_Static))
			{
				if (bInGameThread)
				{
					PrevWorldContext = (UObject*)FAngelscriptManager::CurrentWorldContext;
					FAngelscriptManager::AssignWorldContext(Object);
					bChangedWorldContext = true;
				}
				Context->SetObject(Object);
			}
			else if (ASFunction->bIsWorldContextGenerated)
			{
				checkSlow(ASFunction->WorldContextIndex == ASFunction->Arguments.Num());

				P_GET_OBJECT(UObject, WorldContext);

				if (bInGameThread)
				{
					PrevWorldContext = (UObject*)FAngelscriptManager::CurrentWorldContext;
					FAngelscriptManager::AssignWorldContext(WorldContext);
					bChangedWorldContext = true;

					AS_ENSURE(WorldContext != nullptr || ASFunction->bIsWorldContextOptional, TEXT("Null WorldContext passed into static function call '%s'"), *ASFunction->GetName());
				}
			}
			else if (ASFunction->WorldContextIndex >= 0)
			{
				UObject* WorldContext = *(UObject**)Context->GetAddressOfArg(ASFunction->WorldContextIndex);

				if (bInGameThread)
				{
					PrevWorldContext = (UObject*)FAngelscriptManager::CurrentWorldContext;
					FAngelscriptManager::AssignWorldContext(WorldContext);
					bChangedWorldContext = true;

					AS_ENSURE(WorldContext != nullptr || ASFunction->bIsWorldContextOptional, TEXT("Null WorldContext passed into static function call '%s'"), *ASFunction->GetName());
				}
			}
			else
			{
				// Static function does not have any world context pin
				if (bInGameThread)
				{
					PrevWorldContext = (UObject*)FAngelscriptManager::CurrentWorldContext;
					FAngelscriptManager::AssignWorldContext(nullptr);
					bChangedWorldContext = true;
				}
			}

			P_FINISH;

			Context->Execute();

			if (ASFunction->ReturnArgument.Property != nullptr)
			{
				if (ASFunction->ReturnArgument.VMBehavior == UASFunction::EArgumentVMBehavior::Reference)
				{
					void* RetValue = Context->GetReturnAddress();

					// We may not have a return address, if the angelscript function threw an
					// exception for example.
					if (RetValue != nullptr)
						ASFunction->ReturnArgument.Type.CopyValue(RetValue, RESULT_PARAM);
				}
				else if (ASFunction->ReturnArgument.VMBehavior == UASFunction::EArgumentVMBehavior::ReferencePOD)
				{
					void* RetValue = Context->GetReturnAddress();

					// We may not have a return address, if the angelscript function threw an
					// exception for example.
					if (RetValue != nullptr)
						FMemory::Memcpy(RESULT_PARAM, RetValue, ASFunction->ReturnArgument.ValueBytes);
				}
				else
				{
					ASFunction->ReturnArgument.Type.GetReturnValue(Context, RESULT_PARAM);
				}
			}

			for (int32 i = 0, Num = ASFunction->DestroyArguments.Num(); i < Num; ++i)
			{
				auto& Arg = ASFunction->DestroyArguments[i];
				Arg.Type.DestructValue(ArgStack + Arg.StackOffset);
			}
		}

		if (!TThreadSafe || bChangedWorldContext)
			FAngelscriptManager::AssignWorldContext(PrevWorldContext);
	}
}

template<bool TThreadSafe, bool TNonVirtual>
static FORCEINLINE_DEBUGGABLE void AngelscriptCallFromParms(UASFunction* ASFunction, UObject* Object, void* Parms)
{
	AS_LLM_SCOPE
                                                                                                      

	TOptional<FAngelscriptExecutionReadScope> ExecutionReadScope;
	if constexpr (TThreadSafe)
	{
		ExecutionReadScope.Emplace();
	}
                                                                                                    

#if AS_CAN_HOTRELOAD
	if (ASFunction->ScriptFunction == nullptr)
		return;
#endif

	if constexpr (!TThreadSafe)
	{
		if (!CheckGameThreadExecution(ASFunction))
			return;
	}

	asCScriptFunction* ScriptFunction = (asCScriptFunction*)ASFunction->ScriptFunction;
	asJITFunction_ParmsEntry JitFunction;

	if constexpr (TNonVirtual)
	{
		JitFunction = ASFunction->JitFunction_ParmsEntry;
	}
	else
	{
		ScriptFunction = ResolveScriptVirtual(ASFunction, Object);
		JitFunction = ScriptFunction->jitFunction_ParmsEntry;
	}

	if (!TThreadSafe && JitFunction != nullptr)
	{
		UObject* NewWorldContext = nullptr;
		if (!ASFunction->HasAnyFunctionFlags(FUNC_Static))
			NewWorldContext = Object;
		else if (ASFunction->WorldContextOffsetInParms >= 0)
			NewWorldContext = *(UObject**)((SIZE_T)Parms + ASFunction->WorldContextOffsetInParms);

		FAngelscriptGameThreadScopeWorldContext WorldContext(NewWorldContext);
		FScriptExecution Execution(FAngelscriptManager::GameThreadTLD);

		(JitFunction)(Execution, Object, Parms);
	}
	else
	{
		const bool bInGameThread = !TThreadSafe || IsInGameThread();
		bool bChangedWorldContext = false;
		UObject* PrevWorldContext = nullptr;

		// Scope because FAngelscriptPooledContextBase needs to be destructed before we reset the world context
		{
			FAngelscriptPooledContextBase Context;
			Context->Prepare(ScriptFunction);

			for(int32 i = 0, Num = ASFunction->Arguments.Num(); i < Num; ++i)
			{
				auto& Arg = ASFunction->Arguments[i];
				void* ValuePtr = (void*)((SIZE_T)Parms + Arg.PosInParmStruct);

				switch (Arg.ParmBehavior)
				{
				case UASFunction::EArgumentParmBehavior::Reference:
					// Special case for references to values in the parameter struct
					Context->SetArgAddress(i, ValuePtr);
				break;
				case UASFunction::EArgumentParmBehavior::Value1Byte:
					Context->SetArgByte(i, *(asBYTE*)ValuePtr);
				break;
				case UASFunction::EArgumentParmBehavior::Value2Byte:
					Context->SetArgWord(i, *(asWORD*)ValuePtr);
				break;
				case UASFunction::EArgumentParmBehavior::Value4Byte:
					Context->SetArgDWord(i, *(asDWORD*)ValuePtr);
				break;
				case UASFunction::EArgumentParmBehavior::Value8Byte:
					Context->SetArgQWord(i, *(asQWORD*)ValuePtr);
				break;
				case UASFunction::EArgumentParmBehavior::FloatExtendedToDouble:
					// -4 Indicates an unreal float upgraded to a double in script
					Context->SetArgDouble(i, (double)*(float*)ValuePtr);
				break;
				default:
					UE_ASSUME(false);
				break;
				}
			}

			if (!ASFunction->HasAnyFunctionFlags(FUNC_Static))
			{
				if (bInGameThread)
				{
					PrevWorldContext = (UObject*)FAngelscriptManager::CurrentWorldContext;
					FAngelscriptManager::AssignWorldContext(Object);
					bChangedWorldContext = true;
				}
				Context->SetObject(Object);
			}
			else if(ASFunction->bIsWorldContextGenerated)
			{
				UObject* WorldContext = *(UObject**)((SIZE_T)Parms + ASFunction->WorldContextOffsetInParms);
				checkSlow(ASFunction->WorldContextIndex == ASFunction->Arguments.Num());

				if (bInGameThread)
				{
					PrevWorldContext = (UObject*)FAngelscriptManager::CurrentWorldContext;
					FAngelscriptManager::AssignWorldContext(WorldContext);
					bChangedWorldContext = true;

					AS_ENSURE(WorldContext != nullptr || ASFunction->bIsWorldContextOptional, TEXT("Null WorldContext passed into static function call '%s'"), *ASFunction->GetName());
				}
			}
			else if (ASFunction->WorldContextIndex >= 0)
			{
				UObject* WorldContext = *(UObject**)Context->GetAddressOfArg(ASFunction->WorldContextIndex);

				if (bInGameThread)
				{
					PrevWorldContext = (UObject*)FAngelscriptManager::CurrentWorldContext;
					FAngelscriptManager::AssignWorldContext(WorldContext);
					bChangedWorldContext = true;

					AS_ENSURE(WorldContext != nullptr || ASFunction->bIsWorldContextOptional, TEXT("Null WorldContext passed into static function call '%s'"), *ASFunction->GetName());
				}
			}
			else
			{
				// Static function does not have any world context pin
				if (bInGameThread)
				{
					PrevWorldContext = (UObject*)FAngelscriptManager::CurrentWorldContext;
					FAngelscriptManager::AssignWorldContext(nullptr);
					bChangedWorldContext = true;
				}
			}

			Context->Execute();

			if (ASFunction->ReturnArgument.Property != nullptr)
			{
				void* RetPtr = (void*)((SIZE_T)Parms + ASFunction->ReturnArgument.PosInParmStruct);
				switch (ASFunction->ReturnArgument.ParmBehavior)
				{
				case UASFunction::EArgumentParmBehavior::ReturnObjectPointer:
					// Special case for object pointers, these are returned in a different register
					*(void**)RetPtr = Context->GetReturnObject();
				break;
				case UASFunction::EArgumentParmBehavior::Reference:
				{
					// Special case for value types, we need to actually copy these back into the parm struct
					void* RetValue = Context->GetReturnAddress();

					// We may not have a return address, if the angelscript function threw an
					// exception for example.
					if (RetValue != nullptr)
						ASFunction->ReturnArgument.Type.CopyValue(RetValue, RetPtr);
				}
				break;
				case UASFunction::EArgumentParmBehavior::ReferencePOD:
				{
					// Special case for value types, we need to actually copy these back into the parm struct
					void* RetValue = Context->GetReturnAddress();

					// We may not have a return address, if the angelscript function threw an
					// exception for example.
					if (RetValue != nullptr)
						FMemory::Memcpy(RetPtr, RetValue, ASFunction->ReturnArgument.ValueBytes);
				}
				break;
				case UASFunction::EArgumentParmBehavior::Value1Byte:
					*(uint8*)RetPtr = Context->GetReturnByte();
				break;
				case UASFunction::EArgumentParmBehavior::Value2Byte:
					*(uint16*)RetPtr = Context->GetReturnWord();
				break;
				case UASFunction::EArgumentParmBehavior::Value4Byte:
					*(uint32*)RetPtr = Context->GetReturnDWord();
				break;
				case UASFunction::EArgumentParmBehavior::Value8Byte:
					*(uint64*)RetPtr = Context->GetReturnQWord();
				break;
				case UASFunction::EArgumentParmBehavior::FloatExtendedToDouble:
					// -4 Indicates an unreal float upgraded to a double in script
					*(float*)RetPtr = (float)Context->GetReturnDouble();
				break;
				default:
					UE_ASSUME(false);
				break;
				}
			}
		}

		if (!TThreadSafe || bChangedWorldContext)
		{
			FAngelscriptManager::AssignWorldContext(PrevWorldContext);
		}
	}
}

FORCEINLINE void MakeRawJITCall_NoParam(UObject* Object, asJITFunction_Raw InFunction)
{
	AS_LLM_SCOPE

	checkSlow(FAngelscriptManager::GameThreadTLD == asCThreadManager::GetLocalData());

	FAngelscriptGameThreadScopeWorldContext WorldContext(Object);
	FScriptExecution Execution(FAngelscriptManager::GameThreadTLD);

	using TFuncPtr = void(*)(FScriptExecution&, void*);
	((TFuncPtr)InFunction)(
		Execution, Object
	);
}

template<typename TArgument>
FORCEINLINE void MakeRawJITCall_Arg(UObject* Object, asJITFunction_Raw InFunction, TArgument ArgValue)
{
	AS_LLM_SCOPE

	checkSlow(FAngelscriptManager::GameThreadTLD == asCThreadManager::GetLocalData());

	FAngelscriptGameThreadScopeWorldContext WorldContext(Object);
	FScriptExecution Execution(FAngelscriptManager::GameThreadTLD);

	using TFuncPtr = void(*)(FScriptExecution&, void*, TArgument Arg);
	((TFuncPtr)InFunction)(
		Execution, Object, ArgValue
	);
}

template<typename TReturnValue>
FORCEINLINE TReturnValue MakeRawJITCall_ReturnValue(UObject* Object, asJITFunction_Raw InFunction)
{
	AS_LLM_SCOPE

	checkSlow(FAngelscriptManager::GameThreadTLD == asCThreadManager::GetLocalData());

	FAngelscriptGameThreadScopeWorldContext WorldContext(Object);
	FScriptExecution Execution(FAngelscriptManager::GameThreadTLD);

	using TFuncPtr = TReturnValue(*)(FScriptExecution&, void*);
	return ((TFuncPtr)InFunction)(
		Execution, Object
	);
}

template<typename TArgument, typename TReturnValue>
FORCEINLINE TReturnValue MakeRawJITCall_Arg_ReturnValue(UObject* Object, asJITFunction_Raw InFunction, TArgument ArgValue)
{
	AS_LLM_SCOPE

	checkSlow(FAngelscriptManager::GameThreadTLD == asCThreadManager::GetLocalData());

	FAngelscriptGameThreadScopeWorldContext WorldContext(Object);
	FScriptExecution Execution(FAngelscriptManager::GameThreadTLD);

	using TFuncPtr = TReturnValue(*)(FScriptExecution&, void*, TArgument Arg);
	return ((TFuncPtr)InFunction)(
		Execution, Object, ArgValue
	);
}

void UASFunction::FinalizeArguments()
{
	ArgStackSize = 0;
	for (int32 i = 0, Num = Arguments.Num(); i < Num; ++i)
	{
		auto& Arg = Arguments[i];

		int32 ArgSize = Arg.Type.GetValueSize();
		int32 ArgAlign = Arg.Type.GetValueAlignment();

		int32 AlignOffset = (Align(ArgStackSize, ArgAlign) - ArgStackSize);
		ArgStackSize += AlignOffset;

		Arg.ValueBytes = ArgSize;
		Arg.StackOffset = ArgStackSize;
		Arg.PosInParmStruct = Arg.Property->GetOffset_ForUFunction();

		if (Arg.Type.bIsReference)
		{
			Arg.ParmBehavior = EArgumentParmBehavior::Reference;
			if (Arg.Type.IsObjectPointer())
				Arg.VMBehavior = EArgumentVMBehavior::ReferencePOD;
			else if (Arg.Type.NeedConstruct())
				Arg.VMBehavior = EArgumentVMBehavior::Reference;
			else
				Arg.VMBehavior = EArgumentVMBehavior::ReferencePOD;
		}
		else if (Arg.Type.IsObjectPointer())
		{
			Arg.ParmBehavior = EArgumentParmBehavior::Value8Byte;

			if (WorldContextIndex == i)
				Arg.VMBehavior = EArgumentVMBehavior::WorldContextObject;
			else
				Arg.VMBehavior = EArgumentVMBehavior::ObjectPointer;
		}
		else
		{
			check(Arg.Type.IsPrimitive());

			// Special case: a float property that is represented by a double argument should
			// be converted as part of the call thunk.
			if (Arg.Type.Type == FAngelscriptType::ScriptFloatParamExtendedToDoubleType())
			{
				Arg.ParmBehavior = EArgumentParmBehavior::FloatExtendedToDouble;
				Arg.VMBehavior = EArgumentVMBehavior::FloatExtendedToDouble;
			}
			else
			{
				switch (Arg.Type.GetValueSize())
				{
				default:
					check(false);
				case 1:
					Arg.ParmBehavior = EArgumentParmBehavior::Value1Byte;
					Arg.VMBehavior = EArgumentVMBehavior::Value1Byte;
				break;
				case 2:
					Arg.ParmBehavior = EArgumentParmBehavior::Value2Byte;
					Arg.VMBehavior = EArgumentVMBehavior::Value2Byte;
				break;
				case 4:
					Arg.ParmBehavior = EArgumentParmBehavior::Value4Byte;
					Arg.VMBehavior = EArgumentVMBehavior::Value4Byte;
				break;
				case 8:
					Arg.ParmBehavior = EArgumentParmBehavior::Value8Byte;
					Arg.VMBehavior = EArgumentVMBehavior::Value8Byte;
				break;
				}
			}
		}

		if (Arg.Type.CanDestruct() && Arg.Type.NeedDestruct())
		{
			check(Arg.VMBehavior == EArgumentVMBehavior::Reference);
			DestroyArguments.Add(Arg);
		}

		ArgStackSize += ArgSize;
	}

	if (ReturnArgument.Property != nullptr)
	{
		ReturnArgument.PosInParmStruct = ReturnArgument.Property->GetOffset_ForUFunction();
		ReturnArgument.ValueBytes = ReturnArgument.Type.GetValueSize();

		if (ReturnArgument.Type.bIsReference)
		{
			if (ReturnArgument.Type.NeedCopy())
			{
				ReturnArgument.ParmBehavior = EArgumentParmBehavior::Reference;
				ReturnArgument.VMBehavior = EArgumentVMBehavior::Reference;
			}
			else
			{
				ReturnArgument.ParmBehavior = EArgumentParmBehavior::ReferencePOD;
				ReturnArgument.VMBehavior = EArgumentVMBehavior::ReferencePOD;
			}
		}
		else if (ReturnArgument.Type.IsObjectPointer())
		{
			ReturnArgument.ParmBehavior = EArgumentParmBehavior::ReturnObjectPointer;
			ReturnArgument.VMBehavior = EArgumentVMBehavior::Value8Byte;
		}
		else if (ReturnArgument.Type.IsPrimitive())
		{
			// Special case: a float property that is represented by a double argument should
			// be converted as part of the call thunk.
			if (ReturnArgument.Type.Type == FAngelscriptType::ScriptFloatParamExtendedToDoubleType())
			{
				ReturnArgument.ParmBehavior = EArgumentParmBehavior::FloatExtendedToDouble;
				ReturnArgument.VMBehavior = EArgumentVMBehavior::FloatExtendedToDouble;
			}
			else
			{
				switch (ReturnArgument.Type.GetValueSize())
				{
				default:
					check(false);
				case 1:
					ReturnArgument.ParmBehavior = EArgumentParmBehavior::Value1Byte;
					ReturnArgument.VMBehavior = EArgumentVMBehavior::Value1Byte;
				break;
				case 2:
					ReturnArgument.ParmBehavior = EArgumentParmBehavior::Value2Byte;
					ReturnArgument.VMBehavior = EArgumentVMBehavior::Value2Byte;
				break;
				case 4:
					ReturnArgument.ParmBehavior = EArgumentParmBehavior::Value4Byte;
					ReturnArgument.VMBehavior = EArgumentVMBehavior::Value4Byte;
				break;
				case 8:
					ReturnArgument.ParmBehavior = EArgumentParmBehavior::Value8Byte;
					ReturnArgument.VMBehavior = EArgumentVMBehavior::Value8Byte;
				break;
				}
			}
		}
		else
		{
			if (ReturnArgument.Type.NeedCopy())
				ReturnArgument.ParmBehavior = EArgumentParmBehavior::Reference;
			else
				ReturnArgument.ParmBehavior = EArgumentParmBehavior::ReferencePOD;

			if (ReturnArgument.Type.NeedDestruct())
				ReturnArgument.VMBehavior = EArgumentVMBehavior::ReturnObjectValue;
			else
				ReturnArgument.VMBehavior = EArgumentVMBehavior::ReturnObjectPOD;
		}
	}
	else
	{
		ReturnArgument.VMBehavior = EArgumentVMBehavior::None;
	}
}

void UASClass::GetLifetimeScriptReplicationList(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
                                                                                                                                  
#if 0
                                                                                                                                
	for (TFieldIterator<FProperty> It(this, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		FProperty * Prop = *It;
		if (Prop != NULL && Prop->GetPropertyFlags() & CPF_Net)
		{
			const uint16 RepIndex = Prop->RepIndex;
			FLifetimeProperty* Found = OutLifetimeProps.FindByPredicate([RepIndex](const FLifetimeProperty& LifetimeProp)
			{
				return LifetimeProp.RepIndex == RepIndex;
			});

			if (Found == nullptr)
			{
				const bool bIsPushModel = PushModelReplicatedProperties.Contains(Prop->GetFName());
				OutLifetimeProps.Add(FLifetimeProperty(Prop->RepIndex, Prop->GetBlueprintReplicationCondition(), REPNOTIFY_OnChanged, bIsPushModel));
			}
		}
	}

	UASClass* SuperScriptClass = Cast<UASClass>(GetSuperStruct());
	if (SuperScriptClass != NULL)
	{
		SuperScriptClass->GetLifetimeScriptReplicationList(OutLifetimeProps);
	}
                                                                                                                       
#endif
	(void)TryGetLifetimeScriptReplicationList(this, OutLifetimeProps);
                                                                                                                     
}

                                                                                                         
void UASClass::Link(FArchive& Ar, bool bRelinkExistingProperties)
{
    FAngelscriptExecutionWriteScope Writer;
    RetireRuntimeTerminalClassChain(this);
    const int32 ExistingSize = GetPropertiesSize();
    ScriptFinishDestroyFields.Reset();
    Super::Link(Ar, bRelinkExistingProperties);
    if (bRelinkExistingProperties && ScriptTypePtr)
    {
        // AS bytecode uses compiler-owned offsets, including inherited storage.
        // UE may rebuild the links but cannot assign a different script layout.
        const auto* Type = CastToObjectType(static_cast<asCTypeInfo*>(ScriptTypePtr));
        if (Type)
            for (asUINT I = 0; I != Type->localProperties.GetLength(); ++I)
            {
                const auto* Member = Type->localProperties[I];
                if (!Member) continue;
                auto* Property = FindFProperty<FProperty>(this, FName(ANSI_TO_TCHAR(Member->name.AddressOf())));
                if (Property && Property->GetOwnerStruct() == this && Property->ArrayDim == 1
                    && Property->GetSize() == Member->type.GetSizeInMemoryBytes())
                {
                    SetPropertiesSize(Member->byteOffset);
                    Property->Link(Ar);
                    check(Property->GetOffset_ForInternal() == Member->byteOffset);
                }
            }
        SetPropertiesSize(FMath::Max(GetPropertiesSize(), FMath::Max(ExistingSize, int32(ScriptTypePtr->GetSize()))));
    }
    RebuildRuntimeTerminalClassChain(this);
}

void UASClass::DestroyPersistentUberGraphFrame(UObject* Object, bool bSkipSuperClass) const
{
#if !SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    // SetClass also tears down the old ubergraph frame while retaining the
    // live script object. Only actual object destruction owns AS finalization.
    if (Object && (Object->HasAnyFlags(RF_BeginDestroyed) || GExitPurge)
        && GetFirstASClass(Object->GetClass()) == this
        && (ScriptTypePtr || RetiredScriptTypeForGc) && !IsAngelscriptExecutionShutdownStarted())
    {
        FAngelscriptExecutionWriteScope Writer(FAngelscriptExecutionWriteScope::EAcquire::GarbageCollection);
        SoloASBridge::FScopedScriptFinishDestroy NativeCalls(Object);
        const_cast<UASClass*>(this)->RuntimeDestroyObject(Object);
        AngelscriptRuntimeGcMirror::ReleasePublicDestroyReloadTracking(Object);
    }
#endif
    Super::DestroyPersistentUberGraphFrame(Object, bSkipSuperClass);
}

void UASClass::RetainRetiredScriptTypeForGc()
{
#if !SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    FAngelscriptExecutionWriteScope Writer;
    if (!RetiredScriptTypeForGc && ScriptTypePtr)
    {
        RetiredScriptModuleForGc = AngelscriptRetiredModules::RetainType(ScriptTypePtr);
        check(RetiredScriptModuleForGc);
        RetiredScriptTypeForGc = ScriptTypePtr;
    }
#endif
}

void UASClass::ReleaseRetiredScriptTypeForGc()
{
#if !SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    FAngelscriptExecutionWriteScope Writer;
    if (RetiredScriptModuleForGc)
        AngelscriptRetiredModules::ReleaseModule(RetiredScriptModuleForGc);
    RetiredScriptModuleForGc = nullptr;
    RetiredScriptTypeForGc = nullptr;
#endif
}

void UASClass::BeginDestroy()
{
#if !SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    TArray<UObject*> Receivers;
    GetObjectsOfClass(this, Receivers, true, RF_NoFlags, EInternalObjectFlags::None);
    for (UObject* Receiver : Receivers)
    {
        FinishDestroyReceivers.AddUnique(FObjectKey(Receiver));
    }
#endif
    Super::BeginDestroy();
}

bool UASClass::IsReadyForFinishDestroy()
{
#if !SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    // Keep inherited field metadata and the retired module alive until every
    // actual receiver has completed physical destruction, including BP CDOs.
    if (AngelscriptRuntimeGcMirror::HasRuntimeObjectsPendingFinishDestroyForClass(this))
        return false;
    for (const FObjectKey& Receiver : FinishDestroyReceivers)
    {
        if (Receiver.ResolveObjectPtrEvenIfUnreachable())
        {
            return false;
        }
    }
#endif
    return Super::IsReadyForFinishDestroy() && IsRuntimeTerminalClassReadyForFinish(this);
}

void UASClass::FinishDestroy()
{
    FAngelscriptExecutionWriteScope Writer(FAngelscriptExecutionWriteScope::EAcquire::GarbageCollection);
    RetireRuntimeTerminalClassChain(this);
    Super::FinishDestroy();
    ReleaseRetiredScriptTypeForGc();
}

void PrepareRuntimeScriptTypeIdentity(UASClass* Class, asITypeInfo* Type)
{
	check(Type != nullptr);
	// Allocate the real engine identity before GC freezes receiver metadata.
	// Empty derived types need not have used a type-id operand during compilation.
	const int32 TypeId = Type->GetTypeId();
	if (TypeId <= 0 || Type->GetEngine()->GetTypeInfoById(TypeId) != Type)
	{
		UE_LOG(Angelscript, Fatal, TEXT("ASR-CLASS-TYPE-IDENTITY-INVALID: %s"), *Class->GetPathName());
	}
}

bool UASClass::HasValidScriptClassCarrier() const
{
	if (GetClass() != StaticClass()
		|| !HasAllClassFlags(CLASS_CompiledFromBlueprint)
		|| HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic)
		|| GetLinker() != nullptr
		|| !GetOutermost()->HasAnyPackageFlags(PKG_CompiledIn)
		|| HasAnyFlags(RF_WasLoaded | RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects | RF_NeedInitialization))
	{
		return false;
	}
#if WITH_EDITORONLY_DATA
	if (ClassGeneratedBy != nullptr)
	{
		return false;
	}
#endif
	TSet<const UClass*> Visited;
	for (const UClass* Ancestor = GetSuperClass(); Ancestor != nullptr; Ancestor = Ancestor->GetSuperClass())
	{
		if (Visited.Contains(Ancestor)
			|| (Cast<UBlueprintGeneratedClass>(Ancestor) != nullptr
				&& !FAngelscriptManager::Get().IsBlueprintDependencyClass(const_cast<UClass*>(Ancestor))))
		{
			return false;
		}
		Visited.Add(Ancestor);
	}
	return GetSuperClass() != nullptr
		&& (CodeSuperClass == nullptr
			|| CppClassStaticFunctions.GetAddReferencedObjects()
				== CodeSuperClass->CppClassStaticFunctions.GetAddReferencedObjects());
}
                                                                                                       
                                                                                                                 
bool UASClass::HasValidScriptClassCarrierAncestry(
	const UClass* SuperClass,
	const UClass** OutInvalidBlueprintAncestor)
{
	if (OutInvalidBlueprintAncestor != nullptr)
		*OutInvalidBlueprintAncestor = nullptr;

	TSet<const UClass*> Visited;
	for (const UClass* Ancestor = SuperClass; Ancestor != nullptr; Ancestor = Ancestor->GetSuperClass())
	{
		if (Visited.Contains(Ancestor))
			return false;
		if (Cast<UBlueprintGeneratedClass>(Ancestor) != nullptr
			&& !FAngelscriptManager::Get().IsBlueprintDependencyClass(const_cast<UClass*>(Ancestor)))
		{
			if (OutInvalidBlueprintAncestor != nullptr)
				*OutInvalidBlueprintAncestor = Ancestor;
			return false;
		}
		Visited.Add(Ancestor);
	}
	return true;
}
                                                                                                               
                                                                                                                     
EStructPropertyLinkFlags UASClass::GetPropertyLinkFlags(UStruct* ContainerStruct, FProperty* Property) const
{
	const EStructPropertyLinkFlags Flags = Super::GetPropertyLinkFlags(ContainerStruct, Property);
	if (Cast<UClass>(ContainerStruct) != nullptr && Property->GetOwnerStruct() == this)
	{
		// The script destructor owns class-field lifetimes, including inherited
		// fields in real Blueprint instances. Parameters and standalone struct
		// storage still require the ordinary property destructor chain.
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
		TArray<const FStructProperty*> EncounteredStructs;
		if (Property->ContainsFinishDestroy(EncounteredStructs))
		{
			if (UeDelayedDestroyFields.Contains(Property))
				return Flags;
			// Stock forcibly relinks these fields and calls DestroyValue rather
			// than the native FinishDestroy path. Reject before object allocation
			// until that separate lifetime contract has an admitted adapter.
			UE_LOG(Angelscript, Fatal,
				TEXT("ASR-SCRIPT-FIELD-FINISH-DESTROY-UNSUPPORTED: %s"), *Property->GetPathName());
		}
#endif
		return Flags & ~EStructPropertyLinkFlags::LinkDestructor;
	}
	return Flags;
}
                                                                                                                   
                                                                                                                
static thread_local UObject* ConstructingScriptClassDefaultObject = nullptr;

UObject* UASClass::CreateDefaultObject()
{
	// The metaclass template is not a generated script class.
	if (HasAnyFlags(RF_ClassDefaultObject))
		return UClass::CreateDefaultObject();
	if (UObject* Existing = GetDefaultObject(false))
		return Existing;

	// Only fully linked, direct-born runtime classes use this construction path.
	// Real Blueprint descendants use their own metaclass construction path.
	if (!HasValidScriptClassCarrier() || CodeSuperClass == nullptr
		|| (ScriptTypePtr == nullptr && !bIsModuleStaticCarrier)
		|| (bIsModuleStaticCarrier && (ScriptTypePtr != nullptr || CodeSuperClass != UObject::StaticClass()))
		|| ClassConstructor == nullptr
		|| GetLinker() != nullptr || bLayoutChanging
		|| HasAnyClassFlags(CLASS_NewerVersionExists)
		|| HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedInitialization))
	{
		UE_LOG(Angelscript, Fatal, TEXT("ASR-CDO-CARRIER-INVALID: %s"), *GetPathName());
	}
	UClass* Parent = GetSuperClass();
	UObjectForceRegistration(Parent);
	UObject* ParentDefault = Parent->GetDefaultObject();
	if (ParentDefault == nullptr || ParentDefault->HasAnyFlags(RF_NeedLoad))
	{
		UE_LOG(Angelscript, Fatal, TEXT("ASR-CDO-PARENT-NOT-READY: %s"), *GetPathName());
	}
	if (UObject* ReentrantDefault = GetDefaultObject(false))
		return ReentrantDefault;

	UObject* DefaultObject = StaticAllocateObject(this, GetOuter(), NAME_None,
		RF_Public | RF_ClassDefaultObject | RF_ArchetypeObject);
	std::atomic_thread_fence(std::memory_order_release);
	TSAN_BEFORE(DefaultObject);
	SetDefaultObject(DefaultObject);
	for (TFieldIterator<FMulticastSparseDelegateProperty> It(this,
		EFieldIteratorFlags::ExcludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
	{
		const FSparseDelegate& Delegate = It->GetPropertyValue_InContainer(DefaultObject);
		USparseDelegateFunction* Function = CastChecked<USparseDelegateFunction>(It->SignatureFunction);
		FSparseDelegateStorage::RegisterDelegateOffset(DefaultObject, Function->DelegateName,
			reinterpret_cast<const uint8*>(&Delegate) - reinterpret_cast<const uint8*>(DefaultObject));
	}
	{
		TGuardValue<UObject*> ConstructionScope(ConstructingScriptClassDefaultObject, DefaultObject);
		// Haze CreateDefaultObject excludes script CDOs from InitializeProperties.
		// Native and script constructors run once; the initializer still owns
		// config loading, subobject initialization, PostInitProperties and tracking.
		(*ClassConstructor)(FObjectInitializer(DefaultObject, ParentDefault, EObjectInitializerOptions::None));
	}
	if (GetOutermost()->HasAnyPackageFlags(PKG_CompiledIn)
		&& !GetOutermost()->HasAnyPackageFlags(PKG_RuntimeGenerated))
	{
		NotifyRegistrationEvent(GetOutermost()->GetFName(), GetDefaultObjectName(),
			ENotifyRegistrationType::NRT_ClassCDO, ENotifyRegistrationPhase::NRP_Finished,
			nullptr, false, DefaultObject);
	}
	DefaultObject->PostCDOContruct();
	return DefaultObject;
}
                                                                                                              
                                                                                                       
#include "Extensions/AngelscriptExtensionHooks.h"
static void DispatchScriptDefaultObjectConstruction(const FObjectInitializer& Initializer)
{
    if (ConstructingScriptClassDefaultObject != Initializer.GetObj()) return;
    FAngelscriptTypeGenerationContext Context;
    Context.Point = EAngelscriptTypeGenerationPoint::ConstructDefaultObject;
    Context.ObjectInitializer = &Initializer;
    FString Error;
    if (!DispatchAngelscriptTypeGeneration(Context, Error)) UE_LOG(Angelscript, Fatal, TEXT("%s"), *Error);
}
                                                                                                     
                                                                                                                     
bool UASClass::TryGetLifetimeScriptReplicationList(
	const UASClass* ScriptClass,
	TArray<FLifetimeProperty>& OutLifetimeProps)
{
	if (ScriptClass == nullptr
		|| !IsValid(ScriptClass)
		|| ScriptClass->GetClass() != UASClass::StaticClass()
		|| ScriptClass->GetFName().IsNone())
	{
		return false;
	}

	TSet<uint16> ExistingRepIndices;
	for (const FLifetimeProperty& Existing : OutLifetimeProps)
	{
		ExistingRepIndices.Add(Existing.RepIndex);
	}

	TArray<FLifetimeProperty> PendingScriptLifetimeProps;
	TMap<uint16, const FProperty*> ScriptPropertiesByRepIndex;
	TMap<FName, uint16> ScriptPropertyIndices;

	TSet<const UASClass*> VisitedClasses;
	for (const UASClass* Current = ScriptClass; Current != nullptr; )
	{
		if (VisitedClasses.Contains(Current))
		{
			return false;
		}
		VisitedClasses.Add(Current);

		for (TFieldIterator<FProperty> It(Current, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop == nullptr)
			{
				return false;
			}

			const bool bIsReplicated = Prop->HasAnyPropertyFlags(CPF_Net);
			const bool bHasRepNotify = Prop->HasAnyPropertyFlags(CPF_RepNotify);
			const bool bHasRepNotifyName = !Prop->RepNotifyFunc.IsNone();
			if (bHasRepNotify != bHasRepNotifyName)
			{
				return false;
			}
			if (bHasRepNotify && Current->FindFunctionByName(Prop->RepNotifyFunc) == nullptr)
			{
				return false;
			}
			if (!bIsReplicated)
			{
				continue;
			}

			if (Prop->RepIndex == MAX_uint16)
			{
				return false;
			}

			const FName PropertyName = Prop->GetFName();
			if (PropertyName.IsNone())
			{
				return false;
			}
			if (const uint16* ExistingRepIndex = ScriptPropertyIndices.Find(PropertyName))
			{
				if (*ExistingRepIndex != Prop->RepIndex)
				{
					return false;
				}
			}
			else
			{
				ScriptPropertyIndices.Add(PropertyName, Prop->RepIndex);
			}

			if (ExistingRepIndices.Contains(Prop->RepIndex)
				|| ScriptPropertiesByRepIndex.Contains(Prop->RepIndex))
			{
				return false;
			}

			ScriptPropertiesByRepIndex.Add(Prop->RepIndex, Prop);
			const ELifetimeCondition Condition = Prop->GetBlueprintReplicationCondition();
			// Script replication exposes the stock default notify policy; custom notify timing is not inferred.
			const ELifetimeRepNotifyCondition RepNotifyCondition = REPNOTIFY_OnChanged;
			const bool bIsPushBased = Current->PushModelReplicatedProperties.Contains(PropertyName);
			PendingScriptLifetimeProps.Add(FLifetimeProperty(
				Prop->RepIndex,
				Condition,
				RepNotifyCondition,
				bIsPushBased));
		}

		Current = Cast<UASClass>(Current->GetSuperStruct());
	}

	if (PendingScriptLifetimeProps.Num() > 0)
	{
		OutLifetimeProps.Append(MoveTemp(PendingScriptLifetimeProps));
	}
	return true;
}
                                                                                                                   
                                                                                                                               
#if 0
                                                                                                                             
UClass* UASClass::GetMostUpToDateClass()
                                                                                                                               
#endif
UClass* UASClass::GetAuthoritativeClass()
                                                                                                                             
{
#if !AS_CAN_HOTRELOAD
	return this;
#else
                                                                                                               
	TSet<const UASClass*> Visited;
	for (const UASClass* Current = this; Current != nullptr; Current = Current->NewerVersion)
	{
		if (Visited.Contains(Current))
		{
			ensureMsgf(false, TEXT("Detected a cycle in UASClass::NewerVersion."));
			return this;
		}
		Visited.Add(Current);
	}
                                                                                                             
	if (NewerVersion == nullptr)
		return this;

	UASClass* NewerClass = NewerVersion;
	while (NewerClass->NewerVersion != nullptr)
		NewerClass = NewerClass->NewerVersion;
	return NewerClass;
#endif
}

UASClass* UASClass::GetFirstASClass(UClass* Class)
{
	UClass* Parent = Class;
	while (Parent != nullptr)
	{
		if (Cast<UASClass>(Parent) != nullptr)
			return (UASClass*)Parent;
		Parent = Parent->GetSuperClass();
	}
	return nullptr;
}

UASClass* UASClass::GetFirstASClass(UObject* Object)
{
	UClass* Parent = Object->GetClass();
	while (Parent != nullptr)
	{
		if (Cast<UASClass>(Parent) != nullptr)
			return (UASClass*)Parent;
		Parent = Parent->GetSuperClass();
	}
	return nullptr;
}

UClass* UASClass::GetFirstASOrNativeClass(UClass* Class)
{
	UClass* Parent = Class;
	while (Parent != nullptr)
	{
		if (Cast<UASClass>(Parent) != nullptr)
			return Parent;
		if (Parent->HasAnyClassFlags(CLASS_Native))
			return Parent;
		Parent = Parent->GetSuperClass();
	}
	return nullptr;
}

void UASClass::RuntimeDestroyObject(UObject* Object)
{
                                                                                                              
#if 0
                                                                                                            
	if (ScriptTypePtr == nullptr)
		return;

	auto* ScriptObject = (asCScriptObject*)(Object);
	ScriptObject->CallDestructor((asCObjectType*)ScriptTypePtr);
                                                                                                               
#endif
	FAngelscriptExecutionWriteScope TypeLifetimeScope;
#if SOLOAS_LEGACY_TERMINAL_LIFECYCLE
    DestroyTrackedRuntimeObjectExplicitly(Object);
#else
    if (asITypeInfo* Type = ScriptTypePtr ? ScriptTypePtr : RetiredScriptTypeForGc)
        RuntimeDestroyObject(Object, Type);
#endif
                                                                                                             
}

bool UASClass::IsFunctionImplementedInScript(FName InFunctionName) const
{
	UFunction* Function = FindFunctionByName(InFunctionName);
                                                                                                                    
#if 0
                                                                                                                  
	return Function && Function->GetOuterUClass() && Function->GetOuterUClass()->bIsScriptClass;
                                                                                                                
#endif
	return Function
		&& GetAngelscriptClassScriptType(Function->GetOuterUClass()) != nullptr;
                                                                                                              
}

static thread_local TArray<FObjectInitializer> CurrentObjectInitializers;
thread_local bool GConstructASObjectWithoutDefaults = false;

UObject* UASClass::GetConstructingASObject()
{
	if (UASClass::OverrideConstructingObject != nullptr)
		return UASClass::OverrideConstructingObject;

	FUObjectThreadContext& ThreadContext = FUObjectThreadContext::Get();
	auto* Initializer = ThreadContext.TopInitializer();
	if (Initializer == nullptr)
		return nullptr;
	UObject* Object = Initializer->GetObj();
	if (Object == nullptr)
		return nullptr;
                                                                                                                
#if 0
                                                                                                              
	if (Object->GetClass()->ScriptTypePtr == nullptr)
                                                                                                            
#endif
	if (GetAngelscriptClassScriptType(Object->GetClass()) == nullptr)
                                                                                                          
		return nullptr;
	return Object;
}

static thread_local UObject* GASDefaultConstructorOuter = nullptr;
UASClass::FScopeSetDefaultConstructorOuter::FScopeSetDefaultConstructorOuter(UObject* NewOuter)
{
	PrevOuter = GASDefaultConstructorOuter;
	GASDefaultConstructorOuter = NewOuter;
}

UASClass::FScopeSetDefaultConstructorOuter::~FScopeSetDefaultConstructorOuter()
{
	GASDefaultConstructorOuter = PrevOuter;
}

UObject* UASClass::GetDefaultConstructorOuter()
{
	return GASDefaultConstructorOuter != nullptr ? GASDefaultConstructorOuter : GetTransientPackage();
}

void* UASClass::AllocScriptObject(class asITypeInfo* ScriptType, size_t Size)
{
	if (ScriptType->GetFlags() & asOBJ_VALUE)
	{
		void* Mem = FMemory::Malloc(Size, ScriptType->alignment);
		FMemory::Memzero(Mem, Size);
		return Mem;
	}

	UASClass* Class = (UASClass*)ScriptType->GetUserData();
	/*
	
		This code comes from StaticConstructObject_Internal.

		In order to split it into an allocate and a native construct part
		it has been copied and split into AllocScriptObject and FinishConstructObject.

	*/

	auto* InClass = Class;
	auto* InOuter = GetDefaultConstructorOuter();
	auto InName = NAME_None;
	EObjectFlags InFlags = RF_NoFlags;
	auto InternalSetFlags = EInternalObjectFlags::None;
	UObject* InTemplate = nullptr;
	bool bCopyTransientsFromClassDefaults = false;
	FObjectInstancingGraph* InInstanceGraph = nullptr;
	bool bAssumeTemplateIsArchetype = false;

	UObject* Result = NULL;

	if (InOuter == GetTransientPackage())
		InFlags |= RF_Transient;

	// Subobjects are always created in the constructor, no need to re-create them unless their archetype != CDO or they're blueprint generated.
	// If the existing subobject is to be re-used it can't have BeginDestroy called on it so we need to pass this information to StaticAllocateObject.	
	const bool bIsNativeClass = InClass->HasAnyClassFlags(CLASS_Native | CLASS_Intrinsic);
	const bool bIsNativeFromCDO = bIsNativeClass &&
		(
			!InTemplate || 
			(InName != NAME_None && (bAssumeTemplateIsArchetype || InTemplate == UObject::GetArchetypeFromRequiredInfo(InClass, InOuter, InName, InFlags)))
		);
	const bool bCanRecycleSubObjects = false; // No recycling, we won't know we've done it in FinishConstructObject
	bool bRecycledSubobject = false;	
	Result = StaticAllocateObject(InClass, InOuter, InName, InFlags, InternalSetFlags, bCanRecycleSubObjects, &bRecycledSubobject);
	check(Result != NULL);

	// We delay destroying the initializer until we know our constructor has been called,
	// since it's script calling the constructor instead of our ClassConstructor static function.
	FObjectInitializer& Initializer = CurrentObjectInitializers.Emplace_GetRef(
		Result, nullptr, EObjectInitializerOptions::InitializeProperties, nullptr
	);

	(*Class->ClassConstructor)( Initializer );

	return Result;
}

static FORCEINLINE_DEBUGGABLE void ExecuteDefaultsFunctions(UObject* Object, UASClass* Class)
{
	SoloASBridge::FScopedScriptDefaultConstruction NativeDefaultCalls(Object);
	UASClass* DefaultsClass = Class;
	UASClass* ParentDefaults = Cast<UASClass>(Class->GetSuperClass());

	if (ParentDefaults == nullptr)
	{
		if (Class->DefaultsFunction != nullptr)
		{
			FAngelscriptContext Context(Object);
			Context->Prepare(Class->DefaultsFunction);
			Context->m_executeVirtualCall = false;
			Context->SetObject(Object);
			Context->Execute();
		}
	}
	else
	{
		TArray<asIScriptFunction*, TFixedAllocator<32>> DefaultsFunctions;
		while (DefaultsClass != nullptr)
		{
			if (DefaultsClass->DefaultsFunction != nullptr)
				DefaultsFunctions.Add(DefaultsClass->DefaultsFunction);
			DefaultsClass = Cast<UASClass>(DefaultsClass->GetSuperClass());
		}

		for (int32 i = DefaultsFunctions.Num() - 1; i >= 0; --i)
		{
			FAngelscriptContext Context(Object);
			Context->Prepare(DefaultsFunctions[i]);
			Context->m_executeVirtualCall = false;
			Context->SetObject(Object);
			Context->Execute();
		}
	}
}

static FORCEINLINE_DEBUGGABLE void ExecuteConstructFunction(UObject* Object, UASClass* Class)
{
                                                                                              
    AngelscriptReplicationShadow::PrepareObject(Object);
                                                                                            
	                                                                                                           
	SoloASBridge::FScopedScriptDefaultConstruction NativeConstructCalls(Object);
	                                                                                                         
	if (Class->ConstructFunction != nullptr)
	{
		FAngelscriptContext Context(Object);
		Context->Prepare(Class->ConstructFunction);
		Context->SetObject(Object);
		Context->Execute();
	}
}

void UASClass::FinishConstructObject(class asIScriptObject* ScriptObject, class asITypeInfo* ScriptType)
{
                                                                                                                
	FAngelscriptExecutionReadScope FinishTypeLifetimeScope;
                                                                                                              
	UObject* Object = FAngelscriptManager::AngelscriptToUObject(ScriptObject);
	const bool bIsScriptAllocation = CurrentObjectInitializers.Num() != 0 && CurrentObjectInitializers.Last().GetObj() == Object;

	if (bIsScriptAllocation)
	{
		UASClass* TopClass = UASClass::GetFirstASClass(Object);

#if AS_CAN_HOTRELOAD
		bool bIsInTree = false;
		UASClass* CheckClass = TopClass;
		while (CheckClass != nullptr)
		{
			if (CheckClass->ScriptTypePtr == ScriptType)
			{
				bIsInTree = true;
				break;
			}
			CheckClass = Cast<UASClass>(CheckClass->GetSuperClass());
		}

		if (!bIsInTree)
		{
			CurrentObjectInitializers.RemoveAt(CurrentObjectInitializers.Num() - 1, 1, EAllowShrinking::No);
			return;
		}
#endif

		if (TopClass->ScriptTypePtr == ScriptType)
		{
			CurrentObjectInitializers.RemoveAt(CurrentObjectInitializers.Num() - 1, 1, EAllowShrinking::No);

			// Run the defaults function now that we've finished constructing the childmost script class
			ExecuteDefaultsFunctions(Object, TopClass);
                                                                                                          
			AngelscriptRuntimeGcMirror::TrackRuntimeObjectWithMirror(
				Object,
				ScriptType
			);
                                                                                                        
		}
	}
}
                                                                                                         
void UASClass::ReconstructScriptObject(
	UObject* Object,
	asITypeInfo* OldType,
	asITypeInfo* NewType
)
{
	FAngelscriptExecutionWriteScope TypeLifetimeScope;
	if (
		OldType == nullptr
		|| NewType == nullptr
		|| OldType->GetUserData() != this
		|| NewType->GetUserData() != this
	)
	{
		RejectUnknownNonGcOverwrite(
			Object,
			TEXT("script object reconstruction type identity")
		);
		return;
	}
	const bool bCompleteOwnedFields = SOLOAS_LEGACY_TERMINAL_LIFECYCLE && OldType == NewType && !GExitPurge;
	FSameCookieReconstructionFieldPlan OwnedFieldPlan;
	if (bCompleteOwnedFields && !CaptureSameCookieReconstructionFields(Object, this, OldType, OwnedFieldPlan))
	{
		RejectUnknownNonGcOverwrite(Object, TEXT("same-cookie owned-field capture"));
		return;
	}

	if (!AngelscriptRuntimeGcMirror::ClaimRuntimeTrackedMirrorForReconstruction(
			Object,
			OldType
		))
	{
		verifyf(false, TEXT("RECONSTRUCT_REENTRANT"));
		AngelscriptRuntimeGcMirror::FailRuntimeMirror(
			TEXT("RECONSTRUCT_REENTRANT"),
			Object
		);
		return;
	}

	// RUNTIME_MIRROR_LOCK_RELEASED_BEFORE_SCRIPT
	if (!RuntimeDestroyObject(Object, OldType))
	{
		RejectUnknownNonGcOverwrite(
			Object,
			TEXT("script object reconstruction")
		);
		return;
	}
	if (bCompleteOwnedFields && !CompleteSameCookieReconstructionFields(OwnedFieldPlan))
	{
		RejectUnknownNonGcOverwrite(Object, TEXT("same-cookie owned-field completion"));
		return;
	}

    AngelscriptRuntimeGcMirror::ResetRuntimeScriptDestructor(Object);
	// ConstructScriptObject(Object, NewType) uses placement construction below.
	if (OldType != NewType)
	{
		new(Object) asCScriptObject(
			static_cast<asCObjectType*>(NewType)
		);
	}
	ExecuteConstructFunction(Object, this);
	// ApplyScriptDefaults(Object, NewType) uses the Runtime defaults chain.
	ExecuteDefaultsFunctions(Object, this);
	if (!AngelscriptRuntimeGcMirror::CommitRuntimeTrackedObjectReconstruction(
			Object,
			OldType,
			NewType
		))
	{
		RejectUnknownNonGcOverwrite(
			Object,
			TEXT("MIRROR_COOKIE_UPDATE_FAILED")
		);
	}
	// The mirror commit restores Pending only after construction and defaults.
}

static bool IsScriptRuntimeScalarProperty(
	const FProperty* Property,
	const UASClass* ScriptClass,
	const asITypeInfo* TypeCookie)
{
	const UClass* OwnerClass = Property == nullptr
		? nullptr
		: Property->GetOwnerClass();
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

static FString GetRuntimeEnumSignature(const UEnum* Enum)
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

static const UEnum* GetRuntimeEnumForProperty(const FProperty* Property)
{
	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		return EnumProperty->GetEnum();
	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		return ByteProperty->Enum;
	return nullptr;
}

static void RestoreScriptRuntimeScalarState(
	UObject* Object,
	UASClass* ScriptClass,
	asITypeInfo* TypeCookie,
	const TArray<AngelscriptRuntimeGcMirror::FReinstancedScriptScalarSnapshot>&
		Snapshots)
{
	if (Object == nullptr
		|| ScriptClass == nullptr
		|| Object->GetClass() != ScriptClass)
	{
		return;
	}

	for (const AngelscriptRuntimeGcMirror::FReinstancedScriptScalarSnapshot& Snapshot :
		Snapshots)
	{
		FProperty* CurrentProperty = ScriptClass->FindPropertyByName(
			Snapshot.PropertyName);
		if (CurrentProperty == nullptr
			|| !IsScriptRuntimeScalarProperty(
				CurrentProperty,
				ScriptClass,
				TypeCookie)
			|| static_cast<const void*>(CurrentProperty->GetOwnerStruct())
				!= Snapshot.ExpectedNewOwnerIdentity
			|| CurrentProperty->GetClass() != Snapshot.PropertyClass
			|| GetAngelscriptPropertyTraits(CurrentProperty)
				!= Snapshot.PropertyTraits
			|| CurrentProperty->GetOffset_ForInternal() != Snapshot.Offset
			|| CurrentProperty->GetSize() != Snapshot.Size
			|| CurrentProperty->GetElementSize() != Snapshot.ElementSize
			|| CurrentProperty->ArrayDim != Snapshot.ArrayDim
			|| static_cast<uint64>(CurrentProperty->GetPropertyFlags())
				!= Snapshot.PropertyFlags)
		{
			continue;
		}
		const UEnum* CurrentEnum = GetRuntimeEnumForProperty(CurrentProperty);
		const FFieldClass* CurrentUnderlyingPropertyClass = nullptr;
		if (const FEnumProperty* EnumProperty =
			CastField<FEnumProperty>(CurrentProperty))
		{
			CurrentUnderlyingPropertyClass =
				EnumProperty->GetUnderlyingProperty()->GetClass();
		}
		else if (CurrentEnum != nullptr)
		{
			CurrentUnderlyingPropertyClass = CurrentProperty->GetClass();
		}

		if (Snapshot.Kind ==
			AngelscriptRuntimeGcMirror::EReinstancedScriptScalarKind::Bool)
		{
			FBoolProperty* BoolProperty = CastField<FBoolProperty>(CurrentProperty);
			if (BoolProperty != nullptr
				&& CurrentEnum == nullptr
				&& BoolProperty->IsNativeBool() == Snapshot.bBoolIsNativeBool
				&& static_cast<uint32>(BoolProperty->GetFieldMask())
					== Snapshot.BoolFieldMask
				&& static_cast<uint32>(BoolProperty->GetByteOffset())
					== Snapshot.BoolByteOffset
				&& static_cast<uint32>(BoolProperty->GetBoolFieldSize())
					== Snapshot.BoolFieldSize
				&& static_cast<uint32>(BoolProperty->GetByteMask())
					== Snapshot.BoolByteMask)
			{
				BoolProperty->SetPropertyValue_InContainer(
					Object,
					Snapshot.bBoolValue);
			}
			continue;
		}

		if (Snapshot.PrimitiveBytes.Num() != Snapshot.Size)
			continue;
		if (Snapshot.Kind ==
			AngelscriptRuntimeGcMirror::EReinstancedScriptScalarKind::Enum)
		{
			if (CurrentEnum == nullptr
				|| CurrentUnderlyingPropertyClass
					!= Snapshot.UnderlyingPropertyClass
				|| CurrentEnum->GetPathName() != Snapshot.EnumIdentity
				|| GetRuntimeEnumSignature(CurrentEnum)
					!= Snapshot.EnumSignature)
			{
				continue;
			}
		}
		else if (CurrentEnum != nullptr
			|| CastField<FNumericProperty>(CurrentProperty) == nullptr)
		{
			continue;
		}

		FMemory::Memcpy(
			CurrentProperty->ContainerPtrToValuePtr<void>(Object),
			Snapshot.PrimitiveBytes.GetData(),
			Snapshot.Size);
	}
}

void AngelscriptRuntimeGcMirror::ReconstructReinstancedScriptClassPair(
	UObject* NewObject,
	UASClass* NewClass,
	asITypeInfo* NewTypeCookie,
	const AngelscriptRuntimeGcMirror::FReinstancedScriptPairFinalizationContext& PairContext
)
{
	FAngelscriptExecutionWriteScope TypeLifetimeScope;
	const bool bPairPayloadInvariant = IsInGameThread()
		&& NewObject != nullptr
		&& NewClass != nullptr
		&& NewObject->GetClass() == NewClass
		&& NewTypeCookie != nullptr
		&& NewTypeCookie->GetUserData() == NewClass
		&& NewTypeCookie == NewClass->ScriptTypePtr
		&& FObjectKey(NewObject) == PairContext.NewKey
		&& static_cast<const void*>(NewObject) == PairContext.NewObjectIdentity
		&& PairContext.OldObjectIdentity != nullptr
		&& PairContext.OldTypeCookie != nullptr
		&& PairContext.OldTypeCookie != NewTypeCookie
		&& PairContext.OldTypeCookieUserData != nullptr
		&& PairContext.OldClassIdentity != nullptr
		&& PairContext.OldTypeCookieUserData == PairContext.OldClassIdentity
		&& PairContext.NewClassIdentity == static_cast<const void*>(NewClass)
		&& PairContext.OldKey != PairContext.NewKey
		&& PairContext.CaptureGeneration != 0
		&& PairContext.OldTrackSequence != 0
		&& PairContext.NewTrackSequence != 0;
	if (!bPairPayloadInvariant)
	{
		AngelscriptRuntimeGcMirror::FailRuntimeMirror(
			TEXT("REINSTANCED_PAIR_PAYLOAD_INVARIANT"),
			NewObject
		);
		return;
	}

	TraceScriptConstructionProperty(NewObject, TEXT("pair-before-reconstruction"));
	// Stock has constructed NewObject with NewType and copied reflected values.
	// Pair admission requires its completed initializer and exact Pending mirror;
	// Reserve published the batch and claimed that mirror for finalization.
	// OldType describes the separate old object, never storage in NewObject.
	// Do not destroy or reconstruct the already-live replacement fields here.
	// Keep the captured scalar policy and script class-reference remapping.
	RestoreScriptRuntimeScalarState(
		NewObject,
		NewClass,
		NewTypeCookie,
		PairContext.ScalarSnapshots);
	if (!AngelscriptRuntimeGcMirror::RestoreScriptRuntimeClassReferenceState(
			NewObject,
			NewClass,
			NewTypeCookie,
			PairContext.ClassReferenceSnapshots))
	{
		AngelscriptRuntimeGcMirror::FailRuntimeMirror(
			TEXT("REINSTANCED_PAIR_CLASS_REFERENCE_RESTORE_FAILED"),
			NewObject);
	}
	TraceScriptConstructionProperty(NewObject, TEXT("pair-after-reconstruction"));
}
                                                                                                       

void UASClass::ReconstructScriptObject(UObject* Object)
{
                                                                                                          
#if 0
                                                                                                        
	if (ScriptTypePtr == nullptr)
		return;

	auto* ScriptObject = (asCScriptObject*)(Object);
	ScriptObject->CallDestructor((asCObjectType*)ScriptTypePtr);
	ExecuteConstructFunction(Object, this);
	ExecuteDefaultsFunctions(Object, this);
                                                                                                            
#endif
	asITypeInfo* OldType = static_cast<asITypeInfo*>(ScriptTypePtr);
	asITypeInfo* NewType = static_cast<asITypeInfo*>(ScriptTypePtr);
	ReconstructScriptObject(Object, OldType, NewType);
                                                                                                          
}

static FORCEINLINE_DEBUGGABLE void ApplyOverrideComponents(const FObjectInitializer& Initializer, AActor* Actor, UASClass* ScriptClass)
{
	// Child classes should apply override components first, this is the expected order that C++ does it in
	// The object initializer understands that child overrides are applied before parents
	for(int32 i = 0, Count = ScriptClass->OverrideComponents.Num(); i < Count; ++i)
	{
		auto& Override = ScriptClass->OverrideComponents[i];

		UClass* ComponentClass = Override.ComponentClass;
#if AS_CAN_HOTRELOAD
                                                                                                                        
#if 0
                                                                                                                      
		ComponentClass = ComponentClass->GetMostUpToDateClass();
                                                                                                                    
#endif
		ComponentClass = ComponentClass->GetAuthoritativeClass();
                                                                                                                  
#endif

		Initializer.SetDefaultSubobjectClass(Override.OverrideComponentName, ComponentClass);
	}

	// Parent classes afterward
	if (UASClass* ParentClass = Cast<UASClass>(ScriptClass->GetSuperClass()))
	{
		ApplyOverrideComponents(Initializer, Actor, ParentClass);
	}
}

static FORCEINLINE_DEBUGGABLE void CreateDefaultComponents(const FObjectInitializer& Initializer, AActor* Actor, UASClass* ScriptClass)
{
	// Parent class should get a chance to create components first
	if (UASClass* ParentClass = Cast<UASClass>(ScriptClass->GetSuperClass()))
	{
		CreateDefaultComponents(Initializer, Actor, ParentClass);
	}

	TArray<TPair<USceneComponent*, int32>, TInlineAllocator<4>> DelayedComponentAttach;
	for(int32 i = 0, Count = ScriptClass->DefaultComponents.Num(); i < Count; ++i)
	{
		auto& DefaultComponent = ScriptClass->DefaultComponents[i];

		UActorComponent* Component;

		UClass* ComponentClass = DefaultComponent.ComponentClass;
#if AS_CAN_HOTRELOAD
                                                                                                                         
#if 0
                                                                                                                       
		ComponentClass = ComponentClass->GetMostUpToDateClass();
                                                                                                                     
#endif
		ComponentClass = ComponentClass->GetAuthoritativeClass();
                                                                                                                   
#endif

		if (WITH_EDITOR && DefaultComponent.bEditorOnly)
		{
			Component = (UActorComponent*)Initializer.CreateEditorOnlyDefaultSubobject(
				Actor,
				DefaultComponent.ComponentName,
				ComponentClass,
				false
			);
		}
		else
		{
			Component = (UActorComponent*)Initializer.CreateDefaultSubobject(
				Actor,
				DefaultComponent.ComponentName,
				ComponentClass,
				ComponentClass,
				true,
				false
			);
		}

		// Handle the case where we tried to create an abstract component on a non-abstract actor class,
		// this should give an error.
		AS_ENSURE(Component == nullptr || !Component->GetClass()->HasAnyClassFlags(CLASS_Abstract) || Actor->GetClass()->HasAnyClassFlags(CLASS_Abstract),
			TEXT("Attempted to instantiate abstract component of type %s on non-abstract actor of type %s"),
			*Component->GetClass()->GetName(), *Actor->GetClass()->GetName()
		);

		// Set the new component on the variable in the script class
		UActorComponent** VariablePtr = (UActorComponent**)((SIZE_T)Actor + DefaultComponent.VariableOffset);
		*VariablePtr = Component;

		// Handle attachments for scene components
		if (auto* SceneComponent = Cast<USceneComponent>(Component))
		{
			if (DefaultComponent.bIsRoot)
			{
				auto* PreviousRoot = Actor->GetRootComponent();

				// Component should become the root component
				SceneComponent->SetupAttachment(nullptr);
				Actor->SetRootComponent(SceneComponent);

				// Attach previous root component to this component
				if (PreviousRoot != nullptr)
					PreviousRoot->SetupAttachment(SceneComponent);
			}
			else if (DefaultComponent.Attach == NAME_None)
			{
				if (Actor->GetRootComponent() == nullptr)
				{
					// Component should become the root component, since we don't have any
					SceneComponent->SetupAttachment(nullptr);
					Actor->SetRootComponent(SceneComponent);
				}
				else
				{
					// Component should automatically be attached to the existing root component
					SceneComponent->SetupAttachment(Actor->GetRootComponent(), DefaultComponent.AttachSocket);
				}
			}
			else
			{
				// Attach the component later, when all components have been created
				DelayedComponentAttach.Add( TPair<USceneComponent*, int32>{ SceneComponent, i } );
			}
		}
	}

	for (auto& DelayedAttach : DelayedComponentAttach)
	{
		auto& DefaultComponent = ScriptClass->DefaultComponents[DelayedAttach.Value];

		// Find the component to attach to
		USceneComponent* AttachTo = nullptr;
		for (auto* CheckComponent : Actor->GetComponents())
		{
			if (CheckComponent->GetFName() == DefaultComponent.Attach)
			{
				if (auto* CheckSceneComponent = Cast<USceneComponent>(CheckComponent))
				{
					AttachTo = CheckSceneComponent;
					break;
				}
			}
		}

		// If we can't find the thing to attach to, attach to the root instead
		if (AttachTo == nullptr)
		{
			if (Actor->GetRootComponent() != nullptr)
			{
				DelayedAttach.Key->SetupAttachment(Actor->GetRootComponent(), DefaultComponent.AttachSocket);
			}
			else
			{
				DelayedAttach.Key->SetupAttachment(nullptr);
				Actor->SetRootComponent(DelayedAttach.Key);
			}
		}
		else
		{
			DelayedAttach.Key->SetupAttachment(AttachTo, DefaultComponent.AttachSocket);
		}
	}

	// Fill any override component variables with the right components
	for(int32 i = 0, Count = ScriptClass->OverrideComponents.Num(); i < Count; ++i)
	{
		auto& Override = ScriptClass->OverrideComponents[i];
		UActorComponent** VariablePtr = (UActorComponent**)((SIZE_T)Actor + Override.VariableOffset);

		for (auto* CheckComponent : Actor->GetComponents())
		{
			if (CheckComponent->GetFName() == Override.OverrideComponentName)
			{
				*VariablePtr = CheckComponent;
				break;
			}
		}
	}
}
                                                                                                                             
static FORCEINLINE_DEBUGGABLE void RelinkDefaultComponentProperties(
	AActor* Actor,
	UASClass* ScriptClass
)
{
	if (UASClass* ParentClass = Cast<UASClass>(ScriptClass->GetSuperClass()))
	{
		RelinkDefaultComponentProperties(Actor, ParentClass);
	}

	for (const auto& DefaultComponent : ScriptClass->DefaultComponents)
	{
		UActorComponent* Component = nullptr;
		UClass* ExpectedComponentClass = DefaultComponent.ComponentClass;
#if AS_CAN_HOTRELOAD
		if (ExpectedComponentClass != nullptr)
			ExpectedComponentClass = ExpectedComponentClass->GetAuthoritativeClass();
#endif
		for (UActorComponent* CheckComponent : Actor->GetComponents())
		{
			if (CheckComponent != nullptr
				&& CheckComponent->GetFName() == DefaultComponent.ComponentName
				&& CheckComponent->IsDefaultSubobject()
				&& ExpectedComponentClass != nullptr
				&& CheckComponent->IsA(ExpectedComponentClass))
			{
				Component = CheckComponent;
				break;
			}
		}

		UActorComponent** VariablePtr = (UActorComponent**)(
			(SIZE_T)Actor + DefaultComponent.VariableOffset
		);
		*VariablePtr = Component;
	}

	for (const auto& Override : ScriptClass->OverrideComponents)
	{
		UActorComponent* Component = nullptr;
		UClass* ExpectedComponentClass = Override.ComponentClass;
#if AS_CAN_HOTRELOAD
		if (ExpectedComponentClass != nullptr)
			ExpectedComponentClass = ExpectedComponentClass->GetAuthoritativeClass();
#endif
		for (UActorComponent* CheckComponent : Actor->GetComponents())
		{
			if (CheckComponent != nullptr
				&& CheckComponent->GetFName() == Override.OverrideComponentName
				&& CheckComponent->IsDefaultSubobject()
				&& ExpectedComponentClass != nullptr
				&& CheckComponent->IsA(ExpectedComponentClass))
			{
				Component = CheckComponent;
				break;
			}
		}

		UActorComponent** VariablePtr = (UActorComponent**)(
			(SIZE_T)Actor + Override.VariableOffset
		);
		*VariablePtr = Component;
	}
}
                                                                                                                           

void UASClass::StaticActorConstructor(const FObjectInitializer& Initializer)
{
	UObject* Object = Initializer.GetObj();
                                                                                                                 
	FAngelscriptExecutionReadScope ConstructionTypeLifetimeScope;
                                                                                                               
	UASClass* Class = GetFirstASClass(Object);
                                                                                                                       
	if (Class == nullptr || Class->CodeSuperClass == nullptr
		|| Class->CodeSuperClass->ClassConstructor == nullptr || Class->ScriptTypePtr == nullptr
		|| static_cast<asITypeInfo*>(Class->ScriptTypePtr)->GetUserData() != Class)
	{
		UE_LOG(Angelscript, Fatal, TEXT("ASR-CONSTRUCTION-TYPE-NOT-READY: %s"), *Object->GetPathName());
	}
                                                                                                                     
	asCObjectType* ScriptType = (asCObjectType*)Class->ScriptTypePtr;
	AActor* Actor = (AActor*)Object;

#if AS_CAN_HOTRELOAD
	const bool bApplyDefaults = !GConstructASObjectWithoutDefaults;
	GConstructASObjectWithoutDefaults = false;
#else
	const bool bApplyDefaults = true;
#endif

	// Apply override components
	ApplyOverrideComponents(Initializer, Actor, Class);

	// We need to run the C++ constructor first so everything is valid
	Class->CodeSuperClass->ClassConstructor(Initializer);
                                                                                                     
	DispatchScriptDefaultObjectConstruction(Initializer);
                                                                                                   
	Actor->PrimaryActorTick.bCanEverTick = Class->bCanEverTick;
	Actor->PrimaryActorTick.bStartWithTickEnabled = Class->bStartWithTickEnabled;

	// Construct the C++ part of the angelscript scriptobject
	const bool bIsScriptAllocation = CurrentObjectInitializers.Num() != 0 && CurrentObjectInitializers.Last().GetObj() == Object;

	if (!bIsScriptAllocation && ScriptType != nullptr)
		new(Object) asCScriptObject(ScriptType);

	// Construct any default components we have marked in our hierarchy
	CreateDefaultComponents(Initializer, Actor, Class);

                                                                                                                               
#if 0
                                                                                                                             
	// Call the script constructor function
	if (!bIsScriptAllocation)
		ExecuteConstructFunction(Object, Class);

	// Apply any default statements to the object
	if (bApplyDefaults && !bIsScriptAllocation)
		ExecuteDefaultsFunctions(Object, Class);
                                                                                                                    
#endif
                                                                                                                  
                                                                                                                 
	const bool bOrdinaryBlueprintDefault = Object->HasAnyFlags(RF_ClassDefaultObject)
		&& Object->GetClass()->GetClass() == UBlueprintGeneratedClass::StaticClass();
	const bool bConstructedBeforePropertyInit = !bIsScriptAllocation
		&& (bOrdinaryBlueprintDefault
			|| ConstructingScriptClassDefaultObject == Object
			|| Cast<UASClass>(Object->GetClass()) != nullptr
			|| Object->GetClass()->GetClass() == USoloASScriptBlueprintGeneratedClass::StaticClass());
	if (bConstructedBeforePropertyInit)
	{
		RelinkDefaultComponentProperties(Actor, Class);
		ExecuteConstructFunction(Object, Class);
		// The engine copies inherited Blueprint overrides after this constructor.
		if (bOrdinaryBlueprintDefault && bApplyDefaults)
			ExecuteDefaultsFunctions(Object, Class);
	}
                                                                                                               
                                                                                                           
	if (!bIsScriptAllocation)
	{
		UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(Object->GetClass());
		if (BlueprintClass != nullptr && !BlueprintClass->IsChildOf(Class))
		{
			BlueprintClass = nullptr;
		}
		UObject* BlueprintDefault = BlueprintClass != nullptr
			? BlueprintClass->GetDefaultObject(false)
			: nullptr;
		// Actor construction runs after stock property initialization.
		const_cast<FObjectInitializer&>(Initializer).AddPropertyPostInitCallback(
			[Object, bApplyDefaults, bConstructedBeforePropertyInit, bOrdinaryBlueprintDefault, BlueprintClass, BlueprintDefault]()
			{
				FAngelscriptExecutionReadScope PostInitTypeLifetimeScope;
				UASClass* Anchor = GetFirstASClass(Object);
				asITypeInfo* Type =
					GetAngelscriptClassScriptType(Anchor);
				const bool bCurrentScriptIdentity = Anchor != nullptr
					&& Type != nullptr
					&& Type->GetUserData() == Anchor;
				checkf(
					bCurrentScriptIdentity,
					TEXT("Runtime post-init script identity changed.")
				);
				if (!bCurrentScriptIdentity)
				{
					return;
				}
				if (bConstructedBeforePropertyInit)
				{
					// Stock reinitializes default subobjects from their templates before this callback.
					// Apply actor defaults once, after those copies, to the final component receivers.
					RelinkDefaultComponentProperties(CastChecked<AActor>(Object), Anchor);
					if (bApplyDefaults && !bOrdinaryBlueprintDefault)
						ExecuteDefaultsFunctions(Object, Anchor);
					AngelscriptRuntimeGcMirror::TrackRuntimeObjectWithMirror(Object, Type);
					return;
				}
				TraceScriptConstructionProperty(Object, TEXT("callback-entry"));
				const TArray<FScriptStockPropertySnapshot> StockPropertySnapshots =
					CaptureScriptStockProperties(Object);
				if (AActor* Actor = Cast<AActor>(Object))
				{
					RelinkDefaultComponentProperties(Actor, Anchor);
				}
				ExecuteConstructFunction(Object, Anchor);
				TraceScriptConstructionProperty(Object, TEXT("after-construct"));
				if (bApplyDefaults)
					ExecuteDefaultsFunctions(Object, Anchor);
				TraceScriptConstructionProperty(
					Object,
					bApplyDefaults ? TEXT("after-defaults") : TEXT("defaults-skipped"));
				RestoreScriptStockProperties(Object, StockPropertySnapshots);
				RestoreScriptBlueprintDefaultProperties(Object, Anchor, BlueprintClass, BlueprintDefault);
				TraceScriptConstructionProperty(Object, TEXT("after-stock-restore"));
				AngelscriptRuntimeGcMirror::TrackRuntimeObjectWithMirror(
					Object, Type);
			});
	}
                                                                                                         
}

void UASClass::StaticComponentConstructor(const FObjectInitializer& Initializer)
{
	UObject* Object = Initializer.GetObj();
                                                                                                                     
	FAngelscriptExecutionReadScope ConstructionTypeLifetimeScope;
                                                                                                                   
	UASClass* Class = GetFirstASClass(Object);
                                                                                                                           
	if (Class == nullptr || Class->CodeSuperClass == nullptr
		|| Class->CodeSuperClass->ClassConstructor == nullptr || Class->ScriptTypePtr == nullptr
		|| static_cast<asITypeInfo*>(Class->ScriptTypePtr)->GetUserData() != Class)
	{
		UE_LOG(Angelscript, Fatal, TEXT("ASR-CONSTRUCTION-TYPE-NOT-READY: %s"), *Object->GetPathName());
	}
                                                                                                                         
	asCObjectType* ScriptType = (asCObjectType*)Class->ScriptTypePtr;
	UActorComponent* Component = (UActorComponent*)Object;

#if AS_CAN_HOTRELOAD
	const bool bApplyDefaults = !GConstructASObjectWithoutDefaults;
	GConstructASObjectWithoutDefaults = false;
#else
	const bool bApplyDefaults = true;
#endif

	// We need to run the C++ constructor first so everything is valid
	Class->CodeSuperClass->ClassConstructor(Initializer);
                                                                                                         
	DispatchScriptDefaultObjectConstruction(Initializer);
                                                                                                       
	Component->PrimaryComponentTick.bCanEverTick = Class->bCanEverTick;
	Component->PrimaryComponentTick.bStartWithTickEnabled = Class->bStartWithTickEnabled;

	// Construct the C++ part of the angelscript scriptobject
	const bool bIsScriptAllocation = CurrentObjectInitializers.Num() != 0 && CurrentObjectInitializers.Last().GetObj() == Object;

	if (!bIsScriptAllocation && ScriptType != nullptr)
		new(Object) asCScriptObject(ScriptType);

                                                                                                                                   
#if 0
                                                                                                                                 
	// Call the script constructor function
	if (!bIsScriptAllocation)
		ExecuteConstructFunction(Object, Class);

	// Apply any default statements to the object
	if (bApplyDefaults && !bIsScriptAllocation)
		ExecuteDefaultsFunctions(Object, Class);
                                                                                                                        
#endif
                                                                                                                      
                                                                                                                     
	const bool bConstructedBeforePropertyInit = !bIsScriptAllocation
		&& (ConstructingScriptClassDefaultObject == Object
			|| Cast<UASClass>(Object->GetClass()) != nullptr
			|| Object->GetClass()->GetClass() == USoloASScriptBlueprintGeneratedClass::StaticClass());
	if (bConstructedBeforePropertyInit)
	{
		ExecuteConstructFunction(Object, Class);
		if (bApplyDefaults)
			ExecuteDefaultsFunctions(Object, Class);
	}
                                                                                                                   
                                                                                                               
	if (!bIsScriptAllocation)
	{
		UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(Object->GetClass());
		if (BlueprintClass != nullptr && !BlueprintClass->IsChildOf(Class))
		{
			BlueprintClass = nullptr;
		}
		UObject* BlueprintDefault = BlueprintClass != nullptr
			? BlueprintClass->GetDefaultObject(false)
			: nullptr;
		// Component construction runs after stock property initialization.
		const_cast<FObjectInitializer&>(Initializer).AddPropertyPostInitCallback(
			[Object, bApplyDefaults, bConstructedBeforePropertyInit, BlueprintClass, BlueprintDefault]()
			{
				FAngelscriptExecutionReadScope PostInitTypeLifetimeScope;
				UASClass* Anchor = GetFirstASClass(Object);
				asITypeInfo* Type =
					GetAngelscriptClassScriptType(Anchor);
				const bool bCurrentScriptIdentity = Anchor != nullptr
					&& Type != nullptr
					&& Type->GetUserData() == Anchor;
				checkf(
					bCurrentScriptIdentity,
					TEXT("Runtime post-init script identity changed.")
				);
				if (!bCurrentScriptIdentity)
				{
					return;
				}
				if (bConstructedBeforePropertyInit)
				{
					AngelscriptRuntimeGcMirror::TrackRuntimeObjectWithMirror(Object, Type);
					return;
				}
				TraceScriptConstructionProperty(Object, TEXT("callback-entry"));
				const TArray<FScriptStockPropertySnapshot> StockPropertySnapshots =
					CaptureScriptStockProperties(Object);
				ExecuteConstructFunction(Object, Anchor);
				TraceScriptConstructionProperty(Object, TEXT("after-construct"));
				if (bApplyDefaults)
					ExecuteDefaultsFunctions(Object, Anchor);
				TraceScriptConstructionProperty(
					Object,
					bApplyDefaults ? TEXT("after-defaults") : TEXT("defaults-skipped"));
				RestoreScriptStockProperties(Object, StockPropertySnapshots);
				RestoreScriptBlueprintDefaultProperties(Object, Anchor, BlueprintClass, BlueprintDefault);
				TraceScriptConstructionProperty(Object, TEXT("after-stock-restore"));
				AngelscriptRuntimeGcMirror::TrackRuntimeObjectWithMirror(
					Object, Type);
			});
	}
                                                                                                             
}

void UASClass::StaticObjectConstructor(const FObjectInitializer& Initializer)
{
	UObject* Object = Initializer.GetObj();
                                                                                                                  
	FAngelscriptExecutionReadScope ConstructionTypeLifetimeScope;
                                                                                                                
	UASClass* Class = GetFirstASClass(Object);
                                                                                                                        
	if (Class == nullptr || Class->CodeSuperClass == nullptr
		|| Class->CodeSuperClass->ClassConstructor == nullptr
		|| (Class->bIsModuleStaticCarrier
			? (Class->ScriptTypePtr != nullptr || Class->CodeSuperClass != UObject::StaticClass())
			: (Class->ScriptTypePtr == nullptr || Class->ScriptTypePtr->GetUserData() != Class)))
	{
		UE_LOG(Angelscript, Fatal, TEXT("ASR-CONSTRUCTION-TYPE-NOT-READY: %s"), *Object->GetPathName());
	}
                                                                                                                      
	asCObjectType* ScriptType = (asCObjectType*)Class->ScriptTypePtr;

#if AS_CAN_HOTRELOAD
	const bool bApplyDefaults = !GConstructASObjectWithoutDefaults;
	GConstructASObjectWithoutDefaults = false;
#else
	const bool bApplyDefaults = true;
#endif

	// We need to run the C++ constructor first so everything is valid
	Class->CodeSuperClass->ClassConstructor(Initializer);
                                                                                                      
	DispatchScriptDefaultObjectConstruction(Initializer);
                                                                                                    

	// Construct the C++ part of the angelscript scriptobject
	const bool bIsScriptAllocation = CurrentObjectInitializers.Num() != 0 && CurrentObjectInitializers.Last().GetObj() == Object;

	if (!bIsScriptAllocation && ScriptType != nullptr)
		new(Object) asCScriptObject(ScriptType);

                                                                                                                                
#if 0
                                                                                                                              
	// Call the script constructor function
	if (!bIsScriptAllocation)
		ExecuteConstructFunction(Object, Class);

	// Apply any default statements to the object
	if (bApplyDefaults && !bIsScriptAllocation)
		ExecuteDefaultsFunctions(Object, Class);
                                                                                                                     
#endif
                                                                                                                   
                                                                                                                  
	const bool bConstructedBeforePropertyInit = !bIsScriptAllocation
		&& (ConstructingScriptClassDefaultObject == Object
			|| Cast<UASClass>(Object->GetClass()) != nullptr
			|| Object->GetClass()->GetClass() == USoloASScriptBlueprintGeneratedClass::StaticClass());
	if (bConstructedBeforePropertyInit)
	{
		ExecuteConstructFunction(Object, Class);
		if (bApplyDefaults)
			ExecuteDefaultsFunctions(Object, Class);
	}
                                                                                                                
                                                                                                            
	if (!bIsScriptAllocation)
	{
		if (ScriptType == nullptr)
		{
			return;
		}
		UBlueprintGeneratedClass* BlueprintClass = Cast<UBlueprintGeneratedClass>(Object->GetClass());
		if (BlueprintClass != nullptr && !BlueprintClass->IsChildOf(Class))
		{
			BlueprintClass = nullptr;
		}
		UObject* BlueprintDefault = BlueprintClass != nullptr
			? BlueprintClass->GetDefaultObject(false)
			: nullptr;
		// Object construction runs after stock property initialization.
		const_cast<FObjectInitializer&>(Initializer).AddPropertyPostInitCallback(
			[Object, bApplyDefaults, bConstructedBeforePropertyInit, BlueprintClass, BlueprintDefault]()
			{
				FAngelscriptExecutionReadScope PostInitTypeLifetimeScope;
				UASClass* Anchor = GetFirstASClass(Object);
				asITypeInfo* Type =
					GetAngelscriptClassScriptType(Anchor);
				const bool bCurrentScriptIdentity = Anchor != nullptr
					&& Type != nullptr
					&& Type->GetUserData() == Anchor;
				checkf(
					bCurrentScriptIdentity,
					TEXT("Runtime post-init script identity changed.")
				);
				if (!bCurrentScriptIdentity)
				{
					return;
				}
				if (bConstructedBeforePropertyInit)
				{
					AngelscriptRuntimeGcMirror::TrackRuntimeObjectWithMirror(Object, Type);
					return;
				}
				TraceScriptConstructionProperty(Object, TEXT("callback-entry"));
				const TArray<FScriptStockPropertySnapshot> StockPropertySnapshots =
					CaptureScriptStockProperties(Object);
				ExecuteConstructFunction(Object, Anchor);
				TraceScriptConstructionProperty(Object, TEXT("after-construct"));
				if (bApplyDefaults)
					ExecuteDefaultsFunctions(Object, Anchor);
				TraceScriptConstructionProperty(
					Object,
					bApplyDefaults ? TEXT("after-defaults") : TEXT("defaults-skipped"));
				RestoreScriptStockProperties(Object, StockPropertySnapshots);
				RestoreScriptBlueprintDefaultProperties(Object, Anchor, BlueprintClass, BlueprintDefault);
				TraceScriptConstructionProperty(Object, TEXT("after-stock-restore"));
				AngelscriptRuntimeGcMirror::TrackRuntimeObjectWithMirror(
					Object, Type);
			});
	}
                                                                                                          
}

static FORCEINLINE_DEBUGGABLE void LinkBindComponents(AActor* Actor, UASClass* Class)
{
	// Parent class should get a chance to bind components too
	if (UASClass* ParentClass = Cast<UASClass>(Class->GetSuperClass()))
	{
		LinkBindComponents(Actor, ParentClass);
	}
	
	for (const auto& BindComponent : Class->BindComponents)
	{
		UClass* ComponentClass = BindComponent.ComponentClass;
#if AS_CAN_HOTRELOAD
                                                                                                                               
#if 0
                                                                                                                             
		ComponentClass = ComponentClass->GetMostUpToDateClass();
                                                                                                                           
#endif
		ComponentClass = ComponentClass->GetAuthoritativeClass();
                                                                                                                         
#endif

		// Find the component we want to bind
		UActorComponent* ComponentInstance = nullptr;
		for (auto* CheckComponent : Actor->GetComponents())
		{
			if (IsValid(CheckComponent)
				&& CheckComponent->GetFName() == BindComponent.ComponentName
				&& CheckComponent->IsA(ComponentClass))
			{
				ComponentInstance = CheckComponent;
				break;
			}
		}

		// Set the new component on the variable in the script class
		UObject** VariablePtr = (UObject**)((SIZE_T)Actor + BindComponent.VariableOffset);
		*VariablePtr = ComponentInstance;
	}
}

                                                                                              
#include "ClassGenerator/AngelscriptActorBindingState.h"

bool UASClass::EnsureBindComponentsLinked(AActor* Actor)
{
	checkf(
		IsInGameThread(),
		TEXT("Actor BindComponents linking requires the game thread.")
	);
	if (
		!IsInGameThread()
		|| !IsValid(Actor)
		|| !Actor->IsActorInitialized()
	)
	{
		return false;
	}

	FAngelscriptExecutionReadScope TypeLifetimeScope;
	AngelscriptActorBindingState::PruneInvalidActorBindingStates();
	UASClass* ScriptClass = GetFirstASClass(Actor);
	asITypeInfo* TypeCookie =
		GetAngelscriptClassScriptType(Actor->GetClass());
	if (ScriptClass == nullptr || TypeCookie == nullptr)
	{
		AngelscriptActorBindingState::ForgetActorBindingState(Actor);
		return false;
	}

	UASClass* ClassGeneration =
		Cast<UASClass>(ScriptClass->GetAuthoritativeClass());
	if (ClassGeneration == nullptr)
	{
		ClassGeneration = ScriptClass;
	}

	if (AngelscriptActorBindingState::IsActorBindingStateCurrent(
		Actor,
		ClassGeneration,
		TypeCookie
	))
	{
		return true;
	}

	LinkBindComponents(Actor, ClassGeneration);
	AngelscriptActorBindingState::RememberActorBindingState(
		Actor,
		ClassGeneration,
		TypeCookie
	);
	return true;
}

void UASClass::ResetActorBindingState()
{
	AngelscriptActorBindingState::ResetActorBindingState();
}

bool UASClass::IsActorBindingStateEmpty()
{
	return AngelscriptActorBindingState::IsActorBindingStateEmpty();
}
                                                                                            
void UASClass::OnPostInitializeComponents(AActor* Actor)
{
                                                                                                            
#if 0
                                                                                                          
	if (Actor->GetClass()->ScriptTypePtr != nullptr)
                                                                                                        
#endif
	if (GetAngelscriptClassScriptType(Actor->GetClass()) != nullptr)
                                                                                                      
	{
		if (UASClass* Class = GetFirstASClass(Actor))
		{
			// Link BindComponent properties to the component property
			LinkBindComponents(Actor, Class);
		}
	}
}

#if WITH_EDITOR
FString UASClass::GetSourceFilePath() const
{
	if (ScriptTypePtr == nullptr)
		return TEXT("");
	auto& Manager = FAngelscriptManager::Get();
	auto Module = Manager.GetModule(((asITypeInfo*)ScriptTypePtr)->GetModule());
	if (!Module.IsValid())
		return TEXT("");
	if (Module->Code.Num() == 0)
		return TEXT("");
	return Module->Code[0].AbsoluteFilename;
}

FString UASClass::GetRelativeSourceFilePath() const
{
	if (ScriptTypePtr == nullptr)
		return TEXT("");
	auto& Manager = FAngelscriptManager::Get();
	auto Module = Manager.GetModule(((asITypeInfo*)ScriptTypePtr)->GetModule());
	if (!Module.IsValid())
		return TEXT("");
	if (Module->Code.Num() == 0)
		return TEXT("");
	return Module->Code[0].RelativeFilename;
}

bool UASClass::IsDeveloperOnly() const
{
	if (ScriptTypePtr == nullptr)
		return false;
	auto& Manager = FAngelscriptManager::Get();
	auto Module = Manager.GetModule(((asITypeInfo*)ScriptTypePtr)->GetModule());
	if (!Module.IsValid())
		return false;
	return Module->ModuleName.StartsWith(TEXT("Dev."))
		|| Module->ModuleName.StartsWith(TEXT("Developers."))
		|| Module->ModuleName.StartsWith(TEXT("Editor."));
}

FString UASFunction::GetRelativeSourceFilePath() const
{
	if (ScriptFunction == nullptr)
		return TEXT("");
	auto& Manager = FAngelscriptManager::Get();
	auto Module = Manager.GetModule(ScriptFunction->GetModule());
	if (!Module.IsValid())
		return TEXT("");
	if (Module->Code.Num() == 0)
		return TEXT("");
	return Module->Code[0].RelativeFilename;
}

FString UASFunction::GetSourceFilePath() const
{
	if (ScriptFunction == nullptr)
		return TEXT("");
	auto& Manager = FAngelscriptManager::Get();
	auto Module = Manager.GetModule(ScriptFunction->GetModule());
	if (!Module.IsValid())
		return TEXT("");
	if (Module->Code.Num() == 0)
		return TEXT("");
	return Module->Code[0].AbsoluteFilename;
}

int UASFunction::GetSourceLineNumber() const
{
	if (ScriptFunction == nullptr)
		return -1;

	auto* RealFunc = ((asCScriptFunction*)ScriptFunction);
	auto* scriptData = RealFunc->scriptData;
	if (scriptData == nullptr)
		return -1;

	return scriptData->declaredAt & 0xFFFFF;
}
#endif

uint8 UASFunction::OptimizedCall_ByteReturn(UObject* Object)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return 0;
#endif

	if (JitFunction_Raw != nullptr)
	{
		return MakeRawJITCall_ReturnValue<asBYTE>(Object, JitFunction_Raw);
	}

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (RealFunction->jitFunction_Raw != nullptr)
	{
		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value1Byte);

		return MakeRawJITCall_ReturnValue<asBYTE>(Object, RealFunction->jitFunction_Raw);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->Execute();

		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value1Byte);

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			return 0;

		return Context->GetReturnByte();
	}
}

void UASFunction::OptimizedCall_FloatArg(UObject* Object, float Argument)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (JitFunction_Raw != nullptr)
	{
		MakeRawJITCall_Arg<float>(Object, JitFunction_Raw, Argument);
		return;
	}

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (RealFunction->jitFunction_Raw != nullptr)
	{
		MakeRawJITCall_Arg<float>(Object, RealFunction->jitFunction_Raw, Argument);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);
		checkSlow(Arguments[0].PosInParmStruct == 0);
		checkSlow(Arguments[0].ParmBehavior == EArgumentParmBehavior::Value4Byte);

		Context->m_regs.stackFramePointer[AS_PTR_SIZE] = (asDWORD&)Argument;

		Context->Execute();
	}
}

void UASFunction::OptimizedCall_DoubleArg(UObject* Object, double Argument)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (JitFunction_Raw != nullptr)
	{
		MakeRawJITCall_Arg<double>(Object, JitFunction_Raw, Argument);
		return;
	}

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		MakeRawJITCall_Arg<double>(Object, JitFunc, Argument);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);
		checkSlow(Arguments[0].PosInParmStruct == 0);
		checkSlow(Arguments[0].ParmBehavior == EArgumentParmBehavior::Value8Byte);

		*(asQWORD*)&Context->m_regs.stackFramePointer[AS_PTR_SIZE] = (asQWORD&)Argument;

		Context->Execute();
	}
}

void UASFunction::OptimizedCall(UObject* Object)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (JitFunction_Raw != nullptr)
	{
		MakeRawJITCall_NoParam(Object, JitFunction_Raw);
		return;
	}

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		MakeRawJITCall_NoParam(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->Execute();
	}
}

uint8 UASFunction::OptimizedCall_RefArg_ByteReturn(UObject* Object, void* Argument)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return 0;
#endif

	if (JitFunction_Raw != nullptr)
	{
		return MakeRawJITCall_Arg_ReturnValue<void*,uint8>(Object, JitFunction_Raw, Argument);
	}

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		return MakeRawJITCall_Arg_ReturnValue<void*,uint8>(Object, JitFunc, Argument);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->SetArgAddress(0, Argument);

		Context->Execute();
		
		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			return 0;

		return Context->GetReturnByte();
	}
}

void UASFunction::OptimizedCall_RefArg(UObject* Object, void* Argument)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (JitFunction_Raw != nullptr)
	{
		MakeRawJITCall_Arg<void*>(Object, JitFunction_Raw, Argument);
		return;
	}

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		MakeRawJITCall_Arg<void*>(Object, JitFunc, Argument);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->SetArgAddress(0, Argument);

		Context->Execute();
	}
}


UASFunction* UASFunction::AllocateFunctionFor(UClass* InClass, FName ObjectName, TSharedPtr<FAngelscriptFunctionDesc> FunctionDesc)
{
	asCScriptFunction* ScriptFunction = (asCScriptFunction*)FunctionDesc->ScriptFunction;
	const bool bHasNonVirtualJitFunction = ScriptFunction != nullptr
		&& ScriptFunction->jitFunction != nullptr
		&& ScriptFunction->jitFunction_Raw != nullptr
		&& ScriptFunction->jitFunction_ParmsEntry != nullptr
		&& ScriptFunction->traits.GetTrait(asTRAIT_FINAL)
	;

	// Thread safe functions must go through the most generic path for calls
	if (FunctionDesc->bThreadSafe)
	{
		if (bHasNonVirtualJitFunction)
			return NewObject<UASFunction_JIT>(InClass, ObjectName, RF_Public);
		else
			return NewObject<UASFunction>(InClass, ObjectName, RF_Public);
	}

	if (!FunctionDesc->bIsStatic)
	{
		// void ScriptFunction();
		if (!FunctionDesc->ReturnType.IsValid()
			&& FunctionDesc->Arguments.Num() == 0)
		{
			if (bHasNonVirtualJitFunction)
				return NewObject<UASFunction_NoParams_JIT>(InClass, ObjectName, RF_Public);
			else
				return NewObject<UASFunction_NoParams>(InClass, ObjectName, RF_Public);
		}

		// void ScriptFunction({PRIMITIVE} Value);
		if (!FunctionDesc->ReturnType.IsValid()
			&& FunctionDesc->Arguments.Num() == 1
			&& !FunctionDesc->Arguments[0].Type.bIsReference
			&& FunctionDesc->Arguments[0].Type.IsPrimitive())
		{
			int32 ArgSize = FunctionDesc->Arguments[0].Type.GetValueSize();
			if (ArgSize == 1)
			{
				if (bHasNonVirtualJitFunction)
					return NewObject<UASFunction_ByteArg_JIT>(InClass, ObjectName, RF_Public);
				else
					return NewObject<UASFunction_ByteArg>(InClass, ObjectName, RF_Public);
			}
			else if (ArgSize == 4)
			{
				if (FunctionDesc->Arguments[0].Type.Type == FAngelscriptType::ScriptFloatType())
				{
					if (bHasNonVirtualJitFunction)
						return NewObject<UASFunction_FloatArg_JIT>(InClass, ObjectName, RF_Public);
					else
						return NewObject<UASFunction_FloatArg>(InClass, ObjectName, RF_Public);
				}
				else if (FunctionDesc->Arguments[0].Type.Type == FAngelscriptType::ScriptFloatParamExtendedToDoubleType())
				{
					if (bHasNonVirtualJitFunction)
						return NewObject<UASFunction_FloatExtendedToDoubleArg_JIT>(InClass, ObjectName, RF_Public);
					else
						return NewObject<UASFunction_FloatExtendedToDoubleArg>(InClass, ObjectName, RF_Public);
				}
				else
				{
					if (bHasNonVirtualJitFunction)
						return NewObject<UASFunction_DWordArg_JIT>(InClass, ObjectName, RF_Public);
					else
						return NewObject<UASFunction_DWordArg>(InClass, ObjectName, RF_Public);
				}
			}
			else if (ArgSize == 8)
			{
				if (FunctionDesc->Arguments[0].Type.Type == FAngelscriptType::ScriptDoubleType())
				{
					if (bHasNonVirtualJitFunction)
						return NewObject<UASFunction_DoubleArg_JIT>(InClass, ObjectName, RF_Public);
					else
						return NewObject<UASFunction_DoubleArg>(InClass, ObjectName, RF_Public);
				}
				else
				{
					if (bHasNonVirtualJitFunction)
						return NewObject<UASFunction_QWordArg_JIT>(InClass, ObjectName, RF_Public);
					else
						return NewObject<UASFunction_QWordArg>(InClass, ObjectName, RF_Public);
				}
			}
		}

		// void ScriptFunction({TYPE}& Value);
		if (!FunctionDesc->ReturnType.IsValid()
			&& FunctionDesc->Arguments.Num() == 1
			&& FunctionDesc->Arguments[0].Type.bIsReference)
		{
			if (bHasNonVirtualJitFunction)
				return NewObject<UASFunction_ReferenceArg_JIT>(InClass, ObjectName, RF_Public);
			else
				return NewObject<UASFunction_ReferenceArg>(InClass, ObjectName, RF_Public);
		}

		// {PRIMITIVE} ScriptFunction();
		if (FunctionDesc->ReturnType.IsValid()
			&& !FunctionDesc->ReturnType.bIsReference
			&& FunctionDesc->ReturnType.IsPrimitive()
			&& FunctionDesc->Arguments.Num() == 0)
		{
			int32 ReturnSize = FunctionDesc->ReturnType.GetValueSize();
			if (ReturnSize == 1)
			{
				if (bHasNonVirtualJitFunction)
					return NewObject<UASFunction_ByteReturn_JIT>(InClass, ObjectName, RF_Public);
				else
					return NewObject<UASFunction_ByteReturn>(InClass, ObjectName, RF_Public);
			}
			else if (ReturnSize == 4)
			{
				if (FunctionDesc->ReturnType.Type == FAngelscriptType::ScriptFloatType())
				{
					if (bHasNonVirtualJitFunction)
						return NewObject<UASFunction_FloatReturn_JIT>(InClass, ObjectName, RF_Public);
					else
						return NewObject<UASFunction_FloatReturn>(InClass, ObjectName, RF_Public);
				}
				else if (FunctionDesc->ReturnType.Type == FAngelscriptType::ScriptFloatParamExtendedToDoubleType())
				{
					if (bHasNonVirtualJitFunction)
						return NewObject<UASFunction_FloatExtendedToDoubleReturn_JIT>(InClass, ObjectName, RF_Public);
					else
						return NewObject<UASFunction_FloatExtendedToDoubleReturn>(InClass, ObjectName, RF_Public);
				}
				else
				{
					if (bHasNonVirtualJitFunction)
						return NewObject<UASFunction_DWordReturn_JIT>(InClass, ObjectName, RF_Public);
					else
						return NewObject<UASFunction_DWordReturn>(InClass, ObjectName, RF_Public);
				}
			}
			else if (ReturnSize == 8)
			{
				if (FunctionDesc->ReturnType.Type == FAngelscriptType::ScriptDoubleType())
				{
					if (bHasNonVirtualJitFunction)
						return NewObject<UASFunction_DoubleReturn_JIT>(InClass, ObjectName, RF_Public);
					else
						return NewObject<UASFunction_DoubleReturn>(InClass, ObjectName, RF_Public);
				}
			}
		}

		// UObject ScriptFunction();
		if (FunctionDesc->ReturnType.IsValid()
			&& !FunctionDesc->ReturnType.bIsReference
			&& FunctionDesc->ReturnType.IsObjectPointer()
			&& FunctionDesc->Arguments.Num() == 0)
		{
			if (bHasNonVirtualJitFunction)
				return NewObject<UASFunction_ObjectReturn_JIT>(InClass, ObjectName, RF_Public);
			else
				return NewObject<UASFunction_ObjectReturn>(InClass, ObjectName, RF_Public);
		}
	}

	// Fallback generic path for any non-thread-safe functions otherwise
	if (bHasNonVirtualJitFunction)
		return NewObject<UASFunction_NotThreadSafe_JIT>(InClass, ObjectName, RF_Public);
	else
		return NewObject<UASFunction_NotThreadSafe>(InClass, ObjectName, RF_Public);
}

void UASFunction::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if WITH_EDITOR
	TGuardValue ScopeThreadSafe(GIsInAngelscriptThreadSafeFunction, true);
#endif

	AngelscriptCallFromBPVM<true, false>(this, Object, Stack, RESULT_PARAM);
}

void UASFunction::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if WITH_EDITOR
	TGuardValue ScopeThreadSafe(GIsInAngelscriptThreadSafeFunction, true);
#endif

	AngelscriptCallFromParms<true, false>(this, Object, Parms);
}

UFunction* UASFunction::GetRuntimeValidateFunction()
{
	return ValidateFunction;
}

void UASFunction_NotThreadSafe::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	AngelscriptCallFromBPVM<false, false>(this, Object, Stack, RESULT_PARAM);
}

void UASFunction_NotThreadSafe::RuntimeCallEvent(UObject* Object, void* Parms)
{
	AngelscriptCallFromParms<false, false>(this, Object, Parms);
}

void UASFunction_NoParams::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		P_FINISH;

		MakeRawJITCall_NoParam(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		P_FINISH;
		Context->Execute();
	}
}

void UASFunction_NoParams::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		MakeRawJITCall_NoParam(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->Execute();
	}
}

void UASFunction_DWordArg::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		asDWORD ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);

		P_FINISH;

		MakeRawJITCall_Arg<asDWORD>(Object, JitFunc, ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);

		asDWORD ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);
		Context->m_regs.stackFramePointer[AS_PTR_SIZE] = ArgumentValue;

		P_FINISH;
		Context->Execute();
	}
}

void UASFunction_DWordArg::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		asDWORD ArgumentValue = *(asDWORD*)Parms;
		MakeRawJITCall_Arg<asDWORD>(Object, JitFunc, ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);
		checkSlow(Arguments[0].PosInParmStruct == 0);
		checkSlow(Arguments[0].ParmBehavior == EArgumentParmBehavior::Value4Byte);

		Context->m_regs.stackFramePointer[AS_PTR_SIZE] = *(asDWORD*)Parms;

		Context->Execute();
	}
}

void UASFunction_FloatArg::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		float ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);

		P_FINISH;

		MakeRawJITCall_Arg<float>(Object, JitFunc, ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);

		asDWORD ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);
		Context->m_regs.stackFramePointer[AS_PTR_SIZE] = ArgumentValue;

		P_FINISH;
		Context->Execute();
	}
}

void UASFunction_FloatArg::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		float ArgumentValue = *(float*)Parms;
		MakeRawJITCall_Arg<float>(Object, JitFunc, ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);
		checkSlow(Arguments[0].PosInParmStruct == 0);
		checkSlow(Arguments[0].ParmBehavior == EArgumentParmBehavior::Value4Byte);

		Context->m_regs.stackFramePointer[AS_PTR_SIZE] = *(asDWORD*)Parms;

		Context->Execute();
	}
}

void UASFunction_DoubleArg::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		double ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);

		P_FINISH;

		MakeRawJITCall_Arg<double>(Object, JitFunc, ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);

		asQWORD ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);
		*(asQWORD*)&Context->m_regs.stackFramePointer[AS_PTR_SIZE] = ArgumentValue;

		P_FINISH;
		Context->Execute();
	}
}

void UASFunction_DoubleArg::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		double ArgumentValue = *(double*)Parms;
		MakeRawJITCall_Arg<double>(Object, JitFunc, ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);
		checkSlow(Arguments[0].PosInParmStruct == 0);
		checkSlow(Arguments[0].ParmBehavior == EArgumentParmBehavior::Value8Byte);

		*(asQWORD*)&Context->m_regs.stackFramePointer[AS_PTR_SIZE] = *(asQWORD*)Parms;

		Context->Execute();
	}
}

void UASFunction_FloatExtendedToDoubleArg::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		float ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);

		P_FINISH;

		MakeRawJITCall_Arg<double>(Object, JitFunc, (double)ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);

		float ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);
		*(double*)&Context->m_regs.stackFramePointer[AS_PTR_SIZE] = (double)ArgumentValue;

		P_FINISH;
		Context->Execute();
	}
}

void UASFunction_FloatExtendedToDoubleArg::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		float ArgumentValue = *(float*)Parms;
		MakeRawJITCall_Arg<double>(Object, JitFunc, (double)ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);
		checkSlow(Arguments[0].PosInParmStruct == 0);
		checkSlow(Arguments[0].ParmBehavior == EArgumentParmBehavior::FloatExtendedToDouble);

		*(double*)&Context->m_regs.stackFramePointer[AS_PTR_SIZE] = (double)*(float*)Parms;

		Context->Execute();
	}
}

void UASFunction_QWordArg::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		asQWORD ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);

		P_FINISH;

		MakeRawJITCall_Arg<asQWORD>(Object, JitFunc, ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);

		asQWORD ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);
		*(asQWORD*)&Context->m_regs.stackFramePointer[AS_PTR_SIZE] = ArgumentValue;

		P_FINISH;
		Context->Execute();
	}
}

void UASFunction_QWordArg::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		asQWORD ArgumentValue = *(asQWORD*)Parms;
		MakeRawJITCall_Arg<asQWORD>(Object, JitFunc, ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);
		checkSlow(Arguments[0].PosInParmStruct == 0);
		checkSlow(Arguments[0].ParmBehavior == EArgumentParmBehavior::Value8Byte);

		*(asQWORD*)&Context->m_regs.stackFramePointer[AS_PTR_SIZE] = *(asQWORD*)Parms;

		Context->Execute();
	}
}

void UASFunction_ByteArg::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		asDWORD ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);

		P_FINISH;

		MakeRawJITCall_Arg<asBYTE>(Object, JitFunc, (asBYTE&)ArgumentValue);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);

		asDWORD ArgumentValue;
		Stack.StepCompiledIn<FProperty>(&ArgumentValue);
		*(asBYTE*)&Context->m_regs.stackFramePointer[AS_PTR_SIZE] = *(asBYTE*)&ArgumentValue;

		P_FINISH;
		Context->Execute();
	}
}

void UASFunction_ByteArg::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		MakeRawJITCall_Arg<asBYTE>(Object, JitFunc, *(asBYTE*)Parms);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Context->m_returnValueSize == 0);
		checkSlow(Arguments[0].PosInParmStruct == 0);
		checkSlow(Arguments[0].ParmBehavior == EArgumentParmBehavior::Value1Byte);

		*(asBYTE*)&Context->m_regs.stackFramePointer[AS_PTR_SIZE] = *(asBYTE*)Parms;

		Context->Execute();
	}
}

void UASFunction_ReferenceArg::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	AngelscriptCallFromBPVM<false, false>(this, Object, Stack, RESULT_PARAM);
}

void UASFunction_ReferenceArg::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
		return;
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		MakeRawJITCall_Arg<void*>(Object, JitFunc, Parms);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		checkSlow(Arguments[0].PosInParmStruct == 0);

		*(asPWORD*)&Context->m_regs.stackFramePointer[AS_PTR_SIZE] = (asPWORD)Parms;

		Context->Execute();
	}
}

void UASFunction_ObjectReturn::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(UObject**)RESULT_PARAM = nullptr;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		P_FINISH;

		*(UObject**)RESULT_PARAM = MakeRawJITCall_ReturnValue<UObject*>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		P_FINISH;
		Context->Execute();

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(UObject**)RESULT_PARAM = nullptr;
		else
			*(UObject**)RESULT_PARAM = (UObject*)Context->GetReturnAddress();
	}
}

void UASFunction_ObjectReturn::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(UObject**)Parms = nullptr;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::ReturnObjectPointer);

		*(UObject**)Parms = MakeRawJITCall_ReturnValue<UObject*>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->Execute();

		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::ReturnObjectPointer);

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(UObject**)Parms = nullptr;
		else
			*(UObject**)Parms = (UObject*)Context->GetReturnAddress();
	}
}

void UASFunction_DWordReturn::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(asDWORD*)RESULT_PARAM = 0;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		P_FINISH;

		*(asDWORD*)RESULT_PARAM = MakeRawJITCall_ReturnValue<asDWORD>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		P_FINISH;
		Context->Execute();


		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(asDWORD*)RESULT_PARAM = 0;
		else
			*(asDWORD*)RESULT_PARAM = Context->GetReturnDWord();
	}
}

void UASFunction_DWordReturn::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(asDWORD*)Parms = 0;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value4Byte);

		*(asDWORD*)Parms = MakeRawJITCall_ReturnValue<asDWORD>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->Execute();

		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value4Byte);

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(asDWORD*)Parms = 0;
		else
			*(asDWORD*)Parms = Context->GetReturnDWord();
	}
}

void UASFunction_FloatExtendedToDoubleReturn::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(float*)RESULT_PARAM = 0.f;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		P_FINISH;

		*(float*)RESULT_PARAM = (float)MakeRawJITCall_ReturnValue<double>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		P_FINISH;
		Context->Execute();


		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(float*)RESULT_PARAM = 0;
		else
			*(float*)RESULT_PARAM = (float)Context->GetReturnDouble();
	}
}

void UASFunction_FloatExtendedToDoubleReturn::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(float*)Parms = 0.f;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::FloatExtendedToDouble);

		*(float*)Parms = (float)MakeRawJITCall_ReturnValue<double>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->Execute();

		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::FloatExtendedToDouble);

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(float*)Parms = 0.f;
		else
			*(float*)Parms = (float)Context->GetReturnDouble();
	}
}

void UASFunction_FloatReturn::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(float*)RESULT_PARAM = 0.f;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		P_FINISH;

		*(float*)RESULT_PARAM = MakeRawJITCall_ReturnValue<float>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		P_FINISH;
		Context->Execute();

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(float*)RESULT_PARAM = 0.f;
		else
			*(float*)RESULT_PARAM = Context->GetReturnFloat();
	}
}

void UASFunction_FloatReturn::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(float*)Parms = 0.f;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value4Byte);

		*(float*)Parms = MakeRawJITCall_ReturnValue<float>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->Execute();

		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value4Byte);

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(float*)Parms = 0.f;
		else
			*(float*)Parms = Context->GetReturnFloat();
	}
}

void UASFunction_DoubleReturn::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(double*)RESULT_PARAM = 0.0;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		P_FINISH;

		*(double*)RESULT_PARAM = MakeRawJITCall_ReturnValue<double>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		P_FINISH;
		Context->Execute();

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(double*)RESULT_PARAM = 0.0;
		else
			*(double*)RESULT_PARAM = Context->GetReturnDouble();
	}
}

void UASFunction_DoubleReturn::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(double*)Parms = 0.0;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value8Byte);

		*(double*)Parms = MakeRawJITCall_ReturnValue<double>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->Execute();

		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value8Byte);

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(double*)Parms = 0.0;
		else
			*(double*)Parms = Context->GetReturnDouble();
	}
}

void UASFunction_ByteReturn::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(asBYTE*)RESULT_PARAM = 0;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		P_FINISH;

		*(asBYTE*)RESULT_PARAM = MakeRawJITCall_ReturnValue<asBYTE>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);

		P_FINISH;
		Context->Execute();

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(asBYTE*)RESULT_PARAM = 0;
		else
			*(asBYTE*)RESULT_PARAM = Context->GetReturnByte();
	}
}

void UASFunction_ByteReturn::RuntimeCallEvent(UObject* Object, void* Parms)
{
#if AS_CAN_HOTRELOAD
	if (ScriptFunction == nullptr)
	{
		*(asBYTE*)Parms = 0;
		return;
	}
#endif

	if (!CheckGameThreadExecution(this))
		return;

	asCScriptFunction* RealFunction = ResolveScriptVirtual(this, Object);
	if (auto* JitFunc = RealFunction->jitFunction_Raw)
	{
		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value1Byte);

		*(asBYTE*)Parms = MakeRawJITCall_ReturnValue<asBYTE>(Object, JitFunc);
	}
	else
	{
		AS_LLM_SCOPE
		FAngelscriptGameThreadContext Context(Object);
		Context->Prepare(RealFunction);
		Context->SetObject(Object);
		Context->Execute();

		checkSlow(ReturnArgument.PosInParmStruct == 0);
		checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value1Byte);

		if (Context->m_status == asEContextState::asEXECUTION_EXCEPTION)
			*(asBYTE*)Parms = 0;
		else
			*(asBYTE*)Parms = Context->GetReturnByte();
	}
}

void UASFunction_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	AngelscriptCallFromBPVM<true, true>(this, Object, Stack, RESULT_PARAM);
}

void UASFunction_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	AngelscriptCallFromParms<true, true>(this, Object, Parms);
}

void UASFunction_NotThreadSafe_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	AngelscriptCallFromBPVM<false, true>(this, Object, Stack, RESULT_PARAM);
}

void UASFunction_NotThreadSafe_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	AngelscriptCallFromParms<false, true>(this, Object, Parms);
}

void UASFunction_NoParams_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	P_FINISH;
	MakeRawJITCall_NoParam(Object, JitFunction_Raw);
}

void UASFunction_NoParams_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	MakeRawJITCall_NoParam(Object, JitFunction_Raw);
}

void UASFunction_DWordArg_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	asDWORD ArgumentValue;
	Stack.StepCompiledIn<FProperty>(&ArgumentValue);

	P_FINISH;

	MakeRawJITCall_Arg<asDWORD>(Object, JitFunction_Raw, ArgumentValue);
}

void UASFunction_DWordArg_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	asDWORD ArgumentValue = *(asDWORD*)Parms;
	MakeRawJITCall_Arg<asDWORD>(Object, JitFunction_Raw, ArgumentValue);
}

void UASFunction_FloatArg_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	float ArgumentValue;
	Stack.StepCompiledIn<FProperty>(&ArgumentValue);

	P_FINISH;

	MakeRawJITCall_Arg<float>(Object, JitFunction_Raw, ArgumentValue);
}

void UASFunction_FloatArg_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	float ArgumentValue = *(float*)Parms;
	MakeRawJITCall_Arg<float>(Object, JitFunction_Raw, ArgumentValue);
}

void UASFunction_DoubleArg_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	double ArgumentValue;
	Stack.StepCompiledIn<FProperty>(&ArgumentValue);

	P_FINISH;

	MakeRawJITCall_Arg<double>(Object, JitFunction_Raw, ArgumentValue);
}

void UASFunction_DoubleArg_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	double ArgumentValue = *(double*)Parms;
	MakeRawJITCall_Arg<double>(Object, JitFunction_Raw, ArgumentValue);
}

void UASFunction_FloatExtendedToDoubleArg_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	float ArgumentValue;
	Stack.StepCompiledIn<FProperty>(&ArgumentValue);

	P_FINISH;

	MakeRawJITCall_Arg<double>(Object, JitFunction_Raw, (double)ArgumentValue);
}

void UASFunction_FloatExtendedToDoubleArg_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	float ArgumentValue = *(float*)Parms;
	MakeRawJITCall_Arg<double>(Object, JitFunction_Raw, (double)ArgumentValue);
}

void UASFunction_QWordArg_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	asQWORD ArgumentValue;
	Stack.StepCompiledIn<FProperty>(&ArgumentValue);

	P_FINISH;

	MakeRawJITCall_Arg<asQWORD>(Object, JitFunction_Raw, ArgumentValue);
}

void UASFunction_QWordArg_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	asQWORD ArgumentValue = *(asQWORD*)Parms;
	MakeRawJITCall_Arg<asQWORD>(Object, JitFunction_Raw, ArgumentValue);
}

void UASFunction_ByteArg_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	asDWORD ArgumentValue;
	Stack.StepCompiledIn<FProperty>(&ArgumentValue);

	P_FINISH;

	MakeRawJITCall_Arg<asBYTE>(Object, JitFunction_Raw, (asBYTE&)ArgumentValue);
}

void UASFunction_ByteArg_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	MakeRawJITCall_Arg<asBYTE>(Object, JitFunction_Raw, *(asBYTE*)Parms);
}

void UASFunction_ReferenceArg_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	AngelscriptCallFromBPVM<false, true>(this, Object, Stack, RESULT_PARAM);
}

void UASFunction_ReferenceArg_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	MakeRawJITCall_Arg<void*>(Object, JitFunction_Raw, Parms);
}

void UASFunction_ObjectReturn_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	P_FINISH;

	*(UObject**)RESULT_PARAM = MakeRawJITCall_ReturnValue<UObject*>(Object, JitFunction_Raw);
}

void UASFunction_ObjectReturn_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	checkSlow(ReturnArgument.PosInParmStruct == 0);
	checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::ReturnObjectPointer);

	*(UObject**)Parms = MakeRawJITCall_ReturnValue<UObject*>(Object, JitFunction_Raw);
}

void UASFunction_DWordReturn_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	P_FINISH;

	*(asDWORD*)RESULT_PARAM = MakeRawJITCall_ReturnValue<asDWORD>(Object, JitFunction_Raw);
}

void UASFunction_DWordReturn_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	checkSlow(ReturnArgument.PosInParmStruct == 0);
	checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value4Byte);

	*(asDWORD*)Parms = MakeRawJITCall_ReturnValue<asDWORD>(Object, JitFunction_Raw);
}

void UASFunction_FloatExtendedToDoubleReturn_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	P_FINISH;

	*(float*)RESULT_PARAM = (float)MakeRawJITCall_ReturnValue<double>(Object, JitFunction_Raw);
}

void UASFunction_FloatExtendedToDoubleReturn_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	checkSlow(ReturnArgument.PosInParmStruct == 0);
	checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::FloatExtendedToDouble);

	*(float*)Parms = (float)MakeRawJITCall_ReturnValue<double>(Object, JitFunction_Raw);
}

void UASFunction_FloatReturn_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	P_FINISH;

	*(float*)RESULT_PARAM = MakeRawJITCall_ReturnValue<float>(Object, JitFunction_Raw);
}

void UASFunction_FloatReturn_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	checkSlow(ReturnArgument.PosInParmStruct == 0);
	checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value4Byte);

	*(float*)Parms = MakeRawJITCall_ReturnValue<float>(Object, JitFunction_Raw);
}

void UASFunction_DoubleReturn_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	P_FINISH;

	*(double*)RESULT_PARAM = MakeRawJITCall_ReturnValue<double>(Object, JitFunction_Raw);
}

void UASFunction_DoubleReturn_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	checkSlow(ReturnArgument.PosInParmStruct == 0);
	checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value8Byte);

	*(double*)Parms = MakeRawJITCall_ReturnValue<double>(Object, JitFunction_Raw);
}

void UASFunction_ByteReturn_JIT::RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	P_FINISH;

	*(asBYTE*)RESULT_PARAM = MakeRawJITCall_ReturnValue<asBYTE>(Object, JitFunction_Raw);
}

void UASFunction_ByteReturn_JIT::RuntimeCallEvent(UObject* Object, void* Parms)
{
	if (!CheckGameThreadExecution(this))
		return;
	VerifyScriptVirtualResolved(this, Object);

	checkSlow(ReturnArgument.PosInParmStruct == 0);
	checkSlow(ReturnArgument.ParmBehavior == EArgumentParmBehavior::Value1Byte);

	*(asBYTE*)Parms = MakeRawJITCall_ReturnValue<asBYTE>(Object, JitFunction_Raw);
}
