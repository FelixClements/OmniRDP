#include "viewer_gfx_codec_clearcodec.h"

#include <freerdp/codec/color.h>
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

static int test_full_frame_encode_builds_clearcodec_command(void) {
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 4, 4, 16, 64);
  ViewerGfxClearCodecContext *context = viewer_gfx_clearcodec_context_new();
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  fill_pixels(pixels, sizeof(pixels));
  ok = ok && expect_true(viewer_gfx_clearcodec_is_available(),
                         "ClearCodec reports available");
  ok = ok && expect_true(viewer_gfx_clearcodec_disabled_reason() == NULL,
                         "available ClearCodec has no disabled reason");
  ok = ok && expect_true(context != NULL, "ClearCodec context creates");
  ok = ok && expect_true(viewer_gfx_clearcodec_build_surface_command(
                             context, &snapshot, 9, &command),
                         "full-frame ClearCodec command builds");
  ok = ok && expect_uint32(command.surfaceId, 9, "surface id");
  ok = ok && expect_uint32(command.codecId, RDPGFX_CODECID_CLEARCODEC,
                           "ClearCodec codec id");
  ok = ok && expect_uint32(command.contextId, 0, "context id");
  ok = ok && expect_uint32(command.format, PIXEL_FORMAT_BGRX32, "format");
  ok = ok && expect_uint32(command.left, 0, "left");
  ok = ok && expect_uint32(command.top, 0, "top");
  ok = ok && expect_uint32(command.right, 4, "exclusive right");
  ok = ok && expect_uint32(command.bottom, 4, "exclusive bottom");
  ok = ok && expect_uint32(command.width, 4, "width");
  ok = ok && expect_uint32(command.height, 4, "height");
  ok = ok &&
       expect_true(command.length > 0, "ClearCodec payload length nonzero");
  ok = ok && expect_true(command.data != NULL, "ClearCodec payload allocated");

  viewer_gfx_clearcodec_surface_command_reset(&command);
  viewer_gfx_clearcodec_context_free(context);
  return ok;
}

static int test_dirty_rect_encode_builds_destination_bounds(void) {
  BYTE pixels[100] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 5, 5, 20, 100);
  RECTANGLE_16 dirty_rect = {1, 2, 3, 4};
  ViewerGfxClearCodecContext *context = viewer_gfx_clearcodec_context_new();
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  fill_pixels(pixels, sizeof(pixels));
  ok = ok && expect_true(context != NULL,
                         "ClearCodec context creates for dirty rect");
  ok = ok && expect_true(viewer_gfx_clearcodec_build_surface_command_rect(
                             context, &snapshot, 4, &dirty_rect, &command),
                         "dirty-rect ClearCodec command builds");
  ok = ok && expect_uint32(command.surfaceId, 4, "dirty surface id");
  ok = ok && expect_uint32(command.codecId, RDPGFX_CODECID_CLEARCODEC,
                           "dirty ClearCodec codec id");
  ok = ok && expect_uint32(command.left, 1, "dirty left");
  ok = ok && expect_uint32(command.top, 2, "dirty top");
  ok = ok && expect_uint32(command.right, 4, "dirty exclusive right");
  ok = ok && expect_uint32(command.bottom, 5, "dirty exclusive bottom");
  ok = ok && expect_uint32(command.width, 3, "dirty width");
  ok = ok && expect_uint32(command.height, 3, "dirty height");
  ok =
      ok && expect_true(command.length > 0, "dirty ClearCodec payload nonzero");
  ok = ok &&
       expect_true(command.data != NULL, "dirty ClearCodec payload allocated");

  viewer_gfx_clearcodec_surface_command_reset(&command);
  viewer_gfx_clearcodec_context_free(context);
  return ok;
}

static int test_invalid_inputs_are_rejected_and_reset(void) {
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 4, 4, 16, 64);
  ViewerFramebufferSnapshot invalid = snapshot;
  RECTANGLE_16 bad_rect_order = {2, 0, 1, 0};
  RECTANGLE_16 bad_rect_bounds = {0, 0, 4, 0};
  RECTANGLE_16 valid_rect = {0, 0, 1, 1};
  ViewerGfxClearCodecContext *context = viewer_gfx_clearcodec_context_new();
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  fill_pixels(pixels, sizeof(pixels));
  ok = ok && expect_true(context != NULL,
                         "ClearCodec context creates for invalid tests");
  ok = ok && expect_true(viewer_gfx_clearcodec_build_surface_command(
                             context, &snapshot, 1, &command),
                         "initial ClearCodec command builds");
  ok = ok && expect_true(command.data != NULL, "initial payload allocated");

  invalid.pixel_format = 0;
  ok = ok && expect_false(viewer_gfx_clearcodec_build_surface_command(
                              context, &invalid, 2, &command),
                          "invalid pixel format rejected");
  ok = ok && expect_true(command.data == NULL, "invalid format clears data");
  ok = ok && expect_uint32(command.length, 0, "invalid format clears length");

  ok = ok && expect_false(viewer_gfx_clearcodec_build_surface_command(
                              NULL, &snapshot, 1, &command),
                          "null context rejected");
  ok = ok && expect_false(viewer_gfx_clearcodec_build_surface_command(
                              context, NULL, 1, &command),
                          "null snapshot rejected");
  ok = ok && expect_false(viewer_gfx_clearcodec_build_surface_command(
                              context, &snapshot, 1, NULL),
                          "null command rejected safely");
  ok = ok && expect_false(viewer_gfx_clearcodec_build_surface_command_rect(
                              context, &snapshot, 1, NULL, &command),
                          "null dirty rect rejected");
  ok = ok && expect_false(viewer_gfx_clearcodec_build_surface_command_rect(
                              context, &snapshot, 1, &valid_rect, NULL),
                          "null dirty command rejected safely");
  ok = ok && expect_false(viewer_gfx_clearcodec_build_surface_command_rect(
                              context, &snapshot, 1, &bad_rect_order, &command),
                          "bad dirty rect order rejected");
  ok =
      ok && expect_false(viewer_gfx_clearcodec_build_surface_command_rect(
                             context, &snapshot, 1, &bad_rect_bounds, &command),
                         "out-of-bounds dirty rect rejected");
  viewer_gfx_clearcodec_surface_command_reset(NULL);

  viewer_gfx_clearcodec_surface_command_reset(&command);
  viewer_gfx_clearcodec_context_free(context);
  return ok;
}

int main(void) {
  int ok = 1;

  ok = ok && test_full_frame_encode_builds_clearcodec_command();
  ok = ok && test_dirty_rect_encode_builds_destination_bounds();
  ok = ok && test_invalid_inputs_are_rejected_and_reset();

  return ok ? 0 : 1;
}
