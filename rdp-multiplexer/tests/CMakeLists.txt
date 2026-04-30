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

add_test(NAME test_viewer_state
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_viewer_state${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)

add_test(NAME test_late_join_policy
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_late_join_policy${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)

add_test(NAME test_pointer_shape
    COMMAND ${CMAKE_COMMAND}
        -DTEST_BIN_DIR=${CMAKE_CURRENT_BINARY_DIR}
        -DTEST_EXECUTABLE_NAME=test_pointer_shape${CMAKE_EXECUTABLE_SUFFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/RunBuiltTest.cmake
)
