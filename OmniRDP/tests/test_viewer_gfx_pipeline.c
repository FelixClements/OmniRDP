#include "viewer_gfx_pipeline.h"

#include <stdio.h>
#include <string.h>

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
  viewer->gfx.initialized = TRUE;
  return TRUE;
}

static void uninit_test_viewer(Viewer *viewer) {
  if (viewer && viewer->gfx.initialized) {
    DeleteCriticalSection(&viewer->gfx.lock);
    memset(viewer, 0, sizeof(*viewer));
  }
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

static int test_snapshot_placeholder_validation_and_no_send(void) {
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
  snapshot.pixel_bytes = sizeof(pixels);
  viewer.gfx.last_sent_frame_id = 5;
  viewer.gfx.last_ack_frame_id = 4;
  ok = ok && expect_true(
                 viewer_gfx_pipeline_send_snapshot(&server, &viewer, &snapshot),
                 "snapshot placeholder accepts valid shape");
  ok = ok && expect_uint32(viewer.gfx.surface_width, 2,
                           "snapshot records intended width");
  ok = ok && expect_uint32(viewer.gfx.surface_height, 2,
                           "snapshot records intended height");
  ok = ok && expect_uint32(viewer.gfx.last_sent_frame_id, 5,
                           "snapshot does not send frame");
  ok = ok && expect_uint32(viewer.gfx.last_ack_frame_id, 4,
                           "snapshot does not ack/migrate frames");
  ok = ok && expect_true(!viewer.gfx.surface_created,
                         "snapshot does not create surface");

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
  if (!test_snapshot_placeholder_validation_and_no_send())
    return 1;
  return 0;
}
