// Copyright Cuviss Studio. All Rights Reserved.
#include "Editor/SoloASCopyProperties.h"

#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
namespace SoloASBridge::Private
{
void CopyPropertiesForUnrelatedObjectsOriginal(UObject*, UObject*, UEngine::FCopyPropertiesForUnrelatedObjectsParams);
}
#endif

void SoloAS::CopyPropertiesForUnrelatedObjectsWithArchetypePolicy(UObject* OldObject, UObject* NewObject,
    UEngine::FCopyPropertiesForUnrelatedObjectsParams Params)
{
    check(OldObject && NewObject);
    UObject* OldArchetype = OldObject->GetArchetype();
    TStrongObjectPtr<UObject> NewArchetype(NewObject->GetArchetype());
    TMap<UObject*, UObject*> LocalMappings;
    TMap<UObject*, UObject*>* Mappings = Params.OptionalReplacementMappings;
    bool bInserted = false;
    if (OldArchetype && NewArchetype.IsValid() && OldArchetype != NewArchetype.Get()
        && OldArchetype->HasAnyFlags(RF_ClassDefaultObject) != NewArchetype->HasAnyFlags(RF_ClassDefaultObject))
    {
        if (!Mappings) Mappings = &LocalMappings;
        // Engine FindKey(NewArchetype) suppresses its implicit archetype pair.
        // Preserve any preexisting key and any caller-provided target pairing.
        if (!Mappings->Contains(NewArchetype.Get()) && !Mappings->FindKey(NewArchetype.Get()))
        {
            Mappings->Add(NewArchetype.Get(), NewArchetype.Get());
            bInserted = true;
        }
        Params.OptionalReplacementMappings = Mappings;
    }
    ON_SCOPE_EXIT
    {
        if (bInserted && Mappings->FindRef(NewArchetype.Get()) == NewArchetype.Get())
            Mappings->Remove(NewArchetype.Get());
    };
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    SoloASBridge::Private::CopyPropertiesForUnrelatedObjectsOriginal(OldObject, NewObject, Params);
#else
    UEngine::CopyPropertiesForUnrelatedObjects(OldObject, NewObject, Params);
#endif
}
