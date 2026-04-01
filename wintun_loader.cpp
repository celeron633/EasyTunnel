#include "wintun_loader.h"

#include <string>

namespace {

HMODULE g_wintunModule = nullptr;
std::string g_lastError;

WINTUN_OPEN_ADAPTER_FUNC* g_openAdapter = nullptr;
WINTUN_CREATE_ADAPTER_FUNC* g_createAdapter = nullptr;
WINTUN_CLOSE_ADAPTER_FUNC* g_closeAdapter = nullptr;
WINTUN_START_SESSION_FUNC* g_startSession = nullptr;
WINTUN_END_SESSION_FUNC* g_endSession = nullptr;
WINTUN_GET_READ_WAIT_EVENT_FUNC* g_getReadWaitEvent = nullptr;
WINTUN_RECEIVE_PACKET_FUNC* g_receivePacket = nullptr;
WINTUN_RELEASE_RECEIVE_PACKET_FUNC* g_releaseReceivePacket = nullptr;
WINTUN_ALLOCATE_SEND_PACKET_FUNC* g_allocateSendPacket = nullptr;
WINTUN_SEND_PACKET_FUNC* g_sendPacket = nullptr;

bool LoadSymbol(void** out, const char* name) {
    *out = reinterpret_cast<void*>(GetProcAddress(g_wintunModule, name));
    if (*out == nullptr) {
        g_lastError = std::string("Missing symbol: ") + name;
        return false;
    }
    return true;
}

}  // namespace

bool LoadWintunLibrary(const std::wstring& dllPath) {
    if (g_wintunModule != nullptr) {
        return true;
    }

    const wchar_t* path = dllPath.empty() ? L"wintun.dll" : dllPath.c_str();
    g_wintunModule = LoadLibraryW(path);
    if (g_wintunModule == nullptr) {
        g_lastError = "LoadLibraryW(wintun.dll) failed, last_error=" + std::to_string(GetLastError());
        return false;
    }

    if (!LoadSymbol(reinterpret_cast<void**>(&g_openAdapter), "WintunOpenAdapter") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_createAdapter), "WintunCreateAdapter") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_closeAdapter), "WintunCloseAdapter") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_startSession), "WintunStartSession") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_endSession), "WintunEndSession") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_getReadWaitEvent), "WintunGetReadWaitEvent") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_receivePacket), "WintunReceivePacket") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_releaseReceivePacket), "WintunReleaseReceivePacket") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_allocateSendPacket), "WintunAllocateSendPacket") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_sendPacket), "WintunSendPacket")) {
        UnloadWintunLibrary();
        return false;
    }

    return true;
}

void UnloadWintunLibrary() {
    g_openAdapter = nullptr;
    g_createAdapter = nullptr;
    g_closeAdapter = nullptr;
    g_startSession = nullptr;
    g_endSession = nullptr;
    g_getReadWaitEvent = nullptr;
    g_receivePacket = nullptr;
    g_releaseReceivePacket = nullptr;
    g_allocateSendPacket = nullptr;
    g_sendPacket = nullptr;

    if (g_wintunModule != nullptr) {
        FreeLibrary(g_wintunModule);
        g_wintunModule = nullptr;
    }
}

const char* GetWintunLoadError() {
    return g_lastError.c_str();
}

WINTUN_ADAPTER_HANDLE WtOpenAdapter(const WCHAR* name) {
    return g_openAdapter(name);
}

WINTUN_ADAPTER_HANDLE WtCreateAdapter(const WCHAR* name, const WCHAR* tunnelType, const GUID* requestedGUID) {
    return g_createAdapter(name, tunnelType, requestedGUID);
}

void WtCloseAdapter(WINTUN_ADAPTER_HANDLE adapter) {
    g_closeAdapter(adapter);
}

WINTUN_SESSION_HANDLE WtStartSession(WINTUN_ADAPTER_HANDLE adapter, DWORD capacity) {
    return g_startSession(adapter, capacity);
}

void WtEndSession(WINTUN_SESSION_HANDLE session) {
    g_endSession(session);
}

HANDLE WtGetReadWaitEvent(WINTUN_SESSION_HANDLE session) {
    return g_getReadWaitEvent(session);
}

BYTE* WtReceivePacket(WINTUN_SESSION_HANDLE session, DWORD* packetSize) {
    return g_receivePacket(session, packetSize);
}

void WtReleaseReceivePacket(WINTUN_SESSION_HANDLE session, const BYTE* packet) {
    g_releaseReceivePacket(session, packet);
}

BYTE* WtAllocateSendPacket(WINTUN_SESSION_HANDLE session, DWORD packetSize) {
    return g_allocateSendPacket(session, packetSize);
}

void WtSendPacket(WINTUN_SESSION_HANDLE session, const BYTE* packet) {
    g_sendPacket(session, packet);
}
