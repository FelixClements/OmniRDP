#include "viewer_classic_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(expr)                                                      \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf_s(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expr);                                                        \
      return FALSE;                                                            \
    }                                                                          \
  } while (0)

static BITMAP_UPDATE make_bitmap(BYTE *data, UINT32 len, UINT16 left) {
  static BITMAP_DATA rect;
  static BITMAP_UPDATE update;

  memset(&rect, 0, sizeof(rect));
  memset(&update, 0, sizeof(update));
  rect.destLeft = left;
  rect.destTop = 0;
  rect.destRight = (UINT16)(left + 1U);
  rect.destBottom = 1;
  rect.width = 2;
  rect.height = 2;
  rect.bitsPerPixel = 32;
  rect.bitmapLength = len;
  rect.bitmapDataStream = data;
  update.number = 1;
  update.skipCompression = TRUE;
  update.rectangles = &rect;
  return update;
}

static BOOL test_bitmap_deep_copy(void) {
  BYTE data[4] = {1, 2, 3, 4};
  BITMAP_UPDATE update = make_bitmap(data, sizeof(data), 0);
  ViewerClassicEvent *event = viewer_classic_event_new(&update);
  const BITMAP_UPDATE *copy = viewer_classic_event_bitmap(event);

  ASSERT_TRUE(event != NULL);
  data[0] = 99;
  update.rectangles[0].destLeft = 7;
  ASSERT_TRUE(copy->rectangles[0].bitmapDataStream[0] == 1);
  ASSERT_TRUE(copy->rectangles[0].destLeft == 0);
  ASSERT_TRUE(viewer_classic_event_payload_bytes(event) == 4);
  viewer_classic_event_free(event);
  return TRUE;
}

static BOOL test_bitmap_fifo_drop_and_clear(void) {
  ViewerClassicQueues queues;
  ViewerClassicQueueDropInfo drop = {0};
  BYTE bytes[VIEWER_CLASSIC_QUEUE_CAPACITY + 1U] = {0};

  ASSERT_TRUE(viewer_classic_queues_init(&queues));
  for (UINT32 i = 0; i < VIEWER_CLASSIC_QUEUE_CAPACITY; i++) {
    BITMAP_UPDATE update = make_bitmap(&bytes[i], 1, (UINT16)i);
    ASSERT_TRUE(viewer_classic_queue_enqueue_event_locked(
        &queues, viewer_classic_event_new(&update), &drop));
  }
  ASSERT_TRUE(drop.dropped_count == 0);
  ASSERT_TRUE(viewer_classic_queue_depth_locked(&queues) ==
              VIEWER_CLASSIC_QUEUE_CAPACITY);
  ASSERT_TRUE(viewer_classic_queue_payload_bytes_locked(&queues) ==
              VIEWER_CLASSIC_QUEUE_CAPACITY);

  {
    BITMAP_UPDATE update = make_bitmap(&bytes[VIEWER_CLASSIC_QUEUE_CAPACITY], 1,
                                       VIEWER_CLASSIC_QUEUE_CAPACITY);
    ASSERT_TRUE(viewer_classic_queue_enqueue_event_locked(
        &queues, viewer_classic_event_new(&update), &drop));
  }
  ASSERT_TRUE(drop.dropped_count == 1);
  ASSERT_TRUE(drop.dropped_payload_bytes == 1);

  for (UINT32 i = 1; i <= VIEWER_CLASSIC_QUEUE_CAPACITY; i++) {
    ViewerClassicEvent *event = viewer_classic_queue_dequeue_locked(&queues);
    const BITMAP_UPDATE *copy = viewer_classic_event_bitmap(event);
    ASSERT_TRUE(event != NULL);
    ASSERT_TRUE(copy->rectangles[0].destLeft == i);
    viewer_classic_event_free(event);
  }
  ASSERT_TRUE(viewer_classic_queue_dequeue_locked(&queues) == NULL);

  drop.dropped_count = 0;
  drop.dropped_payload_bytes = 0;
  for (UINT32 i = 0; i < 3; i++) {
    BITMAP_UPDATE update = make_bitmap(&bytes[i], 2, (UINT16)i);
    ASSERT_TRUE(viewer_classic_queue_enqueue_event_locked(
        &queues, viewer_classic_event_new(&update), NULL));
  }
  viewer_classic_queue_clear_locked(&queues, &drop);
  ASSERT_TRUE(drop.dropped_count == 3);
  ASSERT_TRUE(drop.dropped_payload_bytes == 6);
  ASSERT_TRUE(viewer_classic_queue_depth_locked(&queues) == 0);
  viewer_classic_queues_uninit(&queues);
  return TRUE;
}

static BOOL test_snapshot_event(void) {
  BYTE pixels[16] = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerClassicEvent *event = NULL;
  const BITMAP_UPDATE *bitmap = NULL;

  snapshot.width = 2;
  snapshot.height = 2;
  snapshot.stride = 8;
  snapshot.pixel_bytes = sizeof(pixels);
  snapshot.pixels = pixels;
  snapshot.generation = 42;
  event = viewer_classic_event_from_snapshot(&snapshot);
  ASSERT_TRUE(event != NULL);
  bitmap = viewer_classic_event_bitmap(event);
  ASSERT_TRUE(bitmap->number == 1);
  ASSERT_TRUE(bitmap->rectangles[0].destRight == 1);
  ASSERT_TRUE(bitmap->rectangles[0].destBottom == 1);
  ASSERT_TRUE(bitmap->rectangles[0].bitmapLength == sizeof(pixels));
  ASSERT_TRUE(viewer_classic_event_generation(event) == 42);
  viewer_classic_event_free(event);
  return TRUE;
}

static SURFACE_BITS_COMMAND make_surface_bits(BYTE *data, UINT32 len,
                                              UINT16 left) {
  SURFACE_BITS_COMMAND cmd;

  memset(&cmd, 0, sizeof(cmd));
  cmd.destLeft = left;
  cmd.destTop = 0;
  cmd.destRight = (UINT16)(left + 1U);
  cmd.destBottom = 1;
  cmd.bmp.bitmapDataLength = len;
  cmd.bmp.bitmapData = data;
  return cmd;
}

static BOOL test_surface_bits_deep_copy_fifo_and_drop(void) {
  ViewerClassicQueues queues;
  ViewerClassicQueueDropInfo drop = {0};
  BYTE first[2] = {1, 2};
  BYTE second[2] = {3, 4};
  SURFACE_BITS_COMMAND first_cmd = make_surface_bits(first, 2, 1);
  ViewerSurfaceBitsEvent *event = NULL;
  const SURFACE_BITS_COMMAND *cmd = NULL;

  ASSERT_TRUE(viewer_classic_queues_init(&queues));
  ASSERT_TRUE(viewer_surface_bits_queue_enqueue_event_locked(
      &queues, viewer_surface_bits_event_new(&first_cmd), NULL));
  first[0] = 9;
  event = viewer_surface_bits_queue_dequeue_locked(&queues);
  cmd = viewer_surface_bits_event_command(event);
  ASSERT_TRUE(cmd->destLeft == 1);
  ASSERT_TRUE(cmd->bmp.bitmapData[0] == 1);
  viewer_surface_bits_event_free(event);

  for (UINT32 i = 0; i < VIEWER_SURFACE_BITS_QUEUE_CAPACITY; i++) {
    SURFACE_BITS_COMMAND next = make_surface_bits(second, 2, (UINT16)i);
    ASSERT_TRUE(viewer_surface_bits_queue_enqueue_event_locked(
        &queues, viewer_surface_bits_event_new(&next), &drop));
  }
  ASSERT_TRUE(drop.dropped_count == 0);
  {
    SURFACE_BITS_COMMAND next =
        make_surface_bits(second, 2, VIEWER_SURFACE_BITS_QUEUE_CAPACITY);
    ASSERT_TRUE(viewer_surface_bits_queue_enqueue_event_locked(
        &queues, viewer_surface_bits_event_new(&next), &drop));
  }
  ASSERT_TRUE(drop.dropped_count == 1);
  ASSERT_TRUE(drop.dropped_payload_bytes == 2);
  event = viewer_surface_bits_queue_dequeue_locked(&queues);
  cmd = viewer_surface_bits_event_command(event);
  ASSERT_TRUE(cmd->destLeft == 1);
  viewer_surface_bits_event_free(event);
  viewer_classic_queues_uninit(&queues);
  return TRUE;
}

static BOOL test_wake_event_signals(void) {
  ViewerClassicQueues queues;
  BYTE data = 1;
  BITMAP_UPDATE update = make_bitmap(&data, 1, 0);

  ASSERT_TRUE(viewer_classic_queues_init(&queues));
  ASSERT_TRUE(viewer_classic_queues_event(&queues) != NULL);
  ASSERT_TRUE(WaitForSingleObject(viewer_classic_queues_event(&queues), 0) ==
              WAIT_TIMEOUT);
  ASSERT_TRUE(viewer_classic_queue_enqueue_event_locked(
      &queues, viewer_classic_event_new(&update), NULL));
  ASSERT_TRUE(WaitForSingleObject(viewer_classic_queues_event(&queues), 0) ==
              WAIT_OBJECT_0);
  viewer_classic_queues_reset_event(&queues);
  ASSERT_TRUE(WaitForSingleObject(viewer_classic_queues_event(&queues), 0) ==
              WAIT_TIMEOUT);
  viewer_classic_queues_uninit(&queues);
  return TRUE;
}

int main(void) {
  ASSERT_TRUE(test_bitmap_deep_copy());
  ASSERT_TRUE(test_bitmap_fifo_drop_and_clear());
  ASSERT_TRUE(test_snapshot_event());
  ASSERT_TRUE(test_surface_bits_deep_copy_fifo_and_drop());
  ASSERT_TRUE(test_wake_event_signals());
  return 0;
}
