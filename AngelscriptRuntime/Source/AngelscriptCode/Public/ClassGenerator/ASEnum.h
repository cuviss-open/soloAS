// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/UserDefinedEnum.h"

#include "ASEnum.generated.h"

UCLASS()
class ANGELSCRIPTCODE_API UASEnum : public UUserDefinedEnum
{
	GENERATED_BODY()

public:
	virtual FString GenerateFullEnumName(const TCHAR* InEnumName) const override;
};
