#include "Runtime/SoloASBridgeScriptPropertyAliases.h"

#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
struct FScriptPropertyAliasRegistration
{
    const TCHAR* ClassPath;
    const TCHAR* PropertyName;
    const TCHAR* ScriptName;
};

constexpr FScriptPropertyAliasRegistration Registrations[] =
{
    {
        TEXT("/Script/DataprepLibraries.DataprepIsClassOfFetcher"),
        TEXT("Class"),
        TEXT("KeyClass")
    }
};

void ApplyRegistration(const FScriptPropertyAliasRegistration& Registration)
{
    UClass* Class = FindObject<UClass>(nullptr, Registration.ClassPath);
    if (Class == nullptr)
    {
        return;
    }

    FProperty* Property = FindFProperty<FProperty>(
        Class,
        Registration.PropertyName,
        EFieldIterationFlags::None
    );
    if (
        Property == nullptr
        || Property->GetOwnerStruct() != Class
        || Property->GetClass() != FClassProperty::StaticClass()
    )
    {
        return;
    }

#if WITH_METADATA
    const FString* ExistingAlias = Property->FindMetaData(TEXT("ScriptName"));
    if (
        ExistingAlias != nullptr
        && !ExistingAlias->Equals(Registration.ScriptName, ESearchCase::CaseSensitive)
    )
    {
        return;
    }
    if (ExistingAlias == nullptr)
    {
        Property->SetMetaData(TEXT("ScriptName"), Registration.ScriptName);
    }
#endif
}
}

void SoloASBridge::ApplyScriptPropertyAliases()
{
    for (const FScriptPropertyAliasRegistration& Registration : Registrations)
    {
        ApplyRegistration(Registration);
    }
}
