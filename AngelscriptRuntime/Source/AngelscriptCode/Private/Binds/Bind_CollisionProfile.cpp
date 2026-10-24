#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "AngelscriptDocs.h"

#include "Engine/CollisionProfile.h" 

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_CollisionProfile(FAngelscriptBinds::EOrder::Late, []
{
	FAngelscriptBinds::FNamespace ns("CollisionProfile");
	{
		TArray<TSharedPtr<FName>> CollisionProfiles;
		UCollisionProfile::GetProfileNames(CollisionProfiles);

		static TArray<FName> CollisionProfileNames;
		CollisionProfileNames.Empty(CollisionProfiles.Num());

		for (int i = 0; i < CollisionProfiles.Num(); i++)
		{
			const FName CollisionProfileName = *CollisionProfiles[i].Get();
			CollisionProfileNames.Add(CollisionProfileName);

			const FString Declaration = TEXT("const FName ") + CollisionProfileName.ToString();
			FAngelscriptBinds::BindGlobalVariable(TCHAR_TO_ANSI(*Declaration), &CollisionProfileNames[i]);

#if WITH_EDITORONLY_DATA
			// Use the profile name as tooltip
			FCollisionResponseTemplate CollisionResponseTemplate;
			if (UCollisionProfile::Get()->GetProfileTemplate(CollisionProfileName, CollisionResponseTemplate))
			{
				FAngelscriptDocs::AddDocumentationForGlobalVariable(FAngelscriptBinds::GetPreviousGlobalVariableId(), CollisionResponseTemplate.HelpMessage);
			}
#endif
		}
	}
});	