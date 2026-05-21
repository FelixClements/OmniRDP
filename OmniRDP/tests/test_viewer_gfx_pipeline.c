#include "viewer_gfx_pipeline.h"

#include <freerdp/codec/color.h>
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
static UINT g_fail_on_send = 0;
static RDPGFX_CREATE_SURFACE_PDU g_last_create = {0};
static RDPGFX_SURFACE_COMMAND g_last_surface = {0};
static const MONITOR_DEF *g_expected_reset_source = NULL;
static MONITOR_DEF g_last_reset_monitor = {0};

static UINT test_record_send(TestSendOp op) {
  g_send_order[g_send_count++] = op;
  if (g_fail_on_send == g_send_count)
    return ERROR_INTERNAL_ERROR;
  return CHANNEL_RC_OK;
}

static UINT test_reset_graphics(RdpgfxServerContext *context,
                                const RDPGFX_RESET_GRAPHICS_PDU *reset) {
  (void)context;
  if (!reset || (reset->monitorCount != 1) || !reset->monitorDefArray)
    return ERROR_INTERNAL_ERROR;
  if (g_expected_reset_source &&
      (reset->monitorDefArray == g_expected_reset_source))
    return ERROR_INTERNAL_ERROR;
  g_last_reset_monitor = reset->monitorDefArray[0];
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
  memset(&g_last_create, 0, sizeof(g_last_create));
  memset(&g_last_surface, 0, sizeof(g_last_surface));
  memset(&g_last_reset_monitor, 0, sizeof(g_last_reset_monitor));
  g_expected_reset_source = NULL;
  g_send_count = 0;
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
  ok = ok && expect_uint32(viewer.gfx.surface_width, 800,
                           "surface width initialized");
  ok = ok && expect_uint32(viewer.gfx.surface_height, 600,
                           "surface height initialized");
  ok = ok && expect_true(!viewer.gfx.surface_created,
                         "surface placeholder not created");
  ok = ok &&
       expect_true(viewer.gfx.force_full_present, "full present requested");
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

  viewer.gfx.rdpgfx = NULL;
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

static int test_surface_preamble_deep_copies_reset_monitors(void) {
  ViewerServer server = {0};
  Viewer viewer = {0};
  freerdp_peer peer = {0};
  RdpgfxServerContext rdpgfx = {0};
  MONITOR_DEF monitors[1] = {0};
  int ok = 1;

  monitors[0].left = 0;
  monitors[0].top = 0;
  monitors[0].right = 799;
  monitors[0].bottom = 599;
  monitors[0].flags = MONITOR_PRIMARY;

  ok = ok && expect_true(init_test_viewer(&viewer, 800, 600), "viewer init");
  ok = ok && expect_true(
                 InitializeCriticalSectionAndSpinCount(&server.gfx.lock, 4000),
                 "server gfx lock init");
  init_test_rdpgfx(&rdpgfx);
  viewer.peer = &peer;
  viewer.gfx.rdpgfx = &rdpgfx;
  viewer.gfx.use_rdpgfx = TRUE;
  server.gfx.has_latest_reset_graphics = TRUE;
  server.gfx.latest_reset_graphics.width = 800;
  server.gfx.latest_reset_graphics.height = 600;
  server.gfx.latest_reset_graphics.monitorCount = 1;
  server.gfx.latest_reset_graphics.monitorDefArray = monitors;

  reset_send_recorder();
  g_expected_reset_source = monitors;
  ok = ok &&
       expect_true(viewer_gfx_pipeline_send_surface_preamble(&server, &viewer),
                   "surface preamble sends reset");
  ok = ok && expect_uint32(g_send_count, 1, "only reset sent");
  ok = ok && expect_uint32(g_send_order[0], TEST_SEND_RESET, "reset sent");
  ok = ok && expect_uint32((UINT32)g_last_reset_monitor.right, 799,
                           "copied monitor right");
  ok = ok && expect_uint32((UINT32)g_last_reset_monitor.bottom, 599,
                           "copied monitor bottom");
  ok = ok && expect_uint32((UINT32)g_last_reset_monitor.flags, MONITOR_PRIMARY,
                           "copied monitor flags");

  DeleteCriticalSection(&server.gfx.lock);
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
  viewer->gfx.force_full_present = FALSE;
  viewer->gfx.dirty_updates_enabled = TRUE;
  viewer->gfx.dirty_baseline_required = FALSE;
  viewer->gfx.dirty_max_in_flight_frames = 2;
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

int main(void) {
  if (!test_activate_rejects_null_inputs())
    return 1;
  if (!test_activation_initializes_placeholders())
    return 1;
  if (!test_repeated_activation_is_idempotent())
    return 1;
  if (!test_fallback_state_does_not_enable_rdpegfx())
    return 1;
  if (!test_snapshot_validation_rejects_not_ready())
    return 1;
  if (!test_snapshot_sends_full_frame_baseline_order())
    return 1;
  if (!test_snapshot_send_failure_propagates())
    return 1;
  if (!test_surface_preamble_deep_copies_reset_monitors())
    return 1;
  if (!test_dirty_update_eligibility_denials_and_allowed())
    return 1;
  if (!test_dirty_update_eligibility_is_per_viewer())
    return 1;
  return 0;
}
