// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "AngelscriptReplicationShadow.generated.h"

class UASClass;

UCLASS(Transient, NotBlueprintable)
class ANGELSCRIPTCODE_API USoloASReplicationLayoutClass final : public UBlueprintGeneratedClass
{
    GENERATED_BODY()
public:
    TObjectPtr<UASClass> SourceClass;
    virtual void Bind() override;
    virtual EStructPropertyLinkFlags GetPropertyLinkFlags(UStruct* Container, FProperty* Property) const override;
    virtual bool IsFunctionImplementedInScript(FName Name) const override;
    virtual UObject* CreateDefaultObject() override;
    virtual void InitPropertiesFromCustomList(uint8* Data, const uint8* Defaults) override;
    virtual void GetLifetimeBlueprintReplicationList(TArray<FLifetimeProperty>& Out) const override;
    virtual void DestroyPersistentUberGraphFrame(UObject* Object, bool bSkipSuperClass = false) const override;
    virtual void Serialize(FArchive& Ar) override;
    virtual void BeginDestroy() override;
    virtual bool Rename(const TCHAR* Name = nullptr, UObject* Outer = nullptr, ERenameFlags Flags = REN_None) override;
    virtual bool IsAsset() const override { return false; }
};

namespace AngelscriptReplicationShadow
{
void Startup();
void Shutdown();
void PrepareObject(UObject* Object);
ANGELSCRIPTCODE_API bool HasContractFailure();
/** The class used to build replication layouts for a pure script carrier. */
ANGELSCRIPTCODE_API UClass* GetLayoutClass(UASClass* Class);
}
