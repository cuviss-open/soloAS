#pragma once
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "AngelscriptHitResultLibrary.generated.h"

UCLASS(Meta = (ScriptMixin = "FHitResult"))
class UAngelscriptHitResultLibrary : public UObject
{
	GENERATED_BODY()

public:

	/** PrimitiveComponent hit by the trace. */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static void SetComponent(FHitResult& HitResult, UPrimitiveComponent* Component)
	{
		HitResult.Component = Component;
	}

	/** Actor that owns the Component that was hit by the trace. */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static void SetActor(FHitResult& HitResult, AActor* Actor)
	{
		HitResult.HitObjectHandle = FActorInstanceHandle(Actor);
	}

	/** Reset hit result while optionally saving TraceStart and TraceEnd. */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static void Reset(FHitResult& HitResult, float InTime = 1.f, bool bPreserveTraceData = true)
	{
		HitResult.Reset(InTime, bPreserveTraceData);
	}

	/** PrimitiveComponent hit by the trace. */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static UPrimitiveComponent* GetComponent(const FHitResult& HitResult)
	{
		return HitResult.GetComponent();
	}

	/** Actor that owns the Component that was hit by the trace. */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static AActor* GetActor(const FHitResult& HitResult)
	{
		return HitResult.GetActor();
	}

	/**
	 * Physical material that was hit.
	 * @note Must set bReturnPhysicalMaterial on the swept PrimitiveComponent or in the query params for this to be returned.
	 */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static void SetPhysMaterial(FHitResult& HitResult, UPhysicalMaterial* PhysMaterial)
	{
		HitResult.PhysMaterial = PhysMaterial;
	}

	/**
	 * Physical material that was hit.
	 * @note Must set bReturnPhysicalMaterial on the swept PrimitiveComponent or in the query params for this to be returned.
	 */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static UPhysicalMaterial* GetPhysMaterial(const FHitResult& HitResult)
	{
		return HitResult.PhysMaterial.Get();
	}

	/** Indicates if this hit was a result of blocking collision. If false, there was no hit or it was an overlap/touch instead. */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static bool GetbBlockingHit(const FHitResult& HitResult)
	{
		return HitResult.bBlockingHit;
	}

	/** Indicates if this hit was a result of blocking collision. If false, there was no hit or it was an overlap/touch instead. */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial, DeprecatedFunction, DeprecationMessage = "Assign bBlockingHit instead"))
	static void SetBlockingHit(FHitResult& HitResult, bool bIsBlocking)
	{
		HitResult.bBlockingHit = bIsBlocking;
	}

	/** Indicates if this hit was a result of blocking collision. If false, there was no hit or it was an overlap/touch instead. */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static void SetbBlockingHit(FHitResult& HitResult, bool bIsBlocking)
	{
		HitResult.bBlockingHit = bIsBlocking;
	}

	/**
	 * Whether the trace started in penetration, i.e. with an initial blocking overlap.
	 * In the case of penetration, if PenetrationDepth > 0.f, then it will represent the distance along the Normal vector that will result in
	 * minimal contact between the swept shape and the object that was hit. In this case, ImpactNormal will be the normal opposed to movement at that location
	 * (ie, Normal may not equal ImpactNormal). ImpactPoint will be the same as Location, since there is no single impact point to report.
	 */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static bool GetbStartPenetrating(const FHitResult& HitResult)
	{
		return HitResult.bStartPenetrating;
	}

	/**
	 * Whether the trace started in penetration, i.e. with an initial blocking overlap.
	 * In the case of penetration, if PenetrationDepth > 0.f, then it will represent the distance along the Normal vector that will result in
	 * minimal contact between the swept shape and the object that was hit. In this case, ImpactNormal will be the normal opposed to movement at that location
	 * (ie, Normal may not equal ImpactNormal). ImpactPoint will be the same as Location, since there is no single impact point to report.
	 */
	UFUNCTION(ScriptCallable, Meta = (ScriptTrivial))
	static void SetbStartPenetrating(FHitResult& HitResult, bool bStartPenetrating)
	{
		HitResult.bStartPenetrating = bStartPenetrating;
	}
};
