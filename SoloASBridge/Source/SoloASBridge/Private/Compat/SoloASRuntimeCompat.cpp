// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#include "Compat/SoloASRuntimeCompat.h"
#include "Compat/SoloASStructOpsCompat.h"
#include "HAL/PlatformStackWalk.h"
#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "InputCoreTypes.h"
#include "Interfaces/IProjectManager.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "UObject/Package.h"
#include "UObject/UObjectArray.h"
#include "UObject/OverridableManager.h"
#include "UObject/OverriddenPropertySet.h"
#include "Misc/CoreDelegates.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"
#include "UObject/MetaData.h"
#include "Shared/SoloASPrivateAccess.h"
#include "UObject/UObjectThreadContext.h"
#include "UObject/LinkerLoad.h"
#include "Shared/SoloASScriptBlueprint.h"
#include "Misc/ScopeExit.h"
#include "SoloASRuntimeCompat.inl"

// A: preserve successful cache-producer exit on stock engine startup.
void SoloASBridge::Compat::RequestScriptGenerationExit()
{
    if (IsRunningCommandlet())
    {
        FPlatformMisc::RequestExitWithStatus(false, 0);
        return;
    }

    // Stock PreInit reports an exit requested during module loading as a startup failure.
    FCoreDelegates::OnEndFrame.AddLambda([]()
    {
        FPlatformMisc::RequestExitWithStatus(false, 0);
    });
}

// === BEGIN CUVISS STUDIO EXTENSION: CS.Bridge.PluginProjectSearchPaths.PublicReference | by Cuviss Studio ===
// A: the unexported MinimalAPI engine function forwards to this same public service.
const TArray<FString>& SoloASBridge::Compat::GetAdditionalProjectPluginSearchPaths()
{
    return IProjectManager::Get().GetAdditionalPluginDirectories();
}
// === END CUVISS STUDIO EXTENSION: CS.Bridge.PluginProjectSearchPaths.PublicReference | by Cuviss Studio ===

namespace SoloASBridge::PrivateAccess
{
struct FCompatDefaultObjectTag
{
    using Type = TObjectPtr<UObject> UClass::*;
    friend Type Resolve(FCompatDefaultObjectTag) noexcept;
};
template struct TMember<FCompatDefaultObjectTag, &UClass::ClassDefaultObject>;
}
void SoloASBridge::Compat::SetClassDefaultObject(UClass* Class, UObject* Object)
{
                                                                                                                         
#if SOLOAS_UE_BEFORE(5, 6)
    SoloASBridge::PrivateAccess::Get<SoloASBridge::PrivateAccess::FCompatDefaultObjectTag>(*Class) = Object;
#else
    Class->SetDefaultObject(Object);
#endif
// SOLOAS-COMPAT-END
}

                                                                                                                          
#if SOLOAS_UE_BEFORE(5, 7)
#include <cstddef>
#include <cstring>
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#else
#include <dlfcn.h>
#endif
namespace SoloASBridge::Compat
{
namespace
{
using FVectorOps = UScriptStruct::TCppStructOps<FVector>;
template<class Method> int32 SdkVirtualSlot(Method Member)
{
#if PLATFORM_WINDOWS
    static_assert(sizeof(Member) == sizeof(void*), "The SDK ops interface uses single inheritance");
    const uint8* Code = nullptr;
    std::memcpy(&Code, &Member, sizeof(Code));
    // Decode only the compiler's local virtual-call thunk, never an engine implementation.
    if (Code[0] == 0xe9)
    {
        int32 Offset;
        std::memcpy(&Offset, Code + 1, sizeof(Offset));
        Code += 5 + Offset;
    }
    if (Code[0] != 0x48 || Code[1] != 0x8b || Code[2] != 0x01) return INDEX_NONE;
    int32 Position = Code[3] == 0x48 ? 4 : 3;
    int32 Offset = INDEX_NONE;
    if (Code[Position] == 0xff && Code[Position + 1] == 0x60) Offset = Code[Position + 2];
    else if (Code[Position] == 0xff && Code[Position + 1] == 0xa0)
        std::memcpy(&Offset, Code + Position + 2, sizeof(Offset));
    return Offset >= 0 && Offset % sizeof(void*) == 0 ? static_cast<int32>(Offset / sizeof(void*)) : INDEX_NONE;
#else
    // Itanium member pointers encode a virtual slot as byte offset + 1.
    struct FMember { std::ptrdiff_t Pointer, Adjustment; } Value;
    static_assert(sizeof(Value) == sizeof(Member));
    std::memcpy(&Value, &Member, sizeof(Value));
    return Value.Adjustment == 0 && Value.Pointer > 0 && (Value.Pointer & 1)
        && (Value.Pointer - 1) % sizeof(void*) == 0 ? static_cast<int32>((Value.Pointer - 1) / sizeof(void*)) : INDEX_NONE;
#endif
}
struct FSdkSlots
{
    int32 Construct = INDEX_NONE, Copy = INDEX_NONE;
    bool Valid = false;
};
const FSdkSlots& SdkSlots()
{
    static const FSdkSlots Slots = []
    {
        FSdkSlots Result;
        Result.Construct = SdkVirtualSlot(&FVectorOps::Construct);
        Result.Copy = SdkVirtualSlot(&FVectorOps::Copy);
#if PLATFORM_WINDOWS
        constexpr int32 DeclaredConstruct = 2;
#if SOLOAS_UE_BEFORE(5, 6)
        constexpr int32 DeclaredCopy = 12;
#else
        constexpr int32 DeclaredCopy = 14;
#endif
#else
        constexpr int32 DeclaredConstruct = 3;
#if SOLOAS_UE_BEFORE(5, 6)
        constexpr int32 DeclaredCopy = 13;
#else
        constexpr int32 DeclaredCopy = 15;
#endif
#endif
        // Independent compiler member-pointer mapping must agree with Class.h's declaration order.
        FVectorOps Local;
        const auto* Table = *reinterpret_cast<const void* const* const*>(&Local);
        Result.Valid = Result.Construct == DeclaredConstruct && Result.Copy == DeclaredCopy
            && Table[DeclaredConstruct] == Table[Result.Construct]
            && Table[DeclaredCopy] == Table[Result.Copy];
        UE_LOG(LogTemp, Display, TEXT("SOLOAS_UE56_VECTOR_GATE1 sdk_slots=%d construct=%d copy=%d local_addresses_equal=%d"),
            Result.Valid, Result.Construct, Result.Copy, Result.Valid);
        checkf(Result.Valid, TEXT("SOLOAS-COMPAT: local SDK struct ops virtual-slot mapping differs"));
        return Result;
    }();
    return Slots;
}
bool IsCoreFunctionEntry(const void* Address, bool RequireEntry)
{
    const void* Anchor = reinterpret_cast<const void*>(&TBaseStructure<FVector>::Get);
#if PLATFORM_WINDOWS
    HMODULE Core = nullptr, Actual = nullptr;
    const DWORD Flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    const bool Module = GetModuleHandleExW(Flags, reinterpret_cast<LPCWSTR>(Anchor), &Core)
        && GetModuleHandleExW(Flags, reinterpret_cast<LPCWSTR>(Address), &Actual) && Core == Actual;
    DWORD64 ImageBase = 0;
    const auto* Entry = Module ? RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(Address), &ImageBase, nullptr) : nullptr;
    const bool Exact = Entry && ImageBase == reinterpret_cast<DWORD64>(Core)
        && ImageBase + Entry->BeginAddress == reinterpret_cast<DWORD64>(Address);
    UE_LOG(LogTemp, Display, TEXT("SOLOAS_UE56_VECTOR_GATE2 address=%p core_module=%d unwind=%d exact_entry=%d required=%d"),
        Address, Module, Entry != nullptr, Exact, RequireEntry);
    return Module && (!RequireEntry || Exact);
#else
    Dl_info Owner{}, Function{};
    const bool Module = dladdr(Anchor, &Owner) && dladdr(Address, &Function)
        && Owner.dli_fbase == Function.dli_fbase;
    const bool Exact = Module && Function.dli_saddr == Address;
    UE_LOG(LogTemp, Display, TEXT("SOLOAS_UE56_VECTOR_GATE2 address=%p core_module=%d exact_entry=%d required=%d"), Address, Module, Exact, RequireEntry);
    return Module && (!RequireEntry || Exact);
#endif
}
}
FVirtualStructOpsHandlers ReadVirtualStructOpsHandlers(const UScriptStruct::ICppStructOps* Ops)
{
    if (!Ops || !SdkSlots().Valid) return {};
    const auto* Table = *reinterpret_cast<const void* const* const*>(Ops);
#if PLATFORM_WINDOWS
    return {Table, Table[SdkSlots().Construct], Table[SdkSlots().Copy], Table[0], nullptr};
#else
    return {Table, Table[SdkSlots().Construct], Table[SdkSlots().Copy], Table[0], Table[1]};
#endif
}
bool HasCanonicalVectorHandlers(const FVirtualStructOpsHandlers& Handlers)
{
    struct FQualification { FVirtualStructOpsHandlers Handlers; bool Valid = false; };
    // CoreUObject and its native FVector provider live for the process lifetime.
    // Qualify once during binding; later quiet checks only compare saved identities.
    static const FQualification Qualified = []
    {
        FQualification Result;
        Result.Handlers = ReadVirtualStructOpsHandlers(TBaseStructure<FVector>::Get()->GetCppStructOps());
        const auto& H = Result.Handlers;
        const bool Distinct = H.Construct && H.Copy && H.Construct != H.Copy
            && H.Construct != H.Destructor && H.Copy != H.Destructor
            && H.Construct != H.DeletingDestructor && H.Copy != H.DeletingDestructor;
        const UScriptStruct* Provider = TBaseStructure<FVector>::Get();
        const auto Capabilities = Provider->GetCppStructOps()->GetCapabilities();
        const bool NativeMemcpy = !(Provider->StructFlags & STRUCT_CopyNative)
            && Capabilities.IsPlainOldData && !Capabilities.HasCopy;
        UE_LOG(LogTemp, Display, TEXT("SOLOAS_UE56_VECTOR_COPY_POLICY memcpy=%d struct_flags=%u pod=%d has_copy=%d"),
            NativeMemcpy, static_cast<uint32>(Provider->StructFlags), Capabilities.IsPlainOldData, Capabilities.HasCopy);
        const bool ConstructEntry = IsCoreFunctionEntry(H.Construct, true);
        const bool CopyEntry = IsCoreFunctionEntry(H.Copy, !NativeMemcpy);
        UE_LOG(LogTemp, Display, TEXT("SOLOAS_UE56_VECTOR_GATE2 distinct_non_destructor=%d"), Distinct);
        Result.Valid = SdkSlots().Valid && Distinct && ConstructEntry && CopyEntry;
        return Result;
    }();
    const auto& Expected = Qualified.Handlers;
    return Qualified.Valid && Handlers.Table == Expected.Table
        && Handlers.Construct == Expected.Construct && Handlers.Copy == Expected.Copy
        && Handlers.Destructor == Expected.Destructor && Handlers.DeletingDestructor == Expected.DeletingDestructor;
}
}

#endif
// SOLOAS-COMPAT-END

// A: reproduce UE-AS fork's AS declaration changes without changing native reflection flags.
bool SoloASBridge::Compat::IsScriptMethodConst(const UFunction* Function)
{
    return Function->HasAnyFunctionFlags(FUNC_Const)
        && !(Function->GetFName() == FName(TEXT("ModifyRaw"))
            && Function->GetOuterUClass()->GetPathName() == TEXT("/Script/EnhancedInput.InputModifier"));
}
bool SoloASBridge::Compat::IsReplacedScriptTimer(const UFunction* Function)
{
    return Function->GetFName() == FName(TEXT("K2_SetTimer"))
        && Function->GetOuterUClass()->GetPathName() == TEXT("/Script/Engine.KismetSystemLibrary");
}
#if WITH_EDITOR
#include "Modules/ModuleManager.h"
void SoloASBridge::Compat::PrepareScriptBindingModules()
{
    // StructViewer is an on-demand editor module. Load before the native class census.
    FModuleManager::Get().LoadModuleChecked<IModuleInterface>(TEXT("StructViewer"));
}
#endif

                                                                                                                                   
#if SOLOAS_UE_AT_LEAST(5, 8)
namespace SoloASBridge::PrivateAccess
{
struct FCollisionObjectMaskTag
{
    using Type = int64 FCollisionObjectQueryParams::*;
    friend Type Resolve(FCollisionObjectMaskTag) noexcept;
};
template struct TMember<FCollisionObjectMaskTag, &FCollisionObjectQueryParams::ObjectTypesToQuery64>;
}
#endif
// SOLOAS-COMPAT-END
SIZE_T SoloASBridge::Compat::GetCollisionObjectMaskScriptOffset()
{
    static_assert(PLATFORM_LITTLE_ENDIAN, "The UE-AS fork int32 collision mask view requires little-endian storage");
    FCollisionObjectQueryParams Params;
                                                                                                                                   
#if SOLOAS_UE_AT_LEAST(5, 8)
    // A: UE-AS fork scripts expose 32 channels. Writes preserve native channels 32–63.
    const auto* Storage = &SoloASBridge::PrivateAccess::Get<SoloASBridge::PrivateAccess::FCollisionObjectMaskTag>(Params);
#else
    const auto* Storage = &Params.ObjectTypesToQuery;
#endif
// SOLOAS-COMPAT-END
    return reinterpret_cast<const uint8*>(Storage) - reinterpret_cast<const uint8*>(&Params);
}

                                                                                                                     
#if WITH_EDITOR && SOLOAS_UE_BEFORE(5, 5)
#include "Animation/AnimSequence.h"
#include "Animation/PoseAsset.h"
#include "Engine/SkeletalMesh.h"
void SoloASBridge::Compat::SetLegacyRetargetSourceAsset(UObject* Asset, USkeletalMesh* Mesh)
{
    if (!ensureMsgf(!Mesh || !Mesh->HasAnyFlags(RF_Transient),
        TEXT("A transient skeletal mesh cannot be assigned as a retarget source.")))
        return;
    if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset))
        Sequence->RetargetSourceAsset = Mesh;
    else
        CastChecked<UPoseAsset>(Asset)->RetargetSourceAsset = Mesh;
}
const TSoftObjectPtr<USkeletalMesh>& SoloASBridge::Compat::GetLegacyRetargetSourceAsset(const UObject* Asset)
{
    if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Asset))
        return Sequence->RetargetSourceAsset;
    return CastChecked<UPoseAsset>(Asset)->RetargetSourceAsset;
}
#endif
// SOLOAS-COMPAT-END
