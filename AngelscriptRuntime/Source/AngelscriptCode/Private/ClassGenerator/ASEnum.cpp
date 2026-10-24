// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "ClassGenerator/ASEnum.h"

FString UASEnum::GenerateFullEnumName(const TCHAR* InEnumName) const
{
	return UEnum::GenerateFullEnumName(InEnumName);
}
