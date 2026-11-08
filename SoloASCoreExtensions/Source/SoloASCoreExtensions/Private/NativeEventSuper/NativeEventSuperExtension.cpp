// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "NativeEventSuperExtension.h"
#include "AngelscriptBinds.h"
#include "AngelscriptBindDatabase.h"
#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/GCObject.h"
#include "UObject/StructOnScope.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "as_context.h"
#include "EndAngelscriptHeaders.h"
namespace SoloASCoreExtensions::NativeEventSuper
{
namespace
{
struct FAlias
{
    TStrongObjectPtr<UFunction> Function;
    FString ScriptName;
    FString AliasName;
};
struct FNativeParameters final : FStructOnScope, FGCObject
{
    explicit FNativeParameters(UFunction* Function) : FStructOnScope(Function) {}
    void AddReferencedObjects(FReferenceCollector& Collector) override { FStructOnScope::AddReferencedObjects(Collector); }
    FString GetReferencerName() const override { return TEXT("SoloAS.NativeEventSuper.Parameters"); }
};
TArray<TUniquePtr<FAlias>> Aliases;
FDelegateHandle SourceHandle;
uint8 AliasCleanupTag;
bool Enabled()
{
    static const bool Value = [] { bool On = true; if (GConfig) GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableNativeEventSuper"), On, GEngineIni); return On; }();
    return Value;
}
void Invoke(asIScriptGeneric* Call)
{
    auto* Alias = static_cast<FAlias*>(Call->GetFunction()->GetUserData());
    auto* Target = static_cast<UObject*>(Call->GetObject());
    UFunction* Function = Alias ? Alias->Function.Get() : nullptr;
    if (!IsInGameThread() || !Target || !Function || !Target->IsA(Function->GetOuterUClass()))
    { FAngelscriptManager::Throw("Invalid native event super receiver."); return; }
    bool MatchingOverride = false;
    if (auto* Context = FAngelscriptManager::GetCurrentScriptContext())
        for (asUINT Frame = 0; Frame < Context->GetCallstackSize() && !MatchingOverride; ++Frame)
        {
            auto* Caller = Context->GetFunction(Frame);
            if (!Caller || Caller->GetFuncType() == asFUNC_SYSTEM || !Caller->GetModule()) continue;
            auto Module = FAngelscriptManager::Get().GetModule(Caller->GetModule());
            auto Class = Module ? Module->GetClass(Caller->GetObjectType()) : nullptr;
            if (!Class || !Class->Class || !Target->IsA(Class->Class)) continue;
            for (const auto& Method : Class->Methods)
                if (Method->ScriptFunction == Caller && Method->bBlueprintOverride
                    && Method->OriginalFunctionName == Alias->ScriptName && Method->FunctionName == Function->GetName())
                    MatchingOverride = true;
        }
    if (!MatchingOverride)
    { FAngelscriptManager::Throw("Native event super requires its matching script override."); return; }
    FNativeParameters Parameters(Function);
    TArray<FProperty*> Arguments;
    FProperty* Return = nullptr;
    for (TFieldIterator<FProperty> It(Function); It; ++It)
    {
        if (!It->HasAnyPropertyFlags(CPF_Parm)) continue;
        if (It->HasAnyPropertyFlags(CPF_ReturnParm)) Return = *It;
        else Arguments.Add(*It);
    }
    if (Arguments.Num() != int32(Call->GetArgCount()))
    { FAngelscriptManager::Throw("Native event super signature differs."); return; }
    for (int32 Index = 0; Index < Arguments.Num(); ++Index)
    {
        asDWORD Flags = asTM_NONE;
        Call->GetArgTypeId(Index, &Flags);
        void* Source = (Flags & asTM_INOUTREF) ? Call->GetArgAddress(Index) : Call->GetAddressOfArg(Index);
        if (Arguments[Index]->HasAnyPropertyFlags(CPF_OutParm) && !Arguments[Index]->HasAnyPropertyFlags(CPF_ConstParm)
            && !(Flags & asTM_INOUTREF))
        { FAngelscriptManager::Throw("Native event super output requires a VM reference."); return; }
        Arguments[Index]->CopyCompleteValue(Arguments[Index]->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory()), Source);
    }
    void* ReturnAddress = Return ? Return->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory()) : nullptr;
    FFrame Stack(Target, Function, Parameters.GetStructMemory(), nullptr, Function->ChildProperties);
    TArray<FOutParmRec, TInlineAllocator<16>> Outputs;
    Outputs.Reserve(Arguments.Num());
    for (FProperty* Argument : Arguments)
        if (Argument->HasAnyPropertyFlags(CPF_OutParm))
        {
            auto& Output = Outputs.AddDefaulted_GetRef();
            Output.Property = Argument;
            Output.PropAddr = Argument->ContainerPtrToValuePtr<uint8>(Parameters.GetStructMemory());
        }
    for (int32 Index = 0; Index < Outputs.Num(); ++Index)
        Outputs[Index].NextOutParm = Index + 1 < Outputs.Num() ? &Outputs[Index + 1] : nullptr;
    Stack.OutParms = Outputs.IsEmpty() ? nullptr : Outputs.GetData();
    // Invoke the native thunk directly. ProcessEvent would redispatch into the script override.
    Function->Invoke(Target, Stack, ReturnAddress);
    for (int32 Index = 0; Index < Arguments.Num(); ++Index)
        if (Arguments[Index]->HasAnyPropertyFlags(CPF_OutParm) && !Arguments[Index]->HasAnyPropertyFlags(CPF_ConstParm))
            Arguments[Index]->CopyCompleteValue(Call->GetArgAddress(Index), Arguments[Index]->ContainerPtrToValuePtr<void>(Parameters.GetStructMemory()));
    if (Return)
    {
        auto Usage = FAngelscriptTypeUsage::FromProperty(Return);
        void* Out = Call->GetAddressOfReturnLocation();
        if (Usage.NeedConstruct()) Usage.ConstructValue(Out);
        Return->CopyCompleteValue(Out, ReturnAddress);
    }
}
void Rewrite(FAngelscriptPreprocessor& Preprocessor)
{
    for (auto& File : Preprocessor.Files)
        for (auto& Chunk : File.ChunkedCode)
        {
            if (!Chunk.ClassDesc || !Chunk.ClassDesc->bSuperIsCodeClass) continue;
            UClass* Native = Chunk.ClassDesc->CodeSuperClass;
            if (!Native) continue;
            // Scan lexical code only; do not touch comments, strings, or other identifiers.
            bool LineComment = false, BlockComment = false;
            TCHAR Quote = 0;
            for (int32 Index = 0; Index < Chunk.Content.Len(); ++Index)
            {
                TCHAR C = Chunk.Content[Index];
                TCHAR Next = Index + 1 < Chunk.Content.Len() ? Chunk.Content[Index + 1] : 0;
                if (LineComment) { if (C == '\n') LineComment = false; continue; }
                if (BlockComment) { if (C == '*' && Next == '/') { BlockComment = false; ++Index; } continue; }
                if (Quote) { if (C == '\\') ++Index; else if (C == Quote) Quote = 0; continue; }
                if (C == '/' && Next == '/') { LineComment = true; ++Index; continue; }
                if (C == '/' && Next == '*') { BlockComment = true; ++Index; continue; }
                if (C == '"' || C == '\'') { Quote = C; continue; }
                if (Index > 0 && (FChar::IsAlnum(Chunk.Content[Index - 1]) || Chunk.Content[Index - 1] == '_')) continue;
                for (const auto& Alias : Aliases)
                {
                    if (!Native->IsChildOf(Alias->Function->GetOuterUClass())) continue;
                    const FString Token = TEXT("Super::") + Alias->ScriptName;
                    if (Chunk.Content.Mid(Index, Token.Len()) != Token) continue;
                    int32 Open = Index + Token.Len();
                    while (Open < Chunk.Content.Len() && FChar::IsWhitespace(Chunk.Content[Open])) ++Open;
                    if (Open == Chunk.Content.Len() || Chunk.Content[Open] != '(') continue;
                    Preprocessor.ReplaceInChunk(Chunk, Index, Index + Token.Len(), Alias->AliasName);
                    Index += Token.Len() - 1;
                    break;
                }
            }
        }
}
AS_FORCE_LINK const FAngelscriptBinds::FBind RegisterAliases(int32(FAngelscriptBinds::EOrder::Late) + 200, []
{
    if (!Enabled()) return;
    auto* Engine = FAngelscriptManager::Get().GetScriptEngine();
    const asPWORD CleanupKey = reinterpret_cast<asPWORD>(&AliasCleanupTag);
    Engine->SetUserData(&Aliases, CleanupKey);
    Engine->SetEngineUserDataCleanupCallback([](asIScriptEngine*) { Aliases.Reset(); }, CleanupKey);
    for (const auto& Class : FAngelscriptBindDatabase::Get().Classes)
        for (const auto& Method : Class.Methods)
        {
            // EBindTargetType::Method; the public database stores its stable integral value.
            if (Method.BindTargetType != 2 || Method.WorldContextArgument >= 0) continue;
            const auto CanonicalType = FAngelscriptType::GetByAngelscriptTypeName(Class.TypeName);
            UClass* Owner = CanonicalType ? CanonicalType->GetClass(FAngelscriptTypeUsage::DefaultUsage) : nullptr;
            UFunction* Function = Owner ? Owner->FindFunctionByName(FName(*Method.UnrealPath)) : nullptr;
            if (!Function || !Function->HasAllFunctionFlags(FUNC_Native | FUNC_BlueprintEvent)
                || Function->HasAnyFunctionFlags(FUNC_Static | FUNC_NetFuncFlags)
                || !Function->GetOuterUClass()->HasAnyClassFlags(CLASS_Native)) continue;
            const FString ScriptName = Method.ScriptName.IsEmpty() ? Function->GetName() : Method.ScriptName;
            const FString Token = ScriptName + TEXT("(");
            const int32 Offset = Method.Declaration.Find(Token, ESearchCase::CaseSensitive);
            if (Offset == INDEX_NONE) continue;
            auto Alias = MakeUnique<FAlias>();
            Alias->Function.Reset(Function);
            Alias->ScriptName = ScriptName;
            Alias->AliasName = TEXT("__CE_NativeSuper_") + ScriptName;
            FString Declaration = Method.Declaration;
            Declaration.RemoveAt(Offset, ScriptName.Len());
            Declaration.InsertAt(Offset, Alias->AliasName);
            const int32 Id = FAngelscriptBinds::BindMethodDirect(Class.TypeName, Declaration, asFUNCTION(Invoke), asCALL_GENERIC, FASBindFunctionPointers{}, Alias.Get());
            check(Id >= 0);
            Aliases.Add(MoveTemp(Alias));
        }
});
}
bool IsEnabled() { return Enabled(); }
void Start() { if (Enabled() && !SourceHandle.IsValid()) SourceHandle = FAngelscriptPreprocessor::OnProcessChunks.AddStatic(&Rewrite); }
bool IsEligible(UFunction* Function)
{
    return Function && Function->HasAllFunctionFlags(FUNC_Native | FUNC_BlueprintEvent)
        && !Function->HasAnyFunctionFlags(FUNC_Static | FUNC_NetFuncFlags)
        && Function->GetOuterUClass()->HasAnyClassFlags(CLASS_Native);
}
void Stop() { FAngelscriptPreprocessor::OnProcessChunks.Remove(SourceHandle); SourceHandle.Reset(); }
}
