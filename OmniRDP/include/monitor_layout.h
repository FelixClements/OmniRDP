#ifndef MONITOR_LAYOUT_H
#define MONITOR_LAYOUT_H

#include <freerdp/freerdp.h>
#include <freerdp/server/rdpgfx.h>
#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OMNIRDP_MAX_MONITORS 16

typedef struct {
  UINT32 monitor_count;
  UINT32 total_width;
  UINT32 total_height;
  MONITOR_DEF monitors[OMNIRDP_MAX_MONITORS];
} MonitorLayout;

void monitor_layout_init(MonitorLayout *layout, UINT32 monitor_count);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_LAYOUT_H */
