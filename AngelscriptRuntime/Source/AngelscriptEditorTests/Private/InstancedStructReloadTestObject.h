// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "Misc/EngineVersionComparison.h"
                                                                                                                                                   
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "InstancedStructContainer.h"
#else
#include "StructUtils/InstancedStructContainer.h"
#endif
// SOLOAS-COMPAT-END
#include "UObject/Object.h"
#include "InstancedStructReloadTestObject.generated.h"

USTRUCT(BlueprintType)
struct FInstancedStructReloadLifetime
{
    GENERATED_BODY()
    UPROPERTY()
    int32 Marker = 17;

    ~FInstancedStructReloadLifetime();
    static int32 DestructionsAt(const void* Address);
};

template<>
struct TStructOpsTypeTraits<FInstancedStructReloadLifetime> : TStructOpsTypeTraitsBase2<FInstancedStructReloadLifetime>
{
    static constexpr EPropertyObjectReferenceType WithSerializerObjectReferences = EPropertyObjectReferenceType::None;
};

USTRUCT()
struct FInstancedStructReloadWrapper
{
    GENERATED_BODY()
    UPROPERTY()
    FInstancedStruct Payload;
};

UCLASS(Blueprintable)
class UInstancedStructReloadTestObject : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category = "Test")
    static void RecordPayloadDestruction(int32 Generation, int32 Value);

    UPROPERTY()
    FInstancedStructContainer Container;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Test")
    FInstancedStruct Reflected;

    FInstancedStruct Hidden;
    bool bCollectorOnly = false;
    bool bExposeARO = true;

    virtual void Serialize(FArchive& Ar) override;
    static void AddReferencedObjects(UObject* Object, FReferenceCollector& Collector);
};
