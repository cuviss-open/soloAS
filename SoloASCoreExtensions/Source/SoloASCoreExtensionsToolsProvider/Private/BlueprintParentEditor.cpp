// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"
// CS.CoreExtensions.BlueprintInheritance.ParentEditTraversal (UE 5.7-5.8)
// SOLOAS-COMPAT-BEGIN(5.7-5.8) Enable editor protection on both supported versions [test: SoloAS.CoreExtensions.BlueprintInheritance.ParentRecompile] [id: CS.Compatibility.CoreExtensions.BlueprintParentEditor.SupportedVersions]
#if WITH_EDITOR && !IS_MONOLITHIC && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0) && UE_VERSION_OLDER_THAN(5, 9, 0) && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "BlueprintInheritance/SoloASBlueprintImportEditor.h"
#include "ClassGenerator/ASClass.h"
#include "BlueprintEditor.h"
#include "EdGraph/EdGraphPin.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Editor/SoloASEditorEntryHook.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformAtomics.h"
#include "Misc/CoreDelegates.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace
{
using FGetDerived = void (*)(const UClass*, TArray<UClass*>&, bool);
using FMarkModified = void (*)(UBlueprint*, FPropertyChangedEvent);
constexpr TCHAR MarkSymbol[] = TEXT("?MarkBlueprintAsModified@FBlueprintEditorUtils@@SAXPEAVUBlueprint@@UFPropertyChangedEvent@@@Z");
constexpr char MarkMacSymbol[] = "_ZN21FBlueprintEditorUtils23MarkBlueprintAsModifiedEP10UBlueprint21FPropertyChangedEvent";
constexpr TCHAR DerivedSymbol[] = TEXT("?GetDerivedClasses@@YAXPEBVUClass@@AEAV?$TArray@PEAVUClass@@V?$TSizedDefaultAllocator@$0CA@@@@@_N@Z");
constexpr char DerivedMacSymbol[] = "_Z17GetDerivedClassesPK6UClassR6TArrayIP6UClass22TSizedDefaultAllocatorILi32EEEb";
TAutoConsoleVariable<int32> ParentEditTraversal(TEXT("soloas.CE.BlueprintParentEditTraversal"), 1,
    TEXT("Protect imported Blueprint parent editing from non-Blueprint script descendants."));
FSoloASEditorEntryHook MarkHook, DerivedHook;
FSoloASEditorEntryHook AddHook, RemoveHook, BulkRemoveHook, ChangeTypeHook, RenameHook, ReparentHook;
const FGetDerived OriginalDerived = &GetDerivedClasses;
UPTRINT MarkStart = 0, MarkEnd = 0;
thread_local const UBlueprint* ModifyingBlueprint = nullptr;
thread_local int32 ActiveCalls = 0;
bool bWanted = false;
FDelegateHandle Retry;
struct FImportSlot { void** Slot; void* Original; };
TArray<FImportSlot> Slots;
bool bSlotProtectionFailed = false;

bool WriteSlot(void** Slot, void* Expected, void* Value)
{
#if PLATFORM_WINDOWS
    DWORD Protection = 0;
    if (!VirtualProtect(Slot, sizeof(void*), PAGE_READWRITE, &Protection)) return false;
    void* Previous = FPlatformAtomics::InterlockedCompareExchangePointer(Slot, Value, Expected);
    DWORD Ignored = 0;
    const bool Restored = VirtualProtect(Slot, sizeof(void*), Protection, &Ignored) != 0;
    if (!Restored) bSlotProtectionFailed = true;
    return Previous == Expected;
#else
    mach_vm_address_t Region = reinterpret_cast<mach_vm_address_t>(Slot);
    mach_vm_size_t Size = 0;
    vm_region_basic_info_data_64_t Info{};
    mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t Object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &Region, &Size, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&Info), &Count, &Object) != KERN_SUCCESS) return false;
    if (Object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), Object);
    const mach_vm_address_t Page = reinterpret_cast<mach_vm_address_t>(Slot) & ~(mach_vm_address_t(vm_page_size) - 1);
    if (mach_vm_protect(mach_task_self(), Page, vm_page_size, false,
        VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) return false;
    void* Previous = Expected;
    const bool Replaced = __atomic_compare_exchange_n(Slot, &Previous, Value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    const bool Restored = mach_vm_protect(mach_task_self(), Page, vm_page_size, false, Info.protection) == KERN_SUCCESS;
    if (!Restored) bSlotProtectionFailed = true;
    return Replaced;
#endif
}

// Resolve the direct caller once; the CoreUObject hot path only compares an address range.
bool FindMarkRange(void* Address)
{
    MarkStart = MarkEnd = 0;
#if PLATFORM_WINDOWS
    DWORD64 Base = 0;
    const RUNTIME_FUNCTION* Function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(Address), &Base, nullptr);
    if (!Function) return false;
    MarkStart = Base + Function->BeginAddress;
    MarkEnd = Base + Function->EndAddress;
#else
    Dl_info Info{};
    if (!dladdr(Address, &Info) || !Info.dli_fbase) return false;
    const auto* Header = static_cast<const mach_header_64*>(Info.dli_fbase);
    if (Header->magic != MH_MAGIC_64) return false;
    intptr_t Slide = 0;
    bool Found = false;
    for (uint32 I = 0; I < _dyld_image_count(); ++I)
        if (_dyld_get_image_header(I) == reinterpret_cast<const mach_header*>(Header))
        { Slide = _dyld_get_image_vmaddr_slide(I); Found = true; break; }
    if (!Found) return false;
    const uint8* Cursor = reinterpret_cast<const uint8*>(Header + 1);
    const uint8* Limit = Cursor + Header->sizeofcmds;
    UPTRINT Text = 0, LinkEdit = 0;
    const linkedit_data_command* Starts = nullptr;
    for (uint32 I = 0; I < Header->ncmds; ++I)
    {
        if (Cursor + sizeof(load_command) > Limit) return false;
        const auto* Command = reinterpret_cast<const load_command*>(Cursor);
        if (Command->cmdsize < sizeof(load_command) || Cursor + Command->cmdsize > Limit) return false;
        if (Command->cmd == LC_SEGMENT_64)
        {
            const auto* Segment = reinterpret_cast<const segment_command_64*>(Command);
            if (!strcmp(Segment->segname, SEG_TEXT)) Text = Slide + Segment->vmaddr;
            if (!strcmp(Segment->segname, SEG_LINKEDIT)) LinkEdit = Slide + Segment->vmaddr - Segment->fileoff;
        }
        if (Command->cmd == LC_FUNCTION_STARTS) Starts = reinterpret_cast<const linkedit_data_command*>(Command);
        Cursor += Command->cmdsize;
    }
    if (!Starts || !Text || !LinkEdit) return false;
    Cursor = reinterpret_cast<const uint8*>(LinkEdit + Starts->dataoff);
    Limit = Cursor + Starts->datasize;
    UPTRINT Function = Text;
    const UPTRINT Target = reinterpret_cast<UPTRINT>(Address);
    while (Cursor < Limit)
    {
        uint64 Delta = 0; uint32 Shift = 0; uint8 Byte;
        do
        {
            if (Cursor == Limit || Shift >= 64) return false;
            Byte = *Cursor++; Delta |= uint64(Byte & 0x7f) << Shift; Shift += 7;
        } while (Byte & 0x80);
        if (!Delta) break;
        Function += Delta;
        if (MarkStart && Function > Target) { MarkEnd = Function; break; }
        if (Function == Target) MarkStart = Function;
    }
#endif
    return MarkStart == reinterpret_cast<UPTRINT>(Address) && MarkEnd > MarkStart;
}

void DerivedClasses(const UClass* Parent, TArray<UClass*>& Results, bool Recursive)
{
    const UPTRINT Caller = reinterpret_cast<UPTRINT>(SoloASBridge::Editor::GetEditorHookCallerAddress());
    const int32 First = Results.Num();
    void* EntryOriginal = DerivedHook.Original();
    const auto Native = EntryOriginal ? reinterpret_cast<FGetDerived>(EntryOriginal) : OriginalDerived;
    Native(Parent, Results, Recursive);
    if (Caller < MarkStart || Caller >= MarkEnd) return;
    if (!ModifyingBlueprint || ModifyingBlueprint->GeneratedClass != Parent || !Recursive) return;
    for (int32 I = Results.Num() - 1; I >= First; --I)
        if (!Cast<UBlueprintGeneratedClass>(Results[I]) && Cast<UASClass>(Results[I])) Results.RemoveAt(I);
}
void MarkModified(UBlueprint* Blueprint, FPropertyChangedEvent Event)
{
    TGuardValue<int32> Active(ActiveCalls, ActiveCalls + 1);
    const bool Protected = IsInGameThread() && ParentEditTraversal.GetValueOnGameThread() != 0 && Blueprint
        && SoloASCoreExtensions::BlueprintInheritance::IsImportedBlueprintParent(Blueprint->GeneratedClass);
    TGuardValue<const UBlueprint*> Scope(ModifyingBlueprint, Protected ? Blueprint : nullptr);
    reinterpret_cast<FMarkModified>(MarkHook.Original())(Blueprint, Event);
}

bool RejectStructureEdit(UBlueprint* Blueprint)
{
    if (!IsInGameThread() || !Blueprint || ParentEditTraversal.GetValueOnGameThread() == 0
        || !SoloASCoreExtensions::BlueprintInheritance::IsImportedBlueprintParent(Blueprint->GeneratedClass)) return false;
    TArray<UClass*> Children;
    GetDerivedClasses(Blueprint->GeneratedClass, Children, true);
    if (!Children.ContainsByPredicate([](UClass* Class) { return Cast<UASClass>(Class) != nullptr; })) return false;
    const FText Message = NSLOCTEXT("SoloASCoreExtensions", "ImportedParentStructureEditRejected",
        "This Blueprint is inherited by a script class. Variable structure and parent changes are currently unavailable. "
        "Remove the script subclasses or their Blueprint import and restart the editor before changing the structure.");
    UE_LOG(LogTemp, Warning, TEXT("CE-BLUEPRINT-STRUCTURE-EDIT-REJECTED: %s: %s"), *Blueprint->GetPathName(), *Message.ToString());
    if (FSlateApplication::IsInitialized())
    {
        FNotificationInfo Info(Message); Info.ExpireDuration = 8.0f;
        FSlateNotificationManager::Get().AddNotification(Info);
    }
    return true;
}
bool AddVariable(UBlueprint* Blueprint, const FName& Name, const FEdGraphPinType& Type, const FString& Default)
{
    TGuardValue<int32> Active(ActiveCalls, ActiveCalls + 1);
    if (RejectStructureEdit(Blueprint)) return false;
    return reinterpret_cast<bool (*)(UBlueprint*, const FName&, const FEdGraphPinType&, const FString&)>(AddHook.Original())(Blueprint, Name, Type, Default);
}
void RemoveVariable(UBlueprint* Blueprint, FName Name)
{
    TGuardValue<int32> Active(ActiveCalls, ActiveCalls + 1);
    if (RejectStructureEdit(Blueprint)) return;
    reinterpret_cast<void (*)(UBlueprint*, FName)>(RemoveHook.Original())(Blueprint, Name);
}
void BulkRemoveVariables(UBlueprint* Blueprint, const TArray<FName>& Names)
{
    TGuardValue<int32> Active(ActiveCalls, ActiveCalls + 1);
    if (RejectStructureEdit(Blueprint)) return;
    reinterpret_cast<void (*)(UBlueprint*, const TArray<FName>&)>(BulkRemoveHook.Original())(Blueprint, Names);
}
void ChangeVariableType(UBlueprint* Blueprint, FName Name, const FEdGraphPinType& Type)
{
    TGuardValue<int32> Active(ActiveCalls, ActiveCalls + 1);
    if (RejectStructureEdit(Blueprint)) return;
    reinterpret_cast<void (*)(UBlueprint*, FName, const FEdGraphPinType&)>(ChangeTypeHook.Original())(Blueprint, Name, Type);
}
void RenameVariable(UBlueprint* Blueprint, FName OldName, FName NewName)
{
    TGuardValue<int32> Active(ActiveCalls, ActiveCalls + 1);
    if (RejectStructureEdit(Blueprint)) return;
    reinterpret_cast<void (*)(UBlueprint*, FName, FName)>(RenameHook.Original())(Blueprint, OldName, NewName);
}
void ReparentBlueprint(FBlueprintEditor* Editor, UClass* Parent)
{
    TGuardValue<int32> Active(ActiveCalls, ActiveCalls + 1);
    if (RejectStructureEdit(Editor->GetBlueprintObj())) return;
    reinterpret_cast<void (*)(FBlueprintEditor*, UClass*)>(ReparentHook.Original())(Editor, Parent);
}
struct FStructuralEntry
{
    FSoloASEditorEntryHook* Hook;
    const TCHAR* WindowsSymbol;
    const char* MacSymbol;
    void* Replacement;
    const TCHAR* WindowsModule = TEXT("UnrealEditor-UnrealEd.dll");
    const char* MacModule = "UnrealEditor-UnrealEd";
};
const FStructuralEntry StructuralEntries[] = {
    // CS.CoreExtensions.BlueprintInheritance.RejectAddVariable (UE 5.7-5.8)
    {&AddHook, TEXT("?AddMemberVariable@FBlueprintEditorUtils@@SA_NPEAVUBlueprint@@AEBVFName@@AEBUFEdGraphPinType@@AEBVFString@@@Z"),
        "_ZN21FBlueprintEditorUtils17AddMemberVariableEP10UBlueprintRK5FNameRK15FEdGraphPinTypeRK7FString", reinterpret_cast<void*>(&AddVariable)},
    // CS.CoreExtensions.BlueprintInheritance.RejectRemoveVariable (UE 5.7-5.8)
    {&RemoveHook, TEXT("?RemoveMemberVariable@FBlueprintEditorUtils@@SAXPEAVUBlueprint@@VFName@@@Z"),
        "_ZN21FBlueprintEditorUtils20RemoveMemberVariableEP10UBlueprint5FName", reinterpret_cast<void*>(&RemoveVariable)},
    // CS.CoreExtensions.BlueprintInheritance.RejectBulkRemoveVariables (UE 5.7-5.8)
    {&BulkRemoveHook, TEXT("?BulkRemoveMemberVariables@FBlueprintEditorUtils@@SAXPEAVUBlueprint@@AEBV?$TArray@VFName@@V?$TSizedDefaultAllocator@$0CA@@@@@@Z"),
        "_ZN21FBlueprintEditorUtils25BulkRemoveMemberVariablesEP10UBlueprintRK6TArrayI5FName22TSizedDefaultAllocatorILi32EEE", reinterpret_cast<void*>(&BulkRemoveVariables)},
    // CS.CoreExtensions.BlueprintInheritance.RejectChangeVariableType (UE 5.7-5.8)
    {&ChangeTypeHook, TEXT("?ChangeMemberVariableType@FBlueprintEditorUtils@@SAXPEAVUBlueprint@@VFName@@AEBUFEdGraphPinType@@@Z"),
        "_ZN21FBlueprintEditorUtils24ChangeMemberVariableTypeEP10UBlueprint5FNameRK15FEdGraphPinType", reinterpret_cast<void*>(&ChangeVariableType)},
    // CS.CoreExtensions.BlueprintInheritance.RejectRenameVariable (UE 5.7-5.8)
    {&RenameHook, TEXT("?RenameMemberVariable@FBlueprintEditorUtils@@SAXPEAVUBlueprint@@VFName@@1@Z"),
        "_ZN21FBlueprintEditorUtils20RenameMemberVariableEP10UBlueprint5FNameS2_", reinterpret_cast<void*>(&RenameVariable)},
    // CS.CoreExtensions.BlueprintInheritance.RejectReparent (UE 5.7-5.8)
    {&ReparentHook, TEXT("?ReparentBlueprint_NewParentChosen@FBlueprintEditor@@QEAAXPEAVUClass@@@Z"),
        "_ZN16FBlueprintEditor33ReparentBlueprint_NewParentChosenEP6UClass", reinterpret_cast<void*>(&ReparentBlueprint),
        TEXT("UnrealEditor-Kismet.dll"), "UnrealEditor-Kismet"}
};

void BindImports(void* MarkAddress)
{
    auto Add = [](void** Slot)
    {
        if (*Slot == reinterpret_cast<void*>(OriginalDerived)
            && WriteSlot(Slot, *Slot, reinterpret_cast<void*>(&DerivedClasses)))
            Slots.Add({Slot, reinterpret_cast<void*>(OriginalDerived)});
    };
#if PLATFORM_WINDOWS
    MEMORY_BASIC_INFORMATION Region{};
    if (!VirtualQuery(MarkAddress, &Region, sizeof(Region))) return;
    auto* Base = static_cast<uint8*>(Region.AllocationBase);
    const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
    const auto* NT = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
    const auto& Directory = NT->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!Directory.VirtualAddress || !Directory.Size) return;
    const auto* Import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(Base + Directory.VirtualAddress);
    for (; Import->Name; ++Import)
        for (auto* Thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(Base + Import->FirstThunk); Thunk->u1.Function; ++Thunk)
            Add(reinterpret_cast<void**>(&Thunk->u1.Function));
#else
    Dl_info Info{};
    if (!dladdr(MarkAddress, &Info) || !Info.dli_fbase) return;
    const auto* Header = static_cast<const mach_header_64*>(Info.dli_fbase);
    intptr_t Slide = 0;
    bool Found = false;
    for (uint32 I = 0; I < _dyld_image_count(); ++I)
        if (_dyld_get_image_header(I) == reinterpret_cast<const mach_header*>(Header))
        { Slide = _dyld_get_image_vmaddr_slide(I); Found = true; break; }
    if (!Found) return;
    const uint8* Cursor = reinterpret_cast<const uint8*>(Header + 1);
    const uint8* Limit = Cursor + Header->sizeofcmds;
    for (uint32 I = 0; I < Header->ncmds; ++I)
    {
        if (Cursor + sizeof(load_command) > Limit) return;
        const auto* Command = reinterpret_cast<const load_command*>(Cursor);
        if (Command->cmdsize < sizeof(load_command) || Cursor + Command->cmdsize > Limit) return;
        if (Command->cmd == LC_SEGMENT_64)
        {
            const auto* Segment = reinterpret_cast<const segment_command_64*>(Command);
            if (sizeof(segment_command_64) + Segment->nsects * sizeof(section_64) > Command->cmdsize) return;
            const auto* Sections = reinterpret_cast<const section_64*>(Segment + 1);
            for (uint32 J = 0; J < Segment->nsects; ++J)
            {
                const uint32 Kind = Sections[J].flags & SECTION_TYPE;
                if (Kind != S_LAZY_SYMBOL_POINTERS && Kind != S_NON_LAZY_SYMBOL_POINTERS) continue;
                auto** Begin = reinterpret_cast<void**>(Slide + Sections[J].addr);
                for (uint64 K = 0; K < Sections[J].size / sizeof(void*); ++K) Add(Begin + K);
            }
        }
        Cursor += Command->cmdsize;
    }
#endif
}

bool RefreshHooks()
{
    if (ActiveCalls) return false;
    const bool Enabled = bWanted && ParentEditTraversal.GetValueOnGameThread() != 0;
    bool AnyStructural = false, AllStructural = true;
    for (const auto& Entry : StructuralEntries)
    {
        AnyStructural |= Entry.Hook->Original() != nullptr;
        AllStructural &= Entry.Hook->Original() != nullptr;
    }
    if (Enabled && MarkHook.Original() && AllStructural && (!Slots.IsEmpty() || DerivedHook.Original())) return true;
    if (Enabled && (MarkHook.Original() || DerivedHook.Original() || AnyStructural || !Slots.IsEmpty()))
    {
        const bool Wanted = bWanted; bWanted = false;
        const bool Removed = RefreshHooks(); bWanted = Wanted;
        if (!Removed) return false;
    }
    if (!Enabled)
    {
        bool Restored = true;
        for (int32 I = UE_ARRAY_COUNT(StructuralEntries) - 1; I >= 0; --I)
        {
            StructuralEntries[I].Hook->Remove();
            if (StructuralEntries[I].Hook->Original()) Restored = false;
        }
        if (!Restored) return false;
        for (int32 I = Slots.Num() - 1; I >= 0; --I)
        {
            if (*Slots[I].Slot != reinterpret_cast<void*>(&DerivedClasses)
                || WriteSlot(Slots[I].Slot, reinterpret_cast<void*>(&DerivedClasses), Slots[I].Original)) Slots.RemoveAt(I);
            else Restored = false;
        }
        if (!Restored) return false;
        DerivedHook.Remove();
        if (DerivedHook.Original()) return false;
        MarkHook.Remove();
        if (MarkHook.Original()) return false;
        MarkStart = MarkEnd = 0;
        return true;
    }
    // MarkBlueprintAsModified calls itself within UnrealEd; its entry is required.
#if PLATFORM_WINDOWS
    void* Library = FPlatformProcess::GetDllHandle(TEXT("UnrealEditor-UnrealEd.dll"));
    void* Address = Library ? FPlatformProcess::GetDllExport(Library, MarkSymbol) : nullptr;
#else
    void* Library = nullptr;
    void* Address = dlsym(RTLD_DEFAULT, MarkMacSymbol);
#endif
    const bool RangeFound = Address && FindMarkRange(Address);
    if (Library) FPlatformProcess::FreeDllHandle(Library);
    if (!RangeFound || !MarkHook.Install(MarkSymbol, MarkMacSymbol, reinterpret_cast<void*>(&MarkModified))) return false;
    bSlotProtectionFailed = false;
    BindImports(Address);
    if (bSlotProtectionFailed)
    {
        const bool Wanted = bWanted; bWanted = false;
        RefreshHooks(); bWanted = Wanted;
        return false;
    }
    if (Slots.IsEmpty())
    {
        if (!DerivedHook.Install(DerivedSymbol, DerivedMacSymbol, reinterpret_cast<void*>(&DerivedClasses),
            TEXT("UnrealEditor-CoreUObject.dll"), "UnrealEditor-CoreUObject"))
        {
            MarkHook.Remove(); MarkStart = MarkEnd = 0; return false;
        }
    }
    for (const auto& Entry : StructuralEntries)
        if (!Entry.Hook->Install(Entry.WindowsSymbol, Entry.MacSymbol, Entry.Replacement, Entry.WindowsModule, Entry.MacModule))
        {
            const bool Wanted = bWanted; bWanted = false;
            RefreshHooks(); bWanted = Wanted;
            return false;
        }
    UE_LOG(LogTemp, Display, TEXT("CE Blueprint parent editing protected: slots=%d entry=%d structural=%d"), Slots.Num(), DerivedHook.Original() != nullptr, int32(UE_ARRAY_COUNT(StructuralEntries)));
    return true;
}
bool ConfigureHooks(bool Enabled)
{
    bWanted = Enabled;
    const bool Success = RefreshHooks();
    if (!Success) UE_LOG(LogTemp, Error, TEXT("CE Blueprint parent traversal adapter could not be %s."), Enabled ? TEXT("installed") : TEXT("removed"));
    return Success;
}
}

void StartBlueprintParentEditor()
{
    ParentEditTraversal->SetOnChangedCallback(FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*)
    {
        if (!RefreshHooks() && !Retry.IsValid())
            Retry = FCoreDelegates::OnEndFrame.AddLambda([]
            {
                if (RefreshHooks()) { FCoreDelegates::OnEndFrame.Remove(Retry); Retry.Reset(); }
            });
    }));
    SoloASCoreExtensions::BlueprintInheritance::SetImportEditorAdapter(&ConfigureHooks);
}
void StopBlueprintParentEditor()
{
    checkf(SoloASCoreExtensions::BlueprintInheritance::SetImportEditorAdapter(nullptr), TEXT("Cannot unload CE with active Blueprint editor hooks."));
    ParentEditTraversal->SetOnChangedCallback(FConsoleVariableDelegate());
    FCoreDelegates::OnEndFrame.Remove(Retry); Retry.Reset();
}
#else
void StartBlueprintParentEditor() {}
void StopBlueprintParentEditor() {}
#endif
// SOLOAS-COMPAT-END
