// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ExplicitCaptureLambdaExtension.h"

namespace SoloASExplicitCaptureLambdaPrivate
{
bool ValidateCaptureTypeExpression(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 TypeStart,
	int32 TypeEnd,
	FString& OutError
)
{
	enum class ETypeToken : uint8
	{
		Start,
		Identifier,
		Scope,
		TemplateOpen,
		TemplateClose,
		Comma,
		Handle,
	};
	ETypeToken PreviousToken = ETypeToken::Start;
	FString PreviousIdentifier;
	int32 TemplateDepth = 0;
	for (int32 Position = TypeStart; Position <= TypeEnd;)
	{
		if (Kinds[Position] != ELexicalKind::Code)
		{
			OutError = TEXT("capture type cannot contain comments or literals");
			return false;
		}
		const TCHAR Character = Code[Position];
		if (FChar::IsWhitespace(Character))
		{
			++Position;
			continue;
		}
		if (IsIdentifierStart(Character))
		{
			const int32 IdentifierStart = Position++;
			while (Position <= TypeEnd
				&& Kinds[Position] == ELexicalKind::Code
				&& IsIdentifierCharacter(Code[Position]))
			{
				++Position;
			}
			const FString Identifier = Code.Mid(
				IdentifierStart,
				Position - IdentifierStart
			);
			const bool bConstQualifier =
				PreviousToken == ETypeToken::Identifier
				&& PreviousIdentifier == TEXT("const")
				&& Identifier != TEXT("const");
			if ((PreviousToken == ETypeToken::Identifier
					&& !bConstQualifier)
				|| PreviousToken == ETypeToken::TemplateClose
				|| PreviousToken == ETypeToken::Handle)
			{
				OutError = TEXT("capture type contains an illegal declaration token");
				return false;
			}
			PreviousToken = ETypeToken::Identifier;
			PreviousIdentifier = Identifier;
			continue;
		}
		if (Character == ':'
			&& Position + 1 <= TypeEnd
			&& Kinds[Position + 1] == ELexicalKind::Code
			&& Code[Position + 1] == ':')
		{
			if (PreviousToken != ETypeToken::Identifier
				|| PreviousIdentifier == TEXT("const"))
			{
				OutError = TEXT("capture type contains an illegal declaration token");
				return false;
			}
			PreviousToken = ETypeToken::Scope;
			PreviousIdentifier.Reset();
			Position += 2;
			continue;
		}
		if (Character == '<')
		{
			if (PreviousToken != ETypeToken::Identifier
				|| PreviousIdentifier == TEXT("const"))
			{
				OutError = TEXT("capture type contains an illegal declaration token");
				return false;
			}
			++TemplateDepth;
			PreviousToken = ETypeToken::TemplateOpen;
			PreviousIdentifier.Reset();
			++Position;
			continue;
		}
		const bool bCompleteType =
			PreviousToken == ETypeToken::Identifier
			|| PreviousToken == ETypeToken::TemplateClose
			|| PreviousToken == ETypeToken::Handle;
		if (Character == '>')
		{
			if (TemplateDepth == 0 || !bCompleteType
				|| PreviousIdentifier == TEXT("const"))
			{
				OutError = TEXT("capture type template delimiters are malformed");
				return false;
			}
			--TemplateDepth;
			PreviousToken = ETypeToken::TemplateClose;
			PreviousIdentifier.Reset();
			++Position;
			continue;
		}
		if (Character == ',')
		{
			if (TemplateDepth == 0 || !bCompleteType
				|| PreviousIdentifier == TEXT("const"))
			{
				OutError = TEXT("capture type contains an illegal declaration token");
				return false;
			}
			PreviousToken = ETypeToken::Comma;
			PreviousIdentifier.Reset();
			++Position;
			continue;
		}
		if (Character == '@')
		{
			if (!bCompleteType || PreviousToken == ETypeToken::Handle
				|| PreviousIdentifier == TEXT("const"))
			{
				OutError = TEXT("capture type contains an illegal declaration token");
				return false;
			}
			PreviousToken = ETypeToken::Handle;
			PreviousIdentifier.Reset();
			++Position;
			continue;
		}

		OutError = TEXT("capture type contains an illegal declaration token");
		return false;
	}
	const bool bCompleteType =
		PreviousToken == ETypeToken::Identifier
		|| PreviousToken == ETypeToken::TemplateClose
		|| PreviousToken == ETypeToken::Handle;
	if (TemplateDepth != 0)
	{
		OutError = TEXT("capture type template delimiters are malformed");
		return false;
	}
	if (!bCompleteType || PreviousIdentifier == TEXT("const"))
	{
		OutError = TEXT("capture type contains an illegal declaration token");
		return false;
	}
	return true;
}
bool ParseCaptureSegment(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 SegmentStart,
	int32 SegmentEnd,
	TSet<FString>& SeenNames,
	FCaptureDeclaration& OutCapture,
	FString& OutError
)
{
	const int32 Start = SkipTriviaForward(Code, Kinds, SegmentStart);
	const int32 End = SkipTriviaBackward(Code, Kinds, SegmentEnd);
	if (Start > End)
	{
		OutError = TEXT("capture list contains an empty item");
		return false;
	}
	for (int32 Position = Start; Position <= End; ++Position)
	{
		if (Kinds[Position] == ELexicalKind::Code
			&& (Code[Position] == '&' || Code[Position] == '='))
		{
			OutError = Code[Position] == '&'
				? TEXT("delegate lambda reference captures are not supported; capture a value instead")
				: TEXT("capture initializers are not supported");
			return false;
		}
	}
	if (Kinds[End] != ELexicalKind::Code
		|| !IsIdentifierCharacter(Code[End]))
	{
		OutError = TEXT("capture must end with a simple identifier");
		return false;
	}
	int32 NameStart = End;
	while (NameStart > Start
		&& Kinds[NameStart - 1] == ELexicalKind::Code
		&& IsIdentifierCharacter(Code[NameStart - 1]))
	{
		--NameStart;
	}
	if (!IsIdentifierStart(Code[NameStart]))
	{
		OutError = TEXT("capture name is not a valid identifier");
		return false;
	}
	const FString Name = Code.Mid(NameStart, End - NameStart + 1);
	if (SeenNames.Contains(Name))
	{
		OutError = FString::Printf(
			TEXT("capture '%s' is duplicated"),
			*Name
		);
		return false;
	}
    if (Name == TEXT("this") && NameStart != Start)
    { OutError = TEXT("this capture cannot have a type"); return false; }
    if (Name == TEXT("this") && NameStart == Start)
    {
        SeenNames.Add(Name);
        OutCapture.Name = Name;
        return true;
    }
	const int32 DeclarationEnd = SkipTriviaBackward(
		Code,
		Kinds,
		NameStart - 1
	);
	if (DeclarationEnd < Start)
	{
		OutError = TEXT("capture requires an explicit type and name");
		return false;
	}
	if (!ValidateCaptureTypeExpression(
		Code,
		Kinds,
		Start,
		DeclarationEnd,
		OutError))
	{
		return false;
	}
	SeenNames.Add(Name);
	OutCapture.Name = Name;
	OutCapture.Declaration =
		Code.Mid(Start, End - Start + 1).TrimStartAndEnd();
	return true;
}
bool ParseCaptures(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 CaptureOpen,
	int32 CaptureClose,
	TArray<FCaptureDeclaration>& OutCaptures,
	FString& OutError
)
{
	if (SkipTriviaForward(Code, Kinds, CaptureOpen + 1) == CaptureClose)
		return true;
	TArray<TCHAR> DelimiterStack;
	TSet<FString> SeenNames;
	int32 TemplateDepth = 0;
	int32 SegmentStart = CaptureOpen + 1;
	for (int32 Position = SegmentStart; Position < CaptureClose; ++Position)
	{
		if (Kinds[Position] != ELexicalKind::Code)
			continue;
		const TCHAR Character = Code[Position];
		if (Character == '<')
		{
			++TemplateDepth;
			continue;
		}
		if (Character == '>')
		{
			if (TemplateDepth == 0)
			{
				OutError = TEXT("capture template delimiters are malformed");
				return false;
			}
			--TemplateDepth;
			continue;
		}
		if (MatchingClose(Character) != 0)
		{
			DelimiterStack.Add(Character);
			continue;
		}
		if (Character == ')' || Character == ']'
			|| Character == '}')
		{
			if (DelimiterStack.IsEmpty()
				|| MatchingClose(DelimiterStack.Last()) != Character)
			{
				OutError = TEXT("capture delimiters are malformed");
				return false;
			}
			DelimiterStack.Pop(EAllowShrinking::No);
			continue;
		}
		if (Character != ',' || !DelimiterStack.IsEmpty()
			|| TemplateDepth != 0)
			continue;

		FCaptureDeclaration& Capture = OutCaptures.Emplace_GetRef();
		if (!ParseCaptureSegment(
			Code,
			Kinds,
			SegmentStart,
			Position - 1,
			SeenNames,
			Capture,
			OutError))
		{
			return false;
		}
		SegmentStart = Position + 1;
	}
	if (TemplateDepth != 0)
	{
		OutError = TEXT("capture template delimiters are malformed");
		return false;
	}
	FCaptureDeclaration& Capture = OutCaptures.Emplace_GetRef();
	return ParseCaptureSegment(
		Code,
		Kinds,
		SegmentStart,
		CaptureClose - 1,
		SeenNames,
		Capture,
		OutError
	);
}
bool ParseReturnType(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 StartPosition,
	int32& OutBodyOpen,
	FString& OutReturnType,
	FString& OutError
)
{
	FString Canonical;
	int32 TemplateDepth = 0;
	bool bPreviousIdentifier = false;
	for (int32 Position = StartPosition; Position < Code.Len();)
	{
		if (Kinds[Position] != ELexicalKind::Code)
		{
			OutError = TEXT("lambda return type cannot contain comments or literals");
			return false;
		}
		const TCHAR Character = Code[Position];
		if (FChar::IsWhitespace(Character))
		{
			++Position;
			continue;
		}
		if (Character == '{')
		{
			if (Canonical.IsEmpty())
			{
				OutError = TEXT("lambda arrow requires a return type");
				return false;
			}
			if (TemplateDepth != 0)
			{
				OutError = TEXT("lambda return type template delimiters are malformed");
				return false;
			}
			OutBodyOpen = Position;
			OutReturnType = MoveTemp(Canonical);
			return true;
		}
		if (IsIdentifierStart(Character))
		{
			const int32 IdentifierStart = Position++;
			while (Position < Code.Len()
				&& Kinds[Position] == ELexicalKind::Code
				&& IsIdentifierCharacter(Code[Position]))
			{
				++Position;
			}
			if (bPreviousIdentifier
				|| Canonical.EndsWith(TEXT("@"))
				|| Canonical.EndsWith(TEXT("&")))
				Canonical += TEXT(" ");
			Canonical += Code.Mid(
				IdentifierStart, Position - IdentifierStart);
			bPreviousIdentifier = true;
			continue;
		}
		if (Character == ':'
			&& Position + 1 < Code.Len()
			&& Kinds[Position + 1] == ELexicalKind::Code
			&& Code[Position + 1] == ':')
		{
			Canonical += TEXT("::");
			bPreviousIdentifier = false;
			Position += 2;
			continue;
		}
		if (Character == '<')
		{
			++TemplateDepth;
			Canonical += Character;
			bPreviousIdentifier = false;
			++Position;
			continue;
		}
		if (Character == '>')
		{
			if (TemplateDepth == 0)
			{
				OutError = TEXT("lambda return type template delimiters are malformed");
				return false;
			}
			--TemplateDepth;
			Canonical += Character;
			bPreviousIdentifier = false;
			++Position;
			continue;
		}
		if (Character == ',')
		{
			if (TemplateDepth == 0)
			{
				OutError = TEXT("lambda return type comma requires a template");
				return false;
			}
			Canonical += TEXT(", ");
			bPreviousIdentifier = false;
			++Position;
			continue;
		}
		if (Character == '@' || Character == '&')
		{
			Canonical += Character;
			bPreviousIdentifier = false;
			++Position;
			continue;
		}

		OutError = TEXT("lambda return type contains an illegal token");
		return false;
	}
	OutError = TEXT("lambda body is missing after return type");
	return false;
}
}
