#include "StaticJIT/StaticJITHeader.h"
#include "AngelscriptStaticJIT.h"

#include "StartAngelscriptHeaders.h"
#include "as_context.h"
#include "as_scriptengine.h"
#include "as_scriptobject.h"
#include "as_callfunc.h"
#include "as_generic.h"
#include "as_texts.h"
#include "EndAngelscriptHeaders.h"

static const FStaticJITCompiledInfo*& GetActiveCompiledInfo()
{
	static const FStaticJITCompiledInfo* ActiveInfo = nullptr;
	return ActiveInfo;
}

FStaticJITCompiledInfo::FStaticJITCompiledInfo(FGuid Guid)
	: PrecompiledDataGuid(Guid)
{
	const FStaticJITCompiledInfo*& ActiveInfo = GetActiveCompiledInfo();
	checkf(ActiveInfo == nullptr, TEXT("Only one angelscript static JIT info can be compiled in!"))
	ActiveInfo = this;
}

                                                                                                  
FStaticJITCompiledInfo::FStaticJITCompiledInfo(
	FGuid Guid,
	const FStaticJITModuleDataGuid* InModuleDataGuids,
	uint32 InModuleDataGuidCount)
	: PrecompiledDataGuid(Guid)
	, ModuleDataGuids(InModuleDataGuids)
	, ModuleDataGuidCount(InModuleDataGuidCount)
{
	const FStaticJITCompiledInfo*& ActiveInfo = GetActiveCompiledInfo();
	checkf(ActiveInfo == nullptr, TEXT("Only one angelscript static JIT info can be compiled in!"))
	ActiveInfo = this;
}

bool FStaticJITCompiledInfo::GetModuleDataGuid(const FString& ModuleName, FGuid& OutGuid) const
{
	for (uint32 Index = 0; Index < ModuleDataGuidCount; ++Index)
	{
		if (ModuleDataGuids[Index].ModuleName != nullptr
			&& ModuleName == ModuleDataGuids[Index].ModuleName)
		{
			OutGuid = ModuleDataGuids[Index].DataGuid;
			return true;
		}
	}
	return false;
}
                                                                                                

const FStaticJITCompiledInfo* FStaticJITCompiledInfo::Get()
{
	return GetActiveCompiledInfo();
}

void FStaticJITFunction::Register(uint32 FunctionId, asJITFunction InVMEntry, asJITFunction_ParmsEntry InParmsEntry, asJITFunction_Raw InRawFunction)
{
	FJITDatabase::FJITFunctions Funcs;
	Funcs.VMEntry = InVMEntry;
	Funcs.ParmsEntry = InParmsEntry;
	Funcs.RawFunction = InRawFunction;

	auto& JITDatabase = FJITDatabase::Get();
	JITDatabase.Functions.Add(FunctionId, Funcs);
}

void FStaticJITFunction::Register(uint32 FunctionId, asJITFunction InVMEntry, asJITFunction_Raw InRawFunction)
{
	FJITDatabase::FJITFunctions Funcs;
	Funcs.VMEntry = InVMEntry;
	Funcs.ParmsEntry = nullptr;
	Funcs.RawFunction = InRawFunction;

	auto& JITDatabase = FJITDatabase::Get();
	JITDatabase.Functions.Add(FunctionId, Funcs);
}

void FJitRef_Function::Register(FJitRef_Function* FunctionRef)
{
	auto& JITDatabase = FJITDatabase::Get();
	JITDatabase.FunctionLookups.Add((void**)&FunctionRef->Pointer);
}

void FJitRef_SystemFunctionPointer::Register(FJitRef_SystemFunctionPointer* Ref)
{
	auto& JITDatabase = FJITDatabase::Get();
	JITDatabase.SystemFunctionPointerLookups.Add(Ref);
}

void FJitRef_Type::Register(FJitRef_Type* Ref)
{
	auto& JITDatabase = FJITDatabase::Get();
	JITDatabase.TypeInfoLookups.Add((void**)&Ref->Pointer);
}

void FJitRef_GlobalVar::Register(FJitRef_GlobalVar* Ref)
{
	auto& JITDatabase = FJITDatabase::Get();
	JITDatabase.GlobalVarLookups.Add((void**)&Ref->Pointer);
}

void FJitRef_PropertyOffset::Register(FJitRef_PropertyOffset* Ref, uint64 PropertyRef)
{
	Ref->Offset = 0xffff;

	auto& JITDatabase = FJITDatabase::Get();
	JITDatabase.PropertyOffsetLookups.Add(TPair<uint64,uint32*>(PropertyRef, &Ref->Offset));
}

#if AS_JIT_VERIFY_PROPERTY_OFFSETS
FJitVerifyPropertyOffset::FJitVerifyPropertyOffset(uint64 PropertyRef, SIZE_T ComputedOffset)
{
	auto& JITDatabase = FJITDatabase::Get();
	JITDatabase.VerifyPropertyOffsets.Add(TPair<uint64,SIZE_T>(PropertyRef, ComputedOffset));
}

FJitVerifyTypeSize::FJitVerifyTypeSize(uint64 TypeRef, SIZE_T ComputedSize, SIZE_T ComputedAlignment)
{
	auto& JITDatabase = FJITDatabase::Get();
	JITDatabase.VerifyTypeSizes.Add(TPair<uint64,SIZE_T>(TypeRef, ComputedSize));
	JITDatabase.VerifyTypeAlignments.Add(TPair<uint64,SIZE_T>(TypeRef, ComputedAlignment));
}
#endif

void FStaticJITFunction::SetException(FScriptExecution& Execution, EJITException Exception)
{
	Execution.bExceptionThrown = true;

	if (Exception == EJITException::NullPointer)
		FAngelscriptManager::HandleExceptionFromJIT(TXT_NULL_POINTER_ACCESS);
	else if (Exception == EJITException::Div0)
		FAngelscriptManager::HandleExceptionFromJIT(TXT_DIVIDE_BY_ZERO);
	else if (Exception == EJITException::Overflow)
		FAngelscriptManager::HandleExceptionFromJIT(TXT_DIVIDE_OVERFLOW);
	else if (Exception == EJITException::UnboundFunction)
		FAngelscriptManager::HandleExceptionFromJIT(TXT_UNBOUND_FUNCTION);
	else if (Exception == EJITException::OutOfBounds)
		FAngelscriptManager::HandleExceptionFromJIT("Index out of bounds.");
	else
		FAngelscriptManager::HandleExceptionFromJIT("Unknown exception.");
}

void FStaticJITFunction::SetNullPointerException(FScriptExecution& Execution)
{
	Execution.bExceptionThrown = true;
	FAngelscriptManager::HandleExceptionFromJIT(TXT_NULL_POINTER_ACCESS);
}

void FStaticJITFunction::SetDivByZeroException(FScriptExecution& Execution)
{
	Execution.bExceptionThrown = true;
	FAngelscriptManager::HandleExceptionFromJIT(TXT_DIVIDE_BY_ZERO);
}

void FStaticJITFunction::SetOverflowException(FScriptExecution& Execution)
{
	Execution.bExceptionThrown = true;
	FAngelscriptManager::HandleExceptionFromJIT(TXT_DIVIDE_OVERFLOW);
}

void FStaticJITFunction::SetUnboundFunctionException(FScriptExecution& Execution)
{
	Execution.bExceptionThrown = true;
	FAngelscriptManager::HandleExceptionFromJIT(TXT_UNBOUND_FUNCTION);
}

void FStaticJITFunction::SetOutOfBoundsException(FScriptExecution& Execution)
{
	Execution.bExceptionThrown = true;
	FAngelscriptManager::HandleExceptionFromJIT("Index out of bounds.");
}

void FStaticJITFunction::SetSwitchValueInvalidException(FScriptExecution& Execution)
{
	Execution.bExceptionThrown = true;
	FAngelscriptManager::HandleExceptionFromJIT("Invalid enum value passed to switch");
}

void FStaticJITFunction::SetUnknownException(FScriptExecution& Execution)
{
	Execution.bExceptionThrown = true;
	FAngelscriptManager::HandleExceptionFromJIT("Unknown exception");
}

void FStaticJITFunction::ScriptFinishConstruct(FScriptExecution& Execution, asIScriptObject* Object, asITypeInfo* TypeInfo)
{
	SCRIPT_ENGINE->FinishConstructObject(Object, TypeInfo);
}

void FStaticJITFunction::ScriptCallNative(FScriptExecution& Execution, asCScriptFunction* Function, asBYTE* l_sp, asQWORD* valueRegister, void** objectRegister)
{
	asCScriptFunction* descr = Function;
	asSSystemFunctionInterface *sysFunc = descr->sysFuncIntf;

	check(sysFunc->caller.IsBound());

	asDWORD* StackArgs = (asDWORD*)l_sp;

	void* FunctionArgs[32];
	void* ReturnAddress;
	unsigned int ArgIndex = 0;

	// Verify the object pointer if it is a class method
	void *currentObject = 0;
	if( sysFunc->callConv >= ICC_THISCALL )
	{
		// The object pointer should be popped from the context stack
		FunctionArgs[ArgIndex] = *(void**)&StackArgs[0];

		// Skip object pointer
		ArgIndex++;
	}

	// Some system functions want to know the script function that is being called
	if (sysFunc->passFirstParamMetaData != asEFirstParamMetaData::None)
	{
		if (sysFunc->passFirstParamMetaData == asEFirstParamMetaData::ScriptFunction)
		{
			FunctionArgs[ArgIndex] = descr;
			ArgIndex++;
		}
		else
		{
			FunctionArgs[ArgIndex] = descr->objectType;
			ArgIndex++;
		}
	}

	if( descr->DoesReturnOnStack() )
	{
		// Skip the address where the return value will be stored
		if (sysFunc->callConv >= ICC_THISCALL)
			ReturnAddress = *(void**)(StackArgs + AS_PTR_SIZE);
		else
			ReturnAddress = *(void**)StackArgs;

		// TODO: Do we need to destroy the object here in case of exception?
	}
	else if (descr->returnType.IsObjectHandle() && !descr->returnType.IsReference())
	{
		ReturnAddress = objectRegister;
	}
	else
	{
		ReturnAddress = valueRegister;
	}

	for (int i = 0, paramCount = descr->parameterOffsets.GetLength(); i < paramCount; ++i)
	{
		auto& paramType = descr->parameterTypes[i];
		auto paramOffset = descr->parameterOffsets[i];

		if (paramType.GetTokenType() == ttQuestion)
		{
			FunctionArgs[ArgIndex] = *(void**)&StackArgs[paramOffset];
			++ArgIndex;
			FunctionArgs[ArgIndex] = &StackArgs[paramOffset+2];
		}
		else if (paramType.IsObject() || paramType.IsReference())
		{
			FunctionArgs[ArgIndex] = *(void**)&StackArgs[paramOffset];
		}
		else
		{
			FunctionArgs[ArgIndex] = &StackArgs[paramOffset];
		}

		++ArgIndex;
	}

	auto* tld = Execution.tld;
	auto* prevActiveFunction = tld->activeFunction;
	tld->activeFunction = descr;

	if (sysFunc->caller.type == 1)
		sysFunc->caller.FunctionCaller(sysFunc->func, &FunctionArgs[0], ReturnAddress);
	else //if (sysFunc->caller.type == 2)
		sysFunc->caller.MethodCaller(sysFunc->method, &FunctionArgs[0], ReturnAddress);

	tld->activeFunction = prevActiveFunction;
}

void FStaticJITFunction::ScriptCallGeneric(FScriptExecution& Execution, asCScriptFunction* Function, asBYTE* l_sp, asQWORD* valueRegister, void** objectRegister)
{
	asCScriptFunction* descr = Function;
	asSSystemFunctionInterface *sysFunc = descr->sysFuncIntf;

	void (*func)(asIScriptGeneric*) = (void (*)(asIScriptGeneric*))sysFunc->func;
	asDWORD *args = (asDWORD*)l_sp;

	// Verify the object pointer if it is a class method
	void *currentObject = 0;
	check( sysFunc->callConv == ICC_GENERIC_FUNC || sysFunc->callConv == ICC_GENERIC_METHOD );
	if( sysFunc->callConv == ICC_GENERIC_METHOD )
	{
		// The object pointer should be popped from the context stack
		// Check for null pointer
		currentObject = (void*)*(asPWORD*)(args);
		if( currentObject == 0 )
		{
			FStaticJITFunction::SetException(Execution, EJITException::NullPointer);
			return;
		}

		check( sysFunc->baseOffset == 0 );
	}

	asCGeneric gen(SCRIPT_ENGINE, descr, currentObject, args);

	auto* tld = Execution.tld;
	auto* prevActiveFunction = tld->activeFunction;
	tld->activeFunction = descr;

	func(&gen);

	tld->activeFunction = prevActiveFunction;

	*valueRegister = gen.returnVal;
	*objectRegister = (void*)gen.objectRegister;
}
