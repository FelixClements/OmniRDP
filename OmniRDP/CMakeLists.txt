cmake_minimum_required(VERSION 3.20)
project(OmniRDP VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find FreeRDP 3.25.0
set(FREERDP_ROOT ${CMAKE_SOURCE_DIR}/../freerdp-3.25.0)
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
)

# Header files
set(HEADERS
    include/backend.h
    include/viewer_server.h
    include/platform_compat.h
    src/pointer_shape.h
    src/viewer_internal.h
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

# Install
install(TARGETS OmniRDP
    RUNTIME DESTINATION bin
)

# Testing (optional)
enable_testing()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
endif()
