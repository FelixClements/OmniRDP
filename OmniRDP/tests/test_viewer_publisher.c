#include "viewer_publisher.h"

#include <stdio.h>

static int expect_true(BOOL value, const char *message) {
  if (!value) {
    (void)fprintf(stderr, "FAIL: %s\n", message);
    return 0;
  }
  return 1;
}

static int expect_uint64(UINT64 actual, UINT64 expected, const char *message) {
  if (actual != expected) {
    (void)fprintf(stderr, "FAIL: %s actual=%llu expected=%llu\n", message,
                  (unsigned long long)actual, (unsigned long long)expected);
    return 0;
  }
  return 1;
}

static int expect_uint32(UINT32 actual, UINT32 expected, const char *message) {
  if (actual != expected) {
    (void)fprintf(stderr, "FAIL: %s actual=%u expected=%u\n", message, actual,
                  expected);
    return 0;
  }
  return 1;
}

static void fill_pixels(BYTE *pixels, size_t count, BYTE seed) {
  for (size_t i = 0; i < count; i++)
    pixels[i] = (BYTE)(seed + (BYTE)i);
}

static BOOL setup_framebuffer(ViewerFramebuffer *fb, BYTE *pixels,
                              size_t pixel_count) {
  fill_pixels(pixels, pixel_count, 1);
  if (!viewer_framebuffer_init(fb))
    return FALSE;
  if (!viewer_framebuffer_resize(fb, 2, 2, 8, 32))
    return FALSE;
  return viewer_framebuffer_update_pixels(fb, pixels, 8, NULL, 0);
}

static int test_generation_and_metrics(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerPublisherMetrics metrics = {0};
  BYTE pixels[16] = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");
  ok = ok && expect_true(viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "publisher snapshot");

  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.latest_generation_available,
                           snapshot.generation, "latest generation updated");
  ok = ok &&
       expect_uint64(metrics.queued_updates, 1, "first snapshot queued update");
  ok = ok && expect_uint64(metrics.queued_bytes, snapshot.pixel_bytes,
                           "queued bytes updated");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "full dirty rect normalized/preserved");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_dirty_overflow_normalizes_full_frame(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[16] = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");

  EnterCriticalSection(&fb.lock);
  fb.dirty_overflow = TRUE;
  LeaveCriticalSection(&fb.lock);

  ok = ok && expect_true(viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "overflow snapshot");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "overflow normalizes to one rect");
  ok = ok && expect_true(!snapshot.dirty_overflow, "overflow cleared");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].left, 0, "full left");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].top, 0, "full top");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].right, 1, "full right");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].bottom, 1, "full bottom");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_empty_dirty_normalizes_full_frame(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[16] = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");

  EnterCriticalSection(&fb.lock);
  fb.dirty_rect_count = 0;
  fb.dirty_overflow = FALSE;
  LeaveCriticalSection(&fb.lock);

  ok = ok && expect_true(viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "empty dirty snapshot");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "empty dirty normalizes to one rect");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].right, 1, "full right");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].bottom, 1, "full bottom");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_slow_consumer_coalescing(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerPublisherMetrics metrics = {0};
  BYTE pixels[16] = {0};
  RECTANGLE_16 dirty = {0, 0, 0, 0};
  UINT64 first_generation = 0;
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");
  ok = ok && expect_true(viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "first snapshot");
  first_generation = snapshot.generation;
  viewer_framebuffer_snapshot_free(&snapshot);

  fill_pixels(pixels, sizeof(pixels), 20);
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 8, &dirty, 1),
                   "second generation update");
  fill_pixels(pixels, sizeof(pixels), 40);
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 8, &dirty, 1),
                   "third generation update");
  ok = ok && expect_true(viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "coalesced snapshot");
  ok = ok && expect_true(snapshot.generation > first_generation,
                         "snapshot advanced to latest generation");

  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.queued_updates, 1,
                           "coalesced update does not enqueue second pending");
  ok = ok && expect_uint64(metrics.coalesced_updates, 1,
                           "newer pending generation coalesced");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_mark_consumed_allows_next_queue(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerPublisherMetrics metrics = {0};
  BYTE pixels[16] = {0};
  RECTANGLE_16 dirty = {0, 0, 0, 0};
  UINT64 consumed_generation = 0;
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");
  ok = ok && expect_true(viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "first snapshot");
  consumed_generation = snapshot.generation;
  viewer_framebuffer_snapshot_free(&snapshot);

  viewer_publisher_mark_consumed(&publisher, consumed_generation);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.last_generation_sent, consumed_generation,
                           "mark consumed records sent generation");

  fill_pixels(pixels, sizeof(pixels), 60);
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 8, &dirty, 1),
                   "next generation update");
  ok = ok && expect_true(viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "next snapshot");
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.queued_updates, 2,
                           "next generation queues after consume");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

int main(void) {
  if (!test_generation_and_metrics())
    return 1;
  if (!test_dirty_overflow_normalizes_full_frame())
    return 1;
  if (!test_empty_dirty_normalizes_full_frame())
    return 1;
  if (!test_slow_consumer_coalescing())
    return 1;
  if (!test_mark_consumed_allows_next_queue())
    return 1;
  return 0;
}
