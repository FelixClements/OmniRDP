#ifndef VIEWER_GFX_CODEC_UNCOMPRESSED_H
#define VIEWER_GFX_CODEC_UNCOMPRESSED_H

#include "viewer_framebuffer.h"
#include <freerdp/channels/rdpgfx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Builders reset/free any existing command data before constructing output.
 * Callers must pass either a zero-initialized command or one previously
 * initialized by these builders/reset helper. */
BOOL viewer_gfx_uncompressed_build_surface_command(
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    RDPGFX_SURFACE_COMMAND *command);
BOOL viewer_gfx_uncompressed_build_surface_command_rect(
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    const RECTANGLE_16 *dirty_rect, RDPGFX_SURFACE_COMMAND *command);
BOOL viewer_gfx_uncompressed_build_surface_command_region(
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    UINT32 source_left, UINT32 source_top, UINT32 source_right,
    UINT32 source_bottom, UINT32 dest_left, UINT32 dest_top,
    RDPGFX_SURFACE_COMMAND *command);
void viewer_gfx_uncompressed_surface_command_reset(
    RDPGFX_SURFACE_COMMAND *command);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_GFX_CODEC_UNCOMPRESSED_H */
