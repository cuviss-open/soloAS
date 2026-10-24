#pragma once
#include "CoreMinimal.h"

#include "EnhancedInputSubsystems.h"

#include "EnhancedInputSubsystemsMixinLibrary.generated.h"

/**
 * ScriptMixin library to bind functions on UEnhancedInputLocalPlayerSubsystem
 * that are not BlueprintCallable by default (due to not being exposed via
 * multiple inheritance/Unreal interface).
 */
UCLASS(Meta = (ScriptMixin = "UEnhancedInputLocalPlayerSubsystem"))
class UEnhancedInputLocalPlayerSubsystemScriptMixinLibrary : public UObject
{
	GENERATED_BODY()

public:

	UFUNCTION(ScriptCallable)
	static UEnhancedPlayerInput* GetPlayerInput(const UEnhancedInputLocalPlayerSubsystem* Subsystem)
	{
		return Subsystem->GetPlayerInput();
	}

	UFUNCTION(ScriptCallable)
	static UEnhancedInputUserSettings* GetUserSettings(const UEnhancedInputLocalPlayerSubsystem* Subsystem)
	{
		return Subsystem->GetUserSettings();
	}

	/**
	 * Input simulation via injection. Runs modifiers and triggers delegates as if the input had come through the underlying input system as FKeys.
	 * Applies action modifiers and triggers on top.
	 *
	 * @param Action		The Input Action to set inject input for
	 * @param RawValue		The value to set the action to
	 * @param Modifiers		The modifiers to apply to the injected input.
	 * @param Triggers		The triggers to apply to the injected input.
	 */
	UFUNCTION(ScriptCallable, meta=(AutoCreateRefTerm="Modifiers,Triggers"))
	static void InjectInputForAction(UEnhancedInputLocalPlayerSubsystem* Subsystem, const UInputAction* Action, FInputActionValue RawValue, const TArray<UInputModifier*>& Modifiers, const TArray<UInputTrigger*>& Triggers)
	{
		return Subsystem->InjectInputForAction(Action, RawValue, Modifiers, Triggers);
	}

	/**
	 * Input simulation via injection. Runs modifiers and triggers delegates as if the input had come through the underlying input system as FKeys.
	 * Applies action modifiers and triggers on top.
	 *
	 * @param Action		The Input Action to set inject input for
	 * @param Value			The value to set the action to (the type will be controlled by the Action)
	 * @param Modifiers		The modifiers to apply to the injected input.
	 * @param Triggers		The triggers to apply to the injected input.
	 */
	UFUNCTION(ScriptCallable, meta=(AutoCreateRefTerm="Modifiers,Triggers"))
	static void InjectInputVectorForAction(UEnhancedInputLocalPlayerSubsystem* Subsystem, const UInputAction* Action, FVector Value, const TArray<UInputModifier*>& Modifiers, const TArray<UInputTrigger*>& Triggers)
	{
		return Subsystem->InjectInputVectorForAction(Action, Value, Modifiers, Triggers);
	}

	/**
	 * Remove all applied mapping contexts.
	 */
	UFUNCTION(ScriptCallable, BlueprintCosmetic)
	static void ClearAllMappings(UEnhancedInputLocalPlayerSubsystem* Subsystem)
	{
		return Subsystem->ClearAllMappings();
	}
	
	/**
	 * Add a control mapping context.
	 * @param MappingContext		A set of key to action mappings to apply to this player
	 * @param Priority				Higher priority mappings will be applied first and, if they consume input, will block lower priority mappings.
	 * @param Options				Options to consider when adding this mapping context.
	 */
	UFUNCTION(ScriptCallable, BlueprintCosmetic, meta=(AutoCreateRefTerm = "Options"))
	static void AddMappingContext(UEnhancedInputLocalPlayerSubsystem* Subsystem, const UInputMappingContext* MappingContext, int32 Priority, const FModifyContextOptions& Options = FModifyContextOptions())
	{
		return Subsystem->AddMappingContext(MappingContext, Priority, Options);
	}

	/**
	* Remove a specific control context. 
	* This is safe to call even if the context is not applied.
	* @param MappingContext		Context to remove from the player
	* @param Options			Options to consider when removing this input mapping context
	*/
	UFUNCTION(ScriptCallable, BlueprintCosmetic, meta=(AutoCreateRefTerm = "Options"))
	static void RemoveMappingContext(UEnhancedInputLocalPlayerSubsystem* Subsystem, const UInputMappingContext* MappingContext, const FModifyContextOptions& Options = FModifyContextOptions())
	{
		return Subsystem->RemoveMappingContext(MappingContext, Options);
	}

	/**
	* Flag player for reapplication of all mapping contexts at the end of this frame.
	* This is called automatically when adding or removing mappings contexts.
	*
	* @param Options		Options to consider when removing this input mapping context
	*/
	UFUNCTION(ScriptCallable, BlueprintCosmetic, meta=(AutoCreateRefTerm = "Options"))
	static void RequestRebuildControlMappings(UEnhancedInputLocalPlayerSubsystem* Subsystem, const FModifyContextOptions& Options = FModifyContextOptions(), EInputMappingRebuildType RebuildType = EInputMappingRebuildType::Rebuild)
	{
		return Subsystem->RequestRebuildControlMappings(Options, RebuildType);
	}

	/**
	 * Check if a key mapping is safe to add to a given mapping context within the set of active contexts currently applied to the player controller.
	 * @param InputContext		Mapping context to which the action/key mapping is intended to be added
	 * @param Action			Action that can be triggered by the key
	 * @param Key				Key that will provide input values towards triggering the action
	 * @param OutIssues			Issues that may cause this mapping to be invalid (at your discretion). Any potential issues will be recorded, even if not present in FatalIssues.
	 * @param BlockingIssues	All issues that should be considered fatal as a bitset.
	 * @return					Summary of resulting issues.
	 * @see QueryMapKeyInContextSet
	 */
	UFUNCTION(ScriptCallable, BlueprintCosmetic)
	static EMappingQueryResult QueryMapKeyInActiveContextSet(UEnhancedInputLocalPlayerSubsystem* Subsystem, const UInputMappingContext* InputContext, const UInputAction* Action, FKey Key, TArray<FMappingQueryIssue>& OutIssues, EMappingQueryIssue BlockingIssues/* = DefaultMappingIssues::StandardFatal*/)
	{
		return Subsystem->QueryMapKeyInActiveContextSet(InputContext, Action, Key, OutIssues, BlockingIssues);
	}

	/**
	 * Check if a key mapping is safe to add to a collection of mapping contexts
	 * @param PrioritizedActiveContexts	Set of mapping contexts to test against ordered by priority such that earlier entries take precedence over later ones.
	 * @param InputContext		Mapping context to which the action/key mapping is intended to be applied. NOTE: This context must be present in PrioritizedActiveContexts.
	 * @param Action			Action that is triggered by the key
	 * @param Key				Key that will provide input values towards triggering the action
	 * @param OutIssues			Issues that may cause this mapping to be invalid (at your discretion). Any potential issues will be recorded, even if not present in FatalIssues.
	 * @param BlockingIssues	All issues that should be considered fatal as a bitset.
	 * @return					Summary of resulting issues.
	 * @see QueryMapKeyInActiveContextSet
	 */
	UFUNCTION(ScriptCallable, BlueprintCosmetic)
	static EMappingQueryResult QueryMapKeyInContextSet(UEnhancedInputLocalPlayerSubsystem* Subsystem, const TArray<UInputMappingContext*>& PrioritizedActiveContexts, const UInputMappingContext* InputContext, const UInputAction* Action, FKey Key, TArray<FMappingQueryIssue>& OutIssues, EMappingQueryIssue BlockingIssues/* = DefaultMappingIssues::StandardFatal*/)
	{
		return Subsystem->QueryMapKeyInContextSet(PrioritizedActiveContexts, InputContext, Action, Key, OutIssues, BlockingIssues);
	}

	/**
	 * Check if a mapping context is applied to this subsystem's owner.
	 */
	UFUNCTION(ScriptCallable, BlueprintCosmetic)	// TODO: BlueprintPure would be nicer. Move into library?
	static bool HasMappingContext(UEnhancedInputLocalPlayerSubsystem* Subsystem, const UInputMappingContext* MappingContext)
	{
		return Subsystem->HasMappingContext(MappingContext);
	}

	/**
	 * Returns the keys mapped to the given action in the active input mapping contexts.
	 */
	UFUNCTION(ScriptCallable, BlueprintCosmetic)
	static TArray<FKey> QueryKeysMappedToAction(UEnhancedInputLocalPlayerSubsystem* Subsystem, const UInputAction* Action)
	{
		return Subsystem->QueryKeysMappedToAction(Action);
	}
};
