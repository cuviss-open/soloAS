// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptAttachmentCallerScope.h"

#include <atomic>
#include "Runtime/SoloASSceneComponentAttachment.h"
#include "ClassGenerator/AngelscriptClassGenerator.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectThreadContext.h"

namespace AngelscriptAttachmentCaller
{
struct FScope::FState
{
	struct FAllowed
	{
		USceneComponent* Identity = nullptr;
		TWeakObjectPtr<USceneComponent> Component;
		TWeakObjectPtr<UClass> Class;
		TWeakObjectPtr<AActor> Owner;
		TWeakObjectPtr<UClass> OwnerClass;
		bool bHadBegunPlay = false;
		bool bOwnerHadBegunPlay = false;
	};
	struct FRequest
	{
		int32 ComponentIndex;
		int32 ParentIndex;
		FName SocketName;
		EAttachmentRule AttachmentRule;
		EAttachmentRule ScaleRule;
		bool bDetach = false;
		EDetachmentRule DetachLocationRule = EDetachmentRule::KeepRelative;
		EDetachmentRule DetachRotationRule = EDetachmentRule::KeepRelative;
		EDetachmentRule DetachScaleRule = EDetachmentRule::KeepRelative;
		bool bCallModify = true;
	};
	struct FNativeFrame
	{
		int32 ComponentIndex;
		bool bDetach;
	};

	static thread_local FState* Top;
	FState* Previous = nullptr;
	TArray<FAllowed> Allowed;
	TArray<FRequest> Queue;
	TArray<FNativeFrame> NativeFrames;
	TArray<TWeakObjectPtr<USceneComponent>> ObservedParents;
	TWeakObjectPtr<UWorld> World;
	UWorld* WorldIdentity = nullptr;
	FStats Stats;
	int32 Requests = 0;
	bool bAdmitted = false;
	bool bAllowAdditionalComponents = false;
	std::atomic<bool> bFailed{false};
	FDelegateHandle PreGc;
	FDelegateHandle ClassReload;
	FDelegateHandle FullReload;
#if WITH_EDITOR
	FDelegateHandle ObjectsReplaced;
	FDelegateHandle ObjectsReinstanced;
#endif
	FDelegateHandle WorldCleanup;

	void Fail()
	{
		bFailed.store(true, std::memory_order_release);
	}

	bool Validate() const
	{
		if (!IsInGameThread() || bFailed.load(std::memory_order_acquire) || IsGarbageCollecting()) return false;
		UWorld* CurrentWorld = World.Get();
		if (!IsValid(CurrentWorld) || CurrentWorld->bIsTearingDown) return false;
		for (const FAllowed& Item : Allowed)
		{
			USceneComponent* Component = Item.Component.Get();
			UClass* Class = Item.Class.Get();
			AActor* Owner = Item.Owner.Get();
			UClass* OwnerClass = Item.OwnerClass.Get();
			if (Component == nullptr || Component != Item.Identity || Class == nullptr || Owner == nullptr
				|| OwnerClass == nullptr || Owner->GetClass() != OwnerClass
				|| Component->GetClass() != Class || Component->GetOwner() != Owner
				|| Component->GetWorld() != CurrentWorld || Owner->GetWorld() != CurrentWorld
				|| Component->IsTemplate() || Component->IsUnreachable() || Owner->IsUnreachable()
				|| Component->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed | RF_NeedLoad | RF_NeedPostLoad)
				|| Owner->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed | RF_NeedLoad | RF_NeedPostLoad)
				|| Component->IsBeingDestroyed() || Owner->IsActorBeingDestroyed()
				|| (Item.bHadBegunPlay && !Component->HasBegunPlay())
				|| (Item.bOwnerHadBegunPlay && !Owner->HasActorBegunPlay())) return false;
			for (UClass* Root : {Class, OwnerClass})
			{
				for (UClass* Current = Root; Current != nullptr; Current = Current->GetSuperClass())
				{
					if (!IsValid(Current) || Current->IsUnreachable()
						|| Current->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
						|| Current->HasAnyClassFlags(CLASS_NewerVersionExists | CLASS_Deprecated)
						|| Current->bLayoutChanging) return false;
				}
			}
		}
		return true;
	}

	bool Add(USceneComponent* Component)
	{
		if (Find(Component) != INDEX_NONE) return true;
		if (!IsValid(Component) || Allowed.Num() >= FScope::MaxRequests) return false;
		AActor* Owner = Component->GetOwner();
		if (!IsValid(Owner)) return false;
		Allowed.Add({Component, Component, Component->GetClass(), Owner, Owner->GetClass(),
			Component->HasBegunPlay(), Owner->HasActorBegunPlay()});
		return true;
	}

	int32 Find(USceneComponent* Identity) const
	{
		// Raw identity is used only to locate the captured weak serial.
		return Allowed.IndexOfByPredicate([Identity](const FAllowed& Item)
			{ return Item.Identity == Identity; });
	}

	void ObserveParent(USceneComponent* Parent)
	{
		if (IsValid(Parent)) ObservedParents.AddUnique(Parent);
	}

	bool MustDefer(const FRequest& Request) const
	{
		if (NativeFrames.IsEmpty()) return false;
		for (const FNativeFrame& Frame : NativeFrames)
		{
			USceneComponent* Active = Allowed[Frame.ComponentIndex].Component.Get();
			USceneComponent* Requested = Allowed[Request.ComponentIndex].Component.Get();
			// Active Attach reads its current parent again after its callback. Active
			// Detach clears the relationship and writes transforms after its callback.
			if (Active == Requested)
			{
				if (Request.bDetach && Active->GetAttachParent() != nullptr) return true;
				if (!Request.bDetach && (Frame.bDetach || Active->GetAttachParent() == nullptr)) return true;
			}
		}
		USceneComponent* Component = Allowed[Request.ComponentIndex].Component.Get();
		USceneComponent* CurrentParent = Component ? Component->GetAttachParent() : nullptr;
		// Stock/Haze publish AttachParent before the reciprocal edge. A registered
		// nested native detach in that window is an illegal state (native ensure).
		return Component && Component->IsRegistered() && CurrentParent
			&& !CurrentParent->GetAttachChildren().Contains(Component);
	}

	void Execute(const FRequest& Request, bool bDraining)
	{
		if (!Validate()) { ++Stats.Rejections; Fail(); return; }
		USceneComponent* Component = Allowed[Request.ComponentIndex].Component.Get();
		USceneComponent* Parent = Request.bDetach ? nullptr : Allowed[Request.ParentIndex].Component.Get();
		ObserveParent(Component->GetAttachParent());
		ObserveParent(Parent);
		// Native attach/detach keep raw pointers across synchronous user callbacks.
		TStrongObjectPtr<USceneComponent> ActiveComponent(Component);
		TStrongObjectPtr<USceneComponent> ActiveParent(Parent);
		TStrongObjectPtr<USceneComponent> PreviousParent(Component->GetAttachParent());
		TStrongObjectPtr<AActor> ActiveOwner(Component->GetOwner());
		TStrongObjectPtr<AActor> ActiveParentOwner(Parent ? Parent->GetOwner() : nullptr);
		const int32 PreviousDepth = NativeFrames.Num();
		NativeFrames.Add({Request.ComponentIndex, Request.bDetach});
		++Stats.Executed;
		if (bDraining) ++Stats.Drained;
		bool bAttached = true;
		if (Request.bDetach)
			Component->K2_DetachFromComponent(Request.DetachLocationRule, Request.DetachRotationRule,
				Request.DetachScaleRule, Request.bCallModify);
		else
			bAttached = Component->K2_AttachToComponent(Parent, Request.SocketName,
				Request.AttachmentRule, Request.AttachmentRule, Request.ScaleRule, false);
		check(NativeFrames.Num() == PreviousDepth + 1);
		NativeFrames.Pop(EAllowShrinking::No);
		if (!bAttached || !Validate()) { Fail(); return; }
		ObserveParent(Component->GetAttachParent());
		for (int32 Index = 0; Index < ObservedParents.Num(); ++Index)
		{
			const TWeakObjectPtr<USceneComponent> Key = ObservedParents[Index];
			USceneComponent* CapturedParent = Key.Get();
			if (!Validate() || !CapturedParent
				|| !SoloASBridge::NormalizeSceneComponentChildren(CapturedParent)) { Fail(); return; }
		}
		if (!Validate()) Fail();
	}

	void Dispatch(const FRequest& Request)
	{
		if (MustDefer(Request))
		{
			Queue.Add(Request);
			++Stats.Queued;
			return;
		}
		Execute(Request, false);
		if (!NativeFrames.IsEmpty()) return;
		for (int32 Index = 0; Index < Queue.Num() && !bFailed.load(std::memory_order_acquire); ++Index)
		{
			const FRequest Pending = Queue[Index];
			Execute(Pending, true);
		}
		Queue.Reset();
	}
};

thread_local FScope::FState* FScope::FState::Top = nullptr;

FScope::FScope(const TArray<USceneComponent*>& AllowedComponents)
	: FScope(AllowedComponents, false)
{
}

FScope::FScope(const TArray<USceneComponent*>& AllowedComponents, bool bAllowAdditionalComponents)
	: State(MakeUnique<FState>())
{
	State->bAllowAdditionalComponents = bAllowAdditionalComponents;
	State->Previous = FState::Top;
	FState::Top = State.Get();
	if (State->Previous != nullptr)
	{
		State->Previous->Fail();
		State->Fail();
		return;
	}
	if (!IsInGameThread() || IsGarbageCollecting() || AllowedComponents.IsEmpty()
		|| AllowedComponents.Num() > MaxRequests)
	{
		State->Fail();
		return;
	}
	for (USceneComponent* Component : AllowedComponents)
	{
		if (!IsValid(Component) || State->Find(Component) != INDEX_NONE)
		{
			State->Fail();
			return;
		}
		if (State->Allowed.IsEmpty())
		{
			State->World = Component->GetWorld();
			State->WorldIdentity = Component->GetWorld();
		}
		AActor* Owner = Component->GetOwner();
		if (!IsValid(Owner)) { State->Fail(); return; }
		State->Allowed.Add({Component, Component, Component->GetClass(), Owner, Owner->GetClass(),
			Component->HasBegunPlay(), Owner->HasActorBegunPlay()});
	}
	if (!State->Validate()) { State->Fail(); return; }
	State->bAdmitted = true;
	FState* Captured = State.Get();
	State->PreGc = FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddLambda(
		[Captured]() { Captured->Fail(); });
	State->ClassReload = FAngelscriptClassGenerator::OnClassReload.AddLambda(
		[Captured](UClass*, UClass*) { Captured->Fail(); });
	State->FullReload = FAngelscriptClassGenerator::OnFullReload.AddLambda(
		[Captured]() { Captured->Fail(); });
#if WITH_EDITOR
	State->ObjectsReplaced = FCoreUObjectDelegates::OnObjectsReplaced.AddLambda(
		[Captured](const TMap<UObject*, UObject*>&) { Captured->Fail(); });
	State->ObjectsReinstanced = FCoreUObjectDelegates::OnObjectsReinstanced.AddLambda(
		[Captured](const TMap<UObject*, UObject*>&) { Captured->Fail(); });
#endif
	State->WorldCleanup = FWorldDelegates::OnWorldCleanup.AddLambda(
		[Captured](UWorld* CleaningWorld, bool, bool)
		{
			if (Captured->WorldIdentity == CleaningWorld) Captured->Fail();
		});
}

FScope::~FScope()
{
	if (State->bAdmitted)
	{
		check(IsInGameThread());
		FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(State->PreGc);
		FAngelscriptClassGenerator::OnClassReload.Remove(State->ClassReload);
		FAngelscriptClassGenerator::OnFullReload.Remove(State->FullReload);
#if WITH_EDITOR
		FCoreUObjectDelegates::OnObjectsReplaced.Remove(State->ObjectsReplaced);
		FCoreUObjectDelegates::OnObjectsReinstanced.Remove(State->ObjectsReinstanced);
#endif
		FWorldDelegates::OnWorldCleanup.Remove(State->WorldCleanup);
	}
	check(FState::Top == State.Get());
	FState::Top = State->Previous;
}

bool FScope::IsAdmitted() const { return State->bAdmitted; }
bool FScope::HasFailed() const { return State->bFailed.load(std::memory_order_acquire); }
const FStats& FScope::GetStats() const { return State->Stats; }

bool TryHandle(USceneComponent* Component, USceneComponent* Parent, const FName& SocketName,
	EAttachmentRule AttachmentRule, EAttachmentRule ScaleRule)
{
	FScope::FState* State = FScope::FState::Top;
	if (State == nullptr)
	{
		// The lifetime covers the native call, its callbacks, and the queued drain.
		FScope Scope({Component, Parent}, true);
		return TryHandle(Component, Parent, SocketName, AttachmentRule, ScaleRule);
	}
	if (State->bAllowAdditionalComponents && (!State->Add(Component) || !State->Add(Parent)))
	{
		++State->Stats.Rejections;
		State->Fail();
		return true;
	}
	const int32 ComponentIndex = State->Find(Component);
	const int32 ParentIndex = State->Find(Parent);
	const bool bRulesValid = (AttachmentRule == EAttachmentRule::KeepRelative
		|| AttachmentRule == EAttachmentRule::KeepWorld || AttachmentRule == EAttachmentRule::SnapToTarget)
		&& (ScaleRule == EAttachmentRule::KeepRelative || ScaleRule == EAttachmentRule::KeepWorld
			|| ScaleRule == EAttachmentRule::SnapToTarget);
	if (!State->bAdmitted || !State->Validate() || ComponentIndex == INDEX_NONE
		|| ParentIndex == INDEX_NONE || ComponentIndex == ParentIndex || !bRulesValid
		|| ++State->Requests > FScope::MaxRequests
		|| FUObjectThreadContext::Get().IsInConstructor)
	{
		++State->Stats.Rejections;
		State->Fail();
		return true;
	}
	const FScope::FState::FRequest Request{ComponentIndex, ParentIndex, SocketName, AttachmentRule, ScaleRule};
	State->Dispatch(Request);
	return true;
}

void Detach(USceneComponent* Component, EDetachmentRule LocationRule,
	EDetachmentRule RotationRule, EDetachmentRule ScaleRule, bool bCallModify)
{
	FScope::FState* State = FScope::FState::Top;
	if (State == nullptr)
	{
		FScope Scope({Component}, true);
		Detach(Component, LocationRule, RotationRule, ScaleRule, bCallModify);
		return;
	}
	if (State->bAllowAdditionalComponents && !State->Add(Component))
	{
		++State->Stats.Rejections; State->Fail(); return;
	}
	const int32 Index = State->Find(Component);
	auto ValidRule = [](EDetachmentRule Rule)
	{
		return Rule == EDetachmentRule::KeepRelative || Rule == EDetachmentRule::KeepWorld;
	};
	if (!State->bAdmitted || !State->Validate() || Index == INDEX_NONE
		|| !ValidRule(LocationRule) || !ValidRule(RotationRule) || !ValidRule(ScaleRule)
		|| ++State->Requests > FScope::MaxRequests || FUObjectThreadContext::Get().IsInConstructor)
	{
		++State->Stats.Rejections; State->Fail(); return;
	}
	FScope::FState::FRequest Request{Index, INDEX_NONE, NAME_None,
		EAttachmentRule::KeepRelative, EAttachmentRule::KeepRelative};
	Request.bDetach = true;
	Request.DetachLocationRule = LocationRule;
	Request.DetachRotationRule = RotationRule;
	Request.DetachScaleRule = ScaleRule;
	Request.bCallModify = bCallModify;
	State->Dispatch(Request);
}

}
