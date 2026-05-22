#ifndef VIEWER_SERVER_H
#define VIEWER_SERVER_H

#include <freerdp/channels/wtsvc.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/update.h>
#include <time.h>
#include <winpr/wtsapi.h>
#include <winpr/wtypes.h>

#include "viewer_framebuffer.h"
#include "viewer_publisher.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_VIEWERS 10
#define VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY 8U
#define VIEWER_GFX_DIRTY_ACK_TIMEOUT_MS 2000U
#define VIEWER_CLASSIC_QUEUE_CAPACITY 32U
#define VIEWER_SURFACE_BITS_QUEUE_CAPACITY 1024U

#define OMNIRDP_MAX_MONITORS 16

typedef struct {
  UINT32 monitor_count;
  UINT32 total_width;
  UINT32 total_height;
  MONITOR_DEF monitors[OMNIRDP_MAX_MONITORS];
} MonitorLayout;

struct BackendClient;
typedef struct BackendClient BackendClient;
typedef struct ViewerServer ViewerServer;

typedef enum {
  VIEWER_JOIN_STATE_NONE = 0,
  VIEWER_JOIN_STATE_PENDING,
  VIEWER_JOIN_STATE_LIVE,
  VIEWER_JOIN_STATE_REJECTED
} ViewerJoinState;

typedef enum {
  VIEWER_JOIN_STRATEGY_NONE = 0,
  VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK,
  VIEWER_JOIN_STRATEGY_REJECT
} ViewerJoinStrategy;

typedef enum {
  VIEWER_GFX_NEGOTIATION_PENDING = 0,
  VIEWER_GFX_NEGOTIATION_RDPEGFX_READY,
  VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK
} ViewerGfxNegotiationOutcome;

typedef enum {
  VIEWER_AUTH_MODE_NONE = 0,
  VIEWER_AUTH_MODE_BACKEND_CREDENTIALS
} ViewerAuthMode;

typedef struct {
  BOOL nla_enabled;
  BOOL tls_enabled;
  BOOL rdp_enabled;
  ViewerAuthMode auth_mode;
} ViewerSecurityConfig;

typedef struct {
  BOOL initialized;
  BOOL canonical_caps_valid;
  RDPGFX_CAPSET canonical_caps;
  CRITICAL_SECTION lock;
} ViewerGfxPublisherState;

typedef struct {
  UINT32 negotiated_width;
  UINT32 negotiated_height;
  UINT64 last_presented_timestamp;
  HANDLE vcm;
  RdpgfxServerContext *rdpgfx;
  RDPGFX_CAPSET confirmed_caps;
  BYTE *rdpgfx_buffer;
  UINT32 rdpgfx_buffer_size;
  UINT32 max_inflight_frames;
  UINT32 next_frame_id;
  UINT32 last_sent_frame_id;
  UINT32 last_ack_frame_id;
  UINT16 active_surface_id;
  UINT32 surface_width;
  UINT32 surface_height;
  UINT32 rdpgfx_error_count;
  UINT32 rdpgfx_consecutive_errors;
  UINT32 rdpgfx_surface_recreate_count;
  UINT32 rdpgfx_context_reinit_count;
  UINT64 rdpgfx_retry_after_ts;
  BOOL post_connect_complete;
  BOOL ready;
  BOOL force_full_present;
  BOOL channel_opened;
  BOOL vcm_progress_logged;
  BOOL drdynvc_joined;
  BOOL caps_ready;
  BOOL surface_created;
  BOOL rdpgfx_temporarily_disabled;
  ViewerGfxNegotiationOutcome negotiation_outcome;
  BOOL use_rdpgfx;
  BOOL initialized;
  BYTE drdynvc_state;
  ViewerJoinState join_state;
  ViewerJoinStrategy join_strategy;
  UINT64 join_start_ts;
  UINT64 last_activated_ts;
  UINT64 dirty_last_sent_generation;
  UINT64 dirty_last_acked_generation;
  UINT64 dirty_reset_generation;
  UINT32 dirty_in_flight_frames;
  UINT32 dirty_max_in_flight_frames;
  BOOL dirty_suspended_for_no_ack;
  BOOL dirty_updates_enabled;
  BOOL dirty_baseline_required;
  UINT32 dirty_frame_ids[VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY];
  UINT64 dirty_frame_generations[VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY];
  UINT64 dirty_frame_sent_ts[VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY];
  BOOL dirty_frame_valid[VIEWER_GFX_DIRTY_FRAME_MAP_CAPACITY];
  ViewerServer *pipeline_server;
  UINT32 pending_caps_actions;
  UINT pending_caps_channel_rc;
  const char *pending_caps_classic_fallback_reason;
  const char *pending_caps_begin_join_reason;
  CRITICAL_SECTION lock;
} ViewerGraphicsContext;

typedef struct ViewerClassicEvent {
  BITMAP_UPDATE *bitmap; /* deep-copied, owned by this event */
  UINT64 generation;     /* nonzero for framebuffer-backed latest baselines */
} ViewerClassicEvent;

typedef struct ViewerSurfaceBitsEvent {
  SURFACE_BITS_COMMAND cmd; /* deep-copied, owned by this event */
} ViewerSurfaceBitsEvent;

typedef struct {
  freerdp_peer *peer;
  rdpContext *context;
  HANDLE thread;
  BOOL connected;
  BOOL activated;
  BOOL counted_in_viewer_count;
  BOOL cleanup_in_progress;
  UINT32 publish_ref_count;
  BOOL needs_full_refresh;
  BOOL stop_requested;
  UINT32 id;
  time_t connect_time;
  CRITICAL_SECTION send_lock;
  UINT64 full_refresh_deadline_ts;
  UINT64 packets_sent;
  UINT64 packets_failed;
  UINT64 write_block_events;
  UINT64 bitmap_updates_sent;
  UINT64 bitmap_updates_failed;
  UINT64 bitmap_rectangles_sent;
  UINT64 bitmap_payload_bytes_sent;
  UINT64 bitmap_write_block_events;
  UINT64 bitmap_send_time_total_us;
  UINT64 bitmap_send_time_max_us;
  UINT64 bitmap_updates_skipped_writeblock;
  UINT64 bitmap_updates_skipped_throttle;
  UINT64 bitmap_updates_queued;
  UINT64 bitmap_queue_dropped;
  UINT64 classic_last_generation_sent;
  UINT32 consecutive_lag_intervals;
  UINT64 sustained_lag_start_ts;
  UINT64 last_pointer_position_generation;
  UINT64 last_pointer_shape_generation;
  ViewerClassicEvent *classic_queue[VIEWER_CLASSIC_QUEUE_CAPACITY];
  UINT32 classic_queue_head;
  UINT32 classic_queue_tail;
  UINT32 classic_queue_count;
  HANDLE classic_event;
  /* SurfaceBits queue (NSCodec/RemoteFX classic path) */
  ViewerSurfaceBitsEvent
      *surface_bits_queue[VIEWER_SURFACE_BITS_QUEUE_CAPACITY];
  UINT32 surface_bits_queue_head;
  UINT32 surface_bits_queue_tail;
  UINT32 surface_bits_queue_count;
  UINT64 surface_bits_updates_sent;
  UINT64 surface_bits_updates_failed;
  UINT64 surface_bits_updates_skipped_writeblock;
  UINT64 surface_bits_updates_skipped_throttle;
  UINT64 surface_bits_updates_queued;
  UINT64 surface_bits_queue_dropped;
  UINT64 surface_bits_send_time_total_us;
  UINT64 surface_bits_send_time_max_us;
  UINT64 surface_bits_payload_bytes_sent;
  ViewerGraphicsContext gfx;
} Viewer;

struct ViewerServer {
  freerdp_listener *listener;
  Viewer viewers[MAX_VIEWERS];
  UINT32 viewer_count;
  BOOL input_owner_active;
  UINT32 input_owner_viewer_id;
  UINT64 input_owner_last_input_ts;
  CRITICAL_SECTION lock;
  BOOL running;
  BOOL slow_viewer_disconnect_enabled;
  UINT32 slow_viewer_disconnect_ms;
  char *bind_address;
  UINT16 port;
  MonitorLayout monitor_layout; /* shared monitor layout */
  BackendClient *backend;
  ViewerGfxPublisherState gfx;
  ViewerFramebuffer framebuffer;
  ViewerPublisher publisher;
  char *cert_path; /* TLS certificate path (config or NULL for default) */
  char *key_path;  /* TLS key path (config or NULL for default) */
  ViewerSecurityConfig security;
  BOOL viewer_gfx_enabled;
};

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

void monitor_layout_init(MonitorLayout *layout, UINT32 monitor_count);

#ifdef __cplusplus
}
#endif

#endif
