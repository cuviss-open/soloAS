#pragma once
#include "CoreMinimal.h"
#include "UObjectInWorld.generated.h"

UCLASS(Blueprintable)
class ANGELSCRIPTCODE_API UObjectInWorld : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(ScriptReadWrite)
	TObjectPtr<UWorld> World;

	virtual UWorld* GetWorld() const override;

	UFUNCTION(ScriptCallable)
	void SetWorld(UWorld* InWorld);

	UFUNCTION(ScriptCallable)
	void SetWorldContext(UObject* WorldContext);

	UFUNCTION(ScriptCallable)
	void DestroyObject()
	{
		MarkAsGarbage();
	}
};