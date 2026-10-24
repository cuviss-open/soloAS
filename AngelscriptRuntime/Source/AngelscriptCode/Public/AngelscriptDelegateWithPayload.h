#pragma once
#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "AngelscriptDelegateWithPayload.generated.h"

DECLARE_DYNAMIC_DELEGATE(FInternalEmptyDelegate);
DECLARE_DYNAMIC_DELEGATE_OneParam(FInternalEmptyDelegateWithPayload, int, Payload);

USTRUCT(BlueprintType)
struct ANGELSCRIPTCODE_API FAngelscriptDelegateWithPayload
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Delegate")
	FInstancedStruct Payload;
	UPROPERTY(BlueprintReadWrite, Category = "Delegate")
	TWeakObjectPtr<UObject> Object;
	UPROPERTY(BlueprintReadWrite, Category = "Delegate")
	FName FunctionName;

	bool IsBound() const;
	void ExecuteIfBound() const;

	void BindUFunction(UObject* Object, FName FunctionName);
	void BindUFunctionWithPayload(UObject* Object, FName FunctionName, void* PayloadPtr, int PayloadScriptTypeId);

	void Reset();

	static UScriptStruct* GetBoxedPrimitiveStructFromTypeId(int TypeId);
};

USTRUCT(BlueprintType, Meta = (NoAutoAngelscriptBind))
struct FAngelscriptBoxedByte
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Value = 0;
};

USTRUCT(BlueprintType, Meta = (NoAutoAngelscriptBind))
struct FAngelscriptBoxedShort
{
	GENERATED_BODY()

	UPROPERTY()
	uint16 Value = 0;
};

USTRUCT(BlueprintType, Meta = (NoAutoAngelscriptBind))
struct FAngelscriptBoxedInt32
{
	GENERATED_BODY()

	UPROPERTY()
	uint32 Value = 0;
};

USTRUCT(BlueprintType, Meta = (NoAutoAngelscriptBind))
struct FAngelscriptBoxedInt64
{
	GENERATED_BODY()

	UPROPERTY()
	uint64 Value = 0;
};

USTRUCT(BlueprintType, Meta = (NoAutoAngelscriptBind))
struct FAngelscriptBoxedFloat
{
	GENERATED_BODY()

	UPROPERTY()
	float Value = 0;
};

USTRUCT(BlueprintType, Meta = (NoAutoAngelscriptBind))
struct FAngelscriptBoxedDouble
{
	GENERATED_BODY()

	UPROPERTY()
	double Value = 0;
};
