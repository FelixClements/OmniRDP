#ifndef VIEWER_SERVER_H
#define VIEWER_SERVER_H

#include <winpr/wtypes.h>
#include <time.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/update.h>
#include <winpr/wtsapi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_VIEWERS 10
#define VIEWER_GFX_QUEUE_CAPACITY 256U
#define VIEWER_GFX_MAX_PENDING_FRAMES 4U
#define VIEWER_GFX_MAX_ACTIVE_SURFACES 256U
#define VIEWER_GFX_FRAME_RING_CAPACITY 4U
#define VIEWER_GFX_MAX_FRAME_EVENTS 512U
#define VIEWER_CLASSIC_QUEUE_CAPACITY 32U

struct BackendClient;
typedef struct BackendClient BackendClient;

typedef enum
{
    VIEWER_GFX_EVENT_RESET_GRAPHICS = 0,
    VIEWER_GFX_EVENT_CREATE_SURFACE,
    VIEWER_GFX_EVENT_DELETE_SURFACE,
    VIEWER_GFX_EVENT_MAP_SURFACE_TO_OUTPUT,
    VIEWER_GFX_EVENT_START_FRAME,
    VIEWER_GFX_EVENT_SURFACE_COMMAND,
    VIEWER_GFX_EVENT_END_FRAME,
    VIEWER_GFX_EVENT_DELETE_ENCODING_CONTEXT
} ViewerGfxEventType;

typedef enum
{
    VIEWER_JOIN_STATE_NONE = 0,
    VIEWER_JOIN_STATE_PENDING,
    VIEWER_JOIN_STATE_WAIT_NEXT_SAFE_FRAME,
    VIEWER_JOIN_STATE_REPLAYING,
    VIEWER_JOIN_STATE_WAIT_REPLAY_ACK,
    VIEWER_JOIN_STATE_WAIT_BACKEND_REFRESH,
    VIEWER_JOIN_STATE_LIVE,
    VIEWER_JOIN_STATE_REJECTED
} ViewerJoinState;

typedef enum
{
    VIEWER_JOIN_STRATEGY_NONE = 0,
    VIEWER_JOIN_STRATEGY_REPLAY_SAFE_FRAME,
    VIEWER_JOIN_STRATEGY_WAIT_NEXT_SAFE_FRAME,
    VIEWER_JOIN_STRATEGY_BACKEND_REFRESH,
    VIEWER_JOIN_STRATEGY_CLASSIC_FALLBACK,
    VIEWER_JOIN_STRATEGY_REJECT
} ViewerJoinStrategy;

typedef enum
{
    VIEWER_GFX_NEGOTIATION_PENDING = 0,
    VIEWER_GFX_NEGOTIATION_RDPEGFX_READY,
    VIEWER_GFX_NEGOTIATION_CLASSIC_FALLBACK
} ViewerGfxNegotiationOutcome;

typedef struct ViewerGfxEvent
{
    volatile LONG refcount;
    ViewerGfxEventType type;
    union
    {
        RDPGFX_RESET_GRAPHICS_PDU reset_graphics;
        RDPGFX_CREATE_SURFACE_PDU create_surface;
        RDPGFX_DELETE_SURFACE_PDU delete_surface;
        RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map_surface_to_output;
        RDPGFX_START_FRAME_PDU start_frame;
        RDPGFX_SURFACE_COMMAND surface_command;
        RDPGFX_END_FRAME_PDU end_frame;
        RDPGFX_DELETE_ENCODING_CONTEXT_PDU delete_encoding_context;
    } u;
} ViewerGfxEvent;

typedef struct
{
    volatile LONG refcount;
    UINT64 capture_started_ts;
    UINT64 capture_completed_ts;
    UINT32 frame_id;
    UINT32 event_count;
    UINT32 surface_command_count;
    UINT64 total_payload_bytes;
    UINT64 codec_mask;
    BOOL complete;
    BOOL replay_safe;
    ViewerGfxEvent** events;
} ViewerGfxCompleteFrame;

typedef struct
{
    BOOL initialized;
    ViewerGfxCompleteFrame* slots[VIEWER_GFX_FRAME_RING_CAPACITY];
    UINT32 next_slot;
    UINT32 filled_slots;
    UINT32 oldest_frame_id;
    UINT32 newest_frame_id;
    ViewerGfxCompleteFrame* capture_frame;
    CRITICAL_SECTION lock;
} ViewerGfxFrameBuffer;

typedef struct
{
    BOOL in_use;
    BOOL mapped;
    RDPGFX_CREATE_SURFACE_PDU create_surface;
    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map_surface_to_output;
} ViewerGraphicsSurfaceState;

typedef struct
{
    BOOL initialized;
    BOOL has_latest_reset_graphics;
    BOOL canonical_caps_valid;
    BOOL in_frame;
    UINT32 current_frame_id;
    RDPGFX_CAPSET canonical_caps;
    RDPGFX_RESET_GRAPHICS_PDU latest_reset_graphics;
    ViewerGraphicsSurfaceState surfaces[VIEWER_GFX_MAX_ACTIVE_SURFACES];
    ViewerGfxFrameBuffer frame_buffer;
    CRITICAL_SECTION lock;
} ViewerGfxPublisherState;

typedef struct {
    UINT32 negotiated_width;
    UINT32 negotiated_height;
    UINT64 last_presented_timestamp;
    HANDLE vcm;
    RdpgfxServerContext* rdpgfx;
    RDPGFX_CAPSET confirmed_caps;
    BYTE* rdpgfx_buffer;
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
    UINT32 join_target_frame_id;
    UINT64 join_start_ts;
    UINT64 join_refresh_generation;
    UINT32 last_delivered_frame_id;
    UINT32 last_delivered_event_type;
    UINT64 last_delivered_ts;
    UINT64 last_activated_ts;
    ViewerGfxEvent* queue[VIEWER_GFX_QUEUE_CAPACITY];
    UINT32 queue_head;
    UINT32 queue_tail;
    UINT32 queue_count;
    UINT32 pending_frame_count;
    CRITICAL_SECTION lock;
} ViewerGraphicsContext;

typedef struct ViewerClassicEvent
{
    BITMAP_UPDATE* bitmap; /* deep-copied, owned by this event */
} ViewerClassicEvent;

typedef struct {
    freerdp_peer* peer;
    rdpContext* context;
    HANDLE thread;
    BOOL connected;
    BOOL activated;
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
    UINT32 consecutive_lag_intervals;
    UINT64 sustained_lag_start_ts;
    UINT64 last_pointer_position_generation;
    UINT64 last_pointer_shape_generation;
    ViewerClassicEvent* classic_queue[VIEWER_CLASSIC_QUEUE_CAPACITY];
    UINT32 classic_queue_head;
    UINT32 classic_queue_tail;
    UINT32 classic_queue_count;
    HANDLE classic_event;
    ViewerGraphicsContext gfx;
} Viewer;

typedef struct {
    freerdp_listener* listener;
    Viewer viewers[MAX_VIEWERS];
    UINT32 viewer_count;
    BOOL input_owner_active;
    UINT32 input_owner_viewer_id;
    UINT64 input_owner_last_input_ts;
    CRITICAL_SECTION lock;
    BOOL running;
    BOOL slow_viewer_disconnect_enabled;
    UINT32 slow_viewer_disconnect_ms;
    char* bind_address;
    UINT16 port;
    BackendClient* backend;
    ViewerGfxPublisherState gfx;
} ViewerServer;

ViewerServer* viewer_server_init(const char* bind_address, UINT16 port, BackendClient* backend);

BOOL viewer_server_start(ViewerServer* server);

void viewer_server_stop(ViewerServer* server);

void viewer_server_free(ViewerServer* server);

UINT32 viewer_server_get_count(ViewerServer* server);

void viewer_server_notify_backend_layout_change(BackendClient* backend, UINT32 width, UINT32 height,
                                                UINT32 generation);

BOOL viewer_server_publish_surface_bits(BackendClient* backend, const SURFACE_BITS_COMMAND* cmd);

BOOL viewer_server_publish_bitmap_update(BackendClient* backend, const BITMAP_UPDATE* bitmap);

BOOL viewer_server_publish_frame_marker(BackendClient* backend,
                                        const SURFACE_FRAME_MARKER* marker);

BOOL viewer_server_publish_gfx_reset_graphics(BackendClient* backend,
                                              const RDPGFX_RESET_GRAPHICS_PDU* reset_graphics);

BOOL viewer_server_publish_gfx_create_surface(BackendClient* backend,
                                              const RDPGFX_CREATE_SURFACE_PDU* create_surface);

BOOL viewer_server_publish_gfx_delete_surface(BackendClient* backend,
                                              const RDPGFX_DELETE_SURFACE_PDU* delete_surface);

BOOL viewer_server_publish_gfx_map_surface_to_output(
    BackendClient* backend, const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* map_surface_to_output);

BOOL viewer_server_publish_gfx_start_frame(BackendClient* backend,
                                           const RDPGFX_START_FRAME_PDU* start_frame);

BOOL viewer_server_publish_gfx_surface_command(BackendClient* backend,
                                               const RDPGFX_SURFACE_COMMAND* cmd);

BOOL viewer_server_publish_gfx_end_frame(BackendClient* backend,
                                         const RDPGFX_END_FRAME_PDU* end_frame);

BOOL viewer_server_publish_gfx_delete_encoding_context(
    BackendClient* backend,
    const RDPGFX_DELETE_ENCODING_CONTEXT_PDU* delete_encoding_context);

#ifdef __cplusplus
}
#endif

#endif
