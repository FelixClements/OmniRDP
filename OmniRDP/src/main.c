#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#endif
#include "backend.h"
#include "platform_compat.h"
#include "viewer_internal.h"
#include "viewer_server.h"
#include <winpr/thread.h>
#include <winpr/wtsapi.h>

/* Instance runner entry point (instance_runner.c) */
int instance_runner_main(int argc, char* argv[]);

static volatile int running = 1;
static ViewerServer* g_server = NULL;

static void
shutdown_handler(void)
{
	running = 0;
	if (g_server)
		viewer_server_stop(g_server);
}

static DWORD WINAPI
server_thread(LPVOID arg)
{
	ViewerServer* server = (ViewerServer*)arg;
	viewer_server_start(server);
	return 0;
}

void
print_usage(const char* program)
{
	printf("Usage:\n");
	printf("  %s <hostname> <port> <username> <password> [domain] [monitors]\n", program);
	printf("  %s --instance <name> --secrets-handle <handle> [--config <path>]\n", program);
	printf("\nStandalone mode:\n");
	printf("  Example: %s 192.168.1.209 3389 localadmin localadmin . 2\n", program);
	printf("  monitors: number of 1920x1080 monitors (1-16, default: 1)\n");
	printf("\nInstance mode (spawned by OmniRDP-svc):\n");
	printf("  --instance     Instance name from config.ini\n");
	printf("  --secrets-handle  Windows HANDLE for password pipe\n");
	printf("  --config       Path to config.ini (default: "
	       "C:\\ProgramData\\OmniRDP\\config.ini)\n");
}

int
main(int argc, char* argv[])
{
#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		fprintf(stderr, "WSAStartup failed\n");
		return 1;
	}
#endif

	/* Instance mode: --instance <name> --secrets-handle <handle> [--config
	 * <path>] */
	if (argc >= 2 && strcmp(argv[1], "--instance") == 0)
	{
		int ret = instance_runner_main(argc, argv);
#ifdef _WIN32
		WSACleanup();
#endif
		return ret;
	}

	if (argc < 5)
	{
		print_usage(argv[0]);
		return 1;
	}

	const char* hostname = argv[1];
	UINT16 port = (UINT16)atoi(argv[2]);
	const char* username = argv[3];
	const char* password = argv[4];
	const char* domain = NULL;
	UINT32 monitor_count = 1;

	/* Parse optional 5th and 6th arguments.
	 * Format: [domain] [monitors] or just [monitors] (no domain).
	 * Detect whether argv[5] is a monitor count (small integer) or a domain.
	 * If both domain and monitors are needed, use: <hostname> <port> <user>
	 * <pass> <domain> <monitors> */
	if (argc > 6)
	{
		/* Both domain and monitors provided */
		domain = argv[5];
		monitor_count = (UINT32)atoi(argv[6]);
	}
	else if (argc > 5)
	{
		/* Only one optional arg — is it a monitor count or a domain? */
		const char* maybe_monitors = argv[5];
		int parsed = atoi(maybe_monitors);
		if (parsed > 0 && parsed <= OMNIRDP_MAX_MONITORS && strchr(maybe_monitors, '.') == NULL &&
		    strlen(maybe_monitors) < 3)
		{
			/* Looks like a monitor count (small positive integer), not a domain */
			monitor_count = (UINT32)parsed;
			domain = NULL;
		}
		else
		{
			/* Looks like a domain name, not a monitor count */
			domain = argv[5];
		}
	}

	if (monitor_count == 0 || monitor_count > OMNIRDP_MAX_MONITORS)
	{
		fprintf(stderr, "Monitor count must be 1-%d, got %u\n", OMNIRDP_MAX_MONITORS,
		        monitor_count);
		print_usage(argv[0]);
		return 1;
	}

	platform_signal_init(shutdown_handler);

	printf("N:1 Multiplexer v2 - SurfaceBits Passthrough\n");
	printf("============================================\n");
	printf("Connecting to: %s:%d\n", hostname, port);
	printf("Monitors: %u (%ux%u desktop)\n", monitor_count, monitor_count * 1920, 1080);
	printf("Username: %s\n", username);
	printf("Domain: %s\n", domain ? domain : "(workgroup)");
	printf("\n");

	BackendClient* client = backend_init();
	if (!client)
	{
		fprintf(stderr, "Failed to initialize backend client\n");
		return 1;
	}

	backend_set_monitor_count(client, monitor_count);

	if (!backend_configure(client, hostname, port, username, password, domain))
	{
		fprintf(stderr, "Failed to configure backend\n");
		backend_free(client);
		return 1;
	}

	printf("Connecting to Windows Server...\n");

	if (!backend_connect(client))
	{
		fprintf(stderr, "Failed to connect to backend\n");
		backend_free(client);
		return 1;
	}

	printf("Connected successfully!\n");
	printf("Starting viewer server on port 13389...\n");

	ViewerServer* server = viewer_server_init("0.0.0.0", 13389, client, NULL, NULL);
	if (!server)
	{
		fprintf(stderr, "Failed to initialize viewer server\n");
		backend_disconnect(client);
		backend_free(client);
		return 1;
	}

	/* Register FreeRDP's WTS API function table BEFORE any WTS calls.
	 * On Windows, the WinPR WTS stubs default to wtsapi32.dll; this override
	 * ensures WTSOpenServerA/WTSCloseServer use FreeRDP's implementations. */
	{
		extern const WtsApiFunctionTable* FreeRDP_InitWtsApi(void);
		if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi()))
		{
			fprintf(stderr, "Failed to register FreeRDP WTS API\n");
			viewer_server_free(server);
			backend_disconnect(client);
			backend_free(client);
			return 1;
		}
	}

	g_server = server;

	HANDLE server_tid = CreateThread(NULL, 0, server_thread, server, 0, NULL);
	if (!server_tid)
	{
		fprintf(stderr, "Failed to create server thread\n");
		viewer_server_free(server);
		backend_disconnect(client);
		backend_free(client);
		return 1;
	}

	printf("Viewer server started on port 13389\n");
	printf("Press Ctrl+C to disconnect\n\n");
	platform_sleep_ms(2000);

	printf("DEBUG: entering loop, connected=%d, running=%d\n", backend_is_connected(client),
	       running);

	time_t last_stats = time(NULL);
	UINT64 last_tile_count = 0;
	UINT64 last_bitmap_batch_count = 0;

	while (running && backend_is_connected(client))
	{
		if (!backend_iterate(client))
		{
			printf("Connection lost\n");
			break;
		}

		time_t now = time(NULL);
		if (now - last_stats >= 2)
		{
			const double seconds = difftime(now, last_stats);
			const UINT64 tile_count = client->forwarded_surface_bits_count;
			const UINT64 total_bytes = client->forwarded_surface_bits_bytes;
			const UINT64 frame_markers = client->forwarded_frame_marker_count;
			const UINT64 bitmap_batches = client->bitmap_update_batches_total;
			const UINT64 bitmap_rectangles = client->bitmap_update_rectangles_total;
			const UINT64 bitmap_bytes = client->bitmap_update_payload_bytes_total;
			const double avg_rects_per_batch =
			    (bitmap_batches > 0) ? ((double)bitmap_rectangles / (double)bitmap_batches) : 0.0;
			const double avg_callback_us =
			    (bitmap_batches > 0) ? ((double)client->bitmap_update_callback_time_total_us /
			                            (double)bitmap_batches)
			                         : 0.0;
			const double avg_publish_us =
			    (bitmap_batches > 0)
			        ? ((double)client->bitmap_update_publish_time_total_us / (double)bitmap_batches)
			        : 0.0;
			const double fps =
			    (seconds > 0.0) ? ((double)(tile_count - last_tile_count) / seconds) : 0.0;

			if ((tile_count != last_tile_count) || (bitmap_batches != last_bitmap_batch_count))
			{
				printf("[Stats] Tiles: %" PRIu64 " | Bytes: %" PRIu64 " | Markers: %" PRIu64
				       " | Rate: %.1f updates/s"
				       " | Bitmap batches: %" PRIu64 " rects: %" PRIu64 " bytes: %" PRIu64
				       " avgRectBatch: %.2f"
				       " avgCbUs: %.1f avgPubUs: %.1f"
				       " maxCbUs: %" PRIu64 " maxPubUs: %" PRIu64 "\n",
				       tile_count, total_bytes, frame_markers, fps, bitmap_batches,
				       bitmap_rectangles, bitmap_bytes, avg_rects_per_batch, avg_callback_us,
				       avg_publish_us, client->bitmap_update_callback_time_max_us,
				       client->bitmap_update_publish_time_max_us);
				last_tile_count = tile_count;
				last_bitmap_batch_count = bitmap_batches;
			}
			else
			{
				printf("[Stats] No new forwarded updates\n");
			}

			last_stats = now;
		}

		platform_sleep_ms(1);
	}

	printf("\nDisconnecting...\n");
	viewer_server_stop(server);
	WaitForSingleObject(server_tid, INFINITE);
	CloseHandle(server_tid);
	viewer_server_free(server);
	backend_disconnect(client);
	backend_free(client);

	printf("Done.\n");
#ifdef _WIN32
	WSACleanup();
#endif
	return 0;
}
