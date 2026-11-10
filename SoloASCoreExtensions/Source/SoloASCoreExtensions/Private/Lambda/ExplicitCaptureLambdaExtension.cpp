// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ExplicitCaptureLambdaExtension.h"

#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "AngelscriptCodeModule.h"
#include "AngelscriptOptionalProviders.h"
#include "Misc/CoreDelegates.h"
#include "Containers/Ticker.h"

#include "Misc/ConfigCacheIni.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/KismetSystemLibrary.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/SparseDelegate.h"
#include "UObject/UnrealType.h"
#include "ClassGenerator/ASClass.h"
#include "Serialization/Archive.h"
#include "AngelscriptBinds.h"
#include "Lambda/SoloASBridgeLambdaClosure.h"
#include "StartAngelscriptHeaders.h"
#include "angelscript.h"
#include "EndAngelscriptHeaders.h"
#include "AngelscriptManager.h"
#include "Templates/SubclassOf.h"
#include "Preprocessor/AngelscriptPreprocessor.h"
#include "Templates/SubclassOf.h"

namespace SoloASBridge::Private { void InitializeLambdaClosures(); void ShutdownLambdaClosures(); }

namespace SoloASExplicitCaptureLambdaPrivate
{
namespace
{
bool ReadEnabledPolicy(FConfigCacheIni& Config, const FString& IniFilename)
{
    bool bEnabled = true;
    Config.GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableExplicitCaptureLambda"), bEnabled, IniFilename);
    return bEnabled;
}
}

int32 GetSlotsPerSite()
{
    int32 Value = 2;
    GConfig->GetInt(TEXT("SoloASCoreExtensions"), TEXT("LambdaSlotsPerSite"), Value, GEngineIni);
    return FMath::Clamp(Value, 1, 32);
}

TMap<FObjectKey, asITypeInfo*> RegisteredScriptFields;
void ResetStorageMetadata()
{
    RegisteredScriptFields.Reset();
}

void RefreshStorageMetadata()
{
    for (UASClass* Class : TObjectRange<UASClass>())
    {
        auto* Type = Class->ScriptTypePtr;
        if (!Type || Class->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated) || RegisteredScriptFields.FindRef(FObjectKey(Class)) == Type) continue;
        TArray<TUniquePtr<FProperty>> Fields;
        TMap<FName, int32> Offsets;
        for (asUINT Index = 0; Index < Type->GetPropertyCount(); ++Index)
        {
            const char* Name = nullptr; int TypeId = 0, Offset = 0;
            bool Reference = false, Indirect = false; int Composite = 0;
            Type->GetProperty(Index, &Name, &TypeId, nullptr, nullptr, &Offset, &Reference, nullptr, &Composite, &Indirect);
            if (!Name || Reference || Indirect || Composite != 0 || Offset < 0 || FindFProperty<FProperty>(Class, FName(ANSI_TO_TCHAR(Name)))) continue;
            const auto Usage = FAngelscriptTypeUsage::FromTypeId(TypeId);
            if (!Usage.IsValid() || !Usage.CanCreateProperty()) continue;
            FAngelscriptType::FPropertyParams Params;
            Params.Struct = Class; Params.PropertyName = FName(ANSI_TO_TCHAR(Name));
            TUniquePtr<FProperty> Property(Usage.CreateProperty(Params));
            if (!Property) continue;
            FArchive Archive;
            Property->LinkWithoutChangingOffset(Archive);
            if (Offset + Property->GetSize() > Class->GetPropertiesSize()) continue;
            Offsets.Add(Property->GetFName(), Offset);
            Fields.Add(MoveTemp(Property));
        }
        SoloASBridge::RegisterLambdaScriptFields(Class, MoveTemp(Fields), MoveTemp(Offsets));
        RegisteredScriptFields.Add(FObjectKey(Class), Type);
    }
}

namespace
{
FTSTicker::FDelegateHandle NativeInvalidationTicker;
bool bNativeInvalidationPending = false;
}

void InvalidateNativeCaptures()
{
    using namespace AngelscriptCoreExtensionPoints;
    bNativeInvalidationPending = InvalidateNativeDelegateCaptures(FAngelscriptManager::Get().GetScriptEngine())
        == ENativeDelegateResult::Busy;
    if (bNativeInvalidationPending && !NativeInvalidationTicker.IsValid())
    {
        NativeInvalidationTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
        {
            using namespace AngelscriptCoreExtensionPoints;
            bNativeInvalidationPending = InvalidateNativeDelegateCaptures(FAngelscriptManager::Get().GetScriptEngine())
                == ENativeDelegateResult::Busy;
            if (!bNativeInvalidationPending) NativeInvalidationTicker.Reset();
            return bNativeInvalidationPending;
        }));
    }
}

bool IsEnabled()
{
    static const bool bEnabled = ReadEnabledPolicy(*GConfig, GEngineIni);
    return bEnabled;
}
}

namespace
{
USparseDelegateFunction* SparseSignature(int32 TypeId)
{
    const auto Usage = FAngelscriptTypeUsage::FromTypeId(TypeId);
    for (USparseDelegateFunction* Signature : TObjectRange<USparseDelegateFunction>())
        if (FAngelscriptType::GetByData(Signature) == Usage.Type) return Signature;
    return nullptr;
}

bool ValidateLambdaDelegate(int32 TypeId, bool bMulticast)
{
    const auto Usage = FAngelscriptTypeUsage::FromTypeId(TypeId);
    FAngelscriptType::FCppForm Form;
    if (Usage.IsValid() && Usage.Type->GetCppForm(Usage, Form))
    {
        if (Form.TemplateObjectForm == (bMulticast ? TEXT("FMulticastScriptDelegate") : TEXT("FScriptDelegate"))) return true;
    }
    if (bMulticast && SparseSignature(TypeId)) return true;
    FAngelscriptManager::Throw("Lambda binding requires a dynamic delegate/event or a registered native delegate provider.");
    return false;
}

}

namespace SoloASExplicitCaptureLambdaPrivate
{
namespace
{
bool IsIndependentStatementPrefix(
    const FString& Code,
    const TArray<ELexicalKind>& Kinds,
    int32 StatementStart,
    int32 CallStart
)
{
    int32 Position = SkipTriviaForward(Code, Kinds, StatementStart);
    while (Position < CallStart)
    {
        if (Kinds[Position] != ELexicalKind::Code || FChar::IsWhitespace(Code[Position]))
        {
            ++Position;
            continue;
        }

        if (Code[Position] == TEXT('('))
        {
            int32 Close = INDEX_NONE;
            if (!FindMatchingDelimiter(Code, Kinds, Position, Close) || Close >= CallStart)
                return false;
            Position = Close + 1;
            continue;
        }

        if (!IsIdentifierStart(Code[Position]))
            return false;

        const int32 IdentifierStart = Position++;
        while (Position < CallStart && Kinds[Position] == ELexicalKind::Code
            && IsIdentifierCharacter(Code[Position]))
        {
            ++Position;
        }
        const FString Keyword = Code.Mid(IdentifierStart, Position - IdentifierStart);
        if (Keyword != TEXT("if") && Keyword != TEXT("else") && Keyword != TEXT("for")
            && Keyword != TEXT("while") && Keyword != TEXT("switch") && Keyword != TEXT("do"))
        {
            return false;
        }
    }
    return true;
}

struct FLambdaReleaseRewriteDepth
{
    int32 Parentheses;
    int32 Brackets;
};
}

// Only statement calls on receivers that actually bind a lambda in this file
// get the fast release path. C++/Blueprint and all other paths use the GC sweep.
void RewriteLambdaReleaseStatements(FString& Code, const TSet<FString>& Receivers)
{
    TArray<ELexicalKind> Kinds;
    BuildLexicalKinds(Code, Kinds);
    TArray<int32> StatementStarts;
    StatementStarts.Init(0, Code.Len());
    TArray<FLambdaReleaseRewriteDepth> OuterDepths;
    int32 StatementStart = 0;
    int32 ParenthesisDepth = 0;
    int32 BracketDepth = 0;
    for (int32 Position = 0; Position < Code.Len(); ++Position)
    {
        StatementStarts[Position] = StatementStart;
        if (Kinds[Position] != ELexicalKind::Code)
            continue;
        const TCHAR Character = Code[Position];
        if (Character == TEXT('(')) ++ParenthesisDepth;
        else if (Character == TEXT(')')) --ParenthesisDepth;
        else if (Character == TEXT('[')) ++BracketDepth;
        else if (Character == TEXT(']')) --BracketDepth;
        else if (Character == TEXT('{'))
        {
            OuterDepths.Add({ ParenthesisDepth, BracketDepth });
            ParenthesisDepth = 0;
            BracketDepth = 0;
            StatementStart = Position + 1;
        }
        else if (Character == TEXT('}'))
        {
            if (OuterDepths.Num() > 0)
            {
                const FLambdaReleaseRewriteDepth Outer = OuterDepths.Pop(EAllowShrinking::No);
                ParenthesisDepth = Outer.Parentheses;
                BracketDepth = Outer.Brackets;
            }
            StatementStart = Position + 1;
        }
        else if (Character == TEXT(';') && ParenthesisDepth == 0 && BracketDepth == 0)
        {
            StatementStart = Position + 1;
        }
    }

    TArray<FSourceReplacement> Replacements;
    for (int32 Start = 0; Start < Code.Len(); ++Start)
    {
        if (Kinds[Start] != ELexicalKind::Code || !IsIdentifierStart(Code[Start])) continue;
        int32 End = Start + 1;
        while (End < Code.Len() && IsIdentifierCharacter(Code[End])) ++End;
        const FString Receiver = Code.Mid(Start, End - Start);
        const int32 Previous = SkipTriviaBackward(Code, Kinds, Start - 1);
        if (!Receivers.Contains(Receiver) || (Previous >= 0 && Code[Previous] == '.')) { Start = End - 1; continue; }
        int32 Dot = SkipTriviaForward(Code, Kinds, End);
        if (Dot >= Code.Len() || Code[Dot] != '.') { Start = End - 1; continue; }
        int32 MethodStart = SkipTriviaForward(Code, Kinds, Dot + 1);
        int32 MethodEnd = MethodStart;
        while (MethodEnd < Code.Len() && IsIdentifierCharacter(Code[MethodEnd])) ++MethodEnd;
        const FString Method = Code.Mid(MethodStart, MethodEnd - MethodStart);
        if (Method != TEXT("Clear") && Method != TEXT("Unbind")
            && Method != TEXT("BindUFunction")) continue;
        const int32 Open = SkipTriviaForward(Code, Kinds, MethodEnd);
        int32 Close = INDEX_NONE;
        if (Open >= Code.Len() || Code[Open] != '(' || !FindMatchingDelimiter(Code, Kinds, Open, Close)) continue;
        const int32 Semi = SkipTriviaForward(Code, Kinds, Close + 1);
        if (Semi >= Code.Len() || Code[Semi] != ';') continue;

        if (!IsIndependentStatementPrefix(Code, Kinds, StatementStarts[Start], Start))
        {
            Start = End - 1;
            continue;
        }

        auto& Replacement = Replacements.Emplace_GetRef();
        Replacement.StartPosition = Start;
        Replacement.EndPosition = Semi;
        FString Statement = Code.Mid(Start, Semi - Start + 1);
        // Single-cast uses Clear in the script binding API.
        if (Method == TEXT("Unbind") && SkipTriviaForward(Code, Kinds, Open + 1) == Close)
        {
            Statement = Receiver + TEXT(".Clear();");
            for (int32 At = Start; At <= Semi; ++At) if (Code[At] == '\n') Statement += '\n';
        }
        Replacement.Replacement = TEXT("{ ") + Statement + TEXT(" __CuvissSweepLambdaClosures(); }");
        Start = Semi;
    }
    for (int32 Index = Replacements.Num() - 1; Index >= 0; --Index)
    {
        const auto& Replacement = Replacements[Index];
        Code.RemoveAt(Replacement.StartPosition, Replacement.EndPosition - Replacement.StartPosition + 1);
        Code.InsertAt(Replacement.StartPosition, Replacement.Replacement);
    }
}

namespace
{
using FSourceFile = FAngelscriptPreprocessor::FFile;

int32 SourceLineAt(const FSourceFile& File, int32 Position)
{
    int32 Physical = 0;
    for (int32 Index = 0; Index < Position; ++Index) if (File.ProcessedCode[Index] == '\n') ++Physical;
    return File.SourceLines.IsValidIndex(Physical) ? File.SourceLines[Physical] : Physical + 1;
}

// All replacements preserve the original newlines. Inserted methods are separated
// from the surrounding user code, and their body lines are remapped below.
void ApplyMappedReplacements(FSourceFile& File, const TArray<FSourceReplacement>& Replacements, const FString& Generated)
{
    const FString OriginalSource = MoveTemp(File.ProcessedCode);
    TArray<int32> InputLines = MoveTemp(File.SourceLines);
    if (InputLines.IsEmpty())
    {
        InputLines.Add(1);
        for (TCHAR Character : OriginalSource) if (Character == '\n') InputLines.Add(InputLines.Num() + 1);
    }
    int32 Physical = 0;
    int32 Cursor = 0;
    bool bLineStart = true;
    auto Append = [&](TCHAR Character, int32 OriginalLine)
    {
        if (bLineStart) { File.SourceLines.Add(OriginalLine); bLineStart = false; }
        File.ProcessedCode.AppendChar(Character);
        if (Character == '\n') bLineStart = true;
    };
    auto OriginalLine = [&](int32 Index) { return InputLines.IsValidIndex(Index) ? InputLines[Index] : Index + 1; };
    auto CopyUntil = [&](int32 End)
    {
        while (Cursor < End)
        {
            const TCHAR Character = OriginalSource[Cursor++];
            Append(Character, OriginalLine(Physical));
            if (Character == '\n') ++Physical;
        }
    };
    for (const auto& Replacement : Replacements)
    {
        CopyUntil(Replacement.StartPosition);
        int32 ReplacementLine = Physical;
        const bool bInsertion = Replacement.EndPosition < Replacement.StartPosition;
        for (TCHAR Character : Replacement.Replacement)
        {
            Append(Character, OriginalLine(ReplacementLine));
            if (Character == '\n' && !bInsertion && ReplacementLine + 1 < InputLines.Num()) ++ReplacementLine;
        }
        while (Cursor <= Replacement.EndPosition)
            if (OriginalSource[Cursor++] == '\n') ++Physical;
    }
    CopyUntil(OriginalSource.Len());
    for (TCHAR Character : Generated) Append(Character, OriginalLine(Physical));
    if (bLineStart) File.SourceLines.Add(OriginalLine(Physical));
}

bool RegisterBodySource(FSourceFile& File, const FString& Name, int32 OriginalLine)
{
    const FString& Code = File.ProcessedCode;
    TArray<ELexicalKind> Kinds;
    BuildLexicalKinds(Code, Kinds);
    int32 Search = 0;
    while ((Search = Code.Find(Name, ESearchCase::CaseSensitive, ESearchDir::FromStart, Search)) != INDEX_NONE)
    {
        const int32 Start = Search;
        Search += Name.Len();
        if (!MatchesIdentifierAt(Code, Kinds, Start, *Name)) continue;
        int32 Open = SkipTriviaForward(Code, Kinds, Search), Close = INDEX_NONE;
        if (Open >= Code.Len() || Code[Open] != '(' || !FindMatchingDelimiter(Code, Kinds, Open, Close)) continue;
        int32 Trait = SkipTriviaForward(Code, Kinds, Close + 1);
        if (!MatchesIdentifierAt(Code, Kinds, Trait, TEXT("__generated"))) continue;
        Open = SkipTriviaForward(Code, Kinds, Trait + 11);
        if (Open >= Code.Len() || Code[Open] != '{' || !FindMatchingDelimiter(Code, Kinds, Open, Close)) continue;
        int32 DeclarationLine = 0, BodyLine = 0, EndLine = 0;
        for (int32 Index = 0; Index <= Close; ++Index)
            if (Code[Index] == '\n')
            {
                if (Index < Start) ++DeclarationLine;
                if (Index < Open) ++BodyLine;
                ++EndLine;
            }
        for (int32 Line = DeclarationLine; Line <= EndLine && File.SourceLines.IsValidIndex(Line); ++Line)
            File.SourceLines[Line] = OriginalLine + FMath::Max(0, Line - BodyLine);
        File.UserGeneratedFunctions.AddUnique(Name);
        return true;
    }
    return false;
}
}
}

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_SoloASExplicitCaptureLambda(
	(int32)FAngelscriptBinds::EOrder::Late - 1,
	[]
	{
        if (!SoloASExplicitCaptureLambdaPrivate::IsEnabled()) return;
        FAngelscriptBinds::BindGlobalGenericFunction("bool __CuvissIsNativeDelegate(const ?&in Delegate)", [](asIScriptGeneric* Generic)
        {
            auto* Type = Generic->GetEngine()->GetTypeInfoById(Generic->GetArgTypeId(0));
            Generic->SetReturnByte(AngelscriptCoreExtensionPoints::IsNativeDelegateType(Type) ? 1 : 0);
        });
        auto BindNative = [](asIScriptGeneric* Generic)
        {
            if (SoloASExplicitCaptureLambdaPrivate::bNativeInvalidationPending)
            {
                FAngelscriptManager::Throw("Native lambda captures are awaiting invalidation.");
                return;
            }
            const auto Result = AngelscriptCoreExtensionPoints::BindNativeDelegate(*Generic, 2);
            if (Result != AngelscriptCoreExtensionPoints::ENativeDelegateResult::Completed)
                FAngelscriptManager::Throw("Native lambda delegate provider rejected the binding or is unavailable.");
        };
        FAngelscriptBinds::BindGlobalGenericFunction(
            "void __CuvissBindNativeLambda(UObject Owner, const FName& Entry, ?&inout Delegate, FScriptLambdaHandle&out Handle)", BindNative);
        FAngelscriptBinds::BindGlobalGenericFunction(
            "void __CuvissBindNativeLambda(UObject Owner, const FName& Entry, ?&inout Delegate, const ?&in Capture, FScriptLambdaHandle&out Handle)", BindNative);
        using FInvocation = SoloASBridge::FLambdaClosureInvocation;
        auto Guard = FAngelscriptBinds::ValueClass<FInvocation>("__CuvissLambdaInvocation", FBindFlags());
        Guard.Constructor("void f(UObject Closure, const FName& Entry)", [](FInvocation* Address, UObject* Closure, const FName& Entry)
        {
            new(Address) FInvocation(IsInGameThread() ? Closure : nullptr, Entry);
            if (!IsInGameThread()) FAngelscriptManager::Throw("Lambda invocation requires the game thread.");
        });
        Guard.Destructor("void f()", [](FInvocation* Address) { Address->~FInvocation(); });
        Guard.Method("int Num() const", [](const FInvocation& Value) { return Value.Num(); });
        Guard.Method("bool IsBound() const", [](const FInvocation& Value) { return Value.IsBound(); });
        Guard.Method("bool IsItemLive(int Index) const", [](const FInvocation& Value, int Index) { return Value.IsItemLive(Index); });
        Guard.GenericMethod("void Read(int Index, ?&out Value) const", [](asIScriptGeneric* Generic)
        {
            const auto Usage = FAngelscriptTypeUsage::FromTypeId(Generic->GetArgTypeId(1));
            auto* Guard = static_cast<FInvocation*>(Generic->GetObject());
            if (!Guard->Read(int(Generic->GetArgDWord(0)), Cast<UScriptStruct>(Usage.GetUnrealStruct()), Generic->GetArgAddress(1)))
                FAngelscriptManager::Throw("Lambda capture snapshot type mismatch.");
        }, nullptr);
        Guard.GenericMethod("void Commit(int Index, const ?&in Value)", [](asIScriptGeneric* Generic)
        {
            const auto Usage = FAngelscriptTypeUsage::FromTypeId(Generic->GetArgTypeId(1));
            static_cast<FInvocation*>(Generic->GetObject())->Commit(int(Generic->GetArgDWord(0)),
                Cast<UScriptStruct>(Usage.GetUnrealStruct()), Generic->GetArgAddress(1));
        }, nullptr);
        FAngelscriptBinds::BindGlobalGenericFunction(
            "UObject __CuvissPrepareLambdaSlot(UObject Owner, const FName& Site, const FName& Store, const FName& Body, const ?&in Delegate, const ?&in Capture, bool bMulticast, int Slots, FName&out Entry, FScriptLambdaHandle&out Handle, UObject Holder, const FName& RootProperty)",
            [](asIScriptGeneric* Generic)
            {
                if (!IsInGameThread()) { FAngelscriptManager::Throw("Lambda binding requires the game thread."); return; }
                SoloASExplicitCaptureLambdaPrivate::RefreshStorageMetadata();
                if (!ValidateLambdaDelegate(Generic->GetArgTypeId(4), Generic->GetArgByte(6) != 0)) return;
                const auto Usage = FAngelscriptTypeUsage::FromTypeId(Generic->GetArgTypeId(5));
                auto Name = [&](int Index) { return *static_cast<FName*>(Generic->GetArgAddress(Index)); };
                auto* Type = Generic->GetEngine()->GetTypeInfoById(Generic->GetArgTypeId(4));
                const bool bTimer = Type && FString(ANSI_TO_TCHAR(Type->GetName())) == TEXT("FTimerDynamicDelegate");
                UObject* Target = SoloASBridge::PrepareLambdaSlot(static_cast<UObject*>(Generic->GetArgObject(0)),
                    Name(1), Name(2), Name(3), Generic->GetArgAddress(4), Generic->GetArgByte(6) != 0, bTimer,
                    int(Generic->GetArgDWord(7)), Cast<UScriptStruct>(Usage.GetUnrealStruct()), Generic->GetArgAddress(5),
                    *static_cast<FName*>(Generic->GetArgAddress(8)), *static_cast<FScriptLambdaHandle*>(Generic->GetArgAddress(9)), SparseSignature(Generic->GetArgTypeId(4)),
                    static_cast<UObject*>(Generic->GetArgObject(10)), *static_cast<FName*>(Generic->GetArgAddress(11)));
                if (!Target) FAngelscriptManager::Throw("Unable to prepare lambda capture slot.");
                Generic->SetReturnAddress(Target);
            });
        FAngelscriptBinds::BindGlobalGenericFunction(
            "UObject __CuvissPrepareDirectLambda(UObject Owner, const FName& Site, const ?&in Delegate, bool bMulticast, int Slots, FName&out Entry, FScriptLambdaHandle&out Handle, UObject Holder, const FName& RootProperty)",
            [](asIScriptGeneric* Generic)
            {
                SoloASExplicitCaptureLambdaPrivate::RefreshStorageMetadata();
                if (!ValidateLambdaDelegate(Generic->GetArgTypeId(2), Generic->GetArgByte(3) != 0)) return;
                UObject* Target = SoloASBridge::PrepareLambdaSlot(static_cast<UObject*>(Generic->GetArgObject(0)),
                    *static_cast<FName*>(Generic->GetArgAddress(1)), NAME_None, NAME_None, Generic->GetArgAddress(2), Generic->GetArgByte(3) != 0, false,
                    int(Generic->GetArgDWord(4)), nullptr, nullptr, *static_cast<FName*>(Generic->GetArgAddress(5)),
                    *static_cast<FScriptLambdaHandle*>(Generic->GetArgAddress(6)), SparseSignature(Generic->GetArgTypeId(2)),
                    static_cast<UObject*>(Generic->GetArgObject(7)), *static_cast<FName*>(Generic->GetArgAddress(8)));
                if (!Target) FAngelscriptManager::Throw("Unable to prepare lambda identity.");
                Generic->SetReturnAddress(Target);
            });
        FAngelscriptBinds::BindGlobalGenericFunction("void __CuvissTrackLambdaSlot(UObject Target, const FName& Entry, const ?&in Delegate)",
            [](asIScriptGeneric* Generic)
            {
                SoloASBridge::TrackLambdaSlot(static_cast<UObject*>(Generic->GetArgObject(0)),
                    *static_cast<FName*>(Generic->GetArgAddress(1)), Generic->GetArgAddress(2));
            });

        FAngelscriptBinds::BindGlobalFunction("void __CuvissRequireLambdaGameThread()", []
        {
            if (!IsInGameThread()) FAngelscriptManager::Throw("Lambda invocation requires the game thread.");
        });
        FAngelscriptBinds::BindGlobalGenericFunction("void __CuvissRemoveLambdaBindings(?&inout Delegate, UObject Owner)",
            [](asIScriptGeneric* Generic)
            {
                if (!IsInGameThread())
                {
                    FAngelscriptManager::Throw("Lambda unbinding requires the game thread.");
                    return;
                }
                const auto Usage = FAngelscriptTypeUsage::FromTypeId(Generic->GetArgTypeId(0));
                FAngelscriptType::FCppForm Form;
                if (!Usage.IsValid() || !Usage.Type->GetCppForm(Usage, Form)
                    || Form.TemplateObjectForm != TEXT("FMulticastScriptDelegate"))
                {
                    FAngelscriptManager::Throw("RemoveAll requires a multicast delegate.");
                    return;
                }
                SoloASBridge::RemoveLambdaClosureBindings(*static_cast<FMulticastScriptDelegate*>(Generic->GetArgAddress(0)),
                    static_cast<UObject*>(Generic->GetArgObject(1)));
            });
        FAngelscriptBinds::BindGlobalFunction("void __CuvissSweepLambdaClosures()", []
        {
            if (!IsInGameThread()) { FAngelscriptManager::Throw("Lambda release requires the game thread."); return; }
            SoloASBridge::SweepLambdaClosures();
        });

        FAngelscriptBinds::FNamespace SystemNamespace("System");
        FAngelscriptBinds::BindGlobalFunction(
            "FTimerHandle SetTimer(const FTimerDynamicDelegate& Delegate, float32 Time, bool bLooping = false, "
            "bool bMaxOncePerFrame = false, float32 InitialStartDelay = 0.f, float32 InitialStartDelayVariance = 0.f)",
            [](const FTimerDynamicDelegate& Delegate, float Time, bool bLooping, bool bMaxOncePerFrame,
                float InitialStartDelay, float InitialStartDelayVariance) -> FTimerHandle
            {
                FTimerHandle Handle;
                if (!IsInGameThread())
                {
                    FAngelscriptManager::Throw("Lambda timers require the game thread.");
                    return Handle;
                }
                if (!Delegate.IsBound()) return Handle;
                // The generated closure is transient; the script caller owns the world context.
                UWorld* World = GEngine->GetWorldFromContextObject(
                    FAngelscriptManager::CurrentWorldContext, EGetWorldErrorMode::ReturnNull);
                if (World == nullptr)
                {
                    FAngelscriptManager::Throw("SetTimer requires a world context.");
                    return Handle;
                }
                InitialStartDelay += FMath::RandRange(-InitialStartDelayVariance, InitialStartDelayVariance);
                FTimerManager& Timers = World->GetTimerManager();
                Handle = Timers.K2_FindDynamicTimerHandle(Delegate);
                FTimerManagerTimerParameters Parameters;
                Parameters.bLoop = bLooping;
                Parameters.bMaxOncePerFrame = bMaxOncePerFrame;
                Parameters.FirstDelay = Time + InitialStartDelay;
                Timers.SetTimer(Handle, Delegate, Time, Parameters);
                SoloASBridge::TrackLambdaTimer(Delegate, World, Handle);
                SoloASBridge::SweepLambdaClosures();
                return Handle;
            });
	});


#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExplicitCaptureLambdaPolicyTest, "SoloAS.Extensions.ExplicitCaptureLambdaDefaultPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FExplicitCaptureLambdaPolicyTest::RunTest(const FString&)
{
    FConfigCacheIni Config(EConfigCacheType::Temporary);
    const FString Filename = TEXT("ExplicitCaptureLambdaPolicy.ini");
    Config.Add(Filename, FConfigFile());
    TestTrue(TEXT("Ordinary projects enable typed captures by default"),
        SoloASExplicitCaptureLambdaPrivate::ReadEnabledPolicy(Config, Filename));
    Config.SetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableExplicitCaptureLambda"), false, Filename);
    TestFalse(TEXT("Project config can disable typed captures"),
        SoloASExplicitCaptureLambdaPrivate::ReadEnabledPolicy(Config, Filename));
    return !HasAnyErrors();
}
#endif

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_SoloASLambdaHandles((int32)FAngelscriptBinds::EOrder::Late + 1, []
{
    if (!SoloASExplicitCaptureLambdaPrivate::IsEnabled()) return;
    TMap<FString, bool> Types;
    Types.Add(TEXT("_FScriptDelegate"), false); Types.Add(TEXT("_FMulticastScriptDelegate"), true);
    for (UDelegateFunction* Signature : TObjectRange<UDelegateFunction>())
    {
        if (auto* Sparse = Cast<USparseDelegateFunction>(Signature))
        {
            auto Type = FAngelscriptType::GetByData(Signature);
            if (!Type.IsValid()) continue;
            auto Binds = FAngelscriptBinds::ExistingClass(Type->GetAngelscriptTypeName());
            Binds.GenericMethod("void Remove(const FScriptLambdaHandle& Handle)", [](asIScriptGeneric* Generic)
            { SoloASBridge::RemoveLambdaHandle(Generic->GetObject(), true, *static_cast<FScriptLambdaHandle*>(Generic->GetArgAddress(0)), static_cast<USparseDelegateFunction*>(Generic->GetFunction()->GetUserData())); }, Sparse);
            Binds.GenericMethod("void RemoveAll(UObject Owner)", [](asIScriptGeneric* Generic)
            { SoloASBridge::RemoveSparseLambdaBindings(Generic->GetObject(), static_cast<USparseDelegateFunction*>(Generic->GetFunction()->GetUserData()), static_cast<UObject*>(Generic->GetArgObject(0))); }, Sparse);
            continue;
        }
        if (auto Type = FAngelscriptType::GetByData(Signature); Type.IsValid())
            Types.Add(Type->GetAngelscriptTypeName(), Signature->HasAnyFunctionFlags(FUNC_MulticastDelegate));
    }
    for (const auto& Type : Types)
    {
        auto Binds = FAngelscriptBinds::ExistingClass(Type.Key);
        if (Type.Value)
        {
            Binds.Method("void Remove(const FScriptLambdaHandle& Handle)", [](FMulticastScriptDelegate& Delegate, const FScriptLambdaHandle& Handle)
            { SoloASBridge::RemoveLambdaHandle(&Delegate, true, Handle); });
            Binds.Method("void RemoveAll(UObject Owner)", [](FMulticastScriptDelegate& Delegate, UObject* Owner)
            { SoloASBridge::RemoveLambdaClosureBindings(Delegate, Owner); });
        }
        else
        {
            Binds.Method("void Remove(const FScriptLambdaHandle& Handle)", [](FScriptDelegate& Delegate, const FScriptLambdaHandle& Handle)
            { SoloASBridge::RemoveLambdaHandle(&Delegate, false, Handle); });
            Binds.Method("void Unbind(const FScriptLambdaHandle& Handle)", [](FScriptDelegate& Delegate, const FScriptLambdaHandle& Handle)
            { SoloASBridge::RemoveLambdaHandle(&Delegate, false, Handle); });
            Binds.Method("void Unbind()", [](FScriptDelegate& Delegate)
            { Delegate.Unbind(); SoloASBridge::SweepLambdaClosures(); });
        }
    }
});

namespace SoloASCoreExtensions::Algorithms
{
namespace
{
FDelegateHandle Handle;
bool Enabled()
{
    static const bool On = [] { bool Value = true; if (GConfig) GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableExplicitCaptureLambda"), Value, GEngineIni); return Value; }();
    return On;
}
void Rewrite(FAngelscriptPreprocessor& Preprocessor)
{
    using namespace SoloASExplicitCaptureLambdaPrivate;
    for (auto& File : Preprocessor.Files)
    {
        const FString Source = File.ProcessedCode;
        TArray<ELexicalKind> Kinds;
        BuildLexicalKinds(Source, Kinds);
        TArray<FAlgorithmGroup> Groups;
        TArray<FSourceReplacement> Replacements;
        TArray<TPair<FString, int32>> BodySources;
        FLambdaCandidate Candidate;
        int32 Search = 0;
        while (FindNextCandidate(Source, Kinds, Search, Candidate))
        {
            Search = Candidate.MethodPosition + 1;
            if (Candidate.Algorithm.IsEmpty()) continue;
            FSourceReplacement Replacement;
            FString Error;
            if (!ParseAlgorithmCandidate(Source, Kinds, Candidate, MakeFilenameHash(File.RelativeFilename), Groups, Replacement, Error))
            {
                int32 Line = 1;
                for (int32 Index = 0; Index < Candidate.MethodPosition; ++Index) if (Source[Index] == '\n') ++Line;
                Preprocessor.LineError(File, Line, Error);
                Preprocessor.bHasError = true;
                return;
            }
            Search = Replacement.EndPosition + 1;
            const int32 BodyLine = SourceLineAt(File, Replacement.BodyPosition);
            for (const FString& Name : Replacement.DebugBodyFunctions) BodySources.Emplace(Name, BodyLine);
            Replacements.Add(MoveTemp(Replacement));
        }
        if (Replacements.IsEmpty()) continue;
        FinishAlgorithmGroups(Groups, Replacements);
        FString Generated;
        TArray<FSourceReplacement> Methods;
        for (const auto& Replacement : Replacements)
        {
            Generated += Replacement.GeneratedCode;
            if (Replacement.OwnerClassClose != INDEX_NONE)
            {
                auto& Method = Methods.AddDefaulted_GetRef();
                Method.StartPosition = Replacement.OwnerClassClose;
                Method.EndPosition = Replacement.OwnerClassClose - 1;
                Method.Replacement = TEXT("\n") + Replacement.OwnerMethod + TEXT("\n");
            }
        }
        Replacements.Append(Methods);
        Replacements.StableSort([](const auto& Left, const auto& Right) { return Left.StartPosition < Right.StartPosition; });
        ApplyMappedReplacements(File, Replacements, Generated);
        for (const auto& Body : BodySources)
            if (!RegisterBodySource(File, Body.Key, Body.Value))
            { Preprocessor.FileWideError(File, TEXT("Lambda generated body source mapping failed.")); Preprocessor.bHasError = true; return; }
    }
}
}
bool IsEnabled() { return Enabled(); }
void Start() { if (Enabled() && !Handle.IsValid()) Handle = FAngelscriptPreprocessor::OnPostProcessCode.AddStatic(&Rewrite); }
void Stop() { FAngelscriptPreprocessor::OnPostProcessCode.Remove(Handle); Handle.Reset(); }
}

namespace SoloASExplicitCaptureLambdaPrivate
{
namespace
{
FDelegateHandle RewriteHandle, ClassReloadHandle, PostReloadHandle, PreExitHandle;
TArray<UClass*> ReplacedClasses;
uint64 RewriteCount = 0;

void InitFragment(FAngelscriptPreprocessor::FFile& Fragment,
    const FAngelscriptPreprocessor::FFile& File, const FString& Code)
{
    Fragment.Module = File.Module;
    Fragment.StaticsClass = File.StaticsClass;
    Fragment.RelativeFilename = File.RelativeFilename;
    Fragment.AbsoluteFilename = File.AbsoluteFilename;
    Fragment.RawCode = Code;
}

// Parse only CE output. Original chunks and their descriptor identities survive.
FString ProcessPlainCode(FAngelscriptPreprocessor& P,
    const FAngelscriptPreprocessor::FFile& File, const FString& Code)
{
    FAngelscriptPreprocessor::FFile Fragment;
    InitFragment(Fragment, File, Code);
    P.ParseIntoChunks(Fragment);
    P.CondenseFromChunks(Fragment);
    return Fragment.ProcessedCode;
}
void FinishReload(bool)
{
    // OnPostReload describes the actual swap, including a later native failure.
    // Aborted candidates never produce OnClassReload and never retire slots.
    InvalidateNativeCaptures();
    SoloASBridge::ReleaseLambdaSlotsForClasses(ReplacedClasses);
    ReplacedClasses.Reset();
    SoloASBridge::ResetLambdaScriptFields();
    ResetStorageMetadata();
    RefreshStorageMetadata();
}
}
uint64 GetRewriteCount() { return RewriteCount; }
void Rewrite(FAngelscriptPreprocessor& P)
{
    if (!IsEnabled()) return;
    ++RewriteCount;
    struct FPending
    {
        FFileRewritePlan Plan;
        TUniquePtr<FAngelscriptPreprocessor::FFile> Capture;
        TArray<TPair<FString, int32>> BodySources;
    };
    TArray<FPending> Pending;
    for (int32 Index = 0; Index < P.Files.Num(); ++Index)
    {
        auto& File = P.Files[Index];
        const FString& Source = File.ProcessedCode;
        TArray<ELexicalKind> Kinds;
        BuildLexicalKinds(Source, Kinds);
        FPending Item;
        Item.Plan.FileIndex = Index;
        int32 Search = 0, Ordinal = 0;
        FString Generated;
        FLambdaCandidate Candidate;
        while (FindNextCandidate(Source, Kinds, Search, Candidate))
        {
            Search = Candidate.MethodPosition + 1;
            if (!Candidate.Algorithm.IsEmpty()) continue;
            int32 Next = Search;
            if (ClassifyCandidate(Source, Kinds, Candidate, Next) == ELambdaCandidateClassification::Passthrough)
            { Search = Next; continue; }
            FSourceReplacement Replacement;
            FString Error;
            if (!ParseCandidate(Source, Kinds, Candidate, MakeFilenameHash(File.RelativeFilename), ++Ordinal, Replacement, Error))
            { P.FileWideError(File, Error); P.bHasError = true; return; }
            Search = Replacement.EndPosition + 1;
            const int32 BodyLine = SourceLineAt(File, Replacement.BodyPosition);
            for (const FString& Name : Replacement.DebugBodyFunctions) Item.BodySources.Emplace(Name, BodyLine);
            Generated += Replacement.GeneratedCode;
            Item.Plan.Replacements.Add(MoveTemp(Replacement));
        }
        if (Item.Plan.Replacements.IsEmpty()) continue;
        Item.Capture = MakeUnique<FAngelscriptPreprocessor::FFile>();
        InitFragment(*Item.Capture, File, Generated);
        P.ParseIntoChunks(*Item.Capture);
        P.DetectClasses(*Item.Capture);
        Pending.Add(MoveTemp(Item));
    }
    // Register every new type before resolving references between input files.
    for (auto& Item : Pending)
    {
        P.AnalyzeClasses(*Item.Capture);
        P.ProcessMacros(*Item.Capture);
        P.CondenseFromChunks(*Item.Capture);
        if (P.bHasError) return;
        auto& File = P.Files[Item.Plan.FileIndex];
        TSet<FString> Stores, Receivers;
        TArray<FSourceReplacement> Members;
        for (auto& Replacement : Item.Plan.Replacements)
        {
            const auto Owner = P.GetClassDescFor(Replacement.OwnerClassName);
            if (!Owner || !File.Module->Classes.Contains(Owner))
            { P.FileWideError(File, TEXT("Lambda owner descriptor is unavailable.")); P.bHasError = true; return; }
            FString MemberCode = Replacement.OwnerMethod;
            if (!Replacement.OwnerStoreName.IsEmpty() && !Stores.Contains(Replacement.OwnerStoreName))
            {
                MemberCode = TEXT(" UPROPERTY() FSoloASLambdaCaptureStore ") + Replacement.OwnerStoreName + TEXT("; ") + MemberCode;
                Stores.Add(Replacement.OwnerStoreName);
            }
            FAngelscriptPreprocessor::FFile Fragment;
            InitFragment(Fragment, File, TEXT("class ") + Owner->ClassName + TEXT(" {") + MemberCode + TEXT("}"));
            P.ParseIntoChunks(Fragment);
            FString ProcessedMembers;
            for (auto& Chunk : Fragment.ChunkedCode)
            {
                if (Chunk.Type != FAngelscriptPreprocessor::EChunkType::Class) continue;
                Chunk.ClassDesc = Owner;
                for (auto& Macro : Chunk.Macros)
                {
                    if (Macro.Type == FAngelscriptPreprocessor::EMacroType::Function) P.ProcessFunctionMacro(Fragment, Chunk, Macro);
                    else if (Macro.Type == FAngelscriptPreprocessor::EMacroType::Property) P.ProcessPropertyMacro(Fragment, Chunk, Macro);
                }
                P.ProcessReplacements(Fragment, Chunk);
                const int32 Open = Chunk.Content.Find(TEXT("{"));
                const int32 Close = Chunk.Content.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                ProcessedMembers += Chunk.Content.Mid(Open + 1, Close - Open - 1);
            }
            if (P.bHasError) return;
            FSourceReplacement Member;
            Member.StartPosition = Replacement.OwnerClassClose;
            Member.EndPosition = Replacement.OwnerClassClose - 1;
            Member.Replacement = TEXT("\n") + ProcessedMembers + TEXT("\n");
            Members.Add(MoveTemp(Member));
            for (const auto& Code : Fragment.GeneratedCode) Item.Capture->ProcessedCode += TEXT("\n") + Code;
            Receivers.Add(Replacement.Receiver);
            Replacement.Replacement = ProcessPlainCode(P, File, Replacement.Replacement);
        }
        Item.Plan.Replacements.Append(Members);
        Item.Plan.Replacements.StableSort([](const auto& A, const auto& B) { return A.StartPosition < B.StartPosition; });
        ApplyMappedReplacements(File, Item.Plan.Replacements, FString());
        RewriteLambdaReleaseStatements(File.ProcessedCode, Receivers);
        ApplyMappedReplacements(File, {}, Item.Capture->ProcessedCode);
        for (const auto& Body : Item.BodySources)
            if (!RegisterBodySource(File, Body.Key, Body.Value))
            { P.FileWideError(File, TEXT("Lambda generated body source mapping failed.")); P.bHasError = true; return; }
    }
}
void Start()
{
    if (!IsEnabled() || RewriteHandle.IsValid()) return;
    SoloASBridge::Private::InitializeLambdaClosures();
    RewriteHandle = FAngelscriptPreprocessor::OnPostProcessCode.AddStatic(&Rewrite);
    ClassReloadHandle = FAngelscriptClassGenerator::OnClassReload.AddLambda([](UClass* Old, UClass* New) { if (Old) ReplacedClasses.AddUnique(Old); if (New) ReplacedClasses.AddUnique(New); });
    PostReloadHandle = FAngelscriptClassGenerator::OnPostReload.AddStatic(&FinishReload);
    PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([] { SoloASBridge::Private::ShutdownLambdaClosures(); });
}
void Stop()
{
    FAngelscriptPreprocessor::OnPostProcessCode.Remove(RewriteHandle); RewriteHandle.Reset();
    FAngelscriptClassGenerator::OnClassReload.Remove(ClassReloadHandle); ClassReloadHandle.Reset();
    FAngelscriptClassGenerator::OnPostReload.Remove(PostReloadHandle); PostReloadHandle.Reset();
    FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle); PreExitHandle.Reset();
    ReplacedClasses.Reset();
    if (NativeInvalidationTicker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(NativeInvalidationTicker);
    NativeInvalidationTicker.Reset();
    bNativeInvalidationPending = false;
    SoloASBridge::Private::ShutdownLambdaClosures();
    ResetStorageMetadata();
}
}
