// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ExplicitCaptureLambdaExtension.h"
#include "AngelscriptType.h"

namespace SoloASExplicitCaptureLambdaPrivate
{
namespace
{
FString CanonicalType(const FString& Type)
{
    FString Result;
    bool bSpace = false;
    for (TCHAR Character : Type)
    {
        if (FChar::IsWhitespace(Character)) { bSpace = true; continue; }
        if (bSpace && !Result.IsEmpty() && IsIdentifierCharacter(Result[Result.Len() - 1]) && IsIdentifierCharacter(Character)) Result += ' ';
        Result += Character;
        bSpace = false;
    }
    return Result;
}

FString ValueType(FString Type)
{
    Type.TrimStartAndEndInline();
    Type.RemoveFromStart(TEXT("const "));
    Type.ReplaceInline(TEXT("&in"), TEXT(""));
    Type.ReplaceInline(TEXT("&out"), TEXT(""));
    Type.ReplaceInline(TEXT("&"), TEXT(""));
    return CanonicalType(Type);
}

// Parameters and captures use the same declaration grammar. References are
// stripped only for validation; the synchronous signature retains them.
bool ParseDeclarations(const FString& Text, bool bReferences, TArray<FCaptureDeclaration>& Out, FString& Error, bool bCapture = false)
{
    if (Text.TrimStartAndEnd().IsEmpty()) return true;
    TArray<ELexicalKind> Kinds;
    BuildLexicalKinds(Text, Kinds);
    int32 Start = 0, Depth = 0;
    TSet<FString> Names;
    for (int32 At = 0; At <= Text.Len(); ++At)
    {
        if (At < Text.Len() && Kinds[At] != ELexicalKind::Code) continue;
        if (At < Text.Len() && Text[At] == '<') ++Depth;
        if (At < Text.Len() && Text[At] == '>') --Depth;
        if (At < Text.Len() && (Text[At] != ',' || Depth != 0)) continue;
        FString Declaration = Text.Mid(Start, At - Start).TrimStartAndEnd();
        Start = At + 1;
        FString Validated = Declaration;
        if (bReferences)
        {
            Validated.ReplaceInline(TEXT("&in"), TEXT(" "));
            Validated.ReplaceInline(TEXT("&out"), TEXT(" "));
            Validated.ReplaceInline(TEXT("&"), TEXT(" "));
        }
        TArray<ELexicalKind> SegmentKinds;
        BuildLexicalKinds(Validated, SegmentKinds);
        FCaptureDeclaration Capture;
        if (!ParseCaptureSegment(Validated, SegmentKinds, 0, Validated.Len() - 1, Names, Capture, Error)) return false;
        if (Declaration.Contains(TEXT("=")) || Declaration.Contains(TEXT("&&")))
        { Error = TEXT("algorithm captures require a type and name, without initializers"); return false; }
        if (bCapture && Declaration.Contains(TEXT("&")))
        {
            const FString Type = Declaration.LeftChop(Capture.Name.Len()).TrimEnd();
            if (Capture.Name == TEXT("this") || !Type.EndsWith(TEXT("&")) || Type.LeftChop(1).Contains(TEXT("&")))
            { Error = TEXT("reference captures require [Type& Name]"); return false; }
        }
        Capture.Declaration = Declaration;
        Out.Add(MoveTemp(Capture));
    }
    return true;
}

int32 ReceiverExpressionStart(const FString& Code, const TArray<ELexicalKind>& Kinds, int32 End)
{
    int32 Start = End;
    if (End < 0) return INDEX_NONE;
    if (Code[End] == ')' || Code[End] == ']')
    {
        const TCHAR Open = Code[End] == ')' ? '(' : '[';
        int32 Depth = 1;
        for (--Start; Start >= 0; --Start)
        {
            if (Kinds[Start] != ELexicalKind::Code) continue;
            if (Code[Start] == Code[End]) ++Depth;
            if (Code[Start] == Open && --Depth == 0) break;
        }
        if (Start < 0) return INDEX_NONE;
        const int32 Previous = SkipTriviaBackward(Code, Kinds, Start - 1);
        if (Previous >= 0 && (IsIdentifierCharacter(Code[Previous]) || Code[Previous] == ')' || Code[Previous] == ']'))
            Start = ReceiverExpressionStart(Code, Kinds, Previous);
    }
    else
    {
        if (!IsIdentifierCharacter(Code[End])) return INDEX_NONE;
        while (Start > 0 && Kinds[Start - 1] == ELexicalKind::Code && IsIdentifierCharacter(Code[Start - 1])) --Start;
        if (!IsIdentifierStart(Code[Start])) return INDEX_NONE;
    }
    if (Start == INDEX_NONE) return Start;
    const int32 Dot = SkipTriviaBackward(Code, Kinds, Start - 1);
    if (Dot >= 0 && Code[Dot] == '.') return ReceiverExpressionStart(Code, Kinds, SkipTriviaBackward(Code, Kinds, Dot - 1));
    return Start;
}

// Inspect explicit declarations only; element types still come from the lambda.
FString FindContainerType(const FString& Code, const TArray<ELexicalKind>& Kinds,
    const FString& Receiver, int32 CallPosition)
{
    TArray<int32> CallScopes;
    for (int32 At = 0; At < CallPosition; ++At)
    {
        if (Kinds[At] != ELexicalKind::Code) continue;
        if (Code[At] == '{') CallScopes.Add(At);
        if (Code[At] == '}' && !CallScopes.IsEmpty()) CallScopes.Pop(EAllowShrinking::No);
    }
    FString Result;
    TArray<int32> Scopes;
    int32 BestDepth = -1;
    for (int32 At = 0; At < Code.Len(); ++At)
    {
        if (Kinds[At] != ELexicalKind::Code) continue;
        if (Code[At] == '{') Scopes.Add(At);
        if (Code[At] == '}' && !Scopes.IsEmpty()) Scopes.Pop(EAllowShrinking::No);
        if (Scopes.Num() > CallScopes.Num() || Scopes.Num() < BestDepth) continue;
        bool bVisible = true;
        for (int32 Index = 0; Index < Scopes.Num(); ++Index)
            if (Scopes[Index] != CallScopes[Index]) { bVisible = false; break; }
        if (!bVisible || (At > CallPosition && Scopes.Num() == CallScopes.Num())) continue;
        if (!MatchesIdentifierAt(Code, Kinds, At, TEXT("TArray"))
            && !MatchesIdentifierAt(Code, Kinds, At, TEXT("TSet"))
            && !MatchesIdentifierAt(Code, Kinds, At, TEXT("TMap"))) continue;
        int32 Open = At;
        while (Open < Code.Len() && IsIdentifierCharacter(Code[Open])) ++Open;
        Open = SkipTriviaForward(Code, Kinds, Open);
        if (Open >= Code.Len() || Code[Open] != '<') continue;
        int32 End = Open + 1, Depth = 1;
        for (; End < Code.Len() && Depth > 0; ++End)
        {
            if (Kinds[End] != ELexicalKind::Code) continue;
            if (Code[End] == '<') ++Depth;
            if (Code[End] == '>') --Depth;
        }
        if (Depth) continue;
        int32 Name = SkipTriviaForward(Code, Kinds, End);
        if (Name < Code.Len() && Code[Name] == '&') Name = SkipTriviaForward(Code, Kinds, Name + 1);
        if (MatchesIdentifierAt(Code, Kinds, Name, *Receiver))
        { Result = Code.Mid(At, End - At); BestDepth = Scopes.Num(); }
    }
    return Result;
}

FString DeclarationType(const FCaptureDeclaration& Declaration)
{
    return CanonicalType(Declaration.Declaration.LeftChop(Declaration.Name.Len()));
}

FString CaptureParameter(const FString& Type, bool bObjectValue)
{
    if (Type.Contains(TEXT("&")))
    {
        FString Reference = Type;
        Reference.ReplaceInline(TEXT("&in"), TEXT("&"));
        Reference.ReplaceInline(TEXT("&out"), TEXT("&"));
        return Reference;
    }
    const FString Value = ValueType(Type);
    if (bObjectValue) return Type;
    if (Value == TEXT("bool") || Value == TEXT("int") || Value == TEXT("int32") || Value == TEXT("int64")
        || Value == TEXT("uint") || Value == TEXT("uint32") || Value == TEXT("uint64")
        || Value == TEXT("float") || Value == TEXT("float32") || Value == TEXT("float64") || Value == TEXT("double")) return Value;
    return TEXT("const ") + Value + TEXT("&");
}

FString PredicateCall(const FAlgorithmGroup& Group, const FString& A, const FString& B = FString())
{
    FString Args = TEXT("__CuvissSite") + Group.CaptureArguments + TEXT(", ") + A;
    if (!B.IsEmpty()) Args += TEXT(", ") + B;
    return Group.Name + TEXT("_Predicate(") + Args + TEXT(")");
}

FString AlgorithmCode(const FAlgorithmGroup& Group)
{
    const FString Signature = TEXT("int __CuvissSite") + Group.CaptureDeclarations;
    const FString EntrySignature = Signature.Replace(TEXT("__CuvissSnapshot"), TEXT("__CuvissInput"));
    FString SnapshotPrologue;
    TArray<FCaptureDeclaration> BorrowedCaptures;
    FString DeclarationError;
    verify(ParseDeclarations(Group.CaptureDeclarations.IsEmpty() ? FString() : Group.CaptureDeclarations.Mid(2), true, BorrowedCaptures, DeclarationError));
    for (const auto& Capture : BorrowedCaptures)
        if (Capture.Name.StartsWith(TEXT("__CuvissSnapshot")))
            SnapshotPrologue += ValueType(DeclarationType(Capture)) + TEXT(" ") + Capture.Name + TEXT(" = ")
                + Capture.Name.Replace(TEXT("__CuvissSnapshot"), TEXT("__CuvissInput")) + TEXT(";");
    FString Code = TEXT(" bool ") + Group.Name + TEXT("_Predicate(") + Signature + Group.ParameterDeclarations
        + TEXT(") __generated { switch (__CuvissSite) {") + Group.DispatchCases + TEXT("} return false; } ");
    const FString& Type = Group.ElementType;
    const FString& Algorithm = Group.Algorithm;
    FString ReturnType = TEXT("void");
    if (Algorithm == TEXT("IndexOfByPredicate") || Algorithm == TEXT("RemoveAll")) ReturnType = TEXT("int");
    if (Algorithm == TEXT("ContainsByPredicate") || Algorithm == TEXT("FindByPredicate")) ReturnType = TEXT("bool");
    if (Algorithm == TEXT("FilterByPredicate")) ReturnType = Group.ContainerType;
    FString Body;
    const FString Test = PredicateCall(Group, TEXT("__CuvissItems[__CuvissI]"));
    if (Algorithm == TEXT("FindByPredicate"))
        Body = TEXT("for (int __CuvissI = 0; __CuvissI < __CuvissItems.Num(); ++__CuvissI) if (") + Test + TEXT(") { __CuvissFound = __CuvissItems[__CuvissI]; return true; } return false;");
    else if (Algorithm == TEXT("IndexOfByPredicate") || Algorithm == TEXT("ContainsByPredicate"))
    {
        Body = TEXT("for (int __CuvissI = 0; __CuvissI < __CuvissItems.Num(); ++__CuvissI) if (") + Test + TEXT(") return ")
            + (Algorithm == TEXT("ContainsByPredicate") ? TEXT("true") : TEXT("__CuvissI")) + TEXT("; return ")
            + (Algorithm == TEXT("ContainsByPredicate") ? TEXT("false;") : TEXT("-1;"));
    }
    else if (Algorithm == TEXT("RemoveAll"))
    {
        // Compact in place, evaluating the predicate exactly once per element.
        Body = TEXT("int __CuvissWrite = 0; int __CuvissCount = __CuvissItems.Num(); for (int __CuvissI = 0; __CuvissI < __CuvissCount; ++__CuvissI) { if (!(")
            + Test + TEXT(")) { if (__CuvissWrite != __CuvissI) __CuvissItems[__CuvissWrite] = __CuvissItems[__CuvissI]; ++__CuvissWrite; } } __CuvissItems.SetNum(__CuvissWrite); return __CuvissCount - __CuvissWrite;");
    }
    else if (Algorithm == TEXT("FilterByPredicate"))
    {
        Body = Group.ContainerType + TEXT(" __CuvissResult;");
        if (Group.ContainerType.StartsWith(TEXT("TSet<")))
            Body += TEXT("for (const auto& __CuvissValue : __CuvissItems) if (") + PredicateCall(Group, TEXT("__CuvissValue")) + TEXT(") __CuvissResult.Add(__CuvissValue);");
        else if (Group.ContainerType.StartsWith(TEXT("TMap<")))
            Body += TEXT("for (auto __CuvissIt : __CuvissItems) if (") + PredicateCall(Group, TEXT("__CuvissIt.GetKey()"), TEXT("__CuvissIt.GetValue()"))
                + TEXT(") __CuvissResult.Add(__CuvissIt.GetKey(), __CuvissIt.GetValue());");
        else
            Body += TEXT("for (int __CuvissI = 0; __CuvissI < __CuvissItems.Num(); ++__CuvissI) if (") + Test + TEXT(") __CuvissResult.Add(__CuvissItems[__CuvissI]);");
        Body += TEXT("return __CuvissResult;");
    }
    else if (Algorithm == TEXT("KeySort") || Algorithm == TEXT("ValueSort"))
    {
        int32 Comma = INDEX_NONE, Depth = 0;
        for (int32 At = 5; At < Group.ContainerType.Len() - 1; ++At)
        {
            if (Group.ContainerType[At] == '<') ++Depth;
            if (Group.ContainerType[At] == '>') --Depth;
            if (Group.ContainerType[At] == ',' && Depth == 0) { Comma = At; break; }
        }
        const FString KeyType = Group.ContainerType.Mid(5, Comma - 5).TrimStartAndEnd();
        const FString MapValueType = Group.ContainerType.Mid(Comma + 1, Group.ContainerType.Len() - Comma - 2).TrimStartAndEnd();
        const FString Arrays = TEXT("TArray<") + KeyType + TEXT(">& __CuvissKeys, TArray<") + MapValueType + TEXT(">& __CuvissValues");
        const FString Sorted = Algorithm == TEXT("KeySort") ? TEXT("__CuvissKeys") : TEXT("__CuvissValues");
        Code += TEXT(" void ") + Group.Name + TEXT("_Sift(") + Arrays + TEXT(", int __CuvissInitialRoot, int __CuvissEnd, ") + Signature + TEXT(") __generated {")
            + TEXT("int __CuvissRoot = __CuvissInitialRoot; while (__CuvissRoot * 2 + 1 < __CuvissEnd) { int __CuvissChild = __CuvissRoot * 2 + 1; if (__CuvissChild + 1 < __CuvissEnd && ")
            + PredicateCall(Group, Sorted + TEXT("[__CuvissChild]"), Sorted + TEXT("[__CuvissChild + 1]"))
            + TEXT(") ++__CuvissChild; if (!(") + PredicateCall(Group, Sorted + TEXT("[__CuvissRoot]"), Sorted + TEXT("[__CuvissChild]"))
            + TEXT(")) break; __CuvissKeys.Swap(__CuvissRoot, __CuvissChild); __CuvissValues.Swap(__CuvissRoot, __CuvissChild); __CuvissRoot = __CuvissChild; } } ");
        Body = TEXT("TArray<") + KeyType + TEXT("> __CuvissKeys; TArray<") + MapValueType + TEXT("> __CuvissValues; __CuvissItems.GetKeys(__CuvissKeys); __CuvissItems.GetValues(__CuvissValues);")
            + TEXT("int __CuvissN = __CuvissKeys.Num(); for (int __CuvissStart = (__CuvissN >> 1) - 1; __CuvissStart >= 0; --__CuvissStart) ")
            + Group.Name + TEXT("_Sift(__CuvissKeys, __CuvissValues, __CuvissStart, __CuvissN, __CuvissSite") + Group.CaptureArguments + TEXT(");")
            + TEXT("for (int __CuvissEnd = __CuvissN - 1; __CuvissEnd > 0; --__CuvissEnd) { __CuvissKeys.Swap(0, __CuvissEnd); __CuvissValues.Swap(0, __CuvissEnd); ")
            + Group.Name + TEXT("_Sift(__CuvissKeys, __CuvissValues, 0, __CuvissEnd, __CuvissSite") + Group.CaptureArguments + TEXT("); }")
            + TEXT("__CuvissItems.Empty(__CuvissN); for (int __CuvissI = 0; __CuvissI < __CuvissN; ++__CuvissI) __CuvissItems.Add(__CuvissKeys[__CuvissI], __CuvissValues[__CuvissI]);");
    }
    else if (Algorithm == TEXT("StableSort"))
    {
        // Bottom-up merge sort. Prefer the left item on equality.
        Body = TEXT("int __CuvissN = __CuvissItems.Num(); TArray<") + Type + TEXT("> __CuvissBuffer = __CuvissItems;")
            + TEXT("for (int __CuvissWidth = 1; __CuvissWidth < __CuvissN; __CuvissWidth *= 2) {")
            + TEXT("for (int __CuvissStart = 0; __CuvissStart < __CuvissN; __CuvissStart += 2 * __CuvissWidth) {")
            + TEXT("int __CuvissMid = Math::Min(__CuvissStart + __CuvissWidth, __CuvissN); int __CuvissEnd = Math::Min(__CuvissStart + 2 * __CuvissWidth, __CuvissN);")
            + TEXT("int __CuvissLeft = __CuvissStart; int __CuvissRight = __CuvissMid; for (int __CuvissOut = __CuvissStart; __CuvissOut < __CuvissEnd; ++__CuvissOut) {")
            + TEXT("if (__CuvissLeft < __CuvissMid && (__CuvissRight >= __CuvissEnd || !(")
            + PredicateCall(Group, TEXT("__CuvissItems[__CuvissRight]"), TEXT("__CuvissItems[__CuvissLeft]"))
            + TEXT("))) { __CuvissBuffer[__CuvissOut] = __CuvissItems[__CuvissLeft]; ++__CuvissLeft; } else { __CuvissBuffer[__CuvissOut] = __CuvissItems[__CuvissRight]; ++__CuvissRight; } }")
            + TEXT("} __CuvissItems = __CuvissBuffer; }");
    }
    else if (Algorithm == TEXT("Sort"))
    {
        // Heap sort bounds stack and worst-case comparisons without native callbacks.
        Body = TEXT("int __CuvissN = __CuvissItems.Num(); for (int __CuvissStart = (__CuvissN >> 1) - 1; __CuvissStart >= 0; --__CuvissStart) ")
            + Group.Name + TEXT("_Sift(__CuvissItems, __CuvissStart, __CuvissN, __CuvissSite") + Group.CaptureArguments + TEXT(");")
            + TEXT("for (int __CuvissEnd = __CuvissN - 1; __CuvissEnd > 0; --__CuvissEnd) { __CuvissItems.Swap(0, __CuvissEnd); ")
            + Group.Name + TEXT("_Sift(__CuvissItems, 0, __CuvissEnd, __CuvissSite") + Group.CaptureArguments + TEXT("); }");
        Code += TEXT(" void ") + Group.Name + TEXT("_Sift(") + Group.ContainerType
            + TEXT("& __CuvissItems, int __CuvissInitialRoot, int __CuvissEnd, ") + Signature + TEXT(") __generated {")
            + TEXT("int __CuvissRoot = __CuvissInitialRoot; while (__CuvissRoot * 2 + 1 < __CuvissEnd) { int __CuvissChild = __CuvissRoot * 2 + 1; if (__CuvissChild + 1 < __CuvissEnd && ")
            + PredicateCall(Group, TEXT("__CuvissItems[__CuvissChild]"), TEXT("__CuvissItems[__CuvissChild + 1]"))
            + TEXT(") ++__CuvissChild; if (!(") + PredicateCall(Group, TEXT("__CuvissItems[__CuvissRoot]"), TEXT("__CuvissItems[__CuvissChild]"))
            + TEXT(")) break; __CuvissItems.Swap(__CuvissRoot, __CuvissChild); __CuvissRoot = __CuvissChild; } } ");
    }
    const bool bReadOnly = Algorithm == TEXT("FilterByPredicate") || Algorithm == TEXT("FindByPredicate")
        || Algorithm == TEXT("IndexOfByPredicate") || Algorithm == TEXT("ContainsByPredicate");
    Code += TEXT(" ") + ReturnType + TEXT(" ") + Group.Name + TEXT("(")
        + (bReadOnly ? TEXT("const ") : TEXT("")) + Group.ContainerType
        + TEXT("& __CuvissItems, ") + EntrySignature + (Algorithm == TEXT("FindByPredicate") ? TEXT(", ") + Type + TEXT("&out __CuvissFound") : TEXT("")) + TEXT(") __generated { ") + SnapshotPrologue + Body + TEXT(" } ");
    return Code;
}
}

bool ParseAlgorithmCandidate(const FString& Code, const TArray<ELexicalKind>& Kinds,
    const FLambdaCandidate& Candidate, const FString& FilenameHash,
    TArray<FAlgorithmGroup>& Groups, FSourceReplacement& Out, FString& Error)
{
    int32 ReceiverEnd = SkipTriviaBackward(Code, Kinds, Candidate.DotPosition - 1);
    const int32 ReceiverStart = ReceiverExpressionStart(Code, Kinds, ReceiverEnd);
    if (ReceiverStart == INDEX_NONE)
    { Error = TEXT("algorithm lambda receiver must be an identifier"); return false; }
    const FString Receiver = Code.Mid(ReceiverStart, ReceiverEnd - ReceiverStart + 1);
    int32 Position = SkipTriviaForward(Code, Kinds, Candidate.MethodPosition + Candidate.Algorithm.Len());
    const int32 CallOpen = Position;
    const int32 CaptureOpen = SkipTriviaForward(Code, Kinds, CallOpen + 1);
    int32 CaptureClose, ParametersClose, BodyClose, CallClose;
    if (!FindMatchingDelimiter(Code, Kinds, CaptureOpen, CaptureClose))
    { Error = TEXT("algorithm capture delimiters are malformed"); return false; }
    TArray<FCaptureDeclaration> Captures;
    if (!ParseDeclarations(Code.Mid(CaptureOpen + 1, CaptureClose - CaptureOpen - 1), true, Captures, Error, true)) return false;
    const bool bCaptureThis = Captures.ContainsByPredicate([](const auto& Capture) { return Capture.Name == TEXT("this"); });
    Captures.RemoveAll([](const auto& Capture) { return Capture.Name == TEXT("this"); });
    const int32 ParametersOpen = SkipTriviaForward(Code, Kinds, CaptureClose + 1);
    if (ParametersOpen >= Code.Len() || Code[ParametersOpen] != '(' || !FindMatchingDelimiter(Code, Kinds, ParametersOpen, ParametersClose))
    { Error = TEXT("algorithm lambda parameters are malformed"); return false; }
    TArray<FCaptureDeclaration> Parameters;
    if (!ParseDeclarations(Code.Mid(ParametersOpen + 1, ParametersClose - ParametersOpen - 1), true, Parameters, Error)) return false;
    const bool bSort = Candidate.Algorithm == TEXT("Sort") || Candidate.Algorithm == TEXT("StableSort")
        || Candidate.Algorithm == TEXT("KeySort") || Candidate.Algorithm == TEXT("ValueSort");
    const bool bMapFilter = Candidate.Algorithm == TEXT("FilterByPredicate") && Parameters.Num() == 2;
    if (Parameters.Num() != (bSort || bMapFilter ? 2 : 1))
    { Error = TEXT("algorithm lambda has the wrong parameter count"); return false; }
    FString ElementType = ValueType(DeclarationType(Parameters[0]));
    if (bSort && ValueType(DeclarationType(Parameters[1])) != ElementType)
    { Error = TEXT("sort lambda parameters must have the same element type"); return false; }
    Position = SkipTriviaForward(Code, Kinds, ParametersClose + 1);
    if (Position + 1 < Code.Len() && Code[Position] == '-' && Code[Position + 1] == '>')
    {
        FString ReturnType;
        if (!ParseReturnType(Code, Kinds, Position + 2, Position, ReturnType, Error)) return false;
        if (ReturnType != TEXT("bool")) { Error = TEXT("algorithm predicate must return bool"); return false; }
    }
    const int32 BodyOpen = Position;
    if (BodyOpen >= Code.Len() || Code[BodyOpen] != '{' || !FindMatchingDelimiter(Code, Kinds, BodyOpen, BodyClose)
        || !FindMatchingDelimiter(Code, Kinds, CallOpen, CallClose))
    { Error = TEXT("algorithm lambda body or call delimiters are malformed"); return false; }
    FLambdaCandidate NestedCandidate;
    if (FindNextCandidate(Code, Kinds, BodyOpen + 1, NestedCandidate)
        && NestedCandidate.MethodPosition < BodyClose)
    { Error = TEXT("nested explicit-capture lambdas are not supported"); return false; }
    const int32 AfterBody = SkipTriviaForward(Code, Kinds, BodyClose + 1);
    FString OutArgument;
    if (Candidate.Algorithm == TEXT("FindByPredicate"))
    {
        if (AfterBody >= CallClose || Code[AfterBody] != ',')
        { Error = TEXT("FindByPredicate requires an output value after the lambda"); return false; }
        OutArgument = TEXT(", ") + Code.Mid(AfterBody + 1, CallClose - AfterBody - 1).TrimStartAndEnd();
    }
    else if (AfterBody != CallClose)
    { Error = TEXT("unexpected arguments after algorithm lambda"); return false; }
    int32 OwnerClose = INDEX_NONE;
    FString Owner;
    bool bStruct = false;
    for (int32 At = 0; At < ReceiverStart; ++At)
    {
        const bool bClass = MatchesIdentifierAt(Code, Kinds, At, TEXT("class"));
        const bool bIsStruct = MatchesIdentifierAt(Code, Kinds, At, TEXT("struct"));
        if (!bClass && !bIsStruct) continue;
        int32 NameStart = SkipTriviaForward(Code, Kinds, At + (bClass ? 5 : 6));
        int32 NameEnd = NameStart;
        while (NameEnd < Code.Len() && IsIdentifierCharacter(Code[NameEnd])) ++NameEnd;
        int32 Open = NameEnd, Close;
        while (Open < ReceiverStart && !(Kinds[Open] == ELexicalKind::Code && Code[Open] == '{')) ++Open;
        if (Open < ReceiverStart && FindMatchingDelimiter(Code, Kinds, Open, Close) && Close > CallClose)
        { Owner = Code.Mid(NameStart, NameEnd - NameStart); OwnerClose = Close; bStruct = bIsStruct; }
    }
    if (bCaptureThis && (OwnerClose == INDEX_NONE || bStruct))
    { Error = TEXT("[this] algorithm capture requires a UObject instance method"); return false; }
    if (bStruct) { OwnerClose = INDEX_NONE; Owner.Reset(); }
    FString ReceiverName = Receiver;
    ReceiverName.RemoveFromStart(TEXT("this."));
    FString ContainerType = FindContainerType(Code, Kinds, ReceiverName, ReceiverStart);
    if (bMapFilter) ContainerType = TEXT("TMap<") + ElementType + TEXT(", ") + ValueType(DeclarationType(Parameters[1])) + TEXT(">");
    if (ContainerType.IsEmpty() && Candidate.Algorithm == TEXT("FilterByPredicate"))
    {
        Error = TEXT("cannot infer array/set receiver type; first declare a local reference, e.g. TSet<int>& Items = Obj.Items; Items.FilterByPredicate([](int Value) -> bool { return Value > 0; })");
        return false;
    }
    if (ContainerType.IsEmpty()) ContainerType = TEXT("TArray<") + ElementType + TEXT(">");
    ContainerType = CanonicalType(ContainerType);
    const bool bMapSort = Candidate.Algorithm == TEXT("KeySort") || Candidate.Algorithm == TEXT("ValueSort");
    if (bMapSort && !ContainerType.StartsWith(TEXT("TMap<")))
    { Error = TEXT("cannot infer map receiver type; first declare a local reference, e.g. TMap<FName, int>& M = Obj.Scores; M.KeySort([](FName A, FName B) -> bool { return A.ToString() < B.ToString(); })"); return false; }
    if (ContainerType.StartsWith(TEXT("TSet<")) && Candidate.Algorithm != TEXT("FilterByPredicate"))
    { Error = TEXT("TSet only supports FilterByPredicate algorithm lambdas"); return false; }
    if (ContainerType.StartsWith(TEXT("TMap<")) && Candidate.Algorithm == TEXT("FilterByPredicate") && !bMapFilter)
    { Error = TEXT("TMap.FilterByPredicate requires two typed parameters: key and value"); return false; }
    if (ContainerType.StartsWith(TEXT("TMap<")) && !bMapSort && Candidate.Algorithm != TEXT("FilterByPredicate"))
    { Error = TEXT("this algorithm requires a TArray receiver"); return false; }
    auto IsObjectType = [&](const FString& Declaration)
    {
        const FString Name = ValueType(Declaration);
        const auto Registered = FAngelscriptType::GetByAngelscriptTypeName(Name);
        if (Registered.IsValid() && Registered->IsObjectPointer()) return true;
        for (int32 At = 0; At < Code.Len(); ++At)
        {
            if (!MatchesIdentifierAt(Code, Kinds, At, TEXT("class"))) continue;
            const int32 NameStart = SkipTriviaForward(Code, Kinds, At + 5);
            if (MatchesIdentifierAt(Code, Kinds, NameStart, *Name)) return true;
        }
        return false;
    };
    FString CaptureDeclarations, CaptureArguments, CallArguments, BodyDeclarations, BodyArguments, Key;
    Key = Owner + TEXT("|") + Candidate.Algorithm + TEXT("|") + ContainerType;
    for (int32 Index = 0; Index < Captures.Num(); ++Index)
    {
        const auto& Capture = Captures[Index];
        const FString Type = CaptureParameter(DeclarationType(Capture), IsObjectType(DeclarationType(Capture)));
        const bool bSnapshot = !DeclarationType(Capture).Contains(TEXT("&")) && Type.Contains(TEXT("&"));
        const FString Name = FString(bSnapshot ? TEXT("__CuvissSnapshot") : TEXT("__CuvissCap")) + FString::FromInt(Index);
        CaptureDeclarations += TEXT(", ") + Type + TEXT(" ") + Name;
        CaptureArguments += TEXT(", ") + Name;
        CallArguments += TEXT(", ") + Capture.Name;
        BodyDeclarations += (BodyDeclarations.IsEmpty() ? TEXT("") : TEXT(", ")) + Type + TEXT(" ") + Capture.Name;
        BodyArguments += (BodyArguments.IsEmpty() ? TEXT("") : TEXT(", ")) + Name;
        Key += TEXT("|") + Type;
        if (bSnapshot) Key += TEXT("|snapshot");
    }
    FString ParameterDeclarations;
    for (int32 Index = 0; Index < Parameters.Num(); ++Index)
    {
        const auto& Parameter = Parameters[Index];
        const FString Type = CaptureParameter(DeclarationType(Parameter), IsObjectType(DeclarationType(Parameter)));
        ParameterDeclarations += TEXT(", ") + Type + FString::Printf(TEXT(" __CuvissArg%d"), Index);
        BodyDeclarations += (BodyDeclarations.IsEmpty() ? TEXT("") : TEXT(", ")) + Parameter.Declaration;
        BodyArguments += (BodyArguments.IsEmpty() ? TEXT("") : TEXT(", ")) + FString::Printf(TEXT("__CuvissArg%d"), Index);
        Key += TEXT("|") + Type;
    }
    FAlgorithmGroup* Group = Groups.FindByPredicate([&](const auto& Item) { return Item.Key == Key; });
    if (!Group)
    {
        Group = &Groups.Emplace_GetRef();
        Group->Key = Key;
        FTCHARToUTF8 SignatureUtf8(*Key);
        uint8 SignatureHash[FSHA1::DigestSize];
        FSHA1::HashBuffer(SignatureUtf8.Get(), SignatureUtf8.Length(), SignatureHash);
        FString SignatureText;
        for (uint8 Byte : SignatureHash) SignatureText += FString::Printf(TEXT("%02x"), Byte);
        Group->Name = TEXT("__CuvissAlgorithm_") + FilenameHash + TEXT("_") + SignatureText;
        Group->CaptureDeclarations = CaptureDeclarations;
        Group->CaptureArguments = CaptureArguments;
        Group->ParameterDeclarations = ParameterDeclarations;
        Group->ElementType = ElementType;
        Group->ContainerType = ContainerType;
        Group->Algorithm = Candidate.Algorithm;
        Group->OwnerClose = OwnerClose;
    }
    const int32 Site = Group->Sites++;
    const FString BodyName = Group->Name + FString::Printf(TEXT("_Body%d"), Site);
    const FString Body = Code.Mid(BodyOpen, BodyClose - BodyOpen + 1);
    Out.BodyPosition = BodyOpen;
    Out.DebugBodyFunctions.Add(OwnerClose != INDEX_NONE && !bCaptureThis ? BodyName + TEXT("_Global") : BodyName);
    const FString Declaration = TEXT("\n bool ") + BodyName + TEXT("(") + BodyDeclarations + TEXT(") __generated ");
    if (OwnerClose != INDEX_NONE && !bCaptureThis)
    {
        Out.GeneratedCode = Declaration.Replace(*BodyName, *(BodyName + TEXT("_Global"))) + Body + TEXT("\n");
        FString ForwardArgs;
        for (const auto& Capture : Captures) ForwardArgs += (ForwardArgs.IsEmpty() ? TEXT("") : TEXT(", ")) + Capture.Name;
        for (const auto& Parameter : Parameters) ForwardArgs += (ForwardArgs.IsEmpty() ? TEXT("") : TEXT(", ")) + Parameter.Name;
        Out.OwnerMethod = Declaration + TEXT("{ return ") + BodyName + TEXT("_Global(") + ForwardArgs + TEXT("); } ");
    }
    else if (OwnerClose != INDEX_NONE) Out.OwnerMethod = Declaration + Body + TEXT("\n");
    else Out.GeneratedCode = Declaration + Body + TEXT("\n");
    Group->DispatchCases += FString::Printf(TEXT("case %d: return "), Site) + BodyName + TEXT("(") + BodyArguments + TEXT(");");
    Out.OwnerClassClose = OwnerClose;
    Out.StartPosition = ReceiverStart;
    Out.EndPosition = CallClose;
    Out.Replacement = Group->Name + TEXT("(") + Receiver + FString::Printf(TEXT(", %d"), Site) + CallArguments + OutArgument + TEXT(")");
    for (int32 At = ReceiverStart; At <= CallClose; ++At) if (Code[At] == '\n') Out.Replacement += '\n';
    return true;
}

void FinishAlgorithmGroups(const TArray<FAlgorithmGroup>& Groups, TArray<FSourceReplacement>& Replacements)
{
    for (const auto& Group : Groups)
    {
        auto* First = Replacements.FindByPredicate([&](const auto& Replacement) { return Replacement.Replacement.Contains(Group.Name + TEXT("(")); });
        check(First);
        if (Group.OwnerClose != INDEX_NONE) First->OwnerMethod += AlgorithmCode(Group);
        else First->GeneratedCode += AlgorithmCode(Group);
    }
}
}
