// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "angelscript.h"

class asCObjectType;
class asCScriptFunction;
class asIScriptFunction;
class asIScriptContext;
class asIScriptEngine;
class asITypeInfo;
class asCObjectProperty;
class UASClass;
class FIntProperty;
class UScriptStruct;
#if WITH_DEV_AUTOMATION_TESTS
class FAutomationTestBase;
#endif

namespace Cuviss::TerminalAdmission
{
enum class EAdmissionFailure : uint8
{
    None,
    InvalidReceiverShape,
    UnsupportedHierarchy,
    MissingSourceWitness,
    RetiredSource,
    UnsupportedFunction,
    ObjectCleanup,
    BytecodeLimit,
    InvalidInstruction,
    InvalidBranch,
    CyclicControlFlow,
    UnsupportedInstruction,
    UnwitnessedNative,
    ChangedNative,
    UnwitnessedLiteral,
    ActiveContextUnqualified,
    RequestedContextUnqualified,
    TransferContractUnqualified,
    StaleFreshCertificate,
};

enum class EInt32SlotPhase : uint8 { Absent, OriginalLive, OriginalEnded, FreshLive, FreshEnded };

struct FInt32Slot
{
    UASClass* Owner = nullptr;
    const FIntProperty* Property = nullptr;
    const asCObjectProperty* Member = nullptr;
    void* Storage = nullptr;
    FName Name;
    int32 MemberIndex = INDEX_NONE;
    int32 Offset = 0;
    int32 ScriptOffset = 0;
    int32 PropertiesSize = 0;
    int32 TypeSize = 0;
    int32 TypeId = 0;
    uint64 PropertyFlags = 0;
    uint64 Epoch = 0;
    uint64 CapturedEpoch = 0;
    uint64 FinalProviderEpoch = 0;
    bool bRuntimeOwnedOriginal = false;
    int32 CapturedValue = 0;

    bool operator==(const FInt32Slot& Other) const
    {
        return Owner == Other.Owner && Property == Other.Property && Member == Other.Member && Storage == Other.Storage
            && Name == Other.Name && MemberIndex == Other.MemberIndex && Offset == Other.Offset
            && ScriptOffset == Other.ScriptOffset && PropertiesSize == Other.PropertiesSize && TypeSize == Other.TypeSize
            && TypeId == Other.TypeId && PropertyFlags == Other.PropertyFlags && Epoch == Other.Epoch
            && CapturedEpoch == Other.CapturedEpoch && CapturedValue == Other.CapturedValue
            && FinalProviderEpoch == Other.FinalProviderEpoch
            && bRuntimeOwnedOriginal == Other.bRuntimeOwnedOriginal;
    }
};

struct FReceiverShape
{
    asCObjectType* Type = nullptr;
    void* Receiver = nullptr;
    void* FieldStorage = nullptr;
    int32 FieldOffset = 0;
    asITypeInfo* OwnerCookie = nullptr;
    uint64 PlanId = 0;
    uint64 RemovedSourceGeneration = 0;
    FInt32Slot Int32Slot;
    // Actual allocation type remains Type; this is the independently retained field declaration type.
    asCObjectType* FieldOwnerType = nullptr;
    bool bAllowDefaultParentFrame = false;
};

struct FFreshFieldCertificate
{
    FReceiverShape Shape;
    uint64 Generation = 0;
    bool bFreshLive = false;
    uint64 Int32Epoch = 0;
    EInt32SlotPhase Int32Phase = EInt32SlotPhase::Absent;
};

// Numeric operands are recorded only for non-pointer instruction formats.
// Pointer operands are represented by witnessed roles, never process addresses.
struct FInstructionDiagnostic
{
    uint32 WordOffset = 0;
    uint8 Opcode = 0;
    uint8 WordCount = 0;
    uint16 ShortOperand = 0;
    int32 BranchTarget = INDEX_NONE;
    uint8 NativeRole = 0;
    bool bLiveLiteral = false;
    // PshThisMemRef's numeric type-id operand; never populated for pointer formats.
    uint32 TypeOperand = 0;
};

struct FObjectVariableDiagnostic
{
    uint32 Index = 0;
    int32 Offset = 0;
    uint64 TypeFlags = 0;
    uint8 TypeRole = 0; // 0 unknown/null, 1 receiver, 2 witnessed native receiver, 3 witnessed native parameter
    bool bHasType = false;
    bool bHasPosition = false;
    bool bHeap = false;
};

struct FObjectScopeDiagnostic
{
    uint32 ProgramPosition = 0;
    int32 VariableOffset = 0;
    uint8 Option = 0; // asEObjVarInfoOption, including lexical block markers
};

struct FNativeCallDiagnostic
{
    uint8 Role = 0;
    int32 CallerType = 0;
    int32 CallConvention = 0;
    int32 BaseOffset = 0;
    int32 ArgumentWords = 0;
    int32 TotalWords = 0;
    int32 ReturnToken = 0;
    int32 ReturnBytes = 0;
    int32 ScriptReturnWords = 0;
    uint32 ParameterCount = 0;
    uint32 OffsetCount = 0;
    uint32 ModifierCount = 0;
    int32 FirstModifier = 0;
    int32 FirstParameterOffset = INDEX_NONE;
    int32 FirstParameterToken = 0;
    int32 FirstParameterWords = 0;
    uint64 FirstParameterTypeFlags = 0;
    bool bFirstParameterReference = false;
    bool bFirstParameterReadOnly = false;
    bool bReturnOnStack = false;
    bool bHasObjectType = false;
    int32 HiddenArgument = -1;
    int32 OutputTypeArgument = -1;
    uint8 FirstParameterMetadata = 0;
};

struct FAdmissionDiagnostic
{
    EAdmissionFailure Failure = EAdmissionFailure::None;
    uint32 WordOffset = 0;
    uint32 CodeWords = 0;
    uint64 SourceGeneration = 0;
    uint32 ContextCallbacks = 0;
    uint8 ContextSelectionStage = 0; // 0 not attempted, 1 TLS, 2 borrowed, 3 prepared, 4 revalidated
    int32 PrimaryContextState = INDEX_NONE;
    int32 RequestedContextStateBeforePrepare = INDEX_NONE;
    int32 RequestedContextStateAfterPrepare = INDEX_NONE;
    int32 ContextPrepareResult = INDEX_NONE;
    bool bContextGameThread = false;
    bool bHasActiveContext = false;
    bool bHasActiveExecution = false;
    bool bHasPrimaryContext = false;
    bool bLineCallbacksPossible = false;
    TArray<FInstructionDiagnostic> Instructions;
    uint32 HeapObjectCount = 0;
    uint32 ObjectTypeCount = 0;
    uint32 ObjectPositionCount = 0;
    uint32 ObjectScopeCount = 0;
    uint32 VariableWords = 0;
    int32 StackNeeded = 0;
    int32 ArgumentWords = 0;
    int32 TotalWords = 0;
    bool bMetadataTruncated = false;
    TArray<FObjectVariableDiagnostic> ObjectVariables;
    TArray<FObjectScopeDiagnostic> ObjectScopes;
    TArray<FNativeCallDiagnostic> NativeCalls;
};

class FCompiledTerminalDestructorProof;

// Uses retained plan/metadata identities only; never reads an old scalar or receiver field value.
bool ValidateRuntimeTerminalInt32Slot(const FReceiverShape& Shape, bool bRequireFresh, bool bAllowEnded = false);
// Reads only the separately certified fresh lifetime before its first script instruction.
bool ValidateRuntimeTerminalInt32InitialValue(const FReceiverShape& Shape);
ANGELSCRIPTCODE_API bool IsTerminalInt32StorageRange(int32 Offset, int32 ScriptOffset, int32 PropertiesSize, int32 TypeSize,
    UPTRINT Receiver, UPTRINT Storage);
ANGELSCRIPTCODE_API bool TerminalStorageRangesOverlap(int32 AOffset, uint32 AWidth, int32 BOffset, uint32 BWidth);
ANGELSCRIPTCODE_API bool IsTerminalFreshInt32Certificate(const FInt32Slot& Slot, uint64 Epoch, EInt32SlotPhase Phase);
ANGELSCRIPTCODE_API bool IsTerminalCapturedInt32Slot(const FInt32Slot& Slot);

// Requires an independently registered, retained type. Does not renew compiler/resource witnesses.
uint64 ReadRetainedSourceGeneration(asITypeInfo* Type);

// The caller pins the source module and receiver metadata across the entire handshake.
bool PrepareCompiledTerminalDestructor(const FReceiverShape& Shape,
    TSharedPtr<FCompiledTerminalDestructorProof>& OutProof, FAdmissionDiagnostic& Diagnostic);

bool ValidatePreparedCompiledTerminalDestructor(const FCompiledTerminalDestructorProof& Proof,
    const FReceiverShape& Shape);

// Refresh only the original primary's legal idle state at completion/entry.
// All other evidence is revalidated, never renewed. Follow callouts with silent validation.
bool RefreshCompiledTerminalDestructorIdleContext(FCompiledTerminalDestructorProof& Proof);

// No logging, callbacks or allocations. A fresh certificate never certifies old primitive fields.
bool ValidateCompiledTerminalDestructor(const FCompiledTerminalDestructorProof& Proof,
    const FFreshFieldCertificate& Certificate);

// Binds one actual CallDestructor selection after the last silent validation.
bool BeginCompiledTerminalDestructor(FCompiledTerminalDestructorProof& Proof,
    const FFreshFieldCertificate& Certificate);
bool DidCompleteCompiledTerminalDestructor(const FCompiledTerminalDestructorProof& Proof);

// Returned proof metadata only. Function addresses are identities, never dereferenced by readers.
struct FCompiledTerminalDestructorObservation
{
    const void* DispatchFunction = nullptr;
    const void* ParentFunction = nullptr;
    uint64 ChildSourceGeneration = 0;
    uint64 ParentSourceGeneration = 0;
    uint32 PhaseMask = 0;
    uint32 ChildCallCount = 0;
    uint32 ParentEnterCount = 0;
    uint32 ParentReturnCount = 0;
    uint32 ChildResumeCount = 0;
    uint32 Frames = 0;
    bool bDefaultSource = false;
};

// Copies POD after successful context return; no registry lookup, lease renewal or callout.
bool ReadCompletedTerminalDestructorObservation(const FCompiledTerminalDestructorProof& Proof,
    FCompiledTerminalDestructorObservation& OutObservation);

enum class EContextSelection : uint8 { NotApplicable, Selected, Rejected };
EContextSelection SelectCompiledTerminalDestructorContext(void* Receiver, asCObjectType* Type,
    asIScriptContext*& OutContext);
asIScriptContext* TakeReservedContext(asIScriptEngine* Engine);

enum class EContextFence : uint8
{
    Prepare, PushState, ExecuteEntry, NativeEntry, NativeReturn, Suspend, ExecuteCompletion, Exception, Cleanup
};
bool ValidateContextExecution(asIScriptContext* Context, EContextFence Fence,
    asIScriptFunction* PreparingFunction = nullptr);
void PublishTerminalVmRegisters(asIScriptContext* Context, asDWORD* Program, asDWORD* Stack, asDWORD* Frame);
ANGELSCRIPTCODE_API void RequireNonTerminalScriptExecution();
void RequireTerminalFrameAccessAllowed(asIScriptContext* Context);
void RequireTerminalReceiverAssignmentAllowed(asIScriptContext* Context, void* Receiver);

enum class ENativeLeaf : uint8
{
    InstancedStructIsValid = 1, LogDisplay = 2, VectorConstructor = 3,
    InstancedStructInitializeAs = 4, InstancedStructReset = 5
};
void RegisterNativeLeaf(asIScriptFunction* Function, ENativeLeaf Leaf);
void RegisterVectorValueConstructor(asIScriptFunction* Function, UScriptStruct* Provider);
bool ValidateTerminalFreshStorage(const FReceiverShape& Shape, uint64 Epoch);
#if WITH_DEV_AUTOMATION_TESTS
void TestTerminalOwnedEffectsContract(FAutomationTestBase& Test);
#endif
void RegisterStringConstant(const void* Constant);
void RevokeStringConstant(const void* Constant);
void RegisterSourceFunction(asCScriptFunction* Function);
// Published only by successful CompileDefaultDestructor, after FunctionFinalized.
void RegisterDefaultDestructorSource(asCScriptFunction* Function);
enum class EParentFrameFence : uint8 { ChildCallBefore, ParentEntered, ParentRetBefore, ChildRestored };
void ValidateTerminalParentFrame(asIScriptContext* Context, EParentFrameFence Fence,
    asDWORD* Program, asDWORD* Stack, asDWORD* Frame, int CalledFunctionId = 0);
void RevokeFunction(asCScriptFunction* Function);
void ObserveRequestedContext(asIScriptContext* Context);
void ObserveReturnedContext(asIScriptContext* Context);
}
