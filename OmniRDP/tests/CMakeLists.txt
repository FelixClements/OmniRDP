add_executable(test_viewer_state
    test_viewer_state.c
    ${CMAKE_SOURCE_DIR}/src/viewer_internal.c
)

add_executable(test_late_join_policy
    test_late_join_policy.c
    ${CMAKE_SOURCE_DIR}/src/viewer_internal.c
)

add_executable(test_pointer_shape
    test_pointer_shape.c
    ${CMAKE_SOURCE_DIR}/src/pointer_shape.c
)

add_executable(test_viewer_framebuffer
    test_viewer_framebuffer.c
    ${CMAKE_SOURCE_DIR}/src/viewer_framebuffer.c
)

add_executable(test_viewer_publisher
    test_viewer_publisher.c
    ${CMAKE_SOURCE_DIR}/src/viewer_publisher.c
    ${CMAKE_SOURCE_DIR}/src/viewer_framebuffer.c
)

add_executable(test_viewer_gfx_pipeline
    test_viewer_gfx_pipeline.c
    ${CMAKE_SOURCE_DIR}/src/viewer_gfx_pipeline.c
    ${CMAKE_SOURCE_DIR}/src/viewer_internal.c
    ${CMAKE_SOURCE_DIR}/src/platform_compat.c
)

add_executable(test_viewer_gfx_codec_uncompressed
    test_viewer_gfx_codec_uncompressed.c
    ${CMAKE_SOURCE_DIR}/src/viewer_gfx_codec_uncompressed.c
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

set_target_properties(test_viewer_state PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

target_include_directories(test_late_join_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

set_target_properties(test_late_join_policy PROPERTIES
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

target_include_directories(test_viewer_gfx_pipeline PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
)

target_link_libraries(test_viewer_gfx_pipeline ${WINPR_LIB})

target_include_directories(test_viewer_gfx_codec_uncompressed PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${FREERDP_ROOT}/include
    ${FREERDP_ROOT}/winpr/include
    ${FREERDP_BUILD}/include
    ${FREERDP_BUILD}/winpr/include
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

target_include_directories(test_svc_config PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

set_target_properties(test_svc_config PROPERTIES
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

add_test(NAME test_late_join_policy
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_late_join_policy${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_late_join_policy PROPERTIES TIMEOUT 120)

add_test(NAME test_pointer_shape
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_pointer_shape${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_pointer_shape PROPERTIES TIMEOUT 120)

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

add_test(NAME test_viewer_gfx_pipeline
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_gfx_pipeline${CMAKE_EXECUTABLE_SUFFIX}
        -DTEST_DLL_DIR=${CMAKE_BINARY_DIR}/Release
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

add_test(NAME test_svc_config
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_svc_config${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
set_tests_properties(test_svc_config PROPERTIES TIMEOUT 120)
