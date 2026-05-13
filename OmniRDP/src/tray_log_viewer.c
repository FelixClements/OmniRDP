#include "tray_log_viewer.h"
#include "pipe_protocol.h"
#include <stdio.h>
#include <string.h>

#define LOG_VIEWER_TIMER_ID 1
#define LOG_VIEWER_REFRESH_MS 2000

static HWND g_hLogViewer = NULL;
static HWND g_hEdit = NULL;
static PipeClient *g_client = NULL;
static BOOL g_showingError = FALSE;

static void load_log_content(void) {
  if (!g_hEdit)
    return;

  if (!g_client) {
    if (!g_showingError) {
      SetWindowTextA(g_hEdit, "No service connected.");
      g_showingError = TRUE;
    }
    return;
  }

  PipeRequest req;
  memset(&req, 0, sizeof(req));
  req.command = PIPE_CMD_GET_LOGS;

  PipeResponse resp;
  memset(&resp, 0, sizeof(resp));

  if (pipe_client_send_request(g_client, &req, &resp, 5000) != 0) {
    if (!g_showingError) {
      SetWindowTextA(g_hEdit,
                     "Cannot retrieve logs \u2014 service may be busy. Try "
                     "again in a few seconds.");
      g_showingError = TRUE;
    }
    return;
  }

  if (!resp.success) {
    if (!g_showingError) {
      SetWindowTextA(g_hEdit, "Service returned an error retrieving logs.");
      g_showingError = TRUE;
    }
    return;
  }

  /* Parse the JSON payload to extract log lines */
  const char *payload = resp.json_payload;
  if (!payload || payload[0] == '\0') {
    SetWindowTextA(g_hEdit, "(empty log)");
    g_showingError = FALSE;
    return;
  }

  /* Build display text from the JSON logs array.
   *
   * The response json_payload contains the raw value from the
   * "json_payload" JSON field.  For GET_LOGS this looks like:
   *
   *   "{\"logs\":[\"line1\",\"line2\",...]}"
   *
   * The outer quotes and internal backslash-escaping are present
   * because the json_payload value is itself a JSON-encoded string.
   * We handle both escaped (\"logs\":[) and unescaped ("logs":[)
   * forms. */
  char *displayBuf = (char *)HeapAlloc(GetProcessHeap(), 0, 64 * 1024);
  if (!displayBuf) {
    SetWindowTextA(g_hEdit, "Out of memory.");
    return;
  }

  displayBuf[0] = '\0';
  size_t outPos = 0;
  size_t outMax = 64 * 1024 - 1;

  /* Find the logs array — try escaped form first, then unescaped */
  const char *arrStart = strstr(payload, "\\\"logs\\\":[");
  if (!arrStart) {
    arrStart = strstr(payload, "\"logs\":[");
  }

  if (!arrStart) {
    /* Fallback: show raw payload */
    snprintf(displayBuf, outMax, "%s", payload);
    SetWindowTextA(g_hEdit, displayBuf);
    HeapFree(GetProcessHeap(), 0, displayBuf);
    return;
  }

  /* Skip to content after the opening [ */
  const char *p = arrStart;
  while (*p && *p != '[')
    p++;
  if (*p == '[')
    p++;

  /* Parse array of JSON strings. Each element is "text" or \"text\" */
  while (*p && outPos < outMax - 1) {
    /* Skip whitespace */
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
      p++;
    if (*p == ']' || *p == '\0')
      break;

    /* Skip optional backslash before opening quote */
    if (*p == '\\')
      p++;
    if (*p != '"')
      break;
    p++; /* skip opening quote */

    /* Copy string content until closing quote, handling JSON escapes */
    while (*p && *p != '"' && outPos < outMax - 1) {
      if (*p == '\\' && *(p + 1)) {
        p++; /* skip backslash */
        switch (*p) {
        case '"':
          displayBuf[outPos++] = '"';
          break;
        case '\\':
          displayBuf[outPos++] = '\\';
          break;
        case 'n':
          displayBuf[outPos++] = '\n';
          break;
        case 'r':
          displayBuf[outPos++] = '\r';
          break;
        case 't':
          displayBuf[outPos++] = '\t';
          break;
        default:
          displayBuf[outPos++] = *p;
          break;
        }
        p++;
      } else {
        displayBuf[outPos++] = *p;
        p++;
      }
    }
    if (*p == '"')
      p++; /* skip closing quote */

    /* Add CRLF line separator */
    if (outPos + 2 <= outMax) {
      displayBuf[outPos++] = '\r';
      displayBuf[outPos++] = '\n';
    }

    /* Skip past comma, whitespace, and optional backslash (escaped form) */
    while (*p && (*p == ',' || *p == ' ' || *p == '\t'))
      p++;
  }

  displayBuf[outPos] = '\0';

  SetWindowTextA(g_hEdit, displayBuf);
  g_showingError = FALSE;
  /* Scroll to end */
  SendMessage(g_hEdit, EM_SETSEL, (WPARAM)outPos, (LPARAM)outPos);
  SendMessage(g_hEdit, EM_SCROLLCARET, 0, 0);
  HeapFree(GetProcessHeap(), 0, displayBuf);
}

static LRESULT CALLBACK log_viewer_wndproc(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam) {
  switch (msg) {
  case WM_CREATE:
    g_hEdit = CreateWindowExA(
        WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
            ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
        0, 0, 0, 0, hwnd, NULL, GetModuleHandle(NULL), NULL);
    /* Set monospace font */
    {
      HFONT hFont =
          CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
      if (hFont)
        SendMessage(g_hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    g_showingError = FALSE;
    load_log_content();
    SetTimer(hwnd, LOG_VIEWER_TIMER_ID, LOG_VIEWER_REFRESH_MS, NULL);
    return 0;

  case WM_SIZE:
    if (g_hEdit) {
      MoveWindow(g_hEdit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
    }
    return 0;

  case WM_TIMER:
    if (wParam == LOG_VIEWER_TIMER_ID) {
      load_log_content();
    }
    return 0;

  case WM_CLOSE:
    KillTimer(hwnd, LOG_VIEWER_TIMER_ID);
    DestroyWindow(hwnd);
    return 0;

  case WM_DESTROY:
    g_hLogViewer = NULL;
    g_hEdit = NULL;
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wParam, lParam);
}

HWND tray_log_viewer_show(HINSTANCE hInstance, PipeClient *client) {
  if (g_hLogViewer) {
    SetForegroundWindow(g_hLogViewer);
    return g_hLogViewer;
  }

  g_client = client;

  /* Register window class */
  static BOOL registered = FALSE;
  if (!registered) {
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = log_viewer_wndproc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "OmniRDPLogViewer";
    RegisterClassExA(&wc);
    registered = TRUE;
  }

  g_hLogViewer =
      CreateWindowExA(WS_EX_OVERLAPPEDWINDOW, "OmniRDPLogViewer",
                      "OmniRDP Service Log", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                      CW_USEDEFAULT, 800, 600, NULL, NULL, hInstance, NULL);

  if (g_hLogViewer) {
    ShowWindow(g_hLogViewer, SW_SHOW);
    UpdateWindow(g_hLogViewer);
  }

  return g_hLogViewer;
}

void tray_log_viewer_close(void) {
  if (g_hLogViewer) {
    DestroyWindow(g_hLogViewer);
  }
}

BOOL tray_log_viewer_is_visible(void) { return g_hLogViewer != NULL; }
