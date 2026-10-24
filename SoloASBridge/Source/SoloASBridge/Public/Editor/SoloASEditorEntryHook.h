// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "HAL/PlatformProcess.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include <processsnapshot.h>
#include <detours/detours.h>
#include <intrin.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_MAC
#include "Mac/MacSystemIncludes.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <libkern/OSCacheControl.h>
#if defined(__x86_64__)
#include <mach/i386/thread_status.h>
#elif defined(__arm64__) || defined(__aarch64__)
#include <mach/arm/thread_status.h>
#endif
#endif

namespace SoloASBridge::Editor
{
FORCEINLINE const void* GetEditorHookCallerAddress()
{
#if PLATFORM_WINDOWS
	return _ReturnAddress();
#else
	return __builtin_return_address(0);
#endif
}

inline bool GetEditorHookModuleRange(const void* Address, UPTRINT& Start, UPTRINT& End)
{
	if (Address == nullptr)
		return false;
	const UPTRINT Candidate = reinterpret_cast<UPTRINT>(Address);
#if PLATFORM_WINDOWS
	MEMORY_BASIC_INFORMATION Region{};
	if (VirtualQuery(Address, &Region, sizeof(Region)) != sizeof(Region) || Region.AllocationBase == nullptr)
		return false;
	const auto* Base = static_cast<const uint8*>(Region.AllocationBase);
	const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
	if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0)
		return false;
	const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + Dos->e_lfanew);
	if (Nt->Signature != IMAGE_NT_SIGNATURE || Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC
		|| Nt->OptionalHeader.SizeOfImage == 0)
		return false;
	Start = reinterpret_cast<UPTRINT>(Base);
	if (Nt->OptionalHeader.SizeOfImage > static_cast<UPTRINT>(-1) - Start)
		return false;
	End = Start + Nt->OptionalHeader.SizeOfImage;
	return Candidate >= Start && Candidate < End;
#else
	Dl_info Info{};
	if (dladdr(Address, &Info) == 0 || Info.dli_fbase == nullptr)
		return false;
	const auto* Header = reinterpret_cast<const mach_header_64*>(Info.dli_fbase);
	if (Header->magic != MH_MAGIC_64)
		return false;
	intptr_t Slide = 0;
	bool bFoundImage = false;
	for (uint32 ImageIndex = 0; ImageIndex < _dyld_image_count(); ++ImageIndex)
	{
		if (_dyld_get_image_header(ImageIndex) == reinterpret_cast<const mach_header*>(Header))
		{
			Slide = _dyld_get_image_vmaddr_slide(ImageIndex);
			bFoundImage = true;
			break;
		}
	}
	if (!bFoundImage)
		return false;
	const uint8* Cursor = reinterpret_cast<const uint8*>(Header + 1);
	const uint8* CommandsEnd = Cursor + Header->sizeofcmds;
	UPTRINT ImageStart = static_cast<UPTRINT>(-1);
	UPTRINT ImageEnd = 0;
	for (uint32 Index = 0; Index < Header->ncmds && Cursor + sizeof(load_command) <= CommandsEnd; ++Index)
	{
		const auto* Command = reinterpret_cast<const load_command*>(Cursor);
		if (Command->cmdsize < sizeof(load_command) || Cursor + Command->cmdsize > CommandsEnd)
			return false;
		if (Command->cmd == LC_SEGMENT_64 && Command->cmdsize >= sizeof(segment_command_64))
		{
			const auto* Segment = reinterpret_cast<const segment_command_64*>(Command);
			if ((Segment->initprot & VM_PROT_EXECUTE) != 0)
			{
				const UPTRINT SegmentStart = static_cast<UPTRINT>(Slide + Segment->vmaddr);
				if (Segment->vmsize <= static_cast<UPTRINT>(-1) - SegmentStart)
				{
					ImageStart = FMath::Min(ImageStart, SegmentStart);
					ImageEnd = FMath::Max(ImageEnd, SegmentStart + static_cast<UPTRINT>(Segment->vmsize));
				}
			}
		}
		Cursor += Command->cmdsize;
	}
	if (ImageStart >= ImageEnd)
		return false;
	Start = ImageStart;
	End = ImageEnd;
	return Candidate >= Start && Candidate < End;
#endif
}
}

struct FSoloASEditorModuleImageRange
{
	bool Update(FName ModuleName)
	{
		Start = 0;
		End = 0;
		IModuleInterface* Module = FModuleManager::Get().GetModule(ModuleName);
		if (Module == nullptr)
			return false;
		const void* VTable = *reinterpret_cast<void* const*>(Module);
		return SoloASBridge::Editor::GetEditorHookModuleRange(VTable, Start, End);
	}

	void Reset()
	{
		Start = 0;
		End = 0;
	}

	bool Contains(const void* Address) const
	{
		const UPTRINT Candidate = reinterpret_cast<UPTRINT>(Address);
		return Start != 0 && Candidate >= Start && Candidate < End;
	}

	UPTRINT Start = 0;
	UPTRINT End = 0;
};

// Shared platform mechanism for the registered UnrealEd editor entry hooks.
class FSoloASEditorEntryHook
{
public:
    bool Install(const TCHAR* WindowsSymbol, const char* MacSymbol, void* Replacement,
        const TCHAR* WindowsLibrary = TEXT("UnrealEditor-UnrealEd.dll"),
        const char* MacLibrary = "UnrealEditor-UnrealEd")
    {
        if (GOriginalAddress) return false;
        EngineLibraryName = WindowsLibrary;
        MacLibraryName = MacLibrary;
        EngineFunctionName = WindowsSymbol;
        MacFunctionName = MacSymbol;
        RedirectAddress = Replacement;
        return InstallHook();
    }
    void Remove() { RemoveHook(); }
    void* Original() const { return GOriginalAddress; }
    bool IsCallerInTargetModule(const void* Caller) const
    {
		const UPTRINT Address = reinterpret_cast<UPTRINT>(Caller);
		return TargetModuleStart != 0 && Address >= TargetModuleStart && Address < TargetModuleEnd;
    }
private:
    void* GOriginalAddress = nullptr;
    void* RedirectAddress = nullptr;
    const TCHAR* EngineLibraryName = nullptr;
    const char* MacLibraryName = nullptr;
    const TCHAR* EngineFunctionName = nullptr;
    const char* MacFunctionName = nullptr;
	UPTRINT TargetModuleStart = 0;
	UPTRINT TargetModuleEnd = 0;
#if PLATFORM_WINDOWS
	void* GEngineLibraryHandle = nullptr;

    bool UpdateThreads(TArray<HANDLE>& Handles)
    {
        HPSS Snapshot = nullptr;
        if (PssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_THREADS, 0, &Snapshot) != ERROR_SUCCESS)
            return false;
        HPSSWALK Marker = nullptr;
        if (PssWalkMarkerCreate(nullptr, &Marker) != ERROR_SUCCESS)
        {
            PssFreeSnapshot(GetCurrentProcess(), Snapshot);
            return false;
        }
        PSS_THREAD_ENTRY Entry{};
        DWORD Result = ERROR_SUCCESS;
        bool Success = true;
        while ((Result = PssWalkSnapshot(Snapshot, PSS_WALK_THREADS, Marker, &Entry, sizeof(Entry))) == ERROR_SUCCESS)
        {
            // PSS also reports terminated threads, which cannot join the transaction.
            if ((Entry.Flags & PSS_THREAD_FLAGS_TERMINATED) != 0) continue;
            if (Entry.ThreadId != GetCurrentThreadId())
            {
                HANDLE Thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                    0, Entry.ThreadId);
                if (!Thread) { Success = false; break; }
                Handles.Add(Thread);
            }
        }
        // Finish snapshot allocation/free before Detours suspends other threads.
        PssWalkMarkerFree(Marker);
        PssFreeSnapshot(GetCurrentProcess(), Snapshot);
        if (!Success || Result != ERROR_NO_MORE_ITEMS
            || DetourUpdateThread(GetCurrentThread()) != NO_ERROR) return false;
        for (HANDLE Thread : Handles)
            if (DetourUpdateThread(Thread) != NO_ERROR) return false;
        return true;
    }
    void CloseThreads(TArray<HANDLE>& Handles)
    {
        for (HANDLE Thread : Handles) CloseHandle(Thread);
    }

	bool InstallHook()
	{
		void* Library = FPlatformProcess::GetDllHandle(EngineLibraryName);
		if (Library == nullptr)
			return false;

		void* Target = FPlatformProcess::GetDllExport(Library, EngineFunctionName);
		if (Target == nullptr)
		{
			FPlatformProcess::FreeDllHandle(Library);
			return false;
		}
		UPTRINT ModuleStart = 0, ModuleEnd = 0;
		if (!SoloASBridge::Editor::GetEditorHookModuleRange(Target, ModuleStart, ModuleEnd))
		{
			FPlatformProcess::FreeDllHandle(Library);
			return false;
		}

		GOriginalAddress = Target;
		LONG Result = DetourTransactionBegin();
		if (Result != NO_ERROR)
		{
			FPlatformProcess::FreeDllHandle(Library);
			GOriginalAddress = nullptr;
			return false;
		}
		TArray<HANDLE> Threads;
		Result = UpdateThreads(Threads) ? NO_ERROR : ERROR_ACCESS_DENIED;
		if (Result == NO_ERROR)
			Result = DetourAttach(&GOriginalAddress, RedirectAddress);
		if (Result == NO_ERROR)
			Result = DetourTransactionCommit();
		else
			DetourTransactionAbort();
		CloseThreads(Threads);

		if (Result != NO_ERROR)
		{
			FPlatformProcess::FreeDllHandle(Library);
			GOriginalAddress = nullptr;
			return false;
		}
		GEngineLibraryHandle = Library;
		TargetModuleStart = ModuleStart;
		TargetModuleEnd = ModuleEnd;
		return true;
	}

	void RemoveHook()
	{
		if (GOriginalAddress == nullptr)
			return;

		LONG Result = DetourTransactionBegin();
		if (Result != NO_ERROR)
		{
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Could not start UnrealEd editor entry detach transaction."));
			return;
		}
		TArray<HANDLE> Threads;
		Result = UpdateThreads(Threads) ? NO_ERROR : ERROR_ACCESS_DENIED;
		if (Result == NO_ERROR)
			Result = DetourDetach(&GOriginalAddress, RedirectAddress);
		if (Result == NO_ERROR)
			Result = DetourTransactionCommit();
		else
			DetourTransactionAbort();
		CloseThreads(Threads);

		if (Result == NO_ERROR)
		{
			GOriginalAddress = nullptr;
			TargetModuleStart = 0;
			TargetModuleEnd = 0;
			if (GEngineLibraryHandle != nullptr)
				FPlatformProcess::FreeDllHandle(GEngineLibraryHandle);
			GEngineLibraryHandle = nullptr;
		}
		else
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Could not restore UnrealEd editor entry hook."));
	}
#elif PLATFORM_MAC
	uint8 GOriginalBytes[32]{};
	SIZE_T GOriginalByteCount = 0;
	void* GTargetAddress = nullptr;
	mach_vm_address_t GTrampolineAddress = 0;
	mach_vm_size_t GTrampolineAllocationSize = 0;

	bool IsX64PcRelative(const uint8* Bytes, int32& OutLength)
	{
		int32 Cursor = 0;
		bool bOperand16 = false;
		bool bAddress32 = false;
		bool bRexW = false;
		for (int32 PrefixCount = 0; PrefixCount < 8; ++PrefixCount)
		{
			const uint8 Byte = Bytes[Cursor];
			if (Byte == 0x66) { bOperand16 = true; ++Cursor; }
			else if (Byte == 0x67) { bAddress32 = true; ++Cursor; }
			else if (Byte == 0xF0 || Byte == 0xF2 || Byte == 0xF3 || Byte == 0x2E
				|| Byte == 0x36 || Byte == 0x3E || Byte == 0x26 || Byte == 0x64 || Byte == 0x65)
				++Cursor;
			else if (Byte >= 0x40 && Byte <= 0x4F) { bRexW = (Byte & 8) != 0; ++Cursor; }
			else break;
		}

		uint8 Opcode = Bytes[Cursor++];
		bool bHasModRm = false;
		int32 ImmediateSize = 0;

		if (Opcode == 0xC4 || Opcode == 0xC5 || Opcode == 0x62)
			return false;
		if (Opcode == 0x0F)
		{
			const uint8 Opcode2 = Bytes[Cursor++];
			if (Opcode2 != 0x1E && Opcode2 != 0x1F)
				return false;
			bHasModRm = true;
		}
		else if ((Opcode >= 0x50 && Opcode <= 0x5F) || Opcode == 0x90 || Opcode == 0x9C
			|| Opcode == 0x9D || Opcode == 0xC3 || Opcode == 0xC9 || Opcode == 0xCC)
		{
			OutLength = Cursor;
			return true;
		}
		else if (Opcode == 0x68) ImmediateSize = bOperand16 ? 2 : 4;
		else if (Opcode == 0x6A) ImmediateSize = 1;
		else if (Opcode >= 0xB8 && Opcode <= 0xBF) ImmediateSize = bRexW ? 8 : (bOperand16 ? 2 : 4);
		else if (Opcode == 0xC2) ImmediateSize = 2;
		else if ((Opcode >= 0x70 && Opcode <= 0x7F) || Opcode == 0xE8 || Opcode == 0xE9
			|| Opcode == 0xEB || (Opcode >= 0xE0 && Opcode <= 0xE3))
			return false;
		else
		{
			bHasModRm = Opcode == 0x89 || Opcode == 0x8B || Opcode == 0x8D
				|| Opcode == 0x83 || Opcode == 0x81 || Opcode == 0x85 || Opcode == 0x87
				|| Opcode == 0x31 || Opcode == 0x33 || Opcode == 0x84 || Opcode == 0xFF;
			if (!bHasModRm)
				return false;
			if (Opcode == 0x83)
				ImmediateSize = 1;
			else if (Opcode == 0x81)
				ImmediateSize = bOperand16 ? 2 : 4;
		}

		uint8 ModRm = 0;
		if (bHasModRm)
		{
			ModRm = Bytes[Cursor++];
			const uint8 Mod = ModRm >> 6;
			const uint8 Rm = ModRm & 7;
			if (Mod != 3 && Rm == 4)
			{
				const uint8 Sib = Bytes[Cursor++];
				if (Mod == 0 && (Sib & 7) == 5)
					Cursor += 4;
			}
			else if (Mod == 0 && Rm == 5)
			{
				if (!bAddress32)
					return false;
				Cursor += 4;
			}
			if (Mod == 1) ++Cursor;
			else if (Mod == 2) Cursor += 4;
		}
		Cursor += ImmediateSize;
		if (Cursor <= 0 || Cursor > 15)
			return false;
		OutLength = Cursor;
		return true;
	}

	bool HasArmPcRelativeInstruction(const uint8* Bytes)
	{
		uint32 Instruction;
		FMemory::Memcpy(&Instruction, Bytes, sizeof(Instruction));
		return (Instruction & 0x1F000000u) == 0x10000000u
			|| (Instruction & 0x7C000000u) == 0x14000000u
			|| (Instruction & 0x3B000000u) == 0x18000000u
			|| (Instruction & 0xFF000010u) == 0x54000000u
			|| (Instruction & 0x7E000000u) == 0x34000000u
			|| (Instruction & 0x7E000000u) == 0x36000000u;
	}

	void WriteAbsoluteJump(uint8* Destination, const void* Address)
	{
#if defined(__x86_64__)
		const uint8 Jump[] = { 0xFF, 0x25, 0, 0, 0, 0 };
		FMemory::Memcpy(Destination, Jump, sizeof(Jump));
		const uint64 Target = reinterpret_cast<uint64>(Address);
		FMemory::Memcpy(Destination + sizeof(Jump), &Target, sizeof(Target));
#elif defined(__arm64__) || defined(__aarch64__)
		const uint32 Jump[] = { 0x58000050u, 0xD61F0200u };
		FMemory::Memcpy(Destination, Jump, sizeof(Jump));
		const uint64 Target = reinterpret_cast<uint64>(Address);
		FMemory::Memcpy(Destination + sizeof(Jump), &Target, sizeof(Target));
#endif
	}

	bool AllocateTrampoline(const uint8* Target)
	{
#if !defined(__x86_64__) && !defined(__arm64__) && !defined(__aarch64__)
		return false;
#else
		GTrampolineAllocationSize = static_cast<mach_vm_size_t>(vm_page_size);
		if (mach_vm_allocate(mach_task_self(), &GTrampolineAddress, GTrampolineAllocationSize, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
			return false;
		uint8* Trampoline = reinterpret_cast<uint8*>(GTrampolineAddress);

		SIZE_T StolenBytes = 0;
#if defined(__x86_64__)
		constexpr SIZE_T PatchSize = 14;
		while (StolenBytes < PatchSize)
		{
			int32 InstructionSize = 0;
			if (!IsX64PcRelative(Target + StolenBytes, InstructionSize) || StolenBytes + InstructionSize > sizeof(GOriginalBytes))
				goto Failure;
			StolenBytes += InstructionSize;
		}
#else
		constexpr SIZE_T PatchSize = 16;
		StolenBytes = PatchSize;
		for (SIZE_T Offset = 0; Offset < PatchSize; Offset += sizeof(uint32))
			if (HasArmPcRelativeInstruction(Target + Offset))
				goto Failure;
#endif
		FMemory::Memcpy(GOriginalBytes, Target, StolenBytes);
		GOriginalByteCount = StolenBytes;
		FMemory::Memcpy(Trampoline, GOriginalBytes, StolenBytes);
		WriteAbsoluteJump(Trampoline + StolenBytes, Target + StolenBytes);
		if (mach_vm_protect(mach_task_self(), GTrampolineAddress, GTrampolineAllocationSize, false,
			VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS)
			goto Failure;
		sys_icache_invalidate(Trampoline, StolenBytes + 16);
		GOriginalAddress = Trampoline;
		return true;

	Failure:
		mach_vm_deallocate(mach_task_self(), GTrampolineAddress, GTrampolineAllocationSize);
		GTrampolineAddress = 0;
		GTrampolineAllocationSize = 0;
		GOriginalByteCount = 0;
		return false;
#endif
	}

	bool WriteCode(void* Address, const uint8* Bytes, SIZE_T ByteCount)
	{
		const mach_vm_address_t Page = reinterpret_cast<mach_vm_address_t>(Address) & ~(static_cast<mach_vm_address_t>(vm_page_size) - 1);
		if ((reinterpret_cast<mach_vm_address_t>(Address) + ByteCount - 1) / vm_page_size != Page / vm_page_size)
			return false;

		mach_vm_address_t RegionAddress = Page;
		mach_vm_size_t RegionSize = 0;
		vm_region_basic_info_data_64_t RegionInfo{};
		mach_msg_type_number_t RegionInfoCount = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t ObjectName = MACH_PORT_NULL;
		const kern_return_t Query = mach_vm_region(mach_task_self(), &RegionAddress, &RegionSize,
			VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&RegionInfo), &RegionInfoCount, &ObjectName);
		if (ObjectName != MACH_PORT_NULL)
			mach_port_deallocate(mach_task_self(), ObjectName);
		if (Query != KERN_SUCCESS || Page < RegionAddress || Page + vm_page_size > RegionAddress + RegionSize)
			return false;

		const vm_prot_t WriteProtection = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY;
		if (mach_vm_protect(mach_task_self(), Page, vm_page_size, false, WriteProtection) != KERN_SUCCESS)
			return false;
		FMemory::Memcpy(Address, Bytes, ByteCount);
		sys_icache_invalidate(Address, ByteCount);
		kern_return_t Restore = mach_vm_protect(mach_task_self(), Page, vm_page_size, false, RegionInfo.protection);
		if (Restore != KERN_SUCCESS)
			Restore = mach_vm_protect(mach_task_self(), Page, vm_page_size, false, RegionInfo.protection);
		if (Restore != KERN_SUCCESS)
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] editor entry hook changed code but could not restore UnrealEd page protection."));
		return true;
	}

	bool SuspendThreads(void* PatchAddress, SIZE_T PatchSize, TArray<thread_t>& Suspended, bool bRemoving = false)
	{
		thread_act_array_t Threads = nullptr;
		mach_msg_type_number_t ThreadCount = 0;
		if (task_threads(mach_task_self(), &Threads, &ThreadCount) != KERN_SUCCESS)
			return false;
		const thread_t Current = mach_thread_self();
		const mach_vm_address_t Start = reinterpret_cast<mach_vm_address_t>(PatchAddress);
		bool bSucceeded = true;
		for (mach_msg_type_number_t Index = 0; Index < ThreadCount; ++Index)
		{
			const thread_t Thread = Threads[Index];
			if (Thread == Current)
			{
				mach_port_deallocate(mach_task_self(), Thread);
				continue;
			}
			if (thread_suspend(Thread) != KERN_SUCCESS)
			{
				bSucceeded = false;
				mach_port_deallocate(mach_task_self(), Thread);
				continue;
			}
			Suspended.Add(Thread);

			mach_vm_address_t ProgramCounter = 0;
#if defined(__x86_64__)
			x86_thread_state64_t State{};
			mach_msg_type_number_t StateCount = x86_THREAD_STATE64_COUNT;
			if (thread_get_state(Thread, x86_THREAD_STATE64, reinterpret_cast<thread_state_t>(&State), &StateCount) != KERN_SUCCESS)
				bSucceeded = false;
			else
				ProgramCounter = State.__rip;
#elif defined(__arm64__) || defined(__aarch64__)
			arm_thread_state64_t State{};
			mach_msg_type_number_t StateCount = ARM_THREAD_STATE64_COUNT;
			if (thread_get_state(Thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&State), &StateCount) != KERN_SUCCESS)
				bSucceeded = false;
			else
				ProgramCounter = State.__pc;
#else
			bSucceeded = false;
#endif
			if (!bSucceeded || (ProgramCounter >= Start && ProgramCounter < Start + PatchSize)
                || (bRemoving && ProgramCounter >= GTrampolineAddress
                    && ProgramCounter < GTrampolineAddress + GOriginalByteCount + 16))
			{
				bSucceeded = false;
			}
		}
		mach_port_deallocate(mach_task_self(), Current);
		vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(Threads), ThreadCount * sizeof(thread_t));
		return bSucceeded;
	}

	void ResumeThreads(TArray<thread_t>& Suspended)
	{
		for (thread_t Thread : Suspended)
		{
			thread_resume(Thread);
			mach_port_deallocate(mach_task_self(), Thread);
		}
		Suspended.Reset();
	}

	bool InstallHook()
	{
		void* Target = dlsym(RTLD_DEFAULT, MacFunctionName);
		Dl_info SymbolInfo{};
		if (Target == nullptr || dladdr(Target, &SymbolInfo) == 0 || SymbolInfo.dli_fname == nullptr
			|| FCStringAnsi::Strstr(SymbolInfo.dli_fname, MacLibraryName) == nullptr)
			return false;
		UPTRINT ModuleStart = 0, ModuleEnd = 0;
		if (!SoloASBridge::Editor::GetEditorHookModuleRange(Target, ModuleStart, ModuleEnd))
			return false;
		if (!AllocateTrampoline(static_cast<const uint8*>(Target)))
			return false;

		uint8 Patch[32];
		FMemory::Memset(Patch, 0x90, sizeof(Patch));
		WriteAbsoluteJump(Patch, RedirectAddress);
		const SIZE_T PatchSize = GOriginalByteCount;
		TArray<thread_t> Suspended;
		if (!SuspendThreads(Target, PatchSize, Suspended))
		{
			ResumeThreads(Suspended);
			mach_vm_deallocate(mach_task_self(), GTrampolineAddress, GTrampolineAllocationSize);
			GOriginalAddress = nullptr;
			GTrampolineAddress = 0;
			GTrampolineAllocationSize = 0;
			GOriginalByteCount = 0;
			return false;
		}
		const bool bPatched = FMemory::Memcmp(Target, GOriginalBytes, PatchSize) == 0
			&& WriteCode(Target, Patch, PatchSize);
		ResumeThreads(Suspended);
		if (!bPatched)
		{
			mach_vm_deallocate(mach_task_self(), GTrampolineAddress, GTrampolineAllocationSize);
			GOriginalAddress = nullptr;
			GTrampolineAddress = 0;
			GTrampolineAllocationSize = 0;
			GOriginalByteCount = 0;
			return false;
		}
		GTargetAddress = Target;
		TargetModuleStart = ModuleStart;
		TargetModuleEnd = ModuleEnd;
		return true;
	}

	void RemoveHook()
	{
		if (GTargetAddress == nullptr || GOriginalByteCount == 0)
			return;
		TArray<thread_t> Suspended;
		if (!SuspendThreads(GTargetAddress, GOriginalByteCount, Suspended, true))
		{
			ResumeThreads(Suspended);
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Could not suspend editor threads to restore UnrealEd editor entry."));
			return;
		}
		const bool bRestored = WriteCode(GTargetAddress, GOriginalBytes, GOriginalByteCount);
		ResumeThreads(Suspended);
		if (!bRestored)
		{
			UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Could not restore UnrealEd editor entry hook."));
			return;
		}
		mach_vm_deallocate(mach_task_self(), GTrampolineAddress, GTrampolineAllocationSize);
		GOriginalAddress = nullptr;
		GTargetAddress = nullptr;
		GTrampolineAddress = 0;
		GTrampolineAllocationSize = 0;
		GOriginalByteCount = 0;
		TargetModuleStart = 0;
		TargetModuleEnd = 0;
	}
#endif
};

#endif
