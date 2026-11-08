// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "BlueprintQuickCallsExtension.h"
#include "Misc/ConfigCacheIni.h"

#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
enum class EBlueprintQuickCallLexicalKind : uint8
{
	Code,
	Comment,
	String,
};

struct FCall
{
	int32 Start = INDEX_NONE;
	int32 Open = INDEX_NONE;
	int32 Close = INDEX_NONE;
	TArray<FString> Arguments;
};

struct FReplacement
{
	int32 FileIndex = INDEX_NONE;
	int32 Start = INDEX_NONE;
	int32 Length = 0;
	FString Text;
};

struct FFunctionShape
{
	bool bHasReturn = false;
	int32 ParameterCount = 0;
};

void BuildLexicalKinds(const FString& Source, TArray<EBlueprintQuickCallLexicalKind>& Kinds)
{
	Kinds.Init(EBlueprintQuickCallLexicalKind::Code, Source.Len());
	bool bLineComment = false;
	bool bBlockComment = false;
	TCHAR Quote = 0;
	for (int32 Index = 0; Index < Source.Len(); ++Index)
	{
		const TCHAR Character = Source[Index];
		const TCHAR Next = Index + 1 < Source.Len() ? Source[Index + 1] : 0;
		if (bLineComment)
		{
			Kinds[Index] = EBlueprintQuickCallLexicalKind::Comment;
			if (Character == TEXT('\n'))
				bLineComment = false;
			continue;
		}
		if (bBlockComment)
		{
			Kinds[Index] = EBlueprintQuickCallLexicalKind::Comment;
			if (Character == TEXT('*') && Next == TEXT('/'))
			{
				Kinds[++Index] = EBlueprintQuickCallLexicalKind::Comment;
				bBlockComment = false;
			}
			continue;
		}
		if (Quote != 0)
		{
			Kinds[Index] = EBlueprintQuickCallLexicalKind::String;
			if (Character == TEXT('\\') && Index + 1 < Source.Len())
				Kinds[++Index] = EBlueprintQuickCallLexicalKind::String;
			else if (Character == Quote)
				Quote = 0;
			continue;
		}
		if (Character == TEXT('/') && Next == TEXT('/'))
		{
			Kinds[Index] = Kinds[Index + 1] = EBlueprintQuickCallLexicalKind::Comment;
			++Index;
			bLineComment = true;
		}
		else if (Character == TEXT('/') && Next == TEXT('*'))
		{
			Kinds[Index] = Kinds[Index + 1] = EBlueprintQuickCallLexicalKind::Comment;
			++Index;
			bBlockComment = true;
		}
		else if (Character == TEXT('"') || Character == TEXT('\''))
		{
			Kinds[Index] = EBlueprintQuickCallLexicalKind::String;
			Quote = Character;
		}
	}
}

bool IsQuickCallIdentifierCharacter(TCHAR Character)
{
	return FChar::IsAlnum(Character) || Character == TEXT('_');
}

int32 SkipWhitespaceAndComments(
	const FString& Source,
	const TArray<EBlueprintQuickCallLexicalKind>& Kinds,
	int32 Position)
{
	while (Position < Source.Len()
		&& (FChar::IsWhitespace(Source[Position])
			|| Kinds[Position] == EBlueprintQuickCallLexicalKind::Comment))
		++Position;
	return Position;
}

bool ParseArguments(
	const FString& Source,
	const TArray<EBlueprintQuickCallLexicalKind>& Kinds,
	int32 Open,
	int32& OutClose,
	TArray<FString>& OutArguments)
{
	int32 Depth = 1;
	int32 Start = Open + 1;
	for (int32 Index = Open + 1; Index < Source.Len(); ++Index)
	{
		if (Kinds[Index] != EBlueprintQuickCallLexicalKind::Code)
			continue;
		const TCHAR Character = Source[Index];
		if (Character == TEXT('(') || Character == TEXT('[') || Character == TEXT('{'))
			++Depth;
		else if (Character == TEXT(')'))
		{
			if (--Depth == 0)
			{
				OutClose = Index;
				const FString Last = Source.Mid(Start, Index - Start).TrimStartAndEnd();
				if (!Last.IsEmpty() || !OutArguments.IsEmpty())
					OutArguments.Add(Last);
				return true;
			}
		}
		else if ((Character == TEXT(']') || Character == TEXT('}')) && Depth > 1)
			--Depth;
		else if (Character == TEXT(',') && Depth == 1)
		{
			OutArguments.Add(Source.Mid(Start, Index - Start).TrimStartAndEnd());
			Start = Index + 1;
		}
	}
	return false;
}

bool FindNextCall(
	const FString& Source,
	const TArray<EBlueprintQuickCallLexicalKind>& Kinds,
	int32& SearchPosition,
	FCall& OutCall)
{
	static const TCHAR* Name = TEXT("CallBlueprint");
	const int32 NameLength = FCString::Strlen(Name);
	int32 NamePosition = SearchPosition;
	while ((NamePosition = Source.Find(Name, ESearchCase::CaseSensitive, ESearchDir::FromStart, NamePosition)) != INDEX_NONE)
	{
		const int32 NameEnd = NamePosition + NameLength;
		int32 Previous = NamePosition - 1;
        while (Previous >= 0 && FChar::IsWhitespace(Source[Previous])) --Previous;
        if ((Previous < 0 || (Source[Previous] != TEXT('.') && Source[Previous] != TEXT(':')))
            && Kinds[NamePosition] == EBlueprintQuickCallLexicalKind::Code
			&& (NamePosition == 0 || !IsQuickCallIdentifierCharacter(Source[NamePosition - 1]))
			&& (NameEnd == Source.Len() || !IsQuickCallIdentifierCharacter(Source[NameEnd])))
		{
			const int32 Open = SkipWhitespaceAndComments(Source, Kinds, NameEnd);
			if (Open < Source.Len() && Source[Open] == TEXT('(') && Kinds[Open] == EBlueprintQuickCallLexicalKind::Code)
			{
				OutCall = FCall();
				OutCall.Start = NamePosition;
				OutCall.Open = Open;
				if (!ParseArguments(Source, Kinds, Open, OutCall.Close, OutCall.Arguments))
					return false;
				SearchPosition = OutCall.Close + 1;
				return true;
			}
		}
		NamePosition += NameLength;
	}
	return false;
}

void AddDeclaredType(TMap<FString, FString>& Types, FString Declaration)
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

TMap<FString, FString> FindDeclaredTypes(const FString& Source)
{
	TMap<FString, FString> Types;
	TArray<FString> Lines;
	Source.ParseIntoArrayLines(Lines);
	for (FString Line : Lines)
	{
		int32 Comment = INDEX_NONE;
		if (Line.FindChar(TEXT('/'), Comment) && Line.Mid(Comment).StartsWith(TEXT("//")))
			Line.LeftInline(Comment);
		const int32 Open = Line.Find(TEXT("("));
		const int32 Close = Open == INDEX_NONE ? INDEX_NONE
			: Line.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
		if (Open != INDEX_NONE && Close > Open)
		{
			TArray<FString> Parameters;
			Line.Mid(Open + 1, Close - Open - 1).ParseIntoArray(Parameters, TEXT(","), true);
			for (FString Parameter : Parameters)
				AddDeclaredType(Types, MoveTemp(Parameter));
		}
		int32 End = Line.Find(TEXT("="));
		if (End == INDEX_NONE)
			End = Line.Find(TEXT(";"));
		if (End != INDEX_NONE)
			AddDeclaredType(Types, Line.Left(End));
	}
	return Types;
}

UClass* FindQuickCallReceiverClass(const FString& TypeName)
{
	TSharedPtr<FAngelscriptModuleDesc> Module;
	const TSharedPtr<FAngelscriptClassDesc> ClassDesc =
		FAngelscriptManager::Get().GetClass(TypeName, &Module);
	if (ClassDesc.IsValid() && ClassDesc->Class != nullptr)
		return ClassDesc->Class;
	const TSharedPtr<FAngelscriptType> Type =
		FAngelscriptType::GetByAngelscriptTypeName(TypeName);
	return Type.IsValid()
		? Type->GetClass(FAngelscriptTypeUsage::DefaultUsage)
		: nullptr;
}

bool FindFunctionShape(UClass* StaticClass, FName FunctionName, FFunctionShape& OutShape)
{
	if (StaticClass == nullptr)
		return false;
	bool bFound = false;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Candidate = *It;
		if (Candidate != StaticClass && !Candidate->IsChildOf(StaticClass))
			continue;
		UFunction* Function = Candidate->FindFunctionByName(FunctionName);
		if (Function == nullptr)
			continue;
		FFunctionShape Shape;
		for (TFieldIterator<FProperty> PropertyIt(Function); PropertyIt; ++PropertyIt)
		{
			if (!PropertyIt->HasAnyPropertyFlags(CPF_Parm))
				continue;
			if (PropertyIt->HasAnyPropertyFlags(CPF_ReturnParm))
				Shape.bHasReturn = true;
			else
				++Shape.ParameterCount;
		}
		if (!bFound)
		{
			OutShape = Shape;
			bFound = true;
		}
		else if (OutShape.bHasReturn != Shape.bHasReturn
			|| OutShape.ParameterCount != Shape.ParameterCount)
		{
			return false;
		}
	}
	return bFound;
}

bool ParseLiteralFunctionName(const FString& Argument, FName& OutName)
{
	const FString Value = Argument.TrimStartAndEnd();
    if (Value.StartsWith(TEXT("__STATIC_NAME(")) && Value.EndsWith(TEXT(")")))
    {
        const FString IndexText = Value.Mid(14, Value.Len() - 15);
        if (!IndexText.IsNumeric()) return false;
        const int32 Index = FCString::Atoi(*IndexText);
        if (!FAngelscriptManager::StaticNames.IsValidIndex(Index)) return false;
        OutName = FAngelscriptManager::StaticNames[Index];
        return !OutName.IsNone();
    }
    if (!Value.StartsWith(TEXT("n\"")) || !Value.EndsWith(TEXT("\"")) || Value.Len() < 3)
        return false;
	OutName = FName(*Value.Mid(2, Value.Len() - 3));
	return !OutName.IsNone();
}

int32 QuickCallsGetLineNumber(const FString& Source, int32 Position)
{
	int32 Line = 1;
	for (int32 Index = 0; Index < Position; ++Index)
		if (Source[Index] == TEXT('\n'))
			++Line;
	return Line;
}

void RewriteQuickCallsInternal(FAngelscriptPreprocessor& Preprocessor)
{
	if (!SoloASCoreExtensions::BlueprintInheritance::AreQuickCallsEnabled()) return;
	TArray<FReplacement> Replacements;
	for (int32 FileIndex = 0; FileIndex < Preprocessor.Files.Num(); ++FileIndex)
	{
		FAngelscriptPreprocessor::FFile& File = Preprocessor.Files[FileIndex];
		TArray<EBlueprintQuickCallLexicalKind> Kinds;
		BuildLexicalKinds(File.ProcessedCode, Kinds);
		const TMap<FString, FString> DeclaredTypes = FindDeclaredTypes(File.ProcessedCode);
		int32 SearchPosition = 0;
		FCall Call;
		while (FindNextCall(File.ProcessedCode, Kinds, SearchPosition, Call))
		{
			if (Call.Arguments.Num() < 2)
				continue;

			const FString Receiver = Call.Arguments[0].TrimStartAndEnd();
			const FString* TypeName = DeclaredTypes.Find(Receiver);
			UClass* StaticClass = TypeName != nullptr ? FindQuickCallReceiverClass(*TypeName) : nullptr;
			FName FunctionName;
			FFunctionShape Shape;
			const bool bHasStaticShape = StaticClass != nullptr
				&& ParseLiteralFunctionName(Call.Arguments[1], FunctionName)
				&& FindFunctionShape(StaticClass, FunctionName, Shape);
			if (bHasStaticShape)
			{
				const int32 ProvidedArgumentCount = Call.Arguments.Num() - 2;
				const bool bCountMatches = ProvidedArgumentCount == Shape.ParameterCount
					|| (Shape.bHasReturn && ProvidedArgumentCount == Shape.ParameterCount + 1);
				if (!bCountMatches)
				{
					const FString Expected = Shape.bHasReturn
						? FString::Printf(TEXT("%d arguments, or %d including a return slot"), Shape.ParameterCount, Shape.ParameterCount + 1)
						: FString::Printf(TEXT("%d arguments"), Shape.ParameterCount);
					const int32 Line = QuickCallsGetLineNumber(File.ProcessedCode, Call.Start);
					Preprocessor.LineError(File, Line, FString::Printf(
						TEXT("Blueprint function '%s' expects %s, but CallBlueprint received %d arguments."),
						*FunctionName.ToString(), *Expected, ProvidedArgumentCount));
					Preprocessor.bHasError = true;
					return;
				}
			}

			FReplacement& Replacement = Replacements.AddDefaulted_GetRef();
			Replacement.FileIndex = FileIndex;
			Replacement.Start = Call.Start;
			Replacement.Length = FString(TEXT("CallBlueprint")).Len();
			Replacement.Text = TEXT("__CallBlueprintInternal");
		}
	}

	Replacements.Sort([](const FReplacement& Left, const FReplacement& Right)
	{
		if (Left.FileIndex != Right.FileIndex)
			return Left.FileIndex > Right.FileIndex;
		return Left.Start > Right.Start;
	});
	for (const FReplacement& Replacement : Replacements)
	{
		FAngelscriptPreprocessor::FFile& File = Preprocessor.Files[Replacement.FileIndex];
		File.ProcessedCode.RemoveAt(Replacement.Start, Replacement.Length, EAllowShrinking::No);
		File.ProcessedCode.InsertAt(Replacement.Start, Replacement.Text);
	}
 }
}

namespace SoloASCoreExtensions::BlueprintInheritance
{
namespace { FDelegateHandle PostProcessHandle; }
bool AreQuickCallsEnabled()
{
    static const bool Enabled = [] {
        bool Value = true;
        if (GConfig) GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableBlueprintQuickCalls"), Value, GEngineIni);
        return Value;
    }();
    return Enabled;
}
void RewriteQuickCalls(FAngelscriptPreprocessor& Preprocessor) { RewriteQuickCallsInternal(Preprocessor); }
void StartQuickCalls()
{
    if (AreQuickCallsEnabled() && !PostProcessHandle.IsValid())
        PostProcessHandle = FAngelscriptPreprocessor::OnPostProcessCode.AddStatic(&RewriteQuickCalls);
}
void StopQuickCalls()
{
    FAngelscriptPreprocessor::OnPostProcessCode.Remove(PostProcessHandle);
    PostProcessHandle.Reset();
}
}
