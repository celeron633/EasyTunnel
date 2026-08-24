#include "wintun_loader.h"

#ifdef _WIN32

#include <filesystem>
#include <string>
#include <vector>

namespace {

HMODULE g_wintunModule = nullptr;
std::string g_lastError;
std::string g_loadedPath;

WINTUN_OPEN_ADAPTER_FUNC* g_openAdapter = nullptr;
WINTUN_CREATE_ADAPTER_FUNC* g_createAdapter = nullptr;
WINTUN_CLOSE_ADAPTER_FUNC* g_closeAdapter = nullptr;
WINTUN_GET_ADAPTER_LUID_FUNC* g_getAdapterLuid = nullptr;
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

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

bool GetExecutableDirectory(std::filesystem::path* directory) {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            g_lastError = "GetModuleFileNameW failed, last_error=" +
                std::to_string(GetLastError());
            return false;
        }
        if (length < buffer.size()) {
            *directory = std::filesystem::path(
                std::wstring(buffer.data(), length)).parent_path();
            return true;
        }
        if (buffer.size() >= 32768) {
            g_lastError = "Executable path exceeds Windows path limit";
            return false;
        }
        buffer.resize(buffer.size() * 2);
    }
}

}  // namespace

bool LoadWintunLibrary() {
    if (g_wintunModule != nullptr) {
        return true;
    }

    g_lastError.clear();
    g_loadedPath.clear();

    std::filesystem::path executableDirectory;
    if (!GetExecutableDirectory(&executableDirectory)) {
        return false;
    }
    const std::filesystem::path dllPath = executableDirectory / L"wintun.dll";
    g_loadedPath = WideToUtf8(dllPath.wstring());

    g_wintunModule = LoadLibraryExW(
        dllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_wintunModule == nullptr) {
        g_lastError = "LoadLibraryExW(" + g_loadedPath +
            ") failed, last_error=" + std::to_string(GetLastError());
        return false;
    }

    if (!LoadSymbol(reinterpret_cast<void**>(&g_openAdapter), "WintunOpenAdapter") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_createAdapter), "WintunCreateAdapter") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_closeAdapter), "WintunCloseAdapter") ||
        !LoadSymbol(reinterpret_cast<void**>(&g_getAdapterLuid), "WintunGetAdapterLUID") ||
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
    g_getAdapterLuid = nullptr;
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

const char* GetWintunLibraryPath() {
    return g_loadedPath.c_str();
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

void WtGetAdapterLuid(WINTUN_ADAPTER_HANDLE adapter, NET_LUID* luid) {
    g_getAdapterLuid(adapter, luid);
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

#endif  // _WIN32
