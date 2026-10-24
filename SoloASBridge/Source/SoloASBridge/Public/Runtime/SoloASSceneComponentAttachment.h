#pragma once

#include "CoreMinimal.h"

class USceneComponent;

namespace SoloASBridge
{
// Keeps the first reciprocal occurrence and removes retired, stale and duplicate children.
// Does not attach, detach, change transforms or invoke attachment callbacks.
SOLOASBRIDGE_API bool NormalizeSceneComponentChildren(USceneComponent* Parent);
}
