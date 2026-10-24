// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"

namespace AngelscriptAttachmentCaller
{
struct FStats
{
	int32 Executed = 0;
	int32 Queued = 0;
	int32 Drained = 0;
	int32 Rejections = 0;
};

/**
 * Lifecycle transaction for the script component Attach entry.
 * Nested requests execute synchronously unless the active native frame would
 * dereference a cleared parent, overwrite a nested detach/attach, or detach a
 * registered component before its reciprocal child edge has been published.
 * Queued identities are weak; active native frames pin their operands across GC.
 * This scope prevents no GC/reload and offers no rollback.
 */
class ANGELSCRIPTCODE_API FScope final
{
public:
	static constexpr int32 MaxRequests = 32;
	explicit FScope(const TArray<USceneComponent*>& AllowedComponents);
	FScope(const TArray<USceneComponent*>& AllowedComponents, bool bAllowAdditionalComponents);
	~FScope();
	FScope(const FScope&) = delete;
	FScope& operator=(const FScope&) = delete;
	bool IsAdmitted() const;
	bool HasFailed() const;
	const FStats& GetStats() const;

private:
	struct FState;
	TUniquePtr<FState> State;
	friend ANGELSCRIPTCODE_API bool TryHandle(USceneComponent*, USceneComponent*,
		const FName&, EAttachmentRule, EAttachmentRule);
	friend ANGELSCRIPTCODE_API void Detach(USceneComponent*, EDetachmentRule,
		EDetachmentRule, EDetachmentRule, bool);
};

// Establishes a scope automatically for an ordinary script call.
// True means the transaction consumed the request, including a refusal.
ANGELSCRIPTCODE_API bool TryHandle(USceneComponent* Component, USceneComponent* Parent,
	const FName& SocketName, EAttachmentRule AttachmentRule, EAttachmentRule ScaleRule);
ANGELSCRIPTCODE_API void Detach(USceneComponent* Component, EDetachmentRule LocationRule,
	EDetachmentRule RotationRule, EDetachmentRule ScaleRule, bool bCallModify);
}
