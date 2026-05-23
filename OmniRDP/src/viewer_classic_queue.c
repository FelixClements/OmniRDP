#include "viewer_classic_queue.h"

#include <stdlib.h>
#include <string.h>

static void
viewer_classic_queue_drop_oldest_locked(ViewerClassicQueues *queues,
                                        ViewerClassicQueueDropInfo *drop_info) {
  ViewerClassicEvent *oldest = NULL;

  if (!queues || (queues->classic_queue_count == 0))
    return;

  oldest = queues->classic_queue[queues->classic_queue_head];
  queues->classic_queue[queues->classic_queue_head] = NULL;
  queues->classic_queue_head =
      (queues->classic_queue_head + 1U) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  queues->classic_queue_count--;

  if (drop_info) {
    drop_info->dropped_count++;
    drop_info->dropped_payload_bytes +=
        viewer_classic_event_payload_bytes(oldest);
  }
  viewer_classic_event_free(oldest);
}

static void viewer_surface_bits_queue_drop_oldest_locked(
    ViewerClassicQueues *queues, ViewerClassicQueueDropInfo *drop_info) {
  ViewerSurfaceBitsEvent *oldest = NULL;

  if (!queues || (queues->surface_bits_queue_count == 0))
    return;

  oldest = queues->surface_bits_queue[queues->surface_bits_queue_head];
  queues->surface_bits_queue[queues->surface_bits_queue_head] = NULL;
  queues->surface_bits_queue_head = (queues->surface_bits_queue_head + 1U) %
                                    VIEWER_SURFACE_BITS_QUEUE_CAPACITY;
  queues->surface_bits_queue_count--;

  if (drop_info) {
    drop_info->dropped_count++;
    if (oldest)
      drop_info->dropped_payload_bytes += oldest->cmd.bmp.bitmapDataLength;
  }
  viewer_surface_bits_event_free(oldest);
}

BOOL viewer_classic_queues_init(ViewerClassicQueues *queues) {
  if (!queues)
    return FALSE;

  memset(queues, 0, sizeof(*queues));
  queues->classic_event = CreateEventA(NULL, TRUE, FALSE, NULL);
  return queues->classic_event ? TRUE : FALSE;
}

void viewer_classic_queues_uninit(ViewerClassicQueues *queues) {
  if (!queues)
    return;

  viewer_classic_queue_clear_locked(queues, NULL);
  viewer_surface_bits_queue_clear_locked(queues, NULL);
  if (queues->classic_event)
    CloseHandle(queues->classic_event);
  memset(queues, 0, sizeof(*queues));
}

ViewerClassicEvent *viewer_classic_event_new(const BITMAP_UPDATE *bitmap) {
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

  event->bitmap->number = bitmap->number;
  event->bitmap->skipCompression = bitmap->skipCompression;

  if (bitmap->number == 0) {
    event->bitmap->rectangles = NULL;
    return event;
  }

  event->bitmap->rectangles =
      (BITMAP_DATA *)calloc(bitmap->number, sizeof(BITMAP_DATA));
  if (!event->bitmap->rectangles) {
    free(event->bitmap);
    free(event);
    return NULL;
  }

  for (i = 0; i < bitmap->number; i++) {
    event->bitmap->rectangles[i] = bitmap->rectangles[i];
    if ((bitmap->rectangles[i].bitmapLength > 0) &&
        bitmap->rectangles[i].bitmapDataStream) {
      event->bitmap->rectangles[i].bitmapDataStream =
          (BYTE *)malloc(bitmap->rectangles[i].bitmapLength);
      if (!event->bitmap->rectangles[i].bitmapDataStream) {
        for (UINT32 j = 0; j < i; j++) {
          free(event->bitmap->rectangles[j].bitmapDataStream);
          event->bitmap->rectangles[j].bitmapDataStream = NULL;
        }
        free(event->bitmap->rectangles);
        free(event->bitmap);
        free(event);
        return NULL;
      }
      memmove(event->bitmap->rectangles[i].bitmapDataStream,
              bitmap->rectangles[i].bitmapDataStream,
              bitmap->rectangles[i].bitmapLength);
    } else {
      event->bitmap->rectangles[i].bitmapDataStream = NULL;
    }
  }

  return event;
}

ViewerClassicEvent *
viewer_classic_event_from_snapshot(const ViewerFramebufferSnapshot *snapshot) {
  ViewerClassicEvent *event = NULL;
  BITMAP_DATA *rect = NULL;

  if (!snapshot || !snapshot->pixels || (snapshot->width == 0) ||
      (snapshot->height == 0) || (snapshot->stride == 0) ||
      (snapshot->pixel_bytes == 0) || (snapshot->width > UINT16_MAX) ||
      (snapshot->height > UINT16_MAX) || (snapshot->pixel_bytes > UINT32_MAX))
    return NULL;

  event = (ViewerClassicEvent *)calloc(1, sizeof(*event));
  if (!event)
    return NULL;

  event->bitmap = (BITMAP_UPDATE *)calloc(1, sizeof(*event->bitmap));
  if (!event->bitmap) {
    free(event);
    return NULL;
  }

  event->bitmap->rectangles = (BITMAP_DATA *)calloc(1, sizeof(BITMAP_DATA));
  if (!event->bitmap->rectangles) {
    viewer_classic_event_free(event);
    return NULL;
  }

  event->bitmap->number = 1;
  event->bitmap->skipCompression = TRUE;
  rect = &event->bitmap->rectangles[0];
  rect->destLeft = 0;
  rect->destTop = 0;
  rect->destRight = snapshot->width - 1U;
  rect->destBottom = snapshot->height - 1U;
  rect->width = snapshot->width;
  rect->height = snapshot->height;
  rect->bitsPerPixel = 32;
  rect->flags = 0;
  rect->bitmapLength = (UINT32)snapshot->pixel_bytes;
  rect->cbScanWidth = snapshot->stride;
  rect->cbUncompressedSize = (UINT32)snapshot->pixel_bytes;
  rect->compressed = FALSE;
  rect->bitmapDataStream = (BYTE *)malloc(snapshot->pixel_bytes);
  if (!rect->bitmapDataStream) {
    viewer_classic_event_free(event);
    return NULL;
  }

  memmove(rect->bitmapDataStream, snapshot->pixels, snapshot->pixel_bytes);
  event->generation = snapshot->generation;
  return event;
}

void viewer_classic_event_free(ViewerClassicEvent *event) {
  UINT32 i = 0;

  if (!event)
    return;

  if (event->bitmap) {
    if (event->bitmap->rectangles) {
      for (i = 0; i < event->bitmap->number; i++)
        free(event->bitmap->rectangles[i].bitmapDataStream);
      free(event->bitmap->rectangles);
    }
    free(event->bitmap);
  }
  free(event);
}

UINT64 viewer_classic_event_payload_bytes(const ViewerClassicEvent *event) {
  UINT64 bytes = 0;

  if (!event || !event->bitmap || !event->bitmap->rectangles)
    return 0;

  for (UINT32 i = 0; i < event->bitmap->number; i++)
    bytes += event->bitmap->rectangles[i].bitmapLength;
  return bytes;
}

const BITMAP_UPDATE *
viewer_classic_event_bitmap(const ViewerClassicEvent *event) {
  return event ? event->bitmap : NULL;
}

UINT64 viewer_classic_event_generation(const ViewerClassicEvent *event) {
  return event ? event->generation : 0;
}

ViewerSurfaceBitsEvent *
viewer_surface_bits_event_new(const SURFACE_BITS_COMMAND *cmd) {
  ViewerSurfaceBitsEvent *event = NULL;

  if (!cmd)
    return NULL;

  event = (ViewerSurfaceBitsEvent *)calloc(1, sizeof(ViewerSurfaceBitsEvent));
  if (!event)
    return NULL;

  event->cmd = *cmd;
  if ((cmd->bmp.bitmapDataLength > 0) && cmd->bmp.bitmapData) {
    event->cmd.bmp.bitmapData = (BYTE *)malloc(cmd->bmp.bitmapDataLength);
    if (!event->cmd.bmp.bitmapData) {
      free(event);
      return NULL;
    }
    memmove(event->cmd.bmp.bitmapData, cmd->bmp.bitmapData,
            cmd->bmp.bitmapDataLength);
  } else {
    event->cmd.bmp.bitmapData = NULL;
    event->cmd.bmp.bitmapDataLength = 0;
  }

  return event;
}

void viewer_surface_bits_event_free(ViewerSurfaceBitsEvent *event) {
  if (!event)
    return;

  free(event->cmd.bmp.bitmapData);
  free(event);
}

const SURFACE_BITS_COMMAND *
viewer_surface_bits_event_command(const ViewerSurfaceBitsEvent *event) {
  return event ? &event->cmd : NULL;
}

BOOL viewer_classic_queue_enqueue_event_locked(
    ViewerClassicQueues *queues, ViewerClassicEvent *event,
    ViewerClassicQueueDropInfo *drop_info) {
  if (!queues || !event)
    return FALSE;

  while (queues->classic_queue_count >= VIEWER_CLASSIC_QUEUE_CAPACITY)
    viewer_classic_queue_drop_oldest_locked(queues, drop_info);

  queues->classic_queue[queues->classic_queue_tail] = event;
  queues->classic_queue_tail =
      (queues->classic_queue_tail + 1U) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  queues->classic_queue_count++;
  viewer_classic_queues_signal(queues);
  return TRUE;
}

void viewer_classic_queue_enqueue_event_direct_locked(
    ViewerClassicQueues *queues, ViewerClassicEvent *event) {
  if (!queues || !event ||
      (queues->classic_queue_count >= VIEWER_CLASSIC_QUEUE_CAPACITY))
    return;

  queues->classic_queue[queues->classic_queue_tail] = event;
  queues->classic_queue_tail =
      (queues->classic_queue_tail + 1U) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  queues->classic_queue_count++;
  viewer_classic_queues_signal(queues);
}

ViewerClassicEvent *
viewer_classic_queue_dequeue_locked(ViewerClassicQueues *queues) {
  ViewerClassicEvent *event = NULL;

  if (!queues || (queues->classic_queue_count == 0))
    return NULL;

  event = queues->classic_queue[queues->classic_queue_head];
  queues->classic_queue[queues->classic_queue_head] = NULL;
  queues->classic_queue_head =
      (queues->classic_queue_head + 1U) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  queues->classic_queue_count--;
  return event;
}

void viewer_classic_queue_clear_locked(ViewerClassicQueues *queues,
                                       ViewerClassicQueueDropInfo *drop_info) {
  while (queues && (queues->classic_queue_count > 0))
    viewer_classic_queue_drop_oldest_locked(queues, drop_info);
}

UINT32 viewer_classic_queue_depth_locked(const ViewerClassicQueues *queues) {
  return queues ? queues->classic_queue_count : 0;
}

UINT64
viewer_classic_queue_payload_bytes_locked(const ViewerClassicQueues *queues) {
  UINT64 bytes = 0;
  UINT32 index = 0;

  if (!queues)
    return 0;

  index = queues->classic_queue_head;
  for (UINT32 i = 0; i < queues->classic_queue_count; i++) {
    bytes += viewer_classic_event_payload_bytes(queues->classic_queue[index]);
    index = (index + 1U) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  }
  return bytes;
}

BOOL viewer_surface_bits_queue_enqueue_event_locked(
    ViewerClassicQueues *queues, ViewerSurfaceBitsEvent *event,
    ViewerClassicQueueDropInfo *drop_info) {
  if (!queues || !event)
    return FALSE;

  while (queues->surface_bits_queue_count >= VIEWER_SURFACE_BITS_QUEUE_CAPACITY)
    viewer_surface_bits_queue_drop_oldest_locked(queues, drop_info);

  queues->surface_bits_queue[queues->surface_bits_queue_tail] = event;
  queues->surface_bits_queue_tail = (queues->surface_bits_queue_tail + 1U) %
                                    VIEWER_SURFACE_BITS_QUEUE_CAPACITY;
  queues->surface_bits_queue_count++;
  viewer_classic_queues_signal(queues);
  return TRUE;
}

ViewerSurfaceBitsEvent *
viewer_surface_bits_queue_dequeue_locked(ViewerClassicQueues *queues) {
  ViewerSurfaceBitsEvent *event = NULL;

  if (!queues || (queues->surface_bits_queue_count == 0))
    return NULL;

  event = queues->surface_bits_queue[queues->surface_bits_queue_head];
  queues->surface_bits_queue[queues->surface_bits_queue_head] = NULL;
  queues->surface_bits_queue_head = (queues->surface_bits_queue_head + 1U) %
                                    VIEWER_SURFACE_BITS_QUEUE_CAPACITY;
  queues->surface_bits_queue_count--;
  return event;
}

void viewer_surface_bits_queue_clear_locked(
    ViewerClassicQueues *queues, ViewerClassicQueueDropInfo *drop_info) {
  while (queues && (queues->surface_bits_queue_count > 0))
    viewer_surface_bits_queue_drop_oldest_locked(queues, drop_info);
}

UINT32
viewer_surface_bits_queue_depth_locked(const ViewerClassicQueues *queues) {
  return queues ? queues->surface_bits_queue_count : 0;
}

HANDLE viewer_classic_queues_event(const ViewerClassicQueues *queues) {
  return queues ? queues->classic_event : NULL;
}

void viewer_classic_queues_signal(const ViewerClassicQueues *queues) {
  if (queues && queues->classic_event)
    SetEvent(queues->classic_event);
}

void viewer_classic_queues_reset_event(const ViewerClassicQueues *queues) {
  if (queues && queues->classic_event)
    ResetEvent(queues->classic_event);
}
