                                                                                                            
#include "ClassGenerator/AngelscriptTerminalDestructorAdmission.h"
                                                                                                          
#include "StructUtils/InstancedStruct.h"
#include "AngelscriptBinds/Bind_Helpers.h"
#include "ClassGenerator/ASStruct.h"

#include "AngelscriptDocs.h"
#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"

void FAngelscriptInstancedStructHelpers::InitializeAs_Struct(FInstancedStruct* Self, void* Data, const int TypeId)
{
	const UStruct* StructDef = FAngelscriptManager::Get().GetUnrealStructFromAngelscriptTypeId(TypeId);
	if (StructDef == nullptr)
	{
		FAngelscriptManager::Throw("Not a valid USTRUCT");
		return;
	}

	const UScriptStruct* ScriptStructDef = Cast<UScriptStruct>(StructDef);
	if (ScriptStructDef == nullptr)
	{
		FAngelscriptManager::Throw("Not a valid UScriptStruct");
		return;
	}

	Self->InitializeAs(ScriptStructDef, (uint8*)Data);
}

static FScriptStructWildcard InstancedStructEmptyWildcard;
FScriptStructWildcard& FAngelscriptInstancedStructHelpers::GetMemory(FInstancedStruct* Self, const UScriptStruct* StructType)
{
	if (!Self->IsValid())
	{
		FAngelscriptManager::Throw("Source is empty or not valid. Check `IsValid()` before trying to `Get()` the underlying struct.");
		return InstancedStructEmptyWildcard;
	}

	if (StructType != Self->GetScriptStruct())
	{
		const FString Debug =
			FString::Printf(TEXT("Mismatching types. FInstancedStruct contains a %s but tried to Get a %s."), *Self->GetScriptStruct()->GetStructCPPName(), *StructType->GetStructCPPName());
		FAngelscriptManager::Throw(TCHAR_TO_ANSI(*Debug));
		return InstancedStructEmptyWildcard;
	}

	return *(FScriptStructWildcard*)Self->GetMemory();
}

FInstancedStruct FAngelscriptInstancedStructHelpers::Make(void* Data, const int TypeId)
{
	const UStruct* StructDef = FAngelscriptManager::Get().GetUnrealStructFromAngelscriptTypeId(TypeId);
	if (StructDef == nullptr)
	{
		FAngelscriptManager::Throw("Not a valid USTRUCT");
		return FInstancedStruct();
	}

	const UScriptStruct* ScriptStructDef = Cast<UScriptStruct>(StructDef);
	if (ScriptStructDef == nullptr)
	{
		FAngelscriptManager::Throw("Not a valid UScriptStruct");
		return FInstancedStruct();
	}

	FInstancedStruct InstancedStruct;
	InstancedStruct.InitializeAs(ScriptStructDef, (uint8*)Data);
	return InstancedStruct;
}

void FAngelscriptInstancedStructHelpers::ImplicitConstructAnyStruct(FAngelscriptAnyStructParameter* Self, void* Data, const int TypeId)
{
	new(Self) FAngelscriptAnyStructParameter();

	const UStruct* StructDef = FAngelscriptManager::Get().GetUnrealStructFromAngelscriptTypeId(TypeId);
	if (StructDef == nullptr)
	{
		FAngelscriptManager::Throw("Not a valid USTRUCT");
		return;
	}

	const UScriptStruct* ScriptStructDef = Cast<UScriptStruct>(StructDef);
	if (ScriptStructDef == nullptr)
	{
		FAngelscriptManager::Throw("Not a valid UScriptStruct");
		return;
	}

	Self->InstancedStruct.InitializeAs(ScriptStructDef, (uint8*)Data);
}

void FAngelscriptInstancedStructHelpers::ImplicitConstructAnyStructFromInstancedStruct(FAngelscriptAnyStructParameter* Self, const FInstancedStruct& InstancedStruct)
{
	new(Self) FAngelscriptAnyStructParameter();
	Self->InstancedStruct = InstancedStruct;
}

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FInstancedStruct(
	FAngelscriptBinds::EOrder::Late,
	[]
	{
		auto FAngelscriptAnyStructParameter_ = FAngelscriptBinds::ExistingClass("FAngelscriptAnyStructParameter");
		FAngelscriptAnyStructParameter_.ImplicitConstructor("void f(const ?&in Struct)", FUNC(FAngelscriptInstancedStructHelpers::ImplicitConstructAnyStruct));
		FAngelscriptBinds::SetPreviousBindNoDiscard(true);
		FAngelscriptAnyStructParameter_.ImplicitConstructor("void f(const FInstancedStruct& Struct)", FUNC(FAngelscriptInstancedStructHelpers::ImplicitConstructAnyStructFromInstancedStruct));
		FAngelscriptBinds::SetPreviousBindNoDiscard(true);

		auto FInstancedStruct_ = FAngelscriptBinds::ExistingClass("FInstancedStruct");

		FInstancedStruct_.Method("bool opEquals(const FInstancedStruct& Other) const", METHODPR(bool, FInstancedStruct, operator==, (const FInstancedStruct&) const));
		SCRIPT_BIND_DOCUMENTATION("Comparison operators. Deep compares the struct instance when identical.")

		FInstancedStruct_.Method("void InitializeAs(const ?&in Struct)", FUNC(FAngelscriptInstancedStructHelpers::InitializeAs_Struct));
                                                                                                                        
		Cuviss::TerminalAdmission::RegisterNativeLeaf(FAngelscriptBinds::GetPreviousBind(),
			Cuviss::TerminalAdmission::ENativeLeaf::InstancedStructInitializeAs);
                                                                                                                      
		SCRIPT_BIND_DOCUMENTATION("Initializes from struct type and emplace construct.")

		FInstancedStruct_.Method("void InitializeAs(const UScriptStruct StructType)", FUNC(FAngelscriptInstancedStructHelpers::InitializeAs_Default));
		SCRIPT_BIND_DOCUMENTATION("Default initializes a struct of this type")

		FInstancedStruct_.Method("const FScriptStructWildcard& Get(const UScriptStruct StructType) const no_discard", FUNC(FAngelscriptInstancedStructHelpers::GetMemory));
		SCRIPT_BIND_DOCUMENTATION("Returns struct data of a particular type. Throws an exception if the instanced struct does not contain a struct of this type.");
		FAngelscriptBinds::SetPreviousBindArgumentDeterminesOutputType(0);

		FInstancedStruct_.Method("FScriptStructWildcard& GetMutable(const UScriptStruct StructType) no_discard", FUNC(FAngelscriptInstancedStructHelpers::GetMemory));
		SCRIPT_BIND_DOCUMENTATION("Returns struct data of a particular type. Throws an exception if the instanced struct does not contain a struct of this type.");
		FAngelscriptBinds::SetPreviousBindArgumentDeterminesOutputType(0);

		FInstancedStruct_.Method(
			"void Get(?&out Struct) const",
			[](const FInstancedStruct* Self, void* Data, int TypeId)
			{
				if (!Self->IsValid())
				{
					FAngelscriptManager::Throw("Source is empty or not valid. Check `IsValid()` before trying to `Get()` the underlying struct.");
					return;
				}

				const UStruct* StructDef = FAngelscriptManager::Get().GetUnrealStructFromAngelscriptTypeId(TypeId);
				if (StructDef == nullptr)
				{
					FAngelscriptManager::Throw("Not a valid USTRUCT");
					return;
				}

				const UScriptStruct* ScriptStructDef = Cast<UScriptStruct>(StructDef);
				if (ScriptStructDef == nullptr)
				{
					FAngelscriptManager::Throw("Not a valid UScriptStruct");
					return;
				}

				if (ScriptStructDef != Self->GetScriptStruct())
				{
					const FString Debug =
						FString::Printf(TEXT("\nMismatching types. Got %s but expected %s."), *ScriptStructDef->GetStructCPPName(), *Self->GetScriptStruct()->GetStructCPPName());
					FAngelscriptManager::Throw(TCHAR_TO_ANSI(*Debug));
					return;
				}

				// This is making a copy, which we'd prefer to avoid, but the alternative is very complicated (see TOptional)
				ScriptStructDef->CopyScriptStruct(Data, Self->GetMemory());
			});
		SCRIPT_BIND_DOCUMENTATION("Returns a copy of the struct. This getter assumes that all data is valid.");
		FInstancedStruct_.DeprecatePreviousBind("Use Get() or GetMutable() that returns a reference instead of copying");

		FInstancedStruct_.Method("void Reset()", METHOD(FInstancedStruct, Reset));
                                                                                                                 
		Cuviss::TerminalAdmission::RegisterNativeLeaf(FAngelscriptBinds::GetPreviousBind(),
			Cuviss::TerminalAdmission::ENativeLeaf::InstancedStructReset);
                                                                                                               

		FInstancedStruct_.Method("bool Contains(const UScriptStruct StructType) const", FUNC(FAngelscriptInstancedStructHelpers::Contains));
		SCRIPT_BIND_DOCUMENTATION("Check whether the instanced struct contains a struct of this type");

		FInstancedStruct_.Method("bool IsValid() const", METHOD_TRIVIAL(FInstancedStruct, IsValid));
                                                                                                                   
		Cuviss::TerminalAdmission::RegisterNativeLeaf(FAngelscriptBinds::GetPreviousBind(),
			Cuviss::TerminalAdmission::ENativeLeaf::InstancedStructIsValid);
                                                                                                                 
		SCRIPT_BIND_DOCUMENTATION("Returns True if the struct is valid.")

		FInstancedStruct_.Method("UScriptStruct GetScriptStruct() const", METHOD_TRIVIAL(FInstancedStruct, GetScriptStruct));
		SCRIPT_BIND_DOCUMENTATION("Get the type of struct contained within the instanced struct")

		{
			FAngelscriptBinds::FNamespace ns("FInstancedStruct");

			FAngelscriptBinds::BindGlobalFunction("FInstancedStruct Make(const ?&in Struct) no_discard", FUNC(FAngelscriptInstancedStructHelpers::Make));
			SCRIPT_BIND_DOCUMENTATION("Creates a new FInstancedStruct from struct.")
		}
	});
