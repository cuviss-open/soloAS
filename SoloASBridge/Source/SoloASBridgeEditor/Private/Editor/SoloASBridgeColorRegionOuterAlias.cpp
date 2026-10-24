#include "Misc/DelayedAutoRegister.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR
namespace
{
void RegisterColorRegionOuterAlias()
{
    UClass* RegionClass = FindObject<UClass>(
        nullptr,
        TEXT("/Script/ColorCorrectRegions.ColorCorrectRegion")
    );
    if (RegionClass == nullptr)
    {
        return;
    }

    FProperty* Property = FindFProperty<FProperty>(
        RegionClass, TEXT("Outer"), EFieldIterationFlags::None
    );
    if (
        Property == nullptr
        || Property->GetOwnerStruct() != RegionClass
        || Property->GetClass() != FFloatProperty::StaticClass()
    )
    {
        return;
    }

    const FString* ExistingAlias = Property->FindMetaData(TEXT("ScriptName"));
    if (
        ExistingAlias != nullptr &&
        !ExistingAlias->Equals(TEXT("OuterColor"), ESearchCase::CaseSensitive)
    )
    {
        return;
    }
    if (ExistingAlias == nullptr)
    {
        Property->SetMetaData(TEXT("ScriptName"), TEXT("OuterColor"));
    }
}

FDelayedAutoRegisterHelper GColorRegionOuterAliasRegistration(
    EDelayedRegisterRunPhase::ObjectSystemReady,
    &RegisterColorRegionOuterAlias
);
}
#endif
