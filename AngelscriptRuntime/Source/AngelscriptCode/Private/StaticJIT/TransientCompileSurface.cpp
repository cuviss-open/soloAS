                                                                                                          
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "PrecompiledData.h"
#include "AngelscriptBinds.h"
#include "AngelscriptExecutionQuiescence.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UnrealType.h"
#include "AngelscriptModuleGeneration.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"

#include "StartAngelscriptHeaders.h"
#include "as_scriptengine.h"
#include "as_scriptfunction.h"
#include "as_objecttype.h"
#include "as_module.h"
#include "as_property.h"
#include "EndAngelscriptHeaders.h"

namespace
{
constexpr uint64 CompileRecipeLimit = 128ull * 1024 * 1024;
constinit FCompileSurfaceRecipe* CapturingCompileRecipe = nullptr;
uint64 RecipeStringBytes(const asCString& Value)
{
    return Value.GetLength() < 12 ? 0 : FMemory::QuantizeSize(Value.GetLength() + 1);
}
uint64 RecipeUsageBytes(const FAngelscriptTypeUsage& Usage)
{
    uint64 Result = Usage.SubTypes.GetAllocatedSize();
    for (const auto& SubType : Usage.SubTypes) Result += RecipeUsageBytes(SubType);
    return Result;
}
template<typename K> uint64 RecipeMetadataBytes(const TMap<K, FString>& Meta)
{
    uint64 Result = Meta.GetAllocatedSize();
    for (const auto& Entry : Meta) Result += Entry.Value.GetAllocatedSize();
    return Result;
}
template<typename T> uint64 RecipeFunctionDeclarationBytes(const T& Function)
{
    uint64 Result = Function.FunctionName.GetAllocatedSize() + Function.OriginalFunctionName.GetAllocatedSize()
        + Function.ScriptFunctionName.GetAllocatedSize() + RecipeMetadataBytes(Function.Meta)
        + RecipeUsageBytes(Function.ReturnType) + Function.Arguments.GetAllocatedSize();
    for (const auto& Argument : Function.Arguments)
        Result += Argument.ArgumentName.GetAllocatedSize() + Argument.DefaultValue.GetAllocatedSize() + RecipeUsageBytes(Argument.Type);
    return Result;
}
}

struct FCompileSurfaceRecipe::FImpl
{
    asCScriptEngine* Engine = nullptr;
    uint64 NativeEpoch = 0;
    bool bBindingsCaptured = false;
    bool bRuntimeCaptured = false;
    bool bInvalid = false;
    bool bCapturing = true;
    bool bStopping = false;
    FString Failure;
    FDelegateHandle ModulesChanged;
    FDelegateHandle PreExit;
};
FCompileSurfaceRecipe::FCompileSurfaceRecipe() : Impl(MakeUnique<FImpl>()) {}
FCompileSurfaceRecipe::~FCompileSurfaceRecipe()
{
    if (CapturingCompileRecipe == this) CapturingCompileRecipe = nullptr;
    if (Impl->ModulesChanged.IsValid()) FModuleManager::Get().OnModulesChanged().Remove(Impl->ModulesChanged);
    if (Impl->PreExit.IsValid()) FCoreDelegates::OnPreExit.Remove(Impl->PreExit);
}
TUniquePtr<FCompileSurfaceRecipe> FCompileSurfaceRecipe::Begin(asIScriptEngine* Input,
    uint64 RegistrationEpoch, FString& Error)
{
    using namespace AngelscriptCoreExtensionPoints::Private;
    Error.Reset();
    if (!Input || !IsInGameThread() || !RegistrationEpoch || CapturingCompileRecipe
        || !GTransientNativeTracking.load(std::memory_order_acquire))
    { Error = TEXT("Compile recipe requires a registered, idle GT initialization capture"); return nullptr; }
    TUniquePtr<FCompileSurfaceRecipe> Recipe(new FCompileSurfaceRecipe);
    Recipe->Impl->Engine = static_cast<asCScriptEngine*>(Input);
    Recipe->Impl->Engine->bRetainTransientCompileSurface = true;
    Recipe->Impl->ModulesChanged = FModuleManager::Get().OnModulesChanged().AddLambda(
        [Target = Recipe.Get()](FName, EModuleChangeReason Reason)
        {
            if (Reason == EModuleChangeReason::ModuleUnloaded) Target->Stop();
        });
    Recipe->Impl->PreExit = FCoreDelegates::OnPreExit.AddLambda([Target = Recipe.Get()] { Target->Stop(); });
    CapturingCompileRecipe = Recipe.Get();
    return Recipe;
}
bool FCompileSurfaceRecipe::CaptureBindings(FString& Error)
{
    Error.Reset();
    auto& State = *Impl;
    if (!IsInGameThread() || CapturingCompileRecipe != this || State.bBindingsCaptured)
    { Error = TEXT("Compile binding capture is not open"); return false; }
    State.NativeEpoch = AngelscriptCoreExtensionPoints::Private::GetNativeRegistryEpoch(State.Engine);
    State.bBindingsCaptured = true;
    return true;
}
uint64 FCompileSurfaceRecipe::GetAllocatedBytes() const
{
    return sizeof(FCompileSurfaceRecipe) + FMemory::GetAllocSize(Impl.Get()) + Impl->Failure.GetAllocatedSize();
}
bool FCompileSurfaceRecipe::IsReady(FString& Error) const
{
    Error.Reset();
    if (!IsInGameThread() || !Impl->bBindingsCaptured || !Impl->bRuntimeCaptured || Impl->bInvalid
        || Impl->NativeEpoch != AngelscriptCoreExtensionPoints::Private::GetNativeRegistryEpoch(Impl->Engine))
    { Error = Impl->Failure.IsEmpty() ? FString::Printf(TEXT("Transient compile recipe Unavailable: incomplete or changed declarations expectedEpoch=%llu actualEpoch=%llu"),
        Impl->NativeEpoch, AngelscriptCoreExtensionPoints::Private::GetNativeRegistryEpoch(Impl->Engine)) : Impl->Failure; return false; }
    return true;
}
void FCompileSurfaceRecipe::AcceptGameplayTagRegistration(asIScriptEngine* Engine, uint64 PreviousEpoch, int32 PropertyId)
{
    using namespace AngelscriptCoreExtensionPoints::Private;
    if (!IsInGameThread() || PropertyId < 0 || !PreviousEpoch || Impl->Engine != Engine
        || !Impl->bBindingsCaptured || !Impl->bRuntimeCaptured
        || Impl->bInvalid || Impl->bCapturing || Impl->bStopping || IsNativeCompileActionActive()
        || Impl->NativeEpoch != PreviousEpoch || GetNativeRegistryEpoch(Impl->Engine) != PreviousEpoch + 1)
        return;
    ++Impl->NativeEpoch;
}
bool FCompileSurfaceRecipe::IsCapturingOrStopping() const { return Impl->bCapturing || Impl->bStopping; }
void FCompileSurfaceRecipe::FinishCapture()
{
    Impl->bCapturing = false;
    if (CapturingCompileRecipe == this) CapturingCompileRecipe = nullptr;
}
void FCompileSurfaceRecipe::Stop()
{
    if (Impl->bInvalid && Impl->Failure == TEXT("Transient compile recipe stopped before native provider shutdown")) return;
    TGuardValue<bool> Stopping(Impl->bStopping, true);
    Impl->bCapturing = false;
    Impl->bInvalid = true;
    AngelscriptCoreExtensionPoints::Private::InvalidateNativeCompileAction(Impl->Engine);
    Impl->Failure = TEXT("Transient compile recipe stopped before native provider shutdown");
    if (CapturingCompileRecipe == this) CapturingCompileRecipe = nullptr;
}
bool FCompileSurfaceRecipe::BeforeClear(asCScriptEngine* Engine)
{
    if (Engine->bRetainTransientCompileSurface)
    {
        int32 RetainedFunctions = 0;
        Engine->RetainedCompileSurfaceBytes = MeasureRetainedCompileSurface(Engine, RetainedFunctions);
        UE_LOG(Angelscript, Log, TEXT("H6 Clear: retained=1 deletedNativeFunctions=0 retainedNativeFunctions=%d retainedCompileSurfaceBytes=%llu"),
            RetainedFunctions, Engine->RetainedCompileSurfaceBytes);
        return true;
    }
    return false;
}
uint64 FCompileSurfaceRecipe::MeasureRetainedCompileSurface(asCScriptEngine* Engine, int32& RetainedFunctions)
{
    RetainedFunctions = 0;
    uint64 Bytes = AngelscriptCoreExtensionPoints::Private::GetNativeCompileTrackingBytes();
    auto ArrayBytes = [](const auto& Values)
    {
        using T = typename TRemoveReference<decltype(Values[0])>::Type;
        const uint64 Size = uint64(Values.GetCapacity()) * sizeof(T);
        return Size > 8 * AS_PTR_SIZE ? FMemory::QuantizeSize(Size, alignof(T)) : 0;
    };
    for (asUINT I = 0; I < Engine->scriptFunctions.GetLength(); ++I)
    {
        const auto* Function = Engine->scriptFunctions[I];
        if (!Function) continue;
        bool bDeleted = Function->funcType == asFUNC_SYSTEM && !Function->isInUse;
        if (const auto* Object = Function->objectType)
            bDeleted &= Object->beh.construct != Function->id && Object->beh.copy != Function->id
                && Object->beh.factory != Function->id && Object->beh.destruct != Function->id;
        if (bDeleted)
        {
            ++RetainedFunctions;
            Bytes += FMemory::GetAllocSize(const_cast<asCScriptFunction*>(Function))
                + (Function->sysFuncIntf ? FMemory::GetAllocSize(Function->sysFuncIntf) : 0)
                + RecipeStringBytes(Function->name) + ArrayBytes(Function->parameterTypes)
                + ArrayBytes(Function->inOutFlags) + ArrayBytes(Function->parameterOffsets);
        }
        Bytes += ArrayBytes(Function->defaultArgs) + ArrayBytes(Function->parameterNames)
            + RecipeStringBytes(Function->hiddenArgumentDefault);
        for (asUINT N = 0; N < Function->defaultArgs.GetLength(); ++N)
            if (const auto* Value = Function->defaultArgs[N])
                Bytes += FMemory::GetAllocSize(const_cast<asCString*>(Value)) + RecipeStringBytes(*Value);
        for (asUINT N = 0; N < Function->parameterNames.GetLength(); ++N) Bytes += RecipeStringBytes(Function->parameterNames[N]);
    }
    auto ObjectBytes = [&](const asCObjectType* Object)
    {
        return Object ? Object->propertyTable.GetAllocatedSize() + Object->methodTable.GetAllocatedSize() : 0;
    };
    auto EnumBytes = [&](const asCEnumType* Enum)
    {
        uint64 Result = Enum ? ArrayBytes(Enum->enumValues) : 0;
        if (Enum) for (asUINT I = 0; I < Enum->enumValues.GetLength(); ++I)
            Result += FMemory::GetAllocSize(Enum->enumValues[I]) + RecipeStringBytes(Enum->enumValues[I]->name);
        return Result;
    };
    for (asUINT I = 0; I < Engine->registeredObjTypes.GetLength(); ++I) Bytes += ObjectBytes(Engine->registeredObjTypes[I]);
    for (asUINT I = 0; I < Engine->registeredEnums.GetLength(); ++I) Bytes += EnumBytes(Engine->registeredEnums[I]);
    for (asUINT I = 0; I < Engine->scriptModules.GetLength(); ++I)
    {
        const auto* Module = Engine->scriptModules[I];
        if (!Module) continue;
        Bytes += Module->scriptGlobals.GetAllocatedSize() + Module->globalFunctions.GetAllocatedSize()
            + Module->allLocalTypes.GetAllocatedSize() + Module->PreClassData.GetAllocatedSize()
            + ArrayBytes(Module->classTypes) + ArrayBytes(Module->enumTypes) + ArrayBytes(Module->scriptFunctions);
        for (asUINT N = 0; N < Module->classTypes.GetLength(); ++N) Bytes += ObjectBytes(Module->classTypes[N]);
        for (asUINT N = 0; N < Module->enumTypes.GetLength(); ++N) Bytes += EnumBytes(Module->enumTypes[N]);
        for (const auto& Entry : Module->PreClassData) Bytes += RecipeStringBytes(Entry.Key);
    }
    Bytes += Engine->nameSpacesByName.GetAllocatedSize() + Engine->scriptModulesByName.GetAllocatedSize()
        + Engine->allRegisteredTypes.GetAllocatedSize() + Engine->allRegisteredTypesByName.GetAllocatedSize()
        + Engine->registeredGlobalFuncTable.GetAllocatedSize() + Engine->registeredGlobalPropTable.GetAllocatedSize()
        + Engine->varAddressMap.GetAllocatedSize() + Engine->templateInstanceBuckets.GetAllocatedSize()
        + ArrayBytes(Engine->registeredTemplateTypes) + ArrayBytes(Engine->registeredObjTypes)
        + ArrayBytes(Engine->registeredEnums) + ArrayBytes(Engine->registeredTypeDefs) + ArrayBytes(Engine->nameSpaces);
    for (const auto& Bucket : Engine->templateInstanceBuckets) Bytes += Bucket.Value.GetAllocatedSize();
    if (FAngelscriptManager::IsInitialized() && FAngelscriptManager::Get().GetScriptEngine() == Engine)
    {
        const auto& Manager = FAngelscriptManager::Get();
        Bytes += Manager.ActiveModules.GetAllocatedSize() + Manager.ModulesByScriptModule.GetAllocatedSize()
            + Manager.BoundBlueprintEventArgumentSpecializations.GetAllocatedSize();
        for (const auto& Name : Manager.BoundBlueprintEventArgumentSpecializations) Bytes += Name.GetAllocatedSize();
        for (const auto& Pair : Manager.ActiveModules)
        {
            const auto& Module = Pair.Value.Get();
            Bytes += sizeof(FAngelscriptModuleDesc) + Module.ModuleName.GetAllocatedSize()
                + Module.Code.GetAllocatedSize() + Module.Classes.GetAllocatedSize() + Module.Enums.GetAllocatedSize()
                + Module.Delegates.GetAllocatedSize() + Module.DeclaredLiteralAssets.GetAllocatedSize() + Module.PostInitFunctions.GetAllocatedSize();
            for (const auto& Name : Module.DeclaredLiteralAssets) Bytes += Name.GetAllocatedSize();
            for (const auto& Name : Module.PostInitFunctions) Bytes += Name.GetAllocatedSize();
            for (const auto& Code : Module.Code) Bytes += Code.RelativeFilename.GetAllocatedSize() + Code.AbsoluteFilename.GetAllocatedSize() + Code.Code.GetAllocatedSize();
            for (const auto& Class : Module.Classes)
            {
                Bytes += sizeof(FAngelscriptClassDesc) + Class->ClassName.GetAllocatedSize() + Class->SuperClass.GetAllocatedSize()
                    + Class->ConfigName.GetAllocatedSize() + Class->ComposeOntoClass.GetAllocatedSize() + Class->StaticClassGlobalVariableName.GetAllocatedSize()
                    + Class->Properties.GetAllocatedSize() + Class->Methods.GetAllocatedSize() + RecipeMetadataBytes(Class->Meta);
                for (const auto& Property : Class->Properties)
                    Bytes += sizeof(FAngelscriptPropertyDesc) + Property->PropertyName.GetAllocatedSize()
                        + RecipeUsageBytes(Property->PropertyType) + RecipeMetadataBytes(Property->Meta);
                for (const auto& Method : Class->Methods) Bytes += sizeof(FAngelscriptFunctionDesc) + RecipeFunctionDeclarationBytes(*Method);
            }
            for (const auto& Enum : Module.Enums) Bytes += sizeof(FAngelscriptEnumDesc) + Enum->EnumName.GetAllocatedSize()
                + Enum->ValueNames.GetAllocatedSize() + Enum->EnumValues.GetAllocatedSize() + RecipeMetadataBytes(Enum->Meta);
            for (const auto& Delegate : Module.Delegates) Bytes += sizeof(FAngelscriptDelegateDesc) + Delegate->DelegateName.GetAllocatedSize()
                + (Delegate->Signature ? RecipeFunctionDeclarationBytes(*Delegate->Signature) : 0);
        }
        extern TMap<UClass*, TMap<FString, UFunction*>> GBlueprintEventsByScriptName;
        Bytes += GBlueprintEventsByScriptName.GetAllocatedSize();
        for (const auto& Pair : GBlueprintEventsByScriptName)
        {
            Bytes += Pair.Value.GetAllocatedSize();
            for (const auto& Event : Pair.Value) Bytes += Event.Key.GetAllocatedSize();
        }
#if AS_CAN_HOTRELOAD
        Bytes += Manager.ActiveClassesByName.GetAllocatedSize() + Manager.ActiveEnumsByName.GetAllocatedSize()
            + Manager.ActiveDelegatesByName.GetAllocatedSize() + Manager.ActiveLiteralAssetsByName.GetAllocatedSize();
#endif
    }
    return Bytes;
}
bool FCompileSurfaceRecipe::CaptureRuntime(FAngelscriptManager* Manager, FString& Error)
{
    using namespace AngelscriptCoreExtensionPoints::Private;
    Error.Reset();
    auto& State = *Impl;
    if (!IsInGameThread() || CapturingCompileRecipe != this || !State.bBindingsCaptured
        || State.bRuntimeCaptured || State.bInvalid || (Manager && Manager->GetScriptEngine() != State.Engine))
    { Error = State.Failure.IsEmpty() ? TEXT("Runtime declaration capture is not open") : State.Failure; return false; }
        State.NativeEpoch = GetNativeRegistryEpoch(State.Engine);
        State.bRuntimeCaptured = true;
        return true;
}
void FCompileSurfaceRecipe::AddReferencedObjects(FReferenceCollector&) {}

struct FCompileSurfaceLease::FImpl
{
    asCScriptEngine* Engine;
    AngelscriptCoreExtensionPoints::Private::FNativeCompileActionGuard Action;
    uint64 ReservedBytes = 0;
    uint64 InitialTrackingBytes = 0;
    explicit FImpl(asCScriptEngine* InEngine) : Engine(InEngine), Action(InEngine) {}
};
FCompileSurfaceLease::FCompileSurfaceLease(asCScriptEngine* Engine) : Impl(MakeUnique<FImpl>(Engine)) {}
FCompileSurfaceLease::~FCompileSurfaceLease() = default;
uint64 FCompileSurfaceLease::GetReservedBytes() const
{
    if (!Impl->Engine->bRetainTransientCompileSurface) return Impl->ReservedBytes;
    const uint64 Tracking = AngelscriptCoreExtensionPoints::Private::GetNativeCompileTrackingBytes();
    return Impl->ReservedBytes + (Tracking > Impl->InitialTrackingBytes ? Tracking - Impl->InitialTrackingBytes : 0);
}
bool FCompileSurfaceLease::ReserveInput(uint64 Bytes, FString& Error, bool bRecord)
{
    const uint64 Current = GetReservedBytes();
    if (Current > CompileRecipeLimit || Bytes > CompileRecipeLimit - Current)
    { Error = TEXT("H6 action input/lookup metadata exceeds the 128 MiB supplemental budget"); return false; }
    if (bRecord) Impl->ReservedBytes += Bytes;
    return true;
}
TUniquePtr<AngelscriptCoreExtensionPoints::Private::FNativeCompileCoreCallGuard> FCompileSurfaceLease::EnterManagerCall()
{
    using AngelscriptCoreExtensionPoints::Private::FNativeCompileCoreCallGuard;
    return TUniquePtr<FNativeCompileCoreCallGuard>(new FNativeCompileCoreCallGuard(EnterCoreCall()));
}
AngelscriptCoreExtensionPoints::Private::FNativeCompileCoreCallGuard FCompileSurfaceLease::EnterCoreCall()
{
    return AngelscriptCoreExtensionPoints::Private::FNativeCompileCoreCallGuard(Impl->Engine);
}
TUniquePtr<FCompileSurfaceLease> FCompileSurfaceLease::Acquire(FCompileSurfaceRecipe& Recipe, FString& Error)
{
    using namespace AngelscriptCoreExtensionPoints::Private;
    Error.Reset();
    auto& State = *Recipe.Impl;
    auto* Engine = State.Engine;
    if (!IsInGameThread() || !Engine || IsGarbageCollecting() ||
        !IsAngelscriptExecutionWriterHeldByCurrentThread() || !GTransientNativeTracking.load(std::memory_order_acquire)
        || IsNativeCompileActionActive() || Engine->isBuilding || Engine->shuttingDown
        || IsAngelscriptExecutionShutdownStarted())
    { Error = TEXT("Compile restoration requires an idle admitted GT writer"); return nullptr; }
    if (State.bBindingsCaptured && State.bRuntimeCaptured
        && !State.bInvalid && !Recipe.IsCapturingOrStopping())
    {
        // Retained declarations are live VM state, rather than an immutable restoration image.
        // The admitted idle writer may adopt declarations added since startup capture.
        State.NativeEpoch = GetNativeRegistryEpoch(Engine);
    }
    if (!Recipe.IsReady(Error) || Recipe.IsCapturingOrStopping()) return nullptr;
        TUniquePtr<FCompileSurfaceLease> Lease(new FCompileSurfaceLease(Engine));
        Lease->Impl->InitialTrackingBytes = GetNativeCompileTrackingBytes();
        Lease->Impl->ReservedBytes = FMemory::QuantizeSize(sizeof(FCompileSurfaceLease))
            + FMemory::QuantizeSize(sizeof(FImpl)) + FMemory::QuantizeSize(sizeof(FAngelscriptExecutionWriteScope))
            + Recipe.GetAllocatedBytes();
        UE_LOG(Angelscript, Log, TEXT("H6 retained surface: actionBytes=%llu retainedCompileSurfaceBytes=%llu limit=%llu"),
            Lease->Impl->ReservedBytes, Engine->RetainedCompileSurfaceBytes, CompileRecipeLimit);
        return Lease;
}
TUniquePtr<FCompileSurfaceLease> FCompileSurfaceLease::AcquireForRemoval(asIScriptEngine* Input, FString& Error)
{
    using namespace AngelscriptCoreExtensionPoints::Private;
    Error.Reset();
    auto* Engine = static_cast<asCScriptEngine*>(Input);
    if (!IsInGameThread() || !Engine || !Engine->bRetainTransientCompileSurface
        || !IsAngelscriptExecutionWriterHeldByCurrentThread() || IsGarbageCollecting()
        || IsNativeCompileActionActive() || Engine->isBuilding || Engine->shuttingDown
        || IsAngelscriptExecutionShutdownStarted())
    { Error = TEXT("Source removal requires retained live indexes and an idle GT writer"); return nullptr; }
    // Removal consumes existing bytecode and live VM indexes. It neither restores
    // captured subjects nor compiles declarations, so a capture epoch is not used.
    // The action guard still denies native mutation; no owner compile lease is set.
    return TUniquePtr<FCompileSurfaceLease>(new FCompileSurfaceLease(Engine));
}
                                                                                                        
