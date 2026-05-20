#include "viewer_publisher.h"

#include <string.h>

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

static BOOL
viewer_publisher_make_full_frame_dirty(ViewerFramebufferSnapshot *snapshot) {
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
  publisher->metrics.latest_generation_available = snapshot->generation;
  publisher->metrics.latest_dirty_rect_count = snapshot->dirty_rect_count;
  publisher->metrics.latest_dirty_overflow = snapshot->dirty_overflow;
  if (snapshot->generation <= publisher->metrics.last_generation_sent) {
    LeaveCriticalSection(&publisher->lock);
    viewer_framebuffer_snapshot_free(snapshot);
    return FALSE;
  }

  if (!publisher->has_pending_snapshot) {
    publisher->has_pending_snapshot = TRUE;
    publisher->pending_generation = snapshot->generation;
    publisher->metrics.queued_updates++;
    accepted = TRUE;
  } else if (snapshot->generation > publisher->pending_generation) {
    publisher->pending_generation = snapshot->generation;
    publisher->metrics.coalesced_updates++;
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
  publisher->metrics.latest_generation_available = snapshot->generation;
  publisher->metrics.latest_dirty_rect_count = snapshot->dirty_rect_count;
  publisher->metrics.latest_dirty_overflow = snapshot->dirty_overflow;
  publisher->metrics.queued_updates++;
  publisher->metrics.queued_bytes += (UINT64)snapshot->pixel_bytes;
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
