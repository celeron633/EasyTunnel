#pragma once

#ifdef _WIN32

#include <string>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#include "wintun.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

bool LoadWintunLibrary(const std::wstring& dllPath);
void UnloadWintunLibrary();
const char* GetWintunLoadError();

WINTUN_ADAPTER_HANDLE WtOpenAdapter(const WCHAR* name);
WINTUN_ADAPTER_HANDLE WtCreateAdapter(const WCHAR* name, const WCHAR* tunnelType, const GUID* requestedGUID);
void WtCloseAdapter(WINTUN_ADAPTER_HANDLE adapter);
WINTUN_SESSION_HANDLE WtStartSession(WINTUN_ADAPTER_HANDLE adapter, DWORD capacity);
void WtEndSession(WINTUN_SESSION_HANDLE session);
HANDLE WtGetReadWaitEvent(WINTUN_SESSION_HANDLE session);
BYTE* WtReceivePacket(WINTUN_SESSION_HANDLE session, DWORD* packetSize);
void WtReleaseReceivePacket(WINTUN_SESSION_HANDLE session, const BYTE* packet);
BYTE* WtAllocateSendPacket(WINTUN_SESSION_HANDLE session, DWORD packetSize);
void WtSendPacket(WINTUN_SESSION_HANDLE session, const BYTE* packet);

#endif  // _WIN32
