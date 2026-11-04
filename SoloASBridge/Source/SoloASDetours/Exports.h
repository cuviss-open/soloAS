// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once
#include <windows.h>
// Export only the transaction operations consumed by the editor hook owners.
extern "C" {
__declspec(dllexport) LONG WINAPI DetourTransactionBegin(VOID);
__declspec(dllexport) LONG WINAPI DetourTransactionAbort(VOID);
__declspec(dllexport) LONG WINAPI DetourTransactionCommit(VOID);
__declspec(dllexport) LONG WINAPI DetourUpdateThread(HANDLE Thread);
__declspec(dllexport) LONG WINAPI DetourAttach(PVOID* Pointer, PVOID Detour);
__declspec(dllexport) LONG WINAPI DetourDetach(PVOID* Pointer, PVOID Detour);
}
