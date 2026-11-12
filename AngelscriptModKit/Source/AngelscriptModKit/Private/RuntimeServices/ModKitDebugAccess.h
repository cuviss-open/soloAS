// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
                                                                       

#pragma once

#include "CoreMinimal.h"
#include "AngelscriptCoreExtensionPoints.h"
#include "UObject/Object.h"
#include "ModKitDebugAccess.generated.h"

USTRUCT()
struct FModKitDebugAccessRule
{
	GENERATED_BODY()

	/** Exact Angelscript module name, for example FeatureMods.MyMod.Script.Actor. */
	UPROPERTY(EditAnywhere, Category = "AngelscriptModKit|Debug Access")
	FString ModuleName;

	/** Exact project-relative script source identity, normalized with '/'. */
	UPROPERTY(EditAnywhere, Category = "AngelscriptModKit|Debug Access")
	FString SourceIdentity;

	/** Exact operation tokens from FAngelscriptModdingDebugMessageAccessGate. */
	UPROPERTY(EditAnywhere, Category = "AngelscriptModKit|Debug Access")
	TArray<FString> AllowedOperations;
};

UCLASS(Config = Engine, DefaultConfig)
class UModKitDebugAccessSettings : public UObject
{
	GENERATED_BODY()

public:
	/** Empty is deny-all when ModKit is active. No wildcard identities are supported. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit|Debug Access")
	TArray<FModKitDebugAccessRule> DebugAccessRules;

	/** Operations explicitly permitted for global protocol messages. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit|Debug Access")
	TArray<FString> GlobalAllowedOperations;

	/** Operations explicitly permitted for native debug data. */
	UPROPERTY(Config, EditAnywhere, Category = "AngelscriptModKit|Debug Access")
	TArray<FString> NativeAllowedOperations;
};

class FModKitDebugAccess
{
public:
	static void Startup();
	static void Shutdown();
	static ANGELSCRIPTMODKIT_API bool IsAllowed(const AngelscriptCoreExtensionPoints::FDebugAccessView& Context);
	static bool IsRetained();
#if WITH_DEV_AUTOMATION_TESTS
	static bool ConfigureForTest(const TArray<FModKitDebugAccessRule>& Rules,
		const TArray<FString>& GlobalOperations, const TArray<FString>& NativeOperations);
	static bool DenyGlobalOperationForTest(const FString& Operation);
	static AngelscriptCoreExtensionPoints::ERemoveResult RevokeForTest();
#endif
};
