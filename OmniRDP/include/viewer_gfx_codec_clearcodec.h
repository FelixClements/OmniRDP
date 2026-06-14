#ifndef VIEWER_GFX_CODEC_CLEARCODEC_H
#define VIEWER_GFX_CODEC_CLEARCODEC_H

#include "viewer_framebuffer.h"
#include <freerdp/channels/rdpgfx.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL viewer_gfx_clearcodec_is_available(void);
const char *viewer_gfx_clearcodec_disabled_reason(void);

typedef struct ViewerGfxClearCodecContext ViewerGfxClearCodecContext;

ViewerGfxClearCodecContext *viewer_gfx_clearcodec_context_new(void);
void viewer_gfx_clearcodec_context_free(ViewerGfxClearCodecContext *context);

BOOL viewer_gfx_clearcodec_build_surface_command(
    ViewerGfxClearCodecContext *context,
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    RDPGFX_SURFACE_COMMAND *command);
BOOL viewer_gfx_clearcodec_build_surface_command_rect(
    ViewerGfxClearCodecContext *context,
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    const RECTANGLE_16 *dirty_rect, RDPGFX_SURFACE_COMMAND *command);
BOOL viewer_gfx_clearcodec_build_surface_command_region(
    ViewerGfxClearCodecContext *context,
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    UINT32 source_left, UINT32 source_top, UINT32 source_right,
    UINT32 source_bottom, UINT32 dest_left, UINT32 dest_top,
    RDPGFX_SURFACE_COMMAND *command);
void viewer_gfx_clearcodec_surface_command_reset(
    RDPGFX_SURFACE_COMMAND *command);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_GFX_CODEC_CLEARCODEC_H */
