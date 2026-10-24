#pragma once
#include "CoreMinimal.h"

#if WITH_EDITOR
#define AS_DOC(FunctionId, Documentation) FAngelscriptDocs::AddUnrealDocumentation(FunctionId, Documentation);
#define SCRIPT_BIND_DOCUMENTATION(Documentation) FAngelscriptDocs::AddUnrealDocumentation(FAngelscriptBinds::GetPreviousFunctionId(), TEXT(Documentation), TEXT(""), nullptr);
#define SCRIPT_GLOBAL_DOCUMENTATION(Documentation) FAngelscriptDocs::AddDocumentationForGlobalVariable(FAngelscriptBinds::GetPreviousGlobalVariableId(), TEXT(Documentation));
#define SCRIPT_PROPERTY_DOCUMENTATION(Binds, Documentation) FAngelscriptDocs::AddUnrealDocumentationForProperty(Binds.GetTypeId(), FAngelscriptBinds::GetPreviousPropertyOffset(), TEXT(Documentation));
#define SCRIPT_MANUAL_BIND_META(MetaName, MetaValue) FAngelscriptDocs::AddScriptFunctionMeta(FAngelscriptBinds::GetPreviousFunctionId(), TEXT(MetaName), TEXT(MetaValue));
#else
#define AS_DOC(...) 
#define SCRIPT_BIND_DOCUMENTATION(...)
#define SCRIPT_GLOBAL_DOCUMENTATION(...) 
#define SCRIPT_PROPERTY_DOCUMENTATION(...) 
#define SCRIPT_MANUAL_BIND_META(...) 
#endif

class asIScriptEngine;

struct ANGELSCRIPTCODE_API FAngelscriptDocs
{
	static void AddUnrealDocumentation(int FunctionId, FStringView Documentation, FStringView Category, UFunction* Function);
	static void AddUnrealDocumentationForType(int TypeId, FStringView Documentation);
	static void AddUnrealDocumentationForProperty(int TypeId, int PropertyOffset, FStringView Documentation);
	static void AddDocumentationForGlobalVariable(int GlobalVariableId, FStringView Documentation);
	static void AddScriptFunctionMeta(int FunctionId, FStringView MetaName, FStringView MetaValue);

	static const FString& GetUnrealDocumentation(int FunctionId);
	static const struct FPassedDoc& GetFullUnrealDocumentation(int FunctionId);
	static void DumpDocumentation(asIScriptEngine* Engine);

	static const FString& GetUnrealDocumentationForType(int TypeId);
	static const FString& GetUnrealDocumentationForProperty(int TypeId, int PropertyOffset);
	static const FString& GetDocumentationForGlobalVariable(int GlobalVariableId);
	static const TMap<FString, FString>* GetScriptFunctionMeta(int FunctionId);

	static UFunction* LookupAngelscriptFunction(int FunctionId);
};