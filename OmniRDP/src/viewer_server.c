#include "viewer_server.h"
#include "backend.h"
#include "platform_compat.h"
#include "svc_log.h"
#include "viewer_internal.h"

#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/freerdp.h>
#include <freerdp/input.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/update.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <winpr/wlog.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define TAG "multiplexer.viewer"
#define INPUT_IDLE_TIMEOUT_MS 2500U
#define FULL_REFRESH_TIMEOUT_MS 3000U
#define VIEWER_WAIT_REPLAY_ACK_TIMEOUT_MS 5000U
#define VIEWER_RDPEGFX_NEGOTIATION_TIMEOUT_MS 3000U
#define VIEWER_GFX_MIN_REPLAY_BASELINE_COMMANDS 2U
#define VIEWER_GFX_MIN_REPLAY_BASELINE_BYTES 4096U
#define VIEWER_GFX_BACKEND_REFRESH_TIMEOUT_MS 15000U

static ViewerServer *g_viewer_server = NULL;

static BOOL viewer_gfx_enter_classic_fallback(ViewerServer *server,
                                              Viewer *viewer, UINT64 now,
                                              const char *reason);
static BOOL viewer_gfx_request_backend_refresh(ViewerServer *server,
                                               Viewer *viewer, UINT64 now,
                                               const char *reason);
static BOOL viewer_gfx_bootstrap_direct_live(ViewerServer *server,
                                             Viewer *viewer, UINT64 now,
                                             const char *reason);
static void viewer_gfx_queue_clear_locked(ViewerGraphicsContext *gfx);
static UINT64 viewer_perf_now_us(void);
static BOOL viewer_should_log_bitmap_perf(UINT64 batch_count, UINT64 publish_us,
                                          UINT32 send_failed_count);
static BOOL viewer_send_bitmap_update(Viewer *viewer,
                                      const BITMAP_UPDATE *bitmap);
static BOOL viewer_send_bitmap_update_locked(Viewer *viewer,
                                             const BITMAP_UPDATE *bitmap);
static BOOL viewer_send_surface_bits(Viewer *viewer,
                                     const SURFACE_BITS_COMMAND *cmd);

static UINT64 viewer_perf_now_us(void) {
#ifdef _WIN32
  static LARGE_INTEGER frequency = {0};
  LARGE_INTEGER counter = {0};

  if (frequency.QuadPart == 0) {
    if (!QueryPerformanceFrequency(&frequency) || (frequency.QuadPart <= 0))
      return platform_get_timestamp_ms() * 1000ULL;
  }

  if (!QueryPerformanceCounter(&counter))
    return platform_get_timestamp_ms() * 1000ULL;

  return (UINT64)((counter.QuadPart * 1000000ULL) / frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((UINT64)ts.tv_sec * 1000000ULL) + ((UINT64)ts.tv_nsec / 1000ULL);
#endif
}

static BOOL viewer_should_log_bitmap_perf(UINT64 batch_count, UINT64 publish_us,
                                          UINT32 send_failed_count) {
  return (batch_count <= 5) || ((batch_count % 100) == 0) ||
         (publish_us >= 5000ULL) || (send_failed_count > 0);
}

/* ---- Classic bitmap queue: deep-copy helpers ---- */

static ViewerClassicEvent *
viewer_classic_event_new(const BITMAP_UPDATE *bitmap) {
  ViewerClassicEvent *event = NULL;
  UINT32 i = 0;

  if (!bitmap)
    return NULL;

  event = (ViewerClassicEvent *)calloc(1, sizeof(ViewerClassicEvent));
  if (!event)
    return NULL;

  event->bitmap = (BITMAP_UPDATE *)calloc(1, sizeof(BITMAP_UPDATE));
  if (!event->bitmap) {
    free(event);
    return NULL;
  }

  /* Shallow-copy top-level fields */
  event->bitmap->number = bitmap->number;
  event->bitmap->skipCompression = bitmap->skipCompression;

  if (bitmap->number == 0) {
    event->bitmap->rectangles = NULL;
    return event;
  }

  /* Deep-copy the rectangles array */
  event->bitmap->rectangles =
      (BITMAP_DATA *)calloc(bitmap->number, sizeof(BITMAP_DATA));
  if (!event->bitmap->rectangles) {
    free(event->bitmap);
    free(event);
    return NULL;
  }

  for (i = 0; i < bitmap->number; i++) {
    /* Copy inline fields */
    event->bitmap->rectangles[i] = bitmap->rectangles[i];

    /* Deep-copy the bitmap data stream */
    if (bitmap->rectangles[i].bitmapLength > 0 &&
        bitmap->rectangles[i].bitmapDataStream) {
      event->bitmap->rectangles[i].bitmapDataStream =
          (BYTE *)malloc(bitmap->rectangles[i].bitmapLength);
      if (!event->bitmap->rectangles[i].bitmapDataStream) {
        /* Free already-allocated rectangles on failure */
        for (UINT32 j = 0; j < i; j++) {
          free(event->bitmap->rectangles[j].bitmapDataStream);
          event->bitmap->rectangles[j].bitmapDataStream = NULL;
        }
        free(event->bitmap->rectangles);
        free(event->bitmap);
        free(event);
        return NULL;
      }
      memcpy(event->bitmap->rectangles[i].bitmapDataStream,
             bitmap->rectangles[i].bitmapDataStream,
             bitmap->rectangles[i].bitmapLength);
    } else {
      event->bitmap->rectangles[i].bitmapDataStream = NULL;
    }
  }

  return event;
}

static void viewer_classic_event_free(ViewerClassicEvent *event) {
  UINT32 i = 0;

  if (!event)
    return;

  if (event->bitmap) {
    if (event->bitmap->rectangles) {
      for (i = 0; i < event->bitmap->number; i++) {
        free(event->bitmap->rectangles[i].bitmapDataStream);
      }
      free(event->bitmap->rectangles);
    }
    free(event->bitmap);
  }
  free(event);
}

/* ---- SurfaceBits event: deep-copy and free ---- */

static ViewerSurfaceBitsEvent *
viewer_surface_bits_event_new(const SURFACE_BITS_COMMAND *cmd) {
  ViewerSurfaceBitsEvent *event = NULL;

  if (!cmd)
    return NULL;

  event = (ViewerSurfaceBitsEvent *)calloc(1, sizeof(ViewerSurfaceBitsEvent));
  if (!event)
    return NULL;

  /* Shallow-copy all fields */
  event->cmd = *cmd;

  /* Deep-copy the bitmapData buffer */
  if (cmd->bmp.bitmapDataLength > 0 && cmd->bmp.bitmapData) {
    event->cmd.bmp.bitmapData = (BYTE *)malloc(cmd->bmp.bitmapDataLength);
    if (!event->cmd.bmp.bitmapData) {
      free(event);
      return NULL;
    }
    memcpy(event->cmd.bmp.bitmapData, cmd->bmp.bitmapData,
           cmd->bmp.bitmapDataLength);
  } else {
    event->cmd.bmp.bitmapData = NULL;
    event->cmd.bmp.bitmapDataLength = 0;
  }

  return event;
}

static void viewer_surface_bits_event_free(ViewerSurfaceBitsEvent *event) {
  if (!event)
    return;

  free(event->cmd.bmp.bitmapData);
  free(event);
}

/* ---- Classic bitmap queue: queue operations ---- */

/* Drop the oldest entry from the classic queue. Caller must hold send_lock. */
static void viewer_classic_queue_drop_oldest_locked(Viewer *viewer) {
  ViewerClassicEvent *oldest = NULL;

  if (viewer->classic_queue_count == 0)
    return;

  oldest = viewer->classic_queue[viewer->classic_queue_head];
  viewer->classic_queue[viewer->classic_queue_head] = NULL;
  viewer->classic_queue_head =
      (viewer->classic_queue_head + 1) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  viewer->classic_queue_count--;

  viewer_classic_event_free(oldest);
  viewer->bitmap_queue_dropped++;
}

static void viewer_classic_queue_clear_locked(Viewer *viewer) {
  while (viewer->classic_queue_count > 0)
    viewer_classic_queue_drop_oldest_locked(viewer);
}

/* Enqueue a deep-copied BITMAP_UPDATE for a viewer.
 * Called from the backend thread under viewer->send_lock.
 * Returns TRUE on success, FALSE if the viewer should be disconnected. */
static BOOL viewer_classic_enqueue_locked(Viewer *viewer,
                                          const BITMAP_UPDATE *bitmap) {
  ViewerClassicEvent *event = NULL;

  if (!viewer || !bitmap)
    return FALSE;

  /* If queue is full, drop oldest entries to make room */
  while (viewer->classic_queue_count >= VIEWER_CLASSIC_QUEUE_CAPACITY) {
    WLog_WARN(TAG, "Viewer %u classic queue full (%u entries), dropping oldest",
              viewer->id, viewer->classic_queue_count);
    viewer_classic_queue_drop_oldest_locked(viewer);

    /* After dropping, mark viewer for full refresh to resync */
    viewer->needs_full_refresh = TRUE;
    viewer->full_refresh_deadline_ts =
        platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;
  }

  event = viewer_classic_event_new(bitmap);
  if (!event) {
    WLog_ERR(TAG, "Viewer %u failed to allocate classic event", viewer->id);
    return FALSE;
  }

  viewer->classic_queue[viewer->classic_queue_tail] = event;
  viewer->classic_queue_tail =
      (viewer->classic_queue_tail + 1) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  viewer->classic_queue_count++;
  viewer->bitmap_updates_queued++;

  /* Signal the viewer thread that a new event is available */
  if (viewer->classic_event)
    SetEvent(viewer->classic_event);

  return TRUE;
}

/* Enqueue a pre-built event into the viewer's classic queue.
 * Caller must hold send_lock. The event must have been deep-copied
 * by the caller before acquiring the lock. Returns TRUE on success. */
static BOOL viewer_classic_enqueue_event_locked(Viewer *viewer,
                                                ViewerClassicEvent *event) {
  if (!viewer || !event)
    return FALSE;

  /* If queue is full, drop oldest entries to make room */
  while (viewer->classic_queue_count >= VIEWER_CLASSIC_QUEUE_CAPACITY) {
    WLog_WARN(TAG, "Viewer %u classic queue full (%u entries), dropping oldest",
              viewer->id, viewer->classic_queue_count);
    viewer_classic_queue_drop_oldest_locked(viewer);

    /* After dropping, mark viewer for full refresh to resync */
    viewer->needs_full_refresh = TRUE;
    viewer->full_refresh_deadline_ts =
        platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;
  }

  viewer->classic_queue[viewer->classic_queue_tail] = event;
  viewer->classic_queue_tail =
      (viewer->classic_queue_tail + 1) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  viewer->classic_queue_count++;
  viewer->bitmap_updates_queued++;

  /* Signal the viewer thread that a new event is available */
  if (viewer->classic_event)
    SetEvent(viewer->classic_event);

  return TRUE;
}

/* Dequeue the oldest event from the classic queue. Caller must hold send_lock.
 * Returns NULL if queue is empty. Caller must free the returned event. */
static ViewerClassicEvent *viewer_classic_dequeue_locked(Viewer *viewer) {
  ViewerClassicEvent *event = NULL;

  if (!viewer || (viewer->classic_queue_count == 0))
    return NULL;

  event = viewer->classic_queue[viewer->classic_queue_head];
  viewer->classic_queue[viewer->classic_queue_head] = NULL;
  viewer->classic_queue_head =
      (viewer->classic_queue_head + 1) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  viewer->classic_queue_count--;

  return event;
}

/* ---- SurfaceBits queue: queue operations ---- */

/* Drop the oldest entry from the SurfaceBits queue. Caller must hold send_lock.
 */
static void viewer_surface_bits_queue_drop_oldest_locked(Viewer *viewer) {
  ViewerSurfaceBitsEvent *oldest = NULL;

  if (viewer->surface_bits_queue_count == 0)
    return;

  oldest = viewer->surface_bits_queue[viewer->surface_bits_queue_head];
  viewer->surface_bits_queue[viewer->surface_bits_queue_head] = NULL;
  viewer->surface_bits_queue_head = (viewer->surface_bits_queue_head + 1) %
                                    VIEWER_SURFACE_BITS_QUEUE_CAPACITY;
  viewer->surface_bits_queue_count--;

  viewer_surface_bits_event_free(oldest);
  viewer->surface_bits_queue_dropped++;
}

static void viewer_surface_bits_queue_clear_locked(Viewer *viewer) {
  while (viewer->surface_bits_queue_count > 0)
    viewer_surface_bits_queue_drop_oldest_locked(viewer);
}

/* Enqueue a pre-built SurfaceBits event into the viewer's queue.
 * Caller must hold send_lock. Returns TRUE on success. */
static BOOL
viewer_surface_bits_enqueue_event_locked(Viewer *viewer,
                                         ViewerSurfaceBitsEvent *event) {
  if (!viewer || !event)
    return FALSE;

  /* If queue is full, drop oldest entries to make room */
  while (viewer->surface_bits_queue_count >=
         VIEWER_SURFACE_BITS_QUEUE_CAPACITY) {
    WLog_WARN(TAG,
              "Viewer %u SurfaceBits queue full (%u entries), dropping oldest",
              viewer->id, viewer->surface_bits_queue_count);
    viewer_surface_bits_queue_drop_oldest_locked(viewer);

    /* Note: do NOT set needs_full_refresh here. SurfaceBits ARE the
     * refresh data — setting needs_full_refresh would cause the pump
     * to drop all queued SurfaceBits, creating a deadlock. */
  }

  viewer->surface_bits_queue[viewer->surface_bits_queue_tail] = event;
  viewer->surface_bits_queue_tail = (viewer->surface_bits_queue_tail + 1) %
                                    VIEWER_SURFACE_BITS_QUEUE_CAPACITY;
  viewer->surface_bits_queue_count++;
  viewer->surface_bits_updates_queued++;

  /* Signal the viewer thread that a new event is available */
  if (viewer->classic_event)
    SetEvent(viewer->classic_event);

  return TRUE;
}

/* Dequeue the oldest event from the SurfaceBits queue. Caller must hold
 * send_lock. Returns NULL if queue is empty. Caller must free the returned
 * event. */
static ViewerSurfaceBitsEvent *
viewer_surface_bits_dequeue_locked(Viewer *viewer) {
  ViewerSurfaceBitsEvent *event = NULL;

  if (!viewer || (viewer->surface_bits_queue_count == 0))
    return NULL;

  event = viewer->surface_bits_queue[viewer->surface_bits_queue_head];
  viewer->surface_bits_queue[viewer->surface_bits_queue_head] = NULL;
  viewer->surface_bits_queue_head = (viewer->surface_bits_queue_head + 1) %
                                    VIEWER_SURFACE_BITS_QUEUE_CAPACITY;
  viewer->surface_bits_queue_count--;

  return event;
}

static const char *viewer_join_state_name(ViewerJoinState state) {
  switch (state) {
  case VIEWER_JOIN_STATE_NONE:
    return "NONE";
  case VIEWER_JOIN_STATE_PENDING:
    return "PENDING";
  case VIEWER_JOIN_STATE_WAIT_NEXT_SAFE_FRAME:
    return "WAIT_NEXT_SAFE_FRAME";
  case VIEWER_JOIN_STATE_REPLAYING:
    return "REPLAYING";
  case VIEWER_JOIN_STATE_WAIT_REPLAY_ACK:
    return "WAIT_REPLAY_ACK";
  case VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH:
    return "WAIT_BACKEND_REFRESH";
  case VIEWER_JOIN_STATE_LIVE:
    return "LIVE";
  case VIEWER_JOIN_STATE_REJECTED:
    return "REJECTED";
  default:
    return "UNKNOWN";
  }
}

static const char *viewer_join_strategy_name(ViewerJoinStrategy strategy) {
  switch (strategy) {
  case VIEWER_JOIN_STRATEGY_NONE:
    return "NONE";
  case VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME:
    return "REPLAY_SAFE_FRAME";
  case VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME:
    return "WAIT_NEXT_SAFE_FRAME";
  case VIEWER_JOIN_STRATEGY_BACKEND_REFRESH:
    return "BACKEND_REFRESH";
  case VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK:
    return "CLASSIC_FALLBACK";
  case VIEWER_JOIN_STRATEGY_REJECT:
    return "REJECT";
  default:
    return "UNKNOWN";
  }
}

static void viewer_gfx_event_unref(ViewerGfxEvent *event);
static BOOL viewer_send_gfx_event(Viewer *viewer, ViewerGfxEvent *event);

static BOOL viewer_gfx_is_frame_event(ViewerGfxEventType type) {
  return (type == VIEWER_GFX_EVENT_START_FRAME) ||
         (type == VIEWER_GFX_EVENT_SURFACE_COMMAND) ||
         (type == VIEWER_GFX_EVENT_END_FRAME);
}

static void
viewer_gfx_reset_graphics_pdu_reset(RDPGFX_RESET_GRAPHICS_PDU *reset_graphics) {
  if (!reset_graphics)
    return;

  free(reset_graphics->monitorDefArray);
  memset(reset_graphics, 0, sizeof(*reset_graphics));
}

static BOOL
viewer_gfx_reset_graphics_pdu_copy(RDPGFX_RESET_GRAPHICS_PDU *destination,
                                   const RDPGFX_RESET_GRAPHICS_PDU *source) {
  size_t monitor_bytes = 0;

  if (!destination || !source)
    return FALSE;

  memset(destination, 0, sizeof(*destination));
  *destination = *source;
  destination->monitorDefArray = NULL;

  if (source->monitorCount == 0)
    return TRUE;

  monitor_bytes = sizeof(MONITOR_DEF) * source->monitorCount;
  destination->monitorDefArray = (MONITOR_DEF *)malloc(monitor_bytes);
  if (!destination->monitorDefArray)
    return FALSE;

  memcpy(destination->monitorDefArray, source->monitorDefArray, monitor_bytes);
  return TRUE;
}

static ViewerGfxEvent *viewer_gfx_event_alloc(ViewerGfxEventType type) {
  ViewerGfxEvent *event = (ViewerGfxEvent *)calloc(1, sizeof(ViewerGfxEvent));
  if (!event)
    return NULL;

  event->refcount = 1;
  event->type = type;
  return event;
}

static ViewerGfxEvent *viewer_gfx_event_new_reset_graphics(
    const RDPGFX_RESET_GRAPHICS_PDU *reset_graphics) {
  ViewerGfxEvent *event =
      viewer_gfx_event_alloc(VIEWER_GFX_EVENT_RESET_GRAPHICS);
  if (!event)
    return NULL;

  if (!viewer_gfx_reset_graphics_pdu_copy(&event->u.reset_graphics,
                                          reset_graphics)) {
    free(event);
    return NULL;
  }

  return event;
}

static ViewerGfxEvent *
viewer_gfx_event_new_surface_command(const RDPGFX_SURFACE_COMMAND *cmd) {
  ViewerGfxEvent *event =
      viewer_gfx_event_alloc(VIEWER_GFX_EVENT_SURFACE_COMMAND);
  if (!event)
    return NULL;

  event->u.surface_command = *cmd;
  event->u.surface_command.data = NULL;
  event->u.surface_command.extra = NULL;

  if (cmd->length > 0) {
    event->u.surface_command.data = (BYTE *)malloc(cmd->length);
    if (!event->u.surface_command.data) {
      free(event);
      return NULL;
    }
    memcpy(event->u.surface_command.data, cmd->data, cmd->length);
  }

  return event;
}

static ViewerGfxEvent *viewer_gfx_event_new_simple(ViewerGfxEventType type,
                                                   const void *payload,
                                                   size_t payload_size) {
  ViewerGfxEvent *event = viewer_gfx_event_alloc(type);
  if (!event)
    return NULL;

  memcpy(&event->u, payload, payload_size);
  return event;
}

static void viewer_gfx_event_ref(ViewerGfxEvent *event) {
  if (event)
    InterlockedIncrement(&event->refcount);
}

static void viewer_gfx_event_unref(ViewerGfxEvent *event) {
  if (!event)
    return;

  if (InterlockedDecrement(&event->refcount) != 0)
    return;

  switch (event->type) {
  case VIEWER_GFX_EVENT_RESET_GRAPHICS:
    viewer_gfx_reset_graphics_pdu_reset(&event->u.reset_graphics);
    break;

  case VIEWER_GFX_EVENT_SURFACE_COMMAND:
    free(event->u.surface_command.data);
    event->u.surface_command.data = NULL;
    break;

  default:
    break;
  }

  free(event);
}

static UINT64 viewer_gfx_event_payload_bytes(const ViewerGfxEvent *event) {
  if (!event)
    return 0;

  if (event->type == VIEWER_GFX_EVENT_SURFACE_COMMAND)
    return event->u.surface_command.length;

  return 0;
}

static void viewer_gfx_log_refcount_change(const char *owner, UINT32 frame_id,
                                           LONG old_count, LONG new_count) {
  WLog_DBG(TAG, "Frame %" PRIu32 " refcount owner=%s %ld -> %ld", frame_id,
           owner ? owner : "unknown", old_count, new_count);
}

static ViewerGfxCompleteFrame *viewer_gfx_complete_frame_new(void) {
  ViewerGfxCompleteFrame *frame =
      (ViewerGfxCompleteFrame *)calloc(1, sizeof(*frame));

  if (!frame)
    return NULL;

  frame->refcount = 1;
  frame->replay_safe = TRUE;
  frame->events = (ViewerGfxEvent **)calloc(VIEWER_GFX_MAX_FRAME_EVENTS,
                                            sizeof(ViewerGfxEvent *));
  if (!frame->events) {
    free(frame);
    return NULL;
  }

  return frame;
}

static void viewer_gfx_complete_frame_ref(ViewerGfxCompleteFrame *frame) {
  LONG old_count = 0;
  LONG new_count = 0;

  if (!frame)
    return;

  old_count = InterlockedIncrement(&frame->refcount) - 1;
  new_count = old_count + 1;
  viewer_gfx_log_refcount_change("frame-ref", frame->frame_id, old_count,
                                 new_count);
}

static void viewer_gfx_complete_frame_unref(ViewerGfxCompleteFrame *frame) {
  LONG new_count = 0;
  LONG old_count = 0;
  UINT32 i = 0;

  if (!frame)
    return;

  new_count = InterlockedDecrement(&frame->refcount);
  old_count = new_count + 1;
  viewer_gfx_log_refcount_change("frame-unref", frame->frame_id, old_count,
                                 new_count);
  if (new_count != 0)
    return;

  WLog_INFO(TAG, "Freeing complete frame object for frameId=%" PRIu32,
            frame->frame_id);
  for (i = 0; i < frame->event_count; i++)
    viewer_gfx_event_unref(frame->events[i]);
  free(frame->events);
  free(frame);
}

static BOOL
viewer_gfx_complete_frame_append_event(ViewerGfxCompleteFrame *frame,
                                       ViewerGfxEvent *event) {
  if (!frame || !event)
    return FALSE;

  if (frame->event_count >= VIEWER_GFX_MAX_FRAME_EVENTS) {
    WLog_ERR(TAG,
             "Frame %" PRIu32
             " exceeded max events=%u; dropping incomplete capture",
             frame->frame_id, VIEWER_GFX_MAX_FRAME_EVENTS);
    return FALSE;
  }

  viewer_gfx_event_ref(event);
  frame->events[frame->event_count++] = event;
  return TRUE;
}

static void
viewer_gfx_log_frame_buffer_state_locked(const ViewerGfxFrameBuffer *buffer,
                                         const char *reason) {
  UINT32 capture_frame_id = 0;

  if (!buffer)
    return;

  if (buffer->capture_frame)
    capture_frame_id = buffer->capture_frame->frame_id;

  WLog_INFO(TAG,
            "Frame ring state reason=%s filled=%" PRIu32 "/%u nextSlot=%" PRIu32
            " oldest=%" PRIu32 " newest=%" PRIu32 " capture=%" PRIu32,
            reason ? reason : "unknown", buffer->filled_slots,
            VIEWER_GFX_FRAME_RING_CAPACITY, buffer->next_slot,
            buffer->oldest_frame_id, buffer->newest_frame_id, capture_frame_id);
}

static void
viewer_gfx_frame_buffer_recompute_ids_locked(ViewerGfxFrameBuffer *buffer) {
  UINT32 i = 0;
  UINT32 oldest = 0;
  UINT32 newest = 0;
  BOOL have_frame = FALSE;

  if (!buffer)
    return;

  buffer->filled_slots = 0;
  for (i = 0; i < VIEWER_GFX_FRAME_RING_CAPACITY; i++) {
    ViewerGfxCompleteFrame *frame = buffer->slots[i];
    if (!frame)
      continue;

    buffer->filled_slots++;
    if (!have_frame) {
      oldest = frame->frame_id;
      newest = frame->frame_id;
      have_frame = TRUE;
      continue;
    }

    if (frame->frame_id < oldest)
      oldest = frame->frame_id;
    if (frame->frame_id > newest)
      newest = frame->frame_id;
  }

  buffer->oldest_frame_id = have_frame ? oldest : 0;
  buffer->newest_frame_id = have_frame ? newest : 0;
}

static void viewer_gfx_frame_buffer_init(ViewerGfxFrameBuffer *buffer) {
  if (!buffer || buffer->initialized)
    return;

  InitializeCriticalSection(&buffer->lock);
  buffer->initialized = TRUE;
}

static void viewer_gfx_frame_buffer_reset_locked(ViewerGfxFrameBuffer *buffer) {
  UINT32 i = 0;
  ViewerGfxCompleteFrame *saved_golden = NULL;

  if (!buffer)
    return;

  if (buffer->capture_frame) {
    WLog_WARN(TAG,
              "Discarding incomplete frame frameId=%" PRIu32
              " during frame ring reset",
              buffer->capture_frame->frame_id);
    viewer_gfx_complete_frame_unref(buffer->capture_frame);
    buffer->capture_frame = NULL;
  }

  /* Preserve the golden CAPROGRESSIVE frame in slot 0 across the reset.
   * ResetGraphics clears the graphics pipeline state, but the pixel
   * data in a CAPROGRESSIVE frame remains a valid full-screen baseline
   * for late-joining viewers. Without this, every ResetGraphics (which
   * arrives on backend refresh or session reconfiguration) destroys
   * the replay baseline we carefully built up. */
  if (buffer->slots[0] && buffer->slots[0]->complete &&
      (buffer->slots[0]->codec_mask & (1ULL << RDPGFX_CODECID_CAPROGRESSIVE))) {
    saved_golden = buffer->slots[0];
    buffer->slots[0] = NULL;
    WLog_INFO(TAG,
              "Preserving golden CAPROGRESSIVE frame %" PRIu32
              " across ring reset",
              saved_golden->frame_id);
  }

  for (i = 0; i < VIEWER_GFX_FRAME_RING_CAPACITY; i++) {
    if (!buffer->slots[i])
      continue;

    WLog_INFO(TAG, "Evicting frame %" PRIu32 " from ring slot %" PRIu32,
              buffer->slots[i]->frame_id, i);
    viewer_gfx_complete_frame_unref(buffer->slots[i]);
    buffer->slots[i] = NULL;
  }

  /* Restore the saved golden frame to slot 0 */
  if (saved_golden) {
    buffer->slots[0] = saved_golden;
    saved_golden = NULL;
  }

  buffer->next_slot = (buffer->slots[0] ? 1 : 0);
  buffer->filled_slots = (buffer->slots[0] ? 1 : 0);
  buffer->oldest_frame_id = 0;
  buffer->newest_frame_id = 0;
  viewer_gfx_log_frame_buffer_state_locked(buffer, "reset");
}

static void viewer_gfx_frame_buffer_uninit(ViewerGfxFrameBuffer *buffer) {
  if (!buffer || !buffer->initialized)
    return;

  viewer_gfx_frame_buffer_reset_locked(buffer);
  DeleteCriticalSection(&buffer->lock);
  memset(buffer, 0, sizeof(*buffer));
}

static BOOL viewer_gfx_frame_buffer_begin_frame_locked(
    ViewerGfxFrameBuffer *buffer, const RDPGFX_START_FRAME_PDU *start_frame,
    ViewerGfxEvent *event) {
  ViewerGfxCompleteFrame *frame = NULL;

  if (!buffer || !start_frame || !event)
    return FALSE;

  if (buffer->capture_frame) {
    WLog_WARN(TAG,
              "Discarding incomplete frame frameId=%" PRIu32
              " due to new StartFrame frameId=%" PRIu32,
              buffer->capture_frame->frame_id, start_frame->frameId);
    viewer_gfx_complete_frame_unref(buffer->capture_frame);
    buffer->capture_frame = NULL;
  }

  WLog_INFO(TAG, "Allocating frame capture buffer for frameId=%" PRIu32,
            start_frame->frameId);
  frame = viewer_gfx_complete_frame_new();
  if (!frame) {
    WLog_ERR(TAG,
             "Failed to allocate frame capture buffer for frameId=%" PRIu32,
             start_frame->frameId);
    return FALSE;
  }

  frame->frame_id = start_frame->frameId;
  frame->capture_started_ts = platform_get_timestamp_ms();
  WLog_INFO(TAG, "Allocated complete frame object for frameId=%" PRIu32,
            frame->frame_id);

  if (!viewer_gfx_complete_frame_append_event(frame, event)) {
    viewer_gfx_complete_frame_unref(frame);
    WLog_ERR(TAG,
             "Frame %" PRIu32 " capture failed; live forwarding continues but "
             "replay buffer unavailable",
             start_frame->frameId);
    return FALSE;
  }

  buffer->capture_frame = frame;
  return TRUE;
}

static BOOL viewer_gfx_frame_buffer_append_surface_command_locked(
    ViewerGfxFrameBuffer *buffer, const RDPGFX_SURFACE_COMMAND *cmd,
    ViewerGfxEvent *event) {
  ViewerGfxCompleteFrame *frame = NULL;
  ViewerGfxCodecReplayPolicy replay_policy = VIEWER_GFX_CODEC_REPLAY_UNSAFE;

  if (!buffer || !cmd || !event)
    return FALSE;

  frame = buffer->capture_frame;
  if (!frame)
    return FALSE;

  if (!viewer_gfx_complete_frame_append_event(frame, event)) {
    WLog_ERR(TAG,
             "Frame %" PRIu32 " capture failed; live forwarding continues but "
             "replay buffer unavailable",
             frame->frame_id);
    viewer_gfx_complete_frame_unref(frame);
    buffer->capture_frame = NULL;
    return FALSE;
  }

  frame->surface_command_count++;
  frame->total_payload_bytes += viewer_gfx_event_payload_bytes(event);
  replay_policy = viewer_gfx_codec_replay_policy(cmd->codecId, NULL);
  if (cmd->codecId < 64)
    frame->codec_mask |= (1ULL << cmd->codecId);
  else
    frame->codec_mask |= (1ULL << 63);
  if (replay_policy != VIEWER_GFX_CODEC_REPLAY_SAFE)
    frame->replay_safe = FALSE;
  return TRUE;
}

static BOOL
viewer_gfx_frame_buffer_end_frame_locked(ViewerGfxFrameBuffer *buffer,
                                         const RDPGFX_END_FRAME_PDU *end_frame,
                                         ViewerGfxEvent *event) {
  ViewerGfxCompleteFrame *frame = NULL;
  UINT32 slot_index = 0;

  if (!buffer || !end_frame || !event)
    return FALSE;

  frame = buffer->capture_frame;
  if (!frame)
    return FALSE;

  if (frame->frame_id != end_frame->frameId) {
    WLog_ERR(TAG,
             "Buffered frame %" PRIu32
             " invalid or incomplete; evicting and using fallback refresh",
             frame->frame_id);
    viewer_gfx_complete_frame_unref(frame);
    buffer->capture_frame = NULL;
    return FALSE;
  }

  if (!viewer_gfx_complete_frame_append_event(frame, event)) {
    WLog_ERR(TAG,
             "Frame %" PRIu32 " capture failed; live forwarding continues but "
             "replay buffer unavailable",
             frame->frame_id);
    viewer_gfx_complete_frame_unref(frame);
    buffer->capture_frame = NULL;
    return FALSE;
  }

  frame->capture_completed_ts = platform_get_timestamp_ms();
  frame->complete = TRUE;
  buffer->capture_frame = NULL;

  /* When the ring is full, find the WORST frame to evict.
   *
   * CAPROGRESSIVE (codecId=9) frames are full-screen updates always
   * preferred over ClearCodec (codecId=8) tile-based partial updates.
   *
   * If the golden slot (0) has a CAPROGRESSIVE frame and there's at
   * least one non-CAPROGRESSIVE frame in another slot, pick the worst
   * non-CAPROGRESSIVE slot instead of evicting the golden frame. */
  if (buffer->filled_slots >= VIEWER_GFX_FRAME_RING_CAPACITY) {
    UINT32 evict_slot = 0;
    int evict_score = INT_MIN;
    UINT32 best_non_gfx_slot = UINT32_MAX;
    int best_non_gfx_score = INT_MIN;
    BOOL new_is_worse_than_all = TRUE;
    BOOL golden_is_gfx =
        buffer->slots[0] && buffer->slots[0]->complete &&
        (buffer->slots[0]->codec_mask & (1ULL << RDPGFX_CODECID_CAPROGRESSIVE));
    UINT32 i;

    for (i = 0; i < VIEWER_GFX_FRAME_RING_CAPACITY; i++) {
      ViewerGfxCompleteFrame *f = buffer->slots[i];
      int f_score;
      if (!f || !f->complete)
        continue;

      f_score = (int)f->surface_command_count;
      if (!(f->codec_mask & (1ULL << RDPGFX_CODECID_CAPROGRESSIVE)))
        f_score += 10000;

      if (f_score > evict_score) {
        evict_score = f_score;
        evict_slot = i;
      }

      /* Track best non-CAPROGRESSIVE frame as golden protector */
      if (f_score >= 10000 && f_score > best_non_gfx_score) {
        best_non_gfx_score = f_score;
        best_non_gfx_slot = i;
      }

      {
        int new_score = (int)frame->surface_command_count;
        if (!(frame->codec_mask & (1ULL << RDPGFX_CODECID_CAPROGRESSIVE)))
          new_score += 10000;
        if (new_score <= f_score)
          new_is_worse_than_all = FALSE;
      }
    }

    /* If golden slot has CAPROGRESSIVE and would be evicted, redirect
     * eviction to the worst non-CAPROGRESSIVE slot instead. */
    if (golden_is_gfx && (evict_slot == 0) &&
        (best_non_gfx_slot != UINT32_MAX)) {
      evict_slot = best_non_gfx_slot;
      evict_score = best_non_gfx_score;
      WLog_INFO(TAG,
                "Protecting golden CAPROGRESSIVE frame in slot 0; evicting "
                "slot %" PRIu32 " instead",
                best_non_gfx_slot);
    }

    if (new_is_worse_than_all) {
      WLog_INFO(TAG,
                "Discarding frame %" PRIu32 " (%" PRIu32 " cmds, %" PRIu64
                " bytes): worse than all ring frames",
                frame->frame_id, frame->surface_command_count,
                frame->total_payload_bytes);
      viewer_gfx_complete_frame_unref(frame);
      viewer_gfx_frame_buffer_recompute_ids_locked(buffer);
      return TRUE;
    }

    slot_index = evict_slot;
    WLog_INFO(TAG,
              "Ring full: evicting frame %" PRIu32 " (score %d, %" PRIu32
              " cmds, %" PRIu64 " bytes) from slot %" PRIu32,
              buffer->slots[evict_slot]->frame_id, evict_score,
              buffer->slots[evict_slot]->surface_command_count,
              buffer->slots[evict_slot]->total_payload_bytes, evict_slot);
    viewer_gfx_complete_frame_unref(buffer->slots[evict_slot]);
    buffer->slots[evict_slot] = NULL;
  } else {
    slot_index = buffer->next_slot;
    buffer->next_slot = (slot_index + 1U) % VIEWER_GFX_FRAME_RING_CAPACITY;
  }

  WLog_INFO(
      TAG, "Adding frame %" PRIu32 " (%" PRIu64 " bytes) to ring slot %" PRIu32,
      frame->frame_id, frame->total_payload_bytes, slot_index);
  buffer->slots[slot_index] = frame;

  /* If the new frame is CAPROGRESSIVE, ensure it's in slot 0 (golden).
   * Swap it with whatever is in slot 0 unless slot 0 already has a
   * BETTER CAPROGRESSIVE frame (fewer commands = better baseline). */
  if (frame->codec_mask & (1ULL << RDPGFX_CODECID_CAPROGRESSIVE)) {
    UINT32 golden_slot = 0;
    if (slot_index != golden_slot) {
      ViewerGfxCompleteFrame *current_golden = buffer->slots[golden_slot];
      BOOL current_is_gfx =
          current_golden &&
          (current_golden->codec_mask & (1ULL << RDPGFX_CODECID_CAPROGRESSIVE));
      BOOL new_has_clearcodec =
          (frame->codec_mask & (1ULL << RDPGFX_CODECID_CLEARCODEC)) != 0;
      BOOL golden_has_clearcodec =
          current_golden && (current_golden->codec_mask &
                             (1ULL << RDPGFX_CODECID_CLEARCODEC)) != 0;

      /* Never replace a pure CAPROGRESSIVE golden frame with a
       * mixed frame that also contains ClearCodec tile commands.
       * A frame with ClearCodec tiles is a partial update — its
       * CAPROGRESSIVE portion may be tiny (e.g. 245 bytes) and
       * not a usable full-screen baseline. */
      if (current_is_gfx && !golden_has_clearcodec && new_has_clearcodec) {
        /* Golden is pure CAPROGRESSIVE, new is mixed — skip */
      } else {
        int new_score = (int)frame->surface_command_count;
        int golden_score = current_golden
                               ? (int)current_golden->surface_command_count +
                                     (current_is_gfx ? 0 : 10000)
                               : INT_MAX;

        if (new_score < golden_score ||
            (new_score == golden_score &&
             frame->frame_id > current_golden->frame_id)) {
          buffer->slots[golden_slot] = frame;
          buffer->slots[slot_index] = current_golden;
          WLog_INFO(
              TAG, "Swapped CAPROGRESSIVE frame %" PRIu32 " into golden slot 0",
              frame->frame_id);
        }
      }
    }
  }

  viewer_gfx_frame_buffer_recompute_ids_locked(buffer);
  viewer_gfx_log_frame_buffer_state_locked(buffer, "end-frame commit");
  return TRUE;
}

static ViewerGfxCompleteFrame *
viewer_gfx_frame_buffer_latest_locked(const ViewerGfxFrameBuffer *buffer) {
  ViewerGfxCompleteFrame *newest = NULL;
  UINT32 i = 0;

  if (!buffer || (buffer->filled_slots == 0))
    return NULL;

  for (i = 0; i < VIEWER_GFX_FRAME_RING_CAPACITY; i++) {
    ViewerGfxCompleteFrame *frame = buffer->slots[i];
    if (!frame || !frame->complete)
      continue;

    if (!newest || (frame->frame_id > newest->frame_id))
      newest = frame;
  }

  if (!newest)
    return NULL;

  viewer_gfx_complete_frame_ref(newest);
  return newest;
}

static ViewerGfxCompleteFrame *viewer_gfx_frame_buffer_latest_replayable_locked(
    const ViewerGfxFrameBuffer *buffer, UINT32 minimum_frame_id) {
  ViewerGfxCompleteFrame *best = NULL;
  UINT32 i = 0;

  if (!buffer || (buffer->filled_slots == 0))
    return NULL;

  for (i = 0; i < VIEWER_GFX_FRAME_RING_CAPACITY; i++) {
    ViewerGfxCompleteFrame *frame = buffer->slots[i];

    if (!frame || !frame->complete || !frame->replay_safe ||
        (frame->surface_command_count == 0) ||
        (frame->frame_id < minimum_frame_id))
      continue;

    /* Avoid replaying tiny delta-only frames as late-join baselines.
     * They may be protocol-valid but still render as black/stale screens
     * because mstsc has no meaningful self-contained pixels to present. */
    if ((frame->surface_command_count <
         VIEWER_GFX_MIN_REPLAY_BASELINE_COMMANDS) &&
        (frame->total_payload_bytes < VIEWER_GFX_MIN_REPLAY_BASELINE_BYTES))
      continue;

    /* Prefer frames that use CAPROGRESSIVE (codecId=9) over ClearCodec
     * (codecId=8). CAPROGRESSIVE frames are always full-screen updates
     * (even a single command covers the entire surface). ClearCodec
     * frames are tile-based partial updates — a "1 command" ClearCodec
     * frame updates just ONE 64x64 tile, leaving the rest black.
     *
     * Ranking: CAPROGRESSIVE > ClearCodec. Within same codec type:
     * fewer commands preferred, tie-break by larger payload. */
    {
      BOOL frame_is_gfx =
          (frame->codec_mask & (1ULL << RDPGFX_CODECID_CAPROGRESSIVE)) != 0;
      BOOL best_is_gfx =
          best &&
          ((best->codec_mask & (1ULL << RDPGFX_CODECID_CAPROGRESSIVE)) != 0);
      BOOL frame_has_cc =
          (frame->codec_mask & (1ULL << RDPGFX_CODECID_CLEARCODEC)) != 0;
      BOOL best_has_cc =
          best &&
          ((best->codec_mask & (1ULL << RDPGFX_CODECID_CLEARCODEC)) != 0);
      int better = 0;
      if (frame_is_gfx && !best_is_gfx)
        better = 1;
      else if (!frame_is_gfx && best_is_gfx)
        better = -1;
      /* When both have CAPROGRESSIVE, prefer pure CAPROGRESSIVE
       * over mixed (CAPROGRESSIVE + ClearCodec). A mixed frame
       * may have only a tiny CAPROGRESSIVE supplement (e.g., 245
       * bytes) alongside many ClearCodec tile commands — not a
       * usable full-screen baseline. */
      else if (frame_is_gfx && best_is_gfx && !frame_has_cc && best_has_cc)
        better = 1;
      else if (frame_is_gfx && best_is_gfx && frame_has_cc && !best_has_cc)
        better = -1;
      else if (frame->surface_command_count < best->surface_command_count)
        better = 1;
      else if (frame->surface_command_count == best->surface_command_count &&
               frame->total_payload_bytes > best->total_payload_bytes)
        better = 1;
      if (!best || (better > 0))
        best = frame;
    }
  }

  if (!best)
    return NULL;

  viewer_gfx_complete_frame_ref(best);
  return best;
}

static void viewer_gfx_set_join_state_locked(Viewer *viewer,
                                             ViewerJoinState state,
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
    WLog_INFO(
        TAG,
        "Viewer %u join transition %s/%s -> %s/%s reason=%s "
        "targetFrame=%" PRIu32 " refreshGeneration=%" PRIu64,
        viewer->id, viewer_join_state_name(old_state),
        viewer_join_strategy_name(old_strategy), viewer_join_state_name(state),
        viewer_join_strategy_name(strategy), reason ? reason : "unspecified",
        viewer->gfx.join_target_frame_id, viewer->gfx.join_refresh_generation);
  }
}

static void viewer_gfx_begin_join_locked(Viewer *viewer, UINT64 now,
                                         const char *reason) {
  if (!viewer)
    return;

  viewer->gfx.join_target_frame_id = 0;
  viewer->gfx.join_refresh_generation = 0;
  viewer->gfx.join_start_ts = now;
  viewer_gfx_set_join_state_locked(viewer, VIEWER_JOIN_STATE_PENDING,
                                   VIEWER_JOIN_STRATEGY_NONE, reason);
}

static void viewer_gfx_finish_late_join_locked(Viewer *viewer,
                                               const char *reason) {
  if (!viewer)
    return;

  viewer->gfx.join_refresh_generation = 0;
  viewer->gfx.last_activated_ts = platform_get_timestamp_ms();
  /* Clear any partially-enqueued events that arrived during the join gate.
   * Frame events (StartFrame, SurfaceCommands, EndFrame) arrive from the
   * backend asynchronously. If a StartFrame was skipped but SurfaceCommands
   * were enqueued after the LIVE transition, the viewer would receive a
   * partial frame (SurfaceCommands without StartFrame) which violates the
   * RDPEGFX protocol and causes mstsc to disconnect. Clearing the queue
   * ensures the viewer starts clean with the next complete frame. */
  viewer_gfx_queue_clear_locked(&viewer->gfx);
  viewer_gfx_set_join_state_locked(viewer, VIEWER_JOIN_STATE_LIVE,
                                   VIEWER_JOIN_STRATEGY_NONE, reason);
}

static BOOL
viewer_gfx_viewer_accepts_live_locked(const ViewerGraphicsContext *gfx) {
  return gfx && (gfx->join_state == VIEWER_JOIN_STATE_LIVE);
}

static void viewer_gfx_queue_pop_head_locked(ViewerGraphicsContext *gfx) {
  ViewerGfxEvent *event = NULL;

  if (!gfx || (gfx->queue_count == 0))
    return;

  event = gfx->queue[gfx->queue_head];
  gfx->queue[gfx->queue_head] = NULL;
  gfx->queue_head = (gfx->queue_head + 1U) % VIEWER_GFX_QUEUE_CAPACITY;
  gfx->queue_count--;

  if (event && (event->type == VIEWER_GFX_EVENT_END_FRAME) &&
      (gfx->pending_frame_count > 0))
    gfx->pending_frame_count--;

  viewer_gfx_event_unref(event);
}

static BOOL
viewer_gfx_drop_oldest_complete_frame_locked(ViewerGraphicsContext *gfx) {
  UINT32 index = 0;
  UINT32 scanned = 0;

  if (!gfx || (gfx->queue_count == 0))
    return FALSE;

  index = gfx->queue_head;
  while (scanned < gfx->queue_count) {
    ViewerGfxEvent *event = gfx->queue[index];
    if (!event)
      return FALSE;
    if (!viewer_gfx_is_frame_event(event->type))
      return FALSE;
    if (event->type == VIEWER_GFX_EVENT_END_FRAME)
      break;

    index = (index + 1U) % VIEWER_GFX_QUEUE_CAPACITY;
    scanned++;
  }

  if (scanned >= gfx->queue_count)
    return FALSE;

  do {
    ViewerGfxEvent *event = gfx->queue[gfx->queue_head];
    BOOL done = event && (event->type == VIEWER_GFX_EVENT_END_FRAME);
    viewer_gfx_queue_pop_head_locked(gfx);
    if (done)
      break;
  } while (gfx->queue_count > 0);

  return TRUE;
}

static BOOL viewer_gfx_enqueue_locked(ViewerGraphicsContext *gfx,
                                      ViewerGfxEvent *event) {
  BOOL frame_event = FALSE;

  if (!gfx || !event)
    return FALSE;

  frame_event = viewer_gfx_is_frame_event(event->type);
  while ((gfx->queue_count >= VIEWER_GFX_QUEUE_CAPACITY) ||
         (frame_event &&
          (gfx->pending_frame_count >= VIEWER_GFX_MAX_PENDING_FRAMES))) {
    if (!viewer_gfx_drop_oldest_complete_frame_locked(gfx))
      return FALSE;
  }

  gfx->queue[gfx->queue_tail] = event;
  gfx->queue_tail = (gfx->queue_tail + 1U) % VIEWER_GFX_QUEUE_CAPACITY;
  gfx->queue_count++;

  if (event->type == VIEWER_GFX_EVENT_END_FRAME)
    gfx->pending_frame_count++;

  return TRUE;
}

static ViewerGfxEvent *viewer_gfx_dequeue_locked(ViewerGraphicsContext *gfx) {
  ViewerGfxEvent *event = NULL;

  if (!gfx || (gfx->queue_count == 0))
    return NULL;

  event = gfx->queue[gfx->queue_head];
  gfx->queue[gfx->queue_head] = NULL;
  gfx->queue_head = (gfx->queue_head + 1U) % VIEWER_GFX_QUEUE_CAPACITY;
  gfx->queue_count--;

  if (event && (event->type == VIEWER_GFX_EVENT_END_FRAME) &&
      (gfx->pending_frame_count > 0))
    gfx->pending_frame_count--;

  return event;
}

static void viewer_gfx_queue_clear_locked(ViewerGraphicsContext *gfx) {
  while (gfx && (gfx->queue_count > 0)) {
    ViewerGfxEvent *event = viewer_gfx_dequeue_locked(gfx);
    viewer_gfx_event_unref(event);
  }
}

static void viewer_disable_rdpgfx_locked(Viewer *viewer) {
  if (!viewer)
    return;

  viewer_gfx_queue_clear_locked(&viewer->gfx);
  viewer->gfx.ready = FALSE;
  viewer->gfx.use_rdpgfx = FALSE;
  viewer->gfx.caps_ready = FALSE;
  viewer->gfx.rdpgfx_temporarily_disabled = TRUE;
  viewer->gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK;
}

static void viewer_gfx_publisher_state_init(ViewerGfxPublisherState *gfx) {
  if (!gfx || gfx->initialized)
    return;

  InitializeCriticalSection(&gfx->lock);
  viewer_gfx_frame_buffer_init(&gfx->frame_buffer);
  gfx->initialized = TRUE;
  WLog_INFO(TAG, "Initialized shared RDPEGFX frame ring capacity=%u",
            VIEWER_GFX_FRAME_RING_CAPACITY);
}

static void
viewer_gfx_publisher_state_reset_locked(ViewerGfxPublisherState *gfx) {
  ViewerGraphicsSurfaceState saved_surfaces[VIEWER_GFX_MAX_ACTIVE_SURFACES];

  if (!gfx)
    return;

  WLog_INFO(TAG, "Resetting publisher state: preserving surface map, clearing "
                 "current frame and ring");
  viewer_gfx_reset_graphics_pdu_reset(&gfx->latest_reset_graphics);

  /* Preserve the active surface map across ResetGraphics. The oracle
   * identified that clearing surfaces[] breaks late-join bootstrap:
   * the surface preamble and replay frame reference surface IDs that
   * must exist in the publisher state. If the Windows VM hasn't sent
   * new CreateSurface/MapSurfaceToOutput yet, the preamble sends 0
   * surfaces and the viewer gets no renderable baseline. */
  memcpy(saved_surfaces, gfx->surfaces, sizeof(saved_surfaces));
  memset(gfx->surfaces, 0, sizeof(gfx->surfaces));
  viewer_gfx_frame_buffer_reset_locked(&gfx->frame_buffer);
  /* Restore surfaces after ring reset (which preserves golden frame) */
  memcpy(gfx->surfaces, saved_surfaces, sizeof(saved_surfaces));

  gfx->has_latest_reset_graphics = FALSE;
  gfx->in_frame = FALSE;
  gfx->current_frame_id = 0;
}

static void viewer_gfx_publisher_state_uninit(ViewerGfxPublisherState *gfx) {
  if (!gfx || !gfx->initialized)
    return;

  EnterCriticalSection(&gfx->lock);
  viewer_gfx_publisher_state_reset_locked(gfx);
  LeaveCriticalSection(&gfx->lock);
  viewer_gfx_frame_buffer_uninit(&gfx->frame_buffer);
  DeleteCriticalSection(&gfx->lock);
  memset(gfx, 0, sizeof(*gfx));
}

static ViewerGraphicsSurfaceState *
viewer_gfx_find_surface_locked(ViewerGfxPublisherState *gfx,
                               UINT16 surface_id) {
  UINT32 i = 0;

  if (!gfx)
    return NULL;

  for (i = 0; i < VIEWER_GFX_MAX_ACTIVE_SURFACES; i++) {
    ViewerGraphicsSurfaceState *surface = &gfx->surfaces[i];
    if (surface->in_use && (surface->create_surface.surfaceId == surface_id))
      return surface;
  }

  return NULL;
}

static ViewerGraphicsSurfaceState *
viewer_gfx_upsert_surface_locked(ViewerGfxPublisherState *gfx,
                                 UINT16 surface_id) {
  ViewerGraphicsSurfaceState *empty = NULL;
  UINT32 i = 0;

  if (!gfx)
    return NULL;

  for (i = 0; i < VIEWER_GFX_MAX_ACTIVE_SURFACES; i++) {
    ViewerGraphicsSurfaceState *surface = &gfx->surfaces[i];
    if (surface->in_use && (surface->create_surface.surfaceId == surface_id))
      return surface;
    if (!surface->in_use && !empty)
      empty = surface;
  }

  if (empty) {
    memset(empty, 0, sizeof(*empty));
    empty->in_use = TRUE;
  }

  return empty;
}

static void viewer_gfx_remove_surface_locked(ViewerGfxPublisherState *gfx,
                                             UINT16 surface_id) {
  ViewerGraphicsSurfaceState *surface =
      viewer_gfx_find_surface_locked(gfx, surface_id);
  if (surface)
    memset(surface, 0, sizeof(*surface));
}

static const RDPGFX_CAPSET *
viewer_gfx_choose_caps(const ViewerGfxPublisherState *server_gfx,
                       const RDPGFX_CAPS_ADVERTISE_PDU *caps_advertise) {
  UINT16 i = 0;
  const RDPGFX_CAPSET *best = NULL;

  if (!caps_advertise)
    return NULL;

  if (server_gfx && server_gfx->canonical_caps_valid) {
    for (i = 0; i < caps_advertise->capsSetCount; i++) {
      const RDPGFX_CAPSET *caps = &caps_advertise->capsSets[i];
      if ((caps->version == server_gfx->canonical_caps.version) &&
          (caps->flags == server_gfx->canonical_caps.flags))
        return caps;
    }

    return NULL;
  }

  for (i = 0; i < caps_advertise->capsSetCount; i++) {
    const RDPGFX_CAPSET *caps = &caps_advertise->capsSets[i];
    if (!best || (caps->version > best->version))
      best = caps;
  }

  return best;
}

static BOOL
viewer_gfx_try_schedule_late_join_replay_locked(ViewerServer *server,
                                                Viewer *viewer, UINT64 now) {
  ViewerGfxCompleteFrame *latest = NULL;
  ViewerLateJoinPolicyInputs inputs = {0};
  ViewerJoinStrategy strategy = VIEWER_JOIN_STRATEGY_NONE;
  UINT32 newest_frame_id = 0;
  UINT32 minimum_frame_id = 0;
  UINT64 join_refresh_generation = 0;
  BOOL recent_unsafe_codec_activity = FALSE;
  UINT32 i = 0;

  if (!server || !viewer)
    return FALSE;

  EnterCriticalSection(&viewer->gfx.lock);
  minimum_frame_id = viewer->gfx.join_target_frame_id;
  join_refresh_generation = viewer->gfx.join_refresh_generation;
  LeaveCriticalSection(&viewer->gfx.lock);

  EnterCriticalSection(&server->gfx.lock);
  newest_frame_id = server->gfx.frame_buffer.newest_frame_id;
  if (server->gfx.in_frame) {
    UINT32 old_min = minimum_frame_id;
    /* Only skip the in-progress frame if the viewer was specifically
     * targeting it. A new viewer with minimum_frame_id=0 should be
     * able to replay completed frames that are already in the ring,
     * even when another frame is being captured concurrently. */
    if (minimum_frame_id == server->gfx.current_frame_id) {
      minimum_frame_id = server->gfx.current_frame_id + 1U;
      WLog_DBG(TAG,
               "Viewer %u try-schedule: frame %" PRIu32
               " in progress, skipping it",
               viewer->id, server->gfx.current_frame_id);
    } else if (minimum_frame_id > server->gfx.current_frame_id) {
      WLog_DBG(TAG,
               "Viewer %u try-schedule: frame %" PRIu32
               " in progress, minFid %" PRIu32 " already ahead",
               viewer->id, server->gfx.current_frame_id, minimum_frame_id);
    }
  }

  WLog_INFO(TAG,
            "Viewer %u try-schedule: ring scan minFid=%" PRIu32
            " newest=%" PRIu32 " filled=%" PRIu32 "/%u inFrame=%d",
            viewer->id, minimum_frame_id, newest_frame_id,
            server->gfx.frame_buffer.filled_slots,
            VIEWER_GFX_FRAME_RING_CAPACITY, server->gfx.in_frame ? 1 : 0);

  for (i = 0; i < VIEWER_GFX_FRAME_RING_CAPACITY; i++) {
    const ViewerGfxCompleteFrame *frame = server->gfx.frame_buffer.slots[i];

    if (!frame || !frame->complete || (frame->frame_id < minimum_frame_id) ||
        (frame->surface_command_count == 0))
      continue;

    if (!frame->replay_safe) {
      recent_unsafe_codec_activity = TRUE;
      break;
    }
  }

  latest = viewer_gfx_frame_buffer_latest_replayable_locked(
      &server->gfx.frame_buffer, minimum_frame_id);

  inputs.rdpgfx_enabled = TRUE;
  inputs.channel_opened = TRUE;
  inputs.caps_compatible = TRUE;
  inputs.backend_frame_in_progress = server->gfx.in_frame;
  inputs.complete_frame_available = (latest != NULL);
  inputs.replay_safe_codecs_only = (latest != NULL);
  LeaveCriticalSection(&server->gfx.lock);

  if (recent_unsafe_codec_activity && (join_refresh_generation == 0)) {
    if (latest) {
      WLog_INFO(TAG,
                "Viewer %u no replay-safe frames; attempting replay of latest "
                "frame %" PRIu32 " (unsafe codecs may cause partial decode)",
                viewer->id, latest->frame_id);
      viewer->gfx.join_target_frame_id = latest->frame_id;
      viewer_gfx_set_join_state_locked(
          viewer, VIEWER_JOIN_STATE_PENDING,
          VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME,
          "attempting unsafe-codec replay as bootstrap");
      LeaveCriticalSection(&viewer->gfx.lock);
      /* later steps in step_join will call viewer_gfx_replay_frame */
      return TRUE;
    }

    if (latest)
      viewer_gfx_complete_frame_unref(latest);
    return viewer_gfx_bootstrap_direct_live(
        server, viewer, now,
        "no frames in ring; bootstrapping via surface preamble only");
  }

  if (recent_unsafe_codec_activity) {
    inputs.complete_frame_available = FALSE;
    inputs.replay_safe_codecs_only = FALSE;
  }

  strategy = viewer_late_join_select_strategy(&inputs);

  EnterCriticalSection(&viewer->gfx.lock);
  if ((viewer->gfx.join_start_ts == 0) && (now > 0))
    viewer->gfx.join_start_ts = now;

  switch (strategy) {
  case VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME:
    if (latest) {
      /* When a backend refresh is in the pipeline, do NOT allow replay
       * -- the viewer must wait for the refresh to complete and receive
       * a fresh baseline via the surface preamble + live stream. Replay
       * at this stage risks selecting a delta-only frame that provides
       * an insufficient visual baseline for the fresh viewer. */
      if (viewer->gfx.join_refresh_generation != 0) {
        WLog_INFO(TAG,
                  "Viewer %u skipping replay frame %" PRIu32
                  " during backend refresh cycle (refreshGen=%" PRIu64 ")",
                  viewer->id, latest->frame_id,
                  viewer->gfx.join_refresh_generation);
        viewer_gfx_set_join_state_locked(
            viewer, VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH,
            VIEWER_JOIN_STRATEGY_BACKEND_REFRESH,
            "replay blocked during backend refresh cycle");
        break;
      }

      viewer->gfx.join_start_ts = now;
      viewer->gfx.join_target_frame_id = latest->frame_id;
      viewer_gfx_set_join_state_locked(viewer, VIEWER_JOIN_STATE_PENDING,
                                       strategy, "safe replay frame selected");
    }
    break;

  case VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME:
    /* Only set the start timestamp on first entry to this state.
     * Subsequent calls must preserve the original timestamp so the
     * timeout in step_join can actually fire. Resetting it on every
     * iteration prevents the timeout from ever triggering. */
    if (viewer->gfx.join_strategy != VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME)
      viewer->gfx.join_start_ts = now;
    viewer->gfx.join_target_frame_id = newest_frame_id + 1U;
    if (viewer->gfx.join_refresh_generation != 0) {
      viewer_gfx_set_join_state_locked(
          viewer, VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH,
          VIEWER_JOIN_STRATEGY_BACKEND_REFRESH,
          "waiting for replay-safe frame after backend refresh");
    } else {
      viewer_gfx_set_join_state_locked(
          viewer, VIEWER_JOIN_STATE_WAIT_NEXT_SAFE_FRAME, strategy,
          "waiting for next replay-safe frame");
    }
    break;

  default:
    viewer_gfx_set_join_state_locked(viewer, VIEWER_JOIN_STATE_PENDING,
                                     strategy, "strategy selected");
    break;
  }
  LeaveCriticalSection(&viewer->gfx.lock);

  if (latest)
    viewer_gfx_complete_frame_unref(latest);

  return (strategy == VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME) ||
         (strategy == VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME);
}

static BOOL viewer_gfx_replay_frame(ViewerServer *server, Viewer *viewer,
                                    ViewerGfxCompleteFrame *frame) {
  ViewerGfxEvent *reset_event = NULL;
  ViewerGfxEvent **create_events = NULL;
  ViewerGfxEvent **map_events = NULL;
  UINT32 active_surface_count = 0;
  UINT32 mapped_surface_count = 0;
  UINT32 create_index = 0;
  UINT32 map_index = 0;
  UINT32 i = 0;
  UINT64 started_ts = 0;
  UINT64 elapsed_ms = 0;
  BOOL ok = TRUE;
  BOOL has_reset = FALSE;

  if (!server || !viewer || !frame || !frame->complete ||
      (frame->event_count < 2))
    return FALSE;

  EnterCriticalSection(&server->gfx.lock);
  if (server->gfx.has_latest_reset_graphics) {
    reset_event =
        viewer_gfx_event_new_reset_graphics(&server->gfx.latest_reset_graphics);
    if (!reset_event)
      ok = FALSE;
    has_reset = TRUE;
  }

  if (ok) {
    for (i = 0; i < VIEWER_GFX_MAX_ACTIVE_SURFACES; i++) {
      if (server->gfx.surfaces[i].in_use) {
        active_surface_count++;
        if (server->gfx.surfaces[i].mapped)
          mapped_surface_count++;
      }
    }

    if (active_surface_count > 0) {
      create_events = (ViewerGfxEvent **)calloc(active_surface_count,
                                                sizeof(ViewerGfxEvent *));
      if (!create_events)
        ok = FALSE;
    }

    if (ok && (mapped_surface_count > 0)) {
      map_events = (ViewerGfxEvent **)calloc(mapped_surface_count,
                                             sizeof(ViewerGfxEvent *));
      if (!map_events)
        ok = FALSE;
    }
  }

  if (ok) {
    for (i = 0; i < VIEWER_GFX_MAX_ACTIVE_SURFACES; i++) {
      const ViewerGraphicsSurfaceState *surface = &server->gfx.surfaces[i];

      if (!surface->in_use)
        continue;

      create_events[create_index] = viewer_gfx_event_new_simple(
          VIEWER_GFX_EVENT_CREATE_SURFACE, &surface->create_surface,
          sizeof(surface->create_surface));
      if (!create_events[create_index++]) {
        ok = FALSE;
        break;
      }

      if (!surface->mapped)
        continue;

      map_events[map_index] =
          viewer_gfx_event_new_simple(VIEWER_GFX_EVENT_MAP_SURFACE_TO_OUTPUT,
                                      &surface->map_surface_to_output,
                                      sizeof(surface->map_surface_to_output));
      if (!map_events[map_index++]) {
        ok = FALSE;
        break;
      }
    }
  }

  LeaveCriticalSection(&server->gfx.lock);

  if (!ok) {
    WLog_WARN(
        TAG,
        "Viewer %u replay scheduling failed; falling back to backend refresh",
        viewer->id);
    goto cleanup;
  }

  EnterCriticalSection(&viewer->gfx.lock);
  if ((viewer->gfx.join_state != VIEWER_JOIN_STATE_PENDING) &&
      (viewer->gfx.join_state != VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH) &&
      (viewer->gfx.join_state != VIEWER_JOIN_STATE_WAIT_NEXT_SAFE_FRAME)) {
    LeaveCriticalSection(&viewer->gfx.lock);
    ok = FALSE;
    goto cleanup;
  }

  viewer_gfx_set_join_state_locked(viewer, VIEWER_JOIN_STATE_REPLAYING,
                                   viewer->gfx.join_strategy,
                                   "sending replay baseline");
  started_ts = platform_get_timestamp_ms();
  viewer->gfx.join_start_ts = started_ts;
  LeaveCriticalSection(&viewer->gfx.lock);

  WLog_INFO(TAG,
            "Replaying frame %" PRIu32 " to viewer %u: %" PRIu32
            " events, %" PRIu32 " commands",
            frame->frame_id, viewer->id, frame->event_count,
            frame->surface_command_count);
  WLog_INFO(TAG,
            "Viewer %u replay preamble: reset=%d activeSurfaces=%" PRIu32
            " mappedSurfaces=%" PRIu32,
            viewer->id, has_reset ? 1 : 0, active_surface_count,
            mapped_surface_count);

  EnterCriticalSection(&viewer->send_lock);
  if (reset_event && !viewer_send_gfx_event(viewer, reset_event))
    ok = FALSE;

  for (i = 0; ok && (i < active_surface_count); i++)
    ok = viewer_send_gfx_event(viewer, create_events[i]);

  for (i = 0; ok && (i < mapped_surface_count); i++)
    ok = viewer_send_gfx_event(viewer, map_events[i]);

  {
    UINT32 cmd_idx = 0;
    for (i = 0; ok && (i < frame->event_count); i++) {
      ViewerGfxEvent *event = frame->events[i];
      WLog_INFO(TAG,
                "Viewer %u replay %" PRIu32 "/%" PRIu32
                ": eventType=%u frameId=%" PRIu32,
                viewer->id, i + 1U, frame->event_count, event->type,
                frame->frame_id);
      if (event->type == VIEWER_GFX_EVENT_SURFACE_COMMAND) {
        cmd_idx++;
        WLog_INFO(TAG,
                  "Viewer %u replay command %" PRIu32 "/%" PRIu32
                  ": surfaceId=%" PRIu16 " codecId=%" PRIu16 " length=%" PRIu32,
                  viewer->id, cmd_idx, frame->surface_command_count,
                  event->u.surface_command.surfaceId,
                  event->u.surface_command.codecId,
                  event->u.surface_command.length);
      }
      ok = viewer_send_gfx_event(viewer, event);
      if (!ok) {
        WLog_ERR(TAG,
                 "Viewer %u replay failed at event %" PRIu32 "/%" PRIu32
                 "; disconnecting",
                 viewer->id, i + 1U, frame->event_count);
        break;
      }
    }
  }

  if (ok) {
    viewer->needs_full_refresh = FALSE;
    viewer->full_refresh_deadline_ts = 0;
  }
  LeaveCriticalSection(&viewer->send_lock);

  if (!ok) {
    viewer->stop_requested = TRUE;
    WLog_ERR(TAG, "Viewer %u replay failed: queue/send error, disconnecting",
             viewer->id);
    goto cleanup;
  }

  elapsed_ms = platform_get_timestamp_ms() - started_ts;
  EnterCriticalSection(&viewer->gfx.lock);
  viewer->gfx.join_target_frame_id = frame->frame_id;
  viewer->gfx.last_presented_timestamp = platform_get_timestamp_ms();
  viewer_gfx_finish_late_join_locked(
      viewer, "replay baseline sent; switching to live stream");
  LeaveCriticalSection(&viewer->gfx.lock);
  WLog_INFO(TAG, "Frame %" PRIu32 " replayed to viewer %u in %" PRIu64 " ms",
            frame->frame_id, viewer->id, elapsed_ms);
  WLog_INFO(TAG,
            "Viewer %u replay baseline frame=%" PRIu32
            " complete; live stream enabled",
            viewer->id, frame->frame_id);

  if (server->backend) {
    (void)backend_request_full_refresh(server->backend);
    WLog_INFO(TAG,
              "Viewer %u requested backend full refresh after replay cutover",
              viewer->id);
  }

cleanup:
  if (reset_event)
    viewer_gfx_event_unref(reset_event);
  if (create_events) {
    for (i = 0; i < active_surface_count; i++)
      viewer_gfx_event_unref(create_events[i]);
    free(create_events);
  }
  if (map_events) {
    for (i = 0; i < mapped_surface_count; i++)
      viewer_gfx_event_unref(map_events[i]);
    free(map_events);
  }

  return ok;
}

static UINT
viewer_rdpgfx_caps_advertise(RdpgfxServerContext *context,
                             const RDPGFX_CAPS_ADVERTISE_PDU *caps_advertise) {
  Viewer *viewer = context ? (Viewer *)context->custom : NULL;
  ViewerServer *server = g_viewer_server;
  RDPGFX_CAPSET caps = {0};
  RDPGFX_CAPS_CONFIRM_PDU confirm = {0};
  const RDPGFX_CAPSET *selected = NULL;
  UINT rc = CHANNEL_RC_OK;
  BOOL caps_ready_was = FALSE;
  BOOL use_rdpgfx_was = FALSE;
  ViewerGfxNegotiationOutcome outcome_was = VIEWER_GFX_NEGOTIATION_PENDING;

  if (!viewer || !server || !caps_advertise || !context->CapsConfirm)
    return ERROR_INVALID_PARAMETER;

  EnterCriticalSection(&server->gfx.lock);
  selected = viewer_gfx_choose_caps(&server->gfx, caps_advertise);
  if (selected) {
    caps = *selected;
    if (!server->gfx.canonical_caps_valid) {
      server->gfx.canonical_caps = caps;
      server->gfx.canonical_caps_valid = TRUE;
    }
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
    viewer_disable_rdpgfx_locked(viewer);
    LeaveCriticalSection(&viewer->gfx.lock);
    if (viewer->activated)
      (void)viewer_gfx_enter_classic_fallback(server, viewer,
                                              platform_get_timestamp_ms(),
                                              "incompatible RDPEGFX caps");
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
             viewer->id, viewer_join_state_name(viewer->gfx.join_state),
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
      viewer_gfx_begin_join_locked(viewer, platform_get_timestamp_ms(),
                                   "RDPEGFX caps confirmed after activation");
      WLog_INFO(TAG,
                "Viewer %u RDPEGFX caps confirmed after activation; gating "
                "live stream until replay/full refresh",
                viewer->id);
    }
  } else {
    viewer_disable_rdpgfx_locked(viewer);
  }
  LeaveCriticalSection(&viewer->gfx.lock);

  if ((rc != CHANNEL_RC_OK) && viewer->activated)
    (void)viewer_gfx_enter_classic_fallback(server, viewer,
                                            platform_get_timestamp_ms(),
                                            "RDPEGFX caps confirm failed");

  return rc;
}

static UINT viewer_rdpgfx_frame_acknowledge(
    RdpgfxServerContext *context,
    const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frame_acknowledge) {
  Viewer *viewer = context ? (Viewer *)context->custom : NULL;

  if (!viewer || !frame_acknowledge)
    return ERROR_INVALID_PARAMETER;

  EnterCriticalSection(&viewer->gfx.lock);
  viewer->gfx.last_ack_frame_id = frame_acknowledge->frameId;
  viewer->gfx.last_presented_timestamp = platform_get_timestamp_ms();
  if ((viewer->gfx.join_state == VIEWER_JOIN_STATE_WAIT_REPLAY_ACK) &&
      viewer_late_join_ack_releases_live(viewer->gfx.join_target_frame_id,
                                         viewer->gfx.last_ack_frame_id)) {
    viewer_gfx_finish_late_join_locked(viewer, "replay ack received");
  }
  LeaveCriticalSection(&viewer->gfx.lock);
  return CHANNEL_RC_OK;
}

static BOOL viewer_send_gfx_event(Viewer *viewer, ViewerGfxEvent *event) {
  UINT status = CHANNEL_RC_OK;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;

  if (!viewer || !event || !peer || !viewer->gfx.rdpgfx ||
      !viewer->gfx.use_rdpgfx)
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
    status = viewer->gfx.rdpgfx->ResetGraphics(viewer->gfx.rdpgfx,
                                               &event->u.reset_graphics);
    break;

  case VIEWER_GFX_EVENT_CREATE_SURFACE:
    status = viewer->gfx.rdpgfx->CreateSurface(viewer->gfx.rdpgfx,
                                               &event->u.create_surface);
    break;

  case VIEWER_GFX_EVENT_DELETE_SURFACE:
    status = viewer->gfx.rdpgfx->DeleteSurface(viewer->gfx.rdpgfx,
                                               &event->u.delete_surface);
    break;

  case VIEWER_GFX_EVENT_MAP_SURFACE_TO_OUTPUT:
    status = viewer->gfx.rdpgfx->MapSurfaceToOutput(
        viewer->gfx.rdpgfx, &event->u.map_surface_to_output);
    break;

  case VIEWER_GFX_EVENT_START_FRAME:
    status = viewer->gfx.rdpgfx->StartFrame(viewer->gfx.rdpgfx,
                                            &event->u.start_frame);
    break;

  case VIEWER_GFX_EVENT_SURFACE_COMMAND:
    status = viewer->gfx.rdpgfx->SurfaceCommand(viewer->gfx.rdpgfx,
                                                &event->u.surface_command);
    break;

  case VIEWER_GFX_EVENT_END_FRAME:
    status =
        viewer->gfx.rdpgfx->EndFrame(viewer->gfx.rdpgfx, &event->u.end_frame);
    break;

  case VIEWER_GFX_EVENT_DELETE_ENCODING_CONTEXT:
    status = viewer->gfx.rdpgfx->DeleteEncodingContext(
        viewer->gfx.rdpgfx, &event->u.delete_encoding_context);
    break;
  }

  if (status == CHANNEL_RC_OK) {
    viewer->packets_sent++;
    return TRUE;
  }

  viewer->packets_failed++;
  return FALSE;
}

static BOOL viewer_pump_gfx(Viewer *viewer) {
  ViewerGfxEvent *event = NULL;
  UINT32 pumped_frame_events = 0;
  UINT32 pumped_surface_commands = 0;
  BOOL gated = FALSE;

  if (!viewer)
    return FALSE;

  EnterCriticalSection(&viewer->gfx.lock);
  if (viewer->gfx.rdpgfx_temporarily_disabled || !viewer->gfx.channel_opened ||
      !viewer->gfx.caps_ready || !viewer->gfx.use_rdpgfx) {
    LeaveCriticalSection(&viewer->gfx.lock);
    return TRUE;
  }
  gated = (viewer->gfx.join_state != VIEWER_JOIN_STATE_LIVE) &&
          (viewer->gfx.queue_count > 0);
  if (gated) {
    WLog_DBG(TAG,
             "Viewer %u pump-gfx: %" PRIu32
             " events queued but join_state=%s (gated)",
             viewer->id, viewer->gfx.queue_count,
             viewer_join_state_name(viewer->gfx.join_state));
  }
  LeaveCriticalSection(&viewer->gfx.lock);

  for (;;) {
    EnterCriticalSection(&viewer->gfx.lock);
    if (viewer->gfx.join_state != VIEWER_JOIN_STATE_LIVE) {
      LeaveCriticalSection(&viewer->gfx.lock);
      break;
    }
    event = viewer_gfx_dequeue_locked(&viewer->gfx);
    LeaveCriticalSection(&viewer->gfx.lock);

    if (!event)
      break;

    EnterCriticalSection(&viewer->send_lock);
    if (!viewer_send_gfx_event(viewer, event)) {
      LeaveCriticalSection(&viewer->send_lock);
      WLog_ERR(TAG, "Viewer %u pump-gfx send failed (type=%u); disconnecting",
               viewer->id, event->type);
      viewer_gfx_event_unref(event);
      return FALSE;
    }
    LeaveCriticalSection(&viewer->send_lock);

    EnterCriticalSection(&viewer->gfx.lock);
    viewer->gfx.last_delivered_event_type = event->type;
    viewer->gfx.last_delivered_ts = platform_get_timestamp_ms();
    if (event->type == VIEWER_GFX_EVENT_END_FRAME) {
      viewer->gfx.last_delivered_frame_id = event->u.end_frame.frameId;
      pumped_frame_events++;
    } else if (event->type == VIEWER_GFX_EVENT_SURFACE_COMMAND) {
      pumped_surface_commands++;
    }
    LeaveCriticalSection(&viewer->gfx.lock);

    viewer_gfx_event_unref(event);
  }

  if (pumped_frame_events > 0) {
    WLog_INFO(
        TAG,
        "Viewer %u pump-gfx: delivered %" PRIu32 " frames (lastFid=%" PRIu32
        ") + %" PRIu32 " surfaceCmds, remainingQ=%" PRIu32,
        viewer->id, pumped_frame_events, viewer->gfx.last_delivered_frame_id,
        pumped_surface_commands, viewer->gfx.queue_count);
  }

  return TRUE;
}

/* Drain the classic bitmap queue and send updates to the viewer.
 * Called from the viewer thread. Returns TRUE on success, FALSE on
 * fatal error (viewer should be disconnected). */
#define CLASSIC_GATHER_US 15000 /* 15ms gather window for coalescing */

static BOOL viewer_pump_classic(Viewer *viewer) {
  ViewerClassicEvent *event = NULL;
  UINT32 pumped = 0;
  UINT32 coalesced = 0;
  UINT32 sb_pumped = 0;
  BOOL classic_fallback = FALSE;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;

  if (!viewer)
    return FALSE;

  /* Only pump for classic-fallback viewers */
  EnterCriticalSection(&viewer->gfx.lock);
  classic_fallback = viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx);
  LeaveCriticalSection(&viewer->gfx.lock);

  if (!classic_fallback)
    return TRUE;

  /* Hold rdp_update_lock across the entire drain loop so that mstsc
   * receives all bitmap updates as a continuous stream without rendering
   * between individual sends. */
  if (peer && peer->context && peer->context->update)
    rdp_update_lock(peer->context->update);

  for (;;) {
    /* Option C: Frame coalescing — drain the entire queue and merge
     * multiple BITMAP_UPDATEs into a single send when possible. */
    EnterCriticalSection(&viewer->send_lock);

    /* Skip if viewer needs full refresh (will resync via refresh path) */
    if (viewer->needs_full_refresh) {
      /* Drop all queued updates — they're stale relative to the
       * upcoming full refresh */
      if (viewer->classic_queue_count > 0) {
        WLog_INFO(TAG,
                  "Viewer %u pump-classic: dropping %" PRIu32
                  " queued updates (needs full refresh)",
                  viewer->id, viewer->classic_queue_count);
        viewer_classic_queue_clear_locked(viewer);
      }
      LeaveCriticalSection(&viewer->send_lock);
      break;
    }

    event = viewer_classic_dequeue_locked(viewer);
    LeaveCriticalSection(&viewer->send_lock);

    if (!event)
      break;

    /* Option C: Coalesce — if there are more entries in the queue,
     * merge their rectangles into this event's bitmap before sending. */
    EnterCriticalSection(&viewer->send_lock);
    while (viewer->classic_queue_count > 0) {
      ViewerClassicEvent *next = viewer_classic_dequeue_locked(viewer);
      UINT32 old_number = 0;
      BITMAP_DATA *new_rects = NULL;
      UINT32 j = 0;

      if (!next)
        break;

      old_number = event->bitmap->number;
      new_rects = (BITMAP_DATA *)realloc(event->bitmap->rectangles,
                                         (old_number + next->bitmap->number) *
                                             sizeof(BITMAP_DATA));
      if (!new_rects) {
        /* Coalescing failed — send what we have and queue the rest */
        viewer_classic_event_free(next);
        break;
      }

      event->bitmap->rectangles = new_rects;
      for (j = 0; j < next->bitmap->number; j++) {
        event->bitmap->rectangles[old_number + j] = next->bitmap->rectangles[j];
        /* Transfer ownership of bitmapDataStream to avoid double-free */
        next->bitmap->rectangles[j].bitmapDataStream = NULL;
        next->bitmap->rectangles[j].bitmapLength = 0;
      }
      event->bitmap->number += next->bitmap->number;
      coalesced++;

      /* Free the next event struct (but not the transferred data) */
      free(next->bitmap->rectangles);
      next->bitmap->rectangles = NULL;
      next->bitmap->number = 0;
      viewer_classic_event_free(next);
    }
    LeaveCriticalSection(&viewer->send_lock);

    /* Gather delay: if we only have 1 bitmap (no coalescing happened),
     * briefly wait for more backend batches to arrive before sending.
     * This allows multiple BitmapUpdates to accumulate and be coalesced
     * into a single larger PDU, eliminating the "rows of boxes" effect
     * where each small update is rendered individually by mstsc. */
    if (coalesced == 0 && pumped == 0) {
      UINT64 gather_start = viewer_perf_now_us();
      while ((viewer_perf_now_us() - gather_start) < CLASSIC_GATHER_US) {
        EnterCriticalSection(&viewer->send_lock);
        if (viewer->classic_queue_count > 0) {
          ViewerClassicEvent *next = viewer_classic_dequeue_locked(viewer);
          UINT32 old_number = 0;
          BITMAP_DATA *new_rects = NULL;
          UINT32 j = 0;

          LeaveCriticalSection(&viewer->send_lock);

          if (!next)
            break;

          old_number = event->bitmap->number;
          new_rects = (BITMAP_DATA *)realloc(
              event->bitmap->rectangles,
              (old_number + next->bitmap->number) * sizeof(BITMAP_DATA));
          if (!new_rects) {
            viewer_classic_event_free(next);
            break;
          }

          event->bitmap->rectangles = new_rects;
          for (j = 0; j < next->bitmap->number; j++) {
            event->bitmap->rectangles[old_number + j] =
                next->bitmap->rectangles[j];
            next->bitmap->rectangles[j].bitmapDataStream = NULL;
            next->bitmap->rectangles[j].bitmapLength = 0;
          }
          event->bitmap->number += next->bitmap->number;
          coalesced++;

          free(next->bitmap->rectangles);
          next->bitmap->rectangles = NULL;
          next->bitmap->number = 0;
          viewer_classic_event_free(next);

          /* Items are arriving — keep gathering until the window
           * expires or the queue is empty */
          continue;
        }
        LeaveCriticalSection(&viewer->send_lock);
        platform_sleep_ms(1);
      }
    }

    /* Send the (possibly coalesced) bitmap update.
     * rdp_update_lock is already held across the entire pump loop,
     * so mstsc receives all updates as a continuous stream. */
    if (!viewer_send_bitmap_update_locked(viewer, event->bitmap)) {
      WLog_WARN(TAG, "Viewer %u pump-classic: send failed", viewer->id);
      viewer_classic_event_free(event);
      /* Send failure is not fatal — the viewer may recover */
      continue;
    }

    pumped++;
    viewer_classic_event_free(event);
  }

  /* Drain SurfaceBits queue — each event is a separate codec frame, no
   * coalescing. Unlike BitmapUpdate, SurfaceBits are NOT dropped when
   * needs_full_refresh is true. When SurfaceBits (NSCodec/RemoteFX) is the
   * active codec, these tiles ARE the refresh data — dropping them creates a
   * deadlock where the viewer never receives the content needed to resync. */
  for (;;) {
    ViewerSurfaceBitsEvent *sb_event = NULL;

    EnterCriticalSection(&viewer->send_lock);
    sb_event = viewer_surface_bits_dequeue_locked(viewer);
    LeaveCriticalSection(&viewer->send_lock);

    if (!sb_event)
      break;

    /* Send outside send_lock — rdp_update_lock provides FreeRDP's own sync,
     * and the event data is locally owned after dequeue. */
    if (!viewer_send_surface_bits(viewer, &sb_event->cmd)) {
      WLog_WARN(TAG, "Viewer %u pump-classic: SurfaceBits send failed",
                viewer->id);
      viewer_surface_bits_event_free(sb_event);
      continue;
    }

    sb_pumped++;
    viewer_surface_bits_event_free(sb_event);
  }

  if ((pumped > 0) || (sb_pumped > 0)) {
    WLog_INFO(TAG,
              "Viewer %u pump-classic: delivered %" PRIu32
              " bitmaps (coalesced=%" PRIu32 ") %" PRIu32 " SurfaceBits",
              viewer->id, pumped, coalesced, sb_pumped);
  }

  /* Release the update lock acquired at the top of this function. */
  if (peer && peer->context && peer->context->update)
    rdp_update_unlock(peer->context->update);

  return TRUE;
}

static BOOL viewer_server_publish_gfx_event(BackendClient *backend,
                                            ViewerGfxEvent *event) {
  ViewerServer *server = g_viewer_server;
  BOOL sent_any = FALSE;
  int i = 0;

  if (!server || (server->backend != backend) || !event) {
    viewer_gfx_event_unref(event);
    return FALSE;
  }

  EnterCriticalSection(&server->lock);
  for (i = 0; i < MAX_VIEWERS; i++) {
    Viewer *viewer = &server->viewers[i];
    BOOL enqueue = FALSE;
    BOOL waiting_for_ack = FALSE;
    BOOL overflowed = FALSE;
    ViewerJoinState join_state = VIEWER_JOIN_STATE_NONE;

    if (!viewer->peer || !viewer->connected || !viewer->activated ||
        viewer->stop_requested)
      continue;

    EnterCriticalSection(&viewer->gfx.lock);
    enqueue = !viewer->gfx.rdpgfx_temporarily_disabled &&
              viewer->gfx.channel_opened && viewer->gfx.caps_ready &&
              viewer_gfx_negotiation_is_rdpgfx_ready(&viewer->gfx) &&
              viewer_gfx_viewer_accepts_live_locked(&viewer->gfx);
    join_state = viewer->gfx.join_state;
    if (!enqueue && !viewer->gfx.rdpgfx_temporarily_disabled &&
        viewer->gfx.channel_opened && viewer->gfx.caps_ready &&
        viewer_gfx_negotiation_is_rdpgfx_ready(&viewer->gfx) &&
        (viewer->gfx.join_state != VIEWER_JOIN_STATE_LIVE)) {
      WLog_DBG(TAG,
               "Viewer %u skipping live enqueue type=%u during late-join "
               "replay gate (join=%s)",
               viewer->id, event->type,
               viewer_join_state_name(viewer->gfx.join_state));
    }
    waiting_for_ack =
        (viewer->gfx.join_state == VIEWER_JOIN_STATE_WAIT_REPLAY_ACK);
    if (enqueue && (event->type == VIEWER_GFX_EVENT_RESET_GRAPHICS)) {
      UINT64 elapsed =
          platform_get_timestamp_ms() - viewer->gfx.last_activated_ts;
      if (elapsed < 5000) {
        WLog_INFO(TAG,
                  "Viewer %u skipping ResetGraphics (activated %" PRIu64
                  " ms ago)",
                  viewer->id, elapsed);
        enqueue = FALSE;
      }
    }
    if (enqueue) {
      viewer_gfx_event_ref(event);
      if (!viewer_gfx_enqueue_locked(&viewer->gfx, event)) {
        overflowed = waiting_for_ack;
        if (!overflowed) {
          WLog_ERR(TAG,
                   "Viewer %u queue overflow (count=%" PRIu32
                   " pending=%" PRIu32 "); disconnecting",
                   viewer->id, viewer->gfx.queue_count,
                   viewer->gfx.pending_frame_count);
          viewer->stop_requested = TRUE;
        }
        viewer_gfx_event_unref(event);
      } else {
        sent_any = TRUE;
      }
    }
    if (overflowed) {
      viewer_gfx_queue_clear_locked(&viewer->gfx);
      viewer_gfx_set_join_state_locked(viewer, VIEWER_JOIN_STATE_PENDING,
                                       VIEWER_JOIN_STRATEGY_BACKEND_REFRESH,
                                       "WAIT_REPLAY_ACK queue overflow");
    }
    LeaveCriticalSection(&viewer->gfx.lock);
  }
  LeaveCriticalSection(&server->lock);

  if (event->type == VIEWER_GFX_EVENT_END_FRAME) {
    WLog_INFO(TAG, "GFX publish endFrame fid=%" PRIu32 " sent=%d",
              event->u.end_frame.frameId, sent_any);
  } else if (event->type == VIEWER_GFX_EVENT_START_FRAME) {
    WLog_DBG(TAG, "GFX publish startFrame fid=%" PRIu32 " sent=%d",
             event->u.start_frame.frameId, sent_any);
  }

  viewer_gfx_event_unref(event);
  return sent_any;
}

static void viewer_get_backend_layout(BackendClient *backend, UINT32 *width,
                                      UINT32 *height, UINT32 *generation) {
  UINT32 current_width = 0;
  UINT32 current_height = 0;
  UINT32 current_generation = 0;

  if (backend) {
    backend_get_desktop_layout(backend, &current_width, &current_height,
                               &current_generation);
    if (current_width == 0)
      current_width = backend->monitor_layout.total_width;
    if (current_height == 0)
      current_height = backend->monitor_layout.total_height;
  }

  if (width)
    *width = current_width;
  if (height)
    *height = current_height;
  if (generation)
    *generation = current_generation;
}

static void viewer_server_clear_input_owner_locked(ViewerServer *server,
                                                   UINT32 viewer_id,
                                                   const char *reason) {
  if (!server || !server->input_owner_active ||
      (server->input_owner_viewer_id != viewer_id))
    return;

  server->input_owner_active = FALSE;
  server->input_owner_viewer_id = 0;
  server->input_owner_last_input_ts = 0;

  if (reason)
    WLog_INFO(TAG, "Viewer %u %s", viewer_id, reason);
}

static BOOL viewer_input_owner_alive_locked(ViewerServer *server,
                                            UINT32 viewer_id) {
  if (!server || (viewer_id == 0))
    return FALSE;

  for (int i = 0; i < MAX_VIEWERS; i++) {
    const Viewer *current = &server->viewers[i];
    if ((current->id == viewer_id) && current->connected && current->activated)
      return TRUE;
  }

  return FALSE;
}

static void viewer_graphics_context_init(ViewerGraphicsContext *gfx) {
  if (!gfx || gfx->initialized)
    return;

  InitializeCriticalSection(&gfx->lock);
  gfx->initialized = TRUE;
}

static void viewer_graphics_context_reset(ViewerGraphicsContext *gfx,
                                          BackendClient *backend) {
  UINT32 width = 0;
  UINT32 height = 0;

  if (!gfx)
    return;

  viewer_get_backend_layout(backend, &width, &height, NULL);
  gfx->negotiated_width = width;
  gfx->negotiated_height = height;
  gfx->post_connect_complete = FALSE;
  gfx->ready = FALSE;
  gfx->force_full_present = TRUE;
  gfx->use_rdpgfx = FALSE;
  gfx->channel_opened = FALSE;
  gfx->vcm_progress_logged = FALSE;
  gfx->drdynvc_joined = FALSE;
  gfx->caps_ready = FALSE;
  gfx->rdpgfx_temporarily_disabled = FALSE;
  gfx->negotiation_outcome = VIEWER_GFX_NEGOTIATION_PENDING;
  gfx->drdynvc_state = DRDYNVC_STATE_NONE;
  gfx->join_state = VIEWER_JOIN_STATE_NONE;
  gfx->join_strategy = VIEWER_JOIN_STRATEGY_NONE;
  gfx->join_target_frame_id = 0;
  gfx->join_start_ts = 0;
  gfx->join_refresh_generation = 0;
  gfx->last_delivered_frame_id = 0;
  gfx->last_delivered_event_type = 0;
  gfx->last_delivered_ts = 0;
  gfx->last_activated_ts = 0;
  gfx->queue_head = 0;
  gfx->queue_tail = 0;
  gfx->queue_count = 0;
  gfx->pending_frame_count = 0;
}

static void viewer_graphics_context_uninit(ViewerGraphicsContext *gfx) {
  if (!gfx || !gfx->initialized)
    return;

  EnterCriticalSection(&gfx->lock);
  viewer_gfx_queue_clear_locked(gfx);
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
  LeaveCriticalSection(&gfx->lock);

  DeleteCriticalSection(&gfx->lock);
  memset(gfx, 0, sizeof(*gfx));
}

static BOOL viewer_send_state_init(Viewer *viewer) {
  if (!viewer)
    return FALSE;

  InitializeCriticalSection(&viewer->send_lock);
  viewer->packets_sent = 0;
  viewer->packets_failed = 0;
  viewer->write_block_events = 0;
  viewer->bitmap_updates_sent = 0;
  viewer->bitmap_updates_failed = 0;
  viewer->bitmap_rectangles_sent = 0;
  viewer->bitmap_payload_bytes_sent = 0;
  viewer->bitmap_write_block_events = 0;
  viewer->bitmap_send_time_total_us = 0;
  viewer->bitmap_send_time_max_us = 0;
  viewer->bitmap_updates_skipped_writeblock = 0;
  viewer->bitmap_updates_skipped_throttle = 0;
  viewer->bitmap_updates_queued = 0;
  viewer->bitmap_queue_dropped = 0;
  viewer->consecutive_lag_intervals = 0;
  viewer->sustained_lag_start_ts = 0;
  viewer->last_pointer_position_generation = 0;
  viewer->last_pointer_shape_generation = 0;
  viewer->classic_queue_head = 0;
  viewer->classic_queue_tail = 0;
  viewer->classic_queue_count = 0;
  memset(viewer->classic_queue, 0, sizeof(viewer->classic_queue));
  viewer->surface_bits_queue_head = 0;
  viewer->surface_bits_queue_tail = 0;
  viewer->surface_bits_queue_count = 0;
  memset(viewer->surface_bits_queue, 0, sizeof(viewer->surface_bits_queue));
  viewer->surface_bits_updates_sent = 0;
  viewer->surface_bits_updates_failed = 0;
  viewer->surface_bits_updates_skipped_writeblock = 0;
  viewer->surface_bits_updates_skipped_throttle = 0;
  viewer->surface_bits_updates_queued = 0;
  viewer->surface_bits_queue_dropped = 0;
  viewer->surface_bits_send_time_total_us = 0;
  viewer->surface_bits_send_time_max_us = 0;
  viewer->surface_bits_payload_bytes_sent = 0;
  viewer->classic_event = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (!viewer->classic_event) {
    DeleteCriticalSection(&viewer->send_lock);
    return FALSE;
  }
  return TRUE;
}

static void viewer_send_state_uninit(Viewer *viewer) {
  if (!viewer || !viewer->gfx.initialized)
    return;

  /* Free any remaining classic queue entries */
  EnterCriticalSection(&viewer->send_lock);
  viewer_classic_queue_clear_locked(viewer);
  viewer_surface_bits_queue_clear_locked(viewer);
  LeaveCriticalSection(&viewer->send_lock);

  if (viewer->classic_event) {
    CloseHandle(viewer->classic_event);
    viewer->classic_event = NULL;
  }

  DeleteCriticalSection(&viewer->send_lock);
}

static void viewer_join_thread(Viewer *viewer) {
  if (!viewer || !viewer->thread)
    return;

  WaitForSingleObject(viewer->thread, INFINITE);
  CloseHandle(viewer->thread);
  viewer->thread = NULL;
}

static void viewer_cleanup_slot(Viewer *viewer) {
  if (!viewer)
    return;

  viewer_send_state_uninit(viewer);
  viewer_graphics_context_uninit(&viewer->gfx);
  viewer->peer = NULL;
  viewer->context = NULL;
  viewer->connected = FALSE;
  viewer->activated = FALSE;
  viewer->needs_full_refresh = FALSE;
  viewer->stop_requested = FALSE;
  viewer->full_refresh_deadline_ts = 0;
  viewer->last_pointer_position_generation = 0;
  viewer->last_pointer_shape_generation = 0;
}

static void viewer_pointer_shape_entry_reset(PointerShapeEntry *shape) {
  if (!shape)
    return;

  free(shape->xorMaskData);
  free(shape->andMaskData);
  memset(shape, 0, sizeof(*shape));
}

static BOOL viewer_pointer_shape_entry_copy(PointerShapeEntry *destination,
                                            const PointerShapeEntry *source) {
  if (!destination || !source)
    return FALSE;

  memset(destination, 0, sizeof(*destination));
  *destination = *source;
  destination->xorMaskData = NULL;
  destination->andMaskData = NULL;

  if (source->xorMaskLength > 0) {
    destination->xorMaskData = (BYTE *)malloc(source->xorMaskLength);
    if (!destination->xorMaskData) {
      viewer_pointer_shape_entry_reset(destination);
      return FALSE;
    }
    memcpy(destination->xorMaskData, source->xorMaskData,
           source->xorMaskLength);
  }

  if (source->andMaskLength > 0) {
    destination->andMaskData = (BYTE *)malloc(source->andMaskLength);
    if (!destination->andMaskData) {
      viewer_pointer_shape_entry_reset(destination);
      return FALSE;
    }
    memcpy(destination->andMaskData, source->andMaskData,
           source->andMaskLength);
  }

  return TRUE;
}

static BOOL viewer_forward_pointer(Viewer *viewer, BOOL force) {
  ViewerServer *server = g_viewer_server;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;
  BackendClient *backend = server ? server->backend : NULL;
  UINT16 pointer_x = 0;
  UINT16 pointer_y = 0;
  UINT32 pointer_type = SYSPTR_DEFAULT;
  BOOL pointer_visible = TRUE;
  PointerShapeEntry *active_shape = NULL;
  PointerShapeEntry shape_copy = {0};
  POINTER_SYSTEM_UPDATE pointer_system = {0};
  POINTER_POSITION_UPDATE pointer_position = {0};
  POINTER_COLOR_UPDATE pointer_color = {0};
  POINTER_NEW_UPDATE pointer_new = {0};
  UINT64 position_generation = 0;
  UINT64 shape_generation = 0;
  BOOL shape_changed = FALSE;
  BOOL position_changed = FALSE;
  BOOL send_shape = FALSE;
  BOOL send_position = FALSE;
  BOOL sent = TRUE;

  if (!viewer || !server || !backend || !peer || !peer->context ||
      !peer->context->update || !peer->context->update->pointer ||
      !viewer->connected || !viewer->activated || !peer->activated)
    return FALSE;

  backend_get_pointer_snapshot(backend, &pointer_x, &pointer_y,
                               &pointer_visible, &pointer_type, &active_shape,
                               &position_generation, &shape_generation);
  if (active_shape &&
      !viewer_pointer_shape_entry_copy(&shape_copy, active_shape))
    return FALSE;

  shape_changed =
      force || (shape_generation != viewer->last_pointer_shape_generation);
  position_changed = force || (position_generation !=
                               viewer->last_pointer_position_generation);
  send_shape = shape_changed;
  send_position = position_changed && pointer_visible;

  if (!send_shape && !send_position) {
    viewer_pointer_shape_entry_reset(&shape_copy);
    return TRUE;
  }

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    if (!peer->DrainOutputBuffer || (peer->DrainOutputBuffer(peer) < 0) ||
        peer->IsWriteBlocked(peer)) {
      viewer_pointer_shape_entry_reset(&shape_copy);
      return FALSE;
    }
  }

  pointer_position.xPos = pointer_x;
  pointer_position.yPos = pointer_y;
  pointer_system.type = pointer_visible ? pointer_type : SYSPTR_NULL;
  pointer_color.cacheIndex = shape_copy.cacheIndex;
  pointer_color.hotSpotX = shape_copy.hotSpotX;
  pointer_color.hotSpotY = shape_copy.hotSpotY;
  pointer_color.width = shape_copy.width;
  pointer_color.height = shape_copy.height;
  pointer_color.lengthAndMask = shape_copy.andMaskLength;
  pointer_color.lengthXorMask = shape_copy.xorMaskLength;
  pointer_color.xorMaskData = shape_copy.xorMaskData;
  pointer_color.andMaskData = shape_copy.andMaskData;
  pointer_new.xorBpp = shape_copy.xorBpp;
  pointer_new.colorPtrAttr = pointer_color;

  rdp_update_lock(peer->context->update);
  if (send_shape) {
    if (!pointer_visible || !active_shape) {
      IFCALLRET(peer->context->update->pointer->PointerSystem, sent,
                peer->context, &pointer_system);
    } else if ((shape_copy.xorBpp > 0) &&
               peer->context->update->pointer->PointerNew) {
      IFCALLRET(peer->context->update->pointer->PointerNew, sent, peer->context,
                &pointer_new);
    } else {
      IFCALLRET(peer->context->update->pointer->PointerColor, sent,
                peer->context, &pointer_color);
    }
  }

  if (sent && send_position && peer->context->update->pointer->PointerPosition)
    IFCALLRET(peer->context->update->pointer->PointerPosition, sent,
              peer->context, &pointer_position);
  rdp_update_unlock(peer->context->update);

  viewer_pointer_shape_entry_reset(&shape_copy);
  if (!sent)
    return FALSE;

  viewer->last_pointer_position_generation = position_generation;
  viewer->last_pointer_shape_generation = shape_generation;
  return TRUE;
}

static BOOL viewer_send_surface_bits(Viewer *viewer,
                                     const SURFACE_BITS_COMMAND *cmd) {
  BOOL ret = FALSE;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;
  UINT64 send_started_us = 0;
  UINT64 send_us = 0;

  if (!viewer || !peer || !peer->context || !peer->context->update || !cmd)
    return FALSE;

  /* Option A: Skip on write-block — don't stall the viewer thread */
  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    viewer->write_block_events++;
    viewer->surface_bits_updates_skipped_writeblock++;
    return FALSE;
  }

  send_started_us = viewer_perf_now_us();
  rdp_update_lock(peer->context->update);
  IFCALLRET(peer->context->update->SurfaceBits, ret, peer->context, cmd);
  rdp_update_unlock(peer->context->update);
  send_us = viewer_perf_now_us() - send_started_us;
  viewer->surface_bits_send_time_total_us += send_us;
  if (send_us > viewer->surface_bits_send_time_max_us)
    viewer->surface_bits_send_time_max_us = send_us;
  if (ret) {
    viewer->packets_sent++;
    viewer->surface_bits_updates_sent++;
    viewer->surface_bits_payload_bytes_sent += cmd->bmp.bitmapDataLength;
  } else {
    viewer->packets_failed++;
    viewer->surface_bits_updates_failed++;
  }
  return ret;
}

static BOOL viewer_send_bitmap_update(Viewer *viewer,
                                      const BITMAP_UPDATE *bitmap) {
  BOOL ret = FALSE;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;
  UINT64 send_started_us = 0;
  UINT64 send_us = 0;
  UINT64 payload_bytes = 0;
  UINT32 i = 0;

  if (!viewer || !peer || !peer->context || !peer->context->update || !bitmap)
    return FALSE;

  for (i = 0; i < bitmap->number; i++)
    payload_bytes += bitmap->rectangles[i].bitmapLength;

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    viewer->write_block_events++;
    viewer->bitmap_write_block_events++;
    viewer->bitmap_updates_skipped_writeblock++;
    return FALSE;
  }

  send_started_us = viewer_perf_now_us();
  rdp_update_lock(peer->context->update);
  IFCALLRET(peer->context->update->BitmapUpdate, ret, peer->context, bitmap);
  rdp_update_unlock(peer->context->update);
  send_us = viewer_perf_now_us() - send_started_us;
  viewer->bitmap_send_time_total_us += send_us;
  if (send_us > viewer->bitmap_send_time_max_us)
    viewer->bitmap_send_time_max_us = send_us;
  if (ret) {
    viewer->packets_sent++;
    viewer->bitmap_updates_sent++;
    viewer->bitmap_rectangles_sent += bitmap->number;
    viewer->bitmap_payload_bytes_sent += payload_bytes;
  } else {
    viewer->packets_failed++;
    viewer->bitmap_updates_failed++;
  }
  return ret;
}

/* Same as viewer_send_bitmap_update but assumes rdp_update_lock is already
 * held. Used by viewer_pump_classic to batch multiple sends under a single lock
 * acquisition, preventing mstsc from rendering between individual updates. */
static BOOL viewer_send_bitmap_update_locked(Viewer *viewer,
                                             const BITMAP_UPDATE *bitmap) {
  BOOL ret = FALSE;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;
  UINT64 send_started_us = 0;
  UINT64 send_us = 0;
  UINT64 payload_bytes = 0;
  UINT32 i = 0;

  if (!viewer || !peer || !peer->context || !peer->context->update || !bitmap)
    return FALSE;

  for (i = 0; i < bitmap->number; i++)
    payload_bytes += bitmap->rectangles[i].bitmapLength;

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    viewer->write_block_events++;
    viewer->bitmap_write_block_events++;
    viewer->bitmap_updates_skipped_writeblock++;
    return FALSE;
  }

  send_started_us = viewer_perf_now_us();
  /* rdp_update_lock is already held by the caller (viewer_pump_classic) */
  IFCALLRET(peer->context->update->BitmapUpdate, ret, peer->context, bitmap);
  send_us = viewer_perf_now_us() - send_started_us;
  viewer->bitmap_send_time_total_us += send_us;
  if (send_us > viewer->bitmap_send_time_max_us)
    viewer->bitmap_send_time_max_us = send_us;
  if (ret) {
    viewer->packets_sent++;
    viewer->bitmap_updates_sent++;
    viewer->bitmap_rectangles_sent += bitmap->number;
    viewer->bitmap_payload_bytes_sent += payload_bytes;
  } else {
    viewer->packets_failed++;
    viewer->bitmap_updates_failed++;
  }
  return ret;
}

static BOOL viewer_send_frame_marker(Viewer *viewer,
                                     const SURFACE_FRAME_MARKER *marker) {
  BOOL ret = FALSE;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;

  if (!viewer || !peer || !peer->context || !peer->context->update || !marker)
    return FALSE;

  rdp_update_lock(peer->context->update);
  IFCALLRET(peer->context->update->SurfaceFrameMarker, ret, peer->context,
            marker);
  rdp_update_unlock(peer->context->update);
  if (ret)
    viewer->packets_sent++;
  else
    viewer->packets_failed++;
  return ret;
}

static Viewer *find_viewer_by_peer(freerdp_peer *peer) {
  ViewerServer *server = g_viewer_server;
  if (!server)
    return NULL;

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    if (server->viewers[i].peer == peer) {
      LeaveCriticalSection(&server->lock);
      return &server->viewers[i];
    }
  }
  LeaveCriticalSection(&server->lock);
  return NULL;
}

static BOOL can_viewer_send_input(Viewer *viewer) {
  ViewerServer *server = g_viewer_server;
  ViewerInputOwnershipState state = {0};
  const char *clear_reason = NULL;
  BOOL allowed = FALSE;

  if (!viewer || !server || !viewer->connected || !viewer->activated)
    return FALSE;

  EnterCriticalSection(&server->lock);
  if (server->input_owner_active &&
      !viewer_input_owner_alive_locked(server, server->input_owner_viewer_id))
    clear_reason = "disconnected, input ownership cleared";
  else if (server->input_owner_active &&
           ((platform_get_timestamp_ms() - server->input_owner_last_input_ts) >=
            INPUT_IDLE_TIMEOUT_MS))
    clear_reason = "timed out, input now free";

  if (clear_reason)
    viewer_server_clear_input_owner_locked(
        server, server->input_owner_viewer_id, clear_reason);

  state.owner_active = server->input_owner_active;
  state.owner_viewer_id = server->input_owner_viewer_id;
  state.last_input_ts = server->input_owner_last_input_ts;
  allowed = viewer_input_try_acquire(
      &state, viewer->id, viewer->connected, viewer->activated,
      viewer_input_owner_alive_locked(server, state.owner_viewer_id),
      platform_get_timestamp_ms(), INPUT_IDLE_TIMEOUT_MS);
  server->input_owner_active = state.owner_active;
  server->input_owner_viewer_id = state.owner_viewer_id;
  server->input_owner_last_input_ts = state.last_input_ts;
  LeaveCriticalSection(&server->lock);
  return allowed;
}

static BOOL viewer_gfx_handshake_ready_locked(const Viewer *viewer) {
  return viewer && viewer->activated && viewer->gfx.post_connect_complete &&
         (viewer->gfx.drdynvc_state == DRDYNVC_STATE_READY) &&
         viewer->gfx.channel_opened && viewer->gfx.caps_ready &&
         viewer_gfx_negotiation_is_rdpgfx_ready(&viewer->gfx) &&
         !viewer->gfx.rdpgfx_temporarily_disabled;
}

static ViewerGfxCompleteFrame *
viewer_gfx_lookup_replay_frame(ViewerServer *server, UINT32 minimum_frame_id) {
  ViewerGfxCompleteFrame *frame = NULL;

  if (!server)
    return NULL;

  EnterCriticalSection(&server->gfx.lock);
  frame = viewer_gfx_frame_buffer_latest_replayable_locked(
      &server->gfx.frame_buffer, minimum_frame_id);
  LeaveCriticalSection(&server->gfx.lock);
  return frame;
}

static BOOL viewer_gfx_request_backend_refresh(ViewerServer *server,
                                               Viewer *viewer, UINT64 now,
                                               const char *reason) {
  BackendFullRefreshState refresh_state = {0};
  UINT32 newest_frame_id = 0;
  UINT64 generation = 0;

  if (!server || !server->backend || !viewer)
    return FALSE;

  EnterCriticalSection(&server->gfx.lock);
  newest_frame_id = server->gfx.frame_buffer.newest_frame_id;
  LeaveCriticalSection(&server->gfx.lock);

  (void)backend_request_full_refresh(server->backend);
  backend_get_full_refresh_state(server->backend, &refresh_state);
  generation = refresh_state.requested_generation;
  if (generation == 0)
    generation = refresh_state.in_flight_generation;
  if (generation == 0)
    generation = refresh_state.latest_generation;
  if (generation == 0)
    return FALSE;

  EnterCriticalSection(&viewer->gfx.lock);
  viewer->gfx.join_start_ts = now;
  viewer->gfx.join_target_frame_id = newest_frame_id + 1U;
  viewer->gfx.join_refresh_generation = generation;
  viewer_gfx_queue_clear_locked(&viewer->gfx);
  viewer_gfx_set_join_state_locked(
      viewer, VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH,
      VIEWER_JOIN_STRATEGY_BACKEND_REFRESH, reason);
  LeaveCriticalSection(&viewer->gfx.lock);

  WLog_INFO(TAG,
            "Viewer %u late join using backend refresh generation=%" PRIu64
            " reason=%s",
            viewer->id, generation, reason ? reason : "unspecified");
  return TRUE;
}

static BOOL viewer_gfx_enter_classic_fallback(ViewerServer *server,
                                              Viewer *viewer, UINT64 now,
                                              const char *reason) {
  if (!viewer)
    return FALSE;

  EnterCriticalSection(&viewer->gfx.lock);
  viewer_disable_rdpgfx_locked(viewer);
  viewer->gfx.join_start_ts = now;
  viewer_gfx_set_join_state_locked(viewer, VIEWER_JOIN_STATE_PENDING,
                                   VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK,
                                   reason);
  LeaveCriticalSection(&viewer->gfx.lock);

  EnterCriticalSection(&viewer->send_lock);
  viewer->needs_full_refresh = TRUE;
  viewer->full_refresh_deadline_ts = now + FULL_REFRESH_TIMEOUT_MS;
  LeaveCriticalSection(&viewer->send_lock);

  if (server && server->backend)
    (void)backend_request_full_refresh(server->backend);

  WLog_WARN(TAG, "Viewer %u late join falling back to classic path reason=%s",
            viewer->id, reason ? reason : "unspecified");
  return TRUE;
}

static void viewer_gfx_reject_join(Viewer *viewer, const char *reason) {
  if (!viewer)
    return;

  EnterCriticalSection(&viewer->gfx.lock);
  viewer_gfx_set_join_state_locked(viewer, VIEWER_JOIN_STATE_REJECTED,
                                   VIEWER_JOIN_STRATEGY_REJECT, reason);
  LeaveCriticalSection(&viewer->gfx.lock);
  viewer->stop_requested = TRUE;
  WLog_ERR(TAG, "Viewer %u join rejected reason=%s", viewer->id,
           reason ? reason : "unspecified");
}

static BOOL viewer_gfx_send_surface_preamble(ViewerServer *server,
                                             Viewer *viewer) {
  ViewerGfxEvent *reset_event = NULL;
  ViewerGfxEvent **create_events = NULL;
  ViewerGfxEvent **map_events = NULL;
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
    reset_event =
        viewer_gfx_event_new_reset_graphics(&server->gfx.latest_reset_graphics);
    if (!reset_event)
      ok = FALSE;
    has_reset = TRUE;
  }

  if (ok) {
    for (i = 0; i < VIEWER_GFX_MAX_ACTIVE_SURFACES; i++) {
      if (server->gfx.surfaces[i].in_use) {
        active_surface_count++;
        if (server->gfx.surfaces[i].mapped)
          mapped_surface_count++;
      }
    }

    if (active_surface_count > 0) {
      create_events = (ViewerGfxEvent **)calloc(active_surface_count,
                                                sizeof(ViewerGfxEvent *));
      if (!create_events)
        ok = FALSE;
    }

    if (ok && (mapped_surface_count > 0)) {
      map_events = (ViewerGfxEvent **)calloc(mapped_surface_count,
                                             sizeof(ViewerGfxEvent *));
      if (!map_events)
        ok = FALSE;
    }
  }

  if (ok) {
    for (i = 0; i < VIEWER_GFX_MAX_ACTIVE_SURFACES; i++) {
      const ViewerGraphicsSurfaceState *surface = &server->gfx.surfaces[i];
      if (!surface->in_use)
        continue;

      create_events[create_index] = viewer_gfx_event_new_simple(
          VIEWER_GFX_EVENT_CREATE_SURFACE, &surface->create_surface,
          sizeof(surface->create_surface));
      if (!create_events[create_index++]) {
        ok = FALSE;
        break;
      }

      if (!surface->mapped)
        continue;

      map_events[map_index] =
          viewer_gfx_event_new_simple(VIEWER_GFX_EVENT_MAP_SURFACE_TO_OUTPUT,
                                      &surface->map_surface_to_output,
                                      sizeof(surface->map_surface_to_output));
      if (!map_events[map_index++]) {
        ok = FALSE;
        break;
      }
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
  if (reset_event && !viewer_send_gfx_event(viewer, reset_event))
    ok = FALSE;

  for (i = 0; ok && (i < active_surface_count); i++)
    ok = viewer_send_gfx_event(viewer, create_events[i]);

  for (i = 0; ok && (i < mapped_surface_count); i++)
    ok = viewer_send_gfx_event(viewer, map_events[i]);
  LeaveCriticalSection(&viewer->send_lock);

  if (ok) {
    WLog_INFO(TAG, "Viewer %u surface preamble sent successfully", viewer->id);
  } else {
    WLog_ERR(TAG, "Viewer %u surface preamble send failed", viewer->id);
  }

cleanup:
  if (reset_event)
    viewer_gfx_event_unref(reset_event);
  if (create_events) {
    for (i = 0; i < active_surface_count; i++)
      viewer_gfx_event_unref(create_events[i]);
    free(create_events);
  }
  if (map_events) {
    for (i = 0; i < mapped_surface_count; i++)
      viewer_gfx_event_unref(map_events[i]);
    free(map_events);
  }

  return ok;
}

static BOOL viewer_gfx_bootstrap_direct_live(ViewerServer *server,
                                             Viewer *viewer, UINT64 now,
                                             const char *reason) {
  if (!server || !viewer)
    return FALSE;

  WLog_INFO(
      TAG,
      "Viewer %u bootstrapping directly to LIVE via surface preamble reason=%s",
      viewer->id, reason ? reason : "unspecified");

  if (!viewer_gfx_send_surface_preamble(server, viewer)) {
    WLog_ERR(TAG,
             "Viewer %u direct bootstrap failed: surface preamble send error",
             viewer->id);
    return FALSE;
  }

  EnterCriticalSection(&viewer->gfx.lock);
  viewer->gfx.join_start_ts = now;
  viewer_gfx_finish_late_join_locked(viewer, reason);
  LeaveCriticalSection(&viewer->gfx.lock);

  EnterCriticalSection(&viewer->send_lock);
  viewer->needs_full_refresh = FALSE;
  viewer->full_refresh_deadline_ts = 0;
  LeaveCriticalSection(&viewer->send_lock);

  if (server->backend) {
    (void)backend_request_full_refresh(server->backend);
    WLog_INFO(TAG,
              "Viewer %u requested backend full refresh after direct bootstrap",
              viewer->id);
  }

  return TRUE;
}

static BOOL viewer_gfx_step_join(ViewerServer *server, Viewer *viewer,
                                 UINT64 now) {
  ViewerJoinState state = VIEWER_JOIN_STATE_NONE;
  ViewerJoinStrategy strategy = VIEWER_JOIN_STRATEGY_NONE;
  UINT32 target_frame_id = 0;
  UINT64 refresh_generation = 0;
  ViewerGfxCompleteFrame *frame = NULL;
  BackendFullRefreshState refresh_state = {0};

  if (!server || !viewer)
    return FALSE;

  EnterCriticalSection(&viewer->gfx.lock);
  if (!viewer_gfx_handshake_ready_locked(viewer)) {
    LeaveCriticalSection(&viewer->gfx.lock);
    return TRUE;
  }

  state = viewer->gfx.join_state;
  strategy = viewer->gfx.join_strategy;
  target_frame_id = viewer->gfx.join_target_frame_id;
  refresh_generation = viewer->gfx.join_refresh_generation;
  LeaveCriticalSection(&viewer->gfx.lock);

  if (state == VIEWER_JOIN_STATE_LIVE)
    return TRUE;

  if (state == VIEWER_JOIN_STATE_WAIT_REPLAY_ACK) {
    if (viewer_late_join_timeout_fallback_due(
            VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME, viewer->gfx.join_start_ts,
            now, VIEWER_WAIT_REPLAY_ACK_TIMEOUT_MS, TRUE)) {
      if (refresh_generation == 0)
        return viewer_gfx_request_backend_refresh(server, viewer, now,
                                                  "replay ack timeout");
      return viewer_gfx_enter_classic_fallback(
          server, viewer, now, "replay ack timeout after backend refresh");
    }
    return TRUE;
  }

  if ((state == VIEWER_JOIN_STATE_PENDING) ||
      (state == VIEWER_JOIN_STATE_WAIT_NEXT_SAFE_FRAME) ||
      (state == VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH)) {
    (void)viewer_gfx_try_schedule_late_join_replay_locked(server, viewer, now);
    EnterCriticalSection(&viewer->gfx.lock);
    state = viewer->gfx.join_state;
    strategy = viewer->gfx.join_strategy;
    target_frame_id = viewer->gfx.join_target_frame_id;
    refresh_generation = viewer->gfx.join_refresh_generation;
    if ((state == VIEWER_JOIN_STATE_PENDING) &&
        (strategy == VIEWER_JOIN_STRATEGY_NONE)) {
      if (!viewer_gfx_handshake_ready_locked(viewer)) {
        LeaveCriticalSection(&viewer->gfx.lock);
        return FALSE;
      }
    }
    LeaveCriticalSection(&viewer->gfx.lock);
  }

  if ((strategy == VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME) &&
      (target_frame_id != 0)) {
    frame = viewer_gfx_lookup_replay_frame(server, target_frame_id);
    if (!frame) {
      if (refresh_generation == 0)
        return viewer_gfx_request_backend_refresh(
            server, viewer, now, "selected replay frame unavailable");
      return viewer_gfx_enter_classic_fallback(
          server, viewer, now,
          "selected replay frame unavailable after backend refresh");
    }

    if (!viewer_gfx_replay_frame(server, viewer, frame)) {
      viewer_gfx_complete_frame_unref(frame);
      viewer_gfx_reject_join(viewer, "replay baseline send failed");
      return FALSE;
    }

    viewer_gfx_complete_frame_unref(frame);
    return TRUE;
  }

  if (state == VIEWER_JOIN_STATE_WAIT_NEXT_SAFE_FRAME) {
    if (viewer_late_join_timeout_fallback_due(
            VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME,
            viewer->gfx.join_start_ts, now, FULL_REFRESH_TIMEOUT_MS, FALSE)) {
      /* No replay-safe frame arrived within the timeout window.
       * The backend refresh mechanism (RefreshRect) is a classic-RDP
       * path that does not work on GFX-only backends. Instead of
       * waiting for a backend refresh that will never complete,
       * bootstrap directly to LIVE via surface preamble. The viewer
       * receives a blank screen initially but will start receiving
       * frames as soon as the desktop changes. */
      if (refresh_generation == 0)
        return viewer_gfx_bootstrap_direct_live(
            server, viewer, now, "no replay baseline after timeout");
      return viewer_gfx_enter_classic_fallback(
          server, viewer, now,
          "wait-next-safe-frame timeout after backend refresh");
    }
    return TRUE;
  }

  if (state == VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH) {
    if (backend_abandon_full_refresh_if_timed_out(
            server->backend, refresh_generation,
            VIEWER_GFX_BACKEND_REFRESH_TIMEOUT_MS))
      return viewer_gfx_enter_classic_fallback(server, viewer, now,
                                               "backend refresh timed out");

    backend_get_full_refresh_state(server->backend, &refresh_state);
    if ((refresh_state.completed_generation >= refresh_generation) &&
        (refresh_state.completed_outcome !=
         BACKEND_FULL_REFRESH_OUTCOME_NONE)) {
      if (refresh_state.completed_outcome ==
          BACKEND_FULL_REFRESH_OUTCOME_COMPLETED) {
        WLog_INFO(TAG,
                  "Viewer %u backend refresh generation=%" PRIu64
                  " completed; bootstrapping via surface preamble",
                  viewer->id, refresh_generation);
        if (!viewer_gfx_send_surface_preamble(server, viewer)) {
          return viewer_gfx_enter_classic_fallback(
              server, viewer, now,
              "surface preamble failed after backend refresh");
        }
        EnterCriticalSection(&viewer->gfx.lock);
        viewer_gfx_finish_late_join_locked(
            viewer, "backend refresh completed; live stream enabled via "
                    "surface preamble");
        LeaveCriticalSection(&viewer->gfx.lock);
        EnterCriticalSection(&viewer->send_lock);
        viewer->needs_full_refresh = FALSE;
        viewer->full_refresh_deadline_ts = 0;
        LeaveCriticalSection(&viewer->send_lock);
        if (server->backend) {
          (void)backend_request_full_refresh(server->backend);
          WLog_INFO(TAG,
                    "Viewer %u requested follow-up backend refresh after "
                    "preamble bootstrap",
                    viewer->id);
        }
        return TRUE;
      }

      return viewer_gfx_enter_classic_fallback(
          server, viewer, now, "backend refresh failed or timed out");
    }
  }

  if (strategy == VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK)
    return viewer_gfx_enter_classic_fallback(server, viewer, now,
                                             "caps or handshake fallback");

  return TRUE;
}

static BOOL on_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
  Viewer *viewer = find_viewer_by_peer(input->context->peer);
  if (!viewer || !can_viewer_send_input(viewer))
    return TRUE;

  freerdp_input_send_mouse_event(g_viewer_server->backend->context->input,
                                 flags, x, y);
  backend_store_pointer_position(g_viewer_server->backend, x, y);
  return TRUE;
}

static BOOL on_extended_mouse_event(rdpInput *input, UINT16 flags, UINT16 x,
                                    UINT16 y) {
  Viewer *viewer = find_viewer_by_peer(input->context->peer);
  if (!viewer || !can_viewer_send_input(viewer))
    return TRUE;

  freerdp_input_send_extended_mouse_event(
      g_viewer_server->backend->context->input, flags, x, y);
  backend_store_pointer_position(g_viewer_server->backend, x, y);
  return TRUE;
}

static BOOL on_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code) {
  Viewer *viewer = find_viewer_by_peer(input->context->peer);
  if (!viewer || !can_viewer_send_input(viewer))
    return TRUE;

  freerdp_input_send_keyboard_event(g_viewer_server->backend->context->input,
                                    flags, code);
  return TRUE;
}

static DWORD WINAPI viewer_handle_peer(LPVOID arg) {
  Viewer *viewer = (Viewer *)arg;
  freerdp_peer *peer = viewer->peer;
  HANDLE gfx_event = NULL;
  DWORD wait_timeout_ms = INFINITE;

  LOG_I("viewer_server", "Viewer peer accepted (peer=%p)", (void *)peer);

  while (!viewer->stop_requested) {
    UINT64 now = platform_get_timestamp_ms();
    BOOL lag_signal_active = FALSE;
    BOOL write_blocked = FALSE;
    BYTE drdynvc_state = DRDYNVC_STATE_NONE;
    HANDLE wait_objects[MAXIMUM_WAIT_OBJECTS] = {0};
    DWORD wait_count = 0;
    DWORD wait_status = WAIT_FAILED;

    EnterCriticalSection(&viewer->send_lock);
    if (viewer->needs_full_refresh && (viewer->full_refresh_deadline_ts > 0) &&
        (now >= viewer->full_refresh_deadline_ts)) {
      viewer->full_refresh_deadline_ts = 0;
      if (viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx) &&
          (viewer->gfx.join_strategy ==
           VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK)) {
        LeaveCriticalSection(&viewer->send_lock);
        viewer_gfx_reject_join(viewer,
                               "classic fallback full refresh timed out");
        break;
      }
    }
    LeaveCriticalSection(&viewer->send_lock);

    EnterCriticalSection(&viewer->gfx.lock);
    if (viewer_gfx_pending_activation_timeout_due(
            &viewer->gfx, now, VIEWER_RDPEGFX_NEGOTIATION_TIMEOUT_MS)) {
      LeaveCriticalSection(&viewer->gfx.lock);
      (void)viewer_gfx_enter_classic_fallback(g_viewer_server, viewer, now,
                                              "RDPEGFX negotiation timed out");
      continue;
    }
    LeaveCriticalSection(&viewer->gfx.lock);

    write_blocked = peer->IsWriteBlocked && peer->IsWriteBlocked(peer);
    EnterCriticalSection(&viewer->send_lock);
    if (viewer_is_slow(viewer->consecutive_lag_intervals, write_blocked))
      viewer->consecutive_lag_intervals++;
    else
      viewer->consecutive_lag_intervals = 0;

    lag_signal_active = viewer_lag_signal_active(viewer, write_blocked);

    if (lag_signal_active) {
      if (viewer->sustained_lag_start_ts == 0)
        viewer->sustained_lag_start_ts = now;
    } else {
      viewer->sustained_lag_start_ts = 0;
    }
    LeaveCriticalSection(&viewer->send_lock);

    if (g_viewer_server->slow_viewer_disconnect_enabled &&
        viewer_disconnect_due(viewer,
                              g_viewer_server->slow_viewer_disconnect_ms, now))
      break;

    EnterCriticalSection(&viewer->gfx.lock);
    if ((viewer->gfx.join_state == VIEWER_JOIN_STATE_WAIT_NEXT_SAFE_FRAME) ||
        (viewer->gfx.join_state == VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH) ||
        (viewer->gfx.join_state == VIEWER_JOIN_STATE_WAIT_REPLAY_ACK) ||
        (viewer->gfx.join_state == VIEWER_JOIN_STATE_PENDING))
      wait_timeout_ms = 50;
    else {
      /* Use a short periodic timeout even when LIVE so that the GFX
       * pump runs regularly to drain queued frame events. Without this,
       * the viewer thread blocks indefinitely waiting for mstsc input
       * (keyboard/mouse) and queued GFX frames never get sent. A 50ms
       * timeout matches the join-state polling interval and keeps frame
       * delivery responsive. */
      wait_timeout_ms = 50;
    }
    LeaveCriticalSection(&viewer->gfx.lock);

    if (peer && peer->GetEventHandles) {
      wait_count =
          peer->GetEventHandles(peer, wait_objects, MAXIMUM_WAIT_OBJECTS);
      if (wait_count == 0) {
        WLog_ERR(TAG, "Viewer %u failed to get FreeRDP transport event handles",
                 viewer->id);
        break;
      }

      /* Add classic_event to the wait set so the viewer thread wakes
       * immediately when a bitmap update is enqueued (Option B). */
      if (viewer->classic_event && (wait_count < MAXIMUM_WAIT_OBJECTS)) {
        wait_objects[wait_count] = viewer->classic_event;
        wait_count++;
      }

      wait_status = WaitForMultipleObjects(wait_count, wait_objects, FALSE,
                                           wait_timeout_ms);
      if (wait_status == WAIT_TIMEOUT)
        wait_status = WAIT_OBJECT_0;
      if (wait_status == WAIT_FAILED)
        break;

      /* Reset the classic event signal — we'll drain the queue below */
      if (viewer->classic_event)
        ResetEvent(viewer->classic_event);
    } else if (peer && peer->CheckFileDescriptor) {
      if (!peer->CheckFileDescriptor(peer))
        break;
      platform_sleep_ms(1);
      continue;
    } else {
      platform_sleep_ms(1);
      continue;
    }

    if (peer->CheckFileDescriptor && !peer->CheckFileDescriptor(peer))
      break;

    EnterCriticalSection(&viewer->gfx.lock);
    if (viewer->activated && viewer->gfx.vcm &&
        (viewer->gfx.vcm != INVALID_HANDLE_VALUE) &&
        viewer->gfx.post_connect_complete &&
        WTSVirtualChannelManagerIsChannelJoined(viewer->gfx.vcm,
                                                DRDYNVC_SVC_CHANNEL_NAME)) {
      /* Call CheckFileDescriptor every iteration to drain the VCM message
       * queue and trigger drdynvc auto-initialization.  This matches the
       * pattern used by FreeRDP shadow and by the proxy server. */
      if (!WTSVirtualChannelManagerCheckFileDescriptor(viewer->gfx.vcm)) {
        LeaveCriticalSection(&viewer->gfx.lock);
        break;
      }

      drdynvc_state = WTSVirtualChannelManagerGetDrdynvcState(viewer->gfx.vcm);
      if (drdynvc_state != viewer->gfx.drdynvc_state) {
        viewer->gfx.drdynvc_state = drdynvc_state;
        WLog_INFO(TAG, "Viewer %u drdynvc state changed -> %u", viewer->id,
                  (unsigned)drdynvc_state);
      }

      if ((drdynvc_state == DRDYNVC_STATE_READY) && viewer->gfx.rdpgfx &&
          !viewer->gfx.channel_opened &&
          !viewer->gfx.rdpgfx_temporarily_disabled) {
        WLog_INFO(TAG, "Viewer %u attempting RDPEGFX Open", viewer->id);
        if (!viewer->gfx.rdpgfx->Open(viewer->gfx.rdpgfx)) {
          viewer_disable_rdpgfx_locked(viewer);
          LeaveCriticalSection(&viewer->gfx.lock);
          WLog_WARN(TAG, "Viewer %u RDPEGFX Open failed", viewer->id);
          (void)viewer_gfx_enter_classic_fallback(
              g_viewer_server, viewer, now, "RDPEGFX channel open failed");
          continue;
        }
        viewer->gfx.channel_opened = TRUE;
        WLog_INFO(TAG, "Viewer %u RDPEGFX Open succeeded", viewer->id);
      }

      gfx_event = viewer->gfx.rdpgfx
                      ? rdpgfx_server_get_event_handle(viewer->gfx.rdpgfx)
                      : NULL;
    } else {
      gfx_event = NULL;
    }
    LeaveCriticalSection(&viewer->gfx.lock);

    if (gfx_event && (WaitForSingleObject(gfx_event, 0) == WAIT_OBJECT_0)) {
      EnterCriticalSection(&viewer->gfx.lock);
      if (viewer->gfx.rdpgfx && (rdpgfx_server_handle_messages(
                                     viewer->gfx.rdpgfx) != CHANNEL_RC_OK)) {
        viewer_disable_rdpgfx_locked(viewer);
        LeaveCriticalSection(&viewer->gfx.lock);
        WLog_WARN(TAG,
                  "Viewer %u RDPEGFX message handling failed; falling back to "
                  "classic path",
                  viewer->id);
        (void)viewer_gfx_enter_classic_fallback(
            g_viewer_server, viewer, now, "RDPEGFX message handling failed");
        continue;
      }
      LeaveCriticalSection(&viewer->gfx.lock);
    }

    if (g_viewer_server && !viewer_gfx_step_join(g_viewer_server, viewer, now))
      break;

    if (!viewer_pump_gfx(viewer))
      break;

    /* Drain the classic bitmap queue (Option B: async delivery) */
    if (!viewer_pump_classic(viewer))
      break;

    (void)viewer_forward_pointer(viewer, FALSE);

    if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer) &&
        peer->DrainOutputBuffer) {
      if (peer->DrainOutputBuffer(peer) < 0)
        break;
    }
  }

  if (g_viewer_server) {
    EnterCriticalSection(&g_viewer_server->lock);
    viewer_server_clear_input_owner_locked(
        g_viewer_server, viewer->id, "disconnected, input ownership cleared");
    viewer->connected = FALSE;
    viewer->activated = FALSE;
    viewer->needs_full_refresh = FALSE;
    viewer->full_refresh_deadline_ts = 0;
    viewer->peer = NULL;
    viewer->context = NULL;
    LeaveCriticalSection(&g_viewer_server->lock);
  }

  LOG_I("viewer_server", "Viewer peer disconnected (peer=%p)", (void *)peer);
  peer->Disconnect(peer);
  return 0;
}

static BOOL peer_post_connect(freerdp_peer *peer) {
  Viewer *viewer = find_viewer_by_peer(peer);
  RdpgfxServerContext *rdpgfx = NULL;
  BOOL gfx_enabled = FALSE;

  if (!viewer)
    return FALSE;

  viewer->connected = TRUE;
  viewer->activated = peer->activated;

  if (peer->context && peer->context->settings)
    gfx_enabled = freerdp_settings_get_bool(peer->context->settings,
                                            FreeRDP_SupportGraphicsPipeline);

  WLog_INFO(TAG, "Viewer %u peer_post_connect: GraphicsPipeline=%d", viewer->id,
            gfx_enabled);

  /* Force-disable GFX if the backend doesn't use it. The viewer-side
   * setting may not propagate correctly through FreeRDP's server
   * initialization. */
  if (g_viewer_server && g_viewer_server->backend &&
      g_viewer_server->backend->context &&
      g_viewer_server->backend->context->settings) {
    BOOL backend_gfx =
        freerdp_settings_get_bool(g_viewer_server->backend->context->settings,
                                  FreeRDP_SupportGraphicsPipeline);
    if (!backend_gfx)
      gfx_enabled = FALSE;
  }

  EnterCriticalSection(&viewer->gfx.lock);
  viewer_graphics_context_reset(
      &viewer->gfx, g_viewer_server ? g_viewer_server->backend : NULL);

  /* Create VCM here after peer->Initialize has populated context->rdp */
  viewer->gfx.vcm = WTSOpenServerA((LPSTR)peer->context);
  if (!viewer->gfx.vcm || (viewer->gfx.vcm == INVALID_HANDLE_VALUE)) {
    LeaveCriticalSection(&viewer->gfx.lock);
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
    LeaveCriticalSection(&viewer->gfx.lock);
    return TRUE;
  }

  rdpgfx = rdpgfx_server_context_new(viewer->gfx.vcm);
  if (!rdpgfx) {
    LeaveCriticalSection(&viewer->gfx.lock);
    return FALSE;
  }

  rdpgfx->custom = viewer;
  rdpgfx->rdpcontext = peer->context;
  rdpgfx->CapsAdvertise = viewer_rdpgfx_caps_advertise;
  rdpgfx->FrameAcknowledge = viewer_rdpgfx_frame_acknowledge;
  if (!rdpgfx->Initialize(rdpgfx, TRUE)) {
    rdpgfx_server_context_free(rdpgfx);
    LeaveCriticalSection(&viewer->gfx.lock);
    return FALSE;
  }

  viewer->gfx.rdpgfx = rdpgfx;
  viewer->gfx.post_connect_complete = TRUE;
  LeaveCriticalSection(&viewer->gfx.lock);
  return TRUE;
}

static BOOL peer_activate(freerdp_peer *peer) {
  Viewer *viewer = find_viewer_by_peer(peer);
  UINT64 now = platform_get_timestamp_ms();
  ViewerGfxNegotiationOutcome negotiation_outcome =
      VIEWER_GFX_NEGOTIATION_PENDING;

  if (!viewer)
    return FALSE;

  viewer->activated = TRUE;
  EnterCriticalSection(&viewer->send_lock);
  viewer->needs_full_refresh = TRUE;
  viewer->full_refresh_deadline_ts =
      platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;
  LeaveCriticalSection(&viewer->send_lock);

  EnterCriticalSection(&viewer->gfx.lock);
  viewer->gfx.ready = TRUE;
  negotiation_outcome = viewer->gfx.negotiation_outcome;
  if (viewer_gfx_negotiation_is_rdpgfx_ready(&viewer->gfx))
    viewer_gfx_begin_join_locked(viewer, now,
                                 "peer activated for RDPEGFX late join");
  else if (viewer_gfx_activation_waits_for_rdpgfx_caps(&viewer->gfx))
    viewer_gfx_begin_join_locked(
        viewer, now, "peer activated waiting for RDPEGFX caps confirmation");
  else {
    viewer->gfx.join_start_ts = now;
    viewer_gfx_finish_late_join_locked(viewer,
                                       "peer activated on classic path");
    /* Classic path doesn't need a full refresh gate. SurfaceBits
     * arrive continuously from the backend — just start receiving
     * them immediately. */
    viewer->needs_full_refresh = FALSE;
    viewer->full_refresh_deadline_ts = 0;
  }
  LeaveCriticalSection(&viewer->gfx.lock);
  viewer->last_pointer_position_generation = 0;
  viewer->last_pointer_shape_generation = 0;

  WLog_INFO(TAG, "Viewer %u connecting mid-session", viewer->id);
  if (negotiation_outcome == VIEWER_GFX_NEGOTIATION_RDPEGFX_READY)
    WLog_INFO(TAG,
              "Viewer %u RDPEGFX activated; evaluating frame-ring late join",
              viewer->id);

  if (g_viewer_server && g_viewer_server->backend) {
    if (negotiation_outcome == VIEWER_GFX_NEGOTIATION_RDPEGFX_READY) {
      WLog_INFO(
          TAG,
          "Viewer %u RDPEGFX activated; awaiting handshake-gated late join",
          viewer->id);
    } else if (backend_full_refresh_in_flight(g_viewer_server->backend)) {
      WLog_INFO(TAG, "Viewer %u activated, joining in-flight full refresh",
                viewer->id);
    } else {
      (void)backend_request_full_refresh(g_viewer_server->backend);
      WLog_INFO(TAG, "Viewer %u activated, queued full refresh", viewer->id);
    }
  }

  (void)viewer_forward_pointer(viewer, TRUE);
  return TRUE;
}

static BOOL peer_context_new(freerdp_peer *peer, rdpContext *context) {
  ViewerServer *server = g_viewer_server;
  Viewer *viewer = NULL;

  if (!server || !peer || !context)
    return FALSE;

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    if (!server->viewers[i].peer) {
      viewer = &server->viewers[i];
      viewer->id = i + 1;
      viewer->peer = peer;
      viewer->context = context;
      viewer->connected = FALSE;
      viewer->activated = FALSE;
      viewer->needs_full_refresh = FALSE;
      viewer->stop_requested = FALSE;
      viewer->connect_time = time(NULL);
      viewer->full_refresh_deadline_ts = 0;
      if (!viewer_send_state_init(viewer)) {
        viewer->peer = NULL;
        viewer->context = NULL;
        viewer = NULL;
        break;
      }
      viewer_graphics_context_init(&viewer->gfx);
      viewer_graphics_context_reset(&viewer->gfx, server->backend);
      /* VCM created later in peer_post_connect when context->rdp is ready */

      server->viewer_count++;
      break;
    }
  }
  LeaveCriticalSection(&server->lock);
  return viewer != NULL;
}

static void peer_context_free(freerdp_peer *peer, rdpContext *context) {
  ViewerServer *server = g_viewer_server;
  (void)context;

  if (!server)
    return;

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    if (server->viewers[i].peer == peer) {
      viewer_server_clear_input_owner_locked(
          server, server->viewers[i].id,
          "disconnected, input ownership cleared");
      server->viewers[i].stop_requested = TRUE;
      if (server->viewer_count > 0)
        server->viewer_count--;
      if (!server->viewers[i].thread ||
          WaitForSingleObject(server->viewers[i].thread, 0) == WAIT_OBJECT_0)
        viewer_cleanup_slot(&server->viewers[i]);
      break;
    }
  }
  LeaveCriticalSection(&server->lock);
}

/**
 * Re-apply server-side settings that were overwritten by GCC negotiation.
 *
 * When a viewer client connects, FreeRDP's gcc_read_client_core_data()
 * overwrites our server-side settings with the CLIENT's values:
 *   - DesktopWidth/Height → client's screen size (e.g. 1920×1080)
 *   - MonitorCount → client's monitor count (e.g. 1)
 *   - MonitorDefArray → client's monitor layout (e.g. single 1920×1080)
 *   - SupportMonitorLayoutPdu → AND'd with client's earlyCapabilityFlags
 *   - SupportDynamicTimeZone → AND'd with client's earlyCapabilityFlags
 *
 * We are the SERVER — we must restore our own desktop dimensions and
 * monitor layout so that:
 *   1. The Demand Active PDU advertises the correct desktop size (3840×1080)
 *   2. The Monitor Layout PDU sends the correct 2-monitor layout
 *   3. SupportDynamicTimeZone=TRUE so timezone data is consumed (fixes TPKT
 * error)
 *   4. SupportMonitorLayoutPdu=TRUE so the Monitor Layout PDU is sent
 *
 * This callback fires at CONNECTION_STATE_SECURE_SETTINGS_EXCHANGE,
 * which is AFTER GCC negotiation but BEFORE:
 *   - rdp_recv_client_info() (needs SupportDynamicTimeZone)
 *   - CAPABILITIES_EXCHANGE_DEMAND_ACTIVE (needs DesktopWidth/Height)
 *   - CAPABILITIES_EXCHANGE_MONITOR_LAYOUT (needs MonitorCount/DefArray +
 * SupportMonitorLayoutPdu)
 */
static BOOL peer_reached_state(freerdp_peer *peer, CONNECTION_STATE state) {
  ViewerServer *server = NULL;
  const MonitorLayout *layout = NULL;
  rdpSettings *settings = NULL;
  UINT32 i = 0;

  if (state != CONNECTION_STATE_SECURE_SETTINGS_EXCHANGE)
    return TRUE;

  if (!peer || !peer->context || !peer->context->settings)
    return TRUE;

  server = g_viewer_server;
  if (!server || !server->backend)
    return TRUE;

  settings = peer->context->settings;
  layout = &server->backend->monitor_layout;

  WLog_INFO(TAG, "peer_reached_state: Restoring server-side settings after GCC "
                 "negotiation");
  WLog_INFO(TAG,
            "peer_reached_state:   Before: DesktopWidth=%" PRIu32
            ", DesktopHeight=%" PRIu32 ", MonitorCount=%" PRIu32
            ", SupportMonitorLayoutPdu=%s, SupportDynamicTimeZone=%s",
            freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
            freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight),
            freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount),
            freerdp_settings_get_bool(settings, FreeRDP_SupportMonitorLayoutPdu)
                ? "TRUE"
                : "FALSE",
            freerdp_settings_get_bool(settings, FreeRDP_SupportDynamicTimeZone)
                ? "TRUE"
                : "FALSE");

  /* Restore desktop dimensions from backend layout */
  freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
                              layout->total_width);
  freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
                              layout->total_height);

  /* Restore monitor count and layout */
  freerdp_settings_set_uint32(settings, FreeRDP_MonitorCount,
                              layout->monitor_count);

  for (i = 0; i < layout->monitor_count; i++) {
    rdpMonitor mon = {0};
    mon.x = layout->monitors[i].left;
    mon.y = layout->monitors[i].top;
    mon.width = layout->monitors[i].right - layout->monitors[i].left;
    mon.height = layout->monitors[i].bottom - layout->monitors[i].top;
    mon.is_primary =
        (layout->monitors[i].flags & MONITOR_PRIMARY) ? TRUE : FALSE;
    mon.orig_screen = i;
    mon.attributes.physicalWidth = mon.width;
    mon.attributes.physicalHeight = mon.height;
    mon.attributes.orientation = ORIENTATION_LANDSCAPE;
    mon.attributes.desktopScaleFactor = 100;
    mon.attributes.deviceScaleFactor = 100;

    freerdp_settings_set_pointer_array(settings, FreeRDP_MonitorDefArray, i,
                                       &mon);

    WLog_INFO(TAG,
              "peer_reached_state:   monitor[%" PRIu32 "]: x=%" PRId32
              ", y=%" PRId32 ", width=%" PRId32 ", height=%" PRId32
              ", is_primary=%s",
              i, mon.x, mon.y, mon.width, mon.height,
              mon.is_primary ? "TRUE" : "FALSE");
  }

  /* Restore capability flags that were AND'd with client's flags */
  freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
  freerdp_settings_set_bool(settings, FreeRDP_SupportDynamicTimeZone, TRUE);

  WLog_INFO(TAG,
            "peer_reached_state:   After: DesktopWidth=%" PRIu32
            ", DesktopHeight=%" PRIu32 ", MonitorCount=%" PRIu32
            ", SupportMonitorLayoutPdu=TRUE, SupportDynamicTimeZone=TRUE",
            freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
            freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight),
            freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount));

  return TRUE;
}

static BOOL peer_accepted(freerdp_listener *listener, freerdp_peer *peer) {
  ViewerServer *server = g_viewer_server;
  Viewer *viewer = NULL;
  rdpSettings *settings = NULL;

  (void)listener;

  if (!server)
    return FALSE;

  peer->ContextNew = peer_context_new;
  peer->ContextFree = peer_context_free;
  peer->PostConnect = peer_post_connect;
  peer->Activate = peer_activate;
  peer->ReachedState = peer_reached_state;

  if (!freerdp_peer_context_new(peer))
    return FALSE;

  settings = peer->context ? peer->context->settings : NULL;
  if (settings) {
    UINT32 desktop_width = 0;
    UINT32 desktop_height = 0;
    viewer_get_backend_layout(server->backend, &desktop_width, &desktop_height,
                              NULL);

    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    freerdp_settings_set_uint32(settings, FreeRDP_EncryptionLevel,
                                ENCRYPTION_LEVEL_CLIENT_COMPATIBLE);
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_NSCodec, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, FALSE);
    /* SurfaceFrameMarkerEnabled must match codec availability. When
     * codecs are disabled, sending SURFACE_FRAME_MARKER PDUs to the
     * client causes a protocol error (0xd06) because the client
     * hasn't negotiated the Surface Bits Capability Set. */
    freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled,
                              FALSE);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, desktop_width);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
                                desktop_height);

    WLog_INFO(TAG, "peer_accepted: Setting desktop_size=%ux%u for viewer",
              desktop_width, desktop_height);

    /* Enable Monitor Layout PDU so the server advertises multi-monitor
     * layout to connecting viewers. Without this, the server only sends
     * a single-monitor desktop even if DesktopWidth > 1920. */
    freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);

    WLog_INFO(
        TAG,
        "peer_accepted: SupportMonitorLayoutPdu=TRUE, desktop_width=%" PRIu32
        " > 1920=%s",
        desktop_width, desktop_width > 1920 ? "TRUE" : "FALSE");

    /* Configure multi-monitor layout for viewer if backend uses more
     * than one monitor. On the server side, we must set MonitorCount
     * and MonitorDefArray (NOT UseMultimon/SpanMonitors which are
     * client-side only). */
    if (desktop_width > 1920) {
      if (server && server->backend) {
        const MonitorLayout *layout = &server->backend->monitor_layout;
        UINT32 mi = 0;

        WLog_INFO(TAG,
                  "peer_accepted: Configuring multi-monitor for viewer: "
                  "MonitorCount=%" PRIu32,
                  layout->monitor_count);

        freerdp_settings_set_uint32(settings, FreeRDP_MonitorCount,
                                    layout->monitor_count);

        for (mi = 0; mi < layout->monitor_count; mi++) {
          rdpMonitor mon = {0};
          mon.x = layout->monitors[mi].left;
          mon.y = layout->monitors[mi].top;
          mon.width = layout->monitors[mi].right - layout->monitors[mi].left;
          mon.height = layout->monitors[mi].bottom - layout->monitors[mi].top;
          mon.is_primary =
              (layout->monitors[mi].flags & MONITOR_PRIMARY) ? TRUE : FALSE;
          mon.orig_screen = mi;
          mon.attributes.physicalWidth = mon.width;
          mon.attributes.physicalHeight = mon.height;
          mon.attributes.orientation = ORIENTATION_LANDSCAPE;
          mon.attributes.desktopScaleFactor = 100;
          mon.attributes.deviceScaleFactor = 100;

          WLog_INFO(TAG,
                    "peer_accepted: viewer monitor[%" PRIu32 "]: x=%" PRId32
                    ", y=%" PRId32 ", width=%" PRId32 ", height=%" PRId32
                    ", is_primary=%s, orig_screen=%" PRIu32,
                    mi, mon.x, mon.y, mon.width, mon.height,
                    mon.is_primary ? "TRUE" : "FALSE", mon.orig_screen);

          freerdp_settings_set_pointer_array(settings, FreeRDP_MonitorDefArray,
                                             mi, &mon);
        }

        {
          UINT32 monCountAfter =
              freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
          UINT32 k = 0;
          WLog_INFO(TAG,
                    "peer_accepted: After setting MonitorDefArray: "
                    "MonitorCount=%" PRIu32,
                    monCountAfter);
          for (k = 0; k < monCountAfter; k++) {
            const rdpMonitor *m =
                (const rdpMonitor *)freerdp_settings_get_pointer_array(
                    settings, FreeRDP_MonitorDefArray, k);
            if (m)
              WLog_INFO(TAG,
                        "peer_accepted:   viewer settings[%" PRIu32
                        "]: x=%" PRId32 ", y=%" PRId32 ", width=%" PRId32
                        ", height=%" PRId32 ", is_primary=%s",
                        k, m->x, m->y, m->width, m->height,
                        m->is_primary ? "TRUE" : "FALSE");
          }
        }
      }
    }

    {
      const char *cert_file = (server->cert_path && server->cert_path[0])
                                  ? server->cert_path
                                  : platform_cert_path();
      const char *key_file = (server->key_path && server->key_path[0])
                                 ? server->key_path
                                 : platform_key_path();
      rdpCertificate *cert = freerdp_certificate_new_from_file(cert_file);
      rdpPrivateKey *key = freerdp_key_new_from_file(key_file);
      if (cert && key) {
        LOG_I("viewer_server", "TLS certificate loaded from '%s'", cert_file);
        freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate,
                                         cert, 1);
        freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key,
                                         1);
      } else {
        LOG_W("viewer_server", "Failed to load cert from '%s'/'%s'", cert_file,
              key_file);
      }
    }
  }

  if (!peer->Initialize(peer))
    return FALSE;

  if (peer->context && peer->context->input) {
    peer->context->input->MouseEvent = on_mouse_event;
    peer->context->input->ExtendedMouseEvent = on_extended_mouse_event;
    peer->context->input->KeyboardEvent = on_keyboard_event;
  }

  viewer = find_viewer_by_peer(peer);
  if (!viewer)
    return FALSE;

  viewer->thread = CreateThread(NULL, 0, viewer_handle_peer, viewer, 0, NULL);
  return viewer->thread != NULL;
}

ViewerServer *viewer_server_init(const char *bind_address, UINT16 port,
                                 BackendClient *backend, const char *cert_path,
                                 const char *key_path) {
  ViewerServer *server = calloc(1, sizeof(ViewerServer));
  if (!server)
    return NULL;

  server->listener = freerdp_listener_new();
  if (!server->listener) {
    free(server);
    return NULL;
  }

  server->listener->info = server;
  server->listener->PeerAccepted = peer_accepted;
  server->port = port ? port : 3389;
  server->bind_address =
      bind_address ? strdup(bind_address) : strdup("0.0.0.0");
  server->backend = backend;
  server->cert_path = cert_path ? _strdup(cert_path) : NULL;
  server->key_path = key_path ? _strdup(key_path) : NULL;
  if (backend)
    server->monitor_layout = backend->monitor_layout;
  server->slow_viewer_disconnect_enabled = TRUE;
  server->slow_viewer_disconnect_ms = 30000;
  InitializeCriticalSection(&server->lock);
  viewer_gfx_publisher_state_init(&server->gfx);
  g_viewer_server = server;
  return server;
}

BOOL viewer_server_start(ViewerServer *server) {
  if (!server || !server->listener)
    return FALSE;

  if (!server->listener->Open(server->listener, server->bind_address,
                              server->port))
    return FALSE;

  LOG_I("viewer_server", "Viewer server listening on %s:%u",
        server->bind_address, server->port);

  server->running = TRUE;
  while (server->running) {
    if (!server->listener->CheckFileDescriptor(server->listener)) {
      server->running = FALSE;
      break;
    }
    platform_sleep_ms(1);
  }

  return TRUE;
}

void viewer_server_stop(ViewerServer *server) {
  freerdp_peer *peers[MAX_VIEWERS] = {0};

  if (!server)
    return;

  LOG_I("viewer_server", "Viewer server stopped");

  EnterCriticalSection(&server->lock);
  server->running = FALSE;
  for (int i = 0; i < MAX_VIEWERS; i++) {
    if (server->viewers[i].peer) {
      server->viewers[i].stop_requested = TRUE;
      peers[i] = server->viewers[i].peer;
    }
  }
  LeaveCriticalSection(&server->lock);

  if (server->listener)
    server->listener->Close(server->listener);

  for (int i = 0; i < MAX_VIEWERS; i++) {
    if (peers[i])
      peers[i]->Disconnect(peers[i]);
  }
}

void viewer_server_free(ViewerServer *server) {
  if (!server)
    return;

  viewer_server_stop(server);
  for (int i = 0; i < MAX_VIEWERS; i++)
    viewer_join_thread(&server->viewers[i]);

  g_viewer_server = NULL;

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++)
    viewer_cleanup_slot(&server->viewers[i]);
  server->viewer_count = 0;
  LeaveCriticalSection(&server->lock);

  if (server->listener)
    freerdp_listener_free(server->listener);
  free(server->bind_address);
  viewer_gfx_publisher_state_uninit(&server->gfx);
  DeleteCriticalSection(&server->lock);
  free(server);
}

UINT32
viewer_server_get_count(ViewerServer *server) {
  UINT32 count = 0;
  if (!server)
    return 0;

  EnterCriticalSection(&server->lock);
  count = server->viewer_count;
  LeaveCriticalSection(&server->lock);
  return count;
}

void viewer_server_notify_backend_layout_change(BackendClient *backend,
                                                UINT32 width, UINT32 height,
                                                UINT32 generation) {
  ViewerServer *server = g_viewer_server;
  UINT32 queued_viewers = 0;
  UINT64 deadline = platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;

  if (!server || (server->backend != backend) || (width == 0) || (height == 0))
    return;

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    Viewer *viewer = &server->viewers[i];
    if (!viewer->peer || !viewer->gfx.initialized)
      continue;

    EnterCriticalSection(&viewer->gfx.lock);
    viewer->gfx.negotiated_width = width;
    viewer->gfx.negotiated_height = height;
    LeaveCriticalSection(&viewer->gfx.lock);

    EnterCriticalSection(&viewer->send_lock);
    viewer->needs_full_refresh = TRUE;
    viewer->full_refresh_deadline_ts = deadline;
    LeaveCriticalSection(&viewer->send_lock);
    queued_viewers++;

    if (viewer->peer->context && viewer->peer->context->settings) {
      rdpSettings *vs = viewer->peer->context->settings;

      WLog_INFO(TAG,
                "viewer_server_notify_backend_layout_change: viewer[%d]: "
                "setting DesktopWidth=%" PRIu32 ", DesktopHeight=%" PRIu32,
                i, width, height);

      freerdp_settings_set_uint32(vs, FreeRDP_DesktopWidth, width);
      freerdp_settings_set_uint32(vs, FreeRDP_DesktopHeight, height);

      /* Update multi-monitor layout if width exceeds single monitor.
       * On the server side, set MonitorCount and MonitorDefArray
       * (NOT UseMultimon/SpanMonitors which are client-side only). */
      if (width > 1920 && server && server->backend) {
        const MonitorLayout *layout = &server->backend->monitor_layout;
        UINT32 mi = 0;

        WLog_INFO(TAG,
                  "viewer_server_notify_backend_layout_change: viewer[%d]: "
                  "multi-monitor layout: MonitorCount=%" PRIu32,
                  i, layout->monitor_count);

        freerdp_settings_set_uint32(vs, FreeRDP_MonitorCount,
                                    layout->monitor_count);

        for (mi = 0; mi < layout->monitor_count; mi++) {
          rdpMonitor mon = {0};
          mon.x = layout->monitors[mi].left;
          mon.y = layout->monitors[mi].top;
          mon.width = layout->monitors[mi].right - layout->monitors[mi].left;
          mon.height = layout->monitors[mi].bottom - layout->monitors[mi].top;
          mon.is_primary =
              (layout->monitors[mi].flags & MONITOR_PRIMARY) ? TRUE : FALSE;
          mon.orig_screen = mi;
          mon.attributes.physicalWidth = mon.width;
          mon.attributes.physicalHeight = mon.height;
          mon.attributes.orientation = ORIENTATION_LANDSCAPE;
          mon.attributes.desktopScaleFactor = 100;
          mon.attributes.deviceScaleFactor = 100;

          WLog_INFO(TAG,
                    "viewer_server_notify_backend_layout_change: viewer[%d]: "
                    "monitor[%" PRIu32 "]: x=%" PRId32 ", y=%" PRId32
                    ", width=%" PRId32 ", height=%" PRId32 ", is_primary=%s",
                    i, mi, mon.x, mon.y, mon.width, mon.height,
                    mon.is_primary ? "TRUE" : "FALSE");

          freerdp_settings_set_pointer_array(vs, FreeRDP_MonitorDefArray, mi,
                                             &mon);
        }
      }
    }
  }
  LeaveCriticalSection(&server->lock);

  if (queued_viewers > 0)
    (void)backend_request_full_refresh(server->backend);

  WLog_INFO(TAG,
            "Backend layout generation=%" PRIu32
            " propagated to viewers as %ux%u; Backend layout change, queued "
            "full refresh for %u viewers",
            generation, width, height, queued_viewers);
}

BOOL viewer_server_publish_surface_bits(BackendClient *backend,
                                        const SURFACE_BITS_COMMAND *cmd) {
  ViewerServer *server = g_viewer_server;
  Viewer *targets[MAX_VIEWERS] = {0};
  size_t target_count = 0;
  BOOL sent_any = FALSE;
  BOOL refresh_in_flight = FALSE;
  UINT32 classic_target_count = 0;
  UINT32 gated_full_refresh_count = 0;
  UINT32 throttled_count = 0;
  UINT32 enqueue_failed_count = 0;
  UINT32 enqueued_count = 0;
  UINT32 first_gated_viewer_id = 0;
  UINT32 first_throttled_viewer_id = 0;
  UINT64 publish_started_us = 0;
  UINT64 publish_us = 0;

  if (!server || (server->backend != backend) || !cmd)
    return FALSE;

  refresh_in_flight = backend_full_refresh_in_flight(server->backend);

  publish_started_us = viewer_perf_now_us();

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    Viewer *viewer = &server->viewers[i];
    BOOL classic_fallback = FALSE;
    if (!viewer->peer || !viewer->connected || !viewer->activated)
      continue;

    EnterCriticalSection(&viewer->gfx.lock);
    classic_fallback = viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx);
    LeaveCriticalSection(&viewer->gfx.lock);
    if (!classic_fallback)
      continue;

    targets[target_count++] = viewer;
    classic_target_count++;
  }
  LeaveCriticalSection(&server->lock);

  for (size_t i = 0; i < target_count; i++) {
    Viewer *viewer = targets[i];
    BOOL ready_to_send = FALSE;
    BOOL enqueued = FALSE;
    BOOL throttled = FALSE;
    ViewerSurfaceBitsEvent *event = NULL;

    /* Pre-build the deep copy outside send_lock to minimize lock hold time */
    EnterCriticalSection(&viewer->send_lock);
    ready_to_send = viewer->peer && viewer->connected && viewer->activated;
    LeaveCriticalSection(&viewer->send_lock);

    if (!ready_to_send)
      continue;

    if (viewer->consecutive_lag_intervals >= VIEWER_THROTTLE_LAG_INTERVALS) {
      /* Per-viewer throttle: skip updates for slow viewers.
       * However, SurfaceBits ARE the refresh data — even when throttled,
       * we must still deliver them to allow the viewer to resync.
       * Clear the throttle gate and enqueue. */
      EnterCriticalSection(&viewer->send_lock);
      viewer->surface_bits_updates_skipped_throttle++;
      /* Don't set needs_full_refresh for SurfaceBits — they ARE the
       * refresh data. Setting it would cause the pump to drop them. */
      LeaveCriticalSection(&viewer->send_lock);
      throttled = TRUE;
      throttled_count++;
      if (first_throttled_viewer_id == 0)
        first_throttled_viewer_id = viewer->id;
      /* Fall through to enqueue — SurfaceBits must be delivered even
       * for throttled viewers, because they carry the actual pixel data
       * needed to resync. */
    }

    /* Clear needs_full_refresh if set — SurfaceBits ARE the refresh data.
     * Unlike BitmapUpdate where a full refresh is a separate mechanism,
     * SurfaceBits tiles are the only way the viewer receives pixel data,
     * so they must always be delivered. */
    if (viewer->needs_full_refresh) {
      EnterCriticalSection(&viewer->send_lock);
      viewer->needs_full_refresh = FALSE;
      viewer->full_refresh_deadline_ts = 0;
      LeaveCriticalSection(&viewer->send_lock);
      gated_full_refresh_count++;
      if (first_gated_viewer_id == 0)
        first_gated_viewer_id = viewer->id;
    }

    /* Always enqueue SurfaceBits — they carry pixel data that the viewer
     * needs regardless of throttle or refresh state. */
    event = viewer_surface_bits_event_new(cmd);
    if (!event) {
      enqueue_failed_count++;
      continue;
    }
    EnterCriticalSection(&viewer->send_lock);
    enqueued = viewer_surface_bits_enqueue_event_locked(viewer, event);
    LeaveCriticalSection(&viewer->send_lock);
    if (enqueued)
      enqueued_count++;
    else {
      enqueue_failed_count++;
      viewer_surface_bits_event_free(event);
    }

    if (enqueued)
      sent_any = TRUE;

    if (throttled)
      (void)backend_request_full_refresh(server->backend);
  }

  publish_us = viewer_perf_now_us() - publish_started_us;

  if ((classic_target_count == 0) || (gated_full_refresh_count > 0) ||
      (throttled_count > 0) || (enqueue_failed_count > 0) ||
      (enqueued_count == 0)) {
    WLog_INFO(TAG,
              "Classic SurfaceBits publish summary rect=(%u,%u)-(%u,%u) "
              "payload=%" PRIu32 " codecId=%" PRIu16 " classicTargets=%" PRIu32
              " enqueued=%" PRIu32 " gatedFullRefresh=%" PRIu32
              " firstGatedViewer=%" PRIu32 " throttled=%" PRIu32
              " firstThrottledViewer=%" PRIu32 " enqueueFailed=%" PRIu32
              " refreshInFlight=%d publishUs=%" PRIu64,
              cmd->destLeft, cmd->destTop, cmd->destRight, cmd->destBottom,
              cmd->bmp.bitmapDataLength, cmd->bmp.codecID, classic_target_count,
              enqueued_count, gated_full_refresh_count, first_gated_viewer_id,
              throttled_count, first_throttled_viewer_id, enqueue_failed_count,
              refresh_in_flight, publish_us);
  }

  return sent_any;
}

BOOL viewer_server_publish_bitmap_update(BackendClient *backend,
                                         const BITMAP_UPDATE *bitmap) {
  ViewerServer *server = g_viewer_server;
  Viewer *targets[MAX_VIEWERS] = {0};
  size_t target_count = 0;
  BOOL sent_any = FALSE;
  BOOL refresh_in_flight = FALSE;
  UINT32 classic_target_count = 0;
  UINT32 gated_full_refresh_count = 0;
  UINT32 throttled_count = 0;
  UINT32 enqueue_failed_count = 0;
  UINT32 enqueued_count = 0;
  UINT32 first_gated_viewer_id = 0;
  UINT32 first_throttled_viewer_id = 0;
  UINT32 first_rect_payload = 0;
  UINT64 total_payload_bytes = 0;
  UINT64 publish_started_us = 0;
  UINT64 publish_us = 0;
  UINT32 rect_index = 0;

  if (!server || (server->backend != backend) || !bitmap)
    return FALSE;

  refresh_in_flight = backend_full_refresh_in_flight(server->backend);
  if (bitmap->number > 0)
    first_rect_payload = bitmap->rectangles[0].bitmapLength;
  for (rect_index = 0; rect_index < bitmap->number; rect_index++)
    total_payload_bytes += bitmap->rectangles[rect_index].bitmapLength;

  publish_started_us = viewer_perf_now_us();

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    Viewer *viewer = &server->viewers[i];
    BOOL classic_fallback = FALSE;
    if (!viewer->peer || !viewer->connected || !viewer->activated)
      continue;

    EnterCriticalSection(&viewer->gfx.lock);
    classic_fallback = viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx);
    LeaveCriticalSection(&viewer->gfx.lock);
    if (!classic_fallback)
      continue;

    targets[target_count++] = viewer;
    classic_target_count++;
  }
  LeaveCriticalSection(&server->lock);

  for (size_t i = 0; i < target_count; i++) {
    Viewer *viewer = targets[i];
    BOOL ready_to_send = FALSE;
    BOOL enqueued = FALSE;
    BOOL throttled = FALSE;
    ViewerClassicEvent *event = NULL;

    /* Pre-build the deep copy outside send_lock to minimize lock hold time.
     * The bitmap pointer is const and immutable during this call. */
    EnterCriticalSection(&viewer->send_lock);
    ready_to_send = viewer->peer && viewer->connected && viewer->activated;
    LeaveCriticalSection(&viewer->send_lock);

    if (!ready_to_send)
      continue;

    if (viewer->needs_full_refresh && !refresh_in_flight) {
      /* The viewer needs a full refresh but no refresh is in flight.
       * This happens when:
       * 1. The viewer just joined and the backend refresh has already
       *    completed (backend_mark_full_refresh_complete was called)
       * 2. The viewer was throttled and the refresh completed
       * In the classic path there are no frame markers to clear
       * needs_full_refresh, so we clear it here and enqueue the
       * current bitmap update. The viewer will receive this and
       * subsequent updates normally. */
      EnterCriticalSection(&viewer->send_lock);
      viewer->needs_full_refresh = FALSE;
      viewer->full_refresh_deadline_ts = 0;
      LeaveCriticalSection(&viewer->send_lock);
      gated_full_refresh_count++;
      if (first_gated_viewer_id == 0)
        first_gated_viewer_id = viewer->id;

      /* Deep copy outside lock, then enqueue under lock */
      event = viewer_classic_event_new(bitmap);
      if (!event) {
        enqueue_failed_count++;
        continue;
      }
      EnterCriticalSection(&viewer->send_lock);
      enqueued = viewer_classic_enqueue_event_locked(viewer, event);
      LeaveCriticalSection(&viewer->send_lock);
      if (enqueued)
        enqueued_count++;
      else {
        enqueue_failed_count++;
        viewer_classic_event_free(event);
      }
    } else if (viewer->needs_full_refresh && refresh_in_flight) {
      /* A full refresh is in flight — this bitmap IS the refresh data.
       * Clear the gate and enqueue so the viewer receives it. */
      EnterCriticalSection(&viewer->send_lock);
      viewer->needs_full_refresh = FALSE;
      viewer->full_refresh_deadline_ts = 0;
      LeaveCriticalSection(&viewer->send_lock);

      event = viewer_classic_event_new(bitmap);
      if (!event) {
        enqueue_failed_count++;
        continue;
      }
      EnterCriticalSection(&viewer->send_lock);
      enqueued = viewer_classic_enqueue_event_locked(viewer, event);
      LeaveCriticalSection(&viewer->send_lock);
      if (enqueued)
        enqueued_count++;
      else {
        enqueue_failed_count++;
        viewer_classic_event_free(event);
      }
    } else if (viewer->consecutive_lag_intervals >=
               VIEWER_THROTTLE_LAG_INTERVALS) {
      /* Per-viewer throttle: skip updates for slow viewers.
       * They will resync via full refresh when they recover. */
      EnterCriticalSection(&viewer->send_lock);
      viewer->bitmap_updates_skipped_throttle++;
      viewer->needs_full_refresh = TRUE;
      viewer->full_refresh_deadline_ts =
          platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;
      LeaveCriticalSection(&viewer->send_lock);
      throttled = TRUE;
      throttled_count++;
      if (first_throttled_viewer_id == 0)
        first_throttled_viewer_id = viewer->id;
    } else {
      /* Option B: Enqueue the bitmap update for async delivery
       * by the viewer thread. Deep copy outside lock, then
       * enqueue under lock to minimize send_lock hold time. */
      event = viewer_classic_event_new(bitmap);
      if (!event) {
        enqueue_failed_count++;
        continue;
      }
      EnterCriticalSection(&viewer->send_lock);
      enqueued = viewer_classic_enqueue_event_locked(viewer, event);
      LeaveCriticalSection(&viewer->send_lock);
      if (enqueued)
        enqueued_count++;
      else {
        enqueue_failed_count++;
        viewer_classic_event_free(event);
      }
    }

    if (enqueued)
      sent_any = TRUE;

    if (throttled)
      (void)backend_request_full_refresh(server->backend);
  }

  publish_us = viewer_perf_now_us() - publish_started_us;

  if ((classic_target_count == 0) || (gated_full_refresh_count > 0) ||
      (throttled_count > 0) || (enqueue_failed_count > 0) ||
      (enqueued_count == 0) ||
      viewer_should_log_bitmap_perf(backend->bitmap_update_batches_total + 1ULL,
                                    publish_us, enqueue_failed_count)) {
    WLog_INFO(TAG,
              "Classic BitmapUpdate publish summary batch=%" PRIu64
              " rectangles=%" PRIu32 " payload=%" PRIu64
              " skipCompression=%d firstRectPayload=%" PRIu32
              " classicTargets=%" PRIu32 " enqueued=%" PRIu32
              " gatedFullRefresh=%" PRIu32 " firstGatedViewer=%" PRIu32
              " throttled=%" PRIu32 " firstThrottledViewer=%" PRIu32
              " enqueueFailed=%" PRIu32
              " refreshInFlight=%d publishUs=%" PRIu64,
              backend->bitmap_update_batches_total + 1ULL, bitmap->number,
              total_payload_bytes, bitmap->skipCompression, first_rect_payload,
              classic_target_count, enqueued_count, gated_full_refresh_count,
              first_gated_viewer_id, throttled_count, first_throttled_viewer_id,
              enqueue_failed_count, refresh_in_flight, publish_us);
  }

  return sent_any;
}

BOOL viewer_server_publish_frame_marker(BackendClient *backend,
                                        const SURFACE_FRAME_MARKER *marker) {
  ViewerServer *server = g_viewer_server;
  Viewer *targets[MAX_VIEWERS] = {0};
  size_t target_count = 0;
  BOOL sent_any = FALSE;
  BOOL refresh_in_flight = FALSE;
  UINT32 completed_viewers = 0;
  BOOL pending_viewers_remain = FALSE;

  if (!server || (server->backend != backend) || !marker)
    return FALSE;

  refresh_in_flight = backend_full_refresh_in_flight(server->backend);

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    Viewer *viewer = &server->viewers[i];
    BOOL classic_fallback = FALSE;
    if (!viewer->peer || !viewer->connected || !viewer->activated)
      continue;

    EnterCriticalSection(&viewer->gfx.lock);
    classic_fallback = viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx);
    LeaveCriticalSection(&viewer->gfx.lock);
    if (!classic_fallback)
      continue;

    targets[target_count++] = viewer;
  }
  LeaveCriticalSection(&server->lock);

  /* Only forward frame markers to viewers that have negotiated codec support
   * (RemoteFX or NSCodec). When codecs are disabled, the client doesn't
   * expect SURFACE_FRAME_MARKER PDUs and treats them as a protocol error
   * (error 0xd06). Frame markers are only meaningful when SurfaceBits
   * codec data is being sent — they delimit frame boundaries for codec
   * tile streams. With uncompressed BitmapUpdate, frame markers are
   * unnecessary and harmful. */
  for (size_t i = 0; i < target_count; i++) {
    Viewer *viewer = targets[i];
    rdpSettings *settings =
        viewer->peer
            ? viewer->peer->context ? viewer->peer->context->settings : NULL
            : NULL;
    BOOL has_codec = FALSE;

    if (settings) {
      has_codec = freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec) ||
                  freerdp_settings_get_bool(settings, FreeRDP_NSCodec);
    }

    if (!has_codec)
      continue;

    EnterCriticalSection(&viewer->send_lock);
    if (viewer->peer && viewer->connected && viewer->activated &&
        viewer_send_frame_marker(viewer, marker))
      sent_any = TRUE;
    LeaveCriticalSection(&viewer->send_lock);
  }

  if (marker->frameAction == SURFACECMD_FRAMEACTION_END) {
    for (size_t i = 0; i < target_count; i++) {
      Viewer *viewer = targets[i];

      EnterCriticalSection(&viewer->send_lock);
      if (viewer->needs_full_refresh) {
        viewer->needs_full_refresh = FALSE;
        viewer->full_refresh_deadline_ts = 0;
        completed_viewers++;
      }
      LeaveCriticalSection(&viewer->send_lock);

      EnterCriticalSection(&viewer->gfx.lock);
      if (viewer->gfx.join_strategy == VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK)
        viewer_gfx_finish_late_join_locked(
            viewer, "classic fallback full refresh completed");
      LeaveCriticalSection(&viewer->gfx.lock);
    }

    /* Only check classic-fallback targets for pending refresh needs.
     * GFX viewers manage their own refresh lifecycle and should not
     * block classic refresh completion. Viewers that were not targets
     * of this frame marker (e.g., newly joined or throttled viewers)
     * should also not block completion — they will request their own
     * refresh cycle independently. */
    for (size_t i = 0; i < target_count; i++) {
      Viewer *viewer = targets[i];

      EnterCriticalSection(&viewer->send_lock);
      if (viewer->needs_full_refresh)
        pending_viewers_remain = TRUE;
      LeaveCriticalSection(&viewer->send_lock);

      if (pending_viewers_remain)
        break;
    }

    if (!pending_viewers_remain)
      backend_mark_full_refresh_complete(server->backend);

    WLog_INFO(TAG, "Full refresh completed for %u viewers (frame id=%u)",
              completed_viewers, marker->frameId);
  }

  return sent_any;
}

BOOL viewer_server_publish_gfx_reset_graphics(
    BackendClient *backend, const RDPGFX_RESET_GRAPHICS_PDU *reset_graphics) {
  ViewerServer *server = g_viewer_server;
  ViewerGfxEvent *event = NULL;

  if (!server || (server->backend != backend) || !reset_graphics)
    return FALSE;

  event = viewer_gfx_event_new_reset_graphics(reset_graphics);
  if (!event)
    return FALSE;

  WLog_INFO(TAG,
            "ResetGraphics width=%" PRIu32 " height=%" PRIu32
            " monitors=%" PRIu32 " clearing frame ring and capture state",
            reset_graphics->width, reset_graphics->height,
            reset_graphics->monitorCount);

  EnterCriticalSection(&server->gfx.lock);
  viewer_gfx_publisher_state_reset_locked(&server->gfx);
  if (!viewer_gfx_reset_graphics_pdu_copy(&server->gfx.latest_reset_graphics,
                                          reset_graphics)) {
    WLog_ERR(TAG, "ResetGraphics copy failed; frame ring cleared and replay "
                  "state invalidated");
    LeaveCriticalSection(&server->gfx.lock);
    viewer_gfx_event_unref(event);
    return FALSE;
  }
  server->gfx.has_latest_reset_graphics = TRUE;
  LeaveCriticalSection(&server->gfx.lock);

  return viewer_server_publish_gfx_event(backend, event);
}

BOOL viewer_server_publish_gfx_create_surface(
    BackendClient *backend, const RDPGFX_CREATE_SURFACE_PDU *create_surface) {
  ViewerServer *server = g_viewer_server;
  ViewerGraphicsSurfaceState *surface = NULL;
  ViewerGfxEvent *event = NULL;

  if (!server || (server->backend != backend) || !create_surface)
    return FALSE;

  event = viewer_gfx_event_new_simple(VIEWER_GFX_EVENT_CREATE_SURFACE,
                                      create_surface, sizeof(*create_surface));
  if (!event)
    return FALSE;

  WLog_INFO(TAG,
            "CreateSurface surfaceId=%" PRIu16 " size=%" PRIu16 "x%" PRIu16
            " format=0x%04" PRIX16,
            create_surface->surfaceId, create_surface->width,
            create_surface->height, create_surface->pixelFormat);

  EnterCriticalSection(&server->gfx.lock);
  surface =
      viewer_gfx_upsert_surface_locked(&server->gfx, create_surface->surfaceId);
  if (surface) {
    surface->create_surface = *create_surface;
    surface->mapped = FALSE;
    memset(&surface->map_surface_to_output, 0,
           sizeof(surface->map_surface_to_output));
  }
  LeaveCriticalSection(&server->gfx.lock);

  return viewer_server_publish_gfx_event(backend, event);
}

BOOL viewer_server_publish_gfx_delete_surface(
    BackendClient *backend, const RDPGFX_DELETE_SURFACE_PDU *delete_surface) {
  ViewerServer *server = g_viewer_server;
  ViewerGfxEvent *event = NULL;

  if (!server || (server->backend != backend) || !delete_surface)
    return FALSE;

  event = viewer_gfx_event_new_simple(VIEWER_GFX_EVENT_DELETE_SURFACE,
                                      delete_surface, sizeof(*delete_surface));
  if (!event)
    return FALSE;

  WLog_INFO(TAG, "DeleteSurface surfaceId=%" PRIu16, delete_surface->surfaceId);

  EnterCriticalSection(&server->gfx.lock);
  viewer_gfx_remove_surface_locked(&server->gfx, delete_surface->surfaceId);
  LeaveCriticalSection(&server->gfx.lock);

  return viewer_server_publish_gfx_event(backend, event);
}

BOOL viewer_server_publish_gfx_map_surface_to_output(
    BackendClient *backend,
    const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU *map_surface_to_output) {
  ViewerServer *server = g_viewer_server;
  ViewerGraphicsSurfaceState *surface = NULL;
  ViewerGfxEvent *event = NULL;

  if (!server || (server->backend != backend) || !map_surface_to_output)
    return FALSE;

  event = viewer_gfx_event_new_simple(VIEWER_GFX_EVENT_MAP_SURFACE_TO_OUTPUT,
                                      map_surface_to_output,
                                      sizeof(*map_surface_to_output));
  if (!event)
    return FALSE;

  WLog_INFO(TAG,
            "MapSurfaceToOutput surfaceId=%" PRIu16 " origin=(%" PRIu32
            ",%" PRIu32 ")",
            map_surface_to_output->surfaceId,
            map_surface_to_output->outputOriginX,
            map_surface_to_output->outputOriginY);

  EnterCriticalSection(&server->gfx.lock);
  surface = viewer_gfx_upsert_surface_locked(&server->gfx,
                                             map_surface_to_output->surfaceId);
  if (surface) {
    surface->map_surface_to_output = *map_surface_to_output;
    surface->mapped = TRUE;
  }
  LeaveCriticalSection(&server->gfx.lock);

  return viewer_server_publish_gfx_event(backend, event);
}

BOOL viewer_server_publish_gfx_start_frame(
    BackendClient *backend, const RDPGFX_START_FRAME_PDU *start_frame) {
  ViewerServer *server = g_viewer_server;
  ViewerGfxEvent *event = NULL;

  if (!server || (server->backend != backend) || !start_frame)
    return FALSE;

  event = viewer_gfx_event_new_simple(VIEWER_GFX_EVENT_START_FRAME, start_frame,
                                      sizeof(*start_frame));
  if (!event)
    return FALSE;

  WLog_INFO(TAG, "StartFrame received frameId=%" PRIu32 " timestamp=%" PRIu64,
            start_frame->frameId, platform_get_timestamp_ms());

  EnterCriticalSection(&server->gfx.lock);
  server->gfx.in_frame = TRUE;
  server->gfx.current_frame_id = start_frame->frameId;
  if (!viewer_gfx_frame_buffer_begin_frame_locked(&server->gfx.frame_buffer,
                                                  start_frame, event)) {
    WLog_ERR(TAG,
             "Frame %" PRIu32 " capture failed; live forwarding continues but "
             "replay buffer unavailable",
             start_frame->frameId);
  }
  LeaveCriticalSection(&server->gfx.lock);

  return viewer_server_publish_gfx_event(backend, event);
}

BOOL viewer_server_publish_gfx_surface_command(
    BackendClient *backend, const RDPGFX_SURFACE_COMMAND *cmd) {
  ViewerServer *server = g_viewer_server;
  ViewerGfxEvent *event = NULL;
  UINT32 frame_id = 0;

  if (!server || (server->backend != backend) || !cmd)
    return FALSE;

  event = viewer_gfx_event_new_surface_command(cmd);
  if (!event)
    return FALSE;

  EnterCriticalSection(&server->gfx.lock);
  frame_id = server->gfx.current_frame_id;
  WLog_INFO(TAG,
            "SurfaceCommand frameId=%" PRIu32 " surfaceId=%" PRIu16
            " codecId=%" PRIu16 " length=%" PRIu32,
            frame_id, cmd->surfaceId, cmd->codecId, cmd->length);
  if (server->gfx.in_frame && server->gfx.frame_buffer.capture_frame) {
    (void)viewer_gfx_frame_buffer_append_surface_command_locked(
        &server->gfx.frame_buffer, cmd, event);
  } else {
    WLog_WARN(TAG,
              "SurfaceCommand surfaceId=%" PRIu16 " length=%" PRIu32
              " arrived outside active frame; not captured in frame ring",
              cmd->surfaceId, cmd->length);
  }
  LeaveCriticalSection(&server->gfx.lock);

  return viewer_server_publish_gfx_event(backend, event);
}

BOOL viewer_server_publish_gfx_end_frame(
    BackendClient *backend, const RDPGFX_END_FRAME_PDU *end_frame) {
  ViewerServer *server = g_viewer_server;
  ViewerGfxEvent *event = NULL;
  ViewerGfxCompleteFrame *latest = NULL;

  if (!server || (server->backend != backend) || !end_frame)
    return FALSE;

  event = viewer_gfx_event_new_simple(VIEWER_GFX_EVENT_END_FRAME, end_frame,
                                      sizeof(*end_frame));
  if (!event)
    return FALSE;

  EnterCriticalSection(&server->gfx.lock);
  WLog_INFO(TAG, "EndFrame received frameId=%" PRIu32, end_frame->frameId);
  if (viewer_gfx_frame_buffer_end_frame_locked(&server->gfx.frame_buffer,
                                               end_frame, event)) {
    latest = viewer_gfx_frame_buffer_latest_locked(&server->gfx.frame_buffer);
    if (latest) {
      WLog_INFO(TAG,
                "Frame %" PRIu32 " complete: %" PRIu32 " events, %" PRIu32
                " commands, %" PRIu64 " bytes total",
                latest->frame_id, latest->event_count,
                latest->surface_command_count, latest->total_payload_bytes);
    }
  }
  if (server->gfx.in_frame &&
      (server->gfx.current_frame_id == end_frame->frameId)) {
    server->gfx.in_frame = FALSE;
    server->gfx.current_frame_id = 0;
  }
  LeaveCriticalSection(&server->gfx.lock);

  if (latest) {
    viewer_gfx_complete_frame_unref(latest);
    EnterCriticalSection(&server->lock);
    for (int i = 0; i < MAX_VIEWERS; i++) {
      Viewer *viewer = &server->viewers[i];

      if (!viewer->peer || !viewer->connected || !viewer->activated ||
          viewer->stop_requested)
        continue;

      EnterCriticalSection(&viewer->gfx.lock);
      if ((viewer->gfx.join_state == VIEWER_JOIN_STATE_WAIT_NEXT_SAFE_FRAME) ||
          (viewer->gfx.join_state == VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH)) {
        WLog_INFO(TAG,
                  "Viewer %u observed completed frame %" PRIu32
                  " while waiting for replay-safe baseline",
                  viewer->id, end_frame->frameId);
      }
      LeaveCriticalSection(&viewer->gfx.lock);
    }
    LeaveCriticalSection(&server->lock);
  }

  /* GFX-based backend refresh completion: since the backend uses RDPEGFX
   * exclusively and never sends classic frame markers, the backend full
   * refresh mechanism (which depends on frame markers to complete) would
   * be permanently stuck. When a GFX EndFrame arrives and a refresh is
   * in flight, mark it complete through the GFX path so that viewers
   * waiting in WAIT_BACKEND_REFRESH can proceed. */
  if (server->backend && backend_full_refresh_in_flight(server->backend)) {
    WLog_INFO(TAG, "Frame %" PRIu32 " completing backend refresh via GFX path",
              end_frame->frameId);
    backend_mark_full_refresh_complete(server->backend);
  }

  return viewer_server_publish_gfx_event(backend, event);
}

BOOL viewer_server_publish_gfx_delete_encoding_context(
    BackendClient *backend,
    const RDPGFX_DELETE_ENCODING_CONTEXT_PDU *delete_encoding_context) {
  ViewerServer *server = g_viewer_server;
  ViewerGfxEvent *event = NULL;

  if (!server || (server->backend != backend) || !delete_encoding_context)
    return FALSE;

  event = viewer_gfx_event_new_simple(VIEWER_GFX_EVENT_DELETE_ENCODING_CONTEXT,
                                      delete_encoding_context,
                                      sizeof(*delete_encoding_context));
  if (!event)
    return FALSE;

  WLog_INFO(TAG,
            "DeleteEncodingContext surfaceId=%" PRIu16
            " codecContextId=%" PRIu32,
            delete_encoding_context->surfaceId,
            delete_encoding_context->codecContextId);

  return viewer_server_publish_gfx_event(backend, event);
}
