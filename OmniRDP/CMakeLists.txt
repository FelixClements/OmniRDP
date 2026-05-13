cmake_minimum_required(VERSION 3.20)
project(OmniRDP VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── AddressSanitizer (MSVC) ───────────────────────────────────────
option(USE_ADDRESS_SANITIZER "Enable MSVC AddressSanitizer for memory checking" OFF)
if(USE_ADDRESS_SANITIZER)
    message(STATUS "AddressSanitizer enabled for all targets")
    # Apply globally so test targets inherit ASan instrumentation.
    # /Zo enables enhanced debug info for better ASan stack traces.
    add_compile_options(/fsanitize=address /Zo)
    add_link_options(/fsanitize=address)
endif()

# ── Find FreeRDP 3.26.0 ───────────────────────────────────────
# Find FreeRDP 3.26.0
set(FREERDP_ROOT ${CMAKE_SOURCE_DIR}/../freerdp-3.26.0)
if(NOT FREERDP_BUILD)
    set(FREERDP_BUILD ${FREERDP_ROOT}/build)
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

# Source files
set(SOURCES
    src/main.c
    src/backend.c
    src/viewer_internal.c
    src/viewer_server.c
    src/platform_compat.c
    src/pointer_shape.c
    src/instance_runner.c
    src/ini_parser.c
    src/svc_config.c
    src/svc_log.c
    src/svc_dpapi.c
)

# Header files
set(HEADERS
    include/backend.h
    include/viewer_server.h
    include/platform_compat.h
    src/pointer_shape.h
    src/viewer_internal.h
    src/ini_parser.h
    src/svc_config.h
    src/svc_log.h
    src/svc_dpapi.h
    src/pipe_protocol.h
)

# Find FreeRDP libraries in the local build tree
set(FREERDP_LIB_HINTS
    ${FREERDP_BUILD}/libfreerdp/Release
    ${FREERDP_BUILD}/libfreerdp/Debug
    ${FREERDP_BUILD}/libfreerdp
    ${FREERDP_BUILD}/client/common/Release
    ${FREERDP_BUILD}/client/common/Debug
    ${FREERDP_BUILD}/client/common
    ${FREERDP_BUILD}/server/common/Release
    ${FREERDP_BUILD}/server/common/Debug
    ${FREERDP_BUILD}/server/common
    ${FREERDP_BUILD}/winpr/libwinpr/Release
    ${FREERDP_BUILD}/winpr/libwinpr/Debug
    ${FREERDP_BUILD}/winpr/libwinpr
)

find_library(FREERDP_LIB NAMES freerdp3 freerdp HINTS ${FREERDP_LIB_HINTS} REQUIRED)
find_library(FREERDP_CLIENT_LIB NAMES freerdp-client3 freerdp-client HINTS ${FREERDP_LIB_HINTS} REQUIRED)
find_library(FREERDP_SERVER_LIB NAMES freerdp-server3 freerdp-server HINTS ${FREERDP_LIB_HINTS} REQUIRED)
find_library(WINPR_LIB NAMES winpr3 winpr HINTS ${FREERDP_LIB_HINTS} REQUIRED)

# Executable
add_executable(OmniRDP ${SOURCES} ${HEADERS})

# Link libraries
set(MULTIPLEXER_LIBS
    ${FREERDP_CLIENT_LIB}
    ${FREERDP_SERVER_LIB}
    ${FREERDP_LIB}
    ${WINPR_LIB}
)

if(WIN32)
    list(APPEND MULTIPLEXER_LIBS ws2_32 crypt32)
else()
    list(APPEND MULTIPLEXER_LIBS m pthread)
    find_package(X11)
    if(X11_FOUND)
        list(APPEND MULTIPLEXER_LIBS
            ${X11_LIBRARIES}
            Xcursor
            Xext
            Xfixes
            Xi
            Xinerama
            Xrandr
            Xrender
            Xtst
            xkbfile
        )
    endif()
    find_package(OpenSSL)
    if(OpenSSL_FOUND)
        list(APPEND MULTIPLEXER_LIBS OpenSSL::SSL OpenSSL::Crypto)
    endif()
endif()

target_link_libraries(OmniRDP ${MULTIPLEXER_LIBS})

# ── OmniRDP-svc: Windows Service ──────────────────────────────────

set(SVC_SOURCES
    src/svc_main.c
    src/svc_service.c
    src/svc_instance_mgr.c
    src/svc_config.c
    src/svc_log.c
    src/svc_dpapi.c
    src/ini_parser.c
    src/pipe_protocol.c
    src/svc_pipe_server.c
)

set(SVC_HEADERS
    src/svc_service.h
    src/svc_instance_mgr.h
    src/svc_config.h
    src/svc_log.h
    src/svc_dpapi.h
    src/ini_parser.h
    src/pipe_protocol.h
    src/svc_pipe_server.h
)

add_executable(OmniRDP-svc ${SVC_SOURCES} ${SVC_HEADERS})

# Service-only libraries (no FreeRDP dependency)
set(SVC_LIBS
    advapi32    # SCM, DPAPI, security
    crypt32     # DPAPI
    kernel32    # Job objects, processes
    wtsapi32    # WTSQueryUserToken, WTSGetActiveConsoleSessionId
)

target_link_libraries(OmniRDP-svc ${SVC_LIBS})

# ── OmniRDP-tray: System Tray Application ─────────────────────────

set(TRAY_SOURCES
    src/tray_main.c
    src/tray_icon.c
    src/tray_log_viewer.c
    src/tray_status_dlg.c
    src/tray_pipe_client.c
    src/pipe_protocol.c
    src/svc_log.c
)

set(TRAY_HEADERS
    src/tray_icon.h
    src/tray_log_viewer.h
    src/tray_status_dlg.h
    src/tray_pipe_client.h
    src/pipe_protocol.h
    src/svc_log.h
)

add_executable(OmniRDP-tray ${TRAY_SOURCES} ${TRAY_HEADERS})

# Tray app links comctl32 (for ListView) and shell32 (for Shell_NotifyIcon)
# plus the same kernel32/advapi32 used by the service
set(TRAY_LIBS
    comctl32
    shell32
    advapi32
    kernel32
    wtsapi32
)

target_link_libraries(OmniRDP-tray ${TRAY_LIBS})

# Tray app is a Windows GUI app (no console window)
if(WIN32)
    set_target_properties(OmniRDP-tray PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()

# Install
install(TARGETS OmniRDP
    RUNTIME DESTINATION bin
)

install(TARGETS OmniRDP-svc
    RUNTIME DESTINATION bin
)

install(TARGETS OmniRDP-tray
    RUNTIME DESTINATION bin
)

# Testing (optional)
enable_testing()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
