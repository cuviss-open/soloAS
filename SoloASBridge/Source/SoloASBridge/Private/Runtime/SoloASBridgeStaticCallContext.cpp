#include "Runtime/SoloASBridgeStaticCallContext.h"
#include "Runtime/SoloASBridgeCallableDispatch.h"

bool USoloASBridgeStaticCallContext::IsReadyForFinishDestroy()
{
    return !SoloASBridge::HasPendingScriptFinishDestroyObjects() && Super::IsReadyForFinishDestroy();
}
