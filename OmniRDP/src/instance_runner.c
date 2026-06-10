/**
 * @file instance_runner.c
 * @brief Instance mode entry point for OmniRDP service-managed instances
 *
 * When spawned by OmniRDP-svc.exe with --instance <name> --secrets-handle
 * <handle>, this module reads the config, receives the password via anonymous
 * pipe, and runs the backend+viewer multiplexer loop.
 */

#include <inttypes.h>
#include <share.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
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

#define INSTANCE_RUNNER_FIXED_MAX_VIEWERS 10U

static volatile int g_running = 1;
static ViewerServer *g_server = NULL;

#include "safe_string.h"
#include <winpr/wlog.h>

/* Viewer log file state — owned by our callback, rotated by us */
static FILE *g_viewer_logfile = NULL;
static char g_viewer_log_path[MAX_PATH] = {0};
static unsigned int g_viewer_max_size_mb = 10;
static unsigned int g_viewer_max_files = 5;

static const char *bool_str(int value) { return value ? "true" : "false"; }

static const char *svc_log_level_to_wlog(SvcLogLevel level) {
  switch (level) {
  case SVC_LOG_DEBUG:
    return "DEBUG";
  case SVC_LOG_WARN:
    return "WARN";
  case SVC_LOG_ERROR:
    return "ERROR";
  case SVC_LOG_INFO:
  default:
    return "INFO";
  }
}

static ViewerGfxCodec instance_viewer_gfx_codec(int codec) {
  return (codec == SVC_VIEWER_GFX_CODEC_RFX) ? VIEWER_GFX_CODEC_RFX
                                             : VIEWER_GFX_CODEC_UNCOMPRESSED;
}

static const char *instance_viewer_gfx_codec_name(int codec) {
  return (codec == SVC_VIEWER_GFX_CODEC_RFX) ? "rfx" : "uncompressed";
}

static const char *svc_log_level_to_text(SvcLogLevel level) {
  switch (level) {
  case SVC_LOG_DEBUG:
    return "debug";
  case SVC_LOG_WARN:
    return "warn";
  case SVC_LOG_ERROR:
    return "error";
  case SVC_LOG_INFO:
  default:
    return "info";
  }
}

static int instance_key_configured(const SvcConfig *config,
                                   const InstanceConfig *inst,
                                   const char *key) {
  enum { SECTION_CAPACITY = 256 };
  char *section = NULL;
  int configured = 0;
  if (!config || !config->ini || !inst || !key)
    return 0;
  section = (char *)calloc(SECTION_CAPACITY, sizeof(*section));
  if (!section)
    return 0;
  if (omni_format(section, SECTION_CAPACITY, "instance:%s", inst->name) >= 0)
    configured = ini_get(config->ini, section, key, NULL) != NULL;
  free(section);
  return configured;
}

static void log_effective_instance_config(const SvcConfig *config,
                                          const InstanceConfig *inst) {
  LOG_I("instance_runner",
        "Config effective: enabled=%s backend=%s:%u user=%s domain=%s "
        "connect_timeout_ms=%u",
        bool_str(inst->enabled), inst->backend_hostname, inst->backend_port,
        inst->backend_username, inst->backend_domain,
        inst->backend_connect_timeout_ms);
  LOG_I("instance_runner",
        "Config security: backend nla=%s tls=%s rdp=%s server_auth=%s "
        "ignore_cert=%s viewer nla=%s tls=%s rdp=%s auth=%s",
        bool_str(inst->backend_security_nla_enabled),
        bool_str(inst->backend_security_tls_enabled),
        bool_str(inst->backend_security_rdp_enabled),
        bool_str(inst->backend_security_server_authentication),
        bool_str(inst->backend_security_ignore_certificate),
        bool_str(inst->viewer_security_nla_enabled),
        bool_str(inst->viewer_security_tls_enabled),
        bool_str(inst->viewer_security_rdp_enabled), inst->viewer_auth_mode);
  LOG_I("instance_runner",
        "Config viewer: bind=%s:%u cert=%s key=%s max_viewers=%u "
        "slow_disconnect=%s/%ums slow_lag_interval_ms=%u throttle_ups=%u",
        inst->viewer_bind_address, inst->viewer_port,
        inst->viewer_cert_path[0] ? "set" : "unset",
        inst->viewer_key_path[0] ? "set" : "unset", inst->viewer_max_viewers,
        bool_str(inst->viewer_slow_disconnect_enabled),
        inst->viewer_slow_disconnect_after_ms,
        inst->viewer_slow_lag_interval_ms,
        inst->viewer_throttle_max_updates_per_sec);
  LOG_I("instance_runner",
        "Config reconnect: enabled=%s max_attempts=%u initial_delay_ms=%u "
        "max_delay_ms=%u backoff=%.2f",
        bool_str(inst->reconnect_enabled), inst->reconnect_max_attempts,
        inst->reconnect_initial_delay_ms, inst->reconnect_max_delay_ms,
        inst->reconnect_backoff_multiplier);
  LOG_I("instance_runner",
        "Config display/codecs: monitors=%u size=%ux%u depth=%u "
        "backend_gfx_decode=%s backend_rfx=%s nscodec=%s "
        "remote_fx=%s gfx=%s h264=%s avc444=%s avc444v2=%s frame_ack=%u",
        inst->display_monitor_count, inst->display_monitor_width,
        inst->display_monitor_height, inst->display_color_depth,
        bool_str(inst->backend_gfx_decode_only_enabled),
        bool_str(inst->backend_gfx_rfx_enabled), bool_str(inst->codec_nscodec),
        bool_str(inst->codec_remote_fx),
        bool_str(inst->codec_graphics_pipeline), bool_str(inst->codec_h264),
        bool_str(inst->codec_avc444), bool_str(inst->codec_avc444v2),
        inst->codec_frame_acknowledge);

  if (inst->viewer_max_viewers != INSTANCE_RUNNER_FIXED_MAX_VIEWERS ||
      instance_key_configured(config, inst, "viewer.max_viewers"))
    LOG_W("instance_runner",
          "viewer.max_viewers is reserved; runtime fixed maximum is %u",
          INSTANCE_RUNNER_FIXED_MAX_VIEWERS);
  if (inst->viewer_slow_lag_interval_ms != 5000 ||
      instance_key_configured(config, inst, "viewer.slow_lag_interval_ms"))
    LOG_W("instance_runner",
          "viewer.slow_lag_interval_ms is currently reserved/no-op");
  if (inst->viewer_late_join_timeout_ms != 15000 ||
      inst->viewer_late_join_refresh_deadline_ms != 5000 ||
      inst->viewer_late_join_replay_max_frames != 4 ||
      instance_key_configured(config, inst, "viewer.late_join_timeout_ms") ||
      instance_key_configured(config, inst,
                              "viewer.late_join_refresh_deadline_ms") ||
      instance_key_configured(config, inst,
                              "viewer.late_join_replay_max_frames"))
    LOG_W("instance_runner",
          "viewer late-join tunables are currently reserved/no-op");
  if (inst->viewer_throttle_max_updates_per_sec != 0 ||
      instance_key_configured(config, inst,
                              "viewer.throttle_max_updates_per_sec"))
    LOG_W("instance_runner",
          "viewer.throttle_max_updates_per_sec is currently reserved/no-op");
  if (inst->display_monitor_width != 1920 ||
      inst->display_monitor_height != 1080 || inst->display_color_depth != 32 ||
      instance_key_configured(config, inst, "display.monitor_width") ||
      instance_key_configured(config, inst, "display.monitor_height") ||
      instance_key_configured(config, inst, "display.color_depth"))
    LOG_W("instance_runner",
          "display width/height/color_depth are currently reserved/no-op; "
          "backend negotiated geometry is used");
  if (inst->codec_nscodec != 1 || inst->codec_remote_fx != 1 ||
      inst->codec_graphics_pipeline != 0 || inst->codec_h264 != 0 ||
      inst->codec_avc444 != 0 || inst->codec_avc444v2 != 0 ||
      inst->codec_frame_acknowledge != 4 ||
      instance_key_configured(config, inst, "codec.nscodec") ||
      instance_key_configured(config, inst, "codec.remote_fx") ||
      instance_key_configured(config, inst, "codec.graphics_pipeline") ||
      instance_key_configured(config, inst, "codec.h264") ||
      instance_key_configured(config, inst, "codec.avc444") ||
      instance_key_configured(config, inst, "codec.avc444v2") ||
      instance_key_configured(config, inst, "codec.frame_acknowledge"))
    LOG_W("instance_runner", "codec toggles are currently reserved/no-op");
  if (strcmp(config->service.pipe_name, "OmniRDP_ServicePipe") != 0)
    LOG_W("instance_runner",
          "service.pipe_name custom value is currently reserved/no-op for "
          "instance heartbeat/status pipes");
  if (strcmp(inst->security_tls_min_version, "1.2") != 0 ||
      instance_key_configured(config, inst, "security.tls_min_version"))
    LOG_W("instance_runner",
          "security.tls_min_version is currently reserved/no-op");
  if (inst->backend_security_server_authentication ||
      instance_key_configured(config, inst,
                              "backend.security.server_authentication") ||
      instance_key_configured(config, inst, "security.server_authentication"))
    LOG_W("instance_runner",
          "backend server_authentication policy is currently reserved/no-op; "
          "certificate ignore setting is applied");
}

/**
 * @brief WLog callback — handles every FreeRDP log message.
 *
 * Owns the viewer.log FILE handle. Writes messages, checks rotation
 * on every call (same pattern as svc_log_check_rotate).
 */
static BOOL viewer_wlog_callback(const wLogMessage *msg) {
  if (!g_viewer_logfile)
    return FALSE;

  /* Check rotation before writing */
  long pos = ftell(g_viewer_logfile);
  if (pos >= 0) {
    unsigned long long max_bytes =
        (unsigned long long)g_viewer_max_size_mb * 1024ULL * 1024ULL;
    if ((unsigned long long)pos >= max_bytes)
      svc_log_rotate_file(g_viewer_log_path, &g_viewer_logfile,
                          g_viewer_max_files);
  }

  if (!g_viewer_logfile)
    return FALSE;

  /* Write: prefix is already formatted by WLog layout engine */
  fprintf_s(g_viewer_logfile, "%s%s\n",
            msg->PrefixString ? msg->PrefixString : "",
            msg->TextString ? msg->TextString : "");
  fflush(g_viewer_logfile);
  return TRUE;
}

/**
 * @brief Heartbeat thread — sends periodic messages to the service
 *
 * Connects to a named pipe at \\.\pipe\OmniRDP_Instance_<name>
 * and sends a heartbeat message every 5 seconds.
 */
static DWORD WINAPI heartbeat_thread(LPVOID param) {
  const char *instanceName = (const char *)param;
  enum { PIPE_PATH_CAPACITY = 256, HEARTBEAT_MSG_CAPACITY = 64 };
  char *pipePath = (char *)calloc(PIPE_PATH_CAPACITY, sizeof(*pipePath));
  if (!pipePath)
    return 1;
  int pipePathLen =
      omni_format(pipePath, PIPE_PATH_CAPACITY,
                  "\\\\.\\pipe\\OmniRDP_Instance_%s", instanceName);
  if (pipePathLen < 0 || (size_t)pipePathLen >= PIPE_PATH_CAPACITY) {
    free(pipePath);
    return 1;
  }

  /* Wait for the pipe to become available (service creates it) */
  for (int retry = 0; retry < 30; retry++) {
    if (WaitNamedPipeA(pipePath, 1000))
      break;
    Sleep(1000);
  }

  HANDLE hPipe =
      CreateFileA(pipePath, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (hPipe == INVALID_HANDLE_VALUE) {
    LOG_W("heartbeat",
          "Failed to connect to heartbeat pipe for '%s' (error %lu)",
          instanceName, GetLastError());
    free(pipePath);
    return 1;
  }

  LOG_I("heartbeat", "Connected to heartbeat pipe for '%s'", instanceName);
  char *msg = (char *)calloc(HEARTBEAT_MSG_CAPACITY, sizeof(*msg));
  if (!msg) {
    CloseHandle(hPipe);
    free(pipePath);
    return 1;
  }

  while (g_running) {
    /* Send heartbeat: timestamp and viewer count */
    unsigned int vc = viewer_server_get_count(g_server);
    int len = omni_format(msg, HEARTBEAT_MSG_CAPACITY, "heartbeat:%llu:%u\n",
                          (unsigned long long)GetTickCount64(), vc);
    DWORD written;
    if (len > 0 && (size_t)len < HEARTBEAT_MSG_CAPACITY)
      WriteFile(hPipe, msg, (DWORD)len, &written, NULL);

    Sleep(5000); /* Heartbeat every 5 seconds */
  }

  free(msg);
  free(pipePath);
  CloseHandle(hPipe);
  return 0;
}

static void instance_shutdown_handler(void) {
  g_running = 0;
  if (g_server)
    viewer_server_stop(g_server);
}

#ifdef _WIN32
static DWORD WINAPI stop_event_thread(LPVOID param) {
  HANDLE hStopEvent = (HANDLE)param;
  DWORD result = WaitForSingleObject(hStopEvent, INFINITE);
  if (result == WAIT_OBJECT_0) {
    LOG_I("instance_runner", "Stop event signaled; shutting down instance");
    instance_shutdown_handler();
  } else {
    LOG_W("instance_runner", "Stop event wait failed (result=%lu, err=%lu)",
          result, GetLastError());
  }
  if (hStopEvent)
    CloseHandle(hStopEvent);
  return 0;
}
#endif

static DWORD WINAPI instance_server_thread(LPVOID arg) {
  ViewerServer *server = (ViewerServer *)arg;
  viewer_server_start(server);
  return 0;
}

/**
 * @brief Read password from anonymous pipe handle
 *
 * The service creates an anonymous pipe, passes the read end to the child
 * process via --secrets-handle. The child reads the password and closes the
 * handle.
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
  BOOL ok = ReadFile(hRead, password_buf, (DWORD)(password_buf_size - 1),
                     &bytesRead, NULL);
  if (!ok || bytesRead == 0) {
    fprintf(stderr, "Failed to read password from pipe (error %lu)\n",
            GetLastError());
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
 * Expected format: --instance <name> --secrets-handle <handle> [--config
 * <path>]
 */
typedef struct {
  const char *instance_name;
  SIZE_T secrets_handle;
  SIZE_T stop_event_handle;
  const char *config_path;
} InstanceRunnerArgs;

static int parse_instance_args(int argc, char *argv[],
                               InstanceRunnerArgs *args) {
  memset(args, 0, sizeof(*args));
  args->config_path = DEFAULT_CONFIG_PATH;

  int i = 1; /* skip argv[0] */
  while (i < argc) {
    if (strcmp(argv[i], "--instance") == 0 && i + 1 < argc) {
      args->instance_name = argv[++i];
    } else if (strcmp(argv[i], "--secrets-handle") == 0 && i + 1 < argc) {
      /* Parse handle as pointer-sized value */
      args->secrets_handle = (SIZE_T)_strtoui64(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--stop-event") == 0 && i + 1 < argc) {
      args->stop_event_handle = (SIZE_T)_strtoui64(argv[++i], NULL, 10);
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
    fprintf(stderr,
            "Usage: OmniRDP.exe --instance <name> --secrets-handle <handle> "
            "[--stop-event <handle>] [--config <path>]\n");
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
      if (dirLen >= sizeof(exeDir))
        dirLen = sizeof(exeDir) - 1;
      memmove(exeDir, exePath, dirLen);
      exeDir[dirLen] = '\0';
      SetCurrentDirectoryA(exeDir);
    }
  }

  printf("Instance runner: name=%s, config=%s\n", args.instance_name,
         args.config_path);

  /* Load config */
  SvcConfig *config = svc_config_load(args.config_path);
  if (!config) {
    fprintf(stderr, "Failed to load config from %s\n", args.config_path);
    return 1;
  }

  /* Find our instance */
  const InstanceConfig *inst =
      svc_config_find_instance(config, args.instance_name);
  if (!inst) {
    fprintf(stderr, "Instance '%s' not found in config\n", args.instance_name);
    svc_config_free(config);
    return 1;
  }

  /* Initialize logging */
  {
    const char *log_dir = config->service.log_dir[0] != '\0'
                              ? config->service.log_dir
                              : "C:\\ProgramData\\OmniRDP\\logs";
    char instance_log_dir[512];
    SvcLogLevel log_level = SVC_LOG_INFO;
    if (svc_log_level_from_string(config->service.log_level, &log_level) != 0)
      log_level = SVC_LOG_INFO;

    omni_format(instance_log_dir, sizeof(instance_log_dir), "%s\\%s", log_dir,
                args.instance_name);
    svc_log_init(instance_log_dir, log_level, config->service.log_max_size_mb,
                 config->service.log_max_files);
    LOG_I("instance_runner", "Instance '%s' starting (config=%s)",
          args.instance_name, args.config_path);

    /* Configure FreeRDP WLog to use CallbackAppender.
     * OmniRDP owns the viewer.log FILE handle directly so we can
     * rotate it when the size limit is reached.
     * WLOG_PREFIX env var must be set before WLog_GetRoot() is called. */
    _putenv_s("WLOG_PREFIX", "[%hr:%mi:%se:%ml] [%pid:%tid] [%lv][%mn] - ");

    _putenv_s("WLOG_LEVEL", svc_log_level_to_wlog(log_level));

    /* Force WLog root initialization */
    wLog *root = WLog_GetRoot();

    /* Swap to CALLBACK appender (replaces default CONSOLE) */
    WLog_SetLogAppenderType(root, WLOG_APPENDER_CALLBACK);
    wLogAppender *appender = WLog_GetLogAppender(root);

    /* Register our callback to receive all log messages */
    wLogCallbacks cbs = {0};
    cbs.message = viewer_wlog_callback;
    WLog_ConfigureAppender(appender, "callbacks", &cbs);

    /* Open viewer.log ourselves and store rotation params */
    omni_format(g_viewer_log_path, sizeof(g_viewer_log_path), "%s\\viewer.log",
                instance_log_dir);
#ifdef _WIN32
    g_viewer_logfile = _fsopen(g_viewer_log_path, "a", _SH_DENYNO);
    if (!g_viewer_logfile)
      g_viewer_logfile = NULL;
#else
    {
      int fd = open(g_viewer_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd >= 0) {
        g_viewer_logfile = fdopen(fd, "a");
        if (!g_viewer_logfile)
          close(fd);
      }
    }
#endif
    g_viewer_max_size_mb = config->service.log_max_size_mb;
    g_viewer_max_files = config->service.log_max_files;
    LOG_I("instance_runner",
          "Effective logging: level=%s log_dir=%s max_size_mb=%u max_files=%u",
          svc_log_level_to_text(log_level), instance_log_dir,
          config->service.log_max_size_mb, config->service.log_max_files);
    LOG_I("instance_runner", "Viewer log (CallbackAppender) -> %s",
          g_viewer_log_path);
  }

  log_effective_instance_config(config, inst);

  if (!inst->enabled) {
    LOG_W("instance_runner", "Instance '%s' is disabled", args.instance_name);
    svc_config_free(config);
    return 1;
  }

  /* Read password from pipe */
  char password[MAX_PASSWORD_LEN];
  if (read_password_from_pipe(args.secrets_handle, password,
                              sizeof(password)) != 0) {
    svc_config_free(config);
    return 1;
  }

  /* Setup signal handling */
  platform_signal_init(instance_shutdown_handler);

#ifdef _WIN32
  if (args.stop_event_handle != 0) {
    HANDLE hStopThread = CreateThread(NULL, 0, stop_event_thread,
                                      (LPVOID)args.stop_event_handle, 0, NULL);
    if (hStopThread) {
      CloseHandle(hStopThread);
      LOG_I("instance_runner", "Stop event watcher started");
    } else {
      LOG_W("instance_runner", "Failed to create stop event watcher (err=%lu)",
            GetLastError());
      CloseHandle((HANDLE)args.stop_event_handle);
    }
  }
#endif

  /* Initialize backend */
  BackendClient *client = backend_init();
  if (!client) {
    LOG_E("instance_runner", "Failed to initialize backend client for '%s'",
          args.instance_name);
    /* Zero out password before freeing config */
    SecureZeroMemory(password, sizeof(password));
    svc_config_free(config);
    return 1;
  }

  backend_set_monitor_count(client, inst->display_monitor_count);

  BackendSecurityConfig security = {
      inst->backend_security_nla_enabled, inst->backend_security_tls_enabled,
      inst->backend_security_rdp_enabled,
      inst->backend_security_server_authentication,
      inst->backend_security_ignore_certificate};
  if (!backend_configure(client, inst->backend_hostname, inst->backend_port,
                         inst->backend_username, password, inst->backend_domain,
                         &security)) {
    LOG_E("instance_runner", "Failed to configure backend for '%s'",
          args.instance_name);
    SecureZeroMemory(password, sizeof(password));
    backend_free(client);
    svc_config_free(config);
    return 1;
  }

  BackendRdpFileOptions rdp_file_options = {
      inst->backend_rdp_workspace_id,
      inst->backend_rdp_use_redirection_server_name ? TRUE : FALSE,
      inst->backend_rdp_loadbalanceinfo,
      inst->backend_rdp_alternate_full_address};
  if (!backend_apply_rdp_file_options(client, &rdp_file_options)) {
    LOG_E("instance_runner", "Failed to configure backend RDP file options");
    SecureZeroMemory(password, sizeof(password));
    backend_free(client);
    svc_config_free(config);
    return 1;
  }
  if (!backend_set_gfx_decode_only(
          client, inst->backend_gfx_decode_only_enabled ? TRUE : FALSE)) {
    LOG_E("instance_runner", "Failed to configure backend GFX decode gate");
    SecureZeroMemory(password, sizeof(password));
    backend_free(client);
    svc_config_free(config);
    return 1;
  }
  if (!backend_set_rfx_enabled(client,
                               inst->backend_gfx_rfx_enabled ? TRUE : FALSE)) {
    LOG_E("instance_runner",
          "Failed to configure backend RemoteFX candidate mode");
    SecureZeroMemory(password, sizeof(password));
    backend_free(client);
    svc_config_free(config);
    return 1;
  }

  backend_set_connect_timeout(client, (UINT32)inst->backend_connect_timeout_ms);

  /* Zero out password after use */
  SecureZeroMemory(password, sizeof(password));

  if (!backend_connect(client)) {
    LOG_E("instance_runner", "Failed to connect to backend %s:%u",
          inst->backend_hostname, inst->backend_port);
    backend_free(client);
    svc_config_free(config);
    return 1;
  }

  LOG_I("instance_runner", "Connected to backend %s:%u successfully",
        inst->backend_hostname, inst->backend_port);

  if (!g_running) {
    LOG_I("instance_runner", "Stop requested during startup; exiting");
    backend_disconnect(client);
    backend_free(client);
    svc_config_free(config);
    if (g_viewer_logfile) {
      fclose(g_viewer_logfile);
      g_viewer_logfile = NULL;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
  }

  /* Initialize viewer server */
  ViewerAuthMode viewer_auth_mode = VIEWER_AUTH_MODE_NONE;
  if (_stricmp(inst->viewer_auth_mode, "backend_credentials") == 0)
    viewer_auth_mode = VIEWER_AUTH_MODE_BACKEND_CREDENTIALS;
  else if (_stricmp(inst->viewer_auth_mode, "none") != 0) {
    LOG_E("instance_runner",
          "Invalid viewer.auth.mode '%s' for instance '%s'. Valid values are "
          "'none' and 'backend_credentials'. Refusing to start with "
          "ambiguous viewer authentication.",
          inst->viewer_auth_mode, args.instance_name);
    backend_disconnect(client);
    backend_free(client);
    svc_config_free(config);
    return 1;
  }

  if (viewer_auth_mode == VIEWER_AUTH_MODE_BACKEND_CREDENTIALS) {
    if (!inst->viewer_security_nla_enabled) {
      LOG_W("instance_runner",
            "Viewer auth configuration for instance '%s' uses "
            "viewer.auth.mode=%s with viewer.security.nla_enabled=false. "
            "This allows backend-local accounts to reach OmniRDP's "
            "credential check because NLA validates against the OmniRDP "
            "listener host/domain before application logon callbacks run.",
            args.instance_name, inst->viewer_auth_mode);
    }

    if (!inst->viewer_security_tls_enabled) {
      LOG_W("instance_runner",
            "Viewer auth configuration for instance '%s' uses "
            "viewer.auth.mode=%s without viewer.security.tls_enabled=true. "
            "Enable TLS to protect viewer credentials in transit.",
            args.instance_name, inst->viewer_auth_mode);
    }
  }

  ViewerSecurityConfig viewer_security = {
      inst->viewer_security_nla_enabled, inst->viewer_security_tls_enabled,
      inst->viewer_security_rdp_enabled, viewer_auth_mode};
  ViewerPublisherClassicPolicyConfig classic_policy = {0};
  ViewerServer *server = viewer_server_init_ex(
      inst->viewer_bind_address, inst->viewer_port, client,
      inst->viewer_cert_path, inst->viewer_key_path, &viewer_security);
  if (!server) {
    LOG_E("instance_runner", "Failed to initialize viewer server on %s:%u",
          inst->viewer_bind_address, inst->viewer_port);
    backend_disconnect(client);
    backend_free(client);
    svc_config_free(config);
    return 1;
  }
  viewer_server_set_slow_disconnect(
      server, inst->viewer_slow_disconnect_enabled ? TRUE : FALSE,
      (UINT32)inst->viewer_slow_disconnect_after_ms);
  LOG_I("instance_runner",
        "Applied viewer slow disconnect: enabled=%s after_ms=%u",
        bool_str(inst->viewer_slow_disconnect_enabled),
        inst->viewer_slow_disconnect_after_ms);

  classic_policy.enabled =
      inst->viewer_classic_latest_state_enabled ? TRUE : FALSE;
  classic_policy.policy = VIEWER_PUBLISHER_CLASSIC_POLICY_LATEST_STATE;
  classic_policy.max_queue_depth =
      (UINT32)inst->viewer_classic_latest_state_max_queue_depth;
  classic_policy.max_queue_bytes =
      (UINT64)inst->viewer_classic_latest_state_max_queue_bytes;
  viewer_server_set_classic_policy(server, &classic_policy);
  LOG_I("instance_runner",
        "Applied classic latest-state policy: enabled=%s max_depth=%u "
        "max_bytes=%u",
        bool_str(inst->viewer_classic_latest_state_enabled),
        inst->viewer_classic_latest_state_max_queue_depth,
        inst->viewer_classic_latest_state_max_queue_bytes);

  viewer_server_set_gfx_enabled(server,
                                inst->viewer_gfx_enabled ? TRUE : FALSE);
  LOG_I("instance_runner", "Applied viewer GFX gate: enabled=%s",
        bool_str(inst->viewer_gfx_enabled));
  viewer_server_set_gfx_codec(
      server, instance_viewer_gfx_codec(inst->viewer_gfx_codec));
  LOG_I("instance_runner", "Applied viewer GFX codec preference: %s",
        instance_viewer_gfx_codec_name(inst->viewer_gfx_codec));
  viewer_server_set_gfx_rfx_threading(
      server, inst->viewer_gfx_rfx_threading_enabled ? TRUE : FALSE);
  LOG_I("instance_runner",
        "Applied viewer RFX threading experiment: enabled=%s",
        bool_str(inst->viewer_gfx_rfx_threading_enabled));
  viewer_server_set_gfx_dirty_limits(
      server, inst->viewer_gfx_dirty_max_in_flight_frames,
      (UINT64)inst->viewer_gfx_dirty_max_in_flight_bytes);
  LOG_I("instance_runner",
        "Applied viewer GFX dirty pacing: max_in_flight_frames=%u "
        "max_in_flight_bytes=%u",
        inst->viewer_gfx_dirty_max_in_flight_frames,
        inst->viewer_gfx_dirty_max_in_flight_bytes);
  viewer_server_set_gfx_diagnostic_full_frame_dirty(
      server, inst->viewer_gfx_diagnostic_full_frame_dirty ? TRUE : FALSE);
  LOG_I("instance_runner",
        "Applied viewer GFX diagnostic-only full-frame dirty: enabled=%s",
        bool_str(inst->viewer_gfx_diagnostic_full_frame_dirty));
  LOG_I("instance_runner",
        "Viewer GFX dirty fallback defaults: enabled=%s codec=%s "
        "rfx_threading=%s max_in_flight_frames=%u max_in_flight_bytes=%u "
        "diagnostic_full_frame_dirty=%s max_pending_rects=%u "
        "area_fallback_percent=%u consecutive_defer_fallback_count=%u",
        bool_str(inst->viewer_gfx_enabled),
        instance_viewer_gfx_codec_name(inst->viewer_gfx_codec),
        bool_str(inst->viewer_gfx_rfx_threading_enabled),
        inst->viewer_gfx_dirty_max_in_flight_frames,
        inst->viewer_gfx_dirty_max_in_flight_bytes,
        bool_str(inst->viewer_gfx_diagnostic_full_frame_dirty), 128U, 60U, 3U);

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

  HANDLE server_tid =
      CreateThread(NULL, 0, instance_server_thread, server, 0, NULL);
  if (!server_tid) {
    LOG_E("instance_runner", "Failed to create viewer server thread");
    viewer_server_free(server);
    backend_disconnect(client);
    backend_free(client);
    svc_config_free(config);
    return 1;
  }

  LOG_I("instance_runner", "Viewer server started on %s:%u",
        inst->viewer_bind_address, inst->viewer_port);

  /* Start heartbeat thread */
  HANDLE hHeartbeat = CreateThread(NULL, 0, heartbeat_thread,
                                   (LPVOID)args.instance_name, 0, NULL);
  if (!hHeartbeat) {
    LOG_W("instance_runner", "Failed to create heartbeat thread");
  }

  printf("Press Ctrl+C to disconnect\n\n");
  platform_sleep_ms(2000);

  /* Main event loop — same pattern as standalone main.c */
  time_t last_stats = time(NULL);
  UINT64 last_tile_count = 0;
  UINT64 last_bitmap_batch_count = 0;
  UINT64 last_surface_bits_decode_count = 0;
  PlatformProcessCpuSample last_cpu_sample = {0};

  (void)platform_get_process_cpu_sample(&last_cpu_sample);

  while (g_running && backend_is_connected(client)) {
    if (!backend_iterate(client)) {
      puts("Connection lost");
      break;
    }

    time_t now = time(NULL);
    if (now - last_stats >= 2) {
      const double seconds = difftime(now, last_stats);
      const UINT64 tile_count = client->forwarded_surface_bits_count;
      const UINT64 total_bytes = client->forwarded_surface_bits_bytes;
      const UINT64 frame_markers = client->forwarded_frame_marker_count;
      const UINT64 bitmap_batches = client->bitmap_update_batches_total;
      const UINT64 bitmap_rectangles = client->bitmap_update_rectangles_total;
      const UINT64 bitmap_bytes = client->bitmap_update_payload_bytes_total;
      const UINT64 surface_decodes = client->surface_bits_decode_count;
      const UINT64 surface_decode_failures =
          client->surface_bits_decode_failure_count;
      const UINT64 surface_decode_bytes =
          client->surface_bits_payload_bytes_total;
      PlatformProcessCpuSample cpu_sample = {0};
      double process_cpu_percent = 0.0;
      const double avg_rects_per_batch =
          (bitmap_batches > 0)
              ? ((double)bitmap_rectangles / (double)bitmap_batches)
              : 0.0;
      const double avg_callback_us =
          (bitmap_batches > 0)
              ? ((double)client->bitmap_update_callback_time_total_us /
                 (double)bitmap_batches)
              : 0.0;
      const double avg_publish_us =
          (bitmap_batches > 0)
              ? ((double)client->bitmap_update_publish_time_total_us /
                 (double)bitmap_batches)
              : 0.0;
      const double fps =
          (seconds > 0.0) ? ((double)(tile_count - last_tile_count) / seconds)
                          : 0.0;
      const double avg_surface_decode_us =
          (surface_decodes > 0)
              ? ((double)client->surface_bits_decode_time_total_us /
                 (double)surface_decodes)
              : 0.0;

      if (platform_get_process_cpu_sample(&cpu_sample)) {
        process_cpu_percent =
            platform_process_cpu_percent(&last_cpu_sample, &cpu_sample);
        last_cpu_sample = cpu_sample;
      }

      if ((tile_count != last_tile_count) ||
          (bitmap_batches != last_bitmap_batch_count) ||
          (surface_decodes != last_surface_bits_decode_count)) {
        LOG_I("instance_runner",
              "Perf stats: tiles=%" PRIu64 " bytes=%" PRIu64 " markers=%" PRIu64
              " rate=%.1f updates/s"
              " bitmap_batches=%" PRIu64 " bitmap_rects=%" PRIu64
              " bitmap_bytes=%" PRIu64 " avg_rect_batch=%.2f"
              " avg_bitmap_callback_us=%.1f avg_bitmap_publish_us=%.1f"
              " max_bitmap_callback_us=%" PRIu64
              " max_bitmap_publish_us=%" PRIu64 " surface_decodes=%" PRIu64
              " surface_decode_failures=%" PRIu64
              " surface_coded_bytes=%" PRIu64
              " avg_surface_decode_us=%.1f max_surface_decode_us=%" PRIu64
              " process_cpu_percent=%.1f",
              tile_count, total_bytes, frame_markers, fps, bitmap_batches,
              bitmap_rectangles, bitmap_bytes, avg_rects_per_batch,
              avg_callback_us, avg_publish_us,
              client->bitmap_update_callback_time_max_us,
              client->bitmap_update_publish_time_max_us, surface_decodes,
              surface_decode_failures, surface_decode_bytes,
              avg_surface_decode_us, client->surface_bits_decode_time_max_us,
              process_cpu_percent);
        last_tile_count = tile_count;
        last_bitmap_batch_count = bitmap_batches;
        last_surface_bits_decode_count = surface_decodes;
      } else {
        LOG_I("instance_runner",
              "Perf stats: no new forwarded updates process_cpu_percent=%.1f",
              process_cpu_percent);
      }

      last_stats = now;
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

  if (g_viewer_logfile) {
    fclose(g_viewer_logfile);
    g_viewer_logfile = NULL;
  }

  printf("Instance '%s' done.\n", args.instance_name);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
