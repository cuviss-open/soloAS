// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ExplicitCaptureLambdaExtension.h"

namespace SoloASExplicitCaptureLambdaPrivate
{
bool SegmentHasTypedIntent(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 SegmentStart,
	int32 SegmentEnd
)
{
    const int32 First = SkipTriviaForward(Code, Kinds, SegmentStart);
    const int32 Last = SkipTriviaBackward(Code, Kinds, SegmentEnd);
    if (Code.Mid(First, Last - First + 1) == TEXT("this")) return true;
	int32 PreviousIdentifierEnd = INDEX_NONE;
	for (int32 Position = SegmentStart; Position <= SegmentEnd;)
	{
		if (Kinds[Position] != ELexicalKind::Code
			|| !IsIdentifierStart(Code[Position]))
		{
			++Position;
			continue;
		}
		const int32 IdentifierStart = Position++;
		while (Position <= SegmentEnd
			&& Kinds[Position] == ELexicalKind::Code
			&& IsIdentifierCharacter(Code[Position]))
		{
			++Position;
		}
		if (PreviousIdentifierEnd != INDEX_NONE)
		{
			const int32 GapStart = PreviousIdentifierEnd + 1;
			if (SkipTriviaForward(Code, Kinds, GapStart) == IdentifierStart)
				return true;
			for (int32 GapPosition = GapStart;
				GapPosition < IdentifierStart;
				++GapPosition)
			{
				if (Kinds[GapPosition] == ELexicalKind::Code
					&& Code[GapPosition] == '&')
				{
					return true;
				}
			}
		}
		const int32 PreviousCode = SkipTriviaBackward(
			Code,
			Kinds,
			IdentifierStart - 1
		);
		if (PreviousCode >= SegmentStart
			&& (Code[PreviousCode] == '>' || Code[PreviousCode] == '@'))
		{
			return true;
		}
		const int32 NextCode = SkipTriviaForward(Code, Kinds, Position);
		if (NextCode <= SegmentEnd
			&& (Code[NextCode] == '<'
				|| Code[NextCode] == '@'
				|| (Code[NextCode] == ':'
					&& NextCode + 1 <= SegmentEnd
					&& Code[NextCode + 1] == ':')))
		{
			return true;
		}
		PreviousIdentifierEnd = Position - 1;
	}
	return false;
}

bool SegmentHasMalformedToken(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	int32 SegmentStart,
	int32 SegmentEnd
)
{
	const int32 Start = SkipTriviaForward(Code, Kinds, SegmentStart);
	const int32 End = SkipTriviaBackward(Code, Kinds, SegmentEnd);
	if (Start > End)
		return true;
	for (int32 Position = Start; Position <= End; ++Position)
	{
		if (Kinds[Position] != ELexicalKind::Code)
			return true;
		const TCHAR Character = Code[Position];
		if (!FChar::IsWhitespace(Character)
			&& !IsIdentifierCharacter(Character)
			&& Character != ':'
			&& Character != '<'
			&& Character != '>'
			&& Character != ','
			&& Character != '@')
		{
			return true;
		}
	}
	return false;
}

ELambdaCandidateClassification ClassifyCandidate(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	const FLambdaCandidate& Candidate,
	int32& OutNextSearchPosition
)
{
	const int32 MethodLength = Candidate.Algorithm.IsEmpty() ? (Candidate.bMulticast ? 9 : 10) : Candidate.Algorithm.Len();
	OutNextSearchPosition = Candidate.MethodPosition + MethodLength;
	int32 Position = SkipTriviaForward(
		Code,
		Kinds,
		OutNextSearchPosition
	);
	if (Position >= Code.Len() || Code[Position] != '(')
		return ELambdaCandidateClassification::Passthrough;
	Position = SkipTriviaForward(Code, Kinds, Position + 1);
	if (Position >= Code.Len() || Code[Position] != '[')
		return ELambdaCandidateClassification::Passthrough;

    if (!Candidate.Algorithm.IsEmpty())
        return ELambdaCandidateClassification::TypedExplicitCapture;

	const int32 CaptureOpen = Position;
	int32 CaptureClose = INDEX_NONE;
	if (!FindMatchingDelimiter(Code, Kinds, CaptureOpen, CaptureClose))
	{
		const int32 RemainingEnd = Code.Len() - 1;
		return SegmentHasTypedIntent(
			Code, Kinds, CaptureOpen + 1, RemainingEnd)
			? ELambdaCandidateClassification::MalformedTypedExplicitCapture
			: ELambdaCandidateClassification::Passthrough;
	}

    if (SkipTriviaForward(Code, Kinds, CaptureOpen + 1) == CaptureClose)
        return ELambdaCandidateClassification::TypedExplicitCapture;

	bool bTypedIntent = false;
	bool bMalformed = false;
	bool bAllSegmentsTyped = true;
	int32 TemplateDepth = 0;
	int32 SegmentStart = CaptureOpen + 1;
	for (Position = SegmentStart; Position <= CaptureClose; ++Position)
	{
		const bool bAtEnd = Position == CaptureClose;
		if (!bAtEnd && Kinds[Position] == ELexicalKind::Code)
		{
			if (Code[Position] == '<')
				++TemplateDepth;
			else if (Code[Position] == '>')
			{
				if (TemplateDepth == 0)
					bMalformed = true;
				else
					--TemplateDepth;
			}
		}
		if (!bAtEnd
			&& (Kinds[Position] != ELexicalKind::Code
				|| Code[Position] != ','
				|| TemplateDepth != 0))
		{
			continue;
		}

		const int32 SegmentEnd = Position - 1;
		const bool bSegmentTyped = SegmentHasTypedIntent(
			Code,
			Kinds,
			SegmentStart,
			SegmentEnd
		);
		bTypedIntent |= bSegmentTyped;
		bAllSegmentsTyped &= bSegmentTyped;
		bMalformed |= SegmentHasMalformedToken(
			Code,
			Kinds,
			SegmentStart,
			SegmentEnd
		);
		SegmentStart = Position + 1;
	}
	if (!bTypedIntent)
		return ELambdaCandidateClassification::Passthrough;
	if (bMalformed || !bAllSegmentsTyped || TemplateDepth != 0)
	{
		return ELambdaCandidateClassification::MalformedTypedExplicitCapture;
	}
	return ELambdaCandidateClassification::TypedExplicitCapture;
}

bool ParseCandidate(
	const FString& Code,
	const TArray<ELexicalKind>& Kinds,
	const FLambdaCandidate& Candidate,
	const FString& FilenameHash,
	int32 Ordinal,
	FSourceReplacement& OutReplacement,
	FString& OutError
)
{
	int32 ReceiverEnd = SkipTriviaBackward(
		Code,
		Kinds,
		Candidate.DotPosition - 1
	);
	if (ReceiverEnd < 0
		|| Kinds[ReceiverEnd] != ELexicalKind::Code
		|| !IsIdentifierCharacter(Code[ReceiverEnd]))
	{
		OutError = TEXT("lambda receiver must be a simple identifier");
		return false;
	}
	int32 ReceiverStart = ReceiverEnd;
	while (ReceiverStart > 0
		&& Kinds[ReceiverStart - 1] == ELexicalKind::Code
		&& IsIdentifierCharacter(Code[ReceiverStart - 1]))
	{
		--ReceiverStart;
	}
	if (!IsIdentifierStart(Code[ReceiverStart]))
	{
		OutError = TEXT("lambda receiver must be a simple identifier");
		return false;
	}

    FString Holder = TEXT("this");
    const FString RootProperty = Code.Mid(ReceiverStart, ReceiverEnd - ReceiverStart + 1);
    const int32 MemberDot = SkipTriviaBackward(Code, Kinds, ReceiverStart - 1);
    if (MemberDot >= 0 && Code[MemberDot] == '.')
    {
        // Walk member/index expressions backwards, keeping the complete holder
        // expression so Buttons[i++] and getter calls are evaluated once.
        TFunction<int32(int32)> ExpressionStart = [&](int32 End) -> int32
        {
            if (End < 0) return INDEX_NONE;
            int32 Start = End;
            if (Code[End] == ']' || Code[End] == ')')
            {
                const TCHAR Open = Code[End] == ']' ? '[' : '(';
                int32 Depth = 1;
                for (--Start; Start >= 0; --Start)
                {
                    if (Kinds[Start] != ELexicalKind::Code) continue;
                    if (Code[Start] == Code[End]) ++Depth;
                    if (Code[Start] == Open && --Depth == 0) break;
                }
                if (Start < 0) return INDEX_NONE;
                const int32 Previous = SkipTriviaBackward(Code, Kinds, Start - 1);
                if (Previous >= 0 && (IsIdentifierCharacter(Code[Previous]) || Code[Previous] == ']' || Code[Previous] == ')'))
                    Start = ExpressionStart(Previous);
            }
            else
            {
                if (!IsIdentifierCharacter(Code[End])) return INDEX_NONE;
                while (Start > 0 && Kinds[Start - 1] == ELexicalKind::Code && IsIdentifierCharacter(Code[Start - 1])) --Start;
                if (!IsIdentifierStart(Code[Start])) return INDEX_NONE;
            }
            if (Start == INDEX_NONE) return Start;
            const int32 Dot = SkipTriviaBackward(Code, Kinds, Start - 1);
            return Dot >= 0 && Code[Dot] == '.' ? ExpressionStart(SkipTriviaBackward(Code, Kinds, Dot - 1)) : Start;
        };
        const int32 HolderEnd = SkipTriviaBackward(Code, Kinds, MemberDot - 1);
        ReceiverStart = ExpressionStart(HolderEnd);
        if (ReceiverStart == INDEX_NONE) { OutError = TEXT("invalid lambda holder expression"); return false; }
        Holder = Code.Mid(ReceiverStart, HolderEnd - ReceiverStart + 1);
    }
    const FString Receiver = Code.Mid(ReceiverStart, ReceiverEnd - ReceiverStart + 1);
    const FString HolderName = FString::Printf(TEXT("__CuvissLambdaHolder_%s_%d"), *FilenameHash, Ordinal);
    const FString BindReceiver = MemberDot >= 0 && Code[MemberDot] == '.' ? HolderName + TEXT(".") + RootProperty : Receiver;

	const int32 MethodLength = Candidate.Algorithm.IsEmpty() ? (Candidate.bMulticast ? 9 : 10) : Candidate.Algorithm.Len();
	int32 Position = SkipTriviaForward(
		Code,
		Kinds,
		Candidate.MethodPosition + MethodLength
	);
	if (Position >= Code.Len() || Code[Position] != '(')
	{
		OutError = TEXT("lambda binding call requires parentheses");
		return false;
	}
	const int32 CallOpen = Position;
	Position = SkipTriviaForward(Code, Kinds, Position + 1);
	if (Position >= Code.Len() || Code[Position] != '[')
	{
		OutError = TEXT("lambda binding requires an explicit capture list");
		return false;
	}

	const int32 CaptureOpen = Position;
	int32 CaptureClose = INDEX_NONE;
	if (!FindMatchingDelimiter(Code, Kinds, CaptureOpen, CaptureClose))
	{
		OutError = TEXT("capture delimiters are malformed");
		return false;
	}
	TArray<FCaptureDeclaration> Captures;
	if (!ParseCaptures(
		Code,
		Kinds,
		CaptureOpen,
		CaptureClose,
		Captures,
		OutError))
	{
		return false;
	}
	for (const FCaptureDeclaration& Capture : Captures)
	{
		if (Capture.Name == TEXT("this"))
		{
			OutError = TEXT("delegate lambda [this] captures are not supported; capture values by type and name instead");
			return false;
		}
	}

	Position = SkipTriviaForward(Code, Kinds, CaptureClose + 1);
	if (Position >= Code.Len() || Code[Position] != '(')
	{
		OutError = TEXT("lambda parameter list is missing");
		return false;
	}
	const int32 ParametersOpen = Position;
	int32 ParametersClose = INDEX_NONE;
	if (!FindMatchingDelimiter(
		Code,
		Kinds,
		ParametersOpen,
		ParametersClose))
	{
		OutError = TEXT("lambda parameter delimiters are malformed");
		return false;
	}
	const FString Parameters = Code.Mid(
		ParametersOpen,
		ParametersClose - ParametersOpen + 1
	);

	Position = SkipTriviaForward(Code, Kinds, ParametersClose + 1);
	FString ReturnType = TEXT("void");
	if (Position + 1 < Code.Len()
		&& Kinds[Position] == ELexicalKind::Code
		&& Kinds[Position + 1] == ELexicalKind::Code
		&& Code[Position] == '-'
		&& Code[Position + 1] == '>')
	{
		int32 ParsedBodyOpen = INDEX_NONE;
		if (!ParseReturnType(
			Code,
			Kinds,
			Position + 2,
			ParsedBodyOpen,
			ReturnType,
			OutError))
		{
			return false;
		}
		Position = ParsedBodyOpen;
	}
	if (Position >= Code.Len() || Code[Position] != '{')
	{
		OutError = TEXT("lambda body is missing");
		return false;
	}
	const int32 BodyOpen = Position;
	int32 BodyClose = INDEX_NONE;
	if (!FindMatchingDelimiter(Code, Kinds, BodyOpen, BodyClose))
	{
		OutError = TEXT("lambda body delimiters are malformed");
		return false;
	}
	const FString Body = Code.Mid(
		BodyOpen,
		BodyClose - BodyOpen + 1
	);
	if (ReturnType == TEXT("void")
		&& HasValueReturn(Code, Kinds, BodyOpen, BodyClose))
	{
		OutError = TEXT("void lambda body cannot return a value");
		return false;
	}

	Position = SkipTriviaForward(Code, Kinds, BodyClose + 1);
	if (Position >= Code.Len() || Code[Position] != ')')
	{
		OutError = TEXT("lambda binding call is malformed");
		return false;
	}
	int32 CallClose = INDEX_NONE;
	if (!FindMatchingDelimiter(Code, Kinds, CallOpen, CallClose)
		|| CallClose != Position)
	{
		OutError = TEXT("lambda binding call delimiters are malformed");
		return false;
	}
	Position = SkipTriviaForward(Code, Kinds, CallClose + 1);
	if (Position >= Code.Len() || Code[Position] != ';')
	{
		OutError = TEXT("lambda binding call requires a trailing semicolon");
		return false;
	}

	FLambdaCandidate NestedCandidate;
	if (FindNextCandidate(
		Code,
		Kinds,
		Candidate.MethodPosition + MethodLength,
		NestedCandidate)
		&& NestedCandidate.MethodPosition < Position)
	{
		OutError = TEXT("nested explicit-capture lambdas are not supported");
		return false;
	}

	const FString ClassName = FString::Printf(TEXT("__CuvissLambda_%s_%d"), *FilenameHash, Ordinal);
    FString OwnerClass;
    int32 OwnerClose = INDEX_NONE;
    for (int32 At = 0; At < ReceiverStart; ++At)
    {
        if (Kinds[At] != ELexicalKind::Code || Code.Mid(At, 5) != TEXT("class")
            || (At > 0 && IsIdentifierCharacter(Code[At - 1]))
            || (At + 5 < Code.Len() && IsIdentifierCharacter(Code[At + 5]))) continue;
        const int32 NameStart = SkipTriviaForward(Code, Kinds, At + 5);
        int32 NameEnd = NameStart;
        while (NameEnd < Code.Len() && IsIdentifierCharacter(Code[NameEnd])) ++NameEnd;
        int32 Open = NameEnd;
        while (Open < Code.Len() && !(Kinds[Open] == ELexicalKind::Code && Code[Open] == '{')) ++Open;
        int32 Close = INDEX_NONE;
        if (Open < ReceiverStart && FindMatchingDelimiter(Code, Kinds, Open, Close) && Close > Position)
        { OwnerClass = Code.Mid(NameStart, NameEnd - NameStart); OwnerClose = Close; }
    }
    if (OwnerClass.IsEmpty()) { OutError = TEXT("lambda requires a UObject instance method, not a global or struct method"); return false; }
    const bool bCaptureThis = Captures.ContainsByPredicate([](const auto& Capture) { return Capture.Name == TEXT("this"); });
    Captures.RemoveAll([](const auto& Capture) { return Capture.Name == TEXT("this"); });
    FString Declarations, Arguments, CaptureArguments;
    for (const auto& Capture : Captures)
    {
        if (!Declarations.IsEmpty()) { Declarations += TEXT(", "); Arguments += TEXT(", "); CaptureArguments += TEXT(", "); }
        Declarations += Capture.Declaration.LeftChop(Capture.Name.Len()).TrimEnd() + TEXT("& ") + Capture.Name;
        Arguments += Capture.Name;
        CaptureArguments += TEXT("__CuvissSnapshot[__CuvissIndex].") + Capture.Name;
    }
    FString ParameterArguments;
    int32 SegmentStart = ParametersOpen + 1;
    int32 Depth = 0;
    for (int32 At = SegmentStart; At <= ParametersClose; ++At)
    {
        if (At != ParametersClose && Kinds[At] != ELexicalKind::Code) continue;
        if (At != ParametersClose && (Code[At] == '<' || Code[At] == '(' || Code[At] == '[')) ++Depth;
        if (At != ParametersClose && (Code[At] == '>' || Code[At] == ')' || Code[At] == ']')) --Depth;
        if (At != ParametersClose && (Code[At] != ',' || Depth != 0)) continue;
        const int32 End = SkipTriviaBackward(Code, Kinds, At - 1);
        const int32 Start = SkipTriviaForward(Code, Kinds, SegmentStart);
        if (Start <= End)
        {
            int32 NameStart = End;
            while (NameStart > Start && IsIdentifierCharacter(Code[NameStart - 1])) --NameStart;
            if (!IsIdentifierStart(Code[NameStart]) || !IsIdentifierCharacter(Code[End]))
            { OutError = TEXT("lambda parameters require names and no default values"); return false; }
            if (!Declarations.IsEmpty()) { Declarations += TEXT(", "); Arguments += TEXT(", "); CaptureArguments += TEXT(", "); }
            Declarations += Code.Mid(Start, End - Start + 1);
            const FString Name = Code.Mid(NameStart, End - NameStart + 1);
            Arguments += Name;
            CaptureArguments += Name;
            if (!ParameterArguments.IsEmpty()) ParameterArguments += TEXT(", ");
            ParameterArguments += Name;
        }
        SegmentStart = At + 1;
    }
    OutReplacement.BodyPosition = BodyOpen;
    OutReplacement.OwnerClassClose = OwnerClose;
    OutReplacement.Receiver = Receiver;
    OutReplacement.StartPosition = ReceiverStart;
    OutReplacement.EndPosition = Position;
    const FString Prefix = ReturnType == TEXT("void") ? TEXT("") : TEXT("return ");
    int32 StatementStart = ReceiverStart;
    while (StatementStart > 0)
    {
        const int32 Previous = StatementStart - 1;
        if (Kinds[Previous] == ELexicalKind::Code && (Code[Previous] == ';' || Code[Previous] == '{' || Code[Previous] == '}')) break;
        --StatementStart;
    }
    StatementStart = SkipTriviaForward(Code, Kinds, StatementStart);
    const FString Assignment = Code.Mid(StatementStart, ReceiverStart - StatementStart).TrimStartAndEnd();
    const bool bAssignHandle = Assignment.EndsWith(TEXT("=")) && !Assignment.EndsWith(TEXT("=="));
    const bool bReturnHandle = Assignment == TEXT("return");
    if (bAssignHandle || bReturnHandle) OutReplacement.StartPosition = StatementStart;
    OutReplacement.OwnerClassName = OwnerClass;
    const FString ResultName = ClassName + TEXT("_Handle");
    if (Captures.IsEmpty())
    {
        const FString BodyName = ClassName + TEXT("_Body");
        const FString NativeEntry = ClassName + TEXT("_NativeEntry");
        OutReplacement.DebugBodyFunctions = {BodyName, NativeEntry};
        OutReplacement.OwnerMethod = FString::Printf(TEXT("\n %s %s%s __generated %s\n"), *ReturnType, *BodyName, *Parameters, *Body);
        OutReplacement.OwnerMethod += FString::Printf(TEXT("\n UFUNCTION() %s %s%s __generated %s\n"), *ReturnType, *NativeEntry, *Parameters, *Body);
        const int32 SlotCount = GetSlotsPerSite();
        for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
        {
            const FString Entry = FString::Printf(TEXT("%s_Slot_%d"), *ClassName, SlotIndex);
            FString Invocation = FString::Printf(TEXT("{ __CuvissRequireLambdaGameThread(); __CuvissLambdaInvocation __CuvissGuard(this, n\"%s\"); for (int __CuvissIndex = 0; __CuvissIndex < __CuvissGuard.Num(); ++__CuvissIndex) { if (!__CuvissGuard.IsBound()) break; if (!__CuvissGuard.IsItemLive(__CuvissIndex)) continue; "), *Entry);
            Invocation += Prefix + BodyName + TEXT("(") + ParameterArguments + TEXT("); }");
            if (ReturnType != TEXT("void")) Invocation += ReturnType + TEXT(" __CuvissEmpty; return __CuvissEmpty;");
            Invocation += TEXT(" }");
            OutReplacement.OwnerMethod += FString::Printf(TEXT(" UFUNCTION() %s %s%s __generated %s "), *ReturnType, *Entry, *Parameters, *Invocation);
        }
        const FString EntryName = ClassName + TEXT("_Entry");
        const FString TargetName = ClassName + TEXT("_Target");
        OutReplacement.Replacement = FString::Printf(TEXT("FScriptLambdaHandle %s; { FName %s; UObject %s = __CuvissPrepareDirectLambda(this, n\"%s\", %s, %s, %d, %s, %s, %s, n\"%s\"); %s.%sUFunction(%s, %s); __CuvissTrackLambdaSlot(%s, %s, %s); }"),
            *ResultName, *EntryName, *TargetName, *ClassName, *BindReceiver, Candidate.bMulticast ? TEXT("true") : TEXT("false"), SlotCount,
            *EntryName, *ResultName, *HolderName, *RootProperty, *BindReceiver, Candidate.bMulticast ? TEXT("Add") : TEXT("Bind"), *TargetName, *EntryName, *TargetName, *EntryName, *BindReceiver);
        const FString Dynamic = OutReplacement.Replacement;
        OutReplacement.Replacement = FString::Printf(TEXT("FScriptLambdaHandle %s; if (__CuvissIsNativeDelegate(%s)) { __CuvissBindNativeLambda(this, n\"%s\", %s, %s); } else { %s %s = %s; }"),
            *ResultName, *BindReceiver, *NativeEntry, *BindReceiver, *ResultName, *Dynamic.Replace(*ResultName, *(ResultName + TEXT("_Dynamic"))), *ResultName, *(ResultName + TEXT("_Dynamic")));

    }
    else
    {
        const FString CaptureType = TEXT("F") + ClassName + TEXT("_Values");
        const FString Slot = TEXT("__CuvissLambdaStore_") + FilenameHash + TEXT("_") + OwnerClass;
        OutReplacement.OwnerStoreName = Slot;
        const FString BodyName = ClassName + TEXT("_Body");
        const FString GlobalBody = ClassName + TEXT("_GlobalBody");
        FString Generated = TEXT("\nstruct ") + CaptureType + TEXT("\n{\n");
        for (const auto& Capture : Captures) Generated += TEXT("UPROPERTY()\n") + Capture.Declaration + TEXT(";\n");
        Generated += TEXT("}\n");
        FString OwnerBody = Body;
        OutReplacement.DebugBodyFunctions.Add(bCaptureThis ? BodyName : GlobalBody);
        if (!bCaptureThis)
        {
            Generated += ReturnType + TEXT(" ") + GlobalBody + TEXT("(") + Declarations + TEXT(") __generated ") + Body + TEXT("\n");
            OwnerBody = TEXT("{ ") + Prefix + GlobalBody + TEXT("(") + Arguments + TEXT("); }");
        }
        OutReplacement.OwnerMethod = FString::Printf(TEXT("\n %s %s(%s) __generated %s\n"),
            *ReturnType, *BodyName, *Declarations, *OwnerBody);
        const int32 SlotCount = GetSlotsPerSite();
        for (int32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
        {
            const FString Entry = FString::Printf(TEXT("%s_Slot_%d"), *ClassName, SlotIndex);
            FString EntryBody = FString::Printf(TEXT("{ __CuvissRequireLambdaGameThread(); __CuvissLambdaInvocation __CuvissGuard(this, n\"%s\"); for (int __CuvissIndex = 0; __CuvissIndex < __CuvissGuard.Num(); ++__CuvissIndex) { if (!__CuvissGuard.IsBound()) break; if (!__CuvissGuard.IsItemLive(__CuvissIndex)) continue; %s __CuvissSnapshot; __CuvissGuard.Read(__CuvissIndex, __CuvissSnapshot); "), *Entry, *CaptureType);
            const FString CallArguments = CaptureArguments.Replace(TEXT("__CuvissSnapshot[__CuvissIndex]."), TEXT("__CuvissSnapshot."));
            if (ReturnType == TEXT("void"))
                EntryBody += BodyName + TEXT("(") + CallArguments + TEXT("); __CuvissGuard.Commit(__CuvissIndex, __CuvissSnapshot); } }");
            else
                EntryBody += ReturnType + TEXT(" __CuvissResult = ") + BodyName + TEXT("(") + CallArguments
                    + TEXT("); __CuvissGuard.Commit(__CuvissIndex, __CuvissSnapshot); return __CuvissResult; } ")
                    + ReturnType + TEXT(" __CuvissEmpty; return __CuvissEmpty; }");
            OutReplacement.OwnerMethod += FString::Printf(TEXT(" UFUNCTION() %s %s%s __generated %s "), *ReturnType, *Entry, *Parameters, *EntryBody);
        }

        const FString NativeEntry = ClassName + TEXT("_NativeEntry");
        FString NativeParameters = CaptureType + TEXT("& __CuvissNativeCapture");
        const FString DelegateParameters = Parameters.Mid(1, Parameters.Len() - 2).TrimStartAndEnd();
        if (!DelegateParameters.IsEmpty()) NativeParameters += TEXT(", ") + DelegateParameters;
        const FString NativeArguments = CaptureArguments.Replace(TEXT("__CuvissSnapshot[__CuvissIndex]."), TEXT("__CuvissNativeCapture."));
        OutReplacement.OwnerMethod += FString::Printf(TEXT(" UFUNCTION() %s %s(%s) __generated { %s%s(%s); } "),
            *ReturnType, *NativeEntry, *NativeParameters, *Prefix, *BodyName, *NativeArguments);
        const FString CaptureName = ClassName + TEXT("_Capture");
        const FString TargetName = ClassName + TEXT("_Target");
        FString Replacement = CaptureType + TEXT(" ") + CaptureName + TEXT(";");
        for (const auto& Capture : Captures) Replacement += TEXT(" ") + CaptureName + TEXT(".") + Capture.Name + TEXT(" = ") + Capture.Name + TEXT(";");
        const FString EntryName = ClassName + TEXT("_Entry");
        Replacement += FString::Printf(TEXT(" FName %s; UObject %s = __CuvissPrepareLambdaSlot(this, n\"%s\", n\"%s\", n\"%s\", %s, %s, %s, %d, %s, %s, %s, n\"%s\"); %s.%sUFunction(%s, %s); __CuvissTrackLambdaSlot(%s, %s, %s);"),
            *EntryName, *TargetName, *ClassName, *Slot, *BodyName, *BindReceiver, *CaptureName,
            Candidate.bMulticast ? TEXT("true") : TEXT("false"), SlotCount, *EntryName, *ResultName, *HolderName, *RootProperty,
            *BindReceiver, Candidate.bMulticast ? TEXT("Add") : TEXT("Bind"), *TargetName, *EntryName, *TargetName, *EntryName, *BindReceiver);
        // The capture initialization is shared; native storage bypasses owner slots.
        const int32 PrepareStart = Replacement.Find(TEXT(" FName ") + EntryName);
        const FString Initialize = Replacement.Left(PrepareStart);
        const FString DynamicBind = Replacement.Mid(PrepareStart);
        OutReplacement.Replacement = FString::Printf(TEXT("FScriptLambdaHandle %s; { %s if (__CuvissIsNativeDelegate(%s)) { __CuvissBindNativeLambda(this, n\"%s\", %s, %s, %s); } else { %s } }"),
            *ResultName, *Initialize, *BindReceiver, *NativeEntry, *BindReceiver, *CaptureName, *ResultName, *DynamicBind);
        OutReplacement.GeneratedCode = MoveTemp(Generated);
    }
    OutReplacement.Replacement = TEXT("auto ") + HolderName + TEXT(" = ") + Holder + TEXT("; ") + OutReplacement.Replacement;
    if (bAssignHandle) OutReplacement.Replacement += TEXT(" ") + Assignment + TEXT(" ") + ResultName + TEXT(";");
    if (bReturnHandle) OutReplacement.Replacement += TEXT(" return ") + ResultName + TEXT(";");
    if (!bAssignHandle && !bReturnHandle) OutReplacement.Replacement = TEXT("{ ") + OutReplacement.Replacement + TEXT(" }");
    for (int32 At = OutReplacement.StartPosition; At <= Position; ++At)
        if (Code[At] == '\n') OutReplacement.Replacement += '\n';
    return true;
}
}
