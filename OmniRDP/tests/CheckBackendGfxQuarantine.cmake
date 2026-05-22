if(NOT DEFINED BACKEND_SOURCE_FILE)
    message(FATAL_ERROR "BACKEND_SOURCE_FILE is required")
endif()

file(READ "${BACKEND_SOURCE_FILE}" BACKEND_C)

if(BACKEND_C MATCHES "viewer_server_publish_gfx_")
    message(FATAL_ERROR
        "backend.c must not call viewer_server_publish_gfx_*; backend RDPEGFX "
        "callbacks are decode/layout/refresh only.")
endif()

if(BACKEND_C MATCHES "backend_gfx_pdu_publish_allowed")
    message(FATAL_ERROR
        "backend.c must not use a backend GFX PDU publish gate; backend PDU "
        "forwarding is quarantined.")
endif()
