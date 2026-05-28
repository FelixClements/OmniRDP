#include "backend.h"
#include "platform_compat.h"
#include "svc_log.h"
#include "viewer_auth.h"
#include "viewer_classic_transport.h"
#include "viewer_gfx_pipeline.h"
#include "viewer_internal.h"
#include "viewer_pointer.h"
#include "viewer_pointer_transport.h"
#include "viewer_server_internal.h"

#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/freerdp.h>
#include <freerdp/input.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/update.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#include <winpr/sspi.h>
#include <winpr/wlog.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define TAG "multiplexer.viewer"
#define INPUT_IDLE_TIMEOUT_MS 2500U
#define FULL_REFRESH_TIMEOUT_MS 3000U
#define VIEWER_RDPEGFX_NEGOTIATION_TIMEOUT_MS 3000U
#define VIEWER_UPDATE_ACTIVATION_GRACE_MS 250U

static ViewerServer *g_viewer_server = NULL;

static BOOL viewer_string_has_value(const char *value);

static ViewerSecurityConfig viewer_security_default(void) {
  ViewerSecurityConfig security = {0};
  security.nla_enabled = FALSE;
  security.tls_enabled = TRUE;
  security.rdp_enabled = TRUE;
  security.auth_mode = VIEWER_AUTH_MODE_NONE;
  return security;
}

static const char *viewer_auth_mode_name(ViewerAuthMode mode) {
  switch (mode) {
  case VIEWER_AUTH_MODE_BACKEND_CREDENTIALS:
    return "backend_credentials";
  case VIEWER_AUTH_MODE_NONE:
  default:
    return "none";
  }
}

static BOOL viewer_domain_matches_local_alias(const char *viewer_domain) {
#ifdef _WIN32
  char computer_name[MAX_COMPUTERNAME_LENGTH + 1] = {0};
  DWORD computer_name_len =
      (DWORD)(sizeof(computer_name) / sizeof(computer_name[0]));

  if (!viewer_string_has_value(viewer_domain) ||
      strcmp(viewer_domain, ".") == 0 ||
      _stricmp(viewer_domain, "localhost") == 0)
    return TRUE;

  if (GetComputerNameA(computer_name, &computer_name_len) &&
      _stricmp(viewer_domain, computer_name) == 0)
    return TRUE;

  return FALSE;
#else
  return !viewer_string_has_value(viewer_domain) ||
         strcmp(viewer_domain, ".") == 0 ||
         strcasecmp(viewer_domain, "localhost") == 0;
#endif
}

static BOOL viewer_gfx_enter_classic_fallback(ViewerServer *server,
                                              Viewer *viewer, UINT64 now,
                                              const char *reason);
static BOOL viewer_gfx_handle_failure(ViewerServer *server, Viewer *viewer,
                                      UINT64 now, const char *reason);
static BOOL viewer_gfx_send_framebuffer_baseline(ViewerServer *server,
                                                 Viewer *viewer, UINT64 now);
static BOOL viewer_gfx_try_send_dirty_update(ViewerServer *server,
                                             Viewer *viewer, UINT64 now);
static UINT64 viewer_perf_now_us(void);
static BOOL viewer_should_log_bitmap_perf(UINT64 batch_count, UINT64 publish_us,
                                          UINT32 send_failed_count);
static ViewerClassicTransport
viewer_classic_transport_from_viewer(Viewer *viewer);
static BOOL viewer_send_bitmap_update(Viewer *viewer,
                                      const BITMAP_UPDATE *bitmap);
static BOOL viewer_send_bitmap_update_locked(Viewer *viewer,
                                             const BITMAP_UPDATE *bitmap);
static BOOL viewer_send_surface_bits(Viewer *viewer,
                                     const SURFACE_BITS_COMMAND *cmd);
static BOOL viewer_enqueue_classic_event_locked(Viewer *viewer,
                                                ViewerClassicEvent *event);
static BOOL
viewer_enqueue_classic_baseline_from_framebuffer(ViewerServer *server,
                                                 Viewer *viewer);
static void viewer_note_classic_queue_state_locked(const Viewer *viewer);
static void viewer_classic_apply_latest_policy_locked(Viewer *viewer);
static void viewer_clear_classic_queue_locked(Viewer *viewer);

static void viewer_server_accumulate_gfx_dirty_viewers(
    ViewerServer *server, const RECTANGLE_16 *dirty_rects,
    UINT32 dirty_rect_count, BOOL dirty_overflow, UINT64 generation,
    UINT32 width, UINT32 height);

static void viewer_gfx_apply_caps_result_locked(
    ViewerServer *server, Viewer *viewer,
    const ViewerGfxPipelineCapsResult *caps_result, UINT64 now,
    BOOL *enter_classic_fallback, const char **classic_fallback_reason) {
  if (!viewer || !caps_result)
    return;

  if (caps_result->actions & VIEWER_GFX_PIPELINE_CAPS_ACTION_DISABLE_RDPEGFX)
    viewer_gfx_pipeline_disable_rdpgfx_locked(viewer);

  if (caps_result->actions & VIEWER_GFX_PIPELINE_CAPS_ACTION_BEGIN_JOIN) {
    viewer_gfx_pipeline_begin_join_locked(viewer, now,
                                          caps_result->begin_join_reason
                                              ? caps_result->begin_join_reason
                                              : "RDPEGFX caps confirmed");
  }

  if ((caps_result->actions &
       VIEWER_GFX_PIPELINE_CAPS_ACTION_ENTER_CLASSIC_FALLBACK) &&
      server && enter_classic_fallback && classic_fallback_reason) {
    *enter_classic_fallback = TRUE;
    *classic_fallback_reason = caps_result->classic_fallback_reason
                                   ? caps_result->classic_fallback_reason
                                   : "RDPEGFX caps negotiation fallback";
  }
}

static BOOL viewer_update_ready(const Viewer *viewer, const char *operation) {
  freerdp_peer *peer = viewer ? viewer->peer : NULL;
  UINT64 now = platform_get_timestamp_ms();
  UINT64 since_activation = 0;
  BOOL post_connect_complete = FALSE;

  if (!viewer || !peer || !viewer->connected || !viewer->activated ||
      !peer->activated || viewer->stop_requested || !peer->context ||
      !peer->context->update) {
    WLog_INFO(
        TAG,
        "Viewer update gate blocked op=%s viewer=%p peer=%p connected=%s "
        "viewer_activated=%s peer_activated=%s stop=%s context=%s "
        "update=%s",
        operation ? operation : "unknown", (const void *)viewer, (void *)peer,
        viewer ? (viewer->connected ? "true" : "false") : "false",
        viewer ? (viewer->activated ? "true" : "false") : "false",
        peer ? (peer->activated ? "true" : "false") : "false",
        viewer ? (viewer->stop_requested ? "true" : "false") : "false",
        peer ? (peer->context ? "true" : "false") : "false",
        (peer && peer->context) ? (peer->context->update ? "true" : "false")
                                : "false");
    return FALSE;
  }

  post_connect_complete = viewer->gfx.post_connect_complete;
  since_activation =
      viewer->gfx.last_activated_ts ? (now - viewer->gfx.last_activated_ts) : 0;
  if (!post_connect_complete ||
      (since_activation < VIEWER_UPDATE_ACTIVATION_GRACE_MS)) {
    WLog_INFO(TAG,
              "Viewer %u update gate delayed op=%s post_connect_complete=%s "
              "since_activation_ms=%" PRIu64 " required_ms=%u",
              viewer->id, operation ? operation : "unknown",
              post_connect_complete ? "true" : "false", since_activation,
              VIEWER_UPDATE_ACTIVATION_GRACE_MS);
    return FALSE;
  }

  return TRUE;
}

static UINT64 viewer_perf_now_us(void) {
#ifdef _WIN32
  static LARGE_INTEGER frequency = {0};
  LARGE_INTEGER counter = {0};

  if (frequency.QuadPart == 0) {
    if (!QueryPerformanceFrequency(&frequency) || (frequency.QuadPart <= 0))
      return platform_get_timestamp_ms() * 1000ULL;
  }

  if (!QueryPerformanceCounter(&counter))
    return platform_get_timestamp_ms() * 1000ULL;

  return (UINT64)((counter.QuadPart * 1000000ULL) / frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((UINT64)ts.tv_sec * 1000000ULL) + ((UINT64)ts.tv_nsec / 1000ULL);
#endif
}

static BOOL viewer_should_log_bitmap_perf(UINT64 batch_count, UINT64 publish_us,
                                          UINT32 send_failed_count) {
  return (batch_count <= 5) || ((batch_count % 100) == 0) ||
         (publish_us >= 5000ULL) || (send_failed_count > 0);
}

static ViewerClassicTransport
viewer_classic_transport_from_viewer(Viewer *viewer) {
  ViewerClassicTransport transport = {0};

  if (!viewer)
    return transport;

  transport.peer = viewer->peer;
  transport.viewer_id = viewer->id;
  transport.packets_sent = &viewer->packets_sent;
  transport.packets_failed = &viewer->packets_failed;
  transport.write_block_events = &viewer->write_block_events;
  transport.bitmap_updates_sent = &viewer->bitmap_updates_sent;
  transport.bitmap_updates_failed = &viewer->bitmap_updates_failed;
  transport.bitmap_rectangles_sent = &viewer->bitmap_rectangles_sent;
  transport.bitmap_payload_bytes_sent = &viewer->bitmap_payload_bytes_sent;
  transport.bitmap_write_block_events = &viewer->bitmap_write_block_events;
  transport.bitmap_send_time_total_us = &viewer->bitmap_send_time_total_us;
  transport.bitmap_send_time_max_us = &viewer->bitmap_send_time_max_us;
  transport.bitmap_updates_skipped_writeblock =
      &viewer->bitmap_updates_skipped_writeblock;
  transport.surface_bits_updates_sent = &viewer->surface_bits_updates_sent;
  transport.surface_bits_updates_failed = &viewer->surface_bits_updates_failed;
  transport.surface_bits_send_time_total_us =
      &viewer->surface_bits_send_time_total_us;
  transport.surface_bits_send_time_max_us =
      &viewer->surface_bits_send_time_max_us;
  transport.surface_bits_payload_bytes_sent =
      &viewer->surface_bits_payload_bytes_sent;
  transport.surface_bits_updates_skipped_writeblock =
      &viewer->surface_bits_updates_skipped_writeblock;
  transport.last_viewer_send_start_us = &viewer->last_viewer_send_start_us;
  transport.last_viewer_send_end_us = &viewer->last_viewer_send_end_us;
  return transport;
}

static BOOL viewer_string_has_value(const char *value) {
  return value && value[0];
}

static char *viewer_identity_field_to_utf8(const void *field, UINT32 length) {
  char *result = NULL;

  if (!field || (length == 0))
    return _strdup("");

#ifdef _WIN32
  {
    int required = WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)field, (int)length,
                                       NULL, 0, NULL, NULL);
    if (required <= 0)
      return NULL;

    result = (char *)calloc((size_t)required + 1, sizeof(char));
    if (!result)
      return NULL;

    if (WideCharToMultiByte(CP_UTF8, 0, (LPCWCH)field, (int)length, result,
                            required, NULL, NULL) != required) {
      free(result);
      return NULL;
    }
    return result;
  }
#else
  result = (char *)calloc((size_t)length + 1, sizeof(char));
  if (!result)
    return NULL;
  memmove(result, field, length);
  return result;
#endif
}

static BOOL viewer_settings_credentials_to_utf8(freerdp_peer *peer,
                                                ViewerAuthCredentials *out) {
  rdpSettings *settings = NULL;
  const char *settings_user = NULL;
  const char *settings_domain = NULL;
  const char *settings_password = NULL;

  if (!peer || !peer->context || !out)
    return FALSE;

  settings = peer->context->settings;
  if (!settings)
    return FALSE;

  settings_user = freerdp_settings_get_string(settings, FreeRDP_Username);
  settings_domain = freerdp_settings_get_string(settings, FreeRDP_Domain);
  settings_password = freerdp_settings_get_string(settings, FreeRDP_Password);

  out->username = _strdup(settings_user ? settings_user : "");
  out->domain = _strdup(settings_domain ? settings_domain : "");
  out->password = _strdup(settings_password ? settings_password : "");

  if (!out->username || !out->domain || !out->password) {
    viewer_auth_credentials_clear(out);
    return FALSE;
  }

  return viewer_auth_credentials_usable(out) ? TRUE : FALSE;
}

static BOOL
viewer_identity_credentials_to_utf8(const SEC_WINNT_AUTH_IDENTITY *identity,
                                    ViewerAuthCredentials *out) {
  if (!identity || !out)
    return FALSE;

  out->username =
      viewer_identity_field_to_utf8(identity->User, identity->UserLength);
  out->domain =
      viewer_identity_field_to_utf8(identity->Domain, identity->DomainLength);
  out->password = viewer_identity_field_to_utf8(identity->Password,
                                                identity->PasswordLength);

  if (!out->username || !out->domain || !out->password ||
      !viewer_auth_credentials_usable(out)) {
    viewer_auth_credentials_clear(out);
    return FALSE;
  }

  return TRUE;
}

static BOOL viewer_credentials_match_expected(const char *expected_user,
                                              const char *expected_domain,
                                              const char *expected_password,
                                              const char *viewer_user,
                                              const char *viewer_domain,
                                              const char *viewer_password) {
  if (!viewer_string_has_value(expected_user) ||
      !viewer_string_has_value(expected_password))
    return FALSE;

  if (!viewer_user || (_stricmp(viewer_user, expected_user) != 0))
    return FALSE;

  if (viewer_string_has_value(expected_domain) &&
      (!viewer_domain || (_stricmp(viewer_domain, expected_domain) != 0)))
    return FALSE;

  if (!viewer_string_has_value(expected_domain) &&
      !viewer_domain_matches_local_alias(viewer_domain))
    return FALSE;

  if (!viewer_password || (strcmp(viewer_password, expected_password) != 0))
    return FALSE;

  return TRUE;
}

static BOOL viewer_backend_credentials_match(const BackendClient *backend,
                                             const char *viewer_user,
                                             const char *viewer_domain,
                                             const char *viewer_password) {
  if (!backend)
    return FALSE;

  return viewer_credentials_match_expected(backend->username, backend->domain,
                                           backend->password, viewer_user,
                                           viewer_domain, viewer_password);
}

/* ---- Classic queue coordination and policy hooks ---- */

static void viewer_apply_classic_drop_info_locked(
    Viewer *viewer, const ViewerClassicQueueDropInfo *drop_info,
    BOOL mark_full_refresh) {
  if (!viewer || !drop_info || (drop_info->dropped_count == 0))
    return;

  viewer->bitmap_queue_dropped += drop_info->dropped_count;
  if (g_viewer_server)
    viewer_publisher_note_classic_drop_bytes(&g_viewer_server->publisher,
                                             drop_info->dropped_count,
                                             drop_info->dropped_payload_bytes);
  if (mark_full_refresh) {
    viewer->needs_full_refresh = TRUE;
    viewer->full_refresh_deadline_ts =
        platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;
  }
}

static void viewer_apply_surface_bits_drop_info_locked(
    Viewer *viewer, const ViewerClassicQueueDropInfo *drop_info) {
  if (!viewer || !drop_info || (drop_info->dropped_count == 0))
    return;

  viewer->surface_bits_queue_dropped += drop_info->dropped_count;
  if (g_viewer_server)
    viewer_publisher_note_classic_drop_bytes(&g_viewer_server->publisher,
                                             drop_info->dropped_count,
                                             drop_info->dropped_payload_bytes);
}

static void viewer_note_classic_queue_state_locked(const Viewer *viewer) {
  ViewerServer *server = g_viewer_server;

  if (!server || !viewer)
    return;

  viewer_publisher_note_classic_queue_state(
      &server->publisher,
      viewer_classic_queue_depth_locked(&viewer->classic_queues),
      viewer_classic_queue_payload_bytes_locked(&viewer->classic_queues));
}

static void
viewer_classic_enqueue_event_direct_locked(Viewer *viewer,
                                           ViewerClassicEvent *event) {
  if (!viewer || !event ||
      (viewer_classic_queue_depth_locked(&viewer->classic_queues) >=
       VIEWER_CLASSIC_QUEUE_CAPACITY))
    return;

  viewer_classic_queue_enqueue_event_direct_locked(&viewer->classic_queues,
                                                   event);
  viewer->bitmap_updates_queued++;
  viewer_note_classic_queue_state_locked(viewer);
}

static void viewer_classic_apply_latest_policy_locked(Viewer *viewer) {
  ViewerServer *server = g_viewer_server;
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerClassicEvent *event = NULL;
  UINT64 queued_bytes = 0;

  if (!server || !viewer)
    return;

  queued_bytes =
      viewer_classic_queue_payload_bytes_locked(&viewer->classic_queues);
  if (viewer_publisher_classic_queue_decision(
          &server->publisher,
          viewer_classic_queue_depth_locked(&viewer->classic_queues),
          queued_bytes) !=
      VIEWER_PUBLISHER_CLASSIC_DECISION_REPLACE_WITH_BASELINE)
    return;

  if (!viewer_publisher_classic_latest_snapshot(
          &server->publisher, &server->framebuffer,
          viewer->classic_last_generation_sent, &snapshot))
    return;

  event = viewer_classic_event_from_snapshot(&snapshot);
  viewer_framebuffer_snapshot_free(&snapshot);
  if (!event)
    return;

  WLog_INFO(TAG,
            "Viewer %u classic latest-state policy replacing %" PRIu32
            " queued events with framebuffer generation %" PRIu64,
            viewer->id,
            viewer_classic_queue_depth_locked(&viewer->classic_queues),
            viewer_classic_event_generation(event));
  viewer_clear_classic_queue_locked(viewer);
  viewer_classic_enqueue_event_direct_locked(viewer, event);
}

static BOOL
viewer_enqueue_classic_baseline_from_framebuffer(ViewerServer *server,
                                                 Viewer *viewer) {
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerClassicEvent *event = NULL;
  BOOL queued = FALSE;

  if (!server || !viewer)
    return FALSE;

  if (!viewer_publisher_classic_baseline_snapshot(
          &server->publisher, &server->framebuffer, &snapshot))
    return FALSE;

  event = viewer_classic_event_from_snapshot(&snapshot);
  viewer_framebuffer_snapshot_free(&snapshot);
  if (!event)
    return FALSE;

  EnterCriticalSection(&viewer->send_lock);
  queued = viewer_enqueue_classic_event_locked(viewer, event);
  LeaveCriticalSection(&viewer->send_lock);

  if (!queued) {
    viewer_classic_event_free(event);
    return FALSE;
  }

  WLog_INFO(TAG, "Viewer %u queued classic framebuffer baseline", viewer->id);
  return TRUE;
}

static void viewer_clear_classic_queue_locked(Viewer *viewer) {
  ViewerClassicQueueDropInfo drop_info = {0};

  if (!viewer)
    return;

  viewer_classic_queue_clear_locked(&viewer->classic_queues, &drop_info);
  viewer_apply_classic_drop_info_locked(viewer, &drop_info, FALSE);
  viewer_note_classic_queue_state_locked(viewer);
}

static BOOL viewer_enqueue_classic_event_locked(Viewer *viewer,
                                                ViewerClassicEvent *event) {
  ViewerClassicQueueDropInfo drop_info = {0};
  BOOL enqueued = FALSE;
  UINT32 depth = 0;

  if (!viewer || !event)
    return FALSE;

  depth = viewer_classic_queue_depth_locked(&viewer->classic_queues);
  if (depth >= VIEWER_CLASSIC_QUEUE_CAPACITY)
    WLog_WARN(TAG, "Viewer %u classic queue full (%u entries), dropping oldest",
              viewer->id, depth);

  enqueued = viewer_classic_queue_enqueue_event_locked(&viewer->classic_queues,
                                                       event, &drop_info);
  viewer_apply_classic_drop_info_locked(viewer, &drop_info, TRUE);
  if (enqueued)
    viewer->bitmap_updates_queued++;
  viewer_note_classic_queue_state_locked(viewer);
  viewer_classic_apply_latest_policy_locked(viewer);
  return enqueued;
}

static ViewerClassicEvent *viewer_dequeue_classic_event_locked(Viewer *viewer) {
  ViewerClassicEvent *event = NULL;

  if (!viewer)
    return NULL;

  event = viewer_classic_queue_dequeue_locked(&viewer->classic_queues);
  if (event)
    viewer_note_classic_queue_state_locked(viewer);
  return event;
}

static BOOL
viewer_enqueue_surface_bits_event_locked(Viewer *viewer,
                                         ViewerSurfaceBitsEvent *event) {
  ViewerClassicQueueDropInfo drop_info = {0};
  BOOL enqueued = FALSE;
  UINT32 depth = 0;

  if (!viewer || !event)
    return FALSE;

  depth = viewer_surface_bits_queue_depth_locked(&viewer->classic_queues);
  if (depth >= VIEWER_SURFACE_BITS_QUEUE_CAPACITY) {
    WLog_WARN(TAG,
              "Viewer %u SurfaceBits queue full (%u entries), dropping oldest",
              viewer->id, depth);
  }

  enqueued = viewer_surface_bits_queue_enqueue_event_locked(
      &viewer->classic_queues, event, &drop_info);
  viewer_apply_surface_bits_drop_info_locked(viewer, &drop_info);
  if (enqueued)
    viewer->surface_bits_updates_queued++;
  return enqueued;
}

static ViewerSurfaceBitsEvent *
viewer_dequeue_surface_bits_event_locked(Viewer *viewer) {
  return viewer
             ? viewer_surface_bits_queue_dequeue_locked(&viewer->classic_queues)
             : NULL;
}

static BOOL viewer_gfx_publisher_state_init(ViewerGfxPublisherState *gfx) {
  if (!gfx || gfx->initialized)
    return gfx && gfx->initialized;

  if (!InitializeCriticalSectionAndSpinCount(&gfx->lock, 4000)) {
    WLog_ERR(TAG, "Failed to initialize shared RDPEGFX publisher lock");
    return FALSE;
  }

  gfx->initialized = TRUE;
  return TRUE;
}

static void viewer_gfx_publisher_state_uninit(ViewerGfxPublisherState *gfx) {
  if (!gfx || !gfx->initialized)
    return;

  DeleteCriticalSection(&gfx->lock);
  memset(gfx, 0, sizeof(*gfx));
}

static BOOL viewer_pump_gfx(Viewer *viewer) {
  (void)viewer;
  return TRUE;
}

static BOOL viewer_pump_classic(Viewer *viewer) {
  ViewerClassicEvent *event = NULL;
  UINT32 pumped = 0;
  UINT32 coalesced = 0;
  UINT32 sb_pumped = 0;
  BOOL classic_fallback = FALSE;
  ViewerClassicTransport transport = {0};

  if (!viewer)
    return FALSE;

  /* Only pump for classic-fallback viewers */
  EnterCriticalSection(&viewer->gfx.lock);
  classic_fallback = viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx);
  LeaveCriticalSection(&viewer->gfx.lock);

  if (!classic_fallback)
    return TRUE;

  if (!viewer_update_ready(viewer, "pump-classic"))
    return TRUE;

  transport = viewer_classic_transport_from_viewer(viewer);

  /* Hold the update batch across the entire drain loop so that mstsc receives

   * * all bitmap updates as a continuous stream without rendering between
   *
   * individual sends. */
  (void)viewer_classic_transport_begin_batch(&transport);

  for (;;) {
    /* Drain one queued backend BITMAP_UPDATE at a time. Coalescing is disabled

     * * for now; large source batches are split into bounded chunks by the
     * send
     * helper below. */
    EnterCriticalSection(&viewer->send_lock);

    if (viewer_publisher_classic_pump_decision(
            viewer->needs_full_refresh,
            viewer_classic_queue_depth_locked(&viewer->classic_queues)) ==
        VIEWER_PUBLISHER_CLASSIC_PUMP_DROP_BITMAPS_FOR_FULL_REFRESH) {
      UINT32 classic_depth =
          viewer_classic_queue_depth_locked(&viewer->classic_queues);
      WLog_INFO(TAG,
                "Viewer %u pump-classic: dropping %" PRIu32
                " queued updates (needs full refresh)",
                viewer->id, classic_depth);
      viewer_clear_classic_queue_locked(viewer);
      LeaveCriticalSection(&viewer->send_lock);
      break;
    }

    event = viewer_dequeue_classic_event_locked(viewer);
    LeaveCriticalSection(&viewer->send_lock);

    if (!event)
      break;

    /* Conservative stability mode: do not coalesce multiple backend updates

     * * into one BitmapUpdate PDU. Sending one queued batch at a time avoids

     * * oversized or mixed batches while investigating MSTSC 0xd06
     * disconnects.
     */

    /* Send the bitmap update, split into bounded chunks if necessary.
     *
     * The update batch is already held across the entire pump loop, so mstsc

     * * receives all updates as a continuous stream. */
    if (!viewer_send_bitmap_update_locked(viewer,
                                          viewer_classic_event_bitmap(event))) {
      WLog_WARN(TAG, "Viewer %u pump-classic: send failed", viewer->id);
      viewer_classic_event_free(event);
      /* Send failure is not fatal — the viewer may recover */
      continue;
    }

    pumped++;
    if (viewer_classic_event_generation(event) >
        viewer->classic_last_generation_sent)
      viewer->classic_last_generation_sent =
          viewer_classic_event_generation(event);
    viewer_classic_event_free(event);
  }

  /* Drain SurfaceBits queue — each event is a separate codec frame, no
   * coalescing. Unlike BitmapUpdate, SurfaceBits are NOT dropped when
   * needs_full_refresh is true. When SurfaceBits (NSCodec/RemoteFX) is the
   * active codec, these tiles ARE the refresh data — dropping them creates a
   * deadlock where the viewer never receives the content needed to resync. */
  for (;;) {
    ViewerSurfaceBitsEvent *sb_event = NULL;

    EnterCriticalSection(&viewer->send_lock);
    sb_event = viewer_dequeue_surface_bits_event_locked(viewer);
    LeaveCriticalSection(&viewer->send_lock);

    if (!sb_event)
      break;

    /* Send outside send_lock — the transport batch provides FreeRDP's own
     * sync,
     * and the event data is locally owned after dequeue. */
    if (!viewer_classic_transport_send_surface_bits(
            &transport, viewer_surface_bits_event_command(sb_event), FALSE)) {
      WLog_WARN(TAG, "Viewer %u pump-classic: SurfaceBits send failed",
                viewer->id);
      viewer_surface_bits_event_free(sb_event);
      continue;
    }

    sb_pumped++;
    viewer_surface_bits_event_free(sb_event);
  }

  if ((pumped > 0) || (sb_pumped > 0)) {
    WLog_INFO(TAG,
              "Viewer %u pump-classic: delivered %" PRIu32
              " bitmaps (coalesced=%" PRIu32 ") %" PRIu32 " SurfaceBits",
              viewer->id, pumped, coalesced, sb_pumped);
  }

  /* Release the update batch acquired at the top of this function. */
  viewer_classic_transport_end_batch(&transport);

  return TRUE;
}

static void viewer_get_backend_layout(BackendClient *backend, UINT32 *width,
                                      UINT32 *height, UINT32 *generation) {
  UINT32 current_width = 0;
  UINT32 current_height = 0;
  UINT32 current_generation = 0;

  if (backend) {
    backend_get_desktop_layout(backend, &current_width, &current_height,
                               &current_generation);
    if (current_width == 0)
      current_width = backend->monitor_layout.total_width;
    if (current_height == 0)
      current_height = backend->monitor_layout.total_height;
  }

  if (width)
    *width = current_width;
  if (height)
    *height = current_height;
  if (generation)
    *generation = current_generation;
}

static void viewer_server_clear_input_owner_locked(ViewerServer *server,
                                                   UINT32 viewer_id,
                                                   const char *reason) {
  if (!server || !server->input_owner_active ||
      (server->input_owner_viewer_id != viewer_id))
    return;

  server->input_owner_active = FALSE;
  server->input_owner_viewer_id = 0;
  server->input_owner_last_input_ts = 0;

  if (reason)
    WLog_INFO(TAG, "Viewer %u %s", viewer_id, reason);
}

static BOOL viewer_input_owner_alive_locked(ViewerServer *server,
                                            UINT32 viewer_id) {
  if (!server || (viewer_id == 0))
    return FALSE;

  for (int i = 0; i < MAX_VIEWERS; i++) {
    const Viewer *current = &server->viewers[i];
    if ((current->id == viewer_id) && current->connected && current->activated)
      return TRUE;
  }

  return FALSE;
}

static BOOL viewer_graphics_context_init(ViewerGraphicsContext *gfx) {
  if (!gfx || gfx->initialized)
    return gfx && gfx->initialized;

  if (!InitializeCriticalSectionAndSpinCount(&gfx->lock, 4000)) {
    WLog_ERR(TAG, "Failed to initialize viewer RDPEGFX context lock");
    return FALSE;
  }
  gfx->preferred_codec = VIEWER_GFX_CODEC_UNCOMPRESSED;
  gfx->selected_codec = VIEWER_GFX_CODEC_UNCOMPRESSED;
  gfx->initialized = TRUE;
  return TRUE;
}

static void viewer_graphics_context_reset(ViewerGraphicsContext *gfx,
                                          BackendClient *backend) {
  (void)backend;

  if (!gfx)
    return;

  gfx->post_connect_complete = FALSE;
  gfx->ready = FALSE;
  gfx->preferred_codec = g_viewer_server ? g_viewer_server->viewer_gfx_codec
                                         : VIEWER_GFX_CODEC_UNCOMPRESSED;
  gfx->selected_codec = gfx->preferred_codec;
  viewer_gfx_pipeline_invalidate_surface_locked(gfx);
  gfx->channel_opened = FALSE;
  gfx->vcm_progress_logged = FALSE;
  gfx->drdynvc_joined = FALSE;
  gfx->caps_ready = FALSE;
  gfx->drdynvc_state = DRDYNVC_STATE_NONE;
  viewer_gfx_pipeline_reset_join_state_locked(gfx);
  viewer_gfx_pipeline_reset_dirty_state_locked(gfx);
}

static void viewer_graphics_context_uninit(ViewerGraphicsContext *gfx) {
  if (!gfx || !gfx->initialized)
    return;

  DeleteCriticalSection(&gfx->lock);
  memset(gfx, 0, sizeof(*gfx));
}

static void viewer_auth_state_reset(Viewer *viewer) {
  if (!viewer)
    return;

  viewer->auth_state = VIEWER_AUTH_STATE_NONE;
  viewer->auth_deferred_required = FALSE;
  viewer->auth_deferred_checked = FALSE;
  viewer->auth_deferred_accepted = FALSE;
}

static BOOL viewer_send_state_init(Viewer *viewer) {
  if (!viewer)
    return FALSE;

  if (!InitializeCriticalSectionAndSpinCount(&viewer->send_lock, 4000))
    return FALSE;
  viewer->packets_sent = 0;
  viewer->packets_failed = 0;
  viewer->write_block_events = 0;
  viewer->bitmap_updates_sent = 0;
  viewer->bitmap_updates_failed = 0;
  viewer->bitmap_rectangles_sent = 0;
  viewer->bitmap_payload_bytes_sent = 0;
  viewer->bitmap_write_block_events = 0;
  viewer->bitmap_send_time_total_us = 0;
  viewer->bitmap_send_time_max_us = 0;
  viewer->bitmap_updates_skipped_writeblock = 0;
  viewer->bitmap_updates_skipped_throttle = 0;
  viewer->bitmap_updates_queued = 0;
  viewer->bitmap_queue_dropped = 0;
  viewer->classic_last_generation_sent = 0;
  viewer->consecutive_lag_intervals = 0;
  viewer->sustained_lag_start_ts = 0;
  viewer->last_pointer_position_generation = 0;
  viewer->last_pointer_shape_generation = 0;
  memset(&viewer->classic_queues, 0, sizeof(viewer->classic_queues));
  viewer->surface_bits_updates_sent = 0;
  viewer->surface_bits_updates_failed = 0;
  viewer->surface_bits_updates_skipped_writeblock = 0;
  viewer->surface_bits_updates_skipped_throttle = 0;
  viewer->surface_bits_updates_queued = 0;
  viewer->surface_bits_queue_dropped = 0;
  viewer->surface_bits_send_time_total_us = 0;
  viewer->surface_bits_send_time_max_us = 0;
  viewer->surface_bits_payload_bytes_sent = 0;
  if (!viewer_classic_queues_init(&viewer->classic_queues)) {
    DeleteCriticalSection(&viewer->send_lock);
    return FALSE;
  }
  return TRUE;
}

static void viewer_send_state_uninit(Viewer *viewer) {
  if (!viewer || !viewer_classic_queues_event(&viewer->classic_queues))
    return;

  /* Free any remaining classic queue entries */
  EnterCriticalSection(&viewer->send_lock);
  viewer_clear_classic_queue_locked(viewer);
  {
    ViewerClassicQueueDropInfo surface_drop_info = {0};
    viewer_surface_bits_queue_clear_locked(&viewer->classic_queues,
                                           &surface_drop_info);
    viewer_apply_surface_bits_drop_info_locked(viewer, &surface_drop_info);
  }
  viewer->classic_last_generation_sent = 0;
  LeaveCriticalSection(&viewer->send_lock);

  viewer_classic_queues_uninit(&viewer->classic_queues);

  DeleteCriticalSection(&viewer->send_lock);
}

static void viewer_join_thread(Viewer *viewer) {
  if (!viewer || !viewer->thread)
    return;

  WaitForSingleObject(viewer->thread, INFINITE);
  CloseHandle(viewer->thread);
  viewer->thread = NULL;
}

static void viewer_cleanup_slot_finish_locked(Viewer *viewer) {
  if (!viewer)
    return;

  viewer->peer = NULL;
  viewer->context = NULL;
  viewer->connected = FALSE;
  viewer->activated = FALSE;
  viewer->counted_in_viewer_count = FALSE;
  viewer->cleanup_in_progress = FALSE;
  viewer_auth_state_reset(viewer);
  viewer->publish_ref_count = 0;
  viewer->needs_full_refresh = FALSE;
  viewer->stop_requested = FALSE;
  viewer->full_refresh_deadline_ts = 0;
  viewer->last_pointer_position_generation = 0;
  viewer->last_pointer_shape_generation = 0;
  viewer->classic_last_generation_sent = 0;
}

static void viewer_wait_for_publish_refs(ViewerServer *server, Viewer *viewer) {
  for (;;) {
    UINT32 publish_ref_count = 0;

    if (!server || !viewer)
      return;

    EnterCriticalSection(&server->lock);
    publish_ref_count = viewer->publish_ref_count;
    LeaveCriticalSection(&server->lock);

    if (publish_ref_count == 0)
      return;

    platform_sleep_ms(1);
  }
}

static void viewer_cleanup_slot(ViewerServer *server, Viewer *viewer) {
  if (!viewer)
    return;

  viewer_wait_for_publish_refs(server, viewer);

  viewer_send_state_uninit(viewer);
  viewer_gfx_pipeline_uninit(viewer);
  viewer_graphics_context_uninit(&viewer->gfx);

  if (server) {
    EnterCriticalSection(&server->lock);
    viewer_cleanup_slot_finish_locked(viewer);
    LeaveCriticalSection(&server->lock);
  } else
    viewer_cleanup_slot_finish_locked(viewer);
}

static void viewer_release_count_locked(ViewerServer *server, Viewer *viewer,
                                        const char *reason) {
  if (!server || !viewer)
    return;

  viewer_server_clear_input_owner_locked(
      server, viewer->id, "disconnected, input ownership cleared");
  viewer->connected = FALSE;
  viewer->activated = FALSE;
  viewer->needs_full_refresh = FALSE;
  viewer->full_refresh_deadline_ts = 0;
  viewer->stop_requested = TRUE;

  if (!viewer->counted_in_viewer_count)
    return;

  if (server->viewer_count > 0)
    server->viewer_count--;
  else
    WLog_WARN(TAG, "Viewer %u release requested with viewer_count already 0",
              viewer->id);
  viewer->counted_in_viewer_count = FALSE;

  if (reason)
    WLog_INFO(TAG, "Viewer %u released from viewer_count (%s); count=%u",
              viewer->id, reason, server->viewer_count);
}

static BOOL viewer_matches_peer_context_locked(const Viewer *viewer,
                                               freerdp_peer *peer,
                                               rdpContext *context) {
  if (!viewer || !peer || (viewer->peer != peer))
    return FALSE;

  return !context || (viewer->context == context);
}

static BOOL viewer_try_begin_cleanup_locked(Viewer *viewer, freerdp_peer *peer,
                                            rdpContext *context,
                                            BOOL require_thread_stopped,
                                            BOOL close_thread_handle) {
  if (!viewer_matches_peer_context_locked(viewer, peer, context) ||
      viewer->cleanup_in_progress)
    return FALSE;

  if (viewer->thread) {
    if (require_thread_stopped) {
      if (WaitForSingleObject(viewer->thread, 0) != WAIT_OBJECT_0)
        return FALSE;
      close_thread_handle = TRUE;
    }

    if (close_thread_handle) {
      CloseHandle(viewer->thread);
      viewer->thread = NULL;
    }
  }

  viewer->cleanup_in_progress = TRUE;
  return TRUE;
}

static BOOL viewer_try_add_publish_ref_locked(Viewer *viewer) {
  if (!viewer || !viewer->peer || !viewer->connected || !viewer->activated ||
      viewer->cleanup_in_progress)
    return FALSE;

  viewer->publish_ref_count++;
  return TRUE;
}

static BOOL viewer_try_add_layout_ref_locked(Viewer *viewer) {
  if (!viewer || !viewer->peer || !viewer->connected ||
      viewer->cleanup_in_progress || !viewer->gfx.initialized)
    return FALSE;

  viewer->publish_ref_count++;
  return TRUE;
}

static void viewer_release_publish_ref(ViewerServer *server, Viewer *viewer) {
  if (!server || !viewer)
    return;

  EnterCriticalSection(&server->lock);
  if (viewer->publish_ref_count > 0)
    viewer->publish_ref_count--;
  else
    WLog_WARN(TAG, "Viewer %u publish ref release requested with count 0",
              viewer->id);
  LeaveCriticalSection(&server->lock);
}

static void viewer_server_accumulate_gfx_dirty_viewers(
    ViewerServer *server, const RECTANGLE_16 *dirty_rects,
    UINT32 dirty_rect_count, BOOL dirty_overflow, UINT64 generation,
    UINT32 width, UINT32 height) {
  Viewer *targets[MAX_VIEWERS] = {0};
  size_t target_count = 0;

  if (!server || (generation == 0) || (width == 0) || (height == 0))
    return;

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    Viewer *viewer = &server->viewers[i];

    if (!viewer_try_add_publish_ref_locked(viewer))
      continue;

    targets[target_count++] = viewer;
  }
  LeaveCriticalSection(&server->lock);

  for (size_t i = 0; i < target_count; i++) {
    Viewer *viewer = targets[i];
    BOOL signal_viewer = FALSE;

    EnterCriticalSection(&viewer->gfx.lock);
    if (viewer->gfx.initialized && viewer->gfx.use_rdpgfx &&
        viewer->gfx.caps_ready && viewer->gfx.channel_opened &&
        !viewer->gfx.rdpgfx_temporarily_disabled &&
        (viewer->gfx.join_state == VIEWER_JOIN_STATE_LIVE) &&
        viewer->gfx.dirty_updates_enabled &&
        (generation > viewer->gfx.dirty_last_sent_generation)) {
      signal_viewer = viewer_gfx_pipeline_pending_dirty_add_locked(
          &viewer->gfx, dirty_rects, dirty_rect_count, dirty_overflow,
          generation, width, height);
    }
    LeaveCriticalSection(&viewer->gfx.lock);

    if (signal_viewer)
      viewer_classic_queues_signal(&viewer->classic_queues);

    viewer_release_publish_ref(server, viewer);
  }
}

static BOOL viewer_slot_available_locked(Viewer *viewer) {
  if (!viewer || viewer->peer || viewer->context || viewer->cleanup_in_progress)
    return FALSE;

  if (viewer->thread) {
    if (WaitForSingleObject(viewer->thread, 0) != WAIT_OBJECT_0)
      return FALSE;

    CloseHandle(viewer->thread);
    viewer->thread = NULL;
  }

  return TRUE;
}

static BOOL viewer_forward_pointer(Viewer *viewer, BOOL force) {
  ViewerServer *server = g_viewer_server;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;
  BackendClient *backend = server ? server->backend : NULL;
  UINT16 pointer_x = 0;
  UINT16 pointer_y = 0;
  UINT32 pointer_type = SYSPTR_DEFAULT;
  BOOL pointer_visible = TRUE;
  PointerShapeEntry shape_copy = {0};
  BOOL has_active_shape = FALSE;
  ViewerPointerSnapshot pointer_snapshot = {0};
  ViewerPointerUpdatePlan pointer_plan = {0};
  ViewerPointerTransport pointer_transport = {0};
  UINT64 position_generation = 0;
  UINT64 shape_generation = 0;
  BOOL sent = TRUE;

  if (!viewer || !server || !backend || !peer || !peer->context ||
      !peer->context->update || !peer->context->update->pointer ||
      !viewer->connected || !viewer->activated || !peer->activated)
    return FALSE;

  if (!viewer_update_ready(viewer, "pointer"))
    return FALSE;

  if (!backend_get_pointer_snapshot_copy(
          backend, &pointer_x, &pointer_y, &pointer_visible, &pointer_type,
          &shape_copy, &has_active_shape, &position_generation,
          &shape_generation))
    return FALSE;
  WLog_INFO(TAG, "viewer_forward_pointer: x=%u y=%u visible=%d gen=%llu->%llu",
            pointer_x, pointer_y, pointer_visible,
            (unsigned long long)viewer->last_pointer_position_generation,
            (unsigned long long)position_generation);
  pointer_snapshot.x = pointer_x;
  pointer_snapshot.y = pointer_y;
  pointer_snapshot.visible = pointer_visible;
  pointer_snapshot.type = pointer_type;
  pointer_snapshot.active_shape = &shape_copy;
  pointer_snapshot.has_active_shape = has_active_shape;
  pointer_snapshot.position_generation = position_generation;
  pointer_snapshot.shape_generation = shape_generation;
  if (!viewer_pointer_plan_from_snapshot(
          &pointer_snapshot, viewer->last_pointer_position_generation,
          viewer->last_pointer_shape_generation, force, &pointer_plan)) {
    pointer_shape_entry_reset(&shape_copy);
    return FALSE;
  }
  /* disabled: (void)viewer_forward_pointer; logging kept for debug */
  WLog_INFO(
      TAG, "  shape=%d pos=%d send_shape=%d send_position=%d",
      force || (shape_generation != viewer->last_pointer_shape_generation),
      force ||
          (position_generation != viewer->last_pointer_position_generation),
      pointer_plan.send_system || pointer_plan.send_color ||
          pointer_plan.send_new,
      pointer_plan.send_position);

  if (!pointer_plan.send_system && !pointer_plan.send_color &&
      !pointer_plan.send_new && !pointer_plan.send_position) {
    pointer_shape_entry_reset(&shape_copy);
    return TRUE;
  }

  if (pointer_plan.send_position && !pointer_plan.send_system &&
      !pointer_plan.send_color && !pointer_plan.send_new) {
    /* Position-only pointer updates are intentionally suppressed; acknowledge

     * * them so this viewer does not retry that plan forever. */
    viewer->last_pointer_position_generation = position_generation;
    pointer_shape_entry_reset(&shape_copy);
    return TRUE;
  }

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    if (!peer->DrainOutputBuffer || (peer->DrainOutputBuffer(peer) < 0) ||
        peer->IsWriteBlocked(peer)) {
      pointer_shape_entry_reset(&shape_copy);
      return FALSE;
    }
  }

  pointer_transport.peer = peer;
  pointer_transport.classic_transport =
      viewer_classic_transport_from_viewer(viewer);
  sent = viewer_pointer_transport_send_plan(&pointer_transport, &pointer_plan);

  pointer_shape_entry_reset(&shape_copy);
  if (!sent)
    return FALSE;

  WLog_INFO(TAG, "  sending: send_pos=%d sent=%d final_gen=%llu",
            pointer_plan.send_position, sent,
            (unsigned long long)position_generation);
  if (pointer_plan.send_position)
    viewer->last_pointer_position_generation = position_generation;
  viewer->last_pointer_shape_generation = shape_generation;
  return TRUE;
}

static BOOL viewer_send_surface_bits(Viewer *viewer,
                                     const SURFACE_BITS_COMMAND *cmd) {
  ViewerClassicTransport transport =
      viewer_classic_transport_from_viewer(viewer);

  if (!viewer || !cmd)
    return FALSE;

  if (!viewer_update_ready(viewer, "SurfaceBits"))
    return FALSE;

  return viewer_classic_transport_send_surface_bits(&transport, cmd, FALSE);
}

static BOOL viewer_send_bitmap_update(Viewer *viewer,
                                      const BITMAP_UPDATE *bitmap) {
  ViewerClassicTransport transport =
      viewer_classic_transport_from_viewer(viewer);

  if (!viewer || !bitmap)
    return FALSE;

  if (!viewer_update_ready(viewer, "BitmapUpdate"))
    return FALSE;

  return viewer_classic_transport_send_bitmap_update(&transport, bitmap);
}

/* Same as viewer_send_bitmap_update but assumes the update batch is already
 *
 * held. Used by viewer_pump_classic to batch multiple sends under a single
 * lock
 * acquisition, preventing mstsc from rendering between individual
 * updates. */
static BOOL viewer_send_bitmap_update_locked(Viewer *viewer,
                                             const BITMAP_UPDATE *bitmap) {
  ViewerClassicTransport transport =
      viewer_classic_transport_from_viewer(viewer);

  if (!viewer || !bitmap)
    return FALSE;

  if (!viewer_update_ready(viewer, "BitmapUpdateLocked"))
    return FALSE;

  return viewer_classic_transport_send_bitmap_update_batched(&transport,
                                                             bitmap);
}

static BOOL viewer_send_frame_marker(Viewer *viewer,
                                     const SURFACE_FRAME_MARKER *marker) {
  ViewerClassicTransport transport =
      viewer_classic_transport_from_viewer(viewer);

  if (!viewer || !marker)
    return FALSE;

  if (!viewer_update_ready(viewer, "SurfaceFrameMarker"))
    return FALSE;

  return viewer_classic_transport_send_frame_marker(&transport, marker);
}

static Viewer *find_viewer_by_peer(freerdp_peer *peer) {
  ViewerServer *server = g_viewer_server;
  if (!server)
    return NULL;

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    if (server->viewers[i].peer == peer) {
      LeaveCriticalSection(&server->lock);
      return &server->viewers[i];
    }
  }
  LeaveCriticalSection(&server->lock);
  return NULL;
}

static BOOL can_viewer_send_input(Viewer *viewer) {
  ViewerServer *server = g_viewer_server;
  ViewerInputOwnershipState state = {0};
  const char *clear_reason = NULL;
  BOOL allowed = FALSE;

  if (!viewer || !server || !viewer->connected || !viewer->activated)
    return FALSE;

  EnterCriticalSection(&server->lock);
  if (server->input_owner_active &&
      !viewer_input_owner_alive_locked(server, server->input_owner_viewer_id))
    clear_reason = "disconnected, input ownership cleared";
  else if (server->input_owner_active &&
           ((platform_get_timestamp_ms() - server->input_owner_last_input_ts) >=
            INPUT_IDLE_TIMEOUT_MS))
    clear_reason = "timed out, input now free";

  if (clear_reason)
    viewer_server_clear_input_owner_locked(
        server, server->input_owner_viewer_id, clear_reason);

  state.owner_active = server->input_owner_active;
  state.owner_viewer_id = server->input_owner_viewer_id;
  state.last_input_ts = server->input_owner_last_input_ts;
  allowed = viewer_input_try_acquire(
      &state, viewer->id, viewer->connected, viewer->activated,
      viewer_input_owner_alive_locked(server, state.owner_viewer_id),
      platform_get_timestamp_ms(), INPUT_IDLE_TIMEOUT_MS);
  server->input_owner_active = state.owner_active;
  server->input_owner_viewer_id = state.owner_viewer_id;
  server->input_owner_last_input_ts = state.last_input_ts;
  LeaveCriticalSection(&server->lock);
  return allowed;
}

static BOOL viewer_gfx_enter_classic_fallback(ViewerServer *server,
                                              Viewer *viewer, UINT64 now,
                                              const char *reason) {
  if (!viewer)
    return FALSE;

  viewer_gfx_pipeline_enter_classic_fallback(viewer, now, reason, NULL);

  EnterCriticalSection(&viewer->send_lock);
  viewer->needs_full_refresh = TRUE;
  viewer->full_refresh_deadline_ts = now + FULL_REFRESH_TIMEOUT_MS;
  LeaveCriticalSection(&viewer->send_lock);

  if (server && server->backend)
    (void)backend_request_full_refresh(server->backend);

  WLog_WARN(TAG, "Viewer %u late join falling back to classic path reason=%s",
            viewer->id, reason ? reason : "unspecified");
  return TRUE;
}

static BOOL viewer_gfx_handle_failure(ViewerServer *server, Viewer *viewer,
                                      UINT64 now, const char *reason) {
  BOOL disconnect = FALSE;
  freerdp_peer *peer = NULL;

  if (!viewer)
    return FALSE;

  EnterCriticalSection(&viewer->gfx.lock);
  disconnect =
      viewer_gfx_failure_requires_disconnect(&viewer->gfx, viewer->activated);
  LeaveCriticalSection(&viewer->gfx.lock);

  if (!disconnect)
    return viewer_gfx_enter_classic_fallback(server, viewer, now, reason);

  peer = viewer->peer;
  viewer->stop_requested = TRUE;
  WLog_ERR(TAG,
           "Viewer %u post-activation RDPEGFX failure; disconnecting for "
           "classic reconnect reason=%s",
           viewer->id, reason ? reason : "unspecified");
  if (peer && peer->Disconnect)
    peer->Disconnect(peer);
  return FALSE;
}

static void viewer_gfx_reject_join(Viewer *viewer, const char *reason) {
  if (!viewer)
    return;

  viewer_gfx_pipeline_reject_join(viewer, reason);
  viewer->stop_requested = TRUE;
  WLog_ERR(TAG, "Viewer %u join rejected reason=%s", viewer->id,
           reason ? reason : "unspecified");
}

static BOOL viewer_gfx_send_framebuffer_baseline(ViewerServer *server,
                                                 Viewer *viewer, UINT64 now) {
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerGfxJoinResult result = {0};
  BOOL sent = FALSE;

  if (!server || !viewer)
    return FALSE;

  if (!viewer_publisher_classic_baseline_snapshot(
          &server->publisher, &server->framebuffer, &snapshot)) {
    WLog_WARN(TAG, "Viewer %u RDPEGFX baseline snapshot unavailable",
              viewer->id);
    return viewer_gfx_handle_failure(
        server, viewer, now, "RDPEGFX framebuffer baseline unavailable");
  }

  sent = viewer_gfx_pipeline_send_snapshot(server, viewer, &snapshot);
  viewer_framebuffer_snapshot_free(&snapshot);

  if (!sent) {
    WLog_WARN(TAG, "Viewer %u RDPEGFX framebuffer baseline send failed",
              viewer->id);
    return viewer_gfx_handle_failure(
        server, viewer, now, "RDPEGFX framebuffer baseline send failed");
  }

  viewer_gfx_pipeline_on_baseline_result(viewer, now, TRUE, &result);

  EnterCriticalSection(&viewer->send_lock);
  viewer->needs_full_refresh = FALSE;
  viewer->full_refresh_deadline_ts = 0;
  LeaveCriticalSection(&viewer->send_lock);

  WLog_INFO(TAG, "Viewer %u RDPEGFX framebuffer baseline sent", viewer->id);
  if (result.actions & VIEWER_GFX_JOIN_ACTION_SEND_POINTER_BASELINE)
    (void)viewer_forward_pointer(viewer, TRUE);
  return TRUE;
}

static BOOL viewer_gfx_try_send_dirty_update(ViewerServer *server,
                                             Viewer *viewer, UINT64 now) {
  ViewerFramebufferSnapshot snapshot = {0};
  ViewerGfxPendingDirtyBatch dirty_batch = {0};
  const char *reason = NULL;
  UINT64 last_sent_generation = 0;
  UINT64 snapshot_generation = 0;
  UINT32 original_dirty_rect_count = 0;
  UINT32 snapshot_dirty_rect_count = 0;
  BOOL diagnostic_full_frame_dirty = FALSE;
  ViewerGfxDirtySendStatus send_status = VIEWER_GFX_DIRTY_SEND_FAILED;

  if (!server || !viewer || !server->viewer_gfx_enabled)
    return TRUE;

  if (viewer_gfx_pipeline_poll_dirty_pacing(viewer, now, &reason) !=
      VIEWER_GFX_DIRTY_PACING_OK) {
    UINT64 pending_area = 0;
    UINT64 pending_start_generation = 0;
    UINT64 pending_latest_generation = 0;
    UINT64 fallback_count = 0;
    UINT64 remerge_count = 0;
    UINT64 in_flight_bytes = 0;
    UINT32 in_flight_frames = 0;
    UINT32 pending_rect_count = 0;

    EnterCriticalSection(&viewer->gfx.lock);
    in_flight_frames = viewer->gfx.dirty_in_flight_frames;
    in_flight_bytes = viewer->gfx.dirty_in_flight_bytes;
    pending_rect_count = viewer->gfx.pending_dirty_rect_count;
    pending_area = viewer->gfx.pending_dirty_area;
    pending_start_generation = viewer->gfx.pending_dirty_start_generation;
    pending_latest_generation = viewer->gfx.pending_dirty_latest_generation;
    fallback_count = viewer->gfx.dirty_diag_full_frame_fallbacks;
    remerge_count = viewer->gfx.dirty_diag_remerges;
    LeaveCriticalSection(&viewer->gfx.lock);
    WLog_DBG(TAG,
             "Viewer %u RDPEGFX dirty pacing suspended: reason=%s "
             "in_flight_frames=%u in_flight_bytes=%" PRIu64
             " pending_dirty_rects=%u pending_area=%" PRIu64
             " pending_start_generation=%" PRIu64
             " pending_latest_generation=%" PRIu64
             " full_frame_fallbacks=%" PRIu64 " remerges=%" PRIu64,
             viewer->id, reason ? reason : "unknown", in_flight_frames,
             in_flight_bytes, pending_rect_count, pending_area,
             pending_start_generation, pending_latest_generation,
             fallback_count, remerge_count);
    if (reason && (strcmp(reason, "dirty ack timeout") == 0))
      return viewer_gfx_handle_failure(server, viewer, now, reason);
    return TRUE;
  }

  EnterCriticalSection(&viewer->gfx.lock);
  last_sent_generation = viewer->gfx.dirty_last_sent_generation;
  diagnostic_full_frame_dirty =
      server->viewer_gfx_diagnostic_full_frame_dirty &&
      viewer->gfx.initialized && viewer->gfx.use_rdpgfx;
  if (!viewer_gfx_pipeline_pending_dirty_move_locked(&viewer->gfx,
                                                     &dirty_batch)) {
    LeaveCriticalSection(&viewer->gfx.lock);
    return TRUE;
  }
  LeaveCriticalSection(&viewer->gfx.lock);

  if (!viewer_framebuffer_snapshot(&server->framebuffer, &snapshot)) {
    EnterCriticalSection(&viewer->gfx.lock);
    (void)viewer_gfx_pipeline_pending_dirty_remerge_locked(
        &viewer->gfx, &dirty_batch, dirty_batch.width, dirty_batch.height);
    LeaveCriticalSection(&viewer->gfx.lock);
    viewer_classic_queues_signal(&viewer->classic_queues);
    return TRUE;
  }

  if (!viewer_gfx_pipeline_snapshot_apply_pending_dirty(&snapshot,
                                                        &dirty_batch)) {
    viewer_framebuffer_snapshot_free(&snapshot);
    EnterCriticalSection(&viewer->gfx.lock);
    (void)viewer_gfx_pipeline_pending_dirty_remerge_locked(
        &viewer->gfx, &dirty_batch, dirty_batch.width, dirty_batch.height);
    LeaveCriticalSection(&viewer->gfx.lock);
    viewer_classic_queues_signal(&viewer->classic_queues);
    return TRUE;
  }

  snapshot_generation = snapshot.generation;
  original_dirty_rect_count = snapshot.dirty_rect_count;
  snapshot_dirty_rect_count = snapshot.dirty_rect_count;

  if (dirty_batch.full_frame_reason &&
      ((strcmp(dirty_batch.full_frame_reason,
               "pending rectangle count threshold") == 0) ||
       (strcmp(dirty_batch.full_frame_reason, "pending area threshold") ==
        0))) {
    WLog_INFO(TAG,
              "Viewer %u RDPEGFX threshold full-frame dirty fallback: "
              "generation=%" PRIu64 " reason=%s width=%u height=%u",
              viewer->id, snapshot_generation, dirty_batch.full_frame_reason,
              dirty_batch.width, dirty_batch.height);
  }

  if (diagnostic_full_frame_dirty) {
    if (viewer_publisher_make_full_frame_dirty(&snapshot)) {
      WLog_INFO(TAG,
                "Viewer %u RDPEGFX diagnostic full-frame dirty forced: "
                "generation=%" PRIu64 " original_dirty_rects=%u width=%u "
                "height=%u",
                viewer->id, snapshot_generation, original_dirty_rect_count,
                snapshot.width, snapshot.height);
    } else {
      WLog_DBG(TAG,
               "Viewer %u RDPEGFX diagnostic full-frame dirty skipped: "
               "generation=%" PRIu64 " original_dirty_rects=%u width=%u "
               "height=%u",
               viewer->id, snapshot_generation, original_dirty_rect_count,
               snapshot.width, snapshot.height);
    }
    snapshot_dirty_rect_count = snapshot.dirty_rect_count;
  }

  if (!viewer_gfx_pipeline_dirty_update_allowed(server, viewer, &snapshot,
                                                &reason)) {
    UINT64 pending_area = 0;
    UINT64 pending_start_generation = 0;
    UINT64 pending_latest_generation = 0;
    UINT64 fallback_count = 0;
    UINT64 remerge_count = 0;
    UINT32 pending_rect_count = 0;

    viewer_framebuffer_snapshot_free(&snapshot);
    EnterCriticalSection(&viewer->gfx.lock);
    (void)viewer_gfx_pipeline_pending_dirty_remerge_locked(
        &viewer->gfx, &dirty_batch, dirty_batch.width, dirty_batch.height);
    pending_rect_count = viewer->gfx.pending_dirty_rect_count;
    pending_area = viewer->gfx.pending_dirty_area;
    pending_start_generation = viewer->gfx.pending_dirty_start_generation;
    pending_latest_generation = viewer->gfx.pending_dirty_latest_generation;
    fallback_count = viewer->gfx.dirty_diag_full_frame_fallbacks;
    remerge_count = viewer->gfx.dirty_diag_remerges;
    LeaveCriticalSection(&viewer->gfx.lock);
    WLog_DBG(TAG,
             "Viewer %u RDPEGFX dirty update not sent: reason=%s "
             "generation=%" PRIu64 " last_sent_generation=%" PRIu64
             " dirty_rects=%u moved_batch_generation=%" PRIu64
             " pending_dirty_rects=%u pending_area=%" PRIu64
             " pending_start_generation=%" PRIu64
             " pending_latest_generation=%" PRIu64
             " full_frame_fallbacks=%" PRIu64 " remerges=%" PRIu64,
             viewer->id, reason ? reason : "not allowed", snapshot_generation,
             last_sent_generation, snapshot_dirty_rect_count,
             dirty_batch.latest_generation, pending_rect_count, pending_area,
             pending_start_generation, pending_latest_generation,
             fallback_count, remerge_count);
    viewer_classic_queues_signal(&viewer->classic_queues);
    return TRUE;
  }

  send_status =
      viewer_gfx_pipeline_send_dirty_update_result(server, viewer, &snapshot);
  viewer_framebuffer_snapshot_free(&snapshot);

  if (send_status == VIEWER_GFX_DIRTY_SEND_FAILED)
    return viewer_gfx_handle_failure(server, viewer, now,
                                     "RDPEGFX dirty update send failed");

  if (send_status == VIEWER_GFX_DIRTY_SEND_DEFERRED) {
    BOOL forced_full_frame = FALSE;
    UINT64 fallback_generation = 0;
    UINT64 pending_area = 0;
    UINT64 pending_start_generation = 0;
    UINT64 pending_latest_generation = 0;
    UINT64 fallback_count = 0;
    UINT64 remerge_count = 0;
    UINT32 fallback_width = 0;
    UINT32 fallback_height = 0;
    UINT32 pending_rect_count = 0;

    EnterCriticalSection(&viewer->gfx.lock);
    (void)viewer_gfx_pipeline_pending_dirty_remerge_locked(
        &viewer->gfx, &dirty_batch, dirty_batch.width, dirty_batch.height);
    forced_full_frame =
        viewer_gfx_pipeline_note_dirty_deferred_locked(&viewer->gfx);
    if (forced_full_frame) {
      fallback_generation = viewer->gfx.pending_dirty_latest_generation;
      fallback_width = viewer->gfx.pending_dirty_width;
      fallback_height = viewer->gfx.pending_dirty_height;
    }
    pending_rect_count = viewer->gfx.pending_dirty_rect_count;
    pending_area = viewer->gfx.pending_dirty_area;
    pending_start_generation = viewer->gfx.pending_dirty_start_generation;
    pending_latest_generation = viewer->gfx.pending_dirty_latest_generation;
    fallback_count = viewer->gfx.dirty_diag_full_frame_fallbacks;
    remerge_count = viewer->gfx.dirty_diag_remerges;
    LeaveCriticalSection(&viewer->gfx.lock);
    WLog_DBG(TAG,
             "Viewer %u RDPEGFX dirty update deferred: reason=%s "
             "generation=%" PRIu64 " sent_generation=%" PRIu64
             " last_sent_generation=%" PRIu64 " dirty_rects=%u"
             " moved_batch_generation=%" PRIu64
             " pending_dirty_rects=%u pending_area=%" PRIu64
             " pending_start_generation=%" PRIu64
             " pending_latest_generation=%" PRIu64
             " full_frame_fallbacks=%" PRIu64 " remerges=%" PRIu64,
             viewer->id, "dirty send deferred", snapshot_generation,
             snapshot_generation, last_sent_generation,
             snapshot_dirty_rect_count, dirty_batch.latest_generation,
             pending_rect_count, pending_area, pending_start_generation,
             pending_latest_generation, fallback_count, remerge_count);
    if (forced_full_frame) {
      WLog_INFO(TAG,
                "Viewer %u RDPEGFX threshold full-frame dirty fallback: "
                "generation=%" PRIu64
                " reason=consecutive deferred dirty sends width=%u height=%u",
                viewer->id, fallback_generation, fallback_width,
                fallback_height);
    }
    viewer_classic_queues_signal(&viewer->classic_queues);
  }

  return TRUE;
}

static BOOL viewer_gfx_step_join(ViewerServer *server, Viewer *viewer,
                                 UINT64 now) {
  ViewerGfxJoinResult result = {0};

  if (!server || !viewer)
    return FALSE;

  viewer_gfx_pipeline_step_join(server, viewer, now, &result);
  if (result.actions & VIEWER_GFX_JOIN_ACTION_ENTER_CLASSIC_FALLBACK)
    return viewer_gfx_handle_failure(server, viewer, now,
                                     result.classic_fallback_reason);

  if (result.actions & VIEWER_GFX_JOIN_ACTION_SEND_BASELINE)
    return viewer_gfx_send_framebuffer_baseline(server, viewer, now);

  return TRUE;
}

static BOOL on_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
  Viewer *viewer = find_viewer_by_peer(input->context->peer);
  if (!viewer || !can_viewer_send_input(viewer))
    return TRUE;

  BOOL is_move = (flags & 0x0800) != 0; /* PTR_FLAGS_MOVE */
  WLog_INFO(TAG, "VIEWER mouse: flags=0x%04X x=%u y=%u%s", flags, x, y,
            is_move ? "" : " [non-move]");
  freerdp_input_send_mouse_event(g_viewer_server->backend->context->input,
                                 flags, x, y);
  if (is_move)
    backend_store_pointer_position(g_viewer_server->backend, x, y);
  return TRUE;
}

static BOOL on_extended_mouse_event(rdpInput *input, UINT16 flags, UINT16 x,
                                    UINT16 y) {
  Viewer *viewer = find_viewer_by_peer(input->context->peer);
  if (!viewer || !can_viewer_send_input(viewer))
    return TRUE;

  BOOL is_move = (flags & 0x0800) != 0; /* PTR_FLAGS_MOVE */
  WLog_INFO(TAG, "VIEWER extended_mouse: flags=0x%04X x=%u y=%u%s", flags, x, y,
            is_move ? "" : " [non-move]");
  freerdp_input_send_extended_mouse_event(
      g_viewer_server->backend->context->input, flags, x, y);
  if (is_move)
    backend_store_pointer_position(g_viewer_server->backend, x, y);
  return TRUE;
}

static BOOL on_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code) {
  Viewer *viewer = find_viewer_by_peer(input->context->peer);
  if (!viewer || !can_viewer_send_input(viewer))
    return TRUE;

  freerdp_input_send_keyboard_event(g_viewer_server->backend->context->input,
                                    flags, code);
  return TRUE;
}

static DWORD WINAPI viewer_handle_peer(LPVOID arg) {
  Viewer *viewer = (Viewer *)arg;
  freerdp_peer *peer = viewer->peer;
  rdpContext *context = viewer->context;
  BOOL cleanup_slot = FALSE;
  HANDLE gfx_event = NULL;
  DWORD wait_timeout_ms = INFINITE;

  LOG_I("viewer_server", "Viewer peer accepted (peer=%p)", (void *)peer);

  while (!viewer->stop_requested) {
    UINT64 now = platform_get_timestamp_ms();
    BOOL lag_signal_active = FALSE;
    BOOL write_blocked = FALSE;
    BYTE drdynvc_state = DRDYNVC_STATE_NONE;
    HANDLE wait_objects[MAXIMUM_WAIT_OBJECTS] = {0};
    DWORD wait_count = 0;
    DWORD wait_status = WAIT_FAILED;
    ViewerGfxPipelineCapsResult caps_result = {0};
    BOOL caps_enter_classic_fallback = FALSE;
    const char *caps_classic_fallback_reason = NULL;

    EnterCriticalSection(&viewer->send_lock);
    if (viewer->needs_full_refresh && (viewer->full_refresh_deadline_ts > 0) &&
        (now >= viewer->full_refresh_deadline_ts)) {
      viewer->full_refresh_deadline_ts = 0;
      if (viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx) &&
          (viewer->gfx.join_strategy ==
           VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK)) {
        LeaveCriticalSection(&viewer->send_lock);
        viewer_gfx_reject_join(viewer,
                               "classic fallback full refresh timed out");
        break;
      }
    }
    LeaveCriticalSection(&viewer->send_lock);

    EnterCriticalSection(&viewer->gfx.lock);
    if (viewer_gfx_pending_activation_timeout_due(
            &viewer->gfx, now, VIEWER_RDPEGFX_NEGOTIATION_TIMEOUT_MS)) {
      LeaveCriticalSection(&viewer->gfx.lock);
      (void)viewer_gfx_enter_classic_fallback(g_viewer_server, viewer, now,
                                              "RDPEGFX negotiation timed out");
      continue;
    }
    LeaveCriticalSection(&viewer->gfx.lock);

    write_blocked = peer->IsWriteBlocked && peer->IsWriteBlocked(peer);
    EnterCriticalSection(&viewer->send_lock);
    if (viewer_is_slow(viewer->consecutive_lag_intervals, write_blocked))
      viewer->consecutive_lag_intervals++;
    else
      viewer->consecutive_lag_intervals = 0;

    lag_signal_active = viewer_lag_signal_active(viewer, write_blocked);

    if (lag_signal_active) {
      if (viewer->sustained_lag_start_ts == 0)
        viewer->sustained_lag_start_ts = now;
    } else {
      viewer->sustained_lag_start_ts = 0;
    }
    LeaveCriticalSection(&viewer->send_lock);

    if (g_viewer_server->slow_viewer_disconnect_enabled &&
        viewer_disconnect_due(viewer,
                              g_viewer_server->slow_viewer_disconnect_ms, now))
      break;

    EnterCriticalSection(&viewer->gfx.lock);
    if (viewer->gfx.join_state == VIEWER_JOIN_STATE_PENDING)
      wait_timeout_ms = 50;
    else {
      /* Use a short periodic timeout even when LIVE so that the GFX
       *
       * pump runs regularly to drain queued frame events. Without this,
 * the
       * viewer thread blocks indefinitely waiting for mstsc input
       *
       * (keyboard/mouse) and queued GFX frames never get sent. A 50ms
       *
       * timeout matches the join-state polling interval and keeps frame

       * * delivery responsive. */
      wait_timeout_ms = 50;
    }
    LeaveCriticalSection(&viewer->gfx.lock);

    if (peer && peer->GetEventHandles) {
      wait_count =
          peer->GetEventHandles(peer, wait_objects, MAXIMUM_WAIT_OBJECTS);
      if (wait_count == 0) {
        WLog_ERR(TAG, "Viewer %u failed to get FreeRDP transport event handles",
                 viewer->id);
        break;
      }

      /* Add classic_event to the wait set so the viewer thread wakes
       *
       * immediately when a bitmap update is enqueued (Option B). */
      if (viewer_classic_queues_event(&viewer->classic_queues) &&
          (wait_count < MAXIMUM_WAIT_OBJECTS)) {
        wait_objects[wait_count] =
            viewer_classic_queues_event(&viewer->classic_queues);
        wait_count++;
      }

      wait_status = WaitForMultipleObjects(wait_count, wait_objects, FALSE,
                                           wait_timeout_ms);
      if (wait_status == WAIT_TIMEOUT)
        wait_status = WAIT_OBJECT_0;
      if (wait_status == WAIT_FAILED)
        break;

      /* Reset the classic event signal — we'll drain the queue below */
      viewer_classic_queues_reset_event(&viewer->classic_queues);
    } else if (peer && peer->CheckFileDescriptor) {
      if (!peer->CheckFileDescriptor(peer))
        break;
      platform_sleep_ms(1);
      continue;
    } else {
      platform_sleep_ms(1);
      continue;
    }

    if (peer->CheckFileDescriptor && !peer->CheckFileDescriptor(peer))
      break;

    EnterCriticalSection(&viewer->gfx.lock);
    if (viewer->activated && viewer->gfx.vcm &&
        (viewer->gfx.vcm != INVALID_HANDLE_VALUE) &&
        viewer->gfx.post_connect_complete &&
        WTSVirtualChannelManagerIsChannelJoined(viewer->gfx.vcm,
                                                DRDYNVC_SVC_CHANNEL_NAME)) {
      /* Call CheckFileDescriptor every iteration to drain the VCM message

       * * queue and trigger drdynvc auto-initialization.  This matches the

       * * pattern used by FreeRDP shadow and by the proxy server. */
      if (!WTSVirtualChannelManagerCheckFileDescriptor(viewer->gfx.vcm)) {
        LeaveCriticalSection(&viewer->gfx.lock);
        break;
      }

      drdynvc_state = WTSVirtualChannelManagerGetDrdynvcState(viewer->gfx.vcm);
      if (drdynvc_state != viewer->gfx.drdynvc_state) {
        viewer->gfx.drdynvc_state = drdynvc_state;
        WLog_INFO(TAG, "Viewer %u drdynvc state changed -> %u", viewer->id,
                  (unsigned)drdynvc_state);
      }

      if (drdynvc_state == DRDYNVC_STATE_READY) {
        if (!viewer_gfx_pipeline_open_if_ready_locked(viewer)) {
          LeaveCriticalSection(&viewer->gfx.lock);
          (void)viewer_gfx_handle_failure(g_viewer_server, viewer, now,
                                          "RDPEGFX channel open failed");
          continue;
        }
      }

      gfx_event = viewer_gfx_pipeline_get_event_handle_locked(viewer);
    } else {
      gfx_event = NULL;
    }
    LeaveCriticalSection(&viewer->gfx.lock);

    if (gfx_event && (WaitForSingleObject(gfx_event, 0) == WAIT_OBJECT_0)) {
      EnterCriticalSection(&viewer->gfx.lock);
      if (!viewer_gfx_pipeline_handle_messages_locked(viewer, &caps_result)) {
        LeaveCriticalSection(&viewer->gfx.lock);
        WLog_WARN(TAG,
                  "Viewer %u RDPEGFX message handling failed; applying "
                  "failure policy",
                  viewer->id);
        (void)viewer_gfx_handle_failure(g_viewer_server, viewer, now,
                                        "RDPEGFX message handling failed");
        continue;
      }
      viewer_gfx_apply_caps_result_locked(g_viewer_server, viewer, &caps_result,
                                          now, &caps_enter_classic_fallback,
                                          &caps_classic_fallback_reason);
      LeaveCriticalSection(&viewer->gfx.lock);
      if (caps_enter_classic_fallback) {
        (void)viewer_gfx_handle_failure(g_viewer_server, viewer, now,
                                        caps_classic_fallback_reason);
        continue;
      }
    }

    if (g_viewer_server && !viewer_gfx_step_join(g_viewer_server, viewer, now))
      break;

    if (g_viewer_server &&
        !viewer_gfx_try_send_dirty_update(g_viewer_server, viewer, now))
      break;

    if (!viewer_pump_gfx(viewer))
      break;

    /* Drain the classic bitmap queue (Option B: async delivery) */
    if (!viewer_pump_classic(viewer))
      break;

    // Ongoing pointer forwarding applies to activated classic and RDPEGFX
    // viewers; viewer_forward_pointer uses pointer generation checks and the
    // pointer transport boundary to avoid resending unchanged shape/position
    // state.
    (void)viewer_forward_pointer(viewer, FALSE);

    if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer) &&
        peer->DrainOutputBuffer) {
      if (peer->DrainOutputBuffer(peer) < 0)
        break;
    }
  }

  if (g_viewer_server) {
    EnterCriticalSection(&g_viewer_server->lock);
    viewer_release_count_locked(g_viewer_server, viewer,
                                "viewer peer handler disconnect");
    LeaveCriticalSection(&g_viewer_server->lock);
  }

  LOG_I("viewer_server", "Viewer peer disconnected (peer=%p)", (void *)peer);
  peer->Disconnect(peer);

  {
    ViewerServer *server = g_viewer_server;
    if (server) {
      EnterCriticalSection(&server->lock);
      cleanup_slot =
          viewer_try_begin_cleanup_locked(viewer, peer, context, FALSE, FALSE);
      LeaveCriticalSection(&server->lock);
    }

    if (cleanup_slot)
      viewer_cleanup_slot(server, viewer);
  }

  return 0;
}

static BOOL peer_post_connect(freerdp_peer *peer) {
  Viewer *viewer = find_viewer_by_peer(peer);
  BOOL gfx_enabled = FALSE;

  if (!viewer)
    return FALSE;

  viewer->connected = TRUE;
  viewer->activated = peer->activated;

  if (peer->context && peer->context->settings)
    gfx_enabled = freerdp_settings_get_bool(peer->context->settings,
                                            FreeRDP_SupportGraphicsPipeline);

  if (g_viewer_server && !g_viewer_server->viewer_gfx_enabled)
    gfx_enabled = FALSE;

  WLog_INFO(TAG, "Viewer %u peer_post_connect: GraphicsPipeline=%d", viewer->id,
            gfx_enabled);

  EnterCriticalSection(&viewer->gfx.lock);
  viewer_graphics_context_reset(
      &viewer->gfx, g_viewer_server ? g_viewer_server->backend : NULL);

  if (!viewer_gfx_pipeline_post_connect_locked(g_viewer_server, viewer, peer,
                                               gfx_enabled)) {
    LeaveCriticalSection(&viewer->gfx.lock);
    return FALSE;
  }
  LeaveCriticalSection(&viewer->gfx.lock);
  return TRUE;
}

static BOOL on_viewer_logon(freerdp_peer *peer,
                            const SEC_WINNT_AUTH_IDENTITY *identity,
                            BOOL automatic) {
  ViewerServer *server = g_viewer_server;
  ViewerAuthCredentials identity_credentials = {0};
  ViewerAuthCredentials settings_credentials = {0};
  ViewerAuthCredentials selected_credentials = {0};
  ViewerAuthSelection selection = {0};
  Viewer *viewer = NULL;
  const char *credential_source = "none";
  const char *viewer_user = "";
  const char *viewer_domain = "";
  const char *viewer_password = "";
  BOOL accepted = FALSE;

  if (!peer || !server) {
    WLog_WARN(TAG, "Viewer-side logon rejected: missing peer or server");
    return FALSE;
  }

  WLog_INFO(TAG, "Viewer-side logon: auth_mode=%s nla_enabled=%s",
            viewer_auth_mode_name(server->security.auth_mode),
            server->security.nla_enabled ? "true" : "false");

  viewer = find_viewer_by_peer(peer);

  if (server->security.auth_mode == VIEWER_AUTH_MODE_NONE) {
    if (viewer)
      viewer->auth_state = VIEWER_AUTH_STATE_ACCEPTED;
    return TRUE;
  }

  if ((server->security.auth_mode == VIEWER_AUTH_MODE_BACKEND_CREDENTIALS) &&
      !server->backend) {
    WLog_WARN(TAG,
              "Viewer-side logon rejected: auth_mode=%s nla_enabled=%s "
              "missing backend credentials source",
              viewer_auth_mode_name(server->security.auth_mode),
              server->security.nla_enabled ? "true" : "false");
    return FALSE;
  }

  if (viewer_auth_should_defer_backend_credentials(server->security.nla_enabled,
                                                   automatic)) {
    if (!viewer) {
      WLog_WARN(TAG,
                "Viewer-side logon rejected: unable to defer auth without "
                "viewer slot auth_mode=%s nla_enabled=false",
                viewer_auth_mode_name(server->security.auth_mode));
      return FALSE;
    }

    viewer->auth_state = VIEWER_AUTH_STATE_DEFERRED;
    viewer->auth_deferred_required = TRUE;
    viewer->auth_deferred_checked = FALSE;
    viewer->auth_deferred_accepted = FALSE;
    WLog_INFO(TAG,
              "Viewer-side logon provisionally accepted pending deferred "
              "settings credentials auth_mode=%s nla_enabled=false "
              "credential_source=settings",
              viewer_auth_mode_name(server->security.auth_mode));
    return TRUE;
  }

  (void)viewer_identity_credentials_to_utf8(identity, &identity_credentials);
  (void)viewer_settings_credentials_to_utf8(peer, &settings_credentials);

  selection = viewer_auth_select_credentials(&identity_credentials,
                                             &settings_credentials,
                                             server->security.nla_enabled);
  credential_source = selection.source ? selection.source : "none";
  if (!selection.credentials) {
    WLog_WARN(
        TAG,
        "Viewer-side logon rejected: no usable credentials auth_mode=%s "
        "nla_enabled=%s credential_source=%s identity_username_present=%s "
        "identity_domain_present=%s identity_password_present=%s "
        "settings_username_present=%s settings_domain_present=%s "
        "settings_password_present=%s",
        viewer_auth_mode_name(server->security.auth_mode),
        server->security.nla_enabled ? "true" : "false", credential_source,
        viewer_string_has_value(identity_credentials.username) ? "true"
                                                               : "false",
        viewer_string_has_value(identity_credentials.domain) ? "true" : "false",
        viewer_string_has_value(identity_credentials.password) ? "true"
                                                               : "false",
        viewer_string_has_value(settings_credentials.username) ? "true"
                                                               : "false",
        viewer_string_has_value(settings_credentials.domain) ? "true" : "false",
        viewer_string_has_value(settings_credentials.password) ? "true"
                                                               : "false");
    goto out;
  }

  selected_credentials.username = _strdup(selection.credentials->username);
  selected_credentials.domain = _strdup(
      selection.credentials->domain ? selection.credentials->domain : "");
  selected_credentials.password = _strdup(selection.credentials->password);
  if (!selected_credentials.username || !selected_credentials.domain ||
      !selected_credentials.password) {
    WLog_WARN(TAG,
              "Viewer-side logon rejected: failed to copy credentials "
              "auth_mode=%s nla_enabled=%s credential_source=%s",
              viewer_auth_mode_name(server->security.auth_mode),
              server->security.nla_enabled ? "true" : "false",
              credential_source);
    goto out;
  }

  if (!viewer_auth_normalize_domain_user(&selected_credentials)) {
    WLog_WARN(TAG,
              "Viewer-side logon rejected: failed to normalize credentials "
              "auth_mode=%s nla_enabled=%s credential_source=%s",
              viewer_auth_mode_name(server->security.auth_mode),
              server->security.nla_enabled ? "true" : "false",
              credential_source);
    goto out;
  }

  viewer_user =
      selected_credentials.username ? selected_credentials.username : "";
  viewer_domain =
      selected_credentials.domain ? selected_credentials.domain : "";
  viewer_password =
      selected_credentials.password ? selected_credentials.password : "";

  accepted = viewer_backend_credentials_match(server->backend, viewer_user,
                                              viewer_domain, viewer_password);
  if (accepted) {
    if (viewer)
      viewer->auth_state = VIEWER_AUTH_STATE_ACCEPTED;
    WLog_INFO(TAG,
              "Viewer-side logon accepted auth_mode=%s nla_enabled=%s "
              "credential_source=%s username_present=%s domain_present=%s "
              "password_present=%s",
              viewer_auth_mode_name(server->security.auth_mode),
              server->security.nla_enabled ? "true" : "false",
              credential_source, viewer_user[0] ? "true" : "false",
              viewer_domain[0] ? "true" : "false",
              viewer_password[0] ? "true" : "false");
  } else {
    if (viewer)
      viewer->auth_state = VIEWER_AUTH_STATE_REJECTED;
    WLog_WARN(TAG,
              "Viewer-side logon rejected: credentials mismatch auth_mode=%s "
              "nla_enabled=%s credential_source=%s username_present=%s "
              "domain_present=%s password_present=%s",
              viewer_auth_mode_name(server->security.auth_mode),
              server->security.nla_enabled ? "true" : "false",
              credential_source, viewer_user[0] ? "true" : "false",
              viewer_domain[0] ? "true" : "false",
              viewer_password[0] ? "true" : "false");
  }

out:
  if (!accepted && viewer && (viewer->auth_state != VIEWER_AUTH_STATE_ACCEPTED))
    viewer->auth_state = VIEWER_AUTH_STATE_REJECTED;
  viewer_auth_credentials_clear(&identity_credentials);
  viewer_auth_credentials_clear(&settings_credentials);
  viewer_auth_credentials_clear(&selected_credentials);
  return accepted;
}

static BOOL peer_activate(freerdp_peer *peer) {
  Viewer *viewer = find_viewer_by_peer(peer);
  UINT64 now = platform_get_timestamp_ms();
  ViewerGfxJoinResult join_result = {0};
  ViewerGfxNegotiationOutcome negotiation_outcome =
      VIEWER_GFX_NEGOTIATION_PENDING;
  BOOL classic_activation = FALSE;
  BOOL suppress_activation_pointer = FALSE;

  if (!viewer)
    return FALSE;

  viewer->activated = TRUE;
  EnterCriticalSection(&viewer->send_lock);
  viewer->needs_full_refresh = TRUE;
  viewer->full_refresh_deadline_ts =
      platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;
  LeaveCriticalSection(&viewer->send_lock);

  if (!viewer_gfx_pipeline_activate(g_viewer_server, viewer))
    return FALSE;

  viewer_gfx_pipeline_on_peer_activated(viewer, now, &join_result);
  if (join_result.actions & VIEWER_GFX_JOIN_ACTION_ENQUEUE_CLASSIC_BASELINE) {
    EnterCriticalSection(&viewer->send_lock);
    viewer->needs_full_refresh = FALSE;
    viewer->full_refresh_deadline_ts = 0;
    LeaveCriticalSection(&viewer->send_lock);
    classic_activation = TRUE;
  }
  EnterCriticalSection(&viewer->gfx.lock);
  negotiation_outcome = viewer->gfx.negotiation_outcome;
  suppress_activation_pointer =
      g_viewer_server && g_viewer_server->viewer_gfx_enabled &&
      (viewer->gfx.join_state == VIEWER_JOIN_STATE_PENDING) &&
      (viewer->gfx.join_strategy != VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK);
  LeaveCriticalSection(&viewer->gfx.lock);
  if (classic_activation && g_viewer_server &&
      viewer_enqueue_classic_baseline_from_framebuffer(g_viewer_server,
                                                       viewer)) {
    WLog_INFO(TAG, "Viewer %u queued framebuffer baseline for classic join",
              viewer->id);
  }
  viewer->last_pointer_position_generation = 0;
  viewer->last_pointer_shape_generation = 0;

  WLog_INFO(TAG, "Viewer %u connecting mid-session", viewer->id);
  if (negotiation_outcome == VIEWER_GFX_NEGOTIATION_RDPEGFX_READY)
    WLog_INFO(TAG,
              "Viewer %u RDPEGFX activated; evaluating framebuffer baseline "
              "late join",
              viewer->id);

  if (g_viewer_server && g_viewer_server->backend) {
    if ((negotiation_outcome == VIEWER_GFX_NEGOTIATION_RDPEGFX_READY) &&
        g_viewer_server->viewer_gfx_enabled) {
      WLog_INFO(
          TAG,
          "Viewer %u RDPEGFX activated; awaiting handshake-gated late join",
          viewer->id);
    } else if (backend_full_refresh_in_flight(g_viewer_server->backend)) {
      WLog_INFO(TAG, "Viewer %u activated, joining in-flight full refresh",
                viewer->id);
    } else {
      (void)backend_request_full_refresh(g_viewer_server->backend);
      WLog_INFO(TAG, "Viewer %u activated, queued full refresh", viewer->id);
    }
  }

  if (!suppress_activation_pointer)
    (void)viewer_forward_pointer(viewer, TRUE);
  return TRUE;
}

static BOOL peer_context_new(freerdp_peer *peer, rdpContext *context) {
  ViewerServer *server = g_viewer_server;
  Viewer *viewer = NULL;

  if (!server || !peer || !context)
    return FALSE;

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    if (viewer_slot_available_locked(&server->viewers[i])) {
      viewer = &server->viewers[i];
      viewer->id = i + 1;
      viewer->peer = peer;
      viewer->context = context;
      viewer->connected = FALSE;
      viewer->activated = FALSE;
      viewer->counted_in_viewer_count = FALSE;
      viewer->cleanup_in_progress = FALSE;
      viewer_auth_state_reset(viewer);
      viewer->publish_ref_count = 0;
      viewer->needs_full_refresh = FALSE;
      viewer->stop_requested = FALSE;
      viewer->connect_time = time(NULL);
      viewer->full_refresh_deadline_ts = 0;
      if (!viewer_graphics_context_init(&viewer->gfx)) {
        viewer->peer = NULL;
        viewer->context = NULL;
        viewer->counted_in_viewer_count = FALSE;
        viewer->cleanup_in_progress = FALSE;
        viewer_auth_state_reset(viewer);
        viewer->publish_ref_count = 0;
        viewer = NULL;
        break;
      }
      if (!viewer_gfx_pipeline_init(viewer)) {
        viewer_graphics_context_uninit(&viewer->gfx);
        viewer->peer = NULL;
        viewer->context = NULL;
        viewer->counted_in_viewer_count = FALSE;
        viewer->cleanup_in_progress = FALSE;
        viewer_auth_state_reset(viewer);
        viewer->publish_ref_count = 0;
        viewer = NULL;
        break;
      }
      if (!viewer_send_state_init(viewer)) {
        viewer_gfx_pipeline_uninit(viewer);
        viewer_graphics_context_uninit(&viewer->gfx);
        viewer->peer = NULL;
        viewer->context = NULL;
        viewer->counted_in_viewer_count = FALSE;
        viewer->cleanup_in_progress = FALSE;
        viewer_auth_state_reset(viewer);
        viewer->publish_ref_count = 0;
        viewer = NULL;
        break;
      }
      viewer_graphics_context_reset(&viewer->gfx, server->backend);
      /* VCM created later in peer_post_connect when context->rdp is ready */

      server->viewer_count++;
      viewer->counted_in_viewer_count = TRUE;
      break;
    }
  }
  LeaveCriticalSection(&server->lock);
  return viewer != NULL;
}

static void peer_context_free(freerdp_peer *peer, rdpContext *context) {
  ViewerServer *server = g_viewer_server;
  Viewer *matched_viewer = NULL;
  Viewer *cleanup_viewer = NULL;

  if (!server)
    return;

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    if (viewer_matches_peer_context_locked(&server->viewers[i], peer,
                                           context)) {
      viewer_release_count_locked(server, &server->viewers[i],
                                  "FreeRDP context free");
      matched_viewer = &server->viewers[i];
      break;
    }
  }
  LeaveCriticalSection(&server->lock);

  if (!matched_viewer)
    return;

  viewer_wait_for_publish_refs(server, matched_viewer);

  EnterCriticalSection(&server->lock);
  if (viewer_try_begin_cleanup_locked(matched_viewer, peer, context, TRUE,
                                      FALSE))
    cleanup_viewer = matched_viewer;
  LeaveCriticalSection(&server->lock);

  if (cleanup_viewer)
    viewer_cleanup_slot(server, cleanup_viewer);
}

/**
 * Re-apply server-side settings that were overwritten by GCC negotiation.

 * *
 * When a viewer client connects, FreeRDP's gcc_read_client_core_data()
 *
 * overwrites our server-side settings with the CLIENT's values:
 *   -
 * DesktopWidth/Height → client's screen size (e.g. 1920×1080)
 *   -
 * MonitorCount → client's monitor count (e.g. 1)
 *   - MonitorDefArray →
 * client's monitor layout (e.g. single 1920×1080)
 *   -
 * SupportMonitorLayoutPdu → AND'd with client's earlyCapabilityFlags
 *   -
 * SupportDynamicTimeZone → AND'd with client's earlyCapabilityFlags
 *
 * We
 * are the SERVER — we must restore our own desktop dimensions and
 * monitor
 * layout so that:
 *   1. The Demand Active PDU advertises the correct desktop
 * size (3840×1080)
 *   2. The Monitor Layout PDU sends the correct 2-monitor
 * layout
 *   3. SupportDynamicTimeZone=TRUE so timezone data is consumed
 * (fixes TPKT
 * error)
 *   4. SupportMonitorLayoutPdu=TRUE so the Monitor
 * Layout PDU is sent
 *
 * This callback fires at
 * CONNECTION_STATE_SECURE_SETTINGS_EXCHANGE,
 * which is AFTER GCC negotiation
 * but BEFORE:
 *   - rdp_recv_client_info() (needs SupportDynamicTimeZone)
 *
 * - CAPABILITIES_EXCHANGE_DEMAND_ACTIVE (needs DesktopWidth/Height)
 *   -
 * CAPABILITIES_EXCHANGE_MONITOR_LAYOUT (needs MonitorCount/DefArray +
 *
 * SupportMonitorLayoutPdu)
 */
static const char *viewer_connection_state_name(CONNECTION_STATE state) {
  switch (state) {
  case CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_REQUEST:
    return "CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_REQUEST";
  case CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_RESPONSE:
    return "CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_RESPONSE";
  case CONNECTION_STATE_LICENSING:
    return "CONNECTION_STATE_LICENSING";
  case CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_REQUEST:
    return "CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_REQUEST";
  case CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_RESPONSE:
    return "CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_RESPONSE";
  case CONNECTION_STATE_CAPABILITIES_EXCHANGE_DEMAND_ACTIVE:
    return "CONNECTION_STATE_CAPABILITIES_EXCHANGE_DEMAND_ACTIVE";
  case CONNECTION_STATE_CAPABILITIES_EXCHANGE_MONITOR_LAYOUT:
    return "CONNECTION_STATE_CAPABILITIES_EXCHANGE_MONITOR_LAYOUT";
  case CONNECTION_STATE_CAPABILITIES_EXCHANGE_CONFIRM_ACTIVE:
    return "CONNECTION_STATE_CAPABILITIES_EXCHANGE_CONFIRM_ACTIVE";
  case CONNECTION_STATE_FINALIZATION_SYNC:
    return "CONNECTION_STATE_FINALIZATION_SYNC";
  case CONNECTION_STATE_FINALIZATION_COOPERATE:
    return "CONNECTION_STATE_FINALIZATION_COOPERATE";
  case CONNECTION_STATE_FINALIZATION_REQUEST_CONTROL:
    return "CONNECTION_STATE_FINALIZATION_REQUEST_CONTROL";
  case CONNECTION_STATE_FINALIZATION_PERSISTENT_KEY_LIST:
    return "CONNECTION_STATE_FINALIZATION_PERSISTENT_KEY_LIST";
  case CONNECTION_STATE_FINALIZATION_FONT_LIST:
    return "CONNECTION_STATE_FINALIZATION_FONT_LIST";
  case CONNECTION_STATE_FINALIZATION_CLIENT_SYNC:
    return "CONNECTION_STATE_FINALIZATION_CLIENT_SYNC";
  case CONNECTION_STATE_FINALIZATION_CLIENT_COOPERATE:
    return "CONNECTION_STATE_FINALIZATION_CLIENT_COOPERATE";
  case CONNECTION_STATE_FINALIZATION_CLIENT_GRANTED_CONTROL:
    return "CONNECTION_STATE_FINALIZATION_CLIENT_GRANTED_CONTROL";
  case CONNECTION_STATE_FINALIZATION_CLIENT_FONT_MAP:
    return "CONNECTION_STATE_FINALIZATION_CLIENT_FONT_MAP";
  case CONNECTION_STATE_ACTIVE:
    return "CONNECTION_STATE_ACTIVE";
  default:
    return "CONNECTION_STATE_BEFORE_CLIENT_INFO";
  }
}

static BOOL viewer_state_is_after_client_info(CONNECTION_STATE state) {
  switch (state) {
  case CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_REQUEST:
  case CONNECTION_STATE_CONNECT_TIME_AUTO_DETECT_RESPONSE:
  case CONNECTION_STATE_LICENSING:
  case CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_REQUEST:
  case CONNECTION_STATE_MULTITRANSPORT_BOOTSTRAPPING_RESPONSE:
  case CONNECTION_STATE_CAPABILITIES_EXCHANGE_DEMAND_ACTIVE:
  case CONNECTION_STATE_CAPABILITIES_EXCHANGE_MONITOR_LAYOUT:
  case CONNECTION_STATE_CAPABILITIES_EXCHANGE_CONFIRM_ACTIVE:
  case CONNECTION_STATE_FINALIZATION_SYNC:
  case CONNECTION_STATE_FINALIZATION_COOPERATE:
  case CONNECTION_STATE_FINALIZATION_REQUEST_CONTROL:
  case CONNECTION_STATE_FINALIZATION_PERSISTENT_KEY_LIST:
  case CONNECTION_STATE_FINALIZATION_FONT_LIST:
  case CONNECTION_STATE_FINALIZATION_CLIENT_SYNC:
  case CONNECTION_STATE_FINALIZATION_CLIENT_COOPERATE:
  case CONNECTION_STATE_FINALIZATION_CLIENT_GRANTED_CONTROL:
  case CONNECTION_STATE_FINALIZATION_CLIENT_FONT_MAP:
  case CONNECTION_STATE_ACTIVE:
    return TRUE;
  default:
    return FALSE;
  }
}

static BOOL viewer_run_deferred_auth_if_ready(freerdp_peer *peer,
                                              CONNECTION_STATE state) {
  ViewerServer *server = g_viewer_server;
  Viewer *viewer = NULL;
  ViewerAuthCredentials settings_credentials = {0};
  const char *viewer_user = "";
  const char *viewer_domain = "";
  const char *viewer_password = "";
  BOOL settings_usable = FALSE;
  BOOL accepted = FALSE;

  if (!server || !peer)
    return TRUE;

  viewer = find_viewer_by_peer(peer);
  if (!viewer)
    return TRUE;

  if (!viewer->auth_deferred_required || viewer->auth_deferred_checked)
    return TRUE;

  if (!viewer_state_is_after_client_info(state))
    return TRUE;

  viewer->auth_deferred_checked = TRUE;

  if (!server->backend) {
    viewer->auth_state = VIEWER_AUTH_STATE_REJECTED;
    viewer->stop_requested = TRUE;
    WLog_WARN(TAG,
              "Deferred viewer auth rejected: missing backend credentials "
              "auth_mode=%s nla_enabled=false credential_source=settings "
              "state=%s",
              viewer_auth_mode_name(server->security.auth_mode),
              viewer_connection_state_name(state));
    return FALSE;
  }

  settings_usable =
      viewer_settings_credentials_to_utf8(peer, &settings_credentials);
  if (settings_usable &&
      !viewer_auth_normalize_domain_user(&settings_credentials))
    settings_usable = FALSE;

  viewer_user =
      settings_credentials.username ? settings_credentials.username : "";
  viewer_domain =
      settings_credentials.domain ? settings_credentials.domain : "";
  viewer_password =
      settings_credentials.password ? settings_credentials.password : "";

  if (settings_usable) {
    accepted = viewer_backend_credentials_match(server->backend, viewer_user,
                                                viewer_domain, viewer_password);
  }

  if (accepted) {
    viewer->auth_state = VIEWER_AUTH_STATE_ACCEPTED;
    viewer->auth_deferred_required = FALSE;
    viewer->auth_deferred_accepted = TRUE;
    WLog_INFO(TAG,
              "Deferred viewer auth accepted auth_mode=%s nla_enabled=false "
              "credential_source=settings username_present=%s "
              "domain_present=%s password_present=%s state=%s",
              viewer_auth_mode_name(server->security.auth_mode),
              viewer_user[0] ? "true" : "false",
              viewer_domain[0] ? "true" : "false",
              viewer_password[0] ? "true" : "false",
              viewer_connection_state_name(state));
    viewer_auth_credentials_clear(&settings_credentials);
    return TRUE;
  }

  viewer->auth_state = VIEWER_AUTH_STATE_REJECTED;
  viewer->auth_deferred_accepted = FALSE;
  viewer->stop_requested = TRUE;
  WLog_WARN(TAG,
            "Deferred viewer auth rejected auth_mode=%s nla_enabled=false "
            "credential_source=settings username_present=%s domain_present=%s "
            "password_present=%s state=%s",
            viewer_auth_mode_name(server->security.auth_mode),
            viewer_user[0] ? "true" : "false",
            viewer_domain[0] ? "true" : "false",
            viewer_password[0] ? "true" : "false",
            viewer_connection_state_name(state));
  viewer_auth_credentials_clear(&settings_credentials);
  return FALSE;
}

static BOOL peer_reached_state(freerdp_peer *peer, CONNECTION_STATE state) {
  ViewerServer *server = NULL;
  const MonitorLayout *layout = NULL;
  rdpSettings *settings = NULL;
  UINT32 i = 0;

  if (!viewer_run_deferred_auth_if_ready(peer, state))
    return FALSE;

  if (state != CONNECTION_STATE_SECURE_SETTINGS_EXCHANGE)
    return TRUE;

  if (!peer || !peer->context || !peer->context->settings)
    return TRUE;

  server = g_viewer_server;
  if (!server || !server->backend)
    return TRUE;

  settings = peer->context->settings;
  layout = &server->backend->monitor_layout;

  WLog_INFO(TAG, "peer_reached_state: Restoring server-side settings after GCC "
                 "negotiation");
  WLog_INFO(TAG,
            "peer_reached_state:   Before: DesktopWidth=%" PRIu32
            ", DesktopHeight=%" PRIu32 ", MonitorCount=%" PRIu32
            ", SupportMonitorLayoutPdu=%s, SupportDynamicTimeZone=%s",
            freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
            freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight),
            freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount),
            freerdp_settings_get_bool(settings, FreeRDP_SupportMonitorLayoutPdu)
                ? "TRUE"
                : "FALSE",
            freerdp_settings_get_bool(settings, FreeRDP_SupportDynamicTimeZone)
                ? "TRUE"
                : "FALSE");

  /* Restore desktop dimensions from backend layout */
  freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
                              layout->total_width);
  freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
                              layout->total_height);

  /* Restore monitor count and layout */
  freerdp_settings_set_uint32(settings, FreeRDP_MonitorCount,
                              layout->monitor_count);

  for (i = 0; i < layout->monitor_count; i++) {
    rdpMonitor mon = {0};
    mon.x = layout->monitors[i].left;
    mon.y = layout->monitors[i].top;
    mon.width = layout->monitors[i].right - layout->monitors[i].left;
    mon.height = layout->monitors[i].bottom - layout->monitors[i].top;
    mon.is_primary =
        (layout->monitors[i].flags & MONITOR_PRIMARY) ? TRUE : FALSE;
    mon.orig_screen = i;
    mon.attributes.physicalWidth = mon.width;
    mon.attributes.physicalHeight = mon.height;
    mon.attributes.orientation = ORIENTATION_LANDSCAPE;
    mon.attributes.desktopScaleFactor = 100;
    mon.attributes.deviceScaleFactor = 100;

    freerdp_settings_set_pointer_array(settings, FreeRDP_MonitorDefArray, i,
                                       &mon);

    WLog_INFO(TAG,
              "peer_reached_state:   monitor[%" PRIu32 "]: x=%" PRId32
              ", y=%" PRId32 ", width=%" PRId32 ", height=%" PRId32
              ", is_primary=%s",
              i, mon.x, mon.y, mon.width, mon.height,
              mon.is_primary ? "TRUE" : "FALSE");
  }

  /* Restore capability flags that were AND'd with client's flags */
  freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
  freerdp_settings_set_bool(settings, FreeRDP_SupportDynamicTimeZone, TRUE);

  WLog_INFO(TAG,
            "peer_reached_state:   After: DesktopWidth=%" PRIu32
            ", DesktopHeight=%" PRIu32 ", MonitorCount=%" PRIu32
            ", SupportMonitorLayoutPdu=TRUE, SupportDynamicTimeZone=TRUE",
            freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
            freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight),
            freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount));

  return TRUE;
}

static BOOL peer_accepted(freerdp_listener *listener, freerdp_peer *peer) {
  ViewerServer *server = g_viewer_server;
  Viewer *viewer = NULL;
  rdpSettings *settings = NULL;

  (void)listener;

  if (!server)
    return FALSE;

  peer->ContextNew = peer_context_new;
  peer->ContextFree = peer_context_free;
  peer->PostConnect = peer_post_connect;
  peer->Activate = peer_activate;
  peer->ReachedState = peer_reached_state;
  peer->Logon = on_viewer_logon;

  if (!freerdp_peer_context_new(peer))
    return FALSE;

  settings = peer->context ? peer->context->settings : NULL;
  if (settings) {
    UINT32 desktop_width = 0;
    UINT32 desktop_height = 0;
    viewer_get_backend_layout(server->backend, &desktop_width, &desktop_height,
                              NULL);

    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity,
                              server->security.rdp_enabled);
    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity,
                              server->security.tls_enabled);
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity,
                              server->security.nla_enabled);
    freerdp_settings_set_uint32(settings, FreeRDP_EncryptionLevel,
                                ENCRYPTION_LEVEL_CLIENT_COMPATIBLE);
    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_NSCodec, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline,
                              server->viewer_gfx_enabled ? TRUE : FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, FALSE);
    /* SurfaceFrameMarkerEnabled must match codec availability. When
     *
     * codecs are disabled, sending SURFACE_FRAME_MARKER PDUs to the
     *
     * client causes a protocol error (0xd06) because the client
     * hasn't
     * negotiated the Surface Bits Capability Set. */
    freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled,
                              FALSE);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, desktop_width);
    freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
                                desktop_height);

    WLog_INFO(TAG, "peer_accepted: Setting desktop_size=%ux%u for viewer",
              desktop_width, desktop_height);
    WLog_INFO(
        TAG, "peer_accepted: viewer security rdp=%s tls=%s nla=%s auth_mode=%s",
        server->security.rdp_enabled ? "true" : "false",
        server->security.tls_enabled ? "true" : "false",
        server->security.nla_enabled ? "true" : "false",
        viewer_auth_mode_name(server->security.auth_mode));

    /* Enable Monitor Layout PDU so the server advertises multi-monitor
     *
     * layout to connecting viewers. Without this, the server only sends
     *
     * a single-monitor desktop even if DesktopWidth > 1920. */
    freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);

    WLog_INFO(
        TAG,
        "peer_accepted: SupportMonitorLayoutPdu=TRUE, desktop_width=%" PRIu32
        " > 1920=%s",
        desktop_width, desktop_width > 1920 ? "TRUE" : "FALSE");

    /* Configure multi-monitor layout for viewer if backend uses more
     *
     * than one monitor. On the server side, we must set MonitorCount
     * and
     * MonitorDefArray (NOT UseMultimon/SpanMonitors which are
     *
     * client-side only). */
    if (desktop_width > 1920) {
      if (server && server->backend) {
        const MonitorLayout *layout = &server->backend->monitor_layout;
        UINT32 mi = 0;

        WLog_INFO(TAG,
                  "peer_accepted: Configuring multi-monitor for viewer: "
                  "MonitorCount=%" PRIu32,
                  layout->monitor_count);

        freerdp_settings_set_uint32(settings, FreeRDP_MonitorCount,
                                    layout->monitor_count);

        for (mi = 0; mi < layout->monitor_count; mi++) {
          rdpMonitor mon = {0};
          mon.x = layout->monitors[mi].left;
          mon.y = layout->monitors[mi].top;
          mon.width = layout->monitors[mi].right - layout->monitors[mi].left;
          mon.height = layout->monitors[mi].bottom - layout->monitors[mi].top;
          mon.is_primary =
              (layout->monitors[mi].flags & MONITOR_PRIMARY) ? TRUE : FALSE;
          mon.orig_screen = mi;
          mon.attributes.physicalWidth = mon.width;
          mon.attributes.physicalHeight = mon.height;
          mon.attributes.orientation = ORIENTATION_LANDSCAPE;
          mon.attributes.desktopScaleFactor = 100;
          mon.attributes.deviceScaleFactor = 100;

          WLog_INFO(TAG,
                    "peer_accepted: viewer monitor[%" PRIu32 "]: x=%" PRId32
                    ", y=%" PRId32 ", width=%" PRId32 ", height=%" PRId32
                    ", is_primary=%s, orig_screen=%" PRIu32,
                    mi, mon.x, mon.y, mon.width, mon.height,
                    mon.is_primary ? "TRUE" : "FALSE", mon.orig_screen);

          freerdp_settings_set_pointer_array(settings, FreeRDP_MonitorDefArray,
                                             mi, &mon);
        }

        {
          UINT32 monCountAfter =
              freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
          UINT32 k = 0;
          WLog_INFO(TAG,
                    "peer_accepted: After setting MonitorDefArray: "
                    "MonitorCount=%" PRIu32,
                    monCountAfter);
          for (k = 0; k < monCountAfter; k++) {
            const rdpMonitor *m =
                (const rdpMonitor *)freerdp_settings_get_pointer_array(
                    settings, FreeRDP_MonitorDefArray, k);
            if (m)
              WLog_INFO(TAG,
                        "peer_accepted:   viewer settings[%" PRIu32
                        "]: x=%" PRId32 ", y=%" PRId32 ", width=%" PRId32
                        ", height=%" PRId32 ", is_primary=%s",
                        k, m->x, m->y, m->width, m->height,
                        m->is_primary ? "TRUE" : "FALSE");
          }
        }
      }
    }

    {
      const char *cert_file = (server->cert_path && server->cert_path[0])
                                  ? server->cert_path
                                  : platform_cert_path();
      const char *key_file = (server->key_path && server->key_path[0])
                                 ? server->key_path
                                 : platform_key_path();
      rdpCertificate *cert = freerdp_certificate_new_from_file(cert_file);
      rdpPrivateKey *key = freerdp_key_new_from_file(key_file);
      if (cert && key) {
        LOG_I("viewer_server", "TLS certificate loaded from '%s'", cert_file);
        freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate,
                                         cert, 1);
        freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key,
                                         1);
      } else {
        LOG_W("viewer_server", "Failed to load cert from '%s'/'%s'", cert_file,
              key_file);
      }
    }
  }

  if (!peer->Initialize(peer))
    return FALSE;

  if (peer->context && peer->context->input) {
    peer->context->input->MouseEvent = on_mouse_event;
    peer->context->input->ExtendedMouseEvent = on_extended_mouse_event;
    peer->context->input->KeyboardEvent = on_keyboard_event;
  }

  viewer = find_viewer_by_peer(peer);
  if (!viewer)
    return FALSE;

  viewer->thread = CreateThread(NULL, 0, viewer_handle_peer, viewer, 0, NULL);
  return viewer->thread != NULL;
}

ViewerServer *viewer_server_init(const char *bind_address, UINT16 port,
                                 BackendClient *backend, const char *cert_path,
                                 const char *key_path) {
  return viewer_server_init_ex(bind_address, port, backend, cert_path, key_path,
                               NULL);
}

ViewerServer *viewer_server_init_ex(const char *bind_address, UINT16 port,
                                    BackendClient *backend,
                                    const char *cert_path, const char *key_path,
                                    const ViewerSecurityConfig *security) {
  ViewerServer *server = calloc(1, sizeof(ViewerServer));
  if (!server)
    return NULL;

  server->listener = freerdp_listener_new();
  if (!server->listener) {
    free(server);
    return NULL;
  }

  server->listener->info = server;
  server->listener->PeerAccepted = peer_accepted;
  server->port = port ? port : 3389;
  server->bind_address =
      bind_address ? strdup(bind_address) : strdup("0.0.0.0");
  server->backend = backend;
  server->cert_path = cert_path ? _strdup(cert_path) : NULL;
  server->key_path = key_path ? _strdup(key_path) : NULL;
  server->security = security ? *security : viewer_security_default();
  server->viewer_gfx_enabled = FALSE;
  server->viewer_gfx_codec = VIEWER_GFX_CODEC_UNCOMPRESSED;
  server->viewer_gfx_diagnostic_full_frame_dirty = FALSE;
  if (backend)
    server->monitor_layout = backend->monitor_layout;
  server->slow_viewer_disconnect_enabled = TRUE;
  server->slow_viewer_disconnect_ms = 30000;
  if (!InitializeCriticalSectionAndSpinCount(&server->lock, 4000)) {
    free(server->cert_path);
    free(server->key_path);
    free(server->bind_address);
    freerdp_listener_free(server->listener);
    free(server);
    return NULL;
  }
  if (!viewer_gfx_publisher_state_init(&server->gfx)) {
    DeleteCriticalSection(&server->lock);
    free(server->cert_path);
    free(server->key_path);
    free(server->bind_address);
    freerdp_listener_free(server->listener);
    free(server);
    return NULL;
  }
  if (!viewer_framebuffer_init(&server->framebuffer)) {
    viewer_gfx_publisher_state_uninit(&server->gfx);
    DeleteCriticalSection(&server->lock);
    free(server->cert_path);
    free(server->key_path);
    free(server->bind_address);
    freerdp_listener_free(server->listener);
    free(server);
    return NULL;
  }
  if (!viewer_publisher_init(&server->publisher)) {
    viewer_framebuffer_uninit(&server->framebuffer);
    viewer_gfx_publisher_state_uninit(&server->gfx);
    DeleteCriticalSection(&server->lock);
    free(server->cert_path);
    free(server->key_path);
    free(server->bind_address);
    freerdp_listener_free(server->listener);
    free(server);
    return NULL;
  }
  g_viewer_server = server;
  return server;
}

BOOL viewer_server_start(ViewerServer *server) {
  if (!server || !server->listener)
    return FALSE;

  if (!server->listener->Open(server->listener, server->bind_address,
                              server->port))
    return FALSE;

  LOG_I("viewer_server",
        "Viewer server listening on %s:%u auth_mode=%s nla_enabled=%s",
        server->bind_address, server->port,
        viewer_auth_mode_name(server->security.auth_mode),
        server->security.nla_enabled ? "true" : "false");

  server->running = TRUE;
  while (server->running) {
    if (!server->listener->CheckFileDescriptor(server->listener)) {
      server->running = FALSE;
      break;
    }
    platform_sleep_ms(1);
  }

  return TRUE;
}

void viewer_server_set_slow_disconnect(ViewerServer *server, BOOL enabled,
                                       UINT32 disconnect_after_ms) {
  if (!server)
    return;

  server->slow_viewer_disconnect_enabled = enabled;
  server->slow_viewer_disconnect_ms = disconnect_after_ms;
}

void viewer_server_set_classic_policy(
    ViewerServer *server,
    const ViewerPublisherClassicPolicyConfig *classic_policy) {
  if (!server)
    return;

  viewer_publisher_set_classic_policy(&server->publisher, classic_policy);
}

void viewer_server_set_gfx_enabled(ViewerServer *server, BOOL enabled) {
  if (!server)
    return;

  server->viewer_gfx_enabled = enabled ? TRUE : FALSE;
}

void viewer_server_set_gfx_codec(ViewerServer *server, ViewerGfxCodec codec) {
  if (!server)
    return;

  server->viewer_gfx_codec = (codec == VIEWER_GFX_CODEC_RFX)
                                 ? VIEWER_GFX_CODEC_RFX
                                 : VIEWER_GFX_CODEC_UNCOMPRESSED;
}

void viewer_server_set_gfx_diagnostic_full_frame_dirty(ViewerServer *server,
                                                       BOOL enabled) {
  if (!server)
    return;

  server->viewer_gfx_diagnostic_full_frame_dirty = enabled ? TRUE : FALSE;
}

void viewer_server_stop(ViewerServer *server) {
  freerdp_peer *peers[MAX_VIEWERS] = {0};

  if (!server)
    return;

  LOG_I("viewer_server", "Viewer server stopped");

  EnterCriticalSection(&server->lock);
  server->running = FALSE;
  for (int i = 0; i < MAX_VIEWERS; i++) {
    if (server->viewers[i].peer) {
      server->viewers[i].stop_requested = TRUE;
      peers[i] = server->viewers[i].peer;
    }
  }
  LeaveCriticalSection(&server->lock);

  if (server->listener)
    server->listener->Close(server->listener);

  for (int i = 0; i < MAX_VIEWERS; i++) {
    if (peers[i])
      peers[i]->Disconnect(peers[i]);
  }
}

void viewer_server_free(ViewerServer *server) {
  if (!server)
    return;

  viewer_server_stop(server);
  for (int i = 0; i < MAX_VIEWERS; i++)
    viewer_join_thread(&server->viewers[i]);

  g_viewer_server = NULL;

  for (int i = 0; i < MAX_VIEWERS; i++)
    viewer_cleanup_slot(server, &server->viewers[i]);

  EnterCriticalSection(&server->lock);
  server->viewer_count = 0;
  LeaveCriticalSection(&server->lock);

  if (server->listener)
    freerdp_listener_free(server->listener);
  free(server->bind_address);
  viewer_publisher_uninit(&server->publisher);
  viewer_framebuffer_uninit(&server->framebuffer);
  viewer_gfx_publisher_state_uninit(&server->gfx);
  DeleteCriticalSection(&server->lock);
  free(server);
}

UINT32
viewer_server_get_count(ViewerServer *server) {
  UINT32 count = 0;
  if (!server)
    return 0;

  EnterCriticalSection(&server->lock);
  count = server->viewer_count;
  LeaveCriticalSection(&server->lock);
  return count;
}

BOOL viewer_server_update_framebuffer_from_gdi(BackendClient *backend,
                                               const BYTE *pixels, UINT32 width,
                                               UINT32 height, UINT32 stride,
                                               UINT32 pixel_format,
                                               const RECTANGLE_16 *dirty_rects,
                                               UINT32 dirty_rect_count) {
  ViewerServer *server = g_viewer_server;
  RECTANGLE_16 *valid_dirty_rects = NULL;
  const RECTANGLE_16 *update_dirty_rects = dirty_rects;
  UINT32 update_dirty_rect_count = dirty_rect_count;
  BOOL needs_resize = FALSE;
  BOOL updated = FALSE;
  UINT64 generation = 0;
  UINT32 i = 0;

  if (!server || !backend || !pixels || (server->backend != backend))
    return FALSE;

  if ((width == 0) || (height == 0) || (stride == 0))
    return FALSE;

  /* Preserve invalid-argument behavior for malformed backend callbacks. */
  if ((dirty_rect_count > 0) && !dirty_rects)
    return FALSE;

  if (!server->framebuffer.initialized || !server->publisher.initialized)
    return FALSE;

  EnterCriticalSection(&server->framebuffer.lock);
  needs_resize = (server->framebuffer.width != width) ||
                 (server->framebuffer.height != height) ||
                 (server->framebuffer.stride != stride) ||
                 (server->framebuffer.pixel_format != pixel_format) ||
                 !server->framebuffer.pixels;
  LeaveCriticalSection(&server->framebuffer.lock);

  if (needs_resize && !viewer_framebuffer_resize(&server->framebuffer, width,
                                                 height, stride, pixel_format))
    return FALSE;

  /* Backend dirty rectangles are advisory damage from decoded GDI output.
   *
   * Validate them against the current desktop before handing them to the
   *
   * canonical framebuffer. Invalid/out-of-bounds rectangles are dropped; if
   * all
   * provided rectangles are invalid, fall back to a full-frame dirty
   * update to
   * preserve correctness and existing delivery semantics. */
  if (dirty_rect_count > 0) {
    valid_dirty_rects =
        (RECTANGLE_16 *)calloc(dirty_rect_count, sizeof(*valid_dirty_rects));
    if (!valid_dirty_rects)
      return FALSE;

    update_dirty_rect_count = 0;
    for (i = 0; i < dirty_rect_count; i++) {
      if (viewer_framebuffer_dirty_rect_valid(width, height, &dirty_rects[i]))
        valid_dirty_rects[update_dirty_rect_count++] = dirty_rects[i];
    }

    update_dirty_rects =
        (update_dirty_rect_count > 0) ? valid_dirty_rects : NULL;
  }

  updated = viewer_framebuffer_update_pixels(&server->framebuffer, pixels,
                                             stride, update_dirty_rects,
                                             update_dirty_rect_count);
  if (!updated) {
    free(valid_dirty_rects);
    return FALSE;
  }

  EnterCriticalSection(&server->framebuffer.lock);
  generation = server->framebuffer.generation;
  LeaveCriticalSection(&server->framebuffer.lock);
  viewer_publisher_note_framebuffer_update(
      &server->publisher, generation, update_dirty_rect_count,
      update_dirty_rect_count > VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS);
  viewer_server_accumulate_gfx_dirty_viewers(
      server, update_dirty_rects, update_dirty_rect_count,
      update_dirty_rect_count > VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS, generation,
      width, height);
  free(valid_dirty_rects);
  return TRUE;
}

void viewer_server_notify_backend_layout_change(BackendClient *backend,
                                                UINT32 width, UINT32 height,
                                                UINT32 generation) {
  ViewerServer *server = g_viewer_server;
  Viewer *targets[MAX_VIEWERS] = {0};
  int target_slots[MAX_VIEWERS] = {0};
  size_t target_count = 0;
  UINT32 queued_viewers = 0;
  UINT64 deadline = platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;

  if (!server || (server->backend != backend) || (width == 0) || (height == 0))
    return;

  EnterCriticalSection(&server->lock);
  server->monitor_layout = backend->monitor_layout;
  for (int i = 0; i < MAX_VIEWERS; i++) {
    Viewer *viewer = &server->viewers[i];
    if (!viewer_try_add_layout_ref_locked(viewer))
      continue;

    targets[target_count] = viewer;
    target_slots[target_count] = i;
    target_count++;
  }
  LeaveCriticalSection(&server->lock);

  for (size_t target_index = 0; target_index < target_count; target_index++) {
    Viewer *viewer = targets[target_index];
    int i = target_slots[target_index];

    EnterCriticalSection(&viewer->gfx.lock);
    viewer_gfx_pipeline_invalidate_surface_locked(&viewer->gfx);
    LeaveCriticalSection(&viewer->gfx.lock);

    EnterCriticalSection(&viewer->send_lock);
    viewer->needs_full_refresh = TRUE;
    viewer->full_refresh_deadline_ts = deadline;
    LeaveCriticalSection(&viewer->send_lock);
    queued_viewers++;

    if (viewer->peer->context && viewer->peer->context->settings) {
      rdpSettings *vs = viewer->peer->context->settings;

      WLog_INFO(TAG,
                "viewer_server_notify_backend_layout_change: viewer[%d]: "
                "setting DesktopWidth=%" PRIu32 ", DesktopHeight=%" PRIu32,
                i, width, height);

      freerdp_settings_set_uint32(vs, FreeRDP_DesktopWidth, width);
      freerdp_settings_set_uint32(vs, FreeRDP_DesktopHeight, height);

      /* Update multi-monitor layout if width exceeds single monitor.
       * On the server side, set MonitorCount and MonitorDefArray
       * (NOT UseMultimon/SpanMonitors which are client-side only). */
      if (width > 1920 && server && server->backend) {
        const MonitorLayout *layout = &server->backend->monitor_layout;
        UINT32 mi = 0;

        WLog_INFO(TAG,
                  "viewer_server_notify_backend_layout_change: viewer[%d]: "
                  "multi-monitor layout: MonitorCount=%" PRIu32,
                  i, layout->monitor_count);

        freerdp_settings_set_uint32(vs, FreeRDP_MonitorCount,
                                    layout->monitor_count);

        for (mi = 0; mi < layout->monitor_count; mi++) {
          rdpMonitor mon = {0};
          mon.x = layout->monitors[mi].left;
          mon.y = layout->monitors[mi].top;
          mon.width = layout->monitors[mi].right - layout->monitors[mi].left;
          mon.height = layout->monitors[mi].bottom - layout->monitors[mi].top;
          mon.is_primary =
              (layout->monitors[mi].flags & MONITOR_PRIMARY) ? TRUE : FALSE;
          mon.orig_screen = mi;
          mon.attributes.physicalWidth = mon.width;
          mon.attributes.physicalHeight = mon.height;
          mon.attributes.orientation = ORIENTATION_LANDSCAPE;
          mon.attributes.desktopScaleFactor = 100;
          mon.attributes.deviceScaleFactor = 100;

          WLog_INFO(TAG,
                    "viewer_server_notify_backend_layout_change: viewer[%d]: "
                    "monitor[%" PRIu32 "]: x=%" PRId32 ", y=%" PRId32
                    ", width=%" PRId32 ", height=%" PRId32 ", is_primary=%s",
                    i, mi, mon.x, mon.y, mon.width, mon.height,
                    mon.is_primary ? "TRUE" : "FALSE");

          freerdp_settings_set_pointer_array(vs, FreeRDP_MonitorDefArray, mi,
                                             &mon);
        }
      }
    }

    viewer_release_publish_ref(server, viewer);
  }

  if (queued_viewers > 0)
    (void)backend_request_full_refresh(server->backend);

  WLog_INFO(TAG,
            "Backend layout generation=%" PRIu32
            " propagated to viewers as %ux%u; Backend layout change, queued "
            "full refresh for %u viewers",
            generation, width, height, queued_viewers);
}

BOOL viewer_server_publish_surface_bits(BackendClient *backend,
                                        const SURFACE_BITS_COMMAND *cmd) {
  ViewerServer *server = g_viewer_server;
  Viewer *targets[MAX_VIEWERS] = {0};
  size_t target_count = 0;
  BOOL sent_any = FALSE;
  BOOL refresh_in_flight = FALSE;
  UINT32 classic_target_count = 0;
  UINT32 gated_full_refresh_count = 0;
  UINT32 throttled_count = 0;
  UINT32 enqueue_failed_count = 0;
  UINT32 enqueued_count = 0;
  UINT32 first_gated_viewer_id = 0;
  UINT32 first_throttled_viewer_id = 0;
  UINT64 publish_started_us = 0;
  UINT64 publish_us = 0;

  if (!server || (server->backend != backend) || !cmd)
    return FALSE;

  refresh_in_flight = backend_full_refresh_in_flight(server->backend);

  publish_started_us = viewer_perf_now_us();

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    Viewer *viewer = &server->viewers[i];
    if (!viewer_try_add_publish_ref_locked(viewer))
      continue;

    targets[target_count++] = viewer;
  }
  LeaveCriticalSection(&server->lock);

  for (size_t i = 0; i < target_count; i++) {
    Viewer *viewer = targets[i];
    BOOL classic_fallback = FALSE;
    BOOL ready_to_send = FALSE;
    BOOL enqueued = FALSE;
    ViewerPublisherSurfaceBitsPublishDecision decision = {0};
    ViewerSurfaceBitsEvent *event = NULL;

    EnterCriticalSection(&viewer->gfx.lock);
    classic_fallback = viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx);
    LeaveCriticalSection(&viewer->gfx.lock);
    if (!classic_fallback) {
      viewer_release_publish_ref(server, viewer);
      continue;
    }
    classic_target_count++;

    /* Pre-build the deep copy outside send_lock to minimize lock hold time */
    EnterCriticalSection(&viewer->send_lock);
    ready_to_send = viewer->peer && viewer->connected && viewer->activated;
    LeaveCriticalSection(&viewer->send_lock);

    decision = viewer_publisher_surface_bits_publish_decision(
        ready_to_send, viewer->needs_full_refresh,
        viewer->consecutive_lag_intervals >= VIEWER_THROTTLE_LAG_INTERVALS);
    if (decision.action == VIEWER_PUBLISHER_SURFACE_BITS_PUBLISH_NOT_READY) {
      viewer_release_publish_ref(server, viewer);
      continue;
    }

    if (decision.count_throttled) {
      EnterCriticalSection(&viewer->send_lock);
      viewer->surface_bits_updates_skipped_throttle++;
      LeaveCriticalSection(&viewer->send_lock);
      throttled_count++;
      if (first_throttled_viewer_id == 0)
        first_throttled_viewer_id = viewer->id;
    }

    if (decision.clear_full_refresh) {
      EnterCriticalSection(&viewer->send_lock);
      viewer->needs_full_refresh = FALSE;
      viewer->full_refresh_deadline_ts = 0;
      LeaveCriticalSection(&viewer->send_lock);
    }
    if (decision.count_full_refresh_gate) {
      gated_full_refresh_count++;
      if (first_gated_viewer_id == 0)
        first_gated_viewer_id = viewer->id;
    }

    event = viewer_surface_bits_event_new(cmd);
    if (!event) {
      enqueue_failed_count++;
      viewer_release_publish_ref(server, viewer);
      continue;
    }
    EnterCriticalSection(&viewer->send_lock);
    enqueued = viewer_enqueue_surface_bits_event_locked(viewer, event);
    LeaveCriticalSection(&viewer->send_lock);
    if (enqueued)
      enqueued_count++;
    else {
      enqueue_failed_count++;
      viewer_surface_bits_event_free(event);
    }

    if (enqueued)
      sent_any = TRUE;

    if (decision.request_full_refresh)
      (void)backend_request_full_refresh(server->backend);

    viewer_release_publish_ref(server, viewer);
  }

  publish_us = viewer_perf_now_us() - publish_started_us;

  if ((classic_target_count == 0) || (gated_full_refresh_count > 0) ||
      (throttled_count > 0) || (enqueue_failed_count > 0) ||
      (enqueued_count == 0)) {
    WLog_INFO(TAG,
              "Classic SurfaceBits publish summary rect=(%u,%u)-(%u,%u) "
              "payload=%" PRIu32 " codecId=%" PRIu16 " classicTargets=%" PRIu32
              " enqueued=%" PRIu32 " gatedFullRefresh=%" PRIu32
              " firstGatedViewer=%" PRIu32 " throttled=%" PRIu32
              " firstThrottledViewer=%" PRIu32 " enqueueFailed=%" PRIu32
              " refreshInFlight=%d publishUs=%" PRIu64,
              cmd->destLeft, cmd->destTop, cmd->destRight, cmd->destBottom,
              cmd->bmp.bitmapDataLength, cmd->bmp.codecID, classic_target_count,
              enqueued_count, gated_full_refresh_count, first_gated_viewer_id,
              throttled_count, first_throttled_viewer_id, enqueue_failed_count,
              refresh_in_flight, publish_us);
  }

  return sent_any;
}

BOOL viewer_server_publish_bitmap_update(BackendClient *backend,
                                         const BITMAP_UPDATE *bitmap) {
  ViewerServer *server = g_viewer_server;
  Viewer *targets[MAX_VIEWERS] = {0};
  size_t target_count = 0;
  BOOL sent_any = FALSE;
  BOOL refresh_in_flight = FALSE;
  UINT32 classic_target_count = 0;
  UINT32 gated_full_refresh_count = 0;
  UINT32 throttled_count = 0;
  UINT32 enqueue_failed_count = 0;
  UINT32 enqueued_count = 0;
  UINT32 first_gated_viewer_id = 0;
  UINT32 first_throttled_viewer_id = 0;
  UINT32 first_rect_payload = 0;
  UINT64 total_payload_bytes = 0;
  UINT64 publish_started_us = 0;
  UINT64 publish_us = 0;
  UINT32 rect_index = 0;

  if (!server || (server->backend != backend) || !bitmap)
    return FALSE;

  refresh_in_flight = backend_full_refresh_in_flight(server->backend);
  if (bitmap->number > 0)
    first_rect_payload = bitmap->rectangles[0].bitmapLength;
  for (rect_index = 0; rect_index < bitmap->number; rect_index++)
    total_payload_bytes += bitmap->rectangles[rect_index].bitmapLength;

  publish_started_us = viewer_perf_now_us();

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    Viewer *viewer = &server->viewers[i];
    if (!viewer_try_add_publish_ref_locked(viewer))
      continue;

    targets[target_count++] = viewer;
  }
  LeaveCriticalSection(&server->lock);

  for (size_t i = 0; i < target_count; i++) {
    Viewer *viewer = targets[i];
    BOOL classic_fallback = FALSE;
    BOOL ready_to_send = FALSE;
    BOOL enqueued = FALSE;
    ViewerPublisherBitmapPublishDecision decision = {0};
    ViewerClassicEvent *event = NULL;

    EnterCriticalSection(&viewer->gfx.lock);
    classic_fallback = viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx);
    LeaveCriticalSection(&viewer->gfx.lock);
    if (!classic_fallback) {
      viewer_release_publish_ref(server, viewer);
      continue;
    }
    classic_target_count++;

    /* Pre-build the deep copy outside send_lock to minimize lock hold time.
     * The bitmap pointer is const and immutable during this call. */
    EnterCriticalSection(&viewer->send_lock);
    ready_to_send = viewer->peer && viewer->connected && viewer->activated;
    LeaveCriticalSection(&viewer->send_lock);

    decision = viewer_publisher_bitmap_publish_decision(
        ready_to_send, viewer->needs_full_refresh, refresh_in_flight,
        viewer->consecutive_lag_intervals >= VIEWER_THROTTLE_LAG_INTERVALS);
    if (decision.action == VIEWER_PUBLISHER_BITMAP_PUBLISH_NOT_READY) {
      viewer_release_publish_ref(server, viewer);
      continue;
    }

    if (decision.clear_full_refresh) {
      EnterCriticalSection(&viewer->send_lock);
      viewer->needs_full_refresh = FALSE;
      viewer->full_refresh_deadline_ts = 0;
      LeaveCriticalSection(&viewer->send_lock);
    }
    if (decision.count_full_refresh_gate) {
      gated_full_refresh_count++;
      if (first_gated_viewer_id == 0)
        first_gated_viewer_id = viewer->id;
    }

    if (decision.count_throttled) {
      EnterCriticalSection(&viewer->send_lock);
      viewer->bitmap_updates_skipped_throttle++;
      viewer->needs_full_refresh = TRUE;
      viewer->full_refresh_deadline_ts =
          platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;
      LeaveCriticalSection(&viewer->send_lock);
      throttled_count++;
      if (first_throttled_viewer_id == 0)
        first_throttled_viewer_id = viewer->id;
    }

    if (decision.enqueue) {
      event = viewer_classic_event_new(bitmap);
      if (!event) {
        enqueue_failed_count++;
        viewer_release_publish_ref(server, viewer);
        continue;
      }
      EnterCriticalSection(&viewer->send_lock);
      enqueued = viewer_enqueue_classic_event_locked(viewer, event);
      LeaveCriticalSection(&viewer->send_lock);
      if (enqueued)
        enqueued_count++;
      else {
        enqueue_failed_count++;
        viewer_classic_event_free(event);
      }
    }

    if (enqueued)
      sent_any = TRUE;

    if (decision.request_full_refresh)
      (void)backend_request_full_refresh(server->backend);

    viewer_release_publish_ref(server, viewer);
  }

  publish_us = viewer_perf_now_us() - publish_started_us;

  if ((classic_target_count == 0) || (gated_full_refresh_count > 0) ||
      (throttled_count > 0) || (enqueue_failed_count > 0) ||
      (enqueued_count == 0) ||
      viewer_should_log_bitmap_perf(backend->bitmap_update_batches_total + 1ULL,
                                    publish_us, enqueue_failed_count)) {
    WLog_INFO(TAG,
              "Classic BitmapUpdate publish summary batch=%" PRIu64
              " rectangles=%" PRIu32 " payload=%" PRIu64
              " skipCompression=%d firstRectPayload=%" PRIu32
              " classicTargets=%" PRIu32 " enqueued=%" PRIu32
              " gatedFullRefresh=%" PRIu32 " firstGatedViewer=%" PRIu32
              " throttled=%" PRIu32 " firstThrottledViewer=%" PRIu32
              " enqueueFailed=%" PRIu32
              " refreshInFlight=%d publishUs=%" PRIu64,
              backend->bitmap_update_batches_total + 1ULL, bitmap->number,
              total_payload_bytes, bitmap->skipCompression, first_rect_payload,
              classic_target_count, enqueued_count, gated_full_refresh_count,
              first_gated_viewer_id, throttled_count, first_throttled_viewer_id,
              enqueue_failed_count, refresh_in_flight, publish_us);
  }

  return sent_any;
}

BOOL viewer_server_publish_frame_marker(BackendClient *backend,
                                        const SURFACE_FRAME_MARKER *marker) {
  ViewerServer *server = g_viewer_server;
  Viewer *targets[MAX_VIEWERS] = {0};
  size_t target_count = 0;
  BOOL sent_any = FALSE;
  BOOL refresh_in_flight = FALSE;
  UINT32 completed_viewers = 0;
  BOOL pending_viewers_remain = FALSE;

  if (!server || (server->backend != backend) || !marker)
    return FALSE;

  refresh_in_flight = backend_full_refresh_in_flight(server->backend);

  EnterCriticalSection(&server->lock);
  for (int i = 0; i < MAX_VIEWERS; i++) {
    Viewer *viewer = &server->viewers[i];
    if (!viewer_try_add_publish_ref_locked(viewer))
      continue;

    targets[target_count++] = viewer;
  }
  LeaveCriticalSection(&server->lock);

  /* Only forward frame markers to viewers that have negotiated codec support
   * (RemoteFX or NSCodec). When codecs are disabled, the client doesn't
   * expect SURFACE_FRAME_MARKER PDUs and treats them as a protocol error
   * (error 0xd06). Frame markers are only meaningful when SurfaceBits
   * codec data is being sent — they delimit frame boundaries for codec
   * tile streams. With uncompressed BitmapUpdate, frame markers are
   * unnecessary and harmful. */
  for (size_t i = 0; i < target_count; i++) {
    Viewer *viewer = targets[i];
    BOOL classic_fallback = FALSE;
    rdpSettings *settings =
        viewer->peer
            ? viewer->peer->context ? viewer->peer->context->settings : NULL
            : NULL;
    BOOL has_codec = FALSE;

    EnterCriticalSection(&viewer->gfx.lock);
    classic_fallback = viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx);
    LeaveCriticalSection(&viewer->gfx.lock);
    if (!classic_fallback)
      continue;

    if (settings) {
      has_codec = freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec) ||
                  freerdp_settings_get_bool(settings, FreeRDP_NSCodec);
    }

    if (!has_codec)
      continue;

    EnterCriticalSection(&viewer->send_lock);
    if (viewer->peer && viewer->connected && viewer->activated &&
        viewer_send_frame_marker(viewer, marker))
      sent_any = TRUE;
    LeaveCriticalSection(&viewer->send_lock);
  }

  if (marker->frameAction == SURFACECMD_FRAMEACTION_END) {
    for (size_t i = 0; i < target_count; i++) {
      Viewer *viewer = targets[i];
      BOOL classic_fallback = FALSE;

      EnterCriticalSection(&viewer->gfx.lock);
      classic_fallback =
          viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx);
      LeaveCriticalSection(&viewer->gfx.lock);
      if (!classic_fallback)
        continue;

      EnterCriticalSection(&viewer->send_lock);
      if (viewer->needs_full_refresh) {
        viewer->needs_full_refresh = FALSE;
        viewer->full_refresh_deadline_ts = 0;
        completed_viewers++;
      }
      LeaveCriticalSection(&viewer->send_lock);

      EnterCriticalSection(&viewer->gfx.lock);
      if (viewer->gfx.join_strategy == VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK)
        viewer_gfx_pipeline_finish_join_locked(
            viewer, "classic fallback full refresh completed");
      LeaveCriticalSection(&viewer->gfx.lock);
    }

    /* Only check classic-fallback targets for pending refresh needs.
     * GFX viewers manage their own refresh lifecycle and should not
     * block classic refresh completion. Viewers that were not targets
     * of this frame marker (e.g., newly joined or throttled viewers)
     * should also not block completion — they will request their own
     * refresh cycle independently. */
    for (size_t i = 0; i < target_count; i++) {
      Viewer *viewer = targets[i];
      BOOL classic_fallback = FALSE;

      EnterCriticalSection(&viewer->gfx.lock);
      classic_fallback =
          viewer_gfx_negotiation_is_classic_fallback(&viewer->gfx);
      LeaveCriticalSection(&viewer->gfx.lock);
      if (!classic_fallback)
        continue;

      EnterCriticalSection(&viewer->send_lock);
      if (viewer->needs_full_refresh)
        pending_viewers_remain = TRUE;
      LeaveCriticalSection(&viewer->send_lock);

      if (pending_viewers_remain)
        break;
    }

    if (!pending_viewers_remain)
      backend_mark_full_refresh_complete(server->backend);

    WLog_INFO(TAG, "Full refresh completed for %u viewers (frame id=%u)",
              completed_viewers, marker->frameId);
  }

  for (size_t i = 0; i < target_count; i++)
    viewer_release_publish_ref(server, targets[i]);

  return sent_any;
}
