#pragma once

#include "CoreMinimal.h"
#include "ClassGenerator/AngelscriptAdditionalCompileChecks.h"
#include "AngelscriptManager.h"

                                                                                                              
struct FScopedSlowTask;
                                                                                                            
typedef const TArray<TPair<FName, int64>>& EnumNameList;
DECLARE_MULTICAST_DELEGATE_OneParam(FOnAngelscriptPostReload, bool);
DECLARE_MULTICAST_DELEGATE(FOnAngelscriptFullReload);
                                                                                                                         
DECLARE_MULTICAST_DELEGATE_OneParam(FOnAngelscriptPreFullReload, const TArray<UObject*>&);
                                                                                                                       
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAngelscriptLiteralAssetReload, UObject*, UObject*);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAngelscriptClassReload, UClass*, UClass*);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnAngelscriptEnumCreated, UEnum*);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAngelscriptEnumChanged, UEnum*, EnumNameList);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAngelscriptStructReload, UScriptStruct*, UScriptStruct*);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnAngelscriptDelegateReload, UDelegateFunction*, UDelegateFunction*);

struct FAngelscriptClassGenerator
{
	enum EReloadRequirement
	{
		SoftReload,
		FullReloadSuggested,
		FullReloadRequired,
		Error,
	};

	static ANGELSCRIPTCODE_API FOnAngelscriptClassReload OnClassReload;
	static ANGELSCRIPTCODE_API FOnAngelscriptEnumCreated OnEnumCreated;
	static ANGELSCRIPTCODE_API FOnAngelscriptEnumChanged OnEnumChanged;
	static ANGELSCRIPTCODE_API FOnAngelscriptStructReload OnStructReload;
	static ANGELSCRIPTCODE_API FOnAngelscriptDelegateReload OnDelegateReload;
	static ANGELSCRIPTCODE_API FOnAngelscriptFullReload OnFullReload;
                                                                                                                    
	static ANGELSCRIPTCODE_API FOnAngelscriptPreFullReload OnPreFullReload;
                                                                                                                  
	static ANGELSCRIPTCODE_API FOnAngelscriptPostReload OnPostReload;
	static ANGELSCRIPTCODE_API FOnAngelscriptLiteralAssetReload OnLiteralAssetReload;

	void AddModule(TSharedRef<FAngelscriptModuleDesc> Module);

	EReloadRequirement Setup();
                                                                                                                     
	bool ValidatePayloadReloads(FString& Error) const;
                                                                                                                   

	void PerformFullReload();
	void PerformSoftReload();
                                                                                                          
	bool PrepareModuleTransaction(FString& Error);
	bool CommitModuleTransaction(FString& Error);
	void AbortModuleTransaction();
                                                                                                        


	void GetFullReloadLines(TSharedRef<FAngelscriptModuleDesc> Module, TArray<int32>& OutLines);
	bool WantsFullReload(TSharedRef<FAngelscriptModuleDesc> Module);
	bool NeedsFullReload(TSharedRef<FAngelscriptModuleDesc> Module);

                                                                                                                                           
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
	static ANGELSCRIPTCODE_API bool ValidateExplicitFullReloadBindingShape(
		const FString& CanonicalTarget,
		const TArray<TArray<FString>>& CanonicalModuleCodePaths,
		int32& OutTargetModuleIndex,
		int32 QualifyingWitnessCount = INDEX_NONE);
	static ANGELSCRIPTCODE_API bool HasDistinctExplicitFullReloadMarkerPair(
		const UASClass* OldClass,
		const UASClass* NewClass);
#endif
                                                                                                                                         
private:

                                                                                                            
	bool bPreparingModuleTransaction = false;
	bool bModuleTransactionPrepared = false;
	void CompleteReload(FScopedSlowTask& SlowTask);
	void PublishClassRegistration(UASClass* Class);
                                                                                                          
	struct FReloadPropagation
	{
		bool bStartedPropagating = false;
		bool bFinishedPropagating = false;
		bool bHasOutstandingDependencies = false;
		EReloadRequirement ReloadReq = EReloadRequirement::SoftReload;
		TArray<FReloadPropagation*> PendingDependees;
	};

	struct FClassData : public FReloadPropagation
	{
		int32 DataIndex;
		TSharedPtr<FAngelscriptClassDesc> NewClass;
		TSharedPtr<FAngelscriptClassDesc> OldClass;
                                                                                                                          
		bool bExplicitFullReloadTarget = false;
                                                                                                                        
		bool bAnalyzed = false;
		bool bReloaded = false;
		UASClass* ReplacedClass = nullptr;
		UStruct* ReplacedStruct = nullptr;
		TArray<int32> ReloadReqLines;
		bool bHasEvalTick = false;
		bool bFinalized = false;
		UObject* CDONoDefaults = nullptr;
	};

	struct FEnumData
	{
		int32 DataIndex;
		TSharedPtr<FAngelscriptEnumDesc> NewEnum;
		TSharedPtr<FAngelscriptEnumDesc> OldEnum;
		bool bNeedReload = false;
	};

	struct FDelegateData : public FReloadPropagation
	{
		int32 DataIndex;
		TSharedPtr<FAngelscriptDelegateDesc> NewDelegate;
		TSharedPtr<FAngelscriptDelegateDesc> OldDelegate;
		bool bAnalyzed = false;
		TArray<int32> ReloadReqLines;
	};

	struct FModuleData
	{
		int32 ModuleIndex;
		TSharedPtr<FAngelscriptModuleDesc> NewModule;
		TSharedPtr<FAngelscriptModuleDesc> OldModule;
		TArray<FEnumData> Enums;
		TArray<FClassData> Classes;
		TArray<FDelegateData> Delegates;
		EReloadRequirement ReloadReq = EReloadRequirement::SoftReload;
		TArray<int32> ReloadReqLines;

		TArray<TSharedPtr<FAngelscriptClassDesc>> RemovedClasses;
	};

	struct FDataRef
	{
		bool bIsClass = false;
		bool bIsEnum = false;
		bool bIsDelegate = false;
		int32 ModuleIndex;
		int32 DataIndex;

		FDataRef() {}

		FDataRef(const FModuleData& ModuleData, const FClassData& ClassData)
		{
			bIsClass = true;
			ModuleIndex = ModuleData.ModuleIndex;
			DataIndex = ClassData.DataIndex;
		}

		FDataRef(const FModuleData& ModuleData, const FDelegateData& DelegateData)
		{
			bIsDelegate = true;
			ModuleIndex = ModuleData.ModuleIndex;
			DataIndex = DelegateData.DataIndex;
		}

		FDataRef(const FModuleData& ModuleData, const FEnumData& EnumData)
		{
			bIsEnum = true;
			ModuleIndex = ModuleData.ModuleIndex;
			DataIndex = EnumData.DataIndex;
		}
	};

	TMap<FString, int32> ModuleIndexByName;
	TMap<class asIScriptModule*, int32> ModuleIndexByNewScriptModule;
	TMap<FString, FDataRef> DataRefByName;
	TMap<class asITypeInfo*, FDataRef> DataRefByNewScriptType;
	TArray<UClass*> ReinstancedSubsystems;

	bool bIsDoingFullReload = false;
	bool bReinstancingAny = false;
	int32 NextUniqueNumber = 0;

                                                                                                                                
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
	bool ValidateAndMarkExplicitFullReloadTarget();
#endif
                                                                                                                              
	bool ShouldFullReload(FClassData& Class);
	bool ShouldFullReload(FEnumData& Enum);
	bool ShouldFullReload(FDelegateData& Enum);

	void PerformReload(bool bFullReload);

	void AddReloadDependency(FReloadPropagation* Source, const FAngelscriptTypeUsage& Type);
	void AddReloadDependency(FReloadPropagation* Source, class asITypeInfo* TypeInfo);

	void PropagateReloadRequirements(FModuleData& Module, FClassData& Class);
	void PropagateReloadRequirements(FModuleData& Module, FDelegateData& Delegate);
	void ResolvePendingReloadDependees(FReloadPropagation* Source);

	TArray<FModuleData> Modules;
	TMap<class asITypeInfo*, class asITypeInfo*> UpdatedScriptTypeMap;
	TArray<UClass*> AddedClasses;

	bool GetDataFor(int TypeId, FModuleData*& OutModule, FClassData*& OutClass, FDelegateData*& OutDelegate);
	bool GetDataFor(class asITypeInfo* Type, FModuleData*& OutModule, FClassData*& OutClass, FDelegateData*& OutDelegate);

	UClass* ResolveCodeSuperForProperty(const FAngelscriptTypeUsage& Usage);

	void EnsureReloaded(FModuleData& Module, FClassData& Class);
	void EnsureReloaded(UASClass* Class);
	void EnsureReloaded(int TypeId);
	void EnsureClassFinalized(UASClass* Class);

	void Analyze(FModuleData& Module);
	void Analyze(FModuleData& Module, FClassData& Class);
	void Analyze(FModuleData& Module, FDelegateData& Class);

	void SetupModule(FModuleData& Module);

	void InitEnums(FModuleData& Module);
	void AnalyzeEnums(FModuleData& Module);

	void CreateFullReloadClass(FModuleData& Module, FClassData& Class);
	void FullReloadRemoveClass(FModuleData& Module, TSharedPtr<FAngelscriptClassDesc> RemovedClass);
	void CreateFullReloadStruct(FModuleData& Module, FClassData& Class);
	void CreateFullReloadDelegate(FModuleData& Module, FDelegateData& Delegate);
	void DoFullReload(FModuleData& Module, FClassData& Class);
	void DoFullReloadClass(FModuleData& Module, FClassData& Class);
	void DoFullReloadStruct(FModuleData& Module, FClassData& Class);
	void DoFullReload(FModuleData& Module, FEnumData& Enum);
	void DoFullReload(FModuleData& Module, FDelegateData& DelegateData);

#if WITH_EDITOR
	void CopyClassInheritedMetaData(UClass* SuperClass, UClass* NewClass);
#endif

	void SoftReloadFunction(UFunction* Function);
	void SoftReloadType(FAngelscriptTypeUsage& Usage);

	int32 AddClassProperties(TSharedPtr<FAngelscriptClassDesc> Class);
	FProperty* AddFunctionReturnType(UFunction* NewFunction, const FAngelscriptTypeUsage& ReturnType);
	FProperty* AddFunctionArgument(UFunction* NewFunction, const FAngelscriptArgumentDesc& ArgDesc, bool bAddToArgList = true);

	void LinkSoftReloadClasses(FModuleData& Module, FEnumData& Enum);
	void LinkSoftReloadClasses(FModuleData& Module, FClassData& Class);
	void LinkSoftReloadClasses(FModuleData& Module, FDelegateData& Delegate);

	void PrepareSoftReload(FModuleData& Module, FClassData& Class);
	void DoSoftReload(FModuleData& Module, FClassData& Class);

	void CleanupRemovedClass(TSharedPtr<FAngelscriptClassDesc> Class);

	bool IsReloadingModule(TSharedPtr<FAngelscriptModuleDesc> Module);
	TSharedPtr<FAngelscriptClassDesc> EnsureClassAnalyzed(const FString& ClassName);
	TSharedPtr<FAngelscriptClassDesc> GetClassDesc(const FString& ClassName);

	void DestructScriptObject(class asCScriptObject* ScriptObject, UASClass* NewClass, class asCObjectType* ObjectTypeToDestruct);
	void ReinitializeScriptObject(class asCScriptObject* ScriptObject, UASClass* NewClass, class asCObjectType* ObjectTypeToConstruct);

	void DetectAngelscriptReferences(TSharedPtr<FAngelscriptClassDesc> Class);

	void FinalizeClass(FModuleData& ModuleData, FClassData& ClassData);
	void FinalizeActorClass(FModuleData& ModuleData, TSharedPtr<FAngelscriptClassDesc> ClassDesc);
	void FinalizeComponentClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc);
	void FinalizeObjectClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc);

	void VerifyClass(FModuleData& ModuleData, TSharedPtr<FAngelscriptClassDesc> ClassDesc);

	void InitClassTickSettings(FClassData& ClassData);
	void CallPostInitFunctions();
	void InitDefaultObjects();
	void InitDefaultObject(FModuleData& ModuleData, FClassData& ClassData);

	void SetScriptStaticClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc, UClass* Class);
	void SetScriptStaticStruct(TSharedPtr<FAngelscriptClassDesc> ClassDesc, UScriptStruct* Struct);
	void UpdateConstructAndDefaultsFunctions(TSharedPtr<FAngelscriptClassDesc> ClassDesc, UASClass* Class);

	asITypeInfo* GetNamespacedTypeInfoForClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc, TSharedPtr<FAngelscriptModuleDesc> ModuleDesc) const;

	FString GetUnrealName(bool bIsStruct, const FString& ClassName);
	TSet<FString> UsedUnrealNames;
};
