#include "AngelscriptBinds.h"
#include "AngelscriptDocs.h"
#include "AngelscriptManager.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FHitResult(FAngelscriptBinds::EOrder::Late, []
{
	auto FHitResult_ = FAngelscriptBinds::ExistingClass("FHitResult");

	FHitResult_.Constructor("void f(AActor InActor, UPrimitiveComponent InComponent, const FVector& HitLoc, const FVector& HitNorm)", [](FHitResult* Address, class AActor* InActor, class UPrimitiveComponent* InComponent, FVector const& HitLoc, FVector const& HitNorm)
	{
		new(Address) FHitResult(InActor, InComponent, HitLoc, HitNorm);
	});
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FHitResult_, "FHitResult");
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);

	FHitResult_.Constructor("void f(const FVector& TraceStart, const FVector& TraceEnd)", [](FHitResult* Address, FVector const& TraceStart, FVector const& TraceEnd)
	{
		new(Address) FHitResult(TraceStart, TraceEnd);
	});
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FHitResult_, "FHitResult");
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);

	FHitResult_.Property("int FaceIndex", &FHitResult::FaceIndex);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "Face index we hit (for complex hits with triangle meshes).");
	FHitResult_.Property("uint8 ElementIndex", &FHitResult::ElementIndex);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "Index to item that was hit, also hit primitive specific.");
	FHitResult_.Property("int Item", &FHitResult::Item);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "Extra data about item that was hit (hit primitive specific).");
	FHitResult_.Property("int MyItem", &FHitResult::MyItem);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "If the hit result is from a collision this will have extra info about the item that hit the second item.");

	FHitResult_.Property("float32 PenetrationDepth", &FHitResult::PenetrationDepth);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "If this test started in penetration (bStartPenetrating is true) and a depenetration vector can be computed,\nthis value is the distance along Normal that will result in moving out of penetration.\nIf the distance cannot be computed, this distance will be zero.");
	FHitResult_.Property("float32 Distance", &FHitResult::Distance);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "The distance from the TraceStart to the Location in world space. This value is 0 if there was an initial overlap (trace started inside another colliding object).");
	FHitResult_.Property("float32 Time", &FHitResult::Time);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "'Time' of impact along trace direction (ranging from 0.0 to 1.0) if there is a hit, indicating time between TraceStart and TraceEnd.\nFor swept movement (but not queries) this may be pulled back slightly from the actual time of impact, to prevent precision problems with adjacent geometry.");

	FHitResult_.Property("FVector TraceStart", &FHitResult::TraceStart);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "Start location of the trace.\n\nFor example if a sphere is swept against the world, this is the starting location of the center of the sphere.");
	FHitResult_.Property("FVector TraceEnd", &FHitResult::TraceEnd);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "End location of the trace; this is NOT where the impact occurred (if any), but the furthest point in the attempted sweep.\n\nFor example if a sphere is swept against the world, this would be the center of the sphere if there was no blocking hit.");
	FHitResult_.Property("FVector ImpactNormal", &FHitResult::ImpactNormal);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "Normal of the hit in world space, for the object that was hit by the sweep, if any.\nFor example if a sphere hits a flat plane, this is a normalized vector pointing out from the plane.\nIn the case of impact with a corner or edge of a surface, usually the \"most opposing\" normal (opposed to the query direction) is chosen.");
	FHitResult_.Property("FVector ImpactPoint", &FHitResult::ImpactPoint);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "Location in world space of the actual contact of the trace shape (box, sphere, ray, etc) with the impacted object.\n\nExample: for a sphere trace test, this is the point where the surface of the sphere touches the other object.\n@note: In the case of initial overlap (bStartPenetrating=true), ImpactPoint will be the same as Location because there is no meaningful single impact point to report.");
	FHitResult_.Property("FVector Location", &FHitResult::Location);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "The location in world space where the moving shape would end up against the impacted object, if there is a hit. Equal to the point of impact for line tests.\n\nExample: for a sphere trace test, this is the point where the center of the sphere would be located when it touched the other object.\nFor swept movement (but not queries) this may not equal the final location of the shape since hits are pulled back slightly to prevent precision issues from overlapping another surface.");
	FHitResult_.Property("FVector Normal", &FHitResult::Normal);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "Normal of the hit in world space, for the object that was swept. Equal to ImpactNormal for line tests.\nThis is computed for capsules and spheres, otherwise it will be the same as ImpactNormal.\n\nExample: for a sphere trace test, this is a normalized vector pointing in towards the center of the sphere at the point of impact.");
	FHitResult_.Property("FName BoneName", &FHitResult::BoneName);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "Name of bone we hit (for skeletal meshes).");
	FHitResult_.Property("FName MyBoneName", &FHitResult::MyBoneName);
	SCRIPT_PROPERTY_DOCUMENTATION(FHitResult_, "Name of the _my_ bone which took part in hit event (in case of two skeletal meshes colliding).");
});