#include "viewer_gfx_pipeline.h"
#include "platform_compat.h"
#include "viewer_gfx_codec_rfx.h"
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

static UINT64 viewer_gfx_pipeline_default_dirty_byte_limit(void) {
  return (UINT64)VIEWER_GFX_DIRTY_MAX_IN_FLIGHT_BYTES;
}

static UINT64 viewer_gfx_pipeline_now_us(void) {
  return platform_get_timestamp_ms() * 1000ULL;
}

static void
viewer_gfx_pipeline_surface_command_reset(RDPGFX_SURFACE_COMMAND *command) {
  viewer_gfx_uncompressed_surface_command_reset(command);
}

static BOOL
viewer_gfx_pipeline_ensure_rfx_context_locked(ViewerGraphicsContext *gfx) {
  if (!gfx)
    return FALSE;

  if (gfx->rfx_context)
    return TRUE;

  if (!viewer_gfx_rfx_is_available())
    return FALSE;

  gfx->rfx_context = viewer_gfx_rfx_context_new();
  return gfx->rfx_context != NULL;
}

static void viewer_gfx_pipeline_downgrade_to_uncompressed_locked(
    ViewerGraphicsContext *gfx) {
  if (!gfx)
    return;

  viewer_gfx_rfx_context_free(gfx->rfx_context);
  gfx->rfx_context = NULL;
  gfx->selected_codec = VIEWER_GFX_CODEC_UNCOMPRESSED;
}

static BOOL viewer_gfx_pipeline_build_surface_command(
    Viewer *viewer, const ViewerFramebufferSnapshot *snapshot,
    UINT16 surface_id, RDPGFX_SURFACE_COMMAND *command) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;
  ViewerGfxRfxContext *rfx_context = NULL;
  ViewerGfxCodec selected_codec = VIEWER_GFX_CODEC_UNCOMPRESSED;
  BOOL use_rfx = FALSE;

  if (!gfx || !command)
    return FALSE;

  EnterCriticalSection(&gfx->lock);
  if (gfx->selected_codec == VIEWER_GFX_CODEC_RFX) {
    if (viewer_gfx_pipeline_ensure_rfx_context_locked(gfx)) {
      rfx_context = gfx->rfx_context;
      use_rfx = TRUE;
    } else {
      viewer_gfx_pipeline_downgrade_to_uncompressed_locked(gfx);
    }
  }
  LeaveCriticalSection(&gfx->lock);

  if (use_rfx && viewer_gfx_rfx_build_surface_command(rfx_context, snapshot,
                                                      surface_id, command))
    return TRUE;

  if (use_rfx) {
    EnterCriticalSection(&gfx->lock);
    viewer_gfx_pipeline_downgrade_to_uncompressed_locked(gfx);
    LeaveCriticalSection(&gfx->lock);
    viewer_gfx_pipeline_surface_command_reset(command);
  }

  if (!viewer_gfx_uncompressed_build_surface_command(snapshot, surface_id,
                                                     command))
    return FALSE;

  EnterCriticalSection(&gfx->lock);
  selected_codec = gfx->selected_codec;
  LeaveCriticalSection(&gfx->lock);
  return selected_codec == VIEWER_GFX_CODEC_UNCOMPRESSED;
}

static BOOL viewer_gfx_pipeline_build_surface_command_rect(
    Viewer *viewer, const ViewerFramebufferSnapshot *snapshot,
    UINT16 surface_id, const RECTANGLE_16 *dirty_rect,
    RDPGFX_SURFACE_COMMAND *command) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;
  ViewerGfxRfxContext *rfx_context = NULL;
  ViewerGfxCodec selected_codec = VIEWER_GFX_CODEC_UNCOMPRESSED;
  BOOL use_rfx = FALSE;

  if (!gfx || !command)
    return FALSE;

  EnterCriticalSection(&gfx->lock);
  if (gfx->selected_codec == VIEWER_GFX_CODEC_RFX) {
    if (viewer_gfx_pipeline_ensure_rfx_context_locked(gfx)) {
      rfx_context = gfx->rfx_context;
      use_rfx = TRUE;
    } else {
      viewer_gfx_pipeline_downgrade_to_uncompressed_locked(gfx);
    }
  }
  LeaveCriticalSection(&gfx->lock);

  if (use_rfx && viewer_gfx_rfx_build_surface_command_rect(
                     rfx_context, snapshot, surface_id, dirty_rect, command))
    return TRUE;

  if (use_rfx) {
    EnterCriticalSection(&gfx->lock);
    viewer_gfx_pipeline_downgrade_to_uncompressed_locked(gfx);
    LeaveCriticalSection(&gfx->lock);
    viewer_gfx_pipeline_surface_command_reset(command);
  }

  if (!viewer_gfx_uncompressed_build_surface_command_rect(
          snapshot, surface_id, dirty_rect, command)) {
    WLog_DBG(TAG,
             "Viewer %u RDPEGFX dirty command bounds validation failed: "
             "generation=%" PRIu64 " rect=(%u,%u)-(%u,%u) surface=%ux%u "
             "reason=command bounds validation failed",
             viewer ? viewer->id : 0U, snapshot ? snapshot->generation : 0,
             dirty_rect ? dirty_rect->left : 0U,
             dirty_rect ? dirty_rect->top : 0U,
             dirty_rect ? dirty_rect->right : 0U,
             dirty_rect ? dirty_rect->bottom : 0U,
             snapshot ? snapshot->width : 0U, snapshot ? snapshot->height : 0U);
    return FALSE;
  }

  EnterCriticalSection(&gfx->lock);
  selected_codec = gfx->selected_codec;
  LeaveCriticalSection(&gfx->lock);
  return selected_codec == VIEWER_GFX_CODEC_UNCOMPRESSED;
}

static UINT64 viewer_gfx_pipeline_dirty_area(const RECTANGLE_16 *dirty_rects,
                                             UINT32 dirty_rect_count) {
  UINT64 area = 0;
  UINT32 i = 0;

  if (!dirty_rects)
    return 0;

  for (i = 0; i < dirty_rect_count; i++) {
    const RECTANGLE_16 *rect = &dirty_rects[i];
    if ((rect->left <= rect->right) && (rect->top <= rect->bottom)) {
      const UINT32 width = (UINT32)rect->right - (UINT32)rect->left + 1U;
      const UINT32 height = (UINT32)rect->bottom - (UINT32)rect->top + 1U;
      area += (UINT64)width * (UINT64)height;
    }
  }

  return area;
}

static BOOL viewer_gfx_pipeline_full_frame_rect(UINT32 width, UINT32 height,
                                                RECTANGLE_16 *rect) {
  if (!rect || (width == 0) || (height == 0) || (width > (UINT32)UINT16_MAX) ||
      (height > (UINT32)UINT16_MAX))
    return FALSE;

  rect->left = 0;
  rect->top = 0;
  rect->right = (UINT16)(width - 1U);
  rect->bottom = (UINT16)(height - 1U);
  return TRUE;
}

static BOOL viewer_gfx_pipeline_clamp_rect(const RECTANGLE_16 *src,
                                           UINT32 width, UINT32 height,
                                           RECTANGLE_16 *dst) {
  UINT16 max_x = 0;
  UINT16 max_y = 0;

  if (!src || !dst || (width == 0) || (height == 0) ||
      (width > (UINT32)UINT16_MAX) || (height > (UINT32)UINT16_MAX) ||
      (src->left > src->right) || (src->top > src->bottom) ||
      ((UINT32)src->left >= width) || ((UINT32)src->top >= height))
    return FALSE;

  max_x = (UINT16)(width - 1U);
  max_y = (UINT16)(height - 1U);
  *dst = *src;
  if (dst->right > max_x)
    dst->right = max_x;
  if (dst->bottom > max_y)
    dst->bottom = max_y;
  return (dst->left <= dst->right) && (dst->top <= dst->bottom);
}

static BOOL viewer_gfx_pipeline_rects_touch_or_overlap(const RECTANGLE_16 *a,
                                                       const RECTANGLE_16 *b) {
  if (!a || !b)
    return FALSE;
  return ((UINT32)a->left <= ((UINT32)b->right + 1U)) &&
         ((UINT32)b->left <= ((UINT32)a->right + 1U)) &&
         ((UINT32)a->top <= ((UINT32)b->bottom + 1U)) &&
         ((UINT32)b->top <= ((UINT32)a->bottom + 1U));
}

static void viewer_gfx_pipeline_rect_union(RECTANGLE_16 *target,
                                           const RECTANGLE_16 *rect) {
  if (!target || !rect)
    return;
  if (rect->left < target->left)
    target->left = rect->left;
  if (rect->top < target->top)
    target->top = rect->top;
  if (rect->right > target->right)
    target->right = rect->right;
  if (rect->bottom > target->bottom)
    target->bottom = rect->bottom;
}

void viewer_gfx_pipeline_pending_dirty_clear_locked(
    ViewerGraphicsContext *gfx) {
  if (!gfx)
    return;
  gfx->pending_dirty_rect_count = 0;
  gfx->pending_dirty_start_generation = 0;
  gfx->pending_dirty_latest_generation = 0;
  gfx->pending_dirty_area = 0;
  gfx->pending_dirty_update_count = 0;
  gfx->pending_dirty_width = 0;
  gfx->pending_dirty_height = 0;
  gfx->pending_dirty_full_frame = FALSE;
}

static BOOL viewer_gfx_pipeline_pending_dirty_force_full_locked(
    ViewerGraphicsContext *gfx, UINT64 generation, UINT32 width,
    UINT32 height) {
  if (!gfx || !viewer_gfx_pipeline_full_frame_rect(
                  width, height, &gfx->pending_dirty_rects[0]))
    return FALSE;

  gfx->pending_dirty_rect_count = 1;
  gfx->pending_dirty_full_frame = TRUE;
  gfx->pending_dirty_area = (UINT64)width * (UINT64)height;
  if (gfx->pending_dirty_start_generation == 0)
    gfx->pending_dirty_start_generation = generation;
  if (generation > gfx->pending_dirty_latest_generation)
    gfx->pending_dirty_latest_generation = generation;
  gfx->pending_dirty_width = width;
  gfx->pending_dirty_height = height;
  return TRUE;
}

BOOL viewer_gfx_pipeline_pending_dirty_add_locked(
    ViewerGraphicsContext *gfx, const RECTANGLE_16 *dirty_rects,
    UINT32 dirty_rect_count, BOOL dirty_overflow, UINT64 generation,
    UINT32 width, UINT32 height) {
  UINT32 i = 0;
  UINT64 full_area = 0;
  BOOL has_valid_rect = FALSE;

  if (!gfx || (generation == 0) || (width == 0) || (height == 0))
    return FALSE;

  if (!dirty_overflow && (dirty_rect_count > 0) && dirty_rects) {
    for (i = 0; i < dirty_rect_count; i++) {
      RECTANGLE_16 rect = {0};
      if (viewer_gfx_pipeline_clamp_rect(&dirty_rects[i], width, height,
                                         &rect)) {
        has_valid_rect = TRUE;
        break;
      }
    }
    if (!has_valid_rect)
      return FALSE;
  }

  if ((gfx->pending_dirty_latest_generation != 0) &&
      ((gfx->pending_dirty_width != width) ||
       (gfx->pending_dirty_height != height)))
    viewer_gfx_pipeline_pending_dirty_clear_locked(gfx);

  gfx->pending_dirty_update_count++;
  if (gfx->pending_dirty_start_generation == 0)
    gfx->pending_dirty_start_generation = generation;
  if (generation > gfx->pending_dirty_latest_generation)
    gfx->pending_dirty_latest_generation = generation;
  gfx->pending_dirty_width = width;
  gfx->pending_dirty_height = height;

  if (dirty_overflow || (dirty_rect_count == 0) || !dirty_rects ||
      gfx->pending_dirty_full_frame)
    return viewer_gfx_pipeline_pending_dirty_force_full_locked(gfx, generation,
                                                               width, height);

  for (i = 0; i < dirty_rect_count; i++) {
    RECTANGLE_16 rect = {0};
    UINT32 j = 0;
    BOOL merged = FALSE;

    if (!viewer_gfx_pipeline_clamp_rect(&dirty_rects[i], width, height, &rect))
      continue;

    for (j = 0; j < gfx->pending_dirty_rect_count; j++) {
      if (viewer_gfx_pipeline_rects_touch_or_overlap(
              &gfx->pending_dirty_rects[j], &rect)) {
        viewer_gfx_pipeline_rect_union(&gfx->pending_dirty_rects[j], &rect);
        merged = TRUE;
        break;
      }
    }

    if (!merged) {
      if (gfx->pending_dirty_rect_count >= VIEWER_GFX_PENDING_DIRTY_MAX_RECTS)
        return viewer_gfx_pipeline_pending_dirty_force_full_locked(
            gfx, generation, width, height);
      gfx->pending_dirty_rects[gfx->pending_dirty_rect_count++] = rect;
    }
  }

  if (gfx->pending_dirty_rect_count == 0)
    return viewer_gfx_pipeline_pending_dirty_force_full_locked(gfx, generation,
                                                               width, height);

  gfx->pending_dirty_area = viewer_gfx_pipeline_dirty_area(
      gfx->pending_dirty_rects, gfx->pending_dirty_rect_count);
  full_area = (UINT64)width * (UINT64)height;
  if ((full_area > 0) &&
      (gfx->pending_dirty_area > ((full_area * 60ULL) / 100ULL)))
    return viewer_gfx_pipeline_pending_dirty_force_full_locked(gfx, generation,
                                                               width, height);

  return TRUE;
}

BOOL viewer_gfx_pipeline_pending_dirty_move_locked(
    ViewerGraphicsContext *gfx, ViewerGfxPendingDirtyBatch *batch) {
  UINT32 i = 0;

  if (!gfx || !batch)
    return FALSE;
  memset(batch, 0, sizeof(*batch));
  if ((gfx->pending_dirty_latest_generation == 0) ||
      (gfx->pending_dirty_rect_count == 0))
    return FALSE;

  batch->rect_count = gfx->pending_dirty_rect_count;
  batch->start_generation = gfx->pending_dirty_start_generation;
  batch->latest_generation = gfx->pending_dirty_latest_generation;
  batch->area = gfx->pending_dirty_area;
  batch->update_count = gfx->pending_dirty_update_count;
  batch->width = gfx->pending_dirty_width;
  batch->height = gfx->pending_dirty_height;
  batch->full_frame = gfx->pending_dirty_full_frame;
  for (i = 0; i < batch->rect_count; i++)
    batch->rects[i] = gfx->pending_dirty_rects[i];

  viewer_gfx_pipeline_pending_dirty_clear_locked(gfx);
  return TRUE;
}

BOOL viewer_gfx_pipeline_pending_dirty_remerge_locked(
    ViewerGraphicsContext *gfx, const ViewerGfxPendingDirtyBatch *batch,
    UINT32 width, UINT32 height) {
  BOOL merged = FALSE;

  if (!gfx || !batch || (batch->latest_generation == 0) ||
      (batch->rect_count == 0))
    return FALSE;
  merged = viewer_gfx_pipeline_pending_dirty_add_locked(
      gfx, batch->rects, batch->rect_count, batch->full_frame,
      batch->latest_generation, width, height);
  if (merged) {
    if ((batch->start_generation != 0) &&
        ((gfx->pending_dirty_start_generation == 0) ||
         (batch->start_generation < gfx->pending_dirty_start_generation)))
      gfx->pending_dirty_start_generation = batch->start_generation;
    if (batch->update_count > 1)
      gfx->pending_dirty_update_count += batch->update_count - 1U;
  }
  return merged;
}

BOOL viewer_gfx_pipeline_snapshot_apply_pending_dirty(
    ViewerFramebufferSnapshot *snapshot,
    const ViewerGfxPendingDirtyBatch *batch) {
  UINT32 i = 0;

  if (!snapshot || !batch || (batch->latest_generation == 0) ||
      (batch->rect_count == 0))
    return FALSE;

  if (batch->full_frame) {
    if (!viewer_gfx_pipeline_full_frame_rect(snapshot->width, snapshot->height,
                                             &snapshot->dirty_rects[0]))
      return FALSE;
    snapshot->dirty_rect_count = 1;
  } else {
    if (batch->rect_count > VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS)
      return FALSE;
    for (i = 0; i < batch->rect_count; i++)
      snapshot->dirty_rects[i] = batch->rects[i];
    snapshot->dirty_rect_count = batch->rect_count;
  }
  snapshot->dirty_overflow = FALSE;
  snapshot->generation = batch->latest_generation;
  return TRUE;
}

static void
viewer_gfx_pipeline_ensure_dirty_limits_locked(ViewerGraphicsContext *gfx) {
  if (!gfx)
    return;
  if (gfx->dirty_max_in_flight_frames == 0)
    gfx->dirty_max_in_flight_frames = 1;
  if (gfx->dirty_max_in_flight_bytes == 0)
    gfx->dirty_max_in_flight_bytes =
        viewer_gfx_pipeline_default_dirty_byte_limit();
}

static void
viewer_gfx_pipeline_increment_epoch_locked(ViewerGraphicsContext *gfx) {
  if (!gfx)
    return;
  gfx->frame_epoch++;
  if (gfx->frame_epoch == 0)
    gfx->frame_epoch = 1;
}

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
  memset(gfx->dirty_frame_epochs, 0, sizeof(gfx->dirty_frame_epochs));
  memset(gfx->dirty_frame_generations, 0, sizeof(gfx->dirty_frame_generations));
  memset(gfx->dirty_frame_payload_bytes, 0,
         sizeof(gfx->dirty_frame_payload_bytes));
  memset(gfx->dirty_frame_sent_ts, 0, sizeof(gfx->dirty_frame_sent_ts));
  memset(gfx->dirty_frame_valid, 0, sizeof(gfx->dirty_frame_valid));
  gfx->dirty_in_flight_frames = 0;
  gfx->dirty_in_flight_bytes = 0;
  gfx->dirty_suspended_for_no_ack = FALSE;
  gfx->dirty_acknowledgements_suspended = FALSE;
}

static void
viewer_gfx_pipeline_handle_frame_ack_locked(ViewerGraphicsContext *gfx,
                                            UINT32 frame_id) {
  UINT32 i = 0;

  if (!gfx || !gfx->initialized || (frame_id == 0))
    return;

  for (i = 0; i < VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY; i++) {
    if (!gfx->dirty_frame_valid[i] || (gfx->dirty_frame_ids[i] != frame_id) ||
        (gfx->dirty_frame_epochs[i] != gfx->frame_epoch))
      continue;

    gfx->dirty_last_acked_generation = gfx->dirty_frame_generations[i];
    if (gfx->dirty_in_flight_bytes >= gfx->dirty_frame_payload_bytes[i])
      gfx->dirty_in_flight_bytes -= gfx->dirty_frame_payload_bytes[i];
    else
      gfx->dirty_in_flight_bytes = 0;
    gfx->dirty_frame_valid[i] = FALSE;
    gfx->dirty_frame_ids[i] = 0;
    gfx->dirty_frame_epochs[i] = 0;
    gfx->dirty_frame_generations[i] = 0;
    gfx->dirty_frame_payload_bytes[i] = 0;
    gfx->dirty_frame_sent_ts[i] = 0;
    if (gfx->dirty_in_flight_frames > 0)
      gfx->dirty_in_flight_frames--;
    gfx->last_ack_frame_id = frame_id;
    gfx->last_ack_epoch = gfx->frame_epoch;
    gfx->last_presented_timestamp = platform_get_timestamp_ms();
    if ((gfx->dirty_in_flight_frames == 0) && (gfx->dirty_in_flight_bytes == 0))
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

  return viewer_gfx_pipeline_handle_frame_ack_pdu(viewer, frame_acknowledge);
}

void viewer_gfx_pipeline_reset_dirty_state_locked(ViewerGraphicsContext *gfx) {
  if (!gfx)
    return;

  gfx->dirty_last_sent_generation = 0;
  gfx->dirty_last_acked_generation = 0;
  gfx->dirty_reset_generation = 0;
  viewer_gfx_pipeline_increment_epoch_locked(gfx);
  viewer_gfx_pipeline_dirty_map_clear_locked(gfx);
  viewer_gfx_pipeline_ensure_dirty_limits_locked(gfx);
  gfx->dirty_suspended_for_no_ack = FALSE;
  gfx->dirty_acknowledgements_suspended = FALSE;
  gfx->dirty_updates_enabled = FALSE;
  gfx->dirty_baseline_required = TRUE;
  viewer_gfx_pipeline_pending_dirty_clear_locked(gfx);
}

void viewer_gfx_pipeline_invalidate_surface_locked(ViewerGraphicsContext *gfx) {
  if (!gfx)
    return;

  gfx->active_surface_id = 0;
  gfx->surface_width = 0;
  gfx->surface_height = 0;
  gfx->surface_created = FALSE;
  gfx->force_full_present = TRUE;
  viewer_gfx_pipeline_reset_dirty_state_locked(gfx);
  gfx->last_ack_frame_id = 0;
  gfx->last_ack_epoch = gfx->frame_epoch;
  gfx->last_presented_timestamp = 0;
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
  viewer_gfx_pipeline_reset_dirty_state_locked(&viewer->gfx);
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
  ViewerJoinState prior_state = VIEWER_JOIN_STATE_NONE;
  ViewerJoinStrategy prior_strategy = VIEWER_JOIN_STRATEGY_NONE;

  viewer_gfx_pipeline_join_result_clear(result);
  if (!viewer)
    return;

  EnterCriticalSection(&viewer->gfx.lock);
  prior_state = viewer->gfx.join_state;
  prior_strategy = viewer->gfx.join_strategy;
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
  } else if (sent && result) {
    if ((prior_state == VIEWER_JOIN_STATE_PENDING) &&
        (prior_strategy != VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK)) {
      result->actions = VIEWER_GFX_JOIN_ACTION_SEND_POINTER_BASELINE;
      result->log_reason =
          "RDPEGFX pointer baseline after late-join framebuffer baseline";
    }
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
  BOOL live_baseline_required = FALSE;

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
  live_baseline_required =
      (state == VIEWER_JOIN_STATE_LIVE) &&
      (viewer->gfx.dirty_baseline_required || !viewer->gfx.surface_created ||
       viewer->gfx.force_full_present || (viewer->gfx.surface_width == 0) ||
       (viewer->gfx.surface_height == 0));
  LeaveCriticalSection(&viewer->gfx.lock);

  if (live_baseline_required && server->viewer_gfx_enabled) {
    result->actions = VIEWER_GFX_JOIN_ACTION_SEND_BASELINE;
    result->log_reason = "RDPEGFX live canonical resize baseline";
    return;
  }
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

BOOL viewer_gfx_pipeline_init(Viewer *viewer) {
  if (!viewer)
    return FALSE;
  viewer->gfx.preferred_codec = VIEWER_GFX_CODEC_UNCOMPRESSED;
  viewer->gfx.selected_codec = VIEWER_GFX_CODEC_UNCOMPRESSED;
  viewer->gfx.rfx_context = NULL;
  return TRUE;
}

void viewer_gfx_pipeline_uninit(Viewer *viewer) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;

  if (!gfx || !gfx->initialized)
    return;

  EnterCriticalSection(&gfx->lock);
  viewer_gfx_rfx_context_free(gfx->rfx_context);
  gfx->rfx_context = NULL;
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
  viewer_gfx_pipeline_invalidate_surface_locked(gfx);
  gfx->selected_codec = gfx->preferred_codec;
  if (gfx->selected_codec != VIEWER_GFX_CODEC_RFX)
    viewer_gfx_pipeline_downgrade_to_uncompressed_locked(gfx);

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
  viewer_gfx_pipeline_ensure_dirty_limits_locked(gfx);

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
  } else if ((snapshot->width == 0) || (snapshot->height == 0) ||
             (snapshot->width != gfx->surface_width) ||
             (snapshot->height != gfx->surface_height)) {
    deny_reason = "snapshot dimensions differ from GFX surface";
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
  if (gfx->dirty_suspended_for_no_ack ||
      gfx->dirty_acknowledgements_suspended) {
    status = VIEWER_GFX_DIRTY_PACING_SUSPENDED;
    if (reason)
      *reason = gfx->dirty_acknowledgements_suspended
                    ? "dirty updates suspended by client ack policy"
                    : "dirty updates suspended waiting for ack";
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

ViewerGfxDirtySendStatus viewer_gfx_pipeline_send_dirty_update_result(
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
  UINT32 max_in_flight = 0;
  UINT64 frame_epoch = 0;
  UINT64 pending_payload_bytes = 0;
  UINT64 send_start_us = 0;
  UINT64 send_us = 0;
  UINT64 dirty_area = 0;
  UINT32 i = 0;
  UINT rc = CHANNEL_RC_OK;
  BOOL ok = FALSE;

  if (!server || !viewer || !gfx || !snapshot || !snapshot->pixels ||
      (snapshot->dirty_rect_count == 0))
    return VIEWER_GFX_DIRTY_SEND_FAILED;

  if (!viewer_gfx_pipeline_dirty_update_allowed(server, viewer, snapshot, NULL))
    return VIEWER_GFX_DIRTY_SEND_DEFERRED;

  commands = (RDPGFX_SURFACE_COMMAND *)calloc(snapshot->dirty_rect_count,
                                              sizeof(RDPGFX_SURFACE_COMMAND));
  if (!commands)
    return VIEWER_GFX_DIRTY_SEND_FAILED;

  EnterCriticalSection(&gfx->lock);
  rdpgfx = gfx->rdpgfx;
  surface_id = gfx->active_surface_id;
  if (!rdpgfx || !rdpgfx->StartFrame || !rdpgfx->SurfaceCommand ||
      !rdpgfx->EndFrame) {
    LeaveCriticalSection(&gfx->lock);
    goto failed;
  }
  LeaveCriticalSection(&gfx->lock);

  for (i = 0; i < snapshot->dirty_rect_count; i++) {
    if (!viewer_gfx_pipeline_build_surface_command_rect(
            viewer, snapshot, surface_id, &snapshot->dirty_rects[i],
            &commands[i]))
      goto failed;
    if ((UINT64_MAX - pending_payload_bytes) < (UINT64)commands[i].length)
      goto failed;
    pending_payload_bytes += (UINT64)commands[i].length;
  }
  dirty_area = viewer_gfx_pipeline_dirty_area(snapshot->dirty_rects,
                                              snapshot->dirty_rect_count);

  EnterCriticalSection(&gfx->lock);
  viewer_gfx_pipeline_ensure_dirty_limits_locked(gfx);
  max_in_flight = gfx->dirty_max_in_flight_frames;
  if (max_in_flight > VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY)
    max_in_flight = VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY;
  frame_id = gfx->next_frame_id ? gfx->next_frame_id : 1U;
  if (frame_id == UINT32_MAX) {
    viewer_gfx_pipeline_increment_epoch_locked(gfx);
    viewer_gfx_pipeline_dirty_map_clear_locked(gfx);
  }
  for (i = 0; i < VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY; i++) {
    if (!gfx->dirty_frame_valid[i]) {
      map_slot = i;
      break;
    }
  }
  if (((UINT64_MAX - gfx->dirty_in_flight_bytes) < pending_payload_bytes) ||
      ((gfx->dirty_in_flight_bytes + pending_payload_bytes) >
       gfx->dirty_max_in_flight_bytes) ||
      (gfx->dirty_in_flight_frames >= max_in_flight) ||
      (map_slot >= VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY)) {
    LeaveCriticalSection(&gfx->lock);
    goto deferred;
  }
  frame_epoch = gfx->frame_epoch;
  LeaveCriticalSection(&gfx->lock);

  start.timestamp = 0;
  start.frameId = frame_id;
  end.frameId = frame_id;

  send_start_us = viewer_gfx_pipeline_now_us();
  EnterCriticalSection(&viewer->send_lock);
  viewer->last_viewer_send_start_us = send_start_us;
  rc = rdpgfx->StartFrame(rdpgfx, &start);
  for (i = 0; (rc == CHANNEL_RC_OK) && (i < snapshot->dirty_rect_count); i++)
    rc = rdpgfx->SurfaceCommand(rdpgfx, &commands[i]);
  if (rc == CHANNEL_RC_OK)
    rc = rdpgfx->EndFrame(rdpgfx, &end);
  send_us = viewer_gfx_pipeline_now_us() - send_start_us;
  viewer->last_viewer_send_end_us = send_start_us + send_us;
  LeaveCriticalSection(&viewer->send_lock);

  ok = (rc == CHANNEL_RC_OK);
  if (!ok)
    goto failed;

  EnterCriticalSection(&gfx->lock);
  gfx->dirty_frame_valid[map_slot] = TRUE;
  gfx->dirty_frame_ids[map_slot] = frame_id;
  gfx->dirty_frame_epochs[map_slot] = frame_epoch;
  gfx->dirty_frame_generations[map_slot] = snapshot->generation;
  gfx->dirty_frame_payload_bytes[map_slot] = pending_payload_bytes;
  gfx->dirty_frame_sent_ts[map_slot] = platform_get_timestamp_ms();
  gfx->dirty_in_flight_frames++;
  gfx->dirty_in_flight_bytes += pending_payload_bytes;
  gfx->dirty_last_sent_generation = snapshot->generation;
  gfx->dirty_last_sent_rect_count = snapshot->dirty_rect_count;
  gfx->dirty_last_sent_area = dirty_area;
  gfx->gfx_send_time_total_us += send_us;
  if (send_us > gfx->gfx_send_time_max_us)
    gfx->gfx_send_time_max_us = send_us;
  gfx->last_gfx_send_start_us = send_start_us;
  gfx->last_gfx_send_end_us = send_start_us + send_us;
  gfx->last_sent_frame_id = frame_id;
  gfx->next_frame_id = (frame_id == UINT32_MAX) ? 1U : (frame_id + 1U);
  LeaveCriticalSection(&gfx->lock);

cleanup:
  if (commands) {
    for (i = 0; i < snapshot->dirty_rect_count; i++)
      viewer_gfx_pipeline_surface_command_reset(&commands[i]);
    free(commands);
  }
  return ok ? VIEWER_GFX_DIRTY_SEND_SENT : VIEWER_GFX_DIRTY_SEND_FAILED;

deferred:
  if (commands) {
    for (i = 0; i < snapshot->dirty_rect_count; i++)
      viewer_gfx_pipeline_surface_command_reset(&commands[i]);
    free(commands);
  }
  return VIEWER_GFX_DIRTY_SEND_DEFERRED;

failed:
  if (commands) {
    for (i = 0; i < snapshot->dirty_rect_count; i++)
      viewer_gfx_pipeline_surface_command_reset(&commands[i]);
    free(commands);
  }
  return VIEWER_GFX_DIRTY_SEND_FAILED;
}

BOOL viewer_gfx_pipeline_send_dirty_update(
    ViewerServer *server, Viewer *viewer,
    const ViewerFramebufferSnapshot *snapshot) {
  return viewer_gfx_pipeline_send_dirty_update_result(
             server, viewer, snapshot) == VIEWER_GFX_DIRTY_SEND_SENT;
}

BOOL viewer_gfx_pipeline_monitor_layout_snapshot(
    ViewerServer *server, const ViewerFramebufferSnapshot *snapshot,
    MonitorLayout *layout) {
  MONITOR_DEF monitor = {0};

  if (!server || !layout)
    return FALSE;

  memset(layout, 0, sizeof(*layout));
  if ((server->monitor_layout.monitor_count > 0) &&
      (server->monitor_layout.monitor_count <= OMNIRDP_MAX_MONITORS) &&
      (server->monitor_layout.total_width > 0) &&
      (server->monitor_layout.total_height > 0)) {
    *layout = server->monitor_layout;
    return TRUE;
  }

  if (!snapshot || (snapshot->width == 0) || (snapshot->height == 0) ||
      (snapshot->width > (UINT32)INT32_MAX) ||
      (snapshot->height > (UINT32)INT32_MAX))
    return FALSE;

  monitor.left = 0;
  monitor.top = 0;
  monitor.right = (INT32)(snapshot->width - 1U);
  monitor.bottom = (INT32)(snapshot->height - 1U);
  monitor.flags = MONITOR_PRIMARY;

  layout->monitor_count = 1;
  layout->total_width = snapshot->width;
  layout->total_height = snapshot->height;
  layout->monitors[0] = monitor;
  return TRUE;
}

UINT viewer_gfx_pipeline_handle_frame_ack(Viewer *viewer, UINT32 frame_id) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;

  if (!gfx || !gfx->initialized)
    return ERROR_INVALID_PARAMETER;

  EnterCriticalSection(&gfx->lock);
  if (frame_id == 0) {
    gfx->last_ack_frame_id = frame_id;
    gfx->last_presented_timestamp = platform_get_timestamp_ms();
  } else {
    viewer_gfx_pipeline_handle_frame_ack_locked(gfx, frame_id);
  }
  LeaveCriticalSection(&gfx->lock);
  return CHANNEL_RC_OK;
}

UINT viewer_gfx_pipeline_handle_frame_ack_pdu(
    Viewer *viewer, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frame_acknowledge) {
  ViewerGraphicsContext *gfx = viewer ? &viewer->gfx : NULL;
  BOOL suspend_acknowledgements = FALSE;

  if (!gfx || !gfx->initialized || !frame_acknowledge)
    return ERROR_INVALID_PARAMETER;

  suspend_acknowledgements =
      (frame_acknowledge->queueDepth == SUSPEND_FRAME_ACKNOWLEDGEMENT);

  EnterCriticalSection(&gfx->lock);
  if (frame_acknowledge->frameId == 0) {
    gfx->last_ack_frame_id = frame_acknowledge->frameId;
    gfx->last_presented_timestamp = platform_get_timestamp_ms();
  } else {
    viewer_gfx_pipeline_handle_frame_ack_locked(gfx,
                                                frame_acknowledge->frameId);
  }
  if (suspend_acknowledgements) {
    gfx->dirty_acknowledgements_suspended = TRUE;
    gfx->dirty_suspended_for_no_ack = TRUE;
  } else if (gfx->dirty_acknowledgements_suspended &&
             (frame_acknowledge->queueDepth != QUEUE_DEPTH_UNAVAILABLE)) {
    gfx->dirty_acknowledgements_suspended = FALSE;
    if ((gfx->dirty_in_flight_frames == 0) && (gfx->dirty_in_flight_bytes == 0))
      gfx->dirty_suspended_for_no_ack = FALSE;
  }
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
  MonitorLayout layout = {0};
  UINT16 surface_id = 0;
  UINT32 frame_id = 0;
  UINT64 send_start_us = 0;
  UINT64 send_us = 0;
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
  if (frame_id == UINT32_MAX) {
    viewer_gfx_pipeline_increment_epoch_locked(gfx);
    viewer_gfx_pipeline_dirty_map_clear_locked(gfx);
  }
  LeaveCriticalSection(&gfx->lock);

  if (!viewer_gfx_pipeline_build_surface_command(viewer, snapshot, surface_id,
                                                 &command))
    return FALSE;

  if (!viewer_gfx_pipeline_monitor_layout_snapshot(server, snapshot, &layout)) {
    viewer_gfx_pipeline_surface_command_reset(&command);
    return FALSE;
  }

  reset.width = snapshot->width;
  reset.height = snapshot->height;
  reset.monitorCount = layout.monitor_count;
  reset.monitorDefArray = layout.monitors;

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

  send_start_us = viewer_gfx_pipeline_now_us();
  EnterCriticalSection(&viewer->send_lock);
  viewer->last_viewer_send_start_us = send_start_us;
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
  send_us = viewer_gfx_pipeline_now_us() - send_start_us;
  viewer->last_viewer_send_end_us = send_start_us + send_us;
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
    gfx->next_frame_id = (frame_id == UINT32_MAX) ? 1U : (frame_id + 1U);
    gfx->dirty_last_sent_generation = snapshot->generation;
    gfx->dirty_last_sent_rect_count = snapshot->dirty_rect_count;
    gfx->dirty_last_sent_area = viewer_gfx_pipeline_dirty_area(
        snapshot->dirty_rects, snapshot->dirty_rect_count);
    gfx->gfx_send_time_total_us += send_us;
    if (send_us > gfx->gfx_send_time_max_us)
      gfx->gfx_send_time_max_us = send_us;
    gfx->last_gfx_send_start_us = send_start_us;
    gfx->last_gfx_send_end_us = send_start_us + send_us;
    viewer_gfx_pipeline_increment_epoch_locked(gfx);
    viewer_gfx_pipeline_dirty_map_clear_locked(gfx);
    viewer_gfx_pipeline_pending_dirty_clear_locked(gfx);
    gfx->dirty_baseline_required = FALSE;
    gfx->dirty_updates_enabled = TRUE;
    viewer_gfx_pipeline_ensure_dirty_limits_locked(gfx);
  } else {
    gfx->rdpgfx_error_count++;
    gfx->rdpgfx_consecutive_errors++;
  }
  LeaveCriticalSection(&gfx->lock);

  viewer_gfx_pipeline_surface_command_reset(&command);
  return ok;
}
