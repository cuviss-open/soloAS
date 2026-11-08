// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ExplicitCaptureLambdaExtension.h"

namespace SoloASExplicitCaptureLambdaPrivate
{
namespace
{
bool FindNestedLambdaBody(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 BodyOpen,
	int32 Limit
)
{
	if (BodyOpen <= 0 || BodyOpen >= Limit)
		return false;
	for (int32 CaptureOpen = BodyOpen - 1; CaptureOpen >= 0; --CaptureOpen)
	{
		if (Kinds[CaptureOpen] != ELexicalKind::Code
			|| Code[CaptureOpen] != '[')
			continue;

		int32 CaptureClose = INDEX_NONE;
		if (!FindMatchingDelimiter(Code, Kinds, CaptureOpen, CaptureClose)
			|| CaptureClose >= BodyOpen)
			continue;

		int32 ParametersOpen = SkipTriviaForward(
			Code, Kinds, CaptureClose + 1);
		if (ParametersOpen >= BodyOpen || Code[ParametersOpen] != '(')
			continue;

		int32 ParametersClose = INDEX_NONE;
		if (!FindMatchingDelimiter(
			Code, Kinds, ParametersOpen, ParametersClose)
			|| ParametersClose >= BodyOpen)
			continue;

		int32 DeclarationPosition = SkipTriviaForward(
			Code, Kinds, ParametersClose + 1);
		if (DeclarationPosition == BodyOpen)
			return true;
		if (DeclarationPosition + 1 >= BodyOpen
			|| Kinds[DeclarationPosition] != ELexicalKind::Code
			|| Kinds[DeclarationPosition + 1] != ELexicalKind::Code
			|| Code[DeclarationPosition] != '-'
			|| Code[DeclarationPosition + 1] != '>')
			continue;

		int32 ParsedBodyOpen = INDEX_NONE;
		FString ReturnType;
		FString Error;
		if (ParseReturnType(
			Code,
			Kinds,
			DeclarationPosition + 2,
			ParsedBodyOpen,
			ReturnType,
			Error)
			&& ParsedBodyOpen == BodyOpen)
		{
			return true;
		}
	}
	return false;
}
}

bool HasValueReturn(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 BodyOpen,
	int32 BodyClose
)
{
	for (int32 Position = BodyOpen + 1; Position < BodyClose; ++Position)
	{
		if (Kinds[Position] != ELexicalKind::Code)
			continue;
		if (Code[Position] == '{')
		{
			if (FindNestedLambdaBody(Code, Kinds, Position, BodyClose))
			{
				int32 NestedLambdaClose = INDEX_NONE;
				if (FindMatchingDelimiter(
					Code, Kinds, Position, NestedLambdaClose)
					&& NestedLambdaClose < BodyClose)
				{
					Position = NestedLambdaClose;
					continue;
				}
			}
			continue;
		}
		if (!MatchesIdentifierAt(Code, Kinds, Position, TEXT("return")))
			continue;
		const int32 ReturnValueStart = SkipTriviaForward(
			Code, Kinds, Position + 6);
		if (ReturnValueStart >= BodyClose || Code[ReturnValueStart] != ';')
			return true;
		Position = ReturnValueStart;
	}
	return false;
}
}
