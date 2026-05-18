#include "viewer_gfx_pipeline.h"

BOOL viewer_gfx_pipeline_init(Viewer *viewer) { return viewer != NULL; }

void viewer_gfx_pipeline_uninit(Viewer *viewer) { (void)viewer; }

BOOL viewer_gfx_pipeline_activate(ViewerServer *server, Viewer *viewer) {
  (void)server;
  return viewer != NULL;
}

BOOL viewer_gfx_pipeline_send_snapshot(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot) {
  (void)server;
  return (viewer != NULL) && (snapshot != NULL) && (snapshot->pixels != NULL);
}
