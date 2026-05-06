/**
 * @file backend.h
 * @brief Backend client connection to Windows RDP server
 *
 * Uses FreeRDP 3.25.0 client library to connect to Windows Server.
 * Forwards backend SurfaceBits updates to viewer queues.
 */

#ifndef BACKEND_H
#define BACKEND_H

#include "pointer_shape.h"
#include "viewer_server.h"
#include <freerdp/client.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/freerdp.h>
#include <freerdp/pointer.h>
#include <freerdp/primary.h>
#include <freerdp/secondary.h>
#include <freerdp/update.h>
#include <winpr/synch.h>
#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Backend client context
 */
typedef struct BackendClient {
  rdpContext *context;
  char *hostname;
  UINT16 port;
  char *username;
  char *password;
  char *domain;
  BOOL connected;
  void *frame_callback;
  void *user_data;
  UINT32 desktop_width;
  UINT32 desktop_height;
  MonitorLayout monitor_layout;
  UINT32 layout_generation;
  CRITICAL_SECTION layout_lock;
  CRITICAL_SECTION refresh_lock;
  BOOL full_refresh_requested;
  BOOL full_refresh_in_flight;
  UINT64 full_refresh_generation;
  UINT64 full_refresh_requested_generation;
  UINT64 full_refresh_in_flight_generation;
  UINT64 full_refresh_completed_generation;
  UINT64 full_refresh_requested_timestamp_ms;
  UINT64 full_refresh_in_flight_timestamp_ms;
  UINT64 full_refresh_completed_timestamp_ms;
  UINT32 full_refresh_completed_outcome;
  UINT16 pointer_x;
  UINT16 pointer_y;
  BOOL pointer_visible;
  UINT32 pointer_type;
  UINT64 pointer_position_generation;
  UINT64 pointer_shape_generation;
  RdpgfxClientContext *rdpgfx;
  BOOL rdpgfx_channel_open;
  BOOL rdpgfx_caps_confirmed;
  PointerShapeCache *pointer_shape_cache;
  PointerShapeEntry *active_pointer_shape;
  CRITICAL_SECTION pointer_lock;
  BOOL (*orig_begin_paint)(rdpContext *context);
  BOOL (*orig_end_paint)(rdpContext *context);
  BOOL (*orig_bitmap_update)(rdpContext *context, const BITMAP_UPDATE *bitmap);
  BOOL(*orig_surface_bits)
  (rdpContext *context, const SURFACE_BITS_COMMAND *cmd);
  BOOL(*orig_surface_frame_marker)
  (rdpContext *context, const SURFACE_FRAME_MARKER *marker);
  BOOL(*orig_primary_order_info)
  (rdpContext *context, const ORDER_INFO *order_info, const char *order_name);
  BOOL(*orig_secondary_cache_order_info)
  (rdpContext *context, INT16 orderLength, UINT16 extraFlags, UINT8 orderType,
   const char *orderName);
  UINT64 begin_paint_count;
  UINT64 end_paint_count;
  UINT64 bitmap_update_count;
  UINT64 primary_order_info_count;
  UINT64 secondary_cache_order_info_count;
  UINT64 forwarded_bitmap_update_count;
  UINT64 forwarded_bitmap_update_rectangles;
  UINT64 forwarded_bitmap_update_bytes;
  UINT64 bitmap_update_callback_time_total_us;
  UINT64 bitmap_update_callback_time_max_us;
  UINT64 bitmap_update_publish_time_total_us;
  UINT64 bitmap_update_publish_time_max_us;
  UINT64 bitmap_update_batches_total;
  UINT64 bitmap_update_rectangles_total;
  UINT64 bitmap_update_payload_bytes_total;
  UINT64 forwarded_surface_bits_count;
  UINT64 forwarded_surface_bits_bytes;
  UINT64 forwarded_frame_marker_count;
  UINT64 forwarded_gfx_reset_graphics_count;
  UINT64 forwarded_gfx_create_surface_count;
  UINT64 forwarded_gfx_delete_surface_count;
  UINT64 forwarded_gfx_map_surface_to_output_count;
  UINT64 forwarded_gfx_start_frame_count;
  UINT64 forwarded_gfx_surface_command_count;
  UINT64 forwarded_gfx_end_frame_count;
  UINT64 forwarded_gfx_delete_encoding_context_count;
  /* Saved GDI GFX callbacks — GDI decodes & updates framebuffer, we forward */
  void *gdi_StartFrame;
  void *gdi_EndFrame;
  void *gdi_SurfaceCommand;
  void *gdi_ResetGraphics;
  void *gdi_CreateSurface;
  void *gdi_DeleteSurface;
  void *gdi_MapSurfaceToOutput;
  void *gdi_DeleteEncodingContext;
  void *gdi_OnOpen;
  void *gdi_OnClose;
  void *gdi_CapsConfirm;
} BackendClient;

typedef enum BackendFullRefreshOutcome {
  BACKEND_FULL_REFRESH_OUTCOME_NONE = 0,
  BACKEND_FULL_REFRESH_OUTCOME_COMPLETED = 1,
  BACKEND_FULL_REFRESH_OUTCOME_FAILED = 2,
  BACKEND_FULL_REFRESH_OUTCOME_TIMED_OUT = 3,
} BackendFullRefreshOutcome;

typedef struct BackendFullRefreshState {
  BOOL requested;
  BOOL in_flight;
  UINT64 latest_generation;
  UINT64 requested_generation;
  UINT64 in_flight_generation;
  UINT64 completed_generation;
  UINT64 requested_timestamp_ms;
  UINT64 in_flight_timestamp_ms;
  UINT64 completed_timestamp_ms;
  BackendFullRefreshOutcome completed_outcome;
} BackendFullRefreshState;

/**
 * @brief Initialize backend client
 * @return BackendClient instance or NULL on failure
 */
BackendClient *backend_init(void);

/**
 * @brief Configure backend connection parameters
 * @param client Backend client instance
 * @param hostname Windows Server hostname/IP
 * @param port RDP port (usually 3389)
 * @param username Login username
 * @param password Login password
 * @param domain Domain (NULL for workgroup)
 * @return TRUE on success
 */
BOOL backend_configure(BackendClient *client, const char *hostname, UINT16 port,
                       const char *username, const char *password,
                       const char *domain);

/**
 * @brief Connect to Windows Server
 * @param client Backend client instance
 * @return TRUE on successful connection
 */
BOOL backend_connect(BackendClient *client);

/**
 * @brief Disconnect from Windows Server
 * @param client Backend client instance
 */
void backend_disconnect(BackendClient *client);

/**
 * @brief Free backend client resources
 * @param client Backend client instance
 */
void backend_free(BackendClient *client);

/**
 * @brief Check if backend is connected
 * @param client Backend client instance
 * @return TRUE if connected
 */
BOOL backend_is_connected(BackendClient *client);

/**
 * @brief Run the backend client event loop (blocking)
 * @param client Backend client instance
 * @return 0 on clean exit, error code otherwise
 */
int backend_run(BackendClient *client);

/**
 * @brief Process a single iteration of the event loop (non-blocking)
 * @param client Backend client instance
 * @return TRUE if still connected, FALSE if disconnected
 */
BOOL backend_iterate(BackendClient *client);

BOOL backend_request_full_refresh(BackendClient *client);

BOOL backend_full_refresh_in_flight(BackendClient *client);

void backend_mark_full_refresh_complete(BackendClient *client);

void backend_get_full_refresh_state(BackendClient *client,
                                    BackendFullRefreshState *state);

BOOL backend_abandon_full_refresh_if_timed_out(BackendClient *client,
                                               UINT64 generation,
                                               UINT64 timeout_ms);

void backend_get_desktop_layout(BackendClient *client, UINT32 *width,
                                UINT32 *height, UINT32 *generation);

void backend_get_pointer_snapshot(BackendClient *client, UINT16 *x, UINT16 *y,
                                  BOOL *visible, UINT32 *type,
                                  PointerShapeEntry **active_shape,
                                  UINT64 *position_gen, UINT64 *shape_gen);

void backend_store_pointer_position(BackendClient *client, UINT16 x, UINT16 y);

void backend_get_pointer_state(BackendClient *client, UINT16 *x, UINT16 *y,
                               BOOL *visible, UINT32 *type, UINT64 *generation);

void backend_set_monitor_count(BackendClient *client, UINT32 monitor_count);

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_H */
