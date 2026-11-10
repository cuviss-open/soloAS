// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

// Owned and accessed by the editor game thread. Deadlines use monotonic seconds.
class FMcpEditLeases
{
public:
    FGuid Begin(const FGuid& Owner, double Now, double TimeoutSeconds)
    {
        const FGuid Id = FGuid::NewGuid();
        Leases.Add(Id, {Owner, Now + FMath::Clamp(TimeoutSeconds, 1.0, 600.0)});
        return Id;
    }

    bool End(const FGuid& Owner, const FGuid& Id)
    {
        const FLease* Lease = Leases.Find(Id);
        if (!Lease || Lease->Owner != Owner) return false;
        Leases.Remove(Id);
        return true;
    }

    void Disconnect(const FGuid& Owner)
    {
        for (auto It = Leases.CreateIterator(); It; ++It)
            if (It.Value().Owner == Owner) It.RemoveCurrent();
    }

    void Expire(double Now)
    {
        for (auto It = Leases.CreateIterator(); It; ++It)
            if (Now >= It.Value().Deadline) It.RemoveCurrent();
    }

    void ResumeAll() { Leases.Empty(); }
    bool IsPaused() const { return !Leases.IsEmpty(); }
    int32 Count() const { return Leases.Num(); }
    bool HasOwner(const FGuid& Owner) const
    {
        for (const auto& Entry : Leases)
            if (Entry.Value.Owner == Owner) return true;
        return false;
    }

private:
    struct FLease { FGuid Owner; double Deadline; };
    TMap<FGuid, FLease> Leases;
};
