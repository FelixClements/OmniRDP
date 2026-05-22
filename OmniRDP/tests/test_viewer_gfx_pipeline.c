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
static UINT32 g_surface_count = 0;
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
  memset(&g_last_create, 0, sizeof(g_last_create));
  memset(&g_last_surface, 0, sizeof(g_last_surface));
  memset(&g_last_reset_monitor, 0, sizeof(g_last_reset_monitor));
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
  BYTE pixels[64] = {0};
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
  ok = ok && expect_true(!viewer_gfx_pipeline_dirty_update_allowed(
                             &server, &viewer, &snapshot, &reason),
                         "in-flight limit denies when max set to one later");

  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 999),
                           CHANNEL_RC_OK, "unknown ack accepted");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 999,
                           "unknown ack records last ack");
  ok = ok && expect_true(viewer.gfx.last_presented_timestamp != 0,
                         "unknown ack records presented timestamp");
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
  ok = ok && expect_uint64(viewer.gfx.dirty_last_acked_generation, 30,
                           "matching ack advances generation");

  viewer.gfx.rdpgfx = NULL;
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
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 999,
                           "stale ack records last ack");
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

  reset_send_recorder();
  ok = ok && expect_true(
                 viewer_gfx_pipeline_send_snapshot(&server, &viewer, &baseline),
                 "baseline sends and clears dirty mapping");
  ok = ok && expect_uint32(viewer.gfx.dirty_in_flight_frames, 0,
                           "baseline clears dirty in-flight");
  ok = ok && expect_uint32(viewer_gfx_pipeline_handle_frame_ack(&viewer, 30),
                           CHANNEL_RC_OK, "pre-baseline stale ack accepted");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 30,
                           "pre-baseline stale ack records last ack");
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
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 40,
                           "pre-reset stale ack records last ack");
  ok = ok && expect_uint64(viewer.gfx.dirty_last_acked_generation, 0,
                           "stale pre-reset dirty ack ignored");

  viewer.gfx.rdpgfx = NULL;
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
  if (!test_snapshot_validation_rejects_not_ready())
    return 1;
  if (!test_snapshot_sends_full_frame_baseline_order())
    return 1;
  if (!test_snapshot_send_failure_propagates())
    return 1;
  if (!test_dirty_update_eligibility_denials_and_allowed())
    return 1;
  if (!test_dirty_update_eligibility_is_per_viewer())
    return 1;
  if (!test_dirty_update_send_order_and_ack())
    return 1;
  if (!test_frame_ack_accepts_zero_without_dirty_state_change())
    return 1;
  if (!test_dirty_update_multi_rect_and_failures())
    return 1;
  if (!test_dirty_mapping_cleared_by_baseline())
    return 1;
  if (!test_dirty_mapping_cleared_by_reset())
    return 1;
  if (!test_dirty_pacing_timeout_suspend_and_ack_recovery())
    return 1;
  if (!test_dirty_pacing_baseline_reset_and_per_viewer_isolation())
    return 1;
  return 0;
}
