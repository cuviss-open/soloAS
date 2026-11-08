// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "Commandlets/Commandlet.h"
#include "SoloASToolchainCommandlet.generated.h"

UCLASS()
class USoloASToolchainCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    USoloASToolchainCommandlet();
    int32 Main(const FString& Params) override;
};
