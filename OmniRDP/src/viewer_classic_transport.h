#ifndef VIEWER_CLASSIC_TRANSPORT_H
#define VIEWER_CLASSIC_TRANSPORT_H

#include <freerdp/freerdp.h>
#include <freerdp/update.h>
#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  freerdp_peer *peer;
  UINT32 viewer_id;
  UINT64 *packets_sent;
  UINT64 *packets_failed;
  UINT64 *write_block_events;
  UINT64 *bitmap_updates_sent;
  UINT64 *bitmap_updates_failed;
  UINT64 *bitmap_rectangles_sent;
  UINT64 *bitmap_payload_bytes_sent;
  UINT64 *bitmap_write_block_events;
  UINT64 *bitmap_send_time_total_us;
  UINT64 *bitmap_send_time_max_us;
  UINT64 *bitmap_updates_skipped_writeblock;
  UINT64 *surface_bits_updates_sent;
  UINT64 *surface_bits_updates_failed;
  UINT64 *surface_bits_send_time_total_us;
  UINT64 *surface_bits_send_time_max_us;
  UINT64 *surface_bits_payload_bytes_sent;
  UINT64 *surface_bits_updates_skipped_writeblock;
} ViewerClassicTransport;

BOOL viewer_classic_transport_begin_batch(
    const ViewerClassicTransport *transport);
void viewer_classic_transport_end_batch(
    const ViewerClassicTransport *transport);
BOOL viewer_classic_transport_send_bitmap_update(
    ViewerClassicTransport *transport, const BITMAP_UPDATE *bitmap);
BOOL viewer_classic_transport_send_bitmap_update_batched(
    ViewerClassicTransport *transport, const BITMAP_UPDATE *bitmap);
BOOL viewer_classic_transport_send_surface_bits(
    ViewerClassicTransport *transport, const SURFACE_BITS_COMMAND *cmd,
    BOOL update_lock_held);
BOOL viewer_classic_transport_send_frame_marker(
    ViewerClassicTransport *transport, const SURFACE_FRAME_MARKER *marker);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_CLASSIC_TRANSPORT_H */
