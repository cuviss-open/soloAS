#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"

#include "Engine/CollisionProfile.h" 

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_UCollisionProfile(FAngelscriptBinds::EOrder::Late, []
{
	FAngelscriptBinds::FNamespace ns("UCollisionProfile");
	{
		FAngelscriptBinds::BindGlobalFunction("ECollisionChannel ConvertToCollisionChannel(bool TraceType, int32 Index) no_discard", [](bool TraceType, int32 Index) -> ECollisionChannel {
			return UCollisionProfile::Get()->ConvertToCollisionChannel(TraceType, Index);
		});

		FAngelscriptBinds::BindGlobalFunction("EObjectTypeQuery ConvertToObjectType(ECollisionChannel CollisionChannel) no_discard", [](ECollisionChannel CollisionChannel) -> EObjectTypeQuery {
			return UCollisionProfile::Get()->ConvertToObjectType(CollisionChannel);
		});

		FAngelscriptBinds::BindGlobalFunction("ETraceTypeQuery ConvertToTraceType(ECollisionChannel CollisionChannel) no_discard", [](ECollisionChannel CollisionChannel) -> ETraceTypeQuery {
			return UCollisionProfile::Get()->ConvertToTraceType(CollisionChannel);
		});
	}
});	