#pragma once

#include "Containers/ArrayView.h"
#include "CoreTypes.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

class UFunction;
class UObject;
struct FSoloASBridgePreparedNativeCallable;
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
#include "UObject/WeakObjectPtrTemplates.h"
class UClass;
struct FSoloASBridgeTerminalStaticNativeProofForTest;
#endif

enum class ESoloASBridgeCallableDispatchResult : uint8
{
    Succeeded = 0,
    InvalidRequest = 1,
    OwnerNotFound = 2,
    FunctionNotFound = 3,
    TargetRequired = 4,
    TargetTypeMismatch = 5,
    NonNativeFunction = 6,
    UnsupportedSignature = 7,
    ArgumentCountMismatch = 8,
    MissingArgumentStorage = 9,
    MissingReturnStorage = 10,
    OffGameThread = 11,
    ArgumentTypeMismatch = 12,
};

/**
 * Describes one call without retaining reflection objects across reload.
 *
 * OwnerPath must be a canonical class path such as /Script/Module.Class when
 * DeclaredFunction is null. DeclaredFunction is valid only for this call,
 * must be kept GC-valid by the caller for the complete call, and is never
 * retained. Target and every object/class slot value must also remain
 * GC-valid for the complete call. Arguments contains every non-return
 * parameter in reflection order.
 *
 * An exact raw FObjectProperty or FClassProperty input argument contains the
 * UObject* or UClass* value directly. A read-only TSubclassOf input points at
 * initialized wrapper storage. Raw object and class return values use pointer
 * storage, matching every other reflected return type. Other object and class
 * wrappers use initialized property-compatible storage when their declaration
 * is supported. Every other argument points at an initialized value of the
 * reflected property type. Events and ProcessEvent copy inputs into an owned
 * parameter frame and copy writable outputs back after the call.
 *
 * ReturnValue, when required, points at uninitialized property-compatible
 * storage. The Bridge initializes that storage immediately before copying the
 * complete return value and never destroys the caller's external storage. The
 * caller owns its eventual destruction. Exact non-event native declarations
 * pass reference and out slots at the caller's address, including bare T& and
 * raw UObject*& slots. Borrowed slots are never constructed, copied or destroyed
 * by the frame; mutations are visible while the native callee is executing.
 * General overlapping reference aliasing remains unsupported by the call-slot ABI.
 *
 * Ordinary calls must run on the game thread. A template's active default
 * construction scope may invoke exact native non-event declarations on its
 * constructing thread. Other off-thread callers fail before reflection access.
 */
struct FSoloASBridgeCallableDispatchRequest
{
    const TCHAR* OwnerPath = nullptr;
    FName FunctionName = NAME_None;
    const UFunction* DeclaredFunction = nullptr;
    UObject* Target = nullptr;
    TConstArrayView<void*> Arguments;
    void* ReturnValue = nullptr;
    // A: opt in to pointer-sized native reference storage; absence of a caller must fail closed.
    bool bNativeReferenceReturn = false;
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
    const FSoloASBridgeTerminalStaticNativeProofForTest* TerminalStaticProof = nullptr;
    uint64 TerminalStaticBindEpoch = 0;
#endif
};

namespace SoloASBridge
{
/**
 * Invokes the original thunk and retains a GC-visible parameter-frame copy.
 * Game-thread only. Each call owns distinct storage until end of frame. In
 * commandlets or before the engine loop starts, storage lasts until the next call.
 */
SOLOASBRIDGE_API void* InvokeReflectedReturnCopy(UObject* Target, UFunction* Function, void** Arguments);
// A: pointer-sized reference return ABI, backed by a UHT-generated caller.
using FNativeReferenceCaller = void (*)(UObject*, void**, void*);
SOLOASBRIDGE_API FNativeReferenceCaller GetNativeReferenceCaller(const UFunction* Function);
/** Bounds native constructor/default calls to active initialization, its receiver tree and ready static owner CDOs. */
class SOLOASBRIDGE_API FScopedScriptDefaultConstruction
{
public:
    explicit FScopedScriptDefaultConstruction(UObject* Object);
    ~FScopedScriptDefaultConstruction();
    FScopedScriptDefaultConstruction(const FScopedScriptDefaultConstruction&) = delete;
    FScopedScriptDefaultConstruction& operator=(const FScopedScriptDefaultConstruction&) = delete;
private:
    UObject* Previous = nullptr;
    bool bActive = false;
};
/** Bounds exit-time native dispatch to an executing script FinishDestroy body. */
class SOLOASBRIDGE_API FScopedScriptFinishDestroy
{
public:
    explicit FScopedScriptFinishDestroy(UObject* Object);
    ~FScopedScriptFinishDestroy();
    FScopedScriptFinishDestroy(const FScopedScriptFinishDestroy&) = delete;
    FScopedScriptFinishDestroy& operator=(const FScopedScriptFinishDestroy&) = delete;
private:
    UObject* Previous = nullptr;
    bool bActive = false;
};
SOLOASBRIDGE_API UObject* GetScriptFinishDestroyReceiver();
SOLOASBRIDGE_API bool IsExitPurgeScriptNativeCall();
using FPendingScriptFinishDestroy = bool (*)();
SOLOASBRIDGE_API void InitializeScriptStaticCallContext(FPendingScriptFinishDestroy PendingObjects);
SOLOASBRIDGE_API void ReleaseScriptStaticCallContext();
SOLOASBRIDGE_API UObject* GetScriptStaticCallContext();
SOLOASBRIDGE_API bool HasPendingScriptFinishDestroyObjects();

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
/** Copies existing weak handles; producer must have reviewed the exact thunk's null-context contract. */
SOLOASBRIDGE_API TSharedPtr<const FSoloASBridgeTerminalStaticNativeProofForTest, ESPMode::ThreadSafe>
CreateTerminalStaticNativeProofForTest(const TWeakObjectPtr<UFunction>& FunctionKey,
    const TWeakObjectPtr<UClass>& OwnerKey, uint64 BindEpoch);
SOLOASBRIDGE_API void RetireTerminalStaticNativeProofForTest(
    const FSoloASBridgeTerminalStaticNativeProofForTest& Proof);
#endif
/**
 * Captures a fixed-width scalar static native binding on the game thread.
 * Supported scalar properties are native bool, signed and unsigned 8-, 16-,
 * 32- and 64-bit integers, float, and double. Each property must be a single
 * element with the expected reflected width and native alignment;
 * enums, bitfield bools, objects, structs, containers, and fixed arrays are
 * unsupported.
 *
 * Value and return parameters use the captured scalar widths. Reference/out
 * arguments borrow caller storage directly, including const references. Native code and its reflection layout must remain
 * valid for the binding epoch, as for a direct native binding. This handle
 * retains reflection objects, not a DLL. Rebuild after native reconstruction
 * only after draining existing callers. Overlapping identity-sensitive
 * argument storage and any overlap between return and argument storage are
 * rejected using the complete slot ranges. A null result leaves ordinary
 * reflected dispatch available.
 */
SOLOASBRIDGE_API TSharedPtr<
    const FSoloASBridgePreparedNativeCallable, ESPMode::ThreadSafe
> PrepareNativeCallable(const UFunction* Function);

/** Invokes the captured native entry without resolving reflection on a worker. */
SOLOASBRIDGE_API ESoloASBridgeCallableDispatchResult InvokePreparedNativeCallable(
    const FSoloASBridgePreparedNativeCallable& Prepared,
    TConstArrayView<void*> Arguments,
    void* ReturnValue
);

/** Returns whether Function has a signature supported by the call-slot ABI. */
SOLOASBRIDGE_API bool CanInvokeReflectedCallable(const UFunction* Function);

/**
 * Resolves and invokes a reflected native declaration through ProcessEvent.
 *
 * Resolution is repeated for every call so Blueprint/interface overrides and
 * reload replacement objects are honored. Static functions execute on their
 * declaring class default object. In the Bridge ExitPurge destruction callback
 * scope, an exact native declaration without event, script, or network behavior
 * uses its native thunk. Instance calls preserve Target, require an unreachable
 * non-CDO receiver compatible with the native owner, and reject any function,
 * owner, or receiver that has entered BeginDestroy or FinishDestroy.
 */
SOLOASBRIDGE_API ESoloASBridgeCallableDispatchResult InvokeReflectedCallable(
    const FSoloASBridgeCallableDispatchRequest& Request
);
}
