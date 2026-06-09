/*
 * Internal viewer-server RDPEGFX pipeline interface.
 *
 * This module coordinates concrete Viewer/ViewerServer state and is allowed to
 * depend on viewer_server_internal.h. Do not expose this header from the public
 * include tree.
 */
#ifndef VIEWER_GFX_PIPELINE_H
#define VIEWER_GFX_PIPELINE_H

#include "viewer_framebuffer.h"
#include "viewer_server_internal.h"

#include <freerdp/freerdp.h>
#include <freerdp/peer.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL viewer_gfx_pipeline_init(Viewer *viewer);
void viewer_gfx_pipeline_uninit(Viewer *viewer);

#define VIEWER_GFX_PIPELINE_CAPS_ACTION_DISABLE_RDPEGFX 0x00000001U
#define VIEWER_GFX_PIPELINE_CAPS_ACTION_ENTER_CLASSIC_FALLBACK 0x00000002U
#define VIEWER_GFX_PIPELINE_CAPS_ACTION_BEGIN_JOIN 0x00000004U

#define VIEWER_GFX_JOIN_ACTION_NONE 0x00000000U
#define VIEWER_GFX_JOIN_ACTION_SEND_BASELINE 0x00000001U
#define VIEWER_GFX_JOIN_ACTION_ENTER_CLASSIC_FALLBACK 0x00000002U
#define VIEWER_GFX_JOIN_ACTION_ENQUEUE_CLASSIC_BASELINE 0x00000004U
#define VIEWER_GFX_JOIN_ACTION_SEND_POINTER_BASELINE 0x00000008U

typedef struct {
  UINT32 actions;
  UINT channel_rc;
  const char *classic_fallback_reason;
  const char *begin_join_reason;
} ViewerGfxPipelineCapsResult;

typedef struct {
  UINT32 actions;
  const char *classic_fallback_reason;
  const char *log_reason;
} ViewerGfxJoinResult;

typedef enum {
  VIEWER_GFX_DIRTY_PACING_OK = 0,
  VIEWER_GFX_DIRTY_PACING_SUSPENDED,
  VIEWER_GFX_DIRTY_PACING_INVALID
} ViewerGfxDirtyPacingStatus;

typedef enum {
  VIEWER_GFX_DIRTY_SEND_SENT = 0,
  VIEWER_GFX_DIRTY_SEND_DEFERRED,
  VIEWER_GFX_DIRTY_SEND_FAILED
} ViewerGfxDirtySendStatus;

typedef struct {
  RECTANGLE_16 rects[VIEWER_GFX_PENDING_DIRTY_MAX_RECTS];
  UINT32 rect_count;
  UINT64 start_generation;
  UINT64 latest_generation;
  UINT64 area;
  UINT64 update_count;
  UINT32 width;
  UINT32 height;
  BOOL full_frame;
  BOOL overflow;
  const char *full_frame_reason;
} ViewerGfxPendingDirtyBatch;

BOOL viewer_gfx_pipeline_post_connect_locked(ViewerServer *server,
                                             Viewer *viewer, freerdp_peer *peer,
                                             BOOL gfx_enabled);
UINT viewer_gfx_pipeline_caps_advertise(
    RdpgfxServerContext *context,
    const RDPGFX_CAPS_ADVERTISE_PDU *caps_advertise);
BOOL viewer_gfx_pipeline_open_if_ready_locked(Viewer *viewer);
HANDLE viewer_gfx_pipeline_get_event_handle_locked(Viewer *viewer);
BOOL viewer_gfx_pipeline_handle_messages_locked(
    Viewer *viewer, ViewerGfxPipelineCapsResult *caps_result);
BOOL viewer_gfx_pipeline_activate(ViewerServer *server, Viewer *viewer);
void viewer_gfx_pipeline_join_result_clear(ViewerGfxJoinResult *result);
void viewer_gfx_pipeline_begin_join_locked(Viewer *viewer, UINT64 now,
                                           const char *reason);
void viewer_gfx_pipeline_finish_join_locked(Viewer *viewer, const char *reason);
void viewer_gfx_pipeline_disable_rdpgfx_locked(Viewer *viewer);
void viewer_gfx_pipeline_reset_join_state_locked(ViewerGraphicsContext *gfx);
void viewer_gfx_pipeline_reject_join(Viewer *viewer, const char *reason);
void viewer_gfx_pipeline_enter_classic_fallback(Viewer *viewer, UINT64 now,
                                                const char *reason,
                                                ViewerGfxJoinResult *result);
void viewer_gfx_pipeline_on_baseline_result(Viewer *viewer, UINT64 now,
                                            BOOL sent,
                                            ViewerGfxJoinResult *result);
void viewer_gfx_pipeline_on_baseline_unavailable(Viewer *viewer, UINT64 now,
                                                 ViewerGfxJoinResult *result);
void viewer_gfx_pipeline_on_peer_activated(Viewer *viewer, UINT64 now,
                                           ViewerGfxJoinResult *result);
void viewer_gfx_pipeline_step_join(ViewerServer *server, Viewer *viewer,
                                   UINT64 now, ViewerGfxJoinResult *result);
BOOL viewer_gfx_pipeline_dirty_update_allowed(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot, const char **reason);
void viewer_gfx_pipeline_reset_dirty_state_locked(ViewerGraphicsContext *gfx);
void viewer_gfx_pipeline_invalidate_surface_locked(ViewerGraphicsContext *gfx);
void viewer_gfx_pipeline_pending_dirty_clear_locked(ViewerGraphicsContext *gfx);
BOOL viewer_gfx_pipeline_pending_dirty_add_locked(
    ViewerGraphicsContext *gfx, const RECTANGLE_16 *dirty_rects,
    UINT32 dirty_rect_count, BOOL dirty_overflow, UINT64 generation,
    UINT32 width, UINT32 height);
BOOL viewer_gfx_pipeline_pending_dirty_move_locked(
    ViewerGraphicsContext *gfx, ViewerGfxPendingDirtyBatch *batch);
BOOL viewer_gfx_pipeline_pending_dirty_remerge_locked(
    ViewerGraphicsContext *gfx, const ViewerGfxPendingDirtyBatch *batch,
    UINT32 width, UINT32 height);
BOOL viewer_gfx_pipeline_note_dirty_deferred_locked(ViewerGraphicsContext *gfx);
BOOL viewer_gfx_pipeline_snapshot_apply_pending_dirty(
    ViewerFramebufferSnapshot *snapshot,
    const ViewerGfxPendingDirtyBatch *batch);
BOOL viewer_gfx_pipeline_estimate_uncompressed_dirty_payload(
    const ViewerFramebufferSnapshot *snapshot, UINT64 *payload_bytes);
ViewerGfxDirtyPacingStatus
viewer_gfx_pipeline_poll_dirty_pacing(Viewer *viewer, UINT64 now,
                                      const char **reason);
BOOL viewer_gfx_pipeline_send_dirty_update(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot);
ViewerGfxDirtySendStatus viewer_gfx_pipeline_send_dirty_update_result(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot);
BOOL viewer_gfx_pipeline_monitor_layout_snapshot(
    ViewerServer *server, const ViewerFramebufferSnapshot *snapshot,
    MonitorLayout *layout);
UINT viewer_gfx_pipeline_handle_frame_ack(Viewer *viewer, UINT32 frame_id);
UINT viewer_gfx_pipeline_handle_frame_ack_pdu(
    Viewer *viewer, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frame_acknowledge);
BOOL viewer_gfx_pipeline_send_snapshot(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_GFX_PIPELINE_H */
