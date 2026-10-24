// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
namespace SoloASBridge
{
// Cache invalidation is limited to the calling thread's editor replacement pass.
class SOLOASBRIDGE_API FEditorFieldPathReloadScope
{
public:
    FEditorFieldPathReloadScope();
    ~FEditorFieldPathReloadScope();
    FEditorFieldPathReloadScope(const FEditorFieldPathReloadScope&) = delete;
    FEditorFieldPathReloadScope& operator=(const FEditorFieldPathReloadScope&) = delete;
};
}
#endif
