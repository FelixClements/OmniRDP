#include "viewer_classic_transport.h"

#include "platform_compat.h"

#include <freerdp/peer.h>
#include <inttypes.h>
#include <string.h>
#include <winpr/wlog.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#define TAG "multiplexer.viewer.classic_transport"
#define VIEWER_CLASSIC_MAX_RECTS_PER_SEND 64U
#define VIEWER_CLASSIC_MAX_BYTES_PER_SEND (512U * 1024U)

static UINT64 viewer_classic_transport_now_us(void) {
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

static BOOL
viewer_classic_transport_ready(const ViewerClassicTransport *transport) {
  return transport && transport->peer && transport->peer->context &&
         transport->peer->context->update;
}

static void viewer_counter_inc(UINT64 *counter) {
  if (counter)
    (*counter)++;
}

static void viewer_counter_add(UINT64 *counter, UINT64 value) {
  if (counter)
    *counter += value;
}

static BOOL viewer_bitmap_bpp_sane(UINT32 bpp) {
  return (bpp == 8) || (bpp == 15) || (bpp == 16) || (bpp == 24) || (bpp == 32);
}

static BOOL viewer_get_desktop_size(const ViewerClassicTransport *transport,
                                    UINT32 *width, UINT32 *height) {
  rdpSettings *settings = NULL;

  if (!width || !height)
    return FALSE;

  *width = 0;
  *height = 0;
  if (!transport || !transport->peer || !transport->peer->context)
    return FALSE;

  settings = transport->peer->context->settings;
  if (!settings)
    return FALSE;

  *width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
  *height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
  return (*width > 0) && (*height > 0);
}

static BOOL viewer_validate_bitmap_rect(
    const ViewerClassicTransport *transport, const BITMAP_UPDATE *bitmap,
    const BITMAP_DATA *rect, UINT32 rect_index, UINT32 desktop_width,
    UINT32 desktop_height, const char *operation, BOOL log_invalid) {
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
        transport ? transport->viewer_id : 0, operation ? operation : "unknown",
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

BOOL viewer_classic_transport_begin_batch(
    const ViewerClassicTransport *transport) {
  if (!viewer_classic_transport_ready(transport))
    return FALSE;

  rdp_update_lock(transport->peer->context->update);
  return TRUE;
}

void viewer_classic_transport_end_batch(
    const ViewerClassicTransport *transport) {
  if (!viewer_classic_transport_ready(transport))
    return;

  rdp_update_unlock(transport->peer->context->update);
}

static BOOL viewer_send_bitmap_update_chunks(ViewerClassicTransport *transport,
                                             const BITMAP_UPDATE *bitmap,
                                             BOOL update_lock_held,
                                             const char *operation) {
  freerdp_peer *peer = transport ? transport->peer : NULL;
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

  if (!viewer_classic_transport_ready(transport) || !bitmap)
    return FALSE;

  if (!bitmap->rectangles || (bitmap->number == 0)) {
    WLog_WARN(TAG,
              "Viewer %u dropping BitmapUpdate op=%s: count=%" PRIu32
              " rectangles_present=%s",
              transport->viewer_id, operation ? operation : "unknown",
              bitmap->number, bitmap->rectangles ? "true" : "false");
    return TRUE;
  }

  if (!viewer_get_desktop_size(transport, &desktop_width, &desktop_height)) {
    WLog_WARN(TAG, "Viewer %u dropping BitmapUpdate op=%s: no desktop size",
              transport->viewer_id, operation ? operation : "unknown");
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
      send_started_us = viewer_classic_transport_now_us();                     \
      if (!update_lock_held)                                                   \
        rdp_update_lock(peer->context->update);                                \
      IFCALLRET(peer->context->update->BitmapUpdate, chunk_ret, peer->context, \
                &chunk);                                                       \
      if (!update_lock_held)                                                   \
        rdp_update_unlock(peer->context->update);                              \
      send_us = viewer_classic_transport_now_us() - send_started_us;           \
      send_time_total_us += send_us;                                           \
      if (transport->bitmap_send_time_max_us &&                                \
          (send_us > *transport->bitmap_send_time_max_us))                     \
        *transport->bitmap_send_time_max_us = send_us;                         \
      if (chunk_ret) {                                                         \
        viewer_counter_inc(transport->packets_sent);                           \
        viewer_counter_inc(transport->bitmap_updates_sent);                    \
        viewer_counter_add(transport->bitmap_rectangles_sent, chunk.number);   \
        viewer_counter_add(transport->bitmap_payload_bytes_sent, chunk_bytes); \
        valid_sent += chunk.number;                                            \
        chunks_sent++;                                                         \
      } else {                                                                 \
        viewer_counter_inc(transport->packets_failed);                         \
        viewer_counter_inc(transport->bitmap_updates_failed);                  \
        ret = FALSE;                                                           \
      }                                                                        \
      chunk.number = 0;                                                        \
      chunk_bytes = 0;                                                         \
    }                                                                          \
  } while (0)

  for (i = 0; i < bitmap->number; i++) {
    const BITMAP_DATA *rect = &bitmap->rectangles[i];
    UINT32 rect_bytes = rect ? (UINT32)rect->bitmapLength : 0;

    if (!viewer_validate_bitmap_rect(transport, bitmap, rect, i, desktop_width,
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

  viewer_counter_add(transport->bitmap_send_time_total_us, send_time_total_us);

  if ((valid_sent == 0) && (invalid_dropped > 0)) {
    WLog_WARN(TAG,
              "Viewer %u dropped BitmapUpdate op=%s: all rects invalid "
              "original=%" PRIu32 " invalid=%" PRIu32,
              transport->viewer_id, operation ? operation : "unknown",
              bitmap->number, invalid_dropped);
    return TRUE;
  }

  WLog_INFO(TAG,
            "Viewer %u BitmapUpdate op=%s summary original=%" PRIu32
            " valid_sent=%" PRIu32 " invalid_dropped=%" PRIu32
            " chunks_sent=%" PRIu32 " total_payload=%" PRIu64
            " max_rects=%u max_bytes=%u send_ok=%s",
            transport->viewer_id, operation ? operation : "unknown",
            bitmap->number, valid_sent, invalid_dropped, chunks_sent,
            total_payload_bytes, VIEWER_CLASSIC_MAX_RECTS_PER_SEND,
            VIEWER_CLASSIC_MAX_BYTES_PER_SEND, ret ? "true" : "false");

  return ret;
}

BOOL viewer_classic_transport_send_bitmap_update(
    ViewerClassicTransport *transport, const BITMAP_UPDATE *bitmap) {
  freerdp_peer *peer = transport ? transport->peer : NULL;

  if (!viewer_classic_transport_ready(transport) || !bitmap)
    return FALSE;

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    viewer_counter_inc(transport->write_block_events);
    viewer_counter_inc(transport->bitmap_write_block_events);
    viewer_counter_inc(transport->bitmap_updates_skipped_writeblock);
    return FALSE;
  }

  return viewer_send_bitmap_update_chunks(transport, bitmap, FALSE,
                                          "BitmapUpdate");
}

BOOL viewer_classic_transport_send_bitmap_update_batched(
    ViewerClassicTransport *transport, const BITMAP_UPDATE *bitmap) {
  freerdp_peer *peer = transport ? transport->peer : NULL;

  if (!viewer_classic_transport_ready(transport) || !bitmap)
    return FALSE;

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    viewer_counter_inc(transport->write_block_events);
    viewer_counter_inc(transport->bitmap_write_block_events);
    viewer_counter_inc(transport->bitmap_updates_skipped_writeblock);
    return FALSE;
  }

  return viewer_send_bitmap_update_chunks(transport, bitmap, TRUE,
                                          "BitmapUpdateLocked");
}

BOOL viewer_classic_transport_send_surface_bits(
    ViewerClassicTransport *transport, const SURFACE_BITS_COMMAND *cmd,
    BOOL update_lock_held) {
  BOOL ret = FALSE;
  freerdp_peer *peer = transport ? transport->peer : NULL;
  UINT64 send_started_us = 0;
  UINT64 send_us = 0;

  if (!viewer_classic_transport_ready(transport) || !cmd)
    return FALSE;

  if (peer->IsWriteBlocked && peer->IsWriteBlocked(peer)) {
    viewer_counter_inc(transport->write_block_events);
    viewer_counter_inc(transport->surface_bits_updates_skipped_writeblock);
    return FALSE;
  }

  send_started_us = viewer_classic_transport_now_us();
  if (!update_lock_held)
    rdp_update_lock(peer->context->update);
  IFCALLRET(peer->context->update->SurfaceBits, ret, peer->context, cmd);
  if (!update_lock_held)
    rdp_update_unlock(peer->context->update);
  send_us = viewer_classic_transport_now_us() - send_started_us;
  viewer_counter_add(transport->surface_bits_send_time_total_us, send_us);
  if (transport->surface_bits_send_time_max_us &&
      (send_us > *transport->surface_bits_send_time_max_us))
    *transport->surface_bits_send_time_max_us = send_us;
  if (ret) {
    viewer_counter_inc(transport->packets_sent);
    viewer_counter_inc(transport->surface_bits_updates_sent);
    viewer_counter_add(transport->surface_bits_payload_bytes_sent,
                       cmd->bmp.bitmapDataLength);
  } else {
    viewer_counter_inc(transport->packets_failed);
    viewer_counter_inc(transport->surface_bits_updates_failed);
  }
  return ret;
}

BOOL viewer_classic_transport_send_frame_marker(
    ViewerClassicTransport *transport, const SURFACE_FRAME_MARKER *marker) {
  BOOL ret = FALSE;
  freerdp_peer *peer = transport ? transport->peer : NULL;

  if (!viewer_classic_transport_ready(transport) || !marker)
    return FALSE;

  rdp_update_lock(peer->context->update);
  IFCALLRET(peer->context->update->SurfaceFrameMarker, ret, peer->context,
            marker);
  rdp_update_unlock(peer->context->update);
  if (ret)
    viewer_counter_inc(transport->packets_sent);
  else
    viewer_counter_inc(transport->packets_failed);
  return ret;
}
