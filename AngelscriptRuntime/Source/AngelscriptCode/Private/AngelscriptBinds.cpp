#include "AngelscriptBinds.h"

#include "AngelscriptManager.h"
#include "AngelscriptSettings.h"

#include "AngelscriptInclude.h"
#include "AngelscriptSettings.h"
#include "as_property.h"
#include "as_scriptfunction.h"

#if WITH_EDITOR
#include "Misc/Paths.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_LINUX || PLATFORM_MAC
#include <dlfcn.h>
#endif
#endif

#include "StartAngelscriptHeaders.h"
#include "as_scriptfunction.h"
#include "as_objecttype.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

struct FBindFunction
{
	int32 BindOrder;
	TFunction<void()> Function;
	#if WITH_EDITOR
	FString RegistrationModule;
	#endif

	bool operator<(const FBindFunction& Other) const
	{
		return BindOrder < Other.BindOrder;
	}
};

static TArray<FBindFunction>& GetBindArray()
{
	static TArray<FBindFunction> BindArray;
	return BindArray;
}

#if WITH_EDITOR
namespace
{
	FString CurrentRegistrationModule;
	TMap<const void*, FString>& GetRegistrationModuleNames()
	{
		static TMap<const void*, FString> NamesByModule;
		return NamesByModule;
	}
	TMap<int32, FString>& GetFunctionRegistrationModules()
	{
		static TMap<int32, FString> ModulesById;
		return ModulesById;
	}
	TMap<int32, FString>& GetTypeRegistrationModules()
	{
		static TMap<int32, FString> ModulesById;
		return ModulesById;
	}
	TMap<int32, FString>& GetGlobalPropertyRegistrationModules()
	{
		static TMap<int32, FString> ModulesById;
		return ModulesById;
	}
	TMap<FString, FString>& GetPropertyRegistrationModules()
	{
		static TMap<FString, FString> ModulesByKey;
		return ModulesByKey;
	}
	FString MakePropertyRegistrationKey(int32 TypeId, const FString& PropertyName)
	{
		return FString::Printf(TEXT("%d:%d:%s"), TypeId, PropertyName.Len(), *PropertyName);
	}
	void RecordPropertyRegistrationModule(FBindString ClassName, FBindString Signature)
	{
		if (CurrentRegistrationModule.IsEmpty())
		{
			return;
		}
		auto& Manager = FAngelscriptManager::Get();
		asITypeInfo* Type = Manager.Engine->GetTypeInfoByName(ClassName.ToCString());
		if (Type == nullptr)
		{
			return;
		}
		FString Declaration = UTF8_TO_TCHAR(Signature.ToCString());
		Declaration.TrimStartAndEndInline();
		int32 Separator = INDEX_NONE;
		if (!Declaration.FindLastChar(TEXT(' '), Separator) || Separator == INDEX_NONE)
		{
			return;
		}
		FString PropertyName = Declaration.Mid(Separator + 1).TrimStartAndEnd();
		if (PropertyName.IsEmpty())
		{
			return;
		}
		GetPropertyRegistrationModules().Add(MakePropertyRegistrationKey(Type->GetTypeId(), PropertyName), CurrentRegistrationModule);
	}

	FString NormalizeRegistrationModuleName(const FString& ModulePath)
	{
		FString Name = FPaths::GetBaseFilename(ModulePath);
		Name.RemoveFromStart(TEXT("lib"), ESearchCase::IgnoreCase);
		for (const TCHAR* Prefix : {TEXT("UnrealEditor-"), TEXT("UE4Editor-"), TEXT("UE5Editor-")})
		{
			if (Name.RemoveFromStart(Prefix, ESearchCase::CaseSensitive))
			{
				break;
			}
		}
		return Name;
	}

	FString ResolveRegistrationModuleName(const void* Address)
	{
		if (Address == nullptr)
		{
			return FString();
		}
		const void* ModuleKey = nullptr;
		FString ModulePath;
#if PLATFORM_WINDOWS
		HMODULE ModuleHandle = nullptr;
		const DWORD Flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
		if (!GetModuleHandleEx(Flags, reinterpret_cast<LPCTSTR>(Address), &ModuleHandle))
		{
			return FString();
		}
		TCHAR Path[MAX_PATH] = {};
		const DWORD PathLength = GetModuleFileName(ModuleHandle, Path, UE_ARRAY_COUNT(Path));
		if (PathLength == 0 || PathLength >= UE_ARRAY_COUNT(Path))
		{
			return FString();
		}
		ModuleKey = ModuleHandle;
		ModulePath = Path;
#elif PLATFORM_LINUX || PLATFORM_MAC
		Dl_info Info = {};
		if (dladdr(Address, &Info) == 0 || Info.dli_fname == nullptr || Info.dli_fbase == nullptr)
		{
			return FString();
		}
		ModuleKey = Info.dli_fbase;
		ModulePath = UTF8_TO_TCHAR(Info.dli_fname);
#else
		return FString();
#endif
		TMap<const void*, FString>& CachedNames = GetRegistrationModuleNames();
		if (const FString* Cached = CachedNames.Find(ModuleKey))
		{
			return *Cached;
		}
		FString Name = NormalizeRegistrationModuleName(ModulePath);
		if (!Name.IsEmpty())
		{
			CachedNames.Add(ModuleKey, Name);
		}
		return Name;
	}

	void RecordRegistrationModule(FAngelscriptBinds::ERegistrationObjectKind Kind, int32 ObjectId)
	{
		if (CurrentRegistrationModule.IsEmpty() || ObjectId < 0)
		{
			return;
		}
		TMap<int32, FString>* ModulesById = nullptr;
		switch (Kind)
		{
		case FAngelscriptBinds::ERegistrationObjectKind::Function:
			ModulesById = &GetFunctionRegistrationModules();
			break;
		case FAngelscriptBinds::ERegistrationObjectKind::Type:
			ModulesById = &GetTypeRegistrationModules();
			break;
		case FAngelscriptBinds::ERegistrationObjectKind::GlobalProperty:
			ModulesById = &GetGlobalPropertyRegistrationModules();
			break;
		case FAngelscriptBinds::ERegistrationObjectKind::Property:
			break;
		}
		if (ModulesById != nullptr)
		{
			ModulesById->Add(ObjectId, CurrentRegistrationModule);
		}
	}
}

bool FAngelscriptBinds::TryGetRegistrationModule(
	ERegistrationObjectKind Kind,
	int32 ObjectId,
	FString& OutModule,
	const FString& ObjectName)
{
	OutModule.Reset();
	if (Kind == ERegistrationObjectKind::Property)
	{
		const FString* Module = GetPropertyRegistrationModules().Find(MakePropertyRegistrationKey(ObjectId, ObjectName));
		if (Module == nullptr)
		{
			return false;
		}
		OutModule = *Module;
		return true;
	}
	const TMap<int32, FString>* ModulesById = nullptr;
	switch (Kind)
	{
	case ERegistrationObjectKind::Function:
		ModulesById = &GetFunctionRegistrationModules();
		break;
	case ERegistrationObjectKind::Type:
		ModulesById = &GetTypeRegistrationModules();
		break;
	case ERegistrationObjectKind::GlobalProperty:
		ModulesById = &GetGlobalPropertyRegistrationModules();
		break;
	case ERegistrationObjectKind::Property:
		break;
	}
	const FString* Module = ModulesById != nullptr ? ModulesById->Find(ObjectId) : nullptr;
	if (Module == nullptr)
	{
		return false;
	}
	OutModule = *Module;
	return true;
}

void FAngelscriptBinds::RegisterBinds(int32 BindOrder, TFunction<void()> Function, const void* RegistrationObjectAddress)
{
	GetBindArray().Add({BindOrder, Function, ResolveRegistrationModuleName(RegistrationObjectAddress)});
}
#else
void FAngelscriptBinds::RegisterBinds(int32 BindOrder, TFunction<void()> Function)
{
	GetBindArray().Add({BindOrder, Function});
}
#endif

void FAngelscriptBinds::CallBinds()
{
	GetBindArray().Sort();
	for (auto& Function : GetBindArray())
	{
		#if WITH_EDITOR
		CurrentRegistrationModule = Function.RegistrationModule;
		#endif
		Function.Function();
	}
	#if WITH_EDITOR
	CurrentRegistrationModule.Reset();
	#endif
}

FAngelscriptBinds FAngelscriptBinds::ReferenceClass(FBindString Name, UClass* UnrealClass)
{
	auto Binds = FAngelscriptBinds(Name, asOBJ_REF | asOBJ_NOCOUNT | asOBJ_IMPLICIT_HANDLE, 0);
	((asCObjectType*)Binds.ScriptType)->size = UnrealClass->GetStructureSize();
	Binds.ScriptType->alignment = UnrealClass->GetMinAlignment();
	return Binds;
}

FAngelscriptBinds FAngelscriptBinds::ExistingClass(FBindString Name)
{
	return FAngelscriptBinds(Name);
}

FAngelscriptBinds FAngelscriptBinds::ValueClass(FBindString Name, FBindFlags Flags, int32 Size)
{
	auto asFlags = asOBJ_VALUE | asOBJ_APP_CLASS | Flags.ExtraFlags;
	if (Flags.bPOD)
		asFlags |= asOBJ_POD;
	if (Flags.bTemplate)
		asFlags |= asOBJ_TEMPLATE;

	auto Binds = FAngelscriptBinds(Name, asFlags, Size);
	if (Flags.bTemplate && !Flags.TemplateType.IsEmpty())
	{
		auto& Manager = FAngelscriptManager::Get();
		int32 TemplatePos = Binds.ClassName.ToFString().Find(TEXT("<"));
		Binds.ClassName = Binds.ClassName.ToFString().Left(TemplatePos);
		Binds.ScriptType = Manager.Engine->GetTypeInfoByName(Binds.ClassName.ToCString());
		Binds.ClassName = Binds.ClassName.ToFString() + Flags.TemplateType.ToFString();
	}

	if (Flags.Alignment != -1)
	{
		check(Binds.ScriptType->alignment == 8 || Binds.ScriptType->alignment == Flags.Alignment);
		Binds.ScriptType->alignment = Flags.Alignment;
	}
	return Binds;
}

FAngelscriptBinds::FAngelscriptBinds(FBindString Name, int32 Flags, int32 Size)
	: ClassName(Name)
{
	auto& Manager = FAngelscriptManager::Get();
	int TypeId = Manager.Engine->RegisterObjectType(ClassName.ToCString(), Size, Flags);

	if (TypeId == asALREADY_REGISTERED)
	{
		ScriptType = Manager.Engine->GetTypeInfoByName(ClassName.ToCString());
		check(ScriptType != nullptr);
		check(ScriptType->GetSize() == Size);
	}
	else
	{
		ScriptType = Manager.Engine->GetTypeInfoById(TypeId);
		#if WITH_EDITOR
		if (ScriptType != nullptr)
		{
			RecordRegistrationModule(ERegistrationObjectKind::Type, ScriptType->GetTypeId());
		}
		#endif
	}

	ensure(ScriptType != nullptr || ((Flags & asOBJ_TEMPLATE) != 0));
}

FAngelscriptBinds::FAngelscriptBinds(FBindString Name)
	: ClassName(Name)
{
}

void FAngelscriptBinds::GenericMethod(FBindString Signature, void(CDECL *Fun)(asIScriptGeneric*), void* UserData)
{
	auto& Manager = FAngelscriptManager::Get();
	int FunctionId = Manager.Engine->RegisterObjectMethod(ClassName.ToCString(), Signature.ToCString(), asFUNCTION(Fun), asCALL_GENERIC, nullptr);
	OnBind(FunctionId, UserData, nullptr);
}

void FAngelscriptBinds::BindBehaviour(asEBehaviours Beh, FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData)
{
	auto& Manager = FAngelscriptManager::Get();
	int FunctionId = Manager.Engine->RegisterObjectBehaviour(ClassName.ToCString(), Beh, Signature.ToCString(),
		FunctionPointers.FunctionPointer,
		FunctionPointers.GetCallConv(),
		FunctionPointers.FunctionCaller);
	OnBind(FunctionId, UserData, nullptr);
}

int FAngelscriptBinds::PreviouslyBoundFunction = -1;
int FAngelscriptBinds::PreviouslyBoundGlobalProperty = -1;
int FAngelscriptBinds::PreviouslyBoundPropertyOffset = -1;
asIScriptFunction* FAngelscriptBinds::GetPreviousBind()
{
	if (PreviouslyBoundFunction == -1)
		return nullptr;

	auto& Manager = FAngelscriptManager::Get();
	return Manager.Engine->GetFunctionById(PreviouslyBoundFunction);
}

void FAngelscriptBinds::MarkAsImplicitConstructor()
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->traits.SetTrait(asEFuncTrait::asTRAIT_IMPLICITCONSTRUCTOR, true);
		if (auto* ObjectType = Function->objectType)
			ObjectType->hasImplicitConstructors = true;
	}
}

void FAngelscriptBinds::DeprecatePreviousBind(const ANSICHAR* DeprecationMessage)
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->traits.SetTrait(asEFuncTrait::asTRAIT_DEPRECATED, true);
#if WITH_EDITOR
		Function->deprecationMessage = DeprecationMessage;
#endif
	}
}

void FAngelscriptBinds::SetPreviousBindIsPropertyAccessor(bool bIsProperty)
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->traits.SetTrait(asEFuncTrait::asTRAIT_PROPERTY, bIsProperty);
	}
}

void FAngelscriptBinds::SetPreviousBindIsGeneratedAccessor(bool bIsAccessor)
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->traits.SetTrait(asEFuncTrait::asTRAIT_GENERATED_FUNCTION, bIsAccessor);
	}
}

void FAngelscriptBinds::SetPreviousBindUnsafeDuringConstruction(bool bUnsafe)
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->traits.SetTrait(asEFuncTrait::asTRAIT_UNSAFE_DURING_CONSTRUCTION, bUnsafe);
	}
}

void FAngelscriptBinds::SetPreviousBindIsEditorOnly(bool bEditorOnly)
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->traits.SetTrait(asEFuncTrait::asTRAIT_EDITOR_ONLY, bEditorOnly);
	}
}

void FAngelscriptBinds::SetPreviousBindRequiresWorldContext(bool bRequiresWorldContext)
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->traits.SetTrait(asEFuncTrait::asTRAIT_USES_WORLDCONTEXT, bRequiresWorldContext);
	}
}

void FAngelscriptBinds::SetPreviousBindIsCallable(bool bIsCallable)
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->traits.SetTrait(asEFuncTrait::asTRAIT_NOT_CALLABLE, !bIsCallable);
	}
}

void FAngelscriptBinds::SetPreviousBindNoDiscard(bool bNoDiscard)
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->traits.SetTrait(asEFuncTrait::asTRAIT_NODISCARD, bNoDiscard);
	}
}

void FAngelscriptBinds::SetPreviousBindArgumentDeterminesOutputType(int ArgumentIndex)
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->determinesOutputTypeArgumentIndex = ArgumentIndex;
	}
}

void FAngelscriptBinds::SetPreviousBindForceConstArgumentExpressions(bool bForceConst)
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->traits.SetTrait(asEFuncTrait::asTRAIT_FORCE_CONST_ARGUMENT_EXPRESSIONS, bForceConst);
	}
}

void FAngelscriptBinds::PreviousBindPassScriptFunctionAsFirstParam()
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->sysFuncIntf->passFirstParamMetaData = asEFirstParamMetaData::ScriptFunction;
	}
}

void FAngelscriptBinds::PreviousBindPassScriptObjectTypeAsFirstParam()
{
	if (auto* Function = (asCScriptFunction*)GetPreviousBind())
	{
		Function->sysFuncIntf->passFirstParamMetaData = asEFirstParamMetaData::ScriptObjectType;
	}
}

void FAngelscriptBinds::OnBind(int FunctionId, void* UserData, const FAngelscriptType::FBindParams* BindParams)
{
	#if WITH_EDITOR
	RecordRegistrationModule(ERegistrationObjectKind::Function, FunctionId);
	#endif
	auto& Manager = FAngelscriptManager::Get();
	asCScriptFunction* ScriptFunction = (asCScriptFunction*)Manager.Engine->GetFunctionById(FunctionId);
	if (ScriptFunction != nullptr)
	{
		if (UserData != nullptr)
		{
			ScriptFunction->SetUserData(UserData, 0);
		}

		if (BindParams != nullptr && BindParams->bProtected)
		{
			ScriptFunction->SetProtected(true);
		}

		// All C++ bound functions can implicitly be treated as property accessors
		if (Manager.ConfigSettings->bAllowImplicitPropertyAccessors)
		{
			ScriptFunction->SetProperty(true);
		}
	}

	PreviouslyBoundFunction = FunctionId;
}

void FAngelscriptBinds::BindStaticBehaviour(asEBehaviours Beh, FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData)
{
	auto& Manager = FAngelscriptManager::Get();
	int FunctionId = Manager.Engine->RegisterObjectBehaviour(ClassName.ToCString(), Beh, Signature.ToCString(), FunctionPointers.FunctionPointer, asCALL_CDECL, FunctionPointers.FunctionCaller);
	OnBind(FunctionId, UserData, nullptr);
}

int FAngelscriptBinds::BindMethod(FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData)
{
	auto& Manager = FAngelscriptManager::Get();
	int FunctionId = Manager.Engine->RegisterObjectMethod(ClassName.ToCString(), Signature.ToCString(), FunctionPointers.FunctionPointer, FunctionPointers.GetCallConv(), FunctionPointers.FunctionCaller, nullptr);
	OnBind(FunctionId, UserData, nullptr);
	return FunctionId;
}

int FAngelscriptBinds::BindMethod(FBindString Signature, FASBindFunctionPointers FunctionPointers, const FAngelscriptType::FBindParams& BindParams, void* UserData)
{
	auto& Manager = FAngelscriptManager::Get();
	const asUINT Access = asCScriptFunction::GenerateExposedType(BindParams.bCanEdit, BindParams.bCanRead, BindParams.bCanWrite);
	int FunctionId = Manager.Engine->RegisterObjectMethod(ClassName.ToCString(), Signature.ToCString(), FunctionPointers.FunctionPointer, FunctionPointers.GetCallConv(), FunctionPointers.FunctionCaller, nullptr, 0, false, Access);
	OnBind(FunctionId, UserData, nullptr);
	return FunctionId;
}

int FAngelscriptBinds::BindMethodDirect(FBindString ClassName, FBindString Signature, asSFuncPtr Function, asECallConvTypes CallConv, FASBindFunctionPointers FunctionPointers, void* UserData)
{
	auto& Manager = FAngelscriptManager::Get();
	int32 FunctionId = Manager.Engine->RegisterObjectMethod(
		ClassName.ToCString(), Signature.ToCString(),
		Function, CallConv, FunctionPointers.FunctionCaller);
	OnBind(FunctionId, UserData, nullptr);
	return FunctionId;
}

int FAngelscriptBinds::BindBehaviourDirect(FBindString ClassName, asEBehaviours Behaviour, FBindString Signature, asSFuncPtr Function, asECallConvTypes CallConv, FASBindFunctionPointers FunctionPointers, void* UserData)
{
	auto& Manager = FAngelscriptManager::Get();
	int FunctionId = Manager.Engine->RegisterObjectBehaviour(
		ClassName.ToCString(), Behaviour, Signature.ToCString(),
		FunctionPointers.FunctionPointer,
		FunctionPointers.GetCallConv(),
		FunctionPointers.FunctionCaller);
	OnBind(FunctionId, UserData, nullptr);
	return FunctionId;
}

int FAngelscriptBinds::CompileOutInTest(int FunctionId)
{
	auto& Manager = FAngelscriptManager::Get();
	auto* Function = (asCScriptFunction*)Manager.Engine->GetFunctionById(FunctionId);

	if (UE_BUILD_TEST || UE_BUILD_SHIPPING || (WITH_EDITOR && FAngelscriptManager::bSimulateCooked))
	{
		Function->compileOutType = asECompileOutType::CompileOutEntirely;
	}

	if (Manager.ConfigSettings->bForceConstWithinDevelopmentOnlyFunctions)
	{
		Function->traits.SetTrait(asTRAIT_FORCE_CONST_ARGUMENT_EXPRESSIONS, true);
	}

	return FunctionId;
}

int FAngelscriptBinds::CompileOutIfNoLog(int FunctionId)
{
	auto& Manager = FAngelscriptManager::Get();
	auto* Function = (asCScriptFunction*)Manager.Engine->GetFunctionById(FunctionId);

	if (((UE_BUILD_TEST || UE_BUILD_SHIPPING) && !USE_LOGGING_IN_SHIPPING) || (WITH_EDITOR && FAngelscriptManager::bSimulateCooked))
	{
		Function->compileOutType = asECompileOutType::CompileOutEntirely;
	}

	if (Manager.ConfigSettings->bForceConstWithinDevelopmentOnlyFunctions)
	{
		Function->traits.SetTrait(asTRAIT_FORCE_CONST_ARGUMENT_EXPRESSIONS, true);
	}

	return FunctionId;
}

int FAngelscriptBinds::CompileOutAsEnsure(int FunctionId)
{
	auto& Manager = FAngelscriptManager::Get();
	auto* Function = (asCScriptFunction*)Manager.Engine->GetFunctionById(FunctionId);
	Function->traits.SetTrait(asTRAIT_NODISCARD, true);

	if (UE_BUILD_SHIPPING || (WITH_EDITOR && FAngelscriptManager::bSimulateCooked))
	{
		Function->compileOutType = asECompileOutType::ReplaceWithFirstParam;
	}
	return FunctionId;
}

int FAngelscriptBinds::CompileOutAsCheck(int FunctionId)
{
	auto& Manager = FAngelscriptManager::Get();
	auto* Function = (asCScriptFunction*)Manager.Engine->GetFunctionById(FunctionId);

	if (UE_BUILD_SHIPPING || (WITH_EDITOR && FAngelscriptManager::bSimulateCooked))
	{
		Function->compileOutType = asECompileOutType::CompileOutEntirely;
	}

	if (Manager.ConfigSettings->bForceConstWithinDevelopmentOnlyFunctions)
	{
		Function->traits.SetTrait(asTRAIT_FORCE_CONST_ARGUMENT_EXPRESSIONS, true);
	}

	return FunctionId;
}

int FAngelscriptBinds::CompileReplaceWithFirstArgInTest(int FunctionId)
{
	if (UE_BUILD_TEST || UE_BUILD_SHIPPING || (WITH_EDITOR && FAngelscriptManager::bSimulateCooked))
	{
		auto& Manager = FAngelscriptManager::Get();
		auto* Function = (asCScriptFunction*)Manager.Engine->GetFunctionById(FunctionId);
		Function->compileOutType = asECompileOutType::ReplaceWithFirstParam;
	}
	return FunctionId;
}

void FAngelscriptBinds::CompileOutPreviousBind()
{
	auto& Manager = FAngelscriptManager::Get();
	auto* Function = (asCScriptFunction*)Manager.Engine->GetFunctionById(PreviouslyBoundFunction);
	Function->compileOutType = asECompileOutType::CompileOutEntirely;
}

void FAngelscriptBinds::CompileOutPreviousBindAsMethodChain()
{
	auto& Manager = FAngelscriptManager::Get();
	auto* Function = (asCScriptFunction*)Manager.Engine->GetFunctionById(PreviouslyBoundFunction);
	Function->compileOutType = asECompileOutType::CompileOutAsMethodChain;
}

void FAngelscriptBinds::BindProperty(FBindString Signature, size_t Offset)
{
	auto& Manager = FAngelscriptManager::Get();
	Manager.Engine->RegisterObjectProperty(ClassName.ToCString(), Signature.ToCString(), Offset);
	#if WITH_EDITOR
	RecordPropertyRegistrationModule(ClassName, Signature);
	#endif
	PreviouslyBoundPropertyOffset = (int)Offset;
}

void FAngelscriptBinds::BindProperty(FBindString Signature, size_t Offset, const FAngelscriptType::FBindParams& BindParams)
{
	auto& Manager = FAngelscriptManager::Get();
	const asUINT Access = asCObjectProperty::GenerateExposedType(BindParams.bCanEdit, BindParams.bCanRead, BindParams.bCanWrite);
	Manager.Engine->RegisterObjectProperty(ClassName.ToCString(), Signature.ToCString(), Offset, 0, false, Access, BindParams.bProtected);
	#if WITH_EDITOR
	RecordPropertyRegistrationModule(ClassName, Signature);
	#endif
	PreviouslyBoundPropertyOffset = (int)Offset;
}

int FAngelscriptBinds::BindGlobalFunctionCallable(FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData)
{
	auto& Manager = FAngelscriptManager::Get();
	int FunctionId = Manager.Engine->RegisterGlobalFunction(Signature.ToCString(), FunctionPointers.FunctionPointer, asCALL_CDECL, FunctionPointers.FunctionCaller, nullptr);
	OnBind(FunctionId, UserData, nullptr);

	return FunctionId;
}

int FAngelscriptBinds::BindGlobalFunctionDirect(FBindString Signature, asSFuncPtr Function, asECallConvTypes CallConv, FASBindFunctionPointers FunctionPointers, void* UserData)
{
	auto& Manager = FAngelscriptManager::Get();
	int FunctionId = Manager.Engine->RegisterGlobalFunction(Signature.ToCString(), Function, CallConv, FunctionPointers.FunctionCaller);
	OnBind(FunctionId, UserData, nullptr);

	return FunctionId;
}

int FAngelscriptBinds::BindGlobalGenericFunction(FBindString Signature, void(CDECL* Function)(asIScriptGeneric*), void* UserData)
{
	auto& Manager = FAngelscriptManager::Get();
	int FunctionId = Manager.Engine->RegisterGlobalFunction(Signature.ToCString(), asFUNCTION(Function), asCALL_GENERIC, nullptr, nullptr);
	OnBind(FunctionId, UserData, nullptr);

	return FunctionId;
}

void FAngelscriptBinds::BindGlobalVariable(FBindString Signature, const void* Address)
{
	auto& Manager = FAngelscriptManager::Get();
	PreviouslyBoundGlobalProperty = Manager.Engine->RegisterGlobalProperty(Signature.ToCString(), (void*)Address);
	#if WITH_EDITOR
	RecordRegistrationModule(ERegistrationObjectKind::GlobalProperty, PreviouslyBoundGlobalProperty);
	#endif
}

void FAngelscriptBinds::SetPreviousBoundPropertyPureConstant(asQWORD ConstantValue)
{
	auto& Manager = FAngelscriptManager::Get();
	asCGlobalProperty* Property = Manager.Engine->globalProperties[PreviouslyBoundGlobalProperty];
	Property->isPureConstant = true;
	Property->storage = ConstantValue;
}

FAngelscriptBinds::FEnumBind::FEnumBind(FBindString Name)
{
	EnumName = Name;

	auto& Manager = FAngelscriptManager::Get();
	auto* PrevNamespace = Manager.Engine->GetDefaultNamespace();

	TypeId = Manager.Engine->RegisterEnum(Name.ToCString());
	#if WITH_EDITOR
	if (TypeId >= 0 && TypeId != asALREADY_REGISTERED)
	{
		RecordRegistrationModule(ERegistrationObjectKind::Type, TypeId);
	}
	#endif
}

asITypeInfo* FAngelscriptBinds::FEnumBind::GetTypeInfo()
{
	auto& Manager = FAngelscriptManager::Get();
	return Manager.Engine->GetTypeInfoById(TypeId);
}

void FAngelscriptBinds::FEnumBind::FEnumElement::operator=(int32 Value)
{
	auto& Manager = FAngelscriptManager::Get();
	auto* PrevNamespace = Manager.Engine->GetDefaultNamespace();

	auto AnsiEnumName = Bind->EnumName.ToCString();
	Manager.Engine->RegisterEnumValue(AnsiEnumName, Name.ToCString(), Value);
}

FAngelscriptBinds::FNamespace::FNamespace(FBindString Name)
{
	auto& Manager = FAngelscriptManager::Get();
	PrevNamespace.SetDynamic(Manager.Engine->GetDefaultNamespace());
	Manager.Engine->SetDefaultNamespace(Name.ToCString());
}

FAngelscriptBinds::FNamespace::~FNamespace()
{
	auto& Manager = FAngelscriptManager::Get();
	Manager.Engine->SetDefaultNamespace(PrevNamespace.ToCString());
}

asITypeInfo* FAngelscriptBinds::GetTypeInfo()
{
	if (ScriptType == nullptr && !ClassName.IsEmpty())
	{
		auto& Manager = FAngelscriptManager::Get();
		ScriptType = Manager.Engine->GetTypeInfoByName(ClassName.ToCString());
	}
	return ScriptType;
}

bool FAngelscriptBinds::FNamespace::HasFunction(FBindString FunctionName) const
{
	auto& Manager = FAngelscriptManager::Get();
	return Manager.Engine->registeredGlobalFuncTable.FindFirst(
		FunctionName.ToCString(), Manager.Engine->defaultNamespace
	) != nullptr;
}

int FAngelscriptBinds::GetTypeId()
{
	auto* BindType = ScriptType;
	if (BindType == nullptr)
		BindType = GetTypeInfo();
	if (BindType != nullptr)
		return BindType->GetTypeId();
	else
		return -1;
}

void FAngelscriptBinds::SetTypeUserData(void* UserData)
{
	asCObjectType* objType = (asCObjectType*)GetTypeInfo();
	if (objType != nullptr)
		objType->plainUserData = (asPWORD)UserData;
}

bool FAngelscriptBinds::HasMethod(const FString& MethodName)
{
	auto* Type = GetTypeInfo();
	if (!ensure(Type != nullptr))
		return false;
	return Type->GetMethodByName(TCHAR_TO_ANSI(*MethodName)) != nullptr;
}

void FAngelscriptBinds::DisallowAsMemberVariable()
{
	asCObjectType* objType = (asCObjectType*)GetTypeInfo();
	if (objType != nullptr)
		objType->flags |= asOBJ_DISALLOW_MEMBER;
}

void FAngelscriptBinds::DisallowAsLocalVariable()
{
	asCObjectType* objType = (asCObjectType*)GetTypeInfo();
	if (objType != nullptr)
		objType->flags |= asOBJ_DISALLOW_LOCAL_VALUE;
}

bool FAngelscriptBinds::HasGetter(const FString& PropertyName, bool bEvenIfNotProperty)
{
	// Force directly binding the property if bAllowImplicitPropertyAccessors = false
	if (!FAngelscriptManager::Get().ConfigSettings->bAllowImplicitPropertyAccessors && !bEvenIfNotProperty) return false;
	
	TArray<ANSICHAR, TInlineAllocator<64>> FuncName;
	FuncName.SetNumUninitialized(PropertyName.Len() + 4);
	FuncName[0] = 'G';
	FuncName[1] = 'e';
	FuncName[2] = 't';
	for (int32 i = 0, Count = PropertyName.Len(); i < Count; ++i)
		FuncName[i + 3] = (ANSICHAR)PropertyName[i];
	FuncName[FuncName.Num() - 1] = '\0';

	auto* Type = GetTypeInfo();
	if (!ensure(Type != nullptr))
		return false;
	return Type->GetMethodByName(&FuncName[0]) != nullptr;
}

bool FAngelscriptBinds::HasSetter(const FString& PropertyName, bool bEvenIfNotProperty)
{
	// Force directly binding the property if bAllowImplicitPropertyAccessors = false
	if (!FAngelscriptManager::Get().ConfigSettings->bAllowImplicitPropertyAccessors && !bEvenIfNotProperty) return false;

	TArray<ANSICHAR, TInlineAllocator<64>> FuncName;
	FuncName.SetNumUninitialized(PropertyName.Len() + 4);
	FuncName[0] = 'S';
	FuncName[1] = 'e';
	FuncName[2] = 't';
	for (int32 i = 0, Count = PropertyName.Len(); i < Count; ++i)
		FuncName[i + 3] = (ANSICHAR)PropertyName[i];
	FuncName[FuncName.Num() - 1] = '\0';

	auto* Type = GetTypeInfo();
	if (!ensure(Type != nullptr))
		return false;
	return Type->GetMethodByName(&FuncName[0]) != nullptr;
}
