#include "monitor_layout.h"
#include "test_utils.h"
#include "viewer_monitor_layout.h"

#include <freerdp/settings.h>
#include <freerdp/settings_types.h>
#include <winpr/wtypes.h>

static int expect_monitor(const rdpSettings *settings, UINT32 index, INT32 x,
                          INT32 y, INT32 width, INT32 height, BOOL is_primary) {
  const rdpMonitor *monitor =
      (const rdpMonitor *)freerdp_settings_get_pointer_array(
          settings, FreeRDP_MonitorDefArray, index);

  if (!monitor)
    return 1;
  if (monitor->x != x)
    return 1;
  if (monitor->y != y)
    return 1;
  if (monitor->width != width)
    return 1;
  if (monitor->height != height)
    return 1;
  if (monitor->is_primary != is_primary)
    return 1;
  return 0;
}

static int test_apply_two_monitor_layout_allocates_monitor_array(void) {
  MonitorLayout layout = {0};
  rdpSettings *settings = freerdp_settings_new(0);
  int failed = 0;

  if (!settings)
    return 1;
  monitor_layout_init(&layout, 2);

  failed = !viewer_monitor_layout_apply_to_settings(settings, &layout);
  failed =
      failed ||
      (freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) != 3840);
  failed =
      failed ||
      (freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) != 1080);
  failed = failed ||
           (freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount) != 2);
  failed =
      failed ||
      (freerdp_settings_get_uint32(settings, FreeRDP_MonitorDefArraySize) < 2);
  failed = failed || (expect_monitor(settings, 0, 0, 0, 1920, 1080, TRUE) != 0);
  failed =
      failed || (expect_monitor(settings, 1, 1920, 0, 1920, 1080, FALSE) != 0);

  freerdp_settings_free(settings);
  return failed ? 1 : 0;
}

static int
test_apply_single_monitor_layout_replaces_stale_two_monitor_layout(void) {
  MonitorLayout layout = {0};
  rdpSettings *settings = freerdp_settings_new(0);
  int failed = 0;

  if (!settings)
    return 1;
  monitor_layout_init(&layout, 2);
  if (!viewer_monitor_layout_apply_to_settings(settings, &layout)) {
    freerdp_settings_free(settings);
    return 1;
  }

  monitor_layout_init(&layout, 1);
  failed = !viewer_monitor_layout_apply_to_settings(settings, &layout);
  failed =
      failed ||
      (freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) != 1920);
  failed =
      failed ||
      (freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) != 1080);
  failed = failed ||
           (freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount) != 1);
  failed = failed || (expect_monitor(settings, 0, 0, 0, 1920, 1080, TRUE) != 0);

  freerdp_settings_free(settings);
  return failed ? 1 : 0;
}

static int test_apply_invalid_layout_fails_without_partial_settings(void) {
  MonitorLayout layout = {0};
  rdpSettings *settings = freerdp_settings_new(0);
  int failed = 0;

  if (!settings)
    return 1;
  layout.monitor_count = 2;
  layout.total_width = 0;
  layout.total_height = 1080;

  failed = viewer_monitor_layout_apply_to_settings(settings, &layout);
  failed = failed ||
           (freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) == 0);
  failed =
      failed ||
      (freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) == 1080);

  freerdp_settings_free(settings);
  return failed ? 1 : 0;
}

int main(void) {
  test_suppress_crt_dialogs();
  if (test_apply_two_monitor_layout_allocates_monitor_array() != 0)
    return 1;
  if (test_apply_single_monitor_layout_replaces_stale_two_monitor_layout() != 0)
    return 1;
  if (test_apply_invalid_layout_fails_without_partial_settings() != 0)
    return 1;
  return 0;
}
