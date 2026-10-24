// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectKey.h"

struct FScriptInstanceAttachmentOverride
{
	FName ParentName;
	FName SocketName;
	FObjectKey DynamicParent;
	bool bDynamicParent = false;
	bool operator!=(const FScriptInstanceAttachmentOverride& Other) const
	{
		return ParentName != Other.ParentName || SocketName != Other.SocketName
			|| bDynamicParent != Other.bDynamicParent || DynamicParent != Other.DynamicParent;
	}
};
using FScriptInstanceAttachmentOverrides = TMap<FName, FScriptInstanceAttachmentOverride>;

class AActor;

/**
 * Limits inherited component attachment repair to one script reload transaction.
 * The observer is registered before Blueprint reinstancing starts and removed
 * after the final dependent-Blueprint flush by ordinary stack lifetime.
 */
class FAngelscriptInheritedComponentAttachmentRepairScope final
{
public:
	explicit FAngelscriptInheritedComponentAttachmentRepairScope(
		const TMap<UClass*, UClass*>& ReloadClasses);
	~FAngelscriptInheritedComponentAttachmentRepairScope();

	FAngelscriptInheritedComponentAttachmentRepairScope(
		const FAngelscriptInheritedComponentAttachmentRepairScope&) = delete;
	FAngelscriptInheritedComponentAttachmentRepairScope& operator=(
		const FAngelscriptInheritedComponentAttachmentRepairScope&) = delete;

private:
	bool IsReloadedScriptBlueprintActor(const AActor* Actor) const;
	bool IsReloadedScriptActor(const AActor* Actor) const;
	void HandleObjectsReinstanced(const TMap<UObject*, UObject*>& ReinstancedObjects);
	void CaptureInstanceAttachments(const TMap<UObject*, UObject*>& ReplacedObjects);

	class FInstanceState;
	TUniquePtr<FInstanceState> InstanceState;
	TSet<UClass*> ReloadedScriptClasses;
	TMap<UClass*, UClass*> ReloadedScriptClassReplacements;
	FDelegateHandle DelegateHandle;
	FDelegateHandle CopyDelegateHandle;
	TMap<FObjectKey, FScriptInstanceAttachmentOverrides> InstanceAttachments;
	TMap<FObjectKey, FObjectKey> InstanceComponentReplacements;
	bool bOverrideCaptureFailed = false;
	const TCHAR* OverrideCaptureFailureReason = nullptr;
};
