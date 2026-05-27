#include "viewer_gfx_codec_rfx.h"
#include "viewer_gfx_pipeline.h"
#include "viewer_server_internal.h"

#include <freerdp/codec/color.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum {
  TEST_SEND_RESET = 1,
  TEST_SEND_CREATE,
  TEST_SEND_MAP,
  TEST_SEND_START,
  TEST_SEND_SURFACE,
  TEST_SEND_END
} TestSendOp;

static TestSendOp g_send_order[8] = {0};
static UINT32 g_send_count = 0;
static UINT32 g_surface_count = 0;
static UINT g_fail_on_send = 0;
static RDPGFX_RESET_GRAPHICS_PDU g_last_reset = {0};
static RDPGFX_CREATE_SURFACE_PDU g_last_create = {0};
static RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU g_last_map = {0};
static RDPGFX_SURFACE_COMMAND g_last_surface = {0};
static const MONITOR_DEF *g_expected_reset_source = NULL;
static MONITOR_DEF g_last_reset_monitors[OMNIRDP_MAX_MONITORS] = {0};

static UINT test_record_send(TestSendOp op) {
  g_send_order[g_send_count++] = op;
  if (g_fail_on_send == g_send_count)
    return ERROR_INTERNAL_ERROR;
  return CHANNEL_RC_OK;
}

static UINT test_reset_graphics(RdpgfxServerContext *context,
                                const RDPGFX_RESET_GRAPHICS_PDU *reset) {
  (void)context;
  if (!reset || (reset->monitorCount == 0) ||
      (reset->monitorCount > OMNIRDP_MAX_MONITORS) || !reset->monitorDefArray)
    return ERROR_INTERNAL_ERROR;
  if (g_expected_reset_source &&
      (reset->monitorDefArray == g_expected_reset_source))
    return ERROR_INTERNAL_ERROR;
  g_last_reset = *reset;
  memcpy(g_last_reset_monitors, reset->monitorDefArray,
         sizeof(MONITOR_DEF) * reset->monitorCount);
  return test_record_send(TEST_SEND_RESET);
}

static UINT test_create_surface(RdpgfxServerContext *context,
                                const RDPGFX_CREATE_SURFACE_PDU *create) {
  (void)context;
  if (!create)
    return ERROR_INTERNAL_ERROR;
  g_last_create = *create;
  return test_record_send(TEST_SEND_CREATE);
}

static UINT test_map_surface(RdpgfxServerContext *context,
                             const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU *map) {
  (void)context;
  if (!map)
    return ERROR_INTERNAL_ERROR;
  g_last_map = *map;
  return test_record_send(TEST_SEND_MAP);
}

static UINT test_start_frame(RdpgfxServerContext *context,
                             const RDPGFX_START_FRAME_PDU *start) {
  (void)context;
  if (!start)
    return ERROR_INTERNAL_ERROR;
  return test_record_send(TEST_SEND_START);
}

static UINT test_surface_command(RdpgfxServerContext *context,
                                 const RDPGFX_SURFACE_COMMAND *cmd) {
  (void)context;
  if (!cmd)
    return ERROR_INTERNAL_ERROR;
  g_last_surface = *cmd;
  g_surface_count++;
  return test_record_send(TEST_SEND_SURFACE);
}

static UINT test_end_frame(RdpgfxServerContext *context,
                           const RDPGFX_END_FRAME_PDU *end) {
  (void)context;
  if (!end)
    return ERROR_INTERNAL_ERROR;
  return test_record_send(TEST_SEND_END);
}

static void reset_send_recorder(void) {
  memset(g_send_order, 0, sizeof(g_send_order));
  memset(&g_last_reset, 0, sizeof(g_last_reset));
  memset(&g_last_create, 0, sizeof(g_last_create));
  memset(&g_last_map, 0, sizeof(g_last_map));
  memset(&g_last_surface, 0, sizeof(g_last_surface));
  memset(g_last_reset_monitors, 0, sizeof(g_last_reset_monitors));
  g_expected_reset_source = NULL;
  g_send_count = 0;
  g_surface_count = 0;
  g_fail_on_send = 0;
}

RdpgfxServerContext *rdpgfx_server_context_new(HANDLE vcm) {
  (void)vcm;
  return NULL;
}

void rdpgfx_server_context_free(RdpgfxServerContext *context) { (void)context; }

HANDLE rdpgfx_server_get_event_handle(RdpgfxServerContext *context) {
  (void)context;
  return NULL;
}

UINT rdpgfx_server_handle_messages(RdpgfxServerContext *context) {
  (void)context;
  return CHANNEL_RC_OK;
}

HANDLE WTSOpenServerA(LPSTR pServerName) {
  (void)pServerName;
  return NULL;
}

void WTSCloseServer(HANDLE hServer) { (void)hServer; }

static int expect_true(BOOL value, const char *message) {
  if (!value) {
    (void)fprintf(stderr, "FAIL: %s\n", message);
    return 0;
  }
  return 1;
}

static int expect_uint32(UINT32 actual, UINT32 expected, const char *message) {
  if (actual != expected) {
    (void)fprintf(stderr, "FAIL: %s actual=%u expected=%u\n", message, actual,
                  expected);
    return 0;
  }
  return 1;
}

static int expect_uint64(UINT64 actual, UINT64 expected, const char *message) {
  if (actual != expected) {
    (void)fprintf(stderr, "FAIL: %s actual=%llu expected=%llu\n", message,
                  (unsigned long long)actual, (unsigned long long)expected);
    return 0;
  }
  return 1;
}

static BOOL init_test_viewer(Viewer *viewer, UINT32 width, UINT32 height) {
  memset(viewer, 0, sizeof(*viewer));
  viewer->id = 42;
  viewer->gfx.negotiated_width = width;
  viewer->gfx.negotiated_height = height;
  if (!InitializeCriticalSectionAndSpinCount(&viewer->gfx.lock, 4000))
    return FALSE;
  if (!InitializeCriticalSectionAndSpinCount(&viewer->send_lock, 4000)) {
    DeleteCriticalSection(&viewer->gfx.lock);
    return FALSE;
  }
  viewer->gfx.initialized = TRUE;
  return TRUE;
}

static void uninit_test_viewer(Viewer *viewer) {
  if (viewer && viewer->gfx.initialized) {
    viewer_gfx_pipeline_uninit(viewer);
    DeleteCriticalSection(&viewer->send_lock);
    DeleteCriticalSection(&viewer->gfx.lock);
    memset(viewer, 0, sizeof(*viewer));
  }
}

static void init_test_rdpgfx(RdpgfxServerContext *rdpgfx) {
  memset(rdpgfx, 0, sizeof(*rdpgfx));
  rdpgfx->ResetGraphics = test_reset_graphics;
  rdpgfx->CreateSurface = test_create_surface;
  rdpgfx->MapSurfaceToOutput = test_map_surface;
  rdpgfx->StartFrame = test_start_frame;
  rdpgfx->SurfaceCommand = test_surface_command;
  rdpgfx->EndFrame = test_end_frame;
}

static int test_activate_rejects_null_inputs(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  int ok = 1;

  ok = ok && expect_true(!viewer_gfx_pipeline_activate(NULL, &viewer),
                         "activate rejects null server");
  ok = ok && expect_true(!viewer_gfx_pipeline_activate(&server, NULL),
                         "activate rejects null viewer");
  ok = ok && expect_true(!viewer_gfx_pipeline_activate(&server, &viewer),
                         "activate rejects uninitialized gfx");
  return ok;
}

static int test_activation_initializes_placeholders(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 800, 600), "viewer init");
  viewer.gfx.next_frame_id = 99;
  viewer.gfx.last_sent_frame_id = 88;
  viewer.gfx.last_ack_frame_id = 77;
  viewer.gfx.active_surface_id = 55;
  viewer.gfx.surface_created = TRUE;
  viewer.gfx.force_full_present = FALSE;
  viewer.gfx.use_rdpgfx = TRUE;
  viewer.gfx.rdpgfx_temporarily_disabled = TRUE;

  ok = ok && expect_true(viewer_gfx_pipeline_activate(&server, &viewer),
                         "activate succeeds");
  ok = ok && expect_uint32(viewer.gfx.next_frame_id, 1, "next frame reset");
  ok = ok && expect_uint32(viewer.gfx.last_sent_frame_id, 0, "last sent reset");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 0, "last ack reset");
  ok = ok &&
       expect_uint32(viewer.gfx.active_surface_id, 0, "active surface reset");
  ok = ok &&
       expect_uint32(viewer.gfx.surface_width, 0, "surface width left unset");
  ok = ok &&
       expect_uint32(viewer.gfx.surface_height, 0, "surface height left unset");
  ok = ok && expect_true(!viewer.gfx.surface_created,
                         "surface placeholder not created");
  ok = ok &&
       expect_true(viewer.gfx.force_full_present, "full present requested");
  ok = ok && expect_true(viewer.gfx.dirty_baseline_required,
                         "dirty baseline required after activate");
  ok = ok && expect_true(!viewer.gfx.dirty_updates_enabled,
                         "dirty updates disabled after activate");
  ok = ok && expect_true(!viewer.gfx.use_rdpgfx,
                         "temporarily disabled does not enable rdpegfx");
  ok = ok && expect_true(viewer.gfx.negotiation_outcome ==
                             VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK,
                         "temporarily disabled falls back");
  ok = ok && expect_true(viewer.gfx.last_activated_ts != 0,
                         "activation timestamp recorded");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_repeated_activation_is_idempotent(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  UINT64 first_ts = 0;
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 1024, 768), "viewer init");
  ok = ok && expect_true(viewer_gfx_pipeline_activate(&server, &viewer),
                         "first activate succeeds");
  first_ts = viewer.gfx.last_activated_ts;
  viewer.gfx.last_sent_frame_id = 123;
  ok = ok && expect_true(viewer_gfx_pipeline_activate(&server, &viewer),
                         "second activate succeeds");
  ok = ok && expect_uint64(viewer.gfx.last_activated_ts, first_ts,
                           "activation timestamp stable");
  ok = ok && expect_uint32(viewer.gfx.last_sent_frame_id, 0,
                           "placeholder state stable after repeat");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_fallback_state_does_not_enable_rdpegfx(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 640, 480), "viewer init");
  viewer.gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK;
  viewer.gfx.use_rdpgfx = FALSE;
  ok = ok && expect_true(viewer_gfx_pipeline_activate(&server, &viewer),
                         "fallback activate succeeds");
  ok = ok && expect_true(!viewer.gfx.use_rdpgfx,
                         "fallback activation keeps rdpegfx disabled");
  ok = ok && expect_true(viewer.gfx.negotiation_outcome ==
                             VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK,
                         "fallback outcome retained");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_reset_join_state_clears_pipeline_owned_fields(void) {
  ViewerGraphicsContext gfx = {0};
  int ok = 1;

  gfx.use_rdpgfx = TRUE;
  gfx.rdpgfx_temporarily_disabled = TRUE;
  gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK;
  gfx.join_state = VIEWER_JOIN_STATE_REJECTED;
  gfx.join_strategy = VIEWER_JOIN_STRATEGY_REJECT;
  gfx.join_start_ts = 123;
  gfx.last_activated_ts = 456;

  viewer_gfx_pipeline_reset_join_state_locked(&gfx);
  ok = ok && expect_true(!gfx.use_rdpgfx, "reset disables rdpgfx use");
  ok = ok && expect_true(!gfx.rdpgfx_temporarily_disabled,
                         "reset clears temporary disable");
  ok = ok &&
       expect_true(gfx.negotiation_outcome == VIEWER_GFX_NEGOTIATION_PENDING,
                   "reset returns negotiation to pending");
  ok = ok && expect_true(gfx.join_state == VIEWER_JOIN_STATE_NONE,
                         "reset clears join state");
  ok = ok && expect_true(gfx.join_strategy == VIEWER_JOIN_STRATEGY_NONE,
                         "reset clears join strategy");
  ok = ok &&
       expect_uint64(gfx.join_start_ts, 0, "reset clears join start timestamp");
  ok = ok && expect_uint64(gfx.last_activated_ts, 0,
                           "reset clears activation timestamp");

  return ok;
}

static void configure_join_ready_viewer(ViewerServer *server, Viewer *viewer) {
  server->viewer_gfx_enabled = TRUE;
  viewer->activated = TRUE;
  viewer->gfx.post_connect_complete = TRUE;
  viewer->gfx.drdynvc_state = DRDYNVC_STATE_READY;
  viewer->gfx.channel_opened = TRUE;
  viewer->gfx.caps_ready = TRUE;
  viewer->gfx.use_rdpgfx = TRUE;
  viewer->gfx.rdpgfx_temporarily_disabled = FALSE;
  viewer->gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_RDPEGFX_READY;
}

static int test_peer_activation_sets_join_actions(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  ViewerGfxJoinResult result = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 640, 480), "viewer init");
  configure_join_ready_viewer(&server, &viewer);
  viewer.activated = FALSE;
  ok = ok && expect_true(viewer_gfx_pipeline_activate(&server, &viewer),
                         "activate succeeds");
  viewer_gfx_pipeline_on_peer_activated(&viewer, 100, &result);
  ok = ok && expect_true(viewer.gfx.ready, "peer activation marks gfx ready");
  ok = ok && expect_true(viewer.gfx.join_state == VIEWER_JOIN_STATE_PENDING,
                         "rdpgfx activation begins pending join");
  ok = ok && expect_true(viewer.gfx.join_strategy == VIEWER_JOIN_STRATEGY_NONE,
                         "rdpgfx activation uses no fallback strategy");
  ok = ok && expect_uint64(viewer.gfx.join_start_ts, 100,
                           "rdpgfx activation records join timestamp");
  ok = ok && expect_uint32(result.actions, VIEWER_GFX_JOIN_ACTION_NONE,
                           "rdpgfx activation waits for baseline step");

  viewer.gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_PENDING;
  viewer.gfx.join_state = VIEWER_JOIN_STATE_NONE;
  viewer_gfx_pipeline_on_peer_activated(&viewer, 150, &result);
  ok = ok && expect_true(viewer.gfx.join_state == VIEWER_JOIN_STATE_PENDING,
                         "pending activation waits for caps");
  ok = ok && expect_uint64(viewer.gfx.join_start_ts, 150,
                           "pending activation records join timestamp");

  uninit_test_viewer(&viewer);
  memset(&viewer, 0, sizeof(viewer));
  ok = ok && expect_true(init_test_viewer(&viewer, 640, 480), "viewer init 2");
  viewer.gfx.negotiation_outcome = VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK;
  viewer_gfx_pipeline_on_peer_activated(&viewer, 200, &result);
  ok = ok && expect_true(viewer.gfx.join_state == VIEWER_JOIN_STATE_LIVE,
                         "classic activation finishes join");
  ok = ok && expect_uint64(viewer.gfx.join_start_ts, 200,
                           "classic activation records join timestamp");
  ok =
      ok && expect_uint32(
                result.actions, VIEWER_GFX_JOIN_ACTION_ENQUEUE_CLASSIC_BASELINE,
                "classic activation requests classic baseline coordination");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_step_join_and_baseline_result_transitions(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  ViewerGfxJoinResult result = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 640, 480), "viewer init");
  configure_join_ready_viewer(&server, &viewer);
  viewer.gfx.join_state = VIEWER_JOIN_STATE_PENDING;
  viewer.gfx.join_strategy = VIEWER_JOIN_STRATEGY_NONE;
  viewer_gfx_pipeline_step_join(&server, &viewer, 300, &result);
  ok = ok && expect_uint32(result.actions, VIEWER_GFX_JOIN_ACTION_SEND_BASELINE,
                           "pending rdpgfx join requests baseline");

  viewer_gfx_pipeline_on_baseline_result(&viewer, 301, TRUE, &result);
  ok = ok && expect_true(viewer.gfx.join_state == VIEWER_JOIN_STATE_LIVE,
                         "successful baseline makes join live");
  ok = ok && expect_true(viewer.gfx.join_strategy == VIEWER_JOIN_STRATEGY_NONE,
                         "successful baseline clears join strategy");
  ok = ok &&
       expect_uint32(
           result.actions, VIEWER_GFX_JOIN_ACTION_SEND_POINTER_BASELINE,
           "successful baseline requests pointer baseline after framebuffer");

  viewer.gfx.join_state = VIEWER_JOIN_STATE_PENDING;
  viewer.gfx.join_strategy = VIEWER_JOIN_STRATEGY_NONE;
  viewer_gfx_pipeline_on_baseline_result(&viewer, 400, FALSE, &result);
  ok = ok && expect_true(viewer.gfx.join_state == VIEWER_JOIN_STATE_PENDING,
                         "failed baseline keeps fallback pending");
  ok = ok && expect_true(viewer.gfx.join_strategy ==
                             VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK,
                         "failed baseline selects classic fallback strategy");
  ok = ok &&
       expect_true(!viewer.gfx.use_rdpgfx, "failed baseline disables rdpgfx");
  ok = ok && expect_uint32(result.actions,
                           VIEWER_GFX_JOIN_ACTION_ENTER_CLASSIC_FALLBACK,
                           "failed baseline requests fallback coordination");

  configure_join_ready_viewer(&server, &viewer);
  viewer.gfx.join_state = VIEWER_JOIN_STATE_PENDING;
  viewer.gfx.join_strategy = VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK;
  viewer_gfx_pipeline_step_join(&server, &viewer, 500, &result);
  ok = ok && expect_uint32(result.actions,
                           VIEWER_GFX_JOIN_ACTION_ENTER_CLASSIC_FALLBACK,
                           "classic fallback join requests fallback action");

  configure_join_ready_viewer(&server, &viewer);
  viewer.gfx.join_state = VIEWER_JOIN_STATE_LIVE;
  viewer.gfx.dirty_updates_enabled = FALSE;
  viewer.gfx.dirty_baseline_required = TRUE;
  viewer_gfx_pipeline_on_baseline_result(&viewer, 550, TRUE, &result);
  ok = ok && expect_uint32(result.actions, VIEWER_GFX_JOIN_ACTION_NONE,
                           "successful live resize baseline does not request "
                           "late-join pointer baseline");

  viewer.gfx.join_state = VIEWER_JOIN_STATE_LIVE;
  viewer.gfx.dirty_updates_enabled = FALSE;
  viewer.gfx.dirty_baseline_required = TRUE;
  viewer_gfx_pipeline_on_baseline_result(&viewer, 600, FALSE, &result);
  ok = ok && expect_uint32(result.actions,
                           VIEWER_GFX_JOIN_ACTION_ENTER_CLASSIC_FALLBACK,
                           "failed live resize baseline preserves fallback");
  ok = ok && expect_true(!viewer.gfx.dirty_updates_enabled,
                         "failed live resize baseline leaves dirty disabled");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_snapshot_validation_rejects_not_ready(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[16] = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 320, 240), "viewer init");
  ok = ok &&
       expect_true(!viewer_gfx_pipeline_send_snapshot(NULL, &viewer, &snapshot),
                   "snapshot rejects null server");
  ok = ok &&
       expect_true(!viewer_gfx_pipeline_send_snapshot(&server, NULL, &snapshot),
                   "snapshot rejects null viewer");
  ok = ok &&
       expect_true(!viewer_gfx_pipeline_send_snapshot(&server, &viewer, NULL),
                   "snapshot rejects null snapshot");
  ok = ok && expect_true(!viewer_gfx_pipeline_send_snapshot(&server, &viewer,
                                                            &snapshot),
                         "snapshot rejects empty snapshot");

  snapshot.pixels = pixels;
  snapshot.width = 2;
  snapshot.height = 2;
  snapshot.stride = 8;
  snapshot.pixel_format = PIXEL_FORMAT_BGRX32;
  snapshot.pixel_bytes = sizeof(pixels);
  viewer.gfx.last_sent_frame_id = 5;
  viewer.gfx.last_ack_frame_id = 4;
  ok = ok && expect_true(!viewer_gfx_pipeline_send_snapshot(&server, &viewer,
                                                            &snapshot),
                         "snapshot rejects missing RDPEGFX context/caps");
  ok = ok && expect_uint32(viewer.gfx.last_sent_frame_id, 5,
                           "not-ready snapshot does not send frame");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 4,
                           "snapshot does not ack/migrate frames");
  ok = ok && expect_true(!viewer.gfx.surface_created,
                         "snapshot does not create surface");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_snapshot_sends_full_frame_baseline_order(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[16] = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 800, 600), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  snapshot.pixels = pixels;
  snapshot.width = 2;
  snapshot.height = 2;
  snapshot.stride = 8;
  snapshot.pixel_format = PIXEL_FORMAT_BGRX32;
  snapshot.pixel_bytes = sizeof(pixels);
  viewer.gfx.rdpgfx = &rdpgfx;
  viewer.gfx.caps_ready = TRUE;
  viewer.gfx.use_rdpgfx = TRUE;
  viewer.gfx.channel_opened = TRUE;
  viewer.gfx.next_frame_id = 9;

  reset_send_recorder();
  ok = ok && expect_true(
                 viewer_gfx_pipeline_send_snapshot(&server, &viewer, &snapshot),
                 "snapshot sends full-frame baseline");
  ok = ok && expect_uint32(g_send_count, 6, "six send callbacks");
  ok = ok && expect_uint32(g_send_order[0], TEST_SEND_RESET, "reset first");
  ok = ok && expect_uint32(g_send_order[1], TEST_SEND_CREATE, "create second");
  ok = ok && expect_uint32(g_send_order[2], TEST_SEND_MAP, "map third");
  ok = ok && expect_uint32(g_send_order[3], TEST_SEND_START, "start fourth");
  ok = ok && expect_uint32(g_send_order[4], TEST_SEND_SURFACE, "surface fifth");
  ok = ok && expect_uint32(g_send_order[5], TEST_SEND_END, "end sixth");
  ok = ok && expect_uint32(g_last_reset.width, 2, "reset uses snapshot width");
  ok =
      ok && expect_uint32(g_last_reset.height, 2, "reset uses snapshot height");
  ok = ok && expect_uint32(g_last_reset.monitorCount, 1,
                           "reset uses one fallback monitor");
  ok = ok && expect_uint32((UINT32)g_last_reset_monitors[0].right, 1,
                           "reset monitor uses snapshot right");
  ok = ok && expect_uint32((UINT32)g_last_reset_monitors[0].bottom, 1,
                           "reset monitor uses snapshot bottom");
  ok =
      ok && expect_uint32(g_last_create.width, 2, "create uses snapshot width");
  ok = ok &&
       expect_uint32(g_last_create.height, 2, "create uses snapshot height");
  ok = ok &&
       expect_uint32(g_last_map.surfaceId, 0, "map uses active surface id");
  ok = ok && expect_uint32(g_last_create.pixelFormat,
                           GFX_PIXEL_FORMAT_XRGB_8888, "wire format");
  ok = ok && expect_uint32(g_last_surface.format, PIXEL_FORMAT_BGRX32,
                           "surface command FreeRDP format");
  ok = ok && expect_uint32(g_last_surface.codecId, RDPGFX_CODECID_UNCOMPRESSED,
                           "uncompressed codec");
  ok = ok && expect_uint32(viewer.gfx.last_sent_frame_id, 9,
                           "last sent frame updated");
  ok = ok &&
       expect_uint32(viewer.gfx.next_frame_id, 10, "next frame incremented");
  ok = ok && expect_true(viewer.gfx.surface_created, "surface marked created");
  ok = ok && expect_uint32(viewer.gfx.surface_width, 2,
                           "surface width recorded from snapshot");
  ok = ok && expect_uint32(viewer.gfx.surface_height, 2,
                           "surface height recorded from snapshot");
  ok = ok && expect_true(!viewer.gfx.dirty_baseline_required,
                         "baseline requirement cleared");
  ok = ok && expect_true(viewer.gfx.dirty_updates_enabled,
                         "dirty updates enabled after baseline");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_snapshot_rfx_codec_emits_cavideo(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[64] = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(i + 1U);
  snapshot.pixels = pixels;
  snapshot.width = 4;
  snapshot.height = 4;
  snapshot.stride = 16;
  snapshot.pixel_format = PIXEL_FORMAT_BGRX32;
  snapshot.pixel_bytes = sizeof(pixels);
  viewer.gfx.rdpgfx = &rdpgfx;
  viewer.gfx.caps_ready = TRUE;
  viewer.gfx.use_rdpgfx = TRUE;
  viewer.gfx.channel_opened = TRUE;
  viewer.gfx.preferred_codec = VIEWER_GFX_CODEC_RFX;
  viewer.gfx.selected_codec = VIEWER_GFX_CODEC_RFX;

  reset_send_recorder();
  ok = ok && expect_true(
                 viewer_gfx_pipeline_send_snapshot(&server, &viewer, &snapshot),
                 "RFX snapshot sends full-frame baseline");
  ok = ok && expect_uint32(g_last_surface.codecId, RDPGFX_CODECID_CAVIDEO,
                           "RFX baseline uses CAVIDEO");
  ok = ok && expect_true(g_last_surface.length > 0, "RFX baseline payload set");
  ok = ok && expect_uint32(viewer.gfx.selected_codec, VIEWER_GFX_CODEC_RFX,
                           "RFX remains selected after success");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_snapshot_rfx_context_failure_downgrades_to_uncompressed(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[16] = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 2, 2), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  snapshot.pixels = pixels;
  snapshot.width = 2;
  snapshot.height = 2;
  snapshot.stride = 8;
  snapshot.pixel_format = PIXEL_FORMAT_BGRX32;
  snapshot.pixel_bytes = sizeof(pixels);
  viewer.gfx.rdpgfx = &rdpgfx;
  viewer.gfx.caps_ready = TRUE;
  viewer.gfx.use_rdpgfx = TRUE;
  viewer.gfx.channel_opened = TRUE;
  viewer.gfx.preferred_codec = VIEWER_GFX_CODEC_RFX;
  viewer.gfx.selected_codec = VIEWER_GFX_CODEC_RFX;

  viewer_gfx_rfx_test_set_force_context_new_failure(TRUE);
  reset_send_recorder();
  ok = ok && expect_true(
                 viewer_gfx_pipeline_send_snapshot(&server, &viewer, &snapshot),
                 "RFX init failure downgrades and sends baseline");
  viewer_gfx_rfx_test_set_force_context_new_failure(FALSE);
  ok = ok && expect_uint32(g_last_surface.codecId, RDPGFX_CODECID_UNCOMPRESSED,
                           "downgraded baseline uses uncompressed");
  ok = ok &&
       expect_uint32(viewer.gfx.selected_codec, VIEWER_GFX_CODEC_UNCOMPRESSED,
                     "viewer selected codec downgraded");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_snapshot_send_failure_propagates(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[16] = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 2, 2), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  snapshot.pixels = pixels;
  snapshot.width = 2;
  snapshot.height = 2;
  snapshot.stride = 8;
  snapshot.pixel_format = PIXEL_FORMAT_BGRX32;
  snapshot.pixel_bytes = sizeof(pixels);
  viewer.gfx.rdpgfx = &rdpgfx;
  viewer.gfx.caps_ready = TRUE;
  viewer.gfx.use_rdpgfx = TRUE;
  viewer.gfx.channel_opened = TRUE;
  viewer.gfx.next_frame_id = 4;

  reset_send_recorder();
  g_fail_on_send = 3;
  ok = ok && expect_true(!viewer_gfx_pipeline_send_snapshot(&server, &viewer,
                                                            &snapshot),
                         "send failure propagates");
  ok = ok && expect_uint32(g_send_count, 3, "stops at failing callback");
  ok = ok && expect_uint32(viewer.gfx.last_sent_frame_id, 0,
                           "failed send does not update frame");
  ok = ok && expect_true(!viewer.gfx.surface_created,
                         "failed send does not mark surface created");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static void configure_dirty_eligible_viewer(ViewerServer *server,
                                            Viewer *viewer,
                                            RdpgfxServerContext *rdpgfx) {
  server->viewer_gfx_enabled = TRUE;
  viewer->gfx.rdpgfx = rdpgfx;
  viewer->gfx.use_rdpgfx = TRUE;
  viewer->gfx.caps_ready = TRUE;
  viewer->gfx.channel_opened = TRUE;
  viewer->gfx.join_state = VIEWER_JOIN_STATE_LIVE;
  viewer->gfx.surface_created = TRUE;
  viewer->gfx.surface_width = viewer->gfx.negotiated_width;
  viewer->gfx.surface_height = viewer->gfx.negotiated_height;
  viewer->gfx.force_full_present = FALSE;
  viewer->gfx.dirty_updates_enabled = TRUE;
  viewer->gfx.dirty_baseline_required = FALSE;
  viewer->gfx.dirty_max_in_flight_frames = 2;
  viewer->gfx.dirty_max_in_flight_bytes = VIEWER_GFX_DIRTY_MAX_IN_FLIGHT_BYTES;
}

static int test_dirty_update_eligibility_denials_and_allowed(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  const char *reason = NULL;
  int ok = 1;

  snapshot.width = 2;
  snapshot.height = 2;
  snapshot.generation = 10;

  ok = ok && expect_true(init_test_viewer(&viewer, 2, 2), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);

  server.viewer_gfx_enabled = FALSE;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "dirty denied when gate disabled");
  server.viewer_gfx_enabled = TRUE;

  viewer.gfx.caps_ready = FALSE;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "dirty denied when not negotiated");
  viewer.gfx.caps_ready = TRUE;

  viewer.gfx.join_state = VIEWER_JOIN_STATE_PENDING;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "dirty denied before live");
  viewer.gfx.join_state = VIEWER_JOIN_STATE_LIVE;

  viewer.gfx.surface_created = FALSE;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "dirty denied before baseline surface creation");
  viewer.gfx.surface_created = TRUE;

  snapshot.width = 3;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "dirty denied when snapshot width differs");
  snapshot.width = 2;
  snapshot.height = 3;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "dirty denied when snapshot height differs");
  snapshot.height = 2;

  ok = ok && expect_true(viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "dirty allowed when live under limit");

  viewer.gfx.dirty_in_flight_frames = 2;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "dirty denied at in-flight limit");
  viewer.gfx.dirty_in_flight_frames = 0;

  viewer.gfx.dirty_suspended_for_no_ack = TRUE;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "dirty denied while suspended");
  viewer.gfx.dirty_suspended_for_no_ack = FALSE;

  viewer.gfx.dirty_baseline_required = TRUE;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "dirty denied when baseline required");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_surface_invalidation_clears_pipeline_state(void) {
  Viewer viewer = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  viewer.gfx.active_surface_id = 7;
  viewer.gfx.surface_width = 4;
  viewer.gfx.surface_height = 4;
  viewer.gfx.surface_created = TRUE;
  viewer.gfx.force_full_present = FALSE;
  viewer.gfx.dirty_updates_enabled = TRUE;
  viewer.gfx.dirty_baseline_required = FALSE;
  viewer.gfx.dirty_last_sent_generation = 101;
  viewer.gfx.dirty_last_acked_generation = 100;
  viewer.gfx.dirty_reset_generation = 99;
  viewer.gfx.dirty_in_flight_frames = 1;
  viewer.gfx.dirty_in_flight_bytes = 99;
  viewer.gfx.frame_epoch = 3;
  viewer.gfx.dirty_suspended_for_no_ack = TRUE;
  viewer.gfx.dirty_frame_valid[0] = TRUE;
  viewer.gfx.dirty_frame_ids[0] = 99;
  viewer.gfx.dirty_frame_epochs[0] = 3;
  viewer.gfx.dirty_frame_generations[0] = 123;
  viewer.gfx.dirty_frame_payload_bytes[0] = 99;
  viewer.gfx.dirty_frame_sent_ts[0] = 456;

  viewer_gfx_pipeline_invalidate_surface_locked(&viewer.gfx);
  ok = ok && expect_uint32(viewer.gfx.active_surface_id, 0,
                           "invalidation clears active surface");
  ok = ok && expect_uint32(viewer.gfx.surface_width, 0,
                           "invalidation clears surface width");
  ok = ok && expect_uint32(viewer.gfx.surface_height, 0,
                           "invalidation clears surface height");
  ok = ok && expect_true(!viewer.gfx.surface_created,
                         "invalidation clears surface created");
  ok = ok && expect_true(viewer.gfx.force_full_present,
                         "invalidation requests full present");
  ok = ok && expect_true(viewer.gfx.dirty_baseline_required,
                         "invalidation requires dirty baseline");
  ok = ok && expect_true(!viewer.gfx.dirty_updates_enabled,
                         "invalidation disables dirty updates");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_sent_generation, 0,
                           "invalidation clears last sent generation");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_acked_generation, 0,
                           "invalidation clears last acked generation");
  ok = ok && expect_uint64(viewer.gfx.dirty_reset_generation, 0,
                           "invalidation clears reset generation");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "invalidation clears dirty in-flight");
  ok = ok && expect_uint64(viewer.gfx.dirty_in_flight_bytes, 0,
                           "invalidation clears dirty bytes");
  ok = ok && expect_uint64(viewer.gfx.frame_epoch, 4,
                           "invalidation increments epoch");
  ok = ok && expect_true(!viewer.gfx.dirty_suspended_for_no_ack,
                         "invalidation clears dirty suspension");
  ok = ok && expect_true(!viewer.gfx.dirty_frame_valid[0],
                         "invalidation clears dirty map validity");
  ok = ok && expect_uint32(viewer.gfx.dirty_frame_ids[0], 0,
                           "invalidation clears dirty frame id");
  ok = ok && expect_uint64(viewer.gfx.dirty_frame_epochs[0], 0,
                           "invalidation clears dirty epoch");
  ok = ok && expect_uint64(viewer.gfx.dirty_frame_generations[0], 0,
                           "invalidation clears dirty generation");
  ok = ok && expect_uint64(viewer.gfx.dirty_frame_payload_bytes[0], 0,
                           "invalidation clears dirty payload bytes");
  ok = ok && expect_uint64(viewer.gfx.dirty_frame_sent_ts[0], 0,
                           "invalidation clears dirty timestamp");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_dirty_update_eligibility_is_per_viewer(void) {
  ViewerServer server = {0};
  Viewer viewer_a = {0};
  Viewer viewer_b = {0};
  RdpgfxServerContext rdpgfx_a = {0};
  RdpgfxServerContext rdpgfx_b = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  const char *reason = NULL;
  int ok = 1;

  snapshot.width = 2;
  snapshot.height = 2;
  snapshot.generation = 20;

  ok = ok && expect_true(init_test_viewer(&viewer_a, 2, 2), "viewer A init");
  ok = ok && expect_true(init_test_viewer(&viewer_b, 2, 2), "viewer B init");
  configure_dirty_eligible_viewer(&server, &viewer_a, &rdpgfx_a);
  configure_dirty_eligible_viewer(&server, &viewer_b, &rdpgfx_b);
  viewer_a.gfx.dirty_suspended_for_no_ack = TRUE;

  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer_a, &snapshot, &reason),
                         "viewer A suspended denied");
  ok = ok && expect_true(viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer_b, &snapshot, &reason),
                         "viewer B remains allowed");

  viewer_a.gfx.rdpgfx = NULL;
  viewer_b.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer_a);
  uninit_test_viewer(&viewer_b);
  return ok;
}

static ViewerFramebufferSnapshot
make_dirty_snapshot(BYTE *pixels, UINT64 generation, UINT32 rect_count) {
  ViewerFramebufferSnapshot snapshot = {0};

  snapshot.width = 4;
  snapshot.height = 4;
  snapshot.stride = 16;
  snapshot.pixel_format = PIXEL_FORMAT_BGRX32;
  snapshot.pixels = pixels;
  snapshot.pixel_bytes = 64;
  snapshot.generation = generation;
  snapshot.dirty_rect_count = rect_count;
  snapshot.dirty_rects[0].left = 1;
  snapshot.dirty_rects[0].top = 1;
  snapshot.dirty_rects[0].right = 1;
  snapshot.dirty_rects[0].bottom = 1;
  if (rect_count > 1) {
    snapshot.dirty_rects[1].left = 2;
    snapshot.dirty_rects[1].top = 2;
    snapshot.dirty_rects[1].right = 2;
    snapshot.dirty_rects[1].bottom = 2;
  }
  return snapshot;
}

static int test_dirty_update_send_order_and_ack(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[128] = {0};
  ViewerFramebufferSnapshot snapshot = make_dirty_snapshot(pixels, 30, 1);
  const char *reason = NULL;
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.next_frame_id = 11;

  reset_send_recorder();
  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(&server, &viewer,
                                                               &snapshot),
                         "dirty update sends");
  ok = ok && expect_uint32(g_send_count, 3, "start surface end only");
  ok = ok && expect_uint32(g_send_order[0], TEST_SEND_START, "dirty start");
  ok = ok && expect_uint32(g_send_order[1], TEST_SEND_SURFACE, "dirty surface");
  ok = ok && expect_uint32(g_send_order[2], TEST_SEND_END, "dirty end");
  ok = ok && expect_uint32(g_surface_count, 1, "one dirty surface command");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 1,
                           "in-flight incremented");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_sent_generation, 30,
                           "sent generation recorded");
  ok = ok && expect_true(viewer.gfx.dirty_frame_sent_ts[0] != 0,
                         "dirty send timestamp recorded");
  ok = ok && expect_uint64(viewer.gfx.dirty_in_flight_bytes, 4,
                           "dirty payload bytes recorded");
  ok = ok && expect_uint64(viewer.gfx.dirty_frame_epochs[0],
                           viewer.gfx.frame_epoch, "dirty epoch recorded");
  ok = ok && expect_uint64(viewer.gfx.dirty_frame_payload_bytes[0], 4,
                           "dirty map payload bytes recorded");
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "in-flight limit denies when max set to one later");

  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 999),
                           CHANNEL_RC_OK, "unknown ack accepted");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 0,
                           "unknown ack does not record last ack");
  ok = ok && expect_uint64(viewer.gfx.last_presented_timestamp, 0,
                           "unknown ack does not record timestamp");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 1,
                           "unknown ack ignored");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_acked_generation, 0,
                           "unknown ack does not advance generation");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 11),
                           CHANNEL_RC_OK, "matching ack accepted");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 11,
                           "matching ack records last ack");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "matching ack decrements in-flight");
  ok = ok && expect_uint64(viewer.gfx.dirty_in_flight_bytes, 0,
                           "matching ack clears in-flight bytes");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_acked_generation, 30,
                           "matching ack advances generation");
  ok = ok && expect_uint64(viewer.gfx.last_ack_epoch, viewer.gfx.frame_epoch,
                           "matching ack records epoch");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_dirty_update_rfx_codec_emits_cavideo(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[128] = {0};
  ViewerFramebufferSnapshot snapshot = make_dirty_snapshot(pixels, 31, 1);
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.next_frame_id = 12;
  viewer.gfx.preferred_codec = VIEWER_GFX_CODEC_RFX;
  viewer.gfx.selected_codec = VIEWER_GFX_CODEC_RFX;
  for (size_t i = 0; i < sizeof(pixels); i++)
    pixels[i] = (BYTE)(i + 1U);

  reset_send_recorder();
  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(&server, &viewer,
                                                               &snapshot),
                         "RFX dirty update sends");
  ok = ok && expect_uint32(g_surface_count, 1, "one RFX dirty command");
  ok = ok && expect_uint32(g_last_surface.codecId, RDPGFX_CODECID_CAVIDEO,
                           "RFX dirty uses CAVIDEO");
  ok = ok && expect_uint32(g_last_surface.left, 1, "RFX dirty left");
  ok = ok && expect_uint32(g_last_surface.top, 1, "RFX dirty top");
  ok = ok && expect_true(viewer.gfx.dirty_in_flight_bytes > 0,
                         "RFX dirty records payload bytes");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_monitor_layout_snapshot_uses_server_layout(void) {
  ViewerServer server = {0};
  MonitorLayout layout = {0};
  BYTE pixels[128] = {0};
  ViewerFramebufferSnapshot snapshot = make_dirty_snapshot(pixels, 210, 1);
  int ok = 1;

  server.monitor_layout.monitor_count = 2;
  server.monitor_layout.total_width = 3840;
  server.monitor_layout.total_height = 1080;
  server.monitor_layout.monitors[0].left = 0;
  server.monitor_layout.monitors[0].top = 0;
  server.monitor_layout.monitors[0].right = 1919;
  server.monitor_layout.monitors[0].bottom = 1079;
  server.monitor_layout.monitors[0].flags = MONITOR_PRIMARY;
  server.monitor_layout.monitors[1].left = 1920;
  server.monitor_layout.monitors[1].top = 0;
  server.monitor_layout.monitors[1].right = 3839;
  server.monitor_layout.monitors[1].bottom = 1079;

  ok = ok && expect_true(viewer_gfx_pipeline_monitor_layout_snapshot(
                             &server, &snapshot, &layout),
                         "monitor layout snapshot succeeds");
  ok = ok &&
       expect_uint32(layout.monitor_count, 2, "server monitor count copied");
  ok = ok &&
       expect_uint32(layout.total_width, 3840, "server total width copied");
  ok = ok &&
       expect_uint32(layout.total_height, 1080, "server total height copied");
  ok = ok && expect_uint32((UINT32)layout.monitors[0].flags, MONITOR_PRIMARY,
                           "primary flag copied");
  ok = ok && expect_uint32((UINT32)layout.monitors[1].left, 1920,
                           "second monitor copied");

  return ok;
}

static int test_monitor_layout_snapshot_falls_back_to_framebuffer(void) {
  ViewerServer server = {0};
  MonitorLayout layout = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_dirty_snapshot(pixels, 211, 1);
  int ok = 1;

  snapshot.width = 4;
  snapshot.height = 4;
  ok = ok && expect_true(viewer_gfx_pipeline_monitor_layout_snapshot(
                             &server, &snapshot, &layout),
                         "fallback monitor layout succeeds");
  ok = ok &&
       expect_uint32(layout.monitor_count, 1, "fallback monitor count is one");
  ok = ok && expect_uint32(layout.total_width, 4, "fallback width copied");
  ok = ok && expect_uint32(layout.total_height, 4, "fallback height copied");
  ok = ok && expect_uint32((UINT32)layout.monitors[0].right, 3,
                           "fallback right is inclusive");
  ok = ok && expect_uint32((UINT32)layout.monitors[0].bottom, 3,
                           "fallback bottom is inclusive");
  ok = ok && expect_uint32((UINT32)layout.monitors[0].flags, MONITOR_PRIMARY,
                           "fallback primary flag set");

  return ok;
}

static int test_snapshot_sends_server_monitor_layout(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  ViewerFramebufferSnapshot snapshot = {0};
  BYTE pixels[128] = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 800, 600), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  snapshot.pixels = pixels;
  snapshot.width = 8;
  snapshot.height = 4;
  snapshot.stride = 32;
  snapshot.pixel_format = PIXEL_FORMAT_BGRX32;
  snapshot.pixel_bytes = sizeof(pixels);
  server.monitor_layout.monitor_count = 2;
  server.monitor_layout.total_width = 8;
  server.monitor_layout.total_height = 4;
  server.monitor_layout.monitors[0].left = 0;
  server.monitor_layout.monitors[0].top = 0;
  server.monitor_layout.monitors[0].right = 3;
  server.monitor_layout.monitors[0].bottom = 3;
  server.monitor_layout.monitors[0].flags = MONITOR_PRIMARY;
  server.monitor_layout.monitors[1].left = 4;
  server.monitor_layout.monitors[1].top = 0;
  server.monitor_layout.monitors[1].right = 7;
  server.monitor_layout.monitors[1].bottom = 3;
  viewer.gfx.rdpgfx = &rdpgfx;
  viewer.gfx.caps_ready = TRUE;
  viewer.gfx.use_rdpgfx = TRUE;
  viewer.gfx.channel_opened = TRUE;

  reset_send_recorder();
  ok = ok && expect_true(
                 viewer_gfx_pipeline_send_snapshot(&server, &viewer, &snapshot),
                 "snapshot sends multi-monitor reset");
  ok = ok && expect_uint32(g_last_reset.monitorCount, 2,
                           "reset uses server monitor count");
  ok = ok && expect_uint32((UINT32)g_last_reset_monitors[0].flags,
                           MONITOR_PRIMARY, "primary flag sent");
  ok = ok && expect_uint32((UINT32)g_last_reset_monitors[1].left, 4,
                           "second monitor left sent");
  ok = ok && expect_uint32((UINT32)g_last_reset_monitors[1].right, 7,
                           "second monitor right sent");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_frame_ack_accepts_zero_without_dirty_state_change(void) {
  Viewer viewer = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  viewer.gfx.last_ack_frame_id = 77;
  viewer.gfx.last_presented_timestamp = 88;
  viewer.gfx.dirty_frame_valid[0] = TRUE;
  viewer.gfx.dirty_frame_ids[0] = 10;
  viewer.gfx.dirty_frame_generations[0] = 20;
  viewer.gfx.dirty_in_flight_frames = 1;

  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 0),
                           CHANNEL_RC_OK, "zero ack accepted");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 0,
                           "zero ack records last ack");
  ok = ok && expect_true(viewer.gfx.last_presented_timestamp != 0 &&
                             viewer.gfx.last_presented_timestamp != 88,
                         "zero ack updates presented timestamp");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 1,
                           "zero ack leaves dirty map unchanged");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_acked_generation, 0,
                           "zero ack does not advance generation");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_dirty_pacing_timeout_suspend_and_ack_recovery(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_dirty_snapshot(pixels, 80, 1);
  const char *reason = NULL;
  UINT64 sent_ts = 0;
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.next_frame_id = 50;

  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(&server, &viewer,
                                                               &snapshot),
                         "dirty send for pacing");
  sent_ts = viewer.gfx.dirty_frame_sent_ts[0];
  ok = ok && expect_true(sent_ts != 0, "dirty timestamp captured");
  ok = ok && expect_uint32(
                 viewer_gfx_pipeline_poll_dirty_pacing(
                     &viewer, sent_ts + VIEWER_GFX_DIRTY_ACK_TIMEOUT_MS - 1U,
                     &reason),
                 VIEWER_GFX_DIRTY_PACING_OK, "before timeout remains ok");
  ok = ok &&
       expect_uint32(
           viewer_gfx_pipeline_poll_dirty_pacing(
               &viewer, sent_ts + VIEWER_GFX_DIRTY_ACK_TIMEOUT_MS, &reason),
           VIEWER_GFX_DIRTY_PACING_SUSPENDED, "timeout suspends dirty pacing");
  ok = ok && expect_true(viewer.gfx.dirty_suspended_for_no_ack,
                         "dirty suspended flag set");
  snapshot.generation = 81;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "eligibility denied while suspended");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 999),
                           CHANNEL_RC_OK, "stale ack accepted");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 0,
                           "stale ack does not record last ack");
  ok = ok && expect_true(viewer.gfx.dirty_suspended_for_no_ack,
                         "stale ack does not unsuspend");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 50),
                           CHANNEL_RC_OK, "matching suspended ack accepted");
  ok = ok && expect_true(!viewer.gfx.dirty_suspended_for_no_ack,
                         "matching ack clears suspension");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "matching ack clears in-flight");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_frame_ack_suspend_policy_is_per_viewer_and_reset_clears(void) {
  ViewerServer server = {0};
  Viewer viewer_a = {0};
  Viewer viewer_b = {0};
  RdpgfxServerContext rdpgfx_a = {0};
  RdpgfxServerContext rdpgfx_b = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot dirty_a = make_dirty_snapshot(pixels, 82, 1);
  ViewerFramebufferSnapshot dirty_b = make_dirty_snapshot(pixels, 83, 1);
  RDPGFX_FRAME_ACKNOWLEDGE_PDU suspend_ack = {0};
  RDPGFX_FRAME_ACKNOWLEDGE_PDU resume_ack = {0};
  const char *reason = NULL;
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer_a, 4, 4), "viewer A init");
  ok = ok && expect_true(init_test_viewer(&viewer_b, 4, 4), "viewer B init");
  init_test_rdpgfx(&rdpgfx_a);
  init_test_rdpgfx(&rdpgfx_b);
  configure_dirty_eligible_viewer(&server, &viewer_a, &rdpgfx_a);
  configure_dirty_eligible_viewer(&server, &viewer_b, &rdpgfx_b);
  viewer_a.gfx.next_frame_id = 60;
  viewer_b.gfx.next_frame_id = 70;

  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(
                             &server, &viewer_a, &dirty_a),
                         "viewer A dirty sends");
  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(
                             &server, &viewer_b, &dirty_b),
                         "viewer B dirty sends");

  suspend_ack.queueDepth = SUSPEND_FRAME_ACKNOWLEDGEMENT;
  suspend_ack.frameId = 60;
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack_pdu(
                               &viewer_a, &suspend_ack),
                           CHANNEL_RC_OK, "suspend ack accepted");
  ok = ok && expect_true(viewer_a.gfx.dirty_acknowledgements_suspended,
                         "viewer A records ack suspension");
  ok = ok && expect_true(viewer_a.gfx.dirty_suspended_for_no_ack,
                         "viewer A dirty suspended after suspend request");
  ok = ok && expect_uint32(viewer_a.gfx.dirty_in_flight_frames, 0,
                           "suspend ack still releases matching frame");
  dirty_a.generation = 84;
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer_a, &dirty_a, &reason),
                         "viewer A denied while client suspended ACKs");
  ok = ok && expect_true(!viewer_b.gfx.dirty_acknowledgements_suspended,
                         "viewer B does not inherit ACK suspension");
  ok =
      ok && expect_uint32(viewer_gfx_pipeline_poll_dirty_pacing(
                              &viewer_b, platform_get_timestamp_ms(), &reason),
                          VIEWER_GFX_DIRTY_PACING_OK,
                          "viewer B pacing remains independent before timeout");

  resume_ack.queueDepth = 1;
  resume_ack.frameId = 0;
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack_pdu(
                               &viewer_a, &resume_ack),
                           CHANNEL_RC_OK, "resume ack accepted");
  ok = ok && expect_true(!viewer_a.gfx.dirty_acknowledgements_suspended,
                         "nonzero queue depth clears ack suspension");
  ok = ok &&
       expect_true(!viewer_a.gfx.dirty_suspended_for_no_ack,
                   "resume clears no-ack suspension when no frames remain");

  suspend_ack.frameId = 0;
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack_pdu(
                               &viewer_a, &suspend_ack),
                           CHANNEL_RC_OK, "second suspend accepted");
  viewer_gfx_pipeline_reset_dirty_state_locked(&viewer_a.gfx);
  ok = ok && expect_true(!viewer_a.gfx.dirty_acknowledgements_suspended,
                         "reset clears ack suspension");
  ok = ok && expect_true(!viewer_a.gfx.dirty_suspended_for_no_ack,
                         "reset clears dirty suspension");

  viewer_a.gfx.rdpgfx = NULL;
  viewer_b.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer_a);
  uninit_test_viewer(&viewer_b);
  return ok;
}

static int test_dirty_pacing_baseline_reset_and_per_viewer_isolation(void) {
  ViewerServer server = {0};
  Viewer viewer_a = {0};
  Viewer viewer_b = {0};
  RdpgfxServerContext rdpgfx_a = {0};
  RdpgfxServerContext rdpgfx_b = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot dirty = make_dirty_snapshot(pixels, 90, 1);
  ViewerFramebufferSnapshot baseline = make_dirty_snapshot(pixels, 91, 1);
  UINT64 sent_ts = 0;
  const char *reason = NULL;
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer_a, 4, 4), "viewer A init");
  ok = ok && expect_true(init_test_viewer(&viewer_b, 4, 4), "viewer B init");
  init_test_rdpgfx(&rdpgfx_a);
  init_test_rdpgfx(&rdpgfx_b);
  configure_dirty_eligible_viewer(&server, &viewer_a, &rdpgfx_a);
  configure_dirty_eligible_viewer(&server, &viewer_b, &rdpgfx_b);
  viewer_a.gfx.next_frame_id = 60;
  viewer_b.gfx.next_frame_id = 70;

  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(
                             &server, &viewer_a, &dirty),
                         "viewer A dirty send");
  sent_ts = viewer_a.gfx.dirty_frame_sent_ts[0];
  ok = ok &&
       expect_uint32(
           viewer_gfx_pipeline_poll_dirty_pacing(
               &viewer_a, sent_ts + VIEWER_GFX_DIRTY_ACK_TIMEOUT_MS, &reason),
           VIEWER_GFX_DIRTY_PACING_SUSPENDED, "viewer A timeout suspends");
  ok = ok &&
       expect_uint32(
           viewer_gfx_pipeline_poll_dirty_pacing(
               &viewer_b, sent_ts + VIEWER_GFX_DIRTY_ACK_TIMEOUT_MS, &reason),
           VIEWER_GFX_DIRTY_PACING_OK, "viewer B pacing unaffected");

  reset_send_recorder();
  ok = ok && expect_true(viewer_gfx_pipeline_send_snapshot(&server, &viewer_a,
                                                           &baseline),
                         "baseline clears suspended pacing");
  ok = ok && expect_true(!viewer_a.gfx.dirty_suspended_for_no_ack,
                         "baseline clears suspension");
  ok = ok && expect_uint32(viewer_a.gfx.dirty_in_flight_frames, 0,
                           "baseline clears timing map");
  viewer_a.gfx.dirty_suspended_for_no_ack = TRUE;
  viewer_a.gfx.dirty_frame_sent_ts[0] = sent_ts;
  viewer_a.gfx.dirty_frame_valid[0] = TRUE;
  viewer_a.gfx.dirty_in_flight_frames = 1;
  viewer_gfx_pipeline_reset_dirty_state_locked(&viewer_a.gfx);
  ok = ok && expect_true(!viewer_a.gfx.dirty_suspended_for_no_ack,
                         "reset clears suspension");
  ok = ok && expect_uint64(viewer_a.gfx.dirty_frame_sent_ts[0], 0,
                           "reset clears sent timestamp");

  viewer_a.gfx.rdpgfx = NULL;
  viewer_b.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer_a);
  uninit_test_viewer(&viewer_b);
  return ok;
}

static int test_dirty_update_multi_rect_and_failures(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_dirty_snapshot(pixels, 40, 2);
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.next_frame_id = 20;

  reset_send_recorder();
  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(&server, &viewer,
                                                               &snapshot),
                         "multi dirty update sends");
  ok = ok && expect_uint32(g_send_count, 4, "start two surfaces end");
  ok = ok && expect_uint32(g_surface_count, 2, "two dirty surface commands");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 20),
                           CHANNEL_RC_OK, "multi dirty ack accepted");

  snapshot.generation = 41;
  for (UINT32 fail = 1; fail <= 4; fail++) {
    reset_send_recorder();
    g_fail_on_send = fail;
    ok = ok && expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                                 &server, &viewer, &snapshot),
                             VIEWER_GFX_DIRTY_SEND_FAILED,
                             "dirty send failure status is failed");
    reset_send_recorder();
    g_fail_on_send = fail;
    ok = ok && expect_true(!viewer_gfx_pipeline_send_dirty_update(
                               &server, &viewer, &snapshot),
                           "dirty send failure rejected");
    ok = ok && expect_uint64(viewer.gfx.dirty_last_sent_generation, 40,
                             "failed dirty send does not advance generation");
    ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                             "failed dirty send does not increment in-flight");
  }

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_dirty_mapping_cleared_by_baseline(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot dirty = make_dirty_snapshot(pixels, 50, 1);
  ViewerFramebufferSnapshot baseline = make_dirty_snapshot(pixels, 60, 1);
  UINT64 epoch = 0;
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.next_frame_id = 30;
  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(&server, &viewer,
                                                               &dirty),
                         "dirty before baseline sends");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 1,
                           "dirty in flight before baseline");
  ok = ok && expect_uint64(viewer.gfx.dirty_in_flight_bytes, 4,
                           "dirty bytes before baseline");
  epoch = viewer.gfx.frame_epoch;

  reset_send_recorder();
  ok = ok && expect_true(
                 viewer_gfx_pipeline_send_snapshot(&server, &viewer, &baseline),
                 "baseline sends and clears dirty mapping");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "baseline clears dirty in-flight");
  ok = ok && expect_uint64(viewer.gfx.dirty_in_flight_bytes, 0,
                           "baseline clears dirty bytes");
  ok = ok && expect_uint64(viewer.gfx.frame_epoch, epoch + 1U,
                           "baseline increments epoch");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 30),
                           CHANNEL_RC_OK, "pre-baseline stale ack accepted");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 0,
                           "pre-baseline stale ack ignored for last ack");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_acked_generation, 0,
                           "stale pre-baseline dirty ack ignored");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_dirty_mapping_cleared_by_reset(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot dirty = make_dirty_snapshot(pixels, 70, 1);
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.next_frame_id = 40;
  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(&server, &viewer,
                                                               &dirty),
                         "dirty before reset sends");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 1,
                           "dirty in flight before reset");

  viewer_gfx_pipeline_reset_dirty_state_locked(&viewer.gfx);
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "reset clears dirty in-flight");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 40),
                           CHANNEL_RC_OK, "pre-reset stale ack accepted");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 0,
                           "pre-reset stale ack ignored for last ack");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_acked_generation, 0,
                           "stale pre-reset dirty ack ignored");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_stale_ack_after_invalidate_does_not_clear_or_unsuspend(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot dirty = make_dirty_snapshot(pixels, 100, 1);
  UINT64 epoch = 0;
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.next_frame_id = 90;
  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(&server, &viewer,
                                                               &dirty),
                         "dirty before invalidate sends");
  epoch = viewer.gfx.frame_epoch;
  viewer.gfx.dirty_suspended_for_no_ack = TRUE;
  viewer_gfx_pipeline_invalidate_surface_locked(&viewer.gfx);
  ok = ok && expect_uint64(viewer.gfx.frame_epoch, epoch + 1U,
                           "invalidate increments epoch");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 90),
                           CHANNEL_RC_OK, "stale invalidate ack accepted");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 0,
                           "stale invalidate ack not accepted");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_acked_generation, 0,
                           "stale invalidate ack does not advance generation");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "invalidate already cleared in-flight frames");
  ok = ok && expect_uint64(viewer.gfx.dirty_in_flight_bytes, 0,
                           "invalidate already cleared in-flight bytes");
  ok = ok && expect_true(!viewer.gfx.dirty_suspended_for_no_ack,
                         "stale invalidate ack does not need to unsuspend");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static ViewerFramebufferSnapshot make_sized_snapshot(BYTE *pixels, UINT32 width,
                                                     UINT32 height,
                                                     UINT64 generation,
                                                     UINT32 rect_count) {
  ViewerFramebufferSnapshot snapshot = {0};

  snapshot.width = width;
  snapshot.height = height;
  snapshot.stride = width * 4U;
  snapshot.pixel_format = PIXEL_FORMAT_BGRX32;
  snapshot.pixels = pixels;
  snapshot.pixel_bytes = (UINT64)snapshot.stride * height;
  snapshot.generation = generation;
  snapshot.dirty_rect_count = rect_count;
  snapshot.dirty_rects[0].left = 0;
  snapshot.dirty_rects[0].top = 0;
  snapshot.dirty_rects[0].right = 0;
  snapshot.dirty_rects[0].bottom = 0;
  return snapshot;
}

static int test_live_resize_schedules_fresh_baseline(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  ViewerGfxJoinResult result = {0};
  BYTE pixels[160] = {0};
  ViewerFramebufferSnapshot dirty = make_sized_snapshot(pixels, 6, 5, 201, 1);
  ViewerFramebufferSnapshot baseline =
      make_sized_snapshot(pixels, 6, 5, 200, 0);
  UINT64 epoch = 0;
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_join_ready_viewer(&server, &viewer);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.last_ack_frame_id = 77;
  viewer.gfx.last_ack_epoch = viewer.gfx.frame_epoch;
  viewer.gfx.last_presented_timestamp = 123456;
  epoch = viewer.gfx.frame_epoch;

  viewer_gfx_pipeline_invalidate_surface_locked(&viewer.gfx);
  ok = ok && expect_uint64(viewer.gfx.frame_epoch, epoch + 1U,
                           "live resize increments epoch");
  ok = ok &&
       expect_true(!viewer.gfx.surface_created, "live resize clears surface");
  ok = ok && expect_true(!viewer.gfx.dirty_updates_enabled,
                         "live resize disables dirty");
  ok = ok && expect_true(viewer.gfx.dirty_baseline_required,
                         "live resize requires baseline");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 0,
                           "live resize clears accepted ack");
  ok = ok && expect_uint64(viewer.gfx.last_presented_timestamp, 0,
                           "live resize clears presentation timestamp");

  viewer_gfx_pipeline_step_join(&server, &viewer, 1000, &result);
  ok = ok && expect_uint32(result.actions, VIEWER_GFX_JOIN_ACTION_SEND_BASELINE,
                           "live resize schedules baseline");
  reset_send_recorder();
  ok = ok && expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                               &server, &viewer, &dirty),
                           VIEWER_GFX_DIRTY_SEND_DEFERRED,
                           "dirty before resize baseline deferred");
  ok = ok && expect_uint32(g_send_count, 0, "deferred dirty sends nothing");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "deferred dirty records no frame");

  reset_send_recorder();
  ok = ok && expect_true(
                 viewer_gfx_pipeline_send_snapshot(&server, &viewer, &baseline),
                 "resize baseline sends");
  viewer_gfx_pipeline_on_baseline_result(&viewer, 1001, TRUE, &result);
  ok = ok && expect_uint32(g_last_reset.width, 6,
                           "resize baseline uses new snapshot width");
  ok = ok && expect_uint32(g_last_reset.height, 5,
                           "resize baseline uses new snapshot height");
  ok = ok && expect_true(!viewer.gfx.dirty_baseline_required,
                         "baseline clears requirement");
  ok = ok &&
       expect_true(viewer.gfx.dirty_updates_enabled, "baseline enables dirty");
  ok = ok && expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                               &server, &viewer, &dirty),
                           VIEWER_GFX_DIRTY_SEND_SENT,
                           "dirty after resize baseline sends");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_resize_with_inflight_dirty_ignores_old_ack(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE old_pixels[64] = {0};
  BYTE new_pixels[160] = {0};
  ViewerFramebufferSnapshot old_dirty = make_dirty_snapshot(old_pixels, 210, 1);
  ViewerFramebufferSnapshot new_baseline =
      make_sized_snapshot(new_pixels, 6, 5, 211, 0);
  ViewerFramebufferSnapshot new_dirty =
      make_sized_snapshot(new_pixels, 6, 5, 212, 1);
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.next_frame_id = 90;
  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(&server, &viewer,
                                                               &old_dirty),
                         "dirty before resize sends");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 1,
                           "dirty in flight before resize");

  viewer_gfx_pipeline_invalidate_surface_locked(&viewer.gfx);
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "resize clears frames");
  ok = ok && expect_uint64(viewer.gfx.dirty_in_flight_bytes, 0,
                           "resize clears bytes");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 90),
                           CHANNEL_RC_OK, "old resize ack accepted");
  ok = ok &&
       expect_uint32(viewer.gfx.last_ack_frame_id, 0, "old resize ack ignored");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_acked_generation, 0,
                           "old resize ack does not advance generation");
  ok = ok && expect_true(!viewer.gfx.dirty_suspended_for_no_ack,
                         "old resize ack does not unsuspend");
  ok = ok && expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                               &server, &viewer, &new_dirty),
                           VIEWER_GFX_DIRTY_SEND_DEFERRED,
                           "dirty before in-flight resize baseline deferred");

  reset_send_recorder();
  ok = ok && expect_true(viewer_gfx_pipeline_send_snapshot(&server, &viewer,
                                                           &new_baseline),
                         "in-flight resize baseline sends");
  ok = ok &&
       expect_uint32(g_last_create.width, 6, "in-flight resize baseline width");
  ok = ok && expect_uint32(g_last_create.height, 5,
                           "in-flight resize baseline height");
  ok = ok && expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                               &server, &viewer, &new_dirty),
                           VIEWER_GFX_DIRTY_SEND_SENT,
                           "dirty after in-flight resize baseline sends");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_late_join_resize_uses_latest_canonical_snapshot(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  ViewerGfxJoinResult result = {0};
  BYTE pixels[192] = {0};
  ViewerFramebufferSnapshot latest = make_sized_snapshot(pixels, 8, 6, 300, 0);
  ViewerFramebufferSnapshot dirty = make_sized_snapshot(pixels, 8, 6, 301, 1);
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_join_ready_viewer(&server, &viewer);
  viewer.gfx.rdpgfx = &rdpgfx;
  viewer.gfx.join_state = VIEWER_JOIN_STATE_PENDING;
  viewer.gfx.surface_width = 4;
  viewer.gfx.surface_height = 4;
  viewer.gfx.surface_created = TRUE;
  viewer.gfx.dirty_updates_enabled = FALSE;
  viewer_gfx_pipeline_invalidate_surface_locked(&viewer.gfx);

  viewer_gfx_pipeline_step_join(&server, &viewer, 2000, &result);
  ok = ok && expect_uint32(result.actions, VIEWER_GFX_JOIN_ACTION_SEND_BASELINE,
                           "pending resize join schedules baseline");
  ok = ok && expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                               &server, &viewer, &dirty),
                           VIEWER_GFX_DIRTY_SEND_DEFERRED,
                           "late join resize dirty deferred before baseline");
  reset_send_recorder();
  ok = ok &&
       expect_true(viewer_gfx_pipeline_send_snapshot(&server, &viewer, &latest),
                   "late join latest baseline sends");
  ok = ok && expect_uint32(g_last_reset.width, 8,
                           "late join baseline uses latest width");
  ok = ok && expect_uint32(g_last_reset.height, 6,
                           "late join baseline uses latest height");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_dimension_mismatch_dirty_deferred_without_frame(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[160] = {0};
  ViewerFramebufferSnapshot mismatch =
      make_sized_snapshot(pixels, 6, 5, 400, 1);
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  reset_send_recorder();
  ok = ok && expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                               &server, &viewer, &mismatch),
                           VIEWER_GFX_DIRTY_SEND_DEFERRED,
                           "dimension mismatch dirty deferred");
  ok = ok && expect_uint32(g_send_count, 0, "dimension mismatch sends nothing");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "dimension mismatch records no frame");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_dirty_byte_backpressure_and_ack_release(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot dirty = make_dirty_snapshot(pixels, 110, 1);
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.dirty_max_in_flight_frames = 2;
  viewer.gfx.dirty_max_in_flight_bytes = 4;
  viewer.gfx.next_frame_id = 120;
  ok = ok && expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                               &server, &viewer, &dirty),
                           VIEWER_GFX_DIRTY_SEND_SENT,
                           "dirty within byte limit status sent");
  ok = ok && expect_uint64(viewer.gfx.dirty_in_flight_bytes, 4,
                           "dirty bytes at limit");
  dirty.generation = 111;
  ok = ok && expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                               &server, &viewer, &dirty),
                           VIEWER_GFX_DIRTY_SEND_DEFERRED,
                           "dirty over byte limit status deferred");
  ok = ok && expect_true(!viewer_gfx_pipeline_send_dirty_update(
                             &server, &viewer, &dirty),
                         "legacy bool reports unsent for byte defer");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 1,
                           "byte denial does not record frame");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 120),
                           CHANNEL_RC_OK, "byte release ack accepted");
  ok = ok && expect_uint64(viewer.gfx.dirty_in_flight_bytes, 0,
                           "ack releases dirty bytes");
  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(&server, &viewer,
                                                               &dirty),
                         "dirty allowed after ack frees bytes");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_frame_id_wrap_skips_zero_and_starts_epoch(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot dirty = make_dirty_snapshot(pixels, 120, 1);
  UINT64 epoch = 0;
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.frame_epoch = 7;
  viewer.gfx.next_frame_id = UINT32_MAX;
  epoch = viewer.gfx.frame_epoch;
  ok = ok && expect_true(viewer_gfx_pipeline_send_dirty_update(&server, &viewer,
                                                               &dirty),
                         "wrap boundary dirty sends");
  ok = ok && expect_uint32(viewer.gfx.last_sent_frame_id, UINT32_MAX,
                           "UINT32_MAX frame can be sent before wrap");
  ok = ok &&
       expect_uint32(viewer.gfx.next_frame_id, 1, "wrap next frame skips zero");
  ok = ok && expect_uint64(viewer.gfx.frame_epoch, epoch + 1U,
                           "wrap starts new epoch");
  ok = ok && expect_uint64(viewer.gfx.dirty_frame_epochs[0],
                           viewer.gfx.frame_epoch, "wrap frame records epoch");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 0),
                           CHANNEL_RC_OK, "zero ack still accepted at wrap");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 1,
                           "zero ack does not clear wrap frame");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_pending_dirty_accumulates_and_moves_latest_generation(void) {
  Viewer viewer = {0};
  ViewerGfxPendingDirtyBatch batch = {0};
  RECTANGLE_16 first = {0, 0, 9, 9};
  RECTANGLE_16 second = {20, 20, 29, 29};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 100, 100), "viewer init");
  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &first, 1, FALSE, 10, 100, 100),
                         "add first pending dirty");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &second, 1, FALSE, 11, 100, 100),
                         "add second pending dirty");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "move pending dirty");
  ok = ok && expect_uint64(batch.start_generation, 10,
                           "pending start generation preserved");
  ok = ok && expect_uint64(batch.latest_generation, 11,
                           "pending latest generation preserved");
  ok = ok &&
       expect_uint32(batch.rect_count, 2, "pending move returns both rects");
  ok = ok && expect_uint64(batch.update_count, 2,
                           "pending move records update count");
  ok = ok && expect_true(!batch.full_frame, "pending move not full frame");
  ok = ok && expect_true(!viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "pending move clears accumulator");
  LeaveCriticalSection(&viewer.gfx.lock);
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_pending_dirty_overflow_forces_full_frame(void) {
  Viewer viewer = {0};
  ViewerGfxPendingDirtyBatch batch = {0};
  RECTANGLE_16 rect = {0, 0, 0, 0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 100, 100), "viewer init");
  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &rect, 1, TRUE, 20, 100, 100),
                         "overflow pending dirty accepted");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "move overflow pending dirty");
  LeaveCriticalSection(&viewer.gfx.lock);

  ok = ok && expect_true(batch.full_frame, "overflow becomes full frame");
  ok = ok && expect_true(batch.overflow, "overflow metadata preserved");
  ok = ok &&
       expect_uint32(batch.rect_count, 1, "overflow full frame has one rect");
  ok = ok && expect_uint32(batch.rects[0].right, 99,
                           "overflow full frame right edge");
  ok = ok && expect_uint32(batch.rects[0].bottom, 99,
                           "overflow full frame bottom edge");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_pending_dirty_region_union_and_thresholds(void) {
  Viewer viewer = {0};
  ViewerGfxPendingDirtyBatch batch = {0};
  RECTANGLE_16 overlap[2] = {{0, 0, 9, 9}, {5, 5, 14, 14}};
  RECTANGLE_16 adjacent[2] = {{0, 0, 9, 9}, {10, 0, 19, 9}};
  RECTANGLE_16 separate[2] = {{0, 0, 9, 9}, {20, 20, 29, 29}};
  RECTANGLE_16 area_rect = {0, 0, 60, 99};
  RECTANGLE_16 sparse[VIEWER_GFX_PENDING_DIRTY_MAX_RECTS + 1U] = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 1000, 1000), "viewer init");

  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, overlap, 2, FALSE, 60, 1000, 1000),
                         "overlapping dirty rects accepted");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "move overlapping dirty rects");
  LeaveCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_uint32(batch.rect_count, 1, "overlap merged to one rect");
  ok = ok && expect_uint32(batch.rects[0].left, 0, "overlap left");
  ok = ok && expect_uint32(batch.rects[0].top, 0, "overlap top");
  ok = ok && expect_uint32(batch.rects[0].right, 14, "overlap right");
  ok = ok && expect_uint32(batch.rects[0].bottom, 14, "overlap bottom");

  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, adjacent, 2, FALSE, 61, 1000, 1000),
                         "edge-adjacent dirty rects accepted");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "move edge-adjacent dirty rects");
  LeaveCriticalSection(&viewer.gfx.lock);
  ok = ok &&
       expect_uint32(batch.rect_count, 1, "edge-adjacent merged to one rect");
  ok = ok && expect_uint32(batch.rects[0].left, 0, "edge-adjacent left");
  ok = ok && expect_uint32(batch.rects[0].top, 0, "edge-adjacent top");
  ok = ok && expect_uint32(batch.rects[0].right, 19, "edge-adjacent right");
  ok = ok && expect_uint32(batch.rects[0].bottom, 9, "edge-adjacent bottom");

  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, separate, 2, FALSE, 62, 1000, 1000),
                         "non-overlapping dirty rects accepted");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "move non-overlapping dirty rects");
  LeaveCriticalSection(&viewer.gfx.lock);
  ok = ok &&
       expect_uint32(batch.rect_count, 2, "non-overlapping rects preserved");
  ok = ok && expect_true(!batch.full_frame,
                         "non-overlapping rects do not force full frame");

  for (UINT32 i = 0; i < (VIEWER_GFX_PENDING_DIRTY_MAX_RECTS + 1U); i++) {
    sparse[i].left = (UINT16)(i * 2U);
    sparse[i].top = 0;
    sparse[i].right = sparse[i].left;
    sparse[i].bottom = 0;
  }
  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, sparse,
                             VIEWER_GFX_PENDING_DIRTY_MAX_RECTS + 1U, FALSE, 63,
                             1000, 1000),
                         "129 sparse dirty rects accepted");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "move 129 sparse dirty rects");
  LeaveCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(batch.full_frame, "129 sparse rects force full frame");
  ok = ok && expect_true(batch.overflow,
                         "129 sparse rects preserve overflow metadata");
  ok = ok && expect_uint32(batch.rect_count, 1,
                           "129 sparse rects produce one full-frame rect");
  ok = ok && expect_uint32(batch.rects[0].left, 0, "129 full-frame left");
  ok = ok && expect_uint32(batch.rects[0].top, 0, "129 full-frame top");
  ok = ok && expect_uint32(batch.rects[0].right, 999, "129 full-frame right");
  ok = ok && expect_uint32(batch.rects[0].bottom, 999, "129 full-frame bottom");

  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &area_rect, 1, FALSE, 64, 100, 100),
                         "greater-than-60-percent dirty rect accepted");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "move greater-than-60-percent dirty rect");
  LeaveCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(batch.full_frame, ">60 percent forces full frame");
  ok = ok && expect_uint32(batch.rect_count, 1,
                           ">60 percent produces one full-frame rect");
  ok = ok &&
       expect_uint32(batch.rects[0].right, 99, ">60 percent full-frame right");
  ok = ok && expect_uint32(batch.rects[0].bottom, 99,
                           ">60 percent full-frame bottom");

  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, NULL, 0, FALSE, 65, 100, 100),
                         "empty dirty input accepted as full frame");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "move empty dirty input full frame");
  LeaveCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(batch.full_frame, "empty input forces full frame");
  ok = ok && expect_uint32(batch.rect_count, 1,
                           "empty input produces one full-frame rect");
  ok = ok && expect_uint32(batch.rects[0].left, 0, "empty full-frame left");
  ok = ok && expect_uint32(batch.rects[0].top, 0, "empty full-frame top");
  ok = ok && expect_uint32(batch.rects[0].right, 99, "empty full-frame right");
  ok =
      ok && expect_uint32(batch.rects[0].bottom, 99, "empty full-frame bottom");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_pending_dirty_stale_generation_ignored(void) {
  Viewer viewer = {0};
  ViewerGfxPendingDirtyBatch batch = {0};
  RECTANGLE_16 rect = {0, 0, 9, 9};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 100, 100), "viewer init");
  EnterCriticalSection(&viewer.gfx.lock);
  viewer.gfx.dirty_last_sent_generation = 20;
  ok = ok && expect_true(!viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &rect, 1, FALSE, 20, 100, 100),
                         "equal generation ignored");
  ok = ok && expect_true(!viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &rect, 1, FALSE, 19, 100, 100),
                         "older generation ignored");
  ok = ok && expect_true(!viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "stale generations create no pending dirty");
  LeaveCriticalSection(&viewer.gfx.lock);

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_pending_dirty_empty_move_behavior(void) {
  Viewer viewer = {0};
  ViewerGfxPendingDirtyBatch batch = {0};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 100, 100), "viewer init");
  batch.latest_generation = 123;
  batch.rect_count = 9;
  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(!viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "empty pending move returns false");
  LeaveCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_uint64(batch.latest_generation, 0,
                           "empty pending move clears output batch");
  ok = ok && expect_uint32(batch.rect_count, 0,
                           "empty pending move clears output rect count");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_pending_dirty_remerge_preserves_new_updates(void) {
  Viewer viewer = {0};
  ViewerGfxPendingDirtyBatch moved = {0};
  ViewerGfxPendingDirtyBatch final_batch = {0};
  RECTANGLE_16 first = {0, 0, 9, 9};
  RECTANGLE_16 second = {40, 40, 49, 49};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 100, 100), "viewer init");
  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &first, 1, FALSE, 30, 100, 100),
                         "add pending before move");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &moved),
                         "move pending before deferred send");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &second, 1, FALSE, 31, 100, 100),
                         "add new pending while batch moved");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_remerge_locked(
                             &viewer.gfx, &moved, moved.width, moved.height),
                         "remerge moved pending batch");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &final_batch),
                         "move final pending dirty");
  LeaveCriticalSection(&viewer.gfx.lock);

  ok = ok && expect_uint64(final_batch.latest_generation, 31,
                           "remerge keeps latest new generation");
  ok = ok && expect_uint64(final_batch.start_generation, 30,
                           "remerge restores moved start generation");
  ok = ok && expect_uint64(final_batch.update_count, 2,
                           "remerge combines update counts");
  ok = ok && expect_uint32(final_batch.rect_count, 2,
                           "remerge preserves moved and new rects");
  ok = ok && expect_uint32(final_batch.rects[0].left, second.left,
                           "remerge preserves newer rect left");
  ok = ok && expect_uint32(final_batch.rects[0].bottom, second.bottom,
                           "remerge preserves newer rect bottom");
  ok = ok && expect_uint32(final_batch.rects[1].left, first.left,
                           "remerge preserves moved rect left");
  ok = ok && expect_uint32(final_batch.rects[1].bottom, first.bottom,
                           "remerge preserves moved rect bottom");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_pending_dirty_remerge_full_frame_with_newer_update(void) {
  Viewer viewer = {0};
  ViewerGfxPendingDirtyBatch moved = {0};
  ViewerGfxPendingDirtyBatch final_batch = {0};
  RECTANGLE_16 full = {0, 0, 99, 99};
  RECTANGLE_16 newer = {40, 40, 49, 49};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 100, 100), "viewer init");
  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &full, 1, FALSE, 40, 100, 100),
                         "add moved full-frame-equivalent batch");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &moved),
                         "move full-frame batch");
  ok = ok && expect_true(moved.full_frame, "moved batch is full frame");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &newer, 1, FALSE, 41, 100, 100),
                         "add newer small pending while full-frame moved");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_remerge_locked(
                             &viewer.gfx, &moved, moved.width, moved.height),
                         "remerge moved full-frame batch");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &final_batch),
                         "move final full-frame batch");
  LeaveCriticalSection(&viewer.gfx.lock);

  ok = ok && expect_true(final_batch.full_frame,
                         "remerged full-frame remains full frame");
  ok = ok && expect_uint64(final_batch.latest_generation, 41,
                           "full-frame remerge keeps newer generation");
  ok = ok && expect_uint64(final_batch.start_generation, 40,
                           "full-frame remerge restores moved start");
  ok = ok && expect_uint64(final_batch.update_count, 2,
                           "full-frame remerge combines update counts");
  ok = ok && expect_uint32(final_batch.rect_count, 1,
                           "full-frame remerge has one rect");
  ok = ok &&
       expect_uint32(final_batch.rects[0].left, 0, "full-frame remerge left");
  ok = ok &&
       expect_uint32(final_batch.rects[0].top, 0, "full-frame remerge top");
  ok = ok && expect_uint32(final_batch.rects[0].right, 99,
                           "full-frame remerge right");
  ok = ok && expect_uint32(final_batch.rects[0].bottom, 99,
                           "full-frame remerge bottom");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_pending_dirty_denied_send_remerge_simulation(void) {
  Viewer viewer = {0};
  ViewerGfxPendingDirtyBatch moved = {0};
  ViewerGfxPendingDirtyBatch final_batch = {0};
  RECTANGLE_16 moved_rect = {0, 0, 9, 9};
  RECTANGLE_16 newer_rect = {30, 30, 39, 39};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 100, 100), "viewer init");
  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &moved_rect, 1, FALSE, 50, 100, 100),
                         "add moved denied-send batch");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &moved),
                         "move denied-send batch");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &newer_rect, 1, FALSE, 51, 100, 100),
                         "add newer pending before denied remerge");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_remerge_locked(
                             &viewer.gfx, &moved, moved.width, moved.height),
                         "denied-send path remerges moved batch");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &final_batch),
                         "move denied-send final pending");
  LeaveCriticalSection(&viewer.gfx.lock);

  ok = ok && expect_uint64(final_batch.latest_generation, 51,
                           "denied remerge keeps newest generation");
  ok = ok && expect_uint64(final_batch.start_generation, 50,
                           "denied remerge restores moved start");
  ok = ok && expect_uint64(final_batch.update_count, 2,
                           "denied remerge combines update counts");
  ok = ok && expect_uint32(final_batch.rect_count, 2,
                           "denied remerge preserves both rects");
  ok = ok && expect_uint32(final_batch.rects[0].left, newer_rect.left,
                           "denied remerge keeps existing newer rect first");
  ok = ok && expect_uint32(final_batch.rects[0].bottom, newer_rect.bottom,
                           "denied remerge preserves newer rect bottom");
  ok = ok && expect_uint32(final_batch.rects[1].left, moved_rect.left,
                           "denied remerge appends moved rect");
  ok = ok && expect_uint32(final_batch.rects[1].bottom, moved_rect.bottom,
                           "denied remerge preserves moved rect bottom");

  uninit_test_viewer(&viewer);
  return ok;
}

static int test_pending_dirty_snapshot_overrides_generation(void) {
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerGfxPendingDirtyBatch batch = {0};
  int ok = 1;

  snapshot.width = 100;
  snapshot.height = 100;
  snapshot.generation = 99;
  batch.latest_generation = 40;
  batch.rect_count = 1;
  batch.rects[0].left = 1;
  batch.rects[0].top = 2;
  batch.rects[0].right = 3;
  batch.rects[0].bottom = 4;

  ok = ok && expect_true(viewer_gfx_pipeline_snapshot_apply_pending_dirty(
                             &snapshot, &batch),
                         "apply pending dirty to snapshot");
  ok = ok && expect_uint64(snapshot.generation, 40,
                           "pending latest generation overrides snapshot");
  ok = ok && expect_uint32(snapshot.dirty_rect_count, 1,
                           "pending rect count overrides snapshot");
  ok = ok && expect_uint32(snapshot.dirty_rects[0].left, 1,
                           "pending rect copied to snapshot");

  return ok;
}

static int test_dirty_update_command_bounds_validation(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  RdpgfxServerContext rdpgfx = {0};
  BYTE pixels[64] = {0};
  ViewerFramebufferSnapshot snapshot = make_dirty_snapshot(pixels, 130, 1);
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 4, 4), "viewer init");
  init_test_rdpgfx(&rdpgfx);
  configure_dirty_eligible_viewer(&server, &viewer, &rdpgfx);
  viewer.gfx.next_frame_id = 21;
  viewer.gfx.dirty_last_sent_generation = 77;

  snapshot.dirty_rects[0].left = 3;
  snapshot.dirty_rects[0].top = 3;
  snapshot.dirty_rects[0].right = 3;
  snapshot.dirty_rects[0].bottom = 3;
  reset_send_recorder();
  ok = ok &&
       expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                         &server, &viewer, &snapshot),
                     VIEWER_GFX_DIRTY_SEND_SENT, "one-pixel edge dirty sends");
  ok = ok && expect_uint32(g_last_surface.left, 3, "one-pixel dirty left");
  ok = ok && expect_uint32(g_last_surface.top, 3, "one-pixel dirty top");
  ok = ok && expect_uint32(g_last_surface.right, 4,
                           "one-pixel dirty exclusive right");
  ok = ok && expect_uint32(g_last_surface.bottom, 4,
                           "one-pixel dirty exclusive bottom");
  ok = ok && expect_uint32(g_last_surface.width, 1, "one-pixel dirty width");
  ok = ok && expect_uint32(g_last_surface.height, 1, "one-pixel dirty height");

  (void)viewer_gfx_pipeline_handle_frame_ack(&viewer, 21);
  snapshot.generation = 131;
  snapshot.dirty_rects[0].left = 0;
  snapshot.dirty_rects[0].top = 0;
  snapshot.dirty_rects[0].right = 3;
  snapshot.dirty_rects[0].bottom = 3;
  reset_send_recorder();
  ok = ok &&
       expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                         &server, &viewer, &snapshot),
                     VIEWER_GFX_DIRTY_SEND_SENT, "full bounds dirty sends");
  ok = ok && expect_uint32(g_last_surface.left, 0, "full dirty left");
  ok = ok && expect_uint32(g_last_surface.top, 0, "full dirty top");
  ok = ok &&
       expect_uint32(g_last_surface.right, 4, "full dirty exclusive right");
  ok = ok &&
       expect_uint32(g_last_surface.bottom, 4, "full dirty exclusive bottom");
  ok = ok && expect_uint32(g_last_surface.width, 4, "full dirty width");
  ok = ok && expect_uint32(g_last_surface.height, 4, "full dirty height");

  (void)viewer_gfx_pipeline_handle_frame_ack(&viewer, 22);
  snapshot.generation = 132;
  snapshot.dirty_rects[0].left = 2;
  snapshot.dirty_rects[0].top = 0;
  snapshot.dirty_rects[0].right = 1;
  snapshot.dirty_rects[0].bottom = 0;
  reset_send_recorder();
  ok = ok && expect_uint32(viewer_gfx_pipeline_send_dirty_update_result(
                               &server, &viewer, &snapshot),
                           VIEWER_GFX_DIRTY_SEND_FAILED,
                           "invalid dirty rect build fails");
  ok = ok &&
       expect_uint32(g_send_count, 0, "invalid dirty rect sends no frame PDUs");
  ok = ok && expect_uint32(g_surface_count, 0,
                           "invalid dirty rect sends no surface command");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "invalid dirty rect leaves in-flight unchanged");
  ok = ok && expect_uint32(viewer.gfx.next_frame_id, 23,
                           "invalid dirty rect leaves next frame unchanged");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_sent_generation, 131,
                           "invalid dirty rect leaves last generation");

  viewer.gfx.rdpgfx = NULL;
  uninit_test_viewer(&viewer);
  return ok;
}

static int test_pending_dirty_validation_and_clamping(void) {
  Viewer viewer = {0};
  ViewerGfxPendingDirtyBatch batch = {0};
  RECTANGLE_16 clamp_rect = {90, 90, 120, 130};
  RECTANGLE_16 reversed_x = {10, 10, 9, 10};
  RECTANGLE_16 reversed_y = {10, 10, 10, 9};
  RECTANGLE_16 left_oob = {100, 1, 100, 1};
  RECTANGLE_16 top_oob = {1, 100, 1, 100};
  RECTANGLE_16 mixed[2] = {{5, 6, 7, 8}, {200, 0, 201, 1}};
  int ok = 1;

  ok = ok && expect_true(init_test_viewer(&viewer, 100, 100), "viewer init");
  EnterCriticalSection(&viewer.gfx.lock);
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &clamp_rect, 1, FALSE, 50, 100, 100),
                         "pending dirty clamps right and bottom");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "move clamped pending dirty");
  ok = ok &&
       expect_uint32(batch.rects[0].right, 99, "pending dirty right clamped");
  ok = ok &&
       expect_uint32(batch.rects[0].bottom, 99, "pending dirty bottom clamped");

  ok = ok && expect_true(!viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &reversed_x, 1, FALSE, 51, 100, 100),
                         "pending dirty rejects reversed x");
  ok = ok && expect_true(!viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &reversed_y, 1, FALSE, 52, 100, 100),
                         "pending dirty rejects reversed y");
  ok = ok && expect_true(!viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &left_oob, 1, FALSE, 53, 100, 100),
                         "pending dirty rejects left OOB");
  ok = ok && expect_true(!viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, &top_oob, 1, FALSE, 54, 100, 100),
                         "pending dirty rejects top OOB");
  ok = ok && expect_true(!viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "invalid-only input creates no pending dirty");

  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_add_locked(
                             &viewer.gfx, mixed, 2, FALSE, 55, 100, 100),
                         "pending dirty accepts mixed valid and invalid");
  ok = ok && expect_true(viewer_gfx_pipeline_pending_dirty_move_locked(
                             &viewer.gfx, &batch),
                         "move mixed pending dirty");
  LeaveCriticalSection(&viewer.gfx.lock);

  ok = ok && expect_uint32(batch.rect_count, 1,
                           "mixed pending dirty preserves only valid rect");
  ok = ok && expect_true(!batch.full_frame,
                         "mixed pending dirty does not force full frame");
  ok = ok && expect_uint32(batch.rects[0].left, 5,
                           "mixed pending dirty valid left preserved");
  ok = ok && expect_uint32(batch.rects[0].bottom, 8,
                           "mixed pending dirty valid bottom preserved");

  uninit_test_viewer(&viewer);
  return ok;
}

int main(void) {
  if (!test_activate_rejects_null_inputs())
    return 1;
  if (!test_activation_initializes_placeholders())
    return 1;
  if (!test_repeated_activation_is_idempotent())
    return 1;
  if (!test_fallback_state_does_not_enable_rdpegfx())
    return 1;
  if (!test_reset_join_state_clears_pipeline_owned_fields())
    return 1;
  if (!test_peer_activation_sets_join_actions())
    return 1;
  if (!test_step_join_and_baseline_result_transitions())
    return 1;
  if (!test_snapshot_validation_rejects_not_ready())
    return 1;
  if (!test_snapshot_sends_full_frame_baseline_order())
    return 1;
  if (!test_snapshot_rfx_codec_emits_cavideo())
    return 1;
  if (!test_snapshot_rfx_context_failure_downgrades_to_uncompressed())
    return 1;
  if (!test_snapshot_send_failure_propagates())
    return 1;
  if (!test_dirty_update_eligibility_denials_and_allowed())
    return 1;
  if (!test_surface_invalidation_clears_pipeline_state())
    return 1;
  if (!test_dirty_update_eligibility_is_per_viewer())
    return 1;
  if (!test_dirty_update_send_order_and_ack())
    return 1;
  if (!test_dirty_update_command_bounds_validation())
    return 1;
  if (!test_dirty_update_rfx_codec_emits_cavideo())
    return 1;
  if (!test_monitor_layout_snapshot_uses_server_layout())
    return 1;
  if (!test_monitor_layout_snapshot_falls_back_to_framebuffer())
    return 1;
  if (!test_snapshot_sends_server_monitor_layout())
    return 1;
  if (!test_frame_ack_accepts_zero_without_dirty_state_change())
    return 1;
  if (!test_dirty_update_multi_rect_and_failures())
    return 1;
  if (!test_dirty_mapping_cleared_by_baseline())
    return 1;
  if (!test_dirty_mapping_cleared_by_reset())
    return 1;
  if (!test_stale_ack_after_invalidate_does_not_clear_or_unsuspend())
    return 1;
  if (!test_live_resize_schedules_fresh_baseline())
    return 1;
  if (!test_resize_with_inflight_dirty_ignores_old_ack())
    return 1;
  if (!test_late_join_resize_uses_latest_canonical_snapshot())
    return 1;
  if (!test_dimension_mismatch_dirty_deferred_without_frame())
    return 1;
  if (!test_dirty_byte_backpressure_and_ack_release())
    return 1;
  if (!test_frame_id_wrap_skips_zero_and_starts_epoch())
    return 1;
  if (!test_dirty_pacing_timeout_suspend_and_ack_recovery())
    return 1;
  if (!test_frame_ack_suspend_policy_is_per_viewer_and_reset_clears())
    return 1;
  if (!test_dirty_pacing_baseline_reset_and_per_viewer_isolation())
    return 1;
  if (!test_pending_dirty_accumulates_and_moves_latest_generation())
    return 1;
  if (!test_pending_dirty_overflow_forces_full_frame())
    return 1;
  if (!test_pending_dirty_region_union_and_thresholds())
    return 1;
  if (!test_pending_dirty_stale_generation_ignored())
    return 1;
  if (!test_pending_dirty_empty_move_behavior())
    return 1;
  if (!test_pending_dirty_remerge_preserves_new_updates())
    return 1;
  if (!test_pending_dirty_remerge_full_frame_with_newer_update())
    return 1;
  if (!test_pending_dirty_denied_send_remerge_simulation())
    return 1;
  if (!test_pending_dirty_snapshot_overrides_generation())
    return 1;
  if (!test_pending_dirty_validation_and_clamping())
    return 1;
  return 0;
}
