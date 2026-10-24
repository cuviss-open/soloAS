#pragma once

#include "CoreMinimal.h"

#include "AngelscriptManager.h"

#include "ASStruct.generated.h"

UCLASS()
class ANGELSCRIPTCODE_API UASStruct : public UScriptStruct
{
	GENERATED_BODY()
public:
	UASStruct* NewerVersion = nullptr;
	class asITypeInfo* ScriptType = nullptr;
	FGuid Guid;
	bool bIsEditorOnlyStruct = false;

#if WITH_EDITOR
	FString AngelscriptTypename;
#endif

	bool IsNameStableForNetworking() const override { return true; }

                                                                                                                       
#if 0
                                                                                                                     
	UScriptStruct* GetNewestVersion() override
                                                                                                                   
#endif
	UASStruct* ResolveNewestStruct()
                                                                                                                 
	{
#if !AS_CAN_HOTRELOAD
		return this;
#else
		if (NewerVersion == nullptr)
			return this;

		UASStruct* NewerStruct = NewerVersion;
                                                                                                    
		TSet<const UASStruct*> Visited;
		const UASStruct* Current = this;
		while (Current != nullptr)
		{
			if (Visited.Contains(Current))
			{
				ensureMsgf(false, TEXT("Detected a cycle in UASStruct::NewerVersion."));
				return nullptr;
			}

			Visited.Add(Current);
			Current = Current->NewerVersion;
		}
                                                                                                  
		while (NewerStruct->NewerVersion != nullptr)
			NewerStruct = NewerStruct->NewerVersion;
		return NewerStruct;
#endif
	}

                                                                                                                             
	// Stock UE 5.8.3 has no UScriptStruct::GetNewestVersion virtual. Haze adds it
	// for its InstancedStruct migration seam. Omitting
	// `override` keeps this declaration source-compatible with both layouts.
	virtual UScriptStruct* GetNewestVersion()
	{
		return ResolveNewestStruct();
	}
                                                                                                                           
	asIScriptFunction* GetToStringFunction() const;

	FGuid GetCustomGuid() const override
	{
		return Guid;
	}

	void SetGuid(FName FromName);

                                                                                                           
	int32 GetLiveInstanceCount() const;
                                                                                                         

	UASStruct(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	void UpdateScriptType();
                                                                                                    
	void SetAngelscriptCppStructOps(ICppStructOps* InCppStructOps);
                                                                                                  

                                                                                                                         
	void InitializeStruct(void* Dest, int32 ArrayDim = 1) const override;
	void DestroyStruct(void* Dest, int32 ArrayDim = 1) const override;
	uint32 GetStructTypeHash(const void* Src) const override;
                                                                                                                       
	void PrepareCppStructOps() override;
	ICppStructOps* CreateCppStructOps();

	virtual bool IsEditorOnly() const override { return bIsEditorOnlyStruct; }
};

// Wildcard struct used to enable DeterminesOutputType support for structs
USTRUCT(BlueprintType)
struct FScriptStructWildcard
{
	GENERATED_BODY()
};
