#pragma once
#include "CoreMinimal.h"
#include "AngelscriptCreateBlueprintOverride.generated.h"

/**
 * Class that can be overridden from script to override how the 'Create Blueprint'
 * option works when clicked in visual studio code.
 */
UCLASS()
class UAngelscriptCreateBlueprintOverride : public UObject
{
	GENERATED_BODY()

public:

	UFUNCTION(BlueprintImplementableEvent)
	bool AppliesToClass(UClass* ParentClass) const;

	/**
	 * Return true to indicate the entire process was handled and the normal blueprint creation should be skipped.
	 */
	UFUNCTION(BlueprintImplementableEvent)
	bool PreCreateBlueprint(UClass* ParentClass) const;

	UFUNCTION(BlueprintImplementableEvent)
	void PostCreateBlueprint(UClass* ParentClass, UObject* CreatedAsset) const;
};