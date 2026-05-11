#include <assert.h>

#include <freerdp/channels/rdpgfx.h>

#include "test_utils.h"
#include "viewer_internal.h"

static void
test_caps_selection_prefers_highest_version_without_canonical_caps(void) {
  const RDPGFX_CAPSET advertised[] = {
      {.version = RDPGFX_CAPVERSION_81, .flags = 0},
      {.version = RDPGFX_CAPVERSION_107,
       .flags = RDPGFX_CAPS_FLAG_AVC420_ENABLED},
      {.version = RDPGFX_CAPVERSION_103, .flags = RDPGFX_CAPS_FLAG_SMALL_CACHE},
  };
  RDPGFX_CAPSET selected = {0};

  assert(viewer_gfx_select_compatible_caps(
      NULL, FALSE, advertised,
      (UINT16)(sizeof(advertised) / sizeof(advertised[0])), &selected));
  assert(selected.version == RDPGFX_CAPVERSION_107);
  assert(selected.flags == RDPGFX_CAPS_FLAG_AVC420_ENABLED);
}

static void test_caps_selection_requires_exact_canonical_match(void) {
  const RDPGFX_CAPSET canonical = {
      .version = RDPGFX_CAPVERSION_103,
      .flags = RDPGFX_CAPS_FLAG_SMALL_CACHE,
  };
  const RDPGFX_CAPSET compatible[] = {
      {.version = RDPGFX_CAPVERSION_81, .flags = 0},
      {.version = RDPGFX_CAPVERSION_103, .flags = RDPGFX_CAPS_FLAG_SMALL_CACHE},
  };
  const RDPGFX_CAPSET incompatible[] = {
      {.version = RDPGFX_CAPVERSION_103, .flags = 0},
      {.version = RDPGFX_CAPVERSION_107, .flags = RDPGFX_CAPS_FLAG_SMALL_CACHE},
  };
  RDPGFX_CAPSET selected = {0};

  assert(viewer_gfx_select_compatible_caps(
      &canonical, TRUE, compatible,
      (UINT16)(sizeof(compatible) / sizeof(compatible[0])), &selected));
  assert(selected.version == canonical.version);
  assert(selected.flags == canonical.flags);

  assert(!viewer_gfx_select_compatible_caps(
      &canonical, TRUE, incompatible,
      (UINT16)(sizeof(incompatible) / sizeof(incompatible[0])), &selected));
}

static void test_codec_replay_policy_marks_only_replay_safe_codecs_safe(void) {
  const RDPGFX_CAPSET avc_disabled_caps = {
      .version = RDPGFX_CAPVERSION_107,
      .flags = RDPGFX_CAPS_FLAG_AVC_DISABLED,
  };

  assert(viewer_gfx_codec_replay_policy(RDPGFX_CODECID_UNCOMPRESSED, NULL) ==
         VIEWER_GFX_CODEC_REPLAY_SAFE);
  assert(viewer_gfx_codec_replay_policy(RDPGFX_CODECID_CLEARCODEC, NULL) ==
         VIEWER_GFX_CODEC_REPLAY_SAFE);
  assert(viewer_gfx_codec_replay_policy(RDPGFX_CODECID_PLANAR, NULL) ==
         VIEWER_GFX_CODEC_REPLAY_SAFE);
  assert(viewer_gfx_codec_replay_policy(RDPGFX_CODECID_CAPROGRESSIVE, NULL) ==
         VIEWER_GFX_CODEC_REPLAY_SAFE);
  assert(viewer_gfx_codec_replay_policy(RDPGFX_CODECID_AVC420, NULL) ==
         VIEWER_GFX_CODEC_REPLAY_UNSAFE);
  assert(viewer_gfx_codec_replay_policy(RDPGFX_CODECID_AVC444,
                                        &avc_disabled_caps) ==
         VIEWER_GFX_CODEC_REPLAY_REJECTED_BY_CAPS);
}

static void test_strategy_selection_covers_wait_replay_and_refresh_paths(void) {
  ViewerLateJoinPolicyInputs inputs = {
      .rdpgfx_enabled = TRUE,
      .channel_opened = TRUE,
      .caps_compatible = TRUE,
      .replay_safe_codecs_only = TRUE,
      .backend_frame_in_progress = TRUE,
      .complete_frame_available = TRUE,
  };

  assert(viewer_late_join_select_strategy(&inputs) ==
         VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME);

  inputs.backend_frame_in_progress = FALSE;
  assert(viewer_late_join_select_strategy(&inputs) ==
         VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME);

  inputs.replay_safe_codecs_only = FALSE;
  assert(viewer_late_join_select_strategy(&inputs) ==
         VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME);

  inputs.rdpgfx_enabled = FALSE;
  inputs.replay_safe_codecs_only = TRUE;
  assert(viewer_late_join_select_strategy(&inputs) ==
         VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK);
}

static void test_vcm_progress_requires_joined_channel_after_activation(void) {
  ViewerGraphicsContext gfx = {0};

  assert(!viewer_gfx_vcm_progress_should_be_pumped(FALSE, &gfx));
  assert(!viewer_gfx_vcm_progress_should_be_pumped(TRUE, NULL));

  gfx.post_connect_complete = TRUE;
  gfx.vcm = (HANDLE)0x1;
  assert(!viewer_gfx_vcm_progress_should_be_pumped(TRUE, &gfx));

  gfx.drdynvc_joined = TRUE;
  assert(viewer_gfx_vcm_progress_should_be_pumped(TRUE, &gfx));

  gfx.post_connect_complete = FALSE;
  assert(!viewer_gfx_vcm_progress_should_be_pumped(TRUE, &gfx));
}

static void
test_joined_state_controls_drdynvc_init_and_rdpgfx_open_paths(void) {
  ViewerGraphicsContext gfx = {0};

  gfx.post_connect_complete = TRUE;
  gfx.vcm = (HANDLE)0x1;

  assert(!viewer_gfx_drdynvc_initialization_should_run(&gfx));
  assert(!viewer_gfx_rdpgfx_open_should_run(&gfx));

  gfx.drdynvc_joined = TRUE;
  gfx.drdynvc_state = DRDYNVC_STATE_NONE;
  assert(viewer_gfx_drdynvc_initialization_should_run(&gfx));
  assert(!viewer_gfx_rdpgfx_open_should_run(&gfx));

  gfx.drdynvc_state = DRDYNVC_STATE_READY;
  gfx.rdpgfx = (RdpgfxServerContext *)0x1;
  assert(!viewer_gfx_drdynvc_initialization_should_run(&gfx));
  assert(viewer_gfx_rdpgfx_open_should_run(&gfx));

  gfx.channel_opened = TRUE;
  assert(!viewer_gfx_rdpgfx_open_should_run(&gfx));
}

static void test_ack_release_requires_target_frame_to_be_acknowledged(void) {
  assert(!viewer_late_join_ack_releases_live(0, 12));
  assert(!viewer_late_join_ack_releases_live(42, 41));
  assert(viewer_late_join_ack_releases_live(42, 42));
  assert(viewer_late_join_ack_releases_live(42, 99));
}

static void test_timeout_fallback_only_applies_to_waiting_paths(void) {
  assert(!viewer_late_join_timeout_fallback_due(
      VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK, 1000, 5000, 3000, TRUE));
  assert(!viewer_late_join_timeout_fallback_due(
      VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME, 1000, 3999, 3000, TRUE));
  assert(viewer_late_join_timeout_fallback_due(
      VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME, 1000, 4000, 3000, TRUE));
  assert(!viewer_late_join_timeout_fallback_due(
      VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME, 1000, 5000, 3000, FALSE));
  assert(viewer_late_join_timeout_fallback_due(
      VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME, 2000, 5000, 3000, FALSE));
  assert(!viewer_late_join_timeout_fallback_due(
      VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME, 2000, 1999, 3000, FALSE));
}

static void
test_activation_waits_for_caps_and_caps_confirm_can_restart_join(void) {
  ViewerGraphicsContext gfx = {0};

  gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_PENDING;
  gfx.ready = TRUE;
  gfx.join_state = VIEWER_JOIN_STATE_PENDING;
  gfx.join_strategy = VIEWER_JOIN_STRATEGY_NONE;
  gfx.join_start_ts = 1000;

  assert(viewer_gfx_activation_waits_for_rdpgfx_caps(&gfx));
  assert(!viewer_gfx_negotiation_is_classic_fallback(&gfx));
  assert(!viewer_gfx_negotiation_is_rdpgfx_ready(&gfx));
  assert(viewer_gfx_pending_activation_begins_rdpgfx_join(&gfx));
  assert(!viewer_gfx_pending_activation_timeout_due(&gfx, 3999, 3000));
  assert(viewer_gfx_pending_activation_timeout_due(&gfx, 4000, 3000));

  gfx.rdpgfx_temporarily_disabled = TRUE;
  assert(!viewer_gfx_activation_waits_for_rdpgfx_caps(&gfx));
  assert(!viewer_gfx_pending_activation_begins_rdpgfx_join(&gfx));

  gfx.rdpgfx_temporarily_disabled = FALSE;
  gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_RDPEGFX_READY;
  assert(!viewer_gfx_activation_waits_for_rdpgfx_caps(&gfx));
  assert(viewer_gfx_pending_activation_begins_rdpgfx_join(&gfx));

  gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK;
  assert(!viewer_gfx_activation_waits_for_rdpgfx_caps(&gfx));
  assert(viewer_gfx_negotiation_is_classic_fallback(&gfx));
  assert(!viewer_gfx_pending_activation_timeout_due(&gfx, 5000, 3000));

  gfx.join_strategy = VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK;
  assert(!viewer_gfx_pending_activation_begins_rdpgfx_join(&gfx));
}

int main(void) {
  test_suppress_crt_dialogs();
  test_caps_selection_prefers_highest_version_without_canonical_caps();
  test_caps_selection_requires_exact_canonical_match();
  test_codec_replay_policy_marks_only_replay_safe_codecs_safe();
  test_strategy_selection_covers_wait_replay_and_refresh_paths();
  test_vcm_progress_requires_joined_channel_after_activation();
  test_joined_state_controls_drdynvc_init_and_rdpgfx_open_paths();
  test_ack_release_requires_target_frame_to_be_acknowledged();
  test_timeout_fallback_only_applies_to_waiting_paths();
  test_activation_waits_for_caps_and_caps_confirm_can_restart_join();
  return 0;
}
