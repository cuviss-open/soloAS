#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
                                                                                                              
#include "Compat/SoloASRuntimeCompat.h"
                                                                                                            
#include "UObjectTickable.generated.h"

UCLASS(Blueprintable)
                                                                                                           
class ANGELSCRIPTCODE_API UObjectTickable : public UObject, public FSoloASTickableGameObject
                                                                                                         
{
	GENERATED_BODY()

public:

	UObjectTickable(const FObjectInitializer& Initializer)
		: Super(Initializer)
// SOLOAS-COMPAT-BEGIN(<5.7) Keep a single constructor body visible to old UHT [test: SoloAS.EngineCompat.ReflectionSurface] | CS.Runtime.VersionCompatibility.TickableConstructor
        , FSoloASTickableGameObject(IsTemplate() ? ETickableTickType::Never : SoloASNewObjectTickType)
    {}
// SOLOAS-COMPAT-END

	UPROPERTY(ScriptReadWrite)
	TObjectPtr<UWorld> World;

	UPROPERTY(EditDefaultsOnly, Category = "Tickable Object")
	bool bTickWhilePaused = false;

	virtual UWorld* GetWorld() const override;

	UFUNCTION(ScriptCallable)
	void SetWorld(UWorld* InWorld);

	UFUNCTION(ScriptCallable)
	void SetWorldContext(UObject* WorldContext);

	UFUNCTION(ScriptCallable)
	void DestroyObject()
	{
		SetTickableTickType(ETickableTickType::Never);
		MarkAsGarbage();
	}

	UFUNCTION(BlueprintImplementableEvent)
	void BP_Tick(float DeltaTime);

                                                                                                                                                                                        
#if SOLOAS_UE_BEFORE(5, 5)
    bool IsAllowedToTick() const override
    {
        return FSoloASTickableGameObject::IsAllowedToTick() && !IsTemplate() && !IsUnreachable() && !HasAnyInternalFlags(EInternalObjectFlags::Garbage);
    }
#endif
// SOLOAS-COMPAT-END
	bool IsTickableWhenPaused() const override { return bTickWhilePaused; }
	UWorld* GetTickableGameObjectWorld() const { return GetWorld(); }
	bool IsTickable() const override { return !HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_BeginDestroyed); }

	void Tick(float DeltaTime) override;
	TStatId GetStatId() const override;
};
