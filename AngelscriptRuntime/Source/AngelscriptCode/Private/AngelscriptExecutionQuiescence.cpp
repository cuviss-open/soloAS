// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptExecutionQuiescence.h"

#include "Experimental/Async/ConditionVariable.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTLS.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(
	LogAngelscriptExecutionQuiescence,
	Log,
	All
);

namespace
{
// Execution gate precedes Bridge class-state registry lock.
struct FAngelscriptExecutionGate
{
	FCriticalSection Mutex;
	UE::FConditionVariable StateChanged;

	int32 ActiveReaders = 0;
	int32 WaitingWriters = 0;
	uint32 WriterThreadId = 0;
	bool bShutdownPermanent = false;
};

// The gate intentionally has process lifetime. This avoids a global
// destructor racing late scope destruction during module shutdown.
FAngelscriptExecutionGate& GetAngelscriptExecutionGate()
{
	static FAngelscriptExecutionGate* Gate =
		new FAngelscriptExecutionGate();
	return *Gate;
}

thread_local int32 ReaderDepth = 0;
thread_local int32 WriterDepth = 0;
}

FAngelscriptExecutionReadScope::FAngelscriptExecutionReadScope()
{
	if (ReaderDepth > 0)
	{
		++ReaderDepth;
		bAcquired = true;
		return;
	}

	FAngelscriptExecutionGate& Gate = GetAngelscriptExecutionGate();

	// A writer-owner read is already protected by the exclusive scope.
	if (WriterDepth > 0)
	{
		checkf(
			Gate.WriterThreadId == FPlatformTLS::GetCurrentThreadId(),
			TEXT("AngelScript execution writer ownership is inconsistent.")
		);
		ReaderDepth++;
		bAcquired = true;
		return;
	}

	Gate.Mutex.Lock();
	while (
		!Gate.bShutdownPermanent
		&& (
			Gate.WaitingWriters > 0
			|| Gate.WriterThreadId != 0
		)
	)
	{
		Gate.StateChanged.Wait(Gate.Mutex);
	}

	const bool bExecutionAllowed = ensureMsgf(
		!Gate.bShutdownPermanent,
		TEXT(
			"AngelScript execution started after permanent runtime "
			"shutdown."
		)
	);
	if (!bExecutionAllowed)
	{
		Gate.Mutex.Unlock();
		UE_LOG(
			LogAngelscriptExecutionQuiescence,
			Fatal,
			TEXT(
				"AngelScript execution cannot continue after permanent "
				"runtime shutdown."
			)
		);
		return;
	}

	++Gate.ActiveReaders;
	ReaderDepth = 1;
	bAcquired = true;
	Gate.Mutex.Unlock();
}

FAngelscriptExecutionReadScope::~FAngelscriptExecutionReadScope() noexcept
{
	if (!bAcquired)
	{
		return;
	}

	checkf(
		ReaderDepth > 0,
		TEXT("AngelScript execution reader depth underflow.")
	);
	--ReaderDepth;
	if (ReaderDepth > 0 || WriterDepth > 0)
	{
		return;
	}

	FAngelscriptExecutionGate& Gate = GetAngelscriptExecutionGate();
	Gate.Mutex.Lock();
	checkf(
		Gate.ActiveReaders > 0,
		TEXT("AngelScript active execution reader count underflow.")
	);
	--Gate.ActiveReaders;
	if (Gate.ActiveReaders == 0)
	{
		Gate.StateChanged.NotifyAll();
	}
	Gate.Mutex.Unlock();
}

FAngelscriptExecutionWriteScope::FAngelscriptExecutionWriteScope(EAcquire Acquire)
{
	checkf(
		IsInGameThread(),
		TEXT("AngelScript execution mutation is restricted to the game thread.")
	);

	// Even a writer-owned script callback must return before another reload.
	if (Acquire == EAcquire::Try && ReaderDepth > 0)
		return;

	if (WriterDepth > 0)
	{
		++WriterDepth;
		bAcquired = true;
		return;
	}

	checkf(
		ReaderDepth == 0 || Acquire == EAcquire::GarbageCollection,
		TEXT(
			"AngelScript execution read scope cannot be upgraded to a "
			"write scope."
		)
	);
	// Without a current reader this is an ordinary waiting writer. Only borrowing
	// an interrupted reader needs engine GC/purge ownership, including later ticks.
	checkf(ReaderDepth == 0 || Acquire != EAcquire::GarbageCollection
		|| IsGarbageCollecting() || IsIncrementalPurgePending() || GExitPurge,
		TEXT("AngelScript GC reader upgrade requires engine garbage collection or purge."));

	FAngelscriptExecutionGate& Gate = GetAngelscriptExecutionGate();
	const uint32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();

	Gate.Mutex.Lock();
	if (Acquire == EAcquire::Try
		&& (Gate.ActiveReaders != 0 || Gate.WriterThreadId != 0 || Gate.bShutdownPermanent))
	{
		Gate.Mutex.Unlock();
		return;
	}
	++Gate.WaitingWriters;
	// Keep ReaderDepth intact: CompileModules/ScriptUnits Try acquisition must
	// still defer while the interrupted script (including its native call) lives.
	// Transfer only this thread's single outer reader into exclusive ownership.
	bBorrowedReader = ReaderDepth > 0;
	if (bBorrowedReader)
	{
		check(Gate.ActiveReaders > 0);
		--Gate.ActiveReaders;
	}
	int32& ActiveReaders = Gate.ActiveReaders;
	while (ActiveReaders != 0)
	{
		Gate.StateChanged.Wait(Gate.Mutex);
	}
	while (Gate.WriterThreadId != 0)
	{
		Gate.StateChanged.Wait(Gate.Mutex);
	}

	--Gate.WaitingWriters;
	Gate.WriterThreadId = CurrentThreadId;
	WriterDepth = 1;
	bAcquired = true;
	Gate.Mutex.Unlock();
}

FAngelscriptExecutionWriteScope::~FAngelscriptExecutionWriteScope() noexcept
{
	if (!bAcquired)
	{
		return;
	}

	checkf(
		WriterDepth > 0,
		TEXT("AngelScript execution writer depth underflow.")
	);
	--WriterDepth;
	if (WriterDepth > 0)
	{
		return;
	}

	FAngelscriptExecutionGate& Gate = GetAngelscriptExecutionGate();
	const uint32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
	Gate.Mutex.Lock();
	checkf(
		Gate.WriterThreadId == CurrentThreadId,
		TEXT("AngelScript execution write scope released by a non-owner.")
	);
	Gate.WriterThreadId = 0;
	if (bBorrowedReader)
	{
		check(ReaderDepth > 0);
		++Gate.ActiveReaders;
	}
	Gate.StateChanged.NotifyAll();
	Gate.Mutex.Unlock();
}

void BeginAngelscriptExecutionShutdown()
{
	FAngelscriptExecutionGate& Gate = GetAngelscriptExecutionGate();
	Gate.Mutex.Lock();
	Gate.bShutdownPermanent = true;
	Gate.StateChanged.NotifyAll();
	Gate.Mutex.Unlock();
}

bool IsAngelscriptExecutionShutdownStarted()
{
	FAngelscriptExecutionGate& Gate = GetAngelscriptExecutionGate();
	Gate.Mutex.Lock();
	const bool bShutdownStarted = Gate.bShutdownPermanent;
	Gate.Mutex.Unlock();
	return bShutdownStarted;
}

bool IsAngelscriptExecutionWriterHeldByCurrentThread()
{
	return WriterDepth > 0;
}

void CheckAngelscriptExecutionMutationAllowed(bool bIsInitialCompile)
{
	checkf(
		bIsInitialCompile
		|| IsAngelscriptExecutionWriterHeldByCurrentThread(),
		TEXT(
			"AngelScript runtime class-state mutation requires either "
			"initial compilation or the execution write scope."
		)
	);
}
