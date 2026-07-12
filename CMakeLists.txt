cmake_minimum_required(VERSION 3.20)
project(EasyTunnel LANGUAGES C CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)

option(BUILD_GUI "Build the ImGui-based GUI application" ON)

FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(spdlog)

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Common sources (platform-independent) - shared between console and GUI
# ---------------------------------------------------------------------------
set(COMMON_SOURCES
    config.cpp
    hexdump.cpp
    log.cpp
    util.cpp
    tunnel_engine.cpp
    ipv6_utils.cpp
    connection_history.cpp
)

# ---------------------------------------------------------------------------
# Platform-specific sources
# ---------------------------------------------------------------------------
if(WIN32)
    list(APPEND COMMON_SOURCES
        tun_adapter_windows.cpp
        wintun_loader.cpp
    )
else()
    list(APPEND COMMON_SOURCES
        tun_adapter_linux.cpp
    )
endif()

# ---------------------------------------------------------------------------
# Console application (original)
# ---------------------------------------------------------------------------
add_executable(EasyTunnel main.cpp ${COMMON_SOURCES})

if(MSVC)
    target_compile_options(EasyTunnel PRIVATE /W4)
    target_link_options(EasyTunnel PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
else()
    target_compile_options(EasyTunnel PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Platform-specific link libraries
# ---------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(EasyTunnel PRIVATE spdlog::spdlog_header_only ws2_32 iphlpapi Threads::Threads)
else()
    target_link_libraries(EasyTunnel PRIVATE spdlog::spdlog_header_only Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Windows-only: Wintun SDK
# ---------------------------------------------------------------------------
if(WIN32)
    set(WINTUN_VERSION "0.14.1" CACHE STRING "Wintun SDK version used for auto-download")
    set(WINTUN_SDK_DIR "" CACHE PATH "Optional local Wintun SDK directory override")

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|AARCH64|aarch64)$")
        set(WINTUN_ARCH "arm64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(WINTUN_ARCH "amd64")
    else()
        set(WINTUN_ARCH "x86")
    endif()

    if(NOT WINTUN_SDK_DIR)
        set(WINTUN_URL "https://www.wintun.net/builds/wintun-${WINTUN_VERSION}.zip")
        set(WINTUN_BASE_DIR "${CMAKE_BINARY_DIR}/_deps")
        set(WINTUN_ZIP_PATH "${WINTUN_BASE_DIR}/wintun-${WINTUN_VERSION}.zip")
        set(WINTUN_EXTRACT_DIR "${WINTUN_BASE_DIR}/wintun-${WINTUN_VERSION}")

        file(MAKE_DIRECTORY "${WINTUN_BASE_DIR}")

        if(NOT EXISTS "${WINTUN_EXTRACT_DIR}/include/wintun.h")
            message(STATUS "Downloading Wintun SDK from ${WINTUN_URL}")
            file(DOWNLOAD
                "${WINTUN_URL}"
                "${WINTUN_ZIP_PATH}"
                SHOW_PROGRESS
                STATUS WINTUN_DOWNLOAD_STATUS
                TLS_VERIFY ON
            )
            list(GET WINTUN_DOWNLOAD_STATUS 0 WINTUN_DOWNLOAD_CODE)
            list(GET WINTUN_DOWNLOAD_STATUS 1 WINTUN_DOWNLOAD_MSG)
            if(NOT WINTUN_DOWNLOAD_CODE EQUAL 0)
                message(FATAL_ERROR "Failed to download Wintun SDK: ${WINTUN_DOWNLOAD_MSG}")
            endif()

            file(ARCHIVE_EXTRACT
                INPUT "${WINTUN_ZIP_PATH}"
                DESTINATION "${WINTUN_EXTRACT_DIR}"
            )
        endif()

        set(WINTUN_SDK_DIR "${WINTUN_EXTRACT_DIR}")
    endif()

    # Some archives extract as <root>/wintun/include, others as <root>/include.
    set(WINTUN_SDK_ROOT "${WINTUN_SDK_DIR}")
    if(EXISTS "${WINTUN_SDK_DIR}/wintun/include/wintun.h")
        set(WINTUN_SDK_ROOT "${WINTUN_SDK_DIR}/wintun")
    endif()

    set(WINTUN_INCLUDE_DIR "${WINTUN_SDK_ROOT}/include")
    set(WINTUN_DLL_PATH "${WINTUN_SDK_ROOT}/bin/${WINTUN_ARCH}/wintun.dll")

    if(NOT EXISTS "${WINTUN_INCLUDE_DIR}/wintun.h")
        message(FATAL_ERROR "wintun.h not found in ${WINTUN_INCLUDE_DIR}")
    endif()

    if(NOT EXISTS "${WINTUN_DLL_PATH}")
        message(FATAL_ERROR "wintun.dll not found, checked ${WINTUN_DLL_PATH}")
    endif()

    target_include_directories(EasyTunnel PRIVATE "${WINTUN_INCLUDE_DIR}")

    if(EXISTS "${WINTUN_DLL_PATH}")
        add_custom_command(TARGET EasyTunnel POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${WINTUN_DLL_PATH}"
                    "$<TARGET_FILE_DIR:EasyTunnel>/wintun.dll"
            COMMENT "Copy wintun.dll to output directory"
        )
    else()
        message(WARNING "wintun.dll not found at ${WINTUN_DLL_PATH}; copy it manually before running")
    endif()
endif()  # WIN32

if(EXISTS "${CMAKE_SOURCE_DIR}/conf/tunnel.conf.example")
    add_custom_command(TARGET EasyTunnel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/conf/tunnel.conf.example"
                "$<TARGET_FILE_DIR:EasyTunnel>/tunnel.conf.example"
        COMMENT "Copy tunnel.conf.example to output directory"
    )
endif()

# ===========================================================================
# GUI Application (ImGui + GLFW + OpenGL3)
# ===========================================================================
if(BUILD_GUI)
    # -----------------------------------------------------------------------
    # Fetch GLFW
    # -----------------------------------------------------------------------
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
    )
    FetchContent_MakeAvailable(glfw)

    # -----------------------------------------------------------------------
    # Fetch ImGui
    # -----------------------------------------------------------------------
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.8
    )
    FetchContent_MakeAvailable(imgui)

    # Build ImGui as a library
    set(IMGUI_DIR ${imgui_SOURCE_DIR})
    add_library(imgui_lib STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui_lib PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
    )
    target_link_libraries(imgui_lib PUBLIC glfw)

    # Find OpenGL
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui_lib PUBLIC OpenGL::GL)

    # -----------------------------------------------------------------------
    # GUI target
    # -----------------------------------------------------------------------
    set(GUI_SOURCES
        gui/gui_main.cpp
        gui/gui_app.cpp
    )

    add_executable(EasyTunnel_gui ${GUI_SOURCES} ${COMMON_SOURCES})

    target_include_directories(EasyTunnel_gui PRIVATE ${CMAKE_SOURCE_DIR})

    if(MSVC)
        target_compile_options(EasyTunnel_gui PRIVATE /W4)
        target_link_options(EasyTunnel_gui PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
        # Use WinMain entry point to hide console on Windows
        target_link_options(EasyTunnel_gui PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")
    else()
        target_compile_options(EasyTunnel_gui PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    target_link_libraries(EasyTunnel_gui PRIVATE
        spdlog::spdlog_header_only
        imgui_lib
        Threads::Threads
    )

    if(WIN32)
        target_link_libraries(EasyTunnel_gui PRIVATE ws2_32 iphlpapi)
        if(DEFINED WINTUN_INCLUDE_DIR)
            target_include_directories(EasyTunnel_gui PRIVATE "${WINTUN_INCLUDE_DIR}")
        endif()
        if(EXISTS "${WINTUN_DLL_PATH}")
            add_custom_command(TARGET EasyTunnel_gui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${WINTUN_DLL_PATH}"
                        "$<TARGET_FILE_DIR:EasyTunnel_gui>/wintun.dll"
                COMMENT "Copy wintun.dll to GUI output directory"
            )
        endif()
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/conf/tunnel.conf.example")
        add_custom_command(TARGET EasyTunnel_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/conf/tunnel.conf.example"
                    "$<TARGET_FILE_DIR:EasyTunnel_gui>/tunnel.conf.example"
            COMMENT "Copy tunnel.conf.example to GUI output directory"
        )
    endif()
endif()  # BUILD_GUI
