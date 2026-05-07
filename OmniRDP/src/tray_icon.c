/**
 * @file tray_icon.c
 * @brief System tray icon implementation for the OmniRDP tray application
 *
 * Implements the tray icon with four color states (green/yellow/red/gray),
 * a dynamic right-click context menu with service/instance hierarchy,
 * and background polling via SCM enumeration and named-pipe IPC.
 *
 * This file is FreeRDP-independent and Windows-only.
 */

#include "tray_icon.h"
#include "pipe_protocol.h"
#include "svc_log.h"
#include "tray_log_viewer.h"
#include "tray_pipe_client.h"
#include "tray_status_dlg.h"

#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wtsapi32.h>

/* ── Log tag ──────────────────────────────────────────────────────── */
#define LOG_TAG "tray_icon"

/* ── Constants ────────────────────────────────────────────────────── */

/** Custom message sent by the tray icon on user interaction. */
#define WM_TRAYICON (WM_APP + 1)

/** Polling interval for background service/instance refresh (ms). */
#define POLL_INTERVAL_MS 5000U

/** Window class name for the hidden message-only window. */
#define HIDDEN_WIN_CLASS "OmniRDPTrayHiddenWindow"

/* ── Menu Item IDs ───────────────────────────────────────────────── */

#define IDM_OPEN_STATUS 1001
#define IDM_RELOAD_CONFIG 1002
#define IDM_VIEW_LOG 1003
#define IDM_ABOUT 1004
#define IDM_EXIT 1005
#define IDM_INSTALL_SERVICE 1006

/**
 * Dynamic instance action IDs are computed as:
 *   ID = IDM_SERVICE_BASE
 *        + (serviceIdx * IDM_SERVICE_STRIDE)
 *        + (instanceIdx * IDM_INSTANCE_SLOT_SIZE)
 *        + actionOffset
 *
 * This supports up to 8 services × 16 instances per service.
 */
#define IDM_SERVICE_BASE 2000
#define IDM_INSTANCE_ACTION_START 0
#define IDM_INSTANCE_ACTION_STOP 1
#define IDM_INSTANCE_ACTION_RESTART 2
#define IDM_INSTANCE_SLOT_SIZE 4
#define IDM_SERVICE_STRIDE 64

/* ── Forward Declarations ────────────────────────────────────────── */

static LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam);
static void tray_on_command(TrayAppCtx *ctx, WPARAM wParam);
static void add_tray_icon(TrayAppCtx *ctx);
static void remove_tray_icon(TrayAppCtx *ctx);
static DWORD WINAPI poll_thread_proc(LPVOID arg);
static DWORD WINAPI push_listener_thread(LPVOID param);
static const char *instance_state_str(PipeInstanceState state);

/* ══════════════════════════════════════════════════════════════════ */
/*  Helper: Instance State → String                                   */
/* ══════════════════════════════════════════════════════════════════ */

static const char *instance_state_str(PipeInstanceState state) {
  switch (state) {
  case INSTANCE_STOPPED:
    return "Stopped";
  case INSTANCE_STARTING:
    return "Starting";
  case INSTANCE_RUNNING:
    return "Running";
  case INSTANCE_RECONNECTING:
    return "Reconnecting";
  default:
    return "Unknown";
  }
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Hidden Window Procedure                                           */
/* ══════════════════════════════════════════════════════════════════ */

static LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam) {
  TrayAppCtx *ctx = (TrayAppCtx *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

  /*
   * WM_TASKBARCREATED is a dynamically registered message
   * (RegisterWindowMessageA("TaskbarCreated")), not a compile-time
   * constant.  Check it first before the switch.
   */
  if (ctx && ctx->taskbarCreatedMsg != 0 && msg == ctx->taskbarCreatedMsg) {
    if (ctx->running) {
      LOG_I(LOG_TAG, "Taskbar recreated — re-adding tray icon");
      add_tray_icon(ctx);
    }
    return 0;
  }

  switch (msg) {
  /* ── Tray icon notification (left/right click, etc.) ──── */
  case WM_TRAYICON: {
    DWORD mouseMsg = (DWORD)lParam;
    if (mouseMsg == WM_LBUTTONUP) {
      /* Left-click → open status window */
      LOG_I(LOG_TAG, "Tray icon left-click");
      if (ctx) {
        tray_status_dlg_show(ctx, ctx->hInstance);
      }
    } else if (mouseMsg == WM_RBUTTONUP) {
      /* Right-click → show context menu */
      if (ctx)
        tray_icon_show_menu(ctx, hwnd);
    }
    return 0;
  }

  /* ── Menu command ─────────────────────────────────────── */
  case WM_COMMAND: {
    UINT id = LOWORD(wParam);
    if (ctx)
      tray_on_command(ctx, id);
    return 0;
  }

  /* ── Session change (logoff/logon) ────────────────────── */
  case WM_WTSSESSION_CHANGE:
    switch (wParam) {
    case WTS_SESSION_LOGOFF:
      LOG_I(LOG_TAG, "Session logoff detected, shutting down");
      ctx->running = FALSE;
      PostQuitMessage(0);
      break;
    case WTS_SESSION_LOCK:
      LOG_I(LOG_TAG, "Session locked");
      break;
    case WTS_SESSION_UNLOCK:
      LOG_I(LOG_TAG, "Session unlocked");
      break;
    }
    return 0;

  /* ── Clean up on close ────────────────────────────────── */
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Programmatic Icon Creation                                         */
/* ══════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a colored circle icon programmatically
 *
 * Creates a 16x16 icon with a filled circle of the specified color.
 * Used for tray icon state indication.
 */
static HICON create_color_icon(COLORREF color) {
  int size = 16;
  HDC hdc = GetDC(NULL);
  HDC hdcMem = CreateCompatibleDC(hdc);

  /* Create bitmap */
  BITMAPINFO bmi = {0};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = size;
  bmi.bmiHeader.biHeight = size;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void *bits = NULL;
  HBITMAP hBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
  HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBitmap);

  /* Fill with transparent background */
  memset(bits, 0, size * size * 4);

  /* Draw a filled circle */
  int cx = size / 2, cy = size / 2, radius = 6;
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      int dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy <= radius * radius) {
        BYTE *pixel = (BYTE *)bits + (y * size + x) * 4;
        pixel[0] = GetBValue(color); /* Blue */
        pixel[1] = GetGValue(color); /* Green */
        pixel[2] = GetRValue(color); /* Red */
        pixel[3] = 255;              /* Alpha */
      }
    }
  }

  SelectObject(hdcMem, hOldBmp);
  DeleteDC(hdcMem);
  ReleaseDC(NULL, hdc);

  /* Create mask bitmap (all opaque) */
  HBITMAP hMask = CreateBitmap(size, size, 1, 1, NULL);

  /* Create icon */
  ICONINFO iconInfo = {0};
  iconInfo.fIcon = TRUE;
  iconInfo.hbmColor = hBitmap;
  iconInfo.hbmMask = hMask;
  HICON hIcon = CreateIconIndirect(&iconInfo);

  DeleteObject(hBitmap);
  DeleteObject(hMask);

  return hIcon;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Tray Icon Lifecycle Helpers                                       */
/* ══════════════════════════════════════════════════════════════════ */

static void add_tray_icon(TrayAppCtx *ctx) {
  NOTIFYICONDATAA nid;
  memset(&nid, 0, sizeof(nid));
  nid.cbSize = sizeof(NOTIFYICONDATAA);
  nid.hWnd = ctx->hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
  nid.uCallbackMessage = WM_TRAYICON;
  nid.hIcon = ctx->hIcons[ctx->currentState];
  _snprintf(nid.szTip, sizeof(nid.szTip), "OmniRDP Service");
  nid.szTip[sizeof(nid.szTip) - 1] = '\0';

  if (!Shell_NotifyIconA(NIM_ADD, &nid)) {
    LOG_W(LOG_TAG, "Shell_NotifyIconA(NIM_ADD) failed: %lu", GetLastError());
  }
}

static void remove_tray_icon(TrayAppCtx *ctx) {
  NOTIFYICONDATAA nid;
  memset(&nid, 0, sizeof(nid));
  nid.cbSize = sizeof(NOTIFYICONDATAA);
  nid.hWnd = ctx->hwnd;
  nid.uID = 1;

  Shell_NotifyIconA(NIM_DELETE, &nid);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Poll Thread                                                        */
/* ══════════════════════════════════════════════════════════════════ */

static DWORD WINAPI poll_thread_proc(LPVOID arg) {
  TrayAppCtx *ctx = (TrayAppCtx *)arg;
  ULONGLONG lastReconnect = 0;

  LOG_I(LOG_TAG, "Poll thread started");

  while (ctx->running) {
    Sleep(POLL_INTERVAL_MS);
    if (!ctx->running)
      break;

    tray_icon_discover_services(ctx);
    tray_icon_refresh_instances(ctx);
    tray_icon_update(ctx);

    /* Update status window if visible */
    if (tray_status_dlg_is_visible()) {
      tray_status_dlg_refresh(ctx);
    }

    /* Reconnect disconnected services every 10 seconds */
    {
      ULONGLONG now = GetTickCount64();
      if (now - lastReconnect >= 10000) {
        for (unsigned int i = 0; i < ctx->serviceCount; i++) {
          if (!ctx->services[i].connected) {
            pipe_client_disconnect(&ctx->services[i].client);
            if (pipe_client_connect(&ctx->services[i].client) == 0) {
              ctx->services[i].connected = TRUE;
              LOG_I(LOG_TAG, "Reconnected to service '%s'",
                    ctx->services[i].serviceName);
            }
          }
        }
        lastReconnect = now;
      }
    }
  }

  LOG_I(LOG_TAG, "Poll thread exiting");
  return 0;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Push Listener Thread                                               */
/* ══════════════════════════════════════════════════════════════════ */

/**
 * @brief Background thread that listens for push messages from services
 *
 * Calls pipe_client_recv_push() on each connected service pipe.
 * When a push is received, updates the tray icon and refreshes
 * the status window if visible.
 */
static DWORD WINAPI push_listener_thread(LPVOID param) {
  TrayAppCtx *ctx = (TrayAppCtx *)param;

  while (ctx->running) {
    BOOL anyActivity = FALSE;

    EnterCriticalSection(&ctx->lock);
    for (unsigned int i = 0; i < ctx->serviceCount; i++) {
      if (!ctx->services[i].connected)
        continue;

      PipePushType pushType;
      char *payload = NULL;
      DWORD payloadLen = 0;

      if (pipe_client_recv_push(&ctx->services[i].client, &pushType, &payload,
                                &payloadLen) == 0) {
        anyActivity = TRUE;

        if (pushType == PIPE_PUSH_STATS) {
          /* Update instance info from stats push */
          /* The payload contains instance state data */
          LOG_D("tray_push", "Received stats push from '%s'",
                ctx->services[i].serviceName);
        } else if (pushType == PIPE_PUSH_EVENT) {
          LOG_I("tray_push", "Received event push from '%s'",
                ctx->services[i].serviceName);
        }

        if (payload) {
          HeapFree(GetProcessHeap(), 0, payload);
        }
      }
    }
    LeaveCriticalSection(&ctx->lock);

    if (anyActivity) {
      /* Refresh tray icon and status window */
      tray_icon_update(ctx);
      if (tray_status_dlg_is_visible()) {
        tray_status_dlg_refresh(ctx);
      }
    }

    /* Sleep briefly to avoid busy-waiting, but wake up quickly
     * when there's activity */
    Sleep(anyActivity ? 100 : 500);
  }

  return 0;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Public API                                                         */
/* ══════════════════════════════════════════════════════════════════ */

int tray_icon_init(TrayAppCtx *ctx, HINSTANCE hInstance) {
  if (!ctx || !hInstance)
    return -1;

  memset(ctx, 0, sizeof(*ctx));

  /* ── Register the hidden window class ─────────────────────── */
  WNDCLASSEXA wc;
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(WNDCLASSEXA);
  wc.lpfnWndProc = tray_wnd_proc;
  wc.hInstance = hInstance;
  wc.lpszClassName = HIDDEN_WIN_CLASS;

  ATOM atom = RegisterClassExA(&wc);
  if (atom == 0) {
    DWORD err = GetLastError();
    /* If already registered, that's OK */
    if (err != ERROR_CLASS_ALREADY_EXISTS) {
      LOG_E(LOG_TAG, "RegisterClassExA failed: %lu", err);
      return -1;
    }
  }

  /* ── Create the hidden message window ─────────────────────── */
  ctx->hwnd = CreateWindowExA(
      0, HIDDEN_WIN_CLASS, "OmniRDP Tray", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, hInstance, NULL);

  if (!ctx->hwnd) {
    LOG_E(LOG_TAG, "CreateWindowExA failed: %lu", GetLastError());
    return -1;
  }

  /* Associate the context with the window */
  SetWindowLongPtrA(ctx->hwnd, GWLP_USERDATA, (LONG_PTR)ctx);

  /* Register for session change notifications (logoff/logon) */
  WTSRegisterSessionNotification(ctx->hwnd, NOTIFY_FOR_THIS_SESSION);

  /* ── Create colored icons programmatically ────────────────── */
  ctx->hIcons[0] = create_color_icon(RGB(0, 200, 0)); /* Green — all running */
  ctx->hIcons[1] =
      create_color_icon(RGB(255, 200, 0)); /* Yellow — starting/reconnecting */
  ctx->hIcons[2] = create_color_icon(RGB(220, 0, 0)); /* Red — stopped/error */
  ctx->hIcons[3] =
      create_color_icon(RGB(128, 128, 128)); /* Gray — no services */

  /* Check that icons were created */
  for (int i = 0; i < 4; i++) {
    if (!ctx->hIcons[i]) {
      LOG_W("tray_icon", "Failed to create icon %d, using default", i);
      ctx->hIcons[i] = LoadIconA(NULL, IDI_APPLICATION);
    }
  }

  /* ── Register the TaskbarCreated message ──────────────────── */
  ctx->taskbarCreatedMsg = RegisterWindowMessageA("TaskbarCreated");
  if (ctx->taskbarCreatedMsg == 0) {
    LOG_W(LOG_TAG, "RegisterWindowMessageA(TaskbarCreated) failed: %lu",
          GetLastError());
    /* Non-fatal – we just won't handle Explorer restarts */
  }

  /* ── Store instance handle ────────────────────────────────── */
  ctx->hInstance = hInstance;

  /* ── Initialise synchronisation ───────────────────────────── */
  InitializeCriticalSection(&ctx->lock);
  ctx->running = TRUE;
  ctx->currentState = TRAY_ICON_GRAY;

  /* ── Add the tray icon ────────────────────────────────────── */
  add_tray_icon(ctx);

  /* ── Perform initial discovery and refresh ────────────────── */
  tray_icon_discover_services(ctx);
  tray_icon_refresh_instances(ctx);
  tray_icon_update(ctx);

  /* ── Start the background polling thread ──────────────────── */
  ctx->hPollThread = CreateThread(NULL, 0, poll_thread_proc, ctx, 0, NULL);
  if (!ctx->hPollThread) {
    LOG_E(LOG_TAG, "CreateThread(poll_thread) failed: %lu", GetLastError());
    /* Non-fatal – tray still works, just won't auto-refresh */
  }

  /* Start push message listener thread */
  ctx->hPushThread = CreateThread(NULL, 0, push_listener_thread, ctx, 0, NULL);
  if (!ctx->hPushThread) {
    LOG_W("tray_icon", "Failed to create push listener thread");
  }

  LOG_I(LOG_TAG, "Tray icon initialised");
  return 0;
}

/* ══════════════════════════════════════════════════════════════════ */

void tray_icon_update(TrayAppCtx *ctx) {
  if (!ctx)
    return;

  EnterCriticalSection(&ctx->lock);

  TrayIconState newState = TRAY_ICON_GRAY;
  unsigned int runningCount = 0;
  unsigned int totalCount = 0;
  BOOL hasRed = FALSE;
  BOOL hasYellow = FALSE;

  if (ctx->serviceCount == 0) {
    newState = TRAY_ICON_GRAY;
  } else {
    for (unsigned int i = 0; i < ctx->serviceCount; i++) {
      const TrayServiceInfo *svc = &ctx->services[i];

      if (!svc->connected) {
        /* Service discovered but unreachable → RED */
        hasRed = TRUE;
        continue;
      }

      for (unsigned int j = 0; j < svc->instanceCount; j++) {
        totalCount++;

        switch (svc->instances[j].state) {
        case INSTANCE_STOPPED:
          hasRed = TRUE;
          break;

        case INSTANCE_STARTING:
        case INSTANCE_RECONNECTING:
          hasYellow = TRUE;
          break;

        case INSTANCE_RUNNING:
          runningCount++;
          break;

        default:
          hasRed = TRUE;
          break;
        }
      }
    }

    if (hasRed)
      newState = TRAY_ICON_RED;
    else if (hasYellow)
      newState = TRAY_ICON_YELLOW;
    else if (totalCount > 0 && runningCount == totalCount)
      newState = TRAY_ICON_GREEN;
    else if (totalCount == 0)
      newState = TRAY_ICON_GRAY;
    /* else keep current (fallback) */
  }

  ctx->currentState = newState;

  /* ── Update the tray icon and tooltip ─────────────────────── */
  NOTIFYICONDATAA nid;
  memset(&nid, 0, sizeof(nid));
  nid.cbSize = sizeof(NOTIFYICONDATAA);
  nid.hWnd = ctx->hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_TIP;
  nid.hIcon = ctx->hIcons[newState];

  _snprintf(nid.szTip, sizeof(nid.szTip), "OmniRDP: %u/%u running",
            runningCount, totalCount);
  nid.szTip[sizeof(nid.szTip) - 1] = '\0';

  if (!Shell_NotifyIconA(NIM_MODIFY, &nid)) {
    LOG_W(LOG_TAG, "Shell_NotifyIconA(NIM_MODIFY) failed: %lu", GetLastError());
  }

  LeaveCriticalSection(&ctx->lock);
}

/* ══════════════════════════════════════════════════════════════════ */

void tray_icon_show_menu(TrayAppCtx *ctx, HWND hwnd) {
  if (!ctx || !hwnd)
    return;

  HMENU hMenu = CreatePopupMenu();
  if (!hMenu) {
    LOG_E(LOG_TAG, "CreatePopupMenu failed: %lu", GetLastError());
    return;
  }

  /* ── Header (disabled) ────────────────────────────────────── */
  AppendMenuA(hMenu, MF_STRING | MF_GRAYED | MF_DISABLED, 0, "OmniRDP Service");

  /* ── Separator ────────────────────────────────────────────── */
  AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);

  /* ── Open Status Window ───────────────────────────────────── */
  AppendMenuA(hMenu, MF_STRING, IDM_OPEN_STATUS, "Open Status Window");

  /* ── Separator ────────────────────────────────────────────── */
  AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);

  /* ── Services submenu ─────────────────────────────────────── */
  HMENU hServicesMenu = CreatePopupMenu();

  EnterCriticalSection(&ctx->lock);

  if (ctx->serviceCount > 0 && hServicesMenu) {
    for (unsigned int i = 0; i < ctx->serviceCount; i++) {
      const TrayServiceInfo *svc = &ctx->services[i];

      /* Create a submenu for this service */
      HMENU hSvcMenu = CreatePopupMenu();
      if (!hSvcMenu)
        continue;

      /* Service name header (disabled) */
      char svcHeader[280];
      _snprintf(svcHeader, sizeof(svcHeader), "%s", svc->serviceName);
      svcHeader[sizeof(svcHeader) - 1] = '\0';
      AppendMenuA(hSvcMenu, MF_STRING | MF_GRAYED | MF_DISABLED, 0, svcHeader);

      /* Separator after header */
      AppendMenuA(hSvcMenu, MF_SEPARATOR, 0, NULL);

      /* Add each instance as a submenu */
      for (unsigned int j = 0; j < svc->instanceCount; j++) {
        const PipeInstanceInfo *inst = &svc->instances[j];
        const char *stateStr = instance_state_str(inst->state);

        /* Create submenu for this instance */
        HMENU hInstMenu = CreatePopupMenu();
        if (!hInstMenu)
          continue;

        /* Instance label (disabled) */
        char instLabel[192];
        _snprintf(instLabel, sizeof(instLabel), "%s [%s]", inst->name,
                  stateStr);
        instLabel[sizeof(instLabel) - 1] = '\0';
        AppendMenuA(hInstMenu, MF_STRING | MF_GRAYED | MF_DISABLED, 0,
                    instLabel);

        /* Action items */
        UINT baseId = IDM_SERVICE_BASE + (i * IDM_SERVICE_STRIDE) +
                      (j * IDM_INSTANCE_SLOT_SIZE);

        if (inst->state == INSTANCE_RUNNING) {
          AppendMenuA(hInstMenu, MF_STRING, baseId + IDM_INSTANCE_ACTION_STOP,
                      "Stop");
          AppendMenuA(hInstMenu, MF_STRING,
                      baseId + IDM_INSTANCE_ACTION_RESTART, "Restart");
        } else if (inst->state == INSTANCE_STOPPED) {
          AppendMenuA(hInstMenu, MF_STRING, baseId + IDM_INSTANCE_ACTION_START,
                      "Start");
        }
        /* STARTING/RECONNECTING: no actionable items */

        /* Add instance submenu to the service menu */
        char instMenuText[192];
        _snprintf(instMenuText, sizeof(instMenuText), "%s [%s]", inst->name,
                  stateStr);
        instMenuText[sizeof(instMenuText) - 1] = '\0';

        AppendMenuA(hSvcMenu, MF_POPUP | MF_STRING, (UINT_PTR)hInstMenu,
                    instMenuText);
      }

      /* Add the service submenu to the services menu */
      AppendMenuA(hServicesMenu, MF_POPUP | MF_STRING, (UINT_PTR)hSvcMenu,
                  svc->serviceName);
    }
  } else {
    /* No services found */
    AppendMenuA(hServicesMenu, MF_STRING | MF_GRAYED | MF_DISABLED, 0,
                "(No services found)");
  }

  LeaveCriticalSection(&ctx->lock);

  AppendMenuA(hMenu, MF_POPUP | MF_STRING, (UINT_PTR)hServicesMenu, "Services");

  /* ── Separator ────────────────────────────────────────────── */
  AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);

  /* ── Global Actions ───────────────────────────────────────── */
  AppendMenuA(hMenu, MF_STRING, IDM_RELOAD_CONFIG, "Reload All Configurations");
  AppendMenuA(hMenu, MF_STRING, IDM_VIEW_LOG, "View Service Log");

  /* "Install Service..." shown only when none discovered */
  if (ctx->serviceCount == 0) {
    AppendMenuA(hMenu, MF_STRING, IDM_INSTALL_SERVICE, "Install Service...");
  }

  AppendMenuA(hMenu, MF_STRING, IDM_ABOUT, "About");

  /* ── Separator ────────────────────────────────────────────── */
  AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);

  /* ── Exit ─────────────────────────────────────────────────── */
  AppendMenuA(hMenu, MF_STRING, IDM_EXIT, "Exit");

  /* ── Display the popup menu ───────────────────────────────── */
  POINT pt;
  GetCursorPos(&pt);

  SetForegroundWindow(hwnd);
  TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd,
                 NULL);

  /* Work around Windows menu re-selection issue */
  PostMessageA(hwnd, WM_NULL, 0, 0);

  /* Clean up all submenus (DestroyMenu recursively destroys children) */
  DestroyMenu(hMenu);
}

/* ══════════════════════════════════════════════════════════════════ */

void tray_icon_discover_services(TrayAppCtx *ctx) {
  if (!ctx)
    return;

  EnterCriticalSection(&ctx->lock);

  /* ── Clean up existing service entries ────────────────────── */
  for (unsigned int i = 0; i < ctx->serviceCount; i++) {
    pipe_client_cleanup(&ctx->services[i].client);
  }
  ctx->serviceCount = 0;

  /* ── Open the Service Control Manager ─────────────────────── */
  SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
  if (!hSCM) {
    LOG_W(LOG_TAG, "OpenSCManagerA failed: %lu", GetLastError());
    LeaveCriticalSection(&ctx->lock);
    return;
  }

  /* ── First call: determine required buffer size ───────────── */
  DWORD bytesNeeded = 0;
  DWORD servicesReturned = 0;

  BOOL ret = EnumServicesStatusExA(
      hSCM, SC_ENUM_PROCESS_INFO, SERVICE_WIN32_OWN_PROCESS, SERVICE_STATE_ALL,
      NULL, 0, &bytesNeeded, &servicesReturned, NULL, NULL);

  if (!ret && GetLastError() != ERROR_MORE_DATA) {
    LOG_W(LOG_TAG, "EnumServicesStatusExA (size query) failed: %lu",
          GetLastError());
    CloseServiceHandle(hSCM);
    LeaveCriticalSection(&ctx->lock);
    return;
  }

  /* ── Allocate buffer ──────────────────────────────────────── */
  LPENUM_SERVICE_STATUS_PROCESSA services =
      (LPENUM_SERVICE_STATUS_PROCESSA)HeapAlloc(GetProcessHeap(),
                                                HEAP_ZERO_MEMORY, bytesNeeded);

  if (!services) {
    LOG_E(LOG_TAG, "HeapAlloc for service enumeration failed");
    CloseServiceHandle(hSCM);
    LeaveCriticalSection(&ctx->lock);
    return;
  }

  /* ── Second call: retrieve the actual list ────────────────── */
  if (!EnumServicesStatusExA(hSCM, SC_ENUM_PROCESS_INFO,
                             SERVICE_WIN32_OWN_PROCESS, SERVICE_STATE_ALL,
                             (LPBYTE)services, bytesNeeded, &bytesNeeded,
                             &servicesReturned, NULL, NULL)) {
    LOG_W(LOG_TAG, "EnumServicesStatusExA (data) failed: %lu", GetLastError());
    HeapFree(GetProcessHeap(), 0, services);
    CloseServiceHandle(hSCM);
    LeaveCriticalSection(&ctx->lock);
    return;
  }

  /* ── Filter OmniRDP- services and populate the array ──────── */
  for (DWORD i = 0; i < servicesReturned && ctx->serviceCount < 8; i++) {
    const char *svcName = services[i].lpServiceName;

    /* Match services whose name starts with "OmniRDP" */
    if (_strnicmp(svcName, "OmniRDP", 7) != 0)
      continue;
    /* Must be exactly "OmniRDP" or start with "OmniRDP-" */
    if (strlen(svcName) > 7 && svcName[7] != '-')
      continue;

    TrayServiceInfo *svc = &ctx->services[ctx->serviceCount];
    memset(svc, 0, sizeof(TrayServiceInfo));

    /* Store the service name */
    strncpy(svc->serviceName, svcName, sizeof(svc->serviceName) - 1);
    svc->serviceName[sizeof(svc->serviceName) - 1] = '\0';

    /* Derive pipe name: replace hyphens with underscores, append "_Pipe" */
    {
      const char *src = svc->serviceName;
      char *dst = svc->pipeName;
      char *end = svc->pipeName + sizeof(svc->pipeName) - 10;

      while (*src && dst < end) {
        if (*src == '-')
          *dst++ = '_';
        else
          *dst++ = *src;
        src++;
      }
      memcpy(dst, "_Pipe", 6); /* includes NUL */
    }

    /* Initialise and attempt to connect the pipe client */
    pipe_client_init(&svc->client, svc->pipeName);
    if (pipe_client_connect(&svc->client) == 0) {
      svc->connected = TRUE;
      LOG_I(LOG_TAG, "Connected to service '%s' via pipe '%s'",
            svc->serviceName, svc->pipeName);
    } else {
      LOG_W(LOG_TAG, "Could not connect to service '%s' via pipe '%s'",
            svc->serviceName, svc->pipeName);
    }

    ctx->serviceCount++;
  }

  HeapFree(GetProcessHeap(), 0, services);
  CloseServiceHandle(hSCM);

  LeaveCriticalSection(&ctx->lock);
}

/* ══════════════════════════════════════════════════════════════════ */

void tray_icon_refresh_instances(TrayAppCtx *ctx) {
  if (!ctx)
    return;

  EnterCriticalSection(&ctx->lock);

  LOG_D("tray_icon", "refresh_instances: starting (serviceCount=%u)",
        ctx->serviceCount);

  for (unsigned int i = 0; i < ctx->serviceCount; i++) {
    TrayServiceInfo *svc = &ctx->services[i];

    LOG_D("tray_icon", "refresh_instances: service[%u] '%s' connected=%d", i,
          svc->serviceName, svc->connected);

    if (!svc->connected)
      continue;

    /* ── Build the LIST_INSTANCES request ─────────────────── */
    PipeRequest req;
    memset(&req, 0, sizeof(req));
    req.command = PIPE_CMD_LIST_INSTANCES;

    PipeResponse resp;
    memset(&resp, 0, sizeof(resp));

    /* ── Send the request ─────────────────────────────────── */
    LOG_D("tray_icon", "refresh_instances: sending LIST_INSTANCES to '%s'",
          svc->serviceName);
    if (pipe_client_send_request(&svc->client, &req, &resp, 5000) != 0) {
      LOG_W(LOG_TAG,
            "pipe_client_send_request(LIST_INSTANCES) "
            "failed for '%s'",
            svc->serviceName);
      svc->connected = FALSE;
      pipe_client_disconnect(&svc->client);
      continue;
    }

    if (!resp.success) {
      LOG_W(LOG_TAG, "LIST_INSTANCES failed for '%s': %s", svc->serviceName,
            resp.error_message);
      continue;
    }

    LOG_D("tray_icon",
          "refresh_instances: received response from '%s', success=%d, "
          "instanceCount=%u",
          svc->serviceName, resp.success, svc->instanceCount);

    /* ── Parse instance info from json_payload ────────────── */
    /*
     * The response json_payload looks like:
     *   "{\"instances\":[{\"name\":\"<name>\",\"state\":N,...},...]}"
     *
     * We parse it without a JSON library by searching for
     * escaped field patterns.
     */
    svc->instanceCount = 0;

    const char *payload = resp.json_payload;
    if (!payload || payload[0] == '\0')
      continue;

    /* Find the instances array start: \"instances\":[ */
    const char *arrStart = strstr(payload, "\\\"instances\\\":[");
    if (!arrStart) {
      /* Also try unescaped form (simpler payloads) */
      arrStart = strstr(payload, "\"instances\":[");
      if (!arrStart)
        continue;
      arrStart += 13; /* skip past "instances":[ */
    } else {
      arrStart += 15; /* skip past \"instances\":[ */
    }

    const char *p = arrStart;
    while (p && *p && *p != ']' && svc->instanceCount < 32) {
      /* Locate opening brace of this instance object */
      p = strchr(p, '{');
      if (!p)
        break;
      p++; /* skip past { */

      PipeInstanceInfo *info = &svc->instances[svc->instanceCount];
      memset(info, 0, sizeof(PipeInstanceInfo));

      /* ── Parse "state" (integer, straightforward) ──────── */
      {
        const char *field = strstr(p, "\\\"state\\\":");
        if (!field)
          field = strstr(p, "\"state\":");
        if (field) {
          const char *val = field;
          while (*val && *val != ':')
            val++;
          if (*val == ':') {
            val++;
            int stateVal = 0;
            if (sscanf(val, "%d", &stateVal) == 1)
              info->state = (PipeInstanceState)stateVal;
          }
        }
      }

      /* ── Parse "name" ─────────────────────────────────── */
      {
        /*
         * Escaped: \"name\":\"<value>\"
         * Flat:    "name":"<value>"
         */
        const char *field = strstr(p, "\\\"name\\\":\\\"");
        const char *valStart = NULL;

        if (field) {
          valStart = field + 11; /* skip \"name\":\" */
        } else {
          field = strstr(p, "\"name\":\"");
          if (field)
            valStart = field + 8; /* skip "name":" */
        }

        if (valStart) {
          const char *valEnd = NULL;
          /* Try escaped terminator first, then flat */
          valEnd = strstr(valStart, "\\\"");
          if (!valEnd)
            valEnd = strchr(valStart, '"');

          if (valEnd && valEnd > valStart) {
            size_t len = (size_t)(valEnd - valStart);
            if (len > sizeof(info->name) - 1)
              len = sizeof(info->name) - 1;
            memcpy(info->name, valStart, len);
            info->name[len] = '\0';
          }
        }
      }

      /* ── Parse "viewer_count" ─────────────────────────── */
      {
        const char *field = strstr(p, "\\\"viewer_count\\\":");
        if (!field)
          field = strstr(p, "\"viewer_count\":");
        if (field) {
          const char *val = field;
          while (*val && *val != ':')
            val++;
          if (*val == ':') {
            val++;
            unsigned long vc = 0;
            sscanf(val, "%lu", &vc);
            info->viewer_count = (DWORD)vc;
          }
        }
      }

      /* ── Parse "backend_hostname" ───────────────────────── */
      {
        const char *field = strstr(p, "\\\"backend_hostname\\\":\\\"");
        const char *valStart = NULL;

        if (field) {
          valStart = field + 23; /* skip \"backend_hostname\":\" (23 chars) */
        } else {
          field = strstr(p, "\"backend_hostname\":\"");
          if (field)
            valStart = field + 20; /* skip "backend_hostname":" */
        }

        if (valStart) {
          const char *valEnd = strstr(valStart, "\\\"");
          if (!valEnd)
            valEnd = strchr(valStart, '"');
          if (valEnd && valEnd > valStart) {
            size_t len = (size_t)(valEnd - valStart);
            if (len > sizeof(info->backend_hostname) - 1)
              len = sizeof(info->backend_hostname) - 1;
            memcpy(info->backend_hostname, valStart, len);
            info->backend_hostname[len] = '\0';
          }
        }
      }

      /* ── Parse "backend_port" ───────────────────────────── */
      {
        const char *field = strstr(p, "\\\"backend_port\\\":");
        if (!field)
          field = strstr(p, "\"backend_port\":");
        if (field) {
          const char *val = field;
          while (*val && *val != ':')
            val++;
          if (*val == ':') {
            val++;
            unsigned long portVal = 0;
            sscanf(val, "%lu", &portVal);
            info->backend_port = (uint16_t)portVal;
          }
        }
      }

      svc->instanceCount++;

      /* Advance past this object */
      p = strchr(p, '}');
      if (p)
        p++; /* skip past } */
    }

    LOG_D(LOG_TAG, "Refreshed %u instances for '%s'", svc->instanceCount,
          svc->serviceName);
  }

  LeaveCriticalSection(&ctx->lock);
}

/* ══════════════════════════════════════════════════════════════════ */

void tray_icon_cleanup(TrayAppCtx *ctx) {
  if (!ctx)
    return;

  LOG_I(LOG_TAG, "Cleaning up tray icon");

  /* ── Signal shutdown ──────────────────────────────────────── */
  ctx->running = FALSE;

  /* ── Wait for the poll thread to exit ─────────────────────── */
  if (ctx->hPollThread) {
    if (WaitForSingleObject(ctx->hPollThread, 3000) == WAIT_TIMEOUT) {
      LOG_W(LOG_TAG, "Poll thread did not exit within 3s");
    }
    CloseHandle(ctx->hPollThread);
    ctx->hPollThread = NULL;
  }

  /* ── Wait for the push listener thread to exit ────────────── */
  if (ctx->hPushThread) {
    if (WaitForSingleObject(ctx->hPushThread, 3000) == WAIT_TIMEOUT) {
      LOG_W(LOG_TAG, "Push thread did not exit within 3s");
    }
    CloseHandle(ctx->hPushThread);
    ctx->hPushThread = NULL;
  }

  /* ── Close the log viewer if open ─────────────────────────── */
  tray_log_viewer_close();

  /* ── Remove the tray icon ─────────────────────────────────── */
  remove_tray_icon(ctx);

  /* ── Unregister session notifications ─────────────────────── */
  if (ctx->hwnd) {
    WTSUnRegisterSessionNotification(ctx->hwnd);
  }

  /* ── Destroy the hidden window ────────────────────────────── */
  if (ctx->hwnd) {
    DestroyWindow(ctx->hwnd);
    ctx->hwnd = NULL;
  }

  /* ── Clean up pipe clients ────────────────────────────────── */
  EnterCriticalSection(&ctx->lock);
  for (unsigned int i = 0; i < ctx->serviceCount; i++) {
    pipe_client_cleanup(&ctx->services[i].client);
  }
  ctx->serviceCount = 0;
  LeaveCriticalSection(&ctx->lock);
  DeleteCriticalSection(&ctx->lock);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  WM_COMMAND Handler                                                */
/* ══════════════════════════════════════════════════════════════════ */

static void tray_on_command(TrayAppCtx *ctx, WPARAM wParam) {
  UINT id = LOWORD(wParam);

  switch (id) {
  case IDM_OPEN_STATUS:
    LOG_I(LOG_TAG, "Menu: Open Status Window");
    tray_status_dlg_show(ctx, ctx->hInstance);
    break;

  case IDM_RELOAD_CONFIG: {
    LOG_I(LOG_TAG, "Menu: Reload All Configurations");

    EnterCriticalSection(&ctx->lock);
    for (unsigned int i = 0; i < ctx->serviceCount; i++) {
      TrayServiceInfo *svc = &ctx->services[i];
      if (!svc->connected)
        continue;

      PipeRequest req;
      memset(&req, 0, sizeof(req));
      req.command = PIPE_CMD_RELOAD_CONFIG;

      PipeResponse resp;
      memset(&resp, 0, sizeof(resp));

      if (pipe_client_send_request(&svc->client, &req, &resp, 5000) != 0) {
        LOG_W(LOG_TAG, "Reload config failed for '%s'", svc->serviceName);
      }
    }
    LeaveCriticalSection(&ctx->lock);
    break;
  }

  case IDM_VIEW_LOG:
    LOG_I(LOG_TAG, "Menu: View Service Log");
    {
      char logPath[MAX_PATH];
      snprintf(logPath, sizeof(logPath),
               "C:\\ProgramData\\OmniRDP\\logs\\OmniRDP-svc.log");
      tray_log_viewer_show(ctx->hInstance, logPath);
    }
    break;

  case IDM_INSTALL_SERVICE:
    LOG_I(LOG_TAG, "Menu: Install Service");
    {
      /* Find OmniRDP-svc.exe in the same directory as the tray app */
      char exePath[MAX_PATH];
      GetModuleFileNameA(NULL, exePath, MAX_PATH);
      /* Replace tray exe name with service exe name */
      char *lastSlash = strrchr(exePath, '\\');
      if (lastSlash) {
        strcpy(lastSlash + 1, "OmniRDP-svc.exe");
      }

      SHELLEXECUTEINFOA sei = {0};
      sei.cbSize = sizeof(sei);
      sei.fMask = SEE_MASK_NOCLOSEPROCESS;
      sei.lpVerb = "runas"; /* Request elevation */
      sei.lpFile = exePath;
      sei.lpParameters = "--install";
      sei.nShow = SW_SHOWNORMAL;

      if (ShellExecuteExA(&sei)) {
        LOG_I(LOG_TAG, "Service installation launched successfully");
        if (sei.hProcess) {
          /* Wait for installation to complete */
          WaitForSingleObject(sei.hProcess, 30000);
          CloseHandle(sei.hProcess);
        }
        /* Refresh service discovery after installation */
        tray_icon_discover_services(ctx);
        tray_icon_update(ctx);
      } else {
        LOG_W(LOG_TAG,
              "Service installation failed or was cancelled (error %lu)",
              GetLastError());
      }
    }
    break;

  case IDM_ABOUT: {
    LOG_I(LOG_TAG, "Menu: About");
    MessageBoxA(NULL,
                "OmniRDP - RDP Multiplexer Service\n"
                "Version 1.0.0\n\n"
                "Manages multiple RDP gateway instances.",
                "About OmniRDP", MB_OK | MB_ICONINFORMATION);
    break;
  }

  case IDM_EXIT:
    LOG_I(LOG_TAG, "Menu: Exit");
    ctx->running = FALSE;
    PostQuitMessage(0);
    break;

  default: {
    /*
     * Dynamic instance action IDs.
     * Decode: id = IDM_SERVICE_BASE
     *             + svcIdx * IDM_SERVICE_STRIDE
     *             + instIdx * IDM_INSTANCE_SLOT_SIZE
     *             + actionOffset
     */
    if (id >= IDM_SERVICE_BASE) {
      UINT raw = id - IDM_SERVICE_BASE;
      UINT svcIdx = raw / IDM_SERVICE_STRIDE;
      UINT remainder = raw % IDM_SERVICE_STRIDE;
      UINT instIdx = remainder / IDM_INSTANCE_SLOT_SIZE;
      UINT action = remainder % IDM_INSTANCE_SLOT_SIZE;

      EnterCriticalSection(&ctx->lock);

      if (svcIdx < ctx->serviceCount) {
        TrayServiceInfo *svc = &ctx->services[svcIdx];

        if (instIdx < svc->instanceCount && svc->connected) {
          const char *instName = svc->instances[instIdx].name;
          PipeCommand cmd = PIPE_CMD_COUNT; /* invalid */

          switch (action) {
          case IDM_INSTANCE_ACTION_START:
            cmd = PIPE_CMD_START_INSTANCE;
            LOG_I(LOG_TAG, "Menu: Start '%s' on '%s'", instName,
                  svc->serviceName);
            break;

          case IDM_INSTANCE_ACTION_STOP:
            cmd = PIPE_CMD_STOP_INSTANCE;
            LOG_I(LOG_TAG, "Menu: Stop '%s' on '%s'", instName,
                  svc->serviceName);
            break;

          case IDM_INSTANCE_ACTION_RESTART:
            cmd = PIPE_CMD_RESTART_INSTANCE;
            LOG_I(LOG_TAG, "Menu: Restart '%s' on '%s'", instName,
                  svc->serviceName);
            break;
          }

          if (cmd != PIPE_CMD_COUNT) {
            PipeRequest req;
            memset(&req, 0, sizeof(req));
            req.command = cmd;
            strncpy(req.instance_name, instName, sizeof(req.instance_name) - 1);
            req.instance_name[sizeof(req.instance_name) - 1] = '\0';

            PipeResponse resp;
            memset(&resp, 0, sizeof(resp));

            if (pipe_client_send_request(&svc->client, &req, &resp, 5000) !=
                0) {
              LOG_W(LOG_TAG, "Action failed for '%s' on '%s'", instName,
                    svc->serviceName);
            }
          }
        }
      }

      LeaveCriticalSection(&ctx->lock);
    }
    break;
  }
  }
}
