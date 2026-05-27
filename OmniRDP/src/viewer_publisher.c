#include "viewer_publisher.h"

#include "platform_compat.h"

#include <string.h>

static UINT64 viewer_publisher_dirty_area(const RECTANGLE_16 *dirty_rects,
                                          UINT32 dirty_rect_count) {
  UINT64 area = 0;
  UINT32 i = 0;

  if (!dirty_rects)
    return 0;

  for (i = 0; i < dirty_rect_count; i++) {
    const RECTANGLE_16 *rect = &dirty_rects[i];
    UINT32 width = 0;
    UINT32 height = 0;

    if ((rect->left > rect->right) || (rect->top > rect->bottom))
      continue;

    width = (UINT32)rect->right - (UINT32)rect->left + 1U;
    height = (UINT32)rect->bottom - (UINT32)rect->top + 1U;
    area += (UINT64)width * (UINT64)height;
  }

  return area;
}

static void viewer_publisher_record_snapshot_metrics_locked(
    ViewerPublisher *publisher, const ViewerFramebufferSnapshot *snapshot) {
  UINT64 dirty_area = 0;

  if (!publisher || !snapshot)
    return;

  dirty_area = viewer_publisher_dirty_area(snapshot->dirty_rects,
                                           snapshot->dirty_rect_count);
  publisher->metrics.latest_generation_available = snapshot->generation;
  publisher->metrics.latest_dirty_rect_count = snapshot->dirty_rect_count;
  publisher->metrics.latest_dirty_overflow = snapshot->dirty_overflow;
  publisher->metrics.latest_dirty_area = dirty_area;
  publisher->metrics.last_framebuffer_update_ts_ms =
      snapshot->last_update_ts_ms;
}

static BOOL
viewer_publisher_normalize_dirty_rects(ViewerFramebufferSnapshot *snapshot) {
  RECTANGLE_16 full_rect = {0};

  if (!snapshot || (snapshot->width == 0) || (snapshot->height == 0))
    return FALSE;

  if (!snapshot->dirty_overflow && (snapshot->dirty_rect_count > 0))
    return TRUE;

  full_rect.left = 0;
  full_rect.top = 0;
  full_rect.right = (UINT16)(snapshot->width - 1U);
  full_rect.bottom = (UINT16)(snapshot->height - 1U);
  snapshot->dirty_rects[0] = full_rect;
  snapshot->dirty_rect_count = 1;
  snapshot->dirty_overflow = FALSE;
  return TRUE;
}

BOOL viewer_publisher_make_full_frame_dirty(
    ViewerFramebufferSnapshot *snapshot) {
  RECTANGLE_16 full_rect = {0};

  if (!snapshot || (snapshot->width == 0) || (snapshot->height == 0))
    return FALSE;

  full_rect.left = 0;
  full_rect.top = 0;
  full_rect.right = (UINT16)(snapshot->width - 1U);
  full_rect.bottom = (UINT16)(snapshot->height - 1U);
  snapshot->dirty_rects[0] = full_rect;
  snapshot->dirty_rect_count = 1;
  snapshot->dirty_overflow = FALSE;
  return TRUE;
}

static void viewer_publisher_count_drop(ViewerPublisher *publisher) {
  if (!publisher || !publisher->initialized || !publisher->lock_initialized)
    return;

  EnterCriticalSection(&publisher->lock);
  publisher->metrics.dropped_updates++;
  LeaveCriticalSection(&publisher->lock);
}

BOOL viewer_publisher_init(ViewerPublisher *publisher) {
  if (!publisher)
    return FALSE;

  if (publisher->initialized)
    return TRUE;

  memset(publisher, 0, sizeof(*publisher));
  if (!InitializeCriticalSectionAndSpinCount(&publisher->lock, 4000))
    return FALSE;

  publisher->lock_initialized = TRUE;
  publisher->initialized = TRUE;
  return TRUE;
}

void viewer_publisher_uninit(ViewerPublisher *publisher) {
  if (!publisher)
    return;

  if (publisher->lock_initialized) {
    EnterCriticalSection(&publisher->lock);
    publisher->initialized = FALSE;
    publisher->has_pending_snapshot = FALSE;
    publisher->pending_generation = 0;
    memset(&publisher->metrics, 0, sizeof(publisher->metrics));
    LeaveCriticalSection(&publisher->lock);
    DeleteCriticalSection(&publisher->lock);
  }

  memset(publisher, 0, sizeof(*publisher));
}

void viewer_publisher_reset_metrics(ViewerPublisher *publisher) {
  if (!publisher || !publisher->initialized)
    return;

  EnterCriticalSection(&publisher->lock);
  memset(&publisher->metrics, 0, sizeof(publisher->metrics));
  publisher->has_pending_snapshot = FALSE;
  publisher->pending_generation = 0;
  LeaveCriticalSection(&publisher->lock);
}

void viewer_publisher_note_generation(ViewerPublisher *publisher,
                                      UINT64 generation) {
  viewer_publisher_note_framebuffer_update(publisher, generation, 0, FALSE);
}

void viewer_publisher_note_framebuffer_update(ViewerPublisher *publisher,
                                              UINT64 generation,
                                              UINT32 dirty_rect_count,
                                              BOOL dirty_overflow) {
  if (!publisher || !publisher->initialized)
    return;

  EnterCriticalSection(&publisher->lock);
  publisher->metrics.latest_generation_available = generation;
  publisher->metrics.latest_dirty_rect_count = dirty_rect_count;
  publisher->metrics.latest_dirty_overflow = dirty_overflow;
  publisher->metrics.last_framebuffer_update_ts_ms =
      platform_get_timestamp_ms();
  publisher->metrics.observed_framebuffer_updates++;
  publisher->metrics.observed_dirty_rects += dirty_rect_count;
  LeaveCriticalSection(&publisher->lock);
}

void viewer_publisher_note_classic_queue_state(ViewerPublisher *publisher,
                                               UINT32 queue_depth,
                                               UINT64 queued_bytes) {
  if (!publisher || !publisher->initialized)
    return;

  EnterCriticalSection(&publisher->lock);
  publisher->metrics.classic_queue_depth = queue_depth;
  publisher->metrics.classic_queue_bytes = queued_bytes;
  if (queue_depth > publisher->metrics.classic_queue_max_depth)
    publisher->metrics.classic_queue_max_depth = queue_depth;
  if (queued_bytes > publisher->metrics.classic_queue_max_bytes)
    publisher->metrics.classic_queue_max_bytes = queued_bytes;
  LeaveCriticalSection(&publisher->lock);
}

void viewer_publisher_note_classic_drop(ViewerPublisher *publisher) {
  if (!publisher || !publisher->initialized)
    return;

  EnterCriticalSection(&publisher->lock);
  publisher->metrics.classic_queue_dropped_events++;
  LeaveCriticalSection(&publisher->lock);
}

void viewer_publisher_note_classic_drop_bytes(ViewerPublisher *publisher,
                                              UINT32 dropped_count,
                                              UINT64 dropped_bytes) {
  if (!publisher || !publisher->initialized || (dropped_count == 0))
    return;

  EnterCriticalSection(&publisher->lock);
  publisher->metrics.classic_queue_dropped_events += dropped_count;
  publisher->metrics.classic_queue_dropped_bytes += dropped_bytes;
  LeaveCriticalSection(&publisher->lock);
}

void viewer_publisher_set_classic_policy(
    ViewerPublisher *publisher,
    const ViewerPublisherClassicPolicyConfig *config) {
  ViewerPublisherClassicPolicyConfig next = {0};

  if (!publisher || !publisher->initialized)
    return;

  if (config && config->enabled &&
      (config->policy == VIEWER_PUBLISHER_CLASSIC_POLICY_LATEST_STATE)) {
    next = *config;
  } else {
    next.enabled = FALSE;
    next.policy = VIEWER_PUBLISHER_CLASSIC_POLICY_FIFO;
  }

  EnterCriticalSection(&publisher->lock);
  publisher->classic_policy = next;
  LeaveCriticalSection(&publisher->lock);
}

ViewerPublisherClassicDecision viewer_publisher_classic_queue_decision(
    ViewerPublisher *publisher, UINT32 queue_depth, UINT64 queued_bytes) {
  ViewerPublisherClassicDecision decision =
      VIEWER_PUBLISHER_CLASSIC_DECISION_KEEP_FIFO;

  if (!publisher || !publisher->initialized)
    return decision;

  EnterCriticalSection(&publisher->lock);
  if (publisher->classic_policy.enabled &&
      (publisher->classic_policy.policy ==
       VIEWER_PUBLISHER_CLASSIC_POLICY_LATEST_STATE) &&
      (((publisher->classic_policy.max_queue_depth > 0) &&
        (queue_depth > publisher->classic_policy.max_queue_depth)) ||
       ((publisher->classic_policy.max_queue_bytes > 0) &&
        (queued_bytes > publisher->classic_policy.max_queue_bytes)))) {
    decision = VIEWER_PUBLISHER_CLASSIC_DECISION_REPLACE_WITH_BASELINE;
    publisher->metrics.classic_latest_replacements++;
  }
  LeaveCriticalSection(&publisher->lock);
  return decision;
}

ViewerPublisherBitmapPublishDecision
viewer_publisher_bitmap_publish_decision(BOOL ready, BOOL needs_full_refresh,
                                         BOOL refresh_in_flight,
                                         BOOL throttled) {
  ViewerPublisherBitmapPublishDecision decision = {0};

  if (!ready) {
    decision.action = VIEWER_PUBLISHER_BITMAP_PUBLISH_NOT_READY;
    return decision;
  }

  if (needs_full_refresh) {
    decision.action =
        VIEWER_PUBLISHER_BITMAP_PUBLISH_CLEAR_FULL_REFRESH_AND_ENQUEUE;
    decision.clear_full_refresh = TRUE;
    decision.count_full_refresh_gate = !refresh_in_flight;
    decision.enqueue = TRUE;
    return decision;
  }

  if (throttled) {
    decision.action =
        VIEWER_PUBLISHER_BITMAP_PUBLISH_THROTTLE_AND_REQUEST_REFRESH;
    decision.count_throttled = TRUE;
    decision.request_full_refresh = TRUE;
    return decision;
  }

  decision.action = VIEWER_PUBLISHER_BITMAP_PUBLISH_ENQUEUE;
  decision.enqueue = TRUE;
  return decision;
}

ViewerPublisherSurfaceBitsPublishDecision
viewer_publisher_surface_bits_publish_decision(BOOL ready,
                                               BOOL needs_full_refresh,
                                               BOOL throttled) {
  ViewerPublisherSurfaceBitsPublishDecision decision = {0};

  if (!ready) {
    decision.action = VIEWER_PUBLISHER_SURFACE_BITS_PUBLISH_NOT_READY;
    return decision;
  }

  decision.action = VIEWER_PUBLISHER_SURFACE_BITS_PUBLISH_ENQUEUE;
  decision.enqueue = TRUE;
  if (needs_full_refresh) {
    decision.clear_full_refresh = TRUE;
    decision.count_full_refresh_gate = TRUE;
  }
  if (throttled) {
    decision.count_throttled = TRUE;
    decision.request_full_refresh = TRUE;
  }
  return decision;
}

ViewerPublisherClassicPumpDecision
viewer_publisher_classic_pump_decision(BOOL needs_full_refresh,
                                       UINT32 bitmap_queue_depth) {
  if (needs_full_refresh && (bitmap_queue_depth > 0))
    return VIEWER_PUBLISHER_CLASSIC_PUMP_DROP_BITMAPS_FOR_FULL_REFRESH;
  return VIEWER_PUBLISHER_CLASSIC_PUMP_SEND_BITMAPS;
}

BOOL viewer_publisher_classic_latest_snapshot(
    ViewerPublisher *publisher, ViewerFramebuffer *framebuffer,
    UINT64 viewer_last_generation_sent, ViewerFramebufferSnapshot *snapshot) {
  if (!publisher || !publisher->initialized || !framebuffer || !snapshot)
    return FALSE;

  /* Do not hold the publisher lock while taking/copying framebuffer pixels. */
  if (!viewer_framebuffer_snapshot(framebuffer, snapshot)) {
    EnterCriticalSection(&publisher->lock);
    publisher->metrics.classic_latest_snapshot_failures++;
    LeaveCriticalSection(&publisher->lock);
    return FALSE;
  }

  if (!viewer_publisher_make_full_frame_dirty(snapshot)) {
    viewer_framebuffer_snapshot_free(snapshot);
    EnterCriticalSection(&publisher->lock);
    publisher->metrics.classic_latest_snapshot_failures++;
    LeaveCriticalSection(&publisher->lock);
    return FALSE;
  }

  EnterCriticalSection(&publisher->lock);
  viewer_publisher_record_snapshot_metrics_locked(publisher, snapshot);
  if (snapshot->generation <= viewer_last_generation_sent) {
    publisher->metrics.classic_latest_suppressed++;
    LeaveCriticalSection(&publisher->lock);
    viewer_framebuffer_snapshot_free(snapshot);
    return FALSE;
  }

  publisher->metrics.queued_updates++;
  publisher->metrics.queued_bytes += (UINT64)snapshot->pixel_bytes;
  publisher->metrics.last_publisher_enqueue_ts_ms = platform_get_timestamp_ms();
  LeaveCriticalSection(&publisher->lock);
  return TRUE;
}

BOOL viewer_publisher_gfx_dirty_snapshot(ViewerPublisher *publisher,
                                         ViewerFramebuffer *framebuffer,
                                         UINT64 viewer_last_generation_sent,
                                         ViewerFramebufferSnapshot *snapshot) {
  if (!publisher || !publisher->initialized || !framebuffer || !snapshot)
    return FALSE;

  /* Snapshot owns copied pixels/dirty metadata; publisher lock is not held
   * while copying framebuffer data. Generation filtering is per viewer. */
  if (!viewer_framebuffer_snapshot(framebuffer, snapshot)) {
    viewer_publisher_count_drop(publisher);
    return FALSE;
  }

  EnterCriticalSection(&publisher->lock);
  viewer_publisher_record_snapshot_metrics_locked(publisher, snapshot);
  if (snapshot->generation <= viewer_last_generation_sent) {
    LeaveCriticalSection(&publisher->lock);
    viewer_framebuffer_snapshot_free(snapshot);
    return FALSE;
  }
  LeaveCriticalSection(&publisher->lock);

  if (snapshot->dirty_overflow || (snapshot->dirty_rect_count >
                                   VIEWER_PUBLISHER_GFX_DIRTY_RECT_THRESHOLD)) {
    if (!viewer_publisher_make_full_frame_dirty(snapshot)) {
      viewer_framebuffer_snapshot_free(snapshot);
      viewer_publisher_count_drop(publisher);
      return FALSE;
    }
  } else if (!viewer_publisher_normalize_dirty_rects(snapshot)) {
    viewer_framebuffer_snapshot_free(snapshot);
    viewer_publisher_count_drop(publisher);
    return FALSE;
  }

  EnterCriticalSection(&publisher->lock);
  viewer_publisher_record_snapshot_metrics_locked(publisher, snapshot);
  publisher->metrics.queued_updates++;
  publisher->metrics.queued_bytes += (UINT64)snapshot->pixel_bytes;
  publisher->metrics.last_publisher_enqueue_ts_ms = platform_get_timestamp_ms();
  LeaveCriticalSection(&publisher->lock);
  return TRUE;
}

BOOL viewer_publisher_snapshot(ViewerPublisher *publisher,
                               ViewerFramebuffer *framebuffer,
                               ViewerFramebufferSnapshot *snapshot) {
  BOOL accepted = FALSE;

  if (!publisher || !publisher->initialized || !framebuffer || !snapshot)
    return FALSE;

  /* Do not hold the publisher lock while taking/copying framebuffer pixels. */
  if (!viewer_framebuffer_snapshot(framebuffer, snapshot)) {
    viewer_publisher_count_drop(publisher);
    return FALSE;
  }

  if (!viewer_publisher_normalize_dirty_rects(snapshot)) {
    viewer_framebuffer_snapshot_free(snapshot);
    viewer_publisher_count_drop(publisher);
    return FALSE;
  }

  EnterCriticalSection(&publisher->lock);
  viewer_publisher_record_snapshot_metrics_locked(publisher, snapshot);
  if (snapshot->generation <= publisher->metrics.last_generation_sent) {
    LeaveCriticalSection(&publisher->lock);
    viewer_framebuffer_snapshot_free(snapshot);
    return FALSE;
  }

  if (!publisher->has_pending_snapshot) {
    publisher->has_pending_snapshot = TRUE;
    publisher->pending_generation = snapshot->generation;
    publisher->metrics.queued_updates++;
    publisher->metrics.last_publisher_enqueue_ts_ms =
        platform_get_timestamp_ms();
    accepted = TRUE;
  } else if (snapshot->generation > publisher->pending_generation) {
    publisher->pending_generation = snapshot->generation;
    publisher->metrics.coalesced_updates++;
    publisher->metrics.last_publisher_enqueue_ts_ms =
        platform_get_timestamp_ms();
    accepted = TRUE;
  }

  if (accepted)
    publisher->metrics.queued_bytes += (UINT64)snapshot->pixel_bytes;

  LeaveCriticalSection(&publisher->lock);
  return TRUE;
}

BOOL viewer_publisher_classic_baseline_snapshot(
    ViewerPublisher *publisher, ViewerFramebuffer *framebuffer,
    ViewerFramebufferSnapshot *snapshot) {
  if (!publisher || !publisher->initialized || !framebuffer || !snapshot)
    return FALSE;

  /* This late-join baseline is per viewer, so it intentionally bypasses the
   * publisher's global last_generation_sent suppression. Do not hold the
   * publisher lock while taking/copying framebuffer pixels. */
  if (!viewer_framebuffer_snapshot(framebuffer, snapshot)) {
    viewer_publisher_count_drop(publisher);
    return FALSE;
  }

  if (!viewer_publisher_make_full_frame_dirty(snapshot)) {
    viewer_framebuffer_snapshot_free(snapshot);
    viewer_publisher_count_drop(publisher);
    return FALSE;
  }

  EnterCriticalSection(&publisher->lock);
  viewer_publisher_record_snapshot_metrics_locked(publisher, snapshot);
  publisher->metrics.queued_updates++;
  publisher->metrics.queued_bytes += (UINT64)snapshot->pixel_bytes;
  publisher->metrics.last_publisher_enqueue_ts_ms = platform_get_timestamp_ms();
  LeaveCriticalSection(&publisher->lock);
  return TRUE;
}

void viewer_publisher_mark_consumed(ViewerPublisher *publisher,
                                    UINT64 generation) {
  if (!publisher || !publisher->initialized)
    return;

  EnterCriticalSection(&publisher->lock);
  if (generation < publisher->metrics.last_generation_sent) {
    LeaveCriticalSection(&publisher->lock);
    return;
  }

  publisher->metrics.last_generation_sent = generation;
  if (publisher->has_pending_snapshot &&
      (generation >= publisher->pending_generation)) {
    publisher->has_pending_snapshot = FALSE;
    publisher->pending_generation = 0;
  }
  LeaveCriticalSection(&publisher->lock);
}

ViewerPublisherMetrics
viewer_publisher_get_metrics(const ViewerPublisher *publisher) {
  ViewerPublisherMetrics metrics = {0};

  if (!publisher || !publisher->initialized)
    return metrics;

  EnterCriticalSection((CRITICAL_SECTION *)&publisher->lock);
  metrics = publisher->metrics;
  LeaveCriticalSection((CRITICAL_SECTION *)&publisher->lock);
  return metrics;
}
