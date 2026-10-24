#pragma once
#include "CoreMinimal.h"
#include "AngelscriptStaticJIT.h"
#include "StaticJIT/StringInArchive.h"
#include "StaticJIT/PrecompiledDataAllocator.h"
#include "AngelscriptManager.h"

struct FAngelscriptPrecompiledData;

struct FAngelscriptPrecompiledReference
{
	int64 OldReference = 0;

	bool IsNull() const
	{
		return OldReference == 0;
	}

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledReference& Data)
	{
		Ar << Data.OldReference;
		return Ar;
	}
};

                                                                                                  
struct FAngelscriptPrecompiledCacheFile
{
	FString ModuleName;
	FString FileName;
	FGuid DataGuid;
	TArray<FString> Dependencies;
};
                                                                                                

struct FAngelscriptPrecompiledDataType
{
	FAngelscriptPrecompiledReference TypeInfo;
	uint8 TokenType = 0xff;
	uint8 bIsReference : 1 = false;
	uint8 bIsObjectConst : 1 = false;
	uint8 bIsObjectHandle : 1 = false;
	uint8 bIsConstHandle : 1 = false;
	uint8 bIsAuto : 1 = false;
	uint8 bIfHandleThenConst : 1 = false;

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledDataType& Data)
	{
		Ar << Data.TypeInfo;
		Ar << Data.TokenType;

		if (Ar.IsSaving())
		{
			uint8 Mask = 0;
			uint8 Bit = 0;
			if (Data.bIsReference) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bIsObjectConst) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bIsObjectHandle) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bIsConstHandle) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bIsAuto) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bIfHandleThenConst) { Mask |= 1<<Bit; } ++Bit;
			Ar << Mask;
		}
		else
		{
			uint8 Mask = 0;
			Ar << Mask;

			uint8 Bit = 0;
			Data.bIsReference = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bIsObjectConst = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bIsObjectHandle = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bIsConstHandle = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bIsAuto = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bIfHandleThenConst = ((Mask & (1<<Bit)) != 0); ++Bit;
		}
		return Ar;

	}

	void InitFrom(FAngelscriptPrecompiledData& Context, class asCDataType& DataType);
	void Create(FAngelscriptPrecompiledData& Context, class asCDataType& DataType) const;
};

struct FAngelscriptPrecompiledFunctionSignature
{
	FStringInArchive Name;
	FStringInArchive Namespace;
	TArray<FAngelscriptPrecompiledDataType, TPrecompiledAllocator<>> ParameterTypes;
	TArray<int32, TPrecompiledAllocator<>> ParameterFlags;
	TArray<FStringInArchive, TPrecompiledAllocator<>> ParameterDefaultArgs;
	FAngelscriptPrecompiledDataType ReturnType;

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledFunctionSignature& Data)
	{
		Ar << Data.Name;
		Ar << Data.Namespace;
		Ar << Data.ParameterTypes;
		Ar << Data.ParameterFlags;
		Ar << Data.ParameterDefaultArgs;
		Ar << Data.ReturnType;
		return Ar;
	}

	void InitFrom(FAngelscriptPrecompiledData& Context, class asCScriptFunction* Function);
	void Create(FAngelscriptPrecompiledData& Context, struct FAppliedFunctionSignature& OutSignature) const;
};

struct FAngelscriptPrecompiledFunction
{
	// Angelscript data
	FStringInArchive FunctionName;
	FStringInArchive Namespace;
	FAngelscriptPrecompiledDataType ReturnType;
	TArray<FAngelscriptPrecompiledDataType, TPrecompiledAllocator<>> ParameterTypes;
	TArray<FStringInArchive, TPrecompiledAllocator<>> ParameterNames;
	TArray<int32, TPrecompiledAllocator<>> ParameterFlags;
	TArray<FStringInArchive, TPrecompiledAllocator<>> ParameterDefaultArgs;
	int32 FunctionTraits;
	TArray<int32, TPrecompiledAllocator<>> ByteCode;
	TArray<int32, TPrecompiledAllocator<>> ByteCodeReferences;
	int32 VariableSpace = -1;
	TArray<FAngelscriptPrecompiledReference, TPrecompiledAllocator<>> ObjVariableTypes;
	TArray<int32, TPrecompiledAllocator<>> ObjVariablePos;
	int32 ObjVariablesOnHeap = -1;
	TArray<int32, TPrecompiledAllocator<>> VariableInfoProgramPos;
	TArray<int32, TPrecompiledAllocator<>> VariableInfoOffset;
	TArray<int32, TPrecompiledAllocator<>> VariableInfoOption;
	int32 StackNeeded = -1;
	uint32 Id;
	int32 DeclaredAt = 0;
	TArray<int32, TPrecompiledAllocator<>> LineNumbers;

	// Preprocessor data
	FStringInArchive UnrealFunctionName;
	TArray<FStringInArchive, TPrecompiledAllocator<>> MetaSpec;
	TArray<FStringInArchive, TPrecompiledAllocator<>> MetaValues;
	uint8 bIsUFunction = false;
	uint8 bBlueprintCallable : 1 = false;
	uint8 bBlueprintOverride : 1 = false;
	uint8 bBlueprintEvent : 1 = false;
	uint8 bBlueprintPure : 1 = false;
	uint8 bNetFunction : 1 = false;
	uint8 bNetMulticast : 1 = false;
	uint8 bNetClient : 1 = false;
	uint8 bNetServer : 1 = false;
	uint8 bNetValidate : 1 = false;
	uint8 bUnreliable : 1 = false;
	uint8 bBlueprintAuthorityOnly : 1 = false;
	uint8 bExec : 1 = false;
	uint8 bCanOverrideEvent : 1 = false;
	uint8 bDevFunction : 1 = false;
	uint8 bIsStatic : 1 = false;
	uint8 bIsConstMethod : 1 = false;
	uint8 bThreadSafe : 1 = false;
	uint8 bIsNoOp : 1 = false;

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledFunction& Data)
	{
		Ar << Data.FunctionName;
		Ar << Data.Namespace;
		Ar << Data.ReturnType;
		Ar << Data.ParameterTypes;
		Ar << Data.ParameterNames;
		Ar << Data.ParameterFlags;
		Ar << Data.ParameterDefaultArgs;
		Ar << Data.FunctionTraits;
		Ar << Data.ByteCode;
		Ar << Data.ByteCodeReferences;
		Ar << Data.VariableSpace;
		Ar << Data.ObjVariableTypes;
		Ar << Data.ObjVariablePos;
		Ar << Data.ObjVariablesOnHeap;
		Ar << Data.VariableInfoProgramPos;
		Ar << Data.VariableInfoOffset;
		Ar << Data.VariableInfoOption;
		Ar << Data.StackNeeded;
		Ar << Data.Id;
		Ar << Data.DeclaredAt;
		Ar << Data.LineNumbers;

		Ar << Data.bIsUFunction;
		if (Data.bIsUFunction)
		{
			Ar << Data.UnrealFunctionName;
			Ar << Data.MetaSpec;
			Ar << Data.MetaValues;

			if (Ar.IsSaving())
			{
				uint32 Mask = 0;
				uint8 Bit = 0;
				if (Data.bBlueprintCallable) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bBlueprintOverride) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bBlueprintEvent) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bBlueprintPure) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bNetFunction) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bNetMulticast) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bNetClient) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bNetServer) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bNetValidate) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bUnreliable) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bBlueprintAuthorityOnly) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bExec) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bCanOverrideEvent) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bDevFunction) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bIsStatic) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bIsConstMethod) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bThreadSafe) { Mask |= 1<<Bit; } ++Bit;
				if (Data.bIsNoOp) { Mask |= 1<<Bit; } ++Bit;

				Ar << Mask;
			}
			else
			{
				uint32 Mask = 0;
				Ar << Mask;

				uint8 Bit = 0;
				Data.bBlueprintCallable = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bBlueprintOverride = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bBlueprintEvent = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bBlueprintPure = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bNetFunction = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bNetMulticast = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bNetClient = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bNetServer = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bNetValidate = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bUnreliable = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bBlueprintAuthorityOnly = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bExec = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bCanOverrideEvent = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bDevFunction = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bIsStatic = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bIsConstMethod = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bThreadSafe = ((Mask & (1<<Bit)) != 0); ++Bit;
				Data.bIsNoOp = ((Mask & (1<<Bit)) != 0); ++Bit;
			}
		}
		return Ar;
	}

	void InitFrom(FAngelscriptPrecompiledData& Context, class asCModule* Module, class asCScriptFunction* Function);

	asCScriptFunction* Create(FAngelscriptPrecompiledData& Context, class asCModule* Module) const;
	void Process(FAngelscriptPrecompiledData& Context, asCScriptFunction* Function) const;

	TSharedPtr<FAngelscriptFunctionDesc> MakeDesc() const;
};

struct FAngelscriptPrecompiledProperty
{
	// Angelscript data
	FStringInArchive Name;
	FAngelscriptPrecompiledDataType Type;

	// Preprocessor data
	TArray<FStringInArchive, TPrecompiledAllocator<>> MetaSpec;
	TArray<FStringInArchive, TPrecompiledAllocator<>> MetaValues;
	uint8 ReplicationCondition;
	uint8 bIsPrivate : 1 = false;
	uint8 bIsProtected : 1 = false;
	uint8 bIsUnrealProperty : 1 = false;
	uint8 bBlueprintReadable : 1 = false;
	uint8 bBlueprintWritable : 1 = false;
	uint8 bEditConst : 1 = false;
	uint8 bEditableOnDefaults : 1 = false;
	uint8 bEditableOnInstance : 1 = false;
	uint8 bInstancedReference : 1 = false;
	uint8 bPersistentInstance : 1 = false;
	uint8 bAdvancedDisplay : 1 = false;
	uint8 bTransient : 1 = false;
	uint8 bReplicated : 1 = false;
	uint8 bSkipReplication : 1 = false;
	uint8 bSkipSerialization : 1 = false;
	uint8 bSaveGame : 1 = false;
	uint8 bRepNotify : 1 = false;
	uint8 bConfig : 1 = false;
	uint8 bInterp : 1 = false;
	uint8 bAssetRegistrySearchable : 1 = false;
                                                                                                     
	uint8 bUeDelayedDestroyOwner : 1 = false;
                                                                                                   

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledProperty& Data)
	{
		Ar << Data.Name;
		Ar << Data.Type;

		if (Ar.IsSaving())
		{
			uint32 Mask = 0;
			uint8 Bit = 0;
			if (Data.bIsPrivate) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bIsProtected) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bIsUnrealProperty) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bBlueprintReadable) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bBlueprintWritable) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bEditConst) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bEditableOnDefaults) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bEditableOnInstance) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bInstancedReference) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bPersistentInstance) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bAdvancedDisplay) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bTransient) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bReplicated) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bSkipReplication) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bSkipSerialization) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bSaveGame) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bRepNotify) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bConfig) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bInterp) { Mask |= 1<<Bit; } ++Bit;
			if (Data.bAssetRegistrySearchable) { Mask |= 1<<Bit; } ++Bit;
                                                                                                 
			if (Data.bUeDelayedDestroyOwner) { Mask |= (1u << 20); }
                                                                                               

			Ar << Mask;
		}
		else
		{
			uint32 Mask = 0;
			Ar << Mask;

			uint8 Bit = 0;
			Data.bIsPrivate = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bIsProtected = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bIsUnrealProperty = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bBlueprintReadable = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bBlueprintWritable = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bEditConst = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bEditableOnDefaults = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bEditableOnInstance = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bInstancedReference = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bPersistentInstance = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bAdvancedDisplay= ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bTransient = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bReplicated = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bSkipReplication = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bSkipSerialization = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bSaveGame = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bRepNotify = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bConfig = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bInterp = ((Mask & (1<<Bit)) != 0); ++Bit;
			Data.bAssetRegistrySearchable = ((Mask & (1<<Bit)) != 0); ++Bit;
                                                                                                 
			Data.bUeDelayedDestroyOwner = (Mask & (1u << 20)) != 0;
                                                                                               
		}

		if (Data.bIsUnrealProperty)
		{
			Ar << Data.MetaSpec;
			Ar << Data.MetaValues;
			if (Data.bReplicated)
				Ar << Data.ReplicationCondition;
		}
		return Ar;
	}

	void InitFrom(FAngelscriptPrecompiledData& Context, class asCObjectProperty* Property);
	class asCObjectProperty* Create(FAngelscriptPrecompiledData& Context) const;
};

struct FAngelscriptPrecompiledClass
{
	// Angelscript data
	FStringInArchive ClassName;
	FStringInArchive Namespace;
	int32 Flags;
	TArray<FAngelscriptPrecompiledProperty, TPrecompiledAllocator<>> Properties;
	TArray<FAngelscriptPrecompiledFunction, TPrecompiledAllocator<>> Methods;
	TArray<int32, TPrecompiledAllocator<>> MethodTable;
	FAngelscriptPrecompiledReference DerivedFrom;
	FAngelscriptPrecompiledReference ShadowType;
	TArray<FAngelscriptPrecompiledFunction, TPrecompiledAllocator<>> Constructors;
	TArray<FAngelscriptPrecompiledReference, TPrecompiledAllocator<>> FactoryRefs;
	TArray<FAngelscriptPrecompiledReference, TPrecompiledAllocator<>> BehaviorRefs;
	TArray<FAngelscriptPrecompiledFunction, TPrecompiledAllocator<>> BehaviorFunctions;
	TArray<int32, TPrecompiledAllocator<>> BehaviorFunctionTypes;

	// Preprocessor data
	bool bIsInPreprocessor;
	FStringInArchive SuperClass;
	FStringInArchive CodeSuperClass;
	bool bSuperIsCodeClass;
	bool bIsStaticsClass;
	bool bAbstract;
	bool bTransient;
	bool bHideDropdown;
	bool bDefaultToInstanced;
	bool bEditInlineNew;
	bool bIsDeprecatedClass;
	bool bPlaceableOverride;
	bool bPlaceable;
	FStringInArchive ConfigName;
	FStringInArchive StaticClassGlobalVariableName;
	TArray<FStringInArchive, TPrecompiledAllocator<>> MetaSpec;
	TArray<FStringInArchive, TPrecompiledAllocator<>> MetaValues;
	FStringInArchive ComposeOntoClassName;

	// TRANSIENT
	mutable bool bFunctionsPreProcessed = false;
	// END TRANSIENT

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledClass& Data)
	{
		Ar << Data.ClassName;
		Ar << Data.Namespace;
		Ar << Data.Flags;
		Ar << Data.Properties;
		Ar << Data.Methods;
		Ar << Data.MethodTable;
		Ar << Data.DerivedFrom;
		Ar << Data.ShadowType;
		Ar << Data.Constructors;
		Ar << Data.FactoryRefs;
		Ar << Data.BehaviorRefs;
		Ar << Data.BehaviorFunctions;
		Ar << Data.BehaviorFunctionTypes;

		Ar << Data.bIsInPreprocessor;
		if (Data.bIsInPreprocessor)
		{
			Ar << Data.SuperClass;
			Ar << Data.CodeSuperClass;
			Ar << Data.bSuperIsCodeClass;
			Ar << Data.bAbstract;
			Ar << Data.bTransient;
			Ar << Data.bHideDropdown;
			Ar << Data.bDefaultToInstanced;
			Ar << Data.bEditInlineNew;
			Ar << Data.bIsDeprecatedClass;
			Ar << Data.bPlaceableOverride;
			Ar << Data.bPlaceable;
			Ar << Data.ConfigName;
			Ar << Data.StaticClassGlobalVariableName;
			Ar << Data.MetaSpec;
			Ar << Data.MetaValues;
			Ar << Data.ComposeOntoClassName;
		}
		return Ar;
	}

	void InitFrom(FAngelscriptPrecompiledData& Context, class asCModule* Module, class asCObjectType* Type);

	asCObjectType* Create(FAngelscriptPrecompiledData& Context, class asCModule* Module) const;
	void ProcessProperties(FAngelscriptPrecompiledData& Context, asCObjectType* Type) const;
	void CreateFunctions(FAngelscriptPrecompiledData& Context, asCObjectType* Type) const;
	void PreProcessFunctions(FAngelscriptPrecompiledData& Context, asCObjectType* Type) const;
	void ProcessFunctions(FAngelscriptPrecompiledData& Context, asCObjectType* Type) const;
};

struct FAngelscriptPrecompiledEnum
{
	FStringInArchive Name;
	FStringInArchive Namespace;
	TArray<FStringInArchive, TPrecompiledAllocator<>> EnumNames;
	TArray<int32, TPrecompiledAllocator<>> EnumValues;

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledEnum& Data)
	{
		Ar << Data.Name;
		Ar << Data.Namespace;
		Ar << Data.EnumNames;
		Ar << Data.EnumValues;
		return Ar;
	}

	void InitFrom(FAngelscriptPrecompiledData& Context, class asCModule* Module, class asCEnumType* Type);
	class asCEnumType* Create(FAngelscriptPrecompiledData& Context, class asCModule* Module) const;
};

struct FAngelscriptPrecompiledGlobalVariable
{
	FStringInArchive Name;
	FStringInArchive Namespace;
	FAngelscriptPrecompiledDataType Type;
	uint64 PureConstantValue;
	bool bIsPureConstant = false;
	bool bIsDefaultInit = false;
	bool bHasInitFunction = false;
	FAngelscriptPrecompiledFunction InitFunc;

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledGlobalVariable& Data)
	{
		Ar << Data.Name;
		Ar << Data.Namespace;
		Ar << Data.Type;

		Ar << Data.bIsDefaultInit;
		if (!Data.bIsDefaultInit)
		{
			Ar << Data.bIsPureConstant;
			if (Data.bIsPureConstant)
			{
				Ar << Data.PureConstantValue;
			}
			else
			{
				Ar << Data.bHasInitFunction;
				Ar << Data.InitFunc;
			}
		}

		return Ar;
	}

	void InitFrom(FAngelscriptPrecompiledData& Context, class asCModule* Module, class asCGlobalProperty* Property);
	class asCGlobalProperty* Create(FAngelscriptPrecompiledData& Context, class asCModule* Module) const;
	void Process(FAngelscriptPrecompiledData& Context, class asCModule* Module, class asCGlobalProperty* Property) const;
};

                                                                                                  
struct FAngelscriptPrecompiledModuleReference
{
	FStringInArchive ModuleName;
	int32 Row = 1;
	int32 Column = 1;
	uint8 Flags = 0;
	friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledModuleReference& Data)
	{
		Ar << Data.ModuleName << Data.Row << Data.Column << Data.Flags;
		return Ar;
	}
};
                                                                                                
struct FAngelscriptPrecompiledModule
{
                                                                                             
	FStringInArchive ScriptUnitId;
	uint8 ScriptLayer = 0;
	TArray<FStringInArchive, TPrecompiledAllocator<>> ScriptUnitDependencies;
                                                                                           
                                                                                                    
	TArray<FAngelscriptPrecompiledModuleReference, TPrecompiledAllocator<>> ModuleReferences;
                                                                                                  
	FStringInArchive ModuleName;
	TArray<FAngelscriptPrecompiledFunction, TPrecompiledAllocator<>> Functions;
	TArray<FAngelscriptPrecompiledClass, TPrecompiledAllocator<>> Classes;
	TArray<FAngelscriptPrecompiledEnum, TPrecompiledAllocator<>> Enums;
	TArray<FAngelscriptPrecompiledGlobalVariable, TPrecompiledAllocator<>> GlobalVariables;

	FStringInArchive StaticsClassName;
	TArray<FStringInArchive, TPrecompiledAllocator<>> DeclaredEvents;
	TArray<FStringInArchive, TPrecompiledAllocator<>> DeclaredDelegates;
	FStringInArchive ScriptRelativeFilename;
	TArray<FStringInArchive, TPrecompiledAllocator<>> PostInitFunctions;
	TArray<FString> Dependencies;

	// TRANSIENT:
	mutable TArray<class asCObjectType*> ClassTypes;
	mutable TArray<asCGlobalProperty*> GlobalProperties;
	mutable TArray<asCScriptFunction*> GlobalFunctions;
	void ProcessProperties(FAngelscriptPrecompiledData& Context, asITypeInfo* TypeInfo) const;
	// END TRANSIENT

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledModule& Data)
	{
		Ar << Data.ModuleName;
                                                                                                      
		Ar << Data.ScriptUnitId;
		Ar << Data.ScriptLayer;
		Ar << Data.ScriptUnitDependencies;
                                                                                                    
                                                                                                       
		Ar << Data.ModuleReferences;
                                                                                                     
		Ar << Data.Functions;
		Ar << Data.Classes;
		Ar << Data.Enums;
		Ar << Data.GlobalVariables;
		Ar << Data.StaticsClassName;
		Ar << Data.DeclaredEvents;
		Ar << Data.DeclaredDelegates;
		Ar << Data.ScriptRelativeFilename;
		Ar << Data.PostInitFunctions;
		return Ar;
	}

	void InitFrom(FAngelscriptPrecompiledData& Context, class asCModule* Module);

	void ApplyToModule_Stage1(FAngelscriptPrecompiledData& Context, class asIScriptModule* Module) const;
	void ApplyToModule_Stage2(FAngelscriptPrecompiledData& Context, class asIScriptModule* Module) const;
	void ApplyToModule_Stage3(FAngelscriptPrecompiledData& Context, class asIScriptModule* Module) const;
};

                                                                                                  
struct FAngelscriptPrecompiledClassOwner
{
	FAngelscriptPrecompiledData* Context = nullptr;
	const FAngelscriptPrecompiledClass* Class = nullptr;
};
                                                                                                

struct FAngelscriptTypeReference
{
	FStringInArchive Name;
	FStringInArchive Module;
	FStringInArchive Namespace;
	TArray<FAngelscriptPrecompiledDataType, TPrecompiledAllocator<>> SubTypes;

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptTypeReference& Data)
	{
		Ar << Data.Name;
		Ar << Data.Module;
		Ar << Data.Namespace;
		Ar << Data.SubTypes;
		return Ar;
	}
};

struct FAngelscriptFunctionReference
{
	FStringInArchive Name;
	FStringInArchive Module;
	FStringInArchive Namespace;
	bool bIsConst = false;
	bool bIsMethod = false;
	FAngelscriptPrecompiledReference ObjectType;
	TArray<FAngelscriptPrecompiledDataType, TPrecompiledAllocator<>> ParameterTypes;
	FAngelscriptPrecompiledDataType ReturnType;

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptFunctionReference& Data)
	{
		Ar << Data.Name;
		Ar << Data.Module;
		Ar << Data.Namespace;
		Ar << Data.bIsConst;
		Ar << Data.bIsMethod;
		Ar << Data.ObjectType;
		Ar << Data.ParameterTypes;
		Ar << Data.ReturnType;
		return Ar;
	}

	bool MatchesSignature(FAngelscriptPrecompiledData& Context, class asCScriptFunction* Function) const;
};

struct FAngelscriptGlobalReference
{
	FStringInArchive Name;
	FStringInArchive Module;
	FStringInArchive Namespace;
	bool bIsString = false;

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptGlobalReference& Data)
	{
		Ar << Data.Name;
		Ar << Data.Module;
		Ar << Data.Namespace;
		Ar << Data.bIsString;
		return Ar;
	}
};

struct FAngelscriptPropertyReference
{
	FStringInArchive Name;
	int32 OldTypeId;

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPropertyReference& Data)
	{
		Ar << Data.Name;
		Ar << Data.OldTypeId;
		return Ar;
	}
};

struct FAngelscriptPrecompiledData
{
	// AllocMark must be at the top of this struct, because 
	// all the other members are allocated from that memstack,
	// and the topmost member will be destructed *last*.
	FMemMark AllocMark;

	template<typename ValueType>
	struct FIntAsPointerKeyFuncs : public TDefaultMapHashableKeyFuncs<int64, ValueType, false>
	{
		static FORCEINLINE uint32 GetKeyHash(int64 Key)
		{
			return GetTypeHash((void*)Key);
		}
	};

	template<typename K, typename V>
	struct TMapAsPtr : public TMap<K, V, FDefaultSetAllocator, FIntAsPointerKeyFuncs<V>>
	{};

	// Saved data
	FGuid DataGuid;
	                                                                                                  
	TMap<FString, FGuid> ModuleDataGuids;
	                                                                                                
	TMap<FString, FAngelscriptPrecompiledModule> Modules;
	TMapAsPtr<int64, FAngelscriptTypeReference> TypeReferences;
	TMap<int, int64> TypeIdReferenceToPointer;
	TMapAsPtr<int64, FAngelscriptFunctionReference> FunctionReferences;
	TMap<int, int64> FunctionIdReferenceToPointer;
	TMapAsPtr<int64, FAngelscriptGlobalReference> GlobalReferences;
	TMapAsPtr<int64, FAngelscriptPropertyReference> PropertyReferences;
	TArray<FStringInArchive> StaticNames;
	int32 BuildIdentifier = -1;

	// We keep the original loaded data so we can reference strings inside it
	TArray<uint8> LoadedData;

	// Minimize memory usage mode reduces the amount of memory we spend on
	// angelscript data, but removes some debugging information.
	bool bMinimizeMemoryUsage = false;
                                                                                               
	TSet<const void*> FinalizedJitSlots;
	bool bStaticNamesFinalized = false;
	bool IsReferenceReady(int64 Reference) const;
                                                                                             

	inline friend FArchive& operator<<(FArchive& Ar, FAngelscriptPrecompiledData& Data)
	{
		Ar << Data.DataGuid;
		Ar << Data.BuildIdentifier;
                                                                                                      
		if (Ar.IsLoading() && Data.BuildIdentifier != Data.GetCurrentBuildIdentifier())
		{
			Ar.SetError();
			return Ar;
		}
                                                                                                    
		Ar << Data.Modules;
		Ar << Data.TypeReferences;
		Ar << Data.TypeIdReferenceToPointer;
		Ar << Data.FunctionReferences;
		Ar << Data.FunctionIdReferenceToPointer;
		Ar << Data.GlobalReferences;
		Ar << Data.StaticNames;
		Ar << Data.PropertyReferences;
		return Ar;
	}

	FAngelscriptPrecompiledData(asIScriptEngine* InEngine);
	~FAngelscriptPrecompiledData();

	void InitFromActiveScript();
	static void OutputTimingData();

	int32 GetCurrentBuildIdentifier();
	bool IsValidForCurrentBuild();

	void Save(const FString& Filename);
	                                                                                                  
	bool SaveSerialized(const FString& Filename);
	bool SaveModuleCacheSets(FString& OutError);
	static bool ReadModuleCacheSet(const FString& CacheDirectory, TArray<FAngelscriptPrecompiledCacheFile>& OutFiles, FString& OutError, const TArray<uint8>* VerifiedManifestBytes = nullptr);
	                                                                                                
                                                                                                                                    
	void Load(const FString& Filename, const TArray<uint8>* VerifiedBytes = nullptr);
                                                                                                                                  

	struct asSNameSpace* GetNamespace(const FStringInArchive& Namespace);
	void AddRefTo(const class asCDataType& DataType);

	void PrepareToFinalizePrecompiledModules();
	void ClearUnneededRuntimeData();

	// Serve loaded data that would normally come from preprocessor
	TArray<TSharedRef<FAngelscriptModuleDesc>> GetModulesToCompile();

	// Processed function id maps
	TMap<asIScriptFunction*, uint32> ProcessedFunctionToId;
	TMap<uint32, asIScriptFunction*> ProcessedIdToFunction;

	                                                                                                  
	TSharedPtr<TMap<void*, FAngelscriptPrecompiledClassOwner>> ClassesLoadedFromPrecompiledData;
	void AddModuleDependencies(class asCModule* Module, const TArray<FString>& Dependencies) const;
	bool AppendStaticNames(FString& OutError) const;
	                                                                                                
	void ProcessProperties(asITypeInfo* TypeInfo);

	uint32 CreateFunctionId(asIScriptFunction* Function);
	void MapFunctionId(asIScriptFunction* Function, uint32 Id);

	bool GetIdForFunction(asIScriptFunction* Function, uint32& OutId);

	// Context state and reference maps
	asCScriptEngine* Engine = nullptr;
	TMapAsPtr<int64, void*> CachedPointerReferences;
                                                                                                          
	// Init functions share the ordinary global-property and engine ownership graph.
                                                                                                        

	int32 ScriptSectionIdx = -1;
	const FStringInArchive* ScriptRelativeFilename = nullptr;

	TSharedPtr<FAngelscriptModuleDesc> ModuleDesc;
	TSharedPtr<FAngelscriptClassDesc> ClassDesc;
	TSharedPtr<FAngelscriptPropertyDesc> PropertyDesc;
	TSharedPtr<FAngelscriptFunctionDesc> FunctionDesc;

	int32 GetScriptSection();

	// References
	FAngelscriptPrecompiledReference ReferenceTypeInfo(class asITypeInfo* TypeInfo);
	class asCTypeInfo* GetTypeInfo(const FAngelscriptPrecompiledReference& Reference, bool bAddRef = false);

	FAngelscriptPrecompiledReference ReferenceTypeId(int TypeId);
	int GetTypeId(const FAngelscriptPrecompiledReference& Reference, bool bAddRef = false);

	FAngelscriptPrecompiledReference ReferenceFunction(class asIScriptFunction* Function);
	class asCScriptFunction* GetFunction(const FAngelscriptPrecompiledReference& Reference, bool bAddRef = false, bool bMarkInUse = true);

	FAngelscriptPrecompiledReference ReferenceFunctionId(int FunctionId);
	int GetFunctionId(const FAngelscriptPrecompiledReference& Reference, bool bAddRef = false, bool bMarkInUse = true);

	FAngelscriptPrecompiledReference ReferenceGlobalVariable(void* Pointer, FString* OutName = nullptr);
	void* GetGlobalVariable(const FAngelscriptPrecompiledReference& Reference, class asCGlobalProperty** OutProperty = nullptr);

	FAngelscriptPrecompiledReference ReferenceProperty(int Offset, int TypeId, class asCObjectProperty** OutProperty = nullptr, class asCObjectType** OutObjectType = nullptr);
	int64 GetPropertyReferenceId(int Offset, int TypeId);
	short GetPropertyOffset(const FAngelscriptPrecompiledReference& Reference);
	short GetPropertyOffset(int OldOffset, int OldTypeId);
	short GetTypeSize(int NewTypeId, short OldSize);
};
