#include "test_utils.h"
#include "viewer_internal.h"
#include <assert.h>
#include <freerdp/channels/rdpgfx.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_ownership_timeout_transitions(void) {
  ViewerInputOwnershipState state = {0};

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

static void test_dead_owner_clearing(void) {
  ViewerInputOwnershipState state = {.owner_viewer_id = 1,
                                     .last_input_ts = 200};

  assert(viewer_input_try_acquire(&state, 2, TRUE, TRUE, FALSE, 210, 50));
  assert(state.owner_viewer_id == 2);
  assert(state.last_input_ts == 210);
}

static void test_slow_viewer_detection_thresholds(void) {
  assert(!viewer_is_slow(0, FALSE));
  assert(viewer_is_slow(VIEWER_SEVERE_LAG_INTERVALS, FALSE));
  assert(viewer_is_slow(0, TRUE));
}

static void test_sustained_lag_threshold_not_yet_reached(void) {
  const UINT32 disconnect_threshold_ms = VIEWER_SEVERE_LAG_INTERVALS * 16U;
  Viewer viewer = {0};

  viewer.sustained_lag_start_ts = 1000;

  assert(viewer_lag_signal_active(&viewer, TRUE));
  assert(!viewer_disconnect_due(&viewer, disconnect_threshold_ms,
                                1000 + disconnect_threshold_ms - 1U));
}

static void test_sustained_lag_threshold_crosses_exactly(void) {
  const UINT32 disconnect_threshold_ms = VIEWER_SEVERE_LAG_INTERVALS * 16U;
  Viewer viewer = {0};

  viewer.sustained_lag_start_ts = 1000;
  viewer.write_block_events = 1;
  viewer.consecutive_lag_intervals = VIEWER_SEVERE_LAG_INTERVALS;

  assert(viewer_lag_signal_active(&viewer, FALSE));
  assert(viewer_disconnect_due(&viewer, disconnect_threshold_ms,
                               1000 + disconnect_threshold_ms));
}

static void test_sustained_lag_timer_resets_after_clear(void) {
  const UINT32 disconnect_threshold_ms = VIEWER_SEVERE_LAG_INTERVALS * 16U;
  Viewer viewer = {0};

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

static void test_repeated_lag_requires_fresh_full_window(void) {
  const UINT32 disconnect_threshold_ms = VIEWER_SEVERE_LAG_INTERVALS * 16U;
  Viewer viewer = {0};

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

static void test_viewer_ids_start_at_one(void) {
  assert(viewer_slot_index_to_id(0) == 1);
  assert(viewer_slot_index_to_id(1) == 2);
}

static int test_viewer_monitor_from_size_uses_inclusive_bounds(void) {
  MONITOR_DEF monitor = {0};

  if (!viewer_monitor_from_size(1920, 1080, &monitor))
    return 1;
  if (monitor.left != 0)
    return 1;
  if (monitor.top != 0)
    return 1;
  if (monitor.right != 1919)
    return 1;
  if (monitor.bottom != 1079)
    return 1;
  if (monitor.flags != MONITOR_PRIMARY)
    return 1;
  return 0;
}

static int test_monitor_layout_init_uses_inclusive_monitor_bounds(void) {
  MonitorLayout layout = {0};

  monitor_layout_init(&layout, 2);
  if (layout.monitor_count != 2)
    return 1;
  if (layout.total_width != 3840)
    return 1;
  if (layout.total_height != 1080)
    return 1;
  if (layout.monitors[0].left != 0)
    return 1;
  if (layout.monitors[0].right != 1919)
    return 1;
  if (layout.monitors[0].bottom != 1079)
    return 1;
  if (layout.monitors[0].flags != MONITOR_PRIMARY)
    return 1;
  if (layout.monitors[1].left != 1920)
    return 1;
  if (layout.monitors[1].right != 3839)
    return 1;
  if (layout.monitors[1].bottom != 1079)
    return 1;
  if (layout.monitors[1].flags != 0)
    return 1;
  return 0;
}

static void test_gfx_failure_policy_pre_activation_allows_fallback(void) {
  ViewerGraphicsContext gfx = {0};

  gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_RDPEGFX_READY;
  gfx.join_state = VIEWER_JOIN_STATE_PENDING;
  gfx.join_strategy = VIEWER_JOIN_STRATEGY_NONE;
  gfx.use_rdpgfx = TRUE;

  assert(!viewer_gfx_failure_requires_disconnect(&gfx, FALSE));
  assert(!viewer_gfx_failure_requires_disconnect(&gfx, TRUE));
}

static void test_gfx_failure_policy_live_rdpgfx_disconnects(void) {
  ViewerGraphicsContext gfx = {0};

  gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_RDPEGFX_READY;
  gfx.join_state = VIEWER_JOIN_STATE_LIVE;
  gfx.join_strategy = VIEWER_JOIN_STRATEGY_NONE;
  gfx.use_rdpgfx = TRUE;

  assert(viewer_gfx_failure_requires_disconnect(&gfx, TRUE));
}

static void test_gfx_failure_policy_classic_fallback_stays_classic(void) {
  ViewerGraphicsContext gfx = {0};

  gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK;
  gfx.join_state = VIEWER_JOIN_STATE_LIVE;
  gfx.join_strategy = VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK;
  gfx.use_rdpgfx = FALSE;
  gfx.rdpgfx_temporarily_disabled = TRUE;

  assert(!viewer_gfx_failure_requires_disconnect(&gfx, TRUE));
}

#ifdef RDPGFX_CAPVERSION_8
static RDPGFX_CAPSET test_gfx_cap(UINT32 version, UINT32 flags) {
  RDPGFX_CAPSET cap = {0};

  cap.version = version;
  cap.flags = flags;
  return cap;
}

static UINT32 test_gfx_avc_disabled_flag(void) {
#ifdef RDPGFX_CAPS_FLAG_AVC_DISABLED
  return RDPGFX_CAPS_FLAG_AVC_DISABLED;
#else
  return 0;
#endif
}

static int test_gfx_cap_description_formats_version_flags(void) {
  enum { CAP_DESCRIPTION_CAPACITY = 80 };
  char *buffer = (char *)calloc(CAP_DESCRIPTION_CAPACITY, sizeof(*buffer));
  RDPGFX_CAPSET cap = test_gfx_cap(0x12345678U, 0x9ABCDEF0U);
  int result = 1;

  if (!buffer)
    return 1;

  if (!viewer_gfx_capset_describe(&cap, buffer, CAP_DESCRIPTION_CAPACITY))
    goto cleanup;
  if (strcmp(buffer, "version=0x12345678 flags=0x9ABCDEF0") != 0)
    goto cleanup;
  result = 0;

cleanup:
  free(buffer);
  return result;
}

static void test_gfx_supported_clean_cap_selected(void) {
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET advertised[] = {test_gfx_cap(RDPGFX_CAPVERSION_8, 0)};

  assert(viewer_gfx_caps_is_whitelisted(&advertised[0]));
  assert(
      viewer_gfx_select_compatible_caps(NULL, FALSE, advertised, 1, &selected));
  assert(selected.version == RDPGFX_CAPVERSION_8);
  assert(selected.flags == 0);
}

static void
test_gfx_highest_unsupported_version_rejected_without_downgrade(void) {
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET advertised[] = {test_gfx_cap(UINT32_MAX, 0)};

  assert(!viewer_gfx_caps_is_whitelisted(&advertised[0]));
  assert(!viewer_gfx_select_compatible_caps(NULL, FALSE, advertised, 1,
                                            &selected));
}

static void test_gfx_unsupported_avc_flags_rejected(void) {
#if defined(RDPGFX_CAPS_FLAG_AVC420_ENABLED) ||                                \
    defined(RDPGFX_CAPS_FLAG_AVC_THINCLIENT)
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET advertised[] = {
      test_gfx_cap(RDPGFX_CAPVERSION_8, 0
#ifdef RDPGFX_CAPS_FLAG_AVC420_ENABLED
                                            | RDPGFX_CAPS_FLAG_AVC420_ENABLED
#endif
#ifdef RDPGFX_CAPS_FLAG_AVC_THINCLIENT
                                            | RDPGFX_CAPS_FLAG_AVC_THINCLIENT
#endif
                   )};

  assert(!viewer_gfx_caps_is_whitelisted(&advertised[0]));
  assert(!viewer_gfx_select_compatible_caps(NULL, FALSE, advertised, 1,
                                            &selected));
#endif
}

static void test_gfx_unknown_flags_rejected(void) {
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET advertised[] = {test_gfx_cap(RDPGFX_CAPVERSION_8, 0x80000000U)};

  assert(!viewer_gfx_caps_is_whitelisted(&advertised[0]));
  assert(!viewer_gfx_select_compatible_caps(NULL, FALSE, advertised, 1,
                                            &selected));
}

static void test_gfx_downgrades_from_unsupported_high_cap(void) {
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET advertised[] = {
      test_gfx_cap(UINT32_MAX, 0),
      test_gfx_cap(RDPGFX_CAPVERSION_8, test_gfx_avc_disabled_flag())};

  assert(
      viewer_gfx_select_compatible_caps(NULL, FALSE, advertised, 2, &selected));
  assert(selected.version == RDPGFX_CAPVERSION_8);
  assert(selected.flags == test_gfx_avc_disabled_flag());
}

static void test_gfx_canonical_selected_only_from_whitelist(void) {
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET canonical = test_gfx_cap(RDPGFX_CAPVERSION_8, 0);
  RDPGFX_CAPSET unsupported_canonical = test_gfx_cap(UINT32_MAX, 0);
  RDPGFX_CAPSET advertised[] = {canonical};

  assert(viewer_gfx_select_compatible_caps(&canonical, TRUE, advertised, 1,
                                           &selected));
  assert(selected.version == canonical.version);
  assert(!viewer_gfx_select_compatible_caps(&unsupported_canonical, TRUE,
                                            advertised, 1, &selected));
}

static void test_gfx_unsupported_first_does_not_poison_canonical(void) {
  BOOL canonical_valid = FALSE;
  RDPGFX_CAPSET canonical = {0};
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET unsupported[] = {test_gfx_cap(UINT32_MAX, 0)};
  RDPGFX_CAPSET supported[] = {test_gfx_cap(RDPGFX_CAPVERSION_8, 0)};

  if (viewer_gfx_select_compatible_caps(canonical_valid ? &canonical : NULL,
                                        canonical_valid, unsupported, 1,
                                        &selected)) {
    canonical = selected;
    canonical_valid = TRUE;
  }

  assert(!canonical_valid);
  assert(viewer_gfx_select_compatible_caps(canonical_valid ? &canonical : NULL,
                                           canonical_valid, supported, 1,
                                           &selected));
  assert(selected.version == RDPGFX_CAPVERSION_8);
}

static int test_gfx_canonical_version_accepts_different_allowed_flags(void) {
  const UINT32 free_rdp_small_cache_flag = 0x00000002U;
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET canonical =
      test_gfx_cap(RDPGFX_CAPVERSION_8, free_rdp_small_cache_flag);
  RDPGFX_CAPSET advertised[] = {test_gfx_cap(RDPGFX_CAPVERSION_8, 0)};

  if (!viewer_gfx_select_compatible_caps(&canonical, TRUE, advertised, 1,
                                         &selected))
    return 1;
  if (selected.version != RDPGFX_CAPVERSION_8)
    return 1;
  if (selected.flags != advertised[0].flags)
    return 1;
  return 0;
}

static int test_gfx_canonical_different_version_falls_back_for_viewer(void) {
#ifdef RDPGFX_CAPVERSION_81
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET canonical = test_gfx_cap(RDPGFX_CAPVERSION_81, 0);
  RDPGFX_CAPSET advertised[] = {
      test_gfx_cap(RDPGFX_CAPVERSION_8, test_gfx_avc_disabled_flag())};

  if (viewer_gfx_select_compatible_caps(&canonical, TRUE, advertised, 1,
                                        &selected))
    return 1;
#endif
  return 0;
}

static int test_gfx_canonical_same_version_unknown_flags_rejected(void) {
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET canonical = test_gfx_cap(RDPGFX_CAPVERSION_8, 0);
  RDPGFX_CAPSET advertised[] = {test_gfx_cap(RDPGFX_CAPVERSION_8, 0x80000000U)};

  if (viewer_gfx_select_compatible_caps(&canonical, TRUE, advertised, 1,
                                        &selected))
    return 1;
  return 0;
}

#ifdef RDPGFX_CAPVERSION_10
static void test_gfx_prefers_lower_official_version(void) {
  RDPGFX_CAPSET selected = {0};
  RDPGFX_CAPSET advertised[] = {test_gfx_cap(RDPGFX_CAPVERSION_10, 0),
                                test_gfx_cap(RDPGFX_CAPVERSION_8, 0)};

  assert(
      viewer_gfx_select_compatible_caps(NULL, FALSE, advertised, 2, &selected));
  assert(selected.version == RDPGFX_CAPVERSION_8);
}
#endif
#endif

int main(void) {
  test_suppress_crt_dialogs();
  test_ownership_timeout_transitions();
  test_dead_owner_clearing();
  test_slow_viewer_detection_thresholds();
  test_sustained_lag_threshold_not_yet_reached();
  test_sustained_lag_threshold_crosses_exactly();
  test_sustained_lag_timer_resets_after_clear();
  test_repeated_lag_requires_fresh_full_window();
  test_viewer_ids_start_at_one();
  if (test_viewer_monitor_from_size_uses_inclusive_bounds() != 0)
    return 1;
  if (test_monitor_layout_init_uses_inclusive_monitor_bounds() != 0)
    return 1;
  test_gfx_failure_policy_pre_activation_allows_fallback();
  test_gfx_failure_policy_live_rdpgfx_disconnects();
  test_gfx_failure_policy_classic_fallback_stays_classic();
#ifdef RDPGFX_CAPVERSION_8
  if (test_gfx_cap_description_formats_version_flags() != 0)
    return 1;
  test_gfx_supported_clean_cap_selected();
  test_gfx_highest_unsupported_version_rejected_without_downgrade();
  test_gfx_unsupported_avc_flags_rejected();
  test_gfx_unknown_flags_rejected();
  test_gfx_downgrades_from_unsupported_high_cap();
  test_gfx_canonical_selected_only_from_whitelist();
  test_gfx_unsupported_first_does_not_poison_canonical();
  if (test_gfx_canonical_version_accepts_different_allowed_flags() != 0)
    return 1;
  if (test_gfx_canonical_different_version_falls_back_for_viewer() != 0)
    return 1;
  if (test_gfx_canonical_same_version_unknown_flags_rejected() != 0)
    return 1;
#ifdef RDPGFX_CAPVERSION_10
  test_gfx_prefers_lower_official_version();
#endif
#endif
  return 0;
}
