#include "PrecompiledData.h"
                                                                                                           
#include <cstdio>
                                                                                                         

                                                                                                      
#include "Extensions/AngelscriptModdingInternal.h"
#include "Misc/CommandLine.h"
                                                                                                    
                                                                                                       
#include "Extensions/AngelscriptModdingInternal.h"
                                                                                                     

#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"

#include "AngelscriptManager.h"
#include "StaticJIT/StaticJITConfig.h"
#include "StaticJIT/StaticJITHeader.h"
                                                                                                  
#include "Extensions/Cache/AngelscriptExtensionCacheCompatibility.h"
#include "Extensions/AngelscriptExtensionHooks.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
                                                                                                

#include "StartAngelscriptHeaders.h"
#include "as_scriptfunction.h"
#include "as_objecttype.h"
#include "as_module.h"
#include "as_scriptengine.h"
#include "as_callfunc.h"
#include "EndAngelscriptHeaders.h"

                                                                                                       
// Byte-level transform mechanics for the precompiled-data gate. The walk is a
// single pass over each bytecode array (instruction boundaries via the public
// asBCInfo/asBCTypeSize tables); the permutation table itself is strategy from
// the ModKit handler. The reverse walk restores the original opcode FIRST and
// only then looks up its size (asBCTypeSize lookup after the inverse map),
// stepping by DWORDs so a permuted instruction is never misread.
namespace
{
	template<typename Allocator>
	void CuvissApplyOpcodePermutation(TArray<int32, Allocator>& ByteCode, const TArray<uint8>& ForwardMap, bool bInverse)
	{
		if (ByteCode.Num() == 0 || ForwardMap.Num() != 256)
		{
			return;
		}

		uint8 Inverse[256];
		if (bInverse)
		{
			for (int32 i = 0; i < 256; ++i)
			{
				Inverse[ForwardMap[i]] = (uint8)i;
			}
		}

		asDWORD* bc = (asDWORD*)ByteCode.GetData();
		const asDWORD* bcEnd = bc + ByteCode.Num();
		while (bc < bcEnd)
		{
			// Size lookup always uses the ORIGINAL opcode: forward walks read it
			// before writing the permuted value; the inverse walk restores it
			// first and then looks up the restored value.
			const uint8 OriginalOpcode = (uint8)(*bc & 0xFF);
			const uint8 NewOpcode = bInverse ? Inverse[OriginalOpcode] : ForwardMap[OriginalOpcode];
			const int32 InstrSize = asBCTypeSize[asBCInfo[bInverse ? NewOpcode : OriginalOpcode].type];
			*bc = (*bc & ~0xFFu) | NewOpcode;
			bc += InstrSize;
		}
	}

	template<typename Fn>
	void CuvissForEachByteCodeArray(FAngelscriptPrecompiledData& Data, Fn&& Func)
	{
		for (auto& ModulePair : Data.Modules)
		{
			FAngelscriptPrecompiledModule& Module = ModulePair.Value;
			for (FAngelscriptPrecompiledFunction& Function : Module.Functions)
			{
				Func(Module, Function, false);
			}
			for (FAngelscriptPrecompiledGlobalVariable& GlobalVariable : Module.GlobalVariables)
			{
				Func(Module, GlobalVariable.InitFunc, true);
			}
			for (FAngelscriptPrecompiledClass& Class : Module.Classes)
			{
				for (FAngelscriptPrecompiledFunction& Method : Class.Methods)
				{
					Func(Module, Method, false);
				}
				for (FAngelscriptPrecompiledFunction& Constructor : Class.Constructors)
				{
					Func(Module, Constructor, true);
				}
				for (FAngelscriptPrecompiledFunction& Behavior : Class.BehaviorFunctions)
				{
					Func(Module, Behavior, true);
				}
			}
		}
	}

	FString CuvissMakeJitFunctionIdentity(const FString& ModuleName, const FString& FunctionName, uint32 FunctionId)
	{
		if (ModuleName.IsEmpty() || FunctionName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%d:%s|%d:%s|%08x"), ModuleName.Len(), *ModuleName,
			FunctionName.Len(), *FunctionName, FunctionId);
	}

	void CuvissStripDebugInfo(FAngelscriptPrecompiledData& Data)
	{
		CuvissForEachByteCodeArray(Data, [](FAngelscriptPrecompiledModule&, FAngelscriptPrecompiledFunction& Function, bool)
		{
			Function.LineNumbers.Reset();
			Function.DeclaredAt = 0;
		});
	}
}
                                                                                                     

                                                                                                  
namespace
{
	const TCHAR* ModuleCacheManifestName = TEXT("ModuleCaches.json");
	const TCHAR* ModuleCacheDirectoryName = TEXT("ModuleCaches");
	const int32 ModuleCacheManifestVersion = 2;

	FString GetModuleCacheDirectory(const FString& ScriptRelativeFilename)
	{
		FString NormalizedFilename = ScriptRelativeFilename;
		FPaths::NormalizeFilename(NormalizedFilename);
		TArray<FString> PathParts;
		NormalizedFilename.ParseIntoArray(PathParts, TEXT("/"), true);
		if (PathParts.Num() >= 3 && PathParts[0] == TEXT("FeatureMods") && PathParts[2] == TEXT("Script"))
		{
			return FPaths::ProjectDir() / TEXT("FeatureMods") / PathParts[1] / TEXT("Script") / ModuleCacheDirectoryName;
		}
		if (PathParts.Num() >= 4
			&& PathParts[0] == TEXT("Plugins")
			&& PathParts[1] == TEXT("FeatureMods")
			&& PathParts[3] == TEXT("Script"))
		{
			return FPaths::ProjectPluginsDir() / TEXT("FeatureMods") / PathParts[2] / TEXT("Script") / ModuleCacheDirectoryName;
		}
		if (PathParts.Num() >= 3 && PathParts[0] == TEXT("Mods") && PathParts[2] == TEXT("Script"))
		{
			return FPaths::ProjectDir() / TEXT("Mods") / PathParts[1] / TEXT("Script") / ModuleCacheDirectoryName;
		}
		return FAngelscriptManager::GetScriptRootDirectory() / ModuleCacheDirectoryName;
	}

	FString GetModuleCacheFilename(const FString& ModuleName)
	{
	return FMD5::HashAnsiString(*ModuleName) + TEXT(".Cache");
	}
}
                                                                                                

double TIMER_ProcessBytecode = 0.0;
double TIMER_CreateFunction = 0.0;
double   TIMER_LoadBytecode = 0.0;
double   TIMER_CreateParameters = 0.0;
double TIMER_CreateFunctionSignature = 0.0;
double TIMER_CreateClass = 0.0;
double TIMER_ProcessProperties = 0.0;
double TIMER_CreateFunctions = 0.0;
double TIMER_PreProcessFunctions = 0.0;
double   TIMER_AppendProperties = 0.0;
double   TIMER_VirtualTableLookup = 0.0;
double   TIMER_AppendShadowMethods = 0.0;
double TIMER_ProcessFunctions = 0.0;
double TIMER_TypeInfoLookup = 0.0;
double   TIMER_GetNamespace = 0.0;
double   TIMER_FindModuleForType = 0.0;
double   TIMER_FindTypeInModule = 0.0;
double   TIMER_FindTypeInEngine = 0.0;
double   TIMER_FindHardcodedType = 0.0;
double   TIMER_FindTemplateInstance = 0.0;
double   TIMER_FindTemplateSubType = 0.0;
double   TIMER_UpdateReferenceMap = 0.0;
double   TIMER_TypeReferenceFind = 0.0;
double TIMER_FunctionLookup = 0.0;
double TIMER_GlobalLookup = 0.0;
double TIMER_PropertyLookup = 0.0;
double TIMER_GetModulesToCompile = 0.0;

FMemStackBase GScriptPreallocatedMemStack;

struct FAppliedFunctionSignature
{
	asCString funcName;
	asCDataType returnType;
	asCArray<asCDataType> params;
	asCArray<asETypeModifiers> inOutFlags;
	asCArray<asCString*> defaultArgs;
	asSNameSpace* ns = nullptr;
};

void FAngelscriptPrecompiledDataType::InitFrom(FAngelscriptPrecompiledData& Context, asCDataType& DataType)
{
	bIsReference = DataType.IsReference();
	bIsObjectConst = DataType.IsObjectConst();
	bIsConstHandle = DataType.IsReadOnly();
	bIsObjectHandle = DataType.IsObjectHandle();
	bIsAuto = DataType.IsAuto();
	bIfHandleThenConst = DataType.HasIfHandleThenConst();
	TokenType = (int32)DataType.GetTokenType();
	TypeInfo = Context.ReferenceTypeInfo(DataType.GetTypeInfo());
}

void FAngelscriptPrecompiledDataType::Create(FAngelscriptPrecompiledData& Context, class asCDataType& DataType) const
{
	if (bIsAuto)
	{
		DataType = asCDataType::CreateAuto(bIsObjectConst);
		if (bIsObjectHandle)
		{
			DataType.MakeHandle(true);
			DataType.MakeReadOnly(bIsConstHandle);
		}
		if (bIsReference)
			DataType.MakeReference(true);
	}
	else if (!TypeInfo.IsNull())
	{
		if (bIsObjectHandle)
		{
			DataType = asCDataType::CreateObjectHandle(Context.GetTypeInfo(TypeInfo), bIsConstHandle);
			DataType.MakeHandleToConst(bIsObjectConst);
		}
		else
		{
			DataType = asCDataType::CreateType(Context.GetTypeInfo(TypeInfo), bIsObjectConst);
		}

		if (bIsReference)
			DataType.MakeReference(true);
		if (bIfHandleThenConst)
			DataType.SetIfHandleThenConst(true);
	}
	else
	{
		DataType = asCDataType::CreatePrimitive((eTokenType)TokenType, bIsObjectConst);
		if (bIsReference)
			DataType.MakeReference(true);
	}
}

struct FAngelscriptBytecodeReferencer
{
	FAngelscriptPrecompiledData& Context;
	TArray<asCGlobalProperty*, TInlineAllocator<8>> ReferencedProperties;

	FAngelscriptBytecodeReferencer(FAngelscriptPrecompiledData& InContext)
		: Context(InContext)
	{
	}

	void StoreGlobalPtr(asPWORD& GPtr)
	{
		auto Ref = Context.ReferenceGlobalVariable((void*)GPtr);
		check(Ref.OldReference == (int64)GPtr);
	}

	void LoadGlobalPtr(asPWORD& GPtr)
	{
		asCGlobalProperty* Property = nullptr;
		GPtr = (asPWORD)Context.GetGlobalVariable(FAngelscriptPrecompiledReference{ (int64)GPtr }, &Property);

		if (Property != nullptr && !ReferencedProperties.Contains(Property))
		{
			Property->AddRef();
			ReferencedProperties.Add(Property);
		}
	}

	void StoreFunctionId(int& Id)
	{
		auto Ref = Context.ReferenceFunctionId(Id);
		check(Ref.OldReference == (int64)Id);
	}

	void LoadFunctionId(int& Id)
	{
		Id = Context.GetFunctionId(FAngelscriptPrecompiledReference{ (int64)Id }, true);
	}

	void StoreFunctionPtr(asPWORD& Function)
	{
		auto Ref = Context.ReferenceFunction((asCScriptFunction*)Function);
		check(Ref.OldReference == (int64)Function);
	}

	void LoadFunctionPtr(asPWORD& Function)
	{
		Function = (asPWORD)Context.GetFunction(FAngelscriptPrecompiledReference{ (int64)Function }, true);
	}

	void StoreTypeInfo(asPWORD& TypeInfo)
	{
		auto Ref = Context.ReferenceTypeInfo((asITypeInfo*)TypeInfo);
		check(Ref.OldReference == (int64)TypeInfo);
	}

	void LoadTypeInfo(asPWORD& TypeInfo)
	{
		TypeInfo = (asPWORD)Context.GetTypeInfo(FAngelscriptPrecompiledReference{ (int64)TypeInfo }, true);
	}

	void StoreTypeId(int& Id)
	{
		auto Ref = Context.ReferenceTypeId(Id);
		check(Ref.OldReference == (int64)Id);
	}

	void LoadTypeId(int& Id)
	{
		Id = Context.GetTypeId(FAngelscriptPrecompiledReference{ (int64)Id }, true);
	}

	void Store(asDWORD* bc, int32 SizeDWords)
	{
		FAngelscriptScopeTotalTimer Timer(TIMER_ProcessBytecode);

		asDWORD* bc_End = bc + SizeDWords;
		while (bc < bc_End)
		{
			asEBCInstr Instr = (asEBCInstr)*(asBYTE*)bc;

			// Check which references to process in the bytecode
			switch (Instr)
			{
			case asBC_PshGPtr:
			case asBC_PshG4:
			case asBC_LdGRdR4:
			case asBC_CpyVtoG4:
			case asBC_CpyGtoV4:
			case asBC_LDG:
			case asBC_PGA:
			case asBC_SetG4:
				StoreGlobalPtr(asBC_PTRARG(bc));
			break;
			case asBC_CALL:
			case asBC_CALLBND:
			case asBC_CALLINTF:
				StoreFunctionId(asBC_INTARG(bc));
			break;
			case asBC_CopyScript:
			case asBC_FinConstruct:
			case asBC_FREE:
			case asBC_DestructScript:
			case asBC_OBJTYPE:
				StoreTypeInfo(asBC_PTRARG(bc));
			break;
			case asBC_COPY:
				StoreTypeId(asBC_INTARG(bc));
			break;
			case asBC_STR:
				check(false);
			break;
			case asBC_CALLSYS:
			case asBC_FuncPtr:
			case asBC_Thiscall1:
				StoreFunctionPtr(asBC_PTRARG(bc));
			break;
			case asBC_ALLOC:
				StoreTypeInfo(asBC_PTRARG(bc));
				StoreFunctionId(asBC_INTARG(bc + AS_PTR_SIZE));
			break;
			case asBC_TYPEID:
			case asBC_Cast:
				StoreTypeId(asBC_INTARG(bc));
			break;
			case asBC_ADDSi:
			case asBC_LoadThisR:
			case asBC_PshThisMemObj:
			case asBC_PshThisMemRef:
			{
				asCObjectProperty* Property = nullptr;
				asCObjectType* InType = nullptr;
				Context.ReferenceProperty(asBC_SWORDARG0(bc), asBC_INTARG(bc), &Property, &InType);
				asBC_INTARG(bc) = InType->GetTypeId();
			}
			break;
			case asBC_LoadRObjR:
			case asBC_LoadVObjR:
			case asBC_PshVMemObj:
			case asBC_PshVMemRef:
			case asBC_PSFVMemRef:
			{
				asCObjectProperty* Property = nullptr;
				asCObjectType* InType = nullptr;
				Context.ReferenceProperty(
					asBC_SWORDARG1(bc),
					*(int*)(bc+2),
					&Property, &InType);
				*(int*)(bc+2) = InType->GetTypeId();
			}
			break;
			case asBC_SetListType:
				StoreTypeId(asBC_INTARG(bc+1));
			break;
			}

			// Advance bytecode pointer
			int32 InstrSize = asBCTypeSize[asBCInfo[Instr].type];
			bc += InstrSize;
		}
	}

	void Load(asDWORD* bc, int32 SizeDWords)
	{
		FAngelscriptScopeTotalTimer Timer(TIMER_ProcessBytecode);

		asDWORD* bc_End = bc + SizeDWords;
		while (bc < bc_End)
		{
			asEBCInstr Instr = (asEBCInstr)*(asBYTE*)bc;

			// Check which references to process in the bytecode
			switch (Instr)
			{
			case asBC_PshGPtr:
			case asBC_PshG4:
			case asBC_LdGRdR4:
			case asBC_CpyVtoG4:
			case asBC_CpyGtoV4:
			case asBC_LDG:
			case asBC_PGA:
			case asBC_SetG4:
				LoadGlobalPtr(asBC_PTRARG(bc));
			break;
			case asBC_CALL:
			case asBC_CALLBND:
			case asBC_CALLINTF:
				LoadFunctionId(asBC_INTARG(bc));
			break;
			case asBC_CopyScript:
			case asBC_FinConstruct:
			case asBC_FREE:
			case asBC_DestructScript:
			case asBC_OBJTYPE:
				LoadTypeInfo(asBC_PTRARG(bc));
			break;
			case asBC_COPY:
				LoadTypeId(asBC_INTARG(bc));
				asBC_SWORDARG0(bc) = Context.GetTypeSize(asBC_INTARG(bc), asBC_SWORDARG0(bc));
			break;
			case asBC_STR:
				check(false);
			break;
			case asBC_CALLSYS:
			case asBC_FuncPtr:
			case asBC_Thiscall1:
				LoadFunctionPtr(asBC_PTRARG(bc));
			break;
			case asBC_ALLOC:
				LoadTypeInfo(asBC_PTRARG(bc));
				LoadFunctionId(asBC_INTARG(bc + AS_PTR_SIZE));
			break;
			case asBC_TYPEID:
			case asBC_Cast:
				LoadTypeId(asBC_INTARG(bc));
			break;
			case asBC_SetListType:
				LoadTypeId(asBC_INTARG(bc+1));
			break;
			case asBC_ADDSi:
			case asBC_LoadThisR:
			case asBC_PshThisMemObj:
			case asBC_PshThisMemRef:
			{
				int OldOffset = asBC_SWORDARG0(bc);
				int OldTypeId = asBC_INTARG(bc);

				short NewOffset = Context.GetPropertyOffset(OldOffset, OldTypeId);

				asBC_SWORDARG0(bc) = NewOffset;

				// We don't use the typeid at runtime when running bytecode,
				// so no need to look it up.
				asBC_INTARG(bc) = -1;

				break;
			}
			case asBC_LoadRObjR:
			case asBC_LoadVObjR:
			case asBC_PshVMemObj:
			case asBC_PshVMemRef:
			case asBC_PSFVMemRef:
			{
				int OldOffset = asBC_SWORDARG1(bc);
				int OldTypeId = *(int*)(bc+2);

				short NewOffset = Context.GetPropertyOffset(OldOffset, OldTypeId);

				asBC_SWORDARG1(bc) = NewOffset;

				// We don't use the typeid at runtime when running bytecode,
				// so no need to look it up.
				*(int*)(bc+2) = -1;

				break;
			}
			}

			// Advance bytecode pointer
			int32 InstrSize = asBCTypeSize[asBCInfo[Instr].type];
			bc += InstrSize;
		}
	}
};

void FAngelscriptPrecompiledFunction::InitFrom(FAngelscriptPrecompiledData& Context, asCModule* Module, asCScriptFunction* Function)
{
	Id = Context.CreateFunctionId(Function);
	FunctionName = Function->GetName();
	Namespace = Function->GetNamespace();

	ReturnType.InitFrom(Context, Function->returnType);

	int32 ParamCount = Function->parameterTypes.GetLength();

	ParameterTypes.SetNum(ParamCount);
	ParameterNames.SetNum(ParamCount);
	ParameterFlags.SetNum(ParamCount);
	ParameterDefaultArgs.SetNum(ParamCount);

	for (int32 i = 0; i < ParamCount; ++i)
	{
		ParameterTypes[i].InitFrom(Context, Function->parameterTypes[i]);
		ParameterNames[i] = Function->parameterNames[i].AddressOf();
		ParameterFlags[i] = (int32)Function->inOutFlags[i];

		if (Function->defaultArgs[i] != nullptr)
			ParameterDefaultArgs[i] = Function->defaultArgs[i]->AddressOf();
		else
			ParameterDefaultArgs[i].Empty();
	}

	FunctionTraits = (int32)Function->traits.traits;

	auto* scriptData = Function->scriptData;
	VariableSpace = (int32)scriptData->variableSpace;

	int32 BytecodeCount = scriptData->byteCode.GetLength();
	ByteCode.SetNum(BytecodeCount);
	FMemory::Memcpy(ByteCode.GetData(), scriptData->byteCode.AddressOf(), BytecodeCount * sizeof(asDWORD));

	// Make sure all references in the bytecode are saved
	FAngelscriptBytecodeReferencer Referencer(Context);
	Referencer.Store((asDWORD*)ByteCode.GetData(), BytecodeCount);

	int32 VarCount = scriptData->objVariableTypes.GetLength();
	ObjVariableTypes.SetNum(VarCount);
	ObjVariablePos.SetNum(VarCount);
	for (int32 i = 0; i < VarCount; ++i)
	{
		ObjVariableTypes[i] = Context.ReferenceTypeInfo(scriptData->objVariableTypes[i]);
		ObjVariablePos[i] = scriptData->objVariablePos[i];
	}

	int32 VarInfoCount = scriptData->objVariableInfo.GetLength();
	VariableInfoProgramPos.SetNum(VarInfoCount);
	VariableInfoOffset.SetNum(VarInfoCount);
	VariableInfoOption.SetNum(VarInfoCount);

	for (int32 i = 0; i < VarInfoCount; ++i)
	{
		VariableInfoProgramPos[i] = scriptData->objVariableInfo[i].programPos;
		VariableInfoOffset[i] = scriptData->objVariableInfo[i].variableOffset;
		VariableInfoOption[i] = (int32)scriptData->objVariableInfo[i].option;
	}

	ObjVariablesOnHeap = scriptData->objVariablesOnHeap;
	StackNeeded = scriptData->stackNeeded;

#if !UE_BUILD_SHIPPING
	DeclaredAt = scriptData->declaredAt;

	int32 LineNumberCount = scriptData->lineNumbers.GetLength();
	LineNumbers.SetNum(LineNumberCount);
	for (int32 i = 0; i < LineNumberCount; ++i)
		LineNumbers[i] = scriptData->lineNumbers[i];
#endif

	if (Context.FunctionDesc.IsValid())
	{
		auto FunctionDesc = Context.FunctionDesc;
		bIsUFunction = true;

		for (auto Elem : FunctionDesc->Meta)
		{
			MetaSpec.Add(Elem.Key.ToString());
			MetaValues.Add(Elem.Value);
		}

		if (FunctionDesc->OriginalFunctionName.Len() != 0)
			UnrealFunctionName = FunctionDesc->OriginalFunctionName;
		else
			UnrealFunctionName = FunctionDesc->FunctionName;

		bBlueprintCallable = FunctionDesc->bBlueprintCallable;
		bBlueprintOverride = FunctionDesc->bBlueprintOverride;
		bBlueprintEvent = FunctionDesc->bBlueprintEvent;
		bBlueprintPure = FunctionDesc->bBlueprintPure;
		bNetFunction = FunctionDesc->bNetFunction;
		bNetMulticast = FunctionDesc->bNetMulticast;
		bNetClient = FunctionDesc->bNetClient;
		bNetServer = FunctionDesc->bNetServer;
		bNetValidate = FunctionDesc->bNetValidate;
		bUnreliable = FunctionDesc->bUnreliable;
		bBlueprintAuthorityOnly = FunctionDesc->bBlueprintAuthorityOnly;
		bExec = FunctionDesc->bExec;
		bCanOverrideEvent = FunctionDesc->bCanOverrideEvent;
		bDevFunction = FunctionDesc->bDevFunction;
		bIsStatic = FunctionDesc->bIsStatic;
		bIsConstMethod = FunctionDesc->bIsConstMethod;
		bThreadSafe = FunctionDesc->bThreadSafe;
		bIsNoOp = FunctionDesc->bIsNoOp;
	}
	else
	{
		bIsUFunction = false;
	}
}

asCScriptFunction* FAngelscriptPrecompiledFunction::Create(FAngelscriptPrecompiledData& Context, asCModule* Module) const
{
	FAngelscriptScopeTotalTimer Timer(TIMER_CreateFunction);

	asCScriptFunction* Function = asNEW(asCScriptFunction)(Context.Engine, Module, asFUNC_DUMMY);
	Function->funcType = asFUNC_SCRIPT;

	Context.MapFunctionId(Function, Id);

	Function->name = *FunctionName;
	Function->nameSpace = Context.GetNamespace(Namespace);

	ReturnType.Create(Context, Function->returnType);
	Context.AddRefTo(Function->returnType);
	Context.ProcessProperties(Function->returnType.GetTypeInfo());

	int32 ParamCount = ParameterTypes.Num();
	Function->parameterTypes.SetLength(ParamCount);
	Function->parameterNames.SetLength(ParamCount);
	Function->inOutFlags.SetLength(ParamCount);
	Function->defaultArgs.SetLength(ParamCount);

	{
		FAngelscriptScopeTotalTimer SubTimer(TIMER_CreateParameters);
		for (int32 i = 0; i < ParamCount; ++i)
		{
			ParameterTypes[i].Create(Context, Function->parameterTypes[i]);
			Context.AddRefTo(Function->parameterTypes[i]);
			Context.ProcessProperties(Function->parameterTypes[i].GetTypeInfo());

			Function->parameterNames[i] = *ParameterNames[i];
			Function->inOutFlags[i] = (asETypeModifiers)ParameterFlags[i];

			Function->defaultArgs[i] = nullptr;
			if (!Context.bMinimizeMemoryUsage && ParameterDefaultArgs[i].Len() != 0)
				Function->defaultArgs[i] = asNEW(asCString)(*ParameterDefaultArgs[i]);
		}
	}

	Function->traits.traits = (asDWORD)FunctionTraits;

	bool bHasJITFunctions = false;
	auto& JITDatabase = FJITDatabase::Get();
	auto* JITFunctions = JITDatabase.Functions.Find(Id);
	if (JITFunctions != nullptr && !FAngelscriptManager::bScriptDevelopmentMode)
	{
		Function->jitFunction = JITFunctions->VMEntry;
		Function->jitFunction_ParmsEntry = JITFunctions->ParmsEntry;
		Function->jitFunction_Raw= JITFunctions->RawFunction;
		bHasJITFunctions = true;
	}

	// If valid JIT data exists, generated functions don't load script data,
	// this is needed because generated functions don't have their JIT functions
	// registered themselves.
	if (!bHasJITFunctions
		&& Function->traits.GetTrait(asTRAIT_GENERATED_FUNCTION)
		&& !JITDatabase.Functions.IsEmpty())
	{
		bHasJITFunctions = true;
	}

	if (!bHasJITFunctions)
	{
		Function->AllocateScriptFunctionData();
		auto* scriptData = Function->scriptData;

		{
			FAngelscriptScopeTotalTimer SubTimer(TIMER_LoadBytecode);
			scriptData->byteCode.SetLength(ByteCode.Num());
			FMemory::Memcpy(scriptData->byteCode.AddressOf(), ByteCode.GetData(), ByteCode.Num() * sizeof(asDWORD));
		}

		scriptData->variableSpace = VariableSpace;

		int32 VarCount = ObjVariableTypes.Num();
		scriptData->objVariableTypes.SetLength(VarCount);
		scriptData->objVariablePos.SetLength(VarCount);
		for (int32 i = 0; i < VarCount; ++i)
		{
			scriptData->objVariableTypes[i] = Context.GetTypeInfo(ObjVariableTypes[i]);
			scriptData->objVariablePos[i] = ObjVariablePos[i];
		}

		scriptData->objVariablesOnHeap = ObjVariablesOnHeap;
		scriptData->stackNeeded = StackNeeded;

		int32 VarInfoCount = VariableInfoProgramPos.Num();
		scriptData->objVariableInfo.SetLength(VarInfoCount);

		for (int32 i = 0; i < VarInfoCount; ++i)
		{
			scriptData->objVariableInfo[i].programPos = VariableInfoProgramPos[i];
			scriptData->objVariableInfo[i].variableOffset = VariableInfoOffset[i];
			scriptData->objVariableInfo[i].option = (asEObjVarInfoOption)VariableInfoOption[i];
		}

		scriptData->declaredAt = DeclaredAt;
		scriptData->scriptSectionIdx = Context.GetScriptSection();

		int32 LineNumberCount = LineNumbers.Num();
		scriptData->lineNumbers.SetLength(LineNumberCount);
		for (int32 i = 0; i < LineNumberCount; ++i)
			scriptData->lineNumbers[i] = LineNumbers[i];
	}

	return Function;
}

void FAngelscriptPrecompiledFunction::Process(FAngelscriptPrecompiledData& Context, asCScriptFunction* Function) const
{
	// Replace references in the bytecode with their current versions
	if (Function->scriptData != nullptr)
	{
		FAngelscriptBytecodeReferencer Referencer(Context);
		Referencer.Load(Function->scriptData->byteCode.AddressOf(), Function->scriptData->byteCode.GetLength());
	}
}

void FAngelscriptPrecompiledFunctionSignature::InitFrom(FAngelscriptPrecompiledData& Context, class asCScriptFunction* Function)
{
	Name = Function->GetName();
	Namespace = Function->GetNamespace();

	ReturnType.InitFrom(Context, Function->returnType);

	int32 ParamCount = Function->parameterTypes.GetLength();

	ParameterTypes.SetNum(ParamCount);
	ParameterFlags.SetNum(ParamCount);
	ParameterDefaultArgs.SetNum(ParamCount);

	for (int32 i = 0; i < ParamCount; ++i)
	{
		ParameterTypes[i].InitFrom(Context, Function->parameterTypes[i]);
		ParameterFlags[i] = (int32)Function->inOutFlags[i];

		if (Function->defaultArgs[i] != nullptr)
			ParameterDefaultArgs[i] = Function->defaultArgs[i]->AddressOf();
		else
			ParameterDefaultArgs[i].Empty();
	}
}

void FAngelscriptPrecompiledFunctionSignature::Create(FAngelscriptPrecompiledData& Context, FAppliedFunctionSignature& Sig) const
{
	FAngelscriptScopeTotalTimer Timer(TIMER_CreateFunctionSignature);

	Sig.funcName = *Name;
	Sig.ns = Context.GetNamespace(Namespace);

	ReturnType.Create(Context, Sig.returnType);

	int32 ParamCount = ParameterTypes.Num();
	Sig.params.SetLength(ParamCount);
	Sig.inOutFlags.SetLength(ParamCount);
	Sig.defaultArgs.SetLength(ParamCount);

	for (int32 i = 0; i < ParamCount; ++i)
	{
		ParameterTypes[i].Create(Context, Sig.params[i]);
		Sig.inOutFlags[i] = (asETypeModifiers)ParameterFlags[i];

		Sig.defaultArgs[i] = nullptr;
		if (ParameterDefaultArgs[i].Len() != 0)
			Sig.defaultArgs[i] = asNEW(asCString)(*ParameterDefaultArgs[i]);
	}
}

void FAngelscriptPrecompiledProperty::InitFrom(FAngelscriptPrecompiledData& Context, asCObjectProperty* Property)
{
	Name = Property->name.AddressOf();
	Type.InitFrom(Context, Property->type);
	bIsPrivate = Property->isPrivate;
	bIsProtected = Property->isProtected;
                                                                                                
	bUeDelayedDestroyOwner = Property->bUeDelayedDestroyOwner;
                                                                                              

	if (Context.PropertyDesc.IsValid())
	{
		auto PropertyDesc = Context.PropertyDesc;
		bIsUnrealProperty = true;

		for (auto Elem : PropertyDesc->Meta)
		{
			MetaSpec.Add(Elem.Key.ToString());
			MetaValues.Add(Elem.Value);
		}

		bBlueprintReadable = PropertyDesc->bBlueprintReadable;
		bBlueprintWritable = PropertyDesc->bBlueprintWritable;
		bEditConst = PropertyDesc->bEditConst;
		bEditableOnDefaults = PropertyDesc->bEditableOnDefaults;
		bEditableOnInstance = PropertyDesc->bEditableOnInstance;
		bInstancedReference = PropertyDesc->bInstancedReference;
		bPersistentInstance = PropertyDesc->bPersistentInstance;
		bAdvancedDisplay = PropertyDesc->bAdvancedDisplay;
		bTransient = PropertyDesc->bTransient;
		bReplicated = PropertyDesc->bReplicated;
		bSkipReplication = PropertyDesc->bSkipReplication;
		bSkipSerialization = PropertyDesc->bSkipSerialization;
		bSaveGame = PropertyDesc->bSaveGame;
		bRepNotify = PropertyDesc->bRepNotify;
		bConfig = PropertyDesc->bConfig;
		bInterp = PropertyDesc->bInterp;
		bAssetRegistrySearchable = PropertyDesc->bAssetRegistrySearchable;
		ReplicationCondition = (int32)PropertyDesc->ReplicationCondition;
	}
	else
	{
		bIsUnrealProperty = false;
	}
}

asCObjectProperty* FAngelscriptPrecompiledProperty::Create(FAngelscriptPrecompiledData& Context) const
{
	asCObjectProperty* Property = asNEW(asCObjectProperty)();
	Property->name = *Name;
	Type.Create(Context, Property->type);
	Property->isPrivate = bIsPrivate;
	Property->isProtected = bIsProtected;
                                                                                                
	Property->bUeDelayedDestroyOwner = bUeDelayedDestroyOwner;
                                                                                              

	return Property;
}

void FAngelscriptPrecompiledClass::InitFrom(FAngelscriptPrecompiledData& Context, asCModule* Module, asCObjectType* Type)
{
	ClassName = Type->GetName();
	Namespace = Type->GetNamespace();
	Flags = (int32)Type->flags;

	int32 PropCount = Type->properties.GetLength();
	Properties.Reset();

	int32 WritePropertyOffset = 0;
	if (Type->derivedFrom != nullptr)
		WritePropertyOffset = Type->derivedFrom->size;
	else if (Type->shadowType != nullptr && !(Type->flags & asOBJ_VALUE))
		WritePropertyOffset = Type->basePropertyOffset;

	for (int32 i = 0; i < PropCount; ++i)
	{
		asCObjectProperty* Property = Type->properties[i];
		if (Property->byteOffset < WritePropertyOffset)
			continue;

		if (Context.ClassDesc.IsValid())
			Context.PropertyDesc = Context.ClassDesc->GetProperty(ANSI_TO_TCHAR(Property->name.AddressOf()));
		else
			Context.PropertyDesc = nullptr;

		Properties.Emplace_GetRef().InitFrom(Context, Property);
	}

	bool bIsStruct = (Type->flags & asOBJ_VALUE) != 0;
	if (!bIsStruct)
	{
		// Read methods from the virtual function table
		int32 MethodCount = Type->virtualFunctionTable.GetLength();
		MethodTable.Reset(MethodCount);
		Methods.Reset();
		for (int32 i = 0; i < MethodCount; ++i)
		{
			asCScriptFunction* Function = Type->virtualFunctionTable[i];
			if (Function->objectType != Type)
			{
				// Indicate that this entry is from the base class
				MethodTable.Add(-1);
			}
			else
			{
				if (Context.ClassDesc.IsValid())
					Context.FunctionDesc = Context.ClassDesc->GetMethodByScriptName(ANSI_TO_TCHAR(Function->GetName()));
				else
					Context.FunctionDesc = nullptr;

				// Actually add the method data to the method list
				MethodTable.Add(Methods.Num());
				Methods.Emplace_GetRef().InitFrom(Context, Module, Function);
			}
		}
	}
	else
	{
		// Structs don't have virtual methods, so read methods from the normal methods list
		int32 MethodCount = Type->methods.GetLength();
		Methods.SetNum(MethodCount);

		for (int32 i = 0; i < MethodCount; ++i)
		{
			auto* Function = Context.Engine->GetScriptFunction(Type->methods[i]);

			if (Context.ClassDesc.IsValid())
				Context.FunctionDesc = Context.ClassDesc->GetMethodByScriptName(ANSI_TO_TCHAR(Function->GetName()));
			else
				Context.FunctionDesc = nullptr;

			Methods[i].InitFrom(Context, Module, Function);
		}
	}

	Context.FunctionDesc = nullptr;
	Context.PropertyDesc = nullptr;

	DerivedFrom = Context.ReferenceTypeInfo(Type->derivedFrom);
	ShadowType = Context.ReferenceTypeInfo(Type->shadowType);

	auto& beh = Type->beh;
	BehaviorRefs.Reset();
	BehaviorRefs.Add(Context.ReferenceFunctionId(beh.factory));
	BehaviorRefs.Add(Context.ReferenceFunctionId(beh.listFactory));
	BehaviorRefs.Add(Context.ReferenceFunctionId(beh.copyfactory));
	BehaviorRefs.Add(Context.ReferenceFunctionId(beh.construct));
	BehaviorRefs.Add(Context.ReferenceFunctionId(beh.copyconstruct));
	BehaviorRefs.Add(Context.ReferenceFunctionId(beh.destruct));
	BehaviorRefs.Add(Context.ReferenceFunctionId(beh.copy));

	int32 ConstructorCount = beh.constructors.GetLength();
	Constructors.SetNum(ConstructorCount);
	for (int32 i = 0; i < ConstructorCount; ++i)
	{
		auto* Function = Context.Engine->GetScriptFunction(beh.constructors[i]);
		Constructors[i].InitFrom(Context, Module, Function);
	}

	int32 FactoryCount = beh.factories.GetLength();
	FactoryRefs.Reset();
	for (int32 i = 0; i < FactoryCount; ++i)
		FactoryRefs.Add(Context.ReferenceFunctionId(beh.factories[i]));

	if (beh.destruct != 0)
	{
		auto* Function = Context.Engine->GetScriptFunction(beh.destruct);
		BehaviorFunctions.Emplace_GetRef().InitFrom(Context, Module, Function);
		BehaviorFunctionTypes.Add(asBEHAVE_DESTRUCT);
	}

	// Fill preprocessor data
	if (Context.ClassDesc.IsValid())
	{
		auto ClassDesc = Context.ClassDesc;
		bIsInPreprocessor = true;

		bSuperIsCodeClass = ClassDesc->bSuperIsCodeClass;
		if (ClassDesc->CodeSuperClass != nullptr)
			CodeSuperClass = ClassDesc->CodeSuperClass->GetPathName();
		else
			CodeSuperClass.Empty();

		bAbstract = ClassDesc->bAbstract;
		bTransient = ClassDesc->bTransient;
		bHideDropdown = ClassDesc->bHideDropdown;
		bDefaultToInstanced = ClassDesc->bDefaultToInstanced;
		bEditInlineNew = ClassDesc->bEditInlineNew;
		bIsDeprecatedClass = ClassDesc->bIsDeprecatedClass;
		SuperClass = ClassDesc->SuperClass;
		ConfigName = ClassDesc->ConfigName;
		StaticClassGlobalVariableName = ClassDesc->StaticClassGlobalVariableName;
		bPlaceableOverride = ClassDesc->bPlaceable.IsSet();
		bPlaceable = ClassDesc->bPlaceable.Get(true);

		for (auto Elem : ClassDesc->Meta)
		{
			MetaSpec.Add(Elem.Key.ToString());
			MetaValues.Add(Elem.Value);
		}

		ComposeOntoClassName = ClassDesc->ComposeOntoClass;
	}
	else
	{
		bIsInPreprocessor = false;
	}
}

static inline bool ShouldDestructProperty(asCObjectProperty* prop)
{
	if (prop->type.IsObject())
	{
		// Destroy the object
		asCObjectType *propType = (asCObjectType*)(prop->type.GetTypeInfo());
		if (prop->type.IsReference() || propType->flags & asOBJ_REF)
		{
		}
		else if (propType->flags & asOBJ_SCRIPT_OBJECT)
		{
			return true;
		}
		else
		{
			if (propType->beh.destruct)
				return true;
		}
	}
	else if (prop->type.IsFuncdef())
	{
		return true;
	}
	return false;
}

asCObjectType* FAngelscriptPrecompiledClass::Create(FAngelscriptPrecompiledData& Context, asCModule* Module) const
{
	FAngelscriptScopeTotalTimer Timer(TIMER_CreateClass);

	asCObjectType* Type = asNEW(asCObjectType)(Context.Engine);

	// Allocate a new typeid for this type in the engine
	Type->typeId = Context.Engine->typeIdSeqNbr++;
	Context.Engine->mapTypeIdToTypeInfo.Add(Type->typeId, Type);

	Type->name = *ClassName;
	Type->nameSpace = Context.GetNamespace(Namespace);

	Type->flags = (asDWORD)Flags;
	Type->size = -1;

	Type->module = Module;

	Module->classTypes.PushLast(Type);
	Module->allLocalTypes.Add(Type);
	Context.Engine->allScriptDeclaredTypes.Add(Type);

	return Type;
}

void FAngelscriptPrecompiledClass::ProcessProperties(FAngelscriptPrecompiledData& Context, asCObjectType* Type) const
{
	FAngelscriptScopeTotalTimer Timer(TIMER_ProcessProperties);

	auto* Module = (asCModule*)Type->GetModule();

	Type->derivedFrom = (asCObjectType*)Context.GetTypeInfo(DerivedFrom);
	Type->shadowType = Context.GetTypeInfo(ShadowType);

	if (Type->shadowType != nullptr && !(Type->flags & asOBJ_VALUE))
		Type->basePropertyOffset = ((asCObjectType*)Type->shadowType)->size;
	else
		Type->basePropertyOffset = 0;

	if (Type->derivedFrom != nullptr)
	{
		Context.ProcessProperties(Type->derivedFrom);
		Type->size = Type->derivedFrom->size;
		Type->alignment = Type->derivedFrom->alignment;
	}
	else if (Type->shadowType != nullptr)
	{
		Type->alignment = Type->shadowType->alignment;
		if (!(Type->flags & asOBJ_VALUE))
		{
			UClass* UnrealClass = (UClass*)Type->shadowType->GetUserData();
			check(UnrealClass != nullptr);
			Type->size = UnrealClass->GetStructureSize();
		}
		else
		{
			Type->size = 0;
		}
	}
	else
	{
		Type->size = 0;
		Type->alignment = 8;
	}

	int32 PropCount = Properties.Num();
	Type->properties.SetLength(0);
	Type->properties.AllocateNoConstruct(PropCount, 0);
	Type->localProperties.AllocateNoConstruct(PropCount, 0);
	Type->propertyTable.Reserve(PropCount);

	for (int32 i = 0; i < PropCount; ++i)
	{
		asCObjectProperty* Property = Properties[i].Create(Context);

		Type->localProperties.PushLast(Property);
		Type->properties.PushLast(Property);
		Type->propertyTable.Add(Property);

		if (auto* TypeInfo = Property->type.GetTypeInfo())
		{
			TypeInfo->AddRefInternal();
			if (Property->type.IsObject() && (Property->type.GetTypeInfo()->flags & asOBJ_VALUE) != 0)
				Context.ProcessProperties(TypeInfo);
		}

		Type->size = Align(Type->size, Property->type.GetAlignment());
		Type->alignment = FMath::Max(Type->alignment, Property->type.GetAlignment());
		Property->byteOffset = Type->size;

		if( Property->type.IsObject() )
		{
			if ((Property->type.GetTypeInfo()->flags & asOBJ_VALUE) != 0)
				Type->size += Property->type.GetSizeInMemoryBytes();
			else
				Type->size += Property->type.GetSizeOnStackDWords() * 4;
		}
		else if (Property->type.IsFuncdef())
		{
			Type->size += AS_PTR_SIZE*4;
		}
		else
		{
			Type->size += Property->type.GetSizeInMemoryBytes();
		}
	}

	// Align the size of the type with its base alignment
	Type->size = Align(Type->size, Type->alignment);
}

void FAngelscriptPrecompiledClass::CreateFunctions(FAngelscriptPrecompiledData& Context, asCObjectType* Type) const
{
	FAngelscriptScopeTotalTimer Timer(TIMER_CreateFunctions);

	auto* Module = (asCModule*)Type->GetModule();

	// Create all functions
	bool bIsStruct = (Type->flags & asOBJ_VALUE) != 0;
	if (!bIsStruct)
	{
		// Construct the virtual function table for this object type
		int32 MethodCount = MethodTable.Num();
		Type->virtualFunctionTable.SetLength(MethodCount);
		Type->methodTable.Reserve(MethodCount);

		for (int32 i = 0; i < MethodCount; ++i)
		{
			int32 Index = MethodTable[i];
			if (Index == -1)
			{
				Type->virtualFunctionTable[i] = nullptr;
			}
			else
			{
				auto* Function = Methods[Index].Create(Context, Module);

				Function->id = Context.Engine->GetNextScriptFunctionId();
				Module->AddScriptFunction(Function);

				Function->objectType = Type;
				Function->objectType->AddRefInternal();
				Function->vfTableIdx = i;

				Type->virtualFunctionTable[i] = Function;
				Function->AddRefInternal();

				// We need to make sure the function has a signature id before we do anything else
				Function->CalculateParameterOffsets();

				Type->methods.PushLast(Function->id);
				Function->AddRefInternal();

				Type->methodTable.Add(Function);
			}
		}
	}
	else
	{
		// Directly load struct functions into the methods list, since structs
		// don't have virtual function tables.
		int32 MethodCount = Methods.Num();
		Type->methods.SetLength(MethodCount);
		Type->methodTable.Reserve(MethodCount);

		for (int32 i = 0; i < MethodCount; ++i)
		{
			auto* Function = Methods[i].Create(Context, Module);

			Function->id = Context.Engine->GetNextScriptFunctionId();
			Module->AddScriptFunction(Function);
			Function->objectType = Type;
			Function->objectType->AddRefInternal();
			Function->CalculateParameterOffsets();

			Function->AddRefInternal();
			Type->methods[i] = Function->GetId();
			Type->methodTable.Add(Function);
		}
	}

	// Create constructors
	int32 ConstructorCount = Constructors.Num();
	for (int32 i = 0; i < ConstructorCount; ++i)
	{
		auto* Function = Constructors[i].Create(Context, Module);
		Function->objectType = Type;
		Function->objectType->AddRefInternal();

		Function->id = Context.Engine->GetNextScriptFunctionId();

		Function->CalculateParameterOffsets();
		Module->AddScriptFunction(Function);

		Function->AddRefInternal();
		Type->beh.constructors.PushLast(Function->GetId());
		Function->isInUse = true;
	}

	// Create factories
	int32 FactoryCount = FactoryRefs.Num();
	for (int32 i = 0; i < FactoryCount; ++i)
		Type->beh.factories.PushLast(Context.GetFunctionId(FactoryRefs[i], true, true));

	// Create other behavior functions
	int32 BehaviorCount = BehaviorFunctions.Num();
	for (int32 i = 0; i < BehaviorCount; ++i)
	{
		auto* Function = BehaviorFunctions[i].Create(Context, Module);
		Function->objectType = Type;
		Function->objectType->AddRefInternal();
		Function->id = Context.Engine->GetNextScriptFunctionId();

		Function->CalculateParameterOffsets();
		Module->AddScriptFunction(Function);
		Function->AddRefInternal();
		Function->isInUse = true;

		int32 BehType = BehaviorFunctionTypes[i];
		if (BehType == asBEHAVE_DESTRUCT)
		{
			Type->beh.destruct = Function->GetId();
		}
		else
		{
			check(false);
		}
	}
}

void FAngelscriptPrecompiledClass::PreProcessFunctions(FAngelscriptPrecompiledData& Context, asCObjectType* Type) const
{
	FAngelscriptScopeTotalTimer Timer(TIMER_PreProcessFunctions);

	auto* Module = (asCModule*)Type->GetModule();
	bFunctionsPreProcessed = true;

	int32 PotentialMethods = Type->methods.GetLength();
	int32 PotentialProperties = Type->properties.GetLength();
	if (Type->derivedFrom != nullptr)
	{
		int32 DerivedProperties = ((asCObjectType*)Type->derivedFrom)->properties.GetLength();;
		PotentialProperties += DerivedProperties;

		// Make sure functions are preprocessed on the derived type first
		                                                                                                  
		const FAngelscriptPrecompiledClassOwner* DerivedData = Context.ClassesLoadedFromPrecompiledData->Find(Type->derivedFrom);
		                                                                                                
		if (DerivedData != nullptr && DerivedData->Class != nullptr && !DerivedData->Class->bFunctionsPreProcessed)
			DerivedData->Class->PreProcessFunctions(*DerivedData->Context, Type->derivedFrom);
	}

	Type->properties.AllocateNoConstruct(PotentialProperties, true);
	Type->methods.AllocateNoConstruct(PotentialMethods, true);
	Type->methodTable.Reserve(PotentialMethods);
	Type->propertyTable.Reserve(PotentialProperties);

	// Init properties from baseclass
	{
		FAngelscriptScopeTotalTimer SubTimer(TIMER_AppendProperties);

		if (Type->derivedFrom != nullptr)
		{
			auto& properties = ((asCObjectType*)Type->derivedFrom)->properties;
			int32 ShadowOffset = Type->basePropertyOffset;
			for (int32 i = 0, Count = properties.GetLength(); i < Count; ++i)
			{
				asCObjectProperty* Property = properties[i];
				if (Property->byteOffset < ShadowOffset)
					continue;

				Type->properties.PushLast(Property);
				Type->propertyTable.Add(Property);
			}
		}
	}

	// Fill out the missing entries in the virtual function table with the parent class' function
	bool bIsStruct = (Type->flags & asOBJ_VALUE) != 0;
	if (!bIsStruct)
	{
		FAngelscriptScopeTotalTimer SubTimer(TIMER_VirtualTableLookup);

		int32 MethodCount = Type->virtualFunctionTable.GetLength();
		for (int32 i = 0; i < MethodCount; ++i)
		{
			auto* RealFunction = Type->virtualFunctionTable[i];
			if (RealFunction == nullptr)
			{
				// Look up the parent's real function in this slot to place in the vftable
				auto* CheckType = Type->derivedFrom;
				while (CheckType != nullptr)
				{
					if (CheckType->virtualFunctionTable[i] != nullptr)
					{
						RealFunction = CheckType->virtualFunctionTable[i];
						break;
					}
					CheckType = CheckType->derivedFrom;
				}

				Type->virtualFunctionTable[i] = RealFunction;
				RealFunction->AddRefInternal();
				Type->methods.PushLast(RealFunction->id);
				RealFunction->AddRefInternal();
				Type->methodTable.Add(RealFunction);
			}
		}
	}
}

void FAngelscriptPrecompiledClass::ProcessFunctions(FAngelscriptPrecompiledData& Context, asCObjectType* Type) const
{
	FAngelscriptScopeTotalTimer Timer(TIMER_ProcessFunctions);

	// Fill out behavior functions
	int32 BehIndex = 0;
	auto& beh = Type->beh;
	beh.factory = Context.GetFunctionId(BehaviorRefs[BehIndex++]);
	beh.listFactory = Context.GetFunctionId(BehaviorRefs[BehIndex++]);
	beh.copyfactory = Context.GetFunctionId(BehaviorRefs[BehIndex++]);
	beh.construct = Context.GetFunctionId(BehaviorRefs[BehIndex++]);
	beh.copyconstruct = Context.GetFunctionId(BehaviorRefs[BehIndex++]);
	beh.destruct = Context.GetFunctionId(BehaviorRefs[BehIndex++]);
	beh.copy = Context.GetFunctionId(BehaviorRefs[BehIndex++]);

	// Process functions
	bool bIsStruct = (Type->flags & asOBJ_VALUE) != 0;
	if (!bIsStruct)
	{
		int32 MethodCount = MethodTable.Num();
		for (int32 i = 0; i < MethodCount; ++i)
		{
			int32 Index = MethodTable[i];
			if (Index == -1)
				continue;

			auto* Function = Type->virtualFunctionTable[i];
			Methods[Index].Process(Context, Function);
		}
	}
	else
	{
		int32 MethodCount = Methods.Num();
		for (int32 i = 0; i < MethodCount; ++i)
		{
			auto* Function = Context.Engine->GetScriptFunction(Type->methods[i]);
			Methods[i].Process(Context, Function);
		}
	}

	// Process constructors
	int32 ConstructorCount = Constructors.Num();
	for (int32 i = 0; i < ConstructorCount; ++i)
	{
		auto* Function = Context.Engine->GetScriptFunction(beh.constructors[i]);
		Constructors[i].Process(Context, Function);
	}

	// Process other behaviors
	int32 BehaviorCount = BehaviorFunctions.Num();
	for (int32 i = 0; i < BehaviorCount; ++i)
	{
		int32 BehType = BehaviorFunctionTypes[i];
		if (BehType == asBEHAVE_DESTRUCT)
		{
			auto* Function = Context.Engine->GetScriptFunction(beh.destruct);
			BehaviorFunctions[i].Process(Context, Function);
		}
		else
		{
			check(false);
		}
	}

#if DO_CHECK
	check(IsAligned(Type->size, Type->alignment));
	if (Type->derivedFrom != nullptr)
		check(Type->size >= Type->derivedFrom->size);
	if (Type->shadowType != nullptr)
		check(Type->size >= (int)Type->shadowType->GetSize());

	int32 PropCount = Type->properties.GetLength();
	for (int32 i = 0; i < PropCount; ++i)
	{
		asCObjectProperty* Property = Type->properties[i];
		check(Property->byteOffset <= Type->size);
		check(IsAligned(Property->byteOffset, Property->type.GetAlignment()));

		int32 PropSize = 0;
		if( Property->type.IsObject() )
		{
			if( (Property->type.GetTypeInfo()->flags & asOBJ_VALUE) != 0 )
				PropSize = Property->type.GetSizeInMemoryBytes();
			else
				PropSize = Property->type.GetSizeOnStackDWords()*4;
		}
		else if (Property->type.IsFuncdef())
			PropSize = AS_PTR_SIZE*4;
		else
			PropSize = Property->type.GetSizeInMemoryBytes();

		check(Property->byteOffset + PropSize <= Type->size);
	}
#endif
}

void FAngelscriptPrecompiledEnum::InitFrom(FAngelscriptPrecompiledData& Context, asCModule* Module, asCEnumType* Type)
{
	Name = Type->GetName();
	Namespace = Type->GetNamespace();

	int32 ValueCount = Type->enumValues.GetLength();
	EnumNames.SetNum(ValueCount);
	EnumValues.SetNum(ValueCount);
	for (int32 i = 0; i < ValueCount; ++i)
	{
		EnumNames[i] = Type->enumValues[i]->name.AddressOf();
		EnumValues[i] = Type->enumValues[i]->value;
	}
}

asCEnumType* FAngelscriptPrecompiledEnum::Create(FAngelscriptPrecompiledData& Context, asCModule* Module) const
{
	asCEnumType* Type = asNEW(asCEnumType)(Context.Engine);
	Type->name = *Name;
	Type->flags = asOBJ_ENUM;
	Type->size = 1;
	Type->alignment = 1;

	Type->module = Module;
	Type->nameSpace = Context.GetNamespace(Namespace);

	Module->enumTypes.PushLast(Type);
	Module->allLocalTypes.Add(Type);
                                                                                                          
	Context.Engine->allScriptDeclaredTypes.Add(Type);
                                                                                                        

	int32 ValueCount = EnumNames.Num();
	Type->enumValues.SetLength(ValueCount);
	for (int32 i = 0; i < ValueCount; ++i)
	{
		Type->enumValues[i] = asNEW(asSEnumValue)();
		Type->enumValues[i]->name = *EnumNames[i];
		Type->enumValues[i]->value = EnumValues[i];
	}

	return Type;
}

void FAngelscriptPrecompiledGlobalVariable::InitFrom(FAngelscriptPrecompiledData& Context, asCModule* Module, asCGlobalProperty* Property)
{
	Name = Property->name.AddressOf();
	Namespace = Property->nameSpace->name.AddressOf();

	Type.InitFrom(Context, Property->type);

	if (Property->isPureConstant)
	{
		bIsPureConstant = true;
		PureConstantValue = (uint64)Property->storage;
	}
	else if (Property->isDefaultInit)
	{
		bIsDefaultInit = true;
	}
	else
	{
		if (Property->initFunc != nullptr)
		{
			bHasInitFunction = true;
			InitFunc.InitFrom(Context, Module, Property->initFunc);

			// Init functions should never be in the module itself, they are managed by the property
			check(Module->scriptFunctions.IndexOf(Property->initFunc) == -1);
		}
		else
		{
			bHasInitFunction = false;
		}
	}
}

asCGlobalProperty* FAngelscriptPrecompiledGlobalVariable::Create(FAngelscriptPrecompiledData& Context, asCModule* Module) const
{
	asCDataType DataType;
	Type.Create(Context, DataType);
	Context.ProcessProperties(DataType.GetTypeInfo());

	asCGlobalProperty* Property = Module->AllocateGlobalProperty(
		*Name,
		DataType,
		Context.GetNamespace(Namespace)
	);

	if (bIsPureConstant)
	{
		Property->isPureConstant = true;
		Property->storage = (asQWORD)PureConstantValue;
	}
	else if (bIsDefaultInit)
	{
		Property->isDefaultInit = true;
	}
	else if (bHasInitFunction)
	{
		asCScriptFunction* Function = InitFunc.Create(Context, Module);

		Function->id = Context.Engine->GetNextScriptFunctionId();
		Function->CalculateParameterOffsets();

		Context.Engine->AddScriptFunction(Function);
                                                                                                            
		// The global property retains the init function until normal engine teardown.
                                                                                                          

		Property->SetInitFunc(Function);
	}

	return Property;
}

void FAngelscriptPrecompiledGlobalVariable::Process(FAngelscriptPrecompiledData& Context, asCModule* Module, asCGlobalProperty* Property) const
{
	if (Property->initFunc != nullptr)
	{
		check(bHasInitFunction);
		InitFunc.Process(Context, Property->initFunc);
	}
}

void FAngelscriptPrecompiledModule::InitFrom(FAngelscriptPrecompiledData& Context, asCModule* Module)
{
	ModuleName = Module->GetName();
	for (const auto& DependencyPair : Module->moduleDependencies)
	{
		if (DependencyPair.Key != nullptr && DependencyPair.Key != Module)
			Dependencies.AddUnique(ANSI_TO_TCHAR(DependencyPair.Key->GetName()));
	}
	for (FString& InitFunction : Context.ModuleDesc->PostInitFunctions)
		PostInitFunctions.Add(InitFunction);

	TSharedPtr<FAngelscriptClassDesc> StaticsClass;
	for (auto ClassDescInModule : Context.ModuleDesc->Classes)
	{
		if (ClassDescInModule->bIsStaticsClass)
		{
			StaticsClass = ClassDescInModule;
			break;
		}
	}

	if (StaticsClass.IsValid())
		StaticsClassName = StaticsClass->ClassName;
	else
		StaticsClassName.Empty();

	for (int i = 0, Count = Module->scriptFunctions.GetLength(); i < Count; ++i)
	{
		asCScriptFunction* Function = Module->scriptFunctions[i];

		// Functions inside classes are handled by the class
		if (Function->objectType != nullptr)
			continue;

		if (StaticsClass.IsValid())
			Context.FunctionDesc = StaticsClass->GetMethodByScriptName(ANSI_TO_TCHAR(Function->GetName()));
		else
			Context.FunctionDesc = nullptr;

		Functions.Emplace_GetRef().InitFrom(Context, Module, Function);
	}

	Context.FunctionDesc = nullptr;

	for (int i = 0, Count = Module->classTypes.GetLength(); i < Count; ++i)
	{
		Context.ClassDesc = Context.ModuleDesc->GetClass(Module->classTypes[i]);
		Classes.Emplace_GetRef().InitFrom(Context, Module, Module->classTypes[i]);
	}

	for (int i = 0, Count = Module->enumTypes.GetLength(); i < Count; ++i)
		Enums.Emplace_GetRef().InitFrom(Context, Module, Module->enumTypes[i]);

	Module->scriptGlobals.IterateAll([&](asCGlobalProperty* Prop)
	{
		GlobalVariables.Emplace_GetRef().InitFrom(Context, Module, Prop);
	});

	for (auto DelegateDesc : Context.ModuleDesc->Delegates)
	{
		if (DelegateDesc->bIsMulticast)
			DeclaredEvents.Add(DelegateDesc->DelegateName);
		else
			DeclaredDelegates.Add(DelegateDesc->DelegateName);
	}

	if (Context.ModuleDesc->Code.Num() != 0)
		ScriptRelativeFilename = Context.ModuleDesc->Code[0].RelativeFilename;
                                                                                                    
	ScriptUnitId = Context.ModuleDesc->ScriptUnit.Id;
	ScriptLayer = uint8(Context.ModuleDesc->ScriptUnit.Layer);
	for (const FString& Dependency : Context.ModuleDesc->ScriptUnit.Dependencies)
		ScriptUnitDependencies.Add(Dependency);
                                                                                                  
                                                                                                     
	for (const auto& Dependency : Module->moduleDependencies)
	{
		auto& Reference = ModuleReferences.AddDefaulted_GetRef();
		Reference.ModuleName = Dependency.Key->baseModuleName.AddressOf();
		Reference.Row = Dependency.Value.FirstLineNumber;
		Reference.Column = Dependency.Value.FirstColumn;
		Reference.Flags = (Dependency.Value.bIsStructuralDependency ? 1 : 0)
			| (Dependency.Value.bIsHardValueDependency ? 2 : 0);
	}
                                                                                                   
}

int32 FAngelscriptPrecompiledData::GetScriptSection()
{
	if (ScriptSectionIdx != -1)
		return ScriptSectionIdx;
	check(ScriptRelativeFilename != nullptr);

	FString ScriptFile = FAngelscriptManager::GetScriptRootDirectory() / *ScriptRelativeFilename->UnrealString();
	ScriptSectionIdx = Engine->GetScriptSectionNameIndex(TCHAR_TO_ANSI(*ScriptFile));
	return ScriptSectionIdx;
}

void FAngelscriptPrecompiledData::ProcessProperties(asITypeInfo* TypeInfo)
{
	if (TypeInfo == nullptr)
		return;
	                                                                                                  
	FAngelscriptPrecompiledData* PropertyContext = this;
	if (ClassesLoadedFromPrecompiledData.IsValid())
	{
		if (const FAngelscriptPrecompiledClassOwner* Owner = ClassesLoadedFromPrecompiledData->Find(TypeInfo))
		{
			if (Owner->Context != nullptr)
				PropertyContext = Owner->Context;
		}
	}
	                                                                                                

	int32 Flags = ((asCTypeInfo*)TypeInfo)->flags;

	// Some template instances resize based on their subtype
	if ((Flags & asOBJ_TEMPLATE_SUBTYPE_DETERMINES_SIZE) != 0)
	{
		auto* ObjectType = (asCObjectType*)TypeInfo;
		if (auto* SubType = ObjectType->templateSubTypes[0].GetTypeInfo())
		{
			if ((SubType->flags & asOBJ_REF) == 0)
				PropertyContext->ProcessProperties(SubType);
		}
		ObjectType->CalculateTemplateSize();
		return;
	}

	// C++ Objects don't need to process properties
	if ((Flags & asOBJ_SCRIPT_OBJECT) == 0)
		return;

	// If size is not -1 we've already processed it
	auto* ObjectType = (asCObjectType*)TypeInfo;
	if (ObjectType->size != -1)
		return;

	                                                                                                  
	const FAngelscriptPrecompiledClassOwner& PrecompiledOwner = ClassesLoadedFromPrecompiledData->FindChecked(TypeInfo);
	                                                                                                
	PrecompiledOwner.Class->ProcessProperties(*PrecompiledOwner.Context, ObjectType);

	// If it's not in our current module's list of classes to process, that
	// means our dependency ordering has already processed it for us.
}

                                                                                                  
void FAngelscriptPrecompiledData::AddModuleDependencies(asCModule* Module, const TArray<FString>& Dependencies) const
{
	if (Module == nullptr)
		return;

	for (const FString& DependencyName : Dependencies)
	{
		asCModule* ExactModule = nullptr;
		asCModule* CompilingModule = nullptr;
		for (asUINT Index = 0; Index < Engine->scriptModules.GetLength(); ++Index)
		{
			asCModule* Candidate = Engine->scriptModules[Index];
			if (Candidate == nullptr)
				continue;
			const FString CandidateName = ANSI_TO_TCHAR(Candidate->GetName());
			FString CandidateBaseName = CandidateName;
			const char* BaseModuleName = Candidate->baseModuleName.AddressOf();
			if (BaseModuleName != nullptr && BaseModuleName[0] != '\0')
				CandidateBaseName = ANSI_TO_TCHAR(BaseModuleName);
			if (CandidateName == DependencyName)
				ExactModule = Candidate;
			else if (CandidateBaseName == DependencyName)
				CompilingModule = Candidate;
		}
		asCModule* Dependency = CompilingModule != nullptr ? CompilingModule : ExactModule;
		if (Dependency == nullptr || Dependency == Module || Module->moduleDependencies.Contains(Dependency))
			continue;
		asCModule::FModuleDependencyInfo DependencyInfo;
		DependencyInfo.FirstLineNumber = 0;
		DependencyInfo.FirstColumn = 0;
		Module->moduleDependencies.Add(Dependency, DependencyInfo);
	}
}
                                                                                                

                                                                                                  
bool FAngelscriptPrecompiledData::AppendStaticNames(FString& OutError) const
{
	TArray<FName> RequestedNames;
	TSet<FName> SeenNames;
	RequestedNames.Reserve(StaticNames.Num());
	for (const FStringInArchive& SerializedName : StaticNames)
	{
		const FName Name(*SerializedName.UnrealString());
		if (SeenNames.Contains(Name))
		{
			OutError = TEXT("module cache static-name table contains a repeated name");
			return false;
		}
		SeenNames.Add(Name);
		RequestedNames.Add(Name);
	}

	if (FAngelscriptManager::StaticNames.Num() > RequestedNames.Num())
	{
		OutError = TEXT("module cache static-name table is shorter than the active table");
		return false;
	}
	for (int32 Index = 0; Index < FAngelscriptManager::StaticNames.Num(); ++Index)
	{
		if (FAngelscriptManager::StaticNames[Index] != RequestedNames[Index])
		{
			OutError = TEXT("module cache static-name table does not share the active prefix");
			return false;
		}
	}
	for (const TPair<FName, int32>& Pair : FAngelscriptManager::StaticNamesByIndex)
	{
		if (Pair.Value < 0 || Pair.Value >= RequestedNames.Num() || RequestedNames[Pair.Value] != Pair.Key)
		{
			OutError = TEXT("module cache static-name table conflicts with source indices");
			return false;
		}
	}

	for (int32 Index = 0; Index < RequestedNames.Num(); ++Index)
	{
		if (Index >= FAngelscriptManager::StaticNames.Num())
			FAngelscriptManager::StaticNames.Add(RequestedNames[Index]);
		FAngelscriptManager::StaticNamesByIndex.Add(RequestedNames[Index], Index);
	}
	return true;
}
                                                                                                

void FAngelscriptPrecompiledModule::ApplyToModule_Stage1(FAngelscriptPrecompiledData& Context, asIScriptModule* InModule) const
{
	asCModule* Module = (asCModule*)InModule;

	Context.Engine->deferValidationOfTemplateTypes = true;
	Context.Engine->deferCalculatingTemplateSize = true;
	Context.ScriptRelativeFilename = &ScriptRelativeFilename;
	Context.ScriptSectionIdx = -1;

	// Create classes for this module
	for (int32 i = 0, Count = Classes.Num(); i < Count; ++i)
	{
		asCObjectType* Type = Classes[i].Create(Context, Module);
		ClassTypes.Add(Type);
		                                                                                                  
		FAngelscriptPrecompiledClassOwner Owner;
		Owner.Context = &Context;
		Owner.Class = &Classes[i];
		Context.ClassesLoadedFromPrecompiledData->Add(Type, Owner);
		                                                                                                
	}

	// Delegates and events should be tagged before we use them
	for (const FStringInArchive& Event : DeclaredEvents)
	{
		auto* ObjectType = CastToObjectType(Module->allLocalTypes.FindFirst(*Event));
		if (ObjectType != nullptr)
			ObjectType->plainUserData = (asPWORD)FAngelscriptType::TAG_UserData_Multicast_Delegate;
	}

	for (const FStringInArchive& Event : DeclaredDelegates)
	{
		auto* ObjectType = CastToObjectType(Module->allLocalTypes.FindFirst(*Event));
		if (ObjectType != nullptr)
			ObjectType->plainUserData = (asPWORD)FAngelscriptType::TAG_UserData_Delegate;
	}

	// Create enum types
	for (const FAngelscriptPrecompiledEnum& Enum : Enums)
		Enum.Create(Context, Module);
}

void FAngelscriptPrecompiledModule::ApplyToModule_Stage2(FAngelscriptPrecompiledData& Context, asIScriptModule* InModule) const
{
	asCModule* Module = (asCModule*)InModule;
	Context.ScriptRelativeFilename = &ScriptRelativeFilename;
	Context.ScriptSectionIdx = -1;

	// Process properties for all classes we created
	for (int32 i = 0, Count = Classes.Num(); i < Count; ++i)
		Classes[i].ProcessProperties(Context, ClassTypes[i]);

	// Allocate global variables
	GlobalProperties.SetNum(GlobalVariables.Num());
                                                                                                            
	for (int32 i = 0, Count = GlobalVariables.Num(); i < Count; ++i)
	{
		GlobalProperties[i] = GlobalVariables[i].Create(Context, Module);
		Context.Engine->allScriptGlobalVariables.Add(GlobalProperties[i]);
	}
                                                                                                          

	// Create global functions
	GlobalFunctions.SetNum(Functions.Num());
	for (int32 i = 0, Count = Functions.Num(); i < Count; ++i)
	{
		asCScriptFunction* Function = Functions[i].Create(Context, Module);
		Function->id = Context.Engine->GetNextScriptFunctionId();
		Function->CalculateParameterOffsets();
		Module->AddScriptFunction(Function);

		GlobalFunctions[i] = Function;
		Module->globalFunctions.Add(Function);
		Module->globalFunctionList.PushLast(Function);
                                                                                                              
		Context.Engine->allScriptGlobalFunctions.Add(Function);
                                                                                                            
	}

	// Create function objects for each class
	for (int32 i = 0, Count = Classes.Num(); i < Count; ++i)
		Classes[i].CreateFunctions(Context, ClassTypes[i]);
                                                                                                     
	for (const auto& Reference : ModuleReferences)
	{
		asCModule* Dependency = Context.Engine->GetModule(*Reference.ModuleName, false);
		check(Dependency != nullptr);
		asCModule::FModuleDependencyInfo Info;
		Info.FirstLineNumber = Reference.Row;
		Info.FirstColumn = Reference.Column;
		Info.bIsStructuralDependency = (Reference.Flags & 1) != 0;
		Info.bIsHardValueDependency = (Reference.Flags & 2) != 0;
		Module->moduleDependencies.Add(Dependency, Info);
	}
                                                                                                   
}

void FAngelscriptPrecompiledModule::ApplyToModule_Stage3(FAngelscriptPrecompiledData& Context, asIScriptModule* InModule) const
{
	asCModule* Module = (asCModule*)InModule;
	Context.ScriptRelativeFilename = &ScriptRelativeFilename;
	Context.ScriptSectionIdx = -1;

	// Do pre-processing on all classes
	for (int32 i = 0, Count = Classes.Num(); i < Count; ++i)
		Classes[i].PreProcessFunctions(Context, ClassTypes[i]);

	// Process data for global functions
	for (int32 i = 0, Count = Functions.Num(); i < Count; ++i)
		Functions[i].Process(Context, GlobalFunctions[i]);

	// Do post-processing on all classes
	for (int32 i = 0, Count = Classes.Num(); i < Count; ++i)
		Classes[i].ProcessFunctions(Context, ClassTypes[i]);

	// Do post-processing on global properties
	for (int32 i = 0, Count = GlobalVariables.Num(); i < Count; ++i)
		GlobalVariables[i].Process(Context, Module, GlobalProperties[i]);

	// Trigger steps normally performed by a call to Build()
	Module->JITCompile();
	                                                                                                  
	Context.AddModuleDependencies(Module, Dependencies);
	                                                                                                
}

asSNameSpace* FAngelscriptPrecompiledData::GetNamespace(const FStringInArchive& Namespace)
{
	FAngelscriptScopeTotalTimer Timer(TIMER_GetNamespace);
	if (Namespace.Len() != 0)
		return Engine->AddNameSpace(*Namespace);
	else
		return Engine->defaultNamespace;
}

FAngelscriptPrecompiledReference FAngelscriptPrecompiledData::ReferenceTypeInfo(asITypeInfo* TypeInfo)
{
	if (TypeInfo == nullptr)
		return FAngelscriptPrecompiledReference{ 0 };

	int64 Pointer = (int64)(SIZE_T)TypeInfo;
	if (!TypeReferences.Contains(Pointer))
	{
		FAngelscriptTypeReference Ref;
		Ref.Name = TypeInfo->GetName();

		if ((TypeInfo->GetFlags() & asOBJ_TEMPLATE_SUBTYPE) == 0)
		{
			// Regular types are stored by name and subtypes
			Ref.Namespace = TypeInfo->GetNamespace();

			if (asCModule* Module = (asCModule*)TypeInfo->GetModule())
				Ref.Module = Module->GetName();

			int32 SubTypeCount = TypeInfo->GetSubTypeCount();
			if (SubTypeCount > 0)
			{
				auto* ObjType = (asCObjectType*)TypeInfo;
				if (ObjType->templateBaseType != nullptr)
				{
					Ref.SubTypes.SetNum(SubTypeCount);
					for (int32 i = 0; i < SubTypeCount; ++i)
						Ref.SubTypes[i].InitFrom(*this, ObjType->templateSubTypes[i]);
				}
			}
		}
		else
		{
			// Template generic subtypes are stored specially
			Ref.Module = TEXT("$__T__");

			// Find the template type this is part of
			bool bFoundSubType = false;
			for (int32 i = 0, Count = Engine->registeredTemplateTypes.GetLength(); i < Count; ++i)
			{
				auto* TemplType = Engine->registeredTemplateTypes[i];

				for (int32 j = 0, SubCount = TemplType->templateSubTypes.GetLength(); j < SubCount; ++j)
				{
					auto* SubType = TemplType->templateSubTypes[j].GetTypeInfo();
					if (SubType == TypeInfo)
					{
						Ref.Namespace = TemplType->GetName();
						bFoundSubType = true;
						break;
					}
				}

				if (bFoundSubType)
					break;
			}
		}


		TypeReferences.Add(Pointer, Ref);
		TypeIdReferenceToPointer.Add(TypeInfo->GetTypeId(), Pointer);
	}

	return FAngelscriptPrecompiledReference{ (int64)(SIZE_T)TypeInfo };
}

static asCModule* FindModuleForPrecompiledReference(asCScriptEngine* Engine, const FStringInArchive& ReferenceModule)
{
	const FString ReferencedModuleName = ReferenceModule.UnrealString();
	asCModule* ExactModule = (asCModule*)Engine->GetModule(*ReferenceModule, false);
	asCModule* CompilingModule = nullptr;
	for (asUINT Index = 0; Index < Engine->scriptModules.GetLength(); ++Index)
	{
		asCModule* Candidate = Engine->scriptModules[Index];
		if (Candidate == nullptr)
			continue;

		const FString CandidateName = ANSI_TO_TCHAR(Candidate->GetName());
		if (CandidateName == ReferencedModuleName)
			continue;

		FString CandidateBaseName = CandidateName;
		const ANSICHAR* BaseModuleName = Candidate->baseModuleName.AddressOf();
		if (BaseModuleName != nullptr && BaseModuleName[0] != '\0')
			CandidateBaseName = ANSI_TO_TCHAR(BaseModuleName);

		if (CandidateBaseName == ReferencedModuleName)
		{
			CompilingModule = Candidate;
		}
	}
	return CompilingModule != nullptr ? CompilingModule : ExactModule;
}

asCTypeInfo* FAngelscriptPrecompiledData::GetTypeInfo(const FAngelscriptPrecompiledReference& Reference, bool bAddRef)
{
	if (Reference.OldReference == 0)
		return nullptr;

	// Check if it's in the cache
	void** TypeInfoPtr = CachedPointerReferences.Find(Reference.OldReference);
	if (TypeInfoPtr != nullptr)
	{
		if (bAddRef)
			((asCTypeInfo*)*TypeInfoPtr)->AddRefInternal();
		return (asCTypeInfo*)*TypeInfoPtr;
	}

	FAngelscriptScopeTotalTimer Timer(TIMER_TypeInfoLookup);

	// Do the reference lookup
	FAngelscriptTypeReference* RefPtr;
	
	{
		FAngelscriptScopeTotalTimer SubTimer(TIMER_TypeReferenceFind);
		RefPtr = TypeReferences.Find(Reference.OldReference);
		if (RefPtr == nullptr)
		{
			check(!FunctionReferences.Contains(Reference.OldReference));
			check(!GlobalReferences.Contains(Reference.OldReference));
		}
		checkf(RefPtr != nullptr, TEXT("Loaded an angelscript type reference that wasn't saved properly!"));
	}

	FAngelscriptTypeReference& Ref = *RefPtr;

	asCTypeInfo* FoundType = nullptr;
	asSNameSpace* ns = GetNamespace(Ref.Namespace);

	if (Ref.Namespace.Len() != 0 && Ref.Module == "$__T__")
	{
		FAngelscriptScopeTotalTimer SubTimer(TIMER_FindTemplateSubType);

		// This is a special case that refers to a genericized template subtype
		auto* BaseType = (asCObjectType*)Engine->GetTypeInfo(*Ref.Namespace, Engine->defaultNamespace);
		for (int32 i = 0, Count = BaseType->templateSubTypes.GetLength(); i < Count; ++i)
		{
			auto* SubType = BaseType->templateSubTypes[i].GetTypeInfo();
			if (SubType->name == *Ref.Name)
			{
				FoundType = SubType;
				break;
			}
		}
	}
	else if (Ref.Module.Len() != 0 && Ref.SubTypes.Num() == 0)
	{
		// If a module is specified this is a script type
		asCModule* Module = nullptr;
		{
			FAngelscriptScopeTotalTimer SubTimer(TIMER_FindModuleForType);
			Module = FindModuleForPrecompiledReference(Engine, Ref.Module);
		}
		if (Module != nullptr)
		{
			FAngelscriptScopeTotalTimer SubTimer(TIMER_FindTypeInModule);
			FoundType = Module->GetType(*Ref.Name, ns);
		}

		checkf(Module != nullptr,
			TEXT("Module '%s' for angelscript reference to type '%s' could not be found"),
			ANSI_TO_TCHAR(*Ref.Module), ANSI_TO_TCHAR(*Ref.Name));
		checkf(FoundType != nullptr,
			TEXT("Angelscript reference to type '%s' in module '%s' could not be found"),
			ANSI_TO_TCHAR(*Ref.Name), ANSI_TO_TCHAR(*Ref.Module));
	}
	else
	{
		// No module, so this is an engine type
		{
			FAngelscriptScopeTotalTimer SubTimer(TIMER_FindTypeInEngine);
			FoundType = (asCTypeInfo*)Engine->GetTypeInfo(*Ref.Name, ns);
		}

		// The two default behavior types have hardcoded special names
		if (FoundType == nullptr)
		{
			FAngelscriptScopeTotalTimer SubTimer(TIMER_FindHardcodedType);
			if (Ref.Name == "$obj")
				FoundType = &Engine->scriptTypeBehaviours;
			else if (Ref.Name == "$func")
				FoundType = &Engine->functionBehaviours;
		}
	}

	if (FoundType == nullptr)
	{
		checkf(FoundType != nullptr,
			TEXT("Angelscript reference to type '%s' in module '%s' could not be found"),
			ANSI_TO_TCHAR(*Ref.Name), ANSI_TO_TCHAR(*Ref.Module));
		return nullptr;
	}

	// If it's a template type we need to make sure it's instantiated
	if (Ref.SubTypes.Num() != 0)
	{
		FAngelscriptScopeTotalTimer SubTimer(TIMER_FindTemplateInstance);

		asCArray<asCDataType> subTypes;
		subTypes.SetLength(Ref.SubTypes.Num());
		for (int32 i = 0, Count = Ref.SubTypes.Num(); i < Count; ++i)
			Ref.SubTypes[i].Create(*this, subTypes[i]);

		asCObjectType* TemplInst = Engine->GetTemplateInstanceType((asCObjectType*)FoundType, subTypes, nullptr);

		// Keep and leak an external reference to the template type.
		// We're never going to delete this anyway in this case, and assigning
		// it to the appropriate module would be a pain.
		TemplInst->AddRef();
		FoundType = TemplInst;
	}

	if (bAddRef)
		FoundType->AddRefInternal();

	{
		FAngelscriptScopeTotalTimer SubTimer(TIMER_UpdateReferenceMap);
		CachedPointerReferences.Add(Reference.OldReference, FoundType);
	}

	// Make sure we can access typeid on this pointer
	if (FoundType != nullptr)
		FoundType->GetTypeId();

	return FoundType;
}

FAngelscriptPrecompiledReference FAngelscriptPrecompiledData::ReferenceTypeId(int TypeId)
{
	if (TypeId == 0)
		return FAngelscriptPrecompiledReference{ 0 };
	if (TypeId <= asTYPEID_LAST_PRIMITIVE)
		return FAngelscriptPrecompiledReference{ TypeId };

	asITypeInfo* TypeInfo = Engine->GetTypeInfoById(TypeId);
	ReferenceTypeInfo(TypeInfo);

	check((TypeId & (asTYPEID_MASK_SEQNBR | asTYPEID_MASK_OBJECT)) == TypeInfo->GetTypeId());
	return FAngelscriptPrecompiledReference{ (int64)TypeId };
}

int FAngelscriptPrecompiledData::GetTypeId(const FAngelscriptPrecompiledReference& Reference, bool bAddRef)
{
	if (Reference.OldReference == 0)
		return 0;
	if (Reference.OldReference <= asTYPEID_LAST_PRIMITIVE)
		return (int)Reference.OldReference;

	asDWORD Flags = ((asDWORD)Reference.OldReference) & ~(asTYPEID_MASK_SEQNBR | asTYPEID_MASK_OBJECT);
	int32 TypeInfoId = ((int32)Reference.OldReference) & (asTYPEID_MASK_SEQNBR | asTYPEID_MASK_OBJECT);

	int64* FoundPointer = TypeIdReferenceToPointer.Find(TypeInfoId);
	checkf(FoundPointer != nullptr, TEXT("Loaded an angelscript type id that wasn't saved properly!"));

	asCTypeInfo* TypeInfo = GetTypeInfo(FAngelscriptPrecompiledReference{ *FoundPointer }, bAddRef);
	check(TypeInfo->GetTypeId() != 0);
	return TypeInfo->GetTypeId() | Flags;
}

FAngelscriptPrecompiledReference FAngelscriptPrecompiledData::ReferenceFunction(asIScriptFunction* InFunction)
{
	if (InFunction == nullptr)
		return FAngelscriptPrecompiledReference{ 0 };

	int64 Pointer = (int64)(SIZE_T)InFunction;
	if (!FunctionReferences.Contains(Pointer))
	{
		asCScriptFunction* Function = (asCScriptFunction*)(InFunction);

		FAngelscriptFunctionReference Ref;

		Ref.Name = Function->name.AddressOf();
		Ref.Namespace = Function->GetNamespace();

		if (asCModule* Module = Function->module)
			Ref.Module = Module->GetName();

		int32 ParamCount = Function->parameterTypes.GetLength();
		Ref.ParameterTypes.SetNum(ParamCount);
		for (int32 i = 0; i < ParamCount; ++i)
			Ref.ParameterTypes[i].InitFrom(*this, Function->parameterTypes[i]);

		if (asCObjectType* objectType = Function->objectType)
		{
			Ref.bIsConst = Function->IsReadOnly();
			Ref.bIsMethod = true;
			Ref.ObjectType = ReferenceTypeInfo(objectType);
		}
		else if (Ref.Name == "$beh3" || Ref.Name == "$fact")
		{
			// Factories need to be read from the object type's factory list,
			// but the object type is only located in the return value
			Ref.ObjectType = ReferenceTypeInfo(Function->returnType.GetTypeInfo());
			Ref.bIsMethod = true;
			Ref.bIsConst = false;

			check(((asCObjectType*)Function->returnType.GetTypeInfo())->beh.factories.IndexOf(Function->GetId()) != -1);
		}

		Ref.ReturnType.InitFrom(*this, Function->returnType);

		FunctionReferences.Add(Pointer, Ref);
		FunctionIdReferenceToPointer.Add(Function->GetId(), Pointer);
	}

	return FAngelscriptPrecompiledReference{ Pointer };
}

bool FAngelscriptFunctionReference::MatchesSignature(FAngelscriptPrecompiledData& Context, asCScriptFunction* Function) const
{
	if (Function->IsReadOnly() != bIsConst)
		return false;

	int32 ParamCount = ParameterTypes.Num();
	if (Function->parameterTypes.GetLength() != ParamCount)
		return false;

	asCDataType ReturnDataType;
	ReturnType.Create(Context, ReturnDataType);

	if (Function->returnType != ReturnDataType)
		return false;

	for (int32 i = 0; i < ParamCount; ++i)
	{
		asCDataType ParamDataType;
		ParameterTypes[i].Create(Context, ParamDataType);

		if (ParamDataType != Function->parameterTypes[i])
			return false;
	}

	return true;
}

asCScriptFunction* FAngelscriptPrecompiledData::GetFunction(const FAngelscriptPrecompiledReference& Reference, bool bAddRef, bool bMarkInUse)
{
	if (Reference.OldReference == 0)
		return nullptr;

	// Check if it's in the cache
	void** FunctionPtr = CachedPointerReferences.Find(Reference.OldReference);
	if (FunctionPtr != nullptr)
	{
		if (bAddRef)
			((asCScriptFunction*)*FunctionPtr)->AddRefInternal();
		if (bMarkInUse)
			((asCScriptFunction*)*FunctionPtr)->isInUse = true;
		return (asCScriptFunction*)*FunctionPtr;
	}

	FAngelscriptScopeTotalTimer Timer(TIMER_FunctionLookup);

	// Do the reference lookup
	FAngelscriptFunctionReference* RefPtr = FunctionReferences.Find(Reference.OldReference);
	checkf(RefPtr != nullptr, TEXT("Loaded an angelscript type reference that wasn't saved properly!"));

	FAngelscriptFunctionReference& Ref = *RefPtr;
	asCScriptFunction* FoundFunction = nullptr;

	if (Ref.bIsMethod)
	{
		// Look up the function from the object type
		asCObjectType* Type = (asCObjectType*)GetTypeInfo(Ref.ObjectType, false);

		// Type must be preprocessed before we can load anything on it
		if (Type->flags & asOBJ_SCRIPT_OBJECT)
		{
			// Make sure functions are preprocessed on the derived type first
			                                                                                                  
			const FAngelscriptPrecompiledClassOwner* DerivedData = ClassesLoadedFromPrecompiledData->Find(Type->derivedFrom);
			                                                                                                
			if (DerivedData != nullptr && DerivedData->Class != nullptr && !DerivedData->Class->bFunctionsPreProcessed)
				DerivedData->Class->PreProcessFunctions(*DerivedData->Context, Type->derivedFrom);
		}

		// We might need to instantiate the template method
		if ((Type->flags & asOBJ_TEMPLATE) != 0)
		{
			TArray<asCScriptFunction*, TInlineAllocator<8>> TemplateMethods;
			Type->FindMethodUntil(*Ref.Name, [&](asCScriptFunction* CheckFunc)
			{
				if (!CheckFunc->traits.GetTrait(asTRAIT_GENERIC_TEMPLATE_FUNCTION))
				{
					if (Ref.MatchesSignature(*this, CheckFunc))
					{
						FoundFunction = CheckFunc;
						TemplateMethods.Reset();
						return true;
					}
					else
					{
						return false;
					}
				}
				else
				{
					TemplateMethods.Add(CheckFunc);
					return false;
				}
			});

			for (auto* TemplateFunction : TemplateMethods)
			{
				auto* FunctionInstance = Engine->GenerateTemplateFunction(Type, TemplateFunction);
				if (Ref.MatchesSignature(*this, FunctionInstance))
				{
					FoundFunction = FunctionInstance;
					break;
				}
			}
		}
		else
		{
			Type->FindMethodUntil(*Ref.Name, [&](asCScriptFunction* CheckFunc)
			{
				if (Ref.MatchesSignature(*this, CheckFunc))
				{
					FoundFunction = CheckFunc;
					return true;
				}
				return false;
			});
		}

		if (FoundFunction == nullptr)
		{
			// See if we can find the function in one of the object's behaviors instead
			// of in its method table.
			auto& beh = Type->beh;
			auto CheckBeh = [&](int BehId)
			{
				if (FoundFunction != nullptr)
					return;
				if (BehId == 0)
					return;

				auto* BehFunction = Engine->GetScriptFunction(BehId);
				if (BehFunction->name == *Ref.Name)
				{
					if (BehFunction->traits.GetTrait(asTRAIT_GENERIC_TEMPLATE_FUNCTION))
						BehFunction = Engine->GenerateTemplateFunction(Type, BehFunction);

					if (Ref.MatchesSignature(*this, BehFunction))
					{
						FoundFunction = BehFunction;
					}
				}
			};

			CheckBeh(beh.factory);
			CheckBeh(beh.copyfactory);
			CheckBeh(beh.construct);
			CheckBeh(beh.copyconstruct);
			CheckBeh(beh.destruct);
			CheckBeh(beh.copy);
			CheckBeh(beh.templateCallback);

			for (int32 i = 0, Count = beh.constructors.GetLength(); i < Count; ++i)
				CheckBeh(beh.constructors[i]);

			for (int32 i = 0, Count = beh.factories.GetLength(); i < Count; ++i)
				CheckBeh(beh.factories[i]);
		}

		checkf(FoundFunction != nullptr,
			TEXT("Angelscript reference to function '%s' in type '%s' could not be found"),
			ANSI_TO_TCHAR(*Ref.Name), ANSI_TO_TCHAR(Type->GetName()));
	}
	else
	{
		asSNameSpace* ns = GetNamespace(Ref.Namespace);
		if (Ref.Module.Len() != 0)
		{
			// Look up global script function from a module
			asCModule* Module = FindModuleForPrecompiledReference(Engine, Ref.Module);
			checkf(Module != nullptr,
				TEXT("Module '%s' for angelscript reference to function '%s' could not be found"),
				ANSI_TO_TCHAR(*Ref.Module), ANSI_TO_TCHAR(*Ref.Name));
			Module->globalFunctions.FindAllUntil(*Ref.Name, ns, [&](asCScriptFunction* CheckFunc)
			{
				if (CheckFunc == nullptr)
					return false;

				if (!Ref.MatchesSignature(*this, CheckFunc))
					return false;

				FoundFunction = CheckFunc;
				return true;
			});

			checkf(FoundFunction != nullptr,
				TEXT("Angelscript reference to function '%s' in module '%s' could not be found"),
				ANSI_TO_TCHAR(*Ref.Name), ANSI_TO_TCHAR(*Ref.Module));
		}
		else
		{
			// Look up global system function bound to the engine
			Engine->registeredGlobalFuncTable.FindAllUntil(*Ref.Name, ns, [&](asCScriptFunction* CheckFunc)
			{
				if (!Ref.MatchesSignature(*this, CheckFunc))
					return false;

				FoundFunction = CheckFunc;
				return true;
			});

			checkf(FoundFunction != nullptr,
				TEXT("Angelscript reference to global system function '%s' could not be found"),
				ANSI_TO_TCHAR(*Ref.Name));
		}
	}

	CachedPointerReferences.Add(Reference.OldReference, FoundFunction);

	if (bAddRef)
		FoundFunction->AddRefInternal();
	if (bMarkInUse)
		FoundFunction->isInUse = true;
	return FoundFunction;
}


FAngelscriptPrecompiledReference FAngelscriptPrecompiledData::ReferenceFunctionId(int FunctionId)
{
	if (FunctionId == 0)
		return FAngelscriptPrecompiledReference{ 0 };

	asCScriptFunction* Function = Engine->GetScriptFunction(FunctionId);

	check(Function->GetId() == FunctionId);

	ReferenceFunction(Function);
	return FAngelscriptPrecompiledReference{ (int64)FunctionId };
}

int FAngelscriptPrecompiledData::GetFunctionId(const FAngelscriptPrecompiledReference& Reference, bool bAddRef, bool bMarkInUse)
{
	if (Reference.OldReference == 0)
		return 0;

	int64* FoundPointer = FunctionIdReferenceToPointer.Find((int)Reference.OldReference);
	checkf(FoundPointer != nullptr, TEXT("Loaded an angelscript function id that wasn't saved properly!"));

	asCScriptFunction* Function = GetFunction(FAngelscriptPrecompiledReference{ *FoundPointer }, bAddRef, bMarkInUse);
	return Function->GetId();
}

FAngelscriptPrecompiledReference FAngelscriptPrecompiledData::ReferenceGlobalVariable(void* GlobalPtr, FString* OutName)
{
	if (GlobalPtr == nullptr)
		return FAngelscriptPrecompiledReference{ 0 };

	int64 Pointer = (int64)(SIZE_T)GlobalPtr;
	if (!GlobalReferences.Contains(Pointer))
	{
		FAngelscriptGlobalReference Ref;

		if (asCGlobalProperty** PropertyPtr = Engine->varAddressMap.Find(GlobalPtr))
		{
			// This is a global variable managed by the engine
			asCGlobalProperty* Property = *PropertyPtr;

			Ref.Name = Property->name.AddressOf();
			Ref.Namespace = Property->nameSpace->GetName();

			if (Property->module != nullptr)
				Ref.Module = Property->module->name.AddressOf();

			if (OutName != nullptr)
				*OutName = Ref.Name.UnrealString();
		}
		else
		{
			// This is a string literal global
			Ref.bIsString = true;
			Ref.Name.AssignAsUTF8(*(FString*)GlobalPtr);

			if (OutName != nullptr)
				*OutName = *(FString*)GlobalPtr;
		}

		GlobalReferences.Add(Pointer, Ref);
	}

	return FAngelscriptPrecompiledReference{ Pointer };
}

void* FAngelscriptPrecompiledData::GetGlobalVariable(const FAngelscriptPrecompiledReference& Reference, asCGlobalProperty** OutProperty)
{
	if (Reference.OldReference == 0)
	{
		if (OutProperty != nullptr)
			*OutProperty = nullptr;
		return nullptr;
	}

	// Check if it's in the cache
	void** GlobalPtr = CachedPointerReferences.Find(Reference.OldReference);
	if (GlobalPtr != nullptr)
	{
		auto* Property = (asCGlobalProperty*)*GlobalPtr;
		if (OutProperty != nullptr)
			*OutProperty = Property;
		return Property->memory;
	}

	FAngelscriptScopeTotalTimer Timer(TIMER_GlobalLookup);

	// Do the reference lookup
	FAngelscriptGlobalReference* RefPtr = GlobalReferences.Find(Reference.OldReference);
	checkf(RefPtr != nullptr, TEXT("Loaded an angelscript global reference that wasn't saved properly!"));

	FAngelscriptGlobalReference& Ref = *RefPtr;

	asCGlobalProperty* Property = nullptr;

	if (Ref.bIsString)
	{
		// Don't cache here, we need to create a new string for every reference
		if (OutProperty != nullptr)
			*OutProperty = nullptr;

		return new FString(Ref.Name.UnrealString_UTF8());
	}
	else
	{
		asSNameSpace* ns = GetNamespace(Ref.Namespace);

		if (Ref.Module.Len() != 0)
		{
			// Look up a script global from a module
			asCModule* Module = FindModuleForPrecompiledReference(Engine, Ref.Module);
			checkf(Module != nullptr,
				TEXT("Module '%s' for angelscript global reference to '%s' could not be found"),
				ANSI_TO_TCHAR(*Ref.Module), ANSI_TO_TCHAR(*Ref.Name));
			Property = Module->scriptGlobals.FindFirst(*Ref.Name, ns);
		}
		else
		{
			// Look up an system global from the engine
			Property = Engine->registeredGlobalPropTable.FindFirst(*Ref.Name, ns);
		}

		check(Property->memory != nullptr);
		CachedPointerReferences.Add(Reference.OldReference, Property);
	}

	if (OutProperty != nullptr)
		*OutProperty = Property;
	return Property->memory;
}

FAngelscriptPrecompiledReference FAngelscriptPrecompiledData::ReferenceProperty(int Offset, int TypeId, asCObjectProperty** OutProperty, asCObjectType** OutObjectType)
{
	auto* TypeInfo = (asCObjectType*)Engine->GetTypeInfoById(TypeId);
	check(TypeInfo != nullptr);
	asCObjectType* FromType = nullptr;
	asCObjectProperty* Property = TypeInfo->GetPropertyByOffset(Offset, &FromType);
	check(Property != nullptr);

	if (OutProperty != nullptr)
		*OutProperty = Property;
	if (OutObjectType != nullptr)
		*OutObjectType = FromType;

	int64 PropertyId = GetPropertyReferenceId(Offset, FromType->GetTypeId());
	if (PropertyReferences.Contains(PropertyId))
	{
		check(PropertyReferences[PropertyId].Name == Property->name.AddressOf());
		check(PropertyReferences[PropertyId].OldTypeId == FromType->GetTypeId());
		return FAngelscriptPrecompiledReference{ PropertyId };
	}

	ReferenceTypeId(FromType->GetTypeId());

	FAngelscriptPropertyReference PropRef;
	PropRef.Name = Property->name.AddressOf();
	PropRef.OldTypeId = FromType->GetTypeId();

	PropertyReferences.Add(PropertyId, PropRef);
	return FAngelscriptPrecompiledReference{PropertyId};
}

int64 FAngelscriptPrecompiledData::GetPropertyReferenceId(int Offset, int TypeId)
{
	return (((int64)TypeId) << 1) | (((int64)Offset) << 33) | (int64)1;
}

short FAngelscriptPrecompiledData::GetPropertyOffset(int OldOffset, int OldTypeId)
{
	return GetPropertyOffset(FAngelscriptPrecompiledReference{ GetPropertyReferenceId(OldOffset, OldTypeId) });
}

short FAngelscriptPrecompiledData::GetPropertyOffset(const FAngelscriptPrecompiledReference& Reference)
{
	if ((Reference.OldReference & 0xffffffff) == 1)
		return (short)((Reference.OldReference & 0xfffffffe00000000ll) >> 33);

	// Check if it's in the cache
	void** OffsetPtr = CachedPointerReferences.Find(Reference.OldReference);
	if (OffsetPtr != nullptr)
		return (short)(SIZE_T)*OffsetPtr;

	FAngelscriptScopeTotalTimer Timer(TIMER_PropertyLookup);
	FAngelscriptPropertyReference* Ref = PropertyReferences.Find(Reference.OldReference);
	if (Ref == nullptr)
	{
		short UseOldOffset = (short)((Reference.OldReference & 0xfffffffe00000000ll) >> 33);
		CachedPointerReferences.Add(Reference.OldReference, (void*)(SIZE_T)UseOldOffset);
		return UseOldOffset;
	}

	int32 TypeId = GetTypeId(FAngelscriptPrecompiledReference{ Ref->OldTypeId });
	auto* TypeInfo = (asCObjectType*)Engine->GetTypeInfoById(TypeId);
	check(TypeInfo != nullptr);

	asCObjectProperty* Property = TypeInfo->GetFirstProperty(*Ref->Name);
	check(Property != nullptr);

	void* OffsetAsPtr = (void*)(SIZE_T)Property->byteOffset;
	CachedPointerReferences.Add(Reference.OldReference, OffsetAsPtr);

	return (short)Property->byteOffset;
}

short FAngelscriptPrecompiledData::GetTypeSize(int NewTypeId, short OldSize)
{
	auto* TypeInfo = (asCObjectType*)Engine->GetTypeInfoById(NewTypeId);
	if (TypeInfo == nullptr)
		return OldSize;
	return TypeInfo->size;
}

void FAngelscriptPrecompiledData::AddRefTo(const class asCDataType& DataType)
{
	if (auto* TypeInfo = DataType.GetTypeInfo())
		TypeInfo->AddRefInternal();
}

                                                                                                   
bool FAngelscriptPrecompiledData::IsReferenceReady(int64 Reference) const
{
	if (Reference == 0) return true;
	if (const auto* Type = TypeReferences.Find(Reference))
	{
		if (Type->Module.Len() != 0 && FCStringAnsi::Strcmp(*Type->Module, "$__T__") != 0 && Type->SubTypes.IsEmpty()
			&& Engine->GetModule(*Type->Module, false) == nullptr) return false;
		for (const auto& SubType : Type->SubTypes)
			if (!IsReferenceReady(SubType.TypeInfo.OldReference)) return false;
	}
	if (const auto* Function = FunctionReferences.Find(Reference))
	{
		if (Function->Module.Len() != 0 && Engine->GetModule(*Function->Module, false) == nullptr) return false;
		if (!IsReferenceReady(Function->ObjectType.OldReference)
			|| !IsReferenceReady(Function->ReturnType.TypeInfo.OldReference)) return false;
		for (const auto& Parameter : Function->ParameterTypes)
			if (!IsReferenceReady(Parameter.TypeInfo.OldReference)) return false;
	}
	if (const auto* Global = GlobalReferences.Find(Reference))
		if (Global->Module.Len() != 0 && Engine->GetModule(*Global->Module, false) == nullptr) return false;
	return true;
}
                                                                                                 

void FAngelscriptPrecompiledData::PrepareToFinalizePrecompiledModules()
{
	if (FAngelscriptManager::bScriptDevelopmentMode)
		return;

	FJITDatabase& Database = FJITDatabase::Get();
	for (void** FuncPtr : Database.FunctionLookups)
	{
                                                                                                   
		if (FinalizedJitSlots.Contains(FuncPtr) || !IsReferenceReady((int64)*FuncPtr)) continue;
		FinalizedJitSlots.Add(FuncPtr);
                                                                                                 
		*FuncPtr = GetFunction(
			FAngelscriptPrecompiledReference{ (int64)*FuncPtr },
			false
		);
	}

	for (void* SysPtr : Database.SystemFunctionPointerLookups)
	{
		auto* JitRef = (FJitRef_SystemFunctionPointer*)SysPtr;
                                                                                                         
		if (FinalizedJitSlots.Contains(SysPtr) || !IsReferenceReady((int64)((FJitRef_SystemFunctionPointer*)SysPtr)->Pointer)) continue;
		FinalizedJitSlots.Add(SysPtr);
                                                                                                       
		asCScriptFunction* Function = GetFunction(
			FAngelscriptPrecompiledReference{ (int64)JitRef->Pointer },
			false
		);

		asSSystemFunctionInterface* SysFunc = Function->sysFuncIntf;
		switch (SysFunc->callConv)
		{
		case ICC_THISCALL:
		case ICC_THISCALL_RETURNINMEM:
		case ICC_VIRTUAL_THISCALL:
		case ICC_VIRTUAL_THISCALL_RETURNINMEM:
			JitRef->Method = SysFunc->method;
		break;
		default:
			JitRef->Func = SysFunc->func;
		break;
		}
	}

	for (void** TypePtr : Database.TypeInfoLookups)
	{
                                                                                               
		if (FinalizedJitSlots.Contains(TypePtr) || !IsReferenceReady((int64)*TypePtr)) continue;
		FinalizedJitSlots.Add(TypePtr);
                                                                                             
		*TypePtr = GetTypeInfo(
			FAngelscriptPrecompiledReference{ (int64)*TypePtr },
			false
		);
	}

	for (void** GlobalPtr : Database.GlobalVarLookups)
	{
                                                                                                 
		if (FinalizedJitSlots.Contains(GlobalPtr) || !IsReferenceReady((int64)*GlobalPtr)) continue;
		FinalizedJitSlots.Add(GlobalPtr);
                                                                                               
		*GlobalPtr = GetGlobalVariable(
			FAngelscriptPrecompiledReference{ (int64)*GlobalPtr },
			nullptr
		);
	}

	for (TPair<uint64,uint32*> OffsetPtr : Database.PropertyOffsetLookups)
	{
                                                                                                 
		const auto* Property = PropertyReferences.Find(OffsetPtr.Key);
		const auto* TypeReference = Property ? TypeIdReferenceToPointer.Find(Property->OldTypeId) : nullptr;
		if (FinalizedJitSlots.Contains(OffsetPtr.Value)
			|| (TypeReference && !IsReferenceReady(*TypeReference))) continue;
		FinalizedJitSlots.Add(OffsetPtr.Value);
                                                                                               
		*OffsetPtr.Value = (uint32)(SIZE_T)GetPropertyOffset(
			FAngelscriptPrecompiledReference{ (int64)OffsetPtr.Key }
		);
	}

                                                                                               
	if (!bStaticNamesFinalized)
	{
		check(FAngelscriptManager::StaticNames.Num() == 0);
		FAngelscriptManager::StaticNames.Reserve(StaticNames.Num());
		for (const FStringInArchive& StaticName : StaticNames)
			FAngelscriptManager::StaticNames.Add(FName(*StaticName.UnrealString()));
		bStaticNamesFinalized = true;
	}
                                                                                             

#if AS_JIT_VERIFY_PROPERTY_OFFSETS
	for (TPair<uint64,SIZE_T> Element : Database.VerifyPropertyOffsets)
	{
		FAngelscriptPropertyReference* Ref = PropertyReferences.Find(Element.Key);
		if (!ensure(Ref != nullptr))
			continue;

                                                                                                          
		const auto* TypeReference = TypeIdReferenceToPointer.Find(Ref->OldTypeId);
		if (TypeReference && !IsReferenceReady(*TypeReference)) continue;
                                                                                                        
		int32 TypeId = GetTypeId(FAngelscriptPrecompiledReference{ Ref->OldTypeId });
		auto* TypeInfo = (asCObjectType*)Engine->GetTypeInfoById(TypeId);
		check(TypeInfo != nullptr);

		asCObjectProperty* Property = TypeInfo->GetFirstProperty(*Ref->Name);
		check(Property != nullptr);

		checkf(Property->byteOffset == Element.Value, TEXT("Property offset for %s::%s differs between compile time and runtime"),
			StringCast<TCHAR>(TypeInfo->GetName()).Get(), StringCast<TCHAR>(Property->GetName()).Get());
	}

	for (TPair<uint64,SIZE_T> Element : Database.VerifyTypeSizes)
	{
                                                                                                         
		if (!IsReferenceReady((int64)Element.Key)) continue;
                                                                                                       
		asCTypeInfo* TypeInfo = GetTypeInfo(FAngelscriptPrecompiledReference{ (int64)Element.Key }, false);
		check(TypeInfo != nullptr);

		checkf(TypeInfo->size == Element.Value, TEXT("Type size for %s differs between compile time and runtime"),
			StringCast<TCHAR>(TypeInfo->GetName()).Get());
	}

	for (TPair<uint64,SIZE_T> Element : Database.VerifyTypeAlignments)
	{
                                                                                                              
		if (!IsReferenceReady((int64)Element.Key)) continue;
                                                                                                            
		asCTypeInfo* TypeInfo = GetTypeInfo(FAngelscriptPrecompiledReference{ (int64)Element.Key }, false);
		check(TypeInfo != nullptr);

		checkf(TypeInfo->alignment == Element.Value, TEXT("Type size for %s differs between compile time and runtime"),
			StringCast<TCHAR>(TypeInfo->GetName()).Get());
	}
#endif
}

void FAngelscriptPrecompiledData::ClearUnneededRuntimeData()
{
	FAngelscriptScopeTimer Timer(TEXT("ClearUnneededRuntimeData"));

	auto ClearObjectType = [&](asCObjectType* objType)
	{
		objType->propertyTable.EraseAll();
		objType->methodTable.EraseAll();
	};

	auto ClearEnumType = [](asCEnumType* enumType)
	{
		for (int32 i = 0, Count = enumType->enumValues.GetLength(); i < Count; ++i)
			asDELETE(enumType->enumValues[i], asSEnumValue);
		enumType->enumValues.Allocate(0, false);
	};

	for (int32 i = 0, Count = Engine->registeredObjTypes.GetLength(); i < Count; ++i)
	{
		asCObjectType* objType = Engine->registeredObjTypes[i];
		if (objType == nullptr)
			continue;
		ClearObjectType(objType);
	}

	for (int32 i = 0, Count = Engine->registeredEnums.GetLength(); i < Count; ++i)
	{
		asCEnumType* enumType = Engine->registeredEnums[i];
		if (enumType == nullptr)
			continue;
		ClearEnumType(enumType);
	}

	for (int32 i = 0, Count = Engine->scriptFunctions.GetLength(); i < Count; ++i)
	{
		asCScriptFunction* func = Engine->scriptFunctions[i];
		if (func == nullptr)
			continue;
		if (func->funcType == asFUNC_SYSTEM)
		{
			if (!func->isInUse)
			{
				asCObjectType* objType = func->objectType;
				bool bCanDelete = true;
				if (objType != nullptr)
				{
					if (objType->beh.construct == func->id)
						bCanDelete = false;
					else if (objType->beh.copy == func->id)
						bCanDelete = false;
					else if (objType->beh.factory == func->id)
						bCanDelete = false;
					else if (objType->beh.destruct == func->id)
						bCanDelete = false;
				}

				if (bCanDelete)
				{
					func->id = 0;
					asDELETE(func, asCScriptFunction);
					continue;
				}
			}
		}

		func->defaultArgs.Allocate(0, false);
		func->parameterNames.Allocate(0, false);
		func->hiddenArgumentDefault.Allocate(0, false);
	}

                                                                                                                     
	// Global properties and the engine function table still reference init functions.
	// Keep their IDs and internal references intact for normal engine teardown.
                                                                                                                   

	for (int32 i = 0, Count = Engine->scriptModules.GetLength(); i < Count; ++i)
	{
		auto* Module = Engine->scriptModules[i];
		if (Module == nullptr)
			continue;

		for (int32 j = 0, jCount = Module->classTypes.GetLength(); j < jCount; ++j)
		{
			asCObjectType* objType = Module->classTypes[j];
			if (objType == nullptr)
				continue;
			ClearObjectType(objType);
		}

		for (int32 j = 0, jCount = Module->enumTypes.GetLength(); j < jCount; ++j)
		{
			asCEnumType* enumType = Module->enumTypes[j];
			if (enumType == nullptr)
				continue;
			ClearEnumType(enumType);
		}

		Module->scriptGlobals.EraseAll();
		Module->scriptGlobalsList.SetLength(0);
		Module->globalFunctions.EraseAll();
		Module->globalFunctionList.SetLength(0);
		Module->allLocalTypes.EraseAll();
		Module->classTypes.AllocateNoConstruct(0, false);
		Module->enumTypes.AllocateNoConstruct(0, false);
		Module->scriptFunctions.AllocateNoConstruct(0, false);
		Module->bindInformations.AllocateNoConstruct(0, false);
		Module->PreClassData.Empty();
	}

	Engine->nameSpacesByName.EraseAll();
	Engine->scriptModulesByName.EraseAll();
	Engine->templateInstanceBuckets.Empty();
	Engine->allRegisteredTypesByName.EraseAll();
	Engine->allRegisteredTypes.EraseAll();
	Engine->registeredTemplateTypes.AllocateNoConstruct(0, false);
	Engine->registeredGlobalFuncs.SetLength(0);
	Engine->registeredGlobalFuncTable.EraseAll();
	Engine->registeredGlobalProps.SetLength(0);
	Engine->registeredGlobalPropTable.EraseAll();
	Engine->registeredEnums.AllocateNoConstruct(0, false);
	Engine->registeredTypeDefs.AllocateNoConstruct(0, false);
	Engine->registeredObjTypes.AllocateNoConstruct(0, false);
	Engine->nameSpaces.AllocateNoConstruct(1, false);
	Engine->varAddressMap.Empty();

	auto& Manager = FAngelscriptManager::Get();
	Manager.ActiveModules.Empty();
	Manager.BoundBlueprintEventArgumentSpecializations.Empty();
	Manager.ModulesByScriptModule.Empty();
#if AS_CAN_HOTRELOAD
	Manager.ActiveClassesByName.Empty();
	Manager.ActiveLiteralAssetsByName.Empty();
	Manager.ActiveDelegatesByName.Empty();
	Manager.ActiveEnumsByName.Empty();
#endif

	extern TMap<UClass*, TMap<FString, UFunction*>> GBlueprintEventsByScriptName;
	GBlueprintEventsByScriptName.Empty();
}

FAngelscriptPrecompiledData::FAngelscriptPrecompiledData(asIScriptEngine* InEngine)
	: AllocMark(GScriptPreallocatedMemStack)
	, Engine((asCScriptEngine*)InEngine)
{
	DataGuid = FGuid::NewGuid();
	ClassesLoadedFromPrecompiledData = MakeShared<TMap<void*, FAngelscriptPrecompiledClassOwner>>();
}

FAngelscriptPrecompiledData::~FAngelscriptPrecompiledData()
{
}

int32 FAngelscriptPrecompiledData::GetCurrentBuildIdentifier()
{
                                                                                                           
	const int32 Configuration = []() -> int32
	{
                                                                                                         
#if UE_BUILD_DEBUG
	return 1;
#elif UE_BUILD_DEVELOPMENT
	return 2;
#elif UE_BUILD_TEST
	return 3;
#elif UE_BUILD_SHIPPING
	return 4;
#else
	return -1;
#endif
                                                                                                         
	}();
	// Bind the stored member flag to the destructor bytecode that implements it.
	const int32 MemberProtocolIdentifier = Configuration == -1 ? -1 : (Configuration | (1 << 8));
                                                                                                       
	                                                                                                     
	return MemberProtocolIdentifier == -1 ? -1 : (MemberProtocolIdentifier | (1 << 9) | (1 << 10));
	                                                                                                   
}

bool FAngelscriptPrecompiledData::IsValidForCurrentBuild()
{
	return BuildIdentifier == GetCurrentBuildIdentifier() && BuildIdentifier != -1;
}

void FAngelscriptPrecompiledData::InitFromActiveScript()
{
	BuildIdentifier = GetCurrentBuildIdentifier();

	int32 ModuleCount = Engine->GetModuleCount();
	Modules.Reserve(ModuleCount);
	for (int32 i = 0; i < ModuleCount; ++i)
	{
		asCModule* Module = (asCModule*)Engine->GetModuleByIndex(i);
		ModuleDesc = FAngelscriptManager::Get().GetModule(Module);
                                                                                                          
		FString CacheGenerationUnitId;
		const bool bSelectedCacheGenerationUnitId = FParse::Value(FCommandLine::Get(), TEXT("as-cache-unit="), CacheGenerationUnitId)
			&& !CacheGenerationUnitId.IsEmpty()
			&& CacheGenerationUnitId == FPaths::GetCleanFilename(CacheGenerationUnitId)
			&& !CacheGenerationUnitId.Contains(TEXT(".."));
		const bool bIsSelectedLeafCache = ModuleDesc.IsValid() && bSelectedCacheGenerationUnitId
			&& ModuleDesc->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf
			&& ModuleDesc->ScriptUnit.Id == CacheGenerationUnitId;
		if (!ModuleDesc.IsValid()
			|| (ModuleDesc->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf && !bIsSelectedLeafCache)
			|| FAngelscriptManager::Get().GetModule(ModuleDesc->ModuleName) != ModuleDesc) continue;
                                                                                                        

		FString ModuleName = Module->GetName();
                                                                                                      
		// Cuviss Modding registration point: cache generation module filter.
		// Registered policy may exclude units from the project cache.
		if (FAngelscriptModdingCacheGenerationFilterRuntime::IsGameCacheExcludedProjectScriptUnit(ModuleDesc->ScriptUnit.Id)
			&& !bIsSelectedLeafCache)
		{
			continue;
		}
		{
			TArray<FString> CuvissRelativeFilenames;
			if (ModuleDesc.IsValid())
			{
				for (const FAngelscriptModuleDesc::FCodeSection& CuvissSection : ModuleDesc->Code)
				{
					CuvissRelativeFilenames.Add(CuvissSection.RelativeFilename);
				}
			}
			if (FAngelscriptModdingCacheGenerationFilterRuntime::AnyExclude(ModuleName, CuvissRelativeFilenames))
			{
				continue;
			}
		}
		FAngelscriptPrecompiledModule& CuvissCachedModule = Modules.FindOrAdd(ModuleName);
		CuvissCachedModule.InitFrom(*this, Module);
		// Apply the same registered policy to cached unit dependencies.
		CuvissCachedModule.ScriptUnitDependencies.RemoveAll([](const FStringInArchive& Dependency)
		{
			return FAngelscriptModdingCacheGenerationFilterRuntime::IsGameCacheExcludedProjectScriptUnit(Dependency.UnrealString());
		});
                                                                                                    
	}

	for (const FName& StaticName : FAngelscriptManager::StaticNames)
		StaticNames.Add(StaticName.ToString());
}

                                                                                                  
void FAngelscriptPrecompiledData::Save(const FString& Filename)
{
	FString ModuleCacheError;
	if (!SaveModuleCacheSets(ModuleCacheError))
	{
		UE_LOG(Angelscript, Fatal, TEXT("Precompiled module cache generation failed: %s"), *ModuleCacheError);
	}
	if (!SaveSerialized(Filename))
	{
		UE_LOG(Angelscript, Fatal, TEXT("Precompiled cache file was not written: %s"), *Filename);
	}
}
                                                                                                

                                                                                                  
bool FAngelscriptPrecompiledData::SaveSerialized(const FString& Filename)
                                                                                                
{
	TArray<uint8> Data;
	FMemoryWriter Writer(Data, true);
	Writer.SetIsPersistent(true);
	Writer.SetWantBinaryPropertySerialization(true);

                                                                                                       
	// Pre-serialize structure transform (strip debug info, permute opcodes).
	// Empty registry keeps upstream behavior bit-for-bit: the whole block is skipped.
	{
		FAngelscriptModdingPrecompiledDataTransform::FPolicy TransformPolicy;
		if (FAngelscriptModdingPrecompiledDataTransformRuntime::GetPolicy(
				FAngelscriptModdingPrecompiledDataTransform::EDirection::Save, TransformPolicy)
			&& TransformPolicy.bTransform)
		{
			if (TransformPolicy.ShouldStripBytecode)
			{
				// Count cache identities before filtering. A repeated identity is
				// ambiguous and must retain bytecode for every matching record.
				TMap<FString, int32> CuvissIdentityCounts;
				CuvissForEachByteCodeArray(
					*this,
					[&CuvissIdentityCounts](FAngelscriptPrecompiledModule& Module,
						FAngelscriptPrecompiledFunction& Function, bool)
				{
					const FString Identity = CuvissMakeJitFunctionIdentity(
						Module.ModuleName.UnrealString(), Function.FunctionName.UnrealString(), Function.Id);
					if (!Identity.IsEmpty())
					{
						++CuvissIdentityCounts.FindOrAdd(Identity);
					}
				});
				int32 CuvissStrippedCount = 0;
				int32 CuvissVmRequiredRetainedCount = 0;
				int32 CuvissUnmatchedRetainedCount = 0;
				int32 CuvissAmbiguousRetainedCount = 0;
				CuvissForEachByteCodeArray(*this, [&TransformPolicy, &CuvissIdentityCounts, &CuvissStrippedCount,
					&CuvissVmRequiredRetainedCount, &CuvissUnmatchedRetainedCount, &CuvissAmbiguousRetainedCount](
						FAngelscriptPrecompiledModule& Module, FAngelscriptPrecompiledFunction& Function, bool bVmEntryRequired)
				{
					if (Function.ByteCode.IsEmpty())
					{
						return;
					}
					if (bVmEntryRequired)
					{
						++CuvissVmRequiredRetainedCount;
						return;
					}
					const FString Identity = CuvissMakeJitFunctionIdentity(
						Module.ModuleName.UnrealString(), Function.FunctionName.UnrealString(), Function.Id);
					const int32* IdentityCount = CuvissIdentityCounts.Find(Identity);
					if (Identity.IsEmpty() || IdentityCount == nullptr || *IdentityCount != 1)
					{
						++CuvissAmbiguousRetainedCount;
						return;
					}
					if (TransformPolicy.ShouldStripBytecode(Module.ModuleName.UnrealString(), Identity, false))
					{
						Function.ByteCode.Reset();
						++CuvissStrippedCount;
					}
					else
					{
						++CuvissUnmatchedRetainedCount;
					}
				});
				if (TransformPolicy.OnBytecodeStripComplete)
				{
					TransformPolicy.OnBytecodeStripComplete(CuvissStrippedCount, CuvissVmRequiredRetainedCount,
						CuvissUnmatchedRetainedCount, CuvissAmbiguousRetainedCount);
				}
			}
			if (TransformPolicy.bStripDebugInfo)
			{
				CuvissStripDebugInfo(*this);
			}
			if (TransformPolicy.OpcodePermutation.Num() == 256)
			{
				CuvissForEachByteCodeArray(*this, [&TransformPolicy](FAngelscriptPrecompiledModule&, FAngelscriptPrecompiledFunction& Function, bool)
				{
					CuvissApplyOpcodePermutation(Function.ByteCode, TransformPolicy.OpcodePermutation, false);
				});
			}
		}
	}
                                                                                                     
	Writer << *this;

                                                                                                       
	// Post-serialize byte transform: ModKit encrypts the serialized buffer and
	// prepends the MKCC header. Empty registry leaves the bytes untouched.
	{
		FAngelscriptModdingPrecompiledDataTransform::FPolicy TransformPolicy;
		if (FAngelscriptModdingPrecompiledDataTransformRuntime::GetPolicy(
				FAngelscriptModdingPrecompiledDataTransform::EDirection::Save, TransformPolicy)
			&& TransformPolicy.bTransform)
		{
			FAngelscriptModdingPrecompiledDataTransformRuntime::TransformBytes(
				FAngelscriptModdingPrecompiledDataTransform::EDirection::Save, TransformPolicy, Data);
		}
	}
                                                                                                     
	return FFileHelper::SaveArrayToFile(Data, *Filename);
}

                                                                                                  
bool FAngelscriptPrecompiledData::SaveModuleCacheSets(FString& OutError)
{
	if (ModuleDataGuids.IsEmpty())
	{
		return true;
	}

	TMap<FString, TArray<FAngelscriptPrecompiledCacheFile>> FilesByDirectory;
	TSet<FGuid> SeenDataGuids;
	for (const TPair<FString, FAngelscriptPrecompiledModule>& Pair : Modules)
	{
		const FGuid* ModuleGuid = ModuleDataGuids.Find(Pair.Key);
		if (ModuleGuid == nullptr || !ModuleGuid->IsValid() || SeenDataGuids.Contains(*ModuleGuid))
		{
			OutError = FString::Printf(TEXT("missing or repeated module DataGuid for %s"), *Pair.Key);
			return false;
		}
		SeenDataGuids.Add(*ModuleGuid);

		FAngelscriptPrecompiledCacheFile Entry;
		Entry.ModuleName = Pair.Key;
		Entry.FileName = GetModuleCacheFilename(Pair.Key);
		Entry.Dependencies = Pair.Value.Dependencies;
		FilesByDirectory.FindOrAdd(GetModuleCacheDirectory(Pair.Value.ScriptRelativeFilename.UnrealString())).Add(MoveTemp(Entry));
	}

	TArray<FString> Directories;
	FilesByDirectory.GetKeys(Directories);
	Directories.Sort();
	for (const FString& Directory : Directories)
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
		TArray<FAngelscriptPrecompiledCacheFile>& Entries = FilesByDirectory.FindChecked(Directory);
		Entries.Sort([](const FAngelscriptPrecompiledCacheFile& A, const FAngelscriptPrecompiledCacheFile& B)
		{
			return A.ModuleName < B.ModuleName;
		});

		TArray<TSharedPtr<FJsonValue>> ManifestEntries;
		for (const FAngelscriptPrecompiledCacheFile& Entry : Entries)
		{
			const FString CachePath = Directory / Entry.FileName;
			if (IsAngelscriptExtensionProviderRegistered())
			{
				FString CacheError;
				if (!PrepareAngelscriptExtensionCachePublish(
						EAngelscriptExtensionCacheArtifactFamily::Precompiled,
						CachePath,
						CacheError))
				{
					OutError = FString::Printf(TEXT("%s: %s"), *CachePath, *CacheError);
					return false;
				}
			}

			FAngelscriptPrecompiledData ModuleData(Engine);
			ModuleData.DataGuid = ModuleDataGuids.FindChecked(Entry.ModuleName);
			ModuleData.BuildIdentifier = BuildIdentifier;
			ModuleData.Modules.Add(Entry.ModuleName, Modules.FindChecked(Entry.ModuleName));
			ModuleData.TypeReferences = TypeReferences;
			ModuleData.TypeIdReferenceToPointer = TypeIdReferenceToPointer;
			ModuleData.FunctionReferences = FunctionReferences;
			ModuleData.FunctionIdReferenceToPointer = FunctionIdReferenceToPointer;
			ModuleData.GlobalReferences = GlobalReferences;
			ModuleData.PropertyReferences = PropertyReferences;
			ModuleData.StaticNames = StaticNames;
			if (!ModuleData.SaveSerialized(CachePath))
			{
				OutError = FString::Printf(TEXT("cache file was not written: %s"), *CachePath);
				return false;
			}

			if (IsAngelscriptExtensionProviderRegistered())
			{
				FString CacheError;
				if (!WriteAngelscriptExtensionCacheCompanion(
						EAngelscriptExtensionCacheArtifactFamily::Precompiled,
						CachePath,
						CacheError))
				{
					OutError = FString::Printf(TEXT("%s: %s"), *CachePath, *CacheError);
					return false;
				}
			}

			TSharedPtr<FJsonObject> ManifestEntry = MakeShared<FJsonObject>();
			ManifestEntry->SetStringField(TEXT("module"), Entry.ModuleName);
			ManifestEntry->SetStringField(TEXT("file"), Entry.FileName);
			ManifestEntry->SetStringField(TEXT("dataGuid"), ModuleDataGuids.FindChecked(Entry.ModuleName).ToString(EGuidFormats::Digits));
			TArray<TSharedPtr<FJsonValue>> DependencyEntries;
			for (const FString& DependencyName : Entry.Dependencies)
				DependencyEntries.Add(MakeShared<FJsonValueString>(DependencyName));
			ManifestEntry->SetArrayField(TEXT("dependencies"), DependencyEntries);
			ManifestEntries.Add(MakeShared<FJsonValueObject>(ManifestEntry));
		}

		TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
		Manifest->SetNumberField(TEXT("schemaVersion"), ModuleCacheManifestVersion);
		Manifest->SetNumberField(TEXT("buildIdentifier"), BuildIdentifier);
		Manifest->SetArrayField(TEXT("modules"), ManifestEntries);
		FString ManifestContent;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ManifestContent);
		if (!FJsonSerializer::Serialize(Manifest.ToSharedRef(), Writer)
			|| !FFileHelper::SaveStringToFile(ManifestContent, *(Directory / ModuleCacheManifestName)))
		{
			OutError = FString::Printf(TEXT("module cache manifest was not written: %s"), *(Directory / ModuleCacheManifestName));
			return false;
		}
	}

	return true;
}
                                                                                                

                                                                                                  
bool FAngelscriptPrecompiledData::ReadModuleCacheSet(
	const FString& CacheDirectory,
	TArray<FAngelscriptPrecompiledCacheFile>& OutFiles,
	FString& OutError, const TArray<uint8>* VerifiedManifestBytes)
{
	OutFiles.Reset();
	FString ManifestContent;
	if (VerifiedManifestBytes) FFileHelper::BufferToString(ManifestContent, VerifiedManifestBytes->GetData(), VerifiedManifestBytes->Num());
	else if (!FFileHelper::LoadFileToString(ManifestContent, *(CacheDirectory / ModuleCacheManifestName)))
	{
		OutError = TEXT("module cache manifest is missing or unreadable");
		return false;
	}

	TSharedPtr<FJsonObject> Manifest;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ManifestContent), Manifest) || !Manifest.IsValid())
	{
		OutError = TEXT("module cache manifest is malformed");
		return false;
	}
	double SchemaVersion = 0.0;
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Manifest->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion)
		|| SchemaVersion != ModuleCacheManifestVersion
		|| !Manifest->TryGetArrayField(TEXT("modules"), Entries)
		|| Entries == nullptr
		|| Entries->IsEmpty())
	{
		OutError = TEXT("module cache manifest has an unsupported schema or no modules");
		return false;
	}

	TSet<FString> SeenModules;
	TSet<FString> SeenFiles;
	for (const TSharedPtr<FJsonValue>& Value : *Entries)
	{
		const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		FAngelscriptPrecompiledCacheFile CacheFile;
		FString DataGuidText;
		const TArray<TSharedPtr<FJsonValue>>* DependencyEntries = nullptr;
		if (!Entry.IsValid()
			|| !Entry->TryGetStringField(TEXT("module"), CacheFile.ModuleName)
			|| CacheFile.ModuleName.IsEmpty()
			|| !Entry->TryGetStringField(TEXT("file"), CacheFile.FileName)
			|| !Entry->TryGetStringField(TEXT("dataGuid"), DataGuidText)
			|| !Entry->TryGetArrayField(TEXT("dependencies"), DependencyEntries)
			|| DependencyEntries == nullptr
			|| !FGuid::ParseExact(DataGuidText, EGuidFormats::Digits, CacheFile.DataGuid)
			|| !CacheFile.DataGuid.IsValid()
			|| CacheFile.FileName.IsEmpty()
			|| CacheFile.FileName != FPaths::GetCleanFilename(CacheFile.FileName)
			|| !CacheFile.FileName.EndsWith(TEXT(".Cache"), ESearchCase::CaseSensitive)
			|| SeenModules.Contains(CacheFile.ModuleName)
			|| SeenFiles.Contains(CacheFile.FileName))
		{
			OutError = TEXT("module cache manifest contains an invalid or repeated entry");
			OutFiles.Reset();
			return false;
		}
		TSet<FString> SeenDependencies;
		for (const TSharedPtr<FJsonValue>& DependencyValue : *DependencyEntries)
		{
			FString DependencyName;
			if (!DependencyValue.IsValid()
				|| !DependencyValue->TryGetString(DependencyName)
				|| DependencyName.IsEmpty()
				|| DependencyName == CacheFile.ModuleName
				|| SeenDependencies.Contains(DependencyName))
			{
				OutError = TEXT("module cache manifest contains invalid dependencies");
				OutFiles.Reset();
				return false;
			}
			SeenDependencies.Add(DependencyName);
			CacheFile.Dependencies.Add(MoveTemp(DependencyName));
		}
		SeenModules.Add(CacheFile.ModuleName);
		SeenFiles.Add(CacheFile.FileName);
		OutFiles.Add(MoveTemp(CacheFile));
	}
	return true;
}
                                                                                                

                                                                                                                             
void FAngelscriptPrecompiledData::Load(const FString& Filename, const TArray<uint8>* VerifiedBytes)
                                                                                                                           
{
	CachedPointerReferences.Reserve(32000);
	ProcessedFunctionToId.Reserve(16000);

	{
		FAngelscriptScopeTimer Timer(TEXT("read precompiled data from disk"));
                                                                                                 
		BuildIdentifier = -1;
		if (VerifiedBytes) LoadedData = *VerifiedBytes;
		else if (!FFileHelper::LoadFileToArray(LoadedData, *Filename))
			return;
                                                                                               
	}

                                                                                                       
	// Pre-deserialize verify + decrypt. ModKit parses the MKCC header, verifies
	// the key binding and integrity, and writes plaintext back into LoadedData
	// (FStringInArchive pointers reference this exact buffer). A rejection
	// empties LoadedData: the deserialize below then no-ops on the empty
	// archive, BuildIdentifier stays -1, IsValidForCurrentBuild() reports
	// false and the caller falls back to source compilation (fail-closed).
	// Empty registry = no action, upstream bytes flow through untouched.
	{
		FAngelscriptModdingPrecompiledDataTransform::FPolicy TransformPolicy;
		if (FAngelscriptModdingPrecompiledDataTransformRuntime::GetPolicy(
				FAngelscriptModdingPrecompiledDataTransform::EDirection::Load, TransformPolicy))
		{
			if (!FAngelscriptModdingPrecompiledDataTransformRuntime::TransformBytes(
					FAngelscriptModdingPrecompiledDataTransform::EDirection::Load, TransformPolicy, LoadedData))
			{
				LoadedData.Reset();
			}
		}
	}
                                                                                                     

	FMemoryReaderWithPtr Reader(LoadedData);
	Reader.SetIsPersistent(true);
	Reader.SetWantBinaryPropertySerialization(true);

	Reader << *this;
                                                                                                     
	if (Reader.IsError() || Reader.Tell() != Reader.TotalSize())
	{
		BuildIdentifier = -1;
		UE_LOG(Angelscript, Warning, TEXT("ASR-PRECOMPILED-INVALID-DATA: incomplete or malformed cache"));
	}
                                                                                                   

                                                                                                    
	if (IsValidForCurrentBuild() && IsAngelscriptExtensionStageRegistered(EAngelscriptExtensionStage::TypeGeneration))
	{
		TArray<FAngelscriptCachedTypeMetadata> CachedTypes;
		TArray<FString> NativeTypeNames;
		for (const auto& Module : Modules)
			for (const auto& Class : Module.Value.Classes)
			{
				if (Class.MetaSpec.Num() != Class.MetaValues.Num())
				{
					BuildIdentifier = -1;
					UE_LOG(Angelscript, Error, TEXT("ASR-PRECOMPILED-EXTENSION-SCHEMA: incomplete class metadata"));
					fprintf(stderr, "ASR-PRECOMPILED-EXTENSION-SCHEMA: incomplete class metadata; regenerate the script cache.\n");
					fflush(stderr);
					FPlatformMisc::RequestExitWithStatus(true, 1);
					return;
				}
				auto& Cached = CachedTypes.AddDefaulted_GetRef();
				Cached.ModuleName = Module.Key;
				Cached.ClassName = Class.ClassName.UnrealString();
				for (int32 Index = 0; Index < Class.MetaSpec.Num(); ++Index)
					Cached.Metadata.Add(FName(*Class.MetaSpec[Index].UnrealString()), Class.MetaValues[Index].UnrealString());
			}
		for (const auto& Reference : TypeReferences)
			if (Reference.Value.Module.Len() == 0) NativeTypeNames.Add(Reference.Value.Name.UnrealString());
		FAngelscriptTypeGenerationContext Context;
		Context.Point = EAngelscriptTypeGenerationPoint::RestoreCachedTypes;
		Context.CachedTypes = &CachedTypes;
		Context.CachedNativeTypeNames = &NativeTypeNames;
		FString Error;
		if (!DispatchAngelscriptTypeGeneration(Context, Error))
		{
			BuildIdentifier = -1;
			UE_LOG(Angelscript, Error, TEXT("ASR-PRECOMPILED-EXTENSION-SCHEMA: %s"), *Error);
			fprintf(stderr, "ASR-PRECOMPILED-EXTENSION-SCHEMA: %s\n", TCHAR_TO_UTF8(*Error));
			fflush(stderr);
			// Stop before any cached native references are resolved. A schema error
			// is a startup rejection, not an assertion or a crash-reporting path.
			FPlatformMisc::RequestExitWithStatus(true, 1);
		}
	}
                                                                                                  

                                                                                                       
	// Post-deserialize inverse permutation: restore the original opcodes so
	// the interpreter always sees the stock instruction stream. Runs only when
	// a permuted policy was applied. Invalid or incomplete archive data must
	// never be walked.
	if (!Reader.IsError() && Reader.Tell() == Reader.TotalSize())
	{
		FAngelscriptModdingPrecompiledDataTransform::FPolicy TransformPolicy;
		if (FAngelscriptModdingPrecompiledDataTransformRuntime::GetPolicy(
				FAngelscriptModdingPrecompiledDataTransform::EDirection::Load, TransformPolicy)
			&& TransformPolicy.bTransform
			&& TransformPolicy.OpcodePermutation.Num() == 256)
		{
			CuvissForEachByteCodeArray(*this, [&TransformPolicy](FAngelscriptPrecompiledModule&, FAngelscriptPrecompiledFunction& Function, bool)
			{
				CuvissApplyOpcodePermutation(Function.ByteCode, TransformPolicy.OpcodePermutation, true);
			});
		}
	}
                                                                                                     
}

uint32 FAngelscriptPrecompiledData::CreateFunctionId(asIScriptFunction* Function)
{
	uint32* Found = ProcessedFunctionToId.Find(Function);
	if (Found != nullptr)
		return *Found;

	uint32 Id = 0;
	if (Function->GetName()[0] == '\0')
	{
		// Functions with an empty name are global property init functions,
		// we don't need to be consistent with Ids for these
		// Rand() only has 15 bits of precision (???), so we do it twice
		Id = ((uint32)FMath::Rand() << 16) | ((uint32)FMath::Rand() & 0xffff);
	}
	else
	{
		// Generate a consistent Guid for the function, to improve iteration times
		auto* ScriptModule = Function->GetModule();
		if (ScriptModule != nullptr)
		{
			Id = HashCombine(Id, FCrc::StrCrc_DEPRECATED((const ANSICHAR*)ScriptModule->GetName()));
			Id = HashCombine(Id, (uint32)(size_t)ScriptModule->GetUserData());
		}

		auto* ObjectType = Function->GetObjectType();
		if (ObjectType != nullptr)
			Id = HashCombine(Id, FCrc::StrCrc_DEPRECATED((const ANSICHAR*)ObjectType->GetEngine()->GetTypeDeclaration(ObjectType->GetTypeId(), true)));

		Id = HashCombine(Id, FCrc::StrCrc_DEPRECATED((const ANSICHAR*)Function->GetDeclaration(true, true)));
	}

	// Make sure the ID is actually unique
	while (ProcessedIdToFunction.Contains(Id))
		++Id;

	ProcessedFunctionToId.Add(Function, Id);
	ProcessedIdToFunction.Add(Id, Function);
	return Id;
}

bool FAngelscriptPrecompiledData::GetIdForFunction(asIScriptFunction* Function, uint32& OutId)
{
	uint32* Found = ProcessedFunctionToId.Find(Function);
	if (Found == nullptr)
		return false;

	OutId = *Found;
	return true;
}

void FAngelscriptPrecompiledData::MapFunctionId(asIScriptFunction* Function, uint32 Id)
{
	ProcessedFunctionToId.Add(Function, Id);

	asIScriptFunction* PreviousFunction = ProcessedIdToFunction.FindRef(Id);
	check(PreviousFunction == nullptr);

	ProcessedIdToFunction.Add(Id, Function);
}

TArray<TSharedRef<FAngelscriptModuleDesc>> FAngelscriptPrecompiledData::GetModulesToCompile()
{
	AS_LLM_SCOPE

	FAngelscriptScopeTotalTimer Timer(TIMER_GetModulesToCompile);

	TArray<TSharedRef<FAngelscriptModuleDesc>> ModuleDescs;
	TMap<FString, TSharedPtr<FAngelscriptModuleDesc>> ModuleDescByName;
	FString ScriptRootDir = FAngelscriptManager::GetScriptRootDirectory();

	struct FDelayCompose
	{
		TSharedPtr<FAngelscriptClassDesc> ClassDesc;
		FStringInArchive ModuleName;
		FStringInArchive ClassName;
	};

	for (auto& Elem : Modules)
	{
		FAngelscriptPrecompiledModule& Module = Elem.Value;

		ModuleDesc = MakeShared<FAngelscriptModuleDesc>();
		ModuleDesc->ModuleName = Module.ModuleName.UnrealString();
                                                                                                    
		ModuleDesc->ScriptUnit.Id = Module.ScriptUnitId.UnrealString();
		ModuleDesc->ScriptUnit.Layer = EAngelscriptScriptLayer(Module.ScriptLayer);
		if (Module.ScriptLayer > uint8(EAngelscriptScriptLayer::Leaf) || ModuleDesc->ScriptUnit.Id.IsEmpty())
			ModuleDesc->ScriptUnit.ConfigurationError = TEXT("Invalid cached script unit metadata");
		for (const auto& Dependency : Module.ScriptUnitDependencies)
			ModuleDesc->ScriptUnit.Dependencies.Add(Dependency.UnrealString());
                                                                                                  
		                                                                                                  
		ModuleDesc->PrecompiledData = &Module;
		ModuleDesc->PrecompiledDataContext = this;
		const bool bIsModCacheModule = ModuleDesc->ModuleName.StartsWith(TEXT("Mods."))
			|| ModuleDesc->ModuleName.StartsWith(TEXT("FeatureMods."))
			|| ModuleDesc->ModuleName.StartsWith(TEXT("Plugins.FeatureMods."))
			|| ModuleDesc->ModuleName.Contains(TEXT(".FeatureMods."))
			|| ModuleDesc->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf;
		if (bIsModCacheModule && Module.ScriptRelativeFilename.Len() != 0)
		{
			FAngelscriptModuleDesc::FCodeSection CachedSourceSection;
			CachedSourceSection.RelativeFilename = Module.ScriptRelativeFilename.UnrealString();
			CachedSourceSection.AbsoluteFilename = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectDir() / CachedSourceSection.RelativeFilename);
			ModuleDesc->Code.Add(MoveTemp(CachedSourceSection));
		}
		                                                                                                

		// * Handle any functions that need to be called post init
		for (FStringInArchive& FunctionStr : Module.PostInitFunctions)
			ModuleDesc->PostInitFunctions.Add(FunctionStr.UnrealString());

		// * Add the statics class for this module
		TSharedPtr<FAngelscriptClassDesc> StaticsClass;
		if (Module.StaticsClassName.Len() != 0)
		{
			StaticsClass = MakeShared<FAngelscriptClassDesc>();
			StaticsClass->ClassName = Module.StaticsClassName.UnrealString();
			StaticsClass->SuperClass = TEXT("UObject");
			StaticsClass->bIsStaticsClass = true;
			StaticsClass->bSuperIsCodeClass = true;
			StaticsClass->CodeSuperClass = UObject::StaticClass();
			StaticsClass->Meta.Add(TEXT("NotBlueprintable"), TEXT("true"));
			StaticsClass->Meta.Add(TEXT("IsBlueprintBase"), TEXT("false"));

			ModuleDesc->Classes.Add(StaticsClass.ToSharedRef());
		}

		// * Add static functions that were in the pre-processor
		for (FAngelscriptPrecompiledFunction& GlobalFunction : Module.Functions)
		{
			auto NewFunction = GlobalFunction.MakeDesc();
			if (NewFunction.IsValid())
				StaticsClass->Methods.Add(NewFunction.ToSharedRef());
		}

		// * Add classes that were in the pre-processor
		for (FAngelscriptPrecompiledClass& Class : Module.Classes)
		{
			if (!Class.bIsInPreprocessor)
				continue;

			ClassDesc = MakeShared<FAngelscriptClassDesc>();
			ClassDesc->ClassName = Class.ClassName.UnrealString();
			ClassDesc->SuperClass = Class.SuperClass.UnrealString();
			ClassDesc->bSuperIsCodeClass = Class.bSuperIsCodeClass;

			if(Class.CodeSuperClass.Len() != 0)
			{
				ClassDesc->CodeSuperClass = FindObject<UClass>(nullptr, ANSI_TO_TCHAR(*Class.CodeSuperClass));
				check(ClassDesc->CodeSuperClass != nullptr);
			}

			ClassDesc->bAbstract = Class.bAbstract;
			ClassDesc->bTransient = Class.bTransient;
			ClassDesc->bHideDropdown = Class.bHideDropdown;
			ClassDesc->bIsDeprecatedClass = Class.bIsDeprecatedClass;
			ClassDesc->ConfigName = Class.ConfigName.UnrealString();
			ClassDesc->bIsStruct = (Class.Flags & asOBJ_VALUE) != 0;
			ClassDesc->StaticClassGlobalVariableName = Class.StaticClassGlobalVariableName.UnrealString();
			if (Class.bPlaceableOverride)
				ClassDesc->bPlaceable = Class.bPlaceable;
			else
				ClassDesc->bPlaceable.Reset();
			ClassDesc->bDefaultToInstanced = Class.bDefaultToInstanced;
			ClassDesc->bEditInlineNew = Class.bEditInlineNew;
			
			if (Class.Namespace.Len() > 0)
			{
				ClassDesc->Namespace = TOptional<FString>(Class.Namespace.UnrealString());
			}
			else
			{
				ClassDesc->Namespace = TOptional<FString>();
			}

			if (Class.ComposeOntoClassName.Len() != 0)
				ClassDesc->ComposeOntoClass = Class.ComposeOntoClassName.UnrealString();

			for (int32 i = 0, MetaCount = Class.MetaSpec.Num(); i < MetaCount; ++i)
			{
				ClassDesc->Meta.Add(
					ANSI_TO_TCHAR(*Class.MetaSpec[i]),
					Class.MetaValues[i].UnrealString()
				);
			}

			// * Add properties into the class
			for (FAngelscriptPrecompiledProperty& Property : Class.Properties)
			{
				if (!Property.bIsUnrealProperty)
					continue;

				PropertyDesc = MakeShared<FAngelscriptPropertyDesc>();
				PropertyDesc->PropertyName = Property.Name.UnrealString();

				for (int32 i = 0, MetaCount = Property.MetaSpec.Num(); i < MetaCount; ++i)
				{
					PropertyDesc->Meta.Add(
						ANSI_TO_TCHAR(*Property.MetaSpec[i]),
						Property.MetaValues[i].UnrealString()
					);
				}

				PropertyDesc->bBlueprintReadable = Property.bBlueprintReadable;
				PropertyDesc->bBlueprintWritable = Property.bBlueprintWritable;
				PropertyDesc->bEditConst = Property.bEditConst;
				PropertyDesc->bEditableOnDefaults = Property.bEditableOnDefaults;
				PropertyDesc->bEditableOnInstance = Property.bEditableOnInstance;
				PropertyDesc->bInstancedReference = Property.bInstancedReference;
				PropertyDesc->bPersistentInstance = Property.bPersistentInstance;
				PropertyDesc->bAdvancedDisplay = Property.bAdvancedDisplay;
				PropertyDesc->bTransient = Property.bTransient;
				PropertyDesc->bReplicated = Property.bReplicated;
				if (Property.bReplicated)
				{
					PropertyDesc->ReplicationCondition = (ELifetimeCondition)Property.ReplicationCondition;
					PropertyDesc->bRepNotify = Property.bRepNotify;
				}
				PropertyDesc->bSkipReplication = Property.bSkipReplication;
				PropertyDesc->bSkipSerialization = Property.bSkipSerialization;
				PropertyDesc->bSaveGame = Property.bSaveGame;
				PropertyDesc->bConfig = Property.bConfig;
				PropertyDesc->bInterp = Property.bInterp;
				PropertyDesc->bAssetRegistrySearchable = Property.bAssetRegistrySearchable;

				ClassDesc->Properties.Add(PropertyDesc.ToSharedRef());
			}

			// * Add functions into the class
			for (FAngelscriptPrecompiledFunction& Function : Class.Methods)
			{
				auto NewFunction = Function.MakeDesc();
				if(NewFunction.IsValid())
					ClassDesc->Methods.Add(NewFunction.ToSharedRef());
			}

			ModuleDesc->Classes.Add(ClassDesc.ToSharedRef());
		}

		// * Add delegate descriptors for this module
		for (FStringInArchive& Event : Module.DeclaredEvents)
		{
			auto DelegateDesc = MakeShared<FAngelscriptDelegateDesc>();
			DelegateDesc->DelegateName = Event.UnrealString();
			DelegateDesc->bIsMulticast = true;
			ModuleDesc->Delegates.Add(DelegateDesc);
		}

		for (FStringInArchive& Delegate : Module.DeclaredDelegates)
		{
			auto DelegateDesc = MakeShared<FAngelscriptDelegateDesc>();
			DelegateDesc->DelegateName = Delegate.UnrealString();
			DelegateDesc->bIsMulticast = false;
			ModuleDesc->Delegates.Add(DelegateDesc);
		}

		ModuleDescs.Add(ModuleDesc.ToSharedRef());
		ModuleDescByName.Add(ModuleDesc->ModuleName, ModuleDesc);
	}

	return ModuleDescs;
}

TSharedPtr<FAngelscriptFunctionDesc> FAngelscriptPrecompiledFunction::MakeDesc() const
{
	if (!bIsUFunction)
		return nullptr;

	auto FunctionDesc = MakeShared<FAngelscriptFunctionDesc>();
	FunctionDesc->FunctionName = UnrealFunctionName.UnrealString();
	FunctionDesc->ScriptFunctionName = FunctionName.UnrealString();

	for (int32 i = 0, MetaCount = MetaSpec.Num(); i < MetaCount; ++i)
	{
		FunctionDesc->Meta.Add(
			ANSI_TO_TCHAR(*MetaSpec[i]),
			MetaValues[i].UnrealString()
		);
	}

	FunctionDesc->bBlueprintCallable = bBlueprintCallable;
	FunctionDesc->bBlueprintOverride = bBlueprintOverride;
                                                                                                                                                 
	if (bBlueprintOverride)
		FunctionDesc->OriginalFunctionName = UnrealFunctionName.UnrealString();
                                                                                                                                               
	FunctionDesc->bBlueprintEvent = bBlueprintEvent;
	FunctionDesc->bBlueprintPure = bBlueprintPure;
	FunctionDesc->bNetFunction = bNetFunction;
	FunctionDesc->bNetMulticast = bNetMulticast;
	FunctionDesc->bNetClient = bNetClient;
	FunctionDesc->bNetServer = bNetServer;
	FunctionDesc->bNetValidate = bNetValidate;
	FunctionDesc->bUnreliable = bUnreliable;
	FunctionDesc->bBlueprintAuthorityOnly = bBlueprintAuthorityOnly;
	FunctionDesc->bExec = bExec;
	FunctionDesc->bCanOverrideEvent = bCanOverrideEvent;
	FunctionDesc->bDevFunction = bDevFunction;
	FunctionDesc->bIsStatic = bIsStatic;
	FunctionDesc->bIsConstMethod = bIsConstMethod;
	FunctionDesc->bThreadSafe = bThreadSafe;
	FunctionDesc->bIsNoOp = bIsNoOp;

	return FunctionDesc;
}

void FAngelscriptPrecompiledData::OutputTimingData()
{
	FAngelscriptScopeTimer::OutputTime(TEXT("ProcessBytecode"), TIMER_ProcessBytecode);
	FAngelscriptScopeTimer::OutputTime(TEXT("CreateFunction"), TIMER_CreateFunction);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- LoadBytecode"), TIMER_LoadBytecode);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- CreateParameters"), TIMER_CreateParameters);
	FAngelscriptScopeTimer::OutputTime(TEXT("CreateFunctionSignature"), TIMER_CreateFunctionSignature);
	FAngelscriptScopeTimer::OutputTime(TEXT("CreateClass"), TIMER_CreateClass);
	FAngelscriptScopeTimer::OutputTime(TEXT("ProcessProperties"), TIMER_ProcessProperties);
	FAngelscriptScopeTimer::OutputTime(TEXT("CreateFunctions"), TIMER_CreateFunctions);
	FAngelscriptScopeTimer::OutputTime(TEXT("PreProcessFunctions"), TIMER_PreProcessFunctions);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- AppendProperties"), TIMER_AppendProperties);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- VirtualTableLookup"), TIMER_VirtualTableLookup);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- AppendShadowMethods"), TIMER_AppendShadowMethods);
	FAngelscriptScopeTimer::OutputTime(TEXT("ProcessFunctions"), TIMER_ProcessFunctions);
	FAngelscriptScopeTimer::OutputTime(TEXT("TypeInfoLookup"), TIMER_TypeInfoLookup);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- GetNamespace"), TIMER_GetNamespace);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- FindModuleForType"), TIMER_FindModuleForType);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- FindTypeInModule"), TIMER_FindTypeInModule);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- FindTypeInEngine"), TIMER_FindTypeInEngine);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- FindHardcodedType"), TIMER_FindHardcodedType);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- FindTemplateInstance"), TIMER_FindTemplateInstance);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- FindTemplateSubType"), TIMER_FindTemplateSubType);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- UpdateReferenceMap"), TIMER_UpdateReferenceMap);
	FAngelscriptScopeTimer::OutputTime(TEXT(" -- TypeReferenceFind"), TIMER_TypeReferenceFind);
	FAngelscriptScopeTimer::OutputTime(TEXT("FunctionLookup"), TIMER_FunctionLookup);
	FAngelscriptScopeTimer::OutputTime(TEXT("GlobalLookup"), TIMER_GlobalLookup);
	FAngelscriptScopeTimer::OutputTime(TEXT("PropertyLookup"), TIMER_PropertyLookup);
	FAngelscriptScopeTimer::OutputTime(TEXT("GetModulesToCompile"), TIMER_GetModulesToCompile);
}
