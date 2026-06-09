#include "viewer_server_internal.h"

void viewer_server_set_gfx_dirty_limits(ViewerServer *server,
                                        UINT32 max_in_flight_frames,
                                        UINT64 max_in_flight_bytes) {
  if (!server)
    return;

  if (max_in_flight_frames == 0)
    max_in_flight_frames = 1U;
  if (max_in_flight_frames > VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY)
    max_in_flight_frames = VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY;
  if (max_in_flight_bytes == 0)
    max_in_flight_bytes = VIEWER_GFX_DIRTY_MAX_IN_FLIGHT_BYTES;

  server->viewer_gfx_dirty_max_in_flight_frames = max_in_flight_frames;
  server->viewer_gfx_dirty_max_in_flight_bytes = max_in_flight_bytes;
}
