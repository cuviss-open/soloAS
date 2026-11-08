#if __has_include("Extensions/AngelscriptExtensionHooks.h")
// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio.
#include "ExtensionRegistration.h"
#include "Lambda/ExplicitCaptureLambdaExtension.h"
#include "Interfaces/ScriptInterfacesExtension.h"

#include "HAL/PlatformString.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreMisc.h"
#include "Misc/SecureHash.h"

namespace SoloASCoreExtensions
{
namespace ExtensionRegistration
{
namespace
{
struct FExtensionFactoryRegistry
{
	TArray<FExtensionFactoryEntry> Entries;
	bool bFrozen = false;
	bool bLateRegistration = false;
};

FExtensionFactoryRegistry& GetExtensionFactoryRegistry()
{
	static FExtensionFactoryRegistry Registry;
	return Registry;
}

bool IsCanonicalFeatureId(const ANSICHAR* FeatureId)
{
	if (FeatureId == nullptr)
		return false;

	const int32 Length = FCStringAnsi::Strlen(FeatureId);
	if (Length < 5 || Length > 64
		|| FeatureId[0] < 'a' || FeatureId[0] > 'z')
	{
		return false;
	}

	int32 VersionSeparator = INDEX_NONE;
	for (int32 Index = 0; Index < Length; ++Index)
	{
		const ANSICHAR Character = FeatureId[Index];
		if (Character == '/')
		{
			if (VersionSeparator != INDEX_NONE)
				return false;
			VersionSeparator = Index;
			continue;
		}
		if (VersionSeparator == INDEX_NONE
			&& !((Character >= 'a' && Character <= 'z')
				|| (Character >= '0' && Character <= '9')
				|| Character == '.' || Character == '-'))
		{
			return false;
		}
	}

	if (VersionSeparator <= 0
		|| VersionSeparator + 2 >= Length
		|| FeatureId[VersionSeparator + 1] != 'v'
		|| FeatureId[VersionSeparator - 1] == '.'
		|| FeatureId[VersionSeparator - 1] == '-')
	{
		return false;
	}
	if (FeatureId[VersionSeparator + 2] < '1'
		|| FeatureId[VersionSeparator + 2] > '9')
	{
		return false;
	}
	for (int32 Index = VersionSeparator + 3; Index < Length; ++Index)
	{
		if (FeatureId[Index] < '0' || FeatureId[Index] > '9')
			return false;
	}
	return true;
}

}

const FFeaturePolicy& GetFeaturePolicy()
{
    static const FFeaturePolicy Policy = []
    {
        FFeaturePolicy Value;
        FConfigFile Defaults;
        if (const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("SoloASCoreExtensions")))
            Defaults.Read(Plugin->GetBaseDir() / TEXT("Config/DefaultSoloASCoreExtensions.ini"));
        const auto Read = [&](const TCHAR* Key, bool& Enabled)
        {
            Defaults.GetBool(TEXT("SoloASCoreExtensions"), Key, Enabled);
            GConfig->GetBool(TEXT("SoloASCoreExtensions"), Key, Enabled, GEngineIni);
        };
        Read(TEXT("bEnableBlueprintStructs"), Value.bBlueprintStructs);
        Read(TEXT("bEnableNativeEventSuper"), Value.bNativeEventSuper);
        Read(TEXT("bEnableBlueprintInheritance"), Value.bBlueprintInheritance);
        Read(TEXT("bEnableBlueprintQuickCalls"), Value.bBlueprintQuickCalls);
        return Value;
    }();
    return Policy;
}

FString MakeCompatibilityTag(const TArray<FExtensionFactoryEntry>& Entries,
    bool bScriptInterfacesEnabled, bool bLambdaEnabled, int32 SlotsPerSite, const FFeaturePolicy& Policy)
{
	FSHA1 Hash;
	const uint8 Separator = '\n';
	for (const FExtensionFactoryEntry& Entry : Entries)
	{
		const int32 Length = FCStringAnsi::Strlen(Entry.FeatureId);
		Hash.Update(
			reinterpret_cast<const uint8*>(Entry.FeatureId),
			Length
		);
		Hash.Update(&Separator, 1);
	}
	const uint8 ScriptInterfacesEnabled = bScriptInterfacesEnabled ? 1 : 0;
	Hash.Update(&ScriptInterfacesEnabled, 1);
	const uint8 LambdaEnabled = bLambdaEnabled ? 1 : 0;
	Hash.Update(&LambdaEnabled, 1);
    const uint8 LambdaSlots = uint8(SlotsPerSite);
    Hash.Update(&LambdaSlots, 1);
    const uint8 Features[] = {uint8(Policy.bBlueprintStructs), uint8(Policy.bNativeEventSuper),
        uint8(Policy.bBlueprintInheritance), uint8(Policy.bBlueprintQuickCalls)};
    Hash.Update(Features, UE_ARRAY_COUNT(Features));
	Hash.Final();
	uint8 Digest[FSHA1::DigestSize];
	Hash.GetHash(Digest);
	return TEXT("soloas-extensions-v1-")
		+ BytesToHex(Digest, FSHA1::DigestSize).ToLower();
}

FExtensionRegistrar::FExtensionRegistrar(
	const ANSICHAR* FeatureId,
	FExtensionFactory Factory
)
{
	FExtensionFactoryRegistry& Registry = GetExtensionFactoryRegistry();
	if (Registry.bFrozen)
	{
		Registry.bLateRegistration = true;
		return;
	}
	Registry.Entries.Emplace(FExtensionFactoryEntry{FeatureId, Factory});
}

bool SnapshotAndFreezeExtensionFactories(
	FExtensionFactorySnapshot& OutSnapshot,
	FString& OutError
)
{
	OutSnapshot.Entries.Reset();
	OutSnapshot.CompatibilityTag.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Extension factory snapshot requires the game thread.");
		return false;
	}

	FExtensionFactoryRegistry& Registry = GetExtensionFactoryRegistry();
	if (Registry.bFrozen)
	{
		OutError = Registry.bLateRegistration
			? TEXT("A late extension factory registration was rejected.")
			: TEXT("Extension factory registry is frozen for this process; restart to change extensions.");
		return false;
	}
	Registry.bFrozen = true;
	OutSnapshot.Entries = Registry.Entries;
	OutSnapshot.Entries.Sort(
		[](const FExtensionFactoryEntry& Left,
			const FExtensionFactoryEntry& Right)
		{
			if (Left.FeatureId == nullptr)
				return Right.FeatureId != nullptr;
			if (Right.FeatureId == nullptr)
				return false;
			return FCStringAnsi::Strcmp(
				Left.FeatureId,
				Right.FeatureId
			) < 0;
		});

	for (int32 LeftIndex = 0;
		LeftIndex < OutSnapshot.Entries.Num();
		++LeftIndex)
	{
		for (int32 RightIndex = LeftIndex + 1;
			RightIndex < OutSnapshot.Entries.Num();
			++RightIndex)
		{
			const ANSICHAR* LeftId =
				OutSnapshot.Entries[LeftIndex].FeatureId;
			const ANSICHAR* RightId =
				OutSnapshot.Entries[RightIndex].FeatureId;
			if (LeftId != nullptr
				&& RightId != nullptr
				&& FCStringAnsi::Stricmp(LeftId, RightId) == 0)
			{
				OutError = TEXT("Extension feature IDs conflict by ASCII case.");
				OutSnapshot.Entries.Reset();
				return false;
			}
		}
	}
	for (const FExtensionFactoryEntry& Entry : OutSnapshot.Entries)
	{
		if (!IsCanonicalFeatureId(Entry.FeatureId))
		{
			OutError = TEXT("Extension feature ID is not canonical ASCII with a version.");
			OutSnapshot.Entries.Reset();
			return false;
		}
		if (Entry.Factory == nullptr)
		{
			OutError = TEXT("Extension feature factory is null.");
			OutSnapshot.Entries.Reset();
			return false;
		}
	}

	if (!OutSnapshot.Entries.IsEmpty())
		OutSnapshot.CompatibilityTag = MakeCompatibilityTag(OutSnapshot.Entries,
            ScriptInterfaces::IsEnabled(), SoloASExplicitCaptureLambdaPrivate::IsEnabled(),
            SoloASExplicitCaptureLambdaPrivate::GetSlotsPerSite(), GetFeaturePolicy());
	return true;
}
}
}

namespace SoloASCoreExtensions::ExtensionRegistration
{
bool UnregisterExtensionCallbacksReverse(
    TArray<FAngelscriptExtensionCallbackRegistration>& Tokens, FString& OutError)
{
    OutError.Reset();
    // Keep the failed token and every earlier owner available for a safe retry.
    while (!Tokens.IsEmpty())
    {
        if (!UnregisterAngelscriptExtensionCallback(Tokens.Last(), OutError))
            return false;
        Tokens.Pop();
    }
    return true;
}

bool RegisterExtensionCallbacks(const FExtensionFactorySnapshot& Snapshot,
    TArray<FAngelscriptExtensionCallbackRegistration>& Tokens, FString& OutError)
{
    OutError.Reset();
    if (!Tokens.IsEmpty())
    {
        OutError = TEXT("Extension callback output already owns registrations.");
        return false;
    }
    for (const FExtensionFactoryEntry& Entry : Snapshot.Entries)
    {
        if (!GetFeaturePolicy().bNativeEventSuper
            && FCStringAnsi::Strcmp(Entry.FeatureId, "soloas.native-event-super/v1") == 0)
            continue;
        auto Token = Entry.Factory();
        if (Token.IsValid())
        {
            Tokens.Emplace(MoveTemp(Token));
            continue;
        }
        FString RollbackError;
        const bool bRolledBack = UnregisterExtensionCallbacksReverse(Tokens, RollbackError);
        OutError = FString::Printf(TEXT("Extension factory '%s' returned an empty token.%s%s"),
            ANSI_TO_TCHAR(Entry.FeatureId), bRolledBack ? TEXT("") : TEXT(" Rollback failed: "), *RollbackError);
        return false;
    }
    return true;
}
}

namespace SoloASCoreExtensions::ExtensionRegistration
{
EAngelscriptExtensionResult DispatchCoreTypeGeneration(
    FAngelscriptTypeGenerationContext& Context,
    FAngelscriptTypeGenerationExtensionCallback Interfaces,
    FAngelscriptTypeGenerationExtensionCallback BlueprintStructs,
    bool bInterfacesEnabled, bool bBlueprintStructsEnabled)
{
    // Shared points retain their own policy checks and disabled-feature diagnostics.
    const bool bInterfaceTypePoint = Context.Point == EAngelscriptTypeGenerationPoint::ReferenceType
        || Context.Point == EAngelscriptTypeGenerationPoint::ResolveFunction
        || Context.Point == EAngelscriptTypeGenerationPoint::ConfigureClass;
    if (!bBlueprintStructsEnabled && Context.Point == EAngelscriptTypeGenerationPoint::RestoreCachedTypes)
    {
        for (const auto& Cached : *Context.CachedTypes)
        {
            if (Cached.Metadata.Contains(FName(TEXT("SoloASBlueprintStructSchema")))
                || Cached.ClassName.StartsWith(TEXT("U__SoloASBlueprintStructSchema_")))
            {
                Context.Error = TEXT("Cached Blueprint structs require bEnableBlueprintStructs=true; "
                    "restart and regenerate the script cache with matching [SoloASCoreExtensions] settings.");
                return EAngelscriptExtensionResult::Error;
            }
        }
    }
    struct FHandler
    {
        FAngelscriptTypeGenerationExtensionCallback Callback;
        bool bEnabled;
    };
    const FHandler Handlers[] = {
        {BlueprintStructs, bBlueprintStructsEnabled},
        {Interfaces, bInterfacesEnabled || !bInterfaceTypePoint}
    };
    auto Result = EAngelscriptExtensionResult::NotHandled;
    for (const auto& Handler : Handlers)
    {
        if (!Handler.bEnabled) continue;
        const auto Current = Handler.Callback(Context);
        if (Current == EAngelscriptExtensionResult::Error) return Current;
        if (Current == EAngelscriptExtensionResult::Handled) Result = Current;
    }
    return Result;
}
}

#else
#endif
