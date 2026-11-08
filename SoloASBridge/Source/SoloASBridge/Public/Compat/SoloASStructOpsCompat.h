// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "UObject/Class.h"
#include "SoloASEngineCompat.h"
// SOLOAS-COMPAT-BEGIN(<5.5) UE 5.4 has no property visitor interface [test: SoloAS.ScriptStructOperations.CarrierAndLifecycle] [id: CS.Compatibility.Bridge.SoloASStructOpsCompat.PropertyVisitorInclude]
#if SOLOAS_UE_AT_LEAST(5, 5)
#include "UObject/PropertyVisitor.h"
#endif
// SOLOAS-COMPAT-END
namespace SoloASBridge::Compat
{
// SOLOAS-COMPAT-BEGIN(<5.7) UE 5.5/5.6 dispatch struct operations through virtual methods with versioned serializers and visitors [test: SoloAS.ScriptStructOperations.CarrierAndLifecycle] [id: CS.Compatibility.Bridge.SoloASStructOpsCompat.VirtualStructOpsHandlers]
#if SOLOAS_UE_BEFORE(5, 7)
struct FVirtualStructOpsHandlers
{
    const void* Table = nullptr;
    const void* Construct = nullptr;
    const void* Copy = nullptr;
    const void* Destructor = nullptr;
    const void* DeletingDestructor = nullptr;
};
SOLOASBRIDGE_API FVirtualStructOpsHandlers ReadVirtualStructOpsHandlers(const UScriptStruct::ICppStructOps* Ops);
SOLOASBRIDGE_API bool HasCanonicalVectorHandlers(const FVirtualStructOpsHandlers& Handlers);
// Only advertised callbacks may be invoked; absent serializers retain Stock's false result.
struct FScriptStructOps : UScriptStruct::ICppStructOps
{
    using ICppStructOps::ICppStructOps;
#if SOLOAS_UE_BEFORE(5, 6)
    bool Serialize(FArchive&, void*) override { return false; }
    bool Serialize(FStructuredArchive::FSlot, void*) override { return false; }
#else
    bool Serialize(FArchive&, void*, UStruct*, const void*) override { return false; }
    bool Serialize(FStructuredArchive::FSlot, void*, UStruct*, const void*) override { return false; }
#endif
    void PostSerialize(const FArchive&, void*) override {}
    bool NetSerialize(FArchive&, UPackageMap*, bool&, void*) override { return false; }
    bool NetDeltaSerialize(FNetDeltaSerializeInfo&, void*) override { return false; }
    void PostScriptConstruct(void*) override {}
    void GetPreloadDependencies(void*, TArray<UObject*>&) override {}
    bool ExportTextItem(FString&, const void*, const void*, UObject*, int32, UObject*) override { return false; }
    bool ImportTextItem(const TCHAR*&, void*, int32, UObject*, FOutputDevice*) override { return false; }
    bool FindInnerPropertyInstance(FName, const void*, const FProperty*&, const void*&) const override { return false; }
    TPointerToAddStructReferencedObjects AddStructReferencedObjects() override { return nullptr; }
    bool SerializeFromMismatchedTag(const FPropertyTag&, FArchive&, void*) override { return false; }
    bool StructuredSerializeFromMismatchedTag(const FPropertyTag&, FStructuredArchive::FSlot, void*) override { return false; }
#if SOLOAS_UE_AT_LEAST(5, 5)
    void InitializeIntrusiveUnsetOptionalValue(void*) const override { checkNoEntry(); }
    bool IsIntrusiveOptionalValueSet(const void*) const override { checkNoEntry(); return false; }
    void ClearIntrusiveOptionalValue(void*) const override { checkNoEntry(); }
    bool IsIntrusiveOptionalSafeForGC() const override { return false; }
#endif
#if WITH_EDITOR
    bool CanEditChange(const FEditPropertyChain&, const void*) const override { return true; }
#endif
#if SOLOAS_UE_AT_LEAST(5, 5)
#if SOLOAS_UE_BEFORE(5, 6)
    EPropertyVisitorControlFlow Visit(FPropertyVisitorPath&, const FPropertyVisitorData&, const TFunctionRef<EPropertyVisitorControlFlow(const FPropertyVisitorPath&, const FPropertyVisitorData&)>) const override { return EPropertyVisitorControlFlow::StepOver; }
#else
    EPropertyVisitorControlFlow Visit(FPropertyVisitorContext&, const TFunctionRef<EPropertyVisitorControlFlow(const FPropertyVisitorContext&)>) const override { return EPropertyVisitorControlFlow::StepOver; }
#endif
    void* ResolveVisitedPathInfo(void*, const FPropertyVisitorInfo&) const override { return nullptr; }
#endif
};
#else
using FScriptStructOps = UScriptStruct::ICppStructOps;
#endif
// SOLOAS-COMPAT-END
}
