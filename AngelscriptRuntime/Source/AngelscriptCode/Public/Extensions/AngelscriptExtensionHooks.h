// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

#include "AngelscriptType.h"

class UClass;
struct FAngelscriptPreprocessor;
struct FAngelscriptClassDesc;
struct FAngelscriptFunctionDesc;
class UFunction;
struct FAngelscriptMethodBind;
struct FAngelscriptModuleDesc;

/** Result returned synchronously by one optional Runtime extension callback. */
enum class EAngelscriptExtensionResult : uint8
{
	NotHandled,
	Handled,
	Error,
};

enum class EAngelscriptBlueprintEventTarget : uint8
{
	GlobalFunction,
	MixinMethod,
	Method,
};

/** Runtime-owned callback presence used to keep extension seams near zero-cost. */
enum class EAngelscriptExtensionStage : uint8
{
	PreParse = 1,
	BlueprintEventBound = 2,
	NativeInterfaceSelection = 3,
	TypeGeneration = 4,
};

/**
 * Value snapshot of one already-bound Blueprint event.
 *
 * The snapshot is passed by borrowed reference. Type usages and reflection
 * objects reachable through it are valid only for the synchronous callback;
 * providers must not retain any of them.
 */
struct FAngelscriptBlueprintEventBindingSnapshot
{
	EAngelscriptBlueprintEventTarget Target =
		EAngelscriptBlueprintEventTarget::Method;
	FString TargetScriptTypeName;
	FString Declaration;
	FString ScriptName;
	FString NativeDeclaringClassPath;
	FName NativeFunctionName = NAME_None;
	TArray<FAngelscriptTypeUsage> ArgumentTypes;
	FAngelscriptTypeUsage ReturnType;
	bool bInitializeReturn = false;
	bool bZeroReturnPointer = false;
};

/** Synchronous context. Do not retain it or its borrowed preprocessor. */
struct FAngelscriptPreParseContext
{
	FAngelscriptPreprocessor* Preprocessor = nullptr;
	EAngelscriptExtensionResult Result =
		EAngelscriptExtensionResult::NotHandled;
	FString Error;
};

/** Synchronous context. Do not retain it or its borrowed binding snapshot. */
struct FAngelscriptBlueprintEventBoundContext
{
	const FAngelscriptBlueprintEventBindingSnapshot* Binding = nullptr;
	EAngelscriptExtensionResult Result =
		EAngelscriptExtensionResult::NotHandled;
	FString Error;
};

enum class EAngelscriptNativeInterfacePoint : uint8
{
    Select,
    RegisterType,
    PrepareDatabase,
    BindDatabaseMethods,
    BindReflectedMethods,
    FinalizeRecord,
    RegisterEvent,
    ResolveEvent,
};

/** Return true to retain a reflected method; Bind is valid only during Visit. */
using FAngelscriptNativeInterfaceMethodVisitor = TFunctionRef<bool(
    UFunction*, const FAngelscriptBlueprintEventBindingSnapshot&, TFunctionRef<void()> Bind)>;

/** Synchronous context. Do not retain it or its borrowed native class. */
struct FAngelscriptNativeInterfaceSelectionContext
{
    EAngelscriptNativeInterfacePoint Point = EAngelscriptNativeInterfacePoint::Select;
    FString TypeName;
    TSharedPtr<FAngelscriptType> Type;
    // Borrowed synchronous operations. Never retain callbacks or method indices.
    // Database records and their storage stay entirely inside Runtime.
    TFunction<void(UClass*, const FString&)> RegisterType;
    TFunction<bool(UClass*)> ResolveDatabaseClass;
    int32 DatabaseMethodCount = 0;
    TFunction<FString(int32)> GetDatabaseMethodDeclaration;
    TFunction<void(int32, FAngelscriptNativeInterfaceMethodVisitor)> VisitDatabaseMethod;
    TFunction<void(UFunction*, FAngelscriptNativeInterfaceMethodVisitor)> VisitReflectedMethod;
    TFunction<void()> FinalizeRecord;
    TMap<UClass*, TMap<FString, UFunction*>>* EventsByScriptName = nullptr;
    FString ScriptName;
    UFunction* ResolvedFunction = nullptr;
	UClass* NativeClass = nullptr;
	bool bBindAsInterface = false;
	EAngelscriptExtensionResult Result =
		EAngelscriptExtensionResult::NotHandled;
	FString Error;
};

/** Borrowed synchronous type-generation input; extensions own selection policy. */
enum class EAngelscriptTypeGenerationPoint : uint8
{
	PrepareBindings,
	ReferenceType,
	ResolveSuperClass,
	ResolveFunction,
	ConfigureClass,
	PrepareSource,
	ConstructDefaultObject,
	RestoreCachedTypes,
	PrepareModuleCommit,
	CommitModules,
	AbortModules,
	SelectModuleTransaction,
	RetainPreviousInstances,
	PrepareInstanceCommit,
};

/** Owned values read from the existing precompiled class metadata records. */
struct FAngelscriptCachedTypeMetadata
{
	FString ModuleName;
	TMap<FName, FString> Metadata;
	FString ClassName;
};

struct FAngelscriptTypeGenerationContext
{
	EAngelscriptTypeGenerationPoint Point = EAngelscriptTypeGenerationPoint::PrepareBindings;
	FAngelscriptPreprocessor* Preprocessor = nullptr;
	// Borrowed only for the synchronous compile callback.
	const TArray<TSharedRef<FAngelscriptModuleDesc>>* Modules = nullptr;
	bool bHasExternalModuleConsumers = false;
	bool bModulesSwapped = false;
	// Optional provider requests isolated classes before any published generation changes.
	bool bPrepareClassGeneration = false;
	FAngelscriptClassDesc* Class = nullptr;
	FAngelscriptFunctionDesc* Function = nullptr;
	UFunction* ResolvedFunction = nullptr;
	UClass* ReflectionClass = nullptr;
	const class FObjectInitializer* ObjectInitializer = nullptr;
	bool bUseObjectView = false;
	bool bPreloadOnly = false;
	const TArray<FAngelscriptCachedTypeMetadata>* CachedTypes = nullptr;
	const TArray<FString>* CachedNativeTypeNames = nullptr;
	EAngelscriptExtensionResult Result = EAngelscriptExtensionResult::NotHandled;
	FString Error;
	// Optional provider requests one preprocessed dependency closure per commit.
	bool bCompileDependencyClosure = false;
	// Borrowed output for one synchronous Editor reinstance query; never retain.
	TSet<UObject*>* PreviousInstancesToRetain = nullptr;
	// Optional prepared Blueprint generations for Editor coordination only.
	TMap<UClass*, UClass*>* PreparedBlueprintClasses = nullptr;
	TMap<UObject*, UObject*>* PreparedBlueprintObjects = nullptr;
	TSet<UObject*>* OwnedBlueprintMetadata = nullptr;
};

/**
 * Stable cache identity supplied by the one optional Extensions provider.
 * Stage callbacks are registered independently through the typed callback
 * registration functions below.
 */
struct FAngelscriptExtensionProvider
{
	/** 1-64 ASCII bytes from [A-Za-z0-9._/-]; cache identity is lowercase. */
	FName ProviderId = NAME_None;
	/** 1-64 ASCII bytes from [A-Za-z0-9._/-]. */
	FString CompatibilityTag;
};

using FAngelscriptPreParseExtensionCallback =
	EAngelscriptExtensionResult (*)(FAngelscriptPreParseContext& Context);
using FAngelscriptBlueprintEventBoundExtensionCallback =
	EAngelscriptExtensionResult (*)(
		FAngelscriptBlueprintEventBoundContext& Context);
using FAngelscriptNativeInterfaceSelectionExtensionCallback =
	EAngelscriptExtensionResult (*)(
		FAngelscriptNativeInterfaceSelectionContext& Context);

using FAngelscriptTypeGenerationExtensionCallback =
	EAngelscriptExtensionResult (*)(FAngelscriptTypeGenerationContext& Context);

/** Opaque move-only identity for one active provider registration. */
class ANGELSCRIPTCODE_API FAngelscriptExtensionProviderRegistration final
{
public:
	FAngelscriptExtensionProviderRegistration() = default;
	~FAngelscriptExtensionProviderRegistration() = default;

	FAngelscriptExtensionProviderRegistration(
		FAngelscriptExtensionProviderRegistration&& Other) noexcept;
	FAngelscriptExtensionProviderRegistration& operator=(
		FAngelscriptExtensionProviderRegistration&& Other) noexcept;

	FAngelscriptExtensionProviderRegistration(
		const FAngelscriptExtensionProviderRegistration&) = delete;
	FAngelscriptExtensionProviderRegistration& operator=(
		const FAngelscriptExtensionProviderRegistration&) = delete;

	bool IsValid() const;

private:
	friend ANGELSCRIPTCODE_API bool RegisterAngelscriptExtensionProvider(
		const FAngelscriptExtensionProvider&,
		FAngelscriptExtensionProviderRegistration&,
		FString&);
	friend ANGELSCRIPTCODE_API bool UnregisterAngelscriptExtensionProvider(
		FAngelscriptExtensionProviderRegistration&,
		FString&);

	uint64 Serial = 0;
};

/** Opaque move-only identity for one active callback registration. */
class ANGELSCRIPTCODE_API FAngelscriptExtensionCallbackRegistration final
{
public:
	FAngelscriptExtensionCallbackRegistration() = default;
	~FAngelscriptExtensionCallbackRegistration() = default;

	FAngelscriptExtensionCallbackRegistration(
		FAngelscriptExtensionCallbackRegistration&& Other) noexcept;
	FAngelscriptExtensionCallbackRegistration& operator=(
		FAngelscriptExtensionCallbackRegistration&& Other) noexcept;

	FAngelscriptExtensionCallbackRegistration(
		const FAngelscriptExtensionCallbackRegistration&) = delete;
	FAngelscriptExtensionCallbackRegistration& operator=(
		const FAngelscriptExtensionCallbackRegistration&) = delete;

	bool IsValid() const;

private:
	friend ANGELSCRIPTCODE_API bool RegisterAngelscriptPreParseExtension(
		FAngelscriptPreParseExtensionCallback,
		FAngelscriptExtensionCallbackRegistration&,
		FString&);
	friend ANGELSCRIPTCODE_API bool
	RegisterAngelscriptBlueprintEventBoundExtension(
		FAngelscriptBlueprintEventBoundExtensionCallback,
		FAngelscriptExtensionCallbackRegistration&,
		FString&);
	friend ANGELSCRIPTCODE_API bool
	RegisterAngelscriptNativeInterfaceSelectionExtension(
		FAngelscriptNativeInterfaceSelectionExtensionCallback,
		FAngelscriptExtensionCallbackRegistration&,
		FString&, FAngelscriptNativeInterfaceSelectionExtensionCallback);
	friend ANGELSCRIPTCODE_API bool RegisterAngelscriptTypeGenerationExtension(
		FAngelscriptTypeGenerationExtensionCallback,
		FAngelscriptExtensionCallbackRegistration&,
		FString&);
	friend ANGELSCRIPTCODE_API bool UnregisterAngelscriptExtensionCallback(
		FAngelscriptExtensionCallbackRegistration&,
		FString&);

	uint64 Serial = 0;
	uint8 Stage = 0;
};

/** Registers the only active provider. Must be called on the game thread. */
ANGELSCRIPTCODE_API bool RegisterAngelscriptExtensionProvider(
	const FAngelscriptExtensionProvider& Provider,
	FAngelscriptExtensionProviderRegistration& OutRegistration,
	FString& OutError
);

/** Explicitly unregisters the active provider. */
ANGELSCRIPTCODE_API bool UnregisterAngelscriptExtensionProvider(
	FAngelscriptExtensionProviderRegistration& Registration,
	FString& OutError
);

ANGELSCRIPTCODE_API bool RegisterAngelscriptPreParseExtension(
	FAngelscriptPreParseExtensionCallback Callback,
	FAngelscriptExtensionCallbackRegistration& OutRegistration,
	FString& OutError
);
ANGELSCRIPTCODE_API bool RegisterAngelscriptBlueprintEventBoundExtension(
	FAngelscriptBlueprintEventBoundExtensionCallback Callback,
	FAngelscriptExtensionCallbackRegistration& OutRegistration,
	FString& OutError
);
ANGELSCRIPTCODE_API bool RegisterAngelscriptNativeInterfaceSelectionExtension(
	FAngelscriptNativeInterfaceSelectionExtensionCallback Callback,
	FAngelscriptExtensionCallbackRegistration& OutRegistration,
	FString& OutError,
    FAngelscriptNativeInterfaceSelectionExtensionCallback BindingCallback = nullptr
);

/** Explicitly unregisters one callback registration. */
ANGELSCRIPTCODE_API bool UnregisterAngelscriptExtensionCallback(
	FAngelscriptExtensionCallbackRegistration& Registration,
	FString& OutError
);

/** Returns whether a callback is registered for one stage, without exposing Anchor. */
ANGELSCRIPTCODE_API bool IsAngelscriptExtensionStageRegistered(
	EAngelscriptExtensionStage Stage);

/** Returns whether the optional Extensions provider is active. */
ANGELSCRIPTCODE_API bool IsAngelscriptExtensionProviderRegistered();




ANGELSCRIPTCODE_API bool DispatchAngelscriptBlueprintEventBound(
	const FAngelscriptBlueprintEventBindingSnapshot& Binding,
	FString& OutError
);


ANGELSCRIPTCODE_API bool RegisterAngelscriptTypeGenerationExtension(
	FAngelscriptTypeGenerationExtensionCallback Callback,
	FAngelscriptExtensionCallbackRegistration& OutRegistration,
	FString& OutError);
ANGELSCRIPTCODE_API bool DispatchAngelscriptTypeGeneration(
	FAngelscriptTypeGenerationContext& Context,
	FString& OutError);

/** Register a UObject reference view without an AS VM interface or a constructor. */
ANGELSCRIPTCODE_API bool RegisterAngelscriptObjectView(
    UClass* Class, const FString& ScriptName, FString& OutError);

/** Preflight an explicitly requested object generation replacement.
 * Existing bound fields must retain their type, flags and storage offsets;
 * existing methods must retain their signatures. New fields are allowed.
 * This does not migrate objects or mutate bindings.
 */
ANGELSCRIPTCODE_API bool CanRebindAngelscriptObjectViewGeneration(
    UClass* PreviousClass, UClass* ReplacementClass, FString& OutError);

/** Publish the preflighted view after the caller has prepared object migration.
 * The caller owns quiescence and both reflection generations' lifetimes.
 * No borrowed VM type is released and no UObject references are migrated here.
 */
ANGELSCRIPTCODE_API bool RebindAngelscriptObjectViewGeneration(
    UClass* PreviousClass, UClass* ReplacementClass, FString& OutError);

#if WITH_EDITOR && !IS_MONOLITHIC
/** Explicitly rebind an object view without releasing its borrowed VM type.
 * Parent, property layout and method signatures must remain compatible.
 * The caller retains the previous reflection class until this succeeds.
 */
ANGELSCRIPTCODE_API bool RebindAngelscriptObjectView(
    UClass* PreviousClass, UClass* ReplacementClass, FString& OutError);
#endif
