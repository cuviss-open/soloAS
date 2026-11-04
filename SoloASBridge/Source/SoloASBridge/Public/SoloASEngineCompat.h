// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "Runtime/Launch/Resources/Version.h"

#define SOLOAS_UE_AT_LEAST(Major, Minor) \
	((ENGINE_MAJOR_VERSION > (Major)) || \
	 (ENGINE_MAJOR_VERSION == (Major) && ENGINE_MINOR_VERSION >= (Minor)))

#define SOLOAS_UE_BEFORE(Major, Minor) \
	((ENGINE_MAJOR_VERSION < (Major)) || \
	 (ENGINE_MAJOR_VERSION == (Major) && ENGINE_MINOR_VERSION < (Minor)))
