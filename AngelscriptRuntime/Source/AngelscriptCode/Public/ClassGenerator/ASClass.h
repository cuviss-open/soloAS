#pragma once

#include "CoreMinimal.h"

#include "AngelscriptManager.h"
#include "AngelscriptInclude.h"
                                                                                                
#include "UObject/ObjectKey.h"
                                                                                              

#include "ASClass.generated.h"

UCLASS()
class ANGELSCRIPTCODE_API UASClass : public UClass
{
	GENERATED_BODY()
public:
                                                                                                     
	bool HasValidScriptClassCarrier() const;
	virtual void Link(FArchive& Ar, bool bRelinkExistingProperties) override;
	virtual void DestroyPersistentUberGraphFrame(UObject* Object, bool bSkipSuperClass = false) const override;
	TArray<const FProperty*> ScriptFinishDestroyFields;
    void RetainRetiredScriptTypeForGc();
    void ReleaseRetiredScriptTypeForGc();
    asITypeInfo* RetiredScriptTypeForGc = nullptr;
    asIScriptModule* RetiredScriptModuleForGc = nullptr;
	TArray<FObjectKey> FinishDestroyReceivers;
	virtual void BeginDestroy() override;
	virtual bool IsReadyForFinishDestroy() override;
	virtual void FinishDestroy() override;
	virtual UObject* CreateDefaultObject() override;
	virtual EStructPropertyLinkFlags GetPropertyLinkFlags(UStruct* ContainerStruct, FProperty* Property) const override;
                                                                                                   
                                                                                                                       
	static bool HasValidScriptClassCarrierAncestry(
		const UClass* SuperClass,
		const UClass** OutInvalidBlueprintAncestor = nullptr);
                                                                                                                     
	UClass* CodeSuperClass = nullptr;
	UASClass* NewerVersion = nullptr;
	bool bHasASClassParent = false;
	bool bCanEverTick = false;
	bool bStartWithTickEnabled = false;
	bool bIsEditorOnlyClass = false;
                                                                                                                     
#if 0
                                                                                                                   
	int32 ContainerSize = 0;
                                                                                                                   
#endif
	static constexpr bool bUsesStockPropertiesSize = true;
                                                                                                                 
	int32 ScriptPropertyOffset = 0;
                                                                                             
	asITypeInfo* ScriptTypePtr = nullptr;
	bool bIsModuleStaticCarrier = false;
                                                                                           
	class asIScriptFunction* ConstructFunction;
	class asIScriptFunction* DefaultsFunction;
	UClass* ComposeOntoClass = nullptr;
                                                                                                              
private:
	void* UbergraphFrameLayoutPadding = nullptr;
public:
	// Stock's BPGC collector reads this slot in every non-native ancestor.
	// The matching engine offset is asserted in ASClass.cpp for every target.
	FStructProperty* const StockUberGraphFramePointerProperty = nullptr;
                                                                                                            
                                                                                               
	TSet<const FProperty*> UeDelayedDestroyFields;
                                                                                             

#if WITH_EDITOR
	FString AngelscriptTypename;
#endif

	struct FDefaultComponent
	{
		UClass* ComponentClass;
		FName ComponentName;
		SIZE_T VariableOffset;
		bool bIsRoot;
		bool bEditorOnly;
		FName Attach;
		FName AttachSocket;
	};
	TArray<FDefaultComponent> DefaultComponents;

	struct FOverrideComponent
	{
		UClass* ComponentClass;
		FName OverrideComponentName;
		FName VariableName;
		SIZE_T VariableOffset;
	};
	TArray<FOverrideComponent> OverrideComponents;

	struct FBindComponent
	{
		UClass* ComponentClass;
		FName ComponentName;
		FName VariableName;
		SIZE_T VariableOffset;
	};
	TArray<FBindComponent> BindComponents;
	
	TArray<FName> PushModelReplicatedProperties;

	struct ANGELSCRIPTCODE_API FScopeSetDefaultConstructorOuter
	{
		UObject* PrevOuter;
		FScopeSetDefaultConstructorOuter(UObject* NewOuter);
		~FScopeSetDefaultConstructorOuter();
	};

	UASClass(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	static UASClass* GetFirstASClass(UClass* Object);
	static UASClass* GetFirstASClass(UObject* Object);
	static UClass* GetFirstASOrNativeClass(UClass* Object);

	static UObject* GetConstructingASObject();

                                                                                                                                
#if 0
                                                                                                                              
	virtual UClass* GetMostUpToDateClass() override;
                                                                                                                                
#endif
	virtual UClass* GetAuthoritativeClass() override;
                                                                                                                              
                                                                                                                                 
#if 0
                                                                                                                               
	virtual void RuntimeAddReferencedObjects(UObject* InThis, FReferenceCollector& Collector) override {}
                                                                                                                              
#endif
	static constexpr bool bUsesStockReferenceCollection = true;
                                                                                                                            

                                                                                                                             
#if 0
                                                                                                                           
	virtual void GetLifetimeScriptReplicationList(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;
                                                                                                                               
#endif
	virtual void GetLifetimeScriptReplicationList(TArray<class FLifetimeProperty>& OutLifetimeProps) const;
                                                                                                                             
                                                                                                                  
	static bool TryGetLifetimeScriptReplicationList(
		const UASClass* ScriptClass,
		TArray<class FLifetimeProperty>& OutLifetimeProps
	);
                                                                                                                

                                                                                                                           
#if 0
                                                                                                                         
	virtual void RuntimeDestroyObject(UObject* Object) override;
                                                                                                                             
#endif
	virtual void RuntimeDestroyObject(UObject* Object);
                                                                                                                           
                                                                                           
	bool RuntimeDestroyObject(
		UObject* Object,
		class asITypeInfo* TypeCookie
	);
	static bool RuntimeDestroyObjectAdapter(
		UObject* Object,
		void* TypeCookie,
		uint64 Generation
	);
	static void RegisterRuntimeGcLifecycleAdapter();
	static void ShutdownGcLifecycleAdapter();
	static bool ClaimRuntimeTrackedMirrorForReconstruction(
		UObject* Object,
		class asITypeInfo* ExpectedTypeCookie
	);
	static bool UpdateRuntimeTrackedMirrorCookie(
		UObject* Object,
		class asITypeInfo* ExpectedTypeCookie,
		class asITypeInfo* NewTypeCookie
	);
	static void DestroyTrackedRuntimeObjectExplicitly(
		UObject* Object
	);
	static void RejectUnknownNonGcOverwrite(
		UObject* Object,
		const TCHAR* Operation
	);
	static constexpr uint64 RuntimeLifecycleGeneration = 1;
                                                                                         

	void ReconstructScriptObject(UObject* Object);
                                                                                                            
	void ReconstructScriptObject(
		UObject* Object,
		class asITypeInfo* OldType,
		class asITypeInfo* NewType
	);
                                                                                                          

	virtual bool IsFunctionImplementedInScript(FName InFunctionName) const override;

                                                                                                                           
#if 0
                                                                                                                         
	virtual int32 GetContainerSize() const override { return ContainerSize; }
                                                                                                                       
#endif
	using UClass::GetPropertiesSize;
                                                                                                                     

	virtual bool IsSafeForRootSet() const override { return true; }

	virtual bool IsEditorOnly() const override { return bIsEditorOnlyClass; }

#if WITH_EDITOR
	UFUNCTION(ScriptCallable, Category = "Angelscript")
	FString GetSourceFilePath() const;

	UFUNCTION(ScriptCallable, Category = "Angelscript")
	FString GetRelativeSourceFilePath() const;

	UFUNCTION(ScriptCallable, Category = "Angelscript")
	bool IsDeveloperOnly() const;
#endif

	static UObject* GetDefaultConstructorOuter();
	static void* AllocScriptObject(class asITypeInfo* ScriptType, size_t Size);
	static void FinishConstructObject(class asIScriptObject* ScriptObject, class asITypeInfo* ScriptType);

	static void StaticActorConstructor(const FObjectInitializer& Initializer);
	static void StaticComponentConstructor(const FObjectInitializer& Initializer);
	static void StaticObjectConstructor(const FObjectInitializer& Initializer);

	static void StaticDestructor(const FObjectInitializer& Initializer);

	static void OnPostInitializeComponents(AActor* Actor);
                                                                                                 
	static bool EnsureBindComponentsLinked(AActor* Actor);
	static void ResetActorBindingState();
	static bool IsActorBindingStateEmpty();
                                                                                               
	
	static UObject* OverrideConstructingObject;
};

UCLASS()
class ANGELSCRIPTCODE_API UASFunction : public UFunction
{
	GENERATED_BODY()
public:

	class asIScriptFunction* ScriptFunction = nullptr;

	// Cached pointer to RPC _Validate function (if applicable)
	UFunction* ValidateFunction = nullptr;

	enum class EArgumentParmBehavior : uint8
	{
		Reference,
		ReferencePOD,
		Value1Byte,
		Value2Byte,
		Value4Byte,
		Value8Byte,
		FloatExtendedToDouble,
		ReturnObjectPointer,
	};

	enum class EArgumentVMBehavior : uint8
	{
		FloatExtendedToDouble,
		WorldContextObject,
		ObjectPointer,

		ReferencePOD,
		Reference,

		Value1Byte,
		Value2Byte,
		Value4Byte,
		Value8Byte,

		ReturnObjectValue,
		ReturnObjectPOD,

		None,
	};

	struct FArgument
	{
		FProperty* Property = nullptr;
		FAngelscriptTypeUsage Type;
		int32 ValueBytes = 0;
		int32 StackOffset = 0;
		int32 PosInParmStruct = 0;
		EArgumentParmBehavior ParmBehavior;
		EArgumentVMBehavior VMBehavior;

		FArgument() {}
		FArgument(FProperty* InProperty, const FAngelscriptTypeUsage& InType)
			: Property(InProperty), Type(InType)
		{
		}
	};

	bool bHasWorldContext = false;
	bool bIsWorldContextGenerated = false;
	bool bIsWorldContextOptional = false;

	bool bIsNoOp = false;
	int32 WorldContextOffsetInParms = -1;
	int32 WorldContextIndex = -1;

	TArray<FArgument> Arguments;
	TArray<FArgument> DestroyArguments;
	int32 ArgStackSize = 0;
	FArgument ReturnArgument;

	asJITFunction JitFunction = nullptr;
	asJITFunction_ParmsEntry JitFunction_ParmsEntry = nullptr;
	asJITFunction_Raw JitFunction_Raw = nullptr;

#if WITH_EDITOR
	FString GetSourceFilePath() const;
	FString GetRelativeSourceFilePath() const;
	int GetSourceLineNumber() const;
#endif

	void FinalizeArguments();

	uint8 OptimizedCall_ByteReturn(UObject* Object);
	void OptimizedCall_FloatArg(UObject* Object, float Argument);
	void OptimizedCall_DoubleArg(UObject* Object, double Argument);
	void OptimizedCall(UObject* Object);
	void OptimizedCall_RefArg(UObject* Object, void* Argument);
	uint8 OptimizedCall_RefArg_ByteReturn(UObject* Object, void* Argument);

                                                                                                                               
#if 0
                                                                                                                             
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
	virtual UFunction* GetRuntimeValidateFunction() override;
                                                                                                                                   
#endif
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL);
	virtual void RuntimeCallEvent(UObject* Object, void* Parms);
	virtual UFunction* GetRuntimeValidateFunction();
	DECLARE_FUNCTION(execAngelscriptRuntimeThunk);
                                                                                                                                 

	static UASFunction* AllocateFunctionFor(UClass* InClass, FName ObjectName, TSharedPtr<FAngelscriptFunctionDesc> FunctionDesc);
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_NotThreadSafe : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_NoParams : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_DWordArg : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_QWordArg : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_FloatArg : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_FloatExtendedToDoubleArg : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_FloatExtendedToDoubleReturn : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_DoubleArg : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_ByteArg : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_ReferenceArg : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_ObjectReturn : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_DWordReturn : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_FloatReturn : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_DoubleReturn : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_ByteReturn : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_NotThreadSafe_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_NoParams_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_DWordArg_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_QWordArg_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_FloatArg_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_FloatExtendedToDoubleArg_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_FloatExtendedToDoubleReturn_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_DoubleArg_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_ByteArg_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_ReferenceArg_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_ObjectReturn_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_DWordReturn_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_FloatReturn_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_DoubleReturn_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};

UCLASS(NotBlueprintType)
class ANGELSCRIPTCODE_API UASFunction_ByteReturn_JIT : public UASFunction
{
	GENERATED_BODY()
public:
	virtual void RuntimeCallFunction(UObject* Object, FFrame& Stack, RESULT_DECL) override;
	virtual void RuntimeCallEvent(UObject* Object, void* Parms) override;
};
