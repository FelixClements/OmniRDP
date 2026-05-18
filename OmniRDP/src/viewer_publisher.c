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

BOOL viewer_publisher_init(ViewerPublisher *publisher) {
  if (!publisher)
    return FALSE;

  if (publisher->initialized)
    return TRUE;

  memset(publisher, 0, sizeof(*publisher));
  publisher->initialized = TRUE;
  return TRUE;
}

void viewer_publisher_uninit(ViewerPublisher *publisher) {
  if (!publisher)
    return;

  memset(publisher, 0, sizeof(*publisher));
}

void viewer_publisher_reset_metrics(ViewerPublisher *publisher) {
  if (!publisher)
    return;

  memset(&publisher->metrics, 0, sizeof(publisher->metrics));
  publisher->has_pending_snapshot = FALSE;
  publisher->pending_generation = 0;
}

void viewer_publisher_note_generation(ViewerPublisher *publisher,
                                      UINT64 generation) {
  if (!publisher)
    return;

  publisher->metrics.latest_generation_available = generation;
}

BOOL viewer_publisher_snapshot(ViewerPublisher *publisher,
                               ViewerFramebuffer *framebuffer,
                               ViewerFramebufferSnapshot *snapshot) {
  BOOL accepted = FALSE;

  if (!publisher || !publisher->initialized || !framebuffer || !snapshot)
    return FALSE;

  if (!viewer_framebuffer_snapshot(framebuffer, snapshot)) {
    publisher->metrics.dropped_updates++;
    return FALSE;
  }

  if (!viewer_publisher_normalize_dirty_rects(snapshot)) {
    viewer_framebuffer_snapshot_free(snapshot);
    publisher->metrics.dropped_updates++;
    return FALSE;
  }

  publisher->metrics.latest_generation_available = snapshot->generation;
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

  return TRUE;
}

void viewer_publisher_mark_consumed(ViewerPublisher *publisher,
                                    UINT64 generation) {
  if (!publisher || !publisher->initialized)
    return;

  publisher->metrics.last_generation_sent = generation;
  if (publisher->has_pending_snapshot &&
      (generation >= publisher->pending_generation)) {
    publisher->has_pending_snapshot = FALSE;
    publisher->pending_generation = 0;
  }
}

ViewerPublisherMetrics
viewer_publisher_get_metrics(const ViewerPublisher *publisher) {
  ViewerPublisherMetrics metrics = {0};

  if (!publisher)
    return metrics;

  return publisher->metrics;
}
