if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(VIEWER_SERVER_SOURCE "${SOURCE_DIR}/src/viewer_server.c")

file(READ "${VIEWER_SERVER_SOURCE}" VIEWER_SERVER_TEXT)

foreach(FORBIDDEN
        "->BitmapUpdate"
        "->SurfaceBits"
        "->SurfaceFrameMarker"
        "rdp_update_lock"
        "rdp_update_unlock")
    string(FIND "${VIEWER_SERVER_TEXT}" "${FORBIDDEN}" FOUND_AT)
    if(NOT FOUND_AT EQUAL -1)
        message(FATAL_ERROR
            "viewer_server.c contains forbidden classic transport token: ${FORBIDDEN}")
    endif()
endforeach()
