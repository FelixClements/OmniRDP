#include "viewer_internal.h"

#include "safe_string.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static UINT32 viewer_gfx_caps_allowed_flags(void) {
  UINT32 flags = 0;

#ifdef RDPGFX_CAPS_FLAG_THINCLIENT
  flags |= RDPGFX_CAPS_FLAG_THINCLIENT;
#endif
#ifdef RDPGFX_CAPS_FLAG_SMALL_CACHE
  flags |= RDPGFX_CAPS_FLAG_SMALL_CACHE;
#endif
#ifdef RDPGFX_CAPS_FLAG_AVC_DISABLED
  flags |= RDPGFX_CAPS_FLAG_AVC_DISABLED;
#endif
#ifdef RDPGFX_CAPS_FLAG_SCALEDMAP_DISABLE
  flags |= RDPGFX_CAPS_FLAG_SCALEDMAP_DISABLE;
#endif

  return flags;
}

static BOOL viewer_gfx_caps_version_is_whitelisted(UINT32 version) {
  switch (version) {
#ifdef RDPGFX_CAPVERSION_8
  case RDPGFX_CAPVERSION_8:
    return TRUE;
#endif
#ifdef RDPGFX_CAPVERSION_81
  case RDPGFX_CAPVERSION_81:
    return TRUE;
#endif
#ifdef RDPGFX_CAPVERSION_10
  case RDPGFX_CAPVERSION_10:
    return TRUE;
#endif
#ifdef RDPGFX_CAPVERSION_101
  case RDPGFX_CAPVERSION_101:
    return TRUE;
#endif
#ifdef RDPGFX_CAPVERSION_102
  case RDPGFX_CAPVERSION_102:
    return TRUE;
#endif
#ifdef RDPGFX_CAPVERSION_103
  case RDPGFX_CAPVERSION_103:
    return TRUE;
#endif
#ifdef RDPGFX_CAPVERSION_104
  case RDPGFX_CAPVERSION_104:
    return TRUE;
#endif
#ifdef RDPGFX_CAPVERSION_105
  case RDPGFX_CAPVERSION_105:
    return TRUE;
#endif
#ifdef RDPGFX_CAPVERSION_106
  case RDPGFX_CAPVERSION_106:
    return TRUE;
#endif
#ifdef RDPGFX_CAPVERSION_106_ERR
  case RDPGFX_CAPVERSION_106_ERR:
    return TRUE;
#endif
#ifdef RDPGFX_CAPVERSION_107
  case RDPGFX_CAPVERSION_107:
    return TRUE;
#endif
  default:
    return FALSE;
  }
}

BOOL viewer_gfx_caps_is_whitelisted(const RDPGFX_CAPSET *caps) {
  if (!caps)
    return FALSE;

  if (!viewer_gfx_caps_version_is_whitelisted(caps->version))
    return FALSE;

  return (caps->flags & ~viewer_gfx_caps_allowed_flags()) == 0;
}

BOOL viewer_gfx_capset_describe(const RDPGFX_CAPSET *caps, char *buffer,
                                size_t buffer_size) {
  int written = 0;

  if (!caps || !buffer || (buffer_size == 0))
    return FALSE;

  written = omni_format(buffer, buffer_size,
                        "version=0x%08" PRIX32 " flags=0x%08" PRIX32,
                        caps->version, caps->flags);
  if ((written < 0) || ((size_t)written >= buffer_size)) {
    buffer[0] = '\0';
    return FALSE;
  }

  return TRUE;
}

static BOOL viewer_gfx_caps_is_preferred(const RDPGFX_CAPSET *candidate,
                                         const RDPGFX_CAPSET *current) {
  if (!current)
    return TRUE;

  if (candidate->version != current->version)
    return candidate->version > current->version;

#ifdef RDPGFX_CAPS_FLAG_AVC_DISABLED
  if ((candidate->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) !=
      (current->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED))
    return (candidate->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) != 0;
#endif

  return candidate->flags < current->flags;
}

BOOL viewer_gfx_select_compatible_caps(const RDPGFX_CAPSET *canonical_caps,
                                       BOOL canonical_caps_valid,
                                       const RDPGFX_CAPSET *advertised_caps,
                                       UINT16 advertised_caps_count,
                                       RDPGFX_CAPSET *selected_caps) {
  UINT16 i = 0;
  const RDPGFX_CAPSET *best = NULL;

  if (!advertised_caps || (advertised_caps_count == 0) || !selected_caps)
    return FALSE;

  if (canonical_caps_valid) {
    if (!viewer_gfx_caps_is_whitelisted(canonical_caps))
      return FALSE;

    for (i = 0; i < advertised_caps_count; i++) {
      const RDPGFX_CAPSET *caps = &advertised_caps[i];
      if (!viewer_gfx_caps_is_whitelisted(caps))
        continue;
      if (caps->version == canonical_caps->version) {
        *selected_caps = *caps;
        return TRUE;
      }
    }

    return FALSE;
  }

  for (i = 0; i < advertised_caps_count; i++) {
    const RDPGFX_CAPSET *caps = &advertised_caps[i];
    if (!viewer_gfx_caps_is_whitelisted(caps))
      continue;
    if (viewer_gfx_caps_is_preferred(caps, best))
      best = caps;
  }

  if (!best)
    return FALSE;

  *selected_caps = *best;
  return TRUE;
}

BOOL viewer_gfx_activation_waits_for_rdpgfx_caps(
    const ViewerGraphicsContext *gfx) {
  return gfx && (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_PENDING) &&
         !gfx->rdpgfx_temporarily_disabled;
}

BOOL viewer_gfx_pending_activation_begins_rdpgfx_join(
    const ViewerGraphicsContext *gfx) {
  return gfx && gfx->ready && !gfx->rdpgfx_temporarily_disabled &&
         (gfx->join_state == VIEWER_JOIN_STATE_PENDING) &&
         (gfx->join_strategy == VIEWER_JOIN_STRATEGY_NONE) &&
         ((gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_PENDING) ||
          (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_RDPEGFX_READY));
}

BOOL viewer_gfx_vcm_progress_should_be_pumped(
    BOOL viewer_activated, const ViewerGraphicsContext *gfx) {
  return viewer_activated && gfx && gfx->post_connect_complete && gfx->vcm &&
         gfx->drdynvc_joined;
}

BOOL viewer_gfx_drdynvc_initialization_should_run(
    const ViewerGraphicsContext *gfx) {
  return gfx && gfx->vcm && gfx->post_connect_complete && gfx->drdynvc_joined &&
         (gfx->drdynvc_state == DRDYNVC_STATE_NONE);
}

BOOL viewer_gfx_rdpgfx_open_should_run(const ViewerGraphicsContext *gfx) {
  return gfx && gfx->vcm && gfx->post_connect_complete && gfx->drdynvc_joined &&
         (gfx->drdynvc_state == DRDYNVC_STATE_READY) && gfx->rdpgfx &&
         !gfx->channel_opened && !gfx->rdpgfx_temporarily_disabled;
}

BOOL viewer_gfx_negotiation_is_pending(const ViewerGraphicsContext *gfx) {
  return gfx && (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_PENDING);
}

BOOL viewer_gfx_negotiation_is_rdpgfx_ready(const ViewerGraphicsContext *gfx) {
  return gfx &&
         (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_RDPEGFX_READY);
}

BOOL viewer_gfx_negotiation_is_classic_fallback(
    const ViewerGraphicsContext *gfx) {
  return gfx &&
         (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK);
}

BOOL viewer_gfx_pending_activation_timeout_due(const ViewerGraphicsContext *gfx,
                                               UINT64 now, UINT32 timeout_ms) {
  if (!viewer_gfx_negotiation_is_pending(gfx) || !gfx->ready ||
      (gfx->join_state != VIEWER_JOIN_STATE_PENDING) ||
      (gfx->join_strategy != VIEWER_JOIN_STRATEGY_NONE) ||
      (gfx->join_start_ts == 0) || (timeout_ms == 0) ||
      (now < gfx->join_start_ts))
    return FALSE;

  return (now - gfx->join_start_ts) >= timeout_ms;
}

BOOL viewer_gfx_failure_requires_disconnect(const ViewerGraphicsContext *gfx,
                                            BOOL viewer_activated) {
  if (!viewer_activated || !gfx)
    return FALSE;

  return (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_RDPEGFX_READY) &&
         (gfx->join_state == VIEWER_JOIN_STATE_LIVE) &&
         (gfx->join_strategy == VIEWER_JOIN_STRATEGY_NONE) && gfx->use_rdpgfx &&
         !gfx->rdpgfx_temporarily_disabled;
}

BOOL viewer_input_try_acquire(ViewerInputOwnershipState *state,
                              UINT32 viewer_id, BOOL viewer_connected,
                              BOOL viewer_activated, BOOL owner_alive,
                              UINT64 now, UINT64 timeout_ms) {
  if (!state || !viewer_connected || !viewer_activated)
    return FALSE;

  if (state->owner_active && (state->owner_viewer_id == viewer_id)) {
    state->last_input_ts = now;
    return TRUE;
  }

  if (state->owner_active && !owner_alive) {
    state->owner_active = FALSE;
    state->owner_viewer_id = 0;
    state->last_input_ts = 0;
  }

  if (!state->owner_active || (now < state->last_input_ts) ||
      ((now - state->last_input_ts) >= timeout_ms)) {
    state->owner_active = TRUE;
    state->owner_viewer_id = viewer_id;
    state->last_input_ts = now;
    return TRUE;
  }

  return FALSE;
}

UINT32
viewer_slot_index_to_id(UINT32 slot_index) { return slot_index + 1U; }

BOOL viewer_is_slow(UINT32 consecutive_lag_intervals, BOOL write_blocked) {
  return write_blocked ||
         (consecutive_lag_intervals >= VIEWER_SEVERE_LAG_INTERVALS);
}

BOOL viewer_lag_signal_active(const Viewer *viewer, BOOL write_blocked) {
  return viewer && (write_blocked || (viewer->consecutive_lag_intervals >=
                                      VIEWER_SEVERE_LAG_INTERVALS));
}

BOOL viewer_disconnect_due(const Viewer *viewer, UINT32 disconnect_ms,
                           UINT64 now) {
  if (!viewer || (viewer->sustained_lag_start_ts == 0) ||
      (disconnect_ms == 0) || (now < viewer->sustained_lag_start_ts))
    return FALSE;

  return (now - viewer->sustained_lag_start_ts) >= disconnect_ms;
}

BOOL viewer_monitor_from_size(UINT32 width, UINT32 height,
                              MONITOR_DEF *monitor) {
  if (!monitor || (width == 0) || (height == 0))
    return FALSE;

  monitor->left = 0;
  monitor->top = 0;
  monitor->right = (INT32)(width - 1U);
  monitor->bottom = (INT32)(height - 1U);
  monitor->flags = MONITOR_PRIMARY;
  return TRUE;
}

void monitor_layout_init(MonitorLayout *layout, UINT32 monitor_count) {
  UINT32 i = 0;

  if (!layout)
    return;

  memset(layout, 0, sizeof(*layout));

  if (monitor_count == 0)
    monitor_count = 1;
  if (monitor_count > OMNIRDP_MAX_MONITORS)
    monitor_count = OMNIRDP_MAX_MONITORS;

  layout->monitor_count = monitor_count;
  layout->total_width = monitor_count * 1920;
  layout->total_height = 1080;

  fprintf(stderr,
          "[viewer.internal] monitor_layout_init: monitor_count=%" PRIu32
          ", total_width=%" PRIu32 ", total_height=%" PRIu32 "\n",
          monitor_count, layout->total_width, layout->total_height);

  for (i = 0; i < monitor_count; i++) {
    layout->monitors[i].left = (INT32)(i * 1920);
    layout->monitors[i].top = 0;
    layout->monitors[i].right = (INT32)(((i + 1) * 1920) - 1U);
    layout->monitors[i].bottom = 1079;
    layout->monitors[i].flags = (i == 0) ? MONITOR_PRIMARY : 0;

    fprintf(stderr,
            "[viewer.internal] monitor_layout_init: monitor[%" PRIu32
            "]: left=%" PRId32 ", top=%" PRId32 ", right=%" PRId32
            ", bottom=%" PRId32 ", flags=0x%08" PRIx32 "%s\n",
            i, layout->monitors[i].left, layout->monitors[i].top,
            layout->monitors[i].right, layout->monitors[i].bottom,
            layout->monitors[i].flags,
            (layout->monitors[i].flags & MONITOR_PRIMARY) ? " PRIMARY" : "");
  }
}
