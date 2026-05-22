#include "viewer_server.h"
#include "backend.h"
#include "platform_compat.h"
#include "svc_log.h"
#include "viewer_gfx_pipeline.h"
#include "viewer_internal.h"

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
#define VIEWER_CLASSIC_MAX_RECTS_PER_SEND 64U
#define VIEWER_CLASSIC_MAX_BYTES_PER_SEND (512U * 1024U)

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
static BOOL viewer_gfx_send_framebuffer_baseline(ViewerServer *server,
                                                 Viewer *viewer, UINT64 now);
static BOOL viewer_gfx_try_send_dirty_update(ViewerServer *server,
                                             Viewer *viewer, UINT64 now);
static UINT64 viewer_perf_now_us(void);
static BOOL viewer_should_log_bitmap_perf(UINT64 batch_count, UINT64 publish_us,
                                          UINT32 send_failed_count);
static BOOL viewer_send_bitmap_update(Viewer *viewer,
                                      const BITMAP_UPDATE *bitmap);
static BOOL viewer_send_bitmap_update_locked(Viewer *viewer,
                                             const BITMAP_UPDATE *bitmap);
static BOOL viewer_send_surface_bits(Viewer *viewer,
                                     const SURFACE_BITS_COMMAND *cmd);
static BOOL viewer_classic_enqueue_event_locked(Viewer *viewer,
                                                ViewerClassicEvent *event);
static BOOL
viewer_enqueue_classic_baseline_from_framebuffer(ViewerServer *server,
                                                 Viewer *viewer);
static void viewer_note_classic_queue_state_locked(const Viewer *viewer);
static void viewer_classic_apply_latest_policy_locked(Viewer *viewer);
static ViewerClassicEvent *
viewer_classic_event_from_snapshot(const ViewerFramebufferSnapshot *snapshot);
static void viewer_classic_queue_clear_locked(Viewer *viewer);

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
                                                char **viewer_user,
                                                char **viewer_domain,
                                                char **viewer_password) {
  rdpSettings *settings = NULL;
  const char *settings_user = NULL;
  const char *settings_domain = NULL;
  const char *settings_password = NULL;

  if (!peer || !peer->context)
    return FALSE;

  settings = peer->context->settings;
  if (!settings)
    return FALSE;

  settings_user = freerdp_settings_get_string(settings, FreeRDP_Username);
  settings_domain = freerdp_settings_get_string(settings, FreeRDP_Domain);
  settings_password = freerdp_settings_get_string(settings, FreeRDP_Password);

  if (!viewer_string_has_value(settings_user) &&
      !viewer_string_has_value(settings_domain) &&
      !viewer_string_has_value(settings_password))
    return FALSE;

  *viewer_user = _strdup(settings_user ? settings_user : "");
  *viewer_domain = _strdup(settings_domain ? settings_domain : "");
  *viewer_password = _strdup(settings_password ? settings_password : "");

  if (!*viewer_user || !*viewer_domain || !*viewer_password) {
    free(*viewer_user);
    free(*viewer_domain);
    free(*viewer_password);
    *viewer_user = NULL;
    *viewer_domain = NULL;
    *viewer_password = NULL;
    return FALSE;
  }

  return TRUE;
}

static BOOL
viewer_identity_credentials_to_utf8(const SEC_WINNT_AUTH_IDENTITY *identity,
                                    char **viewer_user, char **viewer_domain,
                                    char **viewer_password) {
  if (!identity)
    return FALSE;

  *viewer_user =
      viewer_identity_field_to_utf8(identity->User, identity->UserLength);
  *viewer_domain =
      viewer_identity_field_to_utf8(identity->Domain, identity->DomainLength);
  *viewer_password = viewer_identity_field_to_utf8(identity->Password,
                                                   identity->PasswordLength);

  if (!*viewer_user || !*viewer_domain || !*viewer_password) {
    free(*viewer_user);
    free(*viewer_domain);
    free(*viewer_password);
    *viewer_user = NULL;
    *viewer_domain = NULL;
    *viewer_password = NULL;
    return FALSE;
  }

  return TRUE;
}

static const char *
viewer_comparison_domain_label(const char *configured_domain) {
  return viewer_string_has_value(configured_domain) ? configured_domain
                                                    : "<local>";
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

static BOOL viewer_bitmap_bpp_sane(UINT32 bpp) {
  return (bpp == 8) || (bpp == 15) || (bpp == 16) || (bpp == 24) || (bpp == 32);
}

static BOOL viewer_get_desktop_size(const Viewer *viewer, UINT32 *width,
                                    UINT32 *height) {
  rdpSettings *settings = NULL;

  if (!width || !height)
    return FALSE;

  *width = 0;
  *height = 0;
  if (!viewer || !viewer->peer || !viewer->peer->context)
    return FALSE;

  settings = viewer->peer->context->settings;
  if (!settings)
    return FALSE;

  *width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
  *height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
  return (*width > 0) && (*height > 0);
}

static BOOL
viewer_validate_bitmap_rect(const Viewer *viewer, const BITMAP_UPDATE *bitmap,
                            const BITMAP_DATA *rect, UINT32 rect_index,
                            UINT32 desktop_width, UINT32 desktop_height,
                            const char *operation, BOOL log_invalid) {
  UINT32 dest_width = 0;
  UINT32 dest_height = 0;
  BOOL compressed = FALSE;
  const char *reason = NULL;

  if (!bitmap || !rect) {
    reason = "missing rect";
    goto invalid;
  }

  compressed = rect->compressed ? TRUE : FALSE;

  if ((rect->destRight < rect->destLeft) ||
      (rect->destBottom < rect->destTop)) {
    reason = "invalid bounds";
    goto invalid;
  }

  dest_width = (UINT32)rect->destRight - (UINT32)rect->destLeft + 1U;
  dest_height = (UINT32)rect->destBottom - (UINT32)rect->destTop + 1U;

  if ((dest_width == 0) || (dest_height == 0) || (rect->width == 0) ||
      (rect->height == 0)) {
    reason = "non-positive dimensions";
    goto invalid;
  }

  if ((rect->destRight >= desktop_width) ||
      (rect->destBottom >= desktop_height)) {
    reason = "outside desktop";
    goto invalid;
  }

  if (!viewer_bitmap_bpp_sane(rect->bitsPerPixel)) {
    reason = "invalid bpp";
    goto invalid;
  }

  if ((rect->bitmapLength > 0) && !rect->bitmapDataStream) {
    reason = "missing bitmap data";
    goto invalid;
  }

  return TRUE;

invalid:
  if (log_invalid) {
    WLog_WARN(
        TAG,
        "Viewer %u dropping BitmapUpdate rect op=%s reason=%s rect=%" PRIu32
        "/%" PRIu32 " bounds=(%" PRIu16 ",%" PRIu16 ")-(%" PRIu16 ",%" PRIu16
        ") dest_size=%" PRIu32 "x%" PRIu32 " bitmap_size=%" PRIu16 "x%" PRIu16
        " desktop=%" PRIu32 "x%" PRIu32 " bpp=%" PRIu32 " flags=0x%" PRIx32
        " compressed=%s length=%" PRIu32 " data_present=%s",
        viewer ? viewer->id : 0, operation ? operation : "unknown",
        reason ? reason : "unknown", rect_index, bitmap ? bitmap->number : 0,
        rect ? rect->destLeft : 0, rect ? rect->destTop : 0,
        rect ? rect->destRight : 0, rect ? rect->destBottom : 0, dest_width,
        dest_height, rect ? rect->width : 0, rect ? rect->height : 0,
        desktop_width, desktop_height, rect ? (UINT32)rect->bitsPerPixel : 0,
        rect ? (UINT32)rect->flags : 0, compressed ? "true" : "false",
        rect ? (UINT32)rect->bitmapLength : 0,
        rect && rect->bitmapDataStream ? "true" : "false");
  }
  return FALSE;
}

static BOOL viewer_send_bitmap_update_chunks(Viewer *viewer,
                                             const BITMAP_UPDATE *bitmap,
                                             BOOL update_lock_held,
                                             const char *operation) {
  freerdp_peer *peer = viewer ? viewer->peer : NULL;
  BITMAP_DATA chunk_rects[VIEWER_CLASSIC_MAX_RECTS_PER_SEND];
  BITMAP_UPDATE chunk = {0};
  UINT32 desktop_width = 0;
  UINT32 desktop_height = 0;
  UINT32 valid_sent = 0;
  UINT32 invalid_dropped = 0;
  UINT32 chunks_sent = 0;
  UINT32 i = 0;
  UINT32 chunk_bytes = 0;
  UINT64 total_payload_bytes = 0;
  UINT64 send_time_total_us = 0;
  BOOL ret = TRUE;
  BOOL logged_invalid = FALSE;

  if (!viewer || !peer || !peer->context || !peer->context->update || !bitmap)
    return FALSE;

  if (!bitmap->rectangles || (bitmap->number == 0)) {
    WLog_WARN(TAG,
              "Viewer %u dropping BitmapUpdate op=%s: count=%" PRIu32
              " rectangles_present=%s",
              viewer->id, operation ? operation : "unknown", bitmap->number,
              bitmap->rectangles ? "true" : "false");
    return TRUE;
  }

  if (!viewer_get_desktop_size(viewer, &desktop_width, &desktop_height)) {
    WLog_WARN(TAG, "Viewer %u dropping BitmapUpdate op=%s: no desktop size",
              viewer->id, operation ? operation : "unknown");
    return TRUE;
  }

  memset(&chunk, 0, sizeof(chunk));
  chunk.skipCompression = bitmap->skipCompression;
  chunk.rectangles = chunk_rects;

#define VIEWER_SEND_BITMAP_CHUNK()                                             \
  do {                                                                         \
    BOOL chunk_ret = FALSE;                                                    \
    UINT64 send_started_us = 0;                                                \
    UINT64 send_us = 0;                                                        \
    if (chunk.number > 0) {                                                    \
      send_started_us = viewer_perf_now_us();                                  \
      if (!update_lock_held)                                                   \
        rdp_update_lock(peer->context->update);                                \
      IFCALLRET(peer->context->update->BitmapUpdate, chunk_ret, peer->context, \
                &chunk);                                                       \
      if (!update_lock_held)                                                   \
        rdp_update_unlock(peer->context->update);                              \
      send_us = viewer_perf_now_us() - send_started_us;                        \
      send_time_total_us += send_us;                                           \
      if (send_us > viewer->bitmap_send_time_max_us)                           \
        viewer->bitmap_send_time_max_us = send_us;                             \
      if (chunk_ret) {                                                         \
        viewer->packets_sent++;                                                \
        viewer->bitmap_updates_sent++;                                         \
        viewer->bitmap_rectangles_sent += chunk.number;                        \
        viewer->bitmap_payload_bytes_sent += chunk_bytes;                      \
        valid_sent += chunk.number;                                            \
        chunks_sent++;                                                         \
      } else {                                                                 \
        viewer->packets_failed++;                                              \
        viewer->bitmap_updates_failed++;                                       \
        ret = FALSE;                                                           \
      }                                                                        \
      chunk.number = 0;                                                        \
      chunk_bytes = 0;                                                         \
    }                                                                          \
  } while (0)

  for (i = 0; i < bitmap->number; i++) {
    const BITMAP_DATA *rect = &bitmap->rectangles[i];
    UINT32 rect_bytes = rect ? (UINT32)rect->bitmapLength : 0;

    if (!viewer_validate_bitmap_rect(viewer, bitmap, rect, i, desktop_width,
                                     desktop_height, operation,
                                     !logged_invalid)) {
      invalid_dropped++;
      logged_invalid = TRUE;
      continue;
    }

    if ((chunk.number > 0) &&
        ((chunk.number >= VIEWER_CLASSIC_MAX_RECTS_PER_SEND) ||
         ((chunk_bytes + rect_bytes) > VIEWER_CLASSIC_MAX_BYTES_PER_SEND))) {
      VIEWER_SEND_BITMAP_CHUNK();
      if (!ret)
        break;
    }

    chunk_rects[chunk.number++] = *rect;
    chunk_bytes += rect_bytes;
    total_payload_bytes += rect_bytes;

    if (chunk.number >= VIEWER_CLASSIC_MAX_RECTS_PER_SEND) {
      VIEWER_SEND_BITMAP_CHUNK();
      if (!ret)
        break;
    }
  }

  if (ret && (chunk.number > 0))
    VIEWER_SEND_BITMAP_CHUNK();

#undef VIEWER_SEND_BITMAP_CHUNK

  viewer->bitmap_send_time_total_us += send_time_total_us;

  if ((valid_sent == 0) && (invalid_dropped > 0)) {
    WLog_WARN(TAG,
              "Viewer %u dropped BitmapUpdate op=%s: all rects invalid "
              "original=%" PRIu32 " invalid=%" PRIu32,
              viewer->id, operation ? operation : "unknown", bitmap->number,
              invalid_dropped);
    return TRUE;
  }

  WLog_INFO(TAG,
            "Viewer %u BitmapUpdate op=%s summary original=%" PRIu32
            " valid_sent=%" PRIu32 " invalid_dropped=%" PRIu32
            " chunks_sent=%" PRIu32 " total_payload=%" PRIu64
            " max_rects=%u max_bytes=%u send_ok=%s",
            viewer->id, operation ? operation : "unknown", bitmap->number,
            valid_sent, invalid_dropped, chunks_sent, total_payload_bytes,
            VIEWER_CLASSIC_MAX_RECTS_PER_SEND,
            VIEWER_CLASSIC_MAX_BYTES_PER_SEND, ret ? "true" : "false");

  return ret;
}

/* ---- Classic bitmap queue: deep-copy helpers ---- */

static ViewerClassicEvent *
viewer_classic_event_new(const BITMAP_UPDATE *bitmap) {
  ViewerClassicEvent *event = NULL;
  UINT32 i = 0;

  if (!bitmap)
    return NULL;

  event = (ViewerClassicEvent *)calloc(1, sizeof(ViewerClassicEvent));
  if (!event)
    return NULL;

  event->bitmap = (BITMAP_UPDATE *)calloc(1, sizeof(BITMAP_UPDATE));
  if (!event->bitmap) {
    free(event);
    return NULL;
  }

  /* Shallow-copy top-level fields */
  event->bitmap->number = bitmap->number;
  event->bitmap->skipCompression = bitmap->skipCompression;

  if (bitmap->number == 0) {
    event->bitmap->rectangles = NULL;
    return event;
  }

  /* Deep-copy the rectangles array */
  event->bitmap->rectangles =
      (BITMAP_DATA *)calloc(bitmap->number, sizeof(BITMAP_DATA));
  if (!event->bitmap->rectangles) {
    free(event->bitmap);
    free(event);
    return NULL;
  }

  for (i = 0; i < bitmap->number; i++) {
    /* Copy inline fields */
    event->bitmap->rectangles[i] = bitmap->rectangles[i];

    /* Deep-copy the bitmap data stream */
    if (bitmap->rectangles[i].bitmapLength > 0 &&
        bitmap->rectangles[i].bitmapDataStream) {
      event->bitmap->rectangles[i].bitmapDataStream =
          (BYTE *)malloc(bitmap->rectangles[i].bitmapLength);
      if (!event->bitmap->rectangles[i].bitmapDataStream) {
        /* Free already-allocated rectangles on failure */
        for (UINT32 j = 0; j < i; j++) {
          free(event->bitmap->rectangles[j].bitmapDataStream);
          event->bitmap->rectangles[j].bitmapDataStream = NULL;
        }
        free(event->bitmap->rectangles);
        free(event->bitmap);
        free(event);
        return NULL;
      }
      memmove(event->bitmap->rectangles[i].bitmapDataStream,
              bitmap->rectangles[i].bitmapDataStream,
              bitmap->rectangles[i].bitmapLength);
    } else {
      event->bitmap->rectangles[i].bitmapDataStream = NULL;
    }
  }

  return event;
}

static void viewer_classic_event_free(ViewerClassicEvent *event) {
  UINT32 i = 0;

  if (!event)
    return;

  if (event->bitmap) {
    if (event->bitmap->rectangles) {
      for (i = 0; i < event->bitmap->number; i++) {
        free(event->bitmap->rectangles[i].bitmapDataStream);
      }
      free(event->bitmap->rectangles);
    }
    free(event->bitmap);
  }
  free(event);
}

static UINT64
viewer_classic_event_payload_bytes(const ViewerClassicEvent *event) {
  UINT64 bytes = 0;

  if (!event || !event->bitmap || !event->bitmap->rectangles)
    return 0;

  for (UINT32 i = 0; i < event->bitmap->number; i++)
    bytes += event->bitmap->rectangles[i].bitmapLength;
  return bytes;
}

static UINT64 viewer_classic_queue_payload_bytes_locked(const Viewer *viewer) {
  UINT64 bytes = 0;
  UINT32 index = 0;

  if (!viewer)
    return 0;

  index = viewer->classic_queue_head;
  for (UINT32 i = 0; i < viewer->classic_queue_count; i++) {
    bytes += viewer_classic_event_payload_bytes(viewer->classic_queue[index]);
    index = (index + 1U) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  }
  return bytes;
}

static void viewer_note_classic_queue_state_locked(const Viewer *viewer) {
  ViewerServer *server = g_viewer_server;

  if (!server || !viewer)
    return;

  viewer_publisher_note_classic_queue_state(
      &server->publisher, viewer->classic_queue_count,
      viewer_classic_queue_payload_bytes_locked(viewer));
}

static void
viewer_classic_enqueue_event_direct_locked(Viewer *viewer,
                                           ViewerClassicEvent *event) {
  if (!viewer || !event ||
      (viewer->classic_queue_count >= VIEWER_CLASSIC_QUEUE_CAPACITY))
    return;

  viewer->classic_queue[viewer->classic_queue_tail] = event;
  viewer->classic_queue_tail =
      (viewer->classic_queue_tail + 1) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  viewer->classic_queue_count++;
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

  queued_bytes = viewer_classic_queue_payload_bytes_locked(viewer);
  if (viewer_publisher_classic_queue_decision(
          &server->publisher, viewer->classic_queue_count, queued_bytes) !=
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
            viewer->id, viewer->classic_queue_count, event->generation);
  viewer_classic_queue_clear_locked(viewer);
  viewer_classic_enqueue_event_direct_locked(viewer, event);

  if (viewer->classic_event)
    SetEvent(viewer->classic_event);
}

static ViewerClassicEvent *
viewer_classic_event_from_snapshot(const ViewerFramebufferSnapshot *snapshot) {
  ViewerClassicEvent *event = NULL;
  BITMAP_DATA *rect = NULL;

  if (!snapshot || !snapshot->pixels || (snapshot->width == 0) ||
      (snapshot->height == 0) || (snapshot->stride == 0) ||
      (snapshot->pixel_bytes == 0) || (snapshot->width > UINT16_MAX) ||
      (snapshot->height > UINT16_MAX) || (snapshot->pixel_bytes > UINT32_MAX))
    return NULL;

  event = (ViewerClassicEvent *)calloc(1, sizeof(*event));
  if (!event)
    return NULL;

  event->bitmap = (BITMAP_UPDATE *)calloc(1, sizeof(*event->bitmap));
  if (!event->bitmap) {
    free(event);
    return NULL;
  }

  event->bitmap->rectangles = (BITMAP_DATA *)calloc(1, sizeof(BITMAP_DATA));
  if (!event->bitmap->rectangles) {
    viewer_classic_event_free(event);
    return NULL;
  }

  event->bitmap->number = 1;
  event->bitmap->skipCompression = TRUE;
  rect = &event->bitmap->rectangles[0];
  rect->destLeft = 0;
  rect->destTop = 0;
  rect->destRight = snapshot->width - 1U;
  rect->destBottom = snapshot->height - 1U;
  rect->width = snapshot->width;
  rect->height = snapshot->height;
  rect->bitsPerPixel = 32;
  rect->flags = 0;
  rect->bitmapLength = (UINT32)snapshot->pixel_bytes;
  rect->cbScanWidth = snapshot->stride;
  rect->cbUncompressedSize = (UINT32)snapshot->pixel_bytes;
  rect->compressed = FALSE;
  rect->bitmapDataStream = (BYTE *)malloc(snapshot->pixel_bytes);
  if (!rect->bitmapDataStream) {
    viewer_classic_event_free(event);
    return NULL;
  }

  memmove(rect->bitmapDataStream, snapshot->pixels, snapshot->pixel_bytes);
  event->generation = snapshot->generation;
  return event;
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
  queued = viewer_classic_enqueue_event_locked(viewer, event);
  LeaveCriticalSection(&viewer->send_lock);

  if (!queued) {
    viewer_classic_event_free(event);
    return FALSE;
  }

  WLog_INFO(TAG, "Viewer %u queued classic framebuffer baseline", viewer->id);
  return TRUE;
}

/* ---- SurfaceBits event: deep-copy and free ---- */

static ViewerSurfaceBitsEvent *
viewer_surface_bits_event_new(const SURFACE_BITS_COMMAND *cmd) {
  ViewerSurfaceBitsEvent *event = NULL;

  if (!cmd)
    return NULL;

  event = (ViewerSurfaceBitsEvent *)calloc(1, sizeof(ViewerSurfaceBitsEvent));
  if (!event)
    return NULL;

  /* Shallow-copy all fields */
  event->cmd = *cmd;

  /* Deep-copy the bitmapData buffer */
  if (cmd->bmp.bitmapDataLength > 0 && cmd->bmp.bitmapData) {
    event->cmd.bmp.bitmapData = (BYTE *)malloc(cmd->bmp.bitmapDataLength);
    if (!event->cmd.bmp.bitmapData) {
      free(event);
      return NULL;
    }
    memmove(event->cmd.bmp.bitmapData, cmd->bmp.bitmapData,
            cmd->bmp.bitmapDataLength);
  } else {
    event->cmd.bmp.bitmapData = NULL;
    event->cmd.bmp.bitmapDataLength = 0;
  }

  return event;
}

static void viewer_surface_bits_event_free(ViewerSurfaceBitsEvent *event) {
  if (!event)
    return;

  free(event->cmd.bmp.bitmapData);
  free(event);
}

/* ---- Classic bitmap queue: queue operations ---- */

/* Drop the oldest entry from the classic queue. Caller must hold send_lock. */
static void viewer_classic_queue_drop_oldest_locked(Viewer *viewer) {
  ViewerClassicEvent *oldest = NULL;

  if (viewer->classic_queue_count == 0)
    return;

  oldest = viewer->classic_queue[viewer->classic_queue_head];
  viewer->classic_queue[viewer->classic_queue_head] = NULL;
  viewer->classic_queue_head =
      (viewer->classic_queue_head + 1) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  viewer->classic_queue_count--;

  viewer_classic_event_free(oldest);
  viewer->bitmap_queue_dropped++;
  if (g_viewer_server)
    viewer_publisher_note_classic_drop(&g_viewer_server->publisher);
  viewer_note_classic_queue_state_locked(viewer);
}

static void viewer_classic_queue_clear_locked(Viewer *viewer) {
  while (viewer->classic_queue_count > 0)
    viewer_classic_queue_drop_oldest_locked(viewer);
}

/* Enqueue a deep-copied BITMAP_UPDATE for a viewer.
 * Called from the backend thread under viewer->send_lock.
 * Returns TRUE on success, FALSE if the viewer should be disconnected. */
static BOOL viewer_classic_enqueue_locked(Viewer *viewer,
                                          const BITMAP_UPDATE *bitmap) {
  ViewerClassicEvent *event = NULL;

  if (!viewer || !bitmap)
    return FALSE;

  /* If queue is full, drop oldest entries to make room */
  while (viewer->classic_queue_count >= VIEWER_CLASSIC_QUEUE_CAPACITY) {
    WLog_WARN(TAG, "Viewer %u classic queue full (%u entries), dropping oldest",
              viewer->id, viewer->classic_queue_count);
    viewer_classic_queue_drop_oldest_locked(viewer);

    /* After dropping, mark viewer for full refresh to resync */
    viewer->needs_full_refresh = TRUE;
    viewer->full_refresh_deadline_ts =
        platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;
  }

  event = viewer_classic_event_new(bitmap);
  if (!event) {
    WLog_ERR(TAG, "Viewer %u failed to allocate classic event", viewer->id);
    return FALSE;
  }

  viewer->classic_queue[viewer->classic_queue_tail] = event;
  viewer->classic_queue_tail =
      (viewer->classic_queue_tail + 1) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  viewer->classic_queue_count++;
  viewer->bitmap_updates_queued++;
  viewer_note_classic_queue_state_locked(viewer);
  viewer_classic_apply_latest_policy_locked(viewer);

  /* Signal the viewer thread that a new event is available */
  if (viewer->classic_event)
    SetEvent(viewer->classic_event);

  return TRUE;
}

/* Enqueue a pre-built event into the viewer's classic queue.
 * Caller must hold send_lock. The event must have been deep-copied
 * by the caller before acquiring the lock. Returns TRUE on success. */
static BOOL viewer_classic_enqueue_event_locked(Viewer *viewer,
                                                ViewerClassicEvent *event) {
  if (!viewer || !event)
    return FALSE;

  /* If queue is full, drop oldest entries to make room */
  while (viewer->classic_queue_count >= VIEWER_CLASSIC_QUEUE_CAPACITY) {
    WLog_WARN(TAG, "Viewer %u classic queue full (%u entries), dropping oldest",
              viewer->id, viewer->classic_queue_count);
    viewer_classic_queue_drop_oldest_locked(viewer);

    /* After dropping, mark viewer for full refresh to resync */
    viewer->needs_full_refresh = TRUE;
    viewer->full_refresh_deadline_ts =
        platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;
  }

  viewer->classic_queue[viewer->classic_queue_tail] = event;
  viewer->classic_queue_tail =
      (viewer->classic_queue_tail + 1) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  viewer->classic_queue_count++;
  viewer->bitmap_updates_queued++;
  viewer_note_classic_queue_state_locked(viewer);
  viewer_classic_apply_latest_policy_locked(viewer);

  /* Signal the viewer thread that a new event is available */
  if (viewer->classic_event)
    SetEvent(viewer->classic_event);

  return TRUE;
}

/* Dequeue the oldest event from the classic queue. Caller must hold send_lock.
 * Returns NULL if queue is empty. Caller must free the returned event. */
static ViewerClassicEvent *viewer_classic_dequeue_locked(Viewer *viewer) {
  ViewerClassicEvent *event = NULL;

  if (!viewer || (viewer->classic_queue_count == 0))
    return NULL;

  event = viewer->classic_queue[viewer->classic_queue_head];
  viewer->classic_queue[viewer->classic_queue_head] = NULL;
  viewer->classic_queue_head =
      (viewer->classic_queue_head + 1) % VIEWER_CLASSIC_QUEUE_CAPACITY;
  viewer->classic_queue_count--;
  viewer_note_classic_queue_state_locked(viewer);

  return event;
}

/* ---- SurfaceBits queue: queue operations ---- */

/* Drop the oldest entry from the SurfaceBits queue. Caller must hold send_lock.
 */
static void viewer_surface_bits_queue_drop_oldest_locked(Viewer *viewer) {
  ViewerSurfaceBitsEvent *oldest = NULL;

  if (viewer->surface_bits_queue_count == 0)
    return;

  oldest = viewer->surface_bits_queue[viewer->surface_bits_queue_head];
  viewer->surface_bits_queue[viewer->surface_bits_queue_head] = NULL;
  viewer->surface_bits_queue_head = (viewer->surface_bits_queue_head + 1) %
                                    VIEWER_SURFACE_BITS_QUEUE_CAPACITY;
  viewer->surface_bits_queue_count--;

  viewer_surface_bits_event_free(oldest);
  viewer->surface_bits_queue_dropped++;
}

static void viewer_surface_bits_queue_clear_locked(Viewer *viewer) {
  while (viewer->surface_bits_queue_count > 0)
    viewer_surface_bits_queue_drop_oldest_locked(viewer);
}

/* Enqueue a pre-built SurfaceBits event into the viewer's queue.
 * Caller must hold send_lock. Returns TRUE on success. */
static BOOL
viewer_surface_bits_enqueue_event_locked(Viewer *viewer,
                                         ViewerSurfaceBitsEvent *event) {
  if (!viewer || !event)
    return FALSE;

  /* If queue is full, drop oldest entries to make room */
  while (viewer->surface_bits_queue_count >=
         VIEWER_SURFACE_BITS_QUEUE_CAPACITY) {
    WLog_WARN(TAG,
              "Viewer %u SurfaceBits queue full (%u entries), dropping oldest",
              viewer->id, viewer->surface_bits_queue_count);
    viewer_surface_bits_queue_drop_oldest_locked(viewer);

    /* Note: do NOT set needs_full_refresh here. SurfaceBits ARE the
     * refresh data — setting needs_full_refresh would cause the pump
     * to drop all queued SurfaceBits, creating a deadlock. */
  }

  viewer->surface_bits_queue[viewer->surface_bits_queue_tail] = event;
  viewer->surface_bits_queue_tail = (viewer->surface_bits_queue_tail + 1) %
                                    VIEWER_SURFACE_BITS_QUEUE_CAPACITY;
  viewer->surface_bits_queue_count++;
  viewer->surface_bits_updates_queued++;

  /* Signal the viewer thread that a new event is available */
  if (viewer->classic_event)
    SetEvent(viewer->classic_event);

  return TRUE;
}

/* Dequeue the oldest event from the SurfaceBits queue. Caller must hold
 * send_lock. Returns NULL if queue is empty. Caller must free the returned
 * event. */
static ViewerSurfaceBitsEvent *
viewer_surface_bits_dequeue_locked(Viewer *viewer) {
  ViewerSurfaceBitsEvent *event = NULL;

  if (!viewer || (viewer->surface_bits_queue_count == 0))
    return NULL;

  event = viewer->surface_bits_queue[viewer->surface_bits_queue_head];
  viewer->surface_bits_queue[viewer->surface_bits_queue_head] = NULL;
  viewer->surface_bits_queue_head = (viewer->surface_bits_queue_head + 1) %
                                    VIEWER_SURFACE_BITS_QUEUE_CAPACITY;
  viewer->surface_bits_queue_count--;

  return event;
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
  freerdp_peer *peer = viewer ? viewer->peer : NULL;

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

  /* Hold rdp_update_lock across the entire drain loop so that mstsc receives

   * * all bitmap updates as a continuous stream without rendering between
   *
   * individual sends. */
  if (peer && peer->context && peer->context->update)
    rdp_update_lock(peer->context->update);

  for (;;) {
    /* Drain one queued backend BITMAP_UPDATE at a time. Coalescing is disabled

     * * for now; large source batches are split into bounded chunks by the
     * send
     * helper below. */
    EnterCriticalSection(&viewer->send_lock);

    /* Skip if viewer needs full refresh (will resync via refresh path) */
    if (viewer->needs_full_refresh) {
      /* Drop all queued updates — they're stale relative to the
       * upcoming full refresh */
      if (viewer->classic_queue_count > 0) {
        WLog_INFO(TAG,
                  "Viewer %u pump-classic: dropping %" PRIu32
                  " queued updates (needs full refresh)",
                  viewer->id, viewer->classic_queue_count);
        viewer_classic_queue_clear_locked(viewer);
      }
      LeaveCriticalSection(&viewer->send_lock);
      break;
    }

    event = viewer_classic_dequeue_locked(viewer);
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
     * rdp_update_lock is already held across the entire pump loop, so mstsc

     * * receives all updates as a continuous stream. */
    if (!viewer_send_bitmap_update_locked(viewer, event->bitmap)) {
      WLog_WARN(TAG, "Viewer %u pump-classic: send failed", viewer->id);
      viewer_classic_event_free(event);
      /* Send failure is not fatal — the viewer may recover */
      continue;
    }

    pumped++;
    if (event->generation > viewer->classic_last_generation_sent)
      viewer->classic_last_generation_sent = event->generation;
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
    sb_event = viewer_surface_bits_dequeue_locked(viewer);
    LeaveCriticalSection(&viewer->send_lock);

    if (!sb_event)
      break;

    /* Send outside send_lock — rdp_update_lock provides FreeRDP's own sync,
     * and the event data is locally owned after dequeue. */
    if (!viewer_send_surface_bits(viewer, &sb_event->cmd)) {
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

  /* Release the update lock acquired at the top of this function. */
  if (peer && peer->context && peer->context->update)
    rdp_update_unlock(peer->context->update);

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
  gfx->initialized = TRUE;
  return TRUE;
}

static void viewer_graphics_context_reset(ViewerGraphicsContext *gfx,
                                          BackendClient *backend) {
  UINT32 width = 0;
  UINT32 height = 0;

  if (!gfx)
    return;

  viewer_get_backend_layout(backend, &width, &height, NULL);
  gfx->negotiated_width = width;
  gfx->negotiated_height = height;
  gfx->post_connect_complete = FALSE;
  gfx->ready = FALSE;
  gfx->force_full_present = TRUE;
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
  viewer->classic_queue_head = 0;
  viewer->classic_queue_tail = 0;
  viewer->classic_queue_count = 0;
  memset(viewer->classic_queue, 0, sizeof(viewer->classic_queue));
  viewer->surface_bits_queue_head = 0;
  viewer->surface_bits_queue_tail = 0;
  viewer->surface_bits_queue_count = 0;
  memset(viewer->surface_bits_queue, 0, sizeof(viewer->surface_bits_queue));
  viewer->surface_bits_updates_sent = 0;
  viewer->surface_bits_updates_failed = 0;
  viewer->surface_bits_updates_skipped_writeblock = 0;
  viewer->surface_bits_updates_skipped_throttle = 0;
  viewer->surface_bits_updates_queued = 0;
  viewer->surface_bits_queue_dropped = 0;
  viewer->surface_bits_send_time_total_us = 0;
  viewer->surface_bits_send_time_max_us = 0;
  viewer->surface_bits_payload_bytes_sent = 0;
  viewer->classic_event = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (!viewer->classic_event) {
    DeleteCriticalSection(&viewer->send_lock);
    return FALSE;
  }
  return TRUE;
}

static void viewer_send_state_uninit(Viewer *viewer) {
  if (!viewer || !viewer->classic_event)
    return;

  /* Free any remaining classic queue entries */
  EnterCriticalSection(&viewer->send_lock);
  viewer_classic_queue_clear_locked(viewer);
  viewer_surface_bits_queue_clear_locked(viewer);
  viewer->classic_last_generation_sent = 0;
  LeaveCriticalSection(&viewer->send_lock);

  if (viewer->classic_event) {
    CloseHandle(viewer->classic_event);
    viewer->classic_event = NULL;
  }

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

static void viewer_pointer_shape_entry_reset(PointerShapeEntry *shape) {
  if (!shape)
    return;

  free(shape->xorMaskData);
  free(shape->andMaskData);
  memset(shape, 0, sizeof(*shape));
}

static BOOL viewer_pointer_shape_entry_copy(PointerShapeEntry *destination,
                                            const PointerShapeEntry *source) {
  if (!destination || !source)
    return FALSE;

  memset(destination, 0, sizeof(*destination));
  *destination = *source;
  destination->xorMaskData = NULL;
  destination->andMaskData = NULL;

  if (source->xorMaskLength > 0) {
    if (!source->xorMaskData)
      return FALSE;
    destination->xorMaskData = (BYTE *)malloc(source->xorMaskLength);
    if (!destination->xorMaskData) {
      viewer_pointer_shape_entry_reset(destination);
      return FALSE;
    }
    memmove(destination->xorMaskData, source->xorMaskData,
            source->xorMaskLength);
  }

  if (source->andMaskLength > 0) {
    if (!source->andMaskData) {
      viewer_pointer_shape_entry_reset(destination);
      return FALSE;
    }
    destination->andMaskData = (BYTE *)malloc(source->andMaskLength);
    if (!destination->andMaskData) {
      viewer_pointer_shape_entry_reset(destination);
      return FALSE;
    }
    memmove(destination->andMaskData, source->andMaskData,
            source->andMaskLength);
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
  PointerShapeEntry *active_shape = NULL;
  PointerShapeEntry shape_copy = {0};
  POINTER_SYSTEM_UPDATE pointer_system = {0};
  POINTER_POSITION_UPDATE pointer_position = {0};
  POINTER_COLOR_UPDATE pointer_color = {0};
  POINTER_NEW_UPDATE pointer_new = {0};
  UINT64 position_generation = 0;
  UINT64 shape_generation = 0;
  BOOL shape_changed = FALSE;
  BOOL position_changed = FALSE;
  BOOL send_shape = FALSE;
  BOOL send_position = FALSE;
  BOOL sent = TRUE;

  if (!viewer || !server || !backend || !peer || !peer->context ||
      !peer->context->update || !peer->context->update->pointer ||
      !viewer->connected || !viewer->activated || !peer->activated)
    return FALSE;

  if (!viewer_update_ready(viewer, "pointer"))
    return FALSE;

  backend_get_pointer_snapshot(backend, &pointer_x, &pointer_y,
                               &pointer_visible, &pointer_type, &active_shape,
                               &position_generation, &shape_generation);
  WLog_INFO(TAG, "viewer_forward_pointer: x=%u y=%u visible=%d gen=%llu->%llu",
            pointer_x, pointer_y, pointer_visible,
            (unsigned long long)viewer->last_pointer_position_generation,
            (unsigned long long)position_generation);
  if (active_shape &&
      !viewer_pointer_shape_entry_copy(&shape_copy, active_shape))
    return FALSE;

  shape_changed =
      force || (shape_generation != viewer->last_pointer_shape_generation);
  position_changed = force || (position_generation !=
                               viewer->last_pointer_position_generation);
  send_shape = shape_changed;
  send_position = position_changed && pointer_visible;
  /* disabled: (void)viewer_forward_pointer; logging kept for debug */
  WLog_INFO(TAG, "  shape=%d pos=%d send_shape=%d send_position=%d",
            shape_changed, position_changed, send_shape, send_position);

  if (!send_shape && !send_position) {
    viewer_pointer_shape_entry_reset(&shape_copy);
    return TRUE;
  }

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    if (!peer->DrainOutputBuffer || (peer->DrainOutputBuffer(peer) < 0) ||
        peer->IsWriteBlocked(peer)) {
      viewer_pointer_shape_entry_reset(&shape_copy);
      return FALSE;
    }
  }

  pointer_position.xPos = pointer_x;
  pointer_position.yPos = pointer_y;
  pointer_system.type = pointer_visible ? pointer_type : SYSPTR_NULL;
  pointer_color.cacheIndex = shape_copy.cacheIndex;
  pointer_color.hotSpotX = shape_copy.hotSpotX;
  pointer_color.hotSpotY = shape_copy.hotSpotY;
  pointer_color.width = shape_copy.width;
  pointer_color.height = shape_copy.height;
  pointer_color.lengthAndMask = shape_copy.andMaskLength;
  pointer_color.lengthXorMask = shape_copy.xorMaskLength;
  pointer_color.xorMaskData = shape_copy.xorMaskData;
  pointer_color.andMaskData = shape_copy.andMaskData;
  pointer_new.xorBpp = shape_copy.xorBpp;
  pointer_new.colorPtrAttr = pointer_color;

  rdp_update_lock(peer->context->update);
  if (send_shape) {
    if (!pointer_visible || !active_shape) {
      IFCALLRET(peer->context->update->pointer->PointerSystem, sent,
                peer->context, &pointer_system);
    } else if ((shape_copy.xorBpp > 0) &&
               peer->context->update->pointer->PointerNew) {
      IFCALLRET(peer->context->update->pointer->PointerNew, sent, peer->context,
                &pointer_new);
    } else {
      IFCALLRET(peer->context->update->pointer->PointerColor, sent,
                peer->context, &pointer_color);
    }
  }

  if (sent && send_position && peer->context->update->pointer->PointerPosition)
    IFCALLRET(peer->context->update->pointer->PointerPosition, sent,
              peer->context, &pointer_position);
  rdp_update_unlock(peer->context->update);

  viewer_pointer_shape_entry_reset(&shape_copy);
  if (!sent)
    return FALSE;

  WLog_INFO(TAG, "  sending: send_pos=%d sent=%d final_gen=%llu", send_position,
            sent, (unsigned long long)position_generation);
  if (send_position)
    viewer->last_pointer_position_generation = position_generation;
  viewer->last_pointer_shape_generation = shape_generation;
  return TRUE;
}

static BOOL viewer_send_surface_bits(Viewer *viewer,
                                     const SURFACE_BITS_COMMAND *cmd) {
  BOOL ret = FALSE;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;
  UINT64 send_started_us = 0;
  UINT64 send_us = 0;

  if (!viewer || !peer || !peer->context || !peer->context->update || !cmd)
    return FALSE;

  if (!viewer_update_ready(viewer, "SurfaceBits"))
    return FALSE;

  /* Option A: Skip on write-block — don't stall the viewer thread */
  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    viewer->write_block_events++;
    viewer->surface_bits_updates_skipped_writeblock++;
    return FALSE;
  }

  send_started_us = viewer_perf_now_us();
  rdp_update_lock(peer->context->update);
  IFCALLRET(peer->context->update->SurfaceBits, ret, peer->context, cmd);
  rdp_update_unlock(peer->context->update);
  send_us = viewer_perf_now_us() - send_started_us;
  viewer->surface_bits_send_time_total_us += send_us;
  if (send_us > viewer->surface_bits_send_time_max_us)
    viewer->surface_bits_send_time_max_us = send_us;
  if (ret) {
    viewer->packets_sent++;
    viewer->surface_bits_updates_sent++;
    viewer->surface_bits_payload_bytes_sent += cmd->bmp.bitmapDataLength;
  } else {
    viewer->packets_failed++;
    viewer->surface_bits_updates_failed++;
  }
  return ret;
}

static BOOL viewer_send_bitmap_update(Viewer *viewer,
                                      const BITMAP_UPDATE *bitmap) {
  freerdp_peer *peer = viewer ? viewer->peer : NULL;

  if (!viewer || !peer || !peer->context || !peer->context->update || !bitmap)
    return FALSE;

  if (!viewer_update_ready(viewer, "BitmapUpdate"))
    return FALSE;

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    viewer->write_block_events++;
    viewer->bitmap_write_block_events++;
    viewer->bitmap_updates_skipped_writeblock++;
    return FALSE;
  }

  return viewer_send_bitmap_update_chunks(viewer, bitmap, FALSE,
                                          "BitmapUpdate");
}

/* Same as viewer_send_bitmap_update but assumes rdp_update_lock is already
 *
 * held. Used by viewer_pump_classic to batch multiple sends under a single
 * lock
 * acquisition, preventing mstsc from rendering between individual
 * updates. */
static BOOL viewer_send_bitmap_update_locked(Viewer *viewer,
                                             const BITMAP_UPDATE *bitmap) {
  freerdp_peer *peer = viewer ? viewer->peer : NULL;

  if (!viewer || !peer || !peer->context || !peer->context->update || !bitmap)
    return FALSE;

  if (!viewer_update_ready(viewer, "BitmapUpdateLocked"))
    return FALSE;

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    viewer->write_block_events++;
    viewer->bitmap_write_block_events++;
    viewer->bitmap_updates_skipped_writeblock++;
    return FALSE;
  }

  return viewer_send_bitmap_update_chunks(viewer, bitmap, TRUE,
                                          "BitmapUpdateLocked");
}

static BOOL viewer_send_frame_marker(Viewer *viewer,
                                     const SURFACE_FRAME_MARKER *marker) {
  BOOL ret = FALSE;
  freerdp_peer *peer = viewer ? viewer->peer : NULL;

  if (!viewer || !peer || !peer->context || !peer->context->update || !marker)
    return FALSE;

  if (!viewer_update_ready(viewer, "SurfaceFrameMarker"))
    return FALSE;

  rdp_update_lock(peer->context->update);
  IFCALLRET(peer->context->update->SurfaceFrameMarker, ret, peer->context,
            marker);
  rdp_update_unlock(peer->context->update);
  if (ret)
    viewer->packets_sent++;
  else
    viewer->packets_failed++;
  return ret;
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
  BOOL sent = FALSE;

  if (!server || !viewer)
    return FALSE;

  if (!viewer_publisher_classic_baseline_snapshot(
          &server->publisher, &server->framebuffer, &snapshot)) {
    WLog_WARN(TAG, "Viewer %u RDPEGFX baseline snapshot unavailable",
              viewer->id);
    return viewer_gfx_enter_classic_fallback(
        server, viewer, now, "RDPEGFX framebuffer baseline unavailable");
  }

  sent = viewer_gfx_pipeline_send_snapshot(server, viewer, &snapshot);
  viewer_framebuffer_snapshot_free(&snapshot);

  if (!sent) {
    WLog_WARN(TAG, "Viewer %u RDPEGFX framebuffer baseline send failed",
              viewer->id);
    return viewer_gfx_enter_classic_fallback(
        server, viewer, now, "RDPEGFX framebuffer baseline send failed");
  }

  viewer_gfx_pipeline_on_baseline_result(viewer, now, TRUE, NULL);

  EnterCriticalSection(&viewer->send_lock);
  viewer->needs_full_refresh = FALSE;
  viewer->full_refresh_deadline_ts = 0;
  LeaveCriticalSection(&viewer->send_lock);

  WLog_INFO(TAG, "Viewer %u RDPEGFX framebuffer baseline sent", viewer->id);
  return TRUE;
}

static BOOL viewer_gfx_try_send_dirty_update(ViewerServer *server,
                                             Viewer *viewer, UINT64 now) {
  ViewerFramebufferSnapshot snapshot = {0};
  const char *reason = NULL;
  UINT64 last_sent_generation = 0;
  BOOL sent = FALSE;

  if (!server || !viewer || !server->viewer_gfx_enabled)
    return TRUE;

  if (viewer_gfx_pipeline_poll_dirty_pacing(viewer, now, &reason) !=
      VIEWER_GFX_DIRTY_PACING_OK)
    return TRUE;

  EnterCriticalSection(&viewer->gfx.lock);
  last_sent_generation = viewer->gfx.dirty_last_sent_generation;
  LeaveCriticalSection(&viewer->gfx.lock);

  if (!viewer_publisher_gfx_dirty_snapshot(&server->publisher,
                                           &server->framebuffer,
                                           last_sent_generation, &snapshot))
    return TRUE;

  if (!viewer_gfx_pipeline_dirty_update_allowed(server, viewer, &snapshot,
                                                &reason)) {
    viewer_framebuffer_snapshot_free(&snapshot);
    return TRUE;
  }

  sent = viewer_gfx_pipeline_send_dirty_update(server, viewer, &snapshot);
  viewer_framebuffer_snapshot_free(&snapshot);

  if (!sent)
    return viewer_gfx_enter_classic_fallback(
        server, viewer, now, "RDPEGFX dirty update send failed");

  return TRUE;
}

static BOOL viewer_gfx_step_join(ViewerServer *server, Viewer *viewer,
                                 UINT64 now) {
  ViewerGfxJoinResult result = {0};

  if (!server || !viewer)
    return FALSE;

  viewer_gfx_pipeline_step_join(server, viewer, now, &result);
  if (result.actions & VIEWER_GFX_JOIN_ACTION_ENTER_CLASSIC_FALLBACK)
    return viewer_gfx_enter_classic_fallback(server, viewer, now,
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
      if (viewer->classic_event && (wait_count < MAXIMUM_WAIT_OBJECTS)) {
        wait_objects[wait_count] = viewer->classic_event;
        wait_count++;
      }

      wait_status = WaitForMultipleObjects(wait_count, wait_objects, FALSE,
                                           wait_timeout_ms);
      if (wait_status == WAIT_TIMEOUT)
        wait_status = WAIT_OBJECT_0;
      if (wait_status == WAIT_FAILED)
        break;

      /* Reset the classic event signal — we'll drain the queue below */
      if (viewer->classic_event)
        ResetEvent(viewer->classic_event);
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
          viewer_gfx_pipeline_disable_rdpgfx_locked(viewer);
          LeaveCriticalSection(&viewer->gfx.lock);
          (void)viewer_gfx_enter_classic_fallback(
              g_viewer_server, viewer, now, "RDPEGFX channel open failed");
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
        viewer_gfx_pipeline_disable_rdpgfx_locked(viewer);
        LeaveCriticalSection(&viewer->gfx.lock);
        WLog_WARN(TAG,
                  "Viewer %u RDPEGFX message handling failed; falling back to "
                  "classic path",
                  viewer->id);
        (void)viewer_gfx_enter_classic_fallback(
            g_viewer_server, viewer, now, "RDPEGFX message handling failed");
        continue;
      }
      viewer_gfx_apply_caps_result_locked(g_viewer_server, viewer, &caps_result,
                                          now, &caps_enter_classic_fallback,
                                          &caps_classic_fallback_reason);
      LeaveCriticalSection(&viewer->gfx.lock);
      if (caps_enter_classic_fallback) {
        (void)viewer_gfx_enter_classic_fallback(g_viewer_server, viewer, now,
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

    /* viewer_forward_pointer disabled: cursor position forwarding to viewers

     * * without input lock is not reliable and causes complexity. Viewer
     * cursor
     * position comes from the RDP server via on_pointer_position
     * callbacks. */
    /* (void)viewer_forward_pointer(viewer, FALSE); */

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
  char *viewer_user = NULL;
  char *viewer_domain = NULL;
  char *viewer_password = NULL;
  const char *comparison_user = NULL;
  const char *comparison_domain = NULL;
  const char *credential_source = "none";
  BOOL accepted = FALSE;

  (void)automatic;

  if (!peer || !server) {
    WLog_WARN(TAG, "Viewer-side logon rejected: missing peer or server");
    return FALSE;
  }

  WLog_INFO(TAG, "Viewer-side logon: auth_mode=%s nla_enabled=%s",
            viewer_auth_mode_name(server->security.auth_mode),
            server->security.nla_enabled ? "true" : "false");

  if (server->security.auth_mode == VIEWER_AUTH_MODE_NONE)
    return TRUE;

  if ((server->security.auth_mode == VIEWER_AUTH_MODE_BACKEND_CREDENTIALS) &&
      !server->backend) {
    WLog_WARN(TAG,
              "Viewer-side logon rejected: auth_mode=%s nla_enabled=%s "
              "missing backend credentials source",
              viewer_auth_mode_name(server->security.auth_mode),
              server->security.nla_enabled ? "true" : "false");
    return FALSE;
  }

  if (viewer_settings_credentials_to_utf8(peer, &viewer_user, &viewer_domain,
                                          &viewer_password)) {
    credential_source = "settings";
  } else if (viewer_identity_credentials_to_utf8(
                 identity, &viewer_user, &viewer_domain, &viewer_password)) {
    credential_source = "identity";
  } else {
    viewer_user = _strdup("");
    viewer_domain = _strdup("");
    viewer_password = _strdup("");
    credential_source = "none";
  }

  if (!viewer_user || !viewer_domain || !viewer_password) {
    WLog_WARN(TAG,
              "Viewer-side logon rejected: failed to read credentials "
              "auth_mode=%s nla_enabled=%s credential_source=%s",
              viewer_auth_mode_name(server->security.auth_mode),
              server->security.nla_enabled ? "true" : "false",
              credential_source);
    goto out;
  }

  comparison_user = server->backend->username;
  comparison_domain = server->backend->domain;
  accepted = viewer_backend_credentials_match(server->backend, viewer_user,
                                              viewer_domain, viewer_password);
  if (accepted) {
    WLog_INFO(TAG,
              "Viewer-side logon accepted auth_mode=%s nla_enabled=%s "
              "credential_source=%s "
              "username_present=%s domain_present=%s password_present=%s "
              "viewer='%s%s%s' comparison_user='%s' comparison_domain='%s'",
              viewer_auth_mode_name(server->security.auth_mode),
              server->security.nla_enabled ? "true" : "false",
              credential_source, viewer_user[0] ? "true" : "false",
              viewer_domain[0] ? "true" : "false",
              viewer_password[0] ? "true" : "false", viewer_domain,
              viewer_domain[0] ? "\\" : "", viewer_user, comparison_user,
              viewer_comparison_domain_label(comparison_domain));
  } else {
    WLog_WARN(TAG,
              "Viewer-side logon rejected: credentials mismatch auth_mode=%s "
              "nla_enabled=%s credential_source=%s username_present=%s "
              "domain_present=%s "
              "password_present=%s comparison_user='%s' comparison_domain='%s'",
              viewer_auth_mode_name(server->security.auth_mode),
              server->security.nla_enabled ? "true" : "false",
              credential_source, viewer_user[0] ? "true" : "false",
              viewer_domain[0] ? "true" : "false",
              viewer_password[0] ? "true" : "false", comparison_user,
              viewer_comparison_domain_label(comparison_domain));
  }

out:
  free(viewer_user);
  free(viewer_domain);
  free(viewer_password);
  return accepted;
}

static BOOL peer_activate(freerdp_peer *peer) {
  Viewer *viewer = find_viewer_by_peer(peer);
  UINT64 now = platform_get_timestamp_ms();
  ViewerGfxJoinResult join_result = {0};
  ViewerGfxNegotiationOutcome negotiation_outcome =
      VIEWER_GFX_NEGOTIATION_PENDING;
  BOOL classic_activation = FALSE;

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
static BOOL peer_reached_state(freerdp_peer *peer, CONNECTION_STATE state) {
  ViewerServer *server = NULL;
  const MonitorLayout *layout = NULL;
  rdpSettings *settings = NULL;
  UINT32 i = 0;

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
  free(valid_dirty_rects);
  if (!updated)
    return FALSE;

  EnterCriticalSection(&server->framebuffer.lock);
  generation = server->framebuffer.generation;
  LeaveCriticalSection(&server->framebuffer.lock);
  viewer_publisher_note_framebuffer_update(
      &server->publisher, generation, update_dirty_rect_count,
      update_dirty_rect_count > VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS);
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
    viewer->gfx.negotiated_width = width;
    viewer->gfx.negotiated_height = height;
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
    BOOL throttled = FALSE;
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

    if (!ready_to_send) {
      viewer_release_publish_ref(server, viewer);
      continue;
    }

    if (viewer->consecutive_lag_intervals >= VIEWER_THROTTLE_LAG_INTERVALS) {
      /* Per-viewer throttle: skip updates for slow viewers.
       * However, SurfaceBits ARE the refresh data — even when throttled,
       * we must still deliver them to allow the viewer to resync.
       * Clear the throttle gate and enqueue. */
      EnterCriticalSection(&viewer->send_lock);
      viewer->surface_bits_updates_skipped_throttle++;
      /* Don't set needs_full_refresh for SurfaceBits — they ARE the
       * refresh data. Setting it would cause the pump to drop them. */
      LeaveCriticalSection(&viewer->send_lock);
      throttled = TRUE;
      throttled_count++;
      if (first_throttled_viewer_id == 0)
        first_throttled_viewer_id = viewer->id;
      /* Fall through to enqueue — SurfaceBits must be delivered even
       * for throttled viewers, because they carry the actual pixel data
       * needed to resync. */
    }

    /* Clear needs_full_refresh if set — SurfaceBits ARE the refresh data.
     * Unlike BitmapUpdate where a full refresh is a separate mechanism,
     * SurfaceBits tiles are the only way the viewer receives pixel data,
     * so they must always be delivered. */
    if (viewer->needs_full_refresh) {
      EnterCriticalSection(&viewer->send_lock);
      viewer->needs_full_refresh = FALSE;
      viewer->full_refresh_deadline_ts = 0;
      LeaveCriticalSection(&viewer->send_lock);
      gated_full_refresh_count++;
      if (first_gated_viewer_id == 0)
        first_gated_viewer_id = viewer->id;
    }

    /* Always enqueue SurfaceBits — they carry pixel data that the viewer
     * needs regardless of throttle or refresh state. */
    event = viewer_surface_bits_event_new(cmd);
    if (!event) {
      enqueue_failed_count++;
      viewer_release_publish_ref(server, viewer);
      continue;
    }
    EnterCriticalSection(&viewer->send_lock);
    enqueued = viewer_surface_bits_enqueue_event_locked(viewer, event);
    LeaveCriticalSection(&viewer->send_lock);
    if (enqueued)
      enqueued_count++;
    else {
      enqueue_failed_count++;
      viewer_surface_bits_event_free(event);
    }

    if (enqueued)
      sent_any = TRUE;

    if (throttled)
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
    BOOL throttled = FALSE;
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

    if (!ready_to_send) {
      viewer_release_publish_ref(server, viewer);
      continue;
    }

    if (viewer->needs_full_refresh && !refresh_in_flight) {
      /* The viewer needs a full refresh but no refresh is in flight.
       * This happens when:
       * 1. The viewer just joined and the backend refresh has already
       *    completed (backend_mark_full_refresh_complete was called)
       * 2. The viewer was throttled and the refresh completed
       * In the classic path there are no frame markers to clear
       * needs_full_refresh, so we clear it here and enqueue the
       * current bitmap update. The viewer will receive this and
       * subsequent updates normally. */
      EnterCriticalSection(&viewer->send_lock);
      viewer->needs_full_refresh = FALSE;
      viewer->full_refresh_deadline_ts = 0;
      LeaveCriticalSection(&viewer->send_lock);
      gated_full_refresh_count++;
      if (first_gated_viewer_id == 0)
        first_gated_viewer_id = viewer->id;

      /* Deep copy outside lock, then enqueue under lock */
      event = viewer_classic_event_new(bitmap);
      if (!event) {
        enqueue_failed_count++;
        viewer_release_publish_ref(server, viewer);
        continue;
      }
      EnterCriticalSection(&viewer->send_lock);
      enqueued = viewer_classic_enqueue_event_locked(viewer, event);
      LeaveCriticalSection(&viewer->send_lock);
      if (enqueued)
        enqueued_count++;
      else {
        enqueue_failed_count++;
        viewer_classic_event_free(event);
      }
    } else if (viewer->needs_full_refresh && refresh_in_flight) {
      /* A full refresh is in flight — this bitmap IS the refresh data.
       * Clear the gate and enqueue so the viewer receives it. */
      EnterCriticalSection(&viewer->send_lock);
      viewer->needs_full_refresh = FALSE;
      viewer->full_refresh_deadline_ts = 0;
      LeaveCriticalSection(&viewer->send_lock);

      event = viewer_classic_event_new(bitmap);
      if (!event) {
        enqueue_failed_count++;
        viewer_release_publish_ref(server, viewer);
        continue;
      }
      EnterCriticalSection(&viewer->send_lock);
      enqueued = viewer_classic_enqueue_event_locked(viewer, event);
      LeaveCriticalSection(&viewer->send_lock);
      if (enqueued)
        enqueued_count++;
      else {
        enqueue_failed_count++;
        viewer_classic_event_free(event);
      }
    } else if (viewer->consecutive_lag_intervals >=
               VIEWER_THROTTLE_LAG_INTERVALS) {
      /* Per-viewer throttle: skip updates for slow viewers.
       * They will resync via full refresh when they recover. */
      EnterCriticalSection(&viewer->send_lock);
      viewer->bitmap_updates_skipped_throttle++;
      viewer->needs_full_refresh = TRUE;
      viewer->full_refresh_deadline_ts =
          platform_get_timestamp_ms() + FULL_REFRESH_TIMEOUT_MS;
      LeaveCriticalSection(&viewer->send_lock);
      throttled = TRUE;
      throttled_count++;
      if (first_throttled_viewer_id == 0)
        first_throttled_viewer_id = viewer->id;
    } else {
      /* Option B: Enqueue the bitmap update for async delivery
       * by the viewer thread. Deep copy outside lock, then
       * enqueue under lock to minimize send_lock hold time. */
      event = viewer_classic_event_new(bitmap);
      if (!event) {
        enqueue_failed_count++;
        viewer_release_publish_ref(server, viewer);
        continue;
      }
      EnterCriticalSection(&viewer->send_lock);
      enqueued = viewer_classic_enqueue_event_locked(viewer, event);
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

    if (throttled)
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
