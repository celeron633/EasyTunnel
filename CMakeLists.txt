cmake_minimum_required(VERSION 3.20)
project(EasyTunnel LANGUAGES C CXX)

include(CTest)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)

option(BUILD_GUI "Build the ImGui NAT traversal client" ON)
option(BUILD_TUI "Build the FTXUI terminal client and rendezvous server" ON)

FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.14.1.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(spdlog)

# JSON config parsing (client_config.cpp, rendezvous/config.cpp). Only the
# static library is needed, so the rest of JsonCpp's own build surface is
# disabled before it is pulled in.
FetchContent_Declare(
    jsoncpp
    GIT_REPOSITORY https://github.com/open-source-parsers/jsoncpp.git
    GIT_TAG 1.9.8
)
set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_EXAMPLE OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_CMAKE_PACKAGE OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_OBJECT_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(jsoncpp)

if(BUILD_TUI)
    FetchContent_Declare(
        ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG v6.1.9
    )
    FetchContent_MakeAvailable(ftxui)
endif()

find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(BUILD_TESTING)
    add_executable(EasyTunnel_config_tests
        tests/config_traversal_modes_test.cpp
        config.cpp
        client_config.cpp
        nat_protocol.cpp
        util.cpp
        log.cpp)
    target_include_directories(EasyTunnel_config_tests PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(EasyTunnel_config_tests PRIVATE
        Threads::Threads spdlog::spdlog_header_only jsoncpp_static)
    if(WIN32)
        target_link_libraries(EasyTunnel_config_tests PRIVATE ws2_32)
    endif()
    add_test(NAME config_traversal_modes COMMAND EasyTunnel_config_tests
        ${CMAKE_SOURCE_DIR}/conf/EasyTunnel.json.example)

    add_executable(EasyTunnel_rendezvous_capabilities_tests
        tests/rendezvous_capabilities_test.cpp
        rendezvous/registry.cpp
        rendezvous/ipv4_relay_app.cpp
        rendezvous/config.cpp
        rendezvous_client.cpp
        secure_random.cpp
        nat_protocol.cpp
        util.cpp
        config.cpp
        log.cpp)
    target_include_directories(
        EasyTunnel_rendezvous_capabilities_tests PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(EasyTunnel_rendezvous_capabilities_tests PRIVATE
        Threads::Threads spdlog::spdlog_header_only jsoncpp_static)
    if(WIN32)
        target_link_libraries(
            EasyTunnel_rendezvous_capabilities_tests PRIVATE ws2_32 bcrypt)
    endif()
    add_test(NAME rendezvous_capabilities
        COMMAND EasyTunnel_rendezvous_capabilities_tests)

    add_executable(EasyTunnel_stun_tests
        tests/stun_client_test.cpp
        stun_client.cpp
        secure_random.cpp
        util.cpp
        log.cpp
        config.cpp)
    target_include_directories(EasyTunnel_stun_tests PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(EasyTunnel_stun_tests PRIVATE
        Threads::Threads spdlog::spdlog_header_only)
    if(WIN32)
        target_link_libraries(EasyTunnel_stun_tests PRIVATE ws2_32 bcrypt)
    endif()
    add_test(NAME stun_protocol COMMAND EasyTunnel_stun_tests)

    add_executable(EasyTunnel_nat_punch_plan_tests
        tests/nat_punch_plan_test.cpp
        nat_punch_plan.cpp
        stun_client.cpp
        secure_random.cpp
        util.cpp
        log.cpp
        config.cpp)
    target_include_directories(
        EasyTunnel_nat_punch_plan_tests PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(EasyTunnel_nat_punch_plan_tests PRIVATE
        Threads::Threads spdlog::spdlog_header_only)
    if(WIN32)
        target_link_libraries(
            EasyTunnel_nat_punch_plan_tests PRIVATE ws2_32 bcrypt)
    endif()
    add_test(NAME nat_punch_plan COMMAND EasyTunnel_nat_punch_plan_tests)

    add_executable(EasyTunnel_adaptive_nat_integration_tests
        tests/adaptive_nat_integration_test.cpp
        adaptive_nat_traversal.cpp
        nat_punch_plan.cpp
        stun_client.cpp
        secure_random.cpp
        peer_selection.cpp
        rendezvous_client.cpp
        rendezvous/registry.cpp
        rendezvous/ipv4_relay_app.cpp
        nat_protocol.cpp
        util.cpp
        config.cpp
        log.cpp)
    target_include_directories(
        EasyTunnel_adaptive_nat_integration_tests PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(EasyTunnel_adaptive_nat_integration_tests PRIVATE
        Threads::Threads spdlog::spdlog_header_only)
    if(WIN32)
        target_link_libraries(
            EasyTunnel_adaptive_nat_integration_tests PRIVATE ws2_32 bcrypt)
    endif()
    add_test(NAME adaptive_nat_integration
        COMMAND EasyTunnel_adaptive_nat_integration_tests)
    set_tests_properties(adaptive_nat_integration PROPERTIES TIMEOUT 30)
endif()

# ---------------------------------------------------------------------------
# Common sources (platform-independent) - shared between console and GUI
# ---------------------------------------------------------------------------
set(COMMON_SOURCES
    config.cpp
    client_config.cpp
    hexdump.cpp
    log.cpp
    util.cpp
    tunnel_engine.cpp
    nat_protocol.cpp
    secure_random.cpp
    stun_client.cpp
    nat_punch_plan.cpp
    adaptive_nat_traversal.cpp
    rendezvous_client.cpp
    peer_selection.cpp
    nat_traversal.cpp
    ipv6_fallback.cpp
    ipv4_relay_fallback.cpp
)

# ---------------------------------------------------------------------------
# Platform-specific sources
# ---------------------------------------------------------------------------
if(WIN32)
    list(APPEND COMMON_SOURCES
        tun_adapter_windows.cpp
        wintun_loader.cpp
    )

    # Windows application icon (embedded via a compiled resource script).
    enable_language(RC)
    set(APP_ICON_RESOURCE "${CMAKE_SOURCE_DIR}/res/EasyTunnel.rc")
else()
    list(APPEND COMMON_SOURCES
        tun_adapter_linux.cpp
    )
endif()

# ---------------------------------------------------------------------------
# Console application (original)
# ---------------------------------------------------------------------------
add_executable(EasyTunnel main.cpp ${COMMON_SOURCES} ${APP_ICON_RESOURCE})
add_executable(EasyTunnel_rendezvous
    rendezvous/main.cpp
    rendezvous/server.cpp
    rendezvous/registry.cpp
    rendezvous/ipv4_relay_app.cpp
    rendezvous/config.cpp
    secure_random.cpp
    nat_protocol.cpp util.cpp log.cpp config.cpp)
if(BUILD_TUI)
    set(TUI_SOURCES
        tui/tui_main.cpp
        tui/tui_app.cpp
        tui/tui_app_connection.cpp
        tui/tui_app_statistics.cpp
        tui/tui_app_settings.cpp
        tui/tui_app_log.cpp
        statistics_history.cpp)
    if(WIN32)
        list(APPEND TUI_SOURCES tui/windows_tray.cpp)
    endif()
    add_executable(EasyTunnel_tui
        ${TUI_SOURCES}
        ${COMMON_SOURCES}
        ${APP_ICON_RESOURCE})
    target_include_directories(EasyTunnel_tui PRIVATE ${CMAKE_SOURCE_DIR})

    add_executable(EasyTunnel_rendezvous_tui
        rendezvous/tui_main.cpp
        rendezvous/tui_app.cpp
        rendezvous/tui_dashboard.cpp
        rendezvous/tui_relay_tab.cpp
        rendezvous/tui_config_tab.cpp
        rendezvous/tui_log_tab.cpp
        rendezvous/server.cpp
        rendezvous/registry.cpp
        rendezvous/ipv4_relay_app.cpp
        rendezvous/config.cpp
        secure_random.cpp
        nat_protocol.cpp util.cpp log.cpp config.cpp)
    target_include_directories(EasyTunnel_rendezvous_tui PRIVATE ${CMAKE_SOURCE_DIR})
endif()

if(MSVC)
    target_compile_options(EasyTunnel PRIVATE /W4)
    target_link_options(EasyTunnel PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
else()
    target_compile_options(EasyTunnel PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TUI)
    if(MSVC)
        target_compile_options(EasyTunnel_tui PRIVATE /W4)
        target_compile_options(EasyTunnel_rendezvous_tui PRIVATE /W4 /utf-8)
        target_link_options(EasyTunnel_tui PRIVATE
            "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
    else()
        target_compile_options(EasyTunnel_tui PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(EasyTunnel_rendezvous_tui PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(MSVC)
    target_compile_options(EasyTunnel_rendezvous PRIVATE /W4)
else()
    target_compile_options(EasyTunnel_rendezvous PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Platform-specific link libraries
# ---------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(EasyTunnel PRIVATE spdlog::spdlog_header_only jsoncpp_static ws2_32 iphlpapi bcrypt Threads::Threads)
    target_link_libraries(EasyTunnel_rendezvous PRIVATE spdlog::spdlog_header_only jsoncpp_static ws2_32 bcrypt Threads::Threads)
else()
    target_link_libraries(EasyTunnel PRIVATE spdlog::spdlog_header_only jsoncpp_static Threads::Threads)
    target_link_libraries(EasyTunnel_rendezvous PRIVATE spdlog::spdlog_header_only jsoncpp_static Threads::Threads)
endif()

if(BUILD_TUI)
    target_link_libraries(EasyTunnel_tui PRIVATE
        spdlog::spdlog_header_only jsoncpp_static
        ftxui::component ftxui::dom ftxui::screen
        Threads::Threads)
    target_link_libraries(EasyTunnel_rendezvous_tui PRIVATE
        spdlog::spdlog_header_only jsoncpp_static
        ftxui::component ftxui::dom ftxui::screen
        Threads::Threads)
    if(WIN32)
        target_link_libraries(EasyTunnel_tui PRIVATE ws2_32 iphlpapi shell32 bcrypt)
        target_link_libraries(EasyTunnel_rendezvous_tui PRIVATE ws2_32 bcrypt)
    endif()
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
    if(BUILD_TUI)
        target_include_directories(EasyTunnel_tui PRIVATE "${WINTUN_INCLUDE_DIR}")
    endif()

    if(EXISTS "${WINTUN_DLL_PATH}")
        add_custom_command(TARGET EasyTunnel POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${WINTUN_DLL_PATH}"
                    "$<TARGET_FILE_DIR:EasyTunnel>/wintun.dll"
            COMMENT "Copy wintun.dll to output directory"
        )
        if(BUILD_TUI)
            add_custom_command(TARGET EasyTunnel_tui POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${WINTUN_DLL_PATH}"
                        "$<TARGET_FILE_DIR:EasyTunnel_tui>/wintun.dll"
                COMMENT "Copy wintun.dll to TUI output directory")
        endif()
    else()
        message(WARNING "wintun.dll not found at ${WINTUN_DLL_PATH}; copy it manually before running")
    endif()
endif()  # WIN32

if(EXISTS "${CMAKE_SOURCE_DIR}/conf/EasyTunnel.json.example")
    add_custom_command(TARGET EasyTunnel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/conf/EasyTunnel.json.example"
                "$<TARGET_FILE_DIR:EasyTunnel>/EasyTunnel.json.example"
        COMMENT "Copy EasyTunnel.json.example to output directory"
    )
endif()

if(BUILD_GUI)
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

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.8
    )
    FetchContent_MakeAvailable(imgui)

    FetchContent_Declare(
        implot
        GIT_REPOSITORY https://github.com/epezent/implot.git
        GIT_TAG v1.0
    )
    FetchContent_MakeAvailable(implot)

    set(IMGUI_DIR ${imgui_SOURCE_DIR})
    add_library(imgui_lib STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui_lib PUBLIC
        ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMGUI_DIR}/misc/cpp)
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

    set(IMPLOT_DIR ${implot_SOURCE_DIR})
    add_library(implot_lib STATIC
        ${IMPLOT_DIR}/implot.cpp
        ${IMPLOT_DIR}/implot_items.cpp
    )
    target_include_directories(implot_lib PUBLIC ${IMPLOT_DIR})
    target_link_libraries(implot_lib PUBLIC imgui_lib)

    set(GUI_SOURCES
        gui/gui_main.cpp
        gui/gui_app.cpp
        gui/gui_app_connection.cpp
        gui/gui_app_statistics.cpp
        gui/gui_app_settings.cpp
        gui/gui_app_log.cpp
        statistics_history.cpp
        gui/ui_heartbeat.cpp)
    if(WIN32)
        list(APPEND GUI_SOURCES
            gui/windows_startup.cpp
            gui/windows_tray.cpp)
    endif()
    add_executable(EasyTunnel_gui ${GUI_SOURCES} ${COMMON_SOURCES} ${APP_ICON_RESOURCE})
    target_include_directories(EasyTunnel_gui PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(EasyTunnel_gui PRIVATE
        spdlog::spdlog_header_only jsoncpp_static implot_lib Threads::Threads)

    if(MSVC)
        target_compile_options(EasyTunnel_gui PRIVATE /W4 /utf-8)
        target_link_options(EasyTunnel_gui PRIVATE
            "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
            "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")
    else()
        target_compile_options(EasyTunnel_gui PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(WIN32)
        target_link_libraries(EasyTunnel_gui PRIVATE
            ws2_32 iphlpapi shell32 taskschd ole32 oleaut32 secur32 bcrypt)
        target_include_directories(EasyTunnel_gui PRIVATE "${WINTUN_INCLUDE_DIR}")
        add_custom_command(TARGET EasyTunnel_gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${WINTUN_DLL_PATH}"
                    "$<TARGET_FILE_DIR:EasyTunnel_gui>/wintun.dll"
            COMMENT "Copy wintun.dll to GUI output directory")
    endif()

endif()
