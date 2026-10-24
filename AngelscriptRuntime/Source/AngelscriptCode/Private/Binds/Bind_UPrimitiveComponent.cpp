#include "CoreMinimal.h"

#include "GameFramework/Actor.h"
                                                                                                             
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/ShapeComponent.h"
#include "Components/SphereComponent.h"
#include "UObject/UnrealType.h"
                                                                                                           
#include "Components/PrimitiveComponent.h"

#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"

#include "AngelscriptManager.h"
#include "AngelscriptType.h"
#include "AngelscriptBinds.h"


                                                                                                            
namespace
{
FObjectPropertyBase* GetShapeBodySetupProperty()
{
	static FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(
		UShapeComponent::StaticClass(), TEXT("ShapeBodySetup"));
	return Property;
}

bool HasExistingShapeBodySetup(const UShapeComponent* Component)
{
	FObjectPropertyBase* Property = GetShapeBodySetupProperty();
	if (Property == nullptr)
		return true;
	return Property->GetObjectPropertyValue_InContainer(Component) != nullptr;
}
}
                                                                                                          


AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_UPrimitiveComponent([]
{
	FAngelscriptBinds UPrimitiveComponent_ = FAngelscriptBinds::ExistingClass("UPrimitiveComponent");

	UPrimitiveComponent_.Method("FVector GetBoundingBoxExtents() const", [](const UPrimitiveComponent* Component)->FVector
	{
		return Component->GetCollisionShape().GetExtent();
	});

	UPrimitiveComponent_.Method("FVector GetBoundsOrigin() const", [](const UPrimitiveComponent* Component)->FVector
	{
		return Component->Bounds.Origin;
	});

	UPrimitiveComponent_.Method("FVector GetBoundsExtent() const", [](const UPrimitiveComponent* Component)->FVector
	{
		return Component->Bounds.BoxExtent;
	});

	UPrimitiveComponent_.Method("float64 GetBoundsRadius() const", [](const UPrimitiveComponent* Component)->double
	{
		return Component->Bounds.SphereRadius;
	});

	UPrimitiveComponent_.Method("bool GetbSelectable() const", [](const UPrimitiveComponent* Component) -> bool
	{
		return Component->bSelectable;
	});

	UPrimitiveComponent_.Method("void SetbSelectable(bool bSelectable)", [](UPrimitiveComponent* Component, bool bSelectable)
	{
		Component->bSelectable = bSelectable;
	});

	UPrimitiveComponent_.Method("void SetLightmapType(ELightmapType Type)", [](UPrimitiveComponent* Component, ELightmapType Type)
	{
		Component->SetLightmapType(Type);
	});

                                                                                                                  
	auto UBoxComponent_ = FAngelscriptBinds::ExistingClass("UBoxComponent");
	UBoxComponent_.Method("void SetBoxExtent(FVector NewBoxExtent, bool bUpdateOverlaps = true)",
		[](UBoxComponent* Component, const FVector& NewBoxExtent, bool bUpdateOverlaps)
	{
		if (Component == nullptr)
		{
			FAngelscriptManager::Throw("Box component was null.");
			return;
		}
		if (HasExistingShapeBodySetup(Component))
		{
			Component->SetBoxExtent(NewBoxExtent, bUpdateOverlaps);
			return;
		}

		Component->InitBoxExtent(NewBoxExtent);
		Component->UpdateBounds();
		Component->MarkRenderStateDirty();
	});

	auto UCapsuleComponent_ = FAngelscriptBinds::ExistingClass("UCapsuleComponent");
	UCapsuleComponent_.Method("void SetCapsuleSize(float32 NewRadius, float32 NewHalfHeight, bool bUpdateOverlaps = true)",
		[](UCapsuleComponent* Component, float NewRadius, float NewHalfHeight, bool bUpdateOverlaps)
	{
		if (Component == nullptr)
		{
			FAngelscriptManager::Throw("Capsule component was null.");
			return;
		}
		if (HasExistingShapeBodySetup(Component))
		{
			Component->SetCapsuleSize(NewRadius, NewHalfHeight, bUpdateOverlaps);
			return;
		}

		Component->InitCapsuleSize(NewRadius, NewHalfHeight);
		Component->UpdateBounds();
		Component->MarkRenderStateDirty();
	});

	auto USphereComponent_ = FAngelscriptBinds::ExistingClass("USphereComponent");
	USphereComponent_.Method("void SetSphereRadius(float32 InSphereRadius, bool bUpdateOverlaps = true)",
		[](USphereComponent* Component, float InSphereRadius, bool bUpdateOverlaps)
	{
		if (Component == nullptr)
		{
			FAngelscriptManager::Throw("Sphere component was null.");
			return;
		}
		if (HasExistingShapeBodySetup(Component))
		{
			Component->SetSphereRadius(InSphereRadius, bUpdateOverlaps);
			return;
		}

		Component->InitSphereRadius(InSphereRadius);
		Component->UpdateBounds();
		Component->MarkRenderStateDirty();
	});
                                                                                                                
});
