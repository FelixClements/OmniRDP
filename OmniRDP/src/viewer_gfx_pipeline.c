#include "viewer_gfx_pipeline.h"
#include "viewer_internal.h"

#include <freerdp/channels/drdynvc.h>
#include <freerdp/server/rdpgfx.h>
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
  case VIEWER_JOIN_STATE_WAIT_NEXT_SAFE_FRAME:
    return "wait_next_safe_frame";
  case VIEWER_JOIN_STATE_REPLAYING:
    return "replaying";
  case VIEWER_JOIN_STATE_WAIT_REPLAY_ACK:
    return "wait_replay_ack";
  case VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH:
    return "wait_backend_refresh";
  case VIEWER_JOIN_STATE_LIVE:
    return "live";
  case VIEWER_JOIN_STATE_REJECTED:
    return "rejected";
  default:
    return "unknown";
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

BOOL viewer_gfx_pipeline_post_connect_locked(
    ViewerServer *server, Viewer *viewer, freerdp_peer *peer, BOOL gfx_enabled,
    ViewerGfxFrameAcknowledgeCallback frame_acknowledge) {
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
  rdpgfx->FrameAcknowledge = frame_acknowledge;
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
                "live stream until replay/full refresh",
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
  (void)server;
  return viewer != NULL;
}

BOOL viewer_gfx_pipeline_send_snapshot(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot) {
  (void)server;
  return (viewer != NULL) && (snapshot != NULL) && (snapshot->pixels != NULL);
}
