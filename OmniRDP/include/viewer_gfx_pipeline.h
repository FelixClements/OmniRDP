#ifndef VIEWER_GFX_PIPELINE_H
#define VIEWER_GFX_PIPELINE_H

#include "viewer_framebuffer.h"
#include "viewer_server.h"

#ifdef __cplusplus
extern "C" {
#endif

BOOL viewer_gfx_pipeline_init(Viewer *viewer);
void viewer_gfx_pipeline_uninit(Viewer *viewer);
BOOL viewer_gfx_pipeline_activate(ViewerServer *server, Viewer *viewer);
BOOL viewer_gfx_pipeline_send_snapshot(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_GFX_PIPELINE_H */
