#include "viewer_framebuffer.h"

#include "platform_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect_true(BOOL value, const char *message) {
  if (!value) {
    (void)fprintf_s(stderr, "FAIL: %s\n", message);
    return 0;
  }
  return 1;
}

static int expect_uint64(UINT64 actual, UINT64 expected, const char *message) {
  if (actual != expected) {
    (void)fprintf_s(stderr, "FAIL: %s actual=%llu expected=%llu\n", message,
                    (unsigned long long)actual, (unsigned long long)expected);
    return 0;
  }
  return 1;
}

static int expect_uint32(UINT32 actual, UINT32 expected, const char *message) {
  if (actual != expected) {
    (void)fprintf_s(stderr, "FAIL: %s actual=%u expected=%u\n", message, actual,
                    expected);
    return 0;
  }
  return 1;
}

static size_t pixel_offset(UINT32 x, UINT32 y, UINT32 stride) {
  return ((size_t)y * (size_t)stride) + ((size_t)x * 4U);
}

static void fill_incrementing(BYTE *pixels, size_t pixel_count, BYTE first) {
  size_t i = 0;

  for (i = 0; i < pixel_count; i++)
    pixels[i] = (BYTE)(first + (BYTE)i);
}

static void copy_bytes(BYTE *dest, const BYTE *src, size_t count) {
  size_t i = 0;

  for (i = 0; i < count; i++)
    dest[i] = src[i];
}

static int test_init_uninit(void) {
  ViewerFramebuffer fb = {0};

  if (!expect_true(viewer_framebuffer_init(&fb), "init succeeds"))
    return 0;
  if (!expect_true(fb.initialized, "framebuffer marked initialized"))
    return 0;

  viewer_framebuffer_uninit(&fb);
  return expect_true(!fb.initialized, "framebuffer uninitialized");
}

static int test_resize_generation_and_dirty(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_framebuffer_init(&fb), "init resize test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 4, 3, 16, 32),
                         "resize succeeds");
  ok = ok && expect_uint64(fb.generation, 1, "resize increments generation");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &snapshot),
                         "snapshot after resize succeeds");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "resize creates one dirty rect");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].left, 0, "full rect left");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].top, 0, "full rect top");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].right, 3, "full rect right");
  ok = ok &&
       expect_uint32(snapshot.dirty_rects[0].bottom, 2, "full rect bottom");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_update_snapshot_copy_and_dirty(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE base_pixels[36] = {0};
  BYTE pixels[36] = {0};
  RECTANGLE_16 dirty[2] = {{1, 1, 2, 2}, {0, 0, 1, 1}};
  int ok = 1;

  fill_incrementing(base_pixels, sizeof(base_pixels), 1U);
  copy_bytes(pixels, base_pixels, sizeof(pixels));
  pixels[pixel_offset(1, 1, 12)] = 0xA0U;
  pixels[pixel_offset(2, 2, 12)] = 0xA1U;
  pixels[pixel_offset(0, 0, 12)] = 0xA2U;

  ok = ok && expect_true(viewer_framebuffer_init(&fb), "init update test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 3, 3, 12, 32),
                         "resize update test");
  ok = ok && expect_true(viewer_framebuffer_update_pixels(&fb, base_pixels, 12,
                                                          NULL, 0),
                         "seed full frame before dirty update");
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 12, dirty, 2),
                   "update pixels succeeds");
  ok = ok && expect_uint64(fb.generation, 3, "update increments generation");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &snapshot),
                         "snapshot after update succeeds");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 2,
                           "explicit dirty rect count preserved");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].left, 1,
                           "first dirty rect preserved");
  ok = ok && expect_uint32(snapshot.dirty_rects[1].bottom, 1,
                           "second dirty rect preserved");
  ok = ok && expect_uint32(snapshot.pixels[pixel_offset(1, 1, 12)],
                           pixels[pixel_offset(1, 1, 12)],
                           "dirty pixel from first rect copied");
  ok = ok && expect_uint32(snapshot.pixels[pixel_offset(2, 2, 12)],
                           pixels[pixel_offset(2, 2, 12)],
                           "dirty pixel from second row copied");
  ok = ok && expect_uint32(snapshot.pixels[pixel_offset(0, 0, 12)],
                           pixels[pixel_offset(0, 0, 12)],
                           "dirty pixel from second rect copied");
  ok = ok && expect_uint32(snapshot.pixels[pixel_offset(2, 0, 12)],
                           base_pixels[pixel_offset(2, 0, 12)],
                           "non-dirty pixel remains from previous frame");

  pixels[0] = 0xFFU;
  ok = ok &&
       expect_uint32(snapshot.pixels[0], 0xA2U, "snapshot owns pixel copy");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_dirty_rect_update_preserves_pixels_outside_dirty_rect(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerFramebufferMetrics metrics = {0};
  BYTE base_pixels[48] = {0};
  BYTE pixels[48] = {0};
  RECTANGLE_16 dirty = {1, 1, 2, 1};
  size_t dirty_start = pixel_offset(1, 1, 16);
  int ok = 1;

  fill_incrementing(base_pixels, sizeof(base_pixels), 10U);
  copy_bytes(pixels, base_pixels, sizeof(pixels));
  pixels[0] = 0xE0U;
  pixels[pixel_offset(3, 2, 16)] = 0xE1U;
  for (size_t i = 0; i < 8U; i++)
    pixels[dirty_start + i] = (BYTE)(0xB0U + i);

  ok = ok &&
       expect_true(viewer_framebuffer_init(&fb), "init dirty-only copy test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 4, 3, 16, 32),
                         "resize dirty-only copy test");
  ok = ok && expect_true(viewer_framebuffer_update_pixels(&fb, base_pixels, 16,
                                                          NULL, 0),
                         "seed dirty-only copy test");
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 16, &dirty, 1),
                   "dirty-only update succeeds");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &snapshot),
                         "dirty-only snapshot succeeds");
  ok = ok && expect_true(viewer_framebuffer_get_metrics(&fb, &metrics),
                         "dirty-only metrics query succeeds");

  ok = ok && expect_uint32(snapshot.pixels[0], base_pixels[0],
                           "top-left non-dirty pixel preserved");
  ok = ok && expect_uint32(snapshot.pixels[pixel_offset(3, 2, 16)],
                           base_pixels[pixel_offset(3, 2, 16)],
                           "bottom-right non-dirty pixel preserved");
  for (size_t i = 0; ok && (i < 8U); i++) {
    ok = expect_uint32(snapshot.pixels[dirty_start + i],
                       pixels[dirty_start + i], "dirty rectangle bytes copied");
  }
  ok = ok && expect_uint64(metrics.last_update_dirty_bytes, 8,
                           "dirty-only update dirty bytes");
  ok = ok && expect_uint64(metrics.last_update_copied_bytes, 8,
                           "dirty-only update copied bytes");
  ok = ok && expect_uint64(metrics.last_update_full_frame_bytes, 48,
                           "dirty-only update full frame bytes");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_full_frame_update_copies_all_pixels_after_dirty_copy(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerFramebufferMetrics metrics = {0};
  BYTE base_pixels[48] = {0};
  BYTE dirty_pixels[48] = {0};
  BYTE full_pixels[48] = {0};
  RECTANGLE_16 dirty = {1, 1, 2, 1};
  int ok = 1;

  fill_incrementing(base_pixels, sizeof(base_pixels), 1U);
  fill_incrementing(dirty_pixels, sizeof(dirty_pixels), 80U);
  fill_incrementing(full_pixels, sizeof(full_pixels), 140U);

  ok = ok && expect_true(viewer_framebuffer_init(&fb),
                         "init full-frame fallback test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 4, 3, 16, 32),
                         "resize full-frame fallback test");
  ok = ok && expect_true(viewer_framebuffer_update_pixels(&fb, base_pixels, 16,
                                                          NULL, 0),
                         "seed full-frame fallback test");
  ok = ok && expect_true(viewer_framebuffer_update_pixels(&fb, dirty_pixels, 16,
                                                          &dirty, 1),
                         "dirty update before full-frame fallback");
  ok = ok && expect_true(viewer_framebuffer_update_pixels(&fb, full_pixels, 16,
                                                          NULL, 0),
                         "full-frame fallback update succeeds");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &snapshot),
                         "full-frame fallback snapshot succeeds");
  ok = ok && expect_true(viewer_framebuffer_get_metrics(&fb, &metrics),
                         "full-frame fallback metrics query succeeds");

  for (size_t i = 0; ok && (i < sizeof(full_pixels)); i++)
    ok = expect_uint32(snapshot.pixels[i], full_pixels[i],
                       "full-frame fallback copied byte");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "full-frame fallback has full dirty rect");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].right, 3,
                           "full-frame fallback dirty right");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].bottom, 2,
                           "full-frame fallback dirty bottom");
  ok = ok && expect_uint64(metrics.last_update_dirty_rect_count, 0,
                           "full-frame fallback original dirty count");
  ok = ok && expect_uint64(metrics.last_update_dirty_bytes, 48,
                           "full-frame fallback dirty bytes");
  ok = ok && expect_uint64(metrics.last_update_copied_bytes, 48,
                           "full-frame fallback copied bytes");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_dirty_rect_update_documents_incomplete_dirty_risk(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE base_pixels[48] = {0};
  BYTE pixels[48] = {0};
  RECTANGLE_16 dirty = {1, 1, 1, 1};
  size_t omitted_pixel = pixel_offset(2, 1, 16);
  int ok = 1;

  fill_incrementing(base_pixels, sizeof(base_pixels), 30U);
  copy_bytes(pixels, base_pixels, sizeof(pixels));
  pixels[pixel_offset(1, 1, 16)] = 0xC0U;
  pixels[omitted_pixel] = 0xC1U;

  ok = ok && expect_true(viewer_framebuffer_init(&fb),
                         "init incomplete dirty risk test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 4, 3, 16, 32),
                         "resize incomplete dirty risk test");
  ok = ok && expect_true(viewer_framebuffer_update_pixels(&fb, base_pixels, 16,
                                                          NULL, 0),
                         "seed incomplete dirty risk test");
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 16, &dirty, 1),
                   "incomplete dirty update succeeds");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &snapshot),
                         "incomplete dirty snapshot succeeds");
  ok = ok && expect_uint32(snapshot.pixels[pixel_offset(1, 1, 16)], 0xC0U,
                           "declared dirty pixel updated");
  ok = ok &&
       expect_uint32(snapshot.pixels[omitted_pixel], base_pixels[omitted_pixel],
                     "omitted dirty pixel intentionally stays stale");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_update_pixels_records_dirty_copy_scope(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferMetrics metrics = {0};
  BYTE pixels[48] = {0};
  RECTANGLE_16 dirty = {1, 1, 2, 1};
  int ok = 1;

  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(i + 1U);

  ok = ok && expect_true(viewer_framebuffer_init(&fb),
                         "init dirty copy metrics test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 4, 3, 16, 32),
                         "resize dirty copy metrics test");
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 16, &dirty, 1),
                   "dirty copy metrics update succeeds");
  ok = ok && expect_true(viewer_framebuffer_get_metrics(&fb, &metrics),
                         "dirty copy metrics query succeeds");
  ok = ok && expect_uint64(metrics.last_update_dirty_rect_count, 1,
                           "dirty copy metrics dirty rect count");
  ok = ok && expect_uint64(metrics.last_update_dirty_bytes, 8,
                           "dirty copy metrics dirty bytes");
  ok = ok && expect_uint64(metrics.last_update_copied_bytes, 8,
                           "dirty copy metrics copied bytes");
  ok = ok && expect_uint64(metrics.last_update_full_frame_bytes, 48,
                           "dirty copy metrics full-frame bytes");
  ok = ok && expect_true(metrics.last_update_copy_start_us > 0,
                         "dirty copy metrics copy start recorded");

  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_snapshot_records_copy_scope_and_time(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerFramebufferMetrics metrics = {0};
  BYTE pixels[48] = {0};
  int ok = 1;

  ok = ok &&
       expect_true(viewer_framebuffer_init(&fb), "init snapshot metrics test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 4, 3, 16, 32),
                         "resize snapshot metrics test");
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 16, NULL, 0),
                   "snapshot metrics update succeeds");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &snapshot),
                         "snapshot metrics snapshot succeeds");
  ok = ok && expect_true(viewer_framebuffer_get_metrics(&fb, &metrics),
                         "snapshot metrics query succeeds");
  ok = ok && expect_uint64(metrics.last_snapshot_copied_bytes, 48,
                           "snapshot metrics copied bytes");
  ok = ok && expect_true(metrics.last_snapshot_copy_start_us > 0,
                         "snapshot metrics copy start recorded");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_dirty_snapshot_copies_only_dirty_bounds(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerFramebufferMetrics metrics = {0};
  BYTE pixels[48] = {0};
  RECTANGLE_16 dirty = {1, 1, 2, 1};
  int ok = 1;

  ok = ok && expect_true(viewer_framebuffer_init(&fb),
                         "init dirty snapshot metrics test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 4, 3, 16, 32),
                         "resize dirty snapshot metrics test");
  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(i + 1U);
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 16, &dirty, 1),
                   "dirty snapshot update succeeds");
  ok = ok && expect_true(viewer_framebuffer_dirty_snapshot(&fb, 64, &snapshot),
                         "dirty snapshot succeeds");
  ok = ok && expect_uint32(snapshot.width, 4, "dirty snapshot surface width");
  ok = ok && expect_uint32(snapshot.height, 3, "dirty snapshot surface height");
  ok = ok && expect_uint32(snapshot.pixel_origin_x, 1,
                           "dirty snapshot pixel origin x");
  ok = ok && expect_uint32(snapshot.pixel_origin_y, 1,
                           "dirty snapshot pixel origin y");
  ok = ok &&
       expect_uint32(snapshot.pixel_width, 2, "dirty snapshot pixel width");
  ok = ok &&
       expect_uint32(snapshot.pixel_height, 1, "dirty snapshot pixel height");
  ok = ok && expect_uint32(snapshot.stride, 8, "dirty snapshot tight stride");
  ok = ok && expect_uint32((UINT32)snapshot.pixel_bytes, 8,
                           "dirty snapshot copied bytes");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "dirty snapshot dirty count");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].left, dirty.left,
                           "dirty snapshot left preserved");
  ok = ok && expect_uint32(snapshot.pixels[0], pixels[20],
                           "dirty snapshot first dirty byte");
  ok = ok && expect_true(viewer_framebuffer_get_metrics(&fb, &metrics),
                         "dirty snapshot metrics query succeeds");
  ok = ok && expect_uint64(metrics.last_snapshot_copied_bytes, 8,
                           "dirty snapshot metrics copied bytes");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_snapshot_dirty_rects_copies_batch_bounds(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerFramebufferMetrics metrics = {0};
  BYTE pixels[80] = {0};
  RECTANGLE_16 update_dirty = {0, 0, 0, 0};
  RECTANGLE_16 batch_dirty[2] = {{1, 1, 1, 1}, {3, 2, 3, 2}};
  int ok = 1;

  ok = ok && expect_true(viewer_framebuffer_init(&fb),
                         "init batch dirty snapshot test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 5, 4, 20, 32),
                         "resize batch dirty snapshot test");
  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(0x40U + i);
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 20, NULL, 0),
                   "batch dirty snapshot full update succeeds");
  pixels[0] = 0xF0U;
  ok = ok && expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 20,
                                                          &update_dirty, 1),
                         "batch dirty snapshot dirty update succeeds");
  ok = ok && expect_true(viewer_framebuffer_snapshot_dirty_rects(
                             &fb, batch_dirty, 2, &snapshot),
                         "batch dirty snapshot succeeds");
  ok = ok && expect_uint32(snapshot.pixel_origin_x, 1,
                           "batch dirty snapshot origin x");
  ok = ok && expect_uint32(snapshot.pixel_origin_y, 1,
                           "batch dirty snapshot origin y");
  ok = ok &&
       expect_uint32(snapshot.pixel_width, 3, "batch dirty snapshot width");
  ok = ok &&
       expect_uint32(snapshot.pixel_height, 2, "batch dirty snapshot height");
  ok = ok && expect_uint32((UINT32)snapshot.pixel_bytes, 24,
                           "batch dirty snapshot bytes");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 2,
                           "batch dirty snapshot rect count");
  ok = ok && expect_uint32(snapshot.dirty_rects[1].left, 3,
                           "batch dirty snapshot metadata preserved");
  ok = ok && expect_uint32(snapshot.pixels[0], pixels[24],
                           "batch dirty snapshot first byte");
  ok = ok && expect_true(viewer_framebuffer_get_metrics(&fb, &metrics),
                         "batch dirty snapshot metrics query succeeds");
  ok = ok && expect_uint64(metrics.last_snapshot_copied_bytes, 24,
                           "batch dirty snapshot metrics copied bytes");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_process_cpu_sample_available(void) {
  PlatformProcessCpuSample sample = {0};
  int ok = 1;

  ok = ok && expect_true(platform_get_process_cpu_sample(&sample),
                         "process CPU sample available");
  ok = ok && expect_true(sample.wall_time_ms > 0,
                         "process CPU sample records wall time");

  return ok;
}

static int test_dirty_rect_validation(void) {
  RECTANGLE_16 valid = {1, 1, 2, 2};
  RECTANGLE_16 one_pixel = {2, 2, 2, 2};
  RECTANGLE_16 left_top_edge = {0, 0, 0, 0};
  RECTANGLE_16 right_bottom_edge = {3, 2, 3, 2};
  RECTANGLE_16 reversed_x = {2, 1, 1, 2};
  RECTANGLE_16 reversed_y = {1, 2, 2, 1};
  RECTANGLE_16 out_of_bounds_x = {1, 1, 4, 2};
  RECTANGLE_16 out_of_bounds_y = {1, 1, 2, 3};
  int ok = 1;

  ok = ok && expect_true(viewer_framebuffer_dirty_rect_valid(4, 3, &valid),
                         "valid dirty rect accepted");
  ok = ok && expect_true(viewer_framebuffer_dirty_rect_valid(4, 3, &one_pixel),
                         "one-pixel dirty rect accepted");
  ok = ok &&
       expect_true(viewer_framebuffer_dirty_rect_valid(4, 3, &left_top_edge),
                   "left/top edge dirty rect accepted");
  ok = ok && expect_true(
                 viewer_framebuffer_dirty_rect_valid(4, 3, &right_bottom_edge),
                 "right/bottom edge dirty rect accepted");
  ok = ok && expect_true(!viewer_framebuffer_dirty_rect_valid(4, 3, NULL),
                         "null dirty rect rejected");
  ok = ok && expect_true(!viewer_framebuffer_dirty_rect_valid(0, 3, &valid),
                         "zero width dirty rect rejected");
  ok = ok && expect_true(!viewer_framebuffer_dirty_rect_valid(4, 0, &valid),
                         "zero height dirty rect rejected");
  ok =
      ok && expect_true(!viewer_framebuffer_dirty_rect_valid(4, 3, &reversed_x),
                        "reversed x dirty rect rejected");
  ok =
      ok && expect_true(!viewer_framebuffer_dirty_rect_valid(4, 3, &reversed_y),
                        "reversed y dirty rect rejected");
  ok = ok &&
       expect_true(!viewer_framebuffer_dirty_rect_valid(4, 3, &out_of_bounds_x),
                   "out of bounds x dirty rect rejected");
  ok = ok &&
       expect_true(!viewer_framebuffer_dirty_rect_valid(4, 3, &out_of_bounds_y),
                   "out of bounds y dirty rect rejected");

  return ok;
}

static int test_invalid_update_rejected_without_side_effects(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot before = {0};
  ViewerFramebufferSnapshot after = {0};
  BYTE pixels[16] = {0};
  RECTANGLE_16 valid = {0, 0, 0, 0};
  RECTANGLE_16 invalid = {0, 0, 2, 0};
  int ok = 1;

  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(i + 1U);

  ok = ok && expect_true(viewer_framebuffer_init(&fb),
                         "init invalid update side-effect test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 2, 2, 8, 32),
                         "resize invalid update side-effect test");
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 8, &valid, 1),
                   "valid update before invalid update");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &before),
                         "snapshot before invalid update");

  pixels[0] = 0xFFU;
  ok = ok && expect_true(
                 !viewer_framebuffer_update_pixels(&fb, pixels, 8, &invalid, 1),
                 "invalid update dirty rect rejected");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &after),
                         "snapshot after invalid update");
  ok = ok && expect_uint64(after.generation, before.generation,
                           "invalid update preserves generation");
  ok = ok && expect_uint32(after.dirty_rect_count, before.dirty_rect_count,
                           "invalid update preserves dirty count");
  ok = ok &&
       expect_uint32(after.dirty_rects[0].right, before.dirty_rects[0].right,
                     "invalid update preserves dirty rect");
  ok = ok && expect_uint32(after.pixels[0], before.pixels[0],
                           "invalid update preserves pixels");

  viewer_framebuffer_snapshot_free(&before);
  viewer_framebuffer_snapshot_free(&after);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_invalid_mark_dirty_rejected_without_side_effects(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot before = {0};
  ViewerFramebufferSnapshot after = {0};
  RECTANGLE_16 invalid = {1, 0, 0, 0};
  int ok = 1;

  ok = ok && expect_true(viewer_framebuffer_init(&fb),
                         "init invalid mark side-effect test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 2, 2, 8, 32),
                         "resize invalid mark side-effect test");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &before),
                         "snapshot before invalid mark");
  ok = ok && expect_true(!viewer_framebuffer_mark_dirty(&fb, &invalid),
                         "invalid mark dirty rejected");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &after),
                         "snapshot after invalid mark");
  ok = ok && expect_uint64(after.generation, before.generation,
                           "invalid mark preserves generation");
  ok = ok && expect_uint32(after.dirty_rect_count, before.dirty_rect_count,
                           "invalid mark preserves dirty count");
  ok = ok &&
       expect_uint32(after.dirty_rects[0].bottom, before.dirty_rects[0].bottom,
                     "invalid mark preserves dirty rect");

  viewer_framebuffer_snapshot_free(&before);
  viewer_framebuffer_snapshot_free(&after);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_dirty_overflow(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[4] = {1, 2, 3, 4};
  RECTANGLE_16 dirty[VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS + 1U] = {0};
  int ok = 1;

  for (UINT32 i = 0; i < (VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS + 1U); i++) {
    dirty[i].left = 0;
    dirty[i].top = 0;
    dirty[i].right = 0;
    dirty[i].bottom = 0;
  }

  ok = ok && expect_true(viewer_framebuffer_init(&fb), "init overflow test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 1, 1, 4, 32),
                         "resize overflow test");
  ok = ok && expect_true(viewer_framebuffer_update_pixels(
                             &fb, pixels, 4, dirty,
                             VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS + 1U),
                         "overflow update succeeds");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &snapshot),
                         "overflow snapshot succeeds");
  ok = ok && expect_uint32(snapshot.dirty_rect_count,
                           VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS,
                           "overflow caps dirty rect count");
  ok = ok && expect_true(snapshot.dirty_overflow, "overflow flag set");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_mark_dirty_accumulation_and_generation(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  RECTANGLE_16 dirty_a = {0, 0, 1, 1};
  RECTANGLE_16 dirty_b = {2, 1, 3, 2};
  UINT64 generation_after_resize = 0;
  int ok = 1;

  ok = ok && expect_true(viewer_framebuffer_init(&fb), "init mark dirty test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 4, 3, 16, 32),
                         "resize mark dirty test");
  generation_after_resize = fb.generation;

  ok = ok && expect_true(viewer_framebuffer_mark_dirty(&fb, &dirty_a),
                         "first mark dirty succeeds");
  ok = ok && expect_true(viewer_framebuffer_mark_dirty(&fb, &dirty_b),
                         "second mark dirty succeeds");
  ok = ok && expect_uint64(fb.generation, generation_after_resize,
                           "mark dirty does not increment generation");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &snapshot),
                         "snapshot after mark dirty succeeds");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 3,
                           "mark dirty accumulates with resize dirty rect");
  ok = ok && expect_uint32(snapshot.dirty_rects[1].left, dirty_a.left,
                           "first marked dirty rect preserved");
  ok = ok && expect_uint32(snapshot.dirty_rects[2].right, dirty_b.right,
                           "second marked dirty rect preserved");
  ok = ok && expect_true(!snapshot.dirty_overflow,
                         "mark dirty accumulation has no overflow");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_mark_dirty_overflow(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  RECTANGLE_16 dirty = {0, 0, 0, 0};
  int ok = 1;

  ok = ok && expect_true(viewer_framebuffer_init(&fb),
                         "init mark dirty overflow test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 1, 1, 4, 32),
                         "resize mark dirty overflow test");

  for (UINT32 i = 0; ok && (i < VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS); i++)
    ok = expect_true(viewer_framebuffer_mark_dirty(&fb, &dirty),
                     "mark dirty overflow add succeeds");

  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &snapshot),
                         "mark dirty overflow snapshot succeeds");
  ok = ok && expect_uint32(snapshot.dirty_rect_count,
                           VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS,
                           "mark dirty overflow caps dirty rect count");
  ok = ok &&
       expect_true(snapshot.dirty_overflow, "mark dirty overflow flag set");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
  return ok;
}

static int test_snapshot_free_resets_snapshot(void) {
  ViewerFramebufferSnapshot snapshot = {0};
  int ok = 1;

  snapshot.width = 10;
  snapshot.height = 11;
  snapshot.stride = 40;
  snapshot.pixel_format = 32;
  snapshot.generation = 7;
  snapshot.pixels = (BYTE *)calloc(1, 4);
  snapshot.pixel_bytes = 4;
  snapshot.dirty_rect_count = 1;
  snapshot.dirty_overflow = TRUE;
  if (!snapshot.pixels)
    return 0;

  viewer_framebuffer_snapshot_free(&snapshot);
  ok = ok && expect_uint32(snapshot.width, 0, "snapshot free resets width");
  ok = ok && expect_uint32(snapshot.height, 0, "snapshot free resets height");
  ok = ok && expect_uint32(snapshot.stride, 0, "snapshot free resets stride");
  ok = ok && expect_uint32(snapshot.pixel_origin_x, 0,
                           "snapshot free resets origin x");
  ok = ok && expect_uint32(snapshot.pixel_origin_y, 0,
                           "snapshot free resets origin y");
  ok = ok && expect_uint32(snapshot.pixel_width, 0,
                           "snapshot free resets pixel width");
  ok = ok && expect_uint32(snapshot.pixel_height, 0,
                           "snapshot free resets pixel height");
  ok = ok &&
       expect_uint64(snapshot.generation, 0, "snapshot free resets generation");
  ok = ok && expect_true(snapshot.pixels == NULL,
                         "snapshot free clears pixel pointer");
  ok = ok && expect_uint32((UINT32)snapshot.pixel_bytes, 0,
                           "snapshot free resets pixel byte count");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 0,
                           "snapshot free resets dirty rect count");
  ok = ok && expect_true(!snapshot.dirty_overflow,
                         "snapshot free resets overflow flag");

  return ok;
}

static int test_invalid_args(void) {
  ViewerFramebuffer fb = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[4] = {0};
  RECTANGLE_16 dirty = {0, 0, 0, 0};
  int ok = 1;

  ok = ok && expect_true(!viewer_framebuffer_init(NULL), "null init fails");
  ok = ok && expect_true(!viewer_framebuffer_resize(&fb, 1, 1, 4, 32),
                         "resize before init fails");
  ok = ok && expect_true(viewer_framebuffer_init(&fb), "init invalid test");
  ok = ok && expect_true(!viewer_framebuffer_resize(&fb, 0, 1, 4, 32),
                         "zero width resize fails");
  ok = ok && expect_true(!viewer_framebuffer_snapshot(&fb, &snapshot),
                         "snapshot without pixels fails");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 1, 1, 4, 32),
                         "valid resize succeeds");
  ok = ok &&
       expect_true(!viewer_framebuffer_update_pixels(&fb, NULL, 4, &dirty, 1),
                   "null pixel update fails");
  ok = ok &&
       expect_true(!viewer_framebuffer_update_pixels(&fb, pixels, 0, &dirty, 1),
                   "zero source stride update fails");
  ok = ok &&
       expect_true(!viewer_framebuffer_update_pixels(&fb, pixels, 4, NULL, 1),
                   "missing dirty rect array fails");
  ok = ok && expect_true(!viewer_framebuffer_mark_dirty(&fb, NULL),
                         "null dirty rect fails");

  viewer_framebuffer_uninit(&fb);
  return ok;
}

int main(void) {
  if (!test_init_uninit())
    return 1;
  if (!test_resize_generation_and_dirty())
    return 1;
  if (!test_update_snapshot_copy_and_dirty())
    return 1;
  if (!test_dirty_rect_update_preserves_pixels_outside_dirty_rect())
    return 1;
  if (!test_full_frame_update_copies_all_pixels_after_dirty_copy())
    return 1;
  if (!test_dirty_rect_update_documents_incomplete_dirty_risk())
    return 1;
  if (!test_update_pixels_records_dirty_copy_scope())
    return 1;
  if (!test_snapshot_records_copy_scope_and_time())
    return 1;
  if (!test_dirty_snapshot_copies_only_dirty_bounds())
    return 1;
  if (!test_snapshot_dirty_rects_copies_batch_bounds())
    return 1;
  if (!test_process_cpu_sample_available())
    return 1;
  if (!test_dirty_rect_validation())
    return 1;
  if (!test_invalid_update_rejected_without_side_effects())
    return 1;
  if (!test_invalid_mark_dirty_rejected_without_side_effects())
    return 1;
  if (!test_dirty_overflow())
    return 1;
  if (!test_mark_dirty_accumulation_and_generation())
    return 1;
  if (!test_mark_dirty_overflow())
    return 1;
  if (!test_snapshot_free_resets_snapshot())
    return 1;
  if (!test_invalid_args())
    return 1;
  return 0;
}
