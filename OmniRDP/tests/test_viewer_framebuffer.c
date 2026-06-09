#include "viewer_framebuffer.h"

#include <stdio.h>
#include <stdlib.h>

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
  BYTE pixels[36] = {0};
  RECTANGLE_16 dirty[2] = {{1, 1, 2, 2}, {0, 0, 1, 1}};
  int ok = 1;

  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(i + 1U);

  ok = ok && expect_true(viewer_framebuffer_init(&fb), "init update test");
  ok = ok && expect_true(viewer_framebuffer_resize(&fb, 3, 3, 12, 32),
                         "resize update test");
  ok = ok &&
       expect_true(viewer_framebuffer_update_pixels(&fb, pixels, 12, dirty, 2),
                   "update pixels succeeds");
  ok = ok && expect_uint64(fb.generation, 2, "update increments generation");
  ok = ok && expect_true(viewer_framebuffer_snapshot(&fb, &snapshot),
                         "snapshot after update succeeds");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 2,
                           "explicit dirty rect count preserved");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].left, 1,
                           "first dirty rect preserved");
  ok = ok && expect_uint32(snapshot.dirty_rects[1].bottom, 1,
                           "second dirty rect preserved");
  for (size_t i = 0; ok && (i < sizeof(pixels)); i++)
    ok = expect_uint32(snapshot.pixels[i], pixels[i], "snapshot pixel copied");

  pixels[0] = 0xFFU;
  ok = ok && expect_uint32(snapshot.pixels[0], 1, "snapshot owns pixel copy");

  viewer_framebuffer_snapshot_free(&snapshot);
  viewer_framebuffer_uninit(&fb);
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
