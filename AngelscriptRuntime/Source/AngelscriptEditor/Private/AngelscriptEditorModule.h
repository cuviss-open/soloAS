#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class ANGELSCRIPTEDITOR_API FAngelscriptEditorModule : public FDefaultModuleImpl
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
                                                                                                              
	virtual bool SupportsDynamicReloading() override { return false; }
                                                                                                            

	static void ShowAssetListPopup(const TArray<FString>& AssetPaths, class UASClass* BaseClass);
	static void ShowCreateBlueprintPopup(class UASClass* Class);
	static void OpenVsCode(FString Parameters);

private:
#if PLATFORM_WINDOWS || PLATFORM_MAC	
	static void SearchForVsCode(FString& VsCodeLocation);
#endif
	
	void ReloadTags();
	void RegisterToolsMenuEntries();
};
