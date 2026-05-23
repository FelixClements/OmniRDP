if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(VIEWER_SERVER_HEADER "${SOURCE_DIR}/include/viewer_server.h")

file(READ "${VIEWER_SERVER_HEADER}" HEADER_TEXT)

foreach(FORBIDDEN
        "freerdp/server/rdpgfx.h"
        "freerdp/listener.h"
        "freerdp/channels/wtsvc.h"
        "winpr/wtsapi.h"
        "typedef struct ViewerClassicEvent"
        "typedef struct ViewerSurfaceBitsEvent"
        "ViewerGraphicsContext"
        "ViewerGfxPublisherState"
        "struct ViewerServer {")
    string(FIND "${HEADER_TEXT}" "${FORBIDDEN}" FOUND_AT)
    if(NOT FOUND_AT EQUAL -1)
        message(FATAL_ERROR
            "viewer_server.h facade exposes forbidden internal token: ${FORBIDDEN}")
    endif()
endforeach()
