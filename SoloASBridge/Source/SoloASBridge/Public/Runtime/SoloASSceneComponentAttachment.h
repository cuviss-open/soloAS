#pragma once

#include "CoreMinimal.h"
#include "SoloASEngineCompat.h"

class USceneComponent;

namespace SoloASBridge
{
// Keeps the first reciprocal occurrence and removes retired, stale and duplicate children.
// Does not attach, detach, change transforms or invoke attachment callbacks.
SOLOASBRIDGE_API bool NormalizeSceneComponentChildren(USceneComponent* Parent);
// SOLOAS-COMPAT-BEGIN(<5.8) CS.Compatibility.Bridge.SceneDestroy [test: SoloAS.SceneGcReentrantAttachment] [id: CS.Compatibility.Bridge.SceneDestroy]
#if WITH_EDITOR && SOLOAS_UE_BEFORE(5, 8)
SOLOASBRIDGE_API void RemoveRetiredSceneComponentStaleChildren(USceneComponent* Parent);
#endif
// SOLOAS-COMPAT-END
}
