#ifndef VIEWER_CLASSIC_QUEUE_H
#define VIEWER_CLASSIC_QUEUE_H

#include "viewer_framebuffer.h"

#include <freerdp/update.h>
#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIEWER_CLASSIC_QUEUE_CAPACITY 32U
#define VIEWER_SURFACE_BITS_QUEUE_CAPACITY 1024U

typedef struct ViewerClassicEvent {
  BITMAP_UPDATE *bitmap; /* deep-copied, owned by this event */
  UINT64 generation;     /* nonzero for framebuffer-backed latest baselines */
} ViewerClassicEvent;

typedef struct ViewerSurfaceBitsEvent {
  SURFACE_BITS_COMMAND cmd; /* deep-copied, owned by this event */
} ViewerSurfaceBitsEvent;

typedef struct {
  UINT32 dropped_count;
  UINT64 dropped_payload_bytes;
} ViewerClassicQueueDropInfo;

typedef struct {
  ViewerClassicEvent *classic_queue[VIEWER_CLASSIC_QUEUE_CAPACITY];
  UINT32 classic_queue_head;
  UINT32 classic_queue_tail;
  UINT32 classic_queue_count;
  HANDLE classic_event;
  ViewerSurfaceBitsEvent
      *surface_bits_queue[VIEWER_SURFACE_BITS_QUEUE_CAPACITY];
  UINT32 surface_bits_queue_head;
  UINT32 surface_bits_queue_tail;
  UINT32 surface_bits_queue_count;
} ViewerClassicQueues;

BOOL viewer_classic_queues_init(ViewerClassicQueues *queues);
void viewer_classic_queues_uninit(ViewerClassicQueues *queues);

ViewerClassicEvent *viewer_classic_event_new(const BITMAP_UPDATE *bitmap);
ViewerClassicEvent *
viewer_classic_event_from_snapshot(const ViewerFramebufferSnapshot *snapshot);
void viewer_classic_event_free(ViewerClassicEvent *event);
UINT64 viewer_classic_event_payload_bytes(const ViewerClassicEvent *event);
const BITMAP_UPDATE *
viewer_classic_event_bitmap(const ViewerClassicEvent *event);
UINT64 viewer_classic_event_generation(const ViewerClassicEvent *event);

ViewerSurfaceBitsEvent *
viewer_surface_bits_event_new(const SURFACE_BITS_COMMAND *cmd);
void viewer_surface_bits_event_free(ViewerSurfaceBitsEvent *event);
const SURFACE_BITS_COMMAND *
viewer_surface_bits_event_command(const ViewerSurfaceBitsEvent *event);

/* Caller must hold the owning viewer's send_lock for all *_locked APIs. */
BOOL viewer_classic_queue_enqueue_event_locked(
    ViewerClassicQueues *queues, ViewerClassicEvent *event,
    ViewerClassicQueueDropInfo *drop_info);
void viewer_classic_queue_enqueue_event_direct_locked(
    ViewerClassicQueues *queues, ViewerClassicEvent *event);
ViewerClassicEvent *
viewer_classic_queue_dequeue_locked(ViewerClassicQueues *queues);
void viewer_classic_queue_clear_locked(ViewerClassicQueues *queues,
                                       ViewerClassicQueueDropInfo *drop_info);
UINT32 viewer_classic_queue_depth_locked(const ViewerClassicQueues *queues);
UINT64
viewer_classic_queue_payload_bytes_locked(const ViewerClassicQueues *queues);

BOOL viewer_surface_bits_queue_enqueue_event_locked(
    ViewerClassicQueues *queues, ViewerSurfaceBitsEvent *event,
    ViewerClassicQueueDropInfo *drop_info);
ViewerSurfaceBitsEvent *
viewer_surface_bits_queue_dequeue_locked(ViewerClassicQueues *queues);
void viewer_surface_bits_queue_clear_locked(
    ViewerClassicQueues *queues, ViewerClassicQueueDropInfo *drop_info);
UINT32
viewer_surface_bits_queue_depth_locked(const ViewerClassicQueues *queues);

HANDLE viewer_classic_queues_event(const ViewerClassicQueues *queues);
void viewer_classic_queues_signal(const ViewerClassicQueues *queues);
void viewer_classic_queues_reset_event(const ViewerClassicQueues *queues);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_CLASSIC_QUEUE_H */
