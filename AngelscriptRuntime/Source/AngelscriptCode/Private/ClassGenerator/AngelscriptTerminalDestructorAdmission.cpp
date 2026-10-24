// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.

#include "ClassGenerator/AngelscriptTerminalDestructorAdmission.h"
#include "AngelscriptRuntimeGcLifecycle.h"
#include <cstdlib>
#include <type_traits>
#include <utility>
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "as_context.h"
#include "as_objecttype.h"
#include "as_scriptengine.h"
#include "as_scriptfunction.h"
#include "as_property.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/UObjectArray.h"
#include "UObject/NoExportTypes.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <intrin.h>
#endif

asIScriptContext* AngelscriptRequestContext(asIScriptEngine* Engine, void* Data);
void AngelscriptReturnContext(asIScriptEngine* Engine, asIScriptContext* Context, void* Data);

namespace Cuviss::TerminalAdmission
{
bool IsTerminalInt32StorageRange(int32 Offset, int32 ScriptOffset, int32 PropertiesSize, int32 TypeSize,
    UPTRINT Receiver, UPTRINT Storage)
{
    constexpr int32 Width = sizeof(int32);
    return ScriptOffset > 0 && Offset >= ScriptOffset && Offset <= PropertiesSize && Offset <= TypeSize
        && Width <= PropertiesSize - Offset && Width <= TypeSize - Offset
        && Receiver <= MAX_uint64 - static_cast<uint32>(Offset) - Width
        && Storage == Receiver + static_cast<uint32>(Offset) && Storage % alignof(int32) == 0;
}

bool TerminalStorageRangesOverlap(int32 AOffset, uint32 AWidth, int32 BOffset, uint32 BWidth)
{
    return static_cast<int64>(AOffset) < static_cast<int64>(BOffset) + BWidth
        && static_cast<int64>(BOffset) < static_cast<int64>(AOffset) + AWidth;
}

bool IsTerminalFreshInt32Certificate(const FInt32Slot& Slot, uint64 Epoch, EInt32SlotPhase Phase)
{
    return Slot.Property ? Slot.Epoch && Epoch == Slot.Epoch && Phase == EInt32SlotPhase::FreshLive
        : Slot == FInt32Slot() && Epoch == 0 && Phase == EInt32SlotPhase::Absent;
}

bool IsTerminalCapturedInt32Slot(const FInt32Slot& Slot)
{
    return Slot.Property && Slot.Epoch && Slot.CapturedEpoch == Slot.Epoch;
}

namespace
{
constexpr uint32 MaxCodeWords = 4096;
constexpr int32 MaxInstructions = 1024;

using EOpsFlag = UE::CoreUObject::Private::EStructOpsFakeVTableFlags;
using FOpsTable = UE::CoreUObject::Private::FStructOpsFakeVTable;
// Retain the SDK getter's exact type for identity checks; these values are never invoked.
using FConstructHandlerIdentity = decltype(std::declval<const FOpsTable&>().GetHandlingFunc<EOpsFlag::Construct>());
using FCopyHandlerIdentity = decltype(std::declval<const FOpsTable&>().GetHandlingFunc<EOpsFlag::Copy>());
static_assert(std::is_pointer_v<FConstructHandlerIdentity>
    && std::is_function_v<std::remove_pointer_t<FConstructHandlerIdentity>>);
static_assert(std::is_pointer_v<FCopyHandlerIdentity>
    && std::is_function_v<std::remove_pointer_t<FCopyHandlerIdentity>>);

struct FNativeValueWitness
{
    uint64 Generation = 0;
    const asCScriptFunction* Constructor = nullptr;
    asCObjectType* Type = nullptr;
    asCScriptEngine* Engine = nullptr;
    UScriptStruct* Provider = nullptr;
    int32 Index = INDEX_NONE;
    int32 Serial = 0;
    int32 TypeId = 0;
    uint32 Flags = 0;
    EStructFlags StructFlags = STRUCT_NoFlags;
    UScriptStruct::ICppStructOps* Ops = nullptr;
    const FOpsTable* Table = nullptr;
    EOpsFlag OpsFlags = EOpsFlag::None;
    UScriptStruct::ICppStructOps::FCapabilities Capabilities = {};
    FConstructHandlerIdentity Construct = nullptr;
    FCopyHandlerIdentity Copy = nullptr;

    bool Matches() const
    {
        const auto* Item = GUObjectArray.IndexToObject(Index);
        if (!Generation || !Type || !Engine || !Provider || !Item || Item->GetSerialNumber() != Serial
            || Item->GetObject() != Provider || Type->engine != Engine || Type->typeId != TypeId
            || Engine->GetTypeInfoById(TypeId) != Type || Type->flags != Flags || Type->size != sizeof(FVector)
            || Type->alignment != alignof(FVector)
            || Type->plainUserData != reinterpret_cast<asPWORD>(Provider) || Type->GetSubTypeCount() != 0
            || Type->module || Type->derivedFrom || Type->shadowType
            || (Flags & (asOBJ_REF | asOBJ_SCRIPT_OBJECT | asOBJ_FUNCDEF))
            || !(Flags & asOBJ_VALUE) || Provider->StructFlags != StructFlags
            || Provider->GetCppStructOps() != Ops
            || Provider->GetStructureSize() != sizeof(FVector) || Provider->GetMinAlignment() != alignof(FVector)
            || Provider->GetPropertiesSize() > sizeof(FVector) || !Ops || Ops->FakeVPtr != Table
            || Ops->GetSize() != sizeof(FVector) || Ops->GetAlignment() != alignof(FVector)
            || !Table || Table->Flags != OpsFlags
            || FMemory::Memcmp(&Table->Capabilities, &Capabilities, sizeof(Capabilities)))
            return false;
        // The fixed native provider closes construction over its complete native size,
        // copy over memcpy or the registered FVector assignment, and destruction over
        // the SDK's trivial branch. No reflected property or script fallback is allowed.
        if (!(StructFlags & STRUCT_Native) || !(StructFlags & (STRUCT_IsPlainOldData | STRUCT_NoDestructor))
            || Capabilities.HasDestructor || Capabilities.HasPostScriptConstruct
            || Capabilities.HasAddStructReferencedObjects || Capabilities.ClearOnFinishDestroy)
            return false;
        const auto* Canonical = UE::CoreUObject::Private::TStructOpsFakeVTableForType<FVector>::Value;
        // Only handlers selected by InitializeStruct/CopyScriptStruct need a
        // canonical implementation. Zero construction, POD copying and trivial
        // destruction do not dispatch through any handler. The captured provider,
        // complete table flags and capabilities remain immutable above.
        if (!Capabilities.HasZeroConstructor && (!(OpsFlags & EOpsFlag::Construct)
            || Table->GetHandlingFunc<EOpsFlag::Construct>() != Construct
            || Construct != Canonical->GetHandlingFunc<EOpsFlag::Construct>()))
            return false;
        if (StructFlags & STRUCT_CopyNative)
            return !(StructFlags & STRUCT_IsPlainOldData) && Capabilities.HasCopy
                && !!(OpsFlags & EOpsFlag::Copy) && Table->GetHandlingFunc<EOpsFlag::Copy>() == Copy
                && Copy == Canonical->GetHandlingFunc<EOpsFlag::Copy>();
        return !!(StructFlags & STRUCT_IsPlainOldData);
    }
};

struct FLocalTypeWitness
{
    asCObjectType* Type = nullptr;
    uint64 Generation = 0;
    int32 Position = 0;
    int32 TypeId = 0;
    int32 Size = 0;
    int32 Alignment = 0;
    uint32 Flags = 0;
    asPWORD UserData = 0;
    asSTypeBehaviour Behaviour;

    bool Matches(const asCTypeInfo* Current, int32 Offset) const
    {
        if (!Type || Current != Type || Offset != Position || Type->typeId != TypeId || Type->size != Size
            || Type->alignment != Alignment
            || Type->flags != Flags || Type->plainUserData != UserData || Type->beh.construct != Behaviour.construct
            || Type->beh.copyconstruct != Behaviour.copyconstruct || Type->beh.destruct != Behaviour.destruct
            || Type->beh.copy != Behaviour.copy || Type->beh.addref != Behaviour.addref || Type->beh.release != Behaviour.release
            || Type->beh.factory != Behaviour.factory || Type->beh.copyfactory != Behaviour.copyfactory
            || Type->beh.listFactory != Behaviour.listFactory || Type->beh.templateCallback != Behaviour.templateCallback
            || Type->beh.gcGetRefCount != Behaviour.gcGetRefCount || Type->beh.gcSetFlag != Behaviour.gcSetFlag
            || Type->beh.gcGetFlag != Behaviour.gcGetFlag || Type->beh.gcEnumReferences != Behaviour.gcEnumReferences
            || Type->beh.gcReleaseAllReferences != Behaviour.gcReleaseAllReferences
            || Type->beh.getWeakRefFlag != Behaviour.getWeakRefFlag
            || Type->beh.constructors.GetLength() != Behaviour.constructors.GetLength()
            || Type->beh.factories.GetLength() != Behaviour.factories.GetLength()) return false;
        for (asUINT I = 0; I < Behaviour.constructors.GetLength(); ++I)
            if (Type->beh.constructors[I] != Behaviour.constructors[I]) return false;
        for (asUINT I = 0; I < Behaviour.factories.GetLength(); ++I)
            if (Type->beh.factories[I] != Behaviour.factories[I]) return false;
        return true;
    }
};

struct FNativeWitness
{
    uint64 Generation = 0;
    ENativeLeaf Leaf = ENativeLeaf::InstancedStructIsValid;
    asCScriptEngine* Engine = nullptr;
    asCObjectType* ObjectType = nullptr;
    asSSystemFunctionInterface Interface;
    asCDataType ReturnType;
    TArray<asCDataType> Parameters;
    TArray<int> Offsets;
    TArray<asETypeModifiers> Modifiers;
    uint32 Traits = 0;
    int ArgumentWords = 0;
    int TotalWords = 0;
    int8 HiddenArgument = -1;
    int8 OutputTypeArgument = -1;
    void* Auxiliary = nullptr;
    asECompileOutType CompileOut = asECompileOutType::CompileCalls;

    bool Matches(const asCScriptFunction& Function) const
    {
        const auto* Current = Function.sysFuncIntf;
        if (Function.funcType != asFUNC_SYSTEM || !Current || Function.engine != Engine ||
            Function.objectType != ObjectType || Function.returnType != ReturnType ||
            Function.traits.traits != Traits || Function.spaceNeededForArguments != ArgumentWords ||
            Function.totalSpaceBeforeFunction != TotalWords || Function.hiddenArgumentIndex != HiddenArgument ||
            Function.determinesOutputTypeArgumentIndex != OutputTypeArgument ||
            Function.userData != Auxiliary || Function.compileOutType != CompileOut ||
            Function.parameterTypes.GetLength() != static_cast<asUINT>(Parameters.Num()) ||
            Function.parameterOffsets.GetLength() != static_cast<asUINT>(Offsets.Num()) ||
            Function.inOutFlags.GetLength() != static_cast<asUINT>(Modifiers.Num()))
            return false;
        if (Current->caller.type != Interface.caller.type || Current->func != Interface.func ||
            Current->method != Interface.method || Current->baseOffset != Interface.baseOffset ||
            Current->callConv != Interface.callConv || Current->scriptReturnSize != Interface.scriptReturnSize ||
            Current->passFirstParamMetaData != Interface.passFirstParamMetaData)
            return false;
        if (Current->caller.type == 1)
        {
            if (Current->caller.FunctionCaller != Interface.caller.FunctionCaller)
                return false;
        }
        else if (Current->caller.type == 2)
        {
            if (Current->caller.MethodCaller != Interface.caller.MethodCaller)
                return false;
        }
        else
            return false;
        for (int32 Index = 0; Index < Parameters.Num(); ++Index)
            if (Function.parameterTypes[Index] != Parameters[Index])
                return false;
        for (int32 Index = 0; Index < Offsets.Num(); ++Index)
            if (Function.parameterOffsets[Index] != Offsets[Index])
                return false;
        for (int32 Index = 0; Index < Modifiers.Num(); ++Index)
            if (Function.inOutFlags[Index] != Modifiers[Index])
                return false;
        return true;
    }
};

struct FSourceResource
{
    const void* Address = nullptr;
    uint64 Generation = 0;
    bool bNative = false;
};

struct FSourceWitness
{
    uint64 Generation = 0;
    asCScriptEngine* Engine = nullptr;
    asCObjectType* ObjectType = nullptr;
    asCModule* Module = nullptr;
    asIStringFactory* StringFactory = nullptr;
    asCDataType StringType;
    TArray<asDWORD> Code;
    int StackNeeded = 0;
    uint32 VariableSpace = 0;
    uint32 Traits = 0;
    uint32 HeapObjects = 0;
    uint32 ObjectTypes = 0;
    uint32 ObjectPositions = 0;
    int ArgumentWords = 0;
    int TotalWords = 0;
    TArray<asSObjectVariableInfo> ObjectScopes;
    TArray<FLocalTypeWitness> LocalTypes;
    TArray<FSourceResource> Resources;
    const asCObjectProperty* Int32Member = nullptr;
    asCDataType Int32DataType;
    asCString Int32MemberName;
    int32 Int32MemberIndex = INDEX_NONE;
    int32 Int32ReceiverSize = 0;
    int32 Int32ReceiverTypeId = 0;
    int32 Int32Offset = 0;
    uint32 Int32PropertyCount = 0;
    uint32 Int32LocalPropertyCount = 0;
    uint32 Int32ReceiverFlags = 0;
    bool bInt32MemberWitness = false;
    bool bWithinLimit = false;

    bool Matches(const asCScriptFunction& Function) const
    {
        const auto* Data = Function.scriptData;
        if (!bWithinLimit || !Data || Function.funcType != asFUNC_SCRIPT ||
            Function.engine != Engine || Function.objectType != ObjectType || Function.module != Module ||
            !Engine || Engine->stringFactory != StringFactory || Engine->stringType != StringType ||
            Function.traits.traits != Traits || Function.jitFunction || Function.jitFunction_ParmsEntry ||
            Function.jitFunction_Raw || Data->stackNeeded != StackNeeded ||
            Data->variableSpace != VariableSpace || Data->objVariablesOnHeap != HeapObjects ||
            Data->objVariableTypes.GetLength() != ObjectTypes || Data->objVariablePos.GetLength() != ObjectPositions ||
            Data->objVariableInfo.GetLength() != static_cast<asUINT>(ObjectScopes.Num()) ||
            Function.spaceNeededForArguments != ArgumentWords || Function.totalSpaceBeforeFunction != TotalWords ||
            Data->byteCode.GetLength() != static_cast<asUINT>(Code.Num()))
            return false;
        if (LocalTypes.Num() != static_cast<int32>(ObjectTypes) || ObjectPositions != ObjectTypes)
            return false;
        for (int32 Index = 0; Index < LocalTypes.Num(); ++Index)
            if (!LocalTypes[Index].Matches(Data->objVariableTypes[Index], Data->objVariablePos[Index])) return false;
        for (int32 Index = 0; Index < ObjectScopes.Num(); ++Index)
            if (Data->objVariableInfo[Index].programPos != ObjectScopes[Index].programPos ||
                Data->objVariableInfo[Index].variableOffset != ObjectScopes[Index].variableOffset ||
                Data->objVariableInfo[Index].option != ObjectScopes[Index].option)
                return false;
        for (int32 Index = 0; Index < Code.Num(); ++Index)
            if (Data->byteCode[Index] != Code[Index])
                return false;
        if (bInt32MemberWitness)
        {
            if (!ObjectType || !Int32Member || Int32MemberIndex < 0 ||
                ObjectType->properties.GetLength() != Int32PropertyCount ||
                ObjectType->localProperties.GetLength() != Int32LocalPropertyCount ||
                static_cast<uint32>(Int32MemberIndex) >= Int32LocalPropertyCount ||
                ObjectType->localProperties[Int32MemberIndex] != Int32Member ||
                ObjectType->size != Int32ReceiverSize || ObjectType->typeId != Int32ReceiverTypeId ||
                ObjectType->flags != Int32ReceiverFlags || Int32Member->byteOffset != Int32Offset ||
                Int32Member->type != Int32DataType || Int32Member->name != Int32MemberName || Int32Member->compositeOffset ||
                Int32Member->isCompositeIndirect || Int32Member->bUeDelayedDestroyOwner || Int32Member->isAppBindProperty)
                return false;
        }
        return true;
    }
};

struct FDefaultDestructorSource
{
    uint64 ChildGeneration = 0, ParentGeneration = 0;
    const asCScriptFunction* Parent = nullptr;
    asCObjectType* ChildType = nullptr;
    asCObjectType* ParentType = nullptr;
};

struct FContextWitness
{
    uint64 Generation = 0;
    FCompiledTerminalDestructorProof* Owner = nullptr;
    uint32 Callbacks = 0;
};

struct FAdmissionRegistry
{
    FCriticalSection Mutex;
    uint64 NextGeneration = 0;
    TMap<const asCScriptFunction*, FNativeWitness> Natives;
    TMap<const asCScriptFunction*, FSourceWitness> Sources;
    TMap<const asCScriptFunction*, FDefaultDestructorSource> DefaultDestructors;
    TMap<const asCTypeInfo*, FNativeValueWitness> Values;
    TMap<const void*, uint64> Literals;
    TMap<const asIScriptContext*, FContextWitness> Contexts;

    uint64 NewGeneration()
    {
        // Exhaustion permanently disables new witnesses; never recycle an identity.
        return NextGeneration == MAX_uint64 ? 0 : ++NextGeneration;
    }
};

FAdmissionRegistry& Registry()
{
    // Registration and revocation also run during engine shutdown.
    static FAdmissionRegistry* Instance = new FAdmissionRegistry();
    return *Instance;
}

uint32 ContextCallbacks(const asCContext& Context)
{
    return (Context.m_exceptionCallback ? 1u : 0u) |
        (Context.m_lineCallback ? 2u : 0u) |
        (Context.m_loopDetectionCallback ? 4u : 0u) |
        (Context.m_stackPopCallback ? 8u : 0u);
}

bool SameShape(const FReceiverShape& A, const FReceiverShape& B)
{
    return A.Type == B.Type && A.FieldOwnerType == B.FieldOwnerType
        && A.bAllowDefaultParentFrame == B.bAllowDefaultParentFrame && A.Receiver == B.Receiver && A.FieldStorage == B.FieldStorage &&
        A.FieldOffset == B.FieldOffset && A.OwnerCookie == B.OwnerCookie && A.PlanId == B.PlanId &&
        A.RemovedSourceGeneration == B.RemovedSourceGeneration && A.Int32Slot == B.Int32Slot;
}

bool Reject(FAdmissionDiagnostic& Diagnostic, EAdmissionFailure Failure, uint32 Offset = 0)
{
    Diagnostic.Failure = Failure;
    Diagnostic.WordOffset = Offset;
    return false;
}

bool IsRelativeBranch(uint8 Opcode)
{
    switch (Opcode)
    {
    case asBC_JMP: case asBC_JZ: case asBC_JNZ: case asBC_JS: case asBC_JNS:
    case asBC_JP: case asBC_JNP: case asBC_JLowZ: case asBC_JLowNZ:
    case asBC_JNullR: case asBC_JNonNullR:
        return true;
    default:
        return false;
    }
}

void CaptureCleanupDiagnostic(const asCScriptFunction& Function, const FAdmissionRegistry& State,
    FAdmissionDiagnostic& Diagnostic)
{
    constexpr uint32 MaxMetadataEntries = 64;
    const auto& Data = *Function.scriptData;
    Diagnostic.HeapObjectCount = Data.objVariablesOnHeap;
    Diagnostic.ObjectTypeCount = Data.objVariableTypes.GetLength();
    Diagnostic.ObjectPositionCount = Data.objVariablePos.GetLength();
    Diagnostic.ObjectScopeCount = Data.objVariableInfo.GetLength();
    Diagnostic.VariableWords = Data.variableSpace;
    Diagnostic.StackNeeded = Data.stackNeeded;
    Diagnostic.ArgumentWords = Function.spaceNeededForArguments;
    Diagnostic.TotalWords = Function.totalSpaceBeforeFunction;
    const uint32 Count = FMath::Max(Diagnostic.ObjectTypeCount, Diagnostic.ObjectPositionCount);
    Diagnostic.bMetadataTruncated = Count > MaxMetadataEntries || Diagnostic.ObjectScopeCount > MaxMetadataEntries;
    for (uint32 Index = 0; Index < FMath::Min(Count, MaxMetadataEntries); ++Index)
    {
        FObjectVariableDiagnostic Item;
        Item.Index = Index;
        Item.bHeap = Index < Data.objVariablesOnHeap;
        Item.bHasPosition = Index < Data.objVariablePos.GetLength();
        if (Item.bHasPosition)
            Item.Offset = Data.objVariablePos[Index];
        const auto* Type = Index < Data.objVariableTypes.GetLength() ? Data.objVariableTypes[Index] : nullptr;
        Item.bHasType = Type != nullptr;
        if (Type)
        {
            Item.TypeFlags = Type->flags;
            if (Type == Function.objectType)
                Item.TypeRole = 1;
            else
                for (const auto& Entry : State.Natives)
                {
                    if (Type == Entry.Value.ObjectType)
                        Item.TypeRole = 2;
                    for (const auto& Parameter : Entry.Value.Parameters)
                        if (!Item.TypeRole && Type == Parameter.GetTypeInfo())
                            Item.TypeRole = 3;
                }
        }
        Diagnostic.ObjectVariables.Add(Item);
    }
    for (uint32 Index = 0; Index < FMath::Min(Diagnostic.ObjectScopeCount, MaxMetadataEntries); ++Index)
    {
        const auto& Info = Data.objVariableInfo[Index];
        Diagnostic.ObjectScopes.Add({Info.programPos, Info.variableOffset, static_cast<uint8>(Info.option)});
    }
}

void CaptureNativeDiagnostic(const asCScriptFunction& Function, ENativeLeaf Leaf,
    FAdmissionDiagnostic& Diagnostic)
{
    const uint8 Role = static_cast<uint8>(Leaf);
    for (const auto& Existing : Diagnostic.NativeCalls)
        if (Existing.Role == Role)
            return;
    const auto& Interface = *Function.sysFuncIntf;
    FNativeCallDiagnostic Item;
    Item.Role = Role;
    Item.CallerType = Interface.caller.type;
    Item.CallConvention = Interface.callConv;
    Item.BaseOffset = Interface.baseOffset;
    Item.ArgumentWords = Function.spaceNeededForArguments;
    Item.TotalWords = Function.totalSpaceBeforeFunction;
    Item.ReturnToken = Function.returnType.GetTokenType();
    Item.ReturnBytes = Function.returnType.GetSizeInMemoryBytes();
    Item.ScriptReturnWords = Interface.scriptReturnSize;
    Item.ParameterCount = Function.parameterTypes.GetLength();
    Item.OffsetCount = Function.parameterOffsets.GetLength();
    Item.ModifierCount = Function.inOutFlags.GetLength();
    if (Item.ModifierCount)
        Item.FirstModifier = Function.inOutFlags[0];
    Item.bReturnOnStack = Function.DoesReturnOnStack();
    Item.bHasObjectType = Function.objectType != nullptr;
    Item.HiddenArgument = Function.hiddenArgumentIndex;
    Item.OutputTypeArgument = Function.determinesOutputTypeArgumentIndex;
    Item.FirstParameterMetadata = static_cast<uint8>(Interface.passFirstParamMetaData);
    if (Item.OffsetCount)
        Item.FirstParameterOffset = Function.parameterOffsets[0];
    if (Item.ParameterCount)
    {
        const auto& Parameter = Function.parameterTypes[0];
        Item.FirstParameterToken = Parameter.GetTokenType();
        Item.FirstParameterWords = Parameter.GetSizeOnStackDWords();
        Item.bFirstParameterReference = Parameter.IsReference();
        Item.bFirstParameterReadOnly = Parameter.IsReadOnly();
        if (const auto* Type = Parameter.GetTypeInfo())
            Item.FirstParameterTypeFlags = Type->flags;
    }
    Diagnostic.NativeCalls.Add(Item);
}

// This is structural decoding, not a semantic whitelist. In particular stackInc
// cannot describe CALLSYS's caller ABI or prove any stack/register initialization.
bool Decode(const FSourceWitness& Source, FAdmissionRegistry& State, FAdmissionDiagnostic& Diagnostic)
{
    const uint32 Length = Source.Code.Num();
    TArray<int32> Boundary;
    Boundary.Init(INDEX_NONE, Length);
    for (uint32 Offset = 0; Offset < Length;)
    {
        if (Diagnostic.Instructions.Num() == MaxInstructions)
            return Reject(Diagnostic, EAdmissionFailure::BytecodeLimit, Offset);
        const auto* Instruction = Source.Code.GetData() + Offset;
        const uint8 Opcode = static_cast<uint8>(*Instruction);
        if (Opcode >= asBC_MAXBYTECODE || asBCInfo[Opcode].bc != Opcode)
            return Reject(Diagnostic, EAdmissionFailure::InvalidInstruction, Offset);
        const int Format = asBCInfo[Opcode].type;
        if (Format <= asBCTYPE_INFO || Format >= UE_ARRAY_COUNT(asBCTypeSize))
            return Reject(Diagnostic, EAdmissionFailure::InvalidInstruction, Offset);
        const uint32 Words = asBCTypeSize[Format];
        if (!Words || Words > Length - Offset)
            return Reject(Diagnostic, EAdmissionFailure::InvalidInstruction, Offset);
        FInstructionDiagnostic Item;
        Item.WordOffset = Offset;
        Item.Opcode = Opcode;
        Item.WordCount = Words;
        Item.ShortOperand = static_cast<uint16>(*Instruction >> 16);
        if (Opcode == asBC_PshThisMemRef || Opcode == asBC_LoadThisR)
            Item.TypeOperand = asBC_DWORDARG(Instruction);
        if (IsRelativeBranch(Opcode))
        {
            const int64 Target = static_cast<int64>(Offset) + Words + asBC_INTARG(Instruction);
            if (Target < 0 || Target >= Length)
                return Reject(Diagnostic, EAdmissionFailure::InvalidBranch, Offset);
            Item.BranchTarget = static_cast<int32>(Target);
        }
        if (Opcode == asBC_CALLSYS)
        {
            const auto* Callee = reinterpret_cast<const asCScriptFunction*>(asBC_PTRARG(Instruction));
            // Look up the address before dereferencing the bytecode operand.
            if (const auto* Native = State.Natives.Find(Callee))
                if (Native->Generation && Native->Matches(*Callee))
                {
                    Item.NativeRole = static_cast<uint8>(Native->Leaf);
                    CaptureNativeDiagnostic(*Callee, Native->Leaf, Diagnostic);
                }
        }
        if (Opcode == asBC_PGA)
        {
            const void* Literal = reinterpret_cast<const void*>(asBC_PTRARG(Instruction));
            const auto* Generation = State.Literals.Find(Literal);
            Item.bLiveLiteral = Generation && *Generation;
        }
        Boundary[Offset] = Diagnostic.Instructions.Num();
        Diagnostic.Instructions.Add(Item);
        Offset += Words;
    }

    const int32 Count = Diagnostic.Instructions.Num();
    TArray<int32> Indegrees;
    Indegrees.Init(0, Count);
    auto VisitEdges = [&](int32 Index, auto&& Visit) -> bool
    {
        const auto& Item = Diagnostic.Instructions[Index];
        if (Item.BranchTarget != INDEX_NONE)
        {
            const int32 Target = Boundary[Item.BranchTarget];
            if (Target == INDEX_NONE)
                return false;
            Visit(Target);
        }
        if (Item.Opcode != asBC_RET && Item.Opcode != asBC_JMP)
        {
            if (Index + 1 >= Count)
                return false;
            Visit(Index + 1);
        }
        return true;
    };
    for (int32 Index = 0; Index < Count; ++Index)
    {
        // Indirect switch targets require a separate transfer contract.
        if (Diagnostic.Instructions[Index].Opcode == asBC_JMPP)
            return Reject(Diagnostic, EAdmissionFailure::UnsupportedInstruction,
                Diagnostic.Instructions[Index].WordOffset);
        if (!VisitEdges(Index, [&](int32 Target) { ++Indegrees[Target]; }))
            return Reject(Diagnostic, EAdmissionFailure::InvalidBranch,
                Diagnostic.Instructions[Index].WordOffset);
    }
    TArray<int32> Ready;
    for (int32 Index = 0; Index < Count; ++Index)
        if (!Indegrees[Index])
            Ready.Add(Index);
    for (int32 Cursor = 0; Cursor < Ready.Num(); ++Cursor)
        VisitEdges(Ready[Cursor], [&](int32 Target)
        {
            if (--Indegrees[Target] == 0)
                Ready.Add(Target);
        });
    if (Ready.Num() != Count)
        return Reject(Diagnostic, EAdmissionFailure::CyclicControlFlow);
    return true;
}

bool HasOnlyLexicalScopes(const FSourceWitness& Source)
{
    if (Source.HeapObjects || Source.ObjectTypes || Source.ObjectPositions)
        return false;
    int32 Depth = 0;
    uint32 Previous = 0;
    for (const auto& Scope : Source.ObjectScopes)
    {
        if (Scope.variableOffset != 0 || Scope.programPos < Previous ||
            Scope.programPos > static_cast<uint32>(Source.Code.Num()))
            return false;
        uint32 PC = 0;
        while (PC < Scope.programPos)
        {
            const uint8 Opcode = static_cast<uint8>(Source.Code[PC]);
            if (Opcode >= asBC_MAXBYTECODE)
                return false;
            const int Format = asBCInfo[Opcode].type;
            if (Format <= asBCTYPE_INFO || Format >= UE_ARRAY_COUNT(asBCTypeSize))
                return false;
            const uint32 Width = asBCTypeSize[Format];
            if (!Width || Width > static_cast<uint32>(Source.Code.Num()) - PC)
                return false;
            PC += Width;
        }
        if (PC != Scope.programPos)
            return false;
        if (Scope.option == asBLOCK_BEGIN)
            ++Depth;
        else if (Scope.option == asBLOCK_END && Depth > 0)
            --Depth;
        else
            return false;
        Previous = Scope.programPos;
    }
    return Depth == 0;
}

bool ValidateLocalMetadata(const FSourceWitness& Source, const FAdmissionRegistry& Registry)
{
    if (!Source.ObjectTypes) return HasOnlyLexicalScopes(Source);
    if (Source.HeapObjects || Source.ObjectTypes != Source.ObjectPositions
        || Source.LocalTypes.Num() != static_cast<int32>(Source.ObjectTypes)) return false;
    for (int32 I = 0; I < Source.LocalTypes.Num(); ++I)
    {
        const auto& Local = Source.LocalTypes[I];
        const auto* Value = Registry.Values.Find(Local.Type);
        const auto* Constructor = Value ? Registry.Natives.Find(Value->Constructor) : nullptr;
        if (!Value || !Constructor || !Local.Generation || Local.Generation != Value->Generation
            || Constructor->Generation != Value->Generation || !Value->Matches() || Local.Behaviour.destruct
            || Local.Position <= 0 || Local.Position > static_cast<int32>(Source.VariableSpace)
            || Local.Position * int32(sizeof(asDWORD)) < Local.Size
            || (Local.Position * sizeof(asDWORD)) % alignof(FVector)) return false;
        for (int32 J = 0; J < I; ++J)
            if (TerminalStorageRangesOverlap(-Local.Position * sizeof(asDWORD), Local.Size,
                -Source.LocalTypes[J].Position * sizeof(asDWORD), Source.LocalTypes[J].Size)) return false;
    }
    int32 Depth = 0;
    uint32 Previous = 0;
    TArray<int32> InitDepth;
    InitDepth.Init(0, Source.LocalTypes.Num());
    for (const auto& Scope : Source.ObjectScopes)
    {
        if (Scope.programPos < Previous || Scope.programPos > static_cast<uint32>(Source.Code.Num())) return false;
        uint32 PC = 0;
        while (PC < Scope.programPos)
        {
            const uint8 Opcode = static_cast<uint8>(Source.Code[PC]);
            if (Opcode >= asBC_MAXBYTECODE) return false;
            const int Format = asBCInfo[Opcode].type;
            if (Format <= asBCTYPE_INFO || Format >= UE_ARRAY_COUNT(asBCTypeSize)) return false;
            const uint32 Width = asBCTypeSize[Format];
            if (!Width || Width > static_cast<uint32>(Source.Code.Num()) - PC) return false;
            PC += Width;
        }
        if (PC != Scope.programPos) return false;
        if (Scope.option == asBLOCK_BEGIN && Scope.variableOffset == 0) ++Depth;
        else if (Scope.option == asBLOCK_END && Scope.variableOffset == 0 && Depth > 0)
        {
            for (int32 LiveDepth : InitDepth) if (LiveDepth == Depth) return false;
            --Depth;
        }
        else
        {
            const int32 Local = Source.LocalTypes.IndexOfByPredicate(
                [&](const FLocalTypeWitness& L) { return L.Position == Scope.variableOffset; });
            if (Local == INDEX_NONE || !Depth) return false;
            if (Scope.option == asOBJ_INIT && !InitDepth[Local]) InitDepth[Local] = Depth;
            else if (Scope.option == asOBJ_UNINIT && InitDepth[Local] == Depth) InitDepth[Local] = 0;
            else return false;
        }
        Previous = Scope.programPos;
    }
    return Depth == 0;
}

// Pure transfer kernel: instruction normalization establishes the native ABI and
// identities. No receiver bytes, VM registers or application pointers are read.
enum class ETransferOp : uint8
{
    Suspend, FieldAddress, LiteralAddress, IsValid, Log, BranchLowZero, Jump, Return,
    SetLocal32, SlotAddress, WriteSlot32, ReadSlot32, CompareLocal32, BranchCompareNotZero,
    Constant64, LocalAddress, ValueTypeId, ConstructValue, InitializeOwned, ResetOwned,
};
struct FTransferInstruction
{
    ETransferOp Op;
    int32 Next = INDEX_NONE;
    int32 Branch = INDEX_NONE;
    uint64 Identity = 0;
    uint32 ReturnWords = 0;
    int32 LocalOffset = 0;
    uint32 Width = 0;
    TArray<int32> InitializeLocals;
    TArray<int32> EndLocals;
};
struct FAbstractAddress
{
    bool bField = false;
    uint64 Identity = 0;
    uint8 Kind = 0; // 0 literal, 1 field, 2 constant64, 3 local, 4 type-id
    uint32 Words = AS_PTR_SIZE;
};
enum class ERegisterProvenance : uint8 { Uninitialized, FreshSlotAddress, BooleanLowByte, Compare32 };
enum class ELocalProvenance : uint8 { Uninitialized, InitializedInt32 };
struct FAbstractFrame
{
    bool bReached = false;
    bool bSawFieldCall = false;
    bool bSawLog = false;
    bool bSlotInitialized = false;
    ERegisterProvenance Register = ERegisterProvenance::Uninitialized;
    uint64 RegisterIdentity = 0;
    TArray<ELocalProvenance> Locals;
    int32 Depth = 0;
    FAbstractAddress Addresses[32];
    TArray<uint8> ValueLocals; // 0 never constructed, 1 constructed/pending INIT, 2 live, 3 ended
    bool bOwned = false;
};

struct FNativeCallContract
{
    int32 Instruction = INDEX_NONE;
    int32 Local = INDEX_NONE;
    uint64 Components[3] = {};
    uint64 Literal = 0;
    uint32 StackWords = 0;
};

bool ProveTransfer(const TArray<FTransferInstruction>& Code, uint32 VariableWords,
    int32 StackWords, uint32 ReceiverWords, int32& FailureIndex,
    int32 ValueLocalCount = 0, TArray<FNativeCallContract>* Calls = nullptr, bool bCapturedSlotInitialValue = false)
{
    FailureIndex = 0;
    const int32 Count = Code.Num();
    if (!Count || Count > MaxInstructions || ReceiverWords != AS_PTR_SIZE ||
        VariableWords > MaxCodeWords || StackWords < 0 || StackWords > static_cast<int32>(MaxCodeWords) ||
        VariableWords > static_cast<uint32>(StackWords))
        return false;
    TArray<int32> Indegrees;
    Indegrees.Init(0, Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        FailureIndex = Index;
        const auto& Op = Code[Index];
        const bool bConditional = Op.Op == ETransferOp::BranchLowZero || Op.Op == ETransferOp::BranchCompareNotZero;
        if ((Op.Op == ETransferOp::Return && (Op.Next != INDEX_NONE || Op.Branch != INDEX_NONE)) ||
            (Op.Op != ETransferOp::Return && Op.Next == INDEX_NONE) ||
            (bConditional != (Op.Branch != INDEX_NONE)))
            return false;
        for (int32 Target : {Op.Next, Op.Branch})
        {
            if (Target == INDEX_NONE)
                continue;
            if (Target < 0 || Target >= Count)
                return false;
            ++Indegrees[Target];
        }
    }
    TArray<int32> Order;
    for (int32 Index = 0; Index < Count; ++Index)
        if (Indegrees[Index] == 0)
            Order.Add(Index);
    for (int32 Cursor = 0; Cursor < Order.Num(); ++Cursor)
        for (int32 Target : {Code[Order[Cursor]].Next, Code[Order[Cursor]].Branch})
            if (Target != INDEX_NONE && --Indegrees[Target] == 0)
                Order.Add(Target);
    if (Order.Num() != Count)
        return false;
    TArray<FAbstractFrame> Frames;
    Frames.SetNum(Count);
    Frames[0].bReached = true;
    Frames[0].bSlotInitialized = bCapturedSlotInitialValue;
    Frames[0].Locals.Init(ELocalProvenance::Uninitialized, VariableWords);
    Frames[0].ValueLocals.Init(0, ValueLocalCount);
    for (int32 Index : Order)
    {
        FailureIndex = Index;
        FAbstractFrame State = Frames[Index];
        if (!State.bReached)
            return false;
        const auto& Op = Code[Index];
        for (int32 Local : Op.InitializeLocals)
        {
            if (!State.ValueLocals.IsValidIndex(Local) || State.ValueLocals[Local] != 1) return false;
            State.ValueLocals[Local] = 2;
        }
        for (int32 Local : Op.EndLocals)
        {
            if (!State.ValueLocals.IsValidIndex(Local)) return false;
            if (State.ValueLocals[Local] == 0) continue;
            if (State.ValueLocals[Local] != 2 && State.ValueLocals[Local] != 4) return false;
            State.ValueLocals[Local] = 3;
        }
        uint32 OperandWords = 0;
        for (int32 I = 0; I < State.Depth; ++I) OperandWords += State.Addresses[I].Words;
        FNativeCallContract Call;
        Call.Instruction = Index;
        Call.StackWords = OperandWords;
        const bool bLocal32 = Op.Op == ETransferOp::SetLocal32 || Op.Op == ETransferOp::WriteSlot32 ||
            Op.Op == ETransferOp::ReadSlot32 || Op.Op == ETransferOp::CompareLocal32;
        // l_fp-offset: positive offsets name allocated locals. Zero/negative offsets
        // cover receiver/arguments, including either half of the receiver pointer.
        if (bLocal32 && (Op.Width != sizeof(int32) || Op.LocalOffset <= 0 ||
            static_cast<uint32>(Op.LocalOffset) > VariableWords || Op.LocalOffset > StackWords))
            return false;
        const int32 LocalIndex = bLocal32 ? Op.LocalOffset - 1 : 0;
        switch (Op.Op)
        {
        case ETransferOp::Suspend:
        case ETransferOp::Jump:
            break;
        case ETransferOp::SetLocal32:
            State.Locals[LocalIndex] = ELocalProvenance::InitializedInt32;
            break;
        case ETransferOp::SlotAddress:
            if (Op.Identity != 1 || Op.Width != sizeof(void*))
                return false;
            State.Register = ERegisterProvenance::FreshSlotAddress;
            State.RegisterIdentity = Op.Identity;
            break;
        case ETransferOp::WriteSlot32:
        case ETransferOp::ReadSlot32:
            if (State.Register != ERegisterProvenance::FreshSlotAddress || State.RegisterIdentity != 1)
                return false;
            if (Op.Op == ETransferOp::WriteSlot32)
            {
                if (State.Locals[LocalIndex] != ELocalProvenance::InitializedInt32)
                    return false;
                State.bSlotInitialized = true;
            }
            else
            {
                if (!State.bSlotInitialized)
                    return false;
                State.Locals[LocalIndex] = ELocalProvenance::InitializedInt32;
            }
            break;
        case ETransferOp::CompareLocal32:
            if (State.Locals[LocalIndex] != ELocalProvenance::InitializedInt32)
                return false;
            State.Register = ERegisterProvenance::Compare32;
            State.RegisterIdentity = 0;
            break;
        case ETransferOp::FieldAddress:
        case ETransferOp::LiteralAddress:
            if (!Op.Identity || State.Depth >= 32 ||
                VariableWords + OperandWords + AS_PTR_SIZE > static_cast<uint32>(StackWords))
                return false;
            State.Addresses[State.Depth++] = {Op.Op == ETransferOp::FieldAddress, Op.Identity};
            break;
        case ETransferOp::Constant64:
        case ETransferOp::LocalAddress:
        case ETransferOp::ValueTypeId:
            if (State.Depth >= 32 || VariableWords + OperandWords + Op.Width > static_cast<uint32>(StackWords)) return false;
            if (Op.Op == ETransferOp::LocalAddress && !State.ValueLocals.IsValidIndex(Op.LocalOffset)) return false;
            State.Addresses[State.Depth++] = {false,
                Op.Op == ETransferOp::LocalAddress ? uint64(Op.LocalOffset) : Op.Identity,
                uint8(Op.Op == ETransferOp::Constant64 ? 2 : Op.Op == ETransferOp::LocalAddress ? 3 : 4), Op.Width};
            break;
        case ETransferOp::ConstructValue:
            if (State.Depth < 4 || State.Addresses[State.Depth - 1].Kind != 3) return false;
            Call.Local = int32(State.Addresses[State.Depth - 1].Identity);
            if (!State.ValueLocals.IsValidIndex(Call.Local) || State.ValueLocals[Call.Local] != 0) return false;
            for (int32 I = 0; I < 3; ++I)
            {
                const auto& Scalar = State.Addresses[State.Depth - 2 - I];
                if (Scalar.Kind != 2 || Scalar.Words != 2) return false;
                Call.Components[I] = Scalar.Identity;
            }
            State.Depth -= 4;
            State.ValueLocals[Call.Local] = 1;
            State.Register = ERegisterProvenance::Uninitialized;
            State.RegisterIdentity = 0;
            break;
        case ETransferOp::InitializeOwned:
            if (State.Depth < 3 || !State.Addresses[State.Depth - 1].bField
                || State.Addresses[State.Depth - 2].Kind != 3 || State.Addresses[State.Depth - 3].Kind != 4
                || State.Addresses[State.Depth - 3].Identity != Op.Identity) return false;
            Call.Local = int32(State.Addresses[State.Depth - 2].Identity);
            if (!State.ValueLocals.IsValidIndex(Call.Local) || State.ValueLocals[Call.Local] != 2) return false;
            State.Depth -= 3;
            State.bOwned = true;
            State.Register = ERegisterProvenance::Uninitialized;
            State.RegisterIdentity = 0;
            break;
        case ETransferOp::ResetOwned:
            if (!State.Depth || !State.Addresses[State.Depth - 1].bField || !State.bOwned) return false;
            --State.Depth;
            State.bOwned = false;
            State.Register = ERegisterProvenance::Uninitialized;
            State.RegisterIdentity = 0;
            break;
        case ETransferOp::IsValid:
            if (!State.Depth || !State.Addresses[State.Depth - 1].bField)
                return false;
            --State.Depth;
            State.Register = ERegisterProvenance::BooleanLowByte; // bool thunk writes only one byte
            State.RegisterIdentity = 0;
            State.bSawFieldCall = true;
            break;
        case ETransferOp::Log:
            if (!State.Depth || State.Addresses[State.Depth - 1].bField || State.Addresses[State.Depth - 1].Kind != 0)
                return false;
            Call.Literal = State.Addresses[State.Depth - 1].Identity;
            --State.Depth;
            // No address, scalar or compare state survives a void native return.
            State.Register = ERegisterProvenance::Uninitialized;
            State.RegisterIdentity = 0;
            State.bSawLog = true;
            break;
        case ETransferOp::BranchLowZero:
            if (State.Register != ERegisterProvenance::BooleanLowByte)
                return false;
            break;
        case ETransferOp::BranchCompareNotZero:
            if (State.Register != ERegisterProvenance::Compare32)
                return false;
            break;
        case ETransferOp::Return:
            if (State.Depth || State.bOwned || Op.ReturnWords != ReceiverWords || !State.bSawFieldCall || !State.bSawLog)
                return false;
            for (uint8 Local : State.ValueLocals) if (Local == 1 || Local == 2 || Local == 4) return false;
            break;
        default:
            return false;
        }
        if (Calls && (Op.Op == ETransferOp::ConstructValue || Op.Op == ETransferOp::InitializeOwned
            || Op.Op == ETransferOp::ResetOwned || Op.Op == ETransferOp::IsValid || Op.Op == ETransferOp::Log)) Calls->Add(Call);
        for (int32 Target : {Op.Next, Op.Branch})
        {
            if (Target == INDEX_NONE)
                continue;
            auto& Merge = Frames[Target];
            if (!Merge.bReached)
                Merge = State;
            else
            {
                if (Merge.Depth != State.Depth)
                    return false;
                if (Merge.bOwned != State.bOwned) return false;
                for (int32 Local = 0; Local < ValueLocalCount; ++Local)
                {
                    if (Merge.ValueLocals[Local] == State.ValueLocals[Local]) continue;
                    if ((Merge.ValueLocals[Local] == 0 || Merge.ValueLocals[Local] == 3)
                        && (State.ValueLocals[Local] == 0 || State.ValueLocals[Local] == 3)) Merge.ValueLocals[Local] = 3;
                    else if ((Merge.ValueLocals[Local] == 0 || Merge.ValueLocals[Local] == 2 || Merge.ValueLocals[Local] == 4)
                        && (State.ValueLocals[Local] == 0 || State.ValueLocals[Local] == 2 || State.ValueLocals[Local] == 4))
                        Merge.ValueLocals[Local] = 4; // Only UNINIT can consume a path-dependent live local.
                    else return false;
                }
                for (int32 Slot = 0; Slot < State.Depth; ++Slot)
                    if (Merge.Addresses[Slot].bField != State.Addresses[Slot].bField ||
                        Merge.Addresses[Slot].Identity != State.Addresses[Slot].Identity ||
                        Merge.Addresses[Slot].Kind != State.Addresses[Slot].Kind ||
                        Merge.Addresses[Slot].Words != State.Addresses[Slot].Words)
                        return false;
                if (Merge.Register == ERegisterProvenance::FreshSlotAddress &&
                    State.Register == ERegisterProvenance::FreshSlotAddress &&
                    Merge.RegisterIdentity != State.RegisterIdentity)
                    return false;
                if (Merge.Register != State.Register || Merge.RegisterIdentity != State.RegisterIdentity)
                {
                    Merge.Register = ERegisterProvenance::Uninitialized;
                    Merge.RegisterIdentity = 0;
                }
                for (uint32 Local = 0; Local < VariableWords; ++Local)
                    if (State.Locals[Local] != ELocalProvenance::InitializedInt32)
                        Merge.Locals[Local] = ELocalProvenance::Uninitialized;
                Merge.bSlotInitialized &= State.bSlotInitialized;
                Merge.bSawFieldCall &= State.bSawFieldCall;
                Merge.bSawLog &= State.bSawLog;
            }
        }
    }
    return true;
}
}

class FCompiledTerminalDestructorProof
{
public:
    enum class ELease : uint8 { Empty, Reserved, Selecting, Executing, Returned, Released };
    struct FNativeDependency { const asCScriptFunction* Function; uint64 Generation; };
    struct FLiteralDependency { const void* Constant; uint64 Generation; };

    ~FCompiledTerminalDestructorProof();
    FReceiverShape Shape;
    asCScriptFunction* Function = nullptr; // Certified resource/scalar body, the parent for a two-frame proof.
    asCScriptFunction* DispatchFunction = nullptr; // One actual CallDestructor selection.
    asCObjectType* BodyType = nullptr;
    uint64 DispatchSourceGeneration = 0;
    int32 ActualTypeId = 0;
    uint32 ActualTypeFlags = 0;
    FCompiledTerminalDestructorObservation Observation;
    uint8 ParentFramePhase = 0; // 0 child, 1 CALL published, 2 parent, 3 RET published, 4 restored child.
    asDWORD* ChildFrame = nullptr;
    asDWORD* ParentFrame = nullptr;
    asDWORD* ChildCallStack = nullptr;
    asDWORD* FrozenStackBlock = nullptr;
    const asPWORD* FrozenCallStack = nullptr;
    asDWORD* const* FrozenStackBlocks = nullptr;
    asUINT FrozenCallCapacity = 0, FrozenBlockCount = 0, FrozenBlockCapacity = 0, FrozenStackIndex = 0;
    asUINT FrozenBlueprintFrame = 0;
    bool bParentFrame = false;
    asCScriptEngine* Engine = nullptr;
    uint64 SourceGeneration = 0;
    uint64 FreshGeneration = 0;
    bool bFreshSlotBound = false;
    TArray<FNativeDependency> Natives;
    TArray<FLiteralDependency> Literals;
    FNativeValueWitness Value;
    TArray<FLocalTypeWitness> LocalTypes;
    TArray<asSObjectVariableInfo> LocalScopes;
    TArray<FNativeCallContract> Calls;
    TArray<uint8> LocalPhases;
    TArray<FNativeCallContract> InitializedValues;
    asDWORD* Frame = nullptr;
    const FNativeCallContract* ActiveCall = nullptr;
    ENativeLeaf ActiveLeaf = ENativeLeaf::InstancedStructIsValid;
    int32 ScopeCursor = 0;
    uint32 LastProgramPosition = 0;
    uint8 ResourcePhase = 0; // 0 fresh empty, 1 initializing, 2 owned, 3 releasing
    const uint8* Resource = nullptr;
    const uint8* PreviousResource = nullptr; // Released identity only; never dereferenced.
    uint64 AllocationGeneration = 0;
    uint64 ValueGeneration = 0;
    uint64 EntryAllocationGeneration = 0;
    uint64 EntryValueGeneration = 0;
    uint64 ExpectedValue[3] = {};
    asCContext* Context = nullptr;
    asCContext* PrimaryContext = nullptr;
    asCThreadLocalData* ThreadData = nullptr;
    int32 PrimaryState = asEXECUTION_UNINITIALIZED;
    uint64 ContextGeneration = 0;
    asLineCallback LineCallback = nullptr;
    asLineCallback LoopCallback = nullptr;
    asStackPopCallback StackPopCallback = nullptr;
    bool bExceptionCallback = false;
    asSSystemFunctionInterface ExceptionCallback;
    void* ExceptionObject = nullptr;
    void* ContextCallbackParameter = nullptr;
    asREQUESTCONTEXTFUNC_t RequestCallback = nullptr;
    asRETURNCONTEXTFUNC_t ReturnCallback = nullptr;
    ELease Lease = ELease::Empty;
    asCObjectProperty* FieldProperty = nullptr;
    asCDataType FieldDataType;
    asCDataType SlotDataType;
    int32 FieldPropertyIndex = INDEX_NONE;
    uint32 PropertyCount = 0;
    asCObjectType* FieldType = nullptr;
    asCTypeInfo* LiteralType = nullptr;
    int32 ReceiverTypeId = 0;
    uint32 ReceiverFlags = 0;
    int32 ReceiverSize = 0;
    uint32 FieldFlags = 0;
    uint32 LiteralFlags = 0;
    bool bTransferProved = false;
    bool bCompleted = false;
    bool bOwnContextReference = false;
    bool bPreparedForExecution = false;
    bool bReceiverAssigned = false;
    bool bExecutionStarted = false;
    bool bViolated = false;
    // Set only after full abstract VM transfer validation, never by a witness hook.
    bool bQualified = false;
};

namespace
{
thread_local FCompiledTerminalDestructorProof* PendingProof = nullptr;
thread_local FCompiledTerminalDestructorProof* SelectingProof = nullptr;
thread_local FCompiledTerminalDestructorProof* ExecutingProof = nullptr;

constexpr int TerminalExecutionContextViolation = 86;

[[noreturn]] void FailStopTerminalEntry(FCompiledTerminalDestructorProof& Proof)
{
    Proof.bViolated = true;
    Proof.bQualified = false;
    Proof.bCompleted = false;
    PendingProof = nullptr;
    SelectingProof = nullptr;
    ExecutingProof = nullptr;
    // Either execution violated a published claim, or a retained context cannot
    // be safely returned. No logging, callbacks, destructors or atexit handlers
    // may run after either failure is detected.
#if PLATFORM_WINDOWS
    // Bypass CRT/DLL detach; other threads may hold locks required by detach.
    ::TerminateProcess(::GetCurrentProcess(), TerminalExecutionContextViolation);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
#else
    std::_Exit(TerminalExecutionContextViolation);
#endif
}


bool HasOriginalContextRoute(const FCompiledTerminalDestructorProof& Proof)
{
    return Proof.Engine && Proof.RequestCallback == &AngelscriptRequestContext &&
        Proof.ReturnCallback == &AngelscriptReturnContext &&
        Proof.Engine->requestCtxFunc == Proof.RequestCallback &&
        Proof.Engine->returnCtxFunc == Proof.ReturnCallback &&
        Proof.Engine->ctxCallbackParam == Proof.ContextCallbackParameter;
}

bool ValidateSourceResources(const FSourceWitness& Source, const FAdmissionRegistry& State)
{
    for (const auto& Local : Source.LocalTypes)
    {
        const auto* Value = State.Values.Find(Local.Type);
        const auto* Constructor = Value ? State.Natives.Find(Value->Constructor) : nullptr;
        if (!Local.Generation || !Value || Value->Generation != Local.Generation
            || !Constructor || Constructor->Generation != Local.Generation || !Value->Matches()) return false;
    }
    for (const auto& Resource : Source.Resources)
    {
        if (!Resource.Generation)
            return false;
        if (Resource.bNative)
        {
            const auto* Function = static_cast<const asCScriptFunction*>(Resource.Address);
            const auto* Witness = State.Natives.Find(Function);
            if (!Witness || Witness->Generation != Resource.Generation || !Witness->Matches(*Function))
                return false;
        }
        else
        {
            const auto* Generation = State.Literals.Find(Resource.Address);
            if (!Generation || *Generation != Resource.Generation)
                return false;
        }
    }
    return true;
}

bool ValidateLeafABI(const asCScriptFunction& Function, ENativeLeaf Leaf)
{
    const auto& Interface = *Function.sysFuncIntf;
    if (Function.funcType != asFUNC_SYSTEM || Function.jitFunction || Function.jitFunction_ParmsEntry ||
        Function.jitFunction_Raw || Function.hiddenArgumentIndex != -1 ||
        Function.determinesOutputTypeArgumentIndex != -1 || Function.DoesReturnOnStack() ||
        Function.traits.GetTrait(asTRAIT_USES_WORLDCONTEXT) ||
        Function.compileOutType != asECompileOutType::CompileCalls || Interface.baseOffset != 0 ||
        Interface.passFirstParamMetaData != asEFirstParamMetaData::None || Interface.scriptReturnSize != 0 ||
        Function.totalSpaceBeforeFunction < AS_PTR_SIZE)
        return false;
    if (Leaf == ENativeLeaf::InstancedStructIsValid)
        return Interface.caller.type == 2 && Interface.caller.MethodCaller && Interface.method &&
            Interface.callConv == ICC_THISCALL && Function.objectType && Function.IsReadOnly() &&
            Function.parameterTypes.GetLength() == 0 && Function.parameterOffsets.GetLength() == 0 &&
            Function.inOutFlags.GetLength() == 0 && Function.spaceNeededForArguments == 0 &&
            Function.returnType == asCDataType::CreatePrimitive(ttBool, false) &&
            Function.returnType.GetSizeInMemoryBytes() == 1 && Function.totalSpaceBeforeFunction == AS_PTR_SIZE;
    if (Leaf == ENativeLeaf::InstancedStructReset)
        return Interface.caller.type == 2 && Interface.caller.MethodCaller && Interface.method
            && Interface.callConv == ICC_THISCALL && Function.objectType && !Function.IsReadOnly()
            && Function.parameterTypes.GetLength() == 0 && Function.parameterOffsets.GetLength() == 0
            && Function.inOutFlags.GetLength() == 0 && Function.spaceNeededForArguments == 0
            && Function.totalSpaceBeforeFunction == AS_PTR_SIZE
            && Function.returnType == asCDataType::CreatePrimitive(ttVoid, false);
    if (Leaf == ENativeLeaf::VectorConstructor || Leaf == ENativeLeaf::InstancedStructInitializeAs)
    {
        if (Interface.caller.type != 1 || !Interface.caller.FunctionCaller || !Interface.func
            || Interface.callConv != ICC_CDECL_OBJFIRST || !Function.objectType || Function.IsReadOnly()
            || Function.returnType != asCDataType::CreatePrimitive(ttVoid, false)) return false;
        if (Leaf == ENativeLeaf::VectorConstructor)
        {
            if (!Function.traits.GetTrait(asTRAIT_CONSTRUCTOR) || Function.parameterTypes.GetLength() != 3
                || Function.parameterOffsets.GetLength() != 3 || Function.inOutFlags.GetLength() != 3
                || Function.spaceNeededForArguments != 6 || Function.totalSpaceBeforeFunction != AS_PTR_SIZE + 6) return false;
            for (asUINT I = 0; I < 3; ++I)
                if (Function.parameterTypes[I] != asCDataType::CreatePrimitive(ttFloat64, false)
                    || Function.parameterOffsets[I] != AS_PTR_SIZE + int32(I) * 2 || Function.inOutFlags[I] != asTM_NONE) return false;
            return true;
        }
        if (Function.parameterTypes.GetLength() != 1 || Function.parameterOffsets.GetLength() != 1
            || Function.inOutFlags.GetLength() != 1 || Function.parameterOffsets[0] != AS_PTR_SIZE
            || Function.inOutFlags[0] != asTM_INREF || Function.spaceNeededForArguments != AS_PTR_SIZE + 1
            || Function.totalSpaceBeforeFunction != 2 * AS_PTR_SIZE + 1) return false;
        const auto& Parameter = Function.parameterTypes[0];
        return Parameter.GetTokenType() == ttQuestion && Parameter.IsReference() && Parameter.IsReadOnly()
            && !Parameter.IsObjectHandle() && !Parameter.GetTypeInfo() && Parameter.GetSizeOnStackDWords() == AS_PTR_SIZE + 1;
    }
    if (Leaf != ENativeLeaf::LogDisplay || Interface.caller.type != 1 ||
        !Interface.caller.FunctionCaller || !Interface.func || Interface.callConv != ICC_CDECL ||
        Function.objectType || Function.returnType != asCDataType::CreatePrimitive(ttVoid, false) ||
        Function.parameterTypes.GetLength() != 1 || Function.parameterOffsets.GetLength() != 1 ||
        Function.inOutFlags.GetLength() != 1 || Function.parameterOffsets[0] != 0 ||
        Function.inOutFlags[0] != asTM_INOUTREF || Function.spaceNeededForArguments != AS_PTR_SIZE
        || Function.totalSpaceBeforeFunction != AS_PTR_SIZE)
        return false;
    const auto& Parameter = Function.parameterTypes[0];
    return Parameter.IsObject() && Parameter.IsReference() && Parameter.IsReadOnly() &&
        !Parameter.IsObjectHandle() && Parameter.GetSizeOnStackDWords() == AS_PTR_SIZE &&
        Parameter.GetTypeInfo() && Parameter.GetTypeInfo()->size == sizeof(FString) &&
        (Parameter.GetTypeInfo()->flags & asOBJ_VALUE) != 0;
}

bool ValidateFieldIdentity(const FCompiledTerminalDestructorProof& Proof)
{
    const auto* Receiver = Proof.Shape.Type;
    const auto* Body = Proof.BodyType;
    if (!Body || !Proof.FieldType || !Proof.LiteralType || Receiver->engine != Proof.Engine ||
        Receiver->typeId != Proof.ActualTypeId || Receiver->flags != Proof.ActualTypeFlags ||
        Body->typeId != Proof.ReceiverTypeId || Body->flags != Proof.ReceiverFlags ||
        Body->size != Proof.ReceiverSize || Receiver->size != Proof.ReceiverSize || Proof.ReceiverTypeId <= 0 ||
        Proof.Engine->GetTypeInfoById(Proof.ReceiverTypeId) != Body ||
        Proof.Engine->GetTypeInfoById(Proof.ActualTypeId) != Receiver ||
        Receiver->properties.GetLength() != Proof.PropertyCount || Proof.FieldPropertyIndex < 0 ||
        static_cast<uint32>(Proof.FieldPropertyIndex) >= Proof.PropertyCount ||
        Receiver->properties[Proof.FieldPropertyIndex] != Proof.FieldProperty ||
        Proof.FieldType->engine != Proof.Engine || Proof.LiteralType->engine != Proof.Engine ||
        Proof.FieldType->flags != Proof.FieldFlags || Proof.LiteralType->flags != Proof.LiteralFlags ||
        Proof.FieldType->size != sizeof(FInstancedStruct) || Proof.LiteralType->size != sizeof(FString))
        return false;
    for (uint32 Index = 0; Index < Proof.PropertyCount; ++Index)
        if (static_cast<int32>(Index) != Proof.FieldPropertyIndex && Receiver->properties[Index] &&
            Receiver->properties[Index]->byteOffset == Proof.Shape.FieldOffset)
            return false;
    const auto& Property = *Proof.FieldProperty;
    return Property.byteOffset == Proof.Shape.FieldOffset && Property.type == Proof.FieldDataType &&
        Property.type.GetTypeInfo() == Proof.FieldType && Property.type.IsObject() &&
        !Property.type.IsReference() && !Property.type.IsObjectHandle() &&
        !Property.compositeOffset && !Property.isCompositeIndirect && Property.bUeDelayedDestroyOwner &&
        (Proof.FieldFlags & asOBJ_VALUE) &&
        Proof.Shape.FieldOffset <= Proof.ReceiverSize &&
        sizeof(FInstancedStruct) <= static_cast<uint32>(Proof.ReceiverSize - Proof.Shape.FieldOffset) &&
        (reinterpret_cast<UPTRINT>(Proof.Shape.FieldStorage) % alignof(FInstancedStruct)) == 0;
}

bool ValidateSlotIdentity(const FCompiledTerminalDestructorProof& Proof)
{
    const auto& Slot = Proof.Shape.Int32Slot;
    const bool bCompleted = Proof.bCompleted && !Proof.bViolated && !Proof.Context
        && Proof.Lease == FCompiledTerminalDestructorProof::ELease::Returned;
    if (!ValidateRuntimeTerminalInt32Slot(Proof.Shape, Proof.bFreshSlotBound && !bCompleted, bCompleted))
        return false;
    if (!Slot.Property)
        return Slot == FInt32Slot();
    const auto* Type = Proof.BodyType;
    if (!Slot.Member || Slot.MemberIndex < 0 || Slot.TypeId != Proof.ReceiverTypeId || Slot.TypeSize != Proof.ReceiverSize
        || static_cast<uint32>(Slot.MemberIndex) >= Type->localProperties.GetLength()
        || Type->localProperties[Slot.MemberIndex] != Slot.Member
        || (Slot.FinalProviderEpoch && !IsTerminalCapturedInt32Slot(Slot))
        || (Slot.bRuntimeOwnedOriginal && (!Slot.FinalProviderEpoch || !IsTerminalCapturedInt32Slot(Slot)))
        || !IsTerminalInt32StorageRange(Slot.Offset, Slot.ScriptOffset, Slot.PropertiesSize, Slot.TypeSize,
            reinterpret_cast<UPTRINT>(Proof.Shape.Receiver), reinterpret_cast<UPTRINT>(Slot.Storage)))
        return false;
    return Slot.Member->type == Proof.SlotDataType && Proof.SlotDataType == asCDataType::CreatePrimitive(ttInt, false)
        && Slot.Member->type.IsPrimitive() && !Slot.Member->type.IsReference() && !Slot.Member->type.IsObjectHandle()
        && Slot.Member->type.GetSizeInMemoryBytes() == sizeof(int32) && Slot.Member->byteOffset == Slot.Offset
        && !Slot.Member->compositeOffset && !Slot.Member->isCompositeIndirect && !Slot.Member->bUeDelayedDestroyOwner
        && !Slot.Member->isAppBindProperty;
}

bool QualifyTransfer(FCompiledTerminalDestructorProof& Proof, const FSourceWitness& Source,
    FAdmissionRegistry& State, FAdmissionDiagnostic& Diagnostic)
{
    if (AS_PTR_SIZE != 2 || sizeof(void*) != 8 || Proof.Function->spaceNeededForArguments != 0 ||
        Proof.Function->totalSpaceBeforeFunction != AS_PTR_SIZE || Proof.Function->DoesReturnOnStack() ||
        Proof.Function->parameterOffsets.GetLength() || Proof.Function->inOutFlags.GetLength() ||
        Proof.Function->determinesOutputTypeArgumentIndex != -1 || !ValidateLocalMetadata(Source, State))
        return Reject(Diagnostic, EAdmissionFailure::ObjectCleanup);
    if (!ValidateSourceResources(Source, State))
        return Reject(Diagnostic, EAdmissionFailure::RetiredSource);
    for (const auto& Dependency : Proof.Natives)
    {
        const auto& Native = State.Natives.FindChecked(Dependency.Function);
        if (Native.Engine != Proof.Engine || !ValidateLeafABI(*Dependency.Function, Native.Leaf))
            return Reject(Diagnostic, EAdmissionFailure::ChangedNative);
        if (Native.Leaf == ENativeLeaf::InstancedStructIsValid)
        {
            if (Proof.FieldType && Proof.FieldType != Native.ObjectType)
                return Reject(Diagnostic, EAdmissionFailure::ChangedNative);
            Proof.FieldType = Native.ObjectType;
        }
        else if (Native.Leaf == ENativeLeaf::LogDisplay)
        {
            auto* Type = Native.Parameters[0].GetTypeInfo();
            if (Proof.LiteralType && Proof.LiteralType != Type)
                return Reject(Diagnostic, EAdmissionFailure::ChangedNative);
            Proof.LiteralType = Type;
        }
        else if (Native.Leaf == ENativeLeaf::VectorConstructor)
        {
            const auto* Value = State.Values.Find(Native.ObjectType);
            if (!Value || Value->Constructor != Dependency.Function || Value->Generation != Native.Generation
                || !Value->Matches() || (Proof.Value.Generation && Proof.Value.Generation != Value->Generation))
                return Reject(Diagnostic, EAdmissionFailure::ChangedNative);
            Proof.Value = *Value;
        }
    }
    if (!Proof.FieldType || !Proof.LiteralType || !Source.StringFactory ||
        Source.StringType.GetTypeInfo() != Proof.LiteralType ||
        Proof.Shape.Type->properties.GetLength() > MaxInstructions)
        return Reject(Diagnostic, EAdmissionFailure::InvalidReceiverShape);
    Proof.ActualTypeId = Proof.Shape.Type->typeId;
    Proof.ActualTypeFlags = Proof.Shape.Type->flags;
    Proof.ReceiverTypeId = Proof.BodyType->typeId;
    Proof.ReceiverFlags = Proof.BodyType->flags;
    Proof.ReceiverSize = Proof.BodyType->size;
    Proof.FieldFlags = Proof.FieldType->flags;
    Proof.LiteralFlags = Proof.LiteralType->flags;
    Proof.PropertyCount = Proof.Shape.Type->properties.GetLength();
    for (uint32 Index = 0; Index < Proof.PropertyCount; ++Index)
    {
        auto* Property = Proof.Shape.Type->properties[Index];
        if (Property && Property->byteOffset == Proof.Shape.FieldOffset)
        {
            if (Proof.FieldProperty)
                return Reject(Diagnostic, EAdmissionFailure::InvalidReceiverShape);
            Proof.FieldProperty = Property;
            Proof.FieldPropertyIndex = Index;
            Proof.FieldDataType = Property->type;
        }
    }
    if (!Proof.FieldProperty || !ValidateFieldIdentity(Proof))
        return Reject(Diagnostic, EAdmissionFailure::InvalidReceiverShape);
    if (Proof.Shape.Int32Slot.Property)
    {
        const auto& Slot = Proof.Shape.Int32Slot;
        if (!Source.bInt32MemberWitness || Source.Int32Member != Slot.Member
            || Source.Int32MemberIndex != Slot.MemberIndex || Source.Int32Offset != Slot.Offset)
            return Reject(Diagnostic, EAdmissionFailure::MissingSourceWitness);
        Proof.SlotDataType = Source.Int32DataType;
    }
    if (!ValidateSlotIdentity(Proof))
        return Reject(Diagnostic, EAdmissionFailure::InvalidReceiverShape);
    Proof.LocalTypes = Source.LocalTypes;
    Proof.LocalScopes = Source.ObjectScopes;
    Proof.LocalPhases.Init(0, Source.LocalTypes.Num());
    Proof.InitializedValues.SetNum(Source.LocalTypes.Num());
    if (Source.ObjectTypes && !Proof.Value.Generation) return Reject(Diagnostic, EAdmissionFailure::ObjectCleanup);
    for (const auto& Dependency : Proof.Natives)
    {
        const auto& Native = State.Natives.FindChecked(Dependency.Function);
        if ((Native.Leaf == ENativeLeaf::InstancedStructInitializeAs || Native.Leaf == ENativeLeaf::InstancedStructReset)
            && (!Proof.Value.Generation || Native.ObjectType != Proof.FieldType))
            return Reject(Diagnostic, EAdmissionFailure::ChangedNative);
    }
    TArray<int32> Boundaries;
    Boundaries.Init(INDEX_NONE, Source.Code.Num());
    for (int32 Index = 0; Index < Diagnostic.Instructions.Num(); ++Index)
        Boundaries[Diagnostic.Instructions[Index].WordOffset] = Index;
    TArray<FTransferInstruction> Normalized;
    for (int32 Index = 0; Index < Diagnostic.Instructions.Num(); ++Index)
    {
        const auto& Instruction = Diagnostic.Instructions[Index];
        if (Instruction.BranchTarget != INDEX_NONE && Instruction.BranchTarget <= static_cast<int32>(Instruction.WordOffset))
            return Reject(Diagnostic, EAdmissionFailure::InvalidBranch, Instruction.WordOffset);
        FTransferInstruction Op;
        Op.Next = Index + 1;
        switch (Instruction.Opcode)
        {
        case asBC_SUSPEND:
            Op.Op = ETransferOp::Suspend;
            break;
        case asBC_PshC8:
            Op.Op = ETransferOp::Constant64;
            Op.Identity = asBC_QWORDARG(Source.Code.GetData() + Instruction.WordOffset);
            Op.Width = 2;
            break;
        case asBC_PSF:
            Op.Op = ETransferOp::LocalAddress;
            Op.LocalOffset = Source.LocalTypes.IndexOfByPredicate(
                [&](const FLocalTypeWitness& Local) { return Local.Position == static_cast<int16>(Instruction.ShortOperand); });
            if (Op.LocalOffset == INDEX_NONE || Source.LocalTypes[Op.LocalOffset].Type != Proof.Value.Type)
                return Reject(Diagnostic, EAdmissionFailure::ObjectCleanup, Instruction.WordOffset);
            Op.Width = AS_PTR_SIZE;
            break;
        case asBC_TYPEID:
            Op.Op = ETransferOp::ValueTypeId;
            Op.Identity = asBC_DWORDARG(Source.Code.GetData() + Instruction.WordOffset);
            Op.Width = 1;
            if (!Proof.Value.Generation || Op.Identity != static_cast<uint32>(Proof.Value.TypeId))
                return Reject(Diagnostic, EAdmissionFailure::ChangedNative, Instruction.WordOffset);
            break;
        case asBC_SetV4:
        case asBC_WRTV4:
        case asBC_RDR4:
        case asBC_CMPIi:
            if (!Proof.Shape.Int32Slot.Property)
                return Reject(Diagnostic, EAdmissionFailure::InvalidReceiverShape, Instruction.WordOffset);
            Op.LocalOffset = static_cast<int16>(Instruction.ShortOperand);
            Op.Width = sizeof(int32);
            for (const auto& Local : Source.LocalTypes)
                if (TerminalStorageRangesOverlap(-Op.LocalOffset * int32(sizeof(asDWORD)), sizeof(int32),
                    -Local.Position * int32(sizeof(asDWORD)), Local.Size))
                    return Reject(Diagnostic, EAdmissionFailure::ObjectCleanup, Instruction.WordOffset);
            Op.Op = Instruction.Opcode == asBC_SetV4 ? ETransferOp::SetLocal32
                : Instruction.Opcode == asBC_WRTV4 ? ETransferOp::WriteSlot32
                : Instruction.Opcode == asBC_RDR4 ? ETransferOp::ReadSlot32 : ETransferOp::CompareLocal32;
            break;
        case asBC_LoadThisR:
            if (!Proof.Shape.Int32Slot.Property || static_cast<int16>(Instruction.ShortOperand) != Proof.Shape.Int32Slot.Offset
                || Instruction.TypeOperand != static_cast<uint32>(Proof.ReceiverTypeId))
                return Reject(Diagnostic, EAdmissionFailure::InvalidReceiverShape, Instruction.WordOffset);
            Op.Op = ETransferOp::SlotAddress;
            Op.Identity = 1;
            Op.Width = sizeof(void*);
            break;
        case asBC_PshThisMemRef:
            if (static_cast<int16>(Instruction.ShortOperand) != Proof.Shape.FieldOffset ||
                Instruction.TypeOperand != static_cast<uint32>(Proof.ReceiverTypeId))
                return Reject(Diagnostic, EAdmissionFailure::InvalidReceiverShape, Instruction.WordOffset);
            Op.Op = ETransferOp::FieldAddress;
            Op.Identity = 1; // The only address producer is the certified inline field.
            break;
        case asBC_PGA:
            if (!Instruction.bLiveLiteral)
                return Reject(Diagnostic, EAdmissionFailure::UnwitnessedLiteral, Instruction.WordOffset);
            Op.Op = ETransferOp::LiteralAddress;
            Op.Identity = asBC_PTRARG(Source.Code.GetData() + Instruction.WordOffset);
            break;
        case asBC_CALLSYS:
            if (Instruction.NativeRole == static_cast<uint8>(ENativeLeaf::InstancedStructIsValid))
                Op.Op = ETransferOp::IsValid;
            else if (Instruction.NativeRole == static_cast<uint8>(ENativeLeaf::LogDisplay))
                Op.Op = ETransferOp::Log;
            else if (Instruction.NativeRole == static_cast<uint8>(ENativeLeaf::VectorConstructor))
                Op.Op = ETransferOp::ConstructValue;
            else if (Instruction.NativeRole == static_cast<uint8>(ENativeLeaf::InstancedStructInitializeAs))
            {
                Op.Op = ETransferOp::InitializeOwned;
                Op.Identity = Proof.Value.TypeId;
            }
            else if (Instruction.NativeRole == static_cast<uint8>(ENativeLeaf::InstancedStructReset))
                Op.Op = ETransferOp::ResetOwned;
            else
                return Reject(Diagnostic, EAdmissionFailure::UnwitnessedNative, Instruction.WordOffset);
            break;
        case asBC_JLowZ:
            Op.Op = ETransferOp::BranchLowZero;
            Op.Branch = Boundaries[Instruction.BranchTarget];
            break;
        case asBC_JNZ:
            Op.Op = ETransferOp::BranchCompareNotZero;
            Op.Branch = Boundaries[Instruction.BranchTarget];
            break;
        case asBC_JMP:
            Op.Op = ETransferOp::Jump;
            Op.Next = Boundaries[Instruction.BranchTarget];
            break;
        case asBC_RET:
            Op.Op = ETransferOp::Return;
            Op.ReturnWords = Instruction.ShortOperand;
            Op.Next = INDEX_NONE;
            break;
        default:
            return Reject(Diagnostic, EAdmissionFailure::UnsupportedInstruction, Instruction.WordOffset);
        }
        for (const auto& Scope : Source.ObjectScopes)
            if (Scope.programPos == Instruction.WordOffset && (Scope.option == asOBJ_INIT || Scope.option == asOBJ_UNINIT))
            {
                const int32 Local = Source.LocalTypes.IndexOfByPredicate(
                    [&](const FLocalTypeWitness& L) { return L.Position == Scope.variableOffset; });
                if (Scope.option == asOBJ_INIT) Op.InitializeLocals.Add(Local);
                else Op.EndLocals.Add(Local);
            }
        Normalized.Add(Op);
    }
    int32 FailureIndex = 0;
    if (!ProveTransfer(Normalized, Source.VariableSpace, Source.StackNeeded, AS_PTR_SIZE, FailureIndex,
        Source.LocalTypes.Num(), &Proof.Calls, (Proof.Shape.Int32Slot.FinalProviderEpoch != 0 && IsTerminalCapturedInt32Slot(Proof.Shape.Int32Slot))))
        return Reject(Diagnostic, EAdmissionFailure::TransferContractUnqualified,
            Diagnostic.Instructions[FailureIndex].WordOffset);
    Proof.bTransferProved = true;
    for (auto& Call : Proof.Calls) Call.Instruction = Diagnostic.Instructions[Call.Instruction].WordOffset;
    return true;
}

constexpr asUINT TerminalParentCallWords = 11; // Stock as_context.cpp CALLSTACK_FRAME_SIZE.

bool IsExactDefaultParentBody(const FSourceWitness& Source, int ParentId)
{
    if (!Source.bWithinLimit || Source.Code.Num() != 4 || Source.VariableSpace || Source.HeapObjects
        || Source.ObjectTypes || Source.ObjectPositions || Source.Resources.Num() || Source.LocalTypes.Num()
        || Source.StackNeeded < 0 || Source.StackNeeded > int32(MaxCodeWords)
        || Source.ArgumentWords || Source.TotalWords != AS_PTR_SIZE) return false;
    const auto* Code = Source.Code.GetData();
    return Code[0] == asDWORD(asBC_PshVPtr)
        && Code[1] == asDWORD(asBC_CALL) && asBC_INTARG(Code + 1) == ParentId
        && Code[3] == (asDWORD(asBC_RET) | (asDWORD(AS_PTR_SIZE) << 16));
}

bool ValidateDefaultParentSource(const FCompiledTerminalDestructorProof& Proof, FAdmissionRegistry& State)
{
    if (!Proof.bParentFrame)
        return Proof.DispatchFunction == Proof.Function && Proof.BodyType == Proof.Shape.Type;
    const auto* Compiler = State.DefaultDestructors.Find(Proof.DispatchFunction);
    const auto* Child = State.Sources.Find(Proof.DispatchFunction);
    const auto* Parent = State.Sources.Find(Proof.Function);
    if (!Compiler || !Child || !Parent || !Proof.Shape.bAllowDefaultParentFrame
        || !Proof.DispatchSourceGeneration || Compiler->ChildGeneration != Proof.DispatchSourceGeneration
        || Compiler->ParentGeneration != Proof.SourceGeneration || Compiler->Parent != Proof.Function
        || Compiler->ChildType != Proof.Shape.Type || Compiler->ParentType != Proof.BodyType
        || Child->Generation != Proof.DispatchSourceGeneration || Parent->Generation != Proof.SourceGeneration
        || Child->ObjectType != Proof.Shape.Type || Parent->ObjectType != Proof.BodyType
        || !Child->Matches(*Proof.DispatchFunction) || !Parent->Matches(*Proof.Function)
        || Child->Engine != Proof.Engine || Parent->Engine != Proof.Engine || Child->Module != Parent->Module
        || Child->Module != Proof.Shape.Type->module || Proof.Shape.FieldOwnerType != Proof.BodyType
        || !Proof.Shape.Int32Slot.Property || !Proof.Shape.Int32Slot.Member
        || !Proof.Shape.Int32Slot.bRuntimeOwnedOriginal || !Proof.Shape.Int32Slot.FinalProviderEpoch
        || Proof.Shape.Int32Slot.Owner != Proof.BodyType->GetUserData()
        || Proof.Shape.Type->derivedFrom != Proof.BodyType || Proof.BodyType->derivedFrom
        || Proof.Shape.Type->localProperties.GetLength()
        || Proof.Shape.Type->size != Proof.BodyType->size
        || Proof.Shape.Type->properties.GetLength() != Proof.BodyType->properties.GetLength()
        || Proof.DispatchFunction->objectType != Proof.Shape.Type
        || Proof.Shape.Type->beh.destruct != Proof.DispatchFunction->id
        || Proof.DispatchFunction->parameterTypes.GetLength() || Proof.DispatchFunction->parameterOffsets.GetLength()
        || Proof.DispatchFunction->inOutFlags.GetLength() || Proof.DispatchFunction->hiddenArgumentIndex != -1
        || Proof.DispatchFunction->determinesOutputTypeArgumentIndex != -1 || Proof.DispatchFunction->vfTableIdx != -1
        || Proof.DispatchFunction->returnType != asCDataType::CreatePrimitive(ttVoid, false)
        || Proof.DispatchFunction->DoesReturnOnStack()
        || Proof.DispatchFunction->compileOutType != asECompileOutType::CompileCalls
        || !IsExactDefaultParentBody(*Child, Proof.Function->id)) return false;
    // Every inherited pointer is the unique original parent declaration; no invented child member.
    for (asUINT I = 0; I != Proof.BodyType->localProperties.GetLength(); ++I)
    {
        const auto* Member = Proof.BodyType->localProperties[I];
        if (!Member || Member != Proof.BodyType->properties[I]
            || Member != Proof.Shape.Type->properties[I]) return false;
        for (asUINT Previous = 0; Previous != I; ++Previous)
            if (Proof.BodyType->localProperties[Previous] == Member) return false;
    }
    return true;
}

bool ValidateFrozenParentStack(const FCompiledTerminalDestructorProof& Proof)
{
    const auto* C = Proof.Context;
    return C && Proof.ChildFrame && Proof.ParentFrame && Proof.FrozenStackBlock
        && C->m_stackIndex == Proof.FrozenStackIndex
        && C->m_stackBlocks.GetLength() == Proof.FrozenBlockCount
        && C->m_stackBlocks.GetCapacity() == Proof.FrozenBlockCapacity
        && C->m_stackBlocks.AddressOf() == Proof.FrozenStackBlocks
        && C->m_stackBlocks[C->m_stackIndex] == Proof.FrozenStackBlock
        && C->m_callStack.AddressOf() == Proof.FrozenCallStack
        && C->m_callStack.GetCapacity() == Proof.FrozenCallCapacity
        && Proof.FrozenCallCapacity >= TerminalParentCallWords
        && C->m_blueprintStackFrameIndex == Proof.FrozenBlueprintFrame
        && !C->m_isStackMemoryNotAllocated;
}

bool ReserveParentFrames(FCompiledTerminalDestructorProof& Proof)
{
    auto* C = Proof.Context;
    if (!C || C->m_status != asEXECUTION_PREPARED || C->m_callStack.GetLength()
        || C->m_stackBlocks.GetLength() == 0 || C->m_stackIndex >= C->m_stackBlocks.GetLength()
        || Proof.DispatchFunction->scriptData->variableSpace) return false;
    const int32 Needed = Proof.DispatchFunction->scriptData->stackNeeded + Proof.Function->scriptData->stackNeeded
        + AS_PTR_SIZE + 4;
    if (Needed < 0 || Needed > int32(MaxCodeWords)) return false;
    const UPTRINT Bottom = reinterpret_cast<UPTRINT>(C->m_stackBlocks[C->m_stackIndex]);
    const UPTRINT Stack = reinterpret_cast<UPTRINT>(C->m_regs.stackPointer);
    if (Stack < Bottom || Stack - Bottom < (UPTRINT(Needed) + 2 * AS_PTR_SIZE) * sizeof(asDWORD)) return false;
    // Prepare already allocated the actual block. Insufficient blocks are refused before deferral;
    // this reserve must leave the prepared receiver frame and original stack index in place.
    auto* SavedFrame = C->m_regs.stackFramePointer;
    auto* SavedStack = C->m_regs.stackPointer;
    const asUINT SavedIndex = C->m_stackIndex;
    if (!C->ReserveStackSpace(asUINT(Needed)) || C->m_regs.stackFramePointer != SavedFrame
        || C->m_regs.stackPointer != SavedStack || C->m_stackIndex != SavedIndex) return false;
    if (C->m_callStack.GetCapacity() < TerminalParentCallWords)
        C->m_callStack.AllocateNoConstruct(TerminalParentCallWords, true);
    if (C->m_callStack.GetCapacity() < TerminalParentCallWords) return false;
    Proof.ChildFrame = SavedFrame;
    Proof.ChildCallStack = SavedFrame - AS_PTR_SIZE;
    Proof.ParentFrame = reinterpret_cast<asDWORD*>(reinterpret_cast<UPTRINT>(Proof.ChildCallStack) & ~UPTRINT(15));
    Proof.FrozenStackIndex = SavedIndex;
    Proof.FrozenStackBlock = C->m_stackBlocks[SavedIndex];
    Proof.FrozenStackBlocks = C->m_stackBlocks.AddressOf();
    Proof.FrozenBlockCount = C->m_stackBlocks.GetLength();
    Proof.FrozenBlockCapacity = C->m_stackBlocks.GetCapacity();
    Proof.FrozenCallStack = C->m_callStack.AddressOf();
    Proof.FrozenCallCapacity = C->m_callStack.GetCapacity();
    Proof.FrozenBlueprintFrame = C->m_blueprintStackFrameIndex;
    return ValidateFrozenParentStack(Proof);
}

bool ValidateSavedChildFrame(const FCompiledTerminalDestructorProof& Proof)
{
    const auto* C = Proof.Context;
    if (!ValidateFrozenParentStack(Proof) || C->m_callStack.GetLength() != TerminalParentCallWords) return false;
    const asPWORD* Saved = C->m_callStack.AddressOf();
    return Saved[0] == reinterpret_cast<asPWORD>(Proof.ChildFrame)
        && Saved[1] == reinterpret_cast<asPWORD>(Proof.DispatchFunction)
        && Saved[2] == reinterpret_cast<asPWORD>(Proof.DispatchFunction->scriptData->byteCode.AddressOf() + 3)
        && Saved[3] == reinterpret_cast<asPWORD>(Proof.ChildCallStack)
        && Saved[4] == Proof.FrozenStackIndex && Saved[10] == Proof.FrozenBlueprintFrame
        && *reinterpret_cast<void* const*>(Proof.ChildFrame) == Proof.Shape.Receiver;
}


bool ValidateDependencyGenerations(const FCompiledTerminalDestructorProof& Proof, FAdmissionRegistry& State)
{
    const auto* Source = State.Sources.Find(Proof.Function);
    if (!Proof.SourceGeneration || !Source || Source->Generation != Proof.SourceGeneration
        || (Proof.Shape.RemovedSourceGeneration && Source->Generation != Proof.Shape.RemovedSourceGeneration)
        || !ValidateRuntimeTerminalSourceAssociation(Proof.Shape.Receiver, Proof.Shape.OwnerCookie,
            Proof.Shape.PlanId, Proof.Shape.RemovedSourceGeneration))
        return false;
    // Check every original generation before any function/type metadata access.
    // Registry lookups use addresses only; retired or reused addresses gain no authority.
    for (const auto& Local : Source->LocalTypes)
    {
        const auto* Value = State.Values.Find(Local.Type);
        const auto* Constructor = Value ? State.Natives.Find(Value->Constructor) : nullptr;
        if (!Local.Generation || !Value || Value->Generation != Local.Generation
            || !Constructor || Constructor->Generation != Local.Generation) return false;
    }
    for (const auto& Resource : Source->Resources)
    {
        if (!Resource.Generation)
            return false;
        if (Resource.bNative)
        {
            const auto* Native = State.Natives.Find(static_cast<const asCScriptFunction*>(Resource.Address));
            if (!Native || Native->Generation != Resource.Generation)
                return false;
        }
        else
        {
            const auto* Generation = State.Literals.Find(Resource.Address);
            if (!Generation || *Generation != Resource.Generation)
                return false;
        }
    }
    for (const auto& Dependency : Proof.Natives)
    {
        const auto* Native = State.Natives.Find(Dependency.Function);
        if (!Dependency.Generation || !Native || Native->Generation != Dependency.Generation)
            return false;
    }
    for (const auto& Dependency : Proof.Literals)
    {
        const auto* Generation = State.Literals.Find(Dependency.Constant);
        if (!Dependency.Generation || !Generation || *Generation != Dependency.Generation)
            return false;
    }
    return true;
}

bool ValidateCodeDependencies(const FCompiledTerminalDestructorProof& Proof, FAdmissionRegistry& State)
{
    if (!ValidateDependencyGenerations(Proof, State) || !ValidateDefaultParentSource(Proof, State))
        return false;
    const auto* Source = State.Sources.Find(Proof.Function);
    if (!Source || Source->Generation != Proof.SourceGeneration || !Source->Matches(*Proof.Function) ||
        Proof.BodyType->beh.destruct != Proof.Function->id || Proof.Shape.Type->engine != Proof.Engine ||
        Proof.BodyType->derivedFrom || Proof.Function->vfTableIdx != -1 ||
        Proof.Function->objectType != Proof.BodyType || Proof.Function->parameterTypes.GetLength() ||
        Proof.Function->returnType != asCDataType::CreatePrimitive(ttVoid, false) ||
        Proof.Function->hiddenArgumentIndex != -1 ||
        Proof.Function->compileOutType != asECompileOutType::CompileCalls)
        return false;
    if (!Proof.bTransferProved || !ValidateSourceResources(*Source, State) ||
        Source->HeapObjects || Source->ObjectTypes != Source->ObjectPositions ||
        Proof.Function->spaceNeededForArguments != 0 || Proof.Function->totalSpaceBeforeFunction != AS_PTR_SIZE ||
        Proof.Function->parameterOffsets.GetLength() || Proof.Function->inOutFlags.GetLength() ||
        Proof.Function->determinesOutputTypeArgumentIndex != -1 || Proof.Function->DoesReturnOnStack() ||
        Proof.Shape.Type->module != Source->Module)
        return false;
    for (const auto& Dependency : Proof.Natives)
    {
        const auto* Native = State.Natives.Find(Dependency.Function);
        if (!Native || Native->Generation != Dependency.Generation || !Native->Matches(*Dependency.Function) ||
            !ValidateLeafABI(*Dependency.Function, Native->Leaf))
            return false;
    }
    if (Proof.Value.Generation)
    {
        const auto* Value = State.Values.Find(Proof.Value.Type);
        if (!Value || Value->Generation != Proof.Value.Generation || !Proof.Value.Matches()) return false;
    }
    for (const auto& Dependency : Proof.Literals)
    {
        const auto* Generation = State.Literals.Find(Dependency.Constant);
        if (!Generation || *Generation != Dependency.Generation)
            return false;
    }
    return ValidateFieldIdentity(Proof) && ValidateSlotIdentity(Proof);
}

bool ValidateCallbacks(const FCompiledTerminalDestructorProof& Proof);

bool ValidateTerminalFrame(const FCompiledTerminalDestructorProof& Proof)
{
    return Proof.bExecutionStarted && Proof.Frame && Proof.Context
        && Proof.Context->m_regs.stackFramePointer == Proof.Frame
        && (!Proof.bParentFrame || (ValidateFrozenParentStack(Proof)
            && (Proof.ParentFramePhase != 2 || ValidateSavedChildFrame(Proof))))
        && *reinterpret_cast<void* const*>(Proof.Frame) == Proof.Shape.Receiver;
}

#if WITH_DEV_AUTOMATION_TESTS
thread_local uint32 LocalValueReadsForTest = 0;
#endif

bool ValidateActiveLocalValues(const FCompiledTerminalDestructorProof& Proof)
{
    for (int32 I = 0; I < Proof.LocalPhases.Num(); ++I)
        if (Proof.LocalPhases[I] == 1 || Proof.LocalPhases[I] == 2)
        {
#if WITH_DEV_AUTOMATION_TESTS
            ++LocalValueReadsForTest;
#endif
            if (FMemory::Memcmp(Proof.Frame - Proof.LocalTypes[I].Position,
                Proof.InitializedValues[I].Components, sizeof(FVector))) return false;
        }
    return true;
}

bool ValidateResource(const FCompiledTerminalDestructorProof& Proof)
{
    if (!ValidateTerminalFrame(Proof)
        || !ValidateTerminalFreshStorage(Proof.Shape, Proof.FreshGeneration)) return false;
    if (!ValidateActiveLocalValues(Proof)) return false;
    const auto* Fresh = static_cast<const FInstancedStruct*>(Proof.Shape.FieldStorage);
    if (Proof.ResourcePhase == 0)
        return !Proof.Resource && !Fresh->GetMemory() && !Fresh->GetScriptStruct();
    if (Proof.ResourcePhase != 2 || !Proof.Value.Generation || !Proof.Resource
        || !Proof.AllocationGeneration || !Proof.ValueGeneration || Fresh->GetMemory() != Proof.Resource
        || Fresh->GetScriptStruct() != Proof.Value.Provider
        || reinterpret_cast<UPTRINT>(Proof.Resource) % alignof(FVector)) return false;
    return FMemory::Memcmp(Proof.Resource, Proof.ExpectedValue, sizeof(FVector)) == 0;
}

bool IsSeparateOwnedAllocation(UPTRINT Memory, UPTRINT Frame, uint32 StackWords, UPTRINT Field)
{
    constexpr UPTRINT Size = sizeof(FVector);
    const UPTRINT FrameSize = static_cast<UPTRINT>(StackWords) * sizeof(asDWORD);
    if (!Memory || Memory % alignof(FVector) || Memory > MAX_uint64 - Size || Frame < FrameSize
        || Frame > MAX_uint64 - AS_PTR_SIZE * sizeof(asDWORD) || Field > MAX_uint64 - sizeof(FInstancedStruct)) return false;
    const auto Overlaps = [&](UPTRINT Lower, UPTRINT Upper) { return Memory < Upper && Lower < Memory + Size; };
    return !Overlaps(Frame - FrameSize, Frame + AS_PTR_SIZE * sizeof(asDWORD))
        && !Overlaps(Field, Field + sizeof(FInstancedStruct));
}

bool AdvanceLocalScopes(FCompiledTerminalDestructorProof& Proof, uint32 PC, uint32 CodeLength)
{
    if (PC < Proof.LastProgramPosition || PC >= CodeLength) return false;
    while (Proof.ScopeCursor < Proof.LocalScopes.Num() && Proof.LocalScopes[Proof.ScopeCursor].programPos <= PC)
    {
        const auto& Scope = Proof.LocalScopes[Proof.ScopeCursor++];
        if (Scope.option != asOBJ_INIT && Scope.option != asOBJ_UNINIT) continue;
        const int32 Local = Proof.LocalTypes.IndexOfByPredicate(
            [&](const FLocalTypeWitness& L) { return L.Position == Scope.variableOffset; });
        if (!Proof.LocalPhases.IsValidIndex(Local)) return false;
        auto& Phase = Proof.LocalPhases[Local];
        // An unconstructed local on an untaken source branch has no lifetime.
        if (!Phase) continue;
        if (Scope.option == asOBJ_INIT && Phase == 1) Phase = 2;
        else if (Scope.option == asOBJ_UNINIT && Phase == 2) Phase = 3;
        else return false;
    }
    Proof.LastProgramPosition = PC;
    return true;
}

bool PopulateResourceView(const FCompiledTerminalDestructorProof& Proof, FAngelscriptTerminalResourceView& Out)
{
    Out = {};
    Out.Receiver = reinterpret_cast<UPTRINT>(Proof.Shape.Receiver);
    Out.FreshStorage = reinterpret_cast<UPTRINT>(Proof.Shape.FieldStorage);
    Out.Resource = reinterpret_cast<UPTRINT>(Proof.Resource);
    Out.ValueType = reinterpret_cast<UPTRINT>(Proof.Value.Type);
    Out.FreshEpoch = Proof.FreshGeneration;
    Out.AllocationGeneration = Proof.AllocationGeneration;
    Out.ValueGeneration = Proof.ValueGeneration;
    Out.ProgramPosition = Proof.ActiveCall->Instruction;
    Out.ResourcePhase = Proof.ResourcePhase;
    for (int32 I = 0; I < Proof.LocalPhases.Num(); ++I)
        if (Proof.LocalPhases[I] == 2)
        {
            if (Out.bHasInitializedLocal)
            {
                Out = {};
                return false;
            }
            Out.LocalStorage = reinterpret_cast<UPTRINT>(Proof.Frame - Proof.LocalTypes[I].Position);
            Out.bHasInitializedLocal = true;
            FMemory::Memcpy(Out.Components, Proof.Frame - Proof.LocalTypes[I].Position, sizeof(FVector));
        }
    return true;
}

bool NativeEntry(FCompiledTerminalDestructorProof& Proof, FAdmissionRegistry& State)
{
    auto& Registers = Proof.Context->m_regs;
    const auto* Begin = Proof.Function->scriptData->byteCode.AddressOf();
    const UPTRINT Address = reinterpret_cast<UPTRINT>(Registers.programPointer);
    const UPTRINT Base = reinterpret_cast<UPTRINT>(Begin);
    const uint32 Length = Proof.Function->scriptData->byteCode.GetLength();
    if (Proof.ActiveCall || Address < Base || (Address - Base) % sizeof(asDWORD)
        || (Address - Base) / sizeof(asDWORD) >= Length || !ValidateTerminalFrame(Proof)) return false;
    const uint32 PC = (Address - Base) / sizeof(asDWORD);
    const auto* Call = Proof.Calls.FindByPredicate([&](const FNativeCallContract& C) { return C.Instruction == int32(PC); });
    if (!Call || !AdvanceLocalScopes(Proof, PC, Length) || !ValidateResource(Proof) || static_cast<uint8>(Begin[PC]) != asBC_CALLSYS
        || Registers.stackPointer != Proof.Frame - Proof.Function->scriptData->variableSpace - Call->StackWords) return false;
    const auto* Function = reinterpret_cast<const asCScriptFunction*>(asBC_PTRARG(Begin + PC));
    const auto* Native = State.Natives.Find(Function);
    if (!Native || !Native->Matches(*Function) || !ValidateLeafABI(*Function, Native->Leaf)) return false;
    const auto* Arguments = Registers.stackPointer;
    const void* Self = *reinterpret_cast<void* const*>(Arguments);
    if (Native->Leaf == ENativeLeaf::LogDisplay)
    {
        if (Self != reinterpret_cast<const void*>(Call->Literal)) return false;
    }
    else if (Native->Leaf == ENativeLeaf::VectorConstructor)
    {
        if (!Proof.LocalTypes.IsValidIndex(Call->Local) || Proof.LocalPhases[Call->Local] != 0
            || Self != Proof.Frame - Proof.LocalTypes[Call->Local].Position
            || reinterpret_cast<UPTRINT>(Self) % alignof(FVector)) return false;
        for (int32 I = 0; I < 3; ++I)
            if (FMemory::Memcmp(Arguments + AS_PTR_SIZE + I * 2, &Call->Components[I], sizeof(double))) return false;
    }
    else
    {
        if (Self != Proof.Shape.FieldStorage) return false;
        if (Native->Leaf == ENativeLeaf::InstancedStructInitializeAs)
        {
            if (!Proof.LocalTypes.IsValidIndex(Call->Local) || Proof.LocalPhases[Call->Local] != 2
                || *reinterpret_cast<void* const*>(Arguments + AS_PTR_SIZE) != Proof.Frame - Proof.LocalTypes[Call->Local].Position
                || Arguments[2 * AS_PTR_SIZE] != static_cast<asDWORD>(Proof.Value.TypeId)
                || Proof.LocalTypes[Call->Local].Type != Proof.Value.Type
                || (Proof.ResourcePhase != 0 && Proof.ResourcePhase != 2) || Proof.ValueGeneration == MAX_uint64
                || (Proof.ResourcePhase == 0 && Proof.AllocationGeneration == MAX_uint64)) return false;
            FMemory::Memcpy(Proof.ExpectedValue, Proof.Frame - Proof.LocalTypes[Call->Local].Position, sizeof(FVector));
            Proof.PreviousResource = Proof.Resource;
            Proof.ResourcePhase = 1;
        }
        else if (Native->Leaf == ENativeLeaf::InstancedStructReset)
        {
            if (Proof.ResourcePhase != 2) return false;
            Proof.PreviousResource = Proof.Resource;
            Proof.ResourcePhase = 3;
        }
        else if (Native->Leaf != ENativeLeaf::InstancedStructIsValid) return false;
    }
    Proof.ActiveCall = Call;
    Proof.ActiveLeaf = Native->Leaf;
    Proof.EntryAllocationGeneration = Proof.AllocationGeneration;
    Proof.EntryValueGeneration = Proof.ValueGeneration;
    return true;
}

bool NativeReturn(FCompiledTerminalDestructorProof& Proof)
{
    const auto* Call = Proof.ActiveCall;
    if (!Call || Proof.Context->m_regs.programPointer != Proof.Function->scriptData->byteCode.AddressOf() + Call->Instruction
        || Proof.Context->m_regs.stackFramePointer != Proof.Frame
        || Proof.Context->m_regs.stackPointer != Proof.Frame - Proof.Function->scriptData->variableSpace - Call->StackWords
        || Proof.AllocationGeneration != Proof.EntryAllocationGeneration || Proof.ValueGeneration != Proof.EntryValueGeneration
        || !ValidateTerminalFreshStorage(Proof.Shape, Proof.FreshGeneration)) return false;
    const auto* Fresh = static_cast<const FInstancedStruct*>(Proof.Shape.FieldStorage);
    if (Proof.ActiveLeaf == ENativeLeaf::VectorConstructor)
    {
        if (Proof.LocalPhases[Call->Local] != 0
            || FMemory::Memcmp(Proof.Frame - Proof.LocalTypes[Call->Local].Position, Call->Components, sizeof(FVector))) return false;
        Proof.LocalPhases[Call->Local] = 1; // Only a successful native return permits the later INIT.
        Proof.InitializedValues[Call->Local] = *Call;
    }
    else if (Proof.ActiveLeaf == ENativeLeaf::InstancedStructInitializeAs)
    {
        const auto* Memory = Fresh->GetMemory();
        if (Proof.ResourcePhase != 1 || !Memory || Fresh->GetScriptStruct() != Proof.Value.Provider
            || !IsSeparateOwnedAllocation(reinterpret_cast<UPTRINT>(Memory), reinterpret_cast<UPTRINT>(Proof.Frame),
                Proof.Function->scriptData->stackNeeded, reinterpret_cast<UPTRINT>(Proof.Shape.FieldStorage))
            || (Proof.PreviousResource && Memory != Proof.PreviousResource)
            || Memory == reinterpret_cast<const uint8*>(Proof.Frame - Proof.LocalTypes[Call->Local].Position)
            || FMemory::Memcmp(Memory, Proof.ExpectedValue, sizeof(FVector))) return false;
        if (!Proof.PreviousResource) ++Proof.AllocationGeneration;
        ++Proof.ValueGeneration;
        Proof.Resource = Memory;
        Proof.ResourcePhase = 2;
    }
    else if (Proof.ActiveLeaf == ENativeLeaf::InstancedStructReset)
    {
        if (Proof.ResourcePhase != 3 || !Proof.PreviousResource || Fresh->GetMemory() || Fresh->GetScriptStruct()) return false;
        Proof.Resource = nullptr;
        Proof.ResourcePhase = 0;
    }
    else if (Proof.ActiveLeaf == ENativeLeaf::InstancedStructIsValid)
    {
        if (static_cast<uint8>(Proof.Context->m_regs.valueRegister) != uint8(Proof.ResourcePhase == 2)) return false;
    }
    Proof.ActiveCall = nullptr;
    return ValidateResource(Proof);
}

bool ClosedLocalLifetimes(const FCompiledTerminalDestructorProof& Proof)
{
    if (Proof.ActiveCall || Proof.ResourcePhase != 0 || Proof.Resource) return false;
    for (uint8 Phase : Proof.LocalPhases) if (Phase == 1 || Phase == 2) return false;
    return true;
}

bool IsUnstartedTerminalCleanup(const FCompiledTerminalDestructorProof& Proof,
    asEContextState Status, const asDWORD* Program)
{
    if (Status != asEXECUTION_PREPARED || Program || Proof.bExecutionStarted || !Proof.bPreparedForExecution
        || Proof.bReceiverAssigned || Proof.Frame || Proof.ActiveCall || Proof.ScopeCursor || Proof.LastProgramPosition
        || Proof.ResourcePhase || Proof.Resource || Proof.AllocationGeneration || Proof.ValueGeneration) return false;
    for (uint8 Phase : Proof.LocalPhases) if (Phase != 0) return false;
    return true;
}

bool SameInterface(const asSSystemFunctionInterface& A, const asSSystemFunctionInterface& B)
{
    if (A.func != B.func || A.method != B.method || A.baseOffset != B.baseOffset ||
        A.callConv != B.callConv || A.scriptReturnSize != B.scriptReturnSize ||
        A.passFirstParamMetaData != B.passFirstParamMetaData || A.caller.type != B.caller.type)
        return false;
    if (A.caller.type == 1)
        return A.caller.FunctionCaller == B.caller.FunctionCaller;
    if (A.caller.type == 2)
        return A.caller.MethodCaller == B.caller.MethodCaller;
    return A.caller.type == 0;
}

bool ValidateCallbacks(const FCompiledTerminalDestructorProof& Proof)
{
    const auto& Context = *Proof.Context;
    return IsInGameThread() && Proof.ThreadData && Context.m_regs.tld == Proof.ThreadData &&
        Proof.ThreadData == FAngelscriptManager::GameThreadTLD && !Proof.ThreadData->activeExecution &&
        Proof.ThreadData->primaryContext == Proof.PrimaryContext &&
        (!Proof.PrimaryContext || Proof.PrimaryContext->m_status == Proof.PrimaryState) &&
        !asCContext::CanEverRunLineCallback &&
        Proof.Engine->requestCtxFunc == &AngelscriptRequestContext &&
        Proof.Engine->returnCtxFunc == &AngelscriptReturnContext &&
        Proof.Engine->ctxCallbackParam == Proof.ContextCallbackParameter &&
        Context.m_lineCallback == Proof.LineCallback && Context.m_loopDetectionCallback == Proof.LoopCallback &&
        Context.m_stackPopCallback == Proof.StackPopCallback && Context.m_exceptionCallback == Proof.bExceptionCallback &&
        Context.m_exceptionCallbackObj == Proof.ExceptionObject &&
        SameInterface(Context.m_exceptionCallbackFunc, Proof.ExceptionCallback);
}

bool ValidateIdleContextSelection(const FCompiledTerminalDestructorProof& Proof, int32 PrimaryState)
{
    if (!IsInGameThread() || !Proof.ThreadData ||
        Proof.ThreadData != FAngelscriptManager::GameThreadTLD || Proof.ThreadData->activeContext ||
        Proof.ThreadData->activeExecution || Proof.ThreadData->primaryContext != Proof.PrimaryContext)
        return false;
    // A non-active primary is tried by upstream CallDestructor, but PushState
    // returns asERROR without changing it, then the same RequestContext path runs.
    return !Proof.PrimaryContext || (Proof.PrimaryContext->m_engine == Proof.Engine &&
        Proof.PrimaryContext->m_status == PrimaryState &&
        (PrimaryState == asEXECUTION_UNINITIALIZED || PrimaryState == asEXECUTION_FINISHED));
}

bool ValidateIdleContextSelection(const FCompiledTerminalDestructorProof& Proof)
{
    return ValidateIdleContextSelection(Proof, Proof.PrimaryState);
}

bool ValidateReservedContext(const FCompiledTerminalDestructorProof& Proof, int32 PrimaryState)
{
    const auto* Context = Proof.Context;
    if (!Context || !ValidateIdleContextSelection(Proof, PrimaryState) || Context->m_regs.tld != Proof.ThreadData ||
        asCContext::CanEverRunLineCallback ||
        Proof.Engine->requestCtxFunc != &AngelscriptRequestContext ||
        Proof.Engine->returnCtxFunc != &AngelscriptReturnContext ||
        Proof.Engine->ctxCallbackParam != Proof.ContextCallbackParameter ||
        Context->m_engine != Proof.Engine || Context->m_status != asEXECUTION_PREPARED ||
        Context->m_initialFunction != Proof.DispatchFunction || Context->m_currentFunction != Proof.DispatchFunction ||
        Context->m_callStack.GetLength() || Context->m_loopDetectionCounter != 0 ||
        Context->m_doAbort || Context->m_externalSuspendRequest || Context->m_needToCleanupArgs ||
        Context->m_regs.objectRegister || Context->m_returnIsExternal || Context->m_regs.programPointer ||
        Context->m_lineCallback != Proof.LineCallback || Context->m_loopDetectionCallback != Proof.LoopCallback ||
        Context->m_stackPopCallback != Proof.StackPopCallback ||
        Context->m_exceptionCallback != Proof.bExceptionCallback ||
        Context->m_exceptionCallbackObj != Proof.ExceptionObject ||
        !SameInterface(Context->m_exceptionCallbackFunc, Proof.ExceptionCallback))
        return false;
    // PrepareScriptFunction must not allocate or move the frame on actual entry.
    if (Context->m_stackBlocks.GetLength() == 0 || Context->m_stackIndex >= Context->m_stackBlocks.GetLength() ||
        !Context->m_regs.stackPointer || Context->m_regs.stackPointer != Context->m_regs.stackFramePointer ||
        (reinterpret_cast<UPTRINT>(Context->m_regs.stackPointer) & 15u) != 0)
        return false;
    const UPTRINT Bottom = reinterpret_cast<UPTRINT>(Context->m_stackBlocks[Context->m_stackIndex]);
    const UPTRINT Stack = reinterpret_cast<UPTRINT>(Context->m_regs.stackPointer);
    const int Needed = Proof.bParentFrame
        ? Proof.DispatchFunction->scriptData->stackNeeded + Proof.Function->scriptData->stackNeeded
            + Proof.DispatchFunction->scriptData->variableSpace + AS_PTR_SIZE + 4
        : Proof.Function->scriptData->stackNeeded;
    if (Proof.bParentFrame && !ValidateFrozenParentStack(Proof)) return false;
    return Needed >= 0 && Needed <= static_cast<int>(MaxCodeWords) && Stack >= Bottom &&
        Stack - Bottom >= (static_cast<UPTRINT>(Needed) + 2 * AS_PTR_SIZE) * sizeof(asDWORD);
}

bool ValidateReservedContext(const FCompiledTerminalDestructorProof& Proof)
{
    return ValidateReservedContext(Proof, Proof.PrimaryState);
}

bool ReserveContext(FCompiledTerminalDestructorProof& Proof, FAdmissionDiagnostic& Diagnostic)
{
    Diagnostic.ContextSelectionStage = 1;
    Diagnostic.bContextGameThread = IsInGameThread();
    Diagnostic.bLineCallbacksPossible = asCContext::CanEverRunLineCallback;
    if (!Diagnostic.bContextGameThread)
        return Reject(Diagnostic, EAdmissionFailure::ActiveContextUnqualified);
    Proof.ThreadData = asCThreadManager::GetLocalData();
    if (Proof.ThreadData)
    {
        Diagnostic.bHasActiveContext = Proof.ThreadData->activeContext != nullptr;
        Diagnostic.bHasActiveExecution = Proof.ThreadData->activeExecution != nullptr;
        Diagnostic.bHasPrimaryContext = Proof.ThreadData->primaryContext != nullptr;
    }
    if (!Proof.ThreadData || Proof.ThreadData != FAngelscriptManager::GameThreadTLD ||
        Proof.ThreadData->activeContext || Proof.ThreadData->activeExecution)
        return Reject(Diagnostic, EAdmissionFailure::ActiveContextUnqualified);
    Proof.PrimaryContext = Proof.ThreadData->primaryContext;
    if (Proof.PrimaryContext)
    {
        Proof.PrimaryContext->AddRef();
        Proof.PrimaryState = Proof.PrimaryContext->m_status;
        Diagnostic.PrimaryContextState = Proof.PrimaryState;
    }
    if (!ValidateIdleContextSelection(Proof))
        return Reject(Diagnostic, EAdmissionFailure::ActiveContextUnqualified);
    if (asCContext::CanEverRunLineCallback ||
        Proof.Engine->requestCtxFunc != &AngelscriptRequestContext ||
        Proof.Engine->returnCtxFunc != &AngelscriptReturnContext || PendingProof || SelectingProof || ExecutingProof)
        return Reject(Diagnostic, EAdmissionFailure::RequestedContextUnqualified);
    Proof.ContextCallbackParameter = Proof.Engine->ctxCallbackParam;
    Proof.RequestCallback = Proof.Engine->requestCtxFunc;
    Proof.ReturnCallback = Proof.Engine->returnCtxFunc;
    Proof.Context = static_cast<asCContext*>(Proof.Engine->RequestContext());
    if (!Proof.Context)
        return Reject(Diagnostic, EAdmissionFailure::RequestedContextUnqualified);
    Proof.Lease = FCompiledTerminalDestructorProof::ELease::Reserved;
    auto* Context = Proof.Context;
    Context->AddRef();
    Proof.bOwnContextReference = true;
    {
        auto& State = Registry();
        FScopeLock Lock(&State.Mutex);
        auto* Witness = State.Contexts.Find(Context);
        if (!Witness || !Witness->Generation || Witness->Owner)
            FailStopTerminalEntry(Proof);
        Witness->Owner = &Proof;
        Proof.ContextGeneration = Witness->Generation;
    }
    Diagnostic.ContextSelectionStage = 2;
    Diagnostic.RequestedContextStateBeforePrepare = Context->m_status;
    if (Context == Proof.PrimaryContext)
        return Reject(Diagnostic, EAdmissionFailure::RequestedContextUnqualified);
    Proof.LineCallback = Context->m_lineCallback;
    Proof.LoopCallback = Context->m_loopDetectionCallback;
    Proof.StackPopCallback = Context->m_stackPopCallback;
    Proof.bExceptionCallback = Context->m_exceptionCallback;
    Proof.ExceptionCallback = Context->m_exceptionCallbackFunc;
    Proof.ExceptionObject = Context->m_exceptionCallbackObj;
    Diagnostic.ContextCallbacks = ContextCallbacks(*Context);
    // Preserve the request-time callbacks across cleanup; a changed callback
    // must not become trusted merely because Prepare returned successfully.
    Diagnostic.ContextPrepareResult = Context->Prepare(Proof.DispatchFunction);
    if (Diagnostic.ContextPrepareResult != asSUCCESS)
        return Reject(Diagnostic, EAdmissionFailure::RequestedContextUnqualified);
    if (Proof.bParentFrame && !ReserveParentFrames(Proof))
        return Reject(Diagnostic, EAdmissionFailure::RequestedContextUnqualified);
    Diagnostic.ContextSelectionStage = 3;
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto* Witness = State.Contexts.Find(Context);
    if (!Witness || Witness->Generation != Proof.ContextGeneration || Witness->Owner != &Proof)
        return Reject(Diagnostic, EAdmissionFailure::RequestedContextUnqualified);
    Diagnostic.RequestedContextStateAfterPrepare = Context->m_status;
    if (!ValidateCodeDependencies(Proof, State))
        return Reject(Diagnostic, EAdmissionFailure::RetiredSource);
    if (!ValidateReservedContext(Proof))
        return Reject(Diagnostic, EAdmissionFailure::RequestedContextUnqualified);
    Diagnostic.ContextSelectionStage = 4;
    return true;
}
}

FCompiledTerminalDestructorProof::~FCompiledTerminalDestructorProof()
{
    if (PendingProof == this)
        PendingProof = nullptr;
    if (SelectingProof == this)
        SelectingProof = nullptr;
    if (ExecutingProof == this)
        ExecutingProof = nullptr;
    if (Context)
    {
        auto* ReturnContext = Context;
        {
            auto& State = Registry();
            FScopeLock Lock(&State.Mutex);
            const auto* Witness = State.Contexts.Find(ReturnContext);
            if (Lease != ELease::Reserved || !bOwnContextReference || !ContextGeneration ||
                !Witness || Witness->Generation != ContextGeneration || Witness->Owner != this ||
                !HasOriginalContextRoute(*this))
                FailStopTerminalEntry(*this);
            if (!IsInGameThread() || ThreadData != FAngelscriptManager::GameThreadTLD ||
                ReturnContext->m_engine != Engine || ReturnContext->m_regs.tld != ThreadData ||
                ReturnContext->m_status == asEXECUTION_ACTIVE || ReturnContext->m_status == asEXECUTION_SUSPENDED)
                FailStopTerminalEntry(*this);
            State.Contexts.Remove(ReturnContext);
            Context = nullptr;
            ContextGeneration = 0;
            Lease = ELease::Released;
            bOwnContextReference = false;
            bQualified = false;
            bCompleted = false;
        }
        // Do not redispatch through mutable Engine callbacks. Authority is
        // revoked under the lock before calling the captured, still-known route.
        ReturnCallback(Engine, ReturnContext, ContextCallbackParameter);
        ReturnContext->Release();
    }
    if (PrimaryContext)
    {
        auto* ReleasePrimary = PrimaryContext;
        PrimaryContext = nullptr;
        ReleasePrimary->Release();
    }

}

void RegisterNativeLeaf(asIScriptFunction* Function, ENativeLeaf Leaf)
{
    if (!Function)
        return;
    auto* Concrete = static_cast<asCScriptFunction*>(Function);
    if (Concrete->funcType != asFUNC_SYSTEM || !Concrete->sysFuncIntf ||
        (Concrete->sysFuncIntf->caller.type != 1 && Concrete->sysFuncIntf->caller.type != 2))
        return;
    FNativeWitness Witness;
    Witness.Leaf = Leaf;
    Witness.Engine = Concrete->engine;
    Witness.ObjectType = Concrete->objectType;
    Witness.Interface = *Concrete->sysFuncIntf;
    Witness.ReturnType = Concrete->returnType;
    Witness.Traits = Concrete->traits.traits;
    Witness.ArgumentWords = Concrete->spaceNeededForArguments;
    Witness.TotalWords = Concrete->totalSpaceBeforeFunction;
    Witness.HiddenArgument = Concrete->hiddenArgumentIndex;
    Witness.OutputTypeArgument = Concrete->determinesOutputTypeArgumentIndex;
    Witness.Auxiliary = Concrete->userData;
    Witness.CompileOut = Concrete->compileOutType;
    for (asUINT Index = 0; Index < Concrete->parameterTypes.GetLength(); ++Index)
        Witness.Parameters.Add(Concrete->parameterTypes[Index]);
    for (asUINT Index = 0; Index < Concrete->parameterOffsets.GetLength(); ++Index)
        Witness.Offsets.Add(Concrete->parameterOffsets[Index]);
    for (asUINT Index = 0; Index < Concrete->inOutFlags.GetLength(); ++Index)
        Witness.Modifiers.Add(Concrete->inOutFlags[Index]);
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    Witness.Generation = State.NewGeneration();
    State.Natives.Add(Concrete, MoveTemp(Witness));
}

void RegisterStringConstant(const void* Constant)
{
    if (!Constant)
        return;
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    State.Literals.Add(Constant, State.NewGeneration());
}

void RegisterVectorValueConstructor(asIScriptFunction* Function, UScriptStruct* Provider)
{
    if (!Function || Provider != TBaseStructure<FVector>::Get()) return;
    RegisterNativeLeaf(Function, ENativeLeaf::VectorConstructor);
    auto* Constructor = static_cast<asCScriptFunction*>(Function);
    auto* Type = Constructor->objectType;
    if (!Type || Type->plainUserData != reinterpret_cast<asPWORD>(Provider)) return;
    Provider->PrepareCppStructOps();
    auto* Ops = Provider->GetCppStructOps();
    if (!Ops || !Ops->FakeVPtr) return;
    FNativeValueWitness Value;
    Value.Constructor = Constructor;
    Value.Type = Type;
    Value.Engine = Constructor->engine;
    Value.Provider = Provider;
    Value.Index = GUObjectArray.ObjectToIndex(Provider);
    Value.Serial = GUObjectArray.AllocateSerialNumber(Value.Index);
    Value.TypeId = Type->GetTypeId();
    Value.Flags = Type->flags;
    Value.StructFlags = Provider->StructFlags;
    Value.Ops = Ops;
    Value.Table = Ops->FakeVPtr;
    Value.OpsFlags = Value.Table->Flags;
    FMemory::Memcpy(&Value.Capabilities, &Value.Table->Capabilities, sizeof(Value.Capabilities));
    if (!!(Value.OpsFlags & EOpsFlag::Construct))
        Value.Construct = Value.Table->GetHandlingFunc<EOpsFlag::Construct>();
    if (!!(Value.OpsFlags & EOpsFlag::Copy))
        Value.Copy = Value.Table->GetHandlingFunc<EOpsFlag::Copy>();
    // Registration is at the canonical native binding, never inferred from a type
    // name, size, wildcard argument, or a consumer's later admission request.
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto* Native = State.Natives.Find(Constructor);
    Value.Generation = Native ? Native->Generation : 0;
    if (Value.Matches()) State.Values.Add(Type, MoveTemp(Value));
    else
    {
        const auto* Canonical = UE::CoreUObject::Private::TStructOpsFakeVTableForType<FVector>::Value;
        UE_LOG(LogTemp, Display, TEXT("SOLOAS_TERMINAL_VECTOR_WITNESS_REJECT type=%d size=%d align=%d expected_align=%d flags=%u")
            TEXT(" struct_flags=%u ops_flags=%u canonical_flags=%u construct_same=%d copy_same=%d zero=%d copy=%d")
            TEXT(" destructor=%d post_construct=%d references=%d clear=%d"),
            Value.TypeId, Type->size, Type->alignment, alignof(FVector), Value.Flags,
            static_cast<uint32>(Value.StructFlags), static_cast<uint32>(Value.OpsFlags), static_cast<uint32>(Canonical->Flags),
            !!(Canonical->Flags & EOpsFlag::Construct) && Value.Construct == Canonical->GetHandlingFunc<EOpsFlag::Construct>(),
            !!(Canonical->Flags & EOpsFlag::Copy) && Value.Copy == Canonical->GetHandlingFunc<EOpsFlag::Copy>(),
            Value.Capabilities.HasZeroConstructor, Value.Capabilities.HasCopy, Value.Capabilities.HasDestructor,
            Value.Capabilities.HasPostScriptConstruct, Value.Capabilities.HasAddStructReferencedObjects,
            Value.Capabilities.ClearOnFinishDestroy);
    }
}

#if WITH_DEV_AUTOMATION_TESTS
void TestTerminalOwnedEffectsContract(FAutomationTestBase& Test)
{
    TArray<FTransferInstruction> Code;
    auto Add = [&](ETransferOp Op, uint64 Identity = 0, uint32 Width = 0)
    {
        FTransferInstruction Instruction;
        Instruction.Op = Op;
        Instruction.Next = Code.Num() + 1;
        Instruction.Identity = Identity;
        Instruction.Width = Width;
        Code.Add(Instruction);
    };
    Add(ETransferOp::FieldAddress, 1);
    Add(ETransferOp::IsValid);
    for (uint64 Scalar : {uint64(3), uint64(2), uint64(1)}) Add(ETransferOp::Constant64, Scalar, 2);
    Add(ETransferOp::LocalAddress, 0, AS_PTR_SIZE);
    Add(ETransferOp::ConstructValue);
    Add(ETransferOp::Suspend);
    Code.Last().InitializeLocals.Add(0);
    Add(ETransferOp::ValueTypeId, 17, 1);
    Add(ETransferOp::LocalAddress, 0, AS_PTR_SIZE);
    Add(ETransferOp::FieldAddress, 1);
    Add(ETransferOp::InitializeOwned, 17);
    Add(ETransferOp::FieldAddress, 1);
    Add(ETransferOp::ResetOwned);
    Add(ETransferOp::LiteralAddress, 1);
    Add(ETransferOp::Log);
    Add(ETransferOp::Suspend);
    Code.Last().EndLocals.Add(0);
    Add(ETransferOp::Return);
    Code.Last().Next = INDEX_NONE;
    Code.Last().ReturnWords = AS_PTR_SIZE;
    int32 Failure = 0;
    auto Prove = [&](const TArray<FTransferInstruction>& Instructions)
    { return ProveTransfer(Instructions, 16, 24, AS_PTR_SIZE, Failure, 1); };
    Test.TestTrue(TEXT("Typed construction, wildcard copy, release and local closure are admitted"), Prove(Code));
    auto ReadFirst = Code;
    for (auto& Instruction : ReadFirst)
    {
        if (Instruction.Next != INDEX_NONE) Instruction.Next += 2;
        if (Instruction.Branch != INDEX_NONE) Instruction.Branch += 2;
    }
    FTransferInstruction SlotAddress;
    SlotAddress.Op = ETransferOp::SlotAddress;
    SlotAddress.Identity = 1;
    SlotAddress.Width = sizeof(void*);
    SlotAddress.Next = 1;
    FTransferInstruction ReadSlot;
    ReadSlot.Op = ETransferOp::ReadSlot32;
    ReadSlot.Width = sizeof(int32);
    ReadSlot.LocalOffset = 1;
    ReadSlot.Next = 2;
    ReadFirst.Insert(SlotAddress, 0);
    ReadFirst.Insert(ReadSlot, 1);
    Test.TestFalse(TEXT("Reading before writing remains rejected without a captured initial value"), Prove(ReadFirst));
    Test.TestTrue(TEXT("The bound captured initial value permits the first typed scalar read"),
        ProveTransfer(ReadFirst, 16, 24, AS_PTR_SIZE, Failure, 1, nullptr, true));
    auto WrongReadAddress = ReadFirst;
    WrongReadAddress[0].Identity = 2;
    Test.TestFalse(TEXT("A captured initial value cannot authorize another slot address"),
        ProveTransfer(WrongReadAddress, 16, 24, AS_PTR_SIZE, Failure, 1, nullptr, true));
    auto WrongReadLocal = ReadFirst;
    WrongReadLocal[1].LocalOffset = 0;
    Test.TestFalse(TEXT("A captured initial value cannot overwrite the receiver frame words"),
        ProveTransfer(WrongReadLocal, 16, 24, AS_PTR_SIZE, Failure, 1, nullptr, true));
    auto Changed = Code;
    Changed[8].Identity = 18;
    Test.TestFalse(TEXT("A forged wildcard type-id fails with unchanged instruction count"), Prove(Changed));
    Changed = Code;
    Changed[9].LocalOffset = 1;
    Test.TestFalse(TEXT("A wildcard cannot borrow a different local address"), Prove(Changed));
    Changed = Code;
    Changed[7].InitializeLocals.Reset();
    Test.TestFalse(TEXT("Constructor success alone does not replace the INIT boundary"), Prove(Changed));
    Changed = Code;
    Changed[16].EndLocals.Reset();
    Test.TestFalse(TEXT("A live local cannot cross normal return"), Prove(Changed));
    Changed = Code;
    Changed[13].Op = ETransferOp::IsValid;
    Test.TestFalse(TEXT("Owned resource state must close before return"), Prove(Changed));
    FCompiledTerminalDestructorProof Closed;
    Test.TestTrue(TEXT("An empty ledger has no cleanup obligation"), ClosedLocalLifetimes(Closed));
    Closed.ResourcePhase = 1;
    Test.TestFalse(TEXT("An in-progress initialization cannot clean up"), ClosedLocalLifetimes(Closed));
    Closed.ResourcePhase = 3;
    Test.TestFalse(TEXT("An in-progress release cannot clean up"), ClosedLocalLifetimes(Closed));
    Closed.ResourcePhase = 0;
    Closed.LocalPhases.Add(1);
    Test.TestFalse(TEXT("A constructed local pending INIT cannot clean up"), ClosedLocalLifetimes(Closed));
    Test.TestTrue(TEXT("Owned storage is separate from the whole frame and fresh wrapper"),
        IsSeparateOwnedAllocation(8192, 4096, 24, 6144));
    Test.TestFalse(TEXT("Native memory cannot alias an unused local in the same frame"),
        IsSeparateOwnedAllocation(4040, 4096, 24, 6144));
    Test.TestFalse(TEXT("Native memory cannot partially overlap the fresh wrapper"),
        IsSeparateOwnedAllocation(6136, 4096, 24, 6144));
    Test.TestFalse(TEXT("Native memory must be wholly aligned and non-overflowing"),
        IsSeparateOwnedAllocation(MAX_uint64 - 7, 4096, 24, 6144));

    alignas(FVector) asDWORD LocalFrame[32] = {};
    FCompiledTerminalDestructorProof ScopeProof;
    ScopeProof.Frame = LocalFrame + 24;
    FLocalTypeWitness ScopedLocal;
    ScopedLocal.Position = 14;
    ScopeProof.LocalTypes.Add(ScopedLocal);
    ScopeProof.LocalPhases.Add(2);
    ScopeProof.InitializedValues.AddDefaulted();
    ScopeProof.LocalScopes.Add({101, 14, asOBJ_UNINIT});
    LocalValueReadsForTest = 0;
    Test.TestTrue(TEXT("An active initialized local is checked"), ValidateActiveLocalValues(ScopeProof));
    Test.TestEqual(TEXT("The active local is read exactly once"), LocalValueReadsForTest, uint32(1));
    Test.TestTrue(TEXT("The actual UNINIT boundary closes the active local"), AdvanceLocalScopes(ScopeProof, 101, 103));
    ScopeProof.Frame = nullptr; // Fault injection: any subsequent local read is invalid.
    LocalValueReadsForTest = 0;
    Test.TestTrue(TEXT("An ended local is never inspected"), ValidateActiveLocalValues(ScopeProof));
    Test.TestEqual(TEXT("UNINIT prevents all reads of old local bytes"), LocalValueReadsForTest, uint32(0));
    ScopeProof.LocalPhases[0] = 0;
    ScopeProof.ScopeCursor = 0;
    Test.TestTrue(TEXT("UNINIT on an untaken branch preserves the unconstructed state"), AdvanceLocalScopes(ScopeProof, 101, 103));
    Test.TestEqual(TEXT("The untaken local remains unconstructed"), ScopeProof.LocalPhases[0], uint8(0));

    FAngelscriptTerminalResourceView View;
    FNativeCallContract ViewCall;
    ScopeProof.ActiveCall = &ViewCall;
    Test.TestTrue(TEXT("The production view represents zero live locals"), PopulateResourceView(ScopeProof, View));
    Test.TestFalse(TEXT("The empty view has no local"), View.bHasInitializedLocal);
    ScopeProof.Frame = LocalFrame + 24;
    ScopeProof.LocalPhases[0] = 2;
    Test.TestTrue(TEXT("The production view represents one live local"), PopulateResourceView(ScopeProof, View));
    Test.TestTrue(TEXT("The single live local is represented"), View.bHasInitializedLocal);
    ScopeProof.LocalTypes.Add(ScopedLocal);
    ScopeProof.LocalPhases.Add(2);
    Test.TestFalse(TEXT("Two live locals exceed the observation representation without termination"), PopulateResourceView(ScopeProof, View));
    Test.TestTrue(TEXT("An unrepresentable view exposes no partial scalar state"),
        !View.Receiver && !View.FreshStorage && !View.Resource && !View.ValueType && !View.LocalStorage
        && !View.FreshEpoch && !View.AllocationGeneration && !View.ValueGeneration && !View.ProgramPosition
        && !View.ResourcePhase && !View.bHasInitializedLocal
        && View.Components[0] == 0 && View.Components[1] == 0 && View.Components[2] == 0);

    FCompiledTerminalDestructorProof Unstarted;
    Unstarted.bPreparedForExecution = true;
    Unstarted.LocalPhases.Add(0);
    Test.TestTrue(TEXT("Prepare may clean a witnessed frame that has never begun"),
        IsUnstartedTerminalCleanup(Unstarted, asEXECUTION_PREPARED, nullptr));
    Test.TestFalse(TEXT("An exception state cannot use the Prepare cleanup route"),
        IsUnstartedTerminalCleanup(Unstarted, asEXECUTION_EXCEPTION, nullptr));
    Unstarted.bExecutionStarted = true;
    Test.TestFalse(TEXT("Executed contexts cannot use the unstarted cleanup route"),
        IsUnstartedTerminalCleanup(Unstarted, asEXECUTION_PREPARED, nullptr));
    Unstarted.bExecutionStarted = false;
    Unstarted.ActiveCall = &ViewCall;
    Test.TestFalse(TEXT("Native reentrant cleanup cannot use the unstarted route"),
        IsUnstartedTerminalCleanup(Unstarted, asEXECUTION_PREPARED, nullptr));
    Unstarted.ActiveCall = nullptr;
    Unstarted.LocalPhases[0] = 1;
    Test.TestFalse(TEXT("Constructed but not INIT locals exclude Prepare cleanup"),
        IsUnstartedTerminalCleanup(Unstarted, asEXECUTION_PREPARED, nullptr));
    Unstarted.LocalPhases[0] = 2;
    Test.TestFalse(TEXT("An active local excludes Prepare cleanup"),
        IsUnstartedTerminalCleanup(Unstarted, asEXECUTION_PREPARED, nullptr));
    Unstarted.LocalPhases[0] = 0;
    Test.TestFalse(TEXT("A published VM PC excludes unstarted cleanup"),
        IsUnstartedTerminalCleanup(Unstarted, asEXECUTION_PREPARED, LocalFrame));

    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto It = State.Values.CreateConstIterator();
    if (!Test.TestTrue(TEXT("The canonical FVector native provider was registered"), bool(It))) return;
    const auto& Value = It.Value();
    FLocalTypeWitness Local;
    Local.Type = Value.Type;
    Local.Generation = Value.Generation;
    Local.Position = 8;
    Local.TypeId = Value.TypeId;
    Local.Size = Value.Type->size;
    Local.Alignment = Value.Type->alignment;
    Local.Flags = Value.Type->flags;
    Local.UserData = Value.Type->plainUserData;
    Local.Behaviour = Value.Type->beh;
    Test.TestTrue(TEXT("A retained local type matches complete native metadata"), Local.Matches(Value.Type, 8));
    auto AlteredLocal = Local;
    ++AlteredLocal.TypeId;
    Test.TestFalse(TEXT("Type identity drift is rejected despite unchanged table counts"), AlteredLocal.Matches(Value.Type, 8));
    AlteredLocal = Local;
    ++AlteredLocal.Behaviour.destruct;
    Test.TestFalse(TEXT("A new local destructor cannot acquire cleanup authority"), AlteredLocal.Matches(Value.Type, 8));
    Test.TestFalse(TEXT("A different frame position cannot borrow the witnessed type"), Local.Matches(Value.Type, 10));
    auto AlteredValue = Value;
    AlteredValue.Capabilities.HasDestructor = !AlteredValue.Capabilities.HasDestructor;
    Test.TestFalse(TEXT("Native ops capability drift rejects the provider"), AlteredValue.Matches());
    AlteredValue = Value;
    AlteredValue.Table = nullptr;
    Test.TestFalse(TEXT("Native ops handler-table identity cannot change"), AlteredValue.Matches());
    FSourceWitness Source;
    Source.ObjectTypes = Source.ObjectPositions = 1;
    Source.VariableSpace = 16;
    Source.Code.Add(asBC_SUSPEND);
    Source.LocalTypes.Add(Local);
    Test.TestTrue(TEXT("An unused native local remains unconstructed"), ValidateLocalMetadata(Source, State));
    ++Source.LocalTypes[0].Generation;
    Test.TestFalse(TEXT("A replaced native registration invalidates local provenance"), ValidateLocalMetadata(Source, State));
    Source.LocalTypes[0] = Local;
    Source.ObjectTypes = Source.ObjectPositions = 2;
    Source.LocalTypes.Add(Local);
    Test.TestFalse(TEXT("Overlapping complete native local ranges are rejected"), ValidateLocalMetadata(Source, State));
}
#endif

void RevokeStringConstant(const void* Constant)
{
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    State.Literals.Remove(Constant);
}

void RegisterSourceFunction(asCScriptFunction* Function)
{
    if (!Function || !Function->scriptData || !Function->traits.GetTrait(asTRAIT_DESTRUCTOR))
        return;
    FSourceWitness Witness;
    Witness.Engine = Function->engine;
    Witness.Module = Function->module;
    Witness.StringFactory = Function->engine->stringFactory;
    Witness.StringType = Function->engine->stringType;
    Witness.ObjectType = Function->objectType;
    Witness.Traits = Function->traits.traits;
    const auto& Data = *Function->scriptData;
    Witness.StackNeeded = Data.stackNeeded;
    Witness.VariableSpace = Data.variableSpace;
    Witness.HeapObjects = Data.objVariablesOnHeap;
    Witness.ObjectTypes = Data.objVariableTypes.GetLength();
    Witness.ObjectPositions = Data.objVariablePos.GetLength();
    Witness.ArgumentWords = Function->spaceNeededForArguments;
    Witness.TotalWords = Function->totalSpaceBeforeFunction;
    if (Data.objVariableInfo.GetLength() <= MaxInstructions)
        Witness.ObjectScopes.Append(Data.objVariableInfo.AddressOf(), Data.objVariableInfo.GetLength());
    Witness.bWithinLimit = Data.byteCode.GetLength() > 0 && Data.byteCode.GetLength() <= MaxCodeWords &&
        Data.objVariableInfo.GetLength() <= MaxInstructions;
    if (Witness.bWithinLimit)
        Witness.Code.Append(Data.byteCode.AddressOf(), Data.byteCode.GetLength());
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    Witness.Generation = State.NewGeneration();
    if (Witness.ObjectTypes <= MaxInstructions && Witness.ObjectPositions == Witness.ObjectTypes)
        for (asUINT Index = 0; Index < Witness.ObjectTypes; ++Index)
        {
            FLocalTypeWitness Local;
            auto* Type = CastToObjectType(Data.objVariableTypes[Index]);
            const auto* Value = State.Values.Find(Type);
            Local.Type = Type;
            Local.Position = Data.objVariablePos[Index];
            Local.Generation = Value ? Value->Generation : 0;
            if (Type)
            {
                Local.TypeId = Type->typeId;
                Local.Size = Type->size;
                Local.Alignment = Type->alignment;
                Local.Flags = Type->flags;
                Local.UserData = Type->plainUserData;
                Local.Behaviour = Type->beh;
            }
            Witness.LocalTypes.Add(MoveTemp(Local));
        }
    // Capture resource generations at the compiler's post-AddReferences boundary,
    // not at admission: pointer reuse or registration replacement cannot turn
    // previously compiled operands into a newly trusted call/literal.
    for (uint32 PC = 0; Witness.bWithinLimit && PC < static_cast<uint32>(Witness.Code.Num());)
    {
        const auto* Instruction = Witness.Code.GetData() + PC;
        const uint8 Opcode = static_cast<uint8>(*Instruction);
        if (Opcode >= asBC_MAXBYTECODE || asBCInfo[Opcode].bc != Opcode)
        {
            Witness.bWithinLimit = false;
            break;
        }
        const int Format = asBCInfo[Opcode].type;
        if (Format <= asBCTYPE_INFO || Format >= UE_ARRAY_COUNT(asBCTypeSize) ||
            asBCTypeSize[Format] <= 0 ||
            static_cast<uint32>(asBCTypeSize[Format]) > static_cast<uint32>(Witness.Code.Num()) - PC)
        {
            Witness.bWithinLimit = false;
            break;
        }
        if (Opcode == asBC_CALLSYS)
        {
            const auto* Callee = reinterpret_cast<const asCScriptFunction*>(asBC_PTRARG(Instruction));
            const auto* Native = State.Natives.Find(Callee);
            Witness.Resources.Add({Callee, Native ? Native->Generation : 0, true});
        }
        else if (Opcode == asBC_PGA)
        {
            const void* Literal = reinterpret_cast<const void*>(asBC_PTRARG(Instruction));
            const auto* Generation = State.Literals.Find(Literal);
            Witness.Resources.Add({Literal, Generation ? *Generation : 0, false});
        }
        else if (Opcode == asBC_LoadThisR)
        {
            const auto* Type = Witness.ObjectType;
            const int32 Offset = asBC_SWORDARG0(Instruction);
            if (!Type || Type->properties.GetLength() > MaxInstructions || Type->localProperties.GetLength() > MaxInstructions
                || asBC_DWORDARG(Instruction) != static_cast<uint32>(Type->typeId)
                || (Witness.bInt32MemberWitness && Offset != Witness.Int32Offset))
            {
                Witness.bInt32MemberWitness = true;
                Witness.Int32Member = nullptr;
                break;
            }
            if (!Witness.bInt32MemberWitness)
            {
                Witness.bInt32MemberWitness = true;
                Witness.Int32Offset = Offset;
                Witness.Int32ReceiverSize = Type->size;
                Witness.Int32ReceiverTypeId = Type->typeId;
                Witness.Int32ReceiverFlags = Type->flags;
                Witness.Int32PropertyCount = Type->properties.GetLength();
                Witness.Int32LocalPropertyCount = Type->localProperties.GetLength();
                for (asUINT Index = 0; Index < Type->localProperties.GetLength(); ++Index)
                {
                    const auto* Member = Type->localProperties[Index];
                    if (!Member || Member->byteOffset != Offset) continue;
                    if (Witness.Int32Member || Member->type != asCDataType::CreatePrimitive(ttInt, false)
                        || Member->compositeOffset || Member->isCompositeIndirect || Member->bUeDelayedDestroyOwner
                        || Member->isAppBindProperty)
                    {
                        Witness.Int32Member = nullptr;
                        break;
                    }
                    Witness.Int32Member = Member;
                    Witness.Int32MemberIndex = Index;
                    Witness.Int32DataType = Member->type;
                    Witness.Int32MemberName = Member->name;
                }
            }
        }
        PC += asBCTypeSize[Format];
    }
    State.DefaultDestructors.Remove(Function);
    State.Sources.Add(Function, MoveTemp(Witness));
}

void RegisterDefaultDestructorSource(asCScriptFunction* Function)
{
    // This API is called only from the exact CompileDefaultDestructor completion hook.
    // Generated traits and a matching bytecode sequence cannot publish compile provenance.
    if (!Function || !Function->objectType || !Function->objectType->derivedFrom || !Function->engine) return;
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto* Child = State.Sources.Find(Function);
    const int Id = Function->objectType->derivedFrom->beh.destruct;
    if (!Child || !Child->Generation || Id <= 0 || asUINT(Id) >= Function->engine->scriptFunctions.GetLength()) return;
    const auto* ParentFunction = Function->engine->scriptFunctions[Id];
    const auto* Parent = State.Sources.Find(ParentFunction);
    if (!Parent || !Parent->Generation || !Child->Matches(*Function) || !Parent->Matches(*ParentFunction)
        || ParentFunction->objectType != Function->objectType->derivedFrom
        || Parent->Module != Child->Module || Parent->Engine != Child->Engine
        || !IsExactDefaultParentBody(*Child, Id)) return;
    State.DefaultDestructors.Add(Function, FDefaultDestructorSource{Child->Generation, Parent->Generation, ParentFunction,
        Function->objectType, Function->objectType->derivedFrom});
}


void RevokeFunction(asCScriptFunction* Function)
{
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    State.DefaultDestructors.Remove(Function);
    State.Sources.Remove(Function);
    State.Natives.Remove(Function);
    for (auto It = State.Values.CreateIterator(); It; ++It)
        if (It.Value().Constructor == Function) It.RemoveCurrent();
}

void ObserveRequestedContext(asIScriptContext* Context)
{
    if (!Context)
        return;
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    if (const auto* Existing = State.Contexts.Find(Context))
        if (Existing->Owner)
            FailStopTerminalEntry(*Existing->Owner);
    FContextWitness Witness;
    Witness.Generation = State.NewGeneration();
    Witness.Callbacks = ContextCallbacks(*static_cast<asCContext*>(Context));
    State.Contexts.Add(Context, Witness);
}

void ObserveReturnedContext(asIScriptContext* Context)
{
    bool bReleaseReference = false;
    {
        auto& State = Registry();
        FScopeLock Lock(&State.Mutex);
        const auto* Witness = State.Contexts.Find(Context);
        auto* Proof = ExecutingProof;
        if (Proof && Proof->Context == Context)
        {
            if (!Witness || Witness->Owner != Proof || Witness->Generation != Proof->ContextGeneration ||
                !Proof->ContextGeneration || !Proof->bOwnContextReference ||
                Proof->Lease != FCompiledTerminalDestructorProof::ELease::Executing ||
                !HasOriginalContextRoute(*Proof))
                FailStopTerminalEntry(*Proof);
            const auto* Actual = static_cast<asCContext*>(Context);
            if (Proof->bViolated || !Proof->bExecutionStarted || Actual->m_status != asEXECUTION_FINISHED ||
                Actual->m_currentFunction != Proof->DispatchFunction || Actual->m_callStack.GetLength() ||
                (Proof->bParentFrame && Proof->ParentFramePhase != 4) ||
                !ValidateCodeDependencies(*Proof, State) || !ValidateCallbacks(*Proof))
                FailStopTerminalEntry(*Proof);
            ExecutingProof = nullptr;
            Proof->Context = nullptr;
            Proof->ContextGeneration = 0;
            Proof->Lease = FCompiledTerminalDestructorProof::ELease::Returned;
            Proof->bCompleted = true;
            Proof->bOwnContextReference = false;
            bReleaseReference = true;
        }
        else if (Witness && Witness->Owner)
        {
            // A retained lease may only be returned by its executing owner, or
            // after the reserved destructor has already revoked that ownership.
            FailStopTerminalEntry(*Witness->Owner);
        }
        State.Contexts.Remove(Context);
    }
    if (bReleaseReference)
        Context->Release();
}

uint64 ReadRetainedSourceGeneration(asITypeInfo* Type)
{
    // The lifecycle caller first proves registry identity and owns the existing module lease.
    if (!Type) return 0;
    auto* ObjectType = CastToObjectType(static_cast<asCTypeInfo*>(Type));
    auto* Engine = ObjectType ? ObjectType->engine : nullptr;
    const int Id = ObjectType ? ObjectType->beh.destruct : 0;
    if (!Engine || Id <= 0 || static_cast<asUINT>(Id) >= Engine->scriptFunctions.GetLength()) return 0;
    const auto* Function = Engine->scriptFunctions[Id];
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto* Source = State.Sources.Find(Function);
    // Check the registry before dereferencing a function address; never refresh a witness here.
    if (!Source || !Source->Generation || Source->ObjectType != ObjectType
        || !ValidateSourceResources(*Source, State) || !Source->Matches(*Function)
        || Source->Module != Type->GetModule() || Source->Engine != Engine) return 0;
    return Source->Generation;
}

bool PrepareCompiledTerminalDestructor(const FReceiverShape& Shape,
    TSharedPtr<FCompiledTerminalDestructorProof>& OutProof, FAdmissionDiagnostic& Diagnostic)
{
    OutProof.Reset();
    Diagnostic = FAdmissionDiagnostic();
    if (!Shape.Type || !Shape.Receiver || !Shape.FieldStorage || Shape.FieldOffset <= 0 ||
        !Shape.OwnerCookie || Shape.OwnerCookie != Shape.Type || !Shape.PlanId ||
        !ValidateRuntimeTerminalSourceAssociation(Shape.Receiver, Shape.OwnerCookie, Shape.PlanId, Shape.RemovedSourceGeneration) ||
        reinterpret_cast<UPTRINT>(Shape.FieldStorage) < reinterpret_cast<UPTRINT>(Shape.Receiver) ||
        reinterpret_cast<UPTRINT>(Shape.FieldStorage) - reinterpret_cast<UPTRINT>(Shape.Receiver) !=
            static_cast<UPTRINT>(Shape.FieldOffset))
        return Reject(Diagnostic, EAdmissionFailure::InvalidReceiverShape);
    const bool bParentFrame = Shape.Type->derivedFrom != nullptr;
    if (bParentFrame && (!Shape.bAllowDefaultParentFrame || !Shape.FieldOwnerType
        || Shape.Type->derivedFrom != Shape.FieldOwnerType || Shape.FieldOwnerType->derivedFrom
        || Shape.Type->module != Shape.FieldOwnerType->module || Shape.Type->localProperties.GetLength()
        || Shape.Type->size != Shape.FieldOwnerType->size || Shape.RemovedSourceGeneration))
        return Reject(Diagnostic, EAdmissionFailure::UnsupportedHierarchy);
    if (!bParentFrame && (Shape.FieldOwnerType && Shape.FieldOwnerType != Shape.Type))
        return Reject(Diagnostic, EAdmissionFailure::InvalidReceiverShape);
    auto* Engine = Shape.Type->engine;
    const int Id = Shape.Type->beh.destruct;
    if (!Engine || Id <= 0 || static_cast<asUINT>(Id) >= Engine->scriptFunctions.GetLength())
        return Reject(Diagnostic, EAdmissionFailure::UnsupportedFunction);
    auto* DispatchFunction = Engine->scriptFunctions[Id];
    auto* BodyType = bParentFrame ? Shape.FieldOwnerType : Shape.Type;
    const int BodyId = BodyType->beh.destruct;
    if (BodyId <= 0 || static_cast<asUINT>(BodyId) >= Engine->scriptFunctions.GetLength())
        return Reject(Diagnostic, EAdmissionFailure::UnsupportedFunction);
    auto* Function = Engine->scriptFunctions[BodyId];
    auto Candidate = MakeShared<FCompiledTerminalDestructorProof>();
    Candidate->Shape = Shape;
    Candidate->Function = Function;
    Candidate->DispatchFunction = DispatchFunction;
    Candidate->BodyType = BodyType;
    Candidate->bParentFrame = bParentFrame;
    Candidate->Engine = Engine;
    Candidate->FreshGeneration = Shape.PlanId;
    {
        auto& State = Registry();
        FScopeLock Lock(&State.Mutex);
        const auto* Source = State.Sources.Find(Function);
        if (!Source || !Source->Generation ||
            (Shape.RemovedSourceGeneration && Shape.RemovedSourceGeneration != Source->Generation))
            return Reject(Diagnostic, EAdmissionFailure::MissingSourceWitness);
        Candidate->SourceGeneration = Source->Generation;
        const auto* DispatchSource = State.Sources.Find(DispatchFunction);
        Candidate->DispatchSourceGeneration = DispatchSource ? DispatchSource->Generation : 0;
        if (!ValidateDefaultParentSource(*Candidate, State))
            return Reject(Diagnostic, EAdmissionFailure::MissingSourceWitness);
        Candidate->Observation.DispatchFunction = Candidate->DispatchFunction;
        Candidate->Observation.ParentFunction = Candidate->Function;
        Candidate->Observation.ChildSourceGeneration = Candidate->DispatchSourceGeneration;
        Candidate->Observation.ParentSourceGeneration = Candidate->SourceGeneration;
        Candidate->Observation.Frames = bParentFrame ? 2u : 1u;
        Candidate->Observation.bDefaultSource = bParentFrame;
        Diagnostic.SourceGeneration = Source->Generation;
        Diagnostic.CodeWords = Source->Code.Num();
        if (!Source->bWithinLimit)
            return Reject(Diagnostic, EAdmissionFailure::BytecodeLimit);
        if (!ValidateSourceResources(*Source, State) || !Source->Matches(*Function))
        {
            UE_LOG(LogTemp, Display, TEXT("SOLOAS_TERMINAL_SOURCE_WITNESS_REJECT resources=%d source=%d local_types=%d"),
                ValidateSourceResources(*Source, State), Source->Matches(*Function), Source->LocalTypes.Num());
            return Reject(Diagnostic, EAdmissionFailure::RetiredSource);
        }
        CaptureCleanupDiagnostic(*Function, State, Diagnostic);
        if (!Decode(*Source, State, Diagnostic))
            return false;
        if (Function->objectType != BodyType || Function->parameterTypes.GetLength() ||
            Function->returnType != asCDataType::CreatePrimitive(ttVoid, false) ||
            Function->hiddenArgumentIndex != -1 || Function->vfTableIdx != -1 ||
            Function->compileOutType != asECompileOutType::CompileCalls)
            return Reject(Diagnostic, EAdmissionFailure::UnsupportedFunction);
        if (!ValidateLocalMetadata(*Source, State))
            return Reject(Diagnostic, EAdmissionFailure::ObjectCleanup);
        for (const auto& Item : Diagnostic.Instructions)
        {
            const auto* Instruction = Source->Code.GetData() + Item.WordOffset;
            if (Item.Opcode == asBC_CALLSYS)
            {
                if (!Item.NativeRole)
                    return Reject(Diagnostic, EAdmissionFailure::UnwitnessedNative, Item.WordOffset);
                const auto* Native = reinterpret_cast<const asCScriptFunction*>(asBC_PTRARG(Instruction));
                Candidate->Natives.Add({Native, State.Natives.FindChecked(Native).Generation});
            }
            if (Item.Opcode == asBC_PGA)
            {
                if (!Item.bLiveLiteral)
                    return Reject(Diagnostic, EAdmissionFailure::UnwitnessedLiteral, Item.WordOffset);
                const auto* Literal = reinterpret_cast<const void*>(asBC_PTRARG(Instruction));
                Candidate->Literals.Add({Literal, State.Literals.FindChecked(Literal)});
            }
        }
        if (!QualifyTransfer(*Candidate, *Source, State, Diagnostic))
            return false;
    }
    // Exercise the same normal context acquisition/preparation before any destructive
    // claim. No script is executed, callbacks are neither cleared nor replaced.
    if (!ReserveContext(*Candidate, Diagnostic))
        return false;
    // Prepare can perform cleanup callouts. ReserveContext rechecks every source,
    // resource and context generation after those callouts; the published proof is
    // also checked through the same silent interface used before Deferred.
    Candidate->bQualified = true;
    if (!ValidatePreparedCompiledTerminalDestructor(*Candidate, Shape))
    {
        Candidate->bQualified = false;
        return Reject(Diagnostic, EAdmissionFailure::RetiredSource);
    }
    OutProof = Candidate;
    return true;
}

bool RefreshCompiledTerminalDestructorIdleContext(FCompiledTerminalDestructorProof& Proof)
{
    if (!Proof.bQualified || Proof.bViolated || !Proof.bOwnContextReference ||
        Proof.Lease != FCompiledTerminalDestructorProof::ELease::Reserved ||
        !Proof.Context || !Proof.ContextGeneration || !Proof.Function || !Proof.Engine ||
        !Proof.SourceGeneration || PendingProof || SelectingProof || ExecutingProof)
        return false;
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto* Witness = State.Contexts.Find(Proof.Context);
    if (!Witness || Witness->Generation != Proof.ContextGeneration || Witness->Owner != &Proof ||
        !ValidateDependencyGenerations(Proof, State))
        return false;
    // PrimaryContext is separately retained, but never accept a replacement TLD or
    // primary. A normal AS frame may finish this same primary between GC slices.
    if (!IsInGameThread() || !Proof.ThreadData ||
        Proof.ThreadData != FAngelscriptManager::GameThreadTLD || Proof.ThreadData->activeContext ||
        Proof.ThreadData->activeExecution || Proof.ThreadData->primaryContext != Proof.PrimaryContext ||
        Proof.Context->m_engine != Proof.Engine)
        return false;
    const int32 PrimaryState = Proof.PrimaryContext ? Proof.PrimaryContext->m_status : Proof.PrimaryState;
    if (!ValidateIdleContextSelection(Proof, PrimaryState) || !HasOriginalContextRoute(Proof) ||
        !ValidateCodeDependencies(Proof, State) || !ValidateReservedContext(Proof, PrimaryState))
        return false;
    // Publish only after every original witness still matches. Read-only validators
    // keep requiring this exact state after subsequent observable callouts.
    Proof.PrimaryState = PrimaryState;
    return true;
}

bool ValidatePreparedCompiledTerminalDestructor(const FCompiledTerminalDestructorProof& Proof,
    const FReceiverShape& Shape)
{
    if (!Proof.bQualified || Proof.bViolated || !SameShape(Proof.Shape, Shape) ||
        Proof.Lease != FCompiledTerminalDestructorProof::ELease::Reserved ||
        !Proof.Function || !Proof.Engine || !Proof.SourceGeneration ||
        !Proof.Context || !Proof.ContextGeneration || !Proof.bOwnContextReference)
        return false;
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto* Context = State.Contexts.Find(Proof.Context);
    return Context && Context->Generation == Proof.ContextGeneration && Context->Owner == &Proof &&
        ValidateCodeDependencies(Proof, State) && ValidateReservedContext(Proof);
}

bool ValidateCompiledTerminalDestructor(const FCompiledTerminalDestructorProof& Proof,
    const FFreshFieldCertificate& Certificate)
{
    const bool bSlotCertificate = IsTerminalFreshInt32Certificate(Proof.Shape.Int32Slot,
        Certificate.Int32Epoch, Certificate.Int32Phase);
    return bSlotCertificate && Certificate.bFreshLive && Certificate.Generation &&
        Proof.FreshGeneration == Certificate.Generation &&
        ValidatePreparedCompiledTerminalDestructor(Proof, Certificate.Shape);
}

bool BeginCompiledTerminalDestructor(FCompiledTerminalDestructorProof& Proof,
    const FFreshFieldCertificate& Certificate)
{
    if (PendingProof || SelectingProof || ExecutingProof ||
        !ValidateCompiledTerminalDestructor(Proof, Certificate))
        return false;
    Proof.bFreshSlotBound = Proof.Shape.Int32Slot.Property != nullptr;
    if (!ValidatePreparedCompiledTerminalDestructor(Proof, Certificate.Shape))
        return false;
    PendingProof = &Proof;
    return true;
}

bool DidCompleteCompiledTerminalDestructor(const FCompiledTerminalDestructorProof& Proof)
{
    if (!Proof.bQualified || !Proof.bCompleted || Proof.Lease != FCompiledTerminalDestructorProof::ELease::Returned || Proof.Context)
        return false;
    if (!ClosedLocalLifetimes(Proof)) return false;
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    return ValidateCodeDependencies(Proof, State);
}

bool ReadCompletedTerminalDestructorObservation(const FCompiledTerminalDestructorProof& Proof,
    FCompiledTerminalDestructorObservation& OutObservation)
{
    OutObservation = FCompiledTerminalDestructorObservation();
    if (!Proof.bQualified || !Proof.bCompleted || Proof.bViolated || Proof.Context
        || Proof.Lease != FCompiledTerminalDestructorProof::ELease::Returned)
        return false;
    OutObservation = Proof.Observation;
    return true;
}

asIScriptContext* TakeReservedContext(asIScriptEngine* Engine)
{
    auto* Proof = SelectingProof;
    if (!Proof || Proof->Engine != Engine ||
        Proof->Lease != FCompiledTerminalDestructorProof::ELease::Selecting || !Proof->Context)
        return nullptr;
    SelectingProof = nullptr;
    Proof->Lease = FCompiledTerminalDestructorProof::ELease::Executing;
    ExecutingProof = Proof;
    return Proof->Context;
}

EContextSelection SelectCompiledTerminalDestructorContext(void* Receiver, asCObjectType* Type,
    asIScriptContext*& OutContext)
{
    auto* Proof = PendingProof;
    if (!Proof || Proof->Shape.Receiver != Receiver || Proof->Shape.Type != Type)
        return EContextSelection::NotApplicable;
    PendingProof = nullptr;
    if (!ValidatePreparedCompiledTerminalDestructor(*Proof, Proof->Shape))
        return EContextSelection::Rejected;
    Proof->Lease = FCompiledTerminalDestructorProof::ELease::Selecting;
    SelectingProof = Proof;
    auto* Context = Proof->Engine->RequestContext();
    SelectingProof = nullptr;
    if (!Context || Context != Proof->Context || ExecutingProof != Proof)
    {
        if (Proof->Lease == FCompiledTerminalDestructorProof::ELease::Selecting)
            Proof->Lease = FCompiledTerminalDestructorProof::ELease::Reserved;
        return EContextSelection::Rejected;
    }
    OutContext = Context;
    return EContextSelection::Selected;
}

bool ValidateContextExecution(asIScriptContext* Context, EContextFence Fence,
    asIScriptFunction* PreparingFunction)
{
    auto* Proof = ExecutingProof;
    if (!Proof)
        return true;
    // Preparing a different context does not touch the retained terminal frame.
    // Its eventual Execute/JIT entry remains guarded independently.
    if (Context != Proof->Context &&
        (Fence == EContextFence::Prepare || Fence == EContextFence::PushState))
        return true;
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto* Witness = State.Contexts.Find(Proof->Context);
    const bool bLiveContext = Witness && Witness->Generation == Proof->ContextGeneration && Witness->Owner == Proof;
    auto Fail = [&]() -> bool
    {
        FailStopTerminalEntry(*Proof);
    };
    if (!bLiveContext || !Proof->bQualified || Proof->bViolated || Proof->Context != Context ||
        Proof->Lease != FCompiledTerminalDestructorProof::ELease::Executing ||
        !ValidateCodeDependencies(*Proof, State) || !ValidateCallbacks(*Proof))
        return Fail();
    auto* Actual = Proof->Context;
    const bool bInParent = Proof->bParentFrame && Proof->ParentFramePhase == 2;
    if (Actual->m_engine != Proof->Engine || Actual->m_initialFunction != Proof->DispatchFunction ||
        Actual->m_currentFunction != (bInParent ? Proof->Function : Proof->DispatchFunction) ||
        Actual->m_callStack.GetLength() != (bInParent ? TerminalParentCallWords : 0u) ||
        (Proof->bParentFrame && (!ValidateFrozenParentStack(*Proof)
            || (bInParent && !ValidateSavedChildFrame(*Proof)))) ||
        Actual->m_doAbort || Actual->m_externalSuspendRequest || Actual->m_needToCleanupArgs ||
        Actual->m_loopDetectionCounter < 0 || Actual->m_loopDetectionCounter > MaxInstructions)
        return Fail();
    switch (Fence)
    {
    case EContextFence::Prepare:
        if (Proof->bPreparedForExecution || Proof->bExecutionStarted || PreparingFunction != Proof->DispatchFunction ||
            Actual->m_status != asEXECUTION_PREPARED)
            return Fail();
        Proof->bPreparedForExecution = true;
        return true;
    case EContextFence::ExecuteEntry:
        if (!Proof->bPreparedForExecution || !Proof->bReceiverAssigned || Proof->bExecutionStarted ||
            Actual->m_status != asEXECUTION_PREPARED || !Actual->m_regs.stackFramePointer ||
            *reinterpret_cast<void**>(Actual->m_regs.stackFramePointer) != Proof->Shape.Receiver
            || !ValidateRuntimeTerminalInt32InitialValue(Proof->Shape))
            return Fail();
        Proof->bExecutionStarted = true;
        if (Proof->bParentFrame && Actual->m_regs.stackFramePointer != Proof->ChildFrame) return Fail();
        Proof->Frame = Actual->m_regs.stackFramePointer;
        return true;
    case EContextFence::NativeEntry:
        if (Proof->bParentFrame && !bInParent) return Fail();
        return (Proof->bExecutionStarted && Actual->m_status == asEXECUTION_ACTIVE && NativeEntry(*Proof, State)) || Fail();
    case EContextFence::NativeReturn:
        if (Proof->bParentFrame && !bInParent) return Fail();
        return (Proof->bExecutionStarted && Actual->m_status == asEXECUTION_ACTIVE && NativeReturn(*Proof)) || Fail();
    case EContextFence::Suspend:
    {
        if (!Proof->bExecutionStarted || Actual->m_status != asEXECUTION_ACTIVE || Proof->ActiveCall || !ValidateTerminalFrame(*Proof))
            return Fail();
        const auto* Begin = Proof->Function->scriptData->byteCode.AddressOf();
        const UPTRINT Address = reinterpret_cast<UPTRINT>(Actual->m_regs.programPointer);
        const UPTRINT Base = reinterpret_cast<UPTRINT>(Begin);
        if (Address < Base || (Address - Base) % sizeof(asDWORD)
            || (Address - Base) / sizeof(asDWORD) >= Proof->Function->scriptData->byteCode.GetLength()
            || !AdvanceLocalScopes(*Proof, (Address - Base) / sizeof(asDWORD),
                Proof->Function->scriptData->byteCode.GetLength()) || !ValidateResource(*Proof)) return Fail();
        return true;
    }
    case EContextFence::ExecuteCompletion:
        if (Proof->bParentFrame && Proof->ParentFramePhase != 4) return Fail();
        return (Proof->bExecutionStarted && Actual->m_status == asEXECUTION_FINISHED
            && ClosedLocalLifetimes(*Proof) && ValidateResource(*Proof)) || Fail();
    case EContextFence::Cleanup:
        // CallDestructor prepares the already-reserved context again. Prepare's
        // normal CleanStackFrame cannot dispatch a local or argument cleanup
        // before any execution: its program pointer is still null. Admit only
        // that fully retained, unstarted frame, not exceptional/native reentry.
        if (IsUnstartedTerminalCleanup(*Proof, Actual->m_status, Actual->m_regs.programPointer)
            && ValidateReservedContext(*Proof)) return true;
        // The VM must never dispatch unproved local/argument destructors, including
        // reentrant cleanup entered from a native leaf or an exceptional return.
        return (Proof->bExecutionStarted && Actual->m_status == asEXECUTION_FINISHED
            && ClosedLocalLifetimes(*Proof) && ValidateResource(*Proof)) || Fail();
    case EContextFence::PushState:
    case EContextFence::Exception:
        // Nested execution and target exceptions are not part of this normal-only
        // contract. Abort before callbacks; do not reinterpret them as normal RET.
        return Fail();
    }
    return Fail();
}

void ValidateTerminalParentFrame(asIScriptContext* Context, EParentFrameFence Fence,
    asDWORD* Program, asDWORD* Stack, asDWORD* Frame, int CalledFunctionId)
{
    auto* Proof = ExecutingProof;
    if (!Proof) return;
    if (!Proof->bParentFrame)
    {
        // Ordinary script CALL stays forbidden in the existing single-frame transfer contract.
        if (Fence == EParentFrameFence::ChildCallBefore) FailStopTerminalEntry(*Proof);
        return;
    }
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto* Witness = State.Contexts.Find(Context);
    auto* C = Proof->Context;
    if (C != Context || !Witness || Witness->Owner != Proof || Witness->Generation != Proof->ContextGeneration
        || !Proof->bQualified || Proof->bViolated || !Proof->bExecutionStarted
        || Proof->Lease != FCompiledTerminalDestructorProof::ELease::Executing
        || C->m_status != asEXECUTION_ACTIVE || C->m_initialFunction != Proof->DispatchFunction
        || !ValidateFrozenParentStack(*Proof) || !ValidateCodeDependencies(*Proof, State)
        || !ValidateCallbacks(*Proof) || Proof->ActiveCall || C->m_doAbort || C->m_externalSuspendRequest
        || C->m_needToCleanupArgs) FailStopTerminalEntry(*Proof);
    const auto* ChildCode = Proof->DispatchFunction->scriptData->byteCode.AddressOf();
    switch (Fence)
    {
    case EParentFrameFence::ChildCallBefore:
        if (Proof->ParentFramePhase != 0 || CalledFunctionId != Proof->Function->id
            || C->m_currentFunction != Proof->DispatchFunction || C->m_callStack.GetLength()
            || Program != ChildCode + 1 || Frame != Proof->ChildFrame || Stack != Proof->ChildCallStack
            || *reinterpret_cast<void* const*>(Frame) != Proof->Shape.Receiver
            || *reinterpret_cast<void* const*>(Stack) != Proof->Shape.Receiver
            || !ClosedLocalLifetimes(*Proof)) FailStopTerminalEntry(*Proof);
        C->m_regs.programPointer = Program;
        C->m_regs.stackPointer = Stack;
        C->m_regs.stackFramePointer = Frame;
        if (!ValidateResource(*Proof)) FailStopTerminalEntry(*Proof);
        Proof->ParentFramePhase = 1;
        Proof->Observation.PhaseMask |= 1u;
        ++Proof->Observation.ChildCallCount;
        return;
    case EParentFrameFence::ParentEntered:
        if (Proof->ParentFramePhase != 1 || CalledFunctionId != Proof->Function->id
            || C->m_currentFunction != Proof->Function || !ValidateSavedChildFrame(*Proof)
            || Program != Proof->Function->scriptData->byteCode.AddressOf()
            || Frame != Proof->ParentFrame
            || Stack != Frame - Proof->Function->scriptData->variableSpace
            || *reinterpret_cast<void* const*>(Frame) != Proof->Shape.Receiver) FailStopTerminalEntry(*Proof);
        Proof->Frame = Frame;
        C->m_regs.programPointer = Program;
        C->m_regs.stackPointer = Stack;
        C->m_regs.stackFramePointer = Frame;
        if (!ValidateResource(*Proof)) FailStopTerminalEntry(*Proof);
        Proof->ParentFramePhase = 2;
        Proof->Observation.PhaseMask |= 2u;
        ++Proof->Observation.ParentEnterCount;
        return;
    case EParentFrameFence::ParentRetBefore:
        if (Proof->ParentFramePhase == 4)
        {
            // The only outer RET belongs to the previously restored exact child frame.
            if (C->m_currentFunction != Proof->DispatchFunction || C->m_callStack.GetLength()
                || Program != ChildCode + 3 || Frame != Proof->ChildFrame || Stack != Proof->ChildFrame
                || !ClosedLocalLifetimes(*Proof)) FailStopTerminalEntry(*Proof);
            C->m_regs.programPointer = Program;
            C->m_regs.stackPointer = Stack;
            C->m_regs.stackFramePointer = Frame;
            if (!ValidateResource(*Proof)) FailStopTerminalEntry(*Proof);
            return;
        }
        if (Proof->ParentFramePhase != 2 || C->m_currentFunction != Proof->Function
            || !ValidateSavedChildFrame(*Proof) || Frame != Proof->ParentFrame
            || Stack != Frame - Proof->Function->scriptData->variableSpace) FailStopTerminalEntry(*Proof);
        {
            const auto* Body = Proof->Function->scriptData->byteCode.AddressOf();
            const UPTRINT Address = reinterpret_cast<UPTRINT>(Program), Begin = reinterpret_cast<UPTRINT>(Body);
            const asUINT Length = Proof->Function->scriptData->byteCode.GetLength();
            if (Address < Begin || (Address - Begin) % sizeof(asDWORD)
                || (Address - Begin) / sizeof(asDWORD) >= Length
                || *Program != (asDWORD(asBC_RET) | (asDWORD(AS_PTR_SIZE) << 16))) FailStopTerminalEntry(*Proof);
            C->m_regs.programPointer = Program;
            C->m_regs.stackPointer = Stack;
            C->m_regs.stackFramePointer = Frame;
            if (!AdvanceLocalScopes(*Proof, (Address - Begin) / sizeof(asDWORD), Length)
                || !ClosedLocalLifetimes(*Proof) || !ValidateResource(*Proof)) FailStopTerminalEntry(*Proof);
        }
        Proof->ParentFramePhase = 3;
        Proof->Observation.PhaseMask |= 4u;
        ++Proof->Observation.ParentReturnCount;
        return;
    case EParentFrameFence::ChildRestored:
        if (Proof->ParentFramePhase != 3 || C->m_currentFunction != Proof->DispatchFunction
            || C->m_callStack.GetLength() || Program != ChildCode + 3
            || Frame != Proof->ChildFrame || Stack != Proof->ChildFrame
            || *reinterpret_cast<void* const*>(Frame) != Proof->Shape.Receiver
            || !ClosedLocalLifetimes(*Proof)) FailStopTerminalEntry(*Proof);
        Proof->Frame = Frame;
        C->m_regs.programPointer = Program;
        C->m_regs.stackPointer = Stack;
        C->m_regs.stackFramePointer = Frame;
        if (!ValidateResource(*Proof)) FailStopTerminalEntry(*Proof);
        Proof->ParentFramePhase = 4;
        Proof->Observation.PhaseMask |= 8u;
        ++Proof->Observation.ChildResumeCount;
        return;
    }
    FailStopTerminalEntry(*Proof);
}


void RequireNonTerminalScriptExecution()
{
    if (ExecutingProof)
        FailStopTerminalEntry(*ExecutingProof);
}

void PublishTerminalVmRegisters(asIScriptContext* Context, asDWORD* Program, asDWORD* Stack, asDWORD* Frame)
{
    auto* Proof = ExecutingProof;
    if (!Proof) return;
    if (Context != Proof->Context || Frame != Proof->Frame || !Proof->bExecutionStarted || Proof->ActiveCall)
        FailStopTerminalEntry(*Proof);
    Proof->Context->m_regs.programPointer = Program;
    Proof->Context->m_regs.stackPointer = Stack;
    Proof->Context->m_regs.stackFramePointer = Frame;
}

void RequireTerminalFrameAccessAllowed(asIScriptContext* Context)
{
    auto* Proof = ExecutingProof;
    if (Proof && Proof->Context == Context)
        FailStopTerminalEntry(*Proof);
}

void RequireTerminalReceiverAssignmentAllowed(asIScriptContext* Context, void* Receiver)
{
    auto* Proof = ExecutingProof;
    if (!Proof || Proof->Context != Context)
        return;
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto* Witness = State.Contexts.Find(Context);
    if (!Witness || Witness->Generation != Proof->ContextGeneration || Witness->Owner != Proof ||
        !Proof->bQualified || Proof->bViolated || !Proof->bPreparedForExecution ||
        Proof->bReceiverAssigned || Proof->bExecutionStarted || Receiver != Proof->Shape.Receiver ||
        !ValidateCodeDependencies(*Proof, State) || !ValidateCallbacks(*Proof))
        FailStopTerminalEntry(*Proof);
    // ValidateReservedContext checks the pre-execution frame without indexing
    // parameter metadata. The complete CallDestructor sets its receiver once.
    if (!ValidateReservedContext(*Proof))
        FailStopTerminalEntry(*Proof);
    Proof->bReceiverAssigned = true;
}

bool ReadActiveResourceView(FAngelscriptTerminalResourceView& Out)
{
    Out = {};
    auto* Proof = ExecutingProof;
    if (!Proof || !IsInGameThread() || !Proof->Context || !Proof->ActiveCall
        || Proof->ActiveLeaf != ENativeLeaf::LogDisplay) return false;
    auto& State = Registry();
    FScopeLock Lock(&State.Mutex);
    const auto* Witness = State.Contexts.Find(Proof->Context);
    if (!Witness || Witness->Owner != Proof || Witness->Generation != Proof->ContextGeneration
        || Proof->Lease != FCompiledTerminalDestructorProof::ELease::Executing || !Proof->bQualified || Proof->bViolated
        || Proof->Context->m_status != asEXECUTION_ACTIVE || !ValidateCodeDependencies(*Proof, State)
        || !ValidateCallbacks(*Proof) || !ValidateResource(*Proof)) FailStopTerminalEntry(*Proof);
    return PopulateResourceView(*Proof, Out);
}

}

bool ReadActiveAngelscriptTerminalResourceView(FAngelscriptTerminalResourceView& Out)
{
    return Cuviss::TerminalAdmission::ReadActiveResourceView(Out);
}
