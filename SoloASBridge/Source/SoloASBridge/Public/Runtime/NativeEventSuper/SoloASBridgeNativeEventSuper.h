#pragma once

#include "CoreTypes.h"
#include "UObject/NameTypes.h"

class UObject;

enum class ESoloASBridgeNativeEventSuperResult : uint8
{
    Succeeded = 0,
    InvalidRequest = 1,
    OffGameThread = 2,
    CallerClassNotFound = 3,
    NativeClassNotFound = 4,
    CallerFunctionNotFound = 5,
    NativeFunctionNotFound = 6,
    CallerOwnerInvalid = 7,
    NativeClassInvalid = 8,
    NativeAncestorNotFound = 9,
    NativeAncestorMismatch = 10,
    CallerFunctionShapeInvalid = 11,
    UnsupportedFunctionFlags = 12,
    InterfaceRejected = 13,
    TargetRequired = 14,
    TargetTypeMismatch = 15,
    ParameterLayoutMismatch = 16,
    UnsupportedReferenceReturn = 17,
    UnsupportedWritableObjectReference = 18,
    UnsupportedAliasLayout = 19,
    ParameterBufferInvalid = 20,
    NativeImplementationMissing = 21,
};

/**
 * Describes one exact native-parent invocation.
 *
 * The class paths must be canonical object paths, for example
 * /Script/Module.Class. The parameter buffer must already contain initialized
 * values for the exact caller signature and remains owned by the caller. This
 * API does not retain any request value or reflection object.
 *
 * The exact native function defines the required invocation-buffer extent.
 * The caller function's aggregate ParmsSize is not used as layout identity;
 * each caller parameter property must instead match the native property name,
 * type, offset, size, array shape, and call direction exactly.
 *
 * ParameterBufferCapacity and ParameterBufferAlignment describe the actual
 * caller-owned allocation. They are validated, but this API does not acquire
 * ownership of the allocation or any value in it. The caller guarantees that
 * caller and native parameter properties have identical names, types, offsets,
 * sizes, and call-direction flags. Non-ABI property metadata, including native
 * access specifiers and Blueprint pin requirements, is not part of
 * parameter-buffer compatibility. The caller also guarantees that
 * Target, every reflected object or class value in the initialized buffer,
 * and the backing allocation remain GC-valid and are not concurrently
 * invalidated for the complete synchronous call. Reload must not occur
 * concurrently with the call.
 */
struct FSoloASBridgeNativeEventSuperRequest
{
    const TCHAR* CallerScriptClassPath = nullptr;
    const TCHAR* NativeDeclaringClassPath = nullptr;
    FName FunctionName = NAME_None;
    UObject* Target = nullptr;
    void* InitializedParameterBuffer = nullptr;
    SIZE_T ParameterBufferCapacity = 0;
    uint32 ParameterBufferAlignment = 0;
};

namespace SoloASBridge
{
/**
 * Invokes the exact native ancestor of a BlueprintNativeEvent override
 * synchronously; this is a synchronous-call-only API.
 *
 * Calls must run on the game thread. The caller owns the initialized
 * parameter buffer and any values it contains. No parameter value is copied,
 * initialized, destroyed, allocated, or retained by this API. This performs
 * no caching and no ProcessEvent dispatch; it invokes the exact UFunction with
 * UFunction::Invoke. The caller must keep Target, all reflected object or
 * class values in the buffer, and the backing allocation GC-valid and free of
 * concurrent invalidation until the call completes. Reload must not occur
 * concurrently.
 */
SOLOASBRIDGE_API ESoloASBridgeNativeEventSuperResult
InvokeNativeEventSuper(
    const FSoloASBridgeNativeEventSuperRequest& Request
);
}
