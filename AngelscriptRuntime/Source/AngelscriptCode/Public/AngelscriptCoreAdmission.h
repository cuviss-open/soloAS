// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
struct FAngelscriptModuleDesc;

namespace AngelscriptCoreExtensionPoints
{
enum class ERemoveResult : uint8 { Removed, Busy, Unknown };
// Single providers. Register/remove on the game thread outside callbacks.
// Views are borrowed for this call only; no script execution, compilation,
// GC, candidate mutation or registration changes are allowed in validators.
                                                                                               
using FLoadedSourceValidator = TFunction<bool(FStringView AbsolutePath,
    FStringView RelativePath, FStringView LoadedText, bool bDeleted, FString& Error)>;
ANGELSCRIPTCODE_API FDelegateHandle RegisterLoadedSourceValidator(FLoadedSourceValidator Validator);
ANGELSCRIPTCODE_API ERemoveResult UnregisterLoadedSourceValidator(FDelegateHandle Handle);
                                                                                             
                                                                                                  
using FModuleAdmissionValidator = TFunction<bool(
    TConstArrayView<TSharedRef<FAngelscriptModuleDesc>> Candidates, FString& Error)>;
ANGELSCRIPTCODE_API FDelegateHandle RegisterModuleAdmission(FModuleAdmissionValidator Validator);
ANGELSCRIPTCODE_API ERemoveResult UnregisterModuleAdmission(FDelegateHandle Handle);
                                                                                                
}
