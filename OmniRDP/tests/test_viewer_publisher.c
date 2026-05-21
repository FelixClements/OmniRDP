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

static int expect_int(int actual, int expected, const char *message) {
  if (actual != expected) {
    (void)fprintf(stderr, "FAIL: %s actual=%d expected=%d\n", message, actual,
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

static int test_init_uninit_lock_state(void) {
  ViewerPublisher publisher = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(publisher.initialized, "publisher initialized flag");
  ok = ok && expect_true(publisher.lock_initialized,
                         "publisher lock initialized flag");

  viewer_publisher_uninit(&publisher);
  ok = ok && expect_true(!publisher.initialized, "publisher initialized reset");
  ok = ok && expect_true(!publisher.lock_initialized,
                         "publisher lock initialized reset");
  return ok;
}

static int test_framebuffer_update_observation_metrics(void) {
  ViewerPublisher publisher = {0};
  ViewerPublisherMetrics metrics = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");

  viewer_publisher_note_framebuffer_update(&publisher, 42, 3, FALSE);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.latest_generation_available, 42,
                           "observed generation recorded");
  ok = ok && expect_uint64(metrics.observed_framebuffer_updates, 1,
                           "observed framebuffer update counted");
  ok = ok && expect_uint64(metrics.observed_dirty_rects, 3,
                           "observed dirty rects counted");
  ok = ok && expect_uint32(metrics.latest_dirty_rect_count, 3,
                           "latest dirty rect count recorded");
  ok = ok && expect_true(!metrics.latest_dirty_overflow,
                         "latest dirty overflow false recorded");

  viewer_publisher_note_framebuffer_update(&publisher, 43, 5, TRUE);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.latest_generation_available, 43,
                           "second observed generation recorded");
  ok = ok && expect_uint64(metrics.observed_framebuffer_updates, 2,
                           "second observed framebuffer update counted");
  ok = ok && expect_uint64(metrics.observed_dirty_rects, 8,
                           "cumulative observed dirty rects counted");
  ok = ok && expect_uint32(metrics.latest_dirty_rect_count, 5,
                           "second latest dirty rect count recorded");
  ok = ok && expect_true(metrics.latest_dirty_overflow,
                         "latest dirty overflow true recorded");

  viewer_publisher_reset_metrics(&publisher);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.observed_framebuffer_updates, 0,
                           "reset clears observed update count");
  ok = ok && expect_uint32(metrics.latest_dirty_rect_count, 0,
                           "reset clears latest dirty count");
  ok = ok && expect_true(!metrics.latest_dirty_overflow,
                         "reset clears latest dirty overflow");

  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_classic_queue_observation_metrics(void) {
  ViewerPublisher publisher = {0};
  ViewerPublisherMetrics metrics = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");

  viewer_publisher_note_classic_queue_state(&publisher, 2, 1000);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint32(metrics.classic_queue_depth, 2,
                           "classic queue depth observed");
  ok = ok && expect_uint64(metrics.classic_queue_bytes, 1000,
                           "classic queue bytes observed");
  ok = ok && expect_uint32(metrics.classic_queue_max_depth, 2,
                           "classic queue max depth observed");
  ok = ok && expect_uint64(metrics.classic_queue_max_bytes, 1000,
                           "classic queue max bytes observed");

  viewer_publisher_note_classic_queue_state(&publisher, 1, 500);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint32(metrics.classic_queue_depth, 1,
                           "classic queue depth decreases");
  ok = ok && expect_uint64(metrics.classic_queue_bytes, 500,
                           "classic queue bytes decrease");
  ok = ok && expect_uint32(metrics.classic_queue_max_depth, 2,
                           "classic queue max depth retained");
  ok = ok && expect_uint64(metrics.classic_queue_max_bytes, 1000,
                           "classic queue max bytes retained");

  viewer_publisher_note_classic_queue_state(&publisher, 3, 1500);
  viewer_publisher_note_classic_drop(&publisher);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint32(metrics.classic_queue_max_depth, 3,
                           "classic queue max depth advances");
  ok = ok && expect_uint64(metrics.classic_queue_max_bytes, 1500,
                           "classic queue max bytes advances");
  ok = ok && expect_uint64(metrics.classic_queue_dropped_events, 1,
                           "classic drop counted");

  viewer_publisher_reset_metrics(&publisher);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint32(metrics.classic_queue_depth, 0,
                           "reset clears queue depth");
  ok = ok && expect_uint64(metrics.classic_queue_bytes, 0,
                           "reset clears queue bytes");
  ok = ok && expect_uint64(metrics.classic_queue_dropped_events, 0,
                           "reset clears drop count");
  ok = ok && expect_uint32(metrics.classic_queue_max_depth, 0,
                           "reset clears max queue depth");
  ok = ok && expect_uint64(metrics.classic_queue_max_bytes, 0,
                           "reset clears max queue bytes");

  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_classic_policy_default_fifo(void) {
  ViewerPublisher publisher = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_int(
                 viewer_publisher_classic_queue_decision(&publisher, 99, 99999),
                 VIEWER_PUBLISHER_CLASSIC_DECISION_KEEP_FIFO,
                 "default policy keeps fifo");

  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_classic_policy_enabled_over_depth_limit(void) {
  ViewerPublisher publisher = {0};
  ViewerPublisherClassicPolicyConfig config = {0};
  ViewerPublisherMetrics metrics = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  config.enabled = TRUE;
  config.policy = VIEWER_PUBLISHER_CLASSIC_POLICY_LATEST_STATE;
  config.max_queue_depth = 2;
  viewer_publisher_set_classic_policy(&publisher, &config);

  ok = ok &&
       expect_int(viewer_publisher_classic_queue_decision(&publisher, 3, 0),
                  VIEWER_PUBLISHER_CLASSIC_DECISION_REPLACE_WITH_BASELINE,
                  "over depth replaces with baseline");
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.classic_latest_replacements, 1,
                           "depth replacement counted");

  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_classic_policy_enabled_over_byte_limit(void) {
  ViewerPublisher publisher = {0};
  ViewerPublisherClassicPolicyConfig config = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  config.enabled = TRUE;
  config.policy = VIEWER_PUBLISHER_CLASSIC_POLICY_LATEST_STATE;
  config.max_queue_bytes = 1024;
  viewer_publisher_set_classic_policy(&publisher, &config);

  ok = ok &&
       expect_int(viewer_publisher_classic_queue_decision(&publisher, 1, 1025),
                  VIEWER_PUBLISHER_CLASSIC_DECISION_REPLACE_WITH_BASELINE,
                  "over byte limit replaces with baseline");

  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_classic_policy_below_limit_keeps_fifo(void) {
  ViewerPublisher publisher = {0};
  ViewerPublisherClassicPolicyConfig config = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  config.enabled = TRUE;
  config.policy = VIEWER_PUBLISHER_CLASSIC_POLICY_LATEST_STATE;
  config.max_queue_depth = 3;
  config.max_queue_bytes = 2048;
  viewer_publisher_set_classic_policy(&publisher, &config);

  ok = ok &&
       expect_int(viewer_publisher_classic_queue_decision(&publisher, 3, 2048),
                  VIEWER_PUBLISHER_CLASSIC_DECISION_KEEP_FIFO,
                  "at limits keeps fifo");
  ok = ok &&
       expect_int(viewer_publisher_classic_queue_decision(&publisher, 2, 1000),
                  VIEWER_PUBLISHER_CLASSIC_DECISION_KEEP_FIFO,
                  "below limits keeps fifo");

  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_classic_policy_reset_to_default_fifo(void) {
  ViewerPublisher publisher = {0};
  ViewerPublisherClassicPolicyConfig config = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  config.enabled = TRUE;
  config.policy = VIEWER_PUBLISHER_CLASSIC_POLICY_LATEST_STATE;
  config.max_queue_depth = 1;
  viewer_publisher_set_classic_policy(&publisher, &config);
  viewer_publisher_set_classic_policy(&publisher, NULL);

  ok = ok &&
       expect_int(viewer_publisher_classic_queue_decision(&publisher, 2, 0),
                  VIEWER_PUBLISHER_CLASSIC_DECISION_KEEP_FIFO,
                  "null config resets fifo behavior");

  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_classic_latest_snapshot_newer_generation(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerPublisherMetrics metrics = {0};
  BYTE pixels[16] = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");
  ok = ok && expect_true(viewer_publisher_classic_latest_snapshot(
                             &publisher, &fb, 0, &snapshot),
                         "latest snapshot succeeds for newer generation");
  ok = ok && expect_true(snapshot.generation > 0, "snapshot has generation");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "latest snapshot is full-frame baseline");
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.queued_updates, 1,
                           "latest snapshot counted as queued");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_classic_latest_snapshot_stale_suppression(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerPublisherMetrics metrics = {0};
  BYTE pixels[16] = {0};
  UINT64 generation = 0;
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");
  ok = ok && expect_true(viewer_publisher_classic_latest_snapshot(
                             &publisher, &fb, 0, &snapshot),
                         "initial latest snapshot succeeds");
  generation = snapshot.generation;
  viewer_framebuffer_snapshot_free(&snapshot);

  ok = ok && expect_true(!viewer_publisher_classic_latest_snapshot(
                             &publisher, &fb, generation, &snapshot),
                         "stale latest snapshot suppressed");
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.classic_latest_suppressed, 1,
                           "stale latest suppression counted");

  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
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
  /* Publisher dirty rectangles are inclusive left/top/right/bottom, so a 2x2
   * framebuffer normalizes to right=1,bottom=1. */
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

static int test_classic_baseline_snapshot_full_frame(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerPublisherMetrics metrics = {0};
  BYTE pixels[16] = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");

  EnterCriticalSection(&fb.lock);
  fb.dirty_rect_count = 0;
  fb.dirty_overflow = FALSE;
  LeaveCriticalSection(&fb.lock);

  ok = ok && expect_true(viewer_publisher_classic_baseline_snapshot(
                             &publisher, &fb, &snapshot),
                         "classic baseline snapshot succeeds");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "baseline normalizes to one full rect");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].left, 0, "baseline left");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].top, 0, "baseline top");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].right, 1, "baseline right");
  ok =
      ok && expect_uint32(snapshot.dirty_rects[0].bottom, 1, "baseline bottom");
  ok = ok &&
       expect_true(!snapshot.dirty_overflow, "baseline clears dirty overflow");
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.queued_updates, 1,
                           "baseline counted as queued observation");
  ok = ok && expect_uint64(metrics.queued_bytes, snapshot.pixel_bytes,
                           "baseline bytes observed");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_classic_baseline_not_suppressed_by_consumed_generation(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[16] = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");
  viewer_publisher_mark_consumed(&publisher, 9999ULL);
  ok = ok && expect_true(viewer_publisher_classic_baseline_snapshot(
                             &publisher, &fb, &snapshot),
                         "baseline ignores consumed generation");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_classic_baseline_empty_framebuffer_fails(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(viewer_framebuffer_init(&fb), "framebuffer init");
  ok = ok && expect_true(!viewer_publisher_classic_baseline_snapshot(
                             &publisher, &fb, &snapshot),
                         "empty framebuffer baseline fails");

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

static int test_duplicate_snapshot_after_consume_is_suppressed(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerPublisherMetrics metrics = {0};
  UINT64 queued_bytes = 0;
  UINT64 consumed_generation = 0;
  BYTE pixels[16] = {0};
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
  queued_bytes = metrics.queued_bytes;

  ok = ok && expect_true(!viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "duplicate consumed generation suppressed");
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.queued_updates, 1,
                           "duplicate does not queue update");
  ok = ok && expect_uint64(metrics.coalesced_updates, 0,
                           "duplicate does not coalesce");
  ok = ok && expect_uint64(metrics.queued_bytes, queued_bytes,
                           "duplicate does not add queued bytes");
  ok = ok && expect_uint64(metrics.dropped_updates, 0,
                           "duplicate is not counted as dropped");

  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_stale_consumed_generation_ignored(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerPublisherMetrics metrics = {0};
  BYTE pixels[16] = {0};
  RECTANGLE_16 dirty = {0, 0, 0, 0};
  UINT64 first_generation = 0;
  UINT64 pending_generation = 0;
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");
  ok = ok && expect_true(viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "first snapshot");
  first_generation = snapshot.generation;
  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_publisher_mark_consumed(&publisher, first_generation);

  fill_pixels(pixels, sizeof(pixels), 80);
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 8, &dirty, 1),
                   "next generation update");
  ok = ok && expect_true(viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "next pending snapshot");
  pending_generation = snapshot.generation;
  viewer_framebuffer_snapshot_free(&snapshot);

  viewer_publisher_mark_consumed(&publisher, first_generation - 1U);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.last_generation_sent, first_generation,
                           "stale consumed generation ignored");

  ok = ok && expect_true(viewer_publisher_snapshot(&publisher, &fb, &snapshot),
                         "pending survives stale consume");
  viewer_framebuffer_snapshot_free(&snapshot);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.queued_updates, 2,
                           "stale consume did not clear pending");
  ok = ok && expect_uint64(metrics.coalesced_updates, 0,
                           "same pending generation not re-coalesced");

  viewer_publisher_mark_consumed(&publisher, pending_generation);
  metrics = viewer_publisher_get_metrics(&publisher);
  ok = ok && expect_uint64(metrics.last_generation_sent, pending_generation,
                           "newer consumed generation accepted");

  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_gfx_dirty_snapshot_generation_filtering(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[16] = {0};
  RECTANGLE_16 dirty = {1, 1, 1, 1};
  UINT64 generation = 0;
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");
  ok = ok && expect_true(viewer_publisher_gfx_dirty_snapshot(&publisher, &fb, 0,
                                                             &snapshot),
                         "gfx dirty snapshot for new generation");
  generation = snapshot.generation;
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "gfx dirty snapshot has dirty rect");
  viewer_framebuffer_snapshot_free(&snapshot);

  ok = ok && expect_true(!viewer_publisher_gfx_dirty_snapshot(
                             &publisher, &fb, generation, &snapshot),
                         "gfx dirty snapshot suppresses consumed generation");

  fill_pixels(pixels, sizeof(pixels), 90);
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 8, &dirty, 1),
                   "next dirty update");
  ok = ok && expect_true(viewer_publisher_gfx_dirty_snapshot(
                             &publisher, &fb, generation, &snapshot),
                         "gfx dirty snapshot isolated per viewer generation");
  ok = ok &&
       expect_uint32(snapshot.dirty_rects[0].left, 1, "dirty left preserved");
  ok = ok &&
       expect_uint32(snapshot.dirty_rects[0].top, 1, "dirty top preserved");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_gfx_dirty_snapshot_too_many_rects_full_frame(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[16] = {0};
  RECTANGLE_16 dirty[VIEWER_PUBLISHER_GFX_DIRTY_RECT_THRESHOLD + 1U] = {0};
  int ok = 1;

  for (UINT32 i = 0; i < (UINT32)(sizeof(dirty) / sizeof(dirty[0])); i++)
    dirty[i].left = dirty[i].top = dirty[i].right = dirty[i].bottom = 0;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");
  fill_pixels(pixels, sizeof(pixels), 100);
  ok = ok && expect_true(viewer_framebuffer_update_pixels(
                             &fb, pixels, 8, dirty,
                             (UINT32)(sizeof(dirty) / sizeof(dirty[0]))),
                         "many dirty rect update");
  ok = ok && expect_true(viewer_publisher_gfx_dirty_snapshot(&publisher, &fb, 0,
                                                             &snapshot),
                         "gfx dirty snapshot accepts many rect generation");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "many dirty rects normalize to one full frame");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].left, 0, "full left");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].top, 0, "full top");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].right, 1, "full right");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].bottom, 1, "full bottom");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

static int test_gfx_dirty_snapshot_overflow_full_frame(void) {
  ViewerPublisher publisher = {0};
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[16] = {0};
  RECTANGLE_16 dirty[VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS + 1U] = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_publisher_init(&publisher), "publisher init");
  ok = ok && expect_true(setup_framebuffer(&fb, pixels, sizeof(pixels)),
                         "framebuffer setup");
  fill_pixels(pixels, sizeof(pixels), 110);
  ok = ok && expect_true(viewer_framebuffer_update_pixels(
                             &fb, pixels, 8, dirty,
                             VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS + 1U),
                         "overflow dirty update");
  ok = ok && expect_true(viewer_publisher_gfx_dirty_snapshot(&publisher, &fb, 0,
                                                             &snapshot),
                         "gfx dirty snapshot accepts overflow generation");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "overflow normalizes to one full frame");
  ok = ok && expect_true(!snapshot.dirty_overflow,
                         "overflow flag cleared after normalization");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  viewer_publisher_uninit(&publisher);
  return ok;
}

int main(void) {
  if (!test_init_uninit_lock_state())
    return 1;
  if (!test_framebuffer_update_observation_metrics())
    return 1;
  if (!test_classic_queue_observation_metrics())
    return 1;
  if (!test_classic_policy_default_fifo())
    return 1;
  if (!test_classic_policy_enabled_over_depth_limit())
    return 1;
  if (!test_classic_policy_enabled_over_byte_limit())
    return 1;
  if (!test_classic_policy_below_limit_keeps_fifo())
    return 1;
  if (!test_classic_policy_reset_to_default_fifo())
    return 1;
  if (!test_classic_latest_snapshot_newer_generation())
    return 1;
  if (!test_classic_latest_snapshot_stale_suppression())
    return 1;
  if (!test_generation_and_metrics())
    return 1;
  if (!test_dirty_overflow_normalizes_full_frame())
    return 1;
  if (!test_empty_dirty_normalizes_full_frame())
    return 1;
  if (!test_classic_baseline_snapshot_full_frame())
    return 1;
  if (!test_classic_baseline_not_suppressed_by_consumed_generation())
    return 1;
  if (!test_classic_baseline_empty_framebuffer_fails())
    return 1;
  if (!test_slow_consumer_coalescing())
    return 1;
  if (!test_mark_consumed_allows_next_queue())
    return 1;
  if (!test_duplicate_snapshot_after_consume_is_suppressed())
    return 1;
  if (!test_stale_consumed_generation_ignored())
    return 1;
  if (!test_gfx_dirty_snapshot_generation_filtering())
    return 1;
  if (!test_gfx_dirty_snapshot_too_many_rects_full_frame())
    return 1;
  if (!test_gfx_dirty_snapshot_overflow_full_frame())
    return 1;
  return 0;
}
