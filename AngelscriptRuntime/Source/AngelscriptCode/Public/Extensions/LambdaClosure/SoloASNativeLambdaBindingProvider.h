#pragma once

#include "CoreMinimal.h"

namespace SoloASRuntime
{
// Optional AS engine userdata, registered by the native delegate provider.
// The provider's SDK context remains opaque to sibling consumers.
inline constexpr uint64 NativeLambdaBindingKey = 0x4353564E41544C4CULL;
struct FNativeLambdaBindingProvider
{
    bool (*IsNativeType)(const void* Type);
    void (*Bind)(void* Context);
    void (*InvalidateCaptures)();
};

}
