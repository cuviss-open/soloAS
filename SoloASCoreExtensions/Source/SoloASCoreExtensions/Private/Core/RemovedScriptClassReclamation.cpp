// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "Core/RemovedScriptClassReclamation.h"

#include "CoreMinimal.h"
#include "AngelscriptManager.h"

#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "ClassGenerator/ASClass.h"
#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/UObjectIterator.h"

namespace
{
TAutoConsoleVariable<int32> ReclamationEnabled(
    TEXT("soloas.RemovedScriptClassReclamation"), 1,
    TEXT("Reclaim removed script classes without live instances after reload."));
FDelegateHandle PreReloadHandle;
FDelegateHandle PostReloadHandle;
TArray<TWeakObjectPtr<UASClass>> PreviousClasses;

void BeforeFullReload(const TArray<UObject*>&)
{
    PreviousClasses.Reset();
    if (!ReclamationEnabled.GetValueOnGameThread()) return;
    for (TObjectIterator<UASClass> It; It; ++It)
        if (It->ScriptTypePtr && It->IsRooted()) PreviousClasses.Add(*It);
}

void AfterReload(bool bFullReload)
{
    if (bFullReload && ReclamationEnabled.GetValueOnGameThread())
    {
        for (const TWeakObjectPtr<UASClass>& WeakClass : PreviousClasses)
        {
            UASClass* Class = WeakClass.Get();
            if (!Class || Class->ScriptTypePtr || !Class->RetiredScriptTypeForGc ||
                Class->HasAnyClassFlags(CLASS_NewerVersionExists) ||
                !Class->HasAllClassFlags(CLASS_NotPlaceable | CLASS_HideDropDown | CLASS_Hidden)) continue;

            bool bHasInstance = false;
            for (TObjectIterator<UObject> It; It; ++It)
            {
                if (!It->HasAnyFlags(RF_ClassDefaultObject) && It->IsA(Class))
                {
                    bHasInstance = true;
                    break;
                }
            }
            if (!bHasInstance)
            {
                Class->RemoveFromRoot();
                Class->ClearFlags(RF_Standalone);
            }
        }
    }
    PreviousClasses.Reset();
}
}
#endif

namespace SoloASCoreExtensions
{
void StartRemovedScriptClassReclamation()
{
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
    bool bEnabled = true;
    FConfigFile Defaults;
    if (const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("SoloASCoreExtensions")))
        Defaults.Read(Plugin->GetBaseDir() / TEXT("Config/DefaultSoloASCoreExtensions.ini"));
    Defaults.GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableRemovedScriptClassReclamation"), bEnabled);
    GConfig->GetBool(TEXT("SoloASCoreExtensions"), TEXT("bEnableRemovedScriptClassReclamation"), bEnabled, GEngineIni);
    ReclamationEnabled->Set(bEnabled ? 1 : 0, ECVF_SetByProjectSetting);
    PreReloadHandle = FAngelscriptClassGenerator::OnPreFullReload.AddStatic(&BeforeFullReload);
    PostReloadHandle = FAngelscriptClassGenerator::OnPostReload.AddStatic(&AfterReload);
#endif
}

void StopRemovedScriptClassReclamation()
{
#if WITH_EDITOR && AS_CAN_HOTRELOAD && !UE_BUILD_SHIPPING
    FAngelscriptClassGenerator::OnPreFullReload.Remove(PreReloadHandle);
    FAngelscriptClassGenerator::OnPostReload.Remove(PostReloadHandle);
    PreviousClasses.Reset();
#endif
}
}
