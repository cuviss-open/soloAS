// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#pragma once

#include "CoreMinimal.h"
struct FAngelscriptPreprocessor;

#include "Containers/StringConv.h"
#include "Containers/Set.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace SoloASCoreExtensions::Algorithms { bool IsEnabled(); void Start(); void Stop(); }

namespace SoloASExplicitCaptureLambdaPrivate
{
bool IsEnabled();
void Start();
void Stop();
void Rewrite(FAngelscriptPreprocessor& Preprocessor);
uint64 GetRewriteCount();
int32 GetSlotsPerSite();
void RefreshStorageMetadata();
void ResetStorageMetadata();
void RewriteLambdaReleaseStatements(FString& Code, const TSet<FString>& Receivers);

enum class ELexicalKind : uint8
{
	Code,
	LineComment,
	BlockComment,
	StringLiteral,
	CharacterLiteral,
};

struct FCaptureDeclaration
{
	FString Declaration;
	FString Name;
};

struct FLambdaCandidate
{
	int32 MethodPosition = INDEX_NONE;
	int32 DotPosition = INDEX_NONE;
	bool bMulticast = false;
	FString Algorithm;
};

enum class ELambdaCandidateClassification : uint8
{
	Passthrough,
	TypedExplicitCapture,
	MalformedTypedExplicitCapture,
};

struct FSourceReplacement
{
    FString Receiver;
    FString OwnerClassName;
    int32 OwnerClassClose = INDEX_NONE;
    FString OwnerMethod;
    FString OwnerStoreName;
	int32 StartPosition = INDEX_NONE;
	int32 EndPosition = INDEX_NONE;
	FString Replacement;
	FString GeneratedCode;
	int32 BodyPosition = INDEX_NONE;
	TArray<FString> DebugBodyFunctions;
};

struct FFileRewritePlan
{
	int32 FileIndex = INDEX_NONE;
	TArray<FSourceReplacement> Replacements;
};

inline bool IsIdentifierStart(TCHAR Character)
{
	return (Character >= 'A' && Character <= 'Z')
		|| (Character >= 'a' && Character <= 'z')
		|| Character == '_';
}

inline bool IsIdentifierCharacter(TCHAR Character)
{
	return IsIdentifierStart(Character)
		|| (Character >= '0' && Character <= '9');
}

inline void BuildLexicalKinds(
	const FString& Code,
	TArray<ELexicalKind>& OutKinds
)
{
	OutKinds.Init(ELexicalKind::Code, Code.Len());
	int32 Position = 0;
	while (Position < Code.Len())
	{
		if (Code[Position] == '/'
			&& Position + 1 < Code.Len()
			&& Code[Position + 1] == '/')
		{
			const int32 Start = Position;
			Position += 2;
			while (Position < Code.Len() && Code[Position] != '\n')
				++Position;
			for (int32 Index = Start; Index < Position; ++Index)
				OutKinds[Index] = ELexicalKind::LineComment;
			continue;
		}
		if (Code[Position] == '/'
			&& Position + 1 < Code.Len()
			&& Code[Position + 1] == '*')
		{
			const int32 Start = Position;
			Position += 2;
			while (Position + 1 < Code.Len()
				&& !(Code[Position] == '*' && Code[Position + 1] == '/'))
			{
				++Position;
			}
			Position = Position + 1 < Code.Len()
				? Position + 2
				: Code.Len();
			for (int32 Index = Start; Index < Position; ++Index)
				OutKinds[Index] = ELexicalKind::BlockComment;
			continue;
		}
		if (Code[Position] == '"' || Code[Position] == '\'')
		{
			const TCHAR Quote = Code[Position];
			const ELexicalKind Kind = Quote == '"'
				? ELexicalKind::StringLiteral
				: ELexicalKind::CharacterLiteral;
			OutKinds[Position++] = Kind;
			while (Position < Code.Len())
			{
				OutKinds[Position] = Kind;
				if (Code[Position] == '\\')
				{
					++Position;
					if (Position < Code.Len())
						OutKinds[Position++] = Kind;
					continue;
				}
				if (Code[Position++] == Quote)
					break;
			}
			continue;
		}
		++Position;
	}
}

inline bool IsCommentKind(ELexicalKind Kind)
{
	return Kind == ELexicalKind::LineComment
		|| Kind == ELexicalKind::BlockComment;
}

inline int32 SkipTriviaForward(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 Position
)
{
	while (Position < Code.Len()
		&& (IsCommentKind(Kinds[Position])
			|| (Kinds[Position] == ELexicalKind::Code
				&& FChar::IsWhitespace(Code[Position]))))
	{
		++Position;
	}
	return Position;
}

inline int32 SkipTriviaBackward(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 Position
)
{
	while (Position >= 0
		&& (IsCommentKind(Kinds[Position])
			|| (Kinds[Position] == ELexicalKind::Code
				&& FChar::IsWhitespace(Code[Position]))))
	{
		--Position;
	}
	return Position;
}

inline bool MatchesIdentifierAt(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 Position,
	const TCHAR* Identifier
)
{
	const int32 Length = FCString::Strlen(Identifier);
	if (Position < 0 || Position + Length > Code.Len())
		return false;
	for (int32 Offset = 0; Offset < Length; ++Offset)
	{
		if (Kinds[Position + Offset] != ELexicalKind::Code
			|| Code[Position + Offset] != Identifier[Offset])
		{
			return false;
		}
	}
	if (Position > 0
		&& Kinds[Position - 1] == ELexicalKind::Code
		&& IsIdentifierCharacter(Code[Position - 1]))
	{
		return false;
	}
	const int32 End = Position + Length;
	return End >= Code.Len()
		|| Kinds[End] != ELexicalKind::Code
		|| !IsIdentifierCharacter(Code[End]);
}

inline bool FindNextCandidate(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 SearchPosition,
	FLambdaCandidate& OutCandidate
)
{
	for (int32 Position = SearchPosition; Position < Code.Len(); ++Position)
	{
        bool bMulticast = false;
        FString Algorithm;
        for (const TCHAR* Name : {TEXT("Sort"), TEXT("StableSort"), TEXT("FindByPredicate"),
            TEXT("IndexOfByPredicate"), TEXT("ContainsByPredicate"), TEXT("RemoveAll"),
            TEXT("FilterByPredicate"), TEXT("KeySort"), TEXT("ValueSort")})
            if (MatchesIdentifierAt(Code, Kinds, Position, Name)) { Algorithm = Name; break; }
		if (MatchesIdentifierAt(Code, Kinds, Position, TEXT("AddLambda")))
		{
			bMulticast = true;
		}
		else if (Algorithm.IsEmpty() && !MatchesIdentifierAt(
			Code,
			Kinds,
			Position,
			TEXT("BindLambda")))
		{
			continue;
		}

		const int32 DotPosition = SkipTriviaBackward(
			Code,
			Kinds,
			Position - 1
		);
		if (DotPosition < 0
			|| Kinds[DotPosition] != ELexicalKind::Code
			|| Code[DotPosition] != '.')
		{
			continue;
		}

        if (!Algorithm.IsEmpty())
        {
            const int32 Open = SkipTriviaForward(Code, Kinds, Position + Algorithm.Len());
            if (Open >= Code.Len() || Code[Open] != '(') continue;
            const int32 FirstArgument = SkipTriviaForward(Code, Kinds, Open + 1);
            if (FirstArgument >= Code.Len() || Code[FirstArgument] != '[') continue;
        }

		OutCandidate.MethodPosition = Position;
		OutCandidate.DotPosition = DotPosition;
		OutCandidate.bMulticast = bMulticast;
        OutCandidate.Algorithm = Algorithm;
		return true;
	}
	return false;
}

inline TCHAR MatchingClose(TCHAR Open)
{
	switch (Open)
	{
	case '(':
		return ')';
	case '[':
		return ']';
	case '{':
		return '}';
	default:
		return 0;
	}
}

inline bool FindMatchingDelimiter(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 OpenPosition,
	int32& OutClosePosition
)
{
	if (OpenPosition < 0
		|| OpenPosition >= Code.Len()
		|| Kinds[OpenPosition] != ELexicalKind::Code
		|| MatchingClose(Code[OpenPosition]) == 0)
	{
		return false;
	}

	TArray<TCHAR> DelimiterStack;
	for (int32 Position = OpenPosition; Position < Code.Len(); ++Position)
	{
		if (Kinds[Position] != ELexicalKind::Code)
			continue;
		const TCHAR Character = Code[Position];
		if (MatchingClose(Character) != 0)
		{
			DelimiterStack.Add(Character);
			continue;
		}
		if (Character != ')' && Character != ']'
			&& Character != '}')
		{
			continue;
		}
		if (DelimiterStack.IsEmpty()
			|| MatchingClose(DelimiterStack.Last()) != Character)
		{
			return false;
		}
		DelimiterStack.Pop(EAllowShrinking::No);
		if (DelimiterStack.IsEmpty())
		{
			OutClosePosition = Position;
			return true;
		}
	}
	return false;
}

inline FString MakeFilenameHash(const FString& RelativeFilename)
{
	FString Normalized = RelativeFilename;
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
	FPaths::NormalizeFilename(Normalized);
	while (Normalized.ReplaceInline(TEXT("//"), TEXT("/")) > 0)
	{
	}
	while (Normalized.StartsWith(TEXT("./")))
		Normalized.RightChopInline(2, EAllowShrinking::No);
	Normalized.ToLowerInline();
	if (Normalized.IsEmpty())
		Normalized = TEXT("unknown");

	FTCHARToUTF8 Utf8(*Normalized);
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
	FString HashText;
	HashText.Reserve(FSHA1::DigestSize * 2);
	for (uint8 Byte : Hash)
		HashText += FString::Printf(TEXT("%02x"), Byte);
	return HashText;
}


struct FAlgorithmGroup
{
    FString Key;
    FString Name;
    FString CaptureDeclarations;
    FString CaptureArguments;
    FString ParameterDeclarations;
    FString DispatchCases;
    FString ContainerType;
    FString ElementType;
    FString Algorithm;
    int32 OwnerClose = INDEX_NONE;
    int32 Sites = 0;
};

bool ParseAlgorithmCandidate(const FString& Code, const TArray<ELexicalKind>& Kinds,
    const FLambdaCandidate& Candidate, const FString& FilenameHash,
    TArray<FAlgorithmGroup>& Groups, FSourceReplacement& Out, FString& Error);
void FinishAlgorithmGroups(const TArray<FAlgorithmGroup>& Groups, TArray<FSourceReplacement>& Replacements);

bool ParseCaptureSegment(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 SegmentStart,
	int32 SegmentEnd,
	TSet<FString>& SeenNames,
	FCaptureDeclaration& OutCapture,
	FString& OutError
);

bool ValidateCaptureTypeExpression(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 TypeStart,
	int32 TypeEnd,
	FString& OutError
);

bool ParseCaptures(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 CaptureOpen,
	int32 CaptureClose,
	TArray<FCaptureDeclaration>& OutCaptures,
	FString& OutError
);

bool ParseReturnType(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 StartPosition,
	int32& OutBodyOpen,
	FString& OutReturnType,
	FString& OutError
);

bool HasValueReturn(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 BodyOpen,
	int32 BodyClose
);



ELambdaCandidateClassification ClassifyCandidate(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	const FLambdaCandidate& Candidate,
	int32& OutNextSearchPosition
);

bool ParseCandidate(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	const FLambdaCandidate& Candidate,
	const FString& FilenameHash,
	int32 Ordinal,
	FSourceReplacement& OutReplacement,
	FString& OutError
);
}
