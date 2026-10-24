// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * Keeps borrowed AngelScript execution state stable for the lifetime of the
 * scope. Outermost readers are rejected after execution shutdown begins.
 */
class ANGELSCRIPTCODE_API FAngelscriptExecutionReadScope
{
public:
	FAngelscriptExecutionReadScope();
	~FAngelscriptExecutionReadScope() noexcept;

	FAngelscriptExecutionReadScope(
		const FAngelscriptExecutionReadScope&
	) = delete;
	FAngelscriptExecutionReadScope& operator=(
		const FAngelscriptExecutionReadScope&
	) = delete;

private:
	bool bAcquired = false;
};

/**
 * Drains execution readers and excludes new readers while mutable runtime
 * state is replaced. Writers are restricted to the game thread.
 */
class ANGELSCRIPTCODE_API FAngelscriptExecutionWriteScope
{
public:
	// GarbageCollection only finalizes dying objects; it never permits source
	// publication. The current script reader remains visible to Try acquisition.
	// With no current reader it acquires the same waiting writer as Wait.
	enum class EAcquire : uint8 { Wait, Try, GarbageCollection };
	FAngelscriptExecutionWriteScope(EAcquire Acquire = EAcquire::Wait);
	/** Try never waits or upgrades a reader. A held scope fences retirement. */
	bool IsAcquired() const { return bAcquired; }
	~FAngelscriptExecutionWriteScope() noexcept;

	FAngelscriptExecutionWriteScope(
		const FAngelscriptExecutionWriteScope&
	) = delete;
	FAngelscriptExecutionWriteScope& operator=(
		const FAngelscriptExecutionWriteScope&
	) = delete;

private:
	bool bAcquired = false;
	bool bBorrowedReader = false;
};

/**
 * Permanently rejects new outermost execution readers.
 *
 * Acquire a write scope after this call to drain readers before clearing
 * borrowed runtime state.
 */
ANGELSCRIPTCODE_API void BeginAngelscriptExecutionShutdown();

/** Returns whether permanent AngelScript execution shutdown has begun. */
ANGELSCRIPTCODE_API bool IsAngelscriptExecutionShutdownStarted();

/** Returns whether the current thread owns the execution write scope. */
ANGELSCRIPTCODE_API bool
IsAngelscriptExecutionWriterHeldByCurrentThread();

/**
 * Verifies that class-state mutation is covered by the execution protocol.
 *
 * Initial compilation occurs before concurrent script execution is enabled
 * and is the only mutation path that does not require a write scope.
 */
ANGELSCRIPTCODE_API void CheckAngelscriptExecutionMutationAllowed(
	bool bIsInitialCompile
);
