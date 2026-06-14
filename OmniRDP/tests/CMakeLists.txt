add_executable(test_viewer_state
    test_viewer_state.c
    ${CMAKE_SOURCE_DIR}/src/viewer_internal.c
)

add_executable(test_viewer_monitor_layout
    test_viewer_monitor_layout.c
    ${CMAKE_SOURCE_DIR}/src/viewer_internal.c
    ${CMAKE_SOURCE_DIR}/src/viewer_monitor_layout.c
)

add_executable(test_viewer_auth
    test_viewer_auth.c
    ${CMAKE_SOURCE_DIR}/src/viewer_auth.c
)

add_executable(test_pointer_shape
    test_pointer_shape.c
    ${CMAKE_SOURCE_DIR}/src/pointer_shape.c
)

add_executable(test_viewer_pointer
    test_viewer_pointer.c
    ${CMAKE_SOURCE_DIR}/src/viewer_pointer.c
)

add_executable(test_viewer_framebuffer
    test_viewer_framebuffer.c
    ${CMAKE_SOURCE_DIR}/src/viewer_framebuffer.c
    ${CMAKE_SOURCE_DIR}/src/platform_compat.c
)

add_executable(test_viewer_publisher
    test_viewer_publisher.c
    ${CMAKE_SOURCE_DIR}/src/viewer_publisher.c
    ${CMAKE_SOURCE_DIR}/src/viewer_framebuffer.c
    ${CMAKE_SOURCE_DIR}/src/platform_compat.c
)

add_executable(test_viewer_classic_queue
    test_viewer_classic_queue.c
    ${CMAKE_SOURCE_DIR}/src/viewer_classic_queue.c
)

add_executable(test_viewer_gfx_pipeline
    test_viewer_gfx_pipeline.c
    ${CMAKE_SOURCE_DIR}/src/viewer_gfx_pipeline.c
    ${CMAKE_SOURCE_DIR}/src/viewer_gfx_dirty_limits.c
    ${CMAKE_SOURCE_DIR}/src/viewer_gfx_codec_uncompressed.c
    ${CMAKE_SOURCE_DIR}/src/viewer_gfx_codec_rfx.c
    ${CMAKE_SOURCE_DIR}/src/viewer_gfx_codec_clearcodec.c
    ${CMAKE_SOURCE_DIR}/src/viewer_internal.c
    ${CMAKE_SOURCE_DIR}/src/platform_compat.c
)

add_executable(test_viewer_gfx_codec_uncompressed
    test_viewer_gfx_codec_uncompressed.c
    ${CMAKE_SOURCE_DIR}/src/viewer_gfx_codec_uncompressed.c
)

add_executable(test_viewer_gfx_codec_rfx
    test_viewer_gfx_codec_rfx.c
    ${CMAKE_SOURCE_DIR}/src/viewer_gfx_codec_rfx.c
)

add_executable(test_viewer_gfx_codec_clearcodec
    test_viewer_gfx_codec_clearcodec.c
    ${CMAKE_SOURCE_DIR}/src/viewer_gfx_codec_clearcodec.c
)

add_executable(test_backend_gfx_decode_only
    test_backend_gfx_decode_only.c
    ${CMAKE_SOURCE_DIR}/src/backend.c
    ${CMAKE_SOURCE_DIR}/src/platform_compat.c
    ${CMAKE_SOURCE_DIR}/src/pointer_shape.c
)

add_executable(test_backend_rdp_file_options
    test_backend_rdp_file_options.c
    ${CMAKE_SOURCE_DIR}/src/backend.c
    ${CMAKE_SOURCE_DIR}/src/platform_compat.c
    ${CMAKE_SOURCE_DIR}/src/pointer_shape.c
)

if(MSVC)
    set_source_files_properties(test_viewer_gfx_pipeline.c PROPERTIES
        COMPILE_OPTIONS /wd4273
    )
endif()

add_executable(test_svc_config
    test_svc_config.c
    ${CMAKE_SOURCE_DIR}/src/svc_config.c
    ${CMAKE_SOURCE_DIR}/src/ini_parser.c
    ${CMAKE_SOURCE_DIR}/src/svc_log.c
)

target_include_directories(test_viewer_state PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_include_directories(test_viewer_monitor_layout PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_include_directories(test_viewer_auth PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

set_target_properties(test_viewer_state PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_link_libraries(test_viewer_monitor_layout
    ${FREERDP_LIB}
    ${WINPR_LIB}
)

set_target_properties(test_viewer_monitor_layout PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

set_target_properties(test_viewer_auth PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_include_directories(test_pointer_shape PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

set_target_properties(test_pointer_shape PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_include_directories(test_viewer_pointer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

set_target_properties(test_viewer_pointer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_include_directories(test_viewer_framebuffer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_link_libraries(test_viewer_framebuffer ${WINPR_LIB})

set_target_properties(test_viewer_framebuffer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_include_directories(test_viewer_publisher PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_link_libraries(test_viewer_publisher ${WINPR_LIB})

set_target_properties(test_viewer_publisher PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_include_directories(test_viewer_classic_queue PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_link_libraries(test_viewer_classic_queue ${WINPR_LIB})

set_target_properties(test_viewer_classic_queue PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_include_directories(test_viewer_gfx_pipeline PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_link_libraries(test_viewer_gfx_pipeline ${WINPR_LIB})
target_link_libraries(test_viewer_gfx_pipeline ${FREERDP_LIB})

target_compile_definitions(test_viewer_gfx_pipeline PRIVATE
    VIEWER_GFX_RFX_TESTING
)

target_include_directories(test_viewer_gfx_codec_uncompressed PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_include_directories(test_viewer_gfx_codec_rfx PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_include_directories(test_viewer_gfx_codec_clearcodec PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_compile_definitions(test_viewer_gfx_codec_rfx PRIVATE
    VIEWER_GFX_RFX_TESTING
)

target_link_libraries(test_viewer_gfx_codec_rfx
    ${FREERDP_LIB}
    ${WINPR_LIB}
)

target_link_libraries(test_viewer_gfx_codec_clearcodec
    ${FREERDP_LIB}
    ${WINPR_LIB}
)

if(MSVC)
    target_link_options(test_viewer_gfx_pipeline PRIVATE /ignore:4217)
endif()

set_target_properties(test_viewer_gfx_pipeline PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

set_target_properties(test_viewer_gfx_codec_uncompressed PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

set_target_properties(test_viewer_gfx_codec_rfx PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

set_target_properties(test_viewer_gfx_codec_clearcodec PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_include_directories(test_svc_config PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

set_target_properties(test_svc_config PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_include_directories(test_backend_gfx_decode_only PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_include_directories(test_backend_rdp_file_options PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_link_libraries(test_backend_gfx_decode_only
    ${FREERDP_CLIENT_LIB}
    ${FREERDP_SERVER_LIB}
    ${FREERDP_LIB}
    ${WINPR_LIB}
)

target_link_libraries(test_backend_rdp_file_options
    ${FREERDP_CLIENT_LIB}
    ${FREERDP_SERVER_LIB}
    ${FREERDP_LIB}
    ${WINPR_LIB}
)

set_target_properties(test_backend_gfx_decode_only PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

set_target_properties(test_backend_rdp_file_options PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

add_test(NAME test_viewer_state
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_state${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_state PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_monitor_layout
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_monitor_layout${CMAKE_EXECUTABLE_SUFFIX}
        "-DTEST_DLL_DIR=${FREERDP_BUILD}/libfreerdp/Release\;${FREERDP_BUILD}/winpr/libwinpr/Release\;${CMAKE_SOURCE_DIR}/../artifacts/windows-Release/bin"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_monitor_layout PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_auth
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_auth${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_auth PROPERTIES TIMEOUT 120)

add_test(NAME test_pointer_shape
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_pointer_shape${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_pointer_shape PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_pointer
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_pointer${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_pointer PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_framebuffer
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_framebuffer${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_framebuffer PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_publisher
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_publisher${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_publisher PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_classic_queue
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_classic_queue${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_classic_queue PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_gfx_pipeline
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_gfx_pipeline${CMAKE_EXECUTABLE_SUFFIX}
        "-DTEST_DLL_DIR=${FREERDP_BUILD}/libfreerdp/Release\;${FREERDP_BUILD}/winpr/libwinpr/Release\;${CMAKE_BINARY_DIR}/Release\;${CMAKE_SOURCE_DIR}/../artifacts/windows-Release/bin"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_gfx_pipeline PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_gfx_codec_uncompressed
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_gfx_codec_uncompressed${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_gfx_codec_uncompressed PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_gfx_codec_rfx
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_gfx_codec_rfx${CMAKE_EXECUTABLE_SUFFIX}
        "-DTEST_DLL_DIR=${FREERDP_BUILD}/libfreerdp/Release\;${FREERDP_BUILD}/winpr/libwinpr/Release\;${CMAKE_SOURCE_DIR}/../artifacts/windows-Release/bin"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_gfx_codec_rfx PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_gfx_codec_clearcodec
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_gfx_codec_clearcodec${CMAKE_EXECUTABLE_SUFFIX}
        "-DTEST_DLL_DIR=${FREERDP_BUILD}/libfreerdp/Release\;${FREERDP_BUILD}/winpr/libwinpr/Release\;${CMAKE_SOURCE_DIR}/../artifacts/windows-Release/bin"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_viewer_gfx_codec_clearcodec PROPERTIES TIMEOUT 120)

add_test(NAME test_svc_config
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_svc_config${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_svc_config PROPERTIES TIMEOUT 120)

add_test(NAME test_backend_gfx_decode_only
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_backend_gfx_decode_only${CMAKE_EXECUTABLE_SUFFIX}
        "-DTEST_DLL_DIR=${FREERDP_BUILD}/libfreerdp/Release\;${FREERDP_BUILD}/winpr/libwinpr/Release\;${FREERDP_BUILD}/client/common/Release\;${FREERDP_BUILD}/server/common/Release\;${CMAKE_SOURCE_DIR}/../artifacts/windows-Release/bin"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_backend_gfx_decode_only PROPERTIES TIMEOUT 120)

add_test(NAME test_backend_rdp_file_options
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_backend_rdp_file_options${CMAKE_EXECUTABLE_SUFFIX}
        "-DTEST_DLL_DIR=${FREERDP_BUILD}/libfreerdp/Release\;${FREERDP_BUILD}/winpr/libwinpr/Release\;${FREERDP_BUILD}/client/common/Release\;${FREERDP_BUILD}/server/common/Release\;${CMAKE_SOURCE_DIR}/../artifacts/windows-Release/bin"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_backend_rdp_file_options PROPERTIES TIMEOUT 120)

add_test(NAME test_backend_gfx_quarantine
    COMMAND ${CMAKE_COMMAND}
        -DBACKEND_SOURCE_FILE=${CMAKE_SOURCE_DIR}/src/backend.c
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckBackendGfxQuarantine.cmake
)
set_tests_properties(test_backend_gfx_quarantine PROPERTIES TIMEOUT 120)

add_test(NAME test_no_backend_gfx_replay
    COMMAND ${CMAKE_COMMAND}
        -DPRODUCTION_ROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoBackendGfxReplay.cmake
)
set_tests_properties(test_no_backend_gfx_replay PROPERTIES TIMEOUT 120)

add_test(NAME test_public_viewer_server_facade
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckPublicViewerServerFacade.cmake
)
set_tests_properties(test_public_viewer_server_facade PROPERTIES TIMEOUT 120)

add_test(NAME test_viewer_server_classic_transport_boundary
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckViewerServerClassicTransportBoundary.cmake
)
set_tests_properties(test_viewer_server_classic_transport_boundary PROPERTIES TIMEOUT 120)

add_test(NAME test_refactor_boundaries
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckRefactorBoundaries.cmake
)
set_tests_properties(test_refactor_boundaries PROPERTIES TIMEOUT 120)
