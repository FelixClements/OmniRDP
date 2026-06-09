#include "backend.h"
#include "test_utils.h"

#include <freerdp/settings.h>

#include <string.h>

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

static int expect_string(const char *actual, const char *expected) {
  if (!actual || !expected)
    return actual == expected;
  return strcmp(actual, expected) == 0;
}

static int test_applies_broker_options(void) {
  BackendClient *client = backend_init();
  if (!client)
    return 0;

  BackendSecurityConfig security = {TRUE, TRUE, TRUE, TRUE, FALSE};
  if (!backend_configure(client, "rds-broker.domain.com", 3389, "alice",
                         "secret", "CONTOSO", &security)) {
    backend_free(client);
    return 0;
  }

  BackendRdpFileOptions options = {
      "rds-broker.domain.com", TRUE,
      "tsv://MS Terminal Services Plugin.1.Marketing_Pool",
      "rds-broker-alt.domain.com"};
  if (!backend_apply_rdp_file_options(client, &options)) {
    backend_free(client);
    return 0;
  }

  rdpSettings *settings = client->context->settings;
  const char *server_hostname =
      freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
  const char *user_specified_server_name =
      freerdp_settings_get_string(settings, FreeRDP_UserSpecifiedServerName);
  const BYTE *loadbalanceinfo =
      freerdp_settings_get_pointer(settings, FreeRDP_LoadBalanceInfo);
  const UINT32 loadbalanceinfo_length =
      freerdp_settings_get_uint32(settings, FreeRDP_LoadBalanceInfoLength);
  const char *expected_loadbalanceinfo =
      "tsv://MS Terminal Services Plugin.1.Marketing_Pool";
  const size_t expected_loadbalanceinfo_length =
      strlen(expected_loadbalanceinfo);

  int ok =
      expect_string(server_hostname, "rds-broker-alt.domain.com") &&
      expect_string(client->hostname, "rds-broker-alt.domain.com") &&
      expect_string(user_specified_server_name, "rds-broker-alt.domain.com") &&
      loadbalanceinfo_length == expected_loadbalanceinfo_length &&
      loadbalanceinfo != NULL &&
      memcmp(loadbalanceinfo, expected_loadbalanceinfo,
             expected_loadbalanceinfo_length) == 0;

  backend_free(client);
  return ok;
}

static int test_empty_options_preserve_defaults(void) {
  BackendClient *client = backend_init();
  if (!client)
    return 0;

  BackendSecurityConfig security = {TRUE, TRUE, TRUE, TRUE, FALSE};
  if (!backend_configure(client, "rds-broker.domain.com", 3389, "alice",
                         "secret", "CONTOSO", &security)) {
    backend_free(client);
    return 0;
  }

  BackendRdpFileOptions options = {NULL, FALSE, NULL, NULL};
  if (!backend_apply_rdp_file_options(client, &options)) {
    backend_free(client);
    return 0;
  }

  rdpSettings *settings = client->context->settings;
  const char *server_hostname =
      freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
  const char *user_specified_server_name =
      freerdp_settings_get_string(settings, FreeRDP_UserSpecifiedServerName);
  const UINT32 loadbalanceinfo_length =
      freerdp_settings_get_uint32(settings, FreeRDP_LoadBalanceInfoLength);

  int ok =
      expect_string(server_hostname, "rds-broker.domain.com") &&
      expect_string(client->hostname, "rds-broker.domain.com") &&
      (!user_specified_server_name || user_specified_server_name[0] == '\0') &&
      loadbalanceinfo_length == 0;

  backend_free(client);
  return ok;
}

static int test_server_name_toggle_without_alternate(void) {
  BackendClient *client = backend_init();
  if (!client)
    return 0;

  BackendSecurityConfig security = {TRUE, TRUE, TRUE, TRUE, FALSE};
  if (!backend_configure(client, "rds-broker.domain.com", 3389, "alice",
                         "secret", "CONTOSO", &security)) {
    backend_free(client);
    return 0;
  }

  BackendRdpFileOptions options = {"rds-broker.domain.com", TRUE, "", ""};
  if (!backend_apply_rdp_file_options(client, &options)) {
    backend_free(client);
    return 0;
  }

  rdpSettings *settings = client->context->settings;
  const char *user_specified_server_name =
      freerdp_settings_get_string(settings, FreeRDP_UserSpecifiedServerName);
  int ok = expect_string(user_specified_server_name, "rds-broker.domain.com");

  backend_free(client);
  return ok;
}

int main(void) {
  test_suppress_crt_dialogs();

  if (!test_applies_broker_options())
    return 1;
  if (!test_empty_options_preserve_defaults())
    return 1;
  if (!test_server_name_toggle_without_alternate())
    return 1;

  return 0;
}
