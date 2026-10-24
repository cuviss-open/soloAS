#pragma once

#include "UObject/Object.h"
#include "SoloASBridgeStaticCallContext.generated.h"

UCLASS(Transient)
class USoloASBridgeStaticCallContext final : public UObject
{
    GENERATED_BODY()
public:
    virtual bool IsReadyForFinishDestroy() override;
};
