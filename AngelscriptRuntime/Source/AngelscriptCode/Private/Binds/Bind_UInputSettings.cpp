#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "GameFramework/InputSettings.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_UInputSettings(FAngelscriptBinds::EOrder::Late, [] {
	auto UInputSettings_ = FAngelscriptBinds::ExistingClass("UInputSettings");

	UInputSettings_.Method("FName GetUniqueActionName(const FName BaseActionMappingName) no_discard", METHODPR_TRIVIAL(FName, UInputSettings, GetUniqueActionName, (const FName)));
	UInputSettings_.Method("FName GetUniqueAxisName(const FName BaseAxisMappingName) no_discard", METHODPR_TRIVIAL(FName, UInputSettings, GetUniqueAxisName, (const FName)));

	UInputSettings_.Method("const TArray<FInputActionKeyMapping>& GetActionMappings() const", METHODPR_TRIVIAL(const TArray<FInputActionKeyMapping>&, UInputSettings, GetActionMappings, () const));
	UInputSettings_.Method("const TArray<FInputAxisKeyMapping>& GetAxisMappings() const", METHODPR_TRIVIAL(const TArray<FInputAxisKeyMapping>&, UInputSettings, GetAxisMappings, () const));

	UInputSettings_.Method("bool DoesActionExist(const FName InActionName) no_discard", METHODPR_TRIVIAL(bool, UInputSettings, DoesActionExist, (const FName)));
	UInputSettings_.Method("bool DoesAxisExist(const FName InAxisName) no_discard", METHODPR_TRIVIAL(bool, UInputSettings, DoesAxisExist, (const FName)));
});
