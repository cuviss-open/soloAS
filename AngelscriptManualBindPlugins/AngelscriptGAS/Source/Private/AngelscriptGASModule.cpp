#include "AngelscriptGASModule.h"
                                                                                     
#include "Shared/SoloASBridgeReflection.h"
                                                                                   

#include "AngelscriptCodeModule.h"
#include "ClassGenerator/ASClass.h"

#include "GameplayCueNotify_Static.h"
#include "GameplayCueNotify_Actor.h"
#include "AbilitySystemGlobals.h"
#include "GameplayCueManager.h"
#include "GameplayCueSet.h"

#include "UObject/UObjectIterator.h"

IMPLEMENT_MODULE(FAngelscriptGASModule, AngelscriptGAS);

void FAngelscriptGASModule::StartupModule()
{
#if WITH_EDITOR
	FAngelscriptCodeModule::GetPostCompile().AddLambda([]()
	{
		// We must manually load all AS cues into the global cue map, or you can't define cues+tags in AS. Should work in play though!
		UGameplayCueManager* CueManager = UAbilitySystemGlobals::Get().GetGameplayCueManager();
		if (CueManager)
		{
			TArray<UGameplayCueSet*> CueSets = CueManager->GetGlobalCueSets();

			for (TObjectIterator<UClass> It; It; ++It)
			{
				UASClass* ASClass = Cast<UASClass>(*It);
				if (!ASClass || It->HasAnyClassFlags(CLASS_Abstract))
				{
					continue;
				}

				// Ideally, we would just like to call PostEditChangeProperty on the cue, but since it assumes BP classes, it won't work for AS cues.
				UGameplayCueNotify_Static* CueStaticCDO = Cast<UGameplayCueNotify_Static>(It->GetDefaultObject());
				AGameplayCueNotify_Actor* CueActorCDO = Cast<AGameplayCueNotify_Actor>(It->GetDefaultObject());
				if (CueStaticCDO || CueActorCDO)
				{
					FSoftObjectPath StringRef;
					StringRef.SetPath(It->GetPathName());

					TArray<FGameplayCueReferencePair> CuesToAdd;
					if (CueStaticCDO)
					{
						CuesToAdd.Add(FGameplayCueReferencePair(CueStaticCDO->GameplayCueTag, StringRef));
					}
					else if (CueActorCDO)
					{
						CuesToAdd.Add(FGameplayCueReferencePair(CueActorCDO->GameplayCueTag, StringRef));
					}

					TArray<FSoftObjectPath> StringRefArray;
					StringRefArray.Add(StringRef);
					for (UGameplayCueSet* Set : CueSets)
					{
						Set->RemoveCuesByStringRefs(StringRefArray);

						if (!ASClass->NewerVersion)
						{
							// Only add newest class versions! Or we crash.
							Set->AddCues(CuesToAdd);
						}
					}
				}
			}
		}
	});
#endif
}

void FAngelscriptGASModule::ShutdownModule()
{
                                                                                      
    SoloASBridge::ClearFunctionTraits(FName(TEXT("AngelscriptGAS")));
                                                                                    
}
