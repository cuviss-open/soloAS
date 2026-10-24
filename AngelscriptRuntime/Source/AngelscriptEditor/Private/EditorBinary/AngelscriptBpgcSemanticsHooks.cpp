// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptBpgcSemanticsHooks.h"
#include "CoreMinimal.h"
#include "AngelscriptBpgcSemanticsHooksTest.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "BlueprintEditor.h"
#include "Animation/AnimInstance.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "EnvironmentQuery/EnvQueryNode.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "PersonaUtils.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "Editor/SoloASEditorEntryHook.h"
#include "SKismetInspector.h"
#include "WorldPartition/WorldPartitionRuntimeCellTransformer.h"

#include <atomic>
#include <cstdint>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include <intrin.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_MAC
#include "Mac/MacSystemIncludes.h"
#include "Misc/ScopeExit.h"
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

namespace
{
FSoloASEditorEntryHook WorldPartitionLogicEntry;
FSoloASEditorEntryHook ActorCdoCopyEntry;
FSoloASEditorEntryHook AnimCdoCopyEntry;
FSoloASEditorEntryHook KismetWarningEntry;
FSoloASEditorEntryHook EnvQueryTitleEntry;
FDelegateHandle ModulesChangedHandle;
bool bBpgcSemanticsInitialized = false;
bool bRunningCommandlet = false;

TAutoConsoleVariable<int32> CVarWorldPartitionScriptActors(
	TEXT("soloas.EditorWorldPartitionScriptActors"), 1,
	TEXT("Keep AngelScript Actor classes out of the World Partition ISM conversion path."));
TAutoConsoleVariable<int32> CVarActorCdoCopyGuard(
	TEXT("soloas.EditorActorCdoCopyGeneratedClassGuard"), 1,
	TEXT("Skip Blueprint-only CDO propagation for raw AngelScript Actor classes."));
TAutoConsoleVariable<int32> CVarAnimCdoCopyGuard(
	TEXT("soloas.EditorAnimCdoCopyGeneratedClassGuard"), 1,
	TEXT("Skip Blueprint-only CDO invalidation for raw AngelScript AnimInstance classes."));
TAutoConsoleVariable<int32> CVarKismetScriptParentWarning(
	TEXT("soloas.EditorKismetScriptParentWarning"), 1,
	TEXT("Do not open a nonexistent Blueprint asset from the inherited-component warning for a script parent."));
TAutoConsoleVariable<int32> CVarEnvQueryScriptNodeTitle(
	TEXT("soloas.EditorEnvQueryScriptNodeTitle"), 1,
	TEXT("Use the script class short name on EQS editor and debugger display surfaces."));

struct FBpgcSemanticsModuleAddressRange
{
	uintptr_t Start = 0;
	uintptr_t End = 0;

	bool IsValid() const { return Start < End; }
};

struct FCallerRangeSet
{
	FBpgcSemanticsModuleAddressRange Ranges[2];
	int32 NumRanges = 0;
	uintptr_t Partition = 0;

	void Add(const FBpgcSemanticsModuleAddressRange& Range)
	{
		if (Range.IsValid() && NumRanges < UE_ARRAY_COUNT(Ranges))
		{
			Ranges[NumRanges++] = Range;
		}
	}

	void Finalize()
	{
		if (NumRanges == 2 && Ranges[1].Start < Ranges[0].Start)
		{
			Swap(Ranges[0], Ranges[1]);
		}
		if (NumRanges == 2 && Ranges[0].End >= Ranges[1].Start)
		{
			Ranges[0].End = FMath::Max(Ranges[0].End, Ranges[1].End);
			NumRanges = 1;
		}
		if (NumRanges == 2)
		{
			Partition = Ranges[0].End + (Ranges[1].Start - Ranges[0].End) / 2;
		}
	}

	bool Contains(const void* Address) const
	{
		if (NumRanges == 0 || Address == nullptr)
		{
			return false;
		}
		const uintptr_t Value = reinterpret_cast<uintptr_t>(Address);
		const int32 Index = NumRanges == 2 && Value >= Partition ? 1 : 0;
		const FBpgcSemanticsModuleAddressRange& Range = Ranges[Index];
		return Value >= Range.Start && Value < Range.End;
	}
};

struct FCallerRangeCache
{
	FCallerRangeSet WorldPartition;
	FCallerRangeSet ActorCdoCopy;
	FCallerRangeSet AnimCdoCopy;
	FCallerRangeSet KismetWarning;
	FCallerRangeSet EnvQueryTitle;
};

std::atomic<const FCallerRangeCache*> ActiveCallerRanges{nullptr};
TArray<TUniquePtr<FCallerRangeCache>> RetainedCallerRanges;
std::atomic<uint32> ActiveCallerRangeReaders{0};
std::atomic<bool> bCallerRangeReclaimPending{false};
FCriticalSection CallerRangeCacheMutex;

#if WITH_DEV_AUTOMATION_TESTS
struct FTestCallerRegistration
{
	AngelscriptBpgcSemanticsHooks::Tests::ECallerRange Range;
	FName ModuleName;
};
TArray<FTestCallerRegistration> TestCallerRegistrations;
#endif

bool IsRawRegisteredScriptClass(UClass* Class)
{
	return Class != nullptr && Class->ClassGeneratedBy == nullptr
		&& SoloASBridge::IsRegisteredScriptClass(Class);
}

bool TryGetLoadedImageRange(FName ModuleName, FBpgcSemanticsModuleAddressRange& OutRange)
{
	if (!FModuleManager::Get().IsModuleLoaded(ModuleName))
	{
		return false;
	}

#if PLATFORM_WINDOWS
	const FString ImageName = TEXT("UnrealEditor-") + ModuleName.ToString() + TEXT(".dll");
	HMODULE Module = GetModuleHandleW(*ImageName);
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
	return true;
#else
	const FString ExpectedName = TEXT("UnrealEditor-") + ModuleName.ToString();
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
		if (ImageBaseName != ExpectedName)
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
						ImageEnd = FMath::Max(ImageEnd,
							static_cast<uintptr_t>(Start + Segment->vmsize));
					}
				}
			}
			Cursor += Command->cmdsize;
		}
		OutRange = {ImageStart, ImageEnd};
		return OutRange.IsValid();
	}
	return false;
#endif
}

void AddLoadedModule(FCallerRangeSet& Set, const TCHAR* ModuleName, FName ExcludedModule)
{
	const FName Name(ModuleName);
	if (Name == ExcludedModule)
	{
		return;
	}
	FBpgcSemanticsModuleAddressRange Range;
	if (TryGetLoadedImageRange(Name, Range))
	{
		Set.Add(Range);
	}
}

void RemoveRetiredCallerRangesIfSafe()
{
	if (ActiveCallerRangeReaders.load(std::memory_order_seq_cst) != 0)
	{
		return;
	}

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
	AddLoadedModule(Candidate.WorldPartition, TEXT("Engine"), ExcludedModule);

	AddLoadedModule(Candidate.ActorCdoCopy, TEXT("UnrealEd"), ExcludedModule);
	AddLoadedModule(Candidate.AnimCdoCopy, TEXT("Persona"), ExcludedModule);
	AddLoadedModule(Candidate.KismetWarning, TEXT("Kismet"), ExcludedModule);
	AddLoadedModule(Candidate.EnvQueryTitle, TEXT("AIModule"), ExcludedModule);
#if PLATFORM_MAC
	// Clang emits UEnvQueryNode::GetDescriptionTitle as a tail branch, so the return
	// address seen here is the EQS graph node's GetNodeTitle in EnvironmentQueryEditor.
	AddLoadedModule(Candidate.EnvQueryTitle, TEXT("EnvironmentQueryEditor"), ExcludedModule);
#endif

#if WITH_DEV_AUTOMATION_TESTS
	{
		FScopeLock Lock(&CallerRangeCacheMutex);
		for (const FTestCallerRegistration& Registration : TestCallerRegistrations)
		{
			FCallerRangeSet* Set = nullptr;
			switch (Registration.Range)
			{
			case AngelscriptBpgcSemanticsHooks::Tests::ECallerRange::ActorCdoCopy:
				Set = &Candidate.ActorCdoCopy;
				break;
			case AngelscriptBpgcSemanticsHooks::Tests::ECallerRange::AnimCdoCopy:
				Set = &Candidate.AnimCdoCopy;
				break;
			case AngelscriptBpgcSemanticsHooks::Tests::ECallerRange::KismetWarning:
				Set = &Candidate.KismetWarning;
				break;
			}
			if (Set != nullptr && Registration.ModuleName != ExcludedModule)
			{
				FBpgcSemanticsModuleAddressRange Range;
				if (TryGetLoadedImageRange(Registration.ModuleName, Range))
				{
					Set->Add(Range);
				}
			}
		}
	}
#endif

	Candidate.WorldPartition.Finalize();
	Candidate.ActorCdoCopy.Finalize();
	Candidate.AnimCdoCopy.Finalize();
	Candidate.KismetWarning.Finalize();
	Candidate.EnvQueryTitle.Finalize();
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

bool IsCallerInRange(const void* Address, FCallerRangeSet FCallerRangeCache::*RangeSet)
{
	ActiveCallerRangeReaders.fetch_add(1, std::memory_order_seq_cst);
	const FCallerRangeCache* Cache = ActiveCallerRanges.load(std::memory_order_seq_cst);
	const bool bContains = Cache != nullptr && (Cache->*RangeSet).Contains(Address);
	if (ActiveCallerRangeReaders.fetch_sub(1, std::memory_order_seq_cst) == 1
		&& bCallerRangeReclaimPending.load(std::memory_order_seq_cst))
	{
		RemoveRetiredCallerRangesIfSafe();
	}
	return bContains;
}

#if PLATFORM_WINDOWS
#define SOLOAS_BPGC_CALLER_ADDRESS() _ReturnAddress()
#else
#define SOLOAS_BPGC_CALLER_ADDRESS() __builtin_return_address(0)
#endif

using FIsBlueprintActorWithLogic = bool (*)(const UWorldPartitionRuntimeCellTransformer*, AActor*);
bool IsBlueprintActorWithLogic(const UWorldPartitionRuntimeCellTransformer* Transformer, AActor* Actor)
{
	if (!IsCallerInRange(SOLOAS_BPGC_CALLER_ADDRESS(), &FCallerRangeCache::WorldPartition))
	{
		return reinterpret_cast<FIsBlueprintActorWithLogic>(WorldPartitionLogicEntry.Original())(Transformer, Actor);
	}
	if (CVarWorldPartitionScriptActors.GetValueOnAnyThread() != 0 && Actor != nullptr
		&& SoloASBridge::HasRegisteredScriptClassInAncestry(Actor->GetClass()))
	{
		return true;
	}
	return reinterpret_cast<FIsBlueprintActorWithLogic>(WorldPartitionLogicEntry.Original())(Transformer, Actor);
}

class FScopedClearCompiledFromBlueprintFlag
{
public:
	explicit FScopedClearCompiledFromBlueprintFlag(UClass* InClass)
		: Class(InClass)
		, bWasCompiledFromBlueprint(InClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
	{
		if (bWasCompiledFromBlueprint)
		{
			Class->ClassFlags &= ~CLASS_CompiledFromBlueprint;
		}
	}

	~FScopedClearCompiledFromBlueprintFlag()
	{
		if (bWasCompiledFromBlueprint)
		{
			Class->ClassFlags |= CLASS_CompiledFromBlueprint;
		}
	}

private:
	UClass* Class;
	bool bWasCompiledFromBlueprint;
};

using FCopyActorProperties = int32 (*)(AActor*, AActor*, const EditorUtilities::FCopyOptions&);
int32 CopyActorProperties(AActor* Source, AActor* Target, const EditorUtilities::FCopyOptions& Options)
{
	if (!IsCallerInRange(SOLOAS_BPGC_CALLER_ADDRESS(), &FCallerRangeCache::ActorCdoCopy))
	{
		return reinterpret_cast<FCopyActorProperties>(ActorCdoCopyEntry.Original())(Source, Target, Options);
	}
	if (CVarActorCdoCopyGuard.GetValueOnAnyThread() != 0 && Target != nullptr
		&& Target->HasAnyFlags(RF_ClassDefaultObject) && IsRawRegisteredScriptClass(Target->GetClass()))
	{
		FScopedClearCompiledFromBlueprintFlag ClearScriptClassFlag(Target->GetClass());
		return reinterpret_cast<FCopyActorProperties>(ActorCdoCopyEntry.Original())(Source, Target, Options);
	}
	return reinterpret_cast<FCopyActorProperties>(ActorCdoCopyEntry.Original())(Source, Target, Options);
}

using FCopyAnimPropertiesToCDO = int32 (*)(UAnimInstance*, const PersonaUtils::FCopyOptions&);
int32 CopyAnimPropertiesToCDO(UAnimInstance* Source, const PersonaUtils::FCopyOptions& Options)
{
	if (!IsCallerInRange(SOLOAS_BPGC_CALLER_ADDRESS(), &FCallerRangeCache::AnimCdoCopy))
	{
		return reinterpret_cast<FCopyAnimPropertiesToCDO>(AnimCdoCopyEntry.Original())(Source, Options);
	}
	if (CVarAnimCdoCopyGuard.GetValueOnAnyThread() != 0 && Source != nullptr
		&& IsRawRegisteredScriptClass(Source->GetClass()))
	{
		FScopedClearCompiledFromBlueprintFlag ClearScriptClassFlag(Source->GetClass());
		return reinterpret_cast<FCopyAnimPropertiesToCDO>(AnimCdoCopyEntry.Original())(Source, Options);
	}
	return reinterpret_cast<FCopyAnimPropertiesToCDO>(AnimCdoCopyEntry.Original())(Source, Options);
}

UBlueprint* FindBlueprintForInspector(const SKismetInspector* Inspector)
{
	if (Inspector == nullptr)
	{
		return nullptr;
	}
	for (const TWeakObjectPtr<UObject>& WeakObject : Inspector->GetSelectedObjects())
	{
		for (UObject* Current = WeakObject.Get(); Current != nullptr; Current = Current->GetOuter())
		{
			if (UBlueprint* Blueprint = Cast<UBlueprint>(Current))
			{
				return Blueprint;
			}
			if (UClass* Class = Cast<UClass>(Current))
			{
				if (UBlueprint* Blueprint = Cast<UBlueprint>(Class->ClassGeneratedBy))
				{
					return Blueprint;
				}
			}
			if (UClass* ObjectClass = Current->GetClass())
			{
				if (UBlueprint* Blueprint = Cast<UBlueprint>(ObjectClass->ClassGeneratedBy))
				{
					return Blueprint;
				}
			}
		}
	}
	return nullptr;
}

using FOnInheritedBlueprintComponentWarningHyperlinkClicked =
	void (*)(SKismetInspector*, const FSlateHyperlinkRun::FMetadata&);
void OnInheritedBlueprintComponentWarningHyperlinkClicked(
	SKismetInspector* Inspector, const FSlateHyperlinkRun::FMetadata& Metadata)
{
	if (!IsCallerInRange(SOLOAS_BPGC_CALLER_ADDRESS(), &FCallerRangeCache::KismetWarning))
	{
		return reinterpret_cast<FOnInheritedBlueprintComponentWarningHyperlinkClicked>(KismetWarningEntry.Original())(Inspector, Metadata);
	}
	if (CVarKismetScriptParentWarning.GetValueOnAnyThread() != 0)
	{
		UBlueprint* Blueprint = FindBlueprintForInspector(Inspector);
		if (Blueprint != nullptr && IsRawRegisteredScriptClass(Blueprint->ParentClass))
		{
			return;
		}
	}
	reinterpret_cast<FOnInheritedBlueprintComponentWarningHyperlinkClicked>(KismetWarningEntry.Original())(Inspector, Metadata);
}

bool GetScriptEnvQueryNodeTitle(const UObject* Object, FText& OutTitle)
{
	UClass* Class = Object ? Object->GetClass() : nullptr;
	if (CVarEnvQueryScriptNodeTitle.GetValueOnAnyThread() != 0 && Class != nullptr
		&& SoloASBridge::IsRegisteredScriptClass(Class))
	{
		FString ShortName = Class->GetName();
		const int32 PrefixSeparator = ShortName.Find(TEXT("_"), ESearchCase::CaseSensitive);
		if (PrefixSeparator != INDEX_NONE)
		{
			ShortName.RightChopInline(PrefixSeparator + 1);
		}
		OutTitle = FText::FromString(MoveTemp(ShortName));
		return true;
	}
	return false;
}

#if PLATFORM_WINDOWS
// UEnvQueryTypes::GetShortTypeName is called directly from the AIModule title/debug paths.
// MSVC static-return ABI: hidden FText result in RCX, object in RDX, result address in RAX.
using FGetEnvQueryShortTypeName = FText* (*)(FText*, const UObject*);
FText* GetEnvQueryShortTypeName(FText* Result, const UObject* Object)
{
	if (!IsCallerInRange(SOLOAS_BPGC_CALLER_ADDRESS(), &FCallerRangeCache::EnvQueryTitle))
	{
		return reinterpret_cast<FGetEnvQueryShortTypeName>(EnvQueryTitleEntry.Original())(Result, Object);
	}
	FText ScriptTitle;
	if (GetScriptEnvQueryNodeTitle(Object, ScriptTitle))
	{
		new (Result) FText(MoveTemp(ScriptTitle));
		return Result;
	}
	return reinterpret_cast<FGetEnvQueryShortTypeName>(EnvQueryTitleEntry.Original())(Result, Object);
}
#else
// Keep FText by value so Clang emits the static function's Itanium sret ABI
// (x86_64: hidden result before the object; arm64: indirect result in x8, object in x0).
using FGetEnvQueryShortTypeName = FText (*)(const UObject*);
FText GetEnvQueryShortTypeName(const UObject* Object)
{
	if (!IsCallerInRange(SOLOAS_BPGC_CALLER_ADDRESS(), &FCallerRangeCache::EnvQueryTitle))
	{
		return reinterpret_cast<FGetEnvQueryShortTypeName>(EnvQueryTitleEntry.Original())(Object);
	}
	FText ScriptTitle;
	if (GetScriptEnvQueryNodeTitle(Object, ScriptTitle))
	{
		return ScriptTitle;
	}
	return reinterpret_cast<FGetEnvQueryShortTypeName>(EnvQueryTitleEntry.Original())(Object);
}
#endif

#if PLATFORM_MAC
struct FMacHookInstallReport
{
	FSoloASEditorEntryHook* Entry;
	const TCHAR* Label;
	bool bWasInstalled;
};
#endif

void InstallHookForLoadedModule(FName ModuleName)
{
	if (!FModuleManager::Get().IsModuleLoaded(ModuleName))
	{
		return;
	}
#if PLATFORM_MAC
	FMacHookInstallReport Reports[] = {
		{&WorldPartitionLogicEntry, TEXT("World Partition script Actor"), WorldPartitionLogicEntry.Original() != nullptr},
		{&ActorCdoCopyEntry, TEXT("Actor CDO copy"), ActorCdoCopyEntry.Original() != nullptr},
		{&AnimCdoCopyEntry, TEXT("Persona AnimInstance copy"), AnimCdoCopyEntry.Original() != nullptr},
		{&KismetWarningEntry, TEXT("Kismet script-parent warning"), KismetWarningEntry.Original() != nullptr},
		{&EnvQueryTitleEntry, TEXT("EQS script node title"), EnvQueryTitleEntry.Original() != nullptr}};
	ON_SCOPE_EXIT
	{
		for (const FMacHookInstallReport& Report : Reports)
		{
			if (!Report.bWasInstalled && Report.Entry->Original() != nullptr)
			{
				UE_LOG(LogTemp, Display, TEXT("[SoloAS] %s hook installed."), Report.Label);
			}
		}
	};
#endif
	if (ModuleName == TEXT("Engine"))
	{
		if (!WorldPartitionLogicEntry.Original()
			&& !WorldPartitionLogicEntry.Install(
				TEXT("?IsBlueprintActorWithLogic@UWorldPartitionRuntimeCellTransformer@@MEBA_NPEAVAActor@@@Z"),
				"_ZNK37UWorldPartitionRuntimeCellTransformer25IsBlueprintActorWithLogicEP6AActor",
				reinterpret_cast<void*>(&IsBlueprintActorWithLogic), TEXT("UnrealEditor-Engine.dll"), "UnrealEditor-Engine"))
		{
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] World Partition script Actor hook unavailable; Stock ISM eligibility remains."));
		}
	}
	if (bRunningCommandlet)
	{
		return;
	}
	if (ModuleName == TEXT("UnrealEd"))
	{
		if (!ActorCdoCopyEntry.Original()
			&& !ActorCdoCopyEntry.Install(
				TEXT("?CopyActorProperties@EditorUtilities@@YAHPEAVAActor@@0AEBUFCopyOptions@1@@Z"),
				"_ZN15EditorUtilities19CopyActorPropertiesEP6AActorS1_RKNS_12FCopyOptionsE",
				reinterpret_cast<void*>(&CopyActorProperties)))
		{
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Actor CDO script-class copy guard unavailable; Stock Editor copy path remains."));
		}
	}
	else if (ModuleName == TEXT("Persona"))
	{
		if (!AnimCdoCopyEntry.Original()
			&& !AnimCdoCopyEntry.Install(
				TEXT("?CopyPropertiesToCDO@PersonaUtils@@YAHPEAVUAnimInstance@@AEBUFCopyOptions@1@@Z"),
				"_ZN12PersonaUtils19CopyPropertiesToCDOEP13UAnimInstanceRKNS_12FCopyOptionsE",
				reinterpret_cast<void*>(&CopyAnimPropertiesToCDO), TEXT("UnrealEditor-Persona.dll"), "UnrealEditor-Persona"))
		{
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Persona script AnimInstance copy guard unavailable; Stock Persona path remains."));
		}
	}
	else if (ModuleName == TEXT("Kismet"))
	{
		if (!KismetWarningEntry.Original()
			&& !KismetWarningEntry.Install(
				TEXT("?OnInheritedBlueprintComponentWarningHyperlinkClicked@SKismetInspector@@")
				TEXT("IEAAXAEBV?$TMap@VFString@@V1@VFDefaultSetAllocator@@U?$TDefaultMapHashableKeyFuncs@VFString@@V1@$0A@@@@@@Z"),
				"_ZN16SKismetInspector52OnInheritedBlueprintComponentWarningHyperlinkClickedERK4TMapI7FString"
				"S1_20FDefaultSetAllocator27TDefaultMapHashableKeyFuncsIS1_S1_Lb0EEE",
				reinterpret_cast<void*>(&OnInheritedBlueprintComponentWarningHyperlinkClicked),
				TEXT("UnrealEditor-Kismet.dll"), "UnrealEditor-Kismet"))
		{
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Kismet script-parent warning hook unavailable; Stock warning click remains."));
		}
	}
	else if (ModuleName == TEXT("AIModule"))
	{
		if (!EnvQueryTitleEntry.Original()
			&& !EnvQueryTitleEntry.Install(
				TEXT("?GetShortTypeName@UEnvQueryTypes@@SA?AVFText@@PEBVUObject@@@Z"),
				"_ZN14UEnvQueryTypes16GetShortTypeNameEPK7UObject",
				reinterpret_cast<void*>(&GetEnvQueryShortTypeName),
				TEXT("UnrealEditor-AIModule.dll"), "UnrealEditor-AIModule"))
		{
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] EQS script node title hook unavailable; Stock display text remains."));
		}
	}
}

void InstallAlreadyLoadedModules()
{
	const FName Modules[] = {
		FName(TEXT("Engine")), FName(TEXT("UnrealEd")), FName(TEXT("Persona")),
		FName(TEXT("Kismet")), FName(TEXT("AIModule"))};
	for (const FName ModuleName : Modules)
	{
		if (FModuleManager::Get().IsModuleLoaded(ModuleName))
		{
			InstallHookForLoadedModule(ModuleName);
		}
	}
}

void OnModulesChanged(FName ModuleName, EModuleChangeReason Reason)
{
	if (Reason == EModuleChangeReason::ModuleLoaded)
	{
		RefreshCallerRanges();
		InstallHookForLoadedModule(ModuleName);
	}
	else if (Reason == EModuleChangeReason::ModuleUnloaded)
	{
		RefreshCallerRanges(ModuleName);
	}
}
}

namespace AngelscriptBpgcSemanticsHooks
{
void Initialize()
{
	if (!GIsEditor || bBpgcSemanticsInitialized)
	{
		return;
	}
	bBpgcSemanticsInitialized = true;
	bRunningCommandlet = IsRunningCommandlet();
	ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddStatic(&OnModulesChanged);
	RefreshCallerRanges();
	InstallAlreadyLoadedModules();
}

void Shutdown()
{
	if (!bBpgcSemanticsInitialized)
	{
		return;
	}
	if (ModulesChangedHandle.IsValid())
	{
		FModuleManager::Get().OnModulesChanged().Remove(ModulesChangedHandle);
		ModulesChangedHandle.Reset();
	}
	EnvQueryTitleEntry.Remove();
	KismetWarningEntry.Remove();
	AnimCdoCopyEntry.Remove();
	ActorCdoCopyEntry.Remove();
	WorldPartitionLogicEntry.Remove();
	{
		FScopeLock Lock(&CallerRangeCacheMutex);
		ActiveCallerRanges.store(nullptr, std::memory_order_seq_cst);
		if (ActiveCallerRangeReaders.load(std::memory_order_seq_cst) == 0)
		{
			RetainedCallerRanges.Reset();
			bCallerRangeReclaimPending.store(false, std::memory_order_seq_cst);
		}
		else
		{
			bCallerRangeReclaimPending.store(true, std::memory_order_seq_cst);
		}
#if WITH_DEV_AUTOMATION_TESTS
		TestCallerRegistrations.Reset();
#endif
	}
	bRunningCommandlet = false;
	bBpgcSemanticsInitialized = false;
}
}

#if WITH_DEV_AUTOMATION_TESTS
namespace AngelscriptBpgcSemanticsHooks::Tests
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
}
#endif

#undef SOLOAS_BPGC_CALLER_ADDRESS
#endif
