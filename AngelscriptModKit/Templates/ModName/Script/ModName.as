// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// ModName example script class.
//
// Player mods may use native APIs, base classes and events unless denied by
// the built-in or project blacklist. Denied references fail compilation with
// MODKIT-API-ALLOWLIST.
class AModNameActor : AActor
{
    UFUNCTION()
    int GetMyModMarker()
    {
        return 7;
    }
}