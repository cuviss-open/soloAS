#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "AngelscriptNativeInterfaceBinding.h"

#include "AngelscriptBinds.h"
#include "Extensions/AngelscriptExtensionHooks.h"
#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "AngelscriptNativeInterfaceEventRegistry.h"
#include "AngelscriptBindDatabase.h"
#include "StartAngelscriptHeaders.h"
#include "as_objecttype.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

#include "UObject/Class.h"
#include "UObject/UnrealType.h"


namespace AngelscriptNativeInterfaceBinding
{
namespace
{
FAngelscriptBinds ReferenceNativeInterface(
	FBindString Name,
	UClass* UnrealClass,
	bool bObjectView
)
{
    if (bObjectView)
    {
        auto Binds = FAngelscriptBinds::ReferenceClass(Name, UnrealClass);
        Binds.SetTypeUserData(UnrealClass);
        Binds.Method("UObject opImplCast() const", [](UObject* Object) -> UObject* { return Object; });
        return Binds;
    }
	auto& Manager = FAngelscriptManager::Get();
	const int TypeId = Manager.Engine->RegisterInterface(Name.ToCString());
	check(TypeId >= 0 || TypeId == asALREADY_REGISTERED);

	auto Binds = FAngelscriptBinds::ExistingClass(Name);
	Binds.SetTypeUserData(UnrealClass);
	return Binds;
}

bool NativeInterfaceInherits(FAngelscriptBinds& Binds, FBindString ParentName)
{
	auto& Manager = FAngelscriptManager::Get();
	auto* InterfaceType = static_cast<asCObjectType*>(Binds.GetTypeInfo());
	auto* ParentType = static_cast<asCObjectType*>(
		Manager.Engine->GetTypeInfoByName(ParentName.ToCString()));
    if (InterfaceType && ParentType && !InterfaceType->IsInterface() && !ParentType->IsInterface())
    {
        UClass* Interface = static_cast<UClass*>(InterfaceType->GetUserData());
        UClass* Parent = static_cast<UClass*>(ParentType->GetUserData());
        if (!Interface || !Parent || (!Interface->IsChildOf(Parent) && !Interface->ImplementsInterface(Parent))) return false;
        const FString Declaration = FString(ANSI_TO_TCHAR(ParentName.ToCString())) + TEXT(" opImplCast() const");
        if (InterfaceType->GetMethodByDecl(TCHAR_TO_ANSI(*Declaration)) == nullptr)
            Binds.Method(Declaration, [](UObject* Object) -> UObject* { return Object; });
        return true;
    }
	if (InterfaceType == nullptr || ParentType == nullptr
		|| !InterfaceType->IsInterface() || !ParentType->IsInterface()
		|| InterfaceType == ParentType || ParentType->Implements(InterfaceType))
	{
		return false;
	}
	if (!InterfaceType->Implements(ParentType))
		InterfaceType->interfaces.PushLast(ParentType);
	return true;
}

int NativeInterfaceMethod(FAngelscriptBinds& Binds, FBindString Signature)
{
	auto& Manager = FAngelscriptManager::Get();
	const int FunctionId = Manager.Engine->RegisterInterfaceMethod(
		Binds.GetTypeInfo()->GetName(), Signature.ToCString());
	check(FunctionId >= 0 || FunctionId == asALREADY_REGISTERED);
	return FunctionId;
}

enum class EVisitState : uint8
{
	Unseen,
	Visiting,
	Visited,
};

bool CollectDirectParents(
	UClass* InterfaceClass,
	TArray<UClass*>& OutParents,
	FString& OutError)
{
	if (InterfaceClass == nullptr)
	{
		OutError = TEXT("Native interface parent collection received a null class.");
		return false;
	}

	auto AddParent = [&OutParents, &OutError, InterfaceClass](UClass* ParentClass)
	{
		if (ParentClass == nullptr)
		{
			OutError = FString::Printf(
				TEXT("Native interface parent is missing: %s -> <null>"),
				*InterfaceClass->GetPathName());
			return false;
		}
		if (ParentClass->GetSuperClass() == UObject::StaticClass())
			return true;
		if (!ParentClass->HasAllClassFlags(CLASS_Interface | CLASS_Native))
		{
			OutError = FString::Printf(
				TEXT("Native interface parent is invalid: %s -> %s"),
				*InterfaceClass->GetPathName(),
				*ParentClass->GetPathName());
			return false;
		}
		OutParents.AddUnique(ParentClass);
		return true;
	};

	UClass* SuperClass = InterfaceClass->GetSuperClass();
	if (SuperClass != nullptr
		&& SuperClass->HasAnyClassFlags(CLASS_Interface)
		&& !AddParent(SuperClass))
	{
		return false;
	}
	for (const FImplementedInterface& Implemented : InterfaceClass->Interfaces)
	{
		if (!AddParent(Implemented.Class))
			return false;
	}
	OutParents.Sort([](const UClass& Left, const UClass& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});
	return true;
}

bool BuildClosurePlan(
	UClass* InterfaceClass,
	TMap<UClass*, EVisitState>& VisitStates,
	TArray<UClass*>& OutOrderedInterfaces,
	FString& OutError)
{
	const EVisitState CurrentState = VisitStates.FindRef(InterfaceClass);
	if (CurrentState == EVisitState::Visited)
		return true;
	if (CurrentState == EVisitState::Visiting)
	{
		OutError = FString::Printf(
			TEXT("Native interface inheritance cycle reaches: %s"),
			*InterfaceClass->GetPathName());
		return false;
	}

	VisitStates.Add(InterfaceClass, EVisitState::Visiting);
	TArray<UClass*> OrderedParents;
	if (!CollectDirectParents(InterfaceClass, OrderedParents, OutError))
		return false;
	for (UClass* ParentClass : OrderedParents)
	{
		if (VisitStates.FindRef(ParentClass) == EVisitState::Visiting)
		{
			OutError = FString::Printf(
				TEXT("Native interface inheritance cycle: %s -> %s"),
				*InterfaceClass->GetPathName(),
				*ParentClass->GetPathName());
			return false;
		}
		if (!BuildClosurePlan(
			ParentClass, VisitStates, OutOrderedInterfaces, OutError))
		{
			return false;
		}
	}
	VisitStates.Add(InterfaceClass, EVisitState::Visited);
	OutOrderedInterfaces.Add(InterfaceClass);
	return true;
}

TArray<UClass*> RequireClosurePlan(UClass* SelectedRoot)
{
	TMap<UClass*, EVisitState> VisitStates;
	TArray<UClass*> OrderedInterfaces;
	FString Error;
	if (!BuildClosurePlan(
		SelectedRoot, VisitStates, OrderedInterfaces, Error))
	{
		UE_LOG(Angelscript, Fatal, TEXT("%s"), *Error);
	}
	return OrderedInterfaces;
}

void BindParents(
	TSharedRef<FAngelscriptType> Type,
	UClass* InterfaceClass)
{
	TArray<UClass*> OrderedParents;
	FString Error;
	if (!CollectDirectParents(InterfaceClass, OrderedParents, Error))
		UE_LOG(Angelscript, Fatal, TEXT("%s"), *Error);

	FAngelscriptBinds Binds = FAngelscriptBinds::ExistingClass(
		Type->GetAngelscriptTypeName());
	for (UClass* ParentClass : OrderedParents)
	{
		const TSharedPtr<FAngelscriptType> ParentType =
			FAngelscriptType::GetByClass(ParentClass);
		if (!ParentType.IsValid()
			|| !NativeInterfaceInherits(Binds, 
				ParentType->GetAngelscriptTypeName()))
		{
			UE_LOG(
				Angelscript,
				Fatal,
				TEXT("Native interface inheritance registration failed: %s -> %s"),
				*InterfaceClass->GetPathName(),
				*ParentClass->GetPathName());
		}
	}
}

void BindReflectedMethods(
	TSharedRef<FAngelscriptType> Type,
	UClass* InterfaceClass,
	const FAngelscriptNativeInterfaceSelectionContext& Context,
	FEventDispatcher DispatchEvent,
    TMap<UClass*, TMap<FString, UFunction*>>& EventsByScriptName)
{
	FAngelscriptBinds Binds = FAngelscriptBinds::ExistingClass(
		Type->GetAngelscriptTypeName());
	TSet<FString> BoundDeclarations;
	for (TFieldIterator<UFunction> It(InterfaceClass); It; ++It)
	{
		UFunction* Function = *It;
		if (Function->HasAnyFunctionFlags(FUNC_Static | FUNC_NetFuncFlags)
			|| !Function->HasAnyFunctionFlags(
				FUNC_BlueprintEvent | FUNC_BlueprintCallable
				| FUNC_BlueprintPure))
		{
			continue;
		}

#if WITH_EDITOR
        Context.VisitReflectedMethod(Function, [&](UFunction*, const FAngelscriptBlueprintEventBindingSnapshot& Signature,
            TFunctionRef<void()> Bind)
        {
            if (BoundDeclarations.Contains(Signature.Declaration)) return false;
            BoundDeclarations.Add(Signature.Declaration);
            if ((Binds.GetTypeInfo()->GetFlags() & asOBJ_SCRIPT_OBJECT) != 0)
                NativeInterfaceMethod(Binds, Signature.Declaration);
            else
                Bind();
            DispatchEvent(Type, Function, Signature, EventsByScriptName);
            return true;
        });
#else
        (void)Context;
        (void)DispatchEvent;
#endif
	}
}

#if AS_USE_BIND_DB
void BindDatabaseMethods(
	TSharedRef<FAngelscriptType> Type,
	const FAngelscriptNativeInterfaceSelectionContext& Context,
	FEventDispatcher DispatchEvent,
    TMap<UClass*, TMap<FString, UFunction*>>& EventsByScriptName)
{
	FAngelscriptBinds Binds = FAngelscriptBinds::ExistingClass(
		Type->GetAngelscriptTypeName());
	TSet<FString> BoundDeclarations;
    for (int32 Index = 0; Index < Context.DatabaseMethodCount; ++Index)
    {
        const FString Declaration = Context.GetDatabaseMethodDeclaration(Index);
        if (Declaration.IsEmpty() || BoundDeclarations.Contains(Declaration)) continue;
        BoundDeclarations.Add(Declaration);
        if ((Binds.GetTypeInfo()->GetFlags() & asOBJ_SCRIPT_OBJECT) != 0)
            NativeInterfaceMethod(Binds, Declaration);
        Context.VisitDatabaseMethod(Index, [&](UFunction* Function,
            const FAngelscriptBlueprintEventBindingSnapshot& Signature, TFunctionRef<void()> Bind)
        {
            if ((Binds.GetTypeInfo()->GetFlags() & asOBJ_SCRIPT_OBJECT) == 0) Bind();
            DispatchEvent(Type, Function, Signature, EventsByScriptName);
            return false;
        });
	}
}
#endif
}

asITypeInfo* ReferenceType(
	UClass* InterfaceClass,
	const FString& TypeName)
{
    FAngelscriptTypeGenerationContext Context;
    Context.Point = EAngelscriptTypeGenerationPoint::ReferenceType;
    Context.ReflectionClass = InterfaceClass;
    FString Error;
    if (!DispatchAngelscriptTypeGeneration(Context, Error))
        UE_LOG(Angelscript, Fatal, TEXT("Interface reference type selection failed: %s"), *Error);
	return ReferenceNativeInterface(
		TypeName, InterfaceClass, Context.bUseObjectView).GetTypeInfo();
}

void BindClosure(
	UClass* SelectedRoot,
	const FString& SelectedRootTypeName,
	FTypeRegistrar RegisterType)
{
	if (SelectedRoot == nullptr
		|| !SelectedRoot->HasAllClassFlags(CLASS_Interface | CLASS_Native)
		|| SelectedRootTypeName.IsEmpty())
	{
		UE_LOG(
			Angelscript,
			Fatal,
			TEXT("Native interface selected root is invalid: %s"),
			SelectedRoot != nullptr
				? *SelectedRoot->GetPathName()
				: TEXT("<null>"));
	}

	const TArray<UClass*> OrderedInterfaces = RequireClosurePlan(SelectedRoot);
	for (UClass* InterfaceClass : OrderedInterfaces)
	{
		TSharedPtr<FAngelscriptType> InterfaceType =
			FAngelscriptType::GetByClass(InterfaceClass);
		if (!InterfaceType.IsValid())
		{
			const FString TypeName = InterfaceClass == SelectedRoot
				? SelectedRootTypeName
				: FAngelscriptType::GetBoundClassName(InterfaceClass);
			if (TypeName.IsEmpty())
			{
				UE_LOG(
					Angelscript,
					Fatal,
					TEXT("Native interface parent has no canonical type name: %s"),
					*InterfaceClass->GetPathName());
			}
			ReferenceType(InterfaceClass, TypeName);
            RegisterType(InterfaceClass, TypeName);
			InterfaceType = FAngelscriptType::GetByClass(InterfaceClass);
		}
		if (!InterfaceType.IsValid())
		{
			UE_LOG(
				Angelscript,
				Fatal,
				TEXT("Native interface parent is not bound: %s"),
				*InterfaceClass->GetPathName());
		}
	}

	for (UClass* InterfaceClass : OrderedInterfaces)
	{
		const TSharedPtr<FAngelscriptType> InterfaceType =
			FAngelscriptType::GetByClass(InterfaceClass);
		if (!InterfaceType.IsValid())
		{
			UE_LOG(
				Angelscript,
				Fatal,
				TEXT("Native interface parent verifier is missing: %s"),
				*InterfaceClass->GetPathName());
		}
		BindParents(InterfaceType.ToSharedRef(), InterfaceClass);
	}
}

bool PrepareDatabaseClass(
	UClass* Class,
	TFunctionRef<bool(UClass*)> ResolveDatabaseClass)
{
	if (!IsInterface(Class))
		return false;


	const TArray<UClass*> OrderedInterfaces = RequireClosurePlan(Class);
	for (UClass* InterfaceClass : OrderedInterfaces)
	{
        if (!ResolveDatabaseClass(InterfaceClass))
            UE_LOG(Angelscript, Fatal, TEXT("Native interface DB record is missing: %s -> %s"),
                *Class->GetPathName(), *InterfaceClass->GetPathName());
	}
	return false;
}

bool BindDatabaseMethodsIfInterface(
	UClass* Class,
	const TSharedPtr<FAngelscriptType>& Type,
	const FAngelscriptNativeInterfaceSelectionContext& Context,
	FEventDispatcher DispatchEvent,
    TMap<UClass*, TMap<FString, UFunction*>>& EventsByScriptName)
{
	if (!IsInterface(Class))
		return false;
#if AS_USE_BIND_DB
	if (Type.IsValid())
	{
		BindParents(Type.ToSharedRef(), Class);
		BindDatabaseMethods(Type.ToSharedRef(), Context, DispatchEvent, EventsByScriptName);
	}
#else
	(void)Type;
	(void)Context;
	(void)DispatchEvent;
#endif
	return true;
}

bool IsInterface(UClass* Class)
{
	return Class != nullptr && Class->HasAnyClassFlags(CLASS_Interface);
}

bool BindRuntimeMethodsIfInterface(
	UClass* Class,
	TSharedRef<FAngelscriptType> Type,
	const FAngelscriptNativeInterfaceSelectionContext& Context,
	FEventDispatcher DispatchEvent,
    TMap<UClass*, TMap<FString, UFunction*>>& EventsByScriptName)
{
	if (!IsInterface(Class))
		return false;
	BindParents(Type, Class);
	BindReflectedMethods(Type, Class, Context, DispatchEvent, EventsByScriptName);
	return true;
}

bool FinalizeRuntimeInterfaceRecord(
	UClass* Class,
	TSharedRef<FAngelscriptType> Type,
	TFunctionRef<void()> FinalizeRecord)
{
	if (!IsInterface(Class))
		return false;
	FinalizeRecord();
	return true;
}
}

#else
#endif
