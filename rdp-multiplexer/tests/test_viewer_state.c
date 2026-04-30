#include <assert.h>
#include "viewer_internal.h"

static void test_ownership_timeout_transitions(void)
{
    ViewerInputOwnershipState state = { 0 };

    assert(viewer_input_try_acquire(&state, 1, TRUE, TRUE, FALSE, 100, 50));
    assert(state.owner_viewer_id == 1);
    assert(state.last_input_ts == 100);

    assert(viewer_input_try_acquire(&state, 1, TRUE, TRUE, TRUE, 120, 50));
    assert(state.owner_viewer_id == 1);
    assert(state.last_input_ts == 120);

    assert(!viewer_input_try_acquire(&state, 2, TRUE, TRUE, TRUE, 160, 50));
    assert(state.owner_viewer_id == 1);
    assert(state.last_input_ts == 120);

    assert(viewer_input_try_acquire(&state, 2, TRUE, TRUE, TRUE, 170, 50));
    assert(state.owner_viewer_id == 2);
    assert(state.last_input_ts == 170);
}

static void test_dead_owner_clearing(void)
{
    ViewerInputOwnershipState state = { .owner_viewer_id = 1, .last_input_ts = 200 };

    assert(viewer_input_try_acquire(&state, 2, TRUE, TRUE, FALSE, 210, 50));
    assert(state.owner_viewer_id == 2);
    assert(state.last_input_ts == 210);
}

static void test_slow_viewer_detection_thresholds(void)
{
    assert(!viewer_is_slow(0, FALSE));
    assert(viewer_is_slow(VIEWER_SEVERE_LAG_INTERVALS, FALSE));
    assert(viewer_is_slow(0, TRUE));
}

static void test_sustained_lag_threshold_not_yet_reached(void)
{
    const UINT32 disconnect_threshold_ms = VIEWER_SEVERE_LAG_INTERVALS * 16U;
    Viewer viewer = { 0 };

    viewer.sustained_lag_start_ts = 1000;

    assert(viewer_lag_signal_active(&viewer, TRUE));
    assert(!viewer_disconnect_due(&viewer, disconnect_threshold_ms,
                                  1000 + disconnect_threshold_ms - 1U));
}

static void test_sustained_lag_threshold_crosses_exactly(void)
{
    const UINT32 disconnect_threshold_ms = VIEWER_SEVERE_LAG_INTERVALS * 16U;
    Viewer viewer = { 0 };

    viewer.sustained_lag_start_ts = 1000;
    viewer.write_block_events = 1;
    viewer.consecutive_lag_intervals = VIEWER_SEVERE_LAG_INTERVALS;

    assert(viewer_lag_signal_active(&viewer, FALSE));
    assert(viewer_disconnect_due(&viewer, disconnect_threshold_ms,
                                 1000 + disconnect_threshold_ms));
}

static void test_sustained_lag_timer_resets_after_clear(void)
{
    const UINT32 disconnect_threshold_ms = VIEWER_SEVERE_LAG_INTERVALS * 16U;
    Viewer viewer = { 0 };

    viewer.sustained_lag_start_ts = 1000;
    assert(viewer_lag_signal_active(&viewer, TRUE));
    assert(!viewer_disconnect_due(&viewer, disconnect_threshold_ms, 2000));

    assert(!viewer_lag_signal_active(&viewer, FALSE));
    viewer.sustained_lag_start_ts = 0;

    viewer.write_block_events = 1;
    assert(viewer_lag_signal_active(&viewer, TRUE));
    viewer.sustained_lag_start_ts = 5000;
    assert(!viewer_disconnect_due(&viewer, disconnect_threshold_ms,
                                  5000 + disconnect_threshold_ms - 1U));
}

static void test_repeated_lag_requires_fresh_full_window(void)
{
    const UINT32 disconnect_threshold_ms = VIEWER_SEVERE_LAG_INTERVALS * 16U;
    Viewer viewer = { 0 };

    viewer.sustained_lag_start_ts = 1000;
    assert(!viewer_disconnect_due(&viewer, disconnect_threshold_ms,
                                  1000 + disconnect_threshold_ms - 1U));

    assert(!viewer_lag_signal_active(&viewer, FALSE));
    viewer.sustained_lag_start_ts = 0;

    viewer.write_block_events = 1;
    viewer.consecutive_lag_intervals = VIEWER_SEVERE_LAG_INTERVALS;
    assert(viewer_lag_signal_active(&viewer, FALSE));
    viewer.sustained_lag_start_ts = 22000;
    assert(!viewer_disconnect_due(&viewer, disconnect_threshold_ms,
                                  22000 + disconnect_threshold_ms - 1U));
    assert(viewer_disconnect_due(&viewer, disconnect_threshold_ms,
                                 22000 + disconnect_threshold_ms));
}

static void test_viewer_ids_start_at_one(void)
{
    assert(viewer_slot_index_to_id(0) == 1);
    assert(viewer_slot_index_to_id(1) == 2);
}

int main(void)
{
    test_ownership_timeout_transitions();
    test_dead_owner_clearing();
    test_slow_viewer_detection_thresholds();
    test_sustained_lag_threshold_not_yet_reached();
    test_sustained_lag_threshold_crosses_exactly();
    test_sustained_lag_timer_resets_after_clear();
    test_repeated_lag_requires_fresh_full_window();
    test_viewer_ids_start_at_one();
    return 0;
}
