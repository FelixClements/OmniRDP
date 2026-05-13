/**
 * @file tray_status_dlg.c
 * @brief Status dialog for the OmniRDP tray application
 *
 * Implements a modeless dialog window that displays a ListView table
 * of all managed RDP instances across all discovered OmniRDP services.
 * Provides Start / Stop / Restart / Refresh / Close buttons.
 *
 * This file is FreeRDP-independent and Windows-only.
 */

#define _WIN32_IE 0x0501 /* Required for ListView macros in commctrl.h */

#include "tray_status_dlg.h"
#include "pipe_protocol.h"
#include "svc_log.h"
#include "tray_pipe_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <commctrl.h>
#include <stdio.h>
#include <string.h>

/* ── Log tag ───────────────────────────────────────────────────── */
#define LOG_TAG "status_dlg"

/* ── Child control IDs ─────────────────────────────────────────── */
#define IDC_LISTVIEW 1000
#define IDC_START 1001
#define IDC_STOP 1002
#define IDC_RESTART 1003
#define IDC_REFRESH 1004
#define IDC_CLOSE 1005

/* ── lParam packing ────────────────────────────────────────────── */
#define LPARAM_SERVICE_SHIFT 16
#define LPARAM_INSTANCE_MASK 0xFFFF

/* ── Static state ──────────────────────────────────────────────── */

/** Handle to the single status dialog instance (modeless). */
static HWND g_hDlg = NULL;

/* ── Forward declarations ──────────────────────────────────────── */

static LRESULT CALLBACK status_dlg_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam);

/* ══════════════════════════════════════════════════════════════════ */
/*  Helpers                                                            */
/* ══════════════════════════════════════════════════════════════════ */

/**
 * @brief Convert a PipeInstanceState enum to a human-readable string.
 */
static const char *state_to_string(PipeInstanceState state) {
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

/**
 * @brief Populate (or re-populate) the ListView with current data.
 *
 * Clears all existing items, then walks every service and its
 * managed instances and adds a row for each.
 *
 * @param hListView  Handle to the ListView control
 * @param ctx        Tray app context (locked by caller)
 */
static void populate_listview(HWND hListView, const TrayAppCtx *ctx) {
  int si, ii;

  if (!hListView || !ctx)
    return;

  ListView_DeleteAllItems(hListView);

  for (si = 0; si < ctx->serviceCount; si++) {
    const TrayServiceInfo *svc = &ctx->services[si];

    for (ii = 0; ii < svc->instanceCount; ii++) {
      const PipeInstanceInfo *inst = &svc->instances[ii];
      char serviceName[256];
      char instanceName[128];
      char stateText[32];
      char viewers[32];
      char backend[320];
      int itemIndex;
      LPARAM lParam;
      LVITEMA lvi;

      /* Pack service & instance indices into lParam */
      lParam =
          (LPARAM)((si << LPARAM_SERVICE_SHIFT) | (ii & LPARAM_INSTANCE_MASK));

      _snprintf(serviceName, sizeof(serviceName), "%s", svc->serviceName);
      serviceName[sizeof(serviceName) - 1] = '\0';

      _snprintf(instanceName, sizeof(instanceName), "%s", inst->name);
      instanceName[sizeof(instanceName) - 1] = '\0';

      _snprintf(stateText, sizeof(stateText), "%s",
                state_to_string(inst->state));
      stateText[sizeof(stateText) - 1] = '\0';

      /* Viewer column */
      if (inst->state == INSTANCE_STOPPED)
        snprintf(viewers, sizeof(viewers), "-");
      else {
        _snprintf(viewers, sizeof(viewers), "%lu/10", inst->viewer_count);
        viewers[sizeof(viewers) - 1] = '\0';
      }

      /* Backend column */
      _snprintf(backend, sizeof(backend), "%s:%u", inst->backend_hostname,
                inst->backend_port);
      backend[sizeof(backend) - 1] = '\0';

      itemIndex = ListView_GetItemCount(hListView);

      lvi.mask = LVIF_TEXT | LVIF_PARAM;
      lvi.iItem = itemIndex;
      lvi.iSubItem = 0;
      lvi.pszText = serviceName;
      lvi.lParam = lParam;

      ListView_InsertItem(hListView, &lvi);

      /* Sub-item columns */
      ListView_SetItemText(hListView, itemIndex, 1, instanceName);
      ListView_SetItemText(hListView, itemIndex, 2, stateText);
      ListView_SetItemText(hListView, itemIndex, 3, viewers);
      ListView_SetItemText(hListView, itemIndex, 4, backend);

      /* Viewer Port column */
      {
        char cell[32];
        if (inst->viewer_port > 0) {
          snprintf(cell, sizeof(cell), "%u", inst->viewer_port);
          ListView_SetItemText(hListView, itemIndex, 5, cell);
        }
      }
    }
  }
}

/**
 * @brief Enable / disable Start / Stop / Restart buttons based on the
 *        currently selected item's instance state.
 */
static void update_button_states(HWND hwnd, TrayAppCtx *ctx) {
  HWND hListView;
  int selIndex;
  BOOL hasSelection;
  BOOL canStart = FALSE;
  BOOL canStop = FALSE;
  BOOL canRestart = FALSE;

  hListView = GetDlgItem(hwnd, IDC_LISTVIEW);
  selIndex = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);

  hasSelection = (selIndex != -1);

  if (hasSelection && ctx) {
    LVITEMA lvi;

    lvi.mask = LVIF_PARAM;
    lvi.iItem = selIndex;
    lvi.iSubItem = 0;

    if (ListView_GetItem(hListView, &lvi)) {
      int si = (int)(lvi.lParam >> LPARAM_SERVICE_SHIFT);
      int ii = (int)(lvi.lParam & LPARAM_INSTANCE_MASK);

      if (si >= 0 && si < ctx->serviceCount && ii >= 0 &&
          ii < ctx->services[si].instanceCount) {
        const PipeInstanceState state = ctx->services[si].instances[ii].state;

        canStart = (state == INSTANCE_STOPPED);
        canStop = (state == INSTANCE_RUNNING || state == INSTANCE_RECONNECTING);
        canRestart = canStop;
      }
    }
  }

  EnableWindow(GetDlgItem(hwnd, IDC_START), canStart);
  EnableWindow(GetDlgItem(hwnd, IDC_STOP), canStop);
  EnableWindow(GetDlgItem(hwnd, IDC_RESTART), canRestart);
}

/**
 * @brief Send a pipe command (start / stop / restart) for the
 *        currently selected ListView item.
 */
static void send_instance_command(HWND hwnd, TrayAppCtx *ctx, PipeCommand cmd) {
  HWND hListView;
  int selIndex;
  LVITEMA lvi;
  int si, ii;
  PipeRequest req;
  PipeResponse resp;
  const char *cmdName;

  if (!ctx)
    return;

  hListView = GetDlgItem(hwnd, IDC_LISTVIEW);
  selIndex = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);

  if (selIndex == -1)
    return;

  lvi.mask = LVIF_PARAM;
  lvi.iItem = selIndex;
  lvi.iSubItem = 0;

  if (!ListView_GetItem(hListView, &lvi))
    return;

  si = (int)(lvi.lParam >> LPARAM_SERVICE_SHIFT);
  ii = (int)(lvi.lParam & LPARAM_INSTANCE_MASK);

  if (si < 0 || si >= ctx->serviceCount || ii < 0 ||
      ii >= ctx->services[si].instanceCount)
    return;

  /* Build the request */
  memset(&req, 0, sizeof(req));
  req.command = cmd;
  snprintf(req.instance_name, sizeof(req.instance_name), "%s",
           ctx->services[si].instances[ii].name);

  memset(&resp, 0, sizeof(resp));

  /* Friendly name for logging */
  switch (cmd) {
  case PIPE_CMD_START_INSTANCE:
    cmdName = "start";
    break;
  case PIPE_CMD_STOP_INSTANCE:
    cmdName = "stop";
    break;
  case PIPE_CMD_RESTART_INSTANCE:
    cmdName = "restart";
    break;
  default:
    cmdName = "unknown";
    break;
  }

  LOG_I(LOG_TAG, "Sending '%s' command for instance '%s'", cmdName,
        req.instance_name);

  if (pipe_client_send_request(&ctx->services[si].client, &req, &resp, 0) !=
      0) {
    LOG_E(LOG_TAG, "Failed to send '%s' command for '%s'", cmdName,
          req.instance_name);
    return;
  }

  if (!resp.success) {
    LOG_W(LOG_TAG, "'%s' command failed for '%s': %s", cmdName,
          req.instance_name, resp.error_message);
  } else {
    LOG_I(LOG_TAG, "'%s' command succeeded for '%s'", cmdName,
          req.instance_name);
  }
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Window Procedure                                                   */
/* ══════════════════════════════════════════════════════════════════ */

static LRESULT CALLBACK status_dlg_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam) {
  switch (msg) {
  /* ── WM_CREATE ──────────────────────────────────────────── */
  case WM_CREATE: {
    CREATESTRUCTA *cs = (CREATESTRUCTA *)lParam;
    TrayAppCtx *ctx = (TrayAppCtx *)cs->lpCreateParams;
    HWND hListView;
    LVCOLUMNA col;

    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);

    /* ── ListView ────────────────────────────────────────── */
    hListView = CreateWindowExA(
        0, WC_LISTVIEW, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        0, 0, 100, 100, hwnd, (HMENU)(INT_PTR)IDC_LISTVIEW, cs->hInstance,
        NULL);

    /* Full-row highlight */
    ListView_SetExtendedListViewStyle(hListView, LVS_EX_FULLROWSELECT);

    /* Columns */
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.cx = 120;
    col.pszText = "Service";
    ListView_InsertColumn(hListView, 0, &col);

    col.cx = 150;
    col.pszText = "Instance";
    ListView_InsertColumn(hListView, 1, &col);

    col.cx = 100;
    col.pszText = "State";
    ListView_InsertColumn(hListView, 2, &col);

    col.cx = 80;
    col.pszText = "Viewers";
    ListView_InsertColumn(hListView, 3, &col);

    col.cx = 150;
    col.pszText = "Backend";
    ListView_InsertColumn(hListView, 4, &col);

    col.cx = 80;
    col.pszText = "Viewer";
    ListView_InsertColumn(hListView, 5, &col);

    /* ── Buttons ─────────────────────────────────────────── */
    CreateWindowExA(0, "BUTTON", "Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 70, 25, hwnd, (HMENU)(INT_PTR)IDC_START,
                    cs->hInstance, NULL);

    CreateWindowExA(0, "BUTTON", "Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 70, 25, hwnd, (HMENU)(INT_PTR)IDC_STOP, cs->hInstance,
                    NULL);

    CreateWindowExA(0, "BUTTON", "Restart",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 70, 25, hwnd,
                    (HMENU)(INT_PTR)IDC_RESTART, cs->hInstance, NULL);

    CreateWindowExA(0, "BUTTON", "Refresh",
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 70, 25, hwnd,
                    (HMENU)(INT_PTR)IDC_REFRESH, cs->hInstance, NULL);

    CreateWindowExA(0, "BUTTON", "Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    0, 0, 70, 25, hwnd, (HMENU)(INT_PTR)IDC_CLOSE,
                    cs->hInstance, NULL);

    /* ── Initial population ──────────────────────────────── */
    if (ctx) {
      EnterCriticalSection(&ctx->lock);
      populate_listview(hListView, ctx);
      LeaveCriticalSection(&ctx->lock);
    }

    /* Action buttons start disabled (nothing selected yet) */
    EnableWindow(GetDlgItem(hwnd, IDC_START), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_STOP), FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_RESTART), FALSE);

    return 0;
  }

  /* ── WM_SIZE ────────────────────────────────────────────── */
  case WM_SIZE: {
    int width = LOWORD(lParam);
    int height = HIWORD(lParam);
    int btnHeight = 25;
    int btnY;
    int listHeight;
    int btnCount = 5;
    int btnWidth = 70;
    int gap = 8;
    int totalWidth;
    int startX;
    HWND hListView;

    btnY = height - btnHeight - 5;
    listHeight = (btnY > 5) ? (btnY - 5) : 0;

    hListView = GetDlgItem(hwnd, IDC_LISTVIEW);
    if (hListView)
      SetWindowPos(hListView, NULL, 0, 0, width, listHeight, SWP_NOZORDER);

    /* Centre buttons horizontally */
    totalWidth = btnCount * btnWidth + (btnCount - 1) * gap;
    startX = (width - totalWidth) / 2;
    if (startX < 5)
      startX = 5;

    SetWindowPos(GetDlgItem(hwnd, IDC_START), NULL, startX, btnY, btnWidth,
                 btnHeight, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_STOP), NULL,
                 startX + 1 * (btnWidth + gap), btnY, btnWidth, btnHeight,
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_RESTART), NULL,
                 startX + 2 * (btnWidth + gap), btnY, btnWidth, btnHeight,
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_REFRESH), NULL,
                 startX + 3 * (btnWidth + gap), btnY, btnWidth, btnHeight,
                 SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_CLOSE), NULL,
                 startX + 4 * (btnWidth + gap), btnY, btnWidth, btnHeight,
                 SWP_NOZORDER);

    return 0;
  }

  /* ── WM_NOTIFY (ListView notifications) ─────────────────── */
  case WM_NOTIFY: {
    LPNMHDR nmhdr = (LPNMHDR)lParam;

    if (nmhdr->idFrom == IDC_LISTVIEW && nmhdr->code == LVN_ITEMCHANGED) {
      TrayAppCtx *ctx = (TrayAppCtx *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
      update_button_states(hwnd, ctx);
    }
    return 0;
  }

  /* ── WM_COMMAND (button clicks) ─────────────────────────── */
  case WM_COMMAND: {
    int cmdId = LOWORD(wParam);
    TrayAppCtx *ctx = (TrayAppCtx *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (cmdId) {
    case IDC_START:
      send_instance_command(hwnd, ctx, PIPE_CMD_START_INSTANCE);
      break;

    case IDC_STOP:
      send_instance_command(hwnd, ctx, PIPE_CMD_STOP_INSTANCE);
      break;

    case IDC_RESTART:
      send_instance_command(hwnd, ctx, PIPE_CMD_RESTART_INSTANCE);
      break;

    case IDC_REFRESH:
      if (ctx) {
        tray_icon_refresh_instances(ctx);
        tray_status_dlg_refresh(ctx);
      }
      break;

    case IDC_CLOSE:
      DestroyWindow(hwnd);
      break;
    }
    return 0;
  }

  /* ── WM_CLOSE ───────────────────────────────────────────── */
  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;

  /* ── WM_DESTROY ─────────────────────────────────────────── */
  case WM_DESTROY:
    g_hDlg = NULL;
    return 0;
  }

  return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Public API                                                         */
/* ══════════════════════════════════════════════════════════════════ */

HWND tray_status_dlg_show(TrayAppCtx *ctx, HINSTANCE hInstance) {
  HWND hwnd;

  /* If already open, just bring it to the foreground */
  if (g_hDlg && IsWindow(g_hDlg)) {
    SetForegroundWindow(g_hDlg);
    return g_hDlg;
  }

  /* ── Register window class ──────────────────────────────────── */
  {
    WNDCLASSEXA wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = status_dlg_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "OmniRDPStatusDlg";

    RegisterClassExA(&wc); /* Ignore error if already registered */
  }

  /* ── Ensure common controls are available (ListView) ────────── */
  {
    INITCOMMONCONTROLSEX icc;

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
  }

  /* ── Create the modeless dialog window ──────────────────────── */
  hwnd = CreateWindowExA(
      0, "OmniRDPStatusDlg", "OmniRDP Status", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, 700, 300, NULL, /* no parent */
      NULL,                                         /* no menu */
      hInstance, (LPVOID)ctx /* passed to WM_CREATE via CREATESTRUCT */
  );

  if (!hwnd) {
    LOG_E(LOG_TAG, "Failed to create status dialog: %lu", GetLastError());
    return NULL;
  }

  g_hDlg = hwnd;

  ShowWindow(hwnd, SW_SHOW);

  LOG_I(LOG_TAG, "Status dialog shown");
  return hwnd;
}

void tray_status_dlg_refresh(TrayAppCtx *ctx) {
  HWND hListView;

  if (!g_hDlg || !IsWindow(g_hDlg) || !ctx)
    return;

  hListView = GetDlgItem(g_hDlg, IDC_LISTVIEW);
  if (!hListView)
    return;

  EnterCriticalSection(&ctx->lock);
  populate_listview(hListView, ctx);
  LeaveCriticalSection(&ctx->lock);

  update_button_states(g_hDlg, ctx);
}

void tray_status_dlg_close(void) {
  if (g_hDlg && IsWindow(g_hDlg))
    DestroyWindow(g_hDlg);

  g_hDlg = NULL;
}

BOOL tray_status_dlg_is_visible(void) {
  return (g_hDlg && IsWindowVisible(g_hDlg)) ? TRUE : FALSE;
}

HWND tray_status_dlg_get_hwnd(void) { return g_hDlg; }
