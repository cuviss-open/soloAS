// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CoreMinimal.h"
#include "AngelscriptCoreExtensionPoints.h"

// A socket-free endpoint for tests of the actual debugger protocol implementation.
class ANGELSCRIPTCODE_API FAngelscriptDebugProtocolTest
{
public:
    FAngelscriptDebugProtocolTest();
    ~FAngelscriptDebugProtocolTest();
    void SetSession(uint64 Session);
    void Handle(AngelscriptCoreExtensionPoints::EDebugAccessOperation Operation,
        const FString& Path = FString(), int32 Frame = 0);
    void SendCallStack();
    int32 FindScriptFrame(const FString& ModuleName) const;
    bool SetFrameGeneration(int32 Frame, uint64 Generation);
    uint64 GetFrameGeneration(int32 Frame) const;
    void DelaySends(bool bDelay);
    void Flush();
    bool IsClosed() const;
    bool IsPaused() const;
    bool IsBreakPending() const;
    const TArray<uint8>& Bytes() const;
    void ResetBytes();

private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
#endif
