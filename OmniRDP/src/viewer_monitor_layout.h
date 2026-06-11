#ifndef VIEWER_MONITOR_LAYOUT_H
#define VIEWER_MONITOR_LAYOUT_H

#include "monitor_layout.h"

#include <freerdp/settings.h>
#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL viewer_monitor_layout_apply_to_settings(rdpSettings *settings,
                                             const MonitorLayout *layout);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_MONITOR_LAYOUT_H */
