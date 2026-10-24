#pragma once

#include "CoreMinimal.h"

#include "AngelscriptSettings.generated.h"

UENUM()
enum class EAngelscriptPropertyEditSpecifier : uint8
{
	EditAnywhere,
	EditInstanceOnly,
	EditDefaultsOnly,
	NotEditable,
};

UENUM()
enum class EAngelscriptPropertyBlueprintSpecifier : uint8
{
	BlueprintReadWrite,
	BlueprintReadOnly,
	BlueprintHidden,
};

UENUM()
enum class EAngelscriptMathNamespace : uint8
{
	// Use the Math:: namespace for math functions in angelscript
	Math UMETA(DisplayName = "Math::"),
	// Use the FMath:: namespace for math functions in angelscript
	FMath UMETA(DisplayName = "FMath::"),
};

UENUM()
enum class EAngelscriptDeprecationMode : uint8
{
	Allowed,
	Deprecated,
	Disallowed,
};

UCLASS(Config=Engine, DefaultConfig)
class ANGELSCRIPTCODE_API UAngelscriptSettings : public UObject
{
	GENERATED_BODY()
public:
	// Additional preprocessor flags which will be defined when preprocessing angelscript files.
	// Add them e.g. to BaseEngine.ini:
	//   [/Script/AngelscriptCode.AngelscriptSettings]
	//   +PreprocessorFlags="FOO"
	//   +PreprocessorFlags="BAR"
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	TArray<FString> PreprocessorFlags;

	/* Blueprint asset paths keyed by the short name used by import blueprint declarations. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript")
	TMap<FName, FString> BlueprintBindings;

	/* Whether to allow any C++ function that starts with Get...() to be accessed as a property. (Requires editor restart) */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	bool bAllowImplicitPropertyAccessors = true;

	/* Namespace to use for math library functions in angelscript */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Backwards Compatibility", Meta = (ConfigRestartRequired = true))
	EAngelscriptMathNamespace MathNamespace = EAngelscriptMathNamespace::Math;

	/* Whether UFUNCTION()s should be BlueprintCallable by default without explicit BlueprintCallable specifier. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	bool bDefaultFunctionBlueprintCallable = true;

	/* Default Edit access specifier for script UPROPERTY()s without explicit Edit specifier on classes. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	EAngelscriptPropertyEditSpecifier DefaultPropertyEditSpecifier = EAngelscriptPropertyEditSpecifier::EditAnywhere;

	/* Default Edit access specifier for script UPROPERTY()s without explicit Edit specifier on structs. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	EAngelscriptPropertyEditSpecifier DefaultPropertyEditSpecifierForStructs = EAngelscriptPropertyEditSpecifier::EditAnywhere;

	/* Default Blueprint access specifier for script UPROPERTY()s without explicit Blueprint specifier. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	EAngelscriptPropertyBlueprintSpecifier DefaultPropertyBlueprintSpecifier = EAngelscriptPropertyBlueprintSpecifier::BlueprintReadWrite;

	/* Some properties are implicitly treated as UPROPERTY:s to be seen correctly by the GC, if true this ensures such properties are marked as transient to avoid unintentional serialization */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	bool bMarkNonUpropertyPropertiesAsTransient = false;

	/* Some properties are implicitly treated as UPROPERTY:s to be seen correctly by the GC, if true this ensures such properties are marked as not replicated to avoid unintentional replication of them. Only applies to properties in structs. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	bool bMarkNonUpropertyPropertiesAsNotReplicated = false;

	/**
	 * Whether to deprecate or disallow the usage of AAnyClass::StaticClass().
	 * The newer alternative does not require the StaticClass call, the class name can be used directly.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	EAngelscriptDeprecationMode StaticClassDeprecation = EAngelscriptDeprecationMode::Deprecated;

	/* Whether we should use the ScriptName meta tag for namespaced binds if available. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Backwards Compatibility", Meta = (ConfigRestartRequired = true))
	bool bUseScriptNameForBlueprintLibraryNamespaces = true;

	/* Whether to allow actor and component classes to be instantiated using their raw constructor, instead of forcing SpawnActor or UComponent::Create() calls. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Backwards Compatibility", Meta = (ConfigRestartRequired = true))
	bool bAllowRawConstructorsForComponentsAndActors = false;

                                                                                                                     
	/* Project opt-in for the narrow stock-property preservation compatibility path. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Cuviss Studio|Compatibility", Meta = (ConfigRestartRequired = true))
	bool bEnableStockPurePropertyPreservation = false;

	/* Exact owner/property keys allowed by the project compatibility path. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Cuviss Studio|Compatibility", Meta = (ConfigRestartRequired = true))
	TArray<FString> StockPurePropertyPreservationAllowlist;
                                                                                                                   
	/* Whether to allow global functions from different script files to be used when not in a namespace. When set to false, only global function in namespaces can be used outside the file they're in. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Backwards Compatibility", Meta = (ConfigRestartRequired = true))
	bool bExposeGlobalFunctionsToOtherScriptFiles = false;

	/* Whether to mark DefaultComponents as EditDefaultsOnly by default. Otherwise, they will be VisibleAnywhere by default */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Backwards Compatibility", Meta = (ConfigRestartRequired = true))
	bool bDefaultComponentsEditable = false;

	/**
	 * Whether to deprecate the old generic methods on actors and components that have been superseded by functions with DeterminesOutputType.
	 *
	 * These are AActor::Spawn(), and various GetComponentsByClass() style functions that take an input array instead of returning one.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Backwards Compatibility", Meta = (ConfigRestartRequired = true))
	EAngelscriptDeprecationMode DeprecateOldActorGenericMethods = EAngelscriptDeprecationMode::Deprecated;

	/**
	 * Whether any code that is placed within check() asserts, Print() statements and other development-only functions should only be allowed to call const methods.
	 * This is intended to help catch issues with side-effects being placed inside expressions that get compiled out in shipping builds.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	bool bForceConstWithinDevelopmentOnlyFunctions = true;

	/* Strip these prefixes from namespaced binds. For example, when stripping "Kismet": `UKismetSystemLibrary::IsStandalone()` becomes `SystemLibrary::IsStandalone()` */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	TArray<FString> BlueprintLibraryNamespacePrefixesToStrip = {
		"UKismet",
		"UBlueprint",
	};

	/* Strip these suffixes from namespaced binds. For example, when stripping "Library": `SystemLibrary::IsStandalone()` becomes `System::IsStandalone()` */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	TArray<FString> BlueprintLibraryNamespaceSuffixesToStrip = {
		"Statics",
		"Library",
		"BlueprintLibrary",
		"BlueprintFunctionLibrary",
		"FunctionLibrary",
	};

	/**
	 * Only in editor:
	 * If a script function takes longer than this time to execute, kill it with an exception.
	 * This allows the editor to recover from accidental infinite loops, but does not work in cooked games!
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript")
	float EditorMaximumScriptExecutionTime = 1.f;

#if WITH_EDITORONLY_DATA
	/**
	 * If true, always preprocess editor-only script when running the Editor
	 * This precludes the use of -as-force-preprocess-editor-code commandline switch
	 * If this option is enabled in config, you can opt out of it by using the -as-force-disable-editor-code command line switch
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	bool bAlwaysPreprocessEditorScriptInEditor = false;
#endif // WITH_EDITORONLY_DATA

	/* In order to avoid confusion with blueprints, make the 'float' type in script resolve to 'float64'. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	bool bScriptFloatIsFloat64 = true;

	/**
	 * The port we use for communication between Unreal Engine and the Visual Studio Code plugin for auto-completion and attaching the debugger.
	 * Default is 27099. 
	 * Can be overridden by launch argument -asdebugport
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Angelscript", Meta = (ConfigRestartRequired = true))
	int ConnectionPort = 27099;
	
	/* Deprecate usage of the 'double' type in script, in favor of 'float64' or just 'float' when bScriptFloatIsFloat64. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Warnings and Errors", Meta = (ConfigRestartRequired = true))
	bool bDeprecateDoubleType = false;

	/* Emit a warning when using a float constant (eg `0.f`) to initialize a double value. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Warnings and Errors", Meta = (ConfigRestartRequired = true))
	bool bWarnOnFloatConstantsForDoubleValues = false;

	/* Emit a warning for precision loss when integer division is used. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Warnings and Errors", Meta = (ConfigRestartRequired = true))
	bool bWarnIntegerDivision = true;

	/**
	 * When opening a file in vscode by clicking a source link (for example, by clicking a Class link),
	 * always open in a vscode workspace with the Script folder as the opened workspace.
	 *
	 * You should turn this off if you're using a specific vscode workspace definition, rather than just opening the folder.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Editor")
	bool bOpenFolderOnVSCodeSourceLinks = true;

	/**
	* Sets the path to the VS Code code-workspace file. Setting this will open the workspace. 
	* This path must be relative to the project directory. 
	* Setting this will disable the `bOpenFolderOnVSCodeSourceLinks` setting.
	*/
	UPROPERTY(Config, EditDefaultsOnly, Category = "Editor")
	FString VSCodeWorkspacePath = FString("");

	/**
	 * Throw an exception when calling a function that requires a World Context to be set, but the current object is not in a world.
	 * Note: this error is only checked in editor for performance reasons.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Warnings and Errors")
	bool bErrorWhenUsingInvalidWorldContext = true;

	/**
	 * Show a warning when the result of a const method is not used.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Warnings and Errors")
	bool bWarnOnUnusedReturnValueForConstMethods = true;

	/**
	 * Show a warning when an implicit conversion between signed/unsigned integers can cause incorrect results.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Warnings and Errors")
	bool bWarnOnImplicitSignedUnsignedConversion = true;

	/**
	 * Show an error when a function or property that is editor-only is used outside of an EDITOR block or editor-only script module.
	 *
	 * Note: Can cause false positives if a function or property is declared separately in editor-only and not-editor-only blocks.
	 * In that case, put the preprocessor directives inside the function body, instead of around the whole function.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Warnings and Errors")
	bool bErrorOnIncorrectEditorOnlyCode = true;

	/**
	 * Show a warning when a comparison operator overload is implemented targeting a different type than the containing type.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Warnings and Errors")
	bool bWarnOnDivergentComparisonOperatorOverloads = true;

	/**
	 * Show a warning when an increment or decrement (++ / --) expression is used within a complex expression.
	 * Side effects are not usually expected for longer expressions and can be hard to read.
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Warnings and Errors")
	bool bWarnOnIncrementDecrementInComplexExpression = true;

	/**
	 * Script package names (/Script/ModuleName) that should be considered editor-only for the purposes of checking for incorrect usage.
	 */
	UPROPERTY(Config)
	TArray<FName> AdditionalEditorOnlyScriptPackageNames;

	/**
	 * Functions that should not be automatically evaluated by the debugger even if they are const accessors.
	 */
	UPROPERTY(Config)
	TSet<FString> DebuggerBlacklistAutomaticFunctionEvaluation;

	/**
	 * Functions that should not be automatically evaluated by the debugger even if they are const accessors, iff the object they are being called
	 * on is not currently a valid world context.
	 */
	UPROPERTY(Config)
	TSet<FString> DebuggerBlacklistAutomaticFunctionEvaluationWithoutWorldContext = {
		"AActor.GetWorldTimerManager",
		"AActor.GetGameInstance",
		"AActor.GetPhysicsVolume",
		"AActor.GetActorTimeDilation",
	};

	static UAngelscriptSettings& Get()
	{
		return *GetMutableDefault<UAngelscriptSettings>();
	}
};

