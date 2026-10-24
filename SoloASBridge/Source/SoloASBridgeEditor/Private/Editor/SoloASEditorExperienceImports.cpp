#include "Editor/SoloASEditorExperienceImports.h"

#include "CoreMinimal.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)

#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AssertionMacros.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "ObjectEditorUtils.h"
#include "Editor/SoloASBridgeEditorInlineHook.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "UObject/Class.h"
#include "UObject/LinkerLoad.h"
#include "UObject/UnrealType.h"

#include <cstdarg>
#include <cstring>
#include <stdint.h>
#if PLATFORM_MAC
#include <atomic>
#endif

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include "Windows/HideWindowsPlatformTypes.h"
#else
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSoloASEditorExperienceImports, Log, All);

namespace SoloASBridge::Editor
{
namespace
{
static TAutoConsoleVariable<int32> CVarEditorExperienceImports(
    TEXT("soloas.EditorExperienceImports"),
    1,
    TEXT("Redirect registered Editor imports for SoloAS Blueprint and Details compatibility."));
static TAutoConsoleVariable<int32> CVarEditorLinkerDetachSafety(
    TEXT("soloas.EditorLinkerDetachSafety"),
    1,
    TEXT("Defer imported Editor linker detaches until pending exports are serialized."));

constexpr ANSICHAR HideEventMetadata[] = "HideEventInBP";
constexpr ANSICHAR DetailLayoutFile[] = "DetailLayoutHelpers.cpp";
constexpr ANSICHAR DuplicatePropertyExpression[] =
    "!PropertyNodeMap.Contains(Property->GetFName())";

using FIsNativeFunction = bool (*)(const FField*);
using FIsFunctionHiddenFunction = bool (*)(const UFunction*, const UClass*);
using FCheckVerifyFailedVFunction = bool (*)(
    const ANSICHAR*, const ANSICHAR*, int32, const TCHAR*, va_list);
using FTryFindTypeSlowFunction = UField* (*)(
    UClass*, FStringView, EFindFirstObjectOptions);
using FSetLinkerFunction = void (*)(
    UObject*, FLinkerLoad*, int32, bool, bool);

FCheckVerifyFailedVFunction OriginalCheckVerifyFailedV = nullptr;
FSetLinkerFunction OriginalSetLinker = nullptr;
FSetLinkerFunction EngineSetLinker = nullptr;
FEditorInlineHookHandle SetLinkerEntryHook;
bool bSetLinkerEntryHookAttempted = false;
uintptr_t CoreUObjectImageStart = 0;
uintptr_t CoreUObjectImageEnd = 0;
#if PLATFORM_WINDOWS
HMODULE CoreUObjectModule = nullptr;
#endif
#if PLATFORM_MAC
constexpr int32 MaxMacTargetCallerModules = 2;

struct FMacTargetCallerRange
{
    uintptr_t Start = 0;
    uintptr_t End = 0;
};

struct FMacTargetCallerRangeCache
{
    FMacTargetCallerRange Ranges[MaxMacTargetCallerModules]{};
    uintptr_t Partition = 0;
    int32 RangeCount = 0;
    int32 ModuleCount = 0;
};

FEditorInlineHookHandle FieldIsNativeEntryHook;
FIsNativeFunction OriginalFieldIsNative = nullptr;
bool bMacFieldIsNativeEntryAttempted = false;
int32 LastLoggedMacFieldIsNativeSlots = INDEX_NONE;
int32 LastLoggedMacFunctionHiddenSlots = INDEX_NONE;
int32 LastLoggedMacCheckVerifyFailedSlots = INDEX_NONE;
int32 LastLoggedMacTryFindTypeSlowSlots = INDEX_NONE;
int32 LastLoggedMacTargetModuleCount = INDEX_NONE;
bool bLastLoggedMacEntryHook = false;
std::atomic<const FMacTargetCallerRangeCache*> ActiveMacTargetCallerRangeCache{nullptr};
TArray<TUniquePtr<FMacTargetCallerRangeCache>> RetainedMacTargetCallerRangeCaches;
int32 LastLoggedMacLinkerDetachSlots = INDEX_NONE;
bool bLastLoggedMacSetLinkerEntry = false;
#endif

void WarnOnce(const TCHAR* Reason);

#if PLATFORM_MAC
bool IsMacTargetFieldIsNativeCaller(const void* CallerAddress);
#endif

bool CallEngineIsNative(const FField* Field)
{
#if PLATFORM_MAC
    if (OriginalFieldIsNative != nullptr)
    {
        return OriginalFieldIsNative(Field);
    }
#endif
    return Field->IsNative();
}

bool CallEngineIsFunctionHiddenFromClass(
    const UFunction* Function,
    const UClass* Class)
{
    return FObjectEditorUtils::IsFunctionHiddenFromClass(Function, Class);
}

bool IsDuplicateDetailLayoutCheck(
    const ANSICHAR* Expression,
    const ANSICHAR* File)
{
    if (Expression == nullptr || File == nullptr
        || FCStringAnsi::Strcmp(Expression, DuplicatePropertyExpression) != 0)
    {
        return false;
    }

    const ANSICHAR* BaseName = File;
    for (const ANSICHAR* Cursor = File; *Cursor != '\0'; ++Cursor)
    {
        if (*Cursor == '/' || *Cursor == '\\')
        {
            BaseName = Cursor + 1;
        }
    }
    return FCStringAnsi::Strcmp(BaseName, DetailLayoutFile) == 0;
}

UField* CallEngineTryFindTypeSlow(
    UClass* TypeClass,
    FStringView Name,
    EFindFirstObjectOptions Options)
{
    return UClass::TryFindTypeSlow(TypeClass, Name, Options);
}

bool HookFieldIsNative(const FField* Field)
{
#if PLATFORM_MAC
    // The entry detour is process-wide; preserve the import-hook caller scope.
    if (!IsMacTargetFieldIsNativeCaller(__builtin_return_address(0)))
    {
        return Field != nullptr ? CallEngineIsNative(Field) : false;
    }
#endif
    if (Field == nullptr)
    {
        return false;
    }
#if PLATFORM_MAC
    const bool bEngineNative = CallEngineIsNative(Field);
    if (bEngineNative || CVarEditorExperienceImports.GetValueOnAnyThread() == 0)
    {
        return bEngineNative;
    }
#else
    if (CVarEditorExperienceImports.GetValueOnAnyThread() != 0)
#endif
    {
        // My Blueprint chooses between GetDisplayNameText and the raw FName
        // through IsNative; the Mac entry hook covers direct non-IAT calls.
        if (const FProperty* Property = CastField<FProperty>(Field))
        {
            const UClass* OwnerClass = Cast<UClass>(Property->GetOwnerStruct());
            if (OwnerClass != nullptr
                && SoloASBridge::HasRegisteredScriptClassInAncestry(OwnerClass))
            {
                return true;
            }
        }
    }
#if PLATFORM_MAC
    return bEngineNative;
#else
    return CallEngineIsNative(Field);
#endif
}

bool HookIsFunctionHiddenFromClass(
    const UFunction* Function,
    const UClass* Class)
{
    if (CVarEditorExperienceImports.GetValueOnAnyThread() != 0
        && Function != nullptr
        && Function->HasMetaData(FName(HideEventMetadata)))
    {
        return true;
    }
    return CallEngineIsFunctionHiddenFromClass(Function, Class);
}

bool HookCheckVerifyFailedImpl2(
    const ANSICHAR* Expression,
    const ANSICHAR* File,
    int32 Line,
    const TCHAR* Format,
    ...)
{
    (void)Line;
    if (CVarEditorExperienceImports.GetValueOnAnyThread() != 0
        && IsDuplicateDetailLayoutCheck(Expression, File))
    {
        // checkSlow uses the return value to decide whether to break.
        return false;
    }

    if (OriginalCheckVerifyFailedV == nullptr)
    {
        WarnOnce(TEXT("the original variadic assertion forwarder is unavailable"));
        return true;
    }

    va_list Arguments;
    va_start(Arguments, Format);
    const bool bShouldBreak = OriginalCheckVerifyFailedV(
        Expression, File, Line, Format, Arguments);
    va_end(Arguments);
    return bShouldBreak;
}

UField* HookTryFindTypeSlow(
    UClass* TypeClass,
    FStringView Name,
    EFindFirstObjectOptions Options)
{
    if (CVarEditorExperienceImports.GetValueOnAnyThread() != 0
        && TypeClass == UEnum::StaticClass()
        && EnumHasAnyFlags(Options, EFindFirstObjectOptions::ExactClass))
    {
        Options = EFindFirstObjectOptions::None;
    }
    return CallEngineTryFindTypeSlow(TypeClass, Name, Options);
}

bool PreloadBeforeLinkerDetach(UObject* Object)
{
    if (FLinkerLoad* ExistingLinker = Object->GetLinker())
    {
        ExistingLinker->Preload(Object);
    }
    if (Object->HasAnyFlags(RF_NeedLoad))
    {
        UE_LOG(LogSoloASEditorExperienceImports, Warning,
            TEXT("ASR-LINKER-DETACH-DEFERRED: Kept %s attached because preload did not finish."),
            *Object->GetPathName());
        return false;
    }
    return true;
}

bool ShouldPreloadForLinkerDetach(
    UObject* Object, FLinkerLoad* LinkerLoad, bool bShouldDetachExisting)
{
    return Object != nullptr && LinkerLoad == nullptr && bShouldDetachExisting
        && CVarEditorLinkerDetachSafety.GetValueOnAnyThread() != 0
        && Object->HasAnyFlags(RF_NeedLoad);
}

void CallOriginalSetLinker(
    UObject* Object, FLinkerLoad* LinkerLoad, int32 LinkerIndex,
    bool bShouldDetachExisting, bool bSilenceLogs)
{
    FSetLinkerFunction Original = OriginalSetLinker != nullptr
        ? OriginalSetLinker : EngineSetLinker;
    if (Original != nullptr)
    {
        Original(Object, LinkerLoad, LinkerIndex,
            bShouldDetachExisting, bSilenceLogs);
    }
    else
    {
        WarnOnce(TEXT("the original UObject::SetLinker entry was not resolved"));
    }
}

void HookSetLinkerImport(
    UObject* Object, FLinkerLoad* LinkerLoad, int32 LinkerIndex,
    bool bShouldDetachExisting, bool bSilenceLogs)
{
    if (ShouldPreloadForLinkerDetach(Object, LinkerLoad, bShouldDetachExisting))
    {
        UE_LOG(LogSoloASEditorExperienceImports, Warning,
            TEXT("ASR-LINKER-DETACH-DEFERRED: Kept %s attached until its pending serialization completes."),
            *Object->GetPathName());
        return;
    }
    CallOriginalSetLinker(Object, LinkerLoad, LinkerIndex,
        bShouldDetachExisting, bSilenceLogs);
}

bool IsCoreUObjectCallSite(const void* Address);

void HookSetLinkerEntry(
    UObject* Object, FLinkerLoad* LinkerLoad, int32 LinkerIndex,
    bool bShouldDetachExisting, bool bSilenceLogs)
{
    if (ShouldPreloadForLinkerDetach(Object, LinkerLoad, bShouldDetachExisting))
    {
#if PLATFORM_WINDOWS
        const void* CallerAddress = _ReturnAddress();
#else
        const void* CallerAddress = __builtin_return_address(0);
#endif
        if (IsCoreUObjectCallSite(CallerAddress)
            && !PreloadBeforeLinkerDetach(Object))
        {
            return;
        }
    }
    CallOriginalSetLinker(Object, LinkerLoad, LinkerIndex,
        bShouldDetachExisting, bSilenceLogs);
}

enum class EExperienceImport : uint8
{
    FieldIsNative,
    FunctionHidden,
    CheckVerifyFailed,
    TryFindTypeSlow,
    LinkerDetach,
};

struct FImportSpec
{
    EExperienceImport Id;
    const ANSICHAR* Importer;
    const ANSICHAR* Provider;
    const ANSICHAR* WindowsSymbol;
    const ANSICHAR* MachSymbol;
    void* Replacement;
};

const FImportSpec ImportSpecs[] =
{
    {EExperienceImport::FieldIsNative,
        "UnrealEditor-Kismet", "UnrealEditor-CoreUObject.dll",
        "?IsNative@FField@@QEBA_NXZ", "__ZNK6FField8IsNativeEv",
        reinterpret_cast<void*>(&HookFieldIsNative)},
    {EExperienceImport::FieldIsNative,
        "UnrealEditor-BlueprintGraph", "UnrealEditor-CoreUObject.dll",
        "?IsNative@FField@@QEBA_NXZ", "__ZNK6FField8IsNativeEv",
        reinterpret_cast<void*>(&HookFieldIsNative)},
    {EExperienceImport::FunctionHidden,
        "UnrealEditor-Kismet", "UnrealEditor-UnrealEd.dll",
        "?IsFunctionHiddenFromClass@FObjectEditorUtils@@YA_NPEBVUFunction@@PEBVUClass@@@Z",
        "__ZN18FObjectEditorUtils25IsFunctionHiddenFromClassEPK9UFunctionPK6UClass",
        reinterpret_cast<void*>(&HookIsFunctionHiddenFromClass)},
    {EExperienceImport::CheckVerifyFailed,
        "UnrealEditor-PropertyEditor", "UnrealEditor-Core.dll",
        "?CheckVerifyFailedImpl2@FDebug@@SA_NPEBD0HPEB_WZZ",
        "__ZN6FDebug22CheckVerifyFailedImpl2EPKcS1_iPKDsz",
        reinterpret_cast<void*>(&HookCheckVerifyFailedImpl2)},
    {EExperienceImport::TryFindTypeSlow,
        "UnrealEditor-PropertyEditor", "UnrealEditor-CoreUObject.dll",
        "?TryFindTypeSlow@UClass@@SAPEAVUField@@PEAV1@V?$TStringView@_W@@W4EFindFirstObjectOptions@@@Z",
        "__ZN6UClass15TryFindTypeSlowEPS_11TStringViewIDsE23EFindFirstObjectOptions",
        reinterpret_cast<void*>(&HookTryFindTypeSlow)},
    {EExperienceImport::LinkerDetach,
        "*", "UnrealEditor-CoreUObject.dll",
        "?SetLinker@UObject@@QEAAXPEAVFLinkerLoad@@H_N1@Z",
        "__ZN7UObject9SetLinkerEP11FLinkerLoadibb",
        reinterpret_cast<void*>(&HookSetLinkerImport)},
};

void* ResolveOriginal(const FImportSpec& Spec);

bool IsImportEnabled(EExperienceImport Id)
{
    return Id == EExperienceImport::LinkerDetach
        ? CVarEditorLinkerDetachSafety.GetValueOnAnyThread() != 0
        : CVarEditorExperienceImports.GetValueOnAnyThread() != 0;
}

#if PLATFORM_WINDOWS
constexpr ANSICHAR CheckVerifyFailedVSymbol[] =
    "?CheckVerifyFailedImpl2V@FDebug@@CA_NPEBD0HPEB_WPEAD@Z";
#else
// Unix TCHAR is char16_t (Itanium "Ds"); Darwin arm64 va_list is char*,
// while Darwin x86_64 uses the x86_64 ABI's __va_list_tag array.
constexpr const ANSICHAR* CheckVerifyFailedVSymbols[] =
{
    "__ZN6FDebug23CheckVerifyFailedImpl2VEPKcS1_iPKDsPc",
    "__ZN6FDebug23CheckVerifyFailedImpl2VEPKcS1_iPKDsP13__va_list_tag",
};
#endif

struct FInstalledImport
{
    EExperienceImport Id = EExperienceImport::FieldIsNative;
    void* ImageBase = nullptr;
    FString ImageName;
    void** Slot = nullptr;
    void* Original = nullptr;
    void* Replacement = nullptr;
};

FCriticalSection ImportLock;
TMap<void**, FInstalledImport> InstalledImports;
FDelegateHandle ModulesChangedHandle;
FDelegateHandle ConsoleChangedHandle;
FDelegateHandle LinkerDetachConsoleChangedHandle;
IConsoleVariable* ExperienceVariable = nullptr;
IConsoleVariable* LinkerDetachVariable = nullptr;
bool bImportHooksInitialized = false;
bool bWarned = false;

void WarnOnce(const TCHAR* Reason)
{
    if (bWarned)
    {
        return;
    }
    bWarned = true;
    UE_LOG(LogSoloASEditorExperienceImports, Warning,
        TEXT("Editor experience import redirect incomplete: %s. Stock behavior remains for uncovered imports."),
        Reason);
}

FString NormalizeImageName(const TCHAR* ImageName)
{
    FString Result = FPaths::GetCleanFilename(ImageName == nullptr ? TEXT("") : ImageName);
    Result.RemoveFromEnd(TEXT(".dll"), ESearchCase::IgnoreCase);
    Result.RemoveFromEnd(TEXT(".dylib"), ESearchCase::IgnoreCase);
#if PLATFORM_MAC
    // Mac editor modules ship as libUnrealEditor-<Module>.dylib.
    Result.RemoveFromStart(TEXT("lib"), ESearchCase::CaseSensitive);
#endif
    return Result;
}

bool IsCoreUObjectCallSite(const void* Address)
{
    const uintptr_t CallerAddress = reinterpret_cast<uintptr_t>(Address);
    return CallerAddress >= CoreUObjectImageStart
        && CallerAddress < CoreUObjectImageEnd;
}

bool InstallLinkerDetachEntryHook()
{
    if (SetLinkerEntryHook.Original != nullptr)
    {
        return true;
    }
    if (bSetLinkerEntryHookAttempted)
    {
        return false;
    }
    bSetLinkerEntryHookAttempted = true;

    const FImportSpec* Spec = nullptr;
    for (const FImportSpec& Candidate : ImportSpecs)
    {
        if (Candidate.Id == EExperienceImport::LinkerDetach)
        {
            Spec = &Candidate;
            break;
        }
    }
    void* Target = Spec != nullptr ? ResolveOriginal(*Spec) : nullptr;
    if (Target == nullptr)
    {
        WarnOnce(TEXT("the CoreUObject UObject::SetLinker export was not found for same-module detaches"));
        return false;
    }

#if PLATFORM_WINDOWS
    CoreUObjectModule = GetModuleHandleW(TEXT("UnrealEditor-CoreUObject.dll"));
    if (CoreUObjectModule == nullptr)
    {
        WarnOnce(TEXT("the CoreUObject image was not loaded for same-module detaches"));
        return false;
    }
    const auto* DosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(CoreUObjectModule);
    if (DosHeader->e_magic != IMAGE_DOS_SIGNATURE || DosHeader->e_lfanew <= 0)
    {
        WarnOnce(TEXT("the CoreUObject PE image header could not be read"));
        return false;
    }
    const auto* NtHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8*>(CoreUObjectModule) + DosHeader->e_lfanew);
    if (NtHeaders->Signature != IMAGE_NT_SIGNATURE
        || NtHeaders->OptionalHeader.SizeOfImage == 0)
    {
        WarnOnce(TEXT("the CoreUObject PE image range could not be read"));
        return false;
    }
    CoreUObjectImageStart = reinterpret_cast<uintptr_t>(CoreUObjectModule);
    CoreUObjectImageEnd = CoreUObjectImageStart + NtHeaders->OptionalHeader.SizeOfImage;
#else
    Dl_info Info{};
    if (dladdr(Target, &Info) == 0 || Info.dli_fname == nullptr
        || NormalizeImageName(UTF8_TO_TCHAR(Info.dli_fname))
            != TEXT("UnrealEditor-CoreUObject"))
    {
        WarnOnce(TEXT("UObject::SetLinker did not resolve to the CoreUObject image"));
        return false;
    }
    const auto* Header = static_cast<const mach_header_64*>(Info.dli_fbase);
    if (Header == nullptr || Header->magic != MH_MAGIC_64)
    {
        WarnOnce(TEXT("the CoreUObject Mach-O header could not be read"));
        return false;
    }
    const uint8* CommandBytes = reinterpret_cast<const uint8*>(Header) + sizeof(mach_header_64);
    const segment_command_64* TextSegment = nullptr;
    for (uint32 Index = 0; Index < Header->ncmds; ++Index)
    {
        const auto* Command = reinterpret_cast<const load_command*>(CommandBytes);
        if (Command->cmd == LC_SEGMENT_64)
        {
            const auto* Segment = reinterpret_cast<const segment_command_64*>(Command);
            if (FCStringAnsi::Strcmp(Segment->segname, "__TEXT") == 0)
            {
                TextSegment = Segment;
                break;
            }
        }
        CommandBytes += Command->cmdsize;
    }
    if (TextSegment == nullptr)
    {
        WarnOnce(TEXT("the CoreUObject executable image range could not be read"));
        return false;
    }
    const intptr_t Slide = reinterpret_cast<intptr_t>(Header)
        - static_cast<intptr_t>(TextSegment->vmaddr);
    CoreUObjectImageStart = static_cast<uintptr_t>(
        static_cast<intptr_t>(TextSegment->vmaddr) + Slide);
    CoreUObjectImageEnd = CoreUObjectImageStart + TextSegment->vmsize;
#endif

    EngineSetLinker = reinterpret_cast<FSetLinkerFunction>(Target);
    OriginalSetLinker = EngineSetLinker;
    if (!InstallEditorInlineHook(SetLinkerEntryHook, Target,
        reinterpret_cast<void*>(&HookSetLinkerEntry), TEXT("UObject::SetLinker")))
    {
        WarnOnce(TEXT("the CoreUObject SetLinker entry hook was unavailable; import rebinding remains active"));
        return false;
    }
    OriginalSetLinker = reinterpret_cast<FSetLinkerFunction>(SetLinkerEntryHook.Original);
    UE_LOG(LogSoloASEditorExperienceImports, Display,
        TEXT("UObject::SetLinker entry hook installed with CoreUObject caller scope."));
    return true;
}

bool RemoveLinkerDetachEntryHook()
{
    if (SetLinkerEntryHook.Original == nullptr)
    {
        OriginalSetLinker = EngineSetLinker;
        return true;
    }
    if (!RemoveEditorInlineHook(SetLinkerEntryHook,
        reinterpret_cast<void*>(&HookSetLinkerEntry), TEXT("UObject::SetLinker")))
    {
        WarnOnce(TEXT("the CoreUObject SetLinker entry hook could not be restored"));
        return false;
    }
    OriginalSetLinker = EngineSetLinker;
    return true;
}

bool IsTargetImage(const FString& ImageName)
{
    const FString Normalized = NormalizeImageName(*ImageName);
    const bool bSoloASProduct = Normalized.StartsWith(TEXT("UnrealEditor-Angelscript"))
        || Normalized.StartsWith(TEXT("UnrealEditor-SoloAS"));
    return Normalized == TEXT("UnrealEditor-BlueprintGraph")
        || Normalized == TEXT("UnrealEditor-Kismet")
        || Normalized == TEXT("UnrealEditor-PropertyEditor")
        || (CVarEditorLinkerDetachSafety.GetValueOnAnyThread() != 0
            && Normalized.StartsWith(TEXT("UnrealEditor-"))
            && Normalized != TEXT("UnrealEditor-CoreUObject")
            && !bSoloASProduct);
}

const FImportSpec* FindImportSpec(
    const FString& Importer,
    const ANSICHAR* Provider,
    const ANSICHAR* Symbol,
    bool bMach)
{
    for (const FImportSpec& Spec : ImportSpecs)
    {
        const bool bImporterMatches = FCStringAnsi::Strcmp(Spec.Importer, "*") == 0
            || Importer == UTF8_TO_TCHAR(Spec.Importer);
        if (bImporterMatches
            && FCStringAnsi::Strcmp(
                Provider,
                bMach ? Provider : Spec.Provider) == 0
            && FCStringAnsi::Strcmp(
                Symbol,
                bMach ? Spec.MachSymbol : Spec.WindowsSymbol) == 0
            && IsImportEnabled(Spec.Id))
        {
            return &Spec;
        }
    }
    return nullptr;
}

void* ResolveOriginal(const FImportSpec& Spec)
{
#if PLATFORM_WINDOWS
    HMODULE Provider = GetModuleHandleA(Spec.Provider);
    if (Provider == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(GetProcAddress(Provider, Spec.WindowsSymbol));
#else
    const ANSICHAR* DlsymName = Spec.MachSymbol + 1;
    return dlsym(RTLD_DEFAULT, DlsymName);
#endif
}

void* ResolveOriginalCheckVerifyFailedV()
{
#if PLATFORM_WINDOWS
    HMODULE Provider = GetModuleHandleA("UnrealEditor-Core.dll");
    return Provider != nullptr
        ? reinterpret_cast<void*>(GetProcAddress(Provider, CheckVerifyFailedVSymbol))
        : nullptr;
#else
    for (const ANSICHAR* Symbol : CheckVerifyFailedVSymbols)
    {
        if (void* Address = dlsym(RTLD_DEFAULT, Symbol + 1))
        {
            return Address;
        }
    }
    return nullptr;
#endif
}

#if PLATFORM_MAC
bool IsFieldIsNativeImporter(const FString& ImageName)
{
    for (const FImportSpec& Spec : ImportSpecs)
    {
        if (Spec.Id == EExperienceImport::FieldIsNative
            && ImageName == UTF8_TO_TCHAR(Spec.Importer))
        {
            return true;
        }
    }
    return false;
}

bool IsFieldIsNativeImporter(FName ModuleName)
{
    for (const FImportSpec& Spec : ImportSpecs)
    {
        if (Spec.Id == EExperienceImport::FieldIsNative
            && ModuleName == UTF8_TO_TCHAR(Spec.Importer + sizeof("UnrealEditor-") - 1))
        {
            return true;
        }
    }
    return false;
}

void CacheMacTargetCallerRanges()
{
    FMacTargetCallerRange ModuleRanges[MaxMacTargetCallerModules]{};
    FString ModuleNames[MaxMacTargetCallerModules];
    int32 ModuleCount = 0;
    const uint32 ImageCount = _dyld_image_count();
    for (uint32 Index = 0; Index < ImageCount; ++Index)
    {
        const mach_header* Header = _dyld_get_image_header(Index);
        const char* ImagePath = _dyld_get_image_name(Index);
        if (Header == nullptr || ImagePath == nullptr || Header->magic != MH_MAGIC_64)
        {
            continue;
        }

        const FString ImageName = NormalizeImageName(ANSI_TO_TCHAR(ImagePath));
        if (!IsFieldIsNativeImporter(ImageName))
        {
            continue;
        }
        bool bAlreadyCached = false;
        for (int32 CachedIndex = 0; CachedIndex < ModuleCount; ++CachedIndex)
        {
            bAlreadyCached |= ModuleNames[CachedIndex] == ImageName;
        }
        if (bAlreadyCached)
        {
            continue;
        }
        if (ModuleCount >= MaxMacTargetCallerModules)
        {
            WarnOnce(TEXT("the Mac FField::IsNative importer count exceeded the range cache"));
            continue;
        }

        Dl_info ImageInfo{};
        if (dladdr(Header, &ImageInfo) == 0 || ImageInfo.dli_fname == nullptr
            || NormalizeImageName(ANSI_TO_TCHAR(ImageInfo.dli_fname)) != ImageName)
        {
            WarnOnce(TEXT("a target caller image could not be identified with dladdr"));
            continue;
        }

        const auto* Header64 = reinterpret_cast<const mach_header_64*>(Header);
        const uint8* CommandCursor = reinterpret_cast<const uint8*>(Header64 + 1);
        const uint8* CommandEnd = CommandCursor + Header64->sizeofcmds;
        const intptr_t Slide = _dyld_get_image_vmaddr_slide(Index);
        uintptr_t ImageStart = UINTPTR_MAX;
        uintptr_t ImageEnd = 0;
        for (uint32 CommandIndex = 0; CommandIndex < Header64->ncmds; ++CommandIndex)
        {
            if (CommandCursor > CommandEnd
                || static_cast<SIZE_T>(CommandEnd - CommandCursor) < sizeof(load_command))
            {
                break;
            }

            const auto* Command = reinterpret_cast<const load_command*>(CommandCursor);
            if (Command->cmdsize < sizeof(load_command)
                || static_cast<SIZE_T>(CommandEnd - CommandCursor) < Command->cmdsize)
            {
                break;
            }

            if (Command->cmd == LC_SEGMENT_64
                && Command->cmdsize >= sizeof(segment_command_64))
            {
                const auto* Segment = reinterpret_cast<const segment_command_64*>(Command);
                if ((Segment->initprot & VM_PROT_EXECUTE) != 0)
                {
                    const uintptr_t Start = static_cast<uintptr_t>(Slide + Segment->vmaddr);
                    if (Segment->vmsize <= UINTPTR_MAX - Start)
                    {
                        ImageStart = FMath::Min(ImageStart, Start);
                        ImageEnd = FMath::Max(ImageEnd, static_cast<uintptr_t>(Start + Segment->vmsize));
                    }
                }
            }
            CommandCursor += Command->cmdsize;
        }

        if (ImageStart >= ImageEnd)
        {
            continue;
        }

        FMacTargetCallerRange& Range = ModuleRanges[ModuleCount];
        Range.Start = ImageStart;
        Range.End = ImageEnd;
        ModuleNames[ModuleCount] = ImageName;
        ++ModuleCount;
    }

    if (ModuleCount == 2 && ModuleRanges[1].Start < ModuleRanges[0].Start)
    {
        Swap(ModuleRanges[0], ModuleRanges[1]);
    }

    FMacTargetCallerRangeCache Candidate{};
    Candidate.ModuleCount = ModuleCount;
    if (ModuleCount == 1)
    {
        Candidate.Ranges[0] = ModuleRanges[0];
        Candidate.RangeCount = 1;
    }
    else if (ModuleCount == 2)
    {
        Candidate.Ranges[0] = ModuleRanges[0];
        if (ModuleRanges[0].End >= ModuleRanges[1].Start)
        {
            Candidate.Ranges[0].End = FMath::Max(
                ModuleRanges[0].End, ModuleRanges[1].End);
            Candidate.RangeCount = 1;
        }
        else
        {
            Candidate.Ranges[1] = ModuleRanges[1];
            Candidate.RangeCount = 2;
            Candidate.Partition = ModuleRanges[0].End
                + (ModuleRanges[1].Start - ModuleRanges[0].End) / 2;
        }
    }

    const FMacTargetCallerRangeCache* Current =
        ActiveMacTargetCallerRangeCache.load(std::memory_order_acquire);
    bool bRangesMatch = Current != nullptr;
    if (bRangesMatch)
    {
        for (int32 Index = 0; Index < MaxMacTargetCallerModules; ++Index)
        {
            bRangesMatch &= Current->Ranges[Index].Start == Candidate.Ranges[Index].Start
                && Current->Ranges[Index].End == Candidate.Ranges[Index].End;
        }
    }
    if (Current != nullptr && Current->ModuleCount == Candidate.ModuleCount
        && Current->RangeCount == Candidate.RangeCount
        && Current->Partition == Candidate.Partition && bRangesMatch)
    {
        return;
    }

    RetainedMacTargetCallerRangeCaches.Add(MakeUnique<FMacTargetCallerRangeCache>(Candidate));
    ActiveMacTargetCallerRangeCache.store(
        RetainedMacTargetCallerRangeCaches.Last().Get(), std::memory_order_release);
}

int32 GetMacTargetCallerModuleCount()
{
    const FMacTargetCallerRangeCache* Cache =
        ActiveMacTargetCallerRangeCache.load(std::memory_order_acquire);
    return Cache != nullptr ? Cache->ModuleCount : 0;
}

bool IsMacTargetFieldIsNativeCaller(const void* CallerAddress)
{
    const FMacTargetCallerRangeCache* Cache =
        ActiveMacTargetCallerRangeCache.load(std::memory_order_acquire);
    if (Cache == nullptr || Cache->RangeCount == 0)
    {
        return false;
    }

    const uintptr_t Address = reinterpret_cast<uintptr_t>(CallerAddress);
    // Select one cached interval, then make one range comparison on this hot path.
    const int32 RangeIndex = Cache->RangeCount == 2 && Address >= Cache->Partition ? 1 : 0;
    const FMacTargetCallerRange& Range = Cache->Ranges[RangeIndex];
    return Address - Range.Start < Range.End - Range.Start;
}

bool InstallMacFieldIsNativeEntryHook()
{
    if (FieldIsNativeEntryHook.Original != nullptr)
    {
        return true;
    }
    if (GetMacTargetCallerModuleCount() == 0)
    {
        return false;
    }
    if (bMacFieldIsNativeEntryAttempted)
    {
        return false;
    }

    const FImportSpec* Spec = nullptr;
    for (const FImportSpec& Candidate : ImportSpecs)
    {
        if (Candidate.Id == EExperienceImport::FieldIsNative)
        {
            Spec = &Candidate;
            break;
        }
    }
    void* Target = Spec != nullptr ? ResolveOriginal(*Spec) : nullptr;
    if (Target == nullptr)
    {
        WarnOnce(TEXT("the Mac FField::IsNative entry could not be resolved"));
        return false;
    }
    bMacFieldIsNativeEntryAttempted = true;
    if (!InstallEditorInlineHook(FieldIsNativeEntryHook, Target,
        reinterpret_cast<void*>(&HookFieldIsNative), TEXT("FField::IsNative")))
    {
        WarnOnce(TEXT("the Mac FField::IsNative entry prologue could not be safely redirected"));
        return false;
    }
    OriginalFieldIsNative = reinterpret_cast<FIsNativeFunction>(FieldIsNativeEntryHook.Original);
    return true;
}

bool RemoveMacFieldIsNativeEntryHook()
{
    if (FieldIsNativeEntryHook.Original == nullptr)
    {
        OriginalFieldIsNative = nullptr;
        return true;
    }
    if (!RemoveEditorInlineHook(FieldIsNativeEntryHook,
        reinterpret_cast<void*>(&HookFieldIsNative), TEXT("FField::IsNative")))
    {
        WarnOnce(TEXT("the Mac FField::IsNative entry could not be restored"));
        return false;
    }
    OriginalFieldIsNative = nullptr;
    return true;
}

void LogMacImportRewriteCounts()
{
    int32 FieldIsNativeSlots = 0;
    int32 FunctionHiddenSlots = 0;
    int32 CheckVerifyFailedSlots = 0;
    int32 TryFindTypeSlowSlots = 0;
    const int32 TargetModuleCount = GetMacTargetCallerModuleCount();
    int32 LinkerDetachSlots = 0;
    for (const auto& Entry : InstalledImports)
    {
        if (Entry.Key == nullptr || *Entry.Key != Entry.Value.Replacement)
        {
            continue;
        }
        switch (Entry.Value.Id)
        {
        case EExperienceImport::FieldIsNative: ++FieldIsNativeSlots; break;
        case EExperienceImport::FunctionHidden: ++FunctionHiddenSlots; break;
        case EExperienceImport::CheckVerifyFailed: ++CheckVerifyFailedSlots; break;
        case EExperienceImport::TryFindTypeSlow: ++TryFindTypeSlowSlots; break;
        case EExperienceImport::LinkerDetach: ++LinkerDetachSlots; break;
        }
    }
    const int32 TotalSlots = FieldIsNativeSlots + FunctionHiddenSlots
        + CheckVerifyFailedSlots + TryFindTypeSlowSlots + LinkerDetachSlots;
    const bool bEntryHookInstalled = FieldIsNativeEntryHook.Original != nullptr;
    const bool bSetLinkerEntryInstalled = SetLinkerEntryHook.Original != nullptr;
    if (FieldIsNativeSlots == LastLoggedMacFieldIsNativeSlots
        && FunctionHiddenSlots == LastLoggedMacFunctionHiddenSlots
        && CheckVerifyFailedSlots == LastLoggedMacCheckVerifyFailedSlots
        && TryFindTypeSlowSlots == LastLoggedMacTryFindTypeSlowSlots
        && TargetModuleCount == LastLoggedMacTargetModuleCount
        && LinkerDetachSlots == LastLoggedMacLinkerDetachSlots
        && bEntryHookInstalled == bLastLoggedMacEntryHook
        && bSetLinkerEntryInstalled == bLastLoggedMacSetLinkerEntry)
    {
        return;
    }
    LastLoggedMacFieldIsNativeSlots = FieldIsNativeSlots;
    LastLoggedMacFunctionHiddenSlots = FunctionHiddenSlots;
    LastLoggedMacCheckVerifyFailedSlots = CheckVerifyFailedSlots;
    LastLoggedMacTryFindTypeSlowSlots = TryFindTypeSlowSlots;
    LastLoggedMacTargetModuleCount = TargetModuleCount;
    LastLoggedMacLinkerDetachSlots = LinkerDetachSlots;
    bLastLoggedMacEntryHook = bEntryHookInstalled;
    bLastLoggedMacSetLinkerEntry = bSetLinkerEntryInstalled;
    UE_LOG(LogSoloASEditorExperienceImports, Display,
        TEXT("Mac Editor experience imports rewritten: slots=%d (IsNative=%d Hidden=%d Verify=%d Enum=%d SetLinker=%d) entry_IsNative=%d entry_SetLinker=%d")
        TEXT(" target_modules=%d"),
        TotalSlots, FieldIsNativeSlots, FunctionHiddenSlots, CheckVerifyFailedSlots,
        TryFindTypeSlowSlots, LinkerDetachSlots, bEntryHookInstalled ? 1 : 0,
        bSetLinkerEntryInstalled ? 1 : 0, TargetModuleCount);
}
#endif

bool WritePointer(void** Slot, void* Value)
{
#if PLATFORM_WINDOWS
    void* Original = *Slot;
    DWORD PreviousProtection = 0;
    if (!VirtualProtect(Slot, sizeof(void*), PAGE_READWRITE, &PreviousProtection))
    {
        return false;
    }
    InterlockedExchangePointer(Slot, Value);
    DWORD IgnoredProtection = 0;
    if (!VirtualProtect(Slot, sizeof(void*), PreviousProtection, &IgnoredProtection))
    {
        InterlockedExchangePointer(Slot, Original);
        VirtualProtect(Slot, sizeof(void*), PreviousProtection, &IgnoredProtection);
        return false;
    }
#else
    const mach_vm_address_t Address = reinterpret_cast<mach_vm_address_t>(Slot);
    mach_vm_address_t RegionAddress = Address;
    mach_vm_size_t RegionSize = 0;
    vm_region_basic_info_data_64_t RegionInfo = {};
    mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t Object = MACH_PORT_NULL;
    const kern_return_t RegionQuery = mach_vm_region(
        mach_task_self(), &RegionAddress, &RegionSize,
        VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&RegionInfo), &Count, &Object);
    if (Object != MACH_PORT_NULL)
    {
        mach_port_deallocate(mach_task_self(), Object);
    }
    if (RegionQuery != KERN_SUCCESS || Address < RegionAddress
        || Address + sizeof(void*) > RegionAddress + RegionSize)
    {
        return false;
    }
    const mach_vm_address_t Page = Address
        & ~(static_cast<mach_vm_address_t>(vm_page_size) - 1);
    if (mach_vm_protect(mach_task_self(), Page, vm_page_size, false,
        RegionInfo.protection | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
    {
        return false;
    }
    void* Original = __atomic_load_n(Slot, __ATOMIC_ACQUIRE);
    __atomic_store_n(Slot, Value, __ATOMIC_RELEASE);
    if (mach_vm_protect(mach_task_self(), Page, vm_page_size, false,
        RegionInfo.protection) != KERN_SUCCESS)
    {
        __atomic_store_n(Slot, Original, __ATOMIC_RELEASE);
        mach_vm_protect(mach_task_self(), Page, vm_page_size, false,
            RegionInfo.protection);
        return false;
    }
#endif
    return true;
}

bool ApplySlot(
    void* ImageBase,
    const FString& ImageName,
    void** Slot,
    const FImportSpec& Spec,
    void* OriginalAddress,
    bool bUnboundLazySlot)
{
    if (FInstalledImport* Existing = InstalledImports.Find(Slot))
    {
        return *Slot == Existing->Replacement;
    }

    void* Current = *Slot;
    if (Current == Spec.Replacement)
    {
        WarnOnce(TEXT("an import slot already points at the redirector without a registration record"));
        return false;
    }
    if (Current != OriginalAddress && !bUnboundLazySlot)
    {
        WarnOnce(TEXT("an import slot has an unexpected existing owner"));
        return false;
    }
    if (!WritePointer(Slot, Spec.Replacement))
    {
        WarnOnce(TEXT("an import slot could not be written or its page protection restored"));
        return false;
    }

    FInstalledImport& Installed = InstalledImports.Add(Slot);
    Installed.Id = Spec.Id;
    Installed.ImageBase = ImageBase;
    Installed.ImageName = ImageName;
    Installed.Slot = Slot;
    Installed.Original = Current;
    Installed.Replacement = Spec.Replacement;
    return true;
}

void RestoreSlot(void** Slot)
{
    FInstalledImport* Installed = InstalledImports.Find(Slot);
    if (Installed == nullptr)
    {
        return;
    }
    if (*Slot == Installed->Replacement)
    {
        if (!WritePointer(Slot, Installed->Original))
        {
            WarnOnce(TEXT("an import slot could not be restored"));
            return;
        }
    }
    else if (*Slot != Installed->Original)
    {
        WarnOnce(TEXT("an import slot changed owner before restoration"));
    }
    InstalledImports.Remove(Slot);
}

#if PLATFORM_WINDOWS
bool InImage(uint64 Rva, uint64 Bytes, uint32 ImageSize)
{
    return Rva < ImageSize && Bytes <= ImageSize - Rva;
}

int32 RebindWindowsImage(uint8* Base, const FString& ImageName)
{
    const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
    if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0)
    {
        WarnOnce(TEXT("a target PE image has an invalid DOS header"));
        return 0;
    }
    const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE
        || Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        WarnOnce(TEXT("a target PE image is not a supported 64-bit image"));
        return 0;
    }

    const uint32 ImageSize = Nt->OptionalHeader.SizeOfImage;
    int32 Changed = 0;
    int32 LinkerDetachSlotsChanged = 0;
    const auto& Directory = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (Directory.VirtualAddress == 0
        || !InImage(Directory.VirtualAddress, Directory.Size, ImageSize))
    {
        WarnOnce(TEXT("a target PE image has no valid ordinary import table"));
        return 0;
    }

    for (uint32 Offset = 0;
         Offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= Directory.Size;
         Offset += sizeof(IMAGE_IMPORT_DESCRIPTOR))
    {
        const auto* Import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
            Base + Directory.VirtualAddress + Offset);
        if (Import->Name == 0)
        {
            break;
        }
        if (!InImage(Import->Name, 1, ImageSize))
        {
            continue;
        }
        const ANSICHAR* Provider = reinterpret_cast<const ANSICHAR*>(Base + Import->Name);
        if (Import->OriginalFirstThunk == 0 || Import->FirstThunk == 0)
        {
            WarnOnce(TEXT("a target PE import descriptor omits named thunks"));
            continue;
        }

        for (uint64 Index = 0; ; ++Index)
        {
            const uint64 NameRva = Import->OriginalFirstThunk
                + Index * sizeof(IMAGE_THUNK_DATA64);
            const uint64 SlotRva = Import->FirstThunk
                + Index * sizeof(IMAGE_THUNK_DATA64);
            if (!InImage(NameRva, sizeof(IMAGE_THUNK_DATA64), ImageSize)
                || !InImage(SlotRva, sizeof(void*), ImageSize))
            {
                break;
            }
            const auto* NameThunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(Base + NameRva);
            const uint64 Name = NameThunk->u1.AddressOfData;
            if (Name == 0)
            {
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL64(Name)
                || !InImage(Name, sizeof(WORD), ImageSize))
            {
                continue;
            }
            const ANSICHAR* Symbol = reinterpret_cast<const ANSICHAR*>(
                Base + Name + sizeof(WORD));
            const FString NormalizedImporter = NormalizeImageName(*ImageName);
            const FImportSpec* Spec = nullptr;
            for (const FImportSpec& Candidate : ImportSpecs)
            {
                const bool bImporterMatches = FCStringAnsi::Strcmp(Candidate.Importer, "*") == 0
                    || NormalizedImporter == UTF8_TO_TCHAR(Candidate.Importer);
                if (bImporterMatches
                    && FCStringAnsi::Strcmp(Provider, Candidate.Provider) == 0
                    && FCStringAnsi::Strcmp(Symbol, Candidate.WindowsSymbol) == 0
                    && IsImportEnabled(Candidate.Id))
                {
                    Spec = &Candidate;
                    break;
                }
            }
            if (Spec == nullptr)
            {
                continue;
            }
            if (Spec->Id == EExperienceImport::CheckVerifyFailed
                && OriginalCheckVerifyFailedV == nullptr)
            {
                continue;
            }

            HMODULE ProviderModule = GetModuleHandleA(Spec->Provider);
            FARPROC OriginalAddress = ProviderModule != nullptr
                ? GetProcAddress(ProviderModule, Spec->WindowsSymbol) : nullptr;
            if (OriginalAddress == nullptr)
            {
                WarnOnce(TEXT("a target import symbol is absent from its provider"));
                continue;
            }
            if (Spec->Id == EExperienceImport::LinkerDetach && EngineSetLinker == nullptr)
            {
                EngineSetLinker = reinterpret_cast<FSetLinkerFunction>(OriginalAddress);
                if (SetLinkerEntryHook.Original == nullptr)
                {
                    OriginalSetLinker = EngineSetLinker;
                }
            }
            void** Slot = reinterpret_cast<void**>(Base + SlotRva);
            const bool bWasInstalled = InstalledImports.Contains(Slot);
            const bool bApplied = ApplySlot(Base, ImageName, Slot, *Spec,
                reinterpret_cast<void*>(OriginalAddress), false);
            Changed += bApplied ? 1 : 0;
            if (bApplied && !bWasInstalled && Spec->Id == EExperienceImport::LinkerDetach)
            {
                ++LinkerDetachSlotsChanged;
            }
        }
    }
    if (LinkerDetachSlotsChanged > 0)
    {
        UE_LOG(LogSoloASEditorExperienceImports, Display,
            TEXT("Windows Editor SetLinker imports rewritten: image=%s slots=%d"),
            *ImageName, LinkerDetachSlotsChanged);
    }
    return Changed;
}

void ScanLoadedWindowsImages()
{
    const HANDLE Snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (Snapshot == INVALID_HANDLE_VALUE)
    {
        WarnOnce(TEXT("process module enumeration failed"));
        return;
    }
    ON_SCOPE_EXIT { CloseHandle(Snapshot); };

    MODULEENTRY32W Entry = {};
    Entry.dwSize = sizeof(Entry);
    for (BOOL More = Module32FirstW(Snapshot, &Entry);
         More;
         More = Module32NextW(Snapshot, &Entry))
    {
        const FString ImageName(Entry.szModule);
        if (!IsTargetImage(ImageName))
        {
            continue;
        }
        HMODULE Retained = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(Entry.modBaseAddr), &Retained))
        {
            WarnOnce(TEXT("a target Editor module could not be retained while scanning"));
            continue;
        }
        ON_SCOPE_EXIT { FreeLibrary(Retained); };
        RebindWindowsImage(Entry.modBaseAddr, NormalizeImageName(*ImageName));
    }
}
#else
bool InLinkedit(const segment_command_64& Linkedit, uint64 Offset, uint64 Size)
{
    return Offset >= Linkedit.fileoff
        && Offset - Linkedit.fileoff <= Linkedit.filesize
        && Size <= Linkedit.filesize - (Offset - Linkedit.fileoff);
}

int32 RebindMachImage(const mach_header_64* Header, intptr_t Slide, const FString& ImageName)
{
    if (Header == nullptr || Header->magic != MH_MAGIC_64)
    {
        WarnOnce(TEXT("a target Mach-O image is not a supported 64-bit image"));
        return 0;
    }

    const segment_command_64* Linkedit = nullptr;
    const symtab_command* Symtab = nullptr;
    const dysymtab_command* Dysymtab = nullptr;
    const uint8* Begin = reinterpret_cast<const uint8*>(Header + 1);
    const uint8* End = Begin + Header->sizeofcmds;
    const uint8* Cursor = Begin;
    for (uint32 Index = 0; Index < Header->ncmds; ++Index)
    {
        if (Cursor + sizeof(load_command) > End)
        {
            WarnOnce(TEXT("a target Mach-O load command is truncated"));
            return 0;
        }
        const auto* Command = reinterpret_cast<const load_command*>(Cursor);
        if (Command->cmdsize < sizeof(load_command) || Cursor + Command->cmdsize > End)
        {
            WarnOnce(TEXT("a target Mach-O load command has an invalid size"));
            return 0;
        }
        if (Command->cmd == LC_SEGMENT_64
            && Command->cmdsize >= sizeof(segment_command_64))
        {
            const auto* Segment = reinterpret_cast<const segment_command_64*>(Cursor);
            if (FCStringAnsi::Strncmp(Segment->segname, SEG_LINKEDIT, 16) == 0)
            {
                Linkedit = Segment;
            }
        }
        else if (Command->cmd == LC_SYMTAB
            && Command->cmdsize >= sizeof(symtab_command))
        {
            Symtab = reinterpret_cast<const symtab_command*>(Cursor);
        }
        else if (Command->cmd == LC_DYSYMTAB
            && Command->cmdsize >= sizeof(dysymtab_command))
        {
            Dysymtab = reinterpret_cast<const dysymtab_command*>(Cursor);
        }
        Cursor += Command->cmdsize;
    }
    if (Linkedit == nullptr || Symtab == nullptr || Dysymtab == nullptr
        || Dysymtab->nindirectsyms == 0)
    {
        WarnOnce(TEXT("a target Mach-O image lacks indirect symbol tables"));
        return 0;
    }
    if (!InLinkedit(*Linkedit, Symtab->symoff,
            uint64(Symtab->nsyms) * sizeof(nlist_64))
        || !InLinkedit(*Linkedit, Symtab->stroff, Symtab->strsize)
        || !InLinkedit(*Linkedit, Dysymtab->indirectsymoff,
            uint64(Dysymtab->nindirectsyms) * sizeof(uint32)))
    {
        WarnOnce(TEXT("a target Mach-O symbol table is outside __LINKEDIT"));
        return 0;
    }

    const uintptr_t LinkeditBase = Slide + Linkedit->vmaddr - Linkedit->fileoff;
    const auto* Symbols = reinterpret_cast<const nlist_64*>(LinkeditBase + Symtab->symoff);
    const auto* Strings = reinterpret_cast<const ANSICHAR*>(LinkeditBase + Symtab->stroff);
    const auto* Indirect = reinterpret_cast<const uint32*>(
        LinkeditBase + Dysymtab->indirectsymoff);

    uintptr_t ImageStart = UINTPTR_MAX;
    uintptr_t ImageEnd = 0;
    Cursor = Begin;
    for (uint32 Index = 0; Index < Header->ncmds; ++Index)
    {
        const auto* Command = reinterpret_cast<const load_command*>(Cursor);
        if (Command->cmd == LC_SEGMENT_64
            && Command->cmdsize >= sizeof(segment_command_64))
        {
            const auto* Segment = reinterpret_cast<const segment_command_64*>(Cursor);
            ImageStart = FMath::Min(ImageStart,
                static_cast<uintptr_t>(Slide + Segment->vmaddr));
            ImageEnd = FMath::Max(ImageEnd,
                static_cast<uintptr_t>(Slide + Segment->vmaddr + Segment->vmsize));
        }
        Cursor += Command->cmdsize;
    }

    int32 Changed = 0;
    Cursor = Begin;
    for (uint32 CommandIndex = 0; CommandIndex < Header->ncmds; ++CommandIndex)
    {
        const auto* Command = reinterpret_cast<const load_command*>(Cursor);
        if (Command->cmd == LC_SEGMENT_64
            && Command->cmdsize >= sizeof(segment_command_64))
        {
            const auto* Segment = reinterpret_cast<const segment_command_64*>(Cursor);
            const auto* Sections = reinterpret_cast<const section_64*>(Segment + 1);
            for (uint32 SectionIndex = 0; SectionIndex < Segment->nsects; ++SectionIndex)
            {
                const section_64& Section = Sections[SectionIndex];
                const uint32 SectionType = Section.flags & SECTION_TYPE;
                if (SectionType != S_LAZY_SYMBOL_POINTERS
                    && SectionType != S_NON_LAZY_SYMBOL_POINTERS)
                {
                    continue;
                }
                const uint64 SlotCount = Section.size / sizeof(void*);
                if (uint64(Section.reserved1) + SlotCount > Dysymtab->nindirectsyms)
                {
                    WarnOnce(TEXT("a target Mach-O indirect symbol range is invalid"));
                    continue;
                }
                auto** Slots = reinterpret_cast<void**>(Slide + Section.addr);
                for (uint64 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
                {
                    const uint32 SymbolIndex = Indirect[Section.reserved1 + SlotIndex];
                    if (SymbolIndex & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)
                        || SymbolIndex >= Symtab->nsyms)
                    {
                        continue;
                    }
                    const uint32 StringIndex = Symbols[SymbolIndex].n_un.n_strx;
                    if (StringIndex >= Symtab->strsize)
                    {
                        continue;
                    }
                    const ANSICHAR* Symbol = Strings + StringIndex;
                    const FImportSpec* Spec = FindImportSpec(
                        NormalizeImageName(*ImageName), "", Symbol, true);
                    if (Spec == nullptr)
                    {
                        continue;
                    }
                    if (Spec->Id == EExperienceImport::CheckVerifyFailed
                        && OriginalCheckVerifyFailedV == nullptr)
                    {
                        continue;
                    }
                    void* OriginalAddress = ResolveOriginal(*Spec);
                    if (OriginalAddress == nullptr)
                    {
                        WarnOnce(TEXT("a target Mach-O symbol could not be resolved"));
                        continue;
                    }
                    if (Spec->Id == EExperienceImport::LinkerDetach && EngineSetLinker == nullptr)
                    {
                        EngineSetLinker = reinterpret_cast<FSetLinkerFunction>(OriginalAddress);
                        if (SetLinkerEntryHook.Original == nullptr)
                        {
                            OriginalSetLinker = EngineSetLinker;
                        }
                    }
                    void** Slot = &Slots[SlotIndex];
                    const uintptr_t Current = reinterpret_cast<uintptr_t>(*Slot);
                    const bool bUnboundLazySlot = SectionType == S_LAZY_SYMBOL_POINTERS
                        && Current >= ImageStart && Current < ImageEnd;
                    Changed += ApplySlot(const_cast<mach_header_64*>(Header), ImageName,
                        Slot, *Spec, OriginalAddress, bUnboundLazySlot) ? 1 : 0;
                }
            }
        }
        Cursor += Command->cmdsize;
    }
    return Changed;
}

void ScanLoadedMachImages()
{
    const uint32 ImageCount = _dyld_image_count();
    for (uint32 Index = 0; Index < ImageCount; ++Index)
    {
        const mach_header* Header = _dyld_get_image_header(Index);
        const char* ImagePath = _dyld_get_image_name(Index);
        if (Header == nullptr || ImagePath == nullptr
            || Header->magic != MH_MAGIC_64)
        {
            continue;
        }
        const FString ImageName = NormalizeImageName(ANSI_TO_TCHAR(ImagePath));
        if (!IsTargetImage(ImageName))
        {
            continue;
        }
        RebindMachImage(reinterpret_cast<const mach_header_64*>(Header),
            _dyld_get_image_vmaddr_slide(Index), ImageName);
    }
}
#endif

void ScanLoadedImages()
{
#if PLATFORM_WINDOWS
    ScanLoadedWindowsImages();
#else
    ScanLoadedMachImages();
#endif
}

void ScanLoadedImageForModule(FName ModuleName)
{
    const FString ImageName = TEXT("UnrealEditor-") + ModuleName.ToString();
    if (!IsTargetImage(ImageName))
    {
        return;
    }
#if PLATFORM_WINDOWS
    const FString DllName = ImageName + TEXT(".dll");
    HMODULE Image = nullptr;
    if (GetModuleHandleExW(0, *DllName, &Image))
    {
        ON_SCOPE_EXIT { FreeLibrary(Image); };
        RebindWindowsImage(reinterpret_cast<uint8*>(Image), ImageName);
    }
#else
    const uint32 ImageCount = _dyld_image_count();
    for (uint32 Index = 0; Index < ImageCount; ++Index)
    {
        const mach_header* Header = _dyld_get_image_header(Index);
        const char* ImagePath = _dyld_get_image_name(Index);
        if (Header != nullptr && ImagePath != nullptr
            && Header->magic == MH_MAGIC_64
            && NormalizeImageName(ANSI_TO_TCHAR(ImagePath)) == ImageName)
        {
            RebindMachImage(reinterpret_cast<const mach_header_64*>(Header),
                _dyld_get_image_vmaddr_slide(Index), ImageName);
            return;
        }
    }
#endif
}

void RestoreImports()
{
    FScopeLock Lock(&ImportLock);
    for (auto It = InstalledImports.CreateIterator(); It; ++It)
    {
        FInstalledImport& Patch = It.Value();
        if (Patch.Slot != nullptr && *Patch.Slot == Patch.Replacement)
        {
            if (!WritePointer(Patch.Slot, Patch.Original))
            {
                WarnOnce(TEXT("an import slot could not be restored"));
                continue;
            }
        }
        else if (Patch.Slot != nullptr && *Patch.Slot != Patch.Original)
        {
            WarnOnce(TEXT("an import slot changed owner before restoration"));
        }
        It.RemoveCurrent();
    }
}

void ApplyImportState(FName LoadedModule = NAME_None)
{
    FScopeLock Lock(&ImportLock);
    const bool bExperienceEnabled = CVarEditorExperienceImports.GetValueOnAnyThread() != 0;
    const bool bLinkerDetachEnabled = CVarEditorLinkerDetachSafety.GetValueOnAnyThread() != 0;
#if PLATFORM_MAC
    if (bExperienceEnabled)
    {
        InstallMacFieldIsNativeEntryHook();
    }
#endif
    if (bLinkerDetachEnabled)
    {
        InstallLinkerDetachEntryHook();
    }
    for (auto It = InstalledImports.CreateIterator(); It; ++It)
    {
        FInstalledImport& Patch = It.Value();
        if (IsImportEnabled(Patch.Id))
        {
            continue;
        }
        if (Patch.Slot != nullptr && *Patch.Slot == Patch.Replacement)
        {
            if (!WritePointer(Patch.Slot, Patch.Original))
            {
                WarnOnce(TEXT("an import slot could not be restored"));
                continue;
            }
        }
        else if (Patch.Slot != nullptr && *Patch.Slot != Patch.Original)
        {
            WarnOnce(TEXT("an import slot changed owner before restoration"));
        }
        It.RemoveCurrent();
    }
    if (bExperienceEnabled || bLinkerDetachEnabled)
    {
        if (LoadedModule.IsNone())
        {
            ScanLoadedImages();
        }
        else
        {
            ScanLoadedImageForModule(LoadedModule);
        }
    }
#if PLATFORM_MAC
    LogMacImportRewriteCounts();
#endif
}

void OnExperienceVariableChanged(IConsoleVariable*)
{
    ApplyImportState();
}

void OnModulesChanged(FName ModuleName, EModuleChangeReason Reason)
{
    if (Reason == EModuleChangeReason::ModuleLoaded)
    {
#if PLATFORM_MAC
        if (IsFieldIsNativeImporter(ModuleName))
        {
            FScopeLock RangeLock(&ImportLock);
            CacheMacTargetCallerRanges();
        }
#endif
        ApplyImportState(ModuleName);
    }
    else if (Reason == EModuleChangeReason::ModuleUnloaded)
    {
        const FString ImageName = TEXT("UnrealEditor-") + ModuleName.ToString();
        if (!IsTargetImage(ImageName))
        {
            return;
        }
        FScopeLock Lock(&ImportLock);
#if PLATFORM_MAC
        if (IsFieldIsNativeImporter(ModuleName))
        {
            CacheMacTargetCallerRanges();
        }
#endif
        for (auto It = InstalledImports.CreateIterator(); It; ++It)
        {
            if (NormalizeImageName(*It.Value().ImageName) == ImageName)
            {
                // The target image is unloading; never dereference its import slots.
                It.RemoveCurrent();
            }
        }
#if PLATFORM_MAC
        LogMacImportRewriteCounts();
#endif
    }
}
}

bool IsFieldNativeForScriptEditor(const FField* Field)
{
    return HookFieldIsNative(Field);
}

bool IsFunctionHiddenForScriptEditor(
    const UFunction* Function,
    const UClass* Class)
{
    return HookIsFunctionHiddenFromClass(Function, Class);
}

bool ShouldSuppressDuplicateDetailPropertyCheck(
    const ANSICHAR* Expression,
    const ANSICHAR* File)
{
    return CVarEditorExperienceImports.GetValueOnAnyThread() != 0
        && OriginalCheckVerifyFailedV != nullptr
        && IsDuplicateDetailLayoutCheck(Expression, File);
}

UField* FindTypeForScriptEditCondition(
    UClass* TypeClass,
    FStringView Name,
    EFindFirstObjectOptions Options)
{
    return HookTryFindTypeSlow(TypeClass, Name, Options);
}

void InitializeEditorExperienceImports()
{
    if (bImportHooksInitialized)
    {
        return;
    }
    bImportHooksInitialized = true;
    UE_LOG(LogSoloASEditorExperienceImports, Display,
        TEXT("Editor linker detach safety initialized: enabled=%d"),
        CVarEditorLinkerDetachSafety.GetValueOnAnyThread());
    OriginalCheckVerifyFailedV = reinterpret_cast<FCheckVerifyFailedVFunction>(
        ResolveOriginalCheckVerifyFailedV());
    if (OriginalCheckVerifyFailedV == nullptr)
    {
        WarnOnce(TEXT("the original variadic assertion forwarder export was not found"));
    }
    if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(
        TEXT("soloas.EditorExperienceImports")))
    {
        ExperienceVariable = Variable;
        ConsoleChangedHandle = Variable->OnChangedDelegate().AddStatic(
            &OnExperienceVariableChanged);
    }
    if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(
        TEXT("soloas.EditorLinkerDetachSafety")))
    {
        LinkerDetachVariable = Variable;
        LinkerDetachConsoleChangedHandle = Variable->OnChangedDelegate().AddStatic(
            &OnExperienceVariableChanged);
    }
    ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddStatic(
        &OnModulesChanged);
#if PLATFORM_MAC
    {
        FScopeLock Lock(&ImportLock);
    CacheMacTargetCallerRanges();
    }
#endif
    ApplyImportState();
}

void ShutdownEditorExperienceImports()
{
    if (!bImportHooksInitialized)
    {
        return;
    }
    if (ExperienceVariable != nullptr && ConsoleChangedHandle.IsValid())
    {
        ExperienceVariable->OnChangedDelegate().Remove(ConsoleChangedHandle);
    }
    if (LinkerDetachVariable != nullptr && LinkerDetachConsoleChangedHandle.IsValid())
    {
        LinkerDetachVariable->OnChangedDelegate().Remove(LinkerDetachConsoleChangedHandle);
    }
    if (ModulesChangedHandle.IsValid())
    {
        FModuleManager::Get().OnModulesChanged().Remove(ModulesChangedHandle);
    }
    RestoreImports();
    RemoveLinkerDetachEntryHook();
#if PLATFORM_MAC
    RemoveMacFieldIsNativeEntryHook();
    LastLoggedMacFieldIsNativeSlots = INDEX_NONE;
    LastLoggedMacFunctionHiddenSlots = INDEX_NONE;
    LastLoggedMacCheckVerifyFailedSlots = INDEX_NONE;
    LastLoggedMacTryFindTypeSlowSlots = INDEX_NONE;
    LastLoggedMacTargetModuleCount = INDEX_NONE;
    LastLoggedMacLinkerDetachSlots = INDEX_NONE;
    bLastLoggedMacEntryHook = false;
    bLastLoggedMacSetLinkerEntry = false;
#endif
    ExperienceVariable = nullptr;
    LinkerDetachVariable = nullptr;
    ConsoleChangedHandle.Reset();
    LinkerDetachConsoleChangedHandle.Reset();
    ModulesChangedHandle.Reset();
    bImportHooksInitialized = false;
}
}

#else
namespace SoloASBridge::Editor
{
void InitializeEditorExperienceImports() {}
void ShutdownEditorExperienceImports() {}
}
#endif
