// SPDX-License-Identifier: (Apache-2.0 OR MIT)

#include "AngelscriptBinds.h"

#include "ActiveGameplayEffectHandle.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FActiveGameplayEffectHandle(FAngelscriptBinds::EOrder::Late, [] {
	auto FActiveGameplayEffectHandle_ = FAngelscriptBinds::ExistingClass("FActiveGameplayEffectHandle");

	FActiveGameplayEffectHandle_.Method("bool IsValid() const", METHOD_TRIVIAL(FActiveGameplayEffectHandle, IsValid));
	FActiveGameplayEffectHandle_.Method("bool WasSuccessfullyApplied() const", METHOD_TRIVIAL(FActiveGameplayEffectHandle, WasSuccessfullyApplied));
	FActiveGameplayEffectHandle_.Method("void Invalidate()", METHOD_TRIVIAL(FActiveGameplayEffectHandle, Invalidate));
});
