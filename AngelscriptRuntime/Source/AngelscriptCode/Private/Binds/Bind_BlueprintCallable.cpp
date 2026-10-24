#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "AngelscriptDocs.h"
                                                                                                         
#include "AngelscriptAttachmentCallerScope.h"
#include "Components/SceneComponent.h"
                                                                                                       
                                                                                           
#include "AngelscriptCallableDispatchAdapter.h"
#include "Shared/SoloASBridgeReflection.h"
                                                                                         

#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Misc/PackageName.h"
                                                                                                                                  
#include "Kismet/KismetMathLibrary.h"
                                                                                                                                

#include "Helper_FunctionSignature.h"
                                                                                                        
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
                                                                                                      

extern void RegisterBlueprintEventByScriptName(UClass* Class, const FString& ScriptName, UFunction* Function);

// Bind a native function to angelscript, provided all
// argument and return types are known as FAngelscriptTypes.
static const FName NAME_Function_NotInAngelscript("NotInAngelscript");
static const FName NAME_Function_BlueprintInternalUseOnly("BlueprintInternalUseOnly");
static const FName NAME_Function_UsableInAngelscript("UsableInAngelscript");
                                                                                                      
static bool IsNativeShapeSetterExcludedFromAngelscript(const UFunction* Function)
{
	if (Function == nullptr)
		return false;

	const UClass* OwnerClass = Function->GetOuterUClass();
	const FName FunctionName = Function->GetFName();
	return (OwnerClass == UBoxComponent::StaticClass() && FunctionName == FName(TEXT("SetBoxExtent")))
		|| (OwnerClass == UCapsuleComponent::StaticClass() && FunctionName == FName(TEXT("SetCapsuleSize")))
		|| (OwnerClass == USphereComponent::StaticClass() && FunctionName == FName(TEXT("SetSphereRadius")));
}
                                                                                                    

void BindBlueprintCallable(
	TSharedRef<FAngelscriptType> InType,
	UFunction* Function,
	FAngelscriptMethodBind& DBBind
#if !AS_USE_BIND_DB
	, const TCHAR* OverrideName
#endif
)
{
                                                                                                    
	if (IsNativeShapeSetterExcludedFromAngelscript(Function))
		return;
                                                                                                  

                                                                                        
    // Bridge owns exclusion data; registration must check it here because a
    // native reflected function can enter through Blueprint flags or ScriptCallable.
    ESoloASBridgeFunctionTrait OwnerTraits = ESoloASBridgeFunctionTrait::None;
    if (SoloASBridge::GetFunctionTraits(Function, OwnerTraits)
        && EnumHasAnyFlags(OwnerTraits, ESoloASBridgeFunctionTrait::NotInAngelscript))
        return;
                                                                                      
#if !AS_USE_BIND_DB
	// Don't bind functions that aren't native
	if (!Function->HasAnyFunctionFlags(FUNC_Native))
		return;

	// Specifically excluded functions are not bound
	if (Function->HasMetaData(NAME_Function_NotInAngelscript))
		return;

	// BlueprintInternalUseOnly functions are not bound
	if (Function->HasMetaData(NAME_Function_BlueprintInternalUseOnly) && !Function->HasMetaData(NAME_Function_UsableInAngelscript))
		return;
#endif

	UClass* OwningClass = CastChecked<UClass>(Function->GetOuter());
                                                                                                  
#if 0
                                                                                                
	auto* FuncInMap = OwningClass->ASReflectedFunctionPointers.Find(Function->GetFName());

	// Don't bind functions without a native pointer
	if (FuncInMap == nullptr)
		return;
	if (FuncInMap->CallerPtr == nullptr)
		return;
                                                                                                     
#endif
	(void)OwningClass;
                                                                                                   

#if AS_USE_BIND_DB
	FAngelscriptFunctionSignature Signature;
	Signature.InitFromDB(InType, Function, DBBind, /* bInitTypes= */ false);

#elif !AS_USE_BIND_DB
	FAngelscriptFunctionSignature Signature(InType, Function, OverrideName);

	// Don't bind things that have types that are unknown to us
	if (!Signature.bAllTypesValid)
		return;
#endif

                                                                                                                          
	if (IsRunningCookCommandlet()
		&& OwningClass == UKismetMathLibrary::StaticClass()
		&& (Signature.TargetType == FAngelscriptFunctionSignature::EBindTargetType::GlobalFunction
			|| Signature.TargetType == FAngelscriptFunctionSignature::EBindTargetType::GlobalScopeFunction))
	{
		if (Signature.TargetType == FAngelscriptFunctionSignature::EBindTargetType::GlobalFunction)
		{
			FAngelscriptBinds::FNamespace NamespaceScope(Signature.ClassName);

			if (NamespaceScope.HasFunction(FBindString(Signature.ScriptName)))
				return;
		}
		else
		{
			FAngelscriptBinds::FNamespace GlobalScope(FBindString(""));

			if (GlobalScope.HasFunction(FBindString(Signature.ScriptName)))
				return;
		}
	}
                                                                                                                        
	FASBindFunctionPointers FunctionPointers;
                                                                                                       
#if 0
                                                                                                     
	FunctionPointers.FunctionCaller = asFunctionCaller((asFunctionCaller::FunctionCallerPtr)FuncInMap->CallerPtr);

	if (Function->HasAnyFunctionFlags(FUNC_Static))
	{
		// Static functions directly have the function pointer in the reflected map
		FunctionPointers.FunctionPointer = asFUNCTION(FuncInMap->FunctionPointerOrRetriever);
	}
	else
	{
		// Member methods have a pointer to a retriever function in the map
		((void(*)(void*))FuncInMap->FunctionPointerOrRetriever)(&FunctionPointers.FunctionPointer);
		FunctionPointers.FunctionPointer.flag = 3;
	}
                                                                                                  
#endif
	if (!RegisterAngelscriptCallableDispatch(Function, FunctionPointers))
		return;
                                                                                                

                                                                                                          
	if (Function->GetOuterUClass() == USceneComponent::StaticClass()
		&& Function->GetFName() == TEXT("K2_DetachFromComponent")
		&& Signature.TargetType == FAngelscriptFunctionSignature::EBindTargetType::Method)
	{
		const FASBindFunctionPointers DetachPointers = FUNC_NOJIT(AngelscriptAttachmentCaller::Detach);
		const int FunctionId = FAngelscriptBinds::BindMethodDirect(InType->GetAngelscriptTypeName(),
			Signature.Declaration, DetachPointers.FunctionPointer, asCALL_CDECL_OBJFIRST, DetachPointers);
		Signature.ModifyScriptFunction(FunctionId);
		return;
	}
                                                                                                        
	// Actually bind into angelscript engine
	if (Signature.TargetType == FAngelscriptFunctionSignature::EBindTargetType::Method)
	{
		// Member methods should be bound as THISCALL
		int FunctionId = FAngelscriptBinds::BindMethodDirect(
			InType->GetAngelscriptTypeName(),
			Signature.Declaration, FunctionPointers.FunctionPointer, asCALL_THISCALL, FunctionPointers);
		Signature.ModifyScriptFunction(FunctionId);
	}
	else if (Signature.TargetType == FAngelscriptFunctionSignature::EBindTargetType::GlobalFunction)
	{
		// Static functions should be bound as a global function in a namespace
		FAngelscriptBinds::FNamespace ns(Signature.ClassName);
		int FunctionId = FAngelscriptBinds::BindGlobalFunction(Signature.Declaration, FunctionPointers);
		Signature.ModifyScriptFunction(FunctionId);
	}
	else if (Signature.TargetType == FAngelscriptFunctionSignature::EBindTargetType::MixinMethod)
	{
		// This is a static function converted through mixin to a script member function
		int FunctionId = FAngelscriptBinds::BindMethodDirect(
			Signature.ClassName,
			Signature.Declaration, FunctionPointers.FunctionPointer,
			asCALL_CDECL_OBJFIRST, FunctionPointers);
		Signature.ModifyScriptFunction(FunctionId);
	}
	else if (Signature.TargetType == FAngelscriptFunctionSignature::EBindTargetType::MixinConstructor)
	{
		// This is a static function converted through mixin to a script member function
		int FunctionId = FAngelscriptBinds::BindBehaviourDirect(
			Signature.ClassName, asEBehaviours::asBEHAVE_CONSTRUCT,
			Signature.Declaration, FunctionPointers.FunctionPointer,
			asCALL_CDECL_OBJFIRST, FunctionPointers);
		Signature.ModifyScriptFunction(FunctionId);
	}
	else if (Signature.TargetType == FAngelscriptFunctionSignature::EBindTargetType::MixinImplicitConstructor)
	{
		// This is a static function converted through mixin to a script member function
		int FunctionId = FAngelscriptBinds::BindBehaviourDirect(
			Signature.ClassName, asEBehaviours::asBEHAVE_CONSTRUCT,
			Signature.Declaration, FunctionPointers.FunctionPointer,
			asCALL_CDECL_OBJFIRST, FunctionPointers);
		Signature.ModifyScriptFunction(FunctionId);
		FAngelscriptBinds::MarkAsImplicitConstructor();
	}
	else if (Signature.TargetType == FAngelscriptFunctionSignature::EBindTargetType::GlobalScopeFunction)
	{
		// Some functions have a meta tag to put them in global scope
		int GlobalFunctionId = FAngelscriptBinds::BindGlobalFunction(Signature.Declaration, FunctionPointers);
		Signature.ModifyScriptFunction(GlobalFunctionId);
	}
	else
	{
		checkf(false, TEXT("Unknown bind target type"));
		return;
	}

#if AS_CAN_GENERATE_JIT
#if AS_USE_BIND_DB
	SCRIPT_NATIVE_UFUNCTION(Function, FPackageName::ObjectPathToObjectName(DBBind.UnrealPath), Signature.bTrivial);
#else
	SCRIPT_NATIVE_UFUNCTION(Function, Function->GetName(), Signature.bTrivial);
#endif
#endif

#if !AS_USE_BIND_DB
	Signature.WriteToDB(DBBind);
#endif
}
