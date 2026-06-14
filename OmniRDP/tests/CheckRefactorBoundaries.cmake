if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

function(check_text_forbidden TEXT_VARIABLE FILE_PATH BOUNDARY)
    foreach(FORBIDDEN IN LISTS ARGN)
        string(FIND "${${TEXT_VARIABLE}}" "${FORBIDDEN}" FOUND_AT)
        if(NOT FOUND_AT EQUAL -1)
            message(FATAL_ERROR
                "${BOUNDARY}: forbidden token '${FORBIDDEN}' found in ${FILE_PATH}")
        endif()
    endforeach()
endfunction()

set(PRODUCTION_REPLAY_TOKENS
    "ViewerGfxCompleteFrame"
    "ViewerGfxFrameBuffer"
    "viewer_gfx_replay_frame"
    "viewer_gfx_try_schedule_late_join_replay"
    "viewer_server_publish_gfx_"
    "backend_gfx_pdu_publish_allowed"
)

file(GLOB_RECURSE PRODUCTION_FILES
    "${SOURCE_DIR}/include/*.h"
    "${SOURCE_DIR}/src/*.c"
    "${SOURCE_DIR}/src/*.h"
)

foreach(PRODUCTION_FILE IN LISTS PRODUCTION_FILES)
    file(READ "${PRODUCTION_FILE}" PRODUCTION_TEXT)
    check_text_forbidden(PRODUCTION_TEXT "${PRODUCTION_FILE}"
        "backend replay resurrection audit" ${PRODUCTION_REPLAY_TOKENS})
endforeach()

set(BACKEND_SOURCE "${SOURCE_DIR}/src/backend.c")
file(READ "${BACKEND_SOURCE}" BACKEND_TEXT)
check_text_forbidden(BACKEND_TEXT "${BACKEND_SOURCE}"
    "backend RDPEGFX callback quarantine"
    "viewer_server_publish_gfx_"
    "backend_gfx_pdu_publish_allowed"
    "viewer_gfx_replay"
    "viewer_gfx_pipeline_send")

set(VIEWER_SERVER_SOURCE "${SOURCE_DIR}/src/viewer_server.c")
file(READ "${VIEWER_SERVER_SOURCE}" VIEWER_SERVER_TEXT)
check_text_forbidden(VIEWER_SERVER_TEXT "${VIEWER_SERVER_SOURCE}"
    "viewer_server RDPEGFX transport boundary"
    "rdpgfx->StartFrame"
    "rdpgfx->SurfaceCommand"
    "rdpgfx->EndFrame"
    "rdpgfx->ResetGraphics"
    "rdpgfx->CreateSurface"
    "rdpgfx->MapSurfaceToOutput")
check_text_forbidden(VIEWER_SERVER_TEXT "${VIEWER_SERVER_SOURCE}"
    "viewer_server classic transport boundary"
    "->BitmapUpdate"
    "->SurfaceBits"
    "->SurfaceFrameMarker"
    "rdp_update_lock"
    "rdp_update_unlock")
check_text_forbidden(VIEWER_SERVER_TEXT "${VIEWER_SERVER_SOURCE}"
    "viewer_server pointer transport boundary"
    "PointerSystem"
    "PointerNew"
    "PointerColor"
    "PointerPosition")

set(FRAMEBUFFER_TOKENS
    "freerdp_peer"
    "rdpContext"
    "RdpgfxServerContext"
    "RDPGFX_"
    "SURFACE_BITS_COMMAND"
    "BITMAP_UPDATE"
    "IFCALL"
    "rdp_update_lock"
    "rdp_update_unlock"
    "->BitmapUpdate"
    "->SurfaceBits"
    "->SurfaceFrameMarker"
    "->StartFrame"
    "->SurfaceCommand"
    "->EndFrame"
)

foreach(FRAMEBUFFER_FILE
        "${SOURCE_DIR}/src/viewer_framebuffer.c"
        "${SOURCE_DIR}/include/viewer_framebuffer.h")
    file(READ "${FRAMEBUFFER_FILE}" FRAMEBUFFER_TEXT)
    check_text_forbidden(FRAMEBUFFER_TEXT "${FRAMEBUFFER_FILE}"
        "framebuffer ownership boundary" ${FRAMEBUFFER_TOKENS})
endforeach()

set(CODEC_TOKENS
    "freerdp_peer"
    "rdpContext"
    "RdpgfxServerContext"
    "rdpgfx->"
    "->StartFrame"
    "->SurfaceCommand"
    "->EndFrame"
    "->ResetGraphics"
    "->CreateSurface"
    "->MapSurfaceToOutput"
    "IFCALL"
    "WTSVirtualChannel"
)

foreach(CODEC_FILE
        "${SOURCE_DIR}/src/viewer_gfx_codec_uncompressed.c"
        "${SOURCE_DIR}/include/viewer_gfx_codec_uncompressed.h"
        "${SOURCE_DIR}/src/viewer_gfx_codec_rfx.c"
        "${SOURCE_DIR}/include/viewer_gfx_codec_rfx.h"
        "${SOURCE_DIR}/src/viewer_gfx_codec_clearcodec.c"
        "${SOURCE_DIR}/include/viewer_gfx_codec_clearcodec.h")
    file(READ "${CODEC_FILE}" CODEC_TEXT)
    check_text_forbidden(CODEC_TEXT "${CODEC_FILE}"
        "GFX codec ownership boundary" ${CODEC_TOKENS})
endforeach()

set(VIEWER_SERVER_HEADER "${SOURCE_DIR}/include/viewer_server.h")
file(READ "${VIEWER_SERVER_HEADER}" VIEWER_SERVER_HEADER_TEXT)
check_text_forbidden(VIEWER_SERVER_HEADER_TEXT "${VIEWER_SERVER_HEADER}"
    "public viewer_server facade boundary"
    "freerdp/server/rdpgfx.h"
    "freerdp/listener.h"
    "freerdp/channels/wtsvc.h"
    "winpr/wtsapi.h"
    "RdpgfxServerContext"
    "ViewerClassicQueue"
    "ViewerClassicTransport"
    "typedef struct ViewerClassicEvent"
    "typedef struct ViewerSurfaceBitsEvent"
    "ViewerGraphicsContext"
    "ViewerGfxPublisherState"
    "struct ViewerServer {")

set(PUBLIC_VIEWER_GFX_PIPELINE_HEADER
    "${SOURCE_DIR}/include/viewer_gfx_pipeline.h")
if(EXISTS "${PUBLIC_VIEWER_GFX_PIPELINE_HEADER}")
    message(FATAL_ERROR
        "public include boundary: viewer_gfx_pipeline.h must stay internal, found ${PUBLIC_VIEWER_GFX_PIPELINE_HEADER}")
endif()

file(GLOB_RECURSE PUBLIC_HEADERS "${SOURCE_DIR}/include/*.h")
foreach(PUBLIC_HEADER IN LISTS PUBLIC_HEADERS)
    file(READ "${PUBLIC_HEADER}" PUBLIC_HEADER_TEXT)
    check_text_forbidden(PUBLIC_HEADER_TEXT "${PUBLIC_HEADER}"
        "public include boundary"
        "#include \"viewer_server_internal.h\""
        "#include <viewer_server_internal.h>"
        "#include \"viewer_gfx_pipeline.h\""
        "#include <viewer_gfx_pipeline.h>")
endforeach()
