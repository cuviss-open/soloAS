                                                                                                    
// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "AngelscriptCoreAdmission.h"
class asIScriptEngine;
class asITypeInfo;
class asIScriptGeneric;

namespace AngelscriptCoreExtensionPoints
{
struct FNativeDelegateProvider
{
    TFunction<bool(const asITypeInfo*)> IsNativeType;
    TFunction<bool(asIScriptGeneric&)> Bind;
    TFunction<void()> InvalidateCaptures;
};
enum class ENativeDelegateResult : uint8 { Unavailable, NotNative, Completed, Rejected, Busy };
// One provider per engine. Bind checks the actual argument's type with the same
// provider before dispatch; delegate/capture storage remains provider-owned.
// The consumer invokes Invalidate at its existing safe reload point, outside
// script execution. This API does not install reload/VM observers or AS bindings.
ANGELSCRIPTCODE_API FDelegateHandle RegisterNativeDelegateProvider(asIScriptEngine* Engine, FNativeDelegateProvider Provider);
ANGELSCRIPTCODE_API bool IsNativeDelegateType(const asITypeInfo* Type);
ANGELSCRIPTCODE_API ENativeDelegateResult BindNativeDelegate(asIScriptGeneric& Context, uint32 DelegateArgument);
ANGELSCRIPTCODE_API ENativeDelegateResult InvalidateNativeDelegateCaptures(asIScriptEngine* Engine);
ANGELSCRIPTCODE_API ERemoveResult UnregisterNativeDelegateProvider(FDelegateHandle Handle);

// Registration during any provider callback fails. Removal during a callback
// returns Busy without mutation: callable code, captures and borrowed arguments
// remain alive. Stop new requests and retry after the outer call has returned.
// Removed destroys callbacks; subsequent calls cannot reach them. Callbacks must
// not compile/retire a VM, unload their module, or destroy borrowed arguments.
// Before VM/module shutdown, remove providers while their code/engine is alive.
}
                                                                                                  

                                                                                                  
class UClass;
struct FAngelscriptClassDesc;
namespace AngelscriptCoreExtensionPoints
{
// Game-thread safe points only, outside compilation, script execution and GC.
// Names identify providers uniquely. Queries borrow only string views for this
// call; never retain them or compile/reenter the generator from a query.
// The consumer owns returned interface UClasses until publication. Published
// UClass::Interfaces retains them; removal only drains/destroys the callback,
// never tears down already published types or their VM references.
using FClassInterfaceQuery = TFunction<bool(FStringView ModuleName, FStringView ClassName,
    TArray<UClass*>& Interfaces, FString& Error)>;
ANGELSCRIPTCODE_API FDelegateHandle RegisterClassInterfaceProvider(FName Name, FClassInterfaceQuery Query);
ANGELSCRIPTCODE_API ERemoveResult UnregisterClassInterfaceProvider(FDelegateHandle Handle);
namespace Private
{
void ShutdownClassInterfaceProviders();
bool HasClassInterfaceProviders();
bool QueryClassInterfaces(FStringView ModuleName, const FAngelscriptClassDesc& Class,
    TArray<UClass*>& Out, FString& Error);
}
}
                                                                                                

                                                                                                   
struct FAngelscriptClassDesc;
struct FAngelscriptModuleDesc;
namespace AngelscriptCoreExtensionPoints
{
enum class EBlueprintCarrierResult : uint8 { Unhandled, Reject, Allow };
// Queries borrow the exact proposed parent and names for this synchronous call.
// Only the owning importer may allow its ancestry; the core retains identity
// facts, never this callback, on the published carrier. No superclass mutation.
using FBlueprintCarrierQuery = TFunction<EBlueprintCarrierResult(FStringView ModuleName,
    FStringView ClassName, const UClass* ProposedParent, FString& Diagnostic)>;
// Named single owner, GT safe point outside script execution, compilation and GC.
ANGELSCRIPTCODE_API FDelegateHandle RegisterBlueprintCarrierProvider(FName Name, FBlueprintCarrierQuery Query);
ANGELSCRIPTCODE_API ERemoveResult UnregisterBlueprintCarrierProvider(FDelegateHandle Handle);
namespace Private
{
struct FBlueprintCarrierCompilationScope
{
    FBlueprintCarrierCompilationScope(
        const TMap<FString, TSharedRef<FAngelscriptClassDesc>>& Classes,
        const TMap<FString, TSharedRef<FAngelscriptModuleDesc>>& Modules);
    ~FBlueprintCarrierCompilationScope();
private:
    const TMap<FString, TSharedRef<FAngelscriptClassDesc>>* PreviousClasses;
    const TMap<FString, TSharedRef<FAngelscriptModuleDesc>>* PreviousModules;
};
ANGELSCRIPTCODE_API EBlueprintCarrierResult QueryBlueprintCarrier(FStringView ModuleName,
    FStringView ClassName, const UClass* ProposedParent, FString& Diagnostic);
bool ValidateBlueprintCarrierForCompilation(FStringView ModuleName, FStringView ClassName,
    const UClass* ProposedParent, FString& Diagnostic);
bool TryGetBlueprintCarrierName(const UClass* Class, FString& OutName);
}
}
                                                                                                 
