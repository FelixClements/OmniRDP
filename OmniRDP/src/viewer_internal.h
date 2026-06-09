#ifndef VIEWER_INTERNAL_H
#define VIEWER_INTERNAL_H

#include "viewer_server_internal.h"
#include <freerdp/settings_types.h>
#include <stddef.h>
#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  BOOL owner_active;
  UINT32 owner_viewer_id;
  UINT64 last_input_ts;
} ViewerInputOwnershipState;

#define VIEWER_SEVERE_LAG_INTERVALS 96U
#define VIEWER_THROTTLE_LAG_INTERVALS 16U

BOOL viewer_input_try_acquire(ViewerInputOwnershipState *state,
                              UINT32 viewer_id, BOOL viewer_connected,
                              BOOL viewer_activated, BOOL owner_alive,
                              UINT64 now, UINT64 timeout_ms);

BOOL viewer_gfx_select_compatible_caps(const RDPGFX_CAPSET *canonical_caps,
                                       BOOL canonical_caps_valid,
                                       const RDPGFX_CAPSET *advertised_caps,
                                       UINT16 advertised_caps_count,
                                       RDPGFX_CAPSET *selected_caps);

BOOL viewer_gfx_caps_is_whitelisted(const RDPGFX_CAPSET *caps);

BOOL viewer_gfx_capset_describe(const RDPGFX_CAPSET *caps, char *buffer,
                                size_t buffer_size);

BOOL viewer_gfx_activation_waits_for_rdpgfx_caps(
    const ViewerGraphicsContext *gfx);

BOOL viewer_gfx_pending_activation_begins_rdpgfx_join(
    const ViewerGraphicsContext *gfx);

BOOL viewer_gfx_vcm_progress_should_be_pumped(BOOL viewer_activated,
                                              const ViewerGraphicsContext *gfx);

BOOL viewer_gfx_drdynvc_initialization_should_run(
    const ViewerGraphicsContext *gfx);

BOOL viewer_gfx_rdpgfx_open_should_run(const ViewerGraphicsContext *gfx);

BOOL viewer_gfx_negotiation_is_pending(const ViewerGraphicsContext *gfx);

BOOL viewer_gfx_negotiation_is_rdpgfx_ready(const ViewerGraphicsContext *gfx);

BOOL viewer_gfx_negotiation_is_classic_fallback(
    const ViewerGraphicsContext *gfx);

BOOL viewer_gfx_pending_activation_timeout_due(const ViewerGraphicsContext *gfx,
                                               UINT64 now, UINT32 timeout_ms);

BOOL viewer_gfx_failure_requires_disconnect(const ViewerGraphicsContext *gfx,
                                            BOOL viewer_activated);

UINT32 viewer_slot_index_to_id(UINT32 slot_index);

BOOL viewer_is_slow(UINT32 consecutive_lag_intervals, BOOL write_blocked);

BOOL viewer_lag_signal_active(const Viewer *viewer, BOOL write_blocked);

BOOL viewer_disconnect_due(const Viewer *viewer, UINT32 disconnect_ms,
                           UINT64 now);

BOOL viewer_monitor_from_size(UINT32 width, UINT32 height,
                              MONITOR_DEF *monitor);

#ifdef __cplusplus
}
#endif

#endif
