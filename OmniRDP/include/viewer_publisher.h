#ifndef VIEWER_PUBLISHER_H
#define VIEWER_PUBLISHER_H

#include "viewer_framebuffer.h"

#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  UINT64 queued_updates;
  /* Cumulative bytes accepted as pending/coalesced snapshots, not current
   * queue depth. */
  UINT64 queued_bytes;
  UINT64 coalesced_updates;
  UINT64 dropped_updates;
  UINT64 last_generation_sent;
  UINT64 latest_generation_available;
} ViewerPublisherMetrics;

typedef struct {
  BOOL initialized;
  BOOL has_pending_snapshot;
  UINT64 pending_generation;
  ViewerPublisherMetrics metrics;
} ViewerPublisher;

BOOL viewer_publisher_init(ViewerPublisher *publisher);
void viewer_publisher_uninit(ViewerPublisher *publisher);
void viewer_publisher_reset_metrics(ViewerPublisher *publisher);
void viewer_publisher_note_generation(ViewerPublisher *publisher,
                                      UINT64 generation);
/* Returns FALSE without counting a drop when snapshot generation is already
 * consumed (generation <= last_generation_sent). Dirty rectangles use inclusive
 * left/top/right/bottom coordinates. Empty or overflow dirty lists normalize to
 * one inclusive full-frame rectangle. */
BOOL viewer_publisher_snapshot(ViewerPublisher *publisher,
                               ViewerFramebuffer *framebuffer,
                               ViewerFramebufferSnapshot *snapshot);
void viewer_publisher_mark_consumed(ViewerPublisher *publisher,
                                    UINT64 generation);
ViewerPublisherMetrics
viewer_publisher_get_metrics(const ViewerPublisher *publisher);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_PUBLISHER_H */
