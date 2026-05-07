#include "backend.h"
#include "platform_compat.h"
#include "viewer_internal.h"
#include "viewer_server.h"
#include <freerdp/addin.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/rdpgfx.h>
#include <freerdp/constants.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/pointer.h>
#include <freerdp/update.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <winpr/sysinfo.h>
#include <winpr/wlog.h>
#include <winpr/wtypes.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#define TAG "multiplexer.backend"

static BackendClient* g_backend_client = NULL;

static UINT backend_rdpgfx_on_open(RdpgfxClientContext* context, BOOL* do_caps_advertise,
                                   BOOL* do_frame_acks);
static UINT backend_rdpgfx_on_close(RdpgfxClientContext* context);
static UINT backend_rdpgfx_caps_confirm(RdpgfxClientContext* context,
                                        const RDPGFX_CAPS_CONFIRM_PDU* caps_confirm);
static UINT backend_rdpgfx_reset_graphics(RdpgfxClientContext* context,
                                          const RDPGFX_RESET_GRAPHICS_PDU* reset_graphics);
static UINT backend_rdpgfx_create_surface(RdpgfxClientContext* context,
                                          const RDPGFX_CREATE_SURFACE_PDU* create_surface);
static UINT backend_rdpgfx_delete_surface(RdpgfxClientContext* context,
                                          const RDPGFX_DELETE_SURFACE_PDU* delete_surface);
static UINT
backend_rdpgfx_map_surface_to_output(RdpgfxClientContext* context,
                                     const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* map_surface_to_output);
static UINT backend_rdpgfx_start_frame(RdpgfxClientContext* context,
                                       const RDPGFX_START_FRAME_PDU* start_frame);
static UINT backend_rdpgfx_surface_command(RdpgfxClientContext* context,
                                           const RDPGFX_SURFACE_COMMAND* cmd);
static UINT backend_rdpgfx_end_frame(RdpgfxClientContext* context,
                                     const RDPGFX_END_FRAME_PDU* end_frame);
static UINT backend_rdpgfx_delete_encoding_context(
    RdpgfxClientContext* context,
    const RDPGFX_DELETE_ENCODING_CONTEXT_PDU* delete_encoding_context);
static void backend_on_channel_connected(void* context, const ChannelConnectedEventArgs* e);
static void backend_on_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e);
static void backend_reset_full_refresh_state_locked(BackendClient* client);
static void backend_complete_full_refresh_locked(BackendClient* client, UINT64 generation,
                                                 BackendFullRefreshOutcome outcome,
                                                 UINT64 completed_ts);
static const char* backend_surface_bits_codec_name(UINT16 codec_id);
static BOOL backend_should_log_path_event(UINT64 count);
static UINT64 backend_perf_now_us(void);
static BOOL backend_should_log_bitmap_perf(UINT64 batch_count, UINT64 callback_us,
                                           UINT64 publish_us);
static BOOL backend_forward_bitmap_update(BackendClient* client, const BITMAP_UPDATE* bitmap);
static BOOL on_begin_paint(rdpContext* context);
static BOOL on_end_paint(rdpContext* context);
static BOOL on_bitmap_update(rdpContext* context, const BITMAP_UPDATE* bitmap);
static BOOL on_primary_order_info(rdpContext* context, const ORDER_INFO* order_info,
                                  const char* order_name);
static BOOL on_secondary_cache_order_info(rdpContext* context, INT16 orderLength, UINT16 extraFlags,
                                          UINT8 orderType, const char* orderName);

static const char*
backend_surface_bits_codec_name(UINT16 codec_id)
{
	switch (codec_id)
	{
		case RDP_CODEC_ID_NONE:
			return "NONE";
		case RDP_CODEC_ID_NSCODEC:
			return "NSCODEC";
		case RDP_CODEC_ID_JPEG:
			return "JPEG";
		case RDP_CODEC_ID_REMOTEFX:
			return "REMOTEFX";
		case RDP_CODEC_ID_IMAGE_REMOTEFX:
			return "IMAGE_REMOTEFX";
		default:
			return "UNKNOWN";
	}
}

static BOOL
backend_should_log_path_event(UINT64 count)
{
	return (count <= 5) || ((count % 100) == 0);
}

static UINT64
backend_perf_now_us(void)
{
#ifdef _WIN32
	static LARGE_INTEGER frequency = { 0 };
	LARGE_INTEGER counter = { 0 };

	if (frequency.QuadPart == 0)
	{
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
backend_should_log_bitmap_perf(UINT64 batch_count, UINT64 callback_us, UINT64 publish_us)
{
	return (batch_count <= 5) || ((batch_count % 100) == 0) || (callback_us >= 5000ULL) ||
	       (publish_us >= 5000ULL);
}

#ifdef _WIN32
static BOOL
backend_configure_addin_dll_directory(void)
{
	HMODULE module = NULL;
	CHAR path[MAX_PATH] = { 0 };
	CHAR* separator = NULL;
	DWORD length = 0;

	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCSTR)(const void*)freerdp_new, &module) ||
	    !module)
	{
		WLog_WARN(TAG, "Unable to resolve FreeRDP runtime module for addin loading");
		return FALSE;
	}

	length = GetModuleFileNameA(module, path, ARRAYSIZE(path));
	if ((length == 0) || (length >= ARRAYSIZE(path)))
	{
		WLog_WARN(TAG, "Unable to resolve FreeRDP runtime directory for addin loading");
		return FALSE;
	}

	separator = strrchr(path, '\\');
	if (!separator)
		separator = strrchr(path, '/');
	if (!separator)
	{
		WLog_WARN(TAG, "Unable to parse FreeRDP runtime directory for addin loading");
		return FALSE;
	}

	*separator = '\0';
	if (!SetDllDirectoryA(path))
	{
		WLog_WARN(TAG, "Failed to add FreeRDP runtime directory to DLL search path: %s", path);
		return FALSE;
	}

	WLog_INFO(TAG, "Using FreeRDP addin DLL directory %s", path);
	return TRUE;
}
#endif

static BOOL
backend_register_static_channel_provider(void)
{
	FREERDP_LOAD_CHANNEL_ADDIN_ENTRY_FN provider = freerdp_get_current_addin_provider();

	if (provider == freerdp_channels_load_static_addin_entry)
		return TRUE;

	if (provider && (provider != freerdp_channels_load_static_addin_entry))
		WLog_WARN(TAG, "Replacing existing FreeRDP addin provider");

	if (freerdp_register_addin_provider(freerdp_channels_load_static_addin_entry, 0) !=
	    CHANNEL_RC_OK)
	{
		WLog_ERR(TAG, "Failed to register FreeRDP static channel addin provider");
		return FALSE;
	}

	return TRUE;
}

static BOOL
backend_prepare_rdpgfx_channels(BackendClient* client)
{
	rdpSettings* settings = NULL;
	const char* const rdpgfx[] = { RDPGFX_CHANNEL_NAME };

	if (!client || !client->context)
		return FALSE;

	settings = client->context->settings;
	if (!settings)
		return FALSE;

#ifdef _WIN32
	(void)backend_configure_addin_dll_directory();
#endif

	if (!freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_SupportHeartbeatPdu, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, FALSE))
	{
		return FALSE;
	}

	if (freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline) &&
	    !freerdp_client_add_dynamic_channel(settings, sizeof(rdpgfx) / sizeof(rdpgfx[0]), rdpgfx))
	{
		WLog_ERR(TAG, "Failed to register backend RDPEGFX dynamic channel");
		return FALSE;
	}

	return TRUE;
}

static void
backend_attach_rdpgfx_context(BackendClient* client, RdpgfxClientContext* rdpgfx)
{
	if (!client || !rdpgfx)
		return;

	client->rdpgfx = rdpgfx;
	/* Save GDI's original GFX callbacks. GDI handles decoding and
	 * framebuffer updates. Our callbacks forward to viewers. */
	client->gdi_StartFrame = rdpgfx->StartFrame;
	client->gdi_EndFrame = rdpgfx->EndFrame;
	client->gdi_SurfaceCommand = rdpgfx->SurfaceCommand;
	client->gdi_ResetGraphics = rdpgfx->ResetGraphics;
	client->gdi_CreateSurface = rdpgfx->CreateSurface;
	client->gdi_DeleteSurface = rdpgfx->DeleteSurface;
	client->gdi_MapSurfaceToOutput = rdpgfx->MapSurfaceToOutput;
	client->gdi_DeleteEncodingContext = rdpgfx->DeleteEncodingContext;
	client->gdi_OnOpen = rdpgfx->OnOpen;
	client->gdi_OnClose = rdpgfx->OnClose;
	client->gdi_CapsConfirm = rdpgfx->CapsConfirm;

	client->rdpgfx->custom = client;
	client->rdpgfx->OnOpen = backend_rdpgfx_on_open;
	client->rdpgfx->OnClose = backend_rdpgfx_on_close;
	client->rdpgfx->CapsConfirm = backend_rdpgfx_caps_confirm;
	client->rdpgfx->ResetGraphics = backend_rdpgfx_reset_graphics;
	client->rdpgfx->CreateSurface = backend_rdpgfx_create_surface;
	client->rdpgfx->DeleteSurface = backend_rdpgfx_delete_surface;
	client->rdpgfx->MapSurfaceToOutput = backend_rdpgfx_map_surface_to_output;
	client->rdpgfx->StartFrame = backend_rdpgfx_start_frame;
	client->rdpgfx->SurfaceCommand = backend_rdpgfx_surface_command;
	client->rdpgfx->EndFrame = backend_rdpgfx_end_frame;
	client->rdpgfx->DeleteEncodingContext = backend_rdpgfx_delete_encoding_context;
}

static BOOL
backend_service_full_refresh_request(BackendClient* client)
{
	RECTANGLE_16 rect = { 0 };
	UINT32 width = 0;
	UINT32 height = 0;
	UINT64 generation = 0;
	UINT64 request_ts = 0;
	UINT64 now = 0;
	BOOL should_request = FALSE;
	BOOL sent = FALSE;

	if (!client || !client->context || !client->context->update ||
	    !client->context->update->RefreshRect)
		return FALSE;

	EnterCriticalSection(&client->refresh_lock);
	should_request = client->full_refresh_requested && !client->full_refresh_in_flight;
	generation = client->full_refresh_requested_generation;
	request_ts = client->full_refresh_requested_timestamp_ms;
	LeaveCriticalSection(&client->refresh_lock);

	if (!should_request)
		return FALSE;

	now = platform_get_timestamp_ms();
	backend_get_desktop_layout(client, &width, &height, NULL);
	if ((width == 0) || (height == 0) || (width > UINT16_MAX) || (height > UINT16_MAX))
	{
		EnterCriticalSection(&client->refresh_lock);
		if (client->full_refresh_requested && !client->full_refresh_in_flight &&
		    (client->full_refresh_requested_generation == generation))
		{
			backend_complete_full_refresh_locked(client, generation,
			                                     BACKEND_FULL_REFRESH_OUTCOME_FAILED, now);
		}
		LeaveCriticalSection(&client->refresh_lock);
		WLog_WARN(TAG,
		          "Dropping backend full refresh generation=%" PRIu64
		          " request for invalid layout %ux%u",
		          generation, width, height);
		return FALSE;
	}

	rect.left = 0;
	rect.top = 0;
	rect.right = WINPR_ASSERTING_INT_CAST(UINT16, width);
	rect.bottom = WINPR_ASSERTING_INT_CAST(UINT16, height);
	sent = client->context->update->RefreshRect(client->context, 1, &rect);

	EnterCriticalSection(&client->refresh_lock);
	if (client->full_refresh_requested && !client->full_refresh_in_flight &&
	    (client->full_refresh_requested_generation == generation))
	{
		client->full_refresh_requested = FALSE;
		client->full_refresh_requested_generation = 0;
		client->full_refresh_requested_timestamp_ms = 0;

		if (sent)
		{
			client->full_refresh_in_flight = TRUE;
			client->full_refresh_in_flight_generation = generation;
			client->full_refresh_in_flight_timestamp_ms = now;
		}
		else
		{
			backend_complete_full_refresh_locked(client, generation,
			                                     BACKEND_FULL_REFRESH_OUTCOME_FAILED, now);
		}
	}
	LeaveCriticalSection(&client->refresh_lock);

	if (!sent)
	{
		WLog_WARN(TAG, "Backend full refresh generation=%" PRIu64 " failed for %ux%u", generation,
		          width, height);
		return FALSE;
	}

	WLog_INFO(TAG,
	          "Backend full refresh generation=%" PRIu64 " requested %ux%u queued_at=%" PRIu64
	          " started_at=%" PRIu64,
	          generation, width, height, request_ts, now);
	return TRUE;
}

static void
backend_reset_full_refresh_state_locked(BackendClient* client)
{
	if (!client)
		return;

	client->full_refresh_requested = FALSE;
	client->full_refresh_in_flight = FALSE;
	client->full_refresh_generation = 0;
	client->full_refresh_requested_generation = 0;
	client->full_refresh_in_flight_generation = 0;
	client->full_refresh_completed_generation = 0;
	client->full_refresh_requested_timestamp_ms = 0;
	client->full_refresh_in_flight_timestamp_ms = 0;
	client->full_refresh_completed_timestamp_ms = 0;
	client->full_refresh_completed_outcome = BACKEND_FULL_REFRESH_OUTCOME_NONE;
}

static void
backend_complete_full_refresh_locked(BackendClient* client, UINT64 generation,
                                     BackendFullRefreshOutcome outcome, UINT64 completed_ts)
{
	if (!client)
		return;

	client->full_refresh_requested = FALSE;
	client->full_refresh_in_flight = FALSE;
	client->full_refresh_requested_generation = 0;
	client->full_refresh_in_flight_generation = 0;
	client->full_refresh_requested_timestamp_ms = 0;
	client->full_refresh_in_flight_timestamp_ms = 0;
	client->full_refresh_completed_generation = generation;
	client->full_refresh_completed_timestamp_ms = completed_ts;
	client->full_refresh_completed_outcome = outcome;
}

void
backend_store_pointer_position(BackendClient* client, UINT16 x, UINT16 y)
{
	if (!client)
		return;

	EnterCriticalSection(&client->pointer_lock);
	client->pointer_x = x;
	client->pointer_y = y;
	client->pointer_position_generation++;
	LeaveCriticalSection(&client->pointer_lock);
}

static void
backend_store_pointer_system(BackendClient* client, BOOL visible, UINT32 type)
{
	if (!client)
		return;

	EnterCriticalSection(&client->pointer_lock);
	client->pointer_visible = visible;
	client->pointer_type = type;
	client->active_pointer_shape = NULL;
	client->pointer_shape_generation++;
	LeaveCriticalSection(&client->pointer_lock);
}

static BOOL
backend_activate_pointer_shape(BackendClient* client, PointerShapeEntry* entry)
{
	if (!client || !entry)
		return FALSE;

	EnterCriticalSection(&client->pointer_lock);
	client->pointer_visible = TRUE;
	client->pointer_type = SYSPTR_DEFAULT;
	client->active_pointer_shape = entry;
	client->pointer_shape_generation++;
	LeaveCriticalSection(&client->pointer_lock);
	return TRUE;
}

static void
backend_store_desktop_layout(BackendClient* client, UINT32 width, UINT32 height, BOOL* changed_out)
{
	BOOL changed = FALSE;

	if (!client || width == 0 || height == 0)
	{
		if (changed_out)
			*changed_out = FALSE;
		return;
	}

	EnterCriticalSection(&client->layout_lock);
	if ((client->desktop_width != width) || (client->desktop_height != height))
	{
		client->desktop_width = width;
		client->desktop_height = height;
		client->layout_generation++;
		changed = TRUE;
	}
	LeaveCriticalSection(&client->layout_lock);

	if (changed_out)
		*changed_out = changed;
}

static void
backend_refresh_desktop_layout(BackendClient* client, rdpContext* context)
{
	UINT32 width = 0;
	UINT32 height = 0;
	BOOL changed = FALSE;
	rdpSettings* settings = NULL;

	if (!client)
		return;

	if (context && context->gdi && context->gdi->width > 0 && context->gdi->height > 0)
	{
		width = WINPR_ASSERTING_INT_CAST(UINT32, context->gdi->width);
		height = WINPR_ASSERTING_INT_CAST(UINT32, context->gdi->height);
	}

	settings = context ? context->settings : (client->context ? client->context->settings : NULL);
	if ((width == 0 || height == 0) && settings)
	{
		width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
		height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
	}

	backend_store_desktop_layout(client, width, height, &changed);
	if (changed)
	{
		UINT32 generation = 0;
		backend_get_desktop_layout(client, &width, &height, &generation);
		WLog_INFO(TAG, "Desktop layout updated to %ux%u generation=%" PRIu32, width, height,
		          generation);
		viewer_server_notify_backend_layout_change(client, width, height, generation);
	}
}

static BOOL
backend_forward_surface_bits(BackendClient* client, const SURFACE_BITS_COMMAND* cmd)
{
	if (!client || !cmd)
		return FALSE;

	/* SurfaceBits forwarding disabled — codecs are disabled so Windows Server
	 * sends uncompressed BitmapUpdate instead. Drop any SurfaceBits that
	 * might still arrive (e.g. from a cached codec negotiation). */
	if (cmd->bmp.codecID != RDP_CODEC_ID_NONE)
	{
		WLog_DBG(TAG,
		         "Dropping SurfaceBits rect=(%u,%u)-(%u,%u) codec=%s(%" PRIu16
		         ") reason=codecs-disabled",
		         cmd->destLeft, cmd->destTop, cmd->destRight, cmd->destBottom,
		         backend_surface_bits_codec_name(cmd->bmp.codecID), cmd->bmp.codecID);
		return TRUE;
	}

	if (viewer_server_publish_surface_bits(client, cmd))
	{
		client->forwarded_surface_bits_count++;
		client->forwarded_surface_bits_bytes += cmd->bmp.bitmapDataLength;
	}

	return TRUE;
}

static BOOL
backend_forward_bitmap_update(BackendClient* client, const BITMAP_UPDATE* bitmap)
{
	if (!client || !bitmap)
		return FALSE;

	return viewer_server_publish_bitmap_update(client, bitmap);
}

static BOOL
backend_forward_frame_marker(BackendClient* client, const SURFACE_FRAME_MARKER* marker)
{
	if (!client || !marker)
		return FALSE;

	if (viewer_server_publish_frame_marker(client, marker))
		client->forwarded_frame_marker_count++;

	return TRUE;
}

static UINT
backend_rdpgfx_on_open(RdpgfxClientContext* context, BOOL* do_caps_advertise, BOOL* do_frame_acks)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;

	if (!client)
		return ERROR_INVALID_PARAMETER;

	client->rdpgfx_channel_open = TRUE;
	client->rdpgfx_caps_confirmed = FALSE;
	if (do_caps_advertise)
		*do_caps_advertise = TRUE;
	if (do_frame_acks)
		*do_frame_acks = TRUE;

	WLog_INFO(TAG, "Backend RDPEGFX channel opened");
	return CHANNEL_RC_OK;
}

static UINT
backend_rdpgfx_on_close(RdpgfxClientContext* context)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;

	if (!client)
		return ERROR_INVALID_PARAMETER;

	client->rdpgfx_channel_open = FALSE;
	client->rdpgfx_caps_confirmed = FALSE;
	WLog_INFO(TAG, "Backend RDPEGFX channel closed");
	return CHANNEL_RC_OK;
}

static UINT
backend_rdpgfx_caps_confirm(RdpgfxClientContext* context,
                            const RDPGFX_CAPS_CONFIRM_PDU* caps_confirm)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;

	if (!client || !caps_confirm || !caps_confirm->capsSet)
		return ERROR_INVALID_PARAMETER;

	client->rdpgfx_caps_confirmed = TRUE;
	WLog_INFO(TAG, "Backend RDPEGFX caps confirmed version=0x%08" PRIX32 " flags=0x%08" PRIX32,
	          caps_confirm->capsSet->version, caps_confirm->capsSet->flags);
	return CHANNEL_RC_OK;
}

static UINT
backend_rdpgfx_create_surface(RdpgfxClientContext* context,
                              const RDPGFX_CREATE_SURFACE_PDU* create_surface)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;

	if (!client || !create_surface)
		return ERROR_INVALID_PARAMETER;

	if (viewer_server_publish_gfx_create_surface(client, create_surface))
		client->forwarded_gfx_create_surface_count++;

	return CHANNEL_RC_OK;
}

static UINT
backend_rdpgfx_delete_surface(RdpgfxClientContext* context,
                              const RDPGFX_DELETE_SURFACE_PDU* delete_surface)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;

	if (!client || !delete_surface)
		return ERROR_INVALID_PARAMETER;

	if (viewer_server_publish_gfx_delete_surface(client, delete_surface))
		client->forwarded_gfx_delete_surface_count++;

	return CHANNEL_RC_OK;
}

static UINT
backend_rdpgfx_map_surface_to_output(RdpgfxClientContext* context,
                                     const RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* map_surface_to_output)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;

	if (!client || !map_surface_to_output)
		return ERROR_INVALID_PARAMETER;

	if (viewer_server_publish_gfx_map_surface_to_output(client, map_surface_to_output))
		client->forwarded_gfx_map_surface_to_output_count++;

	return CHANNEL_RC_OK;
}

static UINT
backend_rdpgfx_start_frame(RdpgfxClientContext* context, const RDPGFX_START_FRAME_PDU* start_frame)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;

	if (!client || !start_frame)
		return ERROR_INVALID_PARAMETER;

	if (client->gdi_StartFrame)
		((pcRdpgfxStartFrame)client->gdi_StartFrame)(context, start_frame);

	if (viewer_server_publish_gfx_start_frame(client, start_frame))
		client->forwarded_gfx_start_frame_count++;

	return CHANNEL_RC_OK;
}

static UINT
backend_rdpgfx_end_frame(RdpgfxClientContext* context, const RDPGFX_END_FRAME_PDU* end_frame)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;

	if (!client || !end_frame)
		return ERROR_INVALID_PARAMETER;

	if (client->gdi_EndFrame)
		((pcRdpgfxEndFrame)client->gdi_EndFrame)(context, end_frame);

	if (viewer_server_publish_gfx_end_frame(client, end_frame))
		client->forwarded_gfx_end_frame_count++;

	return CHANNEL_RC_OK;
}

static UINT
backend_rdpgfx_surface_command(RdpgfxClientContext* context, const RDPGFX_SURFACE_COMMAND* cmd)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;

	if (!client || !cmd)
		return ERROR_INVALID_PARAMETER;

	if (client->gdi_SurfaceCommand)
		((pcRdpgfxSurfaceCommand)client->gdi_SurfaceCommand)(context, cmd);

	if (viewer_server_publish_gfx_surface_command(client, cmd))
		client->forwarded_gfx_surface_command_count++;

	return CHANNEL_RC_OK;
}

static UINT
backend_rdpgfx_reset_graphics(RdpgfxClientContext* context,
                              const RDPGFX_RESET_GRAPHICS_PDU* reset_graphics)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;
	BOOL changed = FALSE;

	if (!client || !reset_graphics)
		return ERROR_INVALID_PARAMETER;

	if (client->gdi_ResetGraphics)
		((pcRdpgfxResetGraphics)client->gdi_ResetGraphics)(context, reset_graphics);

	backend_store_desktop_layout(client, reset_graphics->width, reset_graphics->height, &changed);
	if (changed)
	{
		UINT32 width = 0;
		UINT32 height = 0;
		UINT32 generation = 0;

		backend_get_desktop_layout(client, &width, &height, &generation);
		viewer_server_notify_backend_layout_change(client, width, height, generation);
	}

	if (viewer_server_publish_gfx_reset_graphics(client, reset_graphics))
		client->forwarded_gfx_reset_graphics_count++;

	return CHANNEL_RC_OK;
}

static UINT
backend_rdpgfx_delete_encoding_context(
    RdpgfxClientContext* context, const RDPGFX_DELETE_ENCODING_CONTEXT_PDU* delete_encoding_context)
{
	BackendClient* client = context ? (BackendClient*)context->custom : NULL;

	if (!client || !delete_encoding_context)
		return ERROR_INVALID_PARAMETER;

	if (viewer_server_publish_gfx_delete_encoding_context(client, delete_encoding_context))
		client->forwarded_gfx_delete_encoding_context_count++;

	return CHANNEL_RC_OK;
}

static void
backend_on_channel_connected(void* context, const ChannelConnectedEventArgs* e)
{
	BackendClient* client = g_backend_client;

	(void)context;

	if (!client || !e || !e->name || !e->pInterface)
		return;

	if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) != 0)
		return;

	backend_attach_rdpgfx_context(client, (RdpgfxClientContext*)e->pInterface);
	WLog_INFO(TAG, "Backend RDPEGFX dynamic channel connected");
}

static void
backend_on_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e)
{
	BackendClient* client = g_backend_client;

	(void)context;

	if (!client || !e || !e->name)
		return;

	if (strcmp(e->name, RDPGFX_DVC_CHANNEL_NAME) != 0)
		return;

	client->rdpgfx = NULL;
	client->rdpgfx_channel_open = FALSE;
	client->rdpgfx_caps_confirmed = FALSE;
	WLog_INFO(TAG, "Backend RDPEGFX dynamic channel disconnected");
}

static BOOL
on_pointer_position(rdpContext* context, const POINTER_POSITION_UPDATE* pointer_position)
{
	BackendClient* client = g_backend_client;

	(void)context;

	if (!client || !pointer_position)
		return FALSE;

	backend_store_pointer_position(client, WINPR_ASSERTING_INT_CAST(UINT16, pointer_position->xPos),
	                               WINPR_ASSERTING_INT_CAST(UINT16, pointer_position->yPos));
	WLog_INFO(TAG, "PointerPosition x=%" PRIu32 " y=%" PRIu32, pointer_position->xPos,
	          pointer_position->yPos);
	return TRUE;
}

static BOOL
on_pointer_system(rdpContext* context, const POINTER_SYSTEM_UPDATE* pointer_system)
{
	BackendClient* client = g_backend_client;
	BOOL visible = TRUE;

	(void)context;

	if (!client || !pointer_system)
		return FALSE;

	visible = (pointer_system->type != SYSPTR_NULL);

	backend_store_pointer_system(client, visible, pointer_system->type);

	WLog_INFO(TAG, "PointerSystem type=0x%08" PRIX32 " visible=%d", pointer_system->type, visible);
	return TRUE;
}

static BOOL
on_pointer_color(rdpContext* context, const POINTER_COLOR_UPDATE* pointer_color)
{
	BackendClient* client = g_backend_client;
	PointerShapeEntry* entry = NULL;

	(void)context;

	if (!client || !pointer_color)
		return FALSE;

	EnterCriticalSection(&client->pointer_lock);
	entry = pointer_shape_cache_add_from_color(client->pointer_shape_cache, pointer_color);
	LeaveCriticalSection(&client->pointer_lock);

	if (!entry)
	{
		WLog_WARN(TAG, "PointerColor cache=%" PRIu16 " size=%" PRIu16 "x%" PRIu16 " rejected",
		          pointer_color->cacheIndex, pointer_color->width, pointer_color->height);
		return FALSE;
	}

	backend_activate_pointer_shape(client, entry);
	WLog_INFO(TAG, "PointerColor cache=%" PRIu16 " size=%" PRIu16 "x%" PRIu16,
	          pointer_color->cacheIndex, pointer_color->width, pointer_color->height);
	return TRUE;
}

static BOOL
on_pointer_new(rdpContext* context, const POINTER_NEW_UPDATE* pointer_new)
{
	const POINTER_COLOR_UPDATE* color = pointer_new ? &pointer_new->colorPtrAttr : NULL;
	BackendClient* client = g_backend_client;
	PointerShapeEntry* entry = NULL;

	(void)context;

	if (!client || !pointer_new || !color)
		return FALSE;

	EnterCriticalSection(&client->pointer_lock);
	entry = pointer_shape_cache_add_from_new(client->pointer_shape_cache, pointer_new);
	LeaveCriticalSection(&client->pointer_lock);

	if (!entry)
	{
		WLog_WARN(TAG,
		          "PointerNew cache=%" PRIu16 " size=%" PRIu16 "x%" PRIu16 " xorBpp=%" PRIu32
		          " rejected",
		          color->cacheIndex, color->width, color->height, pointer_new->xorBpp);
		return FALSE;
	}

	backend_activate_pointer_shape(client, entry);
	WLog_INFO(TAG, "PointerNew cache=%" PRIu16 " size=%" PRIu16 "x%" PRIu16 " xorBpp=%" PRIu32,
	          color->cacheIndex, color->width, color->height, pointer_new->xorBpp);
	return TRUE;
}

static BOOL
on_pointer_cached(rdpContext* context, const POINTER_CACHED_UPDATE* pointer_cached)
{
	BackendClient* client = g_backend_client;
	PointerShapeEntry* entry = NULL;

	(void)context;

	if (!client || !pointer_cached || (pointer_cached->cacheIndex > UINT16_MAX))
		return FALSE;

	EnterCriticalSection(&client->pointer_lock);
	entry = pointer_shape_cache_get_by_index(
	    client->pointer_shape_cache, WINPR_ASSERTING_INT_CAST(UINT16, pointer_cached->cacheIndex));
	LeaveCriticalSection(&client->pointer_lock);

	if (!entry)
	{
		WLog_WARN(TAG, "PointerCached cache=%" PRIu32 " missing", pointer_cached->cacheIndex);
		return FALSE;
	}

	backend_activate_pointer_shape(client, entry);
	WLog_INFO(TAG, "PointerCached cache=%" PRIu32, pointer_cached->cacheIndex);
	return TRUE;
}

static BOOL
on_desktop_resize(rdpContext* context)
{
	BackendClient* client = g_backend_client;
	if (!client)
		return FALSE;

	backend_refresh_desktop_layout(client, context);
	return TRUE;
}

static BOOL
on_begin_paint(rdpContext* context)
{
	BackendClient* client = g_backend_client;
	BOOL rc = FALSE;

	if (!client)
		return TRUE;

	if (!client->orig_begin_paint)
		return TRUE;

	rc = client->orig_begin_paint(context);
	if (rc)
	{
		client->begin_paint_count++;
		if (backend_should_log_path_event(client->begin_paint_count))
		{
			WLog_INFO(TAG, "Received BeginPaint count=%" PRIu64, client->begin_paint_count);
		}
	}

	return rc;
}

static BOOL
on_end_paint(rdpContext* context)
{
	BackendClient* client = g_backend_client;
	BOOL rc = FALSE;

	if (!client)
		return TRUE;

	if (!client->orig_end_paint)
		return TRUE;

	rc = client->orig_end_paint(context);
	if (rc)
	{
		client->end_paint_count++;
		if (backend_should_log_path_event(client->end_paint_count))
		{
			WLog_INFO(TAG, "Received EndPaint count=%" PRIu64, client->end_paint_count);
		}
	}

	return rc;
}

static BOOL
on_bitmap_update(rdpContext* context, const BITMAP_UPDATE* bitmap)
{
	BackendClient* client = g_backend_client;
	BOOL rc = FALSE;
	UINT64 callback_started_us = 0;

	if (!client)
		return TRUE;

	if (!client->orig_bitmap_update)
		return TRUE;

	callback_started_us = backend_perf_now_us();
	rc = client->orig_bitmap_update(context, bitmap);
	if (rc && bitmap)
	{
		const BITMAP_DATA* first = (bitmap->number > 0) ? &bitmap->rectangles[0] : NULL;
		UINT64 publish_started_us = 0;
		UINT64 publish_us = 0;
		UINT64 callback_us = 0;
		UINT64 total_bytes = 0;
		UINT32 i = 0;
		BOOL forwarded = FALSE;

		for (i = 0; i < bitmap->number; i++)
			total_bytes += bitmap->rectangles[i].bitmapLength;

		client->bitmap_update_count++;

		publish_started_us = backend_perf_now_us();
		forwarded = backend_forward_bitmap_update(client, bitmap);
		publish_us = backend_perf_now_us() - publish_started_us;
		callback_us = backend_perf_now_us() - callback_started_us;

		client->bitmap_update_batches_total++;
		client->bitmap_update_rectangles_total += bitmap->number;
		client->bitmap_update_payload_bytes_total += total_bytes;
		client->bitmap_update_callback_time_total_us += callback_us;
		client->bitmap_update_publish_time_total_us += publish_us;
		if (callback_us > client->bitmap_update_callback_time_max_us)
			client->bitmap_update_callback_time_max_us = callback_us;
		if (publish_us > client->bitmap_update_publish_time_max_us)
			client->bitmap_update_publish_time_max_us = publish_us;

		if (backend_should_log_path_event(client->bitmap_update_count) ||
		    backend_should_log_bitmap_perf(client->bitmap_update_batches_total, callback_us,
		                                   publish_us))
		{
			WLog_INFO(TAG,
			          "Received BitmapUpdate count=%" PRIu64 " rectangles=%" PRIu32
			          " skipCompression=%d firstRect=(%" PRIu32 ",%" PRIu32 ")-(%" PRIu32
			          ",%" PRIu32 ") size=%" PRIu32 "x%" PRIu32 " bpp=%" PRIu32
			          " compressed=%d payload=%" PRIu32 " batch=%" PRIu64 " batchBytes=%" PRIu64
			          " callbackUs=%" PRIu64 " publishUs=%" PRIu64 " publishOk=%d",
			          client->bitmap_update_count, bitmap->number, bitmap->skipCompression,
			          first ? first->destLeft : 0, first ? first->destTop : 0,
			          first ? first->destRight : 0, first ? first->destBottom : 0,
			          first ? first->width : 0, first ? first->height : 0,
			          first ? first->bitsPerPixel : 0, first ? first->compressed : 0,
			          first ? first->bitmapLength : 0, client->bitmap_update_batches_total,
			          total_bytes, callback_us, publish_us, forwarded);
		}

		/* For the classic (non-RDPEGFX) path, the RDP server does not send
		 * SURFACE_FRAME_MARKER PDUs. Each BitmapUpdate is a frame boundary.
		 * Mark the backend full refresh as complete after the first bitmap
		 * update following a refresh request, so that refreshInFlight
		 * transitions to FALSE and viewers can receive normal updates. */
		if (client->full_refresh_in_flight)
		{
			backend_mark_full_refresh_complete(client);
		}
	}

	return rc;
}

static BOOL
on_primary_order_info(rdpContext* context, const ORDER_INFO* order_info, const char* order_name)
{
	BackendClient* client = g_backend_client;
	BOOL rc = FALSE;

	if (!client)
		return TRUE;

	if (!client->orig_primary_order_info)
		return TRUE;

	rc = client->orig_primary_order_info(context, order_info, order_name);
	if (rc)
	{
		client->primary_order_info_count++;
		if (backend_should_log_path_event(client->primary_order_info_count))
		{
			WLog_INFO(TAG,
			          "Received primary drawing order count=%" PRIu64 " order=%s type=%" PRIu32,
			          client->primary_order_info_count, order_name ? order_name : "unknown",
			          order_info ? order_info->orderType : 0);
		}
	}

	return rc;
}

static BOOL
on_secondary_cache_order_info(rdpContext* context, INT16 orderLength, UINT16 extraFlags,
                              UINT8 orderType, const char* orderName)
{
	BackendClient* client = g_backend_client;
	BOOL rc = FALSE;

	if (!client)
		return TRUE;

	if (!client->orig_secondary_cache_order_info)
		return TRUE;

	rc = client->orig_secondary_cache_order_info(context, orderLength, extraFlags, orderType,
	                                             orderName);
	if (rc)
	{
		client->secondary_cache_order_info_count++;
		if (backend_should_log_path_event(client->secondary_cache_order_info_count))
		{
			WLog_INFO(TAG,
			          "Received secondary/cache order count=%" PRIu64 " order=%s type=%" PRIu8
			          " length=%" PRIi16 " extraFlags=%" PRIu16,
			          client->secondary_cache_order_info_count, orderName ? orderName : "unknown",
			          orderType, orderLength, extraFlags);
		}
	}

	return rc;
}

static BOOL
on_surface_frame_marker(rdpContext* context, const SURFACE_FRAME_MARKER* marker)
{
	(void)context;
	WLog_INFO(TAG, "FrameMarker action=%u id=%u", marker->frameAction, marker->frameId);
	return TRUE;
}

static BOOL
on_surface_bits(rdpContext* context, const SURFACE_BITS_COMMAND* cmd)
{
	BackendClient* client = g_backend_client;
	BOOL rc = FALSE;

	if (!client || !client->orig_surface_bits)
		return FALSE;

	rc = client->orig_surface_bits(context, cmd);
	if (!cmd)
		return rc;

	if (!rc)
	{
		WLog_WARN(TAG,
		          "SurfaceBits decode failed rect=(%u,%u)-(%u,%u) size=%ux%u "
		          "bpp=%u payload=%" PRIu32 " codec=%s(%" PRIu16 ")",
		          cmd->destLeft, cmd->destTop, cmd->destRight, cmd->destBottom, cmd->bmp.width,
		          cmd->bmp.height, cmd->bmp.bpp, cmd->bmp.bitmapDataLength,
		          backend_surface_bits_codec_name(cmd->bmp.codecID), cmd->bmp.codecID);
		return rc;
	}

	WLog_INFO(TAG,
	          "Received SurfaceBits rect=(%u,%u)-(%u,%u) size=%ux%u bpp=%u "
	          "payload=%" PRIu32 " codec=%s(%" PRIu16 ") cmdType=%" PRIu16,
	          cmd->destLeft, cmd->destTop, cmd->destRight, cmd->destBottom, cmd->bmp.width,
	          cmd->bmp.height, cmd->bmp.bpp, cmd->bmp.bitmapDataLength,
	          backend_surface_bits_codec_name(cmd->bmp.codecID), cmd->bmp.codecID, cmd->cmdType);

	backend_refresh_desktop_layout(client, context);
	(void)backend_forward_surface_bits(client, cmd);
	return TRUE;
}

static BOOL
on_surface_frame_marker_wrapped(rdpContext* context, const SURFACE_FRAME_MARKER* marker)
{
	BackendClient* client = g_backend_client;
	BOOL rc = FALSE;

	if (!client || !client->orig_surface_frame_marker)
		return FALSE;

	rc = client->orig_surface_frame_marker(context, marker);
	if (!rc || !marker)
		return rc;

	(void)backend_forward_frame_marker(client, marker);
	return rc;
}

static BOOL
on_post_connect(freerdp* instance)
{
	(void)instance;
	BackendClient* client = g_backend_client;
	if (!client)
		return FALSE;

	WLog_INFO(TAG, "Backend connected successfully");

	if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
	{
		WLog_ERR(TAG, "Failed to initialize GDI");
		return FALSE;
	}

	WLog_INFO(TAG, "GDI initialized");
	if (client->context && client->context->update)
	{
		client->orig_begin_paint = client->context->update->BeginPaint;
		client->context->update->BeginPaint = on_begin_paint;
		client->orig_end_paint = client->context->update->EndPaint;
		client->context->update->EndPaint = on_end_paint;
		client->orig_bitmap_update = client->context->update->BitmapUpdate;
		client->context->update->BitmapUpdate = on_bitmap_update;
		client->orig_surface_bits = client->context->update->SurfaceBits;
		client->context->update->SurfaceBits = on_surface_bits;
		client->orig_surface_frame_marker = client->context->update->SurfaceFrameMarker;
		client->context->update->SurfaceFrameMarker = on_surface_frame_marker_wrapped;
		if (client->context->update->primary)
		{
			client->orig_primary_order_info = client->context->update->primary->OrderInfo;
			client->context->update->primary->OrderInfo = on_primary_order_info;
		}
		if (client->context->update->secondary)
		{
			client->orig_secondary_cache_order_info =
			    client->context->update->secondary->CacheOrderInfo;
			client->context->update->secondary->CacheOrderInfo = on_secondary_cache_order_info;
		}
	}
	backend_refresh_desktop_layout(client, client->context);
	client->connected = TRUE;

	{
		rdpSettings* s = client->context ? client->context->settings : NULL;
		if (s)
		{
			UINT32 monCount = freerdp_settings_get_uint32(s, FreeRDP_MonitorCount);
			UINT32 negoFlags = freerdp_settings_get_uint32(s, FreeRDP_NegotiationFlags);
			UINT32 k = 0;
			WLog_INFO(
			    TAG,
			    "on_post_connect: Negotiated settings: DesktopWidth=%" PRIu32
			    ", DesktopHeight=%" PRIu32 ", UseMultimon=%s, ForceMultimon=%s, SpanMonitors=%s, "
			    "SupportMonitorLayoutPdu=%s, MonitorCount=%" PRIu32
			    ", MonitorDefArraySize=%" PRIu32,
			    freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth),
			    freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight),
			    freerdp_settings_get_bool(s, FreeRDP_UseMultimon) ? "TRUE" : "FALSE",
			    freerdp_settings_get_bool(s, FreeRDP_ForceMultimon) ? "TRUE" : "FALSE",
			    freerdp_settings_get_bool(s, FreeRDP_SpanMonitors) ? "TRUE" : "FALSE",
			    freerdp_settings_get_bool(s, FreeRDP_SupportMonitorLayoutPdu) ? "TRUE" : "FALSE",
			    monCount, freerdp_settings_get_uint32(s, FreeRDP_MonitorDefArraySize));
			WLog_INFO(TAG,
			          "on_post_connect: NegotiationFlags=0x%08" PRIx32
			          " EXTENDED_CLIENT_DATA_SUPPORTED=%s",
			          negoFlags, (negoFlags & 0x01) ? "YES" : "NO");
			for (k = 0; k < monCount; k++)
			{
				const rdpMonitor* m = (const rdpMonitor*)freerdp_settings_get_pointer_array(
				    s, FreeRDP_MonitorDefArray, k);
				if (m)
					WLog_INFO(TAG,
					          "on_post_connect:   monitor[%" PRIu32 "]: x=%" PRId32 ", y=%" PRId32
					          ", width=%" PRId32 ", height=%" PRId32 ", is_primary=%s",
					          k, m->x, m->y, m->width, m->height, m->is_primary ? "TRUE" : "FALSE");
			}
		}
	}

	return TRUE;
}

static void
on_post_disconnect(freerdp* instance)
{
	(void)instance;
	BackendClient* client = g_backend_client;
	if (!client)
		return;

	WLog_INFO(TAG, "Backend disconnected");
	client->connected = FALSE;
}

static BOOL
on_authenticate_ex(freerdp* instance, char** username, char** password, char** domain,
                   rdp_auth_reason reason)
{
	(void)instance;
	(void)reason;
	BackendClient* client = g_backend_client;
	if (!client)
		return FALSE;

	if (client->username)
		*username = strdup(client->username);
	if (client->password)
		*password = strdup(client->password);
	if (client->domain)
		*domain = strdup(client->domain);

	return TRUE;
}

static DWORD
on_verify_certificate_ex(freerdp* instance, const char* host, UINT16 port, const char* common_name,
                         const char* subject, const char* issuer, const char* fingerprint,
                         DWORD flags)
{
	(void)instance;
	(void)host;
	(void)port;
	(void)common_name;
	(void)subject;
	(void)issuer;
	(void)fingerprint;
	(void)flags;

	WLog_WARN(TAG, "Certificate verification bypassed (insecure!)");
	return 1;
}

void
backend_set_monitor_count(BackendClient* client, UINT32 monitor_count)
{
	rdpSettings* settings = NULL;

	if (!client)
		return;

	WLog_INFO(TAG, "backend_set_monitor_count: monitor_count=%" PRIu32, monitor_count);

	monitor_layout_init(&client->monitor_layout, monitor_count);
	client->desktop_width = client->monitor_layout.total_width;
	client->desktop_height = client->monitor_layout.total_height;

	WLog_INFO(TAG,
	          "backend_set_monitor_count: layout: total_width=%" PRIu32 ", total_height=%" PRIu32
	          ", monitor_count=%" PRIu32,
	          client->monitor_layout.total_width, client->monitor_layout.total_height,
	          client->monitor_layout.monitor_count);

	/* Update FreeRDP settings if the context has already been initialized
	 * (i.e., backend_init() has been called). These settings must be
	 * configured before backend_connect() is called. */
	if (client->context && client->context->settings)
	{
		UINT32 i = 0;
		settings = client->context->settings;

		freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
		                            client->monitor_layout.total_width);
		freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
		                            client->monitor_layout.total_height);

		if (client->monitor_layout.monitor_count > 1)
		{
			rdpMonitor monitors[OMNIRDP_MAX_MONITORS];
			UINT32 count = client->monitor_layout.monitor_count;

			memset(monitors, 0, sizeof(monitors));

			freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE);
			/* CRITICAL: SupportMonitorLayoutPdu must be TRUE for the server
			 * to honor CS_MONITOR data. Without this, Windows ignores the
			 * monitor definitions and creates a spanned desktop. */
			freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
			/* CRITICAL: ForceMultimon must be TRUE to ensure CS_MONITOR data
			 * is sent even if the server does not advertise
			 * EXTENDED_CLIENT_DATA_SUPPORTED. Without ForceMultimon, FreeRDP
			 * silently skips CS_MONITOR when the server lacks this flag,
			 * resulting in a spanned desktop. This is safe because
			 * SupportMonitorLayoutPdu=TRUE ensures the
			 * RNS_UD_CS_SUPPORT_MONITOR_LAYOUT_PDU flag is set in the
			 * Client Core Data, so the server will honor the monitor layout. */
			freerdp_settings_set_bool(settings, FreeRDP_ForceMultimon, TRUE);

			WLog_INFO(
			    TAG,
			    "backend_set_monitor_count: Setting UseMultimon=TRUE, "
			    "SupportMonitorLayoutPdu=TRUE, ForceMultimon=TRUE, "
			    "DesktopWidth=%" PRIu32 ", DesktopHeight=%" PRIu32 " for %" PRIu32 " monitors",
			    client->monitor_layout.total_width, client->monitor_layout.total_height, count);

			for (i = 0; i < count; i++)
			{
				monitors[i].x = client->monitor_layout.monitors[i].left;
				monitors[i].y = client->monitor_layout.monitors[i].top;
				monitors[i].width = client->monitor_layout.monitors[i].right -
				                    client->monitor_layout.monitors[i].left;
				monitors[i].height = client->monitor_layout.monitors[i].bottom -
				                     client->monitor_layout.monitors[i].top;
				monitors[i].is_primary =
				    (client->monitor_layout.monitors[i].flags & MONITOR_PRIMARY) ? TRUE : FALSE;

				WLog_INFO(TAG,
				          "backend_set_monitor_count: monitor[%" PRIu32 "]: x=%" PRId32
				          ", y=%" PRId32 ", width=%" PRId32 ", height=%" PRId32 ", is_primary=%s",
				          i, monitors[i].x, monitors[i].y, monitors[i].width, monitors[i].height,
				          monitors[i].is_primary ? "TRUE" : "FALSE");
			}
			freerdp_settings_set_monitor_def_array_sorted(settings, monitors, count);

			{
				UINT32 k = 0;
				UINT32 monCountAfter = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
				WLog_INFO(TAG,
				          "backend_set_monitor_count: After set_monitor_def_array_sorted: "
				          "MonitorCount=%" PRIu32 ", UseMultimon=%s, DesktopWidth=%" PRIu32
				          ", DesktopHeight=%" PRIu32,
				          monCountAfter,
				          freerdp_settings_get_bool(settings, FreeRDP_UseMultimon) ? "TRUE"
				                                                                   : "FALSE",
				          freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
				          freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight));
				for (k = 0; k < monCountAfter; k++)
				{
					const rdpMonitor* m = (const rdpMonitor*)freerdp_settings_get_pointer_array(
					    settings, FreeRDP_MonitorDefArray, k);
					if (m)
						WLog_INFO(
						    TAG,
						    "backend_set_monitor_count:   settings[%" PRIu32 "]: x=%" PRId32
						    ", y=%" PRId32 ", width=%" PRId32 ", height=%" PRId32 ", is_primary=%s",
						    k, m->x, m->y, m->width, m->height, m->is_primary ? "TRUE" : "FALSE");
				}
			}
		}
		else
		{
			freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, FALSE);
			freerdp_settings_set_bool(settings, FreeRDP_ForceMultimon, FALSE);
			WLog_INFO(TAG,
			          "backend_set_monitor_count: Single monitor: UseMultimon=FALSE, "
			          "ForceMultimon=FALSE, DesktopWidth=%" PRIu32 ", DesktopHeight=%" PRIu32,
			          client->monitor_layout.total_width, client->monitor_layout.total_height);
		}
	}
}

BackendClient*
backend_init(void)
{
	BackendClient* client = calloc(1, sizeof(BackendClient));
	if (!client)
		return NULL;

	InitializeCriticalSection(&client->layout_lock);
	InitializeCriticalSection(&client->pointer_lock);
	InitializeCriticalSection(&client->refresh_lock);
	client->pointer_visible = TRUE;
	client->pointer_type = SYSPTR_DEFAULT;
	client->pointer_position_generation = 1;
	client->pointer_shape_generation = 1;
	client->pointer_shape_cache = pointer_shape_cache_new();
	if (!client->pointer_shape_cache)
	{
		DeleteCriticalSection(&client->layout_lock);
		DeleteCriticalSection(&client->pointer_lock);
		DeleteCriticalSection(&client->refresh_lock);
		free(client);
		return NULL;
	}

	freerdp* instance = freerdp_new();
	if (!instance)
	{
		pointer_shape_cache_free(client->pointer_shape_cache);
		DeleteCriticalSection(&client->layout_lock);
		DeleteCriticalSection(&client->pointer_lock);
		DeleteCriticalSection(&client->refresh_lock);
		free(client);
		return NULL;
	}

	if (!freerdp_context_new(instance))
	{
		freerdp_free(instance);
		pointer_shape_cache_free(client->pointer_shape_cache);
		DeleteCriticalSection(&client->layout_lock);
		DeleteCriticalSection(&client->pointer_lock);
		free(client);
		return NULL;
	}

	/* freerdp_connect() recreates the channel manager and reloads addins through
	 * LoadChannels. */
	instance->LoadChannels = freerdp_client_load_channels;

	client->context = instance->context;
	g_backend_client = client;

	if (!backend_register_static_channel_provider())
	{
		freerdp_context_free(instance);
		freerdp_free(instance);
		pointer_shape_cache_free(client->pointer_shape_cache);
		DeleteCriticalSection(&client->layout_lock);
		DeleteCriticalSection(&client->pointer_lock);
		DeleteCriticalSection(&client->refresh_lock);
		free(client);
		g_backend_client = NULL;
		return NULL;
	}

	if (!client->context->pubSub ||
	    PubSub_SubscribeChannelConnected(client->context->pubSub, backend_on_channel_connected) <
	        0 ||
	    PubSub_SubscribeChannelDisconnected(client->context->pubSub,
	                                        backend_on_channel_disconnected) < 0)
	{
		freerdp_context_free(instance);
		freerdp_free(instance);
		pointer_shape_cache_free(client->pointer_shape_cache);
		DeleteCriticalSection(&client->layout_lock);
		DeleteCriticalSection(&client->pointer_lock);
		DeleteCriticalSection(&client->refresh_lock);
		free(client);
		g_backend_client = NULL;
		return NULL;
	}

	if (client->context->update)
	{
		WLog_INFO(TAG, "Registering update callbacks");
		client->context->update->DesktopResize = on_desktop_resize;
		client->context->update->SurfaceFrameMarker = on_surface_frame_marker;
		if (client->context->update->pointer)
		{
			client->context->update->pointer->PointerPosition = on_pointer_position;
			client->context->update->pointer->PointerSystem = on_pointer_system;
			client->context->update->pointer->PointerColor = on_pointer_color;
			client->context->update->pointer->PointerNew = on_pointer_new;
			client->context->update->pointer->PointerCached = on_pointer_cached;
		}
	}
	else
	{
		WLog_ERR(TAG, "No update context available!");
	}

	rdpSettings* settings = client->context->settings;
	if (!settings)
	{
		freerdp_context_free(instance);
		freerdp_free(instance);
		pointer_shape_cache_free(client->pointer_shape_cache);
		DeleteCriticalSection(&client->layout_lock);
		DeleteCriticalSection(&client->pointer_lock);
		DeleteCriticalSection(&client->refresh_lock);
		free(client);
		return NULL;
	}

	/* Disable all codec-based SurfaceBits (NSCodec, RemoteFX).
	 * Windows Server falls back to uncompressed BitmapUpdate which
	 * batches all rectangles into a single PDU — much faster than
	 * 510 individual 64x64 tile PDUs per frame. */
	freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_NSCodec, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxH264, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
	freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE);
	/* Raise frame ack from 2 to 4: more frames in flight before ack
	 * reduces pipeline throttling during rapid updates. */
	freerdp_settings_set_uint32(settings, FreeRDP_FrameAcknowledge, 4);
	freerdp_settings_set_bool(settings, FreeRDP_DeactivateClientDecoding, FALSE);
	freerdp_settings_set_bool(settings, FreeRDP_DeviceRedirection, FALSE);
	/* CRITICAL: SupportMonitorLayoutPdu must be TRUE for the server to honor
	 * CS_MONITOR data. Without this flag, the GCC Client Core Data omits
	 * RNS_UD_CS_SUPPORT_MONITOR_LAYOUT_PDU from earlyCapabilityFlags,
	 * and per MS-RDPBCGR 2.2.1.3.6 the server MUST ignore CS_MONITOR,
	 * resulting in a spanned desktop instead of true multimon. */
	freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
	/* Initialize single-monitor layout as default; backend_set_monitor_count()
	 * must be called before backend_connect() to configure multi-monitor. */
	monitor_layout_init(&client->monitor_layout, 1);
	freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, client->monitor_layout.total_width);
	freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
	                            client->monitor_layout.total_height);
	freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
	client->desktop_width = client->monitor_layout.total_width;
	client->desktop_height = client->monitor_layout.total_height;

	WLog_INFO(TAG,
	          "backend_init: Initial monitor layout: total_width=%" PRIu32 ", total_height=%" PRIu32
	          ", monitor_count=%" PRIu32,
	          client->monitor_layout.total_width, client->monitor_layout.total_height,
	          client->monitor_layout.monitor_count);
	WLog_INFO(TAG,
	          "backend_init: Setting DesktopWidth=%" PRIu32 ", DesktopHeight=%" PRIu32
	          ", ColorDepth=32",
	          client->monitor_layout.total_width, client->monitor_layout.total_height);

	if (client->monitor_layout.monitor_count > 1)
	{
		rdpMonitor monitors[OMNIRDP_MAX_MONITORS];
		UINT32 i = 0;
		UINT32 count = client->monitor_layout.monitor_count;

		memset(monitors, 0, sizeof(monitors));

		freerdp_settings_set_bool(settings, FreeRDP_UseMultimon, TRUE);
		/* SupportMonitorLayoutPdu is also set unconditionally above,
		 * but reinforce it here for clarity in the multimon path. */
		freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
		/* ForceMultimon must be TRUE to ensure CS_MONITOR data is sent
		 * even if the server does not advertise EXTENDED_CLIENT_DATA_SUPPORTED.
		 * This is safe because SupportMonitorLayoutPdu=TRUE ensures the
		 * RNS_UD_CS_SUPPORT_MONITOR_LAYOUT_PDU flag is set in Client Core Data. */
		freerdp_settings_set_bool(settings, FreeRDP_ForceMultimon, TRUE);

		WLog_INFO(TAG,
		          "backend_init: Setting UseMultimon=TRUE, "
		          "SupportMonitorLayoutPdu=TRUE, ForceMultimon=TRUE for %" PRIu32 " monitors",
		          count);

		for (i = 0; i < count; i++)
		{
			monitors[i].x = client->monitor_layout.monitors[i].left;
			monitors[i].y = client->monitor_layout.monitors[i].top;
			monitors[i].width =
			    client->monitor_layout.monitors[i].right - client->monitor_layout.monitors[i].left;
			monitors[i].height =
			    client->monitor_layout.monitors[i].bottom - client->monitor_layout.monitors[i].top;
			monitors[i].is_primary =
			    (client->monitor_layout.monitors[i].flags & MONITOR_PRIMARY) ? TRUE : FALSE;

			WLog_INFO(TAG,
			          "backend_init: monitor[%" PRIu32 "]: x=%" PRId32 ", y=%" PRId32
			          ", width=%" PRId32 ", height=%" PRId32 ", is_primary=%s",
			          i, monitors[i].x, monitors[i].y, monitors[i].width, monitors[i].height,
			          monitors[i].is_primary ? "TRUE" : "FALSE");
		}
		freerdp_settings_set_monitor_def_array_sorted(settings, monitors, count);

		{
			UINT32 k = 0;
			UINT32 monCountAfter = freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
			WLog_INFO(TAG,
			          "backend_init: After set_monitor_def_array_sorted: "
			          "MonitorCount=%" PRIu32 ", UseMultimon=%s, DesktopWidth=%" PRIu32
			          ", DesktopHeight=%" PRIu32,
			          monCountAfter,
			          freerdp_settings_get_bool(settings, FreeRDP_UseMultimon) ? "TRUE" : "FALSE",
			          freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
			          freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight));
			for (k = 0; k < monCountAfter; k++)
			{
				const rdpMonitor* m = (const rdpMonitor*)freerdp_settings_get_pointer_array(
				    settings, FreeRDP_MonitorDefArray, k);
				if (m)
					WLog_INFO(TAG,
					          "backend_init:   settings[%" PRIu32 "]: x=%" PRId32 ", y=%" PRId32
					          ", width=%" PRId32 ", height=%" PRId32 ", is_primary=%s",
					          k, m->x, m->y, m->width, m->height, m->is_primary ? "TRUE" : "FALSE");
			}
		}
	}

	client->port = 3389;

	if (!backend_prepare_rdpgfx_channels(client))
	{
		freerdp_context_free(instance);
		freerdp_free(instance);
		pointer_shape_cache_free(client->pointer_shape_cache);
		DeleteCriticalSection(&client->layout_lock);
		DeleteCriticalSection(&client->pointer_lock);
		DeleteCriticalSection(&client->refresh_lock);
		free(client);
		g_backend_client = NULL;
		return NULL;
	}

	instance->PostConnect = on_post_connect;
	instance->PostDisconnect = on_post_disconnect;
	instance->AuthenticateEx = on_authenticate_ex;
	instance->VerifyCertificateEx = on_verify_certificate_ex;

	return client;
}

BOOL
backend_configure(BackendClient* client, const char* hostname, UINT16 port, const char* username,
                  const char* password, const char* domain)
{
	if (!client || !client->context)
		return FALSE;

	rdpSettings* settings = client->context->settings;
	if (!settings)
		return FALSE;

	if (hostname)
	{
		free(client->hostname);
		client->hostname = strdup(hostname);
		freerdp_settings_set_string(settings, FreeRDP_ServerHostname, hostname);
	}

	if (port > 0)
	{
		client->port = port;
		freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, port);
	}

	if (username)
	{
		free(client->username);
		client->username = strdup(username);
		freerdp_settings_set_string(settings, FreeRDP_Username, username);
	}

	if (password)
	{
		free(client->password);
		client->password = strdup(password);
		freerdp_settings_set_string(settings, FreeRDP_Password, password);
	}

	if (domain)
	{
		free(client->domain);
		client->domain = strdup(domain);
		freerdp_settings_set_string(settings, FreeRDP_Domain, domain);
	}

	return TRUE;
}

BOOL
backend_connect(BackendClient* client)
{
	if (!client || !client->context)
		return FALSE;

	WLog_INFO(TAG, "Connecting to %s:%d", client->hostname ? client->hostname : "(null)",
	          client->port);

	EnterCriticalSection(&client->layout_lock);
	client->forwarded_bitmap_update_count = 0;
	client->forwarded_bitmap_update_rectangles = 0;
	client->forwarded_bitmap_update_bytes = 0;
	client->bitmap_update_callback_time_total_us = 0;
	client->bitmap_update_callback_time_max_us = 0;
	client->bitmap_update_publish_time_total_us = 0;
	client->bitmap_update_publish_time_max_us = 0;
	client->bitmap_update_batches_total = 0;
	client->bitmap_update_rectangles_total = 0;
	client->bitmap_update_payload_bytes_total = 0;
	client->forwarded_surface_bits_count = 0;
	client->forwarded_surface_bits_bytes = 0;
	client->forwarded_frame_marker_count = 0;
	client->forwarded_gfx_reset_graphics_count = 0;
	client->forwarded_gfx_create_surface_count = 0;
	client->forwarded_gfx_delete_surface_count = 0;
	client->forwarded_gfx_map_surface_to_output_count = 0;
	client->forwarded_gfx_start_frame_count = 0;
	client->forwarded_gfx_surface_command_count = 0;
	client->forwarded_gfx_end_frame_count = 0;
	client->forwarded_gfx_delete_encoding_context_count = 0;
	LeaveCriticalSection(&client->layout_lock);

	EnterCriticalSection(&client->refresh_lock);
	backend_reset_full_refresh_state_locked(client);
	LeaveCriticalSection(&client->refresh_lock);

	client->rdpgfx = NULL;
	client->rdpgfx_channel_open = FALSE;
	client->rdpgfx_caps_confirmed = FALSE;

	WLog_INFO(TAG, "Waiting for backend RDPEGFX dynamic channel attach");

	if (!freerdp_connect(client->context->instance))
	{
		WLog_ERR(TAG, "Failed to connect to backend");
		return FALSE;
	}

	return TRUE;
}

void
backend_disconnect(BackendClient* client)
{
	if (!client || !client->context)
		return;

	if (client->connected)
	{
		EnterCriticalSection(&client->refresh_lock);
		backend_reset_full_refresh_state_locked(client);
		LeaveCriticalSection(&client->refresh_lock);
		freerdp_disconnect(client->context->instance);
		client->connected = FALSE;
	}
}

void
backend_free(BackendClient* client)
{
	if (!client)
		return;

	backend_disconnect(client);

	if (client->context && client->context->instance)
	{
		freerdp* instance = client->context->instance;
		freerdp_context_free(instance);
		freerdp_free(instance);
		client->context = NULL;
	}

	free(client->hostname);
	free(client->username);
	free(client->password);
	free(client->domain);
	pointer_shape_cache_free(client->pointer_shape_cache);
	client->pointer_shape_cache = NULL;
	client->active_pointer_shape = NULL;
	DeleteCriticalSection(&client->layout_lock);
	DeleteCriticalSection(&client->pointer_lock);
	DeleteCriticalSection(&client->refresh_lock);
	free(client);
}

BOOL
backend_is_connected(BackendClient* client)
{
	return client && client->connected;
}

int
backend_run(BackendClient* client)
{
	if (!client || !client->context)
		return -1;

	return freerdp_connect(client->context->instance) ? 0 : -1;
}

BOOL
backend_iterate(BackendClient* client)
{
	if (!client || !client->context || !client->connected)
		return FALSE;

	(void)backend_service_full_refresh_request(client);
	return freerdp_check_event_handles(client->context);
}

BOOL
backend_request_full_refresh(BackendClient* client)
{
	BOOL queued = FALSE;
	UINT64 now = 0;

	if (!client)
		return FALSE;

	now = platform_get_timestamp_ms();
	EnterCriticalSection(&client->refresh_lock);
	if (!client->full_refresh_requested && !client->full_refresh_in_flight)
	{
		client->full_refresh_generation++;
		client->full_refresh_requested = TRUE;
		client->full_refresh_requested_generation = client->full_refresh_generation;
		client->full_refresh_requested_timestamp_ms = now;
		queued = TRUE;
	}
	LeaveCriticalSection(&client->refresh_lock);

	return queued;
}

BOOL
backend_full_refresh_in_flight(BackendClient* client)
{
	BOOL in_flight = FALSE;

	if (!client)
		return FALSE;

	EnterCriticalSection(&client->refresh_lock);
	in_flight = client->full_refresh_in_flight;
	LeaveCriticalSection(&client->refresh_lock);

	return in_flight;
}

void
backend_mark_full_refresh_complete(BackendClient* client)
{
	UINT64 generation = 0;

	if (!client)
		return;

	EnterCriticalSection(&client->refresh_lock);
	generation = client->full_refresh_in_flight_generation;
	if (generation != 0)
	{
		backend_complete_full_refresh_locked(client, generation,
		                                     BACKEND_FULL_REFRESH_OUTCOME_COMPLETED,
		                                     platform_get_timestamp_ms());
	}
	else
	{
		client->full_refresh_requested = FALSE;
		client->full_refresh_in_flight = FALSE;
		client->full_refresh_requested_generation = 0;
		client->full_refresh_in_flight_generation = 0;
		client->full_refresh_requested_timestamp_ms = 0;
		client->full_refresh_in_flight_timestamp_ms = 0;
	}
	LeaveCriticalSection(&client->refresh_lock);
}

void
backend_get_full_refresh_state(BackendClient* client, BackendFullRefreshState* state)
{
	if (!state)
		return;

	ZeroMemory(state, sizeof(*state));
	state->completed_outcome = BACKEND_FULL_REFRESH_OUTCOME_NONE;

	if (!client)
		return;

	EnterCriticalSection(&client->refresh_lock);
	state->requested = client->full_refresh_requested;
	state->in_flight = client->full_refresh_in_flight;
	state->latest_generation = client->full_refresh_generation;
	state->requested_generation = client->full_refresh_requested_generation;
	state->in_flight_generation = client->full_refresh_in_flight_generation;
	state->completed_generation = client->full_refresh_completed_generation;
	state->requested_timestamp_ms = client->full_refresh_requested_timestamp_ms;
	state->in_flight_timestamp_ms = client->full_refresh_in_flight_timestamp_ms;
	state->completed_timestamp_ms = client->full_refresh_completed_timestamp_ms;
	state->completed_outcome = (BackendFullRefreshOutcome)client->full_refresh_completed_outcome;
	LeaveCriticalSection(&client->refresh_lock);
}

BOOL
backend_abandon_full_refresh_if_timed_out(BackendClient* client, UINT64 generation,
                                          UINT64 timeout_ms)
{
	UINT64 started_ts = 0;
	UINT64 now = 0;
	BOOL timed_out = FALSE;

	if (!client || (generation == 0) || (timeout_ms == 0))
		return FALSE;

	now = platform_get_timestamp_ms();

	EnterCriticalSection(&client->refresh_lock);
	if (client->full_refresh_in_flight && (client->full_refresh_in_flight_generation == generation))
	{
		started_ts = client->full_refresh_in_flight_timestamp_ms;
	}
	else if (client->full_refresh_requested &&
	         (client->full_refresh_requested_generation == generation))
	{
		started_ts = client->full_refresh_requested_timestamp_ms;
	}

	if ((started_ts > 0) && ((now - started_ts) >= timeout_ms))
	{
		backend_complete_full_refresh_locked(client, generation,
		                                     BACKEND_FULL_REFRESH_OUTCOME_TIMED_OUT, now);
		timed_out = TRUE;
	}
	LeaveCriticalSection(&client->refresh_lock);

	if (timed_out)
	{
		WLog_WARN(TAG, "Backend full refresh generation=%" PRIu64 " timed out after %" PRIu64 " ms",
		          generation, timeout_ms);
	}

	return timed_out;
}

void
backend_get_desktop_layout(BackendClient* client, UINT32* width, UINT32* height, UINT32* generation)
{
	if (!client)
	{
		if (width)
			*width = 0;
		if (height)
			*height = 0;
		if (generation)
			*generation = 0;
		return;
	}

	EnterCriticalSection(&client->layout_lock);
	if (width)
		*width = client->desktop_width;
	if (height)
		*height = client->desktop_height;
	if (generation)
		*generation = client->layout_generation;
	LeaveCriticalSection(&client->layout_lock);
}

void
backend_get_pointer_snapshot(BackendClient* client, UINT16* x, UINT16* y, BOOL* visible,
                             UINT32* type, PointerShapeEntry** active_shape, UINT64* position_gen,
                             UINT64* shape_gen)
{
	if (!client)
	{
		if (x)
			*x = 0;
		if (y)
			*y = 0;
		if (visible)
			*visible = FALSE;
		if (type)
			*type = SYSPTR_NULL;
		if (active_shape)
			*active_shape = NULL;
		if (position_gen)
			*position_gen = 0;
		if (shape_gen)
			*shape_gen = 0;
		return;
	}

	EnterCriticalSection(&client->pointer_lock);
	if (x)
		*x = client->pointer_x;
	if (y)
		*y = client->pointer_y;
	if (visible)
		*visible = client->pointer_visible;
	if (type)
		*type = client->pointer_type;
	if (active_shape)
		*active_shape = client->active_pointer_shape;
	if (position_gen)
		*position_gen = client->pointer_position_generation;
	if (shape_gen)
		*shape_gen = client->pointer_shape_generation;
	LeaveCriticalSection(&client->pointer_lock);
}

void
backend_get_pointer_state(BackendClient* client, UINT16* x, UINT16* y, BOOL* visible, UINT32* type,
                          UINT64* generation)
{
	UINT64 position_gen = 0;
	UINT64 shape_gen = 0;

	backend_get_pointer_snapshot(client, x, y, visible, type, NULL, &position_gen, &shape_gen);
	if (generation)
		*generation = position_gen + shape_gen;
}
