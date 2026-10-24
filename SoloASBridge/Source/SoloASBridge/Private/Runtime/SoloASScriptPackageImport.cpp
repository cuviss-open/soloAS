#include "Runtime/SoloASScriptPackageImport.h"

#include "UObject/Package.h"

namespace SoloASBridge
{
bool PrepareScriptPackageForStockImports(UPackage* ScriptPackage)
{
    if (ScriptPackage == nullptr
        || ScriptPackage->GetOuter() != nullptr
        || ScriptPackage->GetFName() != FName(TEXT("/Script/Angelscript")))
    {
        return false;
    }
    ScriptPackage->SetPackageFlags(PKG_InMemoryOnly);
    return (ScriptPackage->GetPackageFlags() & PKG_InMemoryOnly) == PKG_InMemoryOnly;
}
}
