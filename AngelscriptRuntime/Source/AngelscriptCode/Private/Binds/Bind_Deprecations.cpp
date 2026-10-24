#include "CoreMinimal.h"
#include "AngelscriptManager.h"
#include "AngelscriptBinds.h"

#if WITH_EDITOR
static const FName NAME_META_DeprecatedFunction("DeprecatedFunction");
static const FName NAME_META_DeprecationMessage("DeprecationMessage");

void DeprecateMethod(const TCHAR* MethodPath, const TCHAR* DeprecationMessage)
{
	UFunction* Function = FindObject<UFunction>(nullptr, MethodPath);
	if (Function == nullptr)
	{
		UE_LOG(Angelscript, Warning, TEXT("Could not find method %s to add deprecation for"), MethodPath)
		return;
	}

	Function->SetMetaData(NAME_META_DeprecatedFunction, TEXT(""));
	Function->SetMetaData(NAME_META_DeprecationMessage, DeprecationMessage);
}

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_Deprecations((int32)FAngelscriptBinds::EOrder::Early, []
{
	// Epic has deprecated these functions when used in C++, but not for blueprint
	// Likely they're aiming to remove them and use redirectors for the blueprint usages, but this would break for script usage
	// Instead we add blueprint deprecation to them manually here, so it's picked up in script
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableLinearColor"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableVec4"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableQuat"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableMatrix"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableVec3"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariablePosition"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableVec2"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableFloat"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableInt"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableBool"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableActor"), TEXT("Use the SetVariable variant that takes FName instead"));
	DeprecateMethod(TEXT("/Script/Niagara.NiagaraComponent:SetNiagaraVariableObject"), TEXT("Use the SetVariable variant that takes FName instead"));
});
#endif
