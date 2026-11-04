// Copyright Cuviss Studio. All Rights Reserved.
#include "Editor/SoloASCopyProperties.h"

#include "Misc/ScopeExit.h"
#include "SoloASEngineCompat.h"
                                                                                                                              
#if SOLOAS_UE_BEFORE(5, 5)
#include "Engine/Blueprint.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "Serialization/ArchiveReplaceOrClearExternalReferences.h"
#include "UObject/UObjectHash.h"
#include "UObject/GarbageCollection.h"
#include "Misc/Optional.h"
#endif
// SOLOAS-COMPAT-END
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
// SOLOAS-COMPAT-BEGIN(<5.5) CS.Bridge.UE54.AsyncCopyArchetypeLifetime [test: SoloAS.AsyncDefaultCallableReopen]
#if SOLOAS_UE_BEFORE(5, 5)
    TOptional<FGCScopeGuard> AsyncGCGuard;
    if (!IsInGameThread()) AsyncGCGuard.Emplace();
    UObject* const NewArchetype = NewObject->GetArchetype();
    TStrongObjectPtr<UObject> NewArchetypeGuard(IsInGameThread() ? NewArchetype : nullptr);
#else
    TStrongObjectPtr<UObject> NewArchetypeGuard(NewObject->GetArchetype());
    UObject* const NewArchetype = NewArchetypeGuard.Get();
#endif
// SOLOAS-COMPAT-END
    TMap<UObject*, UObject*> LocalMappings;
    TMap<UObject*, UObject*>* Mappings = Params.OptionalReplacementMappings;
    bool bInserted = false;
    if (OldArchetype && NewArchetype && OldArchetype != NewArchetype
        && OldArchetype->HasAnyFlags(RF_ClassDefaultObject) != NewArchetype->HasAnyFlags(RF_ClassDefaultObject))
    {
        if (!Mappings) Mappings = &LocalMappings;
        // Engine FindKey(NewArchetype) suppresses its implicit archetype pair.
        // Preserve any preexisting key and any caller-provided target pairing.
        if (!Mappings->Contains(NewArchetype) && !Mappings->FindKey(NewArchetype))
        {
            Mappings->Add(NewArchetype, NewArchetype);
            bInserted = true;
        }
        Params.OptionalReplacementMappings = Mappings;
    }
    ON_SCOPE_EXIT
    {
        if (bInserted && Mappings->FindRef(NewArchetype) == NewArchetype)
            Mappings->Remove(NewArchetype);
    };
// SOLOAS-COMPAT-BEGIN(<5.5) CS.Bridge.UE54.CopyArchetypePolicy [test: SoloAS.CopyPropertiesArchetypePolicy]
#if SOLOAS_UE_BEFORE(5, 5)
    const bool bSuppressArchetype = OldArchetype && NewArchetype && OldArchetype != NewArchetype
        && OldArchetype->HasAnyFlags(RF_ClassDefaultObject) != NewArchetype->HasAnyFlags(RF_ClassDefaultObject);
    if (bSuppressArchetype || (Mappings && Mappings->FindKey(NewArchetype)))
    {
        const bool bClear = Params.bClearReferences;
        const bool bNotify = Params.bNotifyObjectReplacement;
        TMap<UObject*, UObject*> ReplacementMap;
        ReplacementMap.Add(OldObject, NewObject);
        if (OldObject->GetClass() != NewObject->GetClass())
        {
            if (Params.bReplaceObjectClassReferences)
                ReplacementMap.Add(OldObject->GetClass(), NewObject->GetClass());
            ReplacementMap.Add(OldObject->GetClass()->GetDefaultObject(), NewObject->GetClass()->GetDefaultObject());
        }
        if (!Params.OptionalReplacementMappings)
        {
            LocalMappings = ReplacementMap;
            Params.OptionalReplacementMappings = &LocalMappings;
        }
        Params.bClearReferences = false;
        Params.bNotifyObjectReplacement = false;
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
        SoloASBridge::Private::CopyPropertiesForUnrelatedObjectsOriginal(OldObject, NewObject, Params);
#else
        UEngine::CopyPropertiesForUnrelatedObjects(OldObject, NewObject, Params);
#endif
        TArray<UObject*> OldChildren;
        GetObjectsWithOuter(OldObject, OldChildren, true, RF_NoFlags, EInternalObjectFlags::Garbage);
        for (UObject* OldChild : OldChildren)
        {
            if (UObject* const* Mapped = Params.OptionalReplacementMappings->Find(OldChild))
                ReplacementMap.Add(OldChild, *Mapped);
            else
                ReplacementMap.Add(OldChild, nullptr);
        }
        const IConsoleVariable* Aggressive = IConsoleManager::Get().FindConsoleVariable(TEXT("cpfuo.UseAggressiveReferenceReplacment"));
        if (Aggressive && Aggressive->GetBool())
        {
            for (const auto& Pair : *Params.OptionalReplacementMappings)
            {
                if (!Pair.Key || !Pair.Value || !Pair.Key->IsIn(OldObject)) continue;
                if (UClass* Parent = OldObject->GetClass()->GetSuperClass())
                {
                    if (UObject* Default = Parent->GetDefaultSubobjectByName(Pair.Value->GetFName()))
                        ReplacementMap.Add(Default, Pair.Value);
#if WITH_EDITOR
                    if (UBlueprint* Blueprint = Cast<UBlueprint>(Parent->ClassGeneratedBy))
                        if (Blueprint->SkeletonGeneratedClass)
                            if (UObject* Default = Blueprint->SkeletonGeneratedClass->GetDefaultSubobjectByName(Pair.Value->GetFName()))
                                ReplacementMap.Add(Default, Pair.Value);
#endif
                }
            }
        }
        if (bClear)
        {
            TArray<UObject*> Targets;
            Targets.Add(NewObject);
            GetObjectsWithOuter(NewObject, Targets, true);
            for (UObject* Target : Targets)
            {
                if (Target != NewObject && !Target->IsDefaultSubobject() && !Target->HasAnyFlags(RF_DefaultSubObject)
                    && !Params.OptionalReplacementMappings->FindKey(Target)) continue;
                FArchiveReplaceObjectRef<UObject> Optional(Target, *Params.OptionalReplacementMappings);
                FArchiveReplaceOrClearExternalReferences<UObject> Replace(Target, ReplacementMap, NewObject->GetOutermost());
            }
        }
        if (bNotify && GEngine && !OldObject->HasAnyInternalFlags(EInternalObjectFlags::Async | EInternalObjectFlags::AsyncLoading))
            GEngine->NotifyToolsOfObjectReplacement(ReplacementMap);
        return;
    }
#endif
// SOLOAS-COMPAT-END
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    SoloASBridge::Private::CopyPropertiesForUnrelatedObjectsOriginal(OldObject, NewObject, Params);
#else
    UEngine::CopyPropertiesForUnrelatedObjects(OldObject, NewObject, Params);
#endif
}
