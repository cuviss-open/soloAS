#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "Engine/EngineTypes.h"
#include "AngelscriptDocs.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FRay(FAngelscriptBinds::EOrder::Late, []
{
	auto FRay_ = FAngelscriptBinds::ExistingClass("FRay");

	FRay_.Constructor("void f(const FVector& Origin, const FVector& Direction, bool bDirectionIsNormalized = false)",
	[](FRay* Address, const FVector& InOrigin, const FVector& InDirection, bool bDirectionIsNormalized)
	{
		new(Address) FRay(InOrigin, InDirection, bDirectionIsNormalized);
	});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FRay_, "FRay");

	FRay_.Method("bool opEquals(const FRay& Other) const", METHODPR_TRIVIAL(bool, FRay, operator==, (const FRay&) const));

	FRay_.Method("FVector PointAt(float64 RayParameter) const", METHODPR_TRIVIAL(FVector, FRay, PointAt, (double) const));
	SCRIPT_BIND_DOCUMENTATION(
		"Calculate position on ray at given distance/parameter\n\n"
		"@param RayParameter Scalar distance along Ray\n"
		"@return Point on Ray"
	);

	FRay_.Method("float64 GetParameter(const FVector& Point) const", METHODPR_TRIVIAL(double, FRay, GetParameter, (const FVector&) const));
	SCRIPT_BIND_DOCUMENTATION(
		"Calculate ray parameter (distance from origin to closest point) for query Point\n\n"
		"@param Point query Point\n"
		"@return distance along ray from origin to closest point"
	);

	FRay_.Method("float64 DistSquared(const FVector& Point) const", METHODPR_TRIVIAL(double, FRay, DistSquared, (const FVector&) const));
	SCRIPT_BIND_DOCUMENTATION(
		"Find minimum squared distance from query point to ray\n\n"
		"@param Point query Point\n"
		"@return squared distance to Ray"
	);

	FRay_.Method("float64 Dist(const FVector& Point) const", METHODPR_TRIVIAL(double, FRay, Dist, (const FVector&) const));
	SCRIPT_BIND_DOCUMENTATION(
		"Find minimum distance from query point to ray\n\n"
		"@param Point query Point\n"
		"@return distance to Ray"
	);

	FRay_.Method("FVector ClosestPoint(const FVector& Point) const", METHODPR_TRIVIAL(FVector, FRay, ClosestPoint, (const FVector&) const));
	SCRIPT_BIND_DOCUMENTATION(
		"Find closest point on ray to query point\n\n"
		"@param Point query point\n"
		"@return closest point on Ray"
	);

	FRay_.Method("FString ToString() const", METHODPR_TRIVIAL(FString, FRay, ToString, () const));
	SCRIPT_BIND_DOCUMENTATION(
		"Get a textual representation of the Ray.\n\n"
		"@return Text describing the Ray."
	);
});
