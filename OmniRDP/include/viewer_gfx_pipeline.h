#ifndef VIEWER_GFX_PIPELINE_H
#define VIEWER_GFX_PIPELINE_H

#include "viewer_framebuffer.h"
#include "viewer_server.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL viewer_gfx_pipeline_init(Viewer *viewer);
void viewer_gfx_pipeline_uninit(Viewer *viewer);
typedef UINT (*ViewerGfxFrameAcknowledgeCallback)(
    RdpgfxServerContext *context,
    const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frame_acknowledge);

#define VIEWER_GFX_PIPELINE_CAPS_ACTION_DISABLE_RDPEGFX 0x00000001U
#define VIEWER_GFX_PIPELINE_CAPS_ACTION_ENTER_CLASSIC_FALLBACK 0x00000002U
#define VIEWER_GFX_PIPELINE_CAPS_ACTION_BEGIN_JOIN 0x00000004U

typedef struct {
  UINT32 actions;
  UINT channel_rc;
  const char *classic_fallback_reason;
  const char *begin_join_reason;
} ViewerGfxPipelineCapsResult;

BOOL viewer_gfx_pipeline_post_connect_locked(
    ViewerServer *server, Viewer *viewer, freerdp_peer *peer, BOOL gfx_enabled,
    ViewerGfxFrameAcknowledgeCallback frame_acknowledge);
UINT viewer_gfx_pipeline_caps_advertise(
    RdpgfxServerContext *context,
    const RDPGFX_CAPS_ADVERTISE_PDU *caps_advertise);
BOOL viewer_gfx_pipeline_open_if_ready_locked(Viewer *viewer);
HANDLE viewer_gfx_pipeline_get_event_handle_locked(Viewer *viewer);
BOOL viewer_gfx_pipeline_handle_messages_locked(
    Viewer *viewer, ViewerGfxPipelineCapsResult *caps_result);
BOOL viewer_gfx_pipeline_activate(ViewerServer *server, Viewer *viewer);
BOOL viewer_gfx_pipeline_send_event(Viewer *viewer,
                                    const ViewerGfxEvent *event);
BOOL viewer_gfx_pipeline_send_surface_preamble(ViewerServer *server,
                                               Viewer *viewer);
BOOL viewer_gfx_pipeline_dirty_update_allowed(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot, const char **reason);
BOOL viewer_gfx_pipeline_send_snapshot(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_GFX_PIPELINE_H */
