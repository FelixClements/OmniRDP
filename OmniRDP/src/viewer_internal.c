#include "viewer_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

BOOL viewer_gfx_select_compatible_caps(const RDPGFX_CAPSET* canonical_caps,
                                       BOOL canonical_caps_valid,
                                       const RDPGFX_CAPSET* advertised_caps,
                                       UINT16 advertised_caps_count,
                                       RDPGFX_CAPSET* selected_caps)
{
    UINT16 i = 0;
    const RDPGFX_CAPSET* best = NULL;

    if (!advertised_caps || (advertised_caps_count == 0) || !selected_caps)
        return FALSE;

    if (canonical_caps_valid)
    {
        if (!canonical_caps)
            return FALSE;

        for (i = 0; i < advertised_caps_count; i++)
        {
            const RDPGFX_CAPSET* caps = &advertised_caps[i];
            if ((caps->version == canonical_caps->version) &&
                (caps->flags == canonical_caps->flags))
            {
                *selected_caps = *caps;
                return TRUE;
            }
        }

        return FALSE;
    }

    for (i = 0; i < advertised_caps_count; i++)
    {
        const RDPGFX_CAPSET* caps = &advertised_caps[i];
        if (!best || (caps->version > best->version))
            best = caps;
    }

    if (!best)
        return FALSE;

    *selected_caps = *best;
    return TRUE;
}

ViewerGfxCodecReplayPolicy viewer_gfx_codec_replay_policy(UINT16 codec_id,
                                                       const RDPGFX_CAPSET* confirmed_caps)
{
    switch (codec_id)
    {
        case RDPGFX_CODECID_UNCOMPRESSED:
        case RDPGFX_CODECID_CLEARCODEC:
        case RDPGFX_CODECID_PLANAR:
        case RDPGFX_CODECID_ALPHA:
        /* CAPROGRESSIVE is a stateless codec: each progressive frame is
         * self-contained (the base layer always produces a valid picture
         * regardless of decoder history). Unlike AVC which needs IDR
         * frames for decoder state bootstrap, CAPROGRESSIVE frames can
         * be replayed to late joiners as a usable visual baseline. */
        case RDPGFX_CODECID_CAPROGRESSIVE:
        case RDPGFX_CODECID_CAPROGRESSIVE_V2:
            return VIEWER_GFX_CODEC_REPLAY_SAFE;

        case RDPGFX_CODECID_AVC420:
        case RDPGFX_CODECID_AVC444:
        case RDPGFX_CODECID_AVC444v2:
            if (confirmed_caps && (confirmed_caps->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED))
                return VIEWER_GFX_CODEC_REPLAY_REJECTED_BY_CAPS;
            return VIEWER_GFX_CODEC_REPLAY_UNSAFE;

        case RDPGFX_CODECID_CAVIDEO:
        default:
            return VIEWER_GFX_CODEC_REPLAY_UNSAFE;
    }
}

ViewerJoinStrategy viewer_late_join_select_strategy(const ViewerLateJoinPolicyInputs* inputs)
{
    if (!inputs)
        return VIEWER_JOIN_STRATEGY_NONE;

    if (!inputs->rdpgfx_enabled || !inputs->channel_opened || !inputs->caps_compatible)
        return VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK;

    if (inputs->backend_frame_in_progress)
        return VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME;

    if (inputs->complete_frame_available && inputs->replay_safe_codecs_only)
        return VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME;

    return VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME;
}

BOOL viewer_late_join_ack_releases_live(UINT32 required_ack_frame_id,
                                        UINT32 last_ack_frame_id)
{
    if (required_ack_frame_id == 0)
        return FALSE;

    return last_ack_frame_id >= required_ack_frame_id;
}

BOOL viewer_late_join_timeout_fallback_due(ViewerJoinStrategy strategy,
                                           UINT64 late_join_start_ts, UINT64 now,
                                           UINT32 timeout_ms, BOOL waiting_for_ack)
{
    if ((strategy != VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME) &&
        ((strategy != VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME) || !waiting_for_ack))
        return FALSE;

    if ((late_join_start_ts == 0) || (timeout_ms == 0) || (now < late_join_start_ts))
        return FALSE;

    return (now - late_join_start_ts) >= timeout_ms;
}

BOOL viewer_gfx_activation_waits_for_rdpgfx_caps(const ViewerGraphicsContext* gfx)
{
    return gfx && (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_PENDING) &&
           !gfx->rdpgfx_temporarily_disabled;
}

BOOL viewer_gfx_pending_activation_begins_rdpgfx_join(const ViewerGraphicsContext* gfx)
{
    return gfx && gfx->ready && (gfx->join_state == VIEWER_JOIN_STATE_PENDING) &&
           (gfx->join_strategy == VIEWER_JOIN_STRATEGY_NONE);
}

BOOL viewer_gfx_vcm_progress_should_be_pumped(BOOL viewer_activated,
                                              const ViewerGraphicsContext* gfx)
{
    return viewer_activated && gfx && gfx->post_connect_complete && gfx->vcm &&
           gfx->drdynvc_joined;
}

BOOL viewer_gfx_drdynvc_initialization_should_run(const ViewerGraphicsContext* gfx)
{
    return gfx && gfx->vcm && gfx->post_connect_complete &&
           (gfx->drdynvc_state == DRDYNVC_STATE_NONE);
}

BOOL viewer_gfx_rdpgfx_open_should_run(const ViewerGraphicsContext* gfx)
{
    return gfx && gfx->vcm && gfx->post_connect_complete &&
           (gfx->drdynvc_state == DRDYNVC_STATE_READY) && gfx->rdpgfx && !gfx->channel_opened &&
           !gfx->rdpgfx_temporarily_disabled;
}

BOOL viewer_gfx_negotiation_is_pending(const ViewerGraphicsContext* gfx)
{
    return gfx && (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_PENDING);
}

BOOL viewer_gfx_negotiation_is_rdpgfx_ready(const ViewerGraphicsContext* gfx)
{
    return gfx && (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_RDPEGFX_READY);
}

BOOL viewer_gfx_negotiation_is_classic_fallback(const ViewerGraphicsContext* gfx)
{
    return gfx && (gfx->negotiation_outcome == VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK);
}

BOOL viewer_gfx_pending_activation_timeout_due(const ViewerGraphicsContext* gfx, UINT64 now,
                                               UINT32 timeout_ms)
{
    if (!viewer_gfx_negotiation_is_pending(gfx) || !gfx->ready ||
        (gfx->join_state != VIEWER_JOIN_STATE_PENDING) ||
        (gfx->join_strategy != VIEWER_JOIN_STRATEGY_NONE) || (gfx->join_start_ts == 0) ||
        (timeout_ms == 0) || (now < gfx->join_start_ts))
        return FALSE;

    return (now - gfx->join_start_ts) >= timeout_ms;
}

BOOL viewer_input_try_acquire(ViewerInputOwnershipState* state, UINT32 viewer_id,
                              BOOL viewer_connected, BOOL viewer_activated,
                              BOOL owner_alive, UINT64 now, UINT64 timeout_ms)
{
    if (!state || !viewer_connected || !viewer_activated)
        return FALSE;

    if (state->owner_active && (state->owner_viewer_id == viewer_id))
    {
        state->last_input_ts = now;
        return TRUE;
    }

    if (state->owner_active && !owner_alive)
    {
        state->owner_active = FALSE;
        state->owner_viewer_id = 0;
        state->last_input_ts = 0;
    }

    if (!state->owner_active || (now < state->last_input_ts) ||
        ((now - state->last_input_ts) >= timeout_ms))
    {
        state->owner_active = TRUE;
        state->owner_viewer_id = viewer_id;
        state->last_input_ts = now;
        return TRUE;
    }

    return FALSE;
}

UINT32 viewer_slot_index_to_id(UINT32 slot_index)
{
    return slot_index + 1U;
}

BOOL viewer_is_slow(UINT32 consecutive_lag_intervals, BOOL write_blocked)
{
    return write_blocked || (consecutive_lag_intervals >= VIEWER_SEVERE_LAG_INTERVALS);
}

BOOL viewer_lag_signal_active(const Viewer* viewer, BOOL write_blocked)
{
    return viewer && (write_blocked ||
                      (viewer->consecutive_lag_intervals >= VIEWER_SEVERE_LAG_INTERVALS));
}

BOOL viewer_disconnect_due(const Viewer* viewer, UINT32 disconnect_ms, UINT64 now)
{
    if (!viewer || (viewer->sustained_lag_start_ts == 0) || (disconnect_ms == 0) ||
        (now < viewer->sustained_lag_start_ts))
        return FALSE;

    return (now - viewer->sustained_lag_start_ts) >= disconnect_ms;
}

BOOL viewer_monitor_from_size(UINT32 width, UINT32 height, MONITOR_DEF* monitor)
{
    if (!monitor || (width == 0) || (height == 0))
        return FALSE;

    monitor->left = 0;
    monitor->top = 0;
    monitor->right = (INT32)width;
    monitor->bottom = (INT32)height;
    monitor->flags = MONITOR_PRIMARY;
    return TRUE;
}

void monitor_layout_init(MonitorLayout* layout, UINT32 monitor_count)
{
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

    fprintf(stderr, "[viewer.internal] monitor_layout_init: monitor_count=%"PRIu32", total_width=%"PRIu32", total_height=%"PRIu32"\n",
              monitor_count, layout->total_width, layout->total_height);

    for (i = 0; i < monitor_count; i++)
    {
        layout->monitors[i].left = (INT32)(i * 1920);
        layout->monitors[i].top = 0;
        layout->monitors[i].right = (INT32)((i + 1) * 1920);
        layout->monitors[i].bottom = 1080;
        layout->monitors[i].flags = (i == 0) ? MONITOR_PRIMARY : 0;

        fprintf(stderr, "[viewer.internal] monitor_layout_init: monitor[%"PRIu32"]: left=%"PRId32", top=%"PRId32", right=%"PRId32", bottom=%"PRId32", flags=0x%08"PRIx32"%s\n",
                  i, layout->monitors[i].left, layout->monitors[i].top,
                  layout->monitors[i].right, layout->monitors[i].bottom,
                  layout->monitors[i].flags,
                  (layout->monitors[i].flags & MONITOR_PRIMARY) ? " PRIMARY" : "");
    }
}
