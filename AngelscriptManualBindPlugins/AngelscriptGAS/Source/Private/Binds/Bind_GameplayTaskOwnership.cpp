// SPDX-License-Identifier: (Apache-2.0 OR MIT)
// Copyright Cuviss Studio.
#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "Mixin/GameplayTaskMixinLibrary.h"
#include "Shared/SoloASBridgeReflection.h"
#include "UObject/Class.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_GameplayTaskOwnership(
    (int32)FAngelscriptBinds::EOrder::Early - 1, []
{
    // Defer GAS ownership until the reflected providers have registered.
    UFunction* Function = UGameplayTaskMixinLibrary::StaticClass()->FindFunctionByName(
        TEXT("ReadyForActivation"));
    const bool bRegistered = SoloASBridge::SetFunctionTraits(Function,
        FName(TEXT("AngelscriptGAS")), ESoloASBridgeFunctionTrait::NotInAngelscript);
    check(bRegistered);
});

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_GameplayTaskActivation(
    (int32)FAngelscriptBinds::EOrder::Late + 101, []
{
    // Reflected mixins register at Late + 100, including optional providers.
    auto* TaskType = FAngelscriptManager::Get().GetScriptEngine()->GetTypeInfoByName("UGameplayTask");
    check(TaskType);
    if (TaskType->GetMethodByDecl("void ReadyForActivation()") == nullptr)
    {
        auto Task = FAngelscriptBinds::ExistingClass("UGameplayTask");
        Task.Method("void ReadyForActivation()", &UGameplayTask::ReadyForActivation);
    }
});
