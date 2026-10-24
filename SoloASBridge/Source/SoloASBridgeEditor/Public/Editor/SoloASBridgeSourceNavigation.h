#pragma once

#include "CoreMinimal.h"

namespace SoloASBridge
{
DECLARE_DELEGATE_RetVal_TwoParams(
    bool,
    FScriptSourceNavigationInterceptor,
    const FString&,
    int32);

SOLOASBRIDGEEDITOR_API FDelegateHandle
RegisterScriptSourceNavigationInterceptor(
    FScriptSourceNavigationInterceptor Interceptor);

SOLOASBRIDGEEDITOR_API bool UnregisterScriptSourceNavigationInterceptor(
    FDelegateHandle Handle);

SOLOASBRIDGEEDITOR_API bool TryInterceptScriptSourceNavigation(
    const FString& AbsolutePath,
    int32 LineNumber);
}
