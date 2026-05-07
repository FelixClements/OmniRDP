/**
 * @file tray_icon.h
 * @brief System tray icon for the OmniRDP tray application
 *
 * Manages the tray icon that reflects the state of all OmniRDP service
 * instances.  Supports four icon states (green/yellow/red/gray),
 * a dynamic right-click context menu, and background polling to
 * discover services and refresh instance info.
 *
 * This file is FreeRDP-independent and Windows-only.
 */

#ifndef TRAY_ICON_H
#define TRAY_ICON_H

#include "pipe_protocol.h"
#include "tray_pipe_client.h"
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tray icon state (maps to icon color)
 */
typedef enum {
  TRAY_ICON_GREEN = 0,  /* All instances running */
  TRAY_ICON_YELLOW = 1, /* Some instances starting/reconnecting */
  TRAY_ICON_RED = 2,    /* Some instances stopped unexpectedly */
  TRAY_ICON_GRAY = 3    /* No services found / cannot connect */
} TrayIconState;

/**
 * @brief Information about a discovered service and its instances
 */
typedef struct {
  char serviceName[256]; /* Service name in SCM (e.g., "OmniRDP-Prod") */
  char pipeName[256];    /* Derived pipe name (e.g., "OmniRDP_Prod_Pipe") */
  PipeClient client;     /* Connected pipe client (or disconnected) */
  unsigned int
      instanceCount; /* Number of instances from list_instances response */
  PipeInstanceInfo instances[32]; /* Instance info from service */
  BOOL connected;                 /* TRUE if pipe client is connected */
} TrayServiceInfo;

/**
 * @brief Tray application context (singleton)
 */
typedef struct {
  HWND hwnd;                   /* Hidden message window handle */
  UINT taskbarCreatedMsg;      /* Registered message for taskbar recreation */
  HICON hIcons[4];             /* Icons: green, yellow, red, gray */
  TrayIconState currentState;  /* Current icon state */
  TrayServiceInfo services[8]; /* Discovered services (max 8) */
  unsigned int serviceCount;   /* Number of discovered services */
  BOOL running;                /* TRUE while tray app is active */
  HANDLE hPollThread;          /* Background polling thread */
  HANDLE hPushThread;          /* Push message listener thread */
  HINSTANCE hInstance;         /* Application instance handle */
  CRITICAL_SECTION lock;       /* Thread safety for service info */
} TrayAppCtx;

/**
 * @brief Initialize the tray icon
 *
 * Creates the hidden message window, loads icons, and adds the tray icon.
 *
 * @param ctx Tray app context
 * @param hInstance Application instance handle
 * @return 0 on success, -1 on error
 */
int tray_icon_init(TrayAppCtx *ctx, HINSTANCE hInstance);

/**
 * @brief Update the tray icon based on current service/instance states
 *
 * Recalculates the overall state (green/yellow/red/gray) and updates
 * the tray icon and tooltip text.
 *
 * @param ctx Tray app context
 */
void tray_icon_update(TrayAppCtx *ctx);

/**
 * @brief Show the context menu at the tray icon position
 *
 * Builds the dynamic menu with services, instances, and actions.
 *
 * @param ctx Tray app context
 * @param hwnd Window handle for the menu owner
 */
void tray_icon_show_menu(TrayAppCtx *ctx, HWND hwnd);

/**
 * @brief Remove the tray icon and clean up resources
 *
 * @param ctx Tray app context
 */
void tray_icon_cleanup(TrayAppCtx *ctx);

/**
 * @brief Discover OmniRDP services from the SCM
 *
 * Enumerates services whose names start with "OmniRDP-" and populates
 * the services array. For each discovered service, derives the pipe name
 * and attempts to connect.
 *
 * @param ctx Tray app context
 */
void tray_icon_discover_services(TrayAppCtx *ctx);

/**
 * @brief Refresh instance info from all connected services
 *
 * Sends PIPE_CMD_LIST_INSTANCES to each connected service and
 * updates the instances array.
 *
 * @param ctx Tray app context
 */
void tray_icon_refresh_instances(TrayAppCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TRAY_ICON_H */
