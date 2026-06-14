#ifndef VIEWER_SERVER_H
#define VIEWER_SERVER_H

#include <freerdp/update.h>
#include <winpr/wtypes.h>

#include "viewer_publisher_config.h"

#ifdef __cplusplus
extern "C" {
#endif

struct BackendClient;
typedef struct BackendClient BackendClient;
typedef struct ViewerServer ViewerServer;

typedef enum {
  VIEWER_AUTH_MODE_NONE = 0,
  VIEWER_AUTH_MODE_BACKEND_CREDENTIALS
} ViewerAuthMode;

typedef enum {
  VIEWER_GFX_CODEC_UNCOMPRESSED = 0,
  VIEWER_GFX_CODEC_RFX,
  VIEWER_GFX_CODEC_CLEARCODEC
} ViewerGfxCodec;

typedef struct {
  BOOL nla_enabled;
  BOOL tls_enabled;
  BOOL rdp_enabled;
  ViewerAuthMode auth_mode;
} ViewerSecurityConfig;

ViewerServer *viewer_server_init(const char *bind_address, UINT16 port,
                                 BackendClient *backend, const char *cert_path,
                                 const char *key_path);

ViewerServer *viewer_server_init_ex(const char *bind_address, UINT16 port,
                                    BackendClient *backend,
                                    const char *cert_path, const char *key_path,
                                    const ViewerSecurityConfig *security);

BOOL viewer_server_start(ViewerServer *server);

void viewer_server_set_slow_disconnect(ViewerServer *server, BOOL enabled,
                                       UINT32 disconnect_after_ms);

void viewer_server_set_classic_policy(
    ViewerServer *server,
    const ViewerPublisherClassicPolicyConfig *classic_policy);

void viewer_server_set_gfx_enabled(ViewerServer *server, BOOL enabled);

void viewer_server_set_gfx_codec(ViewerServer *server, ViewerGfxCodec codec);

void viewer_server_set_gfx_rfx_threading(ViewerServer *server, BOOL enabled);

void viewer_server_set_gfx_dirty_limits(ViewerServer *server,
                                        UINT32 max_in_flight_frames,
                                        UINT64 max_in_flight_bytes);

void viewer_server_set_gfx_diagnostic_full_frame_dirty(ViewerServer *server,
                                                       BOOL enabled);

void viewer_server_stop(ViewerServer *server);

void viewer_server_free(ViewerServer *server);

UINT32 viewer_server_get_count(ViewerServer *server);

void viewer_server_notify_backend_layout_change(BackendClient *backend,
                                                UINT32 width, UINT32 height,
                                                UINT32 generation);

BOOL viewer_server_publish_surface_bits(BackendClient *backend,
                                        const SURFACE_BITS_COMMAND *cmd);

BOOL viewer_server_publish_bitmap_update(BackendClient *backend,
                                         const BITMAP_UPDATE *bitmap);

BOOL viewer_server_update_framebuffer_from_gdi(BackendClient *backend,
                                               const BYTE *pixels, UINT32 width,
                                               UINT32 height, UINT32 stride,
                                               UINT32 pixel_format,
                                               const RECTANGLE_16 *dirty_rects,
                                               UINT32 dirty_rect_count);

BOOL viewer_server_publish_frame_marker(BackendClient *backend,
                                        const SURFACE_FRAME_MARKER *marker);

#ifdef __cplusplus
}
#endif

#endif
