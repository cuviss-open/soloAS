// Copyright Cuviss Studio. All Rights Reserved.
#include "CoreExtensions/SoloASBlueprintChildRelink.h"
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "Engine/Blueprint.h"
#include "UObject/UnrealType.h"
#include "UObject/PackageReload.h"
#include "UObject/ObjectKey.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Editor/SoloASEditorEntryHook.h"
#include <cstdint>
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include <intrin.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_MAC
#include "Mac/MacSystemIncludes.h"
#include "Misc/Paths.h"
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif
namespace
{
struct FModuleAddressRange
{
	uintptr_t Start = 0;
	uintptr_t End = 0;

	bool Contains(const void* Address) const
	{
		const uintptr_t Value = reinterpret_cast<uintptr_t>(Address);
		return Start < End && Value >= Start && Value < End;
	}
};

static FSoloASEditorEntryHook GetDerivedClassesEntry;
static FSoloASEditorEntryHook MarkBlueprintAsModifiedEntry;
static FModuleAddressRange KismetAddressRange;
static thread_local const UBlueprint* ModifiedBlueprint = nullptr;
static SoloASBridge::FBlueprintChildRelinkCallback QueueChild = nullptr;
static bool bInitialized = false;
static FDelegateHandle PackageReloadHandle;
static TMap<FObjectKey, TWeakObjectPtr<UBlueprint>> ReloadedBlueprintParents;

void OnPackageReloaded(EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
{
	if (Phase == EPackageReloadPhase::PrePackageFixup && Event != nullptr)
	{
		for (const auto& Pair : Event->GetRepointedObjects())
		{
			UClass* OldClass = Cast<UClass>(Pair.Key);
			UClass* NewClass = Cast<UClass>(Pair.Value);
			if (OldClass && NewClass)
			{
				if (UBlueprint* Blueprint = Cast<UBlueprint>(OldClass->ClassGeneratedBy))
					ReloadedBlueprintParents.Add(FObjectKey(OldClass), Blueprint);
			}
		}
	}
	else if (Phase == EPackageReloadPhase::PostBatchPreGC)
	{
		ReloadedBlueprintParents.Reset();
	}
}

static TAutoConsoleVariable<int32> CVarEditorBlueprintSubclassRelink(
    TEXT("soloas.EditorBlueprintSubclassRelink"), 1,
    TEXT("Rebuild script children after their Blueprint parent compiles."), ECVF_Default);

bool TryGetKismetAddressRange(FModuleAddressRange& OutRange)
{
#if PLATFORM_WINDOWS
	HMODULE Module = GetModuleHandleW(L"UnrealEditor-Kismet.dll");
	if (Module == nullptr)
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
		|| Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
		|| Nt->OptionalHeader.SizeOfImage == 0)
	{
		return false;
	}
	OutRange.Start = reinterpret_cast<uintptr_t>(Module);
	OutRange.End = OutRange.Start + Nt->OptionalHeader.SizeOfImage;
	return OutRange.Start < OutRange.End;
#else
	for (uint32 Index = 0; Index < _dyld_image_count(); ++Index)
	{
		const mach_header* Image = _dyld_get_image_header(Index);
		const char* ImagePath = _dyld_get_image_name(Index);
		if (Image == nullptr || ImagePath == nullptr || Image->magic != MH_MAGIC_64)
		{
			continue;
		}
		FString ImageBaseName = FPaths::GetBaseFilename(ANSI_TO_TCHAR(ImagePath));
		ImageBaseName.RemoveFromStart(TEXT("lib"), ESearchCase::CaseSensitive);
		if (ImageBaseName != TEXT("UnrealEditor-Kismet"))
		{
			continue;
		}

		const auto* Header = reinterpret_cast<const mach_header_64*>(Image);
		const uint8* Cursor = reinterpret_cast<const uint8*>(Header + 1);
		const uint8* End = Cursor + Header->sizeofcmds;
		const intptr_t Slide = _dyld_get_image_vmaddr_slide(Index);
		uintptr_t ImageStart = UINTPTR_MAX;
		uintptr_t ImageEnd = 0;
		for (uint32 CommandIndex = 0; CommandIndex < Header->ncmds; ++CommandIndex)
		{
			if (Cursor > End || static_cast<SIZE_T>(End - Cursor) < sizeof(load_command))
			{
				return false;
			}
			const auto* Command = reinterpret_cast<const load_command*>(Cursor);
			if (Command->cmdsize < sizeof(load_command)
				|| static_cast<SIZE_T>(End - Cursor) < Command->cmdsize)
			{
				return false;
			}
			if (Command->cmd == LC_SEGMENT_64 && Command->cmdsize >= sizeof(segment_command_64))
			{
				const auto* Segment = reinterpret_cast<const segment_command_64*>(Command);
				if ((Segment->initprot & VM_PROT_EXECUTE) != 0)
				{
					const uintptr_t Start = static_cast<uintptr_t>(Slide + Segment->vmaddr);
					if (Segment->vmsize <= UINTPTR_MAX - Start)
					{
						ImageStart = FMath::Min(ImageStart, Start);
						ImageEnd = FMath::Max(ImageEnd,
							static_cast<uintptr_t>(Start + Segment->vmsize));
					}
				}
			}
			Cursor += Command->cmdsize;
		}
		OutRange = {ImageStart, ImageEnd};
		return OutRange.Start < OutRange.End;
	}
	return false;
#endif
}

#if PLATFORM_WINDOWS
#define SOLOAS_BLUEPRINT_RELINK_CALLER_ADDRESS() _ReturnAddress()
#else
#define SOLOAS_BLUEPRINT_RELINK_CALLER_ADDRESS() __builtin_return_address(0)
#endif

using FGetDerivedClasses = void (*)(const UClass*, TArray<UClass*>&, bool);
using FMarkBlueprintAsModified = void (*)(UBlueprint*, FPropertyChangedEvent);

void MarkBlueprintAsModifiedHook(UBlueprint* Blueprint, FPropertyChangedEvent Event)
{
	TGuardValue<const UBlueprint*> Guard(ModifiedBlueprint, Blueprint);
	reinterpret_cast<FMarkBlueprintAsModified>(MarkBlueprintAsModifiedEntry.Original())(Blueprint, Event);
}

void GetDerivedClassesHook(const UClass* ClassToLookFor, TArray<UClass*>& Results, bool bRecursive)
{
	const void* CallerAddress = SOLOAS_BLUEPRINT_RELINK_CALLER_ADDRESS();
	if (ModifiedBlueprint != nullptr && bRecursive
		&& MarkBlueprintAsModifiedEntry.IsCallerInTargetModule(CallerAddress)
		&& ModifiedBlueprint->GeneratedClass.Get() == ClassToLookFor
		&& CVarEditorBlueprintSubclassRelink.GetValueOnAnyThread() != 0)
	{
		const int32 ExistingCount = Results.Num();
		reinterpret_cast<FGetDerivedClasses>(GetDerivedClassesEntry.Original())(
			ClassToLookFor, Results, bRecursive);
		for (int32 Index = Results.Num() - 1; Index >= ExistingCount; --Index)
		{
			// Null Blueprint asks the owner only to identify a script class, including retired classes.
			if (QueueChild != nullptr && QueueChild(Results[Index], nullptr))
				Results.RemoveAt(Index, 1, EAllowShrinking::No);
		}
		return;
	}
	// PackageTools reparenting gathers recursive descendants before reference fixup.
	// UASClass is a BPGC carrier, but must be rebuilt by the script compiler.
	if (bRecursive && !ReloadedBlueprintParents.IsEmpty()
		&& KismetAddressRange.Contains(CallerAddress)
		&& CVarEditorBlueprintSubclassRelink.GetValueOnAnyThread())
	{
		const TWeakObjectPtr<UBlueprint>* Parent = ReloadedBlueprintParents.Find(FObjectKey(ClassToLookFor));
		if (UBlueprint* Blueprint = Parent != nullptr ? Parent->Get() : nullptr)
		{
			const int32 ExistingCount = Results.Num();
			reinterpret_cast<FGetDerivedClasses>(GetDerivedClassesEntry.Original())(
				ClassToLookFor, Results, bRecursive);
			for (int32 Index = Results.Num() - 1; Index >= ExistingCount; --Index)
			{
				if (QueueChild != nullptr && QueueChild(Results[Index], Blueprint))
					Results.RemoveAt(Index, 1, EAllowShrinking::No);
			}
			return;
		}
	}
	if (!KismetAddressRange.Contains(CallerAddress)
		|| CVarEditorBlueprintSubclassRelink.GetValueOnAnyThread() == 0 || bRecursive)
	{
		reinterpret_cast<FGetDerivedClasses>(GetDerivedClassesEntry.Original())(
			ClassToLookFor, Results, bRecursive);
		return;
	}

#if WITH_EDITORONLY_DATA
	const UBlueprint* Blueprint = ClassToLookFor != nullptr
		? Cast<UBlueprint>(ClassToLookFor->ClassGeneratedBy) : nullptr;
	if (Blueprint == nullptr || !Blueprint->bBeingCompiled
		|| Blueprint->GeneratedClass.Get() != ClassToLookFor)
	{
		reinterpret_cast<FGetDerivedClasses>(GetDerivedClassesEntry.Original())(
			ClassToLookFor, Results, bRecursive);
		return;
	}
#else
	reinterpret_cast<FGetDerivedClasses>(GetDerivedClassesEntry.Original())(
		ClassToLookFor, Results, bRecursive);
	return;
#endif
	const int32 ExistingCount = Results.Num();
	reinterpret_cast<FGetDerivedClasses>(GetDerivedClassesEntry.Original())(
		ClassToLookFor, Results, bRecursive);
	for (int32 Index = Results.Num() - 1; Index >= ExistingCount; --Index)
	{
		UClass* ChildClass = Results[Index];
		if (ChildClass != nullptr && ChildClass->GetSuperClass() == ClassToLookFor
			&& QueueChild != nullptr && QueueChild(ChildClass, Blueprint))
		{
			Results.RemoveAt(Index, 1, EAllowShrinking::No);
		}
	}
}

}
namespace SoloASBridge
{
	bool InstallBlueprintChildRelink(FBlueprintChildRelinkCallback Callback)
	{
		if (Callback == nullptr || !GIsEditor || IsRunningCommandlet())
		{
			return false;
		}
		if (bInitialized) return QueueChild == Callback;
		if (!TryGetKismetAddressRange(KismetAddressRange))
		{
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Blueprint relink hook unavailable; Stock child relink remains."));
			return false;
		}
		if (!GetDerivedClassesEntry.Install(
			TEXT("?GetDerivedClasses@@YAXPEBVUClass@@AEAV?$TArray@PEAVUClass@@V?$TSizedDefaultAllocator@$0CA@@@@@_N@Z"),
			"_Z17GetDerivedClassesPK6UClassR6TArrayIP6UClass22TSizedDefaultAllocatorILi32EEEb",
			reinterpret_cast<void*>(&GetDerivedClassesHook),
			TEXT("UnrealEditor-CoreUObject.dll"), "UnrealEditor-CoreUObject"))
		{
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Blueprint relink hook unavailable; Stock child relink remains."));
			KismetAddressRange = {};
			return false;
		}

		QueueChild = Callback;
		if (!MarkBlueprintAsModifiedEntry.Install(
			TEXT("?MarkBlueprintAsModified@FBlueprintEditorUtils@@SAXPEAVUBlueprint@@UFPropertyChangedEvent@@@Z"),
			"_ZN21FBlueprintEditorUtils23MarkBlueprintAsModifiedEP10UBlueprint21FPropertyChangedEvent",
			reinterpret_cast<void*>(&MarkBlueprintAsModifiedHook)))
		{
			GetDerivedClassesEntry.Remove();
			QueueChild = nullptr;
			KismetAddressRange = {};
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Blueprint modified-class hook unavailable; child relink was not installed."));
			return false;
		}
		PackageReloadHandle = FCoreUObjectDelegates::OnPackageReloaded.AddStatic(&OnPackageReloaded);
		bInitialized = true;
		UE_LOG(LogTemp, Display, TEXT("[SoloAS] InstallBlueprintChildRelink: Kismet relink and UnrealEd modified-class scopes installed."));
		return true;
	}

    void ResetBlueprintChildRelinkBatch() {}
    void RemoveBlueprintChildRelink()
    {
        if (!bInitialized) return;
        FCoreUObjectDelegates::OnPackageReloaded.Remove(PackageReloadHandle);
        PackageReloadHandle.Reset();
        ReloadedBlueprintParents.Reset();
        MarkBlueprintAsModifiedEntry.Remove();
        GetDerivedClassesEntry.Remove();
        QueueChild = nullptr;
        KismetAddressRange = {};
        bInitialized = false;
    }
}
#undef SOLOAS_BLUEPRINT_RELINK_CALLER_ADDRESS
#endif
