#pragma once

#include "CoreMinimal.h"

class UClass;

namespace SoloASBridge
{
/**
 * Returns the state registered by Owner for Class, or nullptr when no matching
 * registration exists.
 *
 * The returned pointer is borrowed. The consumer owns its lifetime and must
 * quiesce readers before replacing, clearing, or destroying the state.
 */
SOLOASBRIDGE_API void* GetClassState(
    const UClass* Class,
    const void* Owner
);

/**
 * Atomically installs or replaces Owner's state for Class.
 *
 * A different owner cannot replace an existing registration. Null arguments
 * are rejected.
 */
SOLOASBRIDGE_API bool SetClassState(
    const UClass* Class,
    const void* Owner,
    void* State
);

/**
 * Clears a registration only when Owner and ExpectedState both match.
 *
 * Null arguments and stale expected-state values are rejected.
 */
SOLOASBRIDGE_API bool ClearClassState(
    const UClass* Class,
    const void* Owner,
    const void* ExpectedState
);

/**
 * Clears every class-state registration owned by Owner.
 *
 * The consumer must quiesce readers before calling this function and before
 * destroying any associated state.
 */
SOLOASBRIDGE_API void ClearClassStates(const void* Owner);
}
