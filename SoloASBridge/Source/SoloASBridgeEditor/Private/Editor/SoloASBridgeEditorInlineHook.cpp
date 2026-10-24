// Copyright Cuviss Studio. All Rights Reserved.

#include "Editor/SoloASBridgeEditorInlineHook.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)

#include "HAL/PlatformProcess.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "detours/detours.h"
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_MAC
#include "Mac/MacSystemIncludes.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <libkern/OSCacheControl.h>
#if defined(__x86_64__)
#include <mach/i386/thread_status.h>
#elif defined(__arm64__) || defined(__aarch64__)
#include <mach/arm/thread_status.h>
#endif
#endif

namespace SoloASBridge::Editor
{
#if PLATFORM_MAC
namespace
{
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

	const uint8 Opcode = Bytes[Cursor++];
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
		if (Opcode == 0x83) ImmediateSize = 1;
		else if (Opcode == 0x81) ImmediateSize = bOperand16 ? 2 : 4;
	}

	if (bHasModRm)
	{
		const uint8 ModRm = Bytes[Cursor++];
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

bool WriteAbsoluteJump(uint8* Destination, const void* Address)
{
#if defined(__x86_64__)
	const uint8 Jump[] = { 0xFF, 0x25, 0, 0, 0, 0 };
	FMemory::Memcpy(Destination, Jump, sizeof(Jump));
	const uint64 Target = reinterpret_cast<uint64>(Address);
	FMemory::Memcpy(Destination + sizeof(Jump), &Target, sizeof(Target));
	return true;
#elif defined(__arm64__) || defined(__aarch64__)
	const uint32 Jump[] = { 0x58000050u, 0xD61F0200u };
	FMemory::Memcpy(Destination, Jump, sizeof(Jump));
	const uint64 Target = reinterpret_cast<uint64>(Address);
	FMemory::Memcpy(Destination + sizeof(Jump), &Target, sizeof(Target));
	return true;
#else
	return false;
#endif
}

bool AllocateTrampoline(FEditorInlineHookHandle& Handle, const uint8* Target)
{
#if !defined(__x86_64__) && !defined(__arm64__) && !defined(__aarch64__)
	return false;
#else
	mach_vm_address_t TrampolineAddress = 0;
	const mach_vm_size_t AllocationSize = static_cast<mach_vm_size_t>(vm_page_size);
	if (mach_vm_allocate(mach_task_self(), &TrampolineAddress, AllocationSize,
		VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
		return false;
	uint8* Trampoline = reinterpret_cast<uint8*>(TrampolineAddress);
	SIZE_T StolenBytes = 0;
#if defined(__x86_64__)
	constexpr SIZE_T PatchSize = 14;
	while (StolenBytes < PatchSize)
	{
		int32 InstructionSize = 0;
		if (!IsX64PcRelative(Target + StolenBytes, InstructionSize)
			|| StolenBytes + InstructionSize > sizeof(Handle.OriginalBytes))
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
	FMemory::Memcpy(Handle.OriginalBytes, Target, StolenBytes);
	Handle.OriginalByteCount = StolenBytes;
	FMemory::Memcpy(Trampoline, Handle.OriginalBytes, StolenBytes);
	if (!WriteAbsoluteJump(Trampoline + StolenBytes, Target + StolenBytes)
		|| mach_vm_protect(mach_task_self(), TrampolineAddress, AllocationSize, false,
			VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS)
		goto Failure;
	sys_icache_invalidate(Trampoline, StolenBytes + 16);
	Handle.TrampolineAddress = reinterpret_cast<void*>(TrampolineAddress);
	Handle.TrampolineAllocationSize = AllocationSize;
	return true;

Failure:
	mach_vm_deallocate(mach_task_self(), TrampolineAddress, AllocationSize);
	Handle.OriginalByteCount = 0;
	return false;
#endif
}

bool WriteCode(void* Address, const uint8* Bytes, SIZE_T ByteCount)
{
	const mach_vm_address_t CodeAddress = reinterpret_cast<mach_vm_address_t>(Address);
	const mach_vm_address_t Page = CodeAddress & ~(static_cast<mach_vm_address_t>(vm_page_size) - 1);
	if ((CodeAddress + ByteCount - 1) / vm_page_size != Page / vm_page_size)
		return false;

	mach_vm_address_t RegionAddress = Page;
	mach_vm_size_t RegionSize = 0;
	vm_region_basic_info_data_64_t RegionInfo{};
	mach_msg_type_number_t RegionInfoCount = VM_REGION_BASIC_INFO_COUNT_64;
	mach_port_t ObjectName = MACH_PORT_NULL;
	const kern_return_t Query = mach_vm_region(mach_task_self(), &RegionAddress, &RegionSize,
		VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&RegionInfo),
		&RegionInfoCount, &ObjectName);
	if (ObjectName != MACH_PORT_NULL)
		mach_port_deallocate(mach_task_self(), ObjectName);
	if (Query != KERN_SUCCESS || Page < RegionAddress
		|| Page + vm_page_size > RegionAddress + RegionSize)
		return false;
	if (mach_vm_protect(mach_task_self(), Page, vm_page_size, false,
		VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
		return false;

	FMemory::Memcpy(Address, Bytes, ByteCount);
	sys_icache_invalidate(Address, ByteCount);
	kern_return_t Restore = mach_vm_protect(mach_task_self(), Page, vm_page_size,
		false, RegionInfo.protection);
	if (Restore != KERN_SUCCESS)
		Restore = mach_vm_protect(mach_task_self(), Page, vm_page_size,
			false, RegionInfo.protection);
	if (Restore != KERN_SUCCESS)
		UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Could not restore editor code page protection after an inline hook write."));
	return true;
}

bool SuspendThreads(void* PatchAddress, SIZE_T PatchSize, TArray<thread_t>& Suspended)
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
		if (thread_get_state(Thread, x86_THREAD_STATE64,
			reinterpret_cast<thread_state_t>(&State), &StateCount) != KERN_SUCCESS)
			bSucceeded = false;
		else
			ProgramCounter = State.__rip;
#elif defined(__arm64__) || defined(__aarch64__)
		arm_thread_state64_t State{};
		mach_msg_type_number_t StateCount = ARM_THREAD_STATE64_COUNT;
		if (thread_get_state(Thread, ARM_THREAD_STATE64,
			reinterpret_cast<thread_state_t>(&State), &StateCount) != KERN_SUCCESS)
			bSucceeded = false;
		else
			ProgramCounter = State.__pc;
#else
		bSucceeded = false;
#endif
		if (!bSucceeded || (ProgramCounter >= Start && ProgramCounter < Start + PatchSize))
			bSucceeded = false;
	}
	mach_port_deallocate(mach_task_self(), Current);
	vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(Threads),
		ThreadCount * sizeof(thread_t));
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
}
#endif

bool InstallEditorInlineHook(
	FEditorInlineHookHandle& Handle, void* Target, void* Replacement,
	const TCHAR* HookName)
{
	if (Handle.Original != nullptr || Target == nullptr || Replacement == nullptr)
		return false;
	UPTRINT ModuleStart = 0, ModuleEnd = 0;
	if (!GetEditorHookModuleRange(Target, ModuleStart, ModuleEnd))
		return false;
#if PLATFORM_WINDOWS
	Handle.Original = Target;
	LONG Result = DetourTransactionBegin();
	if (Result == NO_ERROR)
	{
		Result = DetourUpdateThread(GetCurrentThread());
		if (Result == NO_ERROR)
			Result = DetourAttach(reinterpret_cast<PVOID*>(&Handle.Original), Replacement);
		if (Result == NO_ERROR)
			Result = DetourTransactionCommit();
		else
			DetourTransactionAbort();
	}
	if (Result != NO_ERROR)
	{
		Handle.Original = nullptr;
		UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Could not install editor inline hook: %s"), HookName);
		return false;
	}
	Handle.TargetModuleStart = ModuleStart;
	Handle.TargetModuleEnd = ModuleEnd;
	return true;
#elif PLATFORM_MAC
	if (!AllocateTrampoline(Handle, static_cast<const uint8*>(Target)))
		return false;
	uint8 Patch[32];
	FMemory::Memset(Patch, 0x90, sizeof(Patch));
	if (!WriteAbsoluteJump(Patch, Replacement))
	{
		mach_vm_deallocate(mach_task_self(),
			reinterpret_cast<mach_vm_address_t>(Handle.TrampolineAddress),
			Handle.TrampolineAllocationSize);
		Handle.TrampolineAddress = nullptr;
		Handle.TrampolineAllocationSize = 0;
		Handle.OriginalByteCount = 0;
		return false;
	}
	TArray<thread_t> Suspended;
	if (!SuspendThreads(Target, Handle.OriginalByteCount, Suspended))
	{
		ResumeThreads(Suspended);
		mach_vm_deallocate(mach_task_self(),
			reinterpret_cast<mach_vm_address_t>(Handle.TrampolineAddress),
			Handle.TrampolineAllocationSize);
		Handle.TrampolineAddress = nullptr;
		Handle.TrampolineAllocationSize = 0;
		Handle.OriginalByteCount = 0;
		return false;
	}
	const bool bPatched = WriteCode(Target, Patch, Handle.OriginalByteCount);
	ResumeThreads(Suspended);
	if (!bPatched)
	{
		mach_vm_deallocate(mach_task_self(),
			reinterpret_cast<mach_vm_address_t>(Handle.TrampolineAddress),
			Handle.TrampolineAllocationSize);
		Handle.TrampolineAddress = nullptr;
		Handle.TrampolineAllocationSize = 0;
		Handle.OriginalByteCount = 0;
		return false;
	}
	Handle.Original = Handle.TrampolineAddress;
	Handle.TargetAddress = Target;
	Handle.TargetModuleStart = ModuleStart;
	Handle.TargetModuleEnd = ModuleEnd;
	return true;
#endif
}

bool RemoveEditorInlineHook(
	FEditorInlineHookHandle& Handle, void* Replacement,
	const TCHAR* HookName)
{
	if (Handle.Original == nullptr)
		return true;
#if PLATFORM_WINDOWS
	LONG Result = DetourTransactionBegin();
	if (Result == NO_ERROR)
	{
		Result = DetourUpdateThread(GetCurrentThread());
		if (Result == NO_ERROR)
			Result = DetourDetach(reinterpret_cast<PVOID*>(&Handle.Original), Replacement);
		if (Result == NO_ERROR)
			Result = DetourTransactionCommit();
		else
			DetourTransactionAbort();
	}
	if (Result != NO_ERROR)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Could not restore editor inline hook: %s"), HookName);
		return false;
	}
	Handle.Original = nullptr;
	Handle.TargetModuleStart = 0;
	Handle.TargetModuleEnd = 0;
	return true;
#elif PLATFORM_MAC
	TArray<thread_t> Suspended;
	if (!SuspendThreads(Handle.TargetAddress, Handle.OriginalByteCount, Suspended))
	{
		ResumeThreads(Suspended);
		UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Could not suspend editor threads to restore inline hook: %s"), HookName);
		return false;
	}
	const bool bRestored = WriteCode(Handle.TargetAddress,
		Handle.OriginalBytes, Handle.OriginalByteCount);
	ResumeThreads(Suspended);
	if (!bRestored)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SoloAS] Could not restore editor inline hook: %s"), HookName);
		return false;
	}
	mach_vm_deallocate(mach_task_self(),
		reinterpret_cast<mach_vm_address_t>(Handle.TrampolineAddress),
		Handle.TrampolineAllocationSize);
	Handle = FEditorInlineHookHandle{};
	return true;
#endif
}
}

#endif
