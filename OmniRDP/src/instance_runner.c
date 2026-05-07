/**
 * @file instance_runner.c
 * @brief Instance mode entry point for OmniRDP service-managed instances
 *
 * When spawned by OmniRDP-svc.exe with --instance <name> --secrets-handle <handle>,
 * this module reads the config, receives the password via anonymous pipe,
 * and runs the backend+viewer multiplexer loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#include "backend.h"
#include "ini_parser.h"
#include "platform_compat.h"
#include "svc_config.h"
#include "svc_log.h"
#include "viewer_server.h"
#include <winpr/thread.h>
#include <winpr/wtsapi.h>

/* Default config path if --config not specified */
#define DEFAULT_CONFIG_PATH "C:\\ProgramData\\OmniRDP\\config.ini"

/* Maximum password length read from pipe */
#define MAX_PASSWORD_LEN 1024

static volatile int g_running = 1;
static ViewerServer *g_server = NULL;

/**
 * @brief Heartbeat thread — sends periodic messages to the service
 *
 * Connects to a named pipe at \\.\pipe\OmniRDP_Instance_<name>
 * and sends a heartbeat message every 5 seconds.
 */
static DWORD WINAPI heartbeat_thread(LPVOID param) {
    const char *instanceName = (const char *)param;
    char pipePath[256];
    snprintf(pipePath, sizeof(pipePath), "\\\\.\\pipe\\OmniRDP_Instance_%s", instanceName);
    
    /* Wait for the pipe to become available (service creates it) */
    for (int retry = 0; retry < 30; retry++) {
        if (WaitNamedPipeA(pipePath, 1000))
            break;
        Sleep(1000);
    }
    
    HANDLE hPipe = CreateFileA(pipePath, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
        LOG_W("heartbeat", "Failed to connect to heartbeat pipe for '%s' (error %lu)",
              instanceName, GetLastError());
        return 1;
    }
    
    LOG_I("heartbeat", "Connected to heartbeat pipe for '%s'", instanceName);
    
    while (g_running) {
        /* Send heartbeat: just write a timestamp */
        char msg[64];
        int len = snprintf(msg, sizeof(msg), "heartbeat:%llu\n",
                           (unsigned long long)GetTickCount64());
        DWORD written;
        WriteFile(hPipe, msg, (DWORD)len, &written, NULL);
        
        Sleep(5000); /* Heartbeat every 5 seconds */
    }
    
    CloseHandle(hPipe);
    return 0;
}

static void instance_shutdown_handler(void) {
    g_running = 0;
    if (g_server)
        viewer_server_stop(g_server);
}

static DWORD WINAPI instance_server_thread(LPVOID arg) {
    ViewerServer *server = (ViewerServer *)arg;
    viewer_server_start(server);
    return 0;
}

/**
 * @brief Read password from anonymous pipe handle
 *
 * The service creates an anonymous pipe, passes the read end to the child
 * process via --secrets-handle. The child reads the password and closes the handle.
 *
 * @param handle_value The Windows HANDLE value (as SIZE_T from command line)
 * @param password_buf Output buffer for password
 * @param password_buf_size Size of password_buf
 * @return 0 on success, -1 on error
 */
static int read_password_from_pipe(SIZE_T handle_value, char *password_buf,
                                   size_t password_buf_size) {
    HANDLE hRead = (HANDLE)handle_value;

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hRead, password_buf, (DWORD)(password_buf_size - 1), &bytesRead, NULL);
    if (!ok || bytesRead == 0) {
        fprintf(stderr, "Failed to read password from pipe (error %lu)\n", GetLastError());
        CloseHandle(hRead);
        return -1;
    }

    password_buf[bytesRead] = '\0';
    CloseHandle(hRead);
    return 0;
}

/**
 * @brief Parse instance runner command-line arguments
 *
 * Expected format: --instance <name> --secrets-handle <handle> [--config <path>]
 */
typedef struct {
    const char *instance_name;
    SIZE_T secrets_handle;
    const char *config_path;
} InstanceRunnerArgs;

static int parse_instance_args(int argc, char *argv[], InstanceRunnerArgs *args) {
    memset(args, 0, sizeof(*args));
    args->config_path = DEFAULT_CONFIG_PATH;

    int i = 1; /* skip argv[0] */
    while (i < argc) {
        if (strcmp(argv[i], "--instance") == 0 && i + 1 < argc) {
            args->instance_name = argv[++i];
        } else if (strcmp(argv[i], "--secrets-handle") == 0 && i + 1 < argc) {
            /* Parse handle as pointer-sized value */
            args->secrets_handle = (SIZE_T)_strtoui64(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            args->config_path = argv[++i];
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return -1;
        }
        i++;
    }

    if (!args->instance_name) {
        fprintf(stderr, "Missing required argument: --instance <name>\n");
        return -1;
    }
    if (args->secrets_handle == 0) {
        fprintf(stderr, "Missing required argument: --secrets-handle <handle>\n");
        return -1;
    }

    return 0;
}

/**
 * @brief Instance mode entry point
 *
 * Called from main() when --instance flag is detected.
 * Returns exit code (0 = success, non-zero = error).
 */
int instance_runner_main(int argc, char *argv[]) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    InstanceRunnerArgs args;
    if (parse_instance_args(argc, argv, &args) != 0) {
        fprintf(stderr, "Usage: OmniRDP.exe --instance <name> --secrets-handle <handle> "
                         "[--config <path>]\n");
        return 1;
    }

    /* Set working directory to the executable's directory so that
     * relative paths (server.crt, server.key) are resolved correctly.
     * This is needed because the service spawns us with a different
     * working directory (typically C:\Windows\System32). */
    {
        char exePath[MAX_PATH];
        char exeDir[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char *lastSlash = strrchr(exePath, '\\');
        if (lastSlash) {
            size_t dirLen = (size_t)(lastSlash - exePath);
            memcpy(exeDir, exePath, dirLen);
            exeDir[dirLen] = '\0';
            SetCurrentDirectoryA(exeDir);
            LOG_I("instance_runner", "Working directory set to: %s", exeDir);
        }
    }

    printf("Instance runner: name=%s, config=%s\n", args.instance_name, args.config_path);

    /* Load config */
    SvcConfig *config = svc_config_load(args.config_path);
    if (!config) {
        fprintf(stderr, "Failed to load config from %s\n", args.config_path);
        return 1;
    }

    /* Find our instance */
    const InstanceConfig *inst = svc_config_find_instance(config, args.instance_name);
    if (!inst) {
        fprintf(stderr, "Instance '%s' not found in config\n", args.instance_name);
        svc_config_free(config);
        return 1;
    }

    if (!inst->enabled) {
        LOG_W("instance_runner", "Instance '%s' is disabled", args.instance_name);
        svc_config_free(config);
        return 1;
    }

    /* Read password from pipe */
    char password[MAX_PASSWORD_LEN];
    if (read_password_from_pipe(args.secrets_handle, password, sizeof(password)) != 0) {
        svc_config_free(config);
        return 1;
    }

    /* Setup signal handling */
    platform_signal_init(instance_shutdown_handler);

    LOG_I("instance_runner", "Backend target: %s:%u, Viewer: %s:%u, Monitors: %u",
          inst->backend_hostname, inst->backend_port,
          inst->viewer_bind_address, inst->viewer_port, inst->display_monitor_count);

    /* Initialize logging */
    {
        const char *log_dir = config->service.log_dir[0] != '\0'
                              ? config->service.log_dir
                              : "C:\\ProgramData\\OmniRDP\\logs";
        char instance_log_dir[512];
        snprintf(instance_log_dir, sizeof(instance_log_dir), "%s\\%s",
                 log_dir, args.instance_name);
        svc_log_init(instance_log_dir, SVC_LOG_DEBUG, 10, 5);
        LOG_I("instance_runner", "Instance '%s' starting (config=%s)",
              args.instance_name, args.config_path);
    }

    /* Initialize backend */
    BackendClient *client = backend_init();
    if (!client) {
        LOG_E("instance_runner", "Failed to initialize backend client for '%s'", args.instance_name);
        /* Zero out password before freeing config */
        SecureZeroMemory(password, sizeof(password));
        svc_config_free(config);
        return 1;
    }

    backend_set_monitor_count(client, inst->display_monitor_count);

    if (!backend_configure(client, inst->backend_hostname, inst->backend_port,
                           inst->backend_username, password, inst->backend_domain)) {
        LOG_E("instance_runner", "Failed to configure backend for '%s'", args.instance_name);
        SecureZeroMemory(password, sizeof(password));
        backend_free(client);
        svc_config_free(config);
        return 1;
    }

    /* Zero out password after use */
    SecureZeroMemory(password, sizeof(password));

    if (!backend_connect(client)) {
        LOG_E("instance_runner", "Failed to connect to backend %s:%u", inst->backend_hostname, inst->backend_port);
        backend_free(client);
        svc_config_free(config);
        return 1;
    }

    LOG_I("instance_runner", "Connected to backend %s:%u successfully", inst->backend_hostname, inst->backend_port);

    /* Initialize viewer server */
    ViewerServer *server = viewer_server_init(inst->viewer_bind_address,
                                              inst->viewer_port, client);
    if (!server) {
        LOG_E("instance_runner", "Failed to initialize viewer server on %s:%u", inst->viewer_bind_address, inst->viewer_port);
        backend_disconnect(client);
        backend_free(client);
        svc_config_free(config);
        return 1;
    }

    /* Register FreeRDP WTS API */
    {
        extern const WtsApiFunctionTable *FreeRDP_InitWtsApi(void);
        if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi())) {
            LOG_E("instance_runner", "Failed to register FreeRDP WTS API");
            viewer_server_free(server);
            backend_disconnect(client);
            backend_free(client);
            svc_config_free(config);
            return 1;
        }
    }

    g_server = server;

    HANDLE server_tid = CreateThread(NULL, 0, instance_server_thread, server, 0, NULL);
    if (!server_tid) {
        LOG_E("instance_runner", "Failed to create viewer server thread");
        viewer_server_free(server);
        backend_disconnect(client);
        backend_free(client);
        svc_config_free(config);
        return 1;
    }

    LOG_I("instance_runner", "Viewer server started on %s:%u", inst->viewer_bind_address, inst->viewer_port);

    /* Start heartbeat thread */
    HANDLE hHeartbeat = CreateThread(NULL, 0, heartbeat_thread,
                                     (LPVOID)args.instance_name, 0, NULL);
    if (!hHeartbeat) {
        LOG_W("instance_runner", "Failed to create heartbeat thread");
    }

    printf("Press Ctrl+C to disconnect\n\n");
    platform_sleep_ms(2000);

    /* Main event loop — same pattern as standalone main.c */
    while (g_running && backend_is_connected(client)) {
        if (!backend_iterate(client)) {
            printf("Connection lost\n");
            break;
        }
        platform_sleep_ms(1);
    }

    printf("\nDisconnecting instance '%s'...\n", args.instance_name);
    viewer_server_stop(server);
    WaitForSingleObject(server_tid, INFINITE);
    CloseHandle(server_tid);
    viewer_server_free(server);

    if (hHeartbeat) {
        /* Signal the thread to stop (g_running is already 0) */
        WaitForSingleObject(hHeartbeat, 6000);
        CloseHandle(hHeartbeat);
    }
    backend_disconnect(client);
    backend_free(client);
    svc_config_free(config);

    printf("Instance '%s' done.\n", args.instance_name);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
