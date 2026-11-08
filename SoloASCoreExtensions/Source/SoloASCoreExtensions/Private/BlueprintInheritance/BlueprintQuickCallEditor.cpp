// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "BlueprintQuickCallEditor.h"

#if WITH_EDITOR
#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "ToolMenus.h"
#include "Internationalization/PolyglotTextData.h"
#include "Internationalization/TextLocalizationManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "StartAngelscriptHeaders.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

namespace
{
const FName MenuOwner(TEXT("SoloAS.CoreExtensions.BlueprintQuickCalls"));
FDelegateHandle MenuStartupHandle;
bool bStarted = false;

bool CanScan()
{
    return GEditor && !GEditor->PlayWorld && !IsLoading() && !IsAsyncLoading()
        && FAngelscriptManager::IsInitialized() && !FAngelscriptManager::bIsHotReloading;
}

UClass* FindReceiverClass(const FString& TypeName)
{
    TSharedPtr<FAngelscriptModuleDesc> Module;
    TSharedPtr<FAngelscriptClassDesc> ClassDesc = FAngelscriptManager::Get().GetClass(TypeName, &Module);
    if (ClassDesc.IsValid() && ClassDesc->Class != nullptr)
        return ClassDesc->Class;

    TSharedPtr<FAngelscriptType> Type = FAngelscriptType::GetByAngelscriptTypeName(TypeName);
    return Type.IsValid() ? Type->GetClass(FAngelscriptTypeUsage::DefaultUsage) : nullptr;
}

void AddDeclaredVariable(TMap<FString, FString>& Types, FString Declaration)
{
    Declaration = Declaration.TrimStartAndEnd();
    Declaration.RemoveFromStart(TEXT("const "));
    Declaration.RemoveFromStart(TEXT("@"));
    Declaration.TrimStartAndEndInline();

    TArray<FString> Words;
    Declaration.ParseIntoArrayWS(Words);
    if (Words.Num() < 2)
        return;

    FString TypeName = Words[Words.Num() - 2];
    FString VariableName = Words.Last();
    TypeName.RemoveFromEnd(TEXT("@"));
    VariableName.RemoveFromEnd(TEXT(";"));
    VariableName.RemoveFromEnd(TEXT(","));
    if (!TypeName.IsEmpty() && !VariableName.IsEmpty())
        Types.Add(VariableName, TypeName);
}

TMap<FString, FString> FindLocalTypes(const FString& Source)
{
    TMap<FString, FString> Types;
    TArray<FString> Lines;
    Source.ParseIntoArrayLines(Lines);
    for (FString Line : Lines)
    {
        int32 Comment = INDEX_NONE;
        if (Line.FindChar(TEXT('/'), Comment) && Line.Mid(Comment).StartsWith(TEXT("//")))
            Line.LeftInline(Comment);

        const int32 OpenParen = Line.Find(TEXT("("));
        const int32 CloseParen = OpenParen == INDEX_NONE ? INDEX_NONE : Line.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenParen + 1);
        if (OpenParen != INDEX_NONE && CloseParen > OpenParen)
        {
            TArray<FString> Parameters;
            Line.Mid(OpenParen + 1, CloseParen - OpenParen - 1).ParseIntoArray(Parameters, TEXT(","), true);
            for (FString Parameter : Parameters)
                AddDeclaredVariable(Types, MoveTemp(Parameter));
        }

        int32 End = Line.Find(TEXT("="));
        if (End == INDEX_NONE)
            End = Line.Find(TEXT(";"));
        if (End != INDEX_NONE)
            AddDeclaredVariable(Types, Line.Left(End));
    }
    return Types;
}

TArray<FString> SplitCallArguments(const FString& Source)
{
    TArray<FString> Result;
    int32 Start = 0;
    int32 Depth = 0;
    TCHAR Quote = 0;
    for (int32 Index = 0; Index < Source.Len(); ++Index)
    {
        const TCHAR Character = Source[Index];
        if (Quote != 0)
        {
            if (Character == Quote && (Index == 0 || Source[Index - 1] != TEXT('\\')))
                Quote = 0;
            continue;
        }
        if (Character == TEXT('"') || Character == TEXT('\''))
            Quote = Character;
        else if (Character == TEXT('(') || Character == TEXT('[') || Character == TEXT('{'))
            ++Depth;
        else if (Character == TEXT(')') || Character == TEXT(']') || Character == TEXT('}'))
            --Depth;
        else if (Character == TEXT(',') && Depth == 0)
        {
            Result.Add(Source.Mid(Start, Index - Start).TrimStartAndEnd());
            Start = Index + 1;
        }
    }
    if (Start < Source.Len())
        Result.Add(Source.Mid(Start).TrimStartAndEnd());
    return Result;
}

bool FindCall(const FString& Source, const TCHAR* Name, int32& SearchIndex, TArray<FString>& OutArguments, int32& OutLineNumber)
{
	const int32 NameIndex = Source.Find(Name, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchIndex);
    if (NameIndex == INDEX_NONE)
        return false;
	const int32 OpenParen = Source.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromStart, NameIndex + FCString::Strlen(Name));
    if (OpenParen == INDEX_NONE)
        return false;

	OutLineNumber = 1;
	for (int32 Index = 0; Index < NameIndex; ++Index)
		if (Source[Index] == TEXT('\n')) ++OutLineNumber;
    int32 Depth = 1;
    TCHAR Quote = 0;
	for (int32 Index = OpenParen + 1; Index < Source.Len(); ++Index)
    {
		const TCHAR Character = Source[Index];
        if (Quote != 0)
        {
			if (Character == Quote && Source[Index - 1] != TEXT('\\'))
                Quote = 0;
            continue;
        }
        if (Character == TEXT('"') || Character == TEXT('\''))
            Quote = Character;
        else if (Character == TEXT('('))
            ++Depth;
        else if (Character == TEXT(')') && --Depth == 0)
        {
			OutArguments = SplitCallArguments(Source.Mid(OpenParen + 1, Index - OpenParen - 1));
			SearchIndex = Index + 1;
            return true;
        }
    }
	SearchIndex = Source.Len();
    return false;
}

FString GetExpressionType(const FString& Expression, const TMap<FString, FString>& LocalTypes)
{
    const FString Value = Expression.TrimStartAndEnd();
    if (Value == TEXT("true") || Value == TEXT("false")) return TEXT("bool");
    if (Value.StartsWith(TEXT("n\""))) return TEXT("FName");
    if (Value.StartsWith(TEXT("\""))) return TEXT("FString");

    bool bHasDigit = false;
    bool bHasDecimal = false;
    bool bNumeric = !Value.IsEmpty();
    for (TCHAR Character : Value)
    {
        if (FChar::IsDigit(Character)) bHasDigit = true;
        else if (Character == TEXT('.')) bHasDecimal = true;
        else if (Character != TEXT('-') && Character != TEXT('+') && Character != TEXT('f') && Character != TEXT('F')) bNumeric = false;
    }
    if (bNumeric && bHasDigit) return bHasDecimal ? TEXT("float32") : TEXT("int");

    if (const FString* TypeName = LocalTypes.Find(Value))
        return *TypeName;
    return FString();
}

bool IsStaticTypeCompatible(FAngelscriptManager& Manager, const FString& TypeName, FProperty* Property, bool bOutput = false)
{
    if (TypeName.IsEmpty() || Property == nullptr)
        return true;
    const int32 TypeId = Manager.Engine->GetTypeIdByDecl(TCHAR_TO_ANSI(*TypeName));
    if (TypeId < 0)
        return true;

    const FAngelscriptTypeUsage Actual = FAngelscriptTypeUsage::FromTypeId(TypeId);
    const FAngelscriptTypeUsage Expected = FAngelscriptTypeUsage::FromProperty(Property);
    if (!Actual.IsValid() || !Expected.IsValid())
        return true;
    if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
    {
        if (!Actual.IsObjectPointer() || !Expected.IsObjectPointer())
            return Actual.EqualsUnqualified(Expected);
        const UClass* ActualClass = Actual.GetClass();
        return ActualClass == nullptr || (bOutput
            ? ObjectProperty->PropertyClass->IsChildOf(ActualClass)
            : ActualClass->IsChildOf(ObjectProperty->PropertyClass));
    }
    if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        return Actual.GetUnrealStruct() == nullptr || Actual.GetUnrealStruct() == StructProperty->Struct;
    return Actual.EqualsUnqualified(Expected);
}

TArray<UClass*> GetLoadedReceiverClasses(UClass* StaticClass)
{
    TArray<UClass*> Result;
    if (StaticClass == nullptr)
        return Result;
    Result.Add(StaticClass);
    for (TObjectIterator<UBlueprint> It; It; ++It)
    {
        UClass* GeneratedClass = It->GeneratedClass;
        if (GeneratedClass != nullptr && GeneratedClass != StaticClass && GeneratedClass->IsChildOf(StaticClass))
            Result.AddUnique(GeneratedClass);
    }
    return Result;
}

void WarnQuickCall(const FString& Filename, int32 LineNumber, const FString& Message)
{
    UE_LOG(Angelscript, Warning, TEXT("%s:%d: Blueprint quick call: %s"), *Filename, LineNumber, *Message);
}

void ScanBlueprintQuickCalls()
{
    if (!FAngelscriptManager::IsInitialized())
        return;

    FAngelscriptManager& Manager = FAngelscriptManager::Get();
    for (const TSharedRef<FAngelscriptModuleDesc>& Module : Manager.GetActiveModules())
    {
        for (const FAngelscriptModuleDesc::FCodeSection& Code : Module->Code)
        {
            if (!Code.AbsoluteFilename.EndsWith(TEXT(".as")))
                continue;
            const TMap<FString, FString> LocalTypes = FindLocalTypes(Code.Code);
            const TCHAR* Helpers[] = {TEXT("CallBlueprint"), TEXT("GetBlueprintProperty"), TEXT("SetBlueprintProperty"), TEXT("BindBlueprintEvent")};
            for (const TCHAR* Helper : Helpers)
            {
                int32 SearchIndex = 0;
                int32 LineNumber = 1;
                TArray<FString> Arguments;
                while (FindCall(Code.Code, Helper, SearchIndex, Arguments, LineNumber))
                {
                    if (Arguments.Num() < 2)
                        continue;
                    if ((FCString::Strcmp(Helper, TEXT("CallBlueprint")) == 0 && Arguments.Num() < 2)
                        || ((FCString::Strcmp(Helper, TEXT("GetBlueprintProperty")) == 0
                            || FCString::Strcmp(Helper, TEXT("SetBlueprintProperty")) == 0) && Arguments.Num() < 3)
                        || (FCString::Strcmp(Helper, TEXT("BindBlueprintEvent")) == 0 && Arguments.Num() < 4))
                        continue;
                    const FString* ReceiverTypeName = LocalTypes.Find(Arguments[0]);
                    UClass* StaticClass = ReceiverTypeName != nullptr ? FindReceiverClass(*ReceiverTypeName) : nullptr;
                    if (StaticClass == nullptr)
                        continue;

                    const FString MemberNameLiteral = Arguments[1].TrimStartAndEnd();
                    const int32 QuoteStart = MemberNameLiteral.Find(TEXT("\""));
                    const int32 QuoteEnd = QuoteStart == INDEX_NONE
                        ? INDEX_NONE
                        : MemberNameLiteral.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                    if (!MemberNameLiteral.StartsWith(TEXT("n\"")) || QuoteStart == INDEX_NONE || QuoteEnd <= QuoteStart)
                        continue;
                    const FName MemberName(*MemberNameLiteral.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1));
                    const TArray<UClass*> ReceiverClasses = GetLoadedReceiverClasses(StaticClass);
                    bool bFoundMember = false;
                    FString Mismatch;

                    if (FCString::Strcmp(Helper, TEXT("CallBlueprint")) == 0)
                    {
                        for (UClass* Candidate : ReceiverClasses)
                        {
                            UFunction* Function = Candidate->FindFunctionByName(MemberName);
                            if (Function == nullptr)
                                continue;
                            bFoundMember = true;
                            TArray<FProperty*> Parameters;
                            FProperty* ReturnProperty = nullptr;
                            for (TFieldIterator<FProperty> It(Function); It; ++It)
                            {
                                if (!It->HasAnyPropertyFlags(CPF_Parm)) continue;
                                if (It->HasAnyPropertyFlags(CPF_ReturnParm)) ReturnProperty = *It;
                                else Parameters.Add(*It);
                            }
                            const int32 SuppliedArgumentCount = Arguments.Num() - 2;
                            const bool bHasReturnOutput = ReturnProperty != nullptr
                                && SuppliedArgumentCount == Parameters.Num() + 1;
                            const int32 ParameterArgumentStart = bHasReturnOutput ? 3 : 2;
                            if (SuppliedArgumentCount != Parameters.Num() && !bHasReturnOutput)
                            {
                                const FString Expected = ReturnProperty != nullptr
                                    ? FString::Printf(TEXT("%d arguments, or %d including a return slot"), Parameters.Num(), Parameters.Num() + 1)
                                    : FString::Printf(TEXT("%d arguments"), Parameters.Num());
                                Mismatch = FString::Printf(
                                    TEXT("function '%s' expects %s; call supplies %d"),
                                    *MemberName.ToString(), *Expected, SuppliedArgumentCount);
                                continue;
                            }
                            bool bTypesMatch = true;
                            const FString ReturnTypeName = bHasReturnOutput ? GetExpressionType(Arguments[2], LocalTypes) : FString();
                            if (bHasReturnOutput && !ReturnTypeName.IsEmpty() && !IsStaticTypeCompatible(Manager, ReturnTypeName, ReturnProperty, true))
                            {
                                Mismatch = FString::Printf(TEXT("function '%s' return type does not match '%s'"), *MemberName.ToString(), *ReturnTypeName);
                                bTypesMatch = false;
                            }
                            for (int32 Index = 0; bTypesMatch && Index < Parameters.Num(); ++Index)
                            {
                                const FString ArgumentTypeName = GetExpressionType(Arguments[Index + ParameterArgumentStart], LocalTypes);
                                if (!ArgumentTypeName.IsEmpty() && !IsStaticTypeCompatible(Manager, ArgumentTypeName, Parameters[Index], Parameters[Index]->HasAnyPropertyFlags(CPF_OutParm)))
                                {
                                    Mismatch = FString::Printf(
                                        TEXT("function '%s' parameter '%s' does not match '%s'"),
                                        *MemberName.ToString(), *Parameters[Index]->GetName(), *ArgumentTypeName);
                                    bTypesMatch = false;
                                }
                            }
                            if (bTypesMatch)
                            {
                                Mismatch.Reset();
                                break;
                            }
                        }
                    }
                    else if (FCString::Strcmp(Helper, TEXT("BindBlueprintEvent")) == 0)
                    {
                        for (UClass* Candidate : ReceiverClasses)
                        {
                            FMulticastDelegateProperty* Event = FindFProperty<FMulticastDelegateProperty>(Candidate, MemberName);
                            if (Event == nullptr) continue;
                            bFoundMember = true;
                            const FString* TargetTypeName = LocalTypes.Find(Arguments[2]);
                            UClass* TargetClass = TargetTypeName != nullptr ? FindReceiverClass(*TargetTypeName) : nullptr;
                            if (TargetClass == nullptr) continue;
                            const FString HandlerLiteral = Arguments[3].TrimStartAndEnd();
                            const int32 HandlerQuote = HandlerLiteral.Find(TEXT("\""));
                            const int32 HandlerEnd = HandlerQuote == INDEX_NONE
                                ? INDEX_NONE
                                : HandlerLiteral.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                            if (!HandlerLiteral.StartsWith(TEXT("n\"")) || HandlerQuote == INDEX_NONE || HandlerEnd <= HandlerQuote) continue;
                            UFunction* Handler = TargetClass->FindFunctionByName(FName(*HandlerLiteral.Mid(HandlerQuote + 1, HandlerEnd - HandlerQuote - 1)));
                            if (Handler == nullptr || !Event->SignatureFunction->IsSignatureCompatibleWith(Handler))
                                Mismatch = FString::Printf(TEXT("event '%s' does not match the resolved handler"), *MemberName.ToString());
                            else
                            {
                                Mismatch.Reset();
                                break;
                            }
                        }
                    }
                    else
                    {
                        for (UClass* Candidate : ReceiverClasses)
                        {
                            FProperty* Property = FindFProperty<FProperty>(Candidate, MemberName);
                            if (Property == nullptr) continue;
                            bFoundMember = true;
                            const FString ValueTypeName = GetExpressionType(Arguments[2], LocalTypes);
                            if (!ValueTypeName.IsEmpty() && !IsStaticTypeCompatible(Manager, ValueTypeName, Property, FCString::Strcmp(Helper, TEXT("GetBlueprintProperty")) == 0))
                                Mismatch = FString::Printf(TEXT("property '%s' type does not match '%s'"), *MemberName.ToString(), *ValueTypeName);
                            else
                            {
                                Mismatch.Reset();
                                break;
                            }
                        }
                    }

                    if (!bFoundMember)
                        WarnQuickCall(
                            Code.AbsoluteFilename,
                            LineNumber,
                            FString::Printf(
                                TEXT("'%s' was not found on the statically known receiver type '%s' or a loaded Blueprint subclass."),
                                *MemberName.ToString(), **ReceiverTypeName));
                    else if (!Mismatch.IsEmpty())
                        WarnQuickCall(Code.AbsoluteFilename, LineNumber, Mismatch);
                }
            }
        }
    }
}


void RegisterMenu()
{
    static bool bMenuTextsRegistered = false;
    if (!bMenuTextsRegistered)
    {
        const struct { const TCHAR* Key; const TCHAR* English; const TCHAR* Chinese; } Texts[] = {
            {TEXT("BlueprintSection"), TEXT("Blueprint"), TEXT("蓝图")},
            {TEXT("ScanBlueprintQuickCalls"), TEXT("Scan Blueprint quick calls"), TEXT("扫描蓝图快速调用")},
            {TEXT("ScanBlueprintQuickCallsHelp"), TEXT("Check quick calls in loaded scripts against loaded receiver classes."), TEXT("检查已加载脚本中的快速调用是否匹配已加载的接收类。")},
        };
        TArray<FPolyglotTextData> Translations;
        for (const auto& Text : Texts)
        {
            FPolyglotTextData Data(ELocalizedTextSourceCategory::Editor, TEXT("SoloAS"), Text.Key, Text.English, TEXT("en"));
            Data.AddLocalizedString(TEXT("en"), Text.English);
            Data.AddLocalizedString(TEXT("zh"), Text.Chinese);
            Translations.Add(MoveTemp(Data));
        }
        FTextLocalizationManager::Get().RegisterPolyglotTextData(Translations);
        bMenuTextsRegistered = true;
    }
    FToolMenuOwnerScoped Owner(MenuOwner);
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.SoloAS"));
    Menu->AddSection(TEXT("SoloASBlueprint"), NSLOCTEXT("SoloAS", "BlueprintSection", "Blueprint"), FToolMenuInsert(TEXT("AIAssistance"), EToolMenuInsertType::After)).AddMenuEntry(
        TEXT("ScanBlueprintQuickCalls"),
        NSLOCTEXT("SoloAS", "ScanBlueprintQuickCalls", "Scan Blueprint quick calls"),
        NSLOCTEXT("SoloAS", "ScanBlueprintQuickCallsHelp", "Check quick calls in loaded scripts against loaded receiver classes."),
        FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&ScanBlueprintQuickCalls),
            FCanExecuteAction::CreateStatic(&CanScan)));
}
}

namespace SoloASCoreExtensions::BlueprintInheritance
{
void StartQuickCallEditor()
{
    if (bStarted || !GIsEditor || IsRunningCommandlet()) return;
    bStarted = true;
    MenuStartupHandle = UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateStatic(&RegisterMenu));
}

void StopQuickCallEditor()
{
    if (!bStarted) return;
    UToolMenus::UnRegisterStartupCallback(MenuStartupHandle);
    MenuStartupHandle.Reset();
    UToolMenus::UnregisterOwner(MenuOwner);
    bStarted = false;
}
}
#endif
