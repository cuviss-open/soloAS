#include "Components/PoseableMeshComponent.h"

#include "AngelscriptBinds.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_UPoseableMeshComponent(FAngelscriptBinds::EOrder::Late, []
{
	FAngelscriptBinds UPoseableMeshComponent_ = FAngelscriptBinds::ExistingClass("UPoseableMeshComponent");

	UPoseableMeshComponent_.Method("void AllocateTransformData()", [](UPoseableMeshComponent* Component)
	{
		Component->AllocateTransformData();
	});

	UPoseableMeshComponent_.Method("void RefreshBoneTransforms()", [](UPoseableMeshComponent* Component)
	{
		Component->RefreshBoneTransforms();
	});
});