set(_candidates
    "${TEST_BIN_DIR}/${TEST_EXECUTABLE_NAME}"
    "${TEST_BIN_DIR}/Debug/${TEST_EXECUTABLE_NAME}"
    "${TEST_BIN_DIR}/Release/${TEST_EXECUTABLE_NAME}"
    "${TEST_BIN_DIR}/RelWithDebInfo/${TEST_EXECUTABLE_NAME}"
    "${TEST_BIN_DIR}/MinSizeRel/${TEST_EXECUTABLE_NAME}"
)

set(_test_executable "")
foreach(_candidate IN LISTS _candidates)
    if(EXISTS "${_candidate}")
        set(_test_executable "${_candidate}")
        break()
    endif()
endforeach()

if(_test_executable STREQUAL "")
    message(FATAL_ERROR "Unable to locate built test executable: ${TEST_EXECUTABLE_NAME}")
endif()

execute_process(COMMAND "${_test_executable}" RESULT_VARIABLE _test_result)

if(NOT _test_result EQUAL 0)
    message(FATAL_ERROR "${TEST_EXECUTABLE_NAME} failed with exit code ${_test_result}")
endif()
