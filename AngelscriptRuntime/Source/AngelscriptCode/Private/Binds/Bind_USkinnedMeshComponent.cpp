#include "Components/SkinnedMeshComponent.h"

#include "AngelscriptBinds.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_USkinnedMeshComponent(FAngelscriptBinds::EOrder::Late, []
{
	FAngelscriptBinds USkinnedMeshComponent_ = FAngelscriptBinds::ExistingClass("USkinnedMeshComponent");

	USkinnedMeshComponent_.Method("void UpdateLODStatus()", METHOD_TRIVIAL(USkinnedMeshComponent, UpdateLODStatus));
	USkinnedMeshComponent_.Method("void InvalidateCachedBounds()", METHOD_TRIVIAL(USkinnedMeshComponent, InvalidateCachedBounds));
});