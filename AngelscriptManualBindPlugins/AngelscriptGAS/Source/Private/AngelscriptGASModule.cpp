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
                                                                                          
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "Mixin/GameplayEffectSpecMixinLibrary.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASModifierMagnitudeDefaultStack,
    "SoloAS.GAS.ModifierMagnitudeDefaultStack", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGASModifierMagnitudeDefaultStack::RunTest(const FString&)
{
    TStrongObjectPtr<UGameplayEffect> Definition(NewObject<UGameplayEffect>(GetTransientPackage()));
    Definition->DurationPolicy = EGameplayEffectDurationType::Instant;
    FGameplayModifierInfo Modifier;
    Modifier.ModifierOp = EGameplayModOp::Additive;
    Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(5.0f));
    Definition->Modifiers.Add(Modifier);
    FGameplayEffectSpec Spec(Definition.Get(), FGameplayEffectContextHandle(), 1.0f);
    Spec.SetStackCount(3);
    Spec.CalculateModifierMagnitudes();
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    const float Legacy = Spec.GetModifierMagnitude(0, true);
    PRAGMA_ENABLE_DEPRECATION_WARNINGS
    TestEqual(TEXT("The legacy true path includes all three additive stacks"), Legacy, 15.0f);
    TestEqual(TEXT("The script mixin retains the default stack magnitude"),
        UGameplayEffectSpecMixinLibrary::GetModifierMagnitude(Spec, 0), Legacy);
// SOLOAS-COMPAT-BEGIN(<5.6) Old effect definitions have a fixed true stacking policy [test: SoloAS.GAS.ModifierMagnitudeDefaultStack]
#if SOLOAS_UE_AT_LEAST(5, 6)
    TestTrue(TEXT("New effect definitions default to factoring in stack count"), Definition->bFactorInStackCount);
    TestEqual(TEXT("The newer one-argument API matches the legacy true path by default"), Spec.GetModifierMagnitude(0), Legacy);
#endif
// SOLOAS-COMPAT-END
    return !HasAnyErrors();
}
#endif
                                                                                        
