/**
 * @file svc_service.c
 * @brief OmniRDP service lifecycle implementation
 *
 * Implements service install/uninstall, the SCM-controlled main
 * service loop (svc_service_start), and a console-mode equivalent
 * for debugging (svc_service_run_console).
 *
 * Windows-only; no FreeRDP dependency.
 */

#include "svc_service.h"
#include "svc_config.h"
#include "svc_instance_mgr.h"
#include "svc_log.h"
#include "svc_pipe_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Forward declarations ──────────────────────────────────────── */

static void WINAPI service_ctrl_handler(DWORD dwControl);
static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType);

/* ── Module-global state ───────────────────────────────────────── */

/*
 * Points to the active OmniRDPSvcContext so that the SCM control
 * handler and the console control handler can signal the main loop.
 * Only one service instance can be active at a time (which is the
 * normal behaviour for a service process).
 */
static OmniRDPSvcContext *g_ctx = NULL;

static int svc_copy_string(char *dest, size_t dest_size, const char *src) {
  int ret;
  if (!dest || dest_size == 0 || !src) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return -1;
  }
  ret = snprintf(dest, dest_size, "%s", src);
  if (ret < 0 || (size_t)ret >= dest_size) {
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return -1;
  }
  return 0;
}

/* ── Helpers ───────────────────────────────────────────────────── */

/**
 * @brief Map a log level string to a SvcLogLevel enum value.
 *
 * Accepted values (case-insensitive): "debug", "info", "warn", "error".
 * Returns SVC_LOG_INFO for unrecognised strings.
 */
static SvcLogLevel parse_log_level(const char *str) {
  if (!str)
    return SVC_LOG_INFO;
  if (_stricmp(str, "debug") == 0)
    return SVC_LOG_DEBUG;
  if (_stricmp(str, "info") == 0)
    return SVC_LOG_INFO;
  if (_stricmp(str, "warn") == 0)
    return SVC_LOG_WARN;
  if (_stricmp(str, "error") == 0)
    return SVC_LOG_ERROR;
  return SVC_LOG_INFO;
}

/**
 * @brief Build the full path to a binary located in the same directory
 *        as the current executable.
 *
 * @param binary_name  File name (e.g. "OmniRDP.exe")
 * @param out          Output buffer for the full path
 * @param out_size     Size of the output buffer
 * @return 0 on success, -1 on failure
 */
static int get_binary_path(const char *binary_name, char *out,
                           size_t out_size) {
  char module_path[MAX_PATH];
  DWORD len = GetModuleFileNameA(NULL, module_path, sizeof(module_path));
  if (len == 0 || len >= sizeof(module_path))
    return -1;

  /* Find the last backslash and truncate to the directory portion */
  char *last_slash = strrchr(module_path, '\\');
  if (!last_slash)
    return -1;

  *(last_slash + 1) = '\0'; /* keep the trailing backslash */

  int ret = _snprintf(out, out_size, "%s%s", module_path, binary_name);
  if (ret < 0 || (size_t)ret >= out_size)
    return -1;

  return 0;
}

/**
 * @brief Build the service SID account name used in filesystem ACLs.
 */
static int build_service_sid_name(const char *serviceName, char *out,
                                  size_t out_size) {
  int ret;

  if (!serviceName || serviceName[0] == '\0' || !out || out_size == 0) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return -1;
  }

  ret = _snprintf(out, out_size, "NT SERVICE\\%s", serviceName);
  if (ret < 0 || (size_t)ret >= out_size) {
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return -1;
  }

  return 0;
}

/**
 * @brief Set ACL on the ProgramData config directory or config file.
 *
 *
 * Grants:
 * - SYSTEM: Full Control
 * - BUILTIN\Administrators: Full Control

 * * - NT SERVICE\<serviceName>: Modify
 */
static int set_config_acl(const char *path, const char *serviceName,
                          BOOL isDirectory) {
  DWORD dwRes;
  EXPLICIT_ACCESSA ea[3];
  PACL pACL = NULL;
  char serviceSidName[300];

  /* SYSTEM SID */
  SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
  PSID pSystemSid = NULL;
  if (!AllocateAndInitializeSid(&ntAuth, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0,
                                0, 0, 0, 0, &pSystemSid)) {
    return -1;
  }

  /* Administrators SID */
  PSID pAdminSid = NULL;
  if (!AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                &pAdminSid)) {
    FreeSid(pSystemSid);
    return -1;
  }

  if (build_service_sid_name(serviceName, serviceSidName,
                             sizeof(serviceSidName)) != 0) {
    if (pSystemSid)
      FreeSid(pSystemSid);
    if (pAdminSid)
      FreeSid(pAdminSid);
    return -1;
  }

  /* Build explicit access array */
  ZeroMemory(ea, sizeof(ea));

  /* SYSTEM: Full Control */
  ea[0].grfAccessPermissions = GENERIC_ALL;
  ea[0].grfAccessMode = SET_ACCESS;
  ea[0].grfInheritance = isDirectory
                             ? (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)
                             : NO_INHERITANCE;
  ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
  ea[0].Trustee.ptstrName = (LPSTR)pSystemSid;

  /* Administrators: Full Control */
  ea[1].grfAccessPermissions = GENERIC_ALL;
  ea[1].grfAccessMode = SET_ACCESS;
  ea[1].grfInheritance = isDirectory
                             ? (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)
                             : NO_INHERITANCE;
  ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
  ea[1].Trustee.ptstrName = (LPSTR)pAdminSid;

  /* Service SID: Modify */
  ea[2].grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE;
  if (isDirectory)
    ea[2].grfAccessPermissions |= FILE_DELETE_CHILD;
  ea[2].grfAccessMode = SET_ACCESS;
  ea[2].grfInheritance = isDirectory
                             ? (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)
                             : NO_INHERITANCE;
  ea[2].Trustee.TrusteeForm = TRUSTEE_IS_NAME;
  ea[2].Trustee.TrusteeType = TRUSTEE_IS_USER;
  ea[2].Trustee.ptstrName = serviceSidName;

  /* Create ACL */
  dwRes = SetEntriesInAclA(3, ea, NULL, &pACL);
  if (dwRes != ERROR_SUCCESS) {
    if (pSystemSid)
      FreeSid(pSystemSid);
    if (pAdminSid)
      FreeSid(pAdminSid);
    SetLastError(dwRes);
    return -1;
  }

  /* Apply protected ACL, replacing inherited broad Users/Auth Users access. */
  dwRes = SetNamedSecurityInfoA((LPSTR)path, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION |
                                    PROTECTED_DACL_SECURITY_INFORMATION,
                                NULL, NULL, pACL, NULL);

  /* Cleanup */
  if (pACL)
    LocalFree(pACL);
  if (pSystemSid)
    FreeSid(pSystemSid);
  if (pAdminSid)
    FreeSid(pAdminSid);

  if (dwRes != ERROR_SUCCESS) {
    SetLastError(dwRes);
    return -1;
  }

  return 0;
}

static int set_config_file_acl(const char *filePath, const char *serviceName) {
  return set_config_acl(filePath, serviceName, FALSE);
}

static int set_config_dir_acl(const char *dirPath, const char *serviceName) {
  return set_config_acl(dirPath, serviceName, TRUE);
}

static int prepare_config_path_acl(const char *configPath,
                                   const char *serviceName) {
  char dirPath[MAX_PATH];
  char *lastSlash;
  int dirRes;

  if (!configPath || configPath[0] == '\0') {
    SetLastError(ERROR_INVALID_PARAMETER);
    return -1;
  }

  if (svc_copy_string(dirPath, sizeof(dirPath), configPath) != 0)
    return -1;
  lastSlash = strrchr(dirPath, '\\');
  if (!lastSlash) {
    SetLastError(ERROR_INVALID_PARAMETER);
    return -1;
  }

  *lastSlash = '\0';
  dirRes = SHCreateDirectoryExA(NULL, dirPath, NULL);
  if (dirRes != ERROR_SUCCESS && dirRes != ERROR_ALREADY_EXISTS &&
      dirRes != ERROR_FILE_EXISTS) {
    SetLastError((DWORD)dirRes);
    return -1;
  }

  return set_config_dir_acl(dirPath, serviceName);
}

/* ── svc_config_write_template ───────────────────────────────────── */

int svc_config_write_template(const char *configPath) {
  if (!configPath || configPath[0] == '\0') {
    SetLastError(ERROR_INVALID_PARAMETER);
    return -1;
  }

  /* Create directory structure */
  char dirPath[MAX_PATH];
  if (svc_copy_string(dirPath, sizeof(dirPath), configPath) != 0)
    return -1;
  char *lastSlash = strrchr(dirPath, '\\');
  if (lastSlash) {
    *lastSlash = '\0';
    /* Create directory recursively */
    SHCreateDirectoryExA(NULL, dirPath, NULL);
  }

  FILE *fp = NULL;
  fopen_s(&fp, configPath, "w");
  if (!fp) {
    return -1;
  }

  fprintf(fp,
          "; ============================================================\n"
          "; OmniRDP Service Configuration\n"
          "; Location: %s\n"
          ";\n"
          "; This file was auto-generated on first install.\n"
          "; Edit the [instance:Example] section below with your\n"
          "; backend RDP server details, then restart the service.\n"
          ";\n"
          "; Plaintext passwords are automatically encrypted with DPAPI\n"
          "; on first start. You can also use dpapi:<base64> values.\n"
          "; ============================================================\n"
          "\n"
          "[service]\n"
          "; Log level: debug, info, warn, error\n"
          "log_level = info\n"
          "\n"
          "; Log directory (per-service subdirectory is auto-appended)\n"
          "log_dir = C:\\ProgramData\\OmniRDP\\logs\n"
          "\n"
          "; Log rotation\n"
          "log_max_size_mb = 10\n"
          "log_max_files = 5\n"
          "\n"
          "; Named pipe for tray app IPC. Custom values are reserved/no-op in "
          "this build.\n"
          "pipe_name = OmniRDP_ServicePipe\n"
          "\n"
          "; Health monitoring\n"
          "heartbeat_timeout_sec = 10\n"
          "graceful_shutdown_sec = 10\n"
          "health_poll_interval_sec = 2\n"
          "instance_startup_delay_ms = 500\n"
          "\n"
          "[instances]\n"
          "; Comma-separated list of instance names.\n"
          "; Each name must have a matching [instance:<name>] section.\n"
          "; Leave empty to run idle with no instances.\n"
          "names = Example\n"
          "\n"
          "; ============================================================\n"
          "; Instance: Example\n"
          "; Edit these values with your backend RDP server details.\n"
          "; ============================================================\n"
          "[instance:Example]\n"
          "enabled = true\n"
          "\n"
          "; Backend VM connection (OmniRDP -> real backend VM)\n"
          "backend.hostname = 192.168.1.100\n"
          "backend.port = 3389\n"
          "backend.username = Administrator\n"
          "backend.password = changeme\n"
          "backend.domain =\n"
          "backend.connect_timeout_ms = 30000\n"
          "; Backend security (OmniRDP -> backend VM)\n"
          "backend.security.nla_enabled = true\n"
          "backend.security.tls_enabled = true\n"
          "backend.security.rdp_enabled = true\n"
          "backend.security.server_authentication = true\n"
          "backend.security.ignore_certificate = false\n"
          "\n"
          "; Reconnect policy\n"
          "; Viewer listener (clients connect here)\n"
          "viewer.bind_address = 127.0.0.1\n"
          "viewer.port = 3390\n"
          "; Reserved: runtime maximum is currently compiled in as 10\n"
          "viewer.max_viewers = 10\n"
          "viewer.cert_path =\n"
          "viewer.key_path =\n"
          "; Applied: disconnect viewers that remain severely lagged\n"
          "viewer.slow_disconnect_enabled = true\n"
          "; Reserved/no-op in this build\n"
          "viewer.slow_lag_interval_ms = 5000\n"
          "viewer.slow_disconnect_after_ms = 30000\n"
          "; Reserved/no-op in this build\n"
          "viewer.late_join_timeout_ms = 15000\n"
          "viewer.late_join_refresh_deadline_ms = 5000\n"
          "viewer.late_join_replay_max_frames = 4\n"
          "; Reserved/no-op in this build\n"
          "viewer.throttle_max_updates_per_sec = 0\n"
          "; Viewer security/auth (clients -> OmniRDP)\n"
          "viewer.security.nla_enabled = true\n"
          "viewer.security.tls_enabled = true\n"
          "viewer.security.rdp_enabled = true\n"
          "viewer.auth.mode = backend_credentials\n"
          "\n"
          "; Display (monitor_count is applied; width/height/depth are "
          "reserved/no-op)\n"
          "display.monitor_count = 1\n"
          "display.monitor_width = 1920\n"
          "display.monitor_height = 1080\n"
          "display.color_depth = 32\n"
          "\n"
          "; Codecs (reserved/no-op in this build)\n"
          "codec.nscodec = true\n"
          "codec.remote_fx = true\n"
          "codec.graphics_pipeline = false\n"
          "codec.h264 = false\n"
          "codec.avc444 = false\n"
          "codec.avc444v2 = false\n"
          "codec.frame_acknowledge = 4\n"
          "\n"
          "\n"
          "; Legacy compatibility security keys. Prefer backend.security.* for "
          "backend VM connection settings.\n"
          "; Retained for older configs and may populate backend security if "
          "backend.security.* is absent.\n"
          "security.tls_enabled = true\n"
          "security.nla_enabled = true\n"
          "; Reserved/no-op in this build\n"
          "security.tls_min_version = 1.2\n"
          "security.server_authentication = true\n"
          "security.ignore_certificate = false\n",
          configPath);

  fclose(fp);

  return 0;
}

/* ── Service Control Handler ───────────────────────────────────── */

/**
 * @brief SCM control handler (registered via RegisterServiceCtrlHandlerA).
 *
 * Dispatches control codes from the Service Control Manager.
 * The module-global g_ctx must point to a valid OmniRDPSvcContext
 * when this callback is invoked.
 */
static void WINAPI service_ctrl_handler(DWORD dwControl) {
  if (!g_ctx)
    return;

  switch (dwControl) {
  case SERVICE_CONTROL_STOP:
    LOG_I("ServiceCtrlHandler", "Received SERVICE_CONTROL_STOP");
    g_ctx->shuttingDown = TRUE;
    g_ctx->status.dwCurrentState = SERVICE_STOP_PENDING;
    g_ctx->status.dwWaitHint = 30000; /* 30 seconds */
    SetServiceStatus(g_ctx->statusHandle, &g_ctx->status);
    SetEvent(g_ctx->hStopEvent);
    break;

  case SERVICE_CONTROL_INTERROGATE:
    SetServiceStatus(g_ctx->statusHandle, &g_ctx->status);
    break;

  /*
   * SERVICE_CONTROL_PARAMCHANGE is reserved for future use
   * (hot-reload configuration at runtime).
   */
  default:
    break;
  }
}

/* ── Console Control Handler ───────────────────────────────────── */

/**
 * @brief Console control handler (registered via SetConsoleCtrlHandler).
 *
 * Translates Ctrl+C / Ctrl+Break into a stop-event signal so that
 * the console main loop exits cleanly.
 */
static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType) {
  if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
    if (g_ctx) {
      LOG_I("ConsoleCtrlHandler", "Received Ctrl+C / Ctrl+Break");
      g_ctx->shuttingDown = TRUE;
      SetEvent(g_ctx->hStopEvent);
    }
    return TRUE;
  }
  return FALSE;
}

/* ── svc_service_install ───────────────────────────────────────── */

int svc_service_install(const char *serviceName, const char *configPath) {
  if (!serviceName || serviceName[0] == '\0') {
    fprintf(stderr, "svc_service_install: serviceName is required\n");
    return -1;
  }

  SC_HANDLE schSCManager =
      OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
  if (!schSCManager) {
    fprintf(stderr, "OpenSCManager failed: %lu\n", GetLastError());
    return -1;
  }

  /* Get the current executable's full path */
  char modulePath[MAX_PATH];
  DWORD len = GetModuleFileNameA(NULL, modulePath, sizeof(modulePath));
  if (len == 0 || len >= sizeof(modulePath)) {
    fprintf(stderr, "GetModuleFileName failed: %lu\n", GetLastError());
    CloseServiceHandle(schSCManager);
    return -1;
  }

  /*
   * Build the service binary command line:
   *   "<exePath>" --service --service-name "<svcName>" [--config
   * "<configPath>"]
   *
   * Including --service-name ensures the SCM restarts the service
   * with the correct custom name rather than falling back to "OmniRDP".
   */
  char binaryPath[2048];
  int ret = _snprintf(binaryPath, sizeof(binaryPath),
                      "\"%s\" --service --service-name \"%s\"", modulePath,
                      serviceName);
  if (ret < 0 || (size_t)ret >= sizeof(binaryPath)) {
    fprintf(stderr, "Binary path too long\n");
    CloseServiceHandle(schSCManager);
    return -1;
  }

  if (configPath && configPath[0] != '\0') {
    size_t existing = strnlen_s(binaryPath, sizeof(binaryPath));
    if (existing >= sizeof(binaryPath)) {
      fprintf(stderr, "Binary path too long\n");
      CloseServiceHandle(schSCManager);
      return -1;
    }
    ret = _snprintf(binaryPath + existing, sizeof(binaryPath) - existing,
                    " --config \"%s\"", configPath);
    if (ret < 0 || existing + (size_t)ret >= sizeof(binaryPath)) {
      fprintf(stderr, "Binary path with config too long\n");
      CloseServiceHandle(schSCManager);
      return -1;
    }
  }

  SC_HANDLE schService = CreateServiceA(
      schSCManager, /* SCM database */
      serviceName,  /* service name */
      serviceName,  /* display name */
      SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS |
          SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG, /* desired access */
      SERVICE_WIN32_OWN_PROCESS,                        /* service type */
      SERVICE_AUTO_START,                               /* start type */
      SERVICE_ERROR_NORMAL,                             /* error control */
      binaryPath,                                       /* binary path */
      NULL,                                             /* load order group */
      NULL,                                             /* tag identifier */
      NULL,                                             /* dependencies */
      "NT AUTHORITY\\NetworkService", /* service start account */
      NULL);                          /* password */

  if (!schService) {
    DWORD err = GetLastError();

    if (err == ERROR_SERVICE_MARKED_FOR_DELETE) {
      /* Service is pending deletion — wait for it to complete */
      fprintf(stderr, "Service '%s' is pending deletion; waiting...\n",
              serviceName);
      CloseServiceHandle(schSCManager);
      for (int i = 0; i < 30; i++) {
        Sleep(1000);
        schSCManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
        if (!schSCManager)
          break;
        schService = CreateServiceA(
            schSCManager, serviceName, serviceName,
            SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS |
                SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            binaryPath, NULL, NULL, NULL, "NT AUTHORITY\\NetworkService", NULL);
        if (schService)
          break;
        err = GetLastError();
        CloseServiceHandle(schSCManager);
        if (err != ERROR_SERVICE_MARKED_FOR_DELETE)
          break;
      }
    } else if (err == ERROR_SERVICE_EXISTS) {
      /* Service already exists — uninstall and retry */
      fprintf(stderr, "Service '%s' already exists; recreating...\n",
              serviceName);
      CloseServiceHandle(schSCManager);
      svc_service_uninstall(serviceName);
      schSCManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
      if (schSCManager) {
        schService = CreateServiceA(
            schSCManager, serviceName, serviceName,
            SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS |
                SERVICE_QUERY_CONFIG | SERVICE_CHANGE_CONFIG,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            binaryPath, NULL, NULL, NULL, "NT AUTHORITY\\NetworkService", NULL);
      }
    }

    if (!schService) {
      fprintf(stderr, "CreateService failed: %lu\n", GetLastError());
      if (schSCManager)
        CloseServiceHandle(schSCManager);
      return -1;
    }
  }

  /* Set a human-readable description */
  SERVICE_DESCRIPTIONA desc;
  desc.lpDescription = "OmniRDP RDP Multiplexer Service";
  ChangeServiceConfig2A(schService, SERVICE_CONFIG_DESCRIPTION, &desc);

  /* Enable an unrestricted per-service SID while keeping NetworkService. */
  {
    SERVICE_SID_INFO sidInfo;
    sidInfo.dwServiceSidType = SERVICE_SID_TYPE_UNRESTRICTED;
    if (!ChangeServiceConfig2A(schService, SERVICE_CONFIG_SERVICE_SID_INFO,
                               &sidInfo)) {
      fprintf(stderr, "ChangeServiceConfig2(SERVICE_SID_INFO) failed: %lu\n",
              GetLastError());
      CloseServiceHandle(schService);
      CloseServiceHandle(schSCManager);
      return -1;
    }
  }

  /* Generate/repair ProgramData config and ACLs before starting service. */
  if (configPath && configPath[0] != '\0') {
    DWORD attr = GetFileAttributesA(configPath);
    if (prepare_config_path_acl(configPath, serviceName) != 0) {
      fprintf(stderr, "Failed to set config directory ACL: %lu\n",
              GetLastError());
      CloseServiceHandle(schService);
      CloseServiceHandle(schSCManager);
      return -1;
    }

    if (attr == INVALID_FILE_ATTRIBUTES) {
      LOG_I("svc_service", "Config file not found, generating template at '%s'",
            configPath);
      if (svc_config_write_template(configPath) == 0) {
        LOG_I(
            "svc_service",
            "Template config.ini created. Edit it with your backend details.");
      } else {
        fprintf(stderr, "Failed to create template config.ini: %lu\n",
                GetLastError());
        CloseServiceHandle(schService);
        CloseServiceHandle(schSCManager);
        return -1;
      }
    }

    if (GetFileAttributesA(configPath) == INVALID_FILE_ATTRIBUTES) {
      fprintf(stderr, "Config file is missing after template setup: %lu\n",
              GetLastError());
      CloseServiceHandle(schService);
      CloseServiceHandle(schSCManager);
      return -1;
    }

    if (set_config_file_acl(configPath, serviceName) != 0) {
      fprintf(stderr, "Failed to set config file ACL: %lu\n", GetLastError());
      CloseServiceHandle(schService);
      CloseServiceHandle(schSCManager);
      return -1;
    }
  }

  /* Start the service */
  if (!StartServiceA(schService, 0, NULL)) {
    DWORD err = GetLastError();
    if (err == ERROR_SERVICE_ALREADY_RUNNING) {
      printf("Service '%s' is already running.\n", serviceName);
    } else {
      fprintf(stderr,
              "Warning: Failed to start service '%s' (error %lu). "
              "You may need to start it manually: sc start \"%s\"\n",
              serviceName, err, serviceName);
    }
  } else {
    printf("Service '%s' started successfully.\n", serviceName);
  }

  CloseServiceHandle(schService);
  CloseServiceHandle(schSCManager);

  return 0;
}

/* ── svc_service_uninstall ─────────────────────────────────────── */

int svc_service_uninstall(const char *serviceName) {
  if (!serviceName || serviceName[0] == '\0') {
    fprintf(stderr, "svc_service_uninstall: serviceName is required\n");
    return -1;
  }

  SC_HANDLE schSCManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
  if (!schSCManager) {
    fprintf(stderr, "OpenSCManager failed: %lu\n", GetLastError());
    return -1;
  }

  SC_HANDLE schService = OpenServiceA(
      schSCManager, serviceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
  if (!schService) {
    DWORD err = GetLastError();
    if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
      fprintf(stderr, "Service '%s' does not exist — nothing to uninstall.\n",
              serviceName);
      CloseServiceHandle(schSCManager);
      return 0;
    }
    fprintf(stderr, "OpenService failed: %lu\n", err);
    CloseServiceHandle(schSCManager);
    return -1;
  }

  /* Try to stop the service if it is running */
  SERVICE_STATUS status;
  BOOL stopped = FALSE;

  if (QueryServiceStatus(schService, &status)) {
    if (status.dwCurrentState == SERVICE_STOPPED) {
      stopped = TRUE;
    } else if (status.dwCurrentState != SERVICE_STOP_PENDING) {
      ControlService(schService, SERVICE_CONTROL_STOP, &status);
    }

    /* Poll for SERVICE_STOPPED */
    if (!stopped) {
      for (int i = 0; i < 30; i++) {
        if (!QueryServiceStatus(schService, &status))
          break;
        if (status.dwCurrentState == SERVICE_STOPPED) {
          stopped = TRUE;
          break;
        }
        Sleep(1000);
      }
    }
  }

  if (!stopped) {
    fprintf(
        stderr,
        "Service '%s' did not stop within 30 seconds. Cannot safely delete.\n",
        serviceName);
    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return -1;
  }

  /* Delete the service */
  if (!DeleteService(schService)) {
    fprintf(stderr, "DeleteService failed: %lu\n", GetLastError());
    CloseServiceHandle(schService);
    CloseServiceHandle(schSCManager);
    return -1;
  }

  CloseServiceHandle(schService);
  schService = NULL;

  /* Wait for deletion to take effect */
  for (int i = 0; i < 15; i++) {
    Sleep(500);
    schService = OpenServiceA(schSCManager, serviceName, SERVICE_QUERY_STATUS);
    if (!schService) {
      if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
        break;
    } else {
      CloseServiceHandle(schService);
      schService = NULL;
    }
  }

  CloseServiceHandle(schSCManager);

  printf("Service '%s' uninstalled successfully.\n", serviceName);
  return 0;
}

/* ── svc_service_start ─────────────────────────────────────────── */

int svc_service_start(const char *serviceName, const char *configPath) {
  OmniRDPSvcContext ctx;
  BOOL mgrInitialized = FALSE;
  BOOL pipeInitialized = FALSE;
  memset(&ctx, 0, sizeof(ctx));
  g_ctx = &ctx;

  /* ── Determine service name ─────────────────────────────── */
  if (serviceName && serviceName[0] != '\0') {
    if (svc_copy_string(ctx.serviceName, sizeof(ctx.serviceName),
                        serviceName) != 0) {
      g_ctx = NULL;
      return -1;
    }
  } else {
    svc_copy_string(ctx.serviceName, sizeof(ctx.serviceName), "OmniRDP");
  }
  ctx.shuttingDown = FALSE;

  /* ── 1. Register the service control handler ────────────── */
  ctx.statusHandle =
      RegisterServiceCtrlHandlerA(ctx.serviceName, service_ctrl_handler);
  if (!ctx.statusHandle) {
    /* Cannot log yet; fall back to debug output */
    OutputDebugStringA("[svc_service] RegisterServiceCtrlHandlerA failed\n");
    g_ctx = NULL;
    return -1;
  }

  /* ── 2. Report SERVICE_START_PENDING ────────────────────── */
  ctx.status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  ctx.status.dwCurrentState = SERVICE_START_PENDING;
  ctx.status.dwControlsAccepted = SERVICE_ACCEPT_STOP;
  ctx.status.dwWin32ExitCode = NO_ERROR;
  ctx.status.dwServiceSpecificExitCode = 0;
  ctx.status.dwCheckPoint = 0;
  ctx.status.dwWaitHint = 10000; /* 10 seconds */
  SetServiceStatus(ctx.statusHandle, &ctx.status);

  /* ── 3. Create the stop event ───────────────────────────── */
  ctx.hStopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (!ctx.hStopEvent) {
    OutputDebugStringA("[svc_service] CreateEvent failed\n");
    g_ctx = NULL;
    return -1;
  }

  /* ── 4. Load configuration ──────────────────────────────── */
  if (!configPath || configPath[0] == '\0')
    configPath = "C:\\ProgramData\\OmniRDP\\config.ini";

  if (svc_copy_string(ctx.configPath, sizeof(ctx.configPath), configPath) !=
      0) {
    CloseHandle(ctx.hStopEvent);
    g_ctx = NULL;
    return -1;
  }

  ctx.config = svc_config_load(configPath);
  if (!ctx.config) {
    LOG_W("svc_service",
          "Config file '%s' not found or unreadable. "
          "Running with defaults (no instances). "
          "Edit config.ini and reload, or use the tray app to add instances.",
          configPath);
    ctx.config = svc_config_create_default();
    if (!ctx.config) {
      LOG_E("svc_service", "Failed to create default config");
    }
  }

  SvcServiceConfig *svcCfg = ctx.config ? &ctx.config->service : NULL;

  /* ── 5. Initialize logging (per-service log directory) ──── */
  {
    char log_dir[MAX_PATH];
    SvcLogLevel log_level = SVC_LOG_INFO;
    unsigned int max_size_mb = 10;
    unsigned int max_files = 5;

    if (svcCfg && svcCfg->log_dir[0] != '\0') {
      /* Use configured log dir, but append service name for per-service
       * isolation */
      if (snprintf(log_dir, sizeof(log_dir), "%s\\%s", svcCfg->log_dir,
                   ctx.serviceName) < 0 ||
          strnlen_s(log_dir, sizeof(log_dir)) >= sizeof(log_dir) - 1)
        log_dir[0] = '\0';
    } else {
      if (snprintf(log_dir, sizeof(log_dir),
                   "C:\\ProgramData\\OmniRDP\\logs\\%s", ctx.serviceName) < 0 ||
          strnlen_s(log_dir, sizeof(log_dir)) >= sizeof(log_dir) - 1)
        log_dir[0] = '\0';
    }

    if (svcCfg) {
      log_level = parse_log_level(svcCfg->log_level);
      max_size_mb = svcCfg->log_max_size_mb;
      max_files = svcCfg->log_max_files;
    }

    svc_log_init(log_dir, log_level, max_size_mb, max_files);
  }

  LOG_I("svc_service_start",
        "OmniRDP service starting (serviceName=%s, configPath=%s)",
        ctx.serviceName, ctx.configPath);

  /* ── 6. Find the OmniRDP.exe path ───────────────────────── */
  if (get_binary_path("OmniRDP.exe", ctx.exePath, sizeof(ctx.exePath)) != 0) {
    LOG_W("svc_service_start",
          "Failed to locate OmniRDP.exe; using fallback name");
    svc_copy_string(ctx.exePath, sizeof(ctx.exePath), "OmniRDP.exe");
  }

  /* ── 7. Initialize the instance manager ─────────────────── */
  if (inst_mgr_init(&ctx.mgr, ctx.config, ctx.configPath, ctx.exePath) != 0) {
    LOG_E("svc_service_start", "inst_mgr_init failed");
    SetEvent(ctx.hStopEvent);
    /* Fall through to the shutdown path */
  } else {
    mgrInitialized = TRUE;
    /* ── 8. Report SERVICE_RUNNING ───────────────────────── */
    ctx.status.dwCurrentState = SERVICE_RUNNING;
    ctx.status.dwCheckPoint = 0;
    ctx.status.dwWaitHint = 0;
    SetServiceStatus(ctx.statusHandle, &ctx.status);

    /* ── Start all enabled instances ────────────────────── */
    inst_mgr_start_all(&ctx.mgr);

    LOG_I("svc_service_start", "Service is running with %u instances",
          ctx.config ? ctx.config->instance_count : 0);
  }

  /* Initialize pipe server for tray app communication */
  if (mgrInitialized) {
    char pipeName[256];
    if (snprintf(pipeName, sizeof(pipeName), "%s_Pipe", ctx.serviceName) < 0 ||
        strnlen_s(pipeName, sizeof(pipeName)) >= sizeof(pipeName))
      pipeName[0] = '\0';
    if (pipeName[0] == '\0') {
      LOG_E("svc_service_start", "Pipe name construction failed");
    } else {
      /* Replace hyphens with underscores for Windows pipe name compatibility */
      for (char *p = pipeName; *p; p++) {
        if (*p == '-')
          *p = '_';
      }
      if (pipe_server_init(&ctx.pipeServer, pipeName, &ctx.mgr,
                            ctx.serviceName) != 0) {
        LOG_E("svc_service_start", "Failed to initialize pipe server");
        /* Continue anyway — pipe server is not critical for operation */
      } else {
        pipeInitialized = TRUE;
      }
    }
  }

  /* ── 9. Main service loop ───────────────────────────────── */
  {
    unsigned int intervalSec = 2; /* default */
    if (svcCfg && svcCfg->health_poll_interval_sec > 0)
      intervalSec = svcCfg->health_poll_interval_sec;
    DWORD pollIntervalMs = intervalSec * 1000;

    while (!ctx.shuttingDown) {
      DWORD wr = WaitForSingleObject(ctx.hStopEvent, pollIntervalMs);
      if (wr == WAIT_OBJECT_0)
        break; /* stop event was signaled */

      /* Poll instance health and reconnection logic */
      if (mgrInitialized)
        inst_mgr_poll(&ctx.mgr);

      /* Push stats to connected tray apps every 5 seconds */
      {
        static ULONGLONG lastStatsPushMs = 0;
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - lastStatsPushMs >= 5000) {
          if (pipeInitialized)
            pipe_server_push_stats(&ctx.pipeServer);
          lastStatsPushMs = nowMs;
        }
      }
    }
  }

  /* ── 10. Shutdown sequence ──────────────────────────────── */
  LOG_I("svc_service_start", "Service shutting down");

  ctx.status.dwCurrentState = SERVICE_STOP_PENDING;
  ctx.status.dwCheckPoint = 0;
  ctx.status.dwWaitHint = (svcCfg && svcCfg->graceful_shutdown_sec > 0)
                              ? svcCfg->graceful_shutdown_sec * 1000
                              : 10000;
  SetServiceStatus(ctx.statusHandle, &ctx.status);

  if (pipeInitialized) {
    pipe_server_stop(&ctx.pipeServer);
    pipeInitialized = FALSE;
  }

  if (mgrInitialized) {
    inst_mgr_stop_all(&ctx.mgr);
    inst_mgr_cleanup(&ctx.mgr);
    mgrInitialized = FALSE;
  }

  if (ctx.config) {
    svc_config_free(ctx.config);
    ctx.config = NULL;
  }

  svc_log_shutdown();

  CloseHandle(ctx.hStopEvent);
  ctx.hStopEvent = NULL;

  /* Report SERVICE_STOPPED back to the SCM */
  ctx.status.dwCurrentState = SERVICE_STOPPED;
  ctx.status.dwCheckPoint = 0;
  ctx.status.dwWaitHint = 0;
  SetServiceStatus(ctx.statusHandle, &ctx.status);

  g_ctx = NULL;

  return 0;
}

/* ── svc_service_run_console ───────────────────────────────────── */

int svc_service_run_console(const char *serviceName, const char *configPath) {
  OmniRDPSvcContext ctx;
  BOOL mgrInitialized = FALSE;
  BOOL pipeInitialized = FALSE;
  memset(&ctx, 0, sizeof(ctx));
  g_ctx = &ctx;

  /* ── Service name ──────────────────────────────────────────── */
  if (serviceName && serviceName[0] != '\0') {
    if (svc_copy_string(ctx.serviceName, sizeof(ctx.serviceName),
                        serviceName) != 0) {
      g_ctx = NULL;
      return -1;
    }
  } else {
    svc_copy_string(ctx.serviceName, sizeof(ctx.serviceName), "OmniRDP");
  }
  ctx.shuttingDown = FALSE;

  /* ── Register Ctrl+C / Ctrl+Break handler ──────────────────── */
  if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
    fprintf(stderr, "SetConsoleCtrlHandler failed: %lu\n", GetLastError());
    g_ctx = NULL;
    return -1;
  }

  /* ── Create the stop event ─────────────────────────────────── */
  ctx.hStopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
  if (!ctx.hStopEvent) {
    fprintf(stderr, "CreateEvent failed: %lu\n", GetLastError());
    g_ctx = NULL;
    return -1;
  }

  /* ── Load configuration ────────────────────────────────────── */
  if (!configPath || configPath[0] == '\0')
    configPath = "C:\\ProgramData\\OmniRDP\\config.ini";

  if (svc_copy_string(ctx.configPath, sizeof(ctx.configPath), configPath) !=
      0) {
    CloseHandle(ctx.hStopEvent);
    g_ctx = NULL;
    return -1;
  }

  ctx.config = svc_config_load(configPath);
  if (!ctx.config) {
    LOG_W("svc_service",
          "Config file '%s' not found or unreadable. "
          "Running with defaults (no instances). "
          "Edit config.ini and reload, or use the tray app to add instances.",
          configPath);
    ctx.config = svc_config_create_default();
    if (!ctx.config) {
      LOG_E("svc_service", "Failed to create default config");
    }
  }

  SvcServiceConfig *svcCfg = ctx.config ? &ctx.config->service : NULL;

  /* ── Initialize logging (per-service log directory) ────────── */
  {
    char log_dir[MAX_PATH];
    SvcLogLevel log_level = SVC_LOG_INFO;
    unsigned int max_size_mb = 10;
    unsigned int max_files = 5;

    if (svcCfg && svcCfg->log_dir[0] != '\0') {
      /* Use configured log dir, but append service name for per-service
       * isolation */
      if (snprintf(log_dir, sizeof(log_dir), "%s\\%s", svcCfg->log_dir,
                   ctx.serviceName) < 0 ||
          strnlen_s(log_dir, sizeof(log_dir)) >= sizeof(log_dir) - 1)
        log_dir[0] = '\0';
    } else {
      if (snprintf(log_dir, sizeof(log_dir),
                   "C:\\ProgramData\\OmniRDP\\logs\\%s", ctx.serviceName) < 0 ||
          strnlen_s(log_dir, sizeof(log_dir)) >= sizeof(log_dir) - 1)
        log_dir[0] = '\0';
    }

    if (svcCfg) {
      log_level = parse_log_level(svcCfg->log_level);
      max_size_mb = svcCfg->log_max_size_mb;
      max_files = svcCfg->log_max_files;
    }

    svc_log_init(log_dir, log_level, max_size_mb, max_files);
  }

  LOG_I("svc_service_run_console",
        "OmniRDP running in console mode (serviceName=%s, "
        "configPath=%s)",
        ctx.serviceName, ctx.configPath);

  printf("[OmniRDP] Running in console mode. Press Ctrl+C to stop.\n");
  printf("[OmniRDP] Service name: %s\n", ctx.serviceName);
  printf("[OmniRDP] Config path:  %s\n", ctx.configPath);

  /* ── Find the OmniRDP.exe path ─────────────────────────────── */
  if (get_binary_path("OmniRDP.exe", ctx.exePath, sizeof(ctx.exePath)) != 0) {
    LOG_W("svc_service_run_console",
          "Failed to locate OmniRDP.exe; using fallback name");
    svc_copy_string(ctx.exePath, sizeof(ctx.exePath), "OmniRDP.exe");
  }

  /* ── Initialize the instance manager ───────────────────────── */
  if (inst_mgr_init(&ctx.mgr, ctx.config, ctx.configPath, ctx.exePath) != 0) {
    LOG_E("svc_service_run_console", "inst_mgr_init failed");
    printf("[OmniRDP] ERROR: instance manager initialization "
           "failed\n");
    SetEvent(ctx.hStopEvent);
  } else {
    mgrInitialized = TRUE;
    /* ── Start all enabled instances ───────────────────────── */
    inst_mgr_start_all(&ctx.mgr);

    LOG_I("svc_service_run_console", "Console mode running with %u instances",
          ctx.config ? ctx.config->instance_count : 0);
    printf("[OmniRDP] %u instance(s) configured\n",
           ctx.config ? ctx.config->instance_count : 0);
  }

  /* Initialize pipe server for tray app communication */
  if (mgrInitialized) {
    char pipeName[256];
    if (snprintf(pipeName, sizeof(pipeName), "%s_Pipe", ctx.serviceName) < 0 ||
        strnlen_s(pipeName, sizeof(pipeName)) >= sizeof(pipeName))
      pipeName[0] = '\0';
    if (pipeName[0] == '\0') {
      LOG_E("svc_service_run_console", "Pipe name construction failed");
    } else {
      /* Replace hyphens with underscores for Windows pipe name compatibility */
      for (char *p = pipeName; *p; p++) {
        if (*p == '-')
          *p = '_';
      }
      if (pipe_server_init(&ctx.pipeServer, pipeName, &ctx.mgr,
                            ctx.serviceName) != 0) {
        LOG_E("svc_service_run_console", "Failed to initialize pipe server");
        /* Continue anyway — pipe server is not critical for operation */
      } else {
        pipeInitialized = TRUE;
      }
    }
  }

  /* ── Main loop ──────────────────────────────────────────────── */
  {
    unsigned int intervalSec = 2;
    if (svcCfg && svcCfg->health_poll_interval_sec > 0)
      intervalSec = svcCfg->health_poll_interval_sec;
    DWORD pollIntervalMs = intervalSec * 1000;

    while (!ctx.shuttingDown) {
      DWORD wr = WaitForSingleObject(ctx.hStopEvent, pollIntervalMs);
      if (wr == WAIT_OBJECT_0)
        break;
      if (mgrInitialized)
        inst_mgr_poll(&ctx.mgr);

      /* Push stats to connected tray apps every 5 seconds */
      {
        static ULONGLONG lastStatsPushMs = 0;
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - lastStatsPushMs >= 5000) {
          if (pipeInitialized)
            pipe_server_push_stats(&ctx.pipeServer);
          lastStatsPushMs = nowMs;
        }
      }
    }
  }

  /* ── Shutdown ───────────────────────────────────────────────── */
  LOG_I("svc_service_run_console", "Shutting down...");
  printf("[OmniRDP] Shutting down...\n");

  if (pipeInitialized) {
    pipe_server_stop(&ctx.pipeServer);
    pipeInitialized = FALSE;
  }

  if (mgrInitialized) {
    inst_mgr_stop_all(&ctx.mgr);
    inst_mgr_cleanup(&ctx.mgr);
    mgrInitialized = FALSE;
  }

  if (ctx.config) {
    svc_config_free(ctx.config);
    ctx.config = NULL;
  }

  svc_log_shutdown();

  CloseHandle(ctx.hStopEvent);
  ctx.hStopEvent = NULL;

  g_ctx = NULL;

  printf("[OmniRDP] Done.\n");

  return 0;
}
