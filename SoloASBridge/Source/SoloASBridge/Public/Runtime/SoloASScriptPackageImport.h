#pragma once

#include "CoreMinimal.h"

class UPackage;

namespace SoloASBridge
{
SOLOASBRIDGE_API bool PrepareScriptPackageForStockImports(
    UPackage* ScriptPackage
);
}
