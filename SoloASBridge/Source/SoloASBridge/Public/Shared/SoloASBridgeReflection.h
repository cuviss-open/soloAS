#pragma once

#include "CoreMinimal.h"
#include "Shared/SoloASBridgeScriptMetadataProvider.h"

class FProperty;
class UFunction;
class UClass;

namespace SoloASBridge
{
/** Registers binary-safe mixin callable/reference traits for one native class. */
SOLOASBRIDGE_API void RegisterBindingClassTraits(UClass* Class, FName Owner,
    bool bScriptCallable, UClass* NativeTarget = nullptr);
/** Registers C++ reference parameters from the loaded classes of one module. */
SOLOASBRIDGE_API void RegisterBindingModuleReferenceTraits(FName Module, FName Owner);
/** Opens the reflection trait registry for game-thread registrations. */
SOLOASBRIDGE_API void InitializeReflectionTraits();

/** Stops new reflection trait registrations permanently for this module load. */
SOLOASBRIDGE_API void ShutdownReflectionTraits();

/**
 * Installs or replaces property traits owned by RegistrationOwner.
 *
 * A different registration owner cannot overwrite the same field identity.
 */
SOLOASBRIDGE_API bool SetPropertyTraits(
    const FProperty* Property,
    FName RegistrationOwner,
    ESoloASBridgePropertyTrait Traits
);

/** Copies the traits for Property into OutTraits. */
SOLOASBRIDGE_API bool GetPropertyTraits(
    const FProperty* Property,
    ESoloASBridgePropertyTrait& OutTraits
);

/** Clears property traits only when RegistrationOwner matches. */
SOLOASBRIDGE_API bool ClearPropertyTraits(
    const FProperty* Property,
    FName RegistrationOwner
);

/** Clears all property-trait registrations owned by RegistrationOwner. */
SOLOASBRIDGE_API void ClearPropertyTraits(FName RegistrationOwner);

/**
 * Installs or replaces function traits owned by RegistrationOwner.
 *
 * A different registration owner cannot overwrite the same field identity.
 */
SOLOASBRIDGE_API bool SetFunctionTraits(
    const UFunction* Function,
    FName RegistrationOwner,
    ESoloASBridgeFunctionTrait Traits
);

/** Copies the traits for Function into OutTraits. */
SOLOASBRIDGE_API bool GetFunctionTraits(
    const UFunction* Function,
    ESoloASBridgeFunctionTrait& OutTraits
);

/** Clears function traits only when RegistrationOwner matches. */
SOLOASBRIDGE_API bool ClearFunctionTraits(
    const UFunction* Function,
    FName RegistrationOwner
);

/** Clears all function-trait registrations owned by RegistrationOwner. */
SOLOASBRIDGE_API void ClearFunctionTraits(FName RegistrationOwner);
}
