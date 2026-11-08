// Blueprint ancestor generation awaits the core B-class extension point.
#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "Interfaces/ScriptInterfacesExtension.h"
#if WITH_EDITOR && !IS_MONOLITHIC && (PLATFORM_WINDOWS || PLATFORM_MAC)
#include "CoreExtensions/SoloASBlueprintChildRelink.h"
#include "AngelscriptManager.h"
#include "ClassGenerator/ASClass.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Containers/Ticker.h"
#include "Misc/CoreDelegates.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"
namespace
{
struct FPendingBlueprintDefaultPropagation
{
	FString ScriptClassName;
	TArray<FName> PropertyNames;
};

static TArray<FAngelscriptManager::FFilenamePair> PendingFullReloadFiles;
static TArray<TWeakObjectPtr<UASClass>> PendingScriptClasses;
static TArray<FPendingBlueprintDefaultPropagation> PendingBlueprintDefaultPropagations;
static FTSTicker::FDelegateHandle ReloadTickerHandle;
static FDelegateHandle EngineInitHandle;
static bool bInitialized = false;
static bool bFullReloadPending = false;
static bool bFullReloadScheduled = false;
bool RunFullReload(float);
bool QueueScriptClassForFullReload(UClass* ChildClass, const UBlueprint* Blueprint)
{
	UASClass* ScriptClass = Cast<UASClass>(ChildClass);
	if (ScriptClass == nullptr) return false;
	if (Blueprint == nullptr || ScriptClass->ScriptTypePtr == nullptr) return true;
	if (!FAngelscriptManager::IsInitialized() || Blueprint->GeneratedClass == nullptr) return false;
	FAngelscriptManager& Manager = FAngelscriptManager::Get();
	TSharedPtr<FAngelscriptModuleDesc> Module = Manager.GetModule(ScriptClass->ScriptTypePtr->GetModule());
	if (!Module.IsValid() || Module->Code.IsEmpty())
	{
		return false;
	}
	TSharedPtr<FAngelscriptClassDesc> ClassDesc = Module->GetClass(ScriptClass->ScriptTypePtr);
	if (!ClassDesc.IsValid())
	{
		return false;
	}
	if (ClassDesc->Class != ScriptClass || PendingScriptClasses.Contains(ScriptClass)) return true;
	ClassDesc->bPendingFullReload = true;
	PendingScriptClasses.AddUnique(ScriptClass);
	TSet<FName> ExistingParentProperties;
	for (FProperty* Property = ScriptClass->PropertyLink; Property != nullptr; Property = Property->PropertyLinkNext)
	{
		if (Property->GetOwnerStruct() == Blueprint->GeneratedClass.Get())
		{
			ExistingParentProperties.Add(Property->GetFName());
		}
	}
	FPendingBlueprintDefaultPropagation& DefaultPropagation =
		PendingBlueprintDefaultPropagations.AddDefaulted_GetRef();
	DefaultPropagation.ScriptClassName = ClassDesc->ClassName;
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		FProperty* ParentProperty = FindFProperty<FProperty>(Blueprint->GeneratedClass.Get(), Variable.VarName);
		if (ParentProperty != nullptr && ParentProperty->GetOwnerStruct() == Blueprint->GeneratedClass.Get()
			&& !ExistingParentProperties.Contains(Variable.VarName))
		{
			DefaultPropagation.PropertyNames.AddUnique(Variable.VarName);
		}
	}
	if (DefaultPropagation.PropertyNames.IsEmpty())
	{
		PendingBlueprintDefaultPropagations.Pop();
	}
	for (const FAngelscriptModuleDesc::FCodeSection& Section : Module->Code)
	{
		PendingFullReloadFiles.Add({Section.AbsoluteFilename, Section.RelativeFilename});
	}
    bFullReloadPending = true;
    if (!bFullReloadScheduled)
    {
        bFullReloadScheduled = true;
        ReloadTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateStatic(&RunFullReload), 0.0f);
    }
    return true;
}

bool RunFullReload(float)
{
	ReloadTickerHandle.Reset();
	bFullReloadScheduled = false;
	SoloASBridge::ResetBlueprintChildRelinkBatch();
	const bool bRun = bInitialized && bFullReloadPending && FAngelscriptManager::IsInitialized();
	bFullReloadPending = false;
	if (bRun)
	{
		FAngelscriptManager& Manager = FAngelscriptManager::Get();
		for (const TWeakObjectPtr<UASClass>& WeakScriptClass : PendingScriptClasses)
		{
			if (UASClass* ScriptClass = WeakScriptClass.Get())
			{
				ScriptClass->Bind();
				ScriptClass->StaticLink();
			}
		}
		for (const FAngelscriptManager::FFilenamePair& File : PendingFullReloadFiles)
		{
			Manager.FileChangesDetectedForReload.AddUnique(File);
		}
		Manager.LastFileChangeDetectedTime = -1.0;
		Manager.CheckForHotReload(ECompileType::FullReload);
		for (const FPendingBlueprintDefaultPropagation& DefaultPropagation : PendingBlueprintDefaultPropagations)
		{
			TSharedPtr<FAngelscriptClassDesc> ClassDesc = Manager.GetClass(DefaultPropagation.ScriptClassName);
			UASClass* ScriptClass = ClassDesc.IsValid() ? Cast<UASClass>(ClassDesc->Class) : nullptr;
			UClass* ParentClass = ScriptClass != nullptr ? ScriptClass->GetSuperClass() : nullptr;
			if (ScriptClass == nullptr || ParentClass == nullptr)
			{
				continue;
			}
			UObject* ParentDefault = ParentClass->GetDefaultObject();
			UObject* ScriptDefault = ScriptClass->GetDefaultObject();
			for (FName PropertyName : DefaultPropagation.PropertyNames)
			{
				FProperty* Property = FindFProperty<FProperty>(ParentClass, PropertyName);
				if (Property == nullptr)
				{
					continue;
				}
				Property->CopyCompleteValue_InContainer(ScriptDefault, ParentDefault);
				for (TObjectIterator<UObject> It; It; ++It)
				{
					UObject* Instance = *It;
					if (Instance != nullptr && Instance != ScriptDefault
						&& Instance->GetClass() == ScriptClass
						&& !Instance->HasAnyFlags(RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed))
					{
						Property->CopyCompleteValue_InContainer(Instance, ParentDefault);
					}
				}
			}
		}
	}
	PendingScriptClasses.Reset();
	PendingBlueprintDefaultPropagations.Reset();
	PendingFullReloadFiles.Reset();
	return false;
}
}

namespace SoloASCoreExtensions::ScriptInterfaces
{
bool CanReloadBlueprintSubclass(UClass* Class)
{
    auto* ScriptClass = Cast<UASClass>(Class);
    const auto* Relink = IConsoleManager::Get().FindConsoleVariable(TEXT("soloas.EditorBlueprintSubclassRelink"));
    if (!bInitialized || !Relink || Relink->GetInt() == 0 || !IsInGameThread()
        || !ScriptClass || !ScriptClass->ScriptTypePtr || !FAngelscriptManager::IsInitialized()) return false;
    auto& Manager = FAngelscriptManager::Get();
    if (Manager.bIsCompilingModules) return false;
    const auto Module = Manager.GetModule(ScriptClass->ScriptTypePtr->GetModule());
    if (!Module.IsValid() || Module->Code.IsEmpty()) return false;
    const auto Desc = Module->GetClass(ScriptClass->ScriptTypePtr);
    if (!Desc.IsValid() || Desc->Class != ScriptClass) return false;
    for (const auto& Section : Module->Code)
        if (!IFileManager::Get().FileExists(*Section.AbsoluteFilename)) return false;
    return true;
}

void StartBlueprintRelink()
{
    if (!IsEnabled() || !GIsEditor || IsRunningCommandlet()) return;
    StartInterfaceReloadCallbacks();
    const auto Install = []
    {
        StartBlueprintInterfaceDefaults();
        bInitialized = SoloASBridge::InstallBlueprintChildRelink(&QueueScriptClassForFullReload);
        if (bInitialized)
            UE_LOG(LogTemp, Display, TEXT("[SoloASCoreExtensions] StartBlueprintRelink: enabled and installed."));
        if (!bInitialized)
            UE_LOG(LogTemp, Warning, TEXT("[SoloASCoreExtensions] Blueprint child relink could not be installed; "
                "Blueprint parent recompiles cannot rebuild script children. "
                "Check the editor hook version and restart after updating."));
    };
    if (GEngine != nullptr) Install();
    else EngineInitHandle = FCoreDelegates::OnPostEngineInit.AddLambda(Install);
}
void StopBlueprintRelink()
{
    StopInterfaceReloadCallbacks();
    StopBlueprintInterfaceDefaults();
    FCoreDelegates::OnPostEngineInit.Remove(EngineInitHandle);
    if (ReloadTickerHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(ReloadTickerHandle);
    ReloadTickerHandle.Reset();
    SoloASBridge::RemoveBlueprintChildRelink();
    bInitialized = false;
    bFullReloadPending = false;
    bFullReloadScheduled = false;
    PendingScriptClasses.Reset();
    PendingBlueprintDefaultPropagations.Reset();
    PendingFullReloadFiles.Reset();
}
}
#endif

#else
#endif
