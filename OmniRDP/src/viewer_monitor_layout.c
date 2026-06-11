#include "viewer_monitor_layout.h"

#include <limits.h>

BOOL viewer_monitor_layout_apply_to_settings(rdpSettings *settings,
                                             const MonitorLayout *layout) {
  rdpMonitor monitors[OMNIRDP_MAX_MONITORS] = {0};
  UINT32 i = 0;

  if (!settings || !layout || (layout->monitor_count == 0) ||
      (layout->monitor_count > OMNIRDP_MAX_MONITORS) ||
      (layout->total_width == 0) || (layout->total_height == 0) ||
      (layout->total_width > UINT16_MAX) || (layout->total_height > UINT16_MAX))
    return FALSE;

  for (i = 0; i < layout->monitor_count; i++) {
    const MONITOR_DEF *source = &layout->monitors[i];
    const INT64 monitor_width = (INT64)source->right - (INT64)source->left + 1;
    const INT64 monitor_height = (INT64)source->bottom - (INT64)source->top + 1;

    if ((monitor_width <= 0) || (monitor_height <= 0) ||
        (monitor_width > INT32_MAX) || (monitor_height > INT32_MAX))
      return FALSE;

    monitors[i].x = source->left;
    monitors[i].y = source->top;
    monitors[i].width = (INT32)monitor_width;
    monitors[i].height = (INT32)monitor_height;
    monitors[i].is_primary = (source->flags & MONITOR_PRIMARY) ? TRUE : FALSE;
    monitors[i].orig_screen = i;
    monitors[i].attributes.physicalWidth = monitors[i].width;
    monitors[i].attributes.physicalHeight = monitors[i].height;
    monitors[i].attributes.orientation = ORIENTATION_LANDSCAPE;
    monitors[i].attributes.desktopScaleFactor = 100;
    monitors[i].attributes.deviceScaleFactor = 100;
  }

  if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
                                   layout->total_width))
    return FALSE;
  if (!freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
                                   layout->total_height))
    return FALSE;
  if (!freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu,
                                 TRUE))
    return FALSE;

  return freerdp_settings_set_monitor_def_array_sorted(settings, monitors,
                                                       layout->monitor_count);
}
