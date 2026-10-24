#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"

#include "Traits/IsTEnumAsByte.h"
#include "Templates/IsEnum.h"
#include "Templates/UnrealTypeTraits.h"

// Unfortunately we need Angelscript here, since we need access
// to asSMethodPtr to make the template magic work.
// Dependency inversion on the bind event means we 
// don't really include this file outside the Binds folder
// though, thankfully.

#include "AngelscriptInclude.h"
#include "AngelscriptBindDatabase.h"
#include "AngelscriptBindString.h"
#include "AngelscriptType.h"
#include "StaticJIT/StaticJITBinds.h"

                                                                                         
#include "AngelscriptStockAutoCaller.h"
#define ASAutoCaller CuvissStockASAutoCaller
                                                                                       
// Need to disable some casting warnings. Trust us MSVC we know what we're doing
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4191)
#endif

#ifdef _MSC_VER
#define CDECL __cdecl
#else
#define CDECL
#endif

#ifndef AS_FORCE_LINK
#if defined(__GNUC__) || defined(__clang__)
#define AS_FORCE_LINK [[gnu::used, gnu::retain]]
#else
#define AS_FORCE_LINK
#endif
#endif

#define AS_STRINGIZE(x) #x
#define AS_CALLER(Function) FASBindFunctionPointers::GetThunk<decltype(Function), Function>(Function)

#define FUNC_NOJIT(Name) AS_CALLER(&Name)
#define FUNCPR_NOJIT(Ret,Name,Args) AS_CALLER((Ret(*)Args)&Name)
#define METHOD_NOJIT(Cls,Name) AS_CALLER(&Cls::Name)
#define METHODPR_NOJIT(Ret,Cls,Name,Args) AS_CALLER((Ret(Cls::*)Args)&Cls::Name)

#if AS_CAN_GENERATE_JIT
#define METHODPR(Ret,Cls,Name,Args) AS_CALLER((Ret(Cls::*)Args)&Cls::Name), AS_STRINGIZE(Name), false
#define METHODPR_TRIVIAL(Ret,Cls,Name,Args) AS_CALLER((Ret(Cls::*)Args)&Cls::Name), AS_STRINGIZE(Name), true
#define METHOD(Cls,Name) AS_CALLER(&Cls::Name), AS_STRINGIZE(Name), false
#define METHOD_TRIVIAL(Cls,Name) AS_CALLER(&Cls::Name), AS_STRINGIZE(Name), true
#define FUNCPR(Ret,Name,Args) AS_CALLER((Ret(*)Args)&Name), AS_STRINGIZE(Name), false
#define FUNCPR_TRIVIAL(Ret,Name,Args) AS_CALLER((Ret(*)Args)&Name), AS_STRINGIZE(Name), true
#define FUNC(Name) AS_CALLER(&Name), AS_STRINGIZE(Name), false
#define FUNC_TRIVIAL(Name) AS_CALLER(&Name), AS_STRINGIZE(Name), true
#define FUNC_CUSTOMNATIVE(Name, CustomNative) AS_CALLER(&Name), AS_STRINGIZE(CustomNative), false
#define FUNC_TRIVIAL_CUSTOMNATIVE(Name, CustomNative) AS_CALLER(&Name), AS_STRINGIZE(CustomNative), true
#else
#define METHODPR(Ret,Cls,Name,Args) AS_CALLER((Ret(Cls::*)Args)&Cls::Name)
#define METHODPR_TRIVIAL(Ret,Cls,Name,Args) AS_CALLER((Ret(Cls::*)Args)&Cls::Name)
#define METHOD(Cls,Name) AS_CALLER(&Cls::Name)
#define METHOD_TRIVIAL(Cls,Name) AS_CALLER(&Cls::Name)
#define FUNCPR(Ret,Name,Args) AS_CALLER((Ret(*)Args)&Name)
#define FUNCPR_TRIVIAL(Ret,Name,Args) AS_CALLER((Ret(*)Args)&Name)
#define FUNC(Name) AS_CALLER(&Name)
#define FUNC_TRIVIAL(Name) AS_CALLER(&Name)
#define FUNC_CUSTOMNATIVE(Name, CustomNative) AS_CALLER(&Name)
#define FUNC_TRIVIAL_CUSTOMNATIVE(Name, CustomNative) AS_CALLER(&Name)
#endif

struct FASBindFunctionPointers
{
	asSFuncPtr FunctionPointer;
	asFunctionCaller FunctionCaller;

	int GetCallConv() const
	{
		if (FunctionPointer.flag == 3)
			return asCALL_THISCALL;
		else if (FunctionPointer.flag == 2)
			return asCALL_CDECL_OBJFIRST;
		else
			return asCALL_GENERIC;
	}

	template<typename FunctionPointerType, FunctionPointerType FunctionPointer, typename ReturnType, typename... ParamTypes>
	static FASBindFunctionPointers GetThunk(ReturnType(*FunctionPtr)(ParamTypes...))
	{
		return FASBindFunctionPointers{
			asFUNCTION(FunctionPointer),
			&ASAutoCaller::RedirectFunctionCallerThunk<FunctionPointerType, FunctionPointer, ReturnType, ParamTypes...>
		};
	}

	template<typename FunctionPointerType, FunctionPointerType FunctionPointer, typename ReturnType, typename... ParamTypes, typename ObjectType>
	static FASBindFunctionPointers GetThunk(ReturnType(ObjectType::*FunctionPtr)(ParamTypes...))
	{
		return FASBindFunctionPointers{
			asSMethodPtr<sizeof(FunctionPointerType)>::Convert(FunctionPointer),
			&ASAutoCaller::RedirectMethodCallerThunk<FunctionPointerType, FunctionPointer, ReturnType, ObjectType, ParamTypes...>
		};
	}

	template<typename FunctionPointerType, FunctionPointerType FunctionPointer, typename ReturnType, typename... ParamTypes, typename ObjectType>
	static FASBindFunctionPointers GetThunk(ReturnType(ObjectType::*FunctionPtr)(ParamTypes...) const)
	{
		return FASBindFunctionPointers{
			asSMethodPtr<sizeof(FunctionPointerType)>::Convert(FunctionPointer),
			&ASAutoCaller::RedirectMethodCallerThunk<FunctionPointerType, FunctionPointer, ReturnType, ObjectType, ParamTypes...>
		};
	}

	template<typename FunctionPointerType, FunctionPointerType FunctionPointer, typename ReturnType, typename... ParamTypes, typename ObjectType>
	static FASBindFunctionPointers GetThunk(ReturnType(ObjectType::*FunctionPtr)(ParamTypes...) const&)
	{
		return FASBindFunctionPointers{
			asSMethodPtr<sizeof(FunctionPointerType)>::Convert(FunctionPointer),
			&ASAutoCaller::RedirectMethodCallerThunk<FunctionPointerType, FunctionPointer, ReturnType, ObjectType, ParamTypes...>
		};
	}

                                                                                                              
	template<typename ReturnType, typename... ParamTypes>
	static FASBindFunctionPointers GetRaw(ReturnType(*FunctionPtr)(ParamTypes...))
	{
		return FASBindFunctionPointers{
			asFUNCTION(FunctionPtr),
			&ASAutoCaller::RedirectFunctionCallerRaw<ReturnType, ParamTypes...>
		};
	}

	template<typename ReturnType, typename... ParamTypes, typename ObjectType>
	static FASBindFunctionPointers GetRaw(ReturnType(ObjectType::*FunctionPtr)(ParamTypes...))
	{
		using MethodPointerType = decltype(FunctionPtr);
		return FASBindFunctionPointers{
			asSMethodPtr<sizeof(MethodPointerType)>::Convert(FunctionPtr),
			(asFunctionCaller::MethodCallerPtr)&ASAutoCaller::RedirectMethodCallerRaw<
				MethodPointerType, ReturnType, ObjectType, ParamTypes...>
		};
	}

	template<typename ReturnType, typename... ParamTypes, typename ObjectType>
	static FASBindFunctionPointers GetRaw(ReturnType(ObjectType::*FunctionPtr)(ParamTypes...) const)
	{
		using MethodPointerType = decltype(FunctionPtr);
		return FASBindFunctionPointers{
			asSMethodPtr<sizeof(MethodPointerType)>::Convert(FunctionPtr),
			(asFunctionCaller::MethodCallerPtr)&ASAutoCaller::RedirectMethodCallerRaw<
				MethodPointerType, ReturnType, ObjectType, ParamTypes...>
		};
	}

	template<typename ReturnType, typename... ParamTypes, typename ObjectType>
	static FASBindFunctionPointers GetRaw(ReturnType(ObjectType::*FunctionPtr)(ParamTypes...) const&)
	{
		using MethodPointerType = decltype(FunctionPtr);
		return FASBindFunctionPointers{
			asSMethodPtr<sizeof(MethodPointerType)>::Convert(FunctionPtr),
			(asFunctionCaller::MethodCallerPtr)&ASAutoCaller::RedirectMethodCallerRaw<
				MethodPointerType, ReturnType, ObjectType, ParamTypes...>
		};
	}

#define GetRaw CuvissUpstreamGetRaw
                                                                                                            
	template<typename ReturnType, typename... ParamTypes>
	static FASBindFunctionPointers GetRaw(ReturnType(*FunctionPtr)(ParamTypes...))
	{
		return FASBindFunctionPointers{
			asFUNCTION(FunctionPtr),
			&ASAutoCaller::RedirectFunctionCallerRaw<ReturnType, ParamTypes...>
		};
	}

	template<typename ReturnType, typename... ParamTypes, typename ObjectType>
	static FASBindFunctionPointers GetRaw(ReturnType(ObjectType::*FunctionPtr)(ParamTypes...))
	{
		return FASBindFunctionPointers{
			asSMethodPtr<sizeof(FunctionPtr)>::Convert(FunctionPtr),
			(asFunctionCaller::MethodCallerPtr)&ASAutoCaller::RedirectMethodCallerRaw<ReturnType, ObjectType, ParamTypes...>
		};
	}

	template<typename ReturnType, typename... ParamTypes, typename ObjectType>
	static FASBindFunctionPointers GetRaw(ReturnType(ObjectType::*FunctionPtr)(ParamTypes...) const)
	{
		return FASBindFunctionPointers{
			asSMethodPtr<sizeof(FunctionPtr)>::Convert(FunctionPtr),
			(asFunctionCaller::MethodCallerPtr)&ASAutoCaller::RedirectMethodCallerRaw<ReturnType, ObjectType, ParamTypes...>
		};
	}

	template<typename ReturnType, typename... ParamTypes, typename ObjectType>
	static FASBindFunctionPointers GetRaw(ReturnType(ObjectType::*FunctionPtr)(ParamTypes...) const&)
	{
		return FASBindFunctionPointers{
			asSMethodPtr<sizeof(FunctionPtr)>::Convert(FunctionPtr),
			(asFunctionCaller::MethodCallerPtr)&ASAutoCaller::RedirectMethodCallerRaw<ReturnType, ObjectType, ParamTypes...>
		};
	}

                                                                                                               
#undef GetRaw
                                                                                                             
	template<typename T>
	static FASBindFunctionPointers GetLambda(T Lambda)
	{
		return GetThunk<
			decltype(+T{}),
			+T{}
		>(+T{});
	}
};

                                                                                                   
#undef ASAutoCaller
                                                                                                 
struct FBindFlags
{
	bool bPOD = false;
	bool bTemplate = false;
	FBindString TemplateType;
	int Alignment = -1;
	int ExtraFlags = 0;
};

struct FSystemFunctionArgument
{
	FBindString Name;
	FBindString DefaultValue;
	FAngelscriptTypeUsage Type;
	bool bReference = false;
	bool bConst = false;
};

struct FSystemFunctionBind
{
	FBindString Name;
	FAngelscriptTypeUsage ReturnType;
	TArray<FSystemFunctionArgument, TInlineAllocator<4>> Arguments;
	bool bConst = false;
	FBindString Namespace;
	TSharedPtr<FAngelscriptType> ObjectType;
};

struct ANGELSCRIPTCODE_API FAngelscriptBinds
{
	#if WITH_EDITOR
	enum class ERegistrationObjectKind : uint8
	{
		Function,
		Type,
		GlobalProperty,
		Property
	};

	/** Returns the module containing the FBind registration object for one registered SDK object. */
	static bool TryGetRegistrationModule(
		ERegistrationObjectKind Kind,
		int32 ObjectId,
		FString& OutModule,
		const FString& ObjectName = FString());
	#endif

	/* 
	* Class-specific binding.
	*/
	static FAngelscriptBinds ReferenceClass(FBindString Name, UClass* UnrealClass);
	static FAngelscriptBinds ExistingClass(FBindString Name);

	template<typename T>
	static FAngelscriptBinds ValueClass(FBindString Name, FBindFlags Flags)
	{
		if (Flags.Alignment == -1)
			Flags.Alignment = alignof(T);
		Flags.ExtraFlags |= asGetTypeTraits<T>();
		return ValueClass(Name, Flags, sizeof(T));
	}

	static FAngelscriptBinds ValueClass(FBindString Name, UScriptStruct* StructType, FBindFlags Flags)
	{
		int Size;
		auto* Ops = StructType->GetCppStructOps();
		if (Ops != nullptr)
			Size = Ops->GetSize();
		else
			Size = StructType->GetPropertiesSize();
		if (Flags.Alignment == -1)
			Flags.Alignment = StructType->GetMinAlignment();
		return ValueClass(Name, Flags, Size);
	}

	static FAngelscriptBinds ValueClass(FBindString Name, SIZE_T Size, FBindFlags Flags)
	{
		Flags.ExtraFlags |= asOBJ_APP_CLASS_CDAK;
		return ValueClass(Name, Flags, Size);
	}

	inline int Method(FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData = nullptr)
	{
		return BindMethod(Signature, FunctionPointers, UserData);
	}

	inline int Method(FBindString Signature, FASBindFunctionPointers FunctionPointers, FBindString FunctionName, bool bTrivial, void* UserData = nullptr)
	{
		int FunctionId = BindMethod(Signature, FunctionPointers, UserData);
		if (FunctionPointers.FunctionPointer.flag == 2)
		{
			SCRIPT_NATIVE_FUNCTION(FunctionName.ToCString_EnsureConstant(), bTrivial);
		}
		else
		{
			SCRIPT_NATIVE_METHOD(*this, FunctionName.ToCString_EnsureConstant(), bTrivial);
		}
		return FunctionId;
	}

	inline int Method(FBindString Signature, FASBindFunctionPointers FunctionPointers, const FAngelscriptType::FBindParams& BindParams, void* UserData = nullptr)
	{
		return BindMethod(Signature, FunctionPointers, BindParams, UserData);
	}

	inline int Method(FBindString Signature, FASBindFunctionPointers FunctionPointers, FBindString FunctionName, bool bTrivial, const FAngelscriptType::FBindParams& BindParams, void* UserData = nullptr)
	{
		int FunctionId = BindMethod(Signature, FunctionPointers, BindParams, UserData);
		SCRIPT_NATIVE_FUNCTION(FunctionName.ToCString_EnsureConstant(), bTrivial);
		checkf(FunctionPointers.FunctionPointer.flag == 2, TEXT("Method must be bound using a METHOD() macro instead of FUNC() macro"));
		return FunctionId;
	}

	template<typename C, typename Value, typename... Args>
	inline int Method(FBindString Signature, Value(C::*Fun)(Args...)const, void* UserData = nullptr)
	{
		return BindMethod(Signature, FASBindFunctionPointers::GetRaw(Fun), UserData);
	}

	template<typename C, typename Value, typename... Args>
	inline int Method(FBindString Signature, Value(C::*Fun)(Args...)const&, void* UserData = nullptr)
	{
		return BindMethod(Signature, FASBindFunctionPointers::GetRaw(Fun), UserData);
	}

	template<typename C, typename Value, typename... Args>
	inline int Method(FBindString Signature, Value(C::*Fun)(Args...), void* UserData = nullptr)
	{
		return BindMethod(Signature, FASBindFunctionPointers::GetRaw(Fun), UserData);
	}

	template<typename Value, typename... Args>
	inline int Method(FBindString Signature, Value(CDECL *Fun)(Args...), void* UserData = nullptr)
	{
		return BindMethod(Signature, FASBindFunctionPointers::GetRaw(Fun), UserData);
	}

	template<typename T>
	inline int Method(FBindString Signature, T Function, void* UserData = nullptr)
	{
		return BindMethod(Signature, FASBindFunctionPointers::GetLambda(Function), UserData);
	}

	template<typename Value, typename... Args>
	inline void Destructor(FBindString Signature, Value(CDECL *Fun)(Args...), void* UserData = nullptr)
	{
		BindBehaviour(asBEHAVE_DESTRUCT, Signature, FASBindFunctionPointers::GetRaw(Fun), UserData);
	}

	template<typename Value, typename... Args>
	inline void Constructor(FBindString Signature, Value(CDECL *Fun)(Args...), void* UserData = nullptr)
	{
		BindBehaviour(asBEHAVE_CONSTRUCT, Signature, FASBindFunctionPointers::GetRaw(Fun), UserData);
	}

	template<typename Value, typename... Args>
	inline void ImplicitConstructor(FBindString Signature, Value(CDECL *Fun)(Args...), void* UserData = nullptr)
	{
		BindBehaviour(asBEHAVE_CONSTRUCT, Signature, FASBindFunctionPointers::GetRaw(Fun), UserData);
		MarkAsImplicitConstructor();
	}

	template<typename Value, typename... Args>
	inline void Factory(FBindString Signature, Value(CDECL *Fun)(Args...), void* UserData = nullptr)
	{
		Factory(Signature, FASBindFunctionPointers::GetRaw(Fun), UserData);
	}

	void GenericMethod(FBindString Signature, void(CDECL *Fun)(asIScriptGeneric*), void* UserData);

	template<typename T>
	inline void GenericMethod(FBindString Signature, T Function, void* UserData)
	{
		return GenericMethod(Signature, +Function, UserData);
	}

	template<typename C, typename T>
	inline void Property(FBindString Signature, T C::*Ptr)
	{
		return BindProperty(Signature, (size_t)&(((C*)nullptr)->*Ptr));
	}

	inline void Property(FBindString Signature, size_t Offset)
	{
		return BindProperty(Signature, Offset);
	}

	inline void Property(FBindString Signature, size_t Offset, const FAngelscriptType::FBindParams& BindParams)
	{
		return BindProperty(Signature, Offset, BindParams);
	}

	inline void Factory(FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData = nullptr)
	{
		return BindStaticBehaviour(asBEHAVE_FACTORY, Signature, FunctionPointers, UserData);
	}

	inline void Destructor(FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData = nullptr)
	{
		BindBehaviour(asBEHAVE_DESTRUCT, Signature, FunctionPointers, UserData);
	}

	inline void Destructor(FBindString Signature, FASBindFunctionPointers FunctionPointers, FBindString FuncName, bool bTrivial, void* UserData = nullptr)
	{
		BindBehaviour(asBEHAVE_DESTRUCT, Signature, FunctionPointers, UserData);
		SCRIPT_NATIVE_FUNCTION(FuncName.ToCString_EnsureConstant(), bTrivial);
	}

	template<typename T>
	inline void Destructor(FBindString Signature, T Function, void* UserData = nullptr)
	{
		Destructor(Signature, FASBindFunctionPointers::GetLambda(Function), UserData);
	}

	template<typename T>
	inline void Factory(FBindString Signature, T Function, void* UserData = nullptr)
	{
		return Factory(Signature, FASBindFunctionPointers::GetLambda(Function), UserData);
	}

	inline void Constructor(FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData = nullptr)
	{
		return BindBehaviour(asBEHAVE_CONSTRUCT, Signature, FunctionPointers, UserData);
	}

	inline void Constructor(FBindString Signature, FASBindFunctionPointers FunctionPointers, FBindString FuncName, bool bTrivial, void* UserData = nullptr)
	{
		BindBehaviour(asBEHAVE_CONSTRUCT, Signature, FunctionPointers, UserData);
		SCRIPT_NATIVE_FUNCTION(FuncName.ToCString_EnsureConstant(), bTrivial);
	}

	template<typename T>
	inline void Constructor(FBindString Signature, T Function, void* UserData = nullptr)
	{
		return Constructor(Signature, FASBindFunctionPointers::GetLambda(Function), UserData);
	}

	inline void ImplicitConstructor(FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData = nullptr)
	{
		BindBehaviour(asBEHAVE_CONSTRUCT, Signature, FunctionPointers, UserData);
		MarkAsImplicitConstructor();
	}

	inline void ImplicitConstructor(FBindString Signature, FASBindFunctionPointers FunctionPointers, FBindString FuncName, bool bTrivial, void* UserData = nullptr)
	{
		BindBehaviour(asBEHAVE_CONSTRUCT, Signature, FunctionPointers, UserData);
		SCRIPT_NATIVE_FUNCTION(FuncName.ToCString_EnsureConstant(), bTrivial);
		MarkAsImplicitConstructor();
	}

	template<typename T>
	inline void ImplicitConstructor(FBindString Signature, T Function, void* UserData = nullptr)
	{
		ImplicitConstructor(Signature, FASBindFunctionPointers::GetLambda(Function), UserData);
	}

	inline void TemplateCallback(FBindString Signature, FASBindFunctionPointers FunctionPointers)
	{
		return BindStaticBehaviour(asBEHAVE_TEMPLATE_CALLBACK, Signature, FunctionPointers);
	}

	template<typename T>
	inline void TemplateCallback(FBindString Signature, T Function)
	{
		return TemplateCallback(Signature, FASBindFunctionPointers::GetLambda(Function));
	}

	/* 
	* Global binding
	*/
	static inline int BindGlobalFunction(FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData = nullptr)
	{
		return BindGlobalFunctionCallable(Signature, FunctionPointers, UserData);
	}

	static inline int BindGlobalFunction(FBindString Signature, FASBindFunctionPointers FunctionPointers, FBindString FuncName, bool bTrivial, void* UserData = nullptr)
	{
		int FunctionId = BindGlobalFunctionCallable(Signature, FunctionPointers, UserData);
		SCRIPT_NATIVE_FUNCTION(FuncName.ToCString_EnsureConstant(), bTrivial);
		return FunctionId;
	}

	template<typename T>
	static inline int BindGlobalFunction(FBindString Signature, T Function, void* UserData = nullptr)
	{
		return BindGlobalFunctionCallable(Signature, FASBindFunctionPointers::GetLambda(Function), UserData);
	}

	template<typename T>
	static inline int BindGlobalGenericFunction(FBindString Signature, T Function, void* UserData = nullptr)
	{
		return BindGlobalGenericFunction(Signature, +Function, UserData);
	}

	template<typename Value, typename... Args>
	static inline int BindGlobalFunction(FBindString Signature, Value(CDECL *Fun)(Args...), void* UserData = nullptr)
	{
		return BindGlobalFunctionCallable(Signature, FASBindFunctionPointers::GetRaw(Fun), UserData);
	}

	template<typename Value, typename... Args>
	static inline int BindGlobalFunction(FBindString Signature, Value(CDECL *Fun)(Args...), FBindString FuncName, bool bTrivial, void* UserData = nullptr)
	{
		int FunctionId = BindGlobalFunctionCallable(Signature, FASBindFunctionPointers::GetRaw(Fun), UserData);
		SCRIPT_NATIVE_FUNCTION(FuncName.ToCString_EnsureConstant(), bTrivial);
		return FunctionId;
	}

	/**
	 * Enum Binding
	 */
	class ANGELSCRIPTCODE_API FEnumBind
	{
	public:
		FEnumBind(FBindString Name);
		FBindString EnumName;
		int32 TypeId;

		asITypeInfo* GetTypeInfo();

		struct ANGELSCRIPTCODE_API FEnumElement
		{
			FEnumBind* Bind;
			FBindString Name;

			void operator=(int32 Value);

			template<typename E>
			void operator=(E Value)
			{
				*this = (int32)Value;
			}
		};

		FEnumElement operator[](FBindString Name)
		{
			return FEnumElement{this, Name};
		}
	};

	inline static FEnumBind Enum(FBindString Name)
	{
		return FEnumBind(Name);
	}

	/* Register functions to be called at bind-time. */
	enum class EOrder : int32
	{
		Early = -100,
		Normal = 0,
		Late = 100,
	};

	struct ANGELSCRIPTCODE_API FBind
	{
		FBind(int32 BindOrder, TFunction<void()> Function)
		{
			#if WITH_EDITOR
			FAngelscriptBinds::RegisterBinds(BindOrder, Function, this);
			#else
			FAngelscriptBinds::RegisterBinds(BindOrder, Function);
			#endif
		}

		FBind(EOrder BindOrder, TFunction<void()> Function)
		{
			#if WITH_EDITOR
			FAngelscriptBinds::RegisterBinds((int32)BindOrder, Function, this);
			#else
			FAngelscriptBinds::RegisterBinds((int32)BindOrder, Function);
			#endif
		}

		FBind(TFunction<void()> Function)
		{
			#if WITH_EDITOR
			FAngelscriptBinds::RegisterBinds(0, Function, this);
			#else
			FAngelscriptBinds::RegisterBinds(0, Function);
			#endif
		}
	};

	#if WITH_EDITOR
	static void RegisterBinds(int32 BindOrder, TFunction<void()> Function, const void* RegistrationObjectAddress = nullptr);
	#else
	static void RegisterBinds(int32 BindOrder, TFunction<void()> Function);
	#endif

	struct ANGELSCRIPTCODE_API FNamespace
	{
		FBindString PrevNamespace;

		FNamespace(FBindString Name);
		~FNamespace();

		bool HasFunction(FBindString FunctionName) const;
	};

	static int BindGlobalFunctionCallable(FBindString Signature, FASBindFunctionPointers FunctionPointers, void* UserData = nullptr);
	static int BindGlobalFunctionDirect(FBindString Signature, asSFuncPtr Function, asECallConvTypes CallConv, FASBindFunctionPointers FunctionPointers, void* UserData = nullptr);
	static int BindGlobalGenericFunction(FBindString Signature, void(CDECL* Function)(asIScriptGeneric*), void* UserData = nullptr);
	static void BindGlobalVariable(FBindString Signature, const void* Address);
	static int BindMethodDirect(FBindString ClassName, FBindString Signature, asSFuncPtr Function, asECallConvTypes CallConv, FASBindFunctionPointers FunctionPointers, void* UserData = nullptr);
	static int BindBehaviourDirect(FBindString ClassName, asEBehaviours Behaviour, FBindString Signature, asSFuncPtr Function, asECallConvTypes CallConv, FASBindFunctionPointers FunctionPointers, void* UserData = nullptr);

	static int CompileOutInTest(int FunctionId);
	static int CompileOutIfNoLog(int FunctionId);
	static int CompileOutAsEnsure(int FunctionId);
	static int CompileOutAsCheck(int FunctionId);
	static int CompileReplaceWithFirstArgInTest(int FunctionId);

	static void CompileOutPreviousBind();
	static void CompileOutPreviousBindAsMethodChain();

	asITypeInfo* GetTypeInfo();
	int GetTypeId();
	void SetTypeUserData(void* UserData);
	void DisallowAsMemberVariable();
	void DisallowAsLocalVariable();

	bool HasMethod(const FString& MethodName);
	bool HasGetter(const FString& PropertyName, bool bEvenIfNotProperty);
	bool HasSetter(const FString& PropertyName, bool bEvenIfNotProperty);

	static asIScriptFunction* GetPreviousBind();
	static int32 GetPreviousFunctionId() { return PreviouslyBoundFunction; }
	static int32 GetPreviousGlobalVariableId() { return PreviouslyBoundGlobalProperty; }
	static int32 GetPreviousPropertyOffset() { return PreviouslyBoundPropertyOffset; }
	static void DeprecatePreviousBind(const ANSICHAR* DeprecationMessage);
	static void SetPreviousBindIsGeneratedAccessor(bool bIsAccessor);
	static void SetPreviousBindIsEditorOnly(bool bEditorOnly);
	static void SetPreviousBindRequiresWorldContext(bool bRequiresWorldContext);
	static void SetPreviousBindIsPropertyAccessor(bool bIsProperty);
	static void SetPreviousBindIsCallable(bool bIsCallable);
	static void SetPreviousBindNoDiscard(bool bNoDiscard);
	static void SetPreviousBindForceConstArgumentExpressions(bool bForceConst);
	static void SetPreviousBindUnsafeDuringConstruction(bool bUnsafe);
	static void PreviousBindPassScriptFunctionAsFirstParam();
	static void PreviousBindPassScriptObjectTypeAsFirstParam();
	static void SetPreviousBindArgumentDeterminesOutputType(int ArgumentIndex);

	template<typename T>
	static void SetPreviousBoundGlobalVariablePureConstant(T ConstantValue)
	{
		asQWORD RawValue = 0;
		memcpy(&RawValue, &ConstantValue, sizeof(T));
		SetPreviousBoundPropertyPureConstant(RawValue);
	}

	static void MarkAsImplicitConstructor();

private:
	static FAngelscriptBinds ValueClass(FBindString Name, FBindFlags Flags, int32 Size);

	explicit FAngelscriptBinds(FBindString Name, int32 Flags, int32 Size);
	explicit FAngelscriptBinds(FBindString Name);

	FBindString ClassName;
	asITypeInfo* ScriptType = nullptr;

	void BindBehaviour(asEBehaviours Beh, FBindString Signature, FASBindFunctionPointers Caller, void* UserData = nullptr);
	void BindStaticBehaviour(asEBehaviours Beh, FBindString Signature, FASBindFunctionPointers Caller, void* UserData = nullptr);
	int BindMethod(FBindString Signature, FASBindFunctionPointers Caller, void* UserData = nullptr);
	int BindMethod(FBindString Signature, FASBindFunctionPointers Caller, const FAngelscriptType::FBindParams& BindParams, void* UserData = nullptr);
	void BindProperty(FBindString Signature, size_t Offset);
	void BindProperty(FBindString Signature, size_t Offset, const FAngelscriptType::FBindParams& BindParams);

	static void OnBind(int FunctionId, void* UserData, const FAngelscriptType::FBindParams* BindParams);
	static int PreviouslyBoundFunction;
	static int PreviouslyBoundGlobalProperty;
	static int PreviouslyBoundPropertyOffset;

	static void SetPreviousBoundPropertyPureConstant(asQWORD ConstantValue);

	friend struct FAngelscriptManager;
	static void CallBinds();
};

// Re-enable any warnings we disabled
#ifdef _MSC_VER
#pragma warning(pop)
#endif
