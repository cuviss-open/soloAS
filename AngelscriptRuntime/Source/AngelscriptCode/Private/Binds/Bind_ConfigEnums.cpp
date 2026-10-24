#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"

#include "Engine/CollisionProfile.h"
#include "PhysicsEngine/PhysicsSettings.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_ConfigEnums((int32)FAngelscriptBinds::EOrder::Early - 1, []
{
	auto ETraceTypeQuery_ = FAngelscriptBinds::Enum("ETraceTypeQuery");
	auto ECollisionChannel_ = FAngelscriptBinds::Enum("ECollisionChannel");
	auto EObjectTypeQuery_ = FAngelscriptBinds::Enum("EObjectTypeQuery");

	UCollisionProfile* Collision = UCollisionProfile::Get();

                                                                                                                            
#if 0
                                                                                                                          
	for (FCustomChannelSetup& Profile : Collision->DefaultChannelResponses)
	{
                                                                                                                          
#endif
	const FArrayProperty* ChannelResponsesProperty = FindFProperty<FArrayProperty>(
		UCollisionProfile::StaticClass(), TEXT("DefaultChannelResponses"));
	check(ChannelResponsesProperty != nullptr);
	FScriptArrayHelper_InContainer ChannelResponses(
		ChannelResponsesProperty, Collision);
	for (int32 ChannelIndex = 0; ChannelIndex < ChannelResponses.Num(); ++ChannelIndex)
	{
		FCustomChannelSetup& Profile =
			*reinterpret_cast<FCustomChannelSetup*>(
				ChannelResponses.GetRawPtr(ChannelIndex));
                                                                                                                        
		const FString BPName = Profile.Name.ToString().Replace(TEXT(" "), TEXT("_"));

		if (Profile.bTraceType)
		{
			int32 BPValue = (int32)Collision->ConvertToTraceType(Profile.Channel);
			ETraceTypeQuery_[BPName] = BPValue;
		}
		else
		{
			int32 BPValue = (int32)Collision->ConvertToObjectType(Profile.Channel);
			EObjectTypeQuery_[BPName] = BPValue;
		}

		ECollisionChannel_[BPName] = (int32)Profile.Channel;
	}

	ECollisionChannel_["Visibility"] = (int32)ECC_Visibility;
	ECollisionChannel_["Camera"] = (int32)ECC_Camera;
	ECollisionChannel_["WorldStatic"] = (int32)ECC_WorldStatic;
	ECollisionChannel_["WorldDynamic"] = (int32)ECC_WorldDynamic;
	ECollisionChannel_["Pawn"] = (int32)ECC_Pawn;
	ECollisionChannel_["PhysicsBody"] = (int32)ECC_PhysicsBody;
	ECollisionChannel_["Vehicle"] = (int32)ECC_Vehicle;
	ECollisionChannel_["Destructible"] = (int32)ECC_Destructible;

	ETraceTypeQuery_["Visibility"] = (int32)Collision->ConvertToTraceType(ECC_Visibility);
	ETraceTypeQuery_["Camera"] = (int32)Collision->ConvertToTraceType(ECC_Camera);

	EObjectTypeQuery_["WorldStatic"] = (int32)Collision->ConvertToObjectType(ECC_WorldStatic);
	EObjectTypeQuery_["WorldDynamic"] = (int32)Collision->ConvertToObjectType(ECC_WorldDynamic);
	EObjectTypeQuery_["Pawn"] = (int32)Collision->ConvertToObjectType(ECC_Pawn);
	EObjectTypeQuery_["PhysicsBody"] = (int32)Collision->ConvertToObjectType(ECC_PhysicsBody);
	EObjectTypeQuery_["Vehicle"] = (int32)Collision->ConvertToObjectType(ECC_Vehicle);
	EObjectTypeQuery_["Destructible"] = (int32)Collision->ConvertToObjectType(ECC_Destructible);

	auto EPhysicalSurface_ = FAngelscriptBinds::Enum("EPhysicalSurface");
	for (auto& Surface : UPhysicsSettings::Get()->PhysicalSurfaces)
	{
		const FString BPName = Surface.Name.ToString().Replace(TEXT(" "), TEXT("_"));
		EPhysicalSurface_[BPName] = (int32)Surface.Type;
	}
});