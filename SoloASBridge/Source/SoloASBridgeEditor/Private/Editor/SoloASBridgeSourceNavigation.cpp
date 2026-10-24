#include "Editor/SoloASBridgeSourceNavigation.h"
#include "Misc/Paths.h"

namespace SoloASBridge
{
namespace
{
FScriptSourceNavigationInterceptor GScriptSourceNavigationInterceptor;
FDelegateHandle GScriptSourceNavigationHandle;
bool GIsDispatchingScriptSourceNavigation = false;
}

FDelegateHandle RegisterScriptSourceNavigationInterceptor(
    FScriptSourceNavigationInterceptor Interceptor)
{
    if (!IsInGameThread() || GIsDispatchingScriptSourceNavigation
        || !Interceptor.IsBound()
        || GScriptSourceNavigationInterceptor.IsBound())
    {
        return FDelegateHandle();
    }

    GScriptSourceNavigationHandle = FDelegateHandle(
        FDelegateHandle::GenerateNewHandle);
    GScriptSourceNavigationInterceptor = MoveTemp(Interceptor);
    return GScriptSourceNavigationHandle;
}

bool UnregisterScriptSourceNavigationInterceptor(FDelegateHandle Handle)
{
    if (!IsInGameThread() || GIsDispatchingScriptSourceNavigation
        || !GScriptSourceNavigationInterceptor.IsBound()
        || !Handle.IsValid()
        || Handle != GScriptSourceNavigationHandle)
    {
        return false;
    }

    GScriptSourceNavigationInterceptor.Unbind();
    GScriptSourceNavigationHandle.Reset();
    return true;
}

bool TryInterceptScriptSourceNavigation(
    const FString& AbsolutePath,
    int32 LineNumber)
{
    if (!IsInGameThread() || AbsolutePath.IsEmpty()
        || FPaths::IsRelative(AbsolutePath) || LineNumber < 1
        || !GScriptSourceNavigationInterceptor.IsBound()
        || GIsDispatchingScriptSourceNavigation)
    {
        return false;
    }

    TGuardValue<bool> Guard(GIsDispatchingScriptSourceNavigation, true);
    return GScriptSourceNavigationInterceptor.Execute(
        AbsolutePath,
        LineNumber);
}
}
