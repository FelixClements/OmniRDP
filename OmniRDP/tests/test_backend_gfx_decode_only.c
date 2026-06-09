#include "backend.h"
#include "test_utils.h"

#include <freerdp/settings.h>

#include <stdio.h>

void monitor_layout_init(MonitorLayout *layout, UINT32 monitor_count) {
  if (!layout)
    return;
  layout->monitor_count = monitor_count;
  layout->total_width = 0;
  layout->total_height = 0;
}

void viewer_server_notify_backend_layout_change(BackendClient *backend,
                                                UINT32 width, UINT32 height,
                                                UINT32 generation) {
  (void)backend;
  (void)width;
  (void)height;
  (void)generation;
}

BOOL viewer_server_publish_surface_bits(BackendClient *backend,
                                        const SURFACE_BITS_COMMAND *cmd) {
  (void)backend;
  (void)cmd;
  return TRUE;
}

BOOL viewer_server_publish_bitmap_update(BackendClient *backend,
                                         const BITMAP_UPDATE *bitmap) {
  (void)backend;
  (void)bitmap;
  return TRUE;
}

BOOL viewer_server_update_framebuffer_from_gdi(BackendClient *backend,
                                               const BYTE *pixels, UINT32 width,
                                               UINT32 height, UINT32 stride,
                                               UINT32 pixel_format,
                                               const RECTANGLE_16 *dirty_rects,
                                               UINT32 dirty_rect_count) {
  (void)backend;
  (void)pixels;
  (void)width;
  (void)height;
  (void)stride;
  (void)pixel_format;
  (void)dirty_rects;
  (void)dirty_rect_count;
  return TRUE;
}

BOOL viewer_server_publish_frame_marker(BackendClient *backend,
                                        const SURFACE_FRAME_MARKER *marker) {
  (void)backend;
  (void)marker;
  return TRUE;
}

static int expect_true(BOOL value, const char *message) {
  if (!value) {
    (void)fprintf(stderr, "FAIL: %s\n", message);
    return 0;
  }
  return 1;
}

static int expect_false(BOOL value, const char *message) {
  return expect_true(!value, message);
}

static int test_struct_defaults_and_metrics_zero(void) {
  BackendClient client = {0};

  if (client.backend_gfx_decode_only_enabled)
    return 0;

  client.backend_gfx_decode_only_enabled = TRUE;
  if (!client.backend_gfx_decode_only_enabled)
    return 0;

  client.backend_gfx_decode_only_enabled = FALSE;
  if (client.backend_gfx_decode_only_enabled)
    return 0;

  if (client.surface_bits_decode_count != 0)
    return 0;
  if (client.surface_bits_decode_failure_count != 0)
    return 0;
  if (client.surface_bits_decode_time_total_us != 0)
    return 0;
  if (client.surface_bits_decode_time_max_us != 0)
    return 0;
  if (client.surface_bits_payload_bytes_total != 0)
    return 0;

  return 1;
}

static int test_baseline_settings_disable_backend_codecs(void) {
  BackendClient *client = backend_init();
  int ok = 1;
  rdpSettings *settings = NULL;

  if (!client || !client->context || !client->context->settings)
    return 0;

  settings = client->context->settings;
  ok = ok &&
       expect_false(freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec),
                    "baseline disables backend RemoteFX");
  ok = ok && expect_false(freerdp_settings_get_bool(settings, FreeRDP_NSCodec),
                          "baseline disables backend NSCodec");
  ok = ok && expect_false(freerdp_settings_get_bool(
                              settings, FreeRDP_SupportGraphicsPipeline),
                          "baseline disables backend RDPEGFX");
  ok = ok && expect_false(freerdp_settings_get_bool(settings, FreeRDP_GfxH264),
                          "baseline disables backend H264");
  ok =
      ok && expect_false(freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444),
                         "baseline disables backend AVC444");
  ok = ok &&
       expect_false(freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444v2),
                    "baseline disables backend AVC444v2");

  backend_free(client);
  return ok;
}

static int test_decode_only_settings_do_not_enable_backend_rfx(void) {
  BackendClient *client = backend_init();
  int ok = 1;
  rdpSettings *settings = NULL;

  if (!client || !client->context || !client->context->settings)
    return 0;

  ok = ok && expect_true(backend_set_gfx_decode_only(client, TRUE),
                         "decode-only setter succeeds");
  settings = client->context->settings;
  ok = ok && expect_true(freerdp_settings_get_bool(
                             settings, FreeRDP_SupportGraphicsPipeline),
                         "decode-only enables RDPEGFX support");
  ok = ok &&
       expect_false(freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec),
                    "decode-only keeps backend RemoteFX disabled");
  ok = ok && expect_false(freerdp_settings_get_bool(settings, FreeRDP_NSCodec),
                          "decode-only keeps backend NSCodec disabled");
  ok = ok && expect_false(freerdp_settings_get_bool(settings, FreeRDP_GfxH264),
                          "decode-only keeps backend H264 disabled");
  ok =
      ok && expect_false(freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444),
                         "decode-only keeps backend AVC444 disabled");
  ok = ok &&
       expect_false(freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444v2),
                    "decode-only keeps backend AVC444v2 disabled");

  backend_free(client);
  return ok;
}

static int test_rfx_candidate_settings_enable_only_backend_rfx(void) {
  BackendClient *client = backend_init();
  int ok = 1;
  rdpSettings *settings = NULL;

  if (!client || !client->context || !client->context->settings)
    return 0;

  ok = ok && expect_true(backend_set_rfx_enabled(client, TRUE),
                         "backend RFX candidate setter succeeds");
  settings = client->context->settings;
  ok = ok && expect_true(client->backend_rfx_enabled,
                         "backend RFX candidate state recorded");
  ok = ok &&
       expect_true(freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec),
                   "backend RFX candidate enables RemoteFX");
  ok = ok && expect_false(freerdp_settings_get_bool(settings, FreeRDP_NSCodec),
                          "backend RFX candidate keeps NSCodec disabled");
  ok = ok && expect_false(freerdp_settings_get_bool(settings, FreeRDP_GfxH264),
                          "backend RFX candidate keeps H264 disabled");
  ok =
      ok && expect_false(freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444),
                         "backend RFX candidate keeps AVC444 disabled");
  ok = ok &&
       expect_false(freerdp_settings_get_bool(settings, FreeRDP_GfxAVC444v2),
                    "backend RFX candidate keeps AVC444v2 disabled");

  backend_free(client);
  return ok;
}

static int test_decode_only_disable_does_not_clear_backend_rfx(void) {
  BackendClient *client = backend_init();
  int ok = 1;
  rdpSettings *settings = NULL;

  if (!client || !client->context || !client->context->settings)
    return 0;

  ok = ok && expect_true(backend_set_gfx_decode_only(client, TRUE),
                         "decode-only setter enables candidate decode gate");
  ok = ok && expect_true(backend_set_rfx_enabled(client, TRUE),
                         "backend RFX candidate setter succeeds after decode");
  ok = ok && expect_true(backend_set_gfx_decode_only(client, FALSE),
                         "decode-only setter can be disabled independently");
  settings = client->context->settings;
  ok = ok && expect_false(freerdp_settings_get_bool(
                              settings, FreeRDP_SupportGraphicsPipeline),
                          "decode-only disable clears only RDPEGFX support");
  ok = ok &&
       expect_true(freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec),
                   "decode-only disable keeps explicit RemoteFX enabled");

  backend_free(client);
  return ok;
}

int main(void) {
  test_suppress_crt_dialogs();

  if (!test_struct_defaults_and_metrics_zero())
    return 1;
  if (!test_baseline_settings_disable_backend_codecs())
    return 1;
  if (!test_decode_only_settings_do_not_enable_backend_rfx())
    return 1;
  if (!test_rfx_candidate_settings_enable_only_backend_rfx())
    return 1;
  if (!test_decode_only_disable_does_not_clear_backend_rfx())
    return 1;

  return 0;
}
