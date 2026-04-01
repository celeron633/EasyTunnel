cmake_minimum_required(VERSION 3.20)
project(6Tunnel LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "This project is currently configured for MSVC only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(6tunnel
    main.cpp
    config.cpp
    hexdump.cpp
    log.cpp
    util.cpp
    wintun_loader.cpp
)

target_compile_options(6tunnel PRIVATE /W4)
target_link_options(6tunnel PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")

target_link_libraries(6tunnel PRIVATE ws2_32 iphlpapi)

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

target_include_directories(6tunnel PRIVATE "${WINTUN_INCLUDE_DIR}")

if(EXISTS "${CMAKE_SOURCE_DIR}/conf/tunnel.conf.example")
    add_custom_command(TARGET 6tunnel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/conf/tunnel.conf.example"
                "$<TARGET_FILE_DIR:6tunnel>/tunnel.conf.example"
        COMMENT "Copy tunnel.conf.example to output directory"
    )
endif()

if(EXISTS "${WINTUN_DLL_PATH}")
    add_custom_command(TARGET 6tunnel POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WINTUN_DLL_PATH}"
                "$<TARGET_FILE_DIR:6tunnel>/wintun.dll"
        COMMENT "Copy wintun.dll to output directory"
    )
else()
    message(WARNING "wintun.dll not found at ${WINTUN_DLL_PATH}; copy it manually before running")
endif()
