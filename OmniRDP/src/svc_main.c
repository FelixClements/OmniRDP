/**
 * @file svc_main.c
 * @brief Entry point for OmniRDP-svc.exe (Windows Service)
 *
 * Supports multiple invocation modes:
 *   OmniRDP-svc.exe                          → SCM entry point (ServiceMain)
 *   OmniRDP-svc.exe --run                    → Console mode (debugging)
 *   OmniRDP-svc.exe --install                → Register with SCM
 *   OmniRDP-svc.exe --uninstall              → Remove from SCM
 *   OmniRDP-svc.exe --install --service-name "OmniRDP-Prod" --config "path"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tchar.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "svc_log.h"
#include "svc_service.h"

/* ── Default configuration ──────────────────────────────────────── */

#define DEFAULT_SERVICE_NAME "OmniRDP"
#define DEFAULT_CONFIG_PATH "C:\\ProgramData\\OmniRDP\\config.ini"

/* ── Service table entry (for SCM) ──────────────────────────────── */

static char g_serviceName[256] = DEFAULT_SERVICE_NAME;
static char g_configPath[MAX_PATH] = DEFAULT_CONFIG_PATH;

/**
 * @brief ServiceMain callback registered with the SCM.
 *
 * The SCM calls this when the service is started. It delegates to
 * svc_service_start() which handles the full service lifecycle.
 */
static VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
  svc_service_start(g_serviceName, g_configPath);
}

/**
 * @brief Static service table for StartServiceCtrlDispatcher.
 *
 * Only one service entry since OmniRDP-svc.exe is SERVICE_WIN32_OWN_PROCESS.
 */
static SERVICE_TABLE_ENTRY serviceTable[] = {
    {(LPSTR)g_serviceName, (LPSERVICE_MAIN_FUNCTION)ServiceMain}, {NULL, NULL}};

static int copy_arg_string(char *dest, size_t dest_size, const char *src) {
  int ret;
  if (!dest || dest_size == 0 || !src)
    return -1;
  ret = snprintf(dest, dest_size, "%s", src);
  return (ret < 0 || (size_t)ret >= dest_size) ? -1 : 0;
}

/* ── Usage ──────────────────────────────────────────────────────── */

static void print_usage(const char *program) {
  printf("OmniRDP Service Manager\n");
  printf("\n");
  printf("Usage:\n");
  printf("  %s                              Run as Windows Service (SCM)\n",
         program);
  printf("  %s --run                        Run in console mode (debugging)\n",
         program);
  printf(
      "  %s --install                    Install service (default name: %s)\n",
      program, DEFAULT_SERVICE_NAME);
  printf("  %s --uninstall                  Uninstall service\n", program);
  printf("  %s --install --service-name N   Install with custom service name\n",
         program);
  printf("  %s --install --config PATH      Install with custom config path\n",
         program);
  printf("\n");
  printf("Options:\n");
  printf("  --service-name NAME   Service name in SCM (default: %s)\n",
         DEFAULT_SERVICE_NAME);
  printf("  --config PATH         Path to config.ini (default: %s)\n",
         DEFAULT_CONFIG_PATH);
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  BOOL install = FALSE;
  BOOL uninstall = FALSE;
  BOOL runConsole = FALSE;
  BOOL hasServiceName = FALSE;
  BOOL hasConfigPath = FALSE;
  char serviceName[256] = DEFAULT_SERVICE_NAME;
  char configPath[MAX_PATH] = DEFAULT_CONFIG_PATH;

  /* Parse command-line arguments */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--install") == 0) {
      install = TRUE;
    } else if (strcmp(argv[i], "--uninstall") == 0) {
      uninstall = TRUE;
    } else if (strcmp(argv[i], "--run") == 0) {
      runConsole = TRUE;
    } else if (strcmp(argv[i], "--service-name") == 0 && i + 1 < argc) {
      if (copy_arg_string(serviceName, sizeof(serviceName), argv[++i]) != 0) {
        fprintf(stderr, "Service name too long.\n");
        return 1;
      }
      hasServiceName = TRUE;
    } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      if (copy_arg_string(configPath, sizeof(configPath), argv[++i]) != 0) {
        fprintf(stderr, "Config path too long.\n");
        return 1;
      }
      hasConfigPath = TRUE;
    } else if (strcmp(argv[i], "--service") == 0) {
      /*
       * Marker flag: indicates this was launched by the SCM.
       * No action needed — the service will start via ServiceMain.
       * This argument is injected by the SCM binary path registered
       * during svc_service_install.
       */
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      print_usage(argv[0]);
      return 1;
    }
  }

  /* ── Install mode ──────────────────────────────────────────── */
  if (install) {
    printf("Installing service '%s'...\n", serviceName);
    if (hasConfigPath) {
      printf("  Config path: %s\n", configPath);
    }
    int ret = svc_service_install(serviceName, configPath);
    if (ret == 0) {
      printf("Service '%s' installed successfully.\n", serviceName);
    } else {
      fprintf(stderr, "Failed to install service '%s'.\n", serviceName);
    }
    return ret;
  }

  /* ── Uninstall mode ─────────────────────────────────────────── */
  if (uninstall) {
    printf("Uninstalling service '%s'...\n", serviceName);
    int ret = svc_service_uninstall(serviceName);
    if (ret == 0) {
      printf("Service '%s' uninstalled successfully.\n", serviceName);
    } else {
      fprintf(stderr, "Failed to uninstall service '%s'.\n", serviceName);
    }
    return ret;
  }

  /* ── Console mode (debugging) ──────────────────────────────── */
  if (runConsole) {
    printf("Running '%s' in console mode (Ctrl+C to stop)...\n", serviceName);
    printf("  Config: %s\n", configPath);
    int ret = svc_service_run_console(serviceName, configPath);
    printf("Service exited with code %d.\n", ret);
    return ret;
  }

  /* ── Service mode (SCM) ────────────────────────────────────── */
  /* Store globals for ServiceMain callback */
  if (copy_arg_string(g_serviceName, sizeof(g_serviceName), serviceName) != 0 ||
      copy_arg_string(g_configPath, sizeof(g_configPath), configPath) != 0) {
    fprintf(stderr, "Service name or config path too long.\n");
    return 1;
  }

  /* Update the service table with the actual service name.
   * StartServiceCtrlDispatcher requires a non-const pointer. */
  serviceTable[0].lpServiceName = (LPSTR)g_serviceName;

  printf("Starting service '%s' via SCM...\n", serviceName);

  if (!StartServiceCtrlDispatcher(serviceTable)) {
    DWORD err = GetLastError();
    fprintf(stderr, "StartServiceCtrlDispatcher failed (error %lu).\n", err);
    fprintf(
        stderr,
        "Hint: Are you running from a command prompt instead of the SCM?\n");
    fprintf(stderr, "      Use --run for console mode, or --install to "
                    "register the service.\n");
    return 1;
  }

  return 0;
}
