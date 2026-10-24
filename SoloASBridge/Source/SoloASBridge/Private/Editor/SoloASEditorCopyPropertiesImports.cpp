// Copyright Cuviss Studio. All Rights Reserved.
#include "CoreMinimal.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "Editor/SoloASCopyProperties.h"
#include "Editor/SoloASEditorEntryHook.h"
#include "Editor/SoloASEditorFieldPathReload.h"
#include "UObject/FieldPath.h"
#include "Engine/Blueprint.h"
#include "Editor/SoloASEditorObjectImportProperties.h"
#include "Editor/SoloASRetiredLoadingExportRepair.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "UObject/UObjectHash.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <psapi.h>
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

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS
#include <cstdint>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSoloASEditorImports, Log, All);
static TAutoConsoleVariable<int32> CVarEditorCopyPropertiesImports(
    TEXT("soloas.EditorCopyPropertiesImports"), 1,
    TEXT("Redirect editor modular CPFUO imports for script classes. 0 restores Engine imports."));
static TAutoConsoleVariable<int32> CVarEditorObjectImportProperties(
    TEXT("soloas.EditorObjectImportProperties"), 1,
    TEXT("Redirect non-Engine editor ImportObjectProperties imports for script objects. 0 restores UnrealEd imports."));

static TAutoConsoleVariable<int32> CVarEditorScriptNativeBoundary(
    TEXT("soloas.EditorScriptNativeBoundary"), 1,
    TEXT("Redirect editor Blueprint native-boundary imports for script classes. 0 restores UnrealEd imports."));

static TAutoConsoleVariable<int32> CVarEditorFieldPathReloadCache(
    TEXT("soloas.EditorFieldPathReloadCache"), 1,
    TEXT("Invalidate changed FieldPath owners inside editor FullReload replacement scopes."));

// Make the friend declaration visible for the exact native function address.
COREUOBJECT_API FArchive& operator<<(FArchive&, FFieldPath&);

namespace
{
using FFieldPathSerialize = FArchive& (*)(FArchive&, FFieldPath&);
FFieldPathSerialize EngineFieldPathSerialize = static_cast<FFieldPathSerialize>(&operator<<);
thread_local uint32 FieldPathReloadDepth = 0;
bool bFieldPathResolved = false;
FSoloASEditorEntryHook FieldPathEntry;
std::atomic<bool> FieldPathPassThrough{true};

struct FFieldPathCacheAccess : FFieldPath
{
    // Inherited member pointers operate on FFieldPath itself; no layout casts.
    static auto OwnerMember() { return &FFieldPathCacheAccess::ResolvedOwner; }
    static auto ClearMember() { return &FFieldPathCacheAccess::ClearCachedField; }
};

FArchive& SerializeFieldPath(FArchive& Ar, FFieldPath& Path)
{
    // The export itself is redirected, so only its trampoline can call the original.
    const auto Original = reinterpret_cast<FFieldPathSerialize>(FieldPathEntry.Original());
    // Shutdown leaves this entry resident; do not touch CVar or TLS state after it.
    if (FieldPathPassThrough.load(std::memory_order_acquire)
        || !GIsEditor || FieldPathReloadDepth == 0)
        return Original(Ar, Path);
    UStruct* PreviousOwner = (Path.*FFieldPathCacheAccess::OwnerMember()).Get();
    FArchive& Result = Original(Ar, Path);
    if (PreviousOwner != (Path.*FFieldPathCacheAccess::OwnerMember()).Get())
        (Path.*FFieldPathCacheAccess::ClearMember())();
    return Result;
}

using FBoundary = UClass* (*)(UClass*);
using FConstBoundary = const UClass* (*)(const UClass*);
using FSignature = bool (*)(const UFunction*);
FBoundary EngineBoundary = static_cast<FBoundary>(&FBlueprintEditorUtils::FindFirstNativeClass);
FConstBoundary EngineConstBoundary = static_cast<FConstBoundary>(&FBlueprintEditorUtils::FindFirstNativeClass);
FSignature EngineSignature = &FBlueprintEditorUtils::IsNativeSignature;
bool bBoundaryResolved = false;
TSet<const void*> ScannedBoundaryImages;

UClass* Boundary(UClass* Class)
{
    if (CVarEditorScriptNativeBoundary.GetValueOnAnyThread() != 0)
    {
        for (UClass* Cursor = Class; Cursor; Cursor = Cursor->GetSuperClass())
        {
            // Stop at the nearest native boundary, even if a later ancestor is AS.
            if (Cursor->HasAnyClassFlags(CLASS_Native)) break;
            if (SoloASBridge::IsRegisteredScriptClass(Cursor)) return Cursor;
        }
    }
    return EngineBoundary(Class);
}

const UClass* ConstBoundary(const UClass* Class)
{
    if (CVarEditorScriptNativeBoundary.GetValueOnAnyThread() != 0)
    {
        for (const UClass* Cursor = Class; Cursor; Cursor = Cursor->GetSuperClass())
        {
            if (Cursor->HasAnyClassFlags(CLASS_Native)) break;
            if (SoloASBridge::IsRegisteredScriptClass(Cursor)) return Cursor;
        }
    }
    return EngineConstBoundary(Class);
}

bool Signature(const UFunction* Function)
{
    if (CVarEditorScriptNativeBoundary.GetValueOnAnyThread() == 0)
        return EngineSignature(Function);
    UClass* Owner = Function->GetOwnerClass();
    UClass* Native = Boundary(Owner);
    if (Native && SoloASBridge::IsRegisteredScriptClass(Native))
        return Native->FindFunctionByName(Function->GetFName()) != nullptr;
    return EngineSignature(Function);
}

using FCopy = void (*)(UObject*, UObject*, UEngine::FCopyPropertiesForUnrelatedObjectsParams);
using FImportObject = const TCHAR* (*)(FImportObjectParams&);
// This module's import is deliberately never rebound, including during lazy binding.
FCopy EngineCopy = &UEngine::CopyPropertiesForUnrelatedObjects;
FSoloASEditorEntryHook CopyEntry;

#if WITH_DEV_AUTOMATION_TESTS
struct FCallerAddressRange
{
	UPTRINT Start = 0;
	UPTRINT End = 0;

	bool IsValid() const { return Start < End; }
};

struct FCallerRangeSet
{
	FCallerAddressRange Ranges[2];
	int32 NumRanges = 0;
	UPTRINT Partition = 0;

	void Add(const FCallerAddressRange& Range)
	{
		if (Range.IsValid() && NumRanges < UE_ARRAY_COUNT(Ranges))
			Ranges[NumRanges++] = Range;
	}

	void Finalize()
	{
		if (NumRanges == 2 && Ranges[1].Start < Ranges[0].Start)
			Swap(Ranges[0], Ranges[1]);
		if (NumRanges == 2 && Ranges[0].End >= Ranges[1].Start)
		{
			Ranges[0].End = FMath::Max(Ranges[0].End, Ranges[1].End);
			NumRanges = 1;
		}
		if (NumRanges == 2)
			Partition = Ranges[0].End + (Ranges[1].Start - Ranges[0].End) / 2;
	}

	bool Contains(const void* Address) const
	{
		if (NumRanges == 0 || Address == nullptr)
			return false;
		const UPTRINT Value = reinterpret_cast<UPTRINT>(Address);
		const int32 Index = NumRanges == 2 && Value >= Partition ? 1 : 0;
		return Value >= Ranges[Index].Start && Value < Ranges[Index].End;
	}
};

struct FCallerRangeCache
{
	FCallerRangeSet CopyProperties;
	FCallerRangeSet ScriptFunctionSourceNavigation;
	FCallerRangeSet OutlinerScriptSourceLink;
};

struct FTestCallerRegistration
{
	SoloASBridge::Editor::Tests::ECallerRange Range;
	FName ModuleName;
};

TArray<FTestCallerRegistration> TestCallerRegistrations;
std::atomic<const FCallerRangeCache*> ActiveCallerRanges{nullptr};
TArray<TUniquePtr<FCallerRangeCache>> RetainedCallerRanges;
std::atomic<uint32> ActiveCallerRangeReaders{0};
std::atomic<bool> bCallerRangeReclaimPending{false};
FCriticalSection CallerRangeCacheMutex;

FCallerRangeSet* GetCallerRangeSet(FCallerRangeCache& Cache, SoloASBridge::Editor::Tests::ECallerRange Range)
{
	switch (Range)
	{
	case SoloASBridge::Editor::Tests::ECallerRange::CopyProperties:
		return &Cache.CopyProperties;
	case SoloASBridge::Editor::Tests::ECallerRange::ScriptFunctionSourceNavigation:
		return &Cache.ScriptFunctionSourceNavigation;
	case SoloASBridge::Editor::Tests::ECallerRange::OutlinerScriptSourceLink:
		return &Cache.OutlinerScriptSourceLink;
	default:
		return nullptr;
	}
}

const FCallerRangeSet* GetCallerRangeSet(const FCallerRangeCache& Cache, SoloASBridge::Editor::Tests::ECallerRange Range)
{
	switch (Range)
	{
	case SoloASBridge::Editor::Tests::ECallerRange::CopyProperties:
		return &Cache.CopyProperties;
	case SoloASBridge::Editor::Tests::ECallerRange::ScriptFunctionSourceNavigation:
		return &Cache.ScriptFunctionSourceNavigation;
	case SoloASBridge::Editor::Tests::ECallerRange::OutlinerScriptSourceLink:
		return &Cache.OutlinerScriptSourceLink;
	default:
		return nullptr;
	}
}

void RemoveRetiredCallerRangesIfSafe()
{
	if (ActiveCallerRangeReaders.load(std::memory_order_seq_cst) != 0)
		return;

	FScopeLock Lock(&CallerRangeCacheMutex);
	if (ActiveCallerRangeReaders.load(std::memory_order_seq_cst) == 0)
	{
		const FCallerRangeCache* Active = ActiveCallerRanges.load(std::memory_order_seq_cst);
		RetainedCallerRanges.RemoveAll([Active](const TUniquePtr<FCallerRangeCache>& Cache)
		{
			return Cache.Get() != Active;
		});
		bCallerRangeReclaimPending.store(false, std::memory_order_seq_cst);
	}
}

void RefreshCallerRanges(FName ExcludedModule = NAME_None)
{
	FCallerRangeCache Candidate;
	{
		FScopeLock Lock(&CallerRangeCacheMutex);
		for (const FTestCallerRegistration& Registration : TestCallerRegistrations)
		{
			if (Registration.ModuleName == ExcludedModule)
				continue;
			FSoloASEditorModuleImageRange ModuleRange;
			if (!ModuleRange.Update(Registration.ModuleName))
				continue;
			if (FCallerRangeSet* Set = GetCallerRangeSet(Candidate, Registration.Range))
				Set->Add({ModuleRange.Start, ModuleRange.End});
		}
	}
	Candidate.CopyProperties.Finalize();
	Candidate.ScriptFunctionSourceNavigation.Finalize();
	Candidate.OutlinerScriptSourceLink.Finalize();
	FScopeLock Lock(&CallerRangeCacheMutex);
	RetainedCallerRanges.Add(MakeUnique<FCallerRangeCache>(MoveTemp(Candidate)));
	ActiveCallerRanges.store(RetainedCallerRanges.Last().Get(), std::memory_order_seq_cst);
	if (ActiveCallerRangeReaders.load(std::memory_order_seq_cst) == 0)
	{
		const FCallerRangeCache* Active = RetainedCallerRanges.Last().Get();
		RetainedCallerRanges.RemoveAll([Active](const TUniquePtr<FCallerRangeCache>& Cache)
		{
			return Cache.Get() != Active;
		});
		bCallerRangeReclaimPending.store(false, std::memory_order_seq_cst);
	}
	else
	{
		bCallerRangeReclaimPending.store(true, std::memory_order_seq_cst);
	}
}

bool IsTestCallerInRange(SoloASBridge::Editor::Tests::ECallerRange Range, const void* Address)
{
	ActiveCallerRangeReaders.fetch_add(1, std::memory_order_seq_cst);
	const FCallerRangeCache* Cache = ActiveCallerRanges.load(std::memory_order_seq_cst);
	const FCallerRangeSet* Set = Cache != nullptr ? GetCallerRangeSet(*Cache, Range) : nullptr;
	const bool bContains = Set != nullptr && Set->Contains(Address);
	if (ActiveCallerRangeReaders.fetch_sub(1, std::memory_order_seq_cst) == 1
		&& bCallerRangeReclaimPending.load(std::memory_order_seq_cst))
	{
		RemoveRetiredCallerRangesIfSafe();
	}
	return bContains;
}
#endif

void OriginalCopy(UObject* Old, UObject* New, UEngine::FCopyPropertiesForUnrelatedObjectsParams Params)
{
    const FCopy Original = CopyEntry.Original() ? reinterpret_cast<FCopy>(CopyEntry.Original()) : EngineCopy;
    Original(Old, New, Params);
}
template<typename T> constexpr bool HasNativeScriptClassMarker()
{
    return requires(const T& Class) { Class.bIsScriptClass; };
}
FImportObject EngineImportObject = static_cast<FImportObject>(&ImportObjectProperties);
FDelegateHandle ModulesChangedHandle;
SoloASBridge::FEditorObjectImportPredicate ImportObjectPredicate = nullptr;
SoloASBridge::FEditorObjectImportAdapter ImportObjectAdapter = nullptr;
FCriticalSection ImportObjectAdapterLock;
thread_local UObject* CopyOld = nullptr;
thread_local UObject* CopyNew = nullptr;
thread_local bool bInsideImportObject = false;
bool bWarned = false;
TSet<const void*> ScannedImages;
TSet<const void*> ScannedImportImages;
FCriticalSection ImagesLock;

void Warn(const TCHAR* Reason)
{
    if (bWarned) return;
    bWarned = true;
    UE_LOG(LogSoloASEditorImports, Warning,
        TEXT("CPFUO import redirection incomplete: %s. Use the public SoloAS copy wrapper for uncovered direct callers."), Reason);
}

UObject* FindBlueprintCDORoot(UObject* Object)
{
    for (UObject* Current = Object; Current; Current = Current->GetOuter())
    {
        if (Current->HasAnyFlags(RF_ClassDefaultObject)
            && Cast<UBlueprint>(Current->GetClass()->ClassGeneratedBy))
            return Current;
    }
    return nullptr;
}

bool IsCompilerGeneratedDefaultCopy(UObject* Old, UObject* New,
    const UEngine::FCopyPropertiesForUnrelatedObjectsParams& Params)
{
    if (!IsInGameThread() || !Params.bOnlyHandleDirectSubObjects
        || !Params.bReplaceInternalReferenceUponRead || !Params.OptionalReplacementMappings)
        return false;
    UObject* OldCDO = FindBlueprintCDORoot(Old);
    UObject* NewCDO = FindBlueprintCDORoot(New);
    if (!OldCDO || !NewCDO || OldCDO == NewCDO || OldCDO->GetClass() == NewCDO->GetClass()
        || OldCDO->GetClass()->GetSuperClass() != NewCDO->GetClass()->GetSuperClass())
        return false;
    return SoloASBridge::HasRegisteredScriptClassInAncestry(OldCDO->GetClass())
        || SoloASBridge::HasRegisteredScriptClassInAncestry(NewCDO->GetClass());
}

class FScopedCompilerGeneratedDefaultSkips
{
public:
    explicit FScopedCompilerGeneratedDefaultSkips(UObject* Source)
    {
        if (!Source) return;
        TArray<UObject*> Objects;
        Objects.Add(Source);
        GetObjectsWithOuter(Source, Objects, EGetObjectsFlags::IncludeNestedObjects);
        TSet<FProperty*> Seen;
        for (UObject* Object : Objects)
        {
            if (!Object) continue;
            for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
            {
                FProperty* Property = *It;
                if (!Property->HasMetaData(TEXT("BlueprintCompilerGeneratedDefaults"))
                    || Property->HasAnyPropertyFlags(CPF_SkipSerialization) || Seen.Contains(Property))
                    continue;
                Seen.Add(Property);
                Property->SetPropertyFlags(CPF_SkipSerialization);
                ChangedProperties.Add(Property);
            }
        }
    }

    ~FScopedCompilerGeneratedDefaultSkips()
    {
        for (FProperty* Property : ChangedProperties)
            if (Property) Property->ClearPropertyFlags(CPF_SkipSerialization);
    }

private:
    TArray<FProperty*> ChangedProperties;
};

void Copy(UObject* Old, UObject* New, UEngine::FCopyPropertiesForUnrelatedObjectsParams Params)
{
    const bool bScriptClassCopy = Old && New
        && (SoloASBridge::HasRegisteredScriptClassInAncestry(Old->GetClass())
            || SoloASBridge::HasRegisteredScriptClassInAncestry(New->GetClass()));
    const bool bGeneratedDefaultCopy = Old && New && IsCompilerGeneratedDefaultCopy(Old, New, Params);
    if ((CopyOld == Old && CopyNew == New) || CVarEditorCopyPropertiesImports.GetValueOnAnyThread() == 0
        || !Old || !New || (!bScriptClassCopy && !bGeneratedDefaultCopy))
    {
        OriginalCopy(Old, New, Params);
        return;
    }
    TGuardValue<UObject*> OldScope(CopyOld, Old);
    TGuardValue<UObject*> NewScope(CopyNew, New);
    FScopedCompilerGeneratedDefaultSkips GeneratedDefaultSkips(bGeneratedDefaultCopy ? Old : nullptr);
    if (bScriptClassCopy)
        SoloAS::CopyPropertiesForUnrelatedObjectsRepairingLoads(Old, New, Params);
    else
        OriginalCopy(Old, New, Params);
}

void CopyEngineEntry(UObject* Old, UObject* New, UEngine::FCopyPropertiesForUnrelatedObjectsParams Params)
{
    const void* Caller = SoloASBridge::Editor::GetEditorHookCallerAddress();
    bool bAllowedCaller = CopyEntry.IsCallerInTargetModule(Caller);
#if WITH_DEV_AUTOMATION_TESTS
    bAllowedCaller = bAllowedCaller
        || IsTestCallerInRange(SoloASBridge::Editor::Tests::ECallerRange::CopyProperties, Caller);
#endif
    if (!bAllowedCaller)
    {
        OriginalCopy(Old, New, Params);
        return;
    }
    Copy(Old, New, Params);
}

const TCHAR* ImportObject(FImportObjectParams& Params)
{
    if (bInsideImportObject || !IsInGameThread()
        || CVarEditorObjectImportProperties.GetValueOnAnyThread() == 0)
        return EngineImportObject(Params);

    SoloASBridge::FEditorObjectImportPredicate Predicate = nullptr;
    SoloASBridge::FEditorObjectImportAdapter Adapter = nullptr;
    {
        FScopeLock Lock(&ImportObjectAdapterLock);
        Predicate = ImportObjectPredicate;
        Adapter = ImportObjectAdapter;
    }
    if (!Predicate || !Adapter || !Predicate(Params))
        return EngineImportObject(Params);

    bInsideImportObject = true;
    ON_SCOPE_EXIT { bInsideImportObject = false; };
    return Adapter(Params);
}

bool IsProduct(const TCHAR* Image)
{
    const FString Name = FPaths::GetCleanFilename(Image);
    return Name.Contains(TEXT("-SoloAS")) || Name.Contains(TEXT("-Angelscript"));
}

bool HasImportObjectAdapter()
{
	FScopeLock Lock(&ImportObjectAdapterLock);
	return ImportObjectPredicate != nullptr && ImportObjectAdapter != nullptr;
}

bool IsEngineImage(const TCHAR* Image)
{
    if (!Image)
        return false;
    FString EngineDirectory = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
    FString ImagePath = FPaths::ConvertRelativePathToFull(Image);
    FPaths::NormalizeDirectoryName(EngineDirectory);
    FPaths::NormalizeDirectoryName(ImagePath);
    EngineDirectory.ReplaceInline(TEXT("\\"), TEXT("/"));
    ImagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (EngineDirectory.IsEmpty() || ImagePath.IsEmpty())
    {
        Warn(TEXT("EngineDir or module path unavailable; direct object imports left unchanged"));
        return true;
    }
    while (EngineDirectory.EndsWith(TEXT("/")) || EngineDirectory.EndsWith(TEXT("\\")))
        EngineDirectory.LeftChopInline(1);
    EngineDirectory += TEXT("/");
    return ImagePath.StartsWith(EngineDirectory, ESearchCase::IgnoreCase);
}

bool WritePointer(void** Slot, void* Value)
{
#if PLATFORM_WINDOWS
    void* Original = *Slot;
    DWORD Previous = 0;
    if (!VirtualProtect(Slot, sizeof(void*), PAGE_READWRITE, &Previous)) return false;
    InterlockedExchangePointer(Slot, Value);
    DWORD Ignored = 0;
    if (!VirtualProtect(Slot, sizeof(void*), Previous, &Ignored))
    {
        InterlockedExchangePointer(Slot, Original);
        VirtualProtect(Slot, sizeof(void*), Previous, &Ignored);
        Warn(TEXT("could not restore import page protection; pointer rolled back"));
        return false;
    }
#else
    mach_vm_address_t Address = reinterpret_cast<mach_vm_address_t>(Slot);
    mach_vm_size_t Size = 0;
    vm_region_basic_info_data_64_t Info = {};
    mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t Object = MACH_PORT_NULL;
    const kern_return_t Query = mach_vm_region(mach_task_self(), &Address, &Size,
        VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&Info), &Count, &Object);
    if (Object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), Object);
    if (Query != KERN_SUCCESS || reinterpret_cast<mach_vm_address_t>(Slot) < Address
        || reinterpret_cast<mach_vm_address_t>(Slot) + sizeof(void*) > Address + Size) return false;
    const mach_vm_address_t Page = reinterpret_cast<mach_vm_address_t>(Slot) & ~(static_cast<mach_vm_address_t>(vm_page_size) - 1);
    if (mach_vm_protect(mach_task_self(), Page, vm_page_size, false,
        Info.protection | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) return false;
    void* Original = __atomic_load_n(Slot, __ATOMIC_ACQUIRE);
    __atomic_store_n(Slot, Value, __ATOMIC_RELEASE);
    if (mach_vm_protect(mach_task_self(), Page, vm_page_size, false, Info.protection) != KERN_SUCCESS)
    {
        __atomic_store_n(Slot, Original, __ATOMIC_RELEASE);
        mach_vm_protect(mach_task_self(), Page, vm_page_size, false, Info.protection);
        Warn(TEXT("could not restore symbol-pointer page protection; pointer rolled back"));
        return false;
    }
#endif
    return true;
}

#if PLATFORM_WINDOWS
constexpr const char* FieldPathSymbol = "??6@YAAEAVFArchive@@AEAV0@AEAUFFieldPath@@@Z";
constexpr const char* BoundarySymbol = "?FindFirstNativeClass@FBlueprintEditorUtils@@SAPEAVUClass@@PEAV2@@Z";
constexpr const char* ConstBoundarySymbol = "?FindFirstNativeClass@FBlueprintEditorUtils@@SAPEBVUClass@@PEBV2@@Z";
constexpr const char* SignatureSymbol = "?IsNativeSignature@FBlueprintEditorUtils@@SA_NPEBVUFunction@@@Z";
constexpr const char* CopySymbol = "?CopyPropertiesForUnrelatedObjects@UEngine@@SAXPEAVUObject@@0UFCopyPropertiesForUnrelatedObjectsParams@1@@Z";
constexpr const char* ImportObjectPropertiesSymbol = "?ImportObjectProperties@@YAPEB_WAEAUFImportObjectParams@@@Z";

int32 RebindImage(uint8* Base, bool bRestore, const char* Symbol, void* Native, void* Redirect)
{
    const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
    if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0) return 0;
    const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
    if (Nt->Signature != IMAGE_NT_SIGNATURE || Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
    const uint32 ImageSize = Nt->OptionalHeader.SizeOfImage;
    auto InImage = [ImageSize](uint64 Rva, uint64 Bytes) { return Rva < ImageSize && Bytes <= ImageSize - Rva; };
    int32 Changed = 0;
    auto RebindThunks = [&](uint32 Names, uint32 Slots, bool bDelay)
    {
        if (!Names || !Slots) { Warn(TEXT("editor import names unavailable")); return; }
        for (uint64 Index = 0; ; ++Index)
        {
            const uint64 NameRva = Names + Index * sizeof(IMAGE_THUNK_DATA64);
            const uint64 SlotRva = Slots + Index * sizeof(IMAGE_THUNK_DATA64);
            if (!InImage(NameRva, sizeof(IMAGE_THUNK_DATA64)) || !InImage(SlotRva, sizeof(void*))) break;
            const uint64 Name = reinterpret_cast<const IMAGE_THUNK_DATA64*>(Base + NameRva)->u1.AddressOfData;
            if (!Name) break;
            const int32 SymbolLength = FCStringAnsi::Strlen(Symbol) + 1;
            if (IMAGE_SNAP_BY_ORDINAL64(Name) || !InImage(Name, sizeof(WORD) + uint64(SymbolLength))) continue;
            const char* ImportedSymbol = reinterpret_cast<const char*>(Base + Name + sizeof(WORD));
            if (FCStringAnsi::Strncmp(ImportedSymbol, Symbol, SymbolLength) != 0) continue;
            void** Slot = reinterpret_cast<void**>(Base + SlotRva);
            void* Expected = bRestore ? Redirect : Native;
            void* Value = bRestore ? Native : Redirect;
            if (*Slot == Value) continue;
            // A not-yet-bound delay slot points to its own image's helper thunk.
            const uintptr_t Address = reinterpret_cast<uintptr_t>(*Slot);
            const bool bUnboundDelay = bDelay && !bRestore && Address >= reinterpret_cast<uintptr_t>(Base)
                && Address - reinterpret_cast<uintptr_t>(Base) < ImageSize;
            if (*Slot != Expected && !bUnboundDelay) { Warn(TEXT("editor import already owned by another redirector")); continue; }
            if (WritePointer(Slot, Value)) ++Changed;
            else Warn(TEXT("IAT write failed"));
        }
    };
    const auto& Directory = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (Directory.VirtualAddress && InImage(Directory.VirtualAddress, Directory.Size))
    {
        for (uint32 Offset = 0; Offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= Directory.Size; Offset += sizeof(IMAGE_IMPORT_DESCRIPTOR))
        {
            const auto* Import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(Base + Directory.VirtualAddress + Offset);
            if (!Import->Name) break;
            if (Import->OriginalFirstThunk) RebindThunks(Import->OriginalFirstThunk, Import->FirstThunk, false);
        }
    }
    // PE delay descriptors store RVAs on current 64-bit editor targets.
    struct FDelayImport { uint32 Attributes, Name, Module, Iat, Int, BoundIat, UnloadIat, Timestamp; };
    const auto& Delay = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (Delay.VirtualAddress && InImage(Delay.VirtualAddress, Delay.Size))
    {
        for (uint32 Offset = 0; Offset + sizeof(FDelayImport) <= Delay.Size; Offset += sizeof(FDelayImport))
        {
            const auto* Import = reinterpret_cast<const FDelayImport*>(Base + Delay.VirtualAddress + Offset);
            if (!Import->Name) break;
            if (!(Import->Attributes & 1)) { Warn(TEXT("legacy delay-import VA format unsupported")); continue; }
            RebindThunks(Import->Int, Import->Iat, true);
        }
    }
    return Changed;
}

void Scan(bool bRestore, int32 Kind)
{
    FScopeLock Lock(&ImagesLock);
    TSet<const void*>* ImageSets[] = {
        &ScannedImages, &ScannedImportImages, &ScannedBoundaryImages};
    TArray<int32, TInlineAllocator<3>> Kinds;
    if (Kind >= 0)
        Kinds.Add(Kind);
    else
    {
        if (bBoundaryResolved && CVarEditorScriptNativeBoundary.GetValueOnAnyThread() != 0) Kinds.Add(2);
        if (CVarEditorCopyPropertiesImports.GetValueOnAnyThread() != 0) Kinds.Add(0);
        if (CVarEditorObjectImportProperties.GetValueOnAnyThread() != 0 && HasImportObjectAdapter()) Kinds.Add(1);
    }
    if (Kinds.IsEmpty()) return;
    // Enumerate once per notification, retaining dependency-loaded DLL coverage.
    TArray<HMODULE> Modules;
    Modules.SetNumUninitialized(256);
    DWORD BytesNeeded = 0;
    for (;;)
    {
        const DWORD BufferBytes = Modules.Num() * sizeof(HMODULE);
        if (!K32EnumProcessModules(GetCurrentProcess(),
            Modules.GetData(), BufferBytes, &BytesNeeded))
        {
            Warn(TEXT("module enumeration failed"));
            return;
        }
        if (BytesNeeded <= BufferBytes) break;
        Modules.SetNumUninitialized(BytesNeeded / sizeof(HMODULE));
    }
    Modules.SetNum(BytesNeeded / sizeof(HMODULE));
    for (HMODULE Module : Modules)
    {
        if (!bRestore && !Kinds.ContainsByPredicate([&](int32 ScanKind)
            { return !ImageSets[ScanKind]->Contains(Module); })) continue;
        HMODULE Retained = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(Module), &Retained)) continue;
        ON_SCOPE_EXIT { FreeLibrary(Retained); };
        WCHAR ImagePath[MAX_PATH] = {};
        if (!GetModuleFileNameW(Retained, ImagePath, UE_ARRAY_COUNT(ImagePath))) continue;
        const FString ImageName = FPaths::GetCleanFilename(ImagePath);
        uint8* ImageBase = reinterpret_cast<uint8*>(Retained);
        for (const int32 ScanKind : Kinds)
        {
            const bool bObjectImport = ScanKind == 1;
            TSet<const void*>& Scanned = *ImageSets[ScanKind];
            if (!bRestore && Scanned.Contains(ImageBase)) continue;
            // Eligibility is fixed for the lifetime of a loaded image, including exclusions.
            if (!bRestore) Scanned.Add(ImageBase);
            if (IsProduct(*ImageName)
                || (bObjectImport && IsEngineImage(ImagePath))) continue;
            int32 Changed = 0;
            if (ScanKind != 2) Changed = bObjectImport
                ? RebindImage(ImageBase, bRestore, ImportObjectPropertiesSymbol,
                    reinterpret_cast<void*>(EngineImportObject), reinterpret_cast<void*>(&ImportObject))
                : RebindImage(ImageBase, bRestore, CopySymbol,
                    reinterpret_cast<void*>(EngineCopy), reinterpret_cast<void*>(&Copy));
            if (ScanKind == 2)
            {
                Changed += RebindImage(ImageBase, bRestore, BoundarySymbol, reinterpret_cast<void*>(EngineBoundary), reinterpret_cast<void*>(&Boundary));
                Changed += RebindImage(
                    ImageBase, bRestore, ConstBoundarySymbol,
                    reinterpret_cast<void*>(EngineConstBoundary), reinterpret_cast<void*>(&ConstBoundary));
                Changed += RebindImage(ImageBase, bRestore, SignatureSymbol, reinterpret_cast<void*>(EngineSignature), reinterpret_cast<void*>(&Signature));
            }
            if (Changed)
                UE_LOG(LogSoloASEditorImports, Display, TEXT("%s imports module=%s slots=%d restored=%d"),
                    ScanKind == 2 ? TEXT("ScriptNativeBoundary")
                    : (bObjectImport ? TEXT("ImportObjectProperties") : TEXT("CPFUO")),
                    *ImageName, Changed, bRestore);
        }
    }
}
#else
// Mach-O string tables add one underscore to the Itanium C++ mangled name.
constexpr const char* FieldPathSymbol = "__ZlsR8FArchiveR10FFieldPath";
constexpr const char* BoundarySymbol = "__ZN21FBlueprintEditorUtils20FindFirstNativeClassEP6UClass";
constexpr const char* ConstBoundarySymbol = "__ZN21FBlueprintEditorUtils20FindFirstNativeClassEPK6UClass";
constexpr const char* SignatureSymbol = "__ZN21FBlueprintEditorUtils17IsNativeSignatureEPK9UFunction";
constexpr const char* CopySymbol = "__ZN7UEngine33CopyPropertiesForUnrelatedObjectsEP7UObjectS1_NS_40FCopyPropertiesForUnrelatedObjectsParamsE";
constexpr const char* ImportObjectPropertiesSymbol = "__Z22ImportObjectPropertiesR19FImportObjectParams";

int32 RebindImage(const mach_header_64* Header, intptr_t Slide, bool bRestore,
	const char* Symbol, void* Native, void* Redirect)
{
    if (!Header || Header->magic != MH_MAGIC_64) return 0;
    const segment_command_64* Linkedit = nullptr;
    const symtab_command* Symtab = nullptr;
    const dysymtab_command* Dysymtab = nullptr;
    const uint8* Begin = reinterpret_cast<const uint8*>(Header + 1);
    const uint8* End = Begin + Header->sizeofcmds;
    const uint8* Cursor = Begin;
    for (uint32 I = 0; I < Header->ncmds; ++I)
    {
        if (Cursor + sizeof(load_command) > End) return 0;
        const auto* Command = reinterpret_cast<const load_command*>(Cursor);
        if (Command->cmdsize < sizeof(load_command) || Cursor + Command->cmdsize > End) return 0;
        if (Command->cmd == LC_SEGMENT_64 && Command->cmdsize >= sizeof(segment_command_64))
        {
            const auto* Segment = reinterpret_cast<const segment_command_64*>(Cursor);
            if (FCStringAnsi::Strncmp(Segment->segname, SEG_LINKEDIT, 16) == 0) Linkedit = Segment;
        }
        else if (Command->cmd == LC_SYMTAB && Command->cmdsize >= sizeof(symtab_command)) Symtab = reinterpret_cast<const symtab_command*>(Cursor);
        else if (Command->cmd == LC_DYSYMTAB && Command->cmdsize >= sizeof(dysymtab_command)) Dysymtab = reinterpret_cast<const dysymtab_command*>(Cursor);
        Cursor += Command->cmdsize;
    }
    if (!Linkedit || !Symtab || !Dysymtab || !Dysymtab->nindirectsyms) return 0;
    auto InLinkedit = [Linkedit](uint64 Offset, uint64 Size)
    { return Offset >= Linkedit->fileoff && Offset - Linkedit->fileoff <= Linkedit->filesize && Size <= Linkedit->filesize - (Offset - Linkedit->fileoff); };
    if (!InLinkedit(Symtab->symoff, uint64(Symtab->nsyms) * sizeof(nlist_64))
        || !InLinkedit(Symtab->stroff, Symtab->strsize)
        || !InLinkedit(Dysymtab->indirectsymoff, uint64(Dysymtab->nindirectsyms) * sizeof(uint32))) { Warn(TEXT("invalid Mach-O symbol tables")); return 0; }
    const uintptr_t LinkBase = Slide + Linkedit->vmaddr - Linkedit->fileoff;
    const auto* Symbols = reinterpret_cast<const nlist_64*>(LinkBase + Symtab->symoff);
    const auto* Strings = reinterpret_cast<const char*>(LinkBase + Symtab->stroff);
    const auto* Indirect = reinterpret_cast<const uint32*>(LinkBase + Dysymtab->indirectsymoff);
    int32 Changed = 0;
    Cursor = Begin;
    for (uint32 I = 0; I < Header->ncmds; ++I)
    {
        const auto* Command = reinterpret_cast<const load_command*>(Cursor);
        if (Command->cmd == LC_SEGMENT_64 && Command->cmdsize >= sizeof(segment_command_64))
        {
            const auto* Segment = reinterpret_cast<const segment_command_64*>(Cursor);
            if (Segment->nsects > (Command->cmdsize - sizeof(segment_command_64)) / sizeof(section_64)) return Changed;
            const auto* Sections = reinterpret_cast<const section_64*>(Segment + 1);
            for (uint32 J = 0; J < Segment->nsects; ++J)
            {
                const auto& Section = Sections[J];
                const uint32 Type = Section.flags & SECTION_TYPE;
                if (Type != S_NON_LAZY_SYMBOL_POINTERS && Type != S_LAZY_SYMBOL_POINTERS) continue;
                const uint64 Slots = Section.size / sizeof(void*);
                if (Section.reserved1 > Dysymtab->nindirectsyms || Slots > Dysymtab->nindirectsyms - Section.reserved1) continue;
                auto** Pointers = reinterpret_cast<void**>(Slide + Section.addr);
                for (uint64 K = 0; K < Slots; ++K)
                {
                    const uint32 Index = Indirect[Section.reserved1 + K];
                    if ((Index & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) || Index >= Symtab->nsyms) continue;
                    const uint32 StringIndex = Symbols[Index].n_un.n_strx;
                    const uint32 Length = FCStringAnsi::Strlen(Symbol) + 1;
                    if (StringIndex >= Symtab->strsize || Length > Symtab->strsize - StringIndex
                        || FCStringAnsi::Strncmp(Strings + StringIndex, Symbol, Length) != 0) continue;
                    void* Value = bRestore ? Native : Redirect;
                    if (Pointers[K] == Value || (bRestore && Pointers[K] != Redirect)) continue;
                    if (!bRestore && Type != S_LAZY_SYMBOL_POINTERS && Pointers[K] != Native)
                    { Warn(TEXT("editor symbol pointer already owned by another redirector")); continue; }
                    if (WritePointer(&Pointers[K], Value)) ++Changed;
                    else Warn(TEXT("Mach-O symbol-pointer write failed"));
                }
            }
        }
        Cursor += Command->cmdsize;
    }
    return Changed;
}

void Scan(bool bRestore, int32 Kind)
{
    FScopeLock Lock(&ImagesLock);
    const bool bObjectImport = Kind == 1;
    TSet<const void*>& Scanned = Kind == 2 ? ScannedBoundaryImages : (bObjectImport ? ScannedImportImages : ScannedImages);
    for (uint32 I = 0; I < _dyld_image_count(); ++I)
    {
        const char* Name = _dyld_get_image_name(I);
        const mach_header* Header = _dyld_get_image_header(I);
        if (!Header || Header->filetype != MH_DYLIB) continue;
        const FString ImageName = Name ? FString(UTF8_TO_TCHAR(Name)) : FString();
        if (ImageName.IsEmpty()
            || IsProduct(*ImageName)
            || (bObjectImport && IsEngineImage(*ImageName))
            || (!bRestore && Scanned.Contains(Header))) continue;
        void* Retained = dlopen(Name, RTLD_LAZY | RTLD_NOLOAD);
        if (!Retained) { Warn(TEXT("could not retain loaded dylib for rebinding")); continue; }
        ON_SCOPE_EXIT { dlclose(Retained); };
        if (!bRestore) Scanned.Add(Header);
        int32 Changed = 0;
        if (Kind != 2) Changed = bObjectImport
            ? RebindImage(reinterpret_cast<const mach_header_64*>(Header), _dyld_get_image_vmaddr_slide(I), bRestore,
                ImportObjectPropertiesSymbol, reinterpret_cast<void*>(EngineImportObject), reinterpret_cast<void*>(&ImportObject))
            : RebindImage(reinterpret_cast<const mach_header_64*>(Header), _dyld_get_image_vmaddr_slide(I), bRestore,
                CopySymbol, reinterpret_cast<void*>(EngineCopy), reinterpret_cast<void*>(&Copy));
        if (Kind == 2)
        {
            const auto* ImageHeader = reinterpret_cast<const mach_header_64*>(Header);
            const intptr_t Slide = _dyld_get_image_vmaddr_slide(I);
            Changed += RebindImage(ImageHeader, Slide, bRestore, BoundarySymbol, reinterpret_cast<void*>(EngineBoundary), reinterpret_cast<void*>(&Boundary));
            Changed += RebindImage(
                ImageHeader, Slide, bRestore, ConstBoundarySymbol,
                reinterpret_cast<void*>(EngineConstBoundary), reinterpret_cast<void*>(&ConstBoundary));
            Changed += RebindImage(
                ImageHeader, Slide, bRestore, SignatureSymbol,
                reinterpret_cast<void*>(EngineSignature), reinterpret_cast<void*>(&Signature));
        }
        if (Changed)
            UE_LOG(LogSoloASEditorImports, Display, TEXT("%s imports module=%s slots=%d restored=%d"),
                Kind == 2 ? TEXT("ScriptNativeBoundary")
                : (bObjectImport ? TEXT("ImportObjectProperties") : TEXT("CPFUO")),
                *ImageName, Changed, bRestore);
    }
}
#endif

void OnFieldPathSwitchChanged(IConsoleVariable* Variable)
{
    FieldPathPassThrough.store(Variable->GetInt() == 0, std::memory_order_release);
    if (Variable->GetInt() == 0) return;
    // CoreUObject can serialize on background threads. Never detach a published trampoline.
    if (GIsEditor && bFieldPathResolved && FieldPathEntry.Original() == nullptr
        && !FieldPathEntry.Install(TEXT("??6@YAAEAVFArchive@@AEAV0@AEAUFFieldPath@@@Z"),
            "_ZlsR8FArchiveR10FFieldPath", reinterpret_cast<void*>(&SerializeFieldPath),
            TEXT("UnrealEditor-CoreUObject.dll"), "UnrealEditor-CoreUObject"))
        Warn(TEXT("CoreUObject FieldPath serialization entry hook unavailable; original serialization retained"));
}

void OnBoundarySwitchChanged(IConsoleVariable* Variable)
{
    {
        FScopeLock Lock(&ImagesLock);
        ScannedBoundaryImages.Reset();
    }
    if (bBoundaryResolved) Scan(Variable->GetInt() == 0, 2);
}

void OnSwitchChanged(IConsoleVariable* Variable)
{
    {
        FScopeLock Lock(&ImagesLock);
        ScannedImages.Reset();
    }
    Scan(Variable->GetInt() == 0, false);
}

void OnImportSwitchChanged(IConsoleVariable* Variable)
{
    {
        FScopeLock Lock(&ImagesLock);
        ScannedImportImages.Reset();
    }
    const bool bRestore = Variable->GetInt() == 0;
    if (bRestore || HasImportObjectAdapter())
        Scan(bRestore, true);
}
}

#if WITH_DEV_AUTOMATION_TESTS
namespace SoloASBridge::Editor::Tests
{
void RegisterCallerModule(ECallerRange Range, FName ModuleName)
{
	{
		FScopeLock Lock(&CallerRangeCacheMutex);
		TestCallerRegistrations.Add({Range, ModuleName});
	}
	RefreshCallerRanges();
}

void UnregisterCallerModule(ECallerRange Range, FName ModuleName)
{
	{
		FScopeLock Lock(&CallerRangeCacheMutex);
		TestCallerRegistrations.RemoveAll([Range, ModuleName](const FTestCallerRegistration& Registration)
		{
			return Registration.Range == Range && Registration.ModuleName == ModuleName;
		});
	}
	RefreshCallerRanges();
}

bool IsCallerInRegisteredModule(ECallerRange Range, const void* Address)
{
	return IsTestCallerInRange(Range, Address);
}
}
#endif

namespace SoloASBridge
{
FEditorFieldPathReloadScope::FEditorFieldPathReloadScope() { ++FieldPathReloadDepth; }
FEditorFieldPathReloadScope::~FEditorFieldPathReloadScope() { check(FieldPathReloadDepth != 0); --FieldPathReloadDepth; }

void SetEditorObjectImportPropertiesAdapter(FEditorObjectImportPredicate Predicate,
	FEditorObjectImportAdapter Adapter)
{
	{
		FScopeLock Lock(&ImportObjectAdapterLock);
		ImportObjectPredicate = Predicate;
		ImportObjectAdapter = Adapter;
	}
	if (Predicate != nullptr && Adapter != nullptr
		&& CVarEditorObjectImportProperties.GetValueOnAnyThread() != 0)
		Scan(false, true);
	else
		Scan(true, true);
}
}

namespace SoloASBridge::Private
{
void CopyPropertiesForUnrelatedObjectsOriginal(UObject* Old, UObject* New, UEngine::FCopyPropertiesForUnrelatedObjectsParams Params)
{
    OriginalCopy(Old, New, Params);
}

void InitializeEditorCopyPropertiesImports()
{
#if WITH_DEV_AUTOMATION_TESTS
    RefreshCallerRanges();
#endif
#if PLATFORM_WINDOWS
    MEMORY_BASIC_INFORMATION Region = {};
    const bool bCopyResolved = VirtualQuery(reinterpret_cast<const void*>(EngineCopy), &Region, sizeof(Region)) == sizeof(Region)
        && GetProcAddress(static_cast<HMODULE>(Region.AllocationBase), CopySymbol) == reinterpret_cast<FARPROC>(EngineCopy);
    const bool bImportResolved = VirtualQuery(reinterpret_cast<const void*>(EngineImportObject), &Region, sizeof(Region)) == sizeof(Region)
        && GetProcAddress(static_cast<HMODULE>(Region.AllocationBase), ImportObjectPropertiesSymbol)
            == reinterpret_cast<FARPROC>(EngineImportObject);
#else
    const bool bCopyResolved = dlsym(RTLD_DEFAULT, CopySymbol + 1) == reinterpret_cast<void*>(EngineCopy);
    const bool bImportResolved = dlsym(RTLD_DEFAULT, ImportObjectPropertiesSymbol + 1)
        == reinterpret_cast<void*>(EngineImportObject);
#endif
#if PLATFORM_WINDOWS
    auto ResolveBoundary = [](void* Address, const char* Symbol)
    {
        MEMORY_BASIC_INFORMATION Info = {};
        return VirtualQuery(Address, &Info, sizeof(Info)) == sizeof(Info)
            && GetProcAddress(static_cast<HMODULE>(Info.AllocationBase), Symbol) == reinterpret_cast<FARPROC>(Address);
    };
#else
    auto ResolveBoundary = [](void* Address, const char* Symbol)
    { return dlsym(RTLD_DEFAULT, Symbol + 1) == Address; };
#endif
    if (GIsEditor && !HasNativeScriptClassMarker<UClass>() && bCopyResolved)
    {
        if (!CopyEntry.Install(TEXT("?CopyPropertiesForUnrelatedObjects@UEngine@@SAXPEAVUObject@@0UFCopyPropertiesForUnrelatedObjectsParams@1@@Z"),
#if PLATFORM_WINDOWS
            "_ZN7UEngine33CopyPropertiesForUnrelatedObjectsEP7UObjectS1_NS_40FCopyPropertiesForUnrelatedObjectsParamsE",
#else
            CopySymbol + 1,
#endif
            reinterpret_cast<void*>(&CopyEngineEntry), TEXT("UnrealEditor-Engine.dll"), "UnrealEditor-Engine"))
            Warn(TEXT("Engine CPFUO entry hook unavailable; import redirection remains the fallback"));
    }
    bFieldPathResolved = ResolveBoundary(reinterpret_cast<void*>(EngineFieldPathSerialize), FieldPathSymbol);
    if (bFieldPathResolved)
    {
        CVarEditorFieldPathReloadCache.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OnFieldPathSwitchChanged));
        OnFieldPathSwitchChanged(CVarEditorFieldPathReloadCache.AsVariable());
    }
    else Warn(TEXT("CoreUObject FieldPath serialization export name or ABI not found"));
    bBoundaryResolved = ResolveBoundary(reinterpret_cast<void*>(EngineBoundary), BoundarySymbol)
        && ResolveBoundary(reinterpret_cast<void*>(EngineConstBoundary), ConstBoundarySymbol)
        && ResolveBoundary(reinterpret_cast<void*>(EngineSignature), SignatureSymbol);
    if (bBoundaryResolved)
    {
        CVarEditorScriptNativeBoundary.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OnBoundarySwitchChanged));
        if (CVarEditorScriptNativeBoundary.GetValueOnAnyThread() != 0) Scan(false, 2);
    }
    else Warn(TEXT("UnrealEd script native-boundary export name or ABI not found"));
    if (bCopyResolved)
    {
        CVarEditorCopyPropertiesImports.AsVariable()->SetOnChangedCallback(
            FConsoleVariableDelegate::CreateStatic(&OnSwitchChanged));
        if (CVarEditorCopyPropertiesImports.GetValueOnAnyThread() != 0) Scan(false, false);
    }
    else
        Warn(TEXT("Engine CPFUO export name or ABI not found"));
    if (bImportResolved)
    {
        CVarEditorObjectImportProperties.AsVariable()->SetOnChangedCallback(
            FConsoleVariableDelegate::CreateStatic(&OnImportSwitchChanged));
        if (CVarEditorObjectImportProperties.GetValueOnAnyThread() != 0 && HasImportObjectAdapter())
            Scan(false, true);
    }
    else
        Warn(TEXT("UnrealEd ImportObjectProperties export name or ABI not found"));
    ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddLambda(
        [](FName ModuleName, EModuleChangeReason Reason)
        {
#if WITH_DEV_AUTOMATION_TESTS
            if (Reason == EModuleChangeReason::ModuleLoaded)
                RefreshCallerRanges();
            else if (Reason == EModuleChangeReason::ModuleUnloaded)
                RefreshCallerRanges(ModuleName);
#endif
            // Discard identities on unload so a reused base is scanned again.
            // Cached identities are never dereferenced; restore enumerates live images.
            if (Reason == EModuleChangeReason::ModuleUnloaded)
            {
                FScopeLock Lock(&ImagesLock);
                ScannedImages.Reset();
                ScannedImportImages.Reset();
                ScannedBoundaryImages.Reset();
            }
            if (Reason == EModuleChangeReason::ModuleLoaded)
            {
#if PLATFORM_WINDOWS
                Scan(false, -1);
#else
                if (bBoundaryResolved && CVarEditorScriptNativeBoundary.GetValueOnAnyThread() != 0) Scan(false, 2);
                if (CVarEditorCopyPropertiesImports.GetValueOnAnyThread() != 0) Scan(false, false);
                if (CVarEditorObjectImportProperties.GetValueOnAnyThread() != 0 && HasImportObjectAdapter())
                    Scan(false, true);
#endif
            }
        });
}

void ShutdownEditorCopyPropertiesImports()
{
    // The Bridge does not support dynamic reload; keep this CoreUObject entry and
    // its trampoline resident until process exit, forwarding before other state is torn down.
    FieldPathPassThrough.store(true, std::memory_order_release);
    CVarEditorCopyPropertiesImports.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate());
    CVarEditorObjectImportProperties.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate());
    FModuleManager::Get().OnModulesChanged().Remove(ModulesChangedHandle);
#if WITH_DEV_AUTOMATION_TESTS
    {
        FScopeLock TestCallerLock(&CallerRangeCacheMutex);
        TestCallerRegistrations.Reset();
    }
    RefreshCallerRanges();
#endif
    CVarEditorScriptNativeBoundary.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate());
    CVarEditorFieldPathReloadCache.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate());
    Scan(true, 2);
    Scan(true, false);
    CopyEntry.Remove();
    Scan(true, true);
    FScopeLock Lock(&ImagesLock);
    ScannedImages.Reset();
    ScannedImportImages.Reset();
    ScannedBoundaryImages.Reset();
}
}
#endif
