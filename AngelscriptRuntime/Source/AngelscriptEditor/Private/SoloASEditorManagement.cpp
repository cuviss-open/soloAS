// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "SoloASEditorManagement.h"
#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "AngelscriptRuntimeGcLifecycle.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Containers/Ticker.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopedSlowTask.h"
#include "OutputLogModule.h"
#include "PackageTools.h"
#include "ToolMenus.h"
#include "UObject/LinkerLoad.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Widgets/Notifications/SNotificationList.h"

#include "StartAngelscriptHeaders.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

namespace SoloASEditorManagement
{
namespace
{
const FName MenuOwner(TEXT("SoloAS.EditorManagement"));
struct FRebindingPackage
{
    TWeakObjectPtr<UPackage> Package;
    FString Filename;
};
TMap<FName, FRebindingPackage> RebindingPackages;
FTSTicker::FDelegateHandle TickHandle;
TArray<TWeakPtr<SNotificationItem>> Notifications;
bool bStartupScanDone = false;
bool bRebindingToastShown = false;
bool bActionActive = false;
FDelegateHandle EndLoadHandle;

bool IsProjectFile(const FString& Filename)
{
    return FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Filename),
        FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
}

void Notify(const FText& Message, FSimpleDelegate Fix = FSimpleDelegate())
{
    UE_LOG(Angelscript, Display, TEXT("SoloAS management: %s"), *Message.ToString());
    if (IsRunningCommandlet() || !FSlateApplication::IsInitialized()) return;
    FNotificationInfo Info(Message);
    Info.ExpireDuration = 12.0f;
    if (Fix.IsBound())
        Info.ButtonDetails.Add(FNotificationButtonInfo(
            NSLOCTEXT("SoloAS", "FixOnce", "Fix in one click"),
            NSLOCTEXT("SoloAS", "FixOnceHelp", "Run the corresponding SoloAS repair action."), Fix));
    Notifications.Add(FSlateNotificationManager::Get().AddNotification(Info));
}

void Report(const TCHAR* Action, const FActionResult& Result)
{
    Notify(FText::FromString(FString::Printf(TEXT("%s: %d succeeded, %d failed, %d skipped%s."),
        Action, Result.Succeeded, Result.Failed, Result.Skipped,
        Result.bCancelled ? TEXT(" (cancelled)") : TEXT(""))));
}

void ResaveAction() { ResaveEmptyEngineVersionAssets(); }
void RebindingAction() { FixLoadTimeRebindingAssets(); }

bool CanRun()
{
    return GEditor && !GEditor->PlayWorld && !bActionActive && !IsLoading() && !IsAsyncLoading()
        && !FAngelscriptManager::bIsHotReloading;
}

void OnEndLoadPackage(const FEndLoadPackageContext& Context)
{
    if (!IsInGameThread() || !HasAngelscriptRuntimeGcCompilationRetry()) return;
    for (UPackage* Package : Context.LoadedPackages)
    {
        if (!Package || Package->HasAnyFlags(RF_Transient) || Package->GetName().StartsWith(TEXT("/Script/"))) continue;
        FString Filename;
        if (FPackageName::DoesPackageExist(Package->GetName(), &Filename) && IsProjectFile(Filename))
            RebindingPackages.Add(Package->GetFName(), {Package, FPaths::ConvertRelativePathToFull(Filename)});
    }
}
}

TArray<FString> FindEmptyEngineVersionAssets()
{
    TArray<FString> Files;
    FPackageName::FindPackagesInDirectory(Files, FPaths::ProjectContentDir());
    for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
        if (Plugin->CanContainContent() && IsProjectFile(Plugin->GetContentDir()))
            FPackageName::FindPackagesInDirectory(Files, Plugin->GetContentDir());
    TArray<FString> Names;
    for (const FString& Filename : Files)
    {
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename));
        if (!Reader) continue;
        FPackageFileSummary Summary;
        *Reader << Summary;
        FString Name;
        if (!Reader->IsError() && Summary.Tag == PACKAGE_FILE_TAG
            && !(Summary.GetPackageFlags() & PKG_Cooked) && !Summary.SavedByEngineVersion.HasChangelist()
            && FPackageName::TryConvertFilenameToLongPackageName(Filename, Name)) Names.AddUnique(Name);
    }
    Names.Sort();
    return Names;
}

TArray<FName> GetLoadTimeRebindingPackages()
{
    if (IsInGameThread() && !IsLoading() && !IsAsyncLoading() && !IsGarbageCollecting()
        && !FAngelscriptManager::bIsHotReloading)
    {
        for (auto It = RebindingPackages.CreateIterator(); It; ++It)
        {
            // Keep live/unsaved packages and files whose mount point is temporarily unavailable.
            if (!It.Value().Package.IsValid() && !FindPackage(nullptr, *It.Key().ToString())
                && !IFileManager::Get().FileExists(*It.Value().Filename)
                && !FPackageName::DoesPackageExist(It.Key().ToString()))
                It.RemoveCurrent();
        }
    }
    TArray<FName> Names;
    RebindingPackages.GenerateKeyArray(Names);
    return Names;
}

FActionResult ResaveEmptyEngineVersionAssets()
{
    FActionResult Result;
    if (!CanRun()) { Result.Failed = 1; Report(TEXT("Resave empty engine version assets"), Result); return Result; }
    TGuardValue<bool> ActionGuard(bActionActive, true);
    const TArray<FString> Names = FindEmptyEngineVersionAssets();
    FScopedSlowTask Progress(Names.Num(), NSLOCTEXT("SoloAS", "ResaveProgress", "Resaving assets with empty engine version"));
    Progress.MakeDialog(true);
    for (const FString& Name : Names)
    {
        if (Progress.ShouldCancel()) { Result.bCancelled = true; break; }
        Progress.EnterProgressFrame(1, FText::FromString(Name));
        FString Filename;
        UPackage* Existing = FindPackage(nullptr, *Name);
        if ((Existing && Existing->IsDirty()) || !FPackageName::DoesPackageExist(Name, &Filename)
            || !IsProjectFile(Filename) || IFileManager::Get().IsReadOnly(*Filename))
        { ++Result.Skipped; continue; }
        UPackage* Package = LoadPackage(nullptr, *Name, LOAD_None);
        FlushAsyncLoading();
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        if (Package && UPackage::SavePackage(Package, nullptr, *Filename, Args))
        { Package->SetDirtyFlag(false); ++Result.Succeeded; }
        else ++Result.Failed;
    }
    Report(TEXT("Resave empty engine version assets"), Result);
    return Result;
}

FActionResult FixLoadTimeRebindingAssets()
{
    FActionResult Result;
    if (!CanRun()) { Result.Failed = 1; Report(TEXT("Reload affected assets"), Result); return Result; }
    TGuardValue<bool> ActionGuard(bActionActive, true);
    const TArray<FName> Names = GetLoadTimeRebindingPackages();
    FScopedSlowTask Progress(Names.Num(), NSLOCTEXT("SoloAS", "RebindProgress", "Reloading assets affected by load-time rebinding"));
    Progress.MakeDialog(true);
    for (FName Name : Names)
    {
        if (Progress.ShouldCancel()) { Result.bCancelled = true; break; }
        Progress.EnterProgressFrame(1, FText::FromName(Name));
        FString Filename;
        UPackage* Package = FindPackage(nullptr, *Name.ToString());
        if ((Package && Package->IsDirty()) || !FPackageName::DoesPackageExist(Name.ToString(), &Filename)
            || !IsProjectFile(Filename)) { ++Result.Skipped; continue; }
        FText Error;
        const bool bReloaded = Package
            ? UPackageTools::ReloadPackages({Package}, Error, EReloadPackagesInteractionMode::AssumeNegative)
            : LoadPackage(nullptr, *Name.ToString(), LOAD_None) != nullptr;
        FlushAsyncLoading();
        if (bReloaded) { RebindingPackages.Remove(Name); ++Result.Succeeded; }
        else { ++Result.Failed; UE_LOG(Angelscript, Warning, TEXT("SoloAS reload %s failed: %s"), *Name.ToString(), *Error.ToString()); }
    }
    Report(TEXT("Reload affected assets"), Result);
    return Result;
}

void FullReloadScripts()
{
    if (!CanRun() || !FAngelscriptManager::IsInitialized()) return;
    auto& Manager = FAngelscriptManager::Get();
    for (const auto& Module : Manager.GetActiveModules())
    {
        // Use the existing pending-full-reload contract even without a layout edit.
        for (const auto& Class : Module->Classes) Class->bPendingFullReload = true;
        for (const auto& Enum : Module->Enums) Enum->bPendingFullReload = true;
        for (const auto& Section : Module->Code)
            Manager.FileChangesDetectedForReload.AddUnique({Section.AbsoluteFilename, Section.RelativeFilename});
    }
    Manager.LastFileChangeDetectedTime = -1.0;
    Manager.CheckForHotReload(ECompileType::FullReload);
}

void OpenLog()
{
    auto& Log = FModuleManager::LoadModuleChecked<FOutputLogModule>(TEXT("OutputLog"));
    Log.OpenOutputLog();
    Log.UpdateOutputLogFilter({TEXT("Angelscript"), TEXT("LogLinker"), TEXT("LogSoloASEditorImports")}, true, true, true);
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

bool IsStaticTypeCompatible(FAngelscriptManager& Manager, const FString& TypeName, FProperty* Property)
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
        const UClass* ActualClass = Actual.GetClass();
        return ActualClass == nullptr || ActualClass->IsChildOf(ObjectProperty->PropertyClass);
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
                            if (bHasReturnOutput && !ReturnTypeName.IsEmpty() && !IsStaticTypeCompatible(Manager, ReturnTypeName, ReturnProperty))
                            {
                                Mismatch = FString::Printf(TEXT("function '%s' return type does not match '%s'"), *MemberName.ToString(), *ReturnTypeName);
                                bTypesMatch = false;
                            }
                            for (int32 Index = 0; bTypesMatch && Index < Parameters.Num(); ++Index)
                            {
                                const FString ArgumentTypeName = GetExpressionType(Arguments[Index + ParameterArgumentStart], LocalTypes);
                                if (!ArgumentTypeName.IsEmpty() && !IsStaticTypeCompatible(Manager, ArgumentTypeName, Parameters[Index]))
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
                            if (!ValueTypeName.IsEmpty() && !IsStaticTypeCompatible(Manager, ValueTypeName, Property))
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

void ScanBlueprintQuickCallsAction()
{
    ScanBlueprintQuickCalls();
}

void RegisterMenu()
{
    FToolMenuOwnerScoped Owner(MenuOwner);
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu"));
    if (!Menu) return;
    FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("SoloAS"), FText::GetEmpty(),
        FToolMenuInsert(TEXT("Tools"), EToolMenuInsertType::After));
    Section.AddSubMenu(TEXT("SoloAS"),
        NSLOCTEXT("SoloAS", "Menu", "SoloAS"), NSLOCTEXT("SoloAS", "MenuHelp", "Manage SoloAS scripts and repair project assets."),
        FNewToolMenuDelegate::CreateLambda([](UToolMenu* Submenu)
        {
            auto& Section = Submenu->FindOrAddSection(TEXT("Management"));
            auto Add = [&Section](const FName Name, const FText& Label, FExecuteAction Action)
            {
                Section.AddMenuEntry(Name, Label, FText(), FSlateIcon(),
                    FUIAction(Action, FCanExecuteAction::CreateStatic(&CanRun)));
            };
            Add(
                TEXT("ResaveEmptyVersion"),
                NSLOCTEXT("SoloAS", "Resave", "Resave assets with empty engine version"),
                FExecuteAction::CreateStatic(&ResaveAction));
            Add(
                TEXT("FixLoadRebinding"),
                NSLOCTEXT("SoloAS", "FixRebind", "Fix assets affected by load-time rebinding"),
                FExecuteAction::CreateStatic(&RebindingAction));
            if (FModuleManager::Get().IsModuleLoaded(TEXT("SoloASCoreExtensions")))
            {
                Add(
                    TEXT("ScanBlueprintQuickCalls"),
                    NSLOCTEXT("SoloAS", "ScanBlueprintQuickCalls", "Scan Blueprint quick calls"),
                    FExecuteAction::CreateStatic(&ScanBlueprintQuickCallsAction));
            }
            Add(TEXT("FullReloadScripts"), NSLOCTEXT("SoloAS", "FullReload", "Full reload scripts"), FExecuteAction::CreateStatic(&FullReloadScripts));
            Section.AddMenuEntry(
                TEXT("OpenSoloASLog"), NSLOCTEXT("SoloAS", "OpenLog", "Open SoloAS log"),
                FText(), FSlateIcon(), FExecuteAction::CreateStatic(&OpenLog));
        }));
}

void Initialize()
{
    EndLoadHandle = FCoreUObjectDelegates::OnEndLoadPackage.AddStatic(&OnEndLoadPackage);
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
    {
        if (!CanRun() || !FAngelscriptManager::bIsInitialCompileFinished) return true;
        if (!bStartupScanDone)
        {
            bStartupScanDone = true;
            const int32 Count = FindEmptyEngineVersionAssets().Num();
            if (Count)
                Notify(
                    FText::FromString(FString::Printf(TEXT("SoloAS: %d assets have an empty engine version."), Count)),
                    FSimpleDelegate::CreateStatic(&ResaveAction));
        }
        if (!bRebindingToastShown && !GetLoadTimeRebindingPackages().IsEmpty())
        {
            bRebindingToastShown = true;
            Notify(
                NSLOCTEXT("SoloAS", "RebindNotice", "SoloAS deferred a script reload while assets were loading. Reload the affected assets."),
                FSimpleDelegate::CreateStatic(&RebindingAction));
        }
        return true;
    }));
}

void Shutdown()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    FCoreUObjectDelegates::OnEndLoadPackage.Remove(EndLoadHandle);
    EndLoadHandle.Reset();
    for (auto& Weak : Notifications)
        if (auto Item = Weak.Pin()) Item->ExpireAndFadeout();
    Notifications.Reset();
    UToolMenus::UnregisterOwner(MenuOwner);
    RebindingPackages.Reset();
}
}
