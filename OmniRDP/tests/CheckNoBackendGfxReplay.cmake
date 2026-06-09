if(NOT DEFINED PRODUCTION_ROOT)
    message(FATAL_ERROR "PRODUCTION_ROOT is required")
endif()

set(FORBIDDEN_PATTERNS
    "ViewerGfxCompleteFrame"
    "ViewerGfxFrameBuffer"
    "viewer_gfx_replay_frame"
    "viewer_server_publish_gfx_"
    "viewer_gfx_try_schedule_late_join_replay"
    "backend_gfx_pdu_publish_allowed"
)

file(GLOB_RECURSE PRODUCTION_FILES
    "${PRODUCTION_ROOT}/include/*.h"
    "${PRODUCTION_ROOT}/src/*.c"
    "${PRODUCTION_ROOT}/src/*.h"
)

foreach(PRODUCTION_FILE IN LISTS PRODUCTION_FILES)
    file(READ "${PRODUCTION_FILE}" CONTENTS)
    foreach(FORBIDDEN IN LISTS FORBIDDEN_PATTERNS)
        if(CONTENTS MATCHES "${FORBIDDEN}")
            message(FATAL_ERROR
                "Forbidden backend GFX replay pattern '${FORBIDDEN}' found in ${PRODUCTION_FILE}")
        endif()
    endforeach()
endforeach()
