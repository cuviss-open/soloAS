#pragma once
#include "CoreMinimal.h"
#include "Editor.h"

#include "Engine/DataTable.h"
#include "GameFramework/Volume.h"

#include "BlueprintActionDatabase.h"
#include "Kismet2/EnumEditorUtils.h"

#include "ComponentTypeRegistry.h"

#include "ClassGenerator/AngelscriptClassGenerator.h"
                                                                                                           
#include "ClassGenerator/ASClass.h"
#include "UObject/UObjectIterator.h"
                                                                                                         
                                                                                                              
#include "AngelscriptReloadSafety.h"
#include "InstancedStructReload.h"
                                                                                                            
                                                                                                               
#include "Editor/SoloASBridgeEnumEditor.h"
                                                                                                             

struct FClassReloadHelper
{
	struct FReloadState
	{
		bool bRefreshAllActions = false;
		bool bReloadedVolume = false;

		TMap<UClass*, UClass*> ReloadClasses;
		TMap<UObject*, UObject*> ReloadAssets;
		TSet<UClass*> NewClasses;
		TSet<UEnum*> ReloadEnums;
		TSet<UEnum*> NewEnums;
		TMap<UScriptStruct*, UScriptStruct*> ReloadStructs;
		TMap<UDelegateFunction*, UDelegateFunction*> ReloadDelegates;
		TSet<UDelegateFunction*> NewDelegates;

		void PerformReinstance();
                                                                                                                     
		static void CaptureCustomSerializedReferences(const TArray<UObject*>& PreviousTypes);
		static void ClearCustomSerializedReferences();
                                                                                                                   
	};

	static FReloadState& ReloadState()
	{
		static FReloadState State;
		return State;
	}

	static void Init()
	{
                                                                                                                     
		FAngelscriptClassGenerator::OnPreFullReload.AddStatic(&FReloadState::CaptureCustomSerializedReferences);
                                                                                                                   
		FAngelscriptClassGenerator::OnStructReload.AddLambda(
		[](UScriptStruct* OldStruct, UScriptStruct* NewStruct)
		{
			ReloadState().ReloadStructs.Add(OldStruct, NewStruct);
			ReloadState().bRefreshAllActions = true;
		});

		FAngelscriptClassGenerator::OnClassReload.AddLambda(
		[](UClass* OldClass, UClass* NewClass)
		{
			if (OldClass != nullptr)
				ReloadState().ReloadClasses.Add(OldClass, NewClass);
			else
				ReloadState().NewClasses.Add(NewClass);

			bool bRefreshedAll = false;
			if (ReloadState().bRefreshAllActions)
				bRefreshedAll = true;

			if (OldClass != nullptr)
			{
				if (!bRefreshedAll && GEngine != nullptr)
				{
					auto& Database = FBlueprintActionDatabase::Get();
                                                                                                                   
#if 0
                                                                                                                 
					Database.RefreshClassActions(OldClass);
                                                                                                                  
#endif
                    Database.ClearAssetActions(OldClass);
                                                                                                                
				}
			}

			if (NewClass != nullptr)
			{
				if (!bRefreshedAll && GEngine != nullptr)
				{
					auto& Database = FBlueprintActionDatabase::Get();
					Database.RefreshClassActions(NewClass);
				}

				if (NewClass->IsChildOf(UActorComponent::StaticClass()))
					FComponentTypeRegistry::Get().InvalidateClass(NewClass);

				if (NewClass->IsChildOf(AVolume::StaticClass()))
					ReloadState().bReloadedVolume = true;
			}
		});

		FAngelscriptClassGenerator::OnDelegateReload.AddLambda(
		[](UDelegateFunction* OldDelegate, UDelegateFunction* NewDelegate)
		{
			ReloadState().ReloadDelegates.Add(OldDelegate, NewDelegate);
			ReloadState().NewDelegates.Add(NewDelegate);
		});
		
		FAngelscriptClassGenerator::OnLiteralAssetReload.AddLambda(
		[](UObject* OldObject, UObject* NewObject)
		{
			ReloadState().ReloadAssets.Add(OldObject, NewObject);
		});

		FAngelscriptClassGenerator::OnEnumChanged.AddLambda(
		[](UEnum* Enum, const TArray<TPair<FName, int64>>& OldNames)
		{
			ReloadState().ReloadEnums.Add(Enum);

			// Need to refresh blueprints that depend on this
                                                                                                          
#if 0
                                                                                                        
			FEnumEditorUtils::BroadcastChanges((UUserDefinedEnum*)Enum, OldNames);
                                                                                                      
#endif
			SoloASBridge::RefreshRuntimeEnumEditorDependents(
				*CastChecked<UUserDefinedEnum>(Enum),
				OldNames);
                                                                                                    
		});
		
		FAngelscriptClassGenerator::OnEnumCreated.AddLambda(
		[](UEnum* Enum)
		{
			ReloadState().NewEnums.Add(Enum);
		});

                                                                                                              
        AngelscriptReloadSafety::GetPayloadReloadAdmission().BindStatic(&ValidateInstancedStructReload);
                                                                                                            
		FAngelscriptClassGenerator::OnFullReload.AddLambda(
		[]()
		{
			// Do the actual reinstancing required
			ReloadState().PerformReinstance();
		});

		FAngelscriptClassGenerator::OnPostReload.AddLambda(
		[](bool bFullReload)
		{
                                                                                                                    
			FReloadState::ClearCustomSerializedReferences();
                                                                                                                  
			// Refresh action list in blueprint, this is what
			// is used to populate the right click menu.
			if (ReloadState().bRefreshAllActions && GEngine != nullptr)
			{
				auto& Database = FBlueprintActionDatabase::Get();
				Database.RefreshAll();
			}

                                                                                                           
            if (bFullReload && GEngine != nullptr)
            {
                auto& Database = FBlueprintActionDatabase::Get();
                for (TObjectIterator<UASClass> Class; Class; ++Class)
                    if (Class->HasAnyClassFlags(CLASS_NewerVersionExists)
                        || (Class->ScriptTypePtr == nullptr && Class->HasAnyClassFlags(CLASS_Hidden)))
                        Database.ClearAssetActions(*Class);
            }
                                                                                                         

			// Refresh class lists by pretending we just compiled a bp
			if (bFullReload && GEditor != nullptr)
			{
				GEditor->BroadcastBlueprintCompiled();	
			}

			if (!FAngelscriptManager::bIsInitialCompileFinished)
				FComponentTypeRegistry::Get().Invalidate();

			// If we reloaded any volume classes, trigger a geometry rebuild
			if (ReloadState().bReloadedVolume && GEngine != nullptr)
			{
				auto* World = GEditor->GetEditorWorldContext().World();
				ULevel* CurrentLevel = World->GetCurrentLevel();

				GEngine->Exec( World, TEXT("MAP REBUILD ALLVISIBLE") );

				// Map rebuild ("Build Geometry") is currently bugged (as of 5.4.1) and resets the CurrentLevel.
				// To avoid being annoying and resetting the active level on hotreload, restore it after the rebuild
				if (IsValid(CurrentLevel) && World->GetLevels().Contains(CurrentLevel))
					World->SetCurrentLevel(CurrentLevel);
			}

			// Reset state
			ReloadState() = FReloadState();
		});
	}

	static TArray<UDataTable*> GetTablesDependentOnStruct(UStruct* Struct)
	{
		TArray<UDataTable*> Result;
		if (Struct)
		{
			TArray<UObject*> DataTables;
			GetObjectsOfClass(UDataTable::StaticClass(), DataTables);
			for (UObject* DataTableObj : DataTables)
			{
				UDataTable* DataTable = Cast<UDataTable>(DataTableObj);
				if (DataTable && (Struct == DataTable->RowStruct))
				{
					Result.Add(DataTable);
				}
			}
		}
		return Result;
	}

	static void HandleObjectReinstancing(const TMap<UObject*, UObject*>& ReinstancedObjects);
};
