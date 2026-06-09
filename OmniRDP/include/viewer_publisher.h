#ifndef VIEWER_PUBLISHER_H
#define VIEWER_PUBLISHER_H

#include "viewer_framebuffer.h"
#include "viewer_publisher_config.h"

#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIEWER_PUBLISHER_GFX_DIRTY_RECT_THRESHOLD 64U

typedef struct {
  UINT64 queued_updates;
  /* Cumulative bytes accepted as pending/coalesced snapshots, not current
   * queue depth. */
  UINT64 queued_bytes;
  UINT64 coalesced_updates;
  UINT64 dropped_updates;
  UINT64 last_generation_sent;
  UINT64 latest_generation_available;
  UINT64 observed_framebuffer_updates;
  UINT64 observed_dirty_rects;
  UINT64 observed_dirty_area;
  UINT64 latest_dirty_area;
  UINT64 last_framebuffer_update_ts_ms;
  UINT64 last_publisher_enqueue_ts_ms;
  UINT64 classic_queue_bytes;
  UINT64 classic_queue_dropped_bytes;
  UINT64 classic_queue_dropped_events;
  UINT64 classic_queue_max_bytes;
  UINT64 classic_latest_replacements;
  UINT64 classic_latest_suppressed;
  UINT64 classic_latest_snapshot_failures;
  UINT32 latest_dirty_rect_count;
  UINT32 classic_queue_depth;
  UINT32 classic_queue_max_depth;
  BOOL latest_dirty_overflow;
} ViewerPublisherMetrics;

typedef enum {
  VIEWER_PUBLISHER_CLASSIC_DECISION_KEEP_FIFO = 0,
  VIEWER_PUBLISHER_CLASSIC_DECISION_REPLACE_WITH_BASELINE = 1
} ViewerPublisherClassicDecision;

typedef enum {
  VIEWER_PUBLISHER_BITMAP_PUBLISH_NOT_READY = 0,
  VIEWER_PUBLISHER_BITMAP_PUBLISH_ENQUEUE = 1,
  VIEWER_PUBLISHER_BITMAP_PUBLISH_CLEAR_FULL_REFRESH_AND_ENQUEUE = 2,
  VIEWER_PUBLISHER_BITMAP_PUBLISH_THROTTLE_AND_REQUEST_REFRESH = 3
} ViewerPublisherBitmapPublishAction;

typedef struct {
  ViewerPublisherBitmapPublishAction action;
  BOOL clear_full_refresh;
  BOOL count_full_refresh_gate;
  BOOL count_throttled;
  BOOL request_full_refresh;
  BOOL enqueue;
} ViewerPublisherBitmapPublishDecision;

typedef enum {
  VIEWER_PUBLISHER_SURFACE_BITS_PUBLISH_NOT_READY = 0,
  VIEWER_PUBLISHER_SURFACE_BITS_PUBLISH_ENQUEUE = 1
} ViewerPublisherSurfaceBitsPublishAction;

typedef struct {
  ViewerPublisherSurfaceBitsPublishAction action;
  BOOL clear_full_refresh;
  BOOL count_full_refresh_gate;
  BOOL count_throttled;
  BOOL request_full_refresh;
  BOOL enqueue;
} ViewerPublisherSurfaceBitsPublishDecision;

typedef enum {
  VIEWER_PUBLISHER_CLASSIC_PUMP_SEND_BITMAPS = 0,
  VIEWER_PUBLISHER_CLASSIC_PUMP_DROP_BITMAPS_FOR_FULL_REFRESH = 1
} ViewerPublisherClassicPumpDecision;

typedef struct {
  BOOL initialized;
  BOOL lock_initialized;
  BOOL has_pending_snapshot;
  UINT64 pending_generation;
  ViewerPublisherClassicPolicyConfig classic_policy;
  ViewerPublisherMetrics metrics;
  CRITICAL_SECTION lock;
} ViewerPublisher;

BOOL viewer_publisher_init(ViewerPublisher *publisher);
void viewer_publisher_uninit(ViewerPublisher *publisher);
void viewer_publisher_reset_metrics(ViewerPublisher *publisher);
void viewer_publisher_note_generation(ViewerPublisher *publisher,
                                      UINT64 generation);
void viewer_publisher_note_framebuffer_update(ViewerPublisher *publisher,
                                              UINT64 generation,
                                              UINT32 dirty_rect_count,
                                              BOOL dirty_overflow);
void viewer_publisher_note_classic_queue_state(ViewerPublisher *publisher,
                                               UINT32 queue_depth,
                                               UINT64 queued_bytes);
void viewer_publisher_note_classic_drop(ViewerPublisher *publisher);
void viewer_publisher_note_classic_drop_bytes(ViewerPublisher *publisher,
                                              UINT32 dropped_count,
                                              UINT64 dropped_bytes);
void viewer_publisher_set_classic_policy(
    ViewerPublisher *publisher,
    const ViewerPublisherClassicPolicyConfig *config);
ViewerPublisherClassicDecision viewer_publisher_classic_queue_decision(
    ViewerPublisher *publisher, UINT32 queue_depth, UINT64 queued_bytes);
ViewerPublisherBitmapPublishDecision
viewer_publisher_bitmap_publish_decision(BOOL ready, BOOL needs_full_refresh,
                                         BOOL refresh_in_flight,
                                         BOOL throttled);
ViewerPublisherSurfaceBitsPublishDecision
viewer_publisher_surface_bits_publish_decision(BOOL ready,
                                               BOOL needs_full_refresh,
                                               BOOL throttled);
ViewerPublisherClassicPumpDecision
viewer_publisher_classic_pump_decision(BOOL needs_full_refresh,
                                       UINT32 bitmap_queue_depth);
BOOL viewer_publisher_classic_latest_snapshot(
    ViewerPublisher *publisher, ViewerFramebuffer *framebuffer,
    UINT64 viewer_last_generation_sent, ViewerFramebufferSnapshot *snapshot);
BOOL viewer_publisher_gfx_dirty_snapshot(ViewerPublisher *publisher,
                                         ViewerFramebuffer *framebuffer,
                                         UINT64 viewer_last_generation_sent,
                                         ViewerFramebufferSnapshot *snapshot);
BOOL viewer_publisher_make_full_frame_dirty(
    ViewerFramebufferSnapshot *snapshot);
/* Returns FALSE without counting a drop when snapshot generation is already
 * consumed (generation <= last_generation_sent). Dirty rectangles use inclusive
 * left/top/right/bottom coordinates. Empty or overflow dirty lists normalize to
 * one inclusive full-frame rectangle. */
BOOL viewer_publisher_snapshot(ViewerPublisher *publisher,
                               ViewerFramebuffer *framebuffer,
                               ViewerFramebufferSnapshot *snapshot);
BOOL viewer_publisher_classic_baseline_snapshot(
    ViewerPublisher *publisher, ViewerFramebuffer *framebuffer,
    ViewerFramebufferSnapshot *snapshot);
void viewer_publisher_mark_consumed(ViewerPublisher *publisher,
                                    UINT64 generation);
ViewerPublisherMetrics
viewer_publisher_get_metrics(const ViewerPublisher *publisher);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_PUBLISHER_H */
