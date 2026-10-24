#include "Runtime/SoloASBridgeObjectLifecycleSupport.h"

#include "Misc/ScopeLock.h"
#include "UObject/Object.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(
    LogSoloASBridgeObjectLifecycle,
    Log,
    All
);

namespace SoloASBridge::Private
{
FCriticalSection LifecycleMutex;
TMap<FObjectKey, FTrackedEntry> Entries;
FRuntimeDestroyCallback DestroyCallback = nullptr;
FRuntimeOwnerReferenceCallback OwnerReferenceCallback = nullptr;
FRuntimeDestroyPrepare DestroyPrepare = nullptr;
FRuntimeDestroyControlCallback DestroyControl = nullptr;
uint64 NextClaimSequence = 0;
uint64 CallbackGeneration = 0;
FDelegateHandle PostReachabilityHandle;
FDelegateHandle PreBeginDestroyHandle;
int32 DispatchingCount = 0;
bool bDispatchInProgress = false;
thread_local bool bExitPurgeRuntimeDestroyCallback = false;
bool bDeleteListenerRegistered = false;
bool bDeleteListenerUnregistering = false;

namespace
{
bool RemoveLateIdentity(const UObjectBase* Object)
{
    FScopeLock Lock(&LifecycleMutex);
    for (
        auto EntryIterator = Entries.CreateIterator();
        EntryIterator;
        ++EntryIterator
    )
    {
        const FTrackedEntry& Entry = EntryIterator.Value();
        if (Entry.ObjectIdentity == Object)
        {
            const bool bMissedPreDestroy =
                Entry.State == ETrackedState::Pending
                || Entry.State == ETrackedState::Deferred
                || Entry.State == ETrackedState::Completing
                || Entry.State == ETrackedState::Dispatching;
            EntryIterator.RemoveCurrent();
            return bMissedPreDestroy;
        }
    }
    return false;
}

class FLateDeleteSentinel final
    : public FUObjectArray::FUObjectDeleteListener
{
public:
    void NotifyUObjectDeleted(
        const UObjectBase* Object,
        int32 Index
    ) override
    {
        (void)Index;
        if (RemoveLateIdentity(Object) && SOLOAS_LEGACY_TERMINAL_LIFECYCLE)
        {
            UE_LOG(
                LogSoloASBridgeObjectLifecycle,
                Fatal,
                TEXT(
                    "MISSED_PREDESTROY: tracked object reached "
                    "late deletion before Runtime destruction"
                )
            );
        }
    }

    void OnUObjectArrayShutdown() override
    {
        if (bDeleteListenerRegistered)
        {
            {
                FScopeLock Lock(&LifecycleMutex);
                if (bDeleteListenerUnregistering)
                {
                    return;
                }
                checkf(
                    Entries.Num() == 0,
                    TEXT(
                        "LIVE_ENTRIES: UObject array shutdown with tracked objects"
                    )
                );
                if (Entries.Num() != 0)
                {
                    UE_LOG(
                        LogSoloASBridgeObjectLifecycle,
                        Fatal,
                        TEXT(
                            "LIVE_ENTRIES: UObject array shutdown with tracked objects"
                        )
                    );
                }
                bDeleteListenerUnregistering = true;
            }

            GUObjectArray.RemoveUObjectDeleteListener(this);

            {
                FScopeLock Lock(&LifecycleMutex);
                bDeleteListenerRegistered = false;
                bDeleteListenerUnregistering = false;
            }
        }
    }
};

FLateDeleteSentinel LateDeleteSentinel;
}

void FailIfOffGameThread(const TCHAR* Operation)
{
    if (!IsInGameThread())
    {
        UE_LOG(
            LogSoloASBridgeObjectLifecycle,
            Fatal,
            TEXT("%s requires the game thread"),
            Operation
        );
    }
}

void DispatchPreBeginDestroyUnreachable()
{
    // Incremental Gather can mark receivers unreachable after PostReachabilityAnalysis.
    // The second dispatch considers Pending entries only, including ordinary GC.
    DispatchUnreachable();
}

void DispatchUnreachable()
{
    checkf(IsInGameThread(), TEXT("callback requires game thread"));
    FailIfOffGameThread(TEXT("Runtime destroy callback"));

    TArray<FObjectKey> CandidateKeys;
    const TCHAR* DispatchFailure = nullptr;
    {
        FScopeLock Lock(&LifecycleMutex);
        if (bDispatchInProgress) DispatchFailure = TEXT("GC_REENTRY: nested Runtime lifecycle dispatch");
        else if (!DestroyCallback) DispatchFailure = TEXT("CALLBACK_MISSING: lifecycle dispatch has no callback");
        else
        {
            bDispatchInProgress = true;
            CandidateKeys.Reserve(Entries.Num());
            for (const TPair<FObjectKey, FTrackedEntry>& Pair : Entries)
                if (Pair.Value.State == ETrackedState::Pending) CandidateKeys.Add(Pair.Key);
        }
    }
    if (DispatchFailure) UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("%s"), DispatchFailure);

    for (const FObjectKey& Key : CandidateKeys)
    {
        FRuntimeDestroyPreclaim Snapshot;
        FRuntimeDestroyCallback Callback = nullptr;
        FRuntimeDestroyPrepare Prepare = nullptr;
        FRuntimeDestroyControlCallback Control = nullptr;
        const TCHAR* PreclaimFailure = nullptr;
        {
            FScopeLock Lock(&LifecycleMutex);
            FTrackedEntry* Entry = Entries.Find(Key);
            if (!Entry || Entry->State != ETrackedState::Pending) continue;
            UObject* Object = Entry->Key.ResolveObjectPtrEvenIfUnreachable();
            if (!Object || !Object->IsUnreachable()
                || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) continue;
            if (Entry->ObjectIdentity != Object || !Entry->TypeCookie
                || Entry->Generation != CallbackGeneration || !CallbackGeneration)
                PreclaimFailure = TEXT("GENERATION_MISMATCH: invalid preclaim identity");
            else if (++NextClaimSequence == 0) PreclaimFailure = TEXT("CLAIM_SEQUENCE_EXHAUSTED");
            else
            {
                Entry->ClaimSequence = NextClaimSequence;
                Snapshot = {Key, Object, Entry->TypeCookie, Entry->Generation, Entry->ClaimSequence};
                Callback = DestroyCallback;
                Prepare = DestroyPrepare;
                Control = DestroyControl;
            }
        }
        if (PreclaimFailure) UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("%s"), PreclaimFailure);
        FRuntimeDeferredDestroyToken PreparedToken;
        ERuntimeDestroyPrepareResult Result = ERuntimeDestroyPrepareResult::NotApplicable;
        // The writer, qualification and module pin are acquired outside LifecycleMutex.
        if (Prepare) Result = Prepare(Snapshot, PreparedToken);
        if (Result == ERuntimeDestroyPrepareResult::InvalidIdentity)
            UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_PRECLAIM_INVALID_IDENTITY_NO_RETRY"));
        const bool bPrepared = Result == ERuntimeDestroyPrepareResult::Prepared;
        bool bSnapshotMatches = false;
        {
            FScopeLock Lock(&LifecycleMutex);
            FTrackedEntry* Entry = Entries.Find(Key);
            bSnapshotMatches = Entry && Entry->State == ETrackedState::Pending
                && Entry->Key == Snapshot.Key && Entry->ObjectIdentity == Snapshot.Object
                && Entry->Key.ResolveObjectPtrEvenIfUnreachable() == Snapshot.Object
                && Entry->TypeCookie == Snapshot.TypeCookie && Entry->Generation == Snapshot.Generation
                && Entry->ClaimSequence == Snapshot.ClaimSequence && CallbackGeneration == Snapshot.Generation
                && DestroyCallback == Callback && DestroyPrepare == Prepare && DestroyControl == Control;
            if (bPrepared)
                bSnapshotMatches = bSnapshotMatches && Control && PreparedToken.Key == Snapshot.Key
                    && PreparedToken.Object == Snapshot.Object && PreparedToken.TypeCookie == Snapshot.TypeCookie
                    && PreparedToken.Generation == Snapshot.Generation
                    && PreparedToken.ClaimSequence == Snapshot.ClaimSequence
                    && PreparedToken.TrackSequence != 0 && PreparedToken.PlanId != 0;
            if (bSnapshotMatches && bPrepared)
            {
                Entry->DeferredToken = PreparedToken;
                Entry->DeferredControl = Control;
                Entry->State = ETrackedState::Deferred;
            }
        }
        if (!bSnapshotMatches)
        {
            if (bPrepared && (!Control || !Control(PreparedToken, ERuntimeDestroyControl::AbortPrepared)))
                UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_PREPARED_ABORT_FAILED_NO_RETRY"));
            UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_DEFER_IDENTITY_CHANGED"));
        }
        if (bPrepared)
        {
            if (!Control(PreparedToken, ERuntimeDestroyControl::CommitPrepared))
                UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_COMMIT_FAILED_NO_RETRY"));
            continue;
        }
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
        FRuntimeTerminalDestroyTokenForTest TerminalToken;
        FRuntimeTerminalDestroyValidatorForTest TerminalValidator = nullptr;
        {
            FScopeLock Lock(&LifecycleMutex);
            const FTrackedEntry* Entry = Entries.Find(Key);
            if (GExitPurge && !Prepare && Entry && Entry->State == ETrackedState::Pending)
            {
                TerminalToken = Entry->TerminalToken;
                TerminalValidator = Entry->TerminalValidator;
            }
        }
        // Runtime revalidates the full field/type plan under its execution writer.
        // The validator must never execute while the Bridge entry lock is held.
        const bool bDefer = TerminalValidator && TerminalValidator(TerminalToken, ERuntimeTerminalDestroyPhaseForTest::BeforeClaim);
        if (bDefer)
        {
            bool bTerminalIdentityMatches = false;
            {
                FScopeLock Lock(&LifecycleMutex);
                FTrackedEntry* Entry = Entries.Find(Key);
                bTerminalIdentityMatches = Entry && Entry->State == ETrackedState::Pending
                    && Entry->ObjectIdentity == TerminalToken.Object && Entry->TypeCookie == TerminalToken.TypeCookie
                    && Entry->Generation == TerminalToken.Generation && Entry->TerminalValidator == TerminalValidator
                    && Entry->TerminalToken.TrackSequence == TerminalToken.TrackSequence
                    && Entry->TerminalToken.ArmEpoch == TerminalToken.ArmEpoch;
                if (bTerminalIdentityMatches) Entry->State = ETrackedState::Deferred;
            }
            if (!bTerminalIdentityMatches)
                UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_DEFER_IDENTITY_CHANGED"));
            continue;
        }
#endif
        UObject* Object = nullptr;
        void* TypeCookie = nullptr;
        uint64 Generation = 0;
        const TCHAR* ClaimFailure = nullptr;
        {
            FScopeLock Lock(&LifecycleMutex);
            FTrackedEntry* Entry = Entries.Find(Key);
            if (!Entry || Entry->State != ETrackedState::Pending) continue;
            if (Entry->ClaimSequence != Snapshot.ClaimSequence || Entry->ObjectIdentity != Snapshot.Object
                || Entry->TypeCookie != Snapshot.TypeCookie || DestroyCallback != Callback)
                ClaimFailure = TEXT("DISPATCHING: preclaim changed");
            else
            {
                Object = Entry->Key.ResolveObjectPtrEvenIfUnreachable();
                if (!Object || !Object->IsUnreachable()
                    || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) continue;
                if (Entry->Generation != CallbackGeneration)
                    ClaimFailure = TEXT("GENERATION_MISMATCH: tracked object generation drifted");
                else
                {
                    Entry->State = ETrackedState::Dispatching;
                    ++DispatchingCount;
                    TypeCookie = Entry->TypeCookie;
                    Generation = Entry->Generation;
                }
            }
        }
        if (ClaimFailure) UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("%s"), ClaimFailure);

        checkf(
            Generation == CallbackGeneration,
            TEXT("GENERATION_MISMATCH: dispatch generation drifted")
        );
        {
            TGuardValue<bool> CallbackScope(bExitPurgeRuntimeDestroyCallback, GExitPurge);
            if (!Callback(Object, TypeCookie, Generation))
            {
                UE_LOG(
                    LogSoloASBridgeObjectLifecycle,
                    Fatal,
                    TEXT("CALLBACK_FAILED: Runtime destroy callback rejected object")
                );
            }
        }

        bool bCompletedIdentityMatches = false;
        int32 RemovedCount = 0;
        {
            FScopeLock Lock(&LifecycleMutex);
            FTrackedEntry* Entry = Entries.Find(Key);
            bCompletedIdentityMatches = Entry && Entry->State == ETrackedState::Dispatching
                && Entry->Generation == Generation && Entry->Key == Key;
            if (bCompletedIdentityMatches)
            {
                RemovedCount = Entries.Remove(Key);
                if (RemovedCount == 1) --DispatchingCount;
            }
        }
        if (!bCompletedIdentityMatches)
            UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("DISPATCHING: tracked entry identity changed during callback"));
        if (RemovedCount != 1)
            UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("DISPATCHING: completed entry removal failed"));
    }

    bool bDrained = false;
    {
        FScopeLock Lock(&LifecycleMutex);
        bDrained = DispatchingCount == 0;
        if (bDrained) bDispatchInProgress = false;
    }
    if (!bDrained) UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("DISPATCHING: callback count did not drain"));
}

void InitializeObjectLifecycle()
{
    checkf(IsInGameThread(), TEXT("lifecycle startup requires game thread"));
    FailIfOffGameThread(TEXT("Runtime lifecycle startup"));

    FScopeLock Lock(&LifecycleMutex);
    checkf(
        !bDeleteListenerRegistered,
        TEXT("Runtime lifecycle initialized twice")
    );
    checkf(
        Entries.Num() == 0
            && DestroyCallback == nullptr
            && DispatchingCount == 0
            && !bDispatchInProgress
            && !PostReachabilityHandle.IsValid()
            && !PreBeginDestroyHandle.IsValid(),
        TEXT("Runtime lifecycle startup state is not empty")
    );
    if (
        bDeleteListenerRegistered
        || Entries.Num() != 0
        || DestroyCallback != nullptr
        || DispatchingCount != 0
        || bDispatchInProgress
        || PostReachabilityHandle.IsValid()
            || PreBeginDestroyHandle.IsValid()
    )
    {
        UE_LOG(
            LogSoloASBridgeObjectLifecycle,
            Fatal,
            TEXT("Runtime lifecycle startup state is not empty")
        );
    }
    GUObjectArray.AddUObjectDeleteListener(&LateDeleteSentinel);
    bDeleteListenerRegistered = true;
    bDeleteListenerUnregistering = false;
}

void ShutdownObjectLifecycle()
{
    checkf(IsInGameThread(), TEXT("lifecycle shutdown requires game thread"));
    FailIfOffGameThread(TEXT("Runtime lifecycle shutdown"));

    {
        FScopeLock Lock(&LifecycleMutex);
        checkf(
            Entries.Num() == 0,
            TEXT("LIVE_ENTRIES: Bridge shutdown with tracked objects")
        );
        checkf(
            DispatchingCount == 0 && !bDispatchInProgress,
            TEXT("DISPATCHING: Bridge shutdown during lifecycle dispatch")
        );
        checkf(
            DestroyCallback == nullptr
                && CallbackGeneration == 0
                && !PostReachabilityHandle.IsValid()
            && !PreBeginDestroyHandle.IsValid(),
            TEXT("Runtime lifecycle callback was not quiesced")
        );
        if (
            Entries.Num() != 0
            || DispatchingCount != 0
            || bDispatchInProgress
            || DestroyCallback != nullptr
            || CallbackGeneration != 0
            || PostReachabilityHandle.IsValid()
            || PreBeginDestroyHandle.IsValid()
        )
        {
            UE_LOG(
                LogSoloASBridgeObjectLifecycle,
                Fatal,
                TEXT("Runtime lifecycle shutdown requires quiesced empty state")
            );
        }
    }

    if (bDeleteListenerRegistered)
    {
        bDeleteListenerUnregistering = true;
        GUObjectArray.RemoveUObjectDeleteListener(&LateDeleteSentinel);
        bDeleteListenerRegistered = false;
        bDeleteListenerUnregistering = false;
    }
}
}

namespace SoloASBridge
{
namespace
{
thread_local const FRuntimeDeferredDestroyToken* ActiveDeferredCertificate = nullptr;

bool SameDeferredToken(const FRuntimeDeferredDestroyToken& A, const FRuntimeDeferredDestroyToken& B)
{
    return A.Key == B.Key && A.Object == B.Object && A.TypeCookie == B.TypeCookie
        && A.TrackSequence == B.TrackSequence && A.ArmEpoch == B.ArmEpoch
        && A.Generation == B.Generation && A.ClaimSequence == B.ClaimSequence && A.PlanId == B.PlanId;
}
}

const FRuntimeDeferredDestroyToken* GetActiveRuntimeDeferredDestroyToken()
{
    return IsInGameThread() ? ActiveDeferredCertificate : nullptr;
}

bool CompleteRuntimeDeferredDestroy(const FRuntimeDeferredDestroyToken& Input)
{
    Private::FailIfOffGameThread(TEXT("Runtime deferred completion"));
    const FRuntimeDeferredDestroyToken Token = Input;
    FRuntimeDestroyControlCallback Validator = nullptr;
    FRuntimeDestroyCallback Callback = nullptr;
    {
        FScopeLock Lock(&Private::LifecycleMutex);
        auto* Entry = Private::Entries.Find(Token.Key);
        if (!Entry || Entry->State != Private::ETrackedState::Deferred
            || !Token.PlanId || !Token.ClaimSequence || !SameDeferredToken(Entry->DeferredToken, Token)
            || Private::bDispatchInProgress || ActiveDeferredCertificate || !Private::DestroyCallback)
            return false;
        Validator = Entry->DeferredControl;
        Callback = Private::DestroyCallback;
        // Validation can call out. Reserve exclusive completion before leaving the first lock.
        Entry->State = Private::ETrackedState::Completing;
        Private::bDispatchInProgress = true;
    }
    if (!Validator || !Validator(Token, ERuntimeDestroyControl::ValidateForCallback))
        UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_COMPLETION_PROOF_FAILED_NO_RETRY"));
    bool bCallbackIdentityMatches = false;
    {
        FScopeLock Lock(&Private::LifecycleMutex);
        auto* Entry = Private::Entries.Find(Token.Key);
        bCallbackIdentityMatches = Entry && Entry->State == Private::ETrackedState::Completing
            && SameDeferredToken(Entry->DeferredToken, Token)
            && Token.Generation == Private::CallbackGeneration && Entry->DeferredControl == Validator
            && Callback == Private::DestroyCallback;
        if (bCallbackIdentityMatches)
        {
            Entry->State = Private::ETrackedState::Dispatching;
            ++Private::DispatchingCount;
        }
    }
    if (!bCallbackIdentityMatches)
        UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_COMPLETION_IDENTITY_CHANGED_NO_RETRY"));
    bool bCompleted = false;
    {
        TGuardValue<const FRuntimeDeferredDestroyToken*> CertificateScope(ActiveDeferredCertificate, &Token);
        TGuardValue<bool> ExitScope(Private::bExitPurgeRuntimeDestroyCallback, GExitPurge);
        bCompleted = Callback(Token.Object, Token.TypeCookie, Token.Generation);
    }
    bool bCallbackStateMatches = false;
    {
        FScopeLock Lock(&Private::LifecycleMutex);
        auto* Entry = Private::Entries.Find(Token.Key);
        bCallbackStateMatches = bCompleted && Entry && Entry->State == Private::ETrackedState::Dispatching
            && SameDeferredToken(Entry->DeferredToken, Token);
        if (bCallbackStateMatches)
        {
            Private::Entries.Remove(Token.Key);
            --Private::DispatchingCount;
            Private::bDispatchInProgress = false;
        }
    }
    if (!bCallbackStateMatches)
        UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_CALLBACK_FAILED_NO_RETRY"));
    return true;
}
}

#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
namespace SoloASBridge::Private
{
bool CancelPendingTerminalRecipe(UObject* Object, void* ExpectedTypeCookie)
{
    check(IsInGameThread());
    FRuntimeTerminalDestroyTokenForTest Token;
    FRuntimeTerminalDestroyValidatorForTest Validator = nullptr;
    {
        FScopeLock Lock(&LifecycleMutex);
        const auto* Entry = Entries.Find(FObjectKey(Object));
        if (!Entry || bDispatchInProgress) return true;
        if (Entry->State == ETrackedState::Deferred) return false;
        if (Entry->State != ETrackedState::Pending || !Entry->TerminalValidator
            || Entry->Generation != CallbackGeneration
            || (ExpectedTypeCookie && Entry->TypeCookie != ExpectedTypeCookie)) return true;
        Token = Entry->TerminalToken;
        Validator = Entry->TerminalValidator;
    }
    // Writer acquisition, Runtime pin release and recursive Bridge disarm occur outside LifecycleMutex.
    if (!Validator(Token, ERuntimeTerminalDestroyPhaseForTest::CancelPending)) return false;
    {
        FScopeLock Lock(&LifecycleMutex);
        auto* Entry = Entries.Find(Token.Key);
        if (!Entry || Entry->State != ETrackedState::Pending || Entry->ObjectIdentity != Token.Object
            || Entry->TypeCookie != Token.TypeCookie || Entry->Generation != Token.Generation || Entry->TerminalValidator)
            return false;
    }
    return true;
}
}

namespace SoloASBridge
{
namespace
{
thread_local const FRuntimeTerminalDestroyTokenForTest* ActiveTerminalCertificate = nullptr;

bool SameTerminalToken(const FRuntimeTerminalDestroyTokenForTest& A,
    const FRuntimeTerminalDestroyTokenForTest& B)
{
    return A.Key == B.Key && A.Object == B.Object && A.TypeCookie == B.TypeCookie
        && A.TrackSequence == B.TrackSequence && A.ArmEpoch == B.ArmEpoch && A.Generation == B.Generation;
}
}

bool IsTerminalRuntimeDestroyCallbackForTest()
{
    return IsInGameThread() && GExitPurge
        && (ActiveTerminalCertificate != nullptr || GetActiveRuntimeDeferredDestroyToken() != nullptr);
}

bool IsTerminalRuntimeDestroyReceiverForTest(const UObject* Object)
{
    const auto* Cert = GetActiveRuntimeDeferredDestroyToken();
    if (!Cert) Cert = ActiveTerminalCertificate;
    return IsTerminalRuntimeDestroyCallbackForTest() && Object && Cert->Object == Object
        && Cert->Key.ResolveObjectPtrEvenIfUnreachable() == Object
        && Cert->TrackSequence != 0 && Cert->ArmEpoch != 0 && Cert->Generation == Private::CallbackGeneration
        && Object->IsUnreachable() && Object->HasAllFlags(RF_BeginDestroyed | RF_FinishDestroyed);
}

bool ArmRuntimeTerminalDestroyForTest(UObject* Object, void* TypeCookie,
    uint64 TrackSequence, uint64 ArmEpoch, FRuntimeTerminalDestroyValidatorForTest Validator,
    FRuntimeTerminalDestroyTokenForTest& OutToken)
{
    check(IsInGameThread());
    OutToken = {};
    if (!Object || !TypeCookie || !TrackSequence || !ArmEpoch || !Validator
        || Object->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) return false;
    FScopeLock Lock(&Private::LifecycleMutex);
    auto* Entry = Private::Entries.Find(FObjectKey(Object));
    if (!Entry || Entry->State != Private::ETrackedState::Pending || Entry->ObjectIdentity != Object
        || Entry->TypeCookie != TypeCookie || Entry->Generation != Private::CallbackGeneration
        || Entry->TerminalValidator || !Private::DestroyCallback || Private::bDispatchInProgress) return false;
    OutToken = {Entry->Key, Object, TypeCookie, TrackSequence, ArmEpoch, Entry->Generation};
    Entry->TerminalToken = OutToken;
    Entry->TerminalValidator = Validator;
    return true;
}

void DisarmRuntimeTerminalDestroyForTest(const FRuntimeTerminalDestroyTokenForTest& Token)
{
    check(IsInGameThread());
    FScopeLock Lock(&Private::LifecycleMutex);
    auto* Entry = Private::Entries.Find(Token.Key);
    if (!Entry || !SameTerminalToken(Entry->TerminalToken, Token)) return;
    if (Entry->State != Private::ETrackedState::Pending)
        UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_DISARM_AFTER_DEFERRAL"));
    Entry->TerminalValidator = nullptr;
    Entry->TerminalToken = {};
}

bool CompleteRuntimeTerminalDestroyForTest(const FRuntimeTerminalDestroyTokenForTest& Input)
{
    check(IsInGameThread());
    if (Input.PlanId) return CompleteRuntimeDeferredDestroy(Input);
    const FRuntimeTerminalDestroyTokenForTest Token = Input;
    FRuntimeTerminalDestroyValidatorForTest Validator = nullptr;
    FRuntimeDestroyCallback Callback = nullptr;
    {
        FScopeLock Lock(&Private::LifecycleMutex);
        auto* Entry = Private::Entries.Find(Token.Key);
        if (!GExitPurge || !Entry || Entry->State != Private::ETrackedState::Deferred
            || !SameTerminalToken(Entry->TerminalToken, Token) || Private::bDispatchInProgress
            || ActiveTerminalCertificate || !Private::DestroyCallback) return false;
        Validator = Entry->TerminalValidator;
        Callback = Private::DestroyCallback;
        Entry->State = Private::ETrackedState::Completing;
        Private::bDispatchInProgress = true;
    }
    if (!Validator || !Validator(Token, ERuntimeTerminalDestroyPhaseForTest::AfterOriginalFieldDestroy))
        UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_LEGACY_COMPLETION_PROOF_FAILED_NO_RETRY"));
    {
        FScopeLock Lock(&Private::LifecycleMutex);
        auto* Entry = Private::Entries.Find(Token.Key);
        if (!Entry || Entry->State != Private::ETrackedState::Completing
            || !SameTerminalToken(Entry->TerminalToken, Token) || Token.Generation != Private::CallbackGeneration
            || Entry->TerminalValidator != Validator || Callback != Private::DestroyCallback)
            UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_LEGACY_COMPLETION_IDENTITY_CHANGED_NO_RETRY"));
        Entry->State = Private::ETrackedState::Dispatching;
        ++Private::DispatchingCount;
        Private::bDispatchInProgress = true;
    }
    bool bCompleted = false;
    {
        // The certificate is immutable, stack-owned and available only during the real registered callback.
        TGuardValue<const FRuntimeTerminalDestroyTokenForTest*> CertScope(ActiveTerminalCertificate, &Token);
        TGuardValue<bool> ExitScope(Private::bExitPurgeRuntimeDestroyCallback, true);
        bCompleted = Callback(Token.Object, Token.TypeCookie, Token.Generation);
    }
    {
        FScopeLock Lock(&Private::LifecycleMutex);
        auto* Entry = Private::Entries.Find(Token.Key);
        if (!bCompleted || !Entry || Entry->State != Private::ETrackedState::Dispatching
            || !SameTerminalToken(Entry->TerminalToken, Token))
            UE_LOG(LogSoloASBridgeObjectLifecycle, Fatal, TEXT("TERMINAL_CALLBACK_FAILED_NO_RETRY"));
        Private::Entries.Remove(Token.Key);
        --Private::DispatchingCount;
        Private::bDispatchInProgress = false;
    }
    return bCompleted;
}
}
#endif
