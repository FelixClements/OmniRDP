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

static BOOL viewer_gfx_pipeline_send_event_locked(Viewer *viewer,
                                                  const ViewerGfxEvent *event) {
  UINT status = CHANNEL_RC_OK;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;
  RdpgfxServerContext *rdpgfx = viewer ? viewer->gfx.rdpgfx : NULL;

  if (!viewer || !event || !peer || !rdpgfx || !viewer->gfx.use_rdpgfx)
    return FALSE;

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    viewer->write_block_events++;
    if (!peer->DrainOutputBuffer || (peer->DrainOutputBuffer(peer) < 0) ||
        peer->IsWriteBlocked(peer)) {
      viewer->packets_failed++;
      return FALSE;
    }
  }

  switch (event->type) {
  case VIEWER_GFX_EVENT_RESET_GRAPHICS:
    status = rdpgfx->ResetGraphics
                 ? rdpgfx->ResetGraphics(rdpgfx, &event->u.reset_graphics)
                 : ERROR_INVALID_PARAMETER;
    break;
  case VIEWER_GFX_EVENT_CREATE_SURFACE:
    status = rdpgfx->CreateSurface
                 ? rdpgfx->CreateSurface(rdpgfx, &event->u.create_surface)
                 : ERROR_INVALID_PARAMETER;
    break;
  case VIEWER_GFX_EVENT_DELETE_SURFACE:
    status = rdpgfx->DeleteSurface
                 ? rdpgfx->DeleteSurface(rdpgfx, &event->u.delete_surface)
                 : ERROR_INVALID_PARAMETER;
    break;
  case VIEWER_GFX_EVENT_MAP_SURFACE_TO_OUTPUT:
    status = rdpgfx->MapSurfaceToOutput
                 ? rdpgfx->MapSurfaceToOutput(rdpgfx,
                                              &event->u.map_surface_to_output)
                 : ERROR_INVALID_PARAMETER;
    break;
  case VIEWER_GFX_EVENT_START_FRAME:
    status = rdpgfx->StartFrame
                 ? rdpgfx->StartFrame(rdpgfx, &event->u.start_frame)
                 : ERROR_INVALID_PARAMETER;
    break;
  case VIEWER_GFX_EVENT_SURFACE_COMMAND:
    status = rdpgfx->SurfaceCommand
                 ? rdpgfx->SurfaceCommand(rdpgfx, &event->u.surface_command)
                 : ERROR_INVALID_PARAMETER;
    break;
  case VIEWER_GFX_EVENT_END_FRAME:
    status = rdpgfx->EndFrame ? rdpgfx->EndFrame(rdpgfx, &event->u.end_frame)
                              : ERROR_INVALID_PARAMETER;
    break;
  case VIEWER_GFX_EVENT_DELETE_ENCODING_CONTEXT:
    status = rdpgfx->DeleteEncodingContext
                 ? rdpgfx->DeleteEncodingContext(
                       rdpgfx, &event->u.delete_encoding_context)
                 : ERROR_INVALID_PARAMETER;
    break;
  }

  if (status == CHANNEL_RC_OK) {
    viewer->packets_sent++;
    return TRUE;
  }

  viewer->packets_failed++;
  return FALSE;
}

BOOL viewer_gfx_pipeline_send_event(Viewer *viewer,
                                    const ViewerGfxEvent *event) {
  return viewer_gfx_pipeline_send_event_locked(viewer, event);
}

BOOL viewer_gfx_pipeline_send_surface_preamble(ViewerServer *server,
                                               Viewer *viewer) {
  RDPGFX_RESET_GRAPHICS_PDU reset = {0};
  RDPGFX_CREATE_SURFACE_PDU *create_surfaces = NULL;
  RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU *mapped_surfaces = NULL;
  UINT32 active_surface_count = 0;
  UINT32 mapped_surface_count = 0;
  UINT32 create_index = 0;
  UINT32 map_index = 0;
  UINT32 i = 0;
  BOOL ok = TRUE;
  BOOL has_reset = FALSE;

  if (!server || !viewer)
    return FALSE;

  EnterCriticalSection(&server->gfx.lock);
  if (server->gfx.has_latest_reset_graphics) {
    reset = server->gfx.latest_reset_graphics;
    if (reset.monitorCount > 0) {
      if (!reset.monitorDefArray ||
          ((size_t)reset.monitorCount > (SIZE_MAX / sizeof(MONITOR_DEF)))) {
        ok = FALSE;
      } else {
        const size_t monitor_bytes =
            (size_t)reset.monitorCount * sizeof(MONITOR_DEF);
        MONITOR_DEF *monitor_copy = (MONITOR_DEF *)calloc(
            (size_t)reset.monitorCount, sizeof(MONITOR_DEF));
        if (monitor_copy) {
          memmove(monitor_copy, reset.monitorDefArray, monitor_bytes);
          reset.monitorDefArray = monitor_copy;
          has_reset = TRUE;
        } else {
          reset.monitorDefArray = NULL;
          ok = FALSE;
        }
      }
    } else {
      reset.monitorDefArray = NULL;
      has_reset = TRUE;
    }
  }

  for (i = 0; ok && (i < VIEWER_GFX_MAX_ACTIVE_SURFACES); i++) {
    if (server->gfx.surfaces[i].in_use) {
      active_surface_count++;
      if (server->gfx.surfaces[i].mapped)
        mapped_surface_count++;
    }
  }

  if (active_surface_count > 0) {
    create_surfaces = (RDPGFX_CREATE_SURFACE_PDU *)calloc(
        active_surface_count, sizeof(RDPGFX_CREATE_SURFACE_PDU));
    if (!create_surfaces)
      ok = FALSE;
  }

  if (ok && (mapped_surface_count > 0)) {
    mapped_surfaces = (RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU *)calloc(
        mapped_surface_count, sizeof(RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU));
    if (!mapped_surfaces)
      ok = FALSE;
  }

  if (ok) {
    for (i = 0; i < VIEWER_GFX_MAX_ACTIVE_SURFACES; i++) {
      const ViewerGraphicsSurfaceState *surface = &server->gfx.surfaces[i];
      if (!surface->in_use)
        continue;

      create_surfaces[create_index++] = surface->create_surface;
      if (surface->mapped)
        mapped_surfaces[map_index++] = surface->map_surface_to_output;
    }
  }
  LeaveCriticalSection(&server->gfx.lock);

  if (!ok) {
    WLog_WARN(TAG, "Viewer %u surface preamble build failed", viewer->id);
    goto cleanup;
  }

  WLog_INFO(
      TAG,
      "Viewer %u sending surface preamble: reset=%d activeSurfaces=%" PRIu32
      " mappedSurfaces=%" PRIu32,
      viewer->id, has_reset ? 1 : 0, active_surface_count,
      mapped_surface_count);

  EnterCriticalSection(&viewer->send_lock);
  if (has_reset) {
    ViewerGfxEvent event = {0};
    event.type = VIEWER_GFX_EVENT_RESET_GRAPHICS;
    event.u.reset_graphics = reset;
    ok = viewer_gfx_pipeline_send_event_locked(viewer, &event);
  }

  for (i = 0; ok && (i < active_surface_count); i++) {
    ViewerGfxEvent event = {0};
    event.type = VIEWER_GFX_EVENT_CREATE_SURFACE;
    event.u.create_surface = create_surfaces[i];
    ok = viewer_gfx_pipeline_send_event_locked(viewer, &event);
  }

  for (i = 0; ok && (i < mapped_surface_count); i++) {
    ViewerGfxEvent event = {0};
    event.type = VIEWER_GFX_EVENT_MAP_SURFACE_TO_OUTPUT;
    event.u.map_surface_to_output = mapped_surfaces[i];
    ok = viewer_gfx_pipeline_send_event_locked(viewer, &event);
  }
  LeaveCriticalSection(&viewer->send_lock);

  if (ok)
    WLog_INFO(TAG, "Viewer %u surface preamble sent successfully", viewer->id);
  else
    WLog_ERR(TAG, "Viewer %u surface preamble send failed", viewer->id);

cleanup:
  free(reset.monitorDefArray);
  free(create_surfaces);
  free(mapped_surfaces);
  return ok;
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
