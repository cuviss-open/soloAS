#include "Editor/SoloASBridgeCoreRedirectClassificationHook.h"
#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)

#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Editor/SoloASBridgeEditorInlineHook.h"
#include "UObject/Class.h"
#include "UObject/CoreRedirects.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <tlhelp32.h>
#include "Windows/HideWindowsPlatformTypes.h"
#include <intrin.h>
#else
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSoloASBridgeCoreRedirectClassification, Log, All);

using namespace SoloASBridge::Editor;

namespace
{
TAutoConsoleVariable<int32> CVarEditorCoreRedirectClassification(
	TEXT("soloas.EditorCoreRedirectClassification"), 1,
	TEXT("Classify ASClass redirects in the editor. 0 uses the original classifiers."));

using FGetFlagsForTypeName = ECoreRedirectFlags (*)(FName, FName);
using FGetFlagsForTypeClass = ECoreRedirectFlags (*)(UClass*);

constexpr const ANSICHAR CoreRedirectNameSymbol[] =
    "_ZN14FCoreRedirects19GetFlagsForTypeNameE5FNameS0_";
constexpr const ANSICHAR CoreRedirectClassSymbol[] =
    "_ZN14FCoreRedirects20GetFlagsForTypeClassEP6UClass";

struct FInstalledImport
{
    FString ImageName;
    void** Slot = nullptr;
    void* Original = nullptr;
    void* Replacement = nullptr;
};

FEditorInlineHookHandle CoreRedirectEntryHook;
FGetFlagsForTypeName OriginalGetFlagsForTypeName = nullptr;
FGetFlagsForTypeClass OriginalGetFlagsForTypeClass = nullptr;
void* CoreRedirectNameFunctionAddress = nullptr;
void* CoreRedirectClassFunctionAddress = nullptr;
uintptr_t CoreUObjectTextStart = 0;
uintptr_t CoreUObjectTextEnd = 0;
FCriticalSection ImportLock;
TMap<void**, FInstalledImport> InstalledImports;
FDelegateHandle ModulesChangedHandle;
bool bCoreRedirectClassificationInitialized = false;
bool bWarned = false;

void WarnOnce(const TCHAR* Reason)
{
    if (bWarned)
    {
        return;
    }
    bWarned = true;
    UE_LOG(LogSoloASBridgeCoreRedirectClassification, Warning,
        TEXT("CoreRedirect ASClass classification hook unavailable: %s. Stock behavior remains for uncovered calls."),
        Reason);
}

FString NormalizeImageName(const TCHAR* ImageName)
{
    FString Result = FPaths::GetCleanFilename(
        ImageName == nullptr ? TEXT("") : ImageName);
    Result.RemoveFromEnd(TEXT(".dll"), ESearchCase::IgnoreCase);
    Result.RemoveFromEnd(TEXT(".dylib"), ESearchCase::IgnoreCase);
    return Result;
}

bool IsScriptClassName(FName TypeName)
{
    static const FName ASClassName(TEXT("ASClass"));
    return TypeName == ASClassName;
}

ECoreRedirectFlags CallOriginal(FName PackageName, FName TypeName)
{
    return OriginalGetFlagsForTypeName != nullptr
        ? OriginalGetFlagsForTypeName(PackageName, TypeName)
        : ECoreRedirectFlags::Type_Object;
}

ECoreRedirectFlags CallOriginal(UClass* TypeClass)
{
    return OriginalGetFlagsForTypeClass != nullptr
        ? OriginalGetFlagsForTypeClass(TypeClass)
        : ECoreRedirectFlags::Type_Object;
}

ECoreRedirectFlags ImportedGetFlagsForTypeName(FName PackageName, FName TypeName)
{
    if (CVarEditorCoreRedirectClassification.GetValueOnAnyThread() != 0
		&& IsScriptClassName(TypeName))
    {
        return ECoreRedirectFlags::Type_Class;
    }
    return CallOriginal(PackageName, TypeName);
}

ECoreRedirectFlags ImportedGetFlagsForTypeClass(UClass* TypeClass)
{
    if (CVarEditorCoreRedirectClassification.GetValueOnAnyThread() != 0
		&& TypeClass != nullptr && IsScriptClassName(TypeClass->GetFName()))
    {
        return ECoreRedirectFlags::Type_Class;
    }
    return CallOriginal(TypeClass);
}

void* FindReplacement(void* Original)
{
    if (Original == CoreRedirectNameFunctionAddress)
    {
        return reinterpret_cast<void*>(&ImportedGetFlagsForTypeName);
    }
    if (Original == CoreRedirectClassFunctionAddress)
    {
        return reinterpret_cast<void*>(&ImportedGetFlagsForTypeClass);
    }
    return nullptr;
}

bool IsCoreUObjectCaller(const void* Caller)
{
    const uintptr_t Address = reinterpret_cast<uintptr_t>(Caller);
    return Address >= CoreUObjectTextStart && Address < CoreUObjectTextEnd;
}

FORCENOINLINE ECoreRedirectFlags CoreUObjectGetFlagsForTypeName(
    FName PackageName,
    FName TypeName)
{
#if PLATFORM_WINDOWS
    const void* Caller = _ReturnAddress();
#else
    const void* Caller = __builtin_return_address(0);
#endif
    if (CVarEditorCoreRedirectClassification.GetValueOnAnyThread() != 0
		&& IsCoreUObjectCaller(Caller) && IsScriptClassName(TypeName))
    {
        return ECoreRedirectFlags::Type_Class;
    }
    return CallOriginal(PackageName, TypeName);
}

bool WritePointer(void** Slot, void* Value)
{
#if PLATFORM_WINDOWS
    void* Original = *Slot;
    DWORD PreviousProtection = 0;
    if (!VirtualProtect(Slot, sizeof(void*), PAGE_READWRITE,
        &PreviousProtection))
    {
        return false;
    }
    InterlockedExchangePointer(Slot, Value);
    DWORD IgnoredProtection = 0;
    if (!VirtualProtect(Slot, sizeof(void*), PreviousProtection,
        &IgnoredProtection))
    {
        InterlockedExchangePointer(Slot, Original);
        VirtualProtect(Slot, sizeof(void*), PreviousProtection,
            &IgnoredProtection);
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

bool ApplySlot(const FString& ImageName, void** Slot,
    void* Original, void* Replacement, bool bUnboundLazySlot)
{
    if (FInstalledImport* Existing = InstalledImports.Find(Slot))
    {
        return *Slot == Existing->Replacement;
    }

    void* Current = *Slot;
    if (Current == Replacement)
    {
        WarnOnce(TEXT("an import slot already points at the redirector without a registration record"));
        return false;
    }
    if (Current != Original && !bUnboundLazySlot)
    {
        WarnOnce(TEXT("a CoreRedirect import slot has an unexpected existing owner"));
        return false;
    }
    if (!WritePointer(Slot, Replacement))
    {
        WarnOnce(TEXT("an import slot could not be written or its page protection restored"));
        return false;
    }

    FInstalledImport& Installed = InstalledImports.Add(Slot);
    Installed.ImageName = ImageName;
    Installed.Slot = Slot;
    Installed.Original = Current;
    Installed.Replacement = Replacement;
    return true;
}

#if PLATFORM_WINDOWS
bool InImage(uint64 Rva, uint64 Bytes, uint32 ImageSize)
{
    return Rva < ImageSize && Bytes <= ImageSize - Rva;
}

struct FDelayImportDescriptor
{
    DWORD Attributes;
    DWORD DllName;
    DWORD ModuleHandle;
    DWORD ImportAddressTable;
    DWORD ImportNameTable;
    DWORD BoundImportAddressTable;
    DWORD UnloadInformationTable;
    DWORD TimeDateStamp;
};

uint64 DelayValueToRva(uint64 Value, bool bRva, const uint8* Base,
    uint32 ImageSize)
{
    if (bRva)
    {
        return Value;
    }
    const uintptr_t Address = static_cast<uintptr_t>(Value);
    const uintptr_t ImageBase = reinterpret_cast<uintptr_t>(Base);
    return Address >= ImageBase && Address - ImageBase < ImageSize
        ? Address - ImageBase : UINT64_MAX;
}

int32 ScanWindowsThunks(uint8* Base, const FString& ImageName,
    uint64 NameTableRva, uint64 SlotTableRva, uint32 ImageSize,
    bool bDelayImport, bool bRva, HMODULE Provider)
{
    int32 Changed = 0;
    for (uint64 Index = 0; ; ++Index)
    {
        const uint64 NameThunkRva = NameTableRva
            + Index * sizeof(IMAGE_THUNK_DATA64);
        const uint64 SlotRva = SlotTableRva
            + Index * sizeof(IMAGE_THUNK_DATA64);
        if (!InImage(NameThunkRva, sizeof(IMAGE_THUNK_DATA64), ImageSize)
            || !InImage(SlotRva, sizeof(void*), ImageSize))
        {
            break;
        }
        const auto* NameThunk = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
            Base + NameThunkRva);
        if (NameThunk->u1.AddressOfData == 0)
        {
            break;
        }
        if (IMAGE_SNAP_BY_ORDINAL64(NameThunk->u1.Ordinal))
        {
            continue;
        }
        const uint64 NameRva = bDelayImport
            ? DelayValueToRva(NameThunk->u1.AddressOfData, bRva, Base, ImageSize)
            : NameThunk->u1.AddressOfData;
        if (NameRva == UINT64_MAX
            || !InImage(NameRva, sizeof(WORD), ImageSize))
        {
            continue;
        }
        const ANSICHAR* Symbol = reinterpret_cast<const ANSICHAR*>(
            Base + NameRva + sizeof(WORD));
        if (FCStringAnsi::Strstr(Symbol, "FCoreRedirects") == nullptr
            || FCStringAnsi::Strstr(Symbol, "GetFlagsForType") == nullptr)
        {
            continue;
        }
        FARPROC Resolved = GetProcAddress(Provider, Symbol);
        void* Original = reinterpret_cast<void*>(Resolved);
        void* Replacement = FindReplacement(Original);
        if (Replacement == nullptr)
        {
            continue;
        }
        void** Slot = reinterpret_cast<void**>(Base + SlotRva);
        const uintptr_t Current = reinterpret_cast<uintptr_t>(*Slot);
        const uintptr_t ImageStart = reinterpret_cast<uintptr_t>(Base);
        const bool bUnboundLazySlot = bDelayImport
            && Current >= ImageStart && Current < ImageStart + ImageSize;
        Changed += ApplySlot(ImageName, Slot, Original, Replacement,
            bUnboundLazySlot) ? 1 : 0;
    }
    return Changed;
}

int32 RebindWindowsImage(uint8* Base, const FString& ImageName)
{
    const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
    if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0)
    {
        return 0;
    }
    const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE
        || Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        return 0;
    }

    const uint32 ImageSize = Nt->OptionalHeader.SizeOfImage;
    int32 Changed = 0;
    const auto& Imports = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (Imports.VirtualAddress != 0
        && InImage(Imports.VirtualAddress, Imports.Size, ImageSize))
    {
        for (uint32 Offset = 0;
             Offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= Imports.Size;
             Offset += sizeof(IMAGE_IMPORT_DESCRIPTOR))
        {
            const auto* Import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                Base + Imports.VirtualAddress + Offset);
            if (Import->Name == 0)
            {
                break;
            }
            if (!InImage(Import->Name, 1, ImageSize))
            {
                continue;
            }
            const ANSICHAR* ProviderName = reinterpret_cast<const ANSICHAR*>(
                Base + Import->Name);
            if (FCStringAnsi::Stricmp(ProviderName,
                "UnrealEditor-CoreUObject.dll") != 0)
            {
                continue;
            }
            HMODULE Provider = GetModuleHandleA(ProviderName);
            if (Provider == nullptr || Import->FirstThunk == 0)
            {
                continue;
            }
            if (Import->OriginalFirstThunk == 0)
            {
                for (uint64 Index = 0; ; ++Index)
                {
                    const uint64 SlotRva = Import->FirstThunk
                        + Index * sizeof(IMAGE_THUNK_DATA64);
                    if (!InImage(SlotRva, sizeof(void*), ImageSize))
                    {
                        break;
                    }
                    auto* Slot = reinterpret_cast<IMAGE_THUNK_DATA64*>(
                        Base + SlotRva);
                    void* Original = reinterpret_cast<void*>(Slot->u1.Function);
                    void* Replacement = FindReplacement(Original);
                    if (Replacement != nullptr)
                    {
                        Changed += ApplySlot(ImageName,
                            reinterpret_cast<void**>(Slot), Original,
                            Replacement, false) ? 1 : 0;
                    }
                }
                continue;
            }
            Changed += ScanWindowsThunks(Base, ImageName,
                Import->OriginalFirstThunk, Import->FirstThunk,
                ImageSize, false, true, Provider);
        }
    }

    const auto& DelayImports = Nt->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (DelayImports.VirtualAddress != 0
        && InImage(DelayImports.VirtualAddress, DelayImports.Size, ImageSize))
    {
        for (uint32 Offset = 0;
             Offset + sizeof(FDelayImportDescriptor) <= DelayImports.Size;
             Offset += sizeof(FDelayImportDescriptor))
        {
            const auto* Delay = reinterpret_cast<const FDelayImportDescriptor*>(
                Base + DelayImports.VirtualAddress + Offset);
            if (Delay->DllName == 0)
            {
                break;
            }
            const bool bRva = (Delay->Attributes & 1) != 0;
            const uint64 ProviderRva = DelayValueToRva(
                Delay->DllName, bRva, Base, ImageSize);
            const uint64 NameTableRva = DelayValueToRva(
                Delay->ImportNameTable, bRva, Base, ImageSize);
            const uint64 AddressTableRva = DelayValueToRva(
                Delay->ImportAddressTable, bRva, Base, ImageSize);
            if (ProviderRva == UINT64_MAX || NameTableRva == UINT64_MAX
                || AddressTableRva == UINT64_MAX
                || !InImage(ProviderRva, 1, ImageSize))
            {
                continue;
            }
            const ANSICHAR* ProviderName = reinterpret_cast<const ANSICHAR*>(
                Base + ProviderRva);
            if (FCStringAnsi::Stricmp(ProviderName,
                "UnrealEditor-CoreUObject.dll") != 0)
            {
                continue;
            }
            HMODULE Provider = GetModuleHandleA(ProviderName);
            if (Provider == nullptr
                || !InImage(NameTableRva, sizeof(IMAGE_THUNK_DATA64), ImageSize)
                || !InImage(AddressTableRva, sizeof(IMAGE_THUNK_DATA64), ImageSize))
            {
                continue;
            }
            Changed += ScanWindowsThunks(Base, ImageName,
                NameTableRva, AddressTableRva,
                ImageSize, true, bRva, Provider);
        }
    }
    return Changed;
}

void ScanLoadedWindowsImages(const FString* OnlyImageName = nullptr)
{
    if (OnlyImageName != nullptr)
    {
        // A module notification already identifies its image; avoid rebuilding
        // a process-wide Toolhelp snapshot for each newly loaded editor module.
        const FString DllName = *OnlyImageName + TEXT(".dll");
        HMODULE Retained = nullptr;
        if (GetModuleHandleExW(0, *DllName, &Retained))
        {
            ON_SCOPE_EXIT { FreeLibrary(Retained); };
            RebindWindowsImage(reinterpret_cast<uint8*>(Retained), *OnlyImageName);
        }
        return;
    }
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
        const FString ImageName = NormalizeImageName(Entry.szModule);
        if (OnlyImageName != nullptr && ImageName != *OnlyImageName)
        {
            continue;
        }
        if (OnlyImageName == nullptr
            && !ImageName.StartsWith(TEXT("UnrealEditor-")))
        {
            continue;
        }
        HMODULE Retained = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(Entry.modBaseAddr), &Retained))
        {
            continue;
        }
        ON_SCOPE_EXIT { FreeLibrary(Retained); };
        RebindWindowsImage(Entry.modBaseAddr, ImageName);
    }
}
#else
bool InLinkedit(const segment_command_64& Linkedit, uint64 Offset, uint64 Size)
{
    return Offset >= Linkedit.fileoff
        && Offset - Linkedit.fileoff <= Linkedit.filesize
        && Size <= Linkedit.filesize - (Offset - Linkedit.fileoff);
}

int32 RebindMachImage(const mach_header_64* Header, intptr_t Slide,
    const FString& ImageName)
{
    if (Header == nullptr || Header->magic != MH_MAGIC_64)
    {
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
            return 0;
        }
        const auto* Command = reinterpret_cast<const load_command*>(Cursor);
        if (Command->cmdsize < sizeof(load_command)
            || Cursor + Command->cmdsize > End)
        {
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
        || Dysymtab->nindirectsyms == 0
        || !InLinkedit(*Linkedit, Symtab->symoff,
            uint64(Symtab->nsyms) * sizeof(nlist_64))
        || !InLinkedit(*Linkedit, Symtab->stroff, Symtab->strsize)
        || !InLinkedit(*Linkedit, Dysymtab->indirectsymoff,
            uint64(Dysymtab->nindirectsyms) * sizeof(uint32)))
    {
        return 0;
    }

    const uintptr_t LinkeditBase = Slide + Linkedit->vmaddr - Linkedit->fileoff;
    const auto* Symbols = reinterpret_cast<const nlist_64*>(
        LinkeditBase + Symtab->symoff);
    const auto* Strings = reinterpret_cast<const ANSICHAR*>(
        LinkeditBase + Symtab->stroff);
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
            for (uint32 SectionIndex = 0; SectionIndex < Segment->nsects;
                 ++SectionIndex)
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
                    if (FCStringAnsi::Strstr(Symbol, "FCoreRedirects") == nullptr
                        || FCStringAnsi::Strstr(Symbol, "GetFlagsForType") == nullptr)
                    {
                        continue;
                    }
                    void* Original = *Symbol == '_'
                        ? dlsym(RTLD_DEFAULT, Symbol + 1) : nullptr;
                    void* Replacement = FindReplacement(Original);
                    if (Replacement == nullptr)
                    {
                        continue;
                    }
                    void** Slot = &Slots[SlotIndex];
                    const uintptr_t Current = reinterpret_cast<uintptr_t>(*Slot);
                    const bool bUnboundLazySlot = SectionType == S_LAZY_SYMBOL_POINTERS
                        && Current >= ImageStart && Current < ImageEnd;
                    Changed += ApplySlot(ImageName, Slot, Original, Replacement,
                        bUnboundLazySlot) ? 1 : 0;
                }
            }
        }
        Cursor += Command->cmdsize;
    }
    return Changed;
}

void ScanLoadedMachImages(const FString* OnlyImageName = nullptr)
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
        if (OnlyImageName != nullptr && ImageName != *OnlyImageName)
        {
            continue;
        }
        if (OnlyImageName == nullptr
            && !ImageName.StartsWith(TEXT("UnrealEditor-")))
        {
            continue;
        }
        RebindMachImage(reinterpret_cast<const mach_header_64*>(Header),
            _dyld_get_image_vmaddr_slide(Index), ImageName);
    }
}
#endif

void ScanLoadedImages(const FString* OnlyImageName = nullptr)
{
#if PLATFORM_WINDOWS
    ScanLoadedWindowsImages(OnlyImageName);
#else
    ScanLoadedMachImages(OnlyImageName);
#endif
}

void RestoreAllImports()
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
        It.RemoveCurrent();
    }
}

void OnModulesChanged(FName ModuleName, EModuleChangeReason Reason)
{
    const FString ImageName = TEXT("UnrealEditor-") + ModuleName.ToString();
    if (Reason == EModuleChangeReason::ModuleLoaded)
    {
        FScopeLock Lock(&ImportLock);
        ScanLoadedImages(&ImageName);
    }
    else if (Reason == EModuleChangeReason::ModuleUnloaded)
    {
        FScopeLock Lock(&ImportLock);
        for (auto It = InstalledImports.CreateIterator(); It; ++It)
        {
            if (It.Value().ImageName == ImageName)
            {
                // The image is unloading; never dereference its import slots.
                It.RemoveCurrent();
            }
        }
    }
}

bool ResolveTargetAndCallerRange()
{
#if PLATFORM_WINDOWS
    CoreRedirectNameFunctionAddress = reinterpret_cast<void*>(
        static_cast<FGetFlagsForTypeName>(
            &FCoreRedirects::GetFlagsForTypeName));
    CoreRedirectClassFunctionAddress = reinterpret_cast<void*>(
        static_cast<FGetFlagsForTypeClass>(
            &FCoreRedirects::GetFlagsForTypeClass));
    HMODULE Module = nullptr;
    if (CoreRedirectNameFunctionAddress == nullptr
        || CoreRedirectClassFunctionAddress == nullptr
        || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(CoreRedirectNameFunctionAddress), &Module))
    {
        return false;
    }
    HMODULE ClassModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(CoreRedirectClassFunctionAddress), &ClassModule)
        || ClassModule != Module)
    {
        return false;
    }
    char Path[MAX_PATH] = {};
    if (GetModuleFileNameA(Module, Path, UE_ARRAY_COUNT(Path)) == 0
        || FCStringAnsi::Strstr(Path, "UnrealEditor-CoreUObject") == nullptr)
    {
        return false;
    }
    const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Module);
    if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0)
    {
        return false;
    }
    const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const uint8*>(Module) + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE
        || Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        return false;
    }
    CoreUObjectTextStart = reinterpret_cast<uintptr_t>(Module);
    CoreUObjectTextEnd = CoreUObjectTextStart + Nt->OptionalHeader.SizeOfImage;
#else
    CoreRedirectNameFunctionAddress = dlsym(
        RTLD_DEFAULT, CoreRedirectNameSymbol);
    CoreRedirectClassFunctionAddress = dlsym(
        RTLD_DEFAULT, CoreRedirectClassSymbol);
    Dl_info Info{};
    Dl_info ClassInfo{};
    if (CoreRedirectNameFunctionAddress == nullptr
        || CoreRedirectClassFunctionAddress == nullptr
        || dladdr(CoreRedirectNameFunctionAddress, &Info) == 0
        || dladdr(CoreRedirectClassFunctionAddress, &ClassInfo) == 0
        || ClassInfo.dli_fbase != Info.dli_fbase
        || Info.dli_fname == nullptr
        || FCStringAnsi::Strstr(Info.dli_fname,
            "UnrealEditor-CoreUObject") == nullptr
        || Info.dli_fbase == nullptr)
    {
        return false;
    }
    const mach_header_64* Header = nullptr;
    intptr_t Slide = 0;
    for (uint32 Index = 0; Index < _dyld_image_count(); ++Index)
    {
        const mach_header* Candidate = _dyld_get_image_header(Index);
        if (Candidate == Info.dli_fbase && Candidate->magic == MH_MAGIC_64)
        {
            Header = reinterpret_cast<const mach_header_64*>(Candidate);
            Slide = _dyld_get_image_vmaddr_slide(Index);
            break;
        }
    }
    if (Header == nullptr)
    {
        return false;
    }
    const uint8* Cursor = reinterpret_cast<const uint8*>(Header + 1);
    for (uint32 Index = 0; Index < Header->ncmds; ++Index)
    {
        const auto* Command = reinterpret_cast<const load_command*>(Cursor);
        if (Command->cmd == LC_SEGMENT_64
            && Command->cmdsize >= sizeof(segment_command_64))
        {
            const auto* Segment = reinterpret_cast<const segment_command_64*>(Cursor);
            if (FCStringAnsi::Strncmp(Segment->segname, SEG_TEXT, 16) == 0)
            {
                CoreUObjectTextStart = Slide + Segment->vmaddr;
                CoreUObjectTextEnd = CoreUObjectTextStart + Segment->vmsize;
                break;
            }
        }
        Cursor += Command->cmdsize;
    }
    if (CoreUObjectTextStart == 0 || CoreUObjectTextEnd <= CoreUObjectTextStart)
    {
        return false;
    }
#endif
    OriginalGetFlagsForTypeName = reinterpret_cast<FGetFlagsForTypeName>(
        CoreRedirectNameFunctionAddress);
    OriginalGetFlagsForTypeClass = reinterpret_cast<FGetFlagsForTypeClass>(
        CoreRedirectClassFunctionAddress);
    return true;
}

void InitializeCoreRedirectClassificationHookInternal()
{
    if (bCoreRedirectClassificationInitialized)
    {
        return;
    }
    bCoreRedirectClassificationInitialized = true;
    if (!ResolveTargetAndCallerRange())
    {
        WarnOnce(TEXT("the CoreUObject classifier export or image range could not be resolved"));
        return;
    }

    const FName ASClassName(TEXT("ASClass"));
    if (OriginalGetFlagsForTypeName(
        FName(TEXT("/Script/Angelscript")), ASClassName)
        == ECoreRedirectFlags::Type_Class)
    {
        return;
    }

    ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddStatic(
        &OnModulesChanged);
    {
        FScopeLock Lock(&ImportLock);
        ScanLoadedImages();
    }

    if (!InstallEditorInlineHook(CoreRedirectEntryHook,
        CoreRedirectNameFunctionAddress,
        reinterpret_cast<void*>(&CoreUObjectGetFlagsForTypeName),
        TEXT("FCoreRedirects::GetFlagsForTypeName")))
    {
        WarnOnce(TEXT("the CoreUObject caller-scoped entry hook could not be installed"));
    }
    else
    {
        OriginalGetFlagsForTypeName = reinterpret_cast<FGetFlagsForTypeName>(
            CoreRedirectEntryHook.Original);
    }

    UE_LOG(LogSoloASBridgeCoreRedirectClassification, Display,
        TEXT("CoreRedirect ASClass classification hook active: imports=%d coreUObjectEntry=%d"),
        InstalledImports.Num(), CoreRedirectEntryHook.Original != nullptr ? 1 : 0);
}

void ShutdownCoreRedirectClassificationHookInternal()
{
    if (ModulesChangedHandle.IsValid())
    {
        FModuleManager::Get().OnModulesChanged().Remove(ModulesChangedHandle);
        ModulesChangedHandle.Reset();
    }
    RestoreAllImports();
    if (CoreRedirectEntryHook.Original != nullptr
        && RemoveEditorInlineHook(CoreRedirectEntryHook,
            reinterpret_cast<void*>(&CoreUObjectGetFlagsForTypeName),
            TEXT("FCoreRedirects::GetFlagsForTypeName")))
    {
        OriginalGetFlagsForTypeName = reinterpret_cast<FGetFlagsForTypeName>(
            CoreRedirectNameFunctionAddress);
    }
    bCoreRedirectClassificationInitialized = false;
}

}

namespace SoloASBridge::Editor
{
void InitializeCoreRedirectClassificationHook()
{
    InitializeCoreRedirectClassificationHookInternal();
}

void ShutdownCoreRedirectClassificationHook()
{
    ShutdownCoreRedirectClassificationHookInternal();
}
}

#endif
