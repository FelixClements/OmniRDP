#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <winpr/thread.h>
#include <winpr/wtsapi.h>
#include "backend.h"
#include "viewer_server.h"
#include "platform_compat.h"

static volatile int running = 1;
static ViewerServer* g_server = NULL;

static void shutdown_handler(void)
{
    running = 0;
    if (g_server)
        viewer_server_stop(g_server);
}

static DWORD WINAPI server_thread(LPVOID arg)
{
    ViewerServer* server = (ViewerServer*)arg;
    viewer_server_start(server);
    return 0;
}

void print_usage(const char* program)
{
    printf("Usage: %s <hostname> <port> <username> <password> [domain]\n", program);
    printf("Example: %s 192.168.1.209 3389 localadmin localadmin .\n", program);
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    if (argc < 5)
    {
        print_usage(argv[0]);
        return 1;
    }

    const char* hostname = argv[1];
    UINT16 port = (UINT16)atoi(argv[2]);
    const char* username = argv[3];
    const char* password = argv[4];
    const char* domain = (argc > 5) ? argv[5] : NULL;

    platform_signal_init(shutdown_handler);

    printf("N:1 Multiplexer v2 - SurfaceBits Passthrough\n");
    printf("============================================\n");
    printf("Connecting to: %s:%d\n", hostname, port);
    printf("Username: %s\n", username);
    printf("Domain: %s\n", domain ? domain : "(workgroup)");
    printf("\n");

    BackendClient* client = backend_init();
    if (!client)
    {
        fprintf(stderr, "Failed to initialize backend client\n");
        return 1;
    }

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

    ViewerServer* server = viewer_server_init("0.0.0.0", 13389, client);
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

    printf("DEBUG: entering loop, connected=%d, running=%d\n", backend_is_connected(client), running);

    time_t last_stats = time(NULL);
    UINT64 last_tile_count = 0;

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
            const double fps = (seconds > 0.0) ? ((double)(tile_count - last_tile_count) / seconds) : 0.0;

            if (tile_count != last_tile_count)
            {
                printf("[Stats] Tiles: %" PRIu64 " | Bytes: %" PRIu64 " | Markers: %" PRIu64 " | Rate: %.1f updates/s\n",
                       tile_count, total_bytes, frame_markers, fps);
                last_tile_count = tile_count;
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
