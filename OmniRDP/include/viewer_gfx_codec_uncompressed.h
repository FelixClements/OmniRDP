#ifndef VIEWER_GFX_CODEC_UNCOMPRESSED_H
#define VIEWER_GFX_CODEC_UNCOMPRESSED_H

#include "viewer_framebuffer.h"
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/server/rdpgfx.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL viewer_gfx_uncompressed_build_surface_command(
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    RDPGFX_SURFACE_COMMAND *command);
void viewer_gfx_uncompressed_surface_command_reset(
    RDPGFX_SURFACE_COMMAND *command);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_GFX_CODEC_UNCOMPRESSED_H */
