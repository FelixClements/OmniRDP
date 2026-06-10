#include "viewer_gfx_codec_rfx.h"

#include <freerdp/codec/color.h>
#include <freerdp/settings_types.h>
#include <stdio.h>
#include <stdlib.h>

static int expect_true(BOOL value, const char *message) {
  if (!value) {
    (void)fprintf_s(stderr, "FAIL: %s\n", message);
    return 0;
  }
  return 1;
}

static int expect_false(BOOL value, const char *message) {
  if (value) {
    (void)fprintf_s(stderr, "FAIL: %s\n", message);
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

static ViewerFramebufferSnapshot make_snapshot(BYTE *pixels, UINT32 width,
                                               UINT32 height, UINT32 stride,
                                               size_t pixel_bytes) {
  ViewerFramebufferSnapshot snapshot = {0};

  snapshot.width = width;
  snapshot.height = height;
  snapshot.stride = stride;
  snapshot.pixel_format = PIXEL_FORMAT_BGRX32;
  snapshot.pixels = pixels;
  snapshot.pixel_bytes = pixel_bytes;
  return snapshot;
}

static void fill_pixels(BYTE *pixels, size_t count) {
  for (size_t i = 0; i < count; i++)
    pixels[i] = (BYTE)(i + 1U);
}

static int test_context_creation_and_status(void) {
  ViewerGfxRfxContext *context = viewer_gfx_rfx_context_new();
  int ok = 1;

  ok = ok && expect_true(viewer_gfx_rfx_is_available(),
                         "RFX codec reports available");
  ok = ok && expect_true(viewer_gfx_rfx_disabled_reason() == NULL,
                         "available codec has no disabled reason");
  ok = ok && expect_true(context != NULL, "RFX context creates");
  ok = ok && expect_true(viewer_gfx_rfx_context_reset(context, 64, 64),
                         "RFX context reset succeeds");
  ok = ok && expect_false(viewer_gfx_rfx_context_reset(context, 0, 64),
                          "zero-width reset rejected");
  viewer_gfx_rfx_context_free(context);
  viewer_gfx_rfx_context_free(NULL);
  return ok;
}

static int test_context_default_threading_flags_are_disabled(void) {
  ViewerGfxRfxContext *context = viewer_gfx_rfx_context_new();
  int ok = 1;

  ok = ok && expect_true(context != NULL, "default RFX context creates");
  ok = ok && expect_uint32(viewer_gfx_rfx_test_last_threading_flags(),
                           THREADING_FLAGS_DISABLE_THREADS,
                           "default RFX threading disabled");

  viewer_gfx_rfx_context_free(context);
  return ok;
}

static int test_context_threaded_mode_uses_threading_flags(void) {
  ViewerGfxRfxContext *context = viewer_gfx_rfx_context_new_ex(TRUE);
  int ok = 1;

  ok = ok && expect_true(context != NULL, "threaded RFX context creates");
  ok = ok && expect_uint32(viewer_gfx_rfx_test_last_threading_flags(), 0,
                           "threaded RFX context clears disable flag");

  viewer_gfx_rfx_context_free(context);

  context = viewer_gfx_rfx_context_new_ex(FALSE);
  ok = ok && expect_true(context != NULL,
                         "explicit non-threaded RFX context creates");
  ok = ok && expect_uint32(viewer_gfx_rfx_test_last_threading_flags(),
                           THREADING_FLAGS_DISABLE_THREADS,
                           "explicit non-threaded RFX context disables");

  viewer_gfx_rfx_context_free(context);
  return ok;
}

static int test_full_frame_encode_builds_cavideo_command(void) {
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 4, 4, 16, 64);
  ViewerGfxRfxContext *context = viewer_gfx_rfx_context_new();
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  fill_pixels(pixels, sizeof(pixels));
  ok = ok && expect_true(context != NULL, "RFX context creates for full frame");
  ok = ok && expect_true(viewer_gfx_rfx_build_surface_command(
                             context, &snapshot, 9, &command),
                         "full-frame RFX command builds");
  ok = ok && expect_uint32(command.surfaceId, 9, "surface id");
  ok = ok &&
       expect_uint32(command.codecId, RDPGFX_CODECID_CAVIDEO, "RFX codec id");
  ok = ok && expect_uint32(command.contextId, 0, "context id");
  ok = ok && expect_uint32(command.format, PIXEL_FORMAT_BGRX32, "format");
  ok = ok && expect_uint32(command.left, 0, "left");
  ok = ok && expect_uint32(command.top, 0, "top");
  ok = ok && expect_uint32(command.right, 4, "exclusive right");
  ok = ok && expect_uint32(command.bottom, 4, "exclusive bottom");
  ok = ok && expect_uint32(command.width, 4, "width");
  ok = ok && expect_uint32(command.height, 4, "height");
  ok = ok && expect_true(command.length > 0, "RFX payload length nonzero");
  ok = ok && expect_true(command.data != NULL, "RFX payload allocated");

  viewer_gfx_rfx_surface_command_reset(&command);
  viewer_gfx_rfx_context_free(context);
  return ok;
}

static int test_dirty_rect_encode_builds_destination_bounds(void) {
  BYTE pixels[100] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 5, 5, 20, 100);
  RECTANGLE_16 dirty_rect = {1, 2, 3, 4};
  ViewerGfxRfxContext *context = viewer_gfx_rfx_context_new();
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  fill_pixels(pixels, sizeof(pixels));
  ok = ok && expect_true(context != NULL, "RFX context creates for dirty rect");
  ok = ok && expect_true(viewer_gfx_rfx_build_surface_command_rect(
                             context, &snapshot, 4, &dirty_rect, &command),
                         "dirty-rect RFX command builds");
  ok = ok && expect_uint32(command.surfaceId, 4, "dirty surface id");
  ok = ok && expect_uint32(command.codecId, RDPGFX_CODECID_CAVIDEO,
                           "dirty RFX codec id");
  ok = ok && expect_uint32(command.left, 1, "dirty left");
  ok = ok && expect_uint32(command.top, 2, "dirty top");
  ok = ok && expect_uint32(command.right, 4, "dirty exclusive right");
  ok = ok && expect_uint32(command.bottom, 5, "dirty exclusive bottom");
  ok = ok && expect_uint32(command.width, 3, "dirty width");
  ok = ok && expect_uint32(command.height, 3, "dirty height");
  ok = ok && expect_true(command.length > 0, "dirty RFX payload nonzero");
  ok = ok && expect_true(command.data != NULL, "dirty RFX payload allocated");

  viewer_gfx_rfx_surface_command_reset(&command);
  viewer_gfx_rfx_context_free(context);
  return ok;
}

static int test_dirty_rect_encode_uses_cropped_snapshot_origin(void) {
  BYTE pixels[36] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 5, 5, 12, 36);
  RECTANGLE_16 dirty_rect = {1, 2, 3, 4};
  ViewerGfxRfxContext *context = viewer_gfx_rfx_context_new();
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  snapshot.pixel_origin_x = 1;
  snapshot.pixel_origin_y = 2;
  snapshot.pixel_width = 3;
  snapshot.pixel_height = 3;
  fill_pixels(pixels, sizeof(pixels));

  ok = ok && expect_true(context != NULL,
                         "RFX context creates for cropped dirty rect");
  ok = ok && expect_true(viewer_gfx_rfx_build_surface_command_rect(
                             context, &snapshot, 11, &dirty_rect, &command),
                         "cropped dirty-rect RFX command builds");
  ok = ok && expect_uint32(command.surfaceId, 11, "cropped dirty surface id");
  ok = ok && expect_uint32(command.codecId, RDPGFX_CODECID_CAVIDEO,
                           "cropped dirty RFX codec id");
  ok = ok && expect_uint32(command.left, 1, "cropped dirty left");
  ok = ok && expect_uint32(command.top, 2, "cropped dirty top");
  ok = ok && expect_uint32(command.right, 4, "cropped dirty exclusive right");
  ok = ok && expect_uint32(command.bottom, 5, "cropped dirty exclusive bottom");
  ok = ok && expect_uint32(command.width, 3, "cropped dirty width");
  ok = ok && expect_uint32(command.height, 3, "cropped dirty height");
  ok = ok &&
       expect_true(command.length > 0, "cropped dirty RFX payload nonzero");
  ok = ok &&
       expect_true(command.data != NULL, "cropped dirty RFX payload allocated");
  viewer_gfx_rfx_surface_command_reset(&command);

  ok = ok && expect_false(viewer_gfx_rfx_build_surface_command(
                              context, &snapshot, 11, &command),
                          "full RFX command rejects cropped snapshot");

  viewer_gfx_rfx_context_free(context);
  return ok;
}

static int test_dirty_rect_batch_encode_builds_bounding_surface(void) {
  BYTE pixels[128] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 8, 4, 32, 128);
  RECTANGLE_16 dirty_rects[2] = {{1, 1, 2, 1}, {4, 2, 5, 3}};
  ViewerGfxRfxContext *context = viewer_gfx_rfx_context_new();
  RDPGFX_SURFACE_COMMAND command = {0};
  BOOL batched = FALSE;
  int ok = 1;

  fill_pixels(pixels, sizeof(pixels));
  ok =
      ok && expect_true(context != NULL, "RFX context creates for dirty batch");
  ok = ok && expect_true(viewer_gfx_rfx_build_surface_command_rects(
                             context, &snapshot, 12, dirty_rects, 2, &command,
                             &batched),
                         "dirty-rect RFX batch command builds");
  ok = ok && expect_true(batched, "dirty batch reports batched encode");
  ok = ok && expect_uint32(command.surfaceId, 12, "batch dirty surface id");
  ok = ok && expect_uint32(command.codecId, RDPGFX_CODECID_CAVIDEO,
                           "batch dirty RFX codec id");
  ok = ok && expect_uint32(command.left, 1, "batch dirty left");
  ok = ok && expect_uint32(command.top, 1, "batch dirty top");
  ok = ok && expect_uint32(command.right, 6, "batch dirty exclusive right");
  ok = ok && expect_uint32(command.bottom, 4, "batch dirty exclusive bottom");
  ok = ok && expect_uint32(command.width, 5, "batch dirty width");
  ok = ok && expect_uint32(command.height, 3, "batch dirty height");
  ok = ok && expect_true(command.length > 0, "batch dirty RFX payload nonzero");
  ok = ok && expect_true(command.data != NULL, "batch dirty RFX payload set");

  viewer_gfx_rfx_surface_command_reset(&command);
  viewer_gfx_rfx_context_free(context);
  return ok;
}

static int test_dirty_rect_edge_bounds(void) {
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 4, 4, 16, 64);
  RECTANGLE_16 one_pixel_edge = {3, 3, 3, 3};
  RECTANGLE_16 full_frame = {0, 0, 3, 3};
  ViewerGfxRfxContext *context = viewer_gfx_rfx_context_new();
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  fill_pixels(pixels, sizeof(pixels));
  ok = ok && expect_true(context != NULL, "RFX context creates for edge rects");
  ok = ok && expect_true(viewer_gfx_rfx_build_surface_command_rect(
                             context, &snapshot, 7, &one_pixel_edge, &command),
                         "one-pixel edge RFX dirty rect builds");
  ok = ok && expect_uint32(command.left, 3, "one-pixel RFX left");
  ok = ok && expect_uint32(command.top, 3, "one-pixel RFX top");
  ok = ok && expect_uint32(command.right, 4, "one-pixel RFX exclusive right");
  ok = ok && expect_uint32(command.bottom, 4, "one-pixel RFX exclusive bottom");
  ok = ok && expect_uint32(command.width, 1, "one-pixel RFX width");
  ok = ok && expect_uint32(command.height, 1, "one-pixel RFX height");
  ok = ok && expect_true(command.length > 0, "one-pixel RFX payload nonzero");
  ok = ok && expect_true(command.data != NULL, "one-pixel RFX payload set");
  viewer_gfx_rfx_surface_command_reset(&command);

  ok = ok && expect_true(viewer_gfx_rfx_build_surface_command_rect(
                             context, &snapshot, 8, &full_frame, &command),
                         "full-width full-height RFX dirty rect builds");
  ok = ok && expect_uint32(command.left, 0, "full RFX left");
  ok = ok && expect_uint32(command.top, 0, "full RFX top");
  ok = ok && expect_uint32(command.right, 4, "full RFX exclusive right");
  ok = ok && expect_uint32(command.bottom, 4, "full RFX exclusive bottom");
  ok = ok && expect_uint32(command.width, 4, "full RFX width");
  ok = ok && expect_uint32(command.height, 4, "full RFX height");
  ok = ok && expect_true(command.length > 0, "full RFX payload nonzero");
  ok = ok && expect_true(command.data != NULL, "full RFX payload set");

  viewer_gfx_rfx_surface_command_reset(&command);
  viewer_gfx_rfx_context_free(context);
  return ok;
}

static int test_invalid_inputs_are_rejected_and_reset(void) {
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 4, 4, 16, 64);
  ViewerFramebufferSnapshot invalid = snapshot;
  RECTANGLE_16 bad_rect_order = {2, 0, 1, 0};
  RECTANGLE_16 bad_rect_bounds = {0, 0, 4, 0};
  RECTANGLE_16 valid_rect = {0, 0, 1, 1};
  ViewerGfxRfxContext *context = viewer_gfx_rfx_context_new();
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  fill_pixels(pixels, sizeof(pixels));
  ok = ok &&
       expect_true(context != NULL, "RFX context creates for invalid tests");
  ok = ok && expect_true(viewer_gfx_rfx_build_surface_command(
                             context, &snapshot, 1, &command),
                         "initial RFX command builds before invalid reset");
  ok = ok && expect_true(command.data != NULL, "initial payload allocated");

  invalid.pixel_format = 0;
  ok = ok && expect_false(viewer_gfx_rfx_build_surface_command(
                              context, &invalid, 2, &command),
                          "invalid pixel format rejected");
  ok = ok && expect_true(command.data == NULL, "invalid format clears data");
  ok = ok && expect_uint32(command.length, 0, "invalid format clears length");

  ok = ok && expect_false(viewer_gfx_rfx_build_surface_command(NULL, &snapshot,
                                                               1, &command),
                          "null context rejected");
  ok = ok && expect_false(viewer_gfx_rfx_build_surface_command(context, NULL, 1,
                                                               &command),
                          "null snapshot rejected");
  ok = ok && expect_false(viewer_gfx_rfx_build_surface_command(
                              context, &snapshot, 1, NULL),
                          "null command rejected safely");
  ok = ok && expect_false(viewer_gfx_rfx_build_surface_command_rect(
                              context, &snapshot, 1, NULL, &command),
                          "null dirty rect rejected");
  ok = ok && expect_false(viewer_gfx_rfx_build_surface_command_rect(
                              context, &snapshot, 1, &valid_rect, NULL),
                          "null dirty command rejected safely");
  ok = ok && expect_false(viewer_gfx_rfx_build_surface_command_rect(
                              context, &snapshot, 1, &bad_rect_order, &command),
                          "bad dirty rect order rejected");
  ok =
      ok && expect_false(viewer_gfx_rfx_build_surface_command_rect(
                             context, &snapshot, 1, &bad_rect_bounds, &command),
                         "out-of-bounds dirty rect rejected");
  viewer_gfx_rfx_surface_command_reset(NULL);

  viewer_gfx_rfx_surface_command_reset(&command);
  viewer_gfx_rfx_context_free(context);
  return ok;
}

static int test_replacement_and_reset_cleanup(void) {
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 4, 4, 16, 64);
  ViewerGfxRfxContext *context = viewer_gfx_rfx_context_new();
  RDPGFX_SURFACE_COMMAND command = {0};
  BYTE *first_data = NULL;
  int ok = 1;

  fill_pixels(pixels, sizeof(pixels));
  ok = ok && expect_true(context != NULL, "RFX context creates for cleanup");
  ok = ok && expect_true(viewer_gfx_rfx_build_surface_command(
                             context, &snapshot, 1, &command),
                         "initial command builds");
  first_data = command.data;
  ok = ok && expect_true(first_data != NULL, "initial data allocated");
  ok = ok && expect_true(viewer_gfx_rfx_build_surface_command(
                             context, &snapshot, 2, &command),
                         "replacement command builds");
  ok = ok && expect_uint32(command.surfaceId, 2, "replacement surface id");
  ok = ok && expect_true(command.data != NULL, "replacement data allocated");

  viewer_gfx_rfx_surface_command_reset(&command);
  ok = ok && expect_true(command.data == NULL, "reset clears data");
  ok = ok && expect_uint32(command.length, 0, "reset clears length");
  viewer_gfx_rfx_surface_command_reset(&command);

  viewer_gfx_rfx_context_free(context);
  return ok;
}

static int test_context_new_failure_hook(void) {
  ViewerGfxRfxContext *context = NULL;
  int ok = 1;

  viewer_gfx_rfx_test_set_force_context_new_failure(TRUE);
  context = viewer_gfx_rfx_context_new();
  ok = ok && expect_true(context == NULL, "forced context creation fails");
  viewer_gfx_rfx_test_set_force_context_new_failure(FALSE);
  context = viewer_gfx_rfx_context_new();
  ok = ok && expect_true(context != NULL, "context creation recovers");
  viewer_gfx_rfx_context_free(context);
  return ok;
}

int main(void) {
  int ok = 1;

  ok = ok && test_context_creation_and_status();
  ok = ok && test_context_default_threading_flags_are_disabled();
  ok = ok && test_context_threaded_mode_uses_threading_flags();
  ok = ok && test_full_frame_encode_builds_cavideo_command();
  ok = ok && test_dirty_rect_encode_builds_destination_bounds();
  ok = ok && test_dirty_rect_encode_uses_cropped_snapshot_origin();
  ok = ok && test_dirty_rect_batch_encode_builds_bounding_surface();
  ok = ok && test_dirty_rect_edge_bounds();
  ok = ok && test_invalid_inputs_are_rejected_and_reset();
  ok = ok && test_replacement_and_reset_cleanup();
  ok = ok && test_context_new_failure_hook();

  return ok ? 0 : 1;
}
