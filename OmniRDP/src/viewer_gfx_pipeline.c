#include "viewer_gfx_pipeline.h"
#include "platform_compat.h"
#include "viewer_gfx_codec_uncompressed.h"
#include "viewer_internal.h"

#include <freerdp/channels/drdynvc.h>
#include <freerdp/codec/color.h>
#include <freerdp/server/rdpgfx.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <winpr/wlog.h>
#include <winpr/wtsapi.h>

#define TAG "multiplexer.viewer.gfx"

static void viewer_gfx_pipeline_caps_result_clear_locked(Viewer *viewer) {
  if (!viewer)
    return;

  viewer->gfx.pending_caps_actions = 0;
  viewer->gfx.pending_caps_channel_rc = CHANNEL_RC_OK;
  viewer->gfx.pending_caps_classic_fallback_reason = NULL;
  viewer->gfx.pending_caps_begin_join_reason = NULL;
}

static void viewer_gfx_pipeline_caps_result_add_locked(Viewer *viewer,
                                                       UINT32 actions,
                                                       UINT channel_rc,
                                                       const char *fallback,
                                                       const char *begin_join) {
  if (!viewer)
    return;

  viewer->gfx.pending_caps_actions |= actions;
  viewer->gfx.pending_caps_channel_rc = channel_rc;
  if (fallback)
    viewer->gfx.pending_caps_classic_fallback_reason = fallback;
  if (begin_join)
    viewer->gfx.pending_caps_begin_join_reason = begin_join;
}

static void
viewer_gfx_pipeline_dirty_map_clear_locked(ViewerGraphicsContext *gfx) {
  if (!gfx)
    return;

  memset(gfx->dirty_frame_ids, 0, sizeof(gfx->dirty_frame_ids));
  memset(gfx->dirty_frame_generations, 0, sizeof(gfx->dirty_frame_generations));
  memset(gfx->dirty_frame_sent_ts, 0, sizeof(gfx->dirty_frame_sent_ts));
  memset(gfx->dirty_frame_valid, 0, sizeof(gfx->dirty_frame_valid));
  gfx->dirty_in_flight_frames = 0;
  gfx->dirty_suspended_for_no_ack = FALSE;
}

static void
viewer_gfx_pipeline_handle_frame_ack_locked(ViewerGraphicsContext *gfx,
                                            UINT32 frame_id) {
  UINT32 i = 0;

  if (!gfx || !gfx->initialized || (frame_id == 0))
    return;

  for (i = 0; i < VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY; i++) {
    if (!gfx->dirty_frame_valid[i] || (gfx->dirty_frame_ids[i] != frame_id))
      continue;

    gfx->dirty_last_acked_generation = gfx->dirty_frame_generations[i];
    gfx->dirty_frame_valid[i] = FALSE;
    gfx->dirty_frame_ids[i] = 0;
    gfx->dirty_frame_generations[i] = 0;
    gfx->dirty_frame_sent_ts[i] = 0;
    if (gfx->dirty_in_flight_frames > 0)
      gfx->dirty_in_flight_frames--;
    if (gfx->dirty_in_flight_frames == 0)
      gfx->dirty_suspended_for_no_ack = FALSE;
    break;
  }
}

static UINT viewer_gfx_pipeline_frame_acknowledge(
    RdpgfxServerContext *context,
    const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frame_acknowledge) {
  Viewer *viewer = context ? (Viewer *)context->custom : NULL;

  if (!viewer || !frame_acknowledge)
    return ERROR_INVALID_PARAMETER;

  return viewer_gfx_pipeline_handle_frame_ack(viewer,
                                              frame_acknowledge->frameId);
}

void viewer_gfx_pipeline_reset_dirty_state_locked(ViewerGraphicsContext *gfx) {
  if (!gfx)
    return;

  gfx->dirty_last_sent_generation = 0;
  gfx->dirty_last_acked_generation = 0;
  gfx->dirty_reset_generation = 0;
  viewer_gfx_pipeline_dirty_map_clear_locked(gfx);
  gfx->dirty_max_in_flight_frames = 1;
  gfx->dirty_suspended_for_no_ack = FALSE;
  gfx->dirty_updates_enabled = FALSE;
  gfx->dirty_baseline_required = TRUE;
}

static void viewer_gfx_pipeline_caps_result_consume_locked(
    Viewer *viewer, ViewerGfxPipelineCapsResult *result) {
  if (result)
    memset(result, 0, sizeof(*result));
  if (!viewer || !result)
    return;

  result->actions = viewer->gfx.pending_caps_actions;
  result->channel_rc = viewer->gfx.pending_caps_channel_rc;
  result->classic_fallback_reason =
      viewer->gfx.pending_caps_classic_fallback_reason;
  result->begin_join_reason = viewer->gfx.pending_caps_begin_join_reason;
  viewer_gfx_pipeline_caps_result_clear_locked(viewer);
}

static const char *viewer_gfx_pipeline_join_state_name(ViewerJoinState state) {
  switch (state) {
  case VIEWER_JOIN_STATE_NONE:
    return "none";
  case VIEWER_JOIN_STATE_PENDING:
    return "pending";
  case VIEWER_JOIN_STATE_LIVE:
    return "live";
  case VIEWER_JOIN_STATE_REJECTED:
    return "rejected";
  default:
    return "unknown";
  }
}

static const char *
viewer_gfx_pipeline_join_strategy_name(ViewerJoinStrategy strategy) {
  switch (strategy) {
  case VIEWER_JOIN_STRATEGY_NONE:
    return "none";
  case VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK:
    return "classic_fallback";
  case VIEWER_JOIN_STRATEGY_REJECT:
    return "reject";
  default:
    return "unknown";
  }
}

static void
viewer_gfx_pipeline_set_join_state_locked(Viewer *viewer, ViewerJoinState state,
                                          ViewerJoinStrategy strategy,
                                          const char *reason) {
  ViewerJoinState old_state = VIEWER_JOIN_STATE_NONE;
  ViewerJoinStrategy old_strategy = VIEWER_JOIN_STRATEGY_NONE;

  if (!viewer)
    return;

  old_state = viewer->gfx.join_state;
  old_strategy = viewer->gfx.join_strategy;
  viewer->gfx.join_state = state;
  viewer->gfx.join_strategy = strategy;

  if ((old_state != state) || (old_strategy != strategy)) {
    WLog_INFO(TAG, "Viewer %u join transition %s/%s -> %s/%s reason=%s",
              viewer->id, viewer_gfx_pipeline_join_state_name(old_state),
              viewer_gfx_pipeline_join_strategy_name(old_strategy),
              viewer_gfx_pipeline_join_state_name(state),
              viewer_gfx_pipeline_join_strategy_name(strategy),
              reason ? reason : "unspecified");
  }
}

static BOOL viewer_gfx_pipeline_handshake_ready_locked(const Viewer *viewer) {
  return viewer && viewer->activated && viewer->gfx.post_connect_complete &&
         (viewer->gfx.drdynvc_state == DRDYNVC_STATE_READY) &&
         viewer->gfx.channel_opened && viewer->gfx.caps_ready &&
         viewer_gfx_negotiation_is_rdpgfx_ready(&viewer->gfx) &&
         !viewer->gfx.rdpgfx_temporarily_disabled;
}

void viewer_gfx_pipeline_join_result_clear(ViewerGfxJoinResult *result) {
  if (result)
    memset(result, 0, sizeof(*result));
}

void viewer_gfx_pipeline_begin_join_locked(Viewer *viewer, UINT64 now,
                                           const char *reason) {
  if (!viewer)
    return;

  viewer->gfx.join_start_ts = now;
  viewer_gfx_pipeline_set_join_state_locked(viewer, VIEWER_JOIN_STATE_PENDING,
                                            VIEWER_JOIN_STRATEGY_NONE, reason);
}

void viewer_gfx_pipeline_finish_join_locked(Viewer *viewer,
                                            const char *reason) {
  if (!viewer)
    return;

  viewer->gfx.last_activated_ts = platform_get_timestamp_ms();
  viewer_gfx_pipeline_set_join_state_locked(viewer, VIEWER_JOIN_STATE_LIVE,
                                            VIEWER_JOIN_STRATEGY_NONE, reason);
}

void viewer_gfx_pipeline_disable_rdpgfx_locked(Viewer *viewer) {
  if (!viewer)
    return;

  viewer->gfx.ready = FALSE;
  viewer->gfx.use_rdpgfx = FALSE;
  viewer->gfx.caps_ready = FALSE;
  viewer->gfx.rdpgfx_temporarily_disabled = TRUE;
  viewer->gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK;
}

void viewer_gfx_pipeline_reset_join_state_locked(ViewerGraphicsContext *gfx) {
  if (!gfx)
    return;

  gfx->use_rdpgfx = FALSE;
  gfx->rdpgfx_temporarily_disabled = FALSE;
  gfx->negotiation_outcome = VIEWER_GFX_NEGOTIATION_PENDING;
  gfx->join_state = VIEWER_JOIN_STATE_NONE;
  gfx->join_strategy = VIEWER_JOIN_STRATEGY_NONE;
  gfx->join_start_ts = 0;
  gfx->last_activated_ts = 0;
}

void viewer_gfx_pipeline_reject_join(Viewer *viewer, const char *reason) {
  if (!viewer)
    return;

  EnterCriticalSection(&viewer->gfx.lock);
  viewer_gfx_pipeline_set_join_state_locked(
      viewer, VIEWER_JOIN_STATE_REJECTED, VIEWER_JOIN_STRATEGY_REJECT, reason);
  LeaveCriticalSection(&viewer->gfx.lock);
}

static void
viewer_gfx_pipeline_enter_classic_fallback_locked(Viewer *viewer, UINT64 now,
                                                  const char *reason) {
  if (!viewer)
    return;

  viewer_gfx_pipeline_disable_rdpgfx_locked(viewer);
  viewer->gfx.join_start_ts = now;
  viewer_gfx_pipeline_set_join_state_locked(
      viewer, VIEWER_JOIN_STATE_PENDING, VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK,
      reason);
}

void viewer_gfx_pipeline_enter_classic_fallback(Viewer *viewer, UINT64 now,
                                                const char *reason,
                                                ViewerGfxJoinResult *result) {
  viewer_gfx_pipeline_join_result_clear(result);
  if (!viewer)
    return;

  EnterCriticalSection(&viewer->gfx.lock);
  viewer_gfx_pipeline_enter_classic_fallback_locked(viewer, now, reason);
  LeaveCriticalSection(&viewer->gfx.lock);

  if (result) {
    result->actions = VIEWER_GFX_JOIN_ACTION_ENTER_CLASSIC_FALLBACK;
    result->classic_fallback_reason = reason;
  }
}

void viewer_gfx_pipeline_on_baseline_result(Viewer *viewer, UINT64 now,
                                            BOOL sent,
                                            ViewerGfxJoinResult *result) {
  viewer_gfx_pipeline_join_result_clear(result);
  if (!viewer)
    return;

  EnterCriticalSection(&viewer->gfx.lock);
  if (sent) {
    viewer_gfx_pipeline_finish_join_locked(
        viewer, "RDPEGFX framebuffer full-frame baseline sent");
  } else
    viewer_gfx_pipeline_enter_classic_fallback_locked(
        viewer, now, "RDPEGFX framebuffer baseline failed");
  LeaveCriticalSection(&viewer->gfx.lock);

  if (!sent && result) {
    result->actions = VIEWER_GFX_JOIN_ACTION_ENTER_CLASSIC_FALLBACK;
    result->classic_fallback_reason = "RDPEGFX framebuffer baseline failed";
  }
}

void viewer_gfx_pipeline_on_peer_activated(Viewer *viewer, UINT64 now,
                                           ViewerGfxJoinResult *result) {
  viewer_gfx_pipeline_join_result_clear(result);
  if (!viewer)
    return;

  EnterCriticalSection(&viewer->gfx.lock);
  viewer->gfx.ready = TRUE;
  if (viewer_gfx_negotiation_is_rdpgfx_ready(&viewer->gfx)) {
    viewer_gfx_pipeline_begin_join_locked(
        viewer, now, "peer activated for RDPEGFX late join");
  } else if (viewer_gfx_activation_waits_for_rdpgfx_caps(&viewer->gfx)) {
    viewer_gfx_pipeline_begin_join_locked(
        viewer, now, "peer activated waiting for RDPEGFX caps confirmation");
  } else {
    viewer->gfx.join_start_ts = now;
    viewer_gfx_pipeline_finish_join_locked(viewer,
                                           "peer activated on classic path");
    if (result)
      result->actions = VIEWER_GFX_JOIN_ACTION_ENQUEUE_CLASSIC_BASELINE;
  }
  LeaveCriticalSection(&viewer->gfx.lock);
}

void viewer_gfx_pipeline_step_join(ViewerServer *server, Viewer *viewer,
                                   UINT64 now, ViewerGfxJoinResult *result) {
  ViewerJoinState state = VIEWER_JOIN_STATE_NONE;
  ViewerJoinStrategy strategy = VIEWER_JOIN_STRATEGY_NONE;

  (void)now;
  viewer_gfx_pipeline_join_result_clear(result);
  if (!server || !viewer || !result)
    return;

  EnterCriticalSection(&viewer->gfx.lock);
  if (!viewer_gfx_pipeline_handshake_ready_locked(viewer)) {
    LeaveCriticalSection(&viewer->gfx.lock);
    return;
  }
  state = viewer->gfx.join_state;
  strategy = viewer->gfx.join_strategy;
  LeaveCriticalSection(&viewer->gfx.lock);

  if (state == VIEWER_JOIN_STATE_LIVE)
    return;
  if (strategy == VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK) {
    result->actions = VIEWER_GFX_JOIN_ACTION_ENTER_CLASSIC_FALLBACK;
    result->classic_fallback_reason = "caps or handshake fallback";
    return;
  }
  if ((state == VIEWER_JOIN_STATE_PENDING) && server->viewer_gfx_enabled) {
    result->actions = VIEWER_GFX_JOIN_ACTION_SEND_BASELINE;
    result->log_reason = "RDPEGFX pending canonical baseline";
  }
}

BOOL viewer_gfx_pipeline_init(Viewer *viewer) { return viewer != NULL; }

void viewer_gfx_pipeline_uninit(Viewer *viewer) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;

  if (!gfx || !gfx->initialized)
    return;

  EnterCriticalSection(&gfx->lock);
  if (gfx->rdpgfx) {
    if (gfx->channel_opened && gfx->rdpgfx->Close)
      (void)gfx->rdpgfx->Close(gfx->rdpgfx);
    rdpgfx_server_context_free(gfx->rdpgfx);
    gfx->rdpgfx = NULL;
  }
  if (gfx->vcm) {
    WTSCloseServer(gfx->vcm);
    gfx->vcm = NULL;
  }
  gfx->channel_opened = FALSE;
  LeaveCriticalSection(&gfx->lock);
}

BOOL viewer_gfx_pipeline_post_connect_locked(ViewerServer *server,
                                             Viewer *viewer, freerdp_peer *peer,
                                             BOOL gfx_enabled) {
  RdpgfxServerContext *rdpgfx = NULL;

  if (!viewer || !peer || !peer->context)
    return FALSE;

  viewer->gfx.pipeline_server = server;
  viewer_gfx_pipeline_caps_result_clear_locked(viewer);

  /* Create VCM after peer->Initialize has populated context->rdp. */
  viewer->gfx.vcm = WTSOpenServerA((LPSTR)peer->context);
  if (!viewer->gfx.vcm || (viewer->gfx.vcm == INVALID_HANDLE_VALUE)) {
    WLog_ERR(TAG, "Viewer %u WTSOpenServerA failed in post_connect",
             viewer->id);
    return FALSE;
  }

  if (!gfx_enabled) {
    WLog_INFO(TAG,
              "Viewer %u RDPEGFX disabled; using classic SurfaceBits path only",
              viewer->id);
    viewer->gfx.rdpgfx = NULL;
    viewer->gfx.post_connect_complete = TRUE;
    viewer->gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK;
    return TRUE;
  }

  rdpgfx = rdpgfx_server_context_new(viewer->gfx.vcm);
  if (!rdpgfx)
    return FALSE;

  rdpgfx->custom = viewer;
  rdpgfx->rdpcontext = peer->context;
  rdpgfx->CapsAdvertise = viewer_gfx_pipeline_caps_advertise;
  rdpgfx->FrameAcknowledge = viewer_gfx_pipeline_frame_acknowledge;
  if (!rdpgfx->Initialize(rdpgfx, TRUE)) {
    rdpgfx_server_context_free(rdpgfx);
    return FALSE;
  }

  viewer->gfx.rdpgfx = rdpgfx;
  viewer->gfx.post_connect_complete = TRUE;
  return TRUE;
}

UINT viewer_gfx_pipeline_caps_advertise(
    RdpgfxServerContext *context,
    const RDPGFX_CAPS_ADVERTISE_PDU *caps_advertise) {
  Viewer *viewer = context ? (Viewer *)context->custom : NULL;
  ViewerServer *server = viewer ? viewer->gfx.pipeline_server : NULL;
  RDPGFX_CAPSET caps = {0};
  RDPGFX_CAPS_CONFIRM_PDU confirm = {0};
  UINT rc = CHANNEL_RC_OK;
  BOOL selected = FALSE;
  BOOL caps_ready_was = FALSE;
  BOOL use_rdpgfx_was = FALSE;
  ViewerGfxNegotiationOutcome outcome_was = VIEWER_GFX_NEGOTIATION_PENDING;

  if (!viewer || !server || !caps_advertise || !context->CapsConfirm)
    return ERROR_INVALID_PARAMETER;

  EnterCriticalSection(&server->gfx.lock);
  selected = viewer_gfx_select_compatible_caps(
      server->gfx.canonical_caps_valid ? &server->gfx.canonical_caps : NULL,
      server->gfx.canonical_caps_valid, caps_advertise->capsSets,
      caps_advertise->capsSetCount, &caps);
  if (selected && !server->gfx.canonical_caps_valid) {
    server->gfx.canonical_caps = caps;
    server->gfx.canonical_caps_valid = TRUE;
  }
  LeaveCriticalSection(&server->gfx.lock);

  if (!selected) {
    WLog_WARN(TAG,
              "Viewer %u advertised incompatible RDPEGFX caps; staying on "
              "classic path",
              viewer->id);
    EnterCriticalSection(&viewer->gfx.lock);
    if (viewer->gfx.negotiation_outcome ==
        VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK) {
      LeaveCriticalSection(&viewer->gfx.lock);
      return CHANNEL_RC_OK;
    }
    viewer_gfx_pipeline_caps_result_add_locked(
        viewer,
        VIEWER_GFX_PIPELINE_CAPS_ACTION_DISABLE_RDPEGFX |
            (viewer->activated
                 ? VIEWER_GFX_PIPELINE_CAPS_ACTION_ENTER_CLASSIC_FALLBACK
                 : 0U),
        CHANNEL_RC_OK, viewer->activated ? "incompatible RDPEGFX caps" : NULL,
        NULL);
    LeaveCriticalSection(&viewer->gfx.lock);
    return CHANNEL_RC_OK;
  }

  EnterCriticalSection(&viewer->gfx.lock);
  caps_ready_was = viewer->gfx.caps_ready;
  use_rdpgfx_was = viewer->gfx.use_rdpgfx;
  outcome_was = viewer->gfx.negotiation_outcome;

  /* If caps were already confirmed, suppress duplicate caps confirm before
   * sending anything on the wire. A duplicate CapsConfirm resets the active
   * cap set and can trigger a re-negotiation cycle that breaks the channel. */
  if (caps_ready_was && use_rdpgfx_was) {
    WLog_DBG(TAG,
             "Viewer %u suppressing duplicate caps confirm (join_state=%s "
             "caps_ready=%d)",
             viewer->id,
             viewer_gfx_pipeline_join_state_name(viewer->gfx.join_state),
             caps_ready_was);
    LeaveCriticalSection(&viewer->gfx.lock);
    return CHANNEL_RC_OK;
  }
  LeaveCriticalSection(&viewer->gfx.lock);

  confirm.capsSet = &caps;
  rc = context->CapsConfirm(context, &confirm);

  EnterCriticalSection(&viewer->gfx.lock);
  if (rc == CHANNEL_RC_OK) {
    viewer->gfx.confirmed_caps = caps;
    viewer->gfx.caps_ready = TRUE;
    viewer->gfx.use_rdpgfx = TRUE;
    viewer->gfx.rdpgfx_temporarily_disabled = FALSE;
    viewer->gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_RDPEGFX_READY;

    if (!caps_ready_was || !use_rdpgfx_was ||
        (outcome_was != VIEWER_GFX_NEGOTIATION_RDPEGFX_READY)) {
      WLog_INFO(TAG, "Viewer %u RDPEGFX caps confirm progressed negotiation",
                viewer->id);
    }

    if (viewer->activated &&
        viewer_gfx_pending_activation_begins_rdpgfx_join(&viewer->gfx)) {
      viewer_gfx_pipeline_caps_result_add_locked(
          viewer, VIEWER_GFX_PIPELINE_CAPS_ACTION_BEGIN_JOIN, rc, NULL,
          "RDPEGFX caps confirmed after activation");
      WLog_INFO(TAG,
                "Viewer %u RDPEGFX caps confirmed after activation; gating "
                "live stream until framebuffer baseline",
                viewer->id);
    }
  } else {
    viewer_gfx_pipeline_caps_result_add_locked(
        viewer,
        VIEWER_GFX_PIPELINE_CAPS_ACTION_DISABLE_RDPEGFX |
            (viewer->activated
                 ? VIEWER_GFX_PIPELINE_CAPS_ACTION_ENTER_CLASSIC_FALLBACK
                 : 0U),
        rc, viewer->activated ? "RDPEGFX caps confirm failed" : NULL, NULL);
  }
  LeaveCriticalSection(&viewer->gfx.lock);

  return rc;
}

BOOL viewer_gfx_pipeline_open_if_ready_locked(Viewer *viewer) {
  RdpgfxServerContext *rdpgfx = viewer ? viewer->gfx.rdpgfx : NULL;

  if (!viewer || !rdpgfx || viewer->gfx.channel_opened ||
      viewer->gfx.rdpgfx_temporarily_disabled)
    return TRUE;

  if (viewer->gfx.drdynvc_state != DRDYNVC_STATE_READY)
    return TRUE;

  WLog_INFO(TAG, "Viewer %u attempting RDPEGFX Open", viewer->id);
  if (!rdpgfx->Open || !rdpgfx->Open(rdpgfx)) {
    WLog_WARN(TAG, "Viewer %u RDPEGFX Open failed", viewer->id);
    return FALSE;
  }

  viewer->gfx.channel_opened = TRUE;
  WLog_INFO(TAG, "Viewer %u RDPEGFX Open succeeded", viewer->id);
  return TRUE;
}

HANDLE viewer_gfx_pipeline_get_event_handle_locked(Viewer *viewer) {
  return (viewer && viewer->gfx.rdpgfx)
             ? rdpgfx_server_get_event_handle(viewer->gfx.rdpgfx)
             : NULL;
}

BOOL viewer_gfx_pipeline_handle_messages_locked(
    Viewer *viewer, ViewerGfxPipelineCapsResult *caps_result) {
  if (caps_result)
    memset(caps_result, 0, sizeof(*caps_result));

  if (!viewer || !viewer->gfx.rdpgfx)
    return TRUE;

  if (rdpgfx_server_handle_messages(viewer->gfx.rdpgfx) != CHANNEL_RC_OK) {
    viewer_gfx_pipeline_caps_result_consume_locked(viewer, caps_result);
    return FALSE;
  }

  viewer_gfx_pipeline_caps_result_consume_locked(viewer, caps_result);
  return TRUE;
}

BOOL viewer_gfx_pipeline_activate(ViewerServer *server, Viewer *viewer) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;
  BOOL already_activated = FALSE;

  if (!server || !viewer || !gfx || !gfx->initialized)
    return FALSE;

  EnterCriticalSection(&gfx->lock);
  already_activated = gfx->last_activated_ts != 0;

  /* Dormant activation bookkeeping only. This intentionally does not send
   * ResetGraphics/CreateSurface/MapSurfaceToOutput or enable RDPEGFX when the
   * viewer is still on the disabled/classic fallback path. */
  gfx->next_frame_id = 1;
  gfx->last_sent_frame_id = 0;
  gfx->last_ack_frame_id = 0;
  gfx->active_surface_id = 0;
  gfx->surface_width = gfx->negotiated_width;
  gfx->surface_height = gfx->negotiated_height;
  gfx->surface_created = FALSE;
  gfx->force_full_present = TRUE;

  if (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK) {
    gfx->use_rdpgfx = FALSE;
  } else if (gfx->rdpgfx_temporarily_disabled) {
    gfx->use_rdpgfx = FALSE;
    gfx->negotiation_outcome = VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK;
  } else if (!gfx->caps_ready) {
    gfx->use_rdpgfx = FALSE;
    gfx->negotiation_outcome = VIEWER_GFX_NEGOTIATION_PENDING;
  }

  if (!already_activated)
    gfx->last_activated_ts = platform_get_timestamp_ms();
  if (gfx->last_activated_ts == 0)
    gfx->last_activated_ts = 1;

  LeaveCriticalSection(&gfx->lock);
  return TRUE;
}

BOOL viewer_gfx_pipeline_dirty_update_allowed(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot, const char **reason) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;
  const char *deny_reason = NULL;
  BOOL allowed = FALSE;
  UINT32 max_in_flight = 0;

  if (reason)
    *reason = NULL;

  if (!server || !viewer || !gfx || !snapshot) {
    deny_reason = "invalid arguments";
    goto out;
  }

  if (!server->viewer_gfx_enabled) {
    deny_reason = "viewer GFX disabled";
    goto out;
  }

  EnterCriticalSection(&gfx->lock);
  max_in_flight =
      gfx->dirty_max_in_flight_frames ? gfx->dirty_max_in_flight_frames : 1U;

  if (max_in_flight > VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY)
    max_in_flight = VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY;

  if (!gfx->initialized) {
    deny_reason = "GFX context not initialized";
  } else if (!gfx->dirty_updates_enabled) {
    deny_reason = "dirty updates disabled";
  } else if (!gfx->rdpgfx || !gfx->use_rdpgfx || !gfx->caps_ready ||
             !gfx->channel_opened || gfx->rdpgfx_temporarily_disabled ||
             (gfx->join_state != VIEWER_JOIN_STATE_LIVE)) {
    deny_reason = "RDPEGFX not live";
  } else if (!gfx->surface_created || gfx->force_full_present ||
             gfx->dirty_baseline_required) {
    deny_reason = "baseline required";
  } else if (gfx->dirty_suspended_for_no_ack) {
    deny_reason = "suspended waiting for ack";
  } else if (gfx->dirty_in_flight_frames >= max_in_flight) {
    deny_reason = "in-flight limit reached";
  } else if ((snapshot->generation != 0) &&
             (snapshot->generation <= gfx->dirty_last_sent_generation)) {
    deny_reason = "generation already sent";
  } else if ((gfx->dirty_reset_generation != 0) &&
             (snapshot->generation <= gfx->dirty_reset_generation)) {
    deny_reason = "reset generation requires baseline";
  } else {
    allowed = TRUE;
  }

  LeaveCriticalSection(&gfx->lock);

out:
  if (!allowed && reason)
    *reason = deny_reason ? deny_reason : "dirty update denied";
  return allowed;
}

ViewerGfxDirtyPacingStatus
viewer_gfx_pipeline_poll_dirty_pacing(Viewer *viewer, UINT64 now,
                                      const char **reason) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;
  ViewerGfxDirtyPacingStatus status = VIEWER_GFX_DIRTY_PACING_OK;
  UINT32 i = 0;

  if (reason)
    *reason = NULL;

  if (!gfx || !gfx->initialized) {
    if (reason)
      *reason = "invalid GFX context";
    return VIEWER_GFX_DIRTY_PACING_INVALID;
  }

  EnterCriticalSection(&gfx->lock);
  if (gfx->dirty_suspended_for_no_ack) {
    status = VIEWER_GFX_DIRTY_PACING_SUSPENDED;
    if (reason)
      *reason = "dirty updates suspended waiting for ack";
  } else {
    for (i = 0; i < VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY; i++) {
      UINT64 sent_ts = gfx->dirty_frame_sent_ts[i];
      if (!gfx->dirty_frame_valid[i] || (sent_ts == 0))
        continue;

      if (now >= sent_ts &&
          ((now - sent_ts) >= VIEWER_GFX_DIRTY_ACK_TIMEOUT_MS)) {
        gfx->dirty_suspended_for_no_ack = TRUE;
        status = VIEWER_GFX_DIRTY_PACING_SUSPENDED;
        if (reason)
          *reason = "dirty ack timeout";
        break;
      }
    }
  }
  LeaveCriticalSection(&gfx->lock);
  return status;
}

BOOL viewer_gfx_pipeline_send_dirty_update(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;
  RDPGFX_SURFACE_COMMAND *commands = NULL;
  RDPGFX_START_FRAME_PDU start = {0};
  RDPGFX_END_FRAME_PDU end = {0};
  RdpgfxServerContext *rdpgfx = NULL;
  UINT16 surface_id = 0;
  UINT32 frame_id = 0;
  UINT32 map_slot = VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY;
  UINT32 i = 0;
  UINT rc = CHANNEL_RC_OK;
  BOOL ok = FALSE;

  if (!server || !viewer || !gfx || !snapshot || !snapshot->pixels ||
      (snapshot->dirty_rect_count == 0))
    return FALSE;

  if (!viewer_gfx_pipeline_dirty_update_allowed(server, viewer, snapshot, NULL))
    return FALSE;

  commands = (RDPGFX_SURFACE_COMMAND *)calloc(snapshot->dirty_rect_count,
                                              sizeof(RDPGFX_SURFACE_COMMAND));
  if (!commands)
    return FALSE;

  EnterCriticalSection(&gfx->lock);
  rdpgfx = gfx->rdpgfx;
  surface_id = gfx->active_surface_id;
  frame_id = gfx->next_frame_id ? gfx->next_frame_id : 1U;
  for (i = 0; i < VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY; i++) {
    if (!gfx->dirty_frame_valid[i]) {
      map_slot = i;
      break;
    }
  }
  if (!rdpgfx || !rdpgfx->StartFrame || !rdpgfx->SurfaceCommand ||
      !rdpgfx->EndFrame || (map_slot >= VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY)) {
    LeaveCriticalSection(&gfx->lock);
    goto cleanup;
  }
  LeaveCriticalSection(&gfx->lock);

  for (i = 0; i < snapshot->dirty_rect_count; i++) {
    if (!viewer_gfx_uncompressed_build_surface_command_rect(
            snapshot, surface_id, &snapshot->dirty_rects[i], &commands[i]))
      goto cleanup;
  }

  start.timestamp = 0;
  start.frameId = frame_id;
  end.frameId = frame_id;

  EnterCriticalSection(&viewer->send_lock);
  rc = rdpgfx->StartFrame(rdpgfx, &start);
  for (i = 0; (rc == CHANNEL_RC_OK) && (i < snapshot->dirty_rect_count); i++)
    rc = rdpgfx->SurfaceCommand(rdpgfx, &commands[i]);
  if (rc == CHANNEL_RC_OK)
    rc = rdpgfx->EndFrame(rdpgfx, &end);
  LeaveCriticalSection(&viewer->send_lock);

  ok = (rc == CHANNEL_RC_OK);
  if (!ok)
    goto cleanup;

  EnterCriticalSection(&gfx->lock);
  gfx->dirty_frame_valid[map_slot] = TRUE;
  gfx->dirty_frame_ids[map_slot] = frame_id;
  gfx->dirty_frame_generations[map_slot] = snapshot->generation;
  gfx->dirty_frame_sent_ts[map_slot] = platform_get_timestamp_ms();
  gfx->dirty_in_flight_frames++;
  gfx->dirty_last_sent_generation = snapshot->generation;
  gfx->last_sent_frame_id = frame_id;
  gfx->next_frame_id = frame_id + 1U;
  LeaveCriticalSection(&gfx->lock);

cleanup:
  if (commands) {
    for (i = 0; i < snapshot->dirty_rect_count; i++)
      viewer_gfx_uncompressed_surface_command_reset(&commands[i]);
    free(commands);
  }
  return ok;
}

UINT viewer_gfx_pipeline_handle_frame_ack(Viewer *viewer, UINT32 frame_id) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;

  if (!gfx || !gfx->initialized)
    return ERROR_INVALID_PARAMETER;

  EnterCriticalSection(&gfx->lock);
  gfx->last_ack_frame_id = frame_id;
  gfx->last_presented_timestamp = platform_get_timestamp_ms();
  if (frame_id != 0)
    viewer_gfx_pipeline_handle_frame_ack_locked(gfx, frame_id);
  LeaveCriticalSection(&gfx->lock);
  return CHANNEL_RC_OK;
}

BOOL viewer_gfx_pipeline_send_snapshot(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;
  RdpgfxServerContext *rdpgfx = NULL;
  RDPGFX_SURFACE_COMMAND command = {0};
  RDPGFX_RESET_GRAPHICS_PDU reset = {0};
  RDPGFX_CREATE_SURFACE_PDU create = {0};
  RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map = {0};
  RDPGFX_START_FRAME_PDU start = {0};
  RDPGFX_END_FRAME_PDU end = {0};
  MONITOR_DEF monitor = {0};
  UINT16 surface_id = 0;
  UINT32 frame_id = 0;
  UINT rc = CHANNEL_RC_OK;
  BOOL ok = FALSE;

  if (!server || !viewer || !gfx || !gfx->initialized || !snapshot ||
      !snapshot->pixels || (snapshot->width == 0) || (snapshot->height == 0) ||
      (snapshot->stride == 0) || (snapshot->pixel_bytes == 0) ||
      (snapshot->width > (UINT32)UINT16_MAX) ||
      (snapshot->height > (UINT32)UINT16_MAX))
    return FALSE;

  EnterCriticalSection(&gfx->lock);
  rdpgfx = gfx->rdpgfx;
  if (!rdpgfx || !gfx->caps_ready || !gfx->use_rdpgfx ||
      gfx->rdpgfx_temporarily_disabled || !gfx->channel_opened ||
      !rdpgfx->ResetGraphics || !rdpgfx->CreateSurface ||
      !rdpgfx->MapSurfaceToOutput || !rdpgfx->StartFrame ||
      !rdpgfx->SurfaceCommand || !rdpgfx->EndFrame) {
    LeaveCriticalSection(&gfx->lock);
    return FALSE;
  }
  surface_id = gfx->active_surface_id;
  frame_id = gfx->next_frame_id ? gfx->next_frame_id : 1U;
  LeaveCriticalSection(&gfx->lock);

  if (!viewer_gfx_uncompressed_build_surface_command(snapshot, surface_id,
                                                     &command))
    return FALSE;

  monitor.left = 0;
  monitor.top = 0;
  monitor.right = (INT32)(snapshot->width - 1U);
  monitor.bottom = (INT32)(snapshot->height - 1U);
  monitor.flags = MONITOR_PRIMARY;

  reset.width = snapshot->width;
  reset.height = snapshot->height;
  reset.monitorCount = 1;
  reset.monitorDefArray = &monitor;

  create.surfaceId = surface_id;
  create.width = (UINT16)snapshot->width;
  create.height = (UINT16)snapshot->height;
  create.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;

  map.surfaceId = surface_id;
  map.outputOriginX = 0;
  map.outputOriginY = 0;

  start.timestamp = 0;
  start.frameId = frame_id;
  end.frameId = frame_id;

  EnterCriticalSection(&viewer->send_lock);
  rc = rdpgfx->ResetGraphics(rdpgfx, &reset);
  if (rc == CHANNEL_RC_OK)
    rc = rdpgfx->CreateSurface(rdpgfx, &create);
  if (rc == CHANNEL_RC_OK)
    rc = rdpgfx->MapSurfaceToOutput(rdpgfx, &map);
  if (rc == CHANNEL_RC_OK)
    rc = rdpgfx->StartFrame(rdpgfx, &start);
  if (rc == CHANNEL_RC_OK)
    rc = rdpgfx->SurfaceCommand(rdpgfx, &command);
  if (rc == CHANNEL_RC_OK)
    rc = rdpgfx->EndFrame(rdpgfx, &end);
  LeaveCriticalSection(&viewer->send_lock);

  ok = (rc == CHANNEL_RC_OK);

  EnterCriticalSection(&gfx->lock);
  if (ok) {
    gfx->active_surface_id = surface_id;
    gfx->surface_width = snapshot->width;
    gfx->surface_height = snapshot->height;
    gfx->surface_created = TRUE;
    gfx->force_full_present = FALSE;
    gfx->last_sent_frame_id = frame_id;
    gfx->next_frame_id = frame_id + 1U;
    gfx->dirty_last_sent_generation = snapshot->generation;
    viewer_gfx_pipeline_dirty_map_clear_locked(gfx);
    gfx->dirty_baseline_required = FALSE;
    gfx->dirty_updates_enabled = TRUE;
    if (gfx->dirty_max_in_flight_frames == 0)
      gfx->dirty_max_in_flight_frames = 1;
  } else {
    gfx->rdpgfx_error_count++;
    gfx->rdpgfx_consecutive_errors++;
  }
  LeaveCriticalSection(&gfx->lock);

  viewer_gfx_uncompressed_surface_command_reset(&command);
  return ok;
}
