#include "viewer_gfx_codec_uncompressed.h"

#include <freerdp/codec/color.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static int expect_bytes(const BYTE *actual, const BYTE *expected, size_t length,
                        const char *message) {
  if (!actual || !expected || (memcmp(actual, expected, length) != 0)) {
    (void)fprintf_s(stderr, "FAIL: %s\n", message);
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

static int test_full_frame_repacks_padded_stride(void) {
  BYTE pixels[20] = {0};
  BYTE expected[16] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 2, 2, 10, 20);
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(i + 1U);
  memmove(expected, pixels, 8);
  memmove(expected + 8, pixels + 10, 8);

  ok = ok && expect_true(viewer_gfx_uncompressed_build_surface_command(
                             &snapshot, 7, &command),
                         "full-frame command builds");
  ok = ok && expect_uint32(command.surfaceId, 7, "surface id");
  ok = ok &&
       expect_uint32(command.codecId, RDPGFX_CODECID_UNCOMPRESSED, "codec id");
  ok = ok && expect_uint32(command.contextId, 0, "context id");
  ok = ok && expect_uint32(command.format, PIXEL_FORMAT_BGRX32, "format");
  ok = ok && expect_uint32(command.left, 0, "left");
  ok = ok && expect_uint32(command.top, 0, "top");
  ok = ok && expect_uint32(command.right, 2, "exclusive right");
  ok = ok && expect_uint32(command.bottom, 2, "exclusive bottom");
  ok = ok && expect_uint32(command.width, 2, "width");
  ok = ok && expect_uint32(command.height, 2, "height");
  ok = ok && expect_uint32(command.length, sizeof(expected), "payload length");
  ok = ok && expect_bytes(command.data, expected, sizeof(expected),
                          "full-frame payload is tight top-down BGRX32");

  viewer_gfx_uncompressed_surface_command_reset(&command);
  return ok;
}

static int test_dirty_rect_builds_exclusive_bounds(void) {
  BYTE pixels[48] = {0};
  BYTE expected[16] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 4, 3, 16, 48);
  RECTANGLE_16 dirty_rect = {1, 1, 2, 2};
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(i + 1U);
  memmove(expected, pixels + 20, 8);
  memmove(expected + 8, pixels + 36, 8);

  ok = ok && expect_true(viewer_gfx_uncompressed_build_surface_command_rect(
                             &snapshot, 3, &dirty_rect, &command),
                         "dirty-rect command builds");
  ok = ok && expect_uint32(command.surfaceId, 3, "dirty surface id");
  ok = ok && expect_uint32(command.codecId, RDPGFX_CODECID_UNCOMPRESSED,
                           "dirty codec id");
  ok = ok && expect_uint32(command.contextId, 0, "dirty context id");
  ok = ok && expect_uint32(command.format, PIXEL_FORMAT_BGRX32, "dirty format");
  ok = ok && expect_uint32(command.left, 1, "dirty left");
  ok = ok && expect_uint32(command.top, 1, "dirty top");
  ok = ok && expect_uint32(command.right, 3, "dirty exclusive right");
  ok = ok && expect_uint32(command.bottom, 3, "dirty exclusive bottom");
  ok = ok && expect_uint32(command.width, 2, "dirty width");
  ok = ok && expect_uint32(command.height, 2, "dirty height");
  ok = ok &&
       expect_uint32(command.length, sizeof(expected), "dirty payload length");
  ok = ok && expect_bytes(command.data, expected, sizeof(expected),
                          "dirty payload is tight top-down BGRX32");

  viewer_gfx_uncompressed_surface_command_reset(&command);
  return ok;
}

static int test_dirty_rect_uses_cropped_snapshot_origin(void) {
  BYTE pixels[16] = {0};
  RECTANGLE_16 dirty_rect = {1, 1, 2, 2};
  RDPGFX_SURFACE_COMMAND command = {0};
  ViewerFramebufferSnapshot snapshot =
      make_snapshot(pixels, 4, 4, 8, sizeof(pixels));
  int ok = 1;

  snapshot.pixel_origin_x = 1;
  snapshot.pixel_origin_y = 1;
  snapshot.pixel_width = 2;
  snapshot.pixel_height = 2;
  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(0xA0U + i);

  ok = ok && expect_true(viewer_gfx_uncompressed_build_surface_command_rect(
                             &snapshot, 12, &dirty_rect, &command),
                         "cropped dirty snapshot command builds");
  ok = ok && expect_uint32(command.left, 1, "cropped dirty left");
  ok = ok && expect_uint32(command.top, 1, "cropped dirty top");
  ok = ok && expect_uint32(command.right, 3, "cropped dirty exclusive right");
  ok = ok && expect_uint32(command.bottom, 3, "cropped dirty exclusive bottom");
  ok = ok && expect_uint32(command.width, 2, "cropped dirty width");
  ok = ok && expect_uint32(command.height, 2, "cropped dirty height");
  ok = ok && expect_uint32(command.length, sizeof(pixels),
                           "cropped dirty payload length");
  ok = ok && expect_bytes(command.data, pixels, sizeof(pixels),
                          "cropped dirty payload starts at origin");
  viewer_gfx_uncompressed_surface_command_reset(&command);

  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              &snapshot, 12, &command),
                          "full command rejects cropped snapshot");
  return ok;
}

static int test_region_rebases_destination_bounds(void) {
  BYTE pixels[128] = {0};
  BYTE expected[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 8, 4, 32, 128);
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(i + 1U);
  memmove(expected, pixels + 16, 16);
  memmove(expected + 16, pixels + 48, 16);
  memmove(expected + 32, pixels + 80, 16);
  memmove(expected + 48, pixels + 112, 16);

  ok = ok && expect_true(viewer_gfx_uncompressed_build_surface_command_region(
                             &snapshot, 2, 4, 0, 8, 4, 0, 0, &command),
                         "rebased region command builds");
  ok = ok && expect_uint32(command.surfaceId, 2, "rebased surface id");
  ok = ok && expect_uint32(command.left, 0, "rebased local left");
  ok = ok && expect_uint32(command.top, 0, "rebased local top");
  ok = ok && expect_uint32(command.right, 4, "rebased local right");
  ok = ok && expect_uint32(command.bottom, 4, "rebased local bottom");
  ok = ok && expect_uint32(command.width, 4, "rebased width");
  ok = ok && expect_uint32(command.height, 4, "rebased height");
  ok = ok &&
       expect_uint32(command.length, sizeof(expected), "rebased payload size");
  ok = ok && expect_bytes(command.data, expected, sizeof(expected),
                          "rebased payload copies source crop");

  viewer_gfx_uncompressed_surface_command_reset(&command);
  return ok;
}

static int test_dirty_rect_edge_bounds(void) {
  BYTE pixels[64] = {0};
  BYTE expected_pixel[4] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 4, 4, 16, 64);
  RECTANGLE_16 one_pixel_edge = {3, 3, 3, 3};
  RECTANGLE_16 full_frame = {0, 0, 3, 3};
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(i + 1U);
  memmove(expected_pixel, pixels + 60, sizeof(expected_pixel));

  ok = ok && expect_true(viewer_gfx_uncompressed_build_surface_command_rect(
                             &snapshot, 5, &one_pixel_edge, &command),
                         "one-pixel edge dirty rect builds");
  ok = ok && expect_uint32(command.left, 3, "one-pixel left");
  ok = ok && expect_uint32(command.top, 3, "one-pixel top");
  ok = ok && expect_uint32(command.right, 4, "one-pixel exclusive right");
  ok = ok && expect_uint32(command.bottom, 4, "one-pixel exclusive bottom");
  ok = ok && expect_uint32(command.width, 1, "one-pixel width");
  ok = ok && expect_uint32(command.height, 1, "one-pixel height");
  ok = ok && expect_uint32(command.length, sizeof(expected_pixel),
                           "one-pixel payload length");
  ok = ok && expect_bytes(command.data, expected_pixel, sizeof(expected_pixel),
                          "one-pixel payload copied");
  viewer_gfx_uncompressed_surface_command_reset(&command);

  ok = ok && expect_true(viewer_gfx_uncompressed_build_surface_command_rect(
                             &snapshot, 6, &full_frame, &command),
                         "full-width full-height dirty rect builds");
  ok = ok && expect_uint32(command.left, 0, "full rect left");
  ok = ok && expect_uint32(command.top, 0, "full rect top");
  ok = ok && expect_uint32(command.right, 4, "full rect exclusive right");
  ok = ok && expect_uint32(command.bottom, 4, "full rect exclusive bottom");
  ok = ok && expect_uint32(command.width, 4, "full rect width");
  ok = ok && expect_uint32(command.height, 4, "full rect height");
  ok = ok && expect_uint32(command.length, sizeof(pixels),
                           "full rect payload length");

  viewer_gfx_uncompressed_surface_command_reset(&command);
  return ok;
}

static int test_invalid_inputs(void) {
  BYTE pixels[16] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 2, 2, 8, 16);
  ViewerFramebufferSnapshot invalid = snapshot;
  RECTANGLE_16 valid_rect = {0, 0, 0, 0};
  RECTANGLE_16 bad_rect_order = {1, 0, 0, 0};
  RECTANGLE_16 bad_rect_bounds = {0, 0, 2, 0};
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              NULL, 1, &command),
                          "null snapshot rejected");
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              &snapshot, 1, NULL),
                          "null command rejected");

  invalid = snapshot;
  invalid.pixels = NULL;
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              &invalid, 1, &command),
                          "null pixels rejected");

  invalid = snapshot;
  invalid.width = 0;
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              &invalid, 1, &command),
                          "zero width rejected");

  invalid = snapshot;
  invalid.pixel_format = 0;
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              &invalid, 1, &command),
                          "unsupported format rejected");

  invalid = snapshot;
  invalid.stride = 7;
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              &invalid, 1, &command),
                          "bad stride rejected");

  invalid = snapshot;
  invalid.pixel_bytes = 15;
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              &invalid, 1, &command),
                          "short pixel buffer rejected");

  invalid = snapshot;
  invalid.height = 0;
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command_rect(
                              &invalid, 1, &valid_rect, &command),
                          "dirty rect dimension mismatch rejected");

  invalid = snapshot;
  invalid.width = (UINT32)UINT16_MAX + 1U;
  invalid.height = 1;
  invalid.stride = 4;
  invalid.pixel_bytes = 4;
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              &invalid, 1, &command),
                          "oversized width rejected");

#if SIZE_MAX > UINT32_MAX
  invalid = snapshot;
  invalid.width = UINT16_MAX;
  invalid.height = UINT16_MAX;
  invalid.stride = (UINT32)UINT16_MAX * 4U;
  invalid.pixel_bytes = (size_t)invalid.height * (size_t)invalid.stride;
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              &invalid, 1, &command),
                          "oversized payload rejected");
#endif

  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command_rect(
                              &snapshot, 1, NULL, &command),
                          "null dirty rect rejected");
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command_rect(
                              &snapshot, 1, &valid_rect, NULL),
                          "null dirty command rejected");
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command_rect(
                              &snapshot, 1, &bad_rect_order, &command),
                          "invalid dirty rect order rejected");
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command_rect(
                              &snapshot, 1, &bad_rect_bounds, &command),
                          "out-of-bounds dirty rect rejected");

  return ok;
}

static int test_reset_idempotent(void) {
  BYTE pixels[16] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 2, 2, 8, 16);
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_gfx_uncompressed_build_surface_command(
                             &snapshot, 1, &command),
                         "command builds before reset");
  viewer_gfx_uncompressed_surface_command_reset(&command);
  ok = ok && expect_true(command.data == NULL, "reset clears data");
  ok = ok && expect_uint32(command.length, 0, "reset clears length");
  viewer_gfx_uncompressed_surface_command_reset(&command);
  viewer_gfx_uncompressed_surface_command_reset(NULL);
  return ok;
}

static int test_build_resets_existing_command(void) {
  BYTE pixels[16] = {0};
  ViewerFramebufferSnapshot snapshot = make_snapshot(pixels, 2, 2, 8, 16);
  ViewerFramebufferSnapshot invalid = snapshot;
  RDPGFX_SURFACE_COMMAND command = {0};
  int ok = 1;

  ok = ok && expect_true(viewer_gfx_uncompressed_build_surface_command(
                             &snapshot, 1, &command),
                         "initial command builds before replacement");
  ok = ok && expect_true(command.data != NULL, "initial command owns data");
  ok = ok && expect_true(viewer_gfx_uncompressed_build_surface_command(
                             &snapshot, 2, &command),
                         "second build replaces existing command");
  ok = ok && expect_uint32(command.surfaceId, 2, "replacement surface id");
  ok = ok && expect_true(command.data != NULL, "replacement command owns data");

  invalid.pixel_format = 0;
  ok = ok && expect_false(viewer_gfx_uncompressed_build_surface_command(
                              &invalid, 3, &command),
                          "invalid replacement rejected");
  ok = ok &&
       expect_true(command.data == NULL, "invalid replacement resets data");
  ok = ok &&
       expect_uint32(command.length, 0, "invalid replacement clears length");

  viewer_gfx_uncompressed_surface_command_reset(&command);
  return ok;
}

int main(void) {
  int ok = 1;

  ok = ok && test_full_frame_repacks_padded_stride();
  ok = ok && test_dirty_rect_builds_exclusive_bounds();
  ok = ok && test_dirty_rect_uses_cropped_snapshot_origin();
  ok = ok && test_region_rebases_destination_bounds();
  ok = ok && test_dirty_rect_edge_bounds();
  ok = ok && test_invalid_inputs();
  ok = ok && test_reset_idempotent();
  ok = ok && test_build_resets_existing_command();

  return ok ? 0 : 1;
}
