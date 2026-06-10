#ifndef VIEWER_GFX_CODEC_RFX_H
#define VIEWER_GFX_CODEC_RFX_H

#include "viewer_framebuffer.h"
#include <freerdp/channels/rdpgfx.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL viewer_gfx_rfx_is_available(void);
const char *viewer_gfx_rfx_disabled_reason(void);

typedef struct ViewerGfxRfxContext ViewerGfxRfxContext;

ViewerGfxRfxContext *viewer_gfx_rfx_context_new(void);
ViewerGfxRfxContext *viewer_gfx_rfx_context_new_ex(BOOL threaded);
void viewer_gfx_rfx_context_free(ViewerGfxRfxContext *context);
BOOL viewer_gfx_rfx_context_reset(ViewerGfxRfxContext *context, UINT32 width,
                                  UINT32 height);

/* Encode-only builders. Callers remain responsible for frame lifecycle and
 * transport sends. Builders reset/free any existing command data before
 * constructing output. */
BOOL viewer_gfx_rfx_build_surface_command(
    ViewerGfxRfxContext *context, const ViewerFramebufferSnapshot *snapshot,
    UINT16 surface_id, RDPGFX_SURFACE_COMMAND *command);
BOOL viewer_gfx_rfx_build_surface_command_rect(
    ViewerGfxRfxContext *context, const ViewerFramebufferSnapshot *snapshot,
    UINT16 surface_id, const RECTANGLE_16 *dirty_rect,
    RDPGFX_SURFACE_COMMAND *command);
BOOL viewer_gfx_rfx_build_surface_command_rects(
    ViewerGfxRfxContext *context, const ViewerFramebufferSnapshot *snapshot,
    UINT16 surface_id, const RECTANGLE_16 *dirty_rects, UINT32 dirty_rect_count,
    RDPGFX_SURFACE_COMMAND *command, BOOL *batched);
void viewer_gfx_rfx_surface_command_reset(RDPGFX_SURFACE_COMMAND *command);

#ifdef VIEWER_GFX_RFX_TESTING
void viewer_gfx_rfx_test_set_force_context_new_failure(BOOL force_failure);
UINT32 viewer_gfx_rfx_test_last_threading_flags(void);
UINT32
viewer_gfx_rfx_test_context_threading_flags(ViewerGfxRfxContext *context);
#endif

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_GFX_CODEC_RFX_H */
