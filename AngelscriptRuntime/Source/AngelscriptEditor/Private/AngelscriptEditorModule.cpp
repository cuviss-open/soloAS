#include "AngelscriptEditorModule.h"
                                                                                                            
#include "EditorBinary/AngelscriptPythonTypeRetirement.h"
                                                                                                          
#include "ClassReloadHelper.h"
#include "AngelscriptSettings.h"
                                                                                                      
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "EditorBinary/AngelscriptDataTableRowValidationHook.h"
#endif
                                                                                                    
                                                                                                     
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "EditorBinary/AngelscriptNativeBoundaryHook.h"
#endif
                                                                                                   
                                                                                                          
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "EditorBinary/AngelscriptActorConstructionHook.h"
#endif
                                                                                                        
                                                                                        
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "EditorBinary/AngelscriptBpgcSemanticsHooks.h"
#endif
                                                                                      
                                                                                                                  
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "EditorObject/AngelscriptEditorObjectPrecreateImportAdapter.h"
#include "Editor/SoloASEditorObjectImportProperties.h"
#endif
                                                                                                                
                                                                                                
#include "StateTree/AngelscriptStateTreeClassProvider.h"
                                                                                              

#include "AngelscriptCodeModule.h"
#include "AngelscriptManager.h"
#include "AngelscriptBinds/Bind_FGameplayTag.h"
#include "ClassGenerator/ASClass.h"
#include "AngelscriptCreateBlueprintOverride.h"
#include "BlueprintActionFilter.h"
#include "BlueprintEventNodeSpawner.h"
#include "BlueprintGraphModule.h"
#include "EditorMenuExtensions/ScriptEditorMenuExtension.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompilerModule.h"
#include "FileHelpers.h"
#include "SPositiveActionButton.h"

#include "ISettingsModule.h"
#include "HAL/FileManager.h"
#include "Modules/ModuleManager.h"
#include "IDirectoryWatcher.h"
#include "DirectoryWatcherModule.h"
#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Framework/Application/MenuStack.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "LevelEditor.h"
#include "GameplayTagsModule.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/MessageDialog.h"
#include "Curves/CurveFloat.h"
#include "Widgets/Docking/SDockTab.h"
#include "Engine/DataAsset.h"
#include "UObject/UObjectIterator.h"
                                                                                                         
#include "CurveAssetEditorModule.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
                                                                                                       
#include "ToolMenus.h"
                                                                                           
#include "SoloASEditorManagement.h"
                                                                                         
#include "ToolMenuSection.h"

#include "AngelscriptContentBrowserDataSource.h"
#include "IContentBrowserDataModule.h"
#include "ContentBrowserDataSubsystem.h"

#include "Editor.h"
#include "SourceCodeNavigation.h"

#if PLATFORM_WINDOWS
#include "Internationalization/Regex.h"
                                                                                                                  
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
                                                                                                                
#endif

#if PLATFORM_MAC
#include "Mac/MacSystemIncludes.h"
#endif

IMPLEMENT_MODULE(FAngelscriptEditorModule, AngelscriptEditor);

extern void RegisterAngelscriptSourceNavigation();
                                                                                                         
extern void UnregisterAngelscriptSourceNavigation();
                                                                                                       
                                                                                                      
struct FAngelscriptCurveSaveCommand
{
	TWeakPtr<FUICommandList> Commands;
	TSharedPtr<const FUICommandInfo> Command;
	FUIAction OriginalAction;
	FDelegateHandle WrapperExecuteHandle;
};
FDelegateHandle GAngelscriptCurveExtenderHandle;
TArray<FAngelscriptCurveSaveCommand> GAngelscriptCurveSaveCommands;
                                                                                                    

                                                                                                           
FDelegateHandle GScriptEditorEventActionFilterHandle;
                                                                                                         

                                                                                                      
static bool RejectHiddenScriptEventAction(
	FBlueprintActionFilter const&,
	FBlueprintActionInfo& ActionInfo)
{
	const UBlueprintNodeSpawner* Spawner = ActionInfo.NodeSpawner;
	if (Spawner == nullptr || !Spawner->IsA<UBlueprintEventNodeSpawner>())
		return false;

	const UFunction* Function = ActionInfo.GetAssociatedFunction();
	return Function != nullptr
		&& Function->HasMetaData(FName(TEXT("HideEventInBP")));
}
                                                                                                    

void OnScriptFileChanges(const TArray<FFileChangeData>& Changes)
{
	// Ignore file changes before initialization finishes
	if (!FAngelscriptManager::IsInitialized())
		return;

	IFileManager& FileManager = IFileManager::Get();
	for (const FFileChangeData& Change : Changes)
	{
		FString AbsolutePath = FPaths::ConvertRelativePathToFull(Change.Filename);
		FString RelativePath;

		// Scan through all root paths in order, this will mimic module lookup.
		for (const auto& RootPath : FAngelscriptManager::Get().AllRootPaths)
		{
			if (AbsolutePath.StartsWith(RootPath))
			{
				RelativePath = AbsolutePath;
				FPaths::MakePathRelativeTo(RelativePath, *(RootPath + TEXT("/")));
				break;
			}
		}

		if (RelativePath.IsEmpty()) // Could not find a matching root
			continue;

		FAngelscriptManager& AngelscriptManager = FAngelscriptManager::Get();
		AngelscriptManager.LastFileChangeDetectedTime = FPlatformTime::Seconds();

		if (Change.Filename.EndsWith(TEXT(".as")))
		{
                                                                                                                 
			#if WITH_EDITOR && PLATFORM_DESKTOP && !UE_BUILD_SHIPPING
			FAngelscriptManager::OnScriptFileChanged.Broadcast(
				FAngelscriptManager::FFilenamePair{ AbsolutePath, RelativePath },
				Change.Action == FFileChangeData::EFileChangeAction::FCA_Removed);
			#endif
                                                                                                               
			if (Change.Action == FFileChangeData::EFileChangeAction::FCA_Removed)
				AngelscriptManager.FileDeletionsDetectedForReload.AddUnique(FAngelscriptManager::FFilenamePair{ AbsolutePath, RelativePath });
			else
				AngelscriptManager.FileChangesDetectedForReload.AddUnique(FAngelscriptManager::FFilenamePair{ AbsolutePath, RelativePath });
		}
		else
		{
			// Windows unfortunately does not emit notifications for all the files when a folder is deleted or moved,
			// so we have to handle that case ourselves.

			if (Change.Action == FFileChangeData::EFileChangeAction::FCA_Removed)
			{
				// A folder got deleted, find all the script files we've previously loaded in this folder
				FString AbsoluteFolderPath = AbsolutePath / TEXT("");
				for (auto Module : AngelscriptManager.GetActiveModules())
				{
					for (const auto& CodeSection : Module->Code)
					{
						if (CodeSection.AbsoluteFilename.StartsWith(AbsoluteFolderPath))
						{
							AngelscriptManager.FileDeletionsDetectedForReload.AddUnique(FAngelscriptManager::FFilenamePair{
								CodeSection.AbsoluteFilename, CodeSection.RelativeFilename });
						}
					}
				}
			}
			else if (Change.Action == FFileChangeData::EFileChangeAction::FCA_Added && FileManager.DirectoryExists(*AbsolutePath))
			{
				// We added a new folder, find all the scripts in it and mark them to be loaded
				TArray<FAngelscriptManager::FFilenamePair> ContainedScriptFiles;
				FAngelscriptManager::FindScriptFiles(FileManager, RelativePath, AbsolutePath,
					TEXT("*.as"), ContainedScriptFiles, false, false);

				for (auto ScriptFile : ContainedScriptFiles)
					AngelscriptManager.FileChangesDetectedForReload.AddUnique(ScriptFile);
			}
		}
	}
                                                                                                            
	#if WITH_EDITOR && PLATFORM_DESKTOP && !UE_BUILD_SHIPPING
	FAngelscriptManager::OnScriptWatcherBatchCompleted.Broadcast();
	#endif
                                                                                                          
}

void ForceEditorWindowToFront()
{
	TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
	if (!ParentWindow.IsValid())
	{
		TSharedPtr<SDockTab> LevelEditorTab = FModuleManager::Get().GetModuleChecked<FLevelEditorModule>("LevelEditor").GetLevelEditorTab();
		ParentWindow = LevelEditorTab->GetParentWindow();
		check(ParentWindow.IsValid());
	}
	if (ParentWindow.IsValid())
	{
		ParentWindow->HACK_ForceToFront();
	}
}

void OnEngineInitDone()
{
	// Register the content browser data source
	auto* DataSource = NewObject<UAngelscriptContentBrowserDataSource>(GetTransientPackage(), "AngelscriptData", RF_MarkAsRootSet | RF_Transient);
	DataSource->Initialize();

	UContentBrowserDataSubsystem* ContentBrowserData = IContentBrowserDataModule::Get().GetSubsystem();
	ContentBrowserData->ActivateDataSource("AngelscriptData");
}

void OnLiteralAssetSaved(UObject* Object)
{
	if (UCurveFloat* Curve = Cast<UCurveFloat>(Object))
	{
		if (!FAngelscriptManager::Get().HasAnyDebugServerClients())
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Visual Studio Code extension must be running to save a script literal curve")));
			return;
		}

		TArray<FString> NewContent;

		int GraphWidth = 64;
		int GraphHeight = 16;
		
		float MinTime;
		float MaxTime;
		Curve->FloatCurve.GetTimeRange(MinTime, MaxTime);

		float MinValue;
		float MaxValue;
		Curve->FloatCurve.GetValueRange(MinValue, MaxValue);

		if (MaxTime > MinTime && MaxValue > MinValue)
		{
			FString EmptyLine;
			EmptyLine.Reserve(GraphWidth);
			for (int Char = 0; Char < GraphWidth; ++Char)
				EmptyLine.AppendChar(' ');
			for (int Line = 0; Line < GraphHeight; ++Line)
				NewContent.Add(EmptyLine);

			float ColInterval = (MaxTime - MinTime) / (float)(GraphWidth - 1);
			float RowInterval = (MaxValue - MinValue) / (float)(GraphHeight);
			for (int Column = 0; Column < GraphWidth; ++Column)
			{
				float Time = MinTime + (Column * ColInterval);
				float Value = Curve->FloatCurve.Eval(Time);

				int TargetRow = FMath::Clamp(FMath::FloorToInt32((Value - MinValue) / RowInterval), 0, GraphHeight-1);
				float RowBase = TargetRow * RowInterval;
				float PctInRow = FMath::Clamp((Value - RowBase) / RowInterval, 0.f, 1.f);

				TCHAR CharType = L'·';
				if (PctInRow < 0.33f)
					CharType = '.';
				else if (PctInRow > 0.66f)
					CharType = '\'';

				NewContent[GraphHeight - TargetRow - 1][Column] = CharType;
			}

			for (int Line = 0; Line < GraphHeight; ++Line)
			{
				if (Line == 0)
				{
					FString Value = FString::SanitizeFloat(MaxValue);
					if (Value.Len() >= 4)
						NewContent[Line] = Value.Mid(0, 4) + TEXT("|") + NewContent[Line] + TEXT("|");
					else
						NewContent[Line] = Value.RightPad(4) + TEXT("|") + NewContent[Line] + TEXT("|");
				}
				else if (Line == GraphHeight - 1)
				{
					FString Value = FString::SanitizeFloat(MinValue);
					if (Value.Len() >= 4)
						NewContent[Line] = Value.Mid(0, 4) + TEXT("|") + NewContent[Line] + TEXT("|");
					else
						NewContent[Line] = Value.RightPad(4) + TEXT("|") + NewContent[Line] + TEXT("|");
				}
				else
				{
					NewContent[Line] = TEXT("    |") + NewContent[Line] + TEXT("|");
				}
			}

			FString RuleLine = TEXT("    -");
			for (int Char = 0; Char < GraphWidth; ++Char)
				RuleLine.AppendChar('-');
			RuleLine += TEXT("-");

			NewContent.Insert(TEXT("/*"), 0);
			NewContent.Insert(RuleLine, 1);

			FString BottomLegend = TEXT("    ");

			FString MinTimeStr = FString::SanitizeFloat(MinTime);
			if (MinTimeStr.Len() >= 4)
				BottomLegend += MinTimeStr.Mid(0, 4);
			else
				BottomLegend += MinTimeStr.RightPad(4);

			for (int Char = 0; Char < GraphWidth - 6; ++Char)
				BottomLegend.AppendChar(' ');

			FString MaxTimeStr = FString::SanitizeFloat(MaxTime);
			if (MaxTimeStr.Len() >= 4)
				BottomLegend += MaxTimeStr.Mid(0, 4);
			else
				BottomLegend += MaxTimeStr.LeftPad(4);

			NewContent.Add(RuleLine);
			NewContent.Add(BottomLegend);
			NewContent.Add(TEXT("*/"));
		}

		for (const FRichCurveKey& Key : Curve->FloatCurve.Keys)
		{
			if (Key.InterpMode == ERichCurveInterpMode::RCIM_Constant)
			{
				NewContent.Add(FString::Format(TEXT("AddConstantCurveKey({0}, {1});"), {
					FString::SanitizeFloat(Key.Time),
					FString::SanitizeFloat(Key.Value),
				}));
			}
			else if (Key.InterpMode == ERichCurveInterpMode::RCIM_Linear)
			{
				NewContent.Add(FString::Format(TEXT("AddLinearCurveKey({0}, {1});"), {
					FString::SanitizeFloat(Key.Time),
					FString::SanitizeFloat(Key.Value),
				}));
			}
			else if (Key.InterpMode == ERichCurveInterpMode::RCIM_Cubic)
			{
				if (Key.TangentMode == ERichCurveTangentMode::RCTM_Auto)
				{
					NewContent.Add(FString::Format(TEXT("AddAutoCurveKey({0}, {1});"), {
						FString::SanitizeFloat(Key.Time),
						FString::SanitizeFloat(Key.Value),
					}));
				}
				else if (Key.TangentMode == ERichCurveTangentMode::RCTM_SmartAuto)
				{
					NewContent.Add(FString::Format(TEXT("AddSmartAutoCurveKey({0}, {1});"), {
						FString::SanitizeFloat(Key.Time),
						FString::SanitizeFloat(Key.Value),
					}));
				}
				else if (Key.TangentMode == ERichCurveTangentMode::RCTM_Break || Key.TangentMode == ERichCurveTangentMode::RCTM_User)
				{
					if (Key.TangentWeightMode == ERichCurveTangentWeightMode::RCTWM_WeightedNone)
					{
						if (Key.TangentMode == ERichCurveTangentMode::RCTM_Break)
						{
							NewContent.Add(FString::Format(TEXT("AddCurveKeyBrokenTangent({0}, {1}, {2}, {3});"), {
								FString::SanitizeFloat(Key.Time),
								FString::SanitizeFloat(Key.Value),
								FString::SanitizeFloat(Key.ArriveTangent),
								FString::SanitizeFloat(Key.LeaveTangent),
							}));
						}
						else
						{
							NewContent.Add(FString::Format(TEXT("AddCurveKeyTangent({0}, {1}, {2});"), {
								FString::SanitizeFloat(Key.Time),
								FString::SanitizeFloat(Key.Value),
								FString::SanitizeFloat(Key.ArriveTangent),
							}));
						}
					}
					else
					{
						FString FunctionName;
						if (Key.TangentWeightMode == ERichCurveTangentWeightMode::RCTWM_WeightedArrive)
							FunctionName = TEXT("AddCurveKeyWeightedArriveTangent");
						else if (Key.TangentWeightMode == ERichCurveTangentWeightMode::RCTWM_WeightedLeave)
							FunctionName = TEXT("AddCurveKeyWeightedLeaveTangent");
						else if (Key.TangentWeightMode == ERichCurveTangentWeightMode::RCTWM_WeightedBoth)
							FunctionName = TEXT("AddCurveKeyWeightedBothTangent");

						FString BrokenBool = TEXT("false");
						if (Key.TangentMode == ERichCurveTangentMode::RCTM_Break)
							BrokenBool = TEXT("true");

						NewContent.Add(FString::Format(TEXT("{0}({1}, {2}, {3}, {4}, {5}, {6}, {7});"), {
							FunctionName,
							FString::SanitizeFloat(Key.Time),
							FString::SanitizeFloat(Key.Value),
							BrokenBool,
							FString::SanitizeFloat(Key.ArriveTangent),
							FString::SanitizeFloat(Key.LeaveTangent),
							FString::SanitizeFloat(Key.ArriveTangentWeight),
							FString::SanitizeFloat(Key.LeaveTangentWeight),
						}));
					}
				}
			}
		}

		if (Curve->FloatCurve.DefaultValue != MAX_flt)
			NewContent.Add(FString::Format(TEXT("DefaultValue = {0};"), {FString::SanitizeFloat(Curve->FloatCurve.DefaultValue)}));

		switch (Curve->FloatCurve.PreInfinityExtrap.GetValue())
		{
		case ERichCurveExtrapolation::RCCE_Cycle: NewContent.Add(TEXT("PreInfinityExtrap = ERichCurveExtrapolation::RCCE_Cycle;")); break;
		case ERichCurveExtrapolation::RCCE_CycleWithOffset: NewContent.Add(TEXT("PreInfinityExtrap = ERichCurveExtrapolation::RCCE_CycleWithOffset;")); break;
		case ERichCurveExtrapolation::RCCE_Oscillate: NewContent.Add(TEXT("PreInfinityExtrap = ERichCurveExtrapolation::RCCE_Oscillate;")); break;
		case ERichCurveExtrapolation::RCCE_Linear: NewContent.Add(TEXT("PreInfinityExtrap = ERichCurveExtrapolation::RCCE_Linear;")); break;
		case ERichCurveExtrapolation::RCCE_None: NewContent.Add(TEXT("PreInfinityExtrap = ERichCurveExtrapolation::RCCE_None;")); break;
		}

		switch (Curve->FloatCurve.PostInfinityExtrap.GetValue())
		{
		case ERichCurveExtrapolation::RCCE_Cycle: NewContent.Add(TEXT("PostInfinityExtrap = ERichCurveExtrapolation::RCCE_Cycle;")); break;
		case ERichCurveExtrapolation::RCCE_CycleWithOffset: NewContent.Add(TEXT("PostInfinityExtrap = ERichCurveExtrapolation::RCCE_CycleWithOffset;")); break;
		case ERichCurveExtrapolation::RCCE_Oscillate: NewContent.Add(TEXT("PostInfinityExtrap = ERichCurveExtrapolation::RCCE_Oscillate;")); break;
		case ERichCurveExtrapolation::RCCE_Linear: NewContent.Add(TEXT("PostInfinityExtrap = ERichCurveExtrapolation::RCCE_Linear;")); break;
		case ERichCurveExtrapolation::RCCE_None: NewContent.Add(TEXT("PostInfinityExtrap = ERichCurveExtrapolation::RCCE_None;")); break;
		}

		FAngelscriptManager::Get().ReplaceScriptAssetContent(Curve->GetName(), NewContent);
	}
	else
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Cannot save asset declared as an angelscript asset literal")));
	}
}


void FAngelscriptEditorModule::StartupModule()
{
                                                                                                             
	{
		FBlueprintGraphModule& BlueprintGraph =
			FModuleManager::LoadModuleChecked<FBlueprintGraphModule>(TEXT("BlueprintGraph"));
		FBlueprintGraphModule::FActionMenuRejectionTest Filter =
			FBlueprintGraphModule::FActionMenuRejectionTest::CreateStatic(
				&RejectHiddenScriptEventAction);
		GScriptEditorEventActionFilterHandle = Filter.GetHandle();
		BlueprintGraph.GetExtendedActionMenuFilters().Add(Filter);
	}
                                                                                                           
                                                                                                      
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
	AngelscriptDataTableRowValidationHook::Initialize();
#endif
                                                                                                    
                                                                                                     
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
	AngelscriptNativeBoundaryHook::Initialize();
#endif
                                                                                                   
                                                                                                          
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    AngelscriptActorConstructionHook::Initialize();
#endif
                                                                                                        
                                                                                        
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
	AngelscriptBpgcSemanticsHooks::Initialize();
#endif
                                                                                      
                                                                                                                  
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
	SoloASBridge::SetEditorObjectImportPropertiesAdapter(
		&FAngelscriptEditorObjectPrecreateImportAdapter::ShouldUseImportWholeText,
		&FAngelscriptEditorObjectPrecreateImportAdapter::ImportWholeText);
#endif
                                                                                                                
                                                                                                
	FAngelscriptStateTreeClassProvider::Initialize();
                                                                                              
	FClassReloadHelper::Init();
                                                                                                            
    AngelscriptPythonTypeRetirement::Initialize();
                                                                                                          
	RegisterAngelscriptSourceNavigation();

	if (FAngelscriptManager::bIsInitialCompileFinished)
		FComponentTypeRegistry::Get().Invalidate();

	IGameplayTagsModule::OnTagSettingsChanged.AddRaw(this, &FAngelscriptEditorModule::ReloadTags);
	IGameplayTagsModule::OnGameplayTagTreeChanged.AddRaw(this, &FAngelscriptEditorModule::ReloadTags);
	FCoreDelegates::GetOnPostEngineInit().AddStatic(&OnEngineInitDone);

	UScriptEditorMenuExtension::InitializeExtensions();

	// Register a directory watch on the script directory so we know when to reload
	FDirectoryWatcherModule& DirectoryWatcherModule = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>("DirectoryWatcher");
	IDirectoryWatcher* DirectoryWatcher = DirectoryWatcherModule.Get();

	if (ensure(DirectoryWatcher != nullptr))
	{
		TArray<FString> AllRootPaths = FAngelscriptManager::MakeAllScriptRoots();
		for (const auto& RootPath : AllRootPaths)
		{
			FDelegateHandle WatchHandle;
			DirectoryWatcher->RegisterDirectoryChangedCallback_Handle(
				*RootPath,
				IDirectoryWatcher::FDirectoryChanged::CreateStatic(&OnScriptFileChanges),
				WatchHandle,
				IDirectoryWatcher::IncludeDirectoryChanges);
		}
	}

	// Register the angelscript settings that can be edited in project settings
	ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings");
	if (SettingsModule)
	{
		SettingsModule->RegisterSettings(
			"Project", "Plugins", "Angelscript", 
			NSLOCTEXT("Angelscript", "AngelscriptSettingsTitle", "Angelscript"),
			NSLOCTEXT("Angelscript", "AngelscriptSettingsDescription", "Configuration for behavior of the angelscript compiler and script engine."),
			GetMutableDefault<UAngelscriptSettings>()
		);
	}

	// Helper to pop open the content browser or asset editor from the debug server
	FAngelscriptCodeModule::GetDebugListAssets().AddLambda(
		[](TArray<FString> AssetPaths, UASClass* BaseClass)
		{
			FAngelscriptEditorModule::ShowAssetListPopup(AssetPaths, BaseClass);
		}
	);

	// Helper to create a new blueprint from a script class
	FAngelscriptCodeModule::GetEditorCreateBlueprint().AddLambda(
		[](UASClass* ScriptClass)
		{
			FAngelscriptEditorModule::ShowCreateBlueprintPopup(ScriptClass);
		}
	);

	// Handle the editor trying to save angelscript literal assets
                                                                                                
#if 0
                                                                                              
	extern UNREALED_API TFunction<void(UObject*)> GAssetEditorToolkit_PreSaveObject;
	GAssetEditorToolkit_PreSaveObject = [](UObject* Object)
	{
		if (!FAngelscriptManager::IsInitialized())
			return;
		if (!GIsEditor)
			return;
		if (Object != nullptr && Object->GetOutermost() == FAngelscriptManager::Get().AssetsPackage)
			OnLiteralAssetSaved(Object);
	};
                                                                                                        
#endif
	FAssetEditorExtender CurveExtender = FAssetEditorExtender::CreateLambda(
		[](const TSharedRef<FUICommandList> Commands,
		   const TArray<UObject*> Objects)
		{
			if (!FAngelscriptManager::IsInitialized() || !GIsEditor)
				return MakeShared<FExtender>();
			if (Objects.Num() != 1)
				return MakeShared<FExtender>();
			UObject* Object = Objects[0];
			if (Object == nullptr
				|| Object->GetOuter() != FAngelscriptManager::Get().AssetsPackage
				|| !Object->IsAsset())
				return MakeShared<FExtender>();
			const TSharedPtr<FUICommandInfo> SaveCommand =
				FInputBindingManager::Get().FindCommandInContext(
					TEXT("AssetEditor"), TEXT("SaveAsset"));
			const FUIAction* Original = SaveCommand.IsValid()
				? Commands->GetActionForCommand(SaveCommand) : nullptr;
			if (Original == nullptr || !Original->ExecuteAction.IsBound())
				return MakeShared<FExtender>();
			const bool bAlreadyWrapped = GAngelscriptCurveSaveCommands.ContainsByPredicate(
				[&Commands](const FAngelscriptCurveSaveCommand& Record)
				{ return Record.Commands.Pin() == Commands; });
			if (bAlreadyWrapped)
				return MakeShared<FExtender>();
			FUIAction Wrapped = *Original;
			const FExecuteAction OriginalExecute = Original->ExecuteAction;
			const FName AssetName = Object->GetFName();
			Wrapped.ExecuteAction = FExecuteAction::CreateLambda(
				[AssetName, OriginalExecute]()
			{
				if (FAngelscriptManager::IsInitialized() && GIsEditor)
				{
					UObject* Object = FindObject<UObject>(
						FAngelscriptManager::Get().AssetsPackage,
						*AssetName.ToString());
					if (Object != nullptr
						&& Object->GetOuter() == FAngelscriptManager::Get().AssetsPackage
						&& Object->IsAsset())
						OnLiteralAssetSaved(Object);
				}
				OriginalExecute.ExecuteIfBound();
			});
			GAngelscriptCurveSaveCommands.Add(
				{Commands, SaveCommand, *Original, Wrapped.ExecuteAction.GetHandle()});
			Commands->MapAction(SaveCommand, Wrapped);
			return MakeShared<FExtender>();
		});
	GAngelscriptCurveExtenderHandle = CurveExtender.GetHandle();
	FModuleManager::LoadModuleChecked<FCurveAssetEditorModule>("CurveAssetEditor")
		.GetMenuExtensibilityManager()->GetExtenderDelegates().Add(
			MoveTemp(CurveExtender));
                                                                                                      

                                                                                           
	SoloASEditorManagement::Initialize();
                                                                                         
	// Register a callback that notifies us when the editor is ready for us to register UI extensions
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAngelscriptEditorModule::RegisterToolsMenuEntries));
}

void FAngelscriptEditorModule::ShowCreateBlueprintPopup(UASClass* Class)
{
	// Check if we have any overrides
	for (TObjectIterator<UASClass> ScriptClassIterator; ScriptClassIterator; ++ScriptClassIterator)
	{
		UASClass* ScriptClass = *ScriptClassIterator;
		if (!ScriptClass->IsChildOf(UAngelscriptCreateBlueprintOverride::StaticClass()))
			continue;

		UAngelscriptCreateBlueprintOverride* OverrideCDO = Cast<UAngelscriptCreateBlueprintOverride>(ScriptClass->GetDefaultObject());
		if (OverrideCDO != nullptr && OverrideCDO->AppliesToClass(Class))
		{
			if (OverrideCDO->PreCreateBlueprint(Class))
				return;
		}
	}

	// Only allow creating a blueprint of blueprintable types
	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(Class))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
			FString::Format(
				TEXT("Error: Not allowed to create blueprint for class {0}"),
				{ Class->GetName(), }
		)));
		return;
	}

	const bool bIsDataAsset = Class->IsChildOf<UDataAsset>();

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	ForceEditorWindowToFront();

	FString Title;
	if (bIsDataAsset)
		Title = FString::Printf(TEXT("Create Asset of %s%s"), Class->GetPrefixCPP(), *Class->GetName());
	else
		Title = FString::Printf(TEXT("Create Blueprint of %s%s"), Class->GetPrefixCPP(), *Class->GetName());

	FString AssetPath;
	if (FAngelscriptCodeModule::GetEditorGetCreateBlueprintDefaultAssetPath().IsBound())
		AssetPath = FAngelscriptCodeModule::GetEditorGetCreateBlueprintDefaultAssetPath().Execute(Class);

	// If we don't have a name, try a standard name
	if (AssetPath.Len() == 0)
	{
		if (bIsDataAsset)
			AssetPath = FString::Printf(TEXT("DA_%s"), *Class->GetName());
		else
			AssetPath = FString::Printf(TEXT("BP_%s"), *Class->GetName());
	}

	// Try to find a folder to place it in if we haven't specified one
	if (!AssetPath.StartsWith(TEXT("/")))
	{
		FString ScriptRelativePath = Class->GetRelativeSourceFilePath();

		TArray<FString> Subfolders;
		ScriptRelativePath.ParseIntoArray(Subfolders, TEXT("/"), true);
		
		FString InitialDirectory = TEXT("/Game");
		FString DiskDirectory = FPaths::ProjectContentDir();
		for (int i = Subfolders.Num() - 2; i >= 0; --i)
		{
			FString TestDirectory = InitialDirectory;
			FString TestDiskPath = DiskDirectory;
			for (int n = 0; n <= i; ++n)
			{
				TestDirectory = TestDirectory / Subfolders[n];
				TestDiskPath = TestDiskPath / Subfolders[n];
			}

			if (AssetRegistry.HasAssets(*TestDirectory, true) || IFileManager::Get().DirectoryExists(*TestDiskPath))
			{
				InitialDirectory = TestDirectory;
				break;
			}
		}

		AssetPath = InitialDirectory / AssetPath;
	}

	FSaveAssetDialogConfig SaveAssetDialogConfig;
	{
		SaveAssetDialogConfig.DefaultPath = FPaths::GetPath(AssetPath);
		SaveAssetDialogConfig.DefaultAssetName = FPaths::GetCleanFilename(AssetPath);
		SaveAssetDialogConfig.AssetClassNames.Add(Class->GetClassPathName());
		SaveAssetDialogConfig.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::Disallow;
		SaveAssetDialogConfig.DialogTitleOverride = FText::FromString(Title);
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	FString SaveObjectPath = ContentBrowserModule.Get().CreateModalSaveAssetDialog(SaveAssetDialogConfig);
	
	if (!SaveObjectPath.IsEmpty())
	{
		// Get the full name of where we want to create the physics asset.
		const FString UserPackageName = FPackageName::ObjectPathToPackageName(SaveObjectPath);
		FName AssetName(*FPackageName::GetLongPackageAssetName(UserPackageName));

		// Check if the user inputed a valid asset name, if they did not, give it the generated default name
		if (AssetName == NAME_None)
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Error: Invalid name for new asset.")));
			return;
		}

		// Create a new package for the asset
		UPackage* Package = CreatePackage(*UserPackageName);
		UObject* Asset = nullptr;
		check(Package);

		if (bIsDataAsset)
		{
			Asset = NewObject<UDataAsset>(Package, Class, AssetName, RF_Public | RF_Transactional | RF_Standalone);
		}
		else
		{
			UClass* BlueprintClass = nullptr;
			UClass* BlueprintGeneratedClass = nullptr;

			IKismetCompilerInterface& KismetCompilerModule = FModuleManager::LoadModuleChecked<IKismetCompilerInterface>("KismetCompiler");
			KismetCompilerModule.GetBlueprintTypesForClass(Class, BlueprintClass, BlueprintGeneratedClass);

			// Create and init a new Blueprint
			Asset = FKismetEditorUtilities::CreateBlueprint(
				Class, Package, AssetName, BPTYPE_Normal,
				BlueprintClass, BlueprintGeneratedClass, FName("AngelscriptCreateBlueprint")
			);
		}

		if (Asset)
		{
			// Notify the asset registry
			FAssetRegistryModule::AssetCreated(Asset);
		}

		// Mark the package dirty...
		Package->MarkPackageDirty();

		TArray<UPackage*> Packages;
		Packages.Add(Package);

		FEditorFileUtils::FPromptForCheckoutAndSaveParams Params;
		FEditorFileUtils::PromptForCheckoutAndSave(Packages, Params);

		// Open the blueprint editor for the new asset
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);

		// Check if we have any overrides
		for (TObjectIterator<UASClass> ScriptClassIterator; ScriptClassIterator; ++ScriptClassIterator)
		{
			UASClass* ScriptClass = *ScriptClassIterator;
			if (!ScriptClass->IsChildOf(UAngelscriptCreateBlueprintOverride::StaticClass()))
				continue;

			UAngelscriptCreateBlueprintOverride* OverrideCDO = Cast<UAngelscriptCreateBlueprintOverride>(ScriptClass->GetDefaultObject());
			if (OverrideCDO != nullptr && OverrideCDO->AppliesToClass(Class))
			{
				OverrideCDO->PostCreateBlueprint(Class, Asset);
			}
		}
	}
}

void FAngelscriptEditorModule::OpenVsCode(FString Parameters)
{
	FString Params = Parameters;

	const FString VSCodeWorkspacePath = FAngelscriptManager::ConfigSettings->VSCodeWorkspacePath;
	if (!VSCodeWorkspacePath.IsEmpty())
	{
		Params = FString::Printf(TEXT("\"%s\" %s"), *(FPaths::ProjectDir() / VSCodeWorkspacePath), *Params);
	}
	else if (FAngelscriptManager::ConfigSettings->bOpenFolderOnVSCodeSourceLinks)
	{
		const FString ScriptPath = FAngelscriptManager::Get().GetScriptRootDirectory();
		Params = FString::Printf(TEXT("\"%s\" %s"), *ScriptPath, *Params);
	}

#if PLATFORM_WINDOWS || PLATFORM_MAC
	static bool bSearchedForVsCode = false;
	static FString VsCodeLocation;

	if (!bSearchedForVsCode)
	{
		bSearchedForVsCode = true;
		SearchForVsCode(VsCodeLocation);
	}
	
	if (!VsCodeLocation.IsEmpty())
	{
		uint32 ProcessID;
		FPlatformProcess::CreateProc(*VsCodeLocation, *Params, true, false, false, &ProcessID, 0, nullptr, nullptr, nullptr);
	}
#else
	FPlatformMisc::OsExecute(nullptr, TEXT("code"), *Params);
#endif
}

#if PLATFORM_WINDOWS
void FAngelscriptEditorModule::SearchForVsCode(FString& VsCodeLocation)
{
	FString IDEPath;
	if (!FWindowsPlatformMisc::QueryRegKey(HKEY_CURRENT_USER, TEXT("SOFTWARE\\Classes\\Applications\\Code.exe\\shell\\open\\command\\"), TEXT(""), IDEPath))
		FWindowsPlatformMisc::QueryRegKey(HKEY_LOCAL_MACHINE, TEXT("SOFTWARE\\Classes\\Applications\\Code.exe\\shell\\open\\command\\"), TEXT(""), IDEPath);

	FString PatternString(TEXT("\"(.*)\" \".*\""));
	FRegexPattern Pattern(PatternString);
	FRegexMatcher Matcher(Pattern, IDEPath);
	if (Matcher.FindNext())
	{
		FString URL = Matcher.GetCaptureGroup(1);
		if (FPaths::FileExists(URL))
		{
			VsCodeLocation = URL;
		}
	}
}
#elif PLATFORM_MAC
void FAngelscriptEditorModule::SearchForVsCode(FString& VsCodeLocation)
{
	NSURL* AppURL = [[NSWorkspace sharedWorkspace] URLForApplicationWithBundleIdentifier:@"com.microsoft.VSCode"];
	if (AppURL != nullptr)
	{
		VsCodeLocation = FString([AppURL path]);
	}
}
#endif

void FAngelscriptEditorModule::ShowAssetListPopup(const TArray<FString>& AssetPaths, UASClass* BaseClass)
{
	// Ignore open blueprint messages until everything is initialized
	if (!FAngelscriptManager::IsInitialized())
		return;
	if (!FAngelscriptManager::Get().bIsInitialCompileFinished)
		return;

	ForceEditorWindowToFront();

	if (AssetPaths.Num() == 1)
	{
		// Show a progress bar after a short while, we might need to wait for assets to load
		FScopedSlowTask ProgressBar(2.f, FText::FromString(FString::Printf(TEXT("Opening %s"), *AssetPaths[0])));
		ProgressBar.EnterProgressFrame(0.5f);
		ProgressBar.MakeDialog(false, true);

		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(AssetPaths[0]);
	}
	else
	{
		TArray<FAssetData> AssetData;

		FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		FAssetPickerConfig AssetPickerConfig;
		AssetPickerConfig.OnAssetDoubleClicked = FOnAssetSelected::CreateLambda([](const FAssetData& AssetData)
		{
			if (UObject* ObjectToEdit = AssetData.GetAsset())
			{
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(ObjectToEdit);
			}
		});

		AssetPickerConfig.OnAssetEnterPressed = FOnAssetEnterPressed::CreateLambda([](const TArray<FAssetData>& SelectedAssets)
		{
			for (auto AssetIt = SelectedAssets.CreateConstIterator(); AssetIt; ++AssetIt)
			{
				if (UObject* ObjectToEdit = AssetIt->GetAsset())
				{
					GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(ObjectToEdit);
				}
			}
		});

		AssetPickerConfig.InitialAssetViewType = EAssetViewType::List;
		AssetPickerConfig.bAllowNullSelection = false;
		AssetPickerConfig.bShowBottomToolbar = true;
		AssetPickerConfig.bAutohideSearchBar = false;
		AssetPickerConfig.bCanShowClasses = false;
		AssetPickerConfig.bAddFilterUI = true;
		AssetPickerConfig.SaveSettingsName = TEXT("GlobalAssetPicker");

		for (const FString& Path : AssetPaths)
		{
			AssetPickerConfig.Filter.PackageNames.Add(*Path);
		}

		const FVector2D AssetPickerSize(600.0f, 586.0f);

		// Create the contents of the popup
		auto ActualWidget = SNew(SBox)
		.WidthOverride(AssetPickerSize.X)
		.HeightOverride(AssetPickerSize.Y)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				ContentBrowserModule.Get().CreateAssetPicker(AssetPickerConfig)
			]
			+ SVerticalBox::Slot()
			.HAlign(EHorizontalAlignment::HAlign_Center)
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(SBox)
					.HeightOverride(34.f)
					[
						SNew(SPositiveActionButton)
							.Visibility_Lambda([BaseClass]() {
								return BaseClass != nullptr ? EVisibility::Visible : EVisibility::Collapsed;
							})
							.Text(
								BaseClass != nullptr && BaseClass->IsChildOf(UDataAsset::StaticClass())
									? FText::FromString("Create New Data Asset")
									: FText::FromString("Create New Blueprint")
							)
							.OnClicked_Lambda(
								[BaseClass]()
								{
									if (BaseClass != nullptr)
										FAngelscriptEditorModule::ShowCreateBlueprintPopup(BaseClass);
									return FReply::Handled();
								}
							)
					]
				]
			]
		];

		// Wrap the picker widget in a multibox-style menu body
		FMenuBuilder MenuBuilder(/*BShouldCloseAfterSelection=*/ false, /*CommandList=*/ nullptr);
		MenuBuilder.BeginSection("AssetPickerOpenAsset", NSLOCTEXT("GlobalAssetPicker", "WindowTitle", "Open Asset"));
		MenuBuilder.AddWidget(ActualWidget, FText::GetEmpty(), /*bNoIndent=*/ true);
		MenuBuilder.EndSection();

		// Determine where the pop-up should open
		TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		FVector2D WindowPosition = FSlateApplication::Get().GetCursorPos();
		if (!ParentWindow.IsValid())
		{
			TSharedPtr<SDockTab> LevelEditorTab = FModuleManager::Get().GetModuleChecked<FLevelEditorModule>("LevelEditor").GetLevelEditorTab();
			ParentWindow = LevelEditorTab->GetParentWindow();
			check(ParentWindow.IsValid());
		}

		if (ParentWindow.IsValid())
		{
			FSlateRect ParentMonitorRect = ParentWindow->GetFullScreenInfo();
			const FVector2D MonitorCenter((ParentMonitorRect.Right + ParentMonitorRect.Left) * 0.5f, (ParentMonitorRect.Top + ParentMonitorRect.Bottom) * 0.5f);
			WindowPosition = MonitorCenter - AssetPickerSize * 0.5f;

			// Open the pop-up
			FPopupTransitionEffect TransitionEffect(FPopupTransitionEffect::None);
			FSlateApplication::Get().PushMenu(ParentWindow.ToSharedRef(), FWidgetPath(), MenuBuilder.MakeWidget(), WindowPosition, TransitionEffect, /*bFocusImmediately=*/ true);
		}
	}
}

void FAngelscriptEditorModule::ShutdownModule()
{
                                                                                                             
    AngelscriptPythonTypeRetirement::Shutdown();
                                                                                                           
                                                                                                              
	if (FBlueprintGraphModule* BlueprintGraph =
		FModuleManager::GetModulePtr<FBlueprintGraphModule>(TEXT("BlueprintGraph")))
	{
		const FDelegateHandle FilterHandle = GScriptEditorEventActionFilterHandle;
		BlueprintGraph->GetExtendedActionMenuFilters().RemoveAll(
			[FilterHandle](const FBlueprintGraphModule::FActionMenuRejectionTest& Filter)
			{
				return Filter.GetHandle() == FilterHandle;
			});
	}
	GScriptEditorEventActionFilterHandle.Reset();
                                                                                                            
                                                                                                       
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
	AngelscriptDataTableRowValidationHook::Shutdown();
#endif
                                                                                                     
                                                                                                      
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
	AngelscriptNativeBoundaryHook::Shutdown();
#endif
                                                                                                    
                                                                                                           
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
    AngelscriptActorConstructionHook::Shutdown();
#endif
                                                                                                         
                                                                                         
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
	AngelscriptBpgcSemanticsHooks::Shutdown();
#endif
                                                                                       
                                                                                                                   
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
	SoloASBridge::SetEditorObjectImportPropertiesAdapter(nullptr, nullptr);
#endif
                                                                                                                 
                                                                                                 
	FAngelscriptStateTreeClassProvider::Shutdown();
                                                                                               
                                                                                                               
#if WITH_EDITOR && PLATFORM_DESKTOP && !UE_BUILD_SHIPPING
	if (FAngelscriptManager::IsInitialized())
		FAngelscriptManager::OnScriptEditorShutdown.Broadcast();
#endif
                                                                                                             
                                                                                                      
	UnregisterAngelscriptSourceNavigation();
                                                                                                    
                                                                                                         
	for (const FAngelscriptCurveSaveCommand& Record :
		 GAngelscriptCurveSaveCommands)
	{
		if (TSharedPtr<FUICommandList> Commands = Record.Commands.Pin())
		{
			const FUIAction* Current = Commands->GetActionForCommand(Record.Command);
			if (Current != nullptr
				&& Current->ExecuteAction.GetHandle() == Record.WrapperExecuteHandle)
				Commands->MapAction(Record.Command, Record.OriginalAction);
		}
	}
	GAngelscriptCurveSaveCommands.Reset();
	if (FCurveAssetEditorModule* CurveModule =
		FModuleManager::GetModulePtr<FCurveAssetEditorModule>("CurveAssetEditor"))
	{
		CurveModule->GetMenuExtensibilityManager()->GetExtenderDelegates().RemoveAll(
			[](const FAssetEditorExtender& Extender)
			{ return Extender.GetHandle() == GAngelscriptCurveExtenderHandle; });
	}
	GAngelscriptCurveExtenderHandle.Reset();
                                                                                                       
                                                                                            
	SoloASEditorManagement::Shutdown();
                                                                                          
	// Unregister the tool menu extension
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
}

void FAngelscriptEditorModule::ReloadTags()
{
	AngelscriptReloadGameplayTags();
}

void FAngelscriptEditorModule::RegisterToolsMenuEntries()
{
                                                                                        
	SoloASEditorManagement::RegisterMenu();
                                                                                      
	FToolMenuOwnerScoped OwnerScoped(this);

	// Extend the Tools -> Programming menu
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("MainFrame.MainMenu.Tools");
	if (!Menu) return;
	FToolMenuSection& Section = Menu->FindOrAddSection("Programming");
	
	FToolUIActionChoice Action(FExecuteAction::CreateLambda([]()
	{
		// Open VS Code to the <Project>/Script directory
		const FString ScriptPath = FPaths::ProjectDir() / TEXT("Script");
		const FString VSCodeWorkspacePath = FAngelscriptManager::ConfigSettings->VSCodeWorkspacePath;
		FString ParamPath;
		if (VSCodeWorkspacePath.IsEmpty())
		{
			ParamPath = ScriptPath;
		}
		else
		{
			ParamPath = FPaths::ProjectDir() / VSCodeWorkspacePath;
		}

		FAngelscriptEditorModule::OpenVsCode(FString::Printf(TEXT("\"%s\""), *ParamPath));
	}));
	
	Section.AddMenuEntry("ASOpenCode",
		NSLOCTEXT("Angelscript", "OpenCode.Label", "Open Angelscript workspace (VS Code)"),
		NSLOCTEXT("Angelscript", "OpenCode.ToolTip", "Opens Visual Studio Code in this project's Angelscript workspace"),
		FSourceCodeNavigation::GetOpenSourceCodeIDEIcon(),
		Action);
}
