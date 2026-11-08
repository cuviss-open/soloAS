// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "Commandlets/Commandlet.h"
#include "SoloASProvidersCommandlet.generated.h"

UCLASS()
class USoloASProvidersCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    USoloASProvidersCommandlet();
    int32 Main(const FString& Params) override;
};
