#include "tray_log_viewer.h"
#include <stdio.h>
#include <string.h>

#define LOG_VIEWER_TIMER_ID 1
#define LOG_VIEWER_REFRESH_MS 2000

static HWND g_hLogViewer = NULL;
static HWND g_hEdit = NULL;
static char g_logPath[MAX_PATH] = {0};

static void load_log_content(void) {
  if (!g_hEdit || g_logPath[0] == '\0')
    return;

  FILE *fp = NULL;
  if (fopen_s(&fp, g_logPath, "r") != 0 || !fp) {
    SetWindowTextA(g_hEdit, "Log file not found or cannot be opened.");
    return;
  }

  /* Get file size */
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    SetWindowTextA(g_hEdit, "Log file cannot be read.");
    return;
  }
  long fileSize = ftell(fp);
  if (fileSize < 0 || fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    SetWindowTextA(g_hEdit, "Log file cannot be read.");
    return;
  }

  /* Read last 64KB max */
  long maxSize = 64 * 1024;
  long skip = 0;
  if (fileSize > maxSize) {
    skip = fileSize - maxSize;
    if (fseek(fp, skip, SEEK_SET) != 0) {
      fclose(fp);
      SetWindowTextA(g_hEdit, "Log file cannot be read.");
      return;
    }
    /* Skip to next newline within the bounded 64KB window. */
    long remaining = maxSize;
    char lineBuf[512];
    while (remaining > 0 && fgets(lineBuf, sizeof(lineBuf), fp) != NULL) {
      size_t consumed = strnlen_s(lineBuf, sizeof(lineBuf));
      const char *newline = strchr(lineBuf, '\n');
      if ((long)consumed > remaining)
        consumed = (size_t)remaining;
      skip += (long)consumed;
      remaining -= (long)consumed;
      if (newline)
        break;
    }
    if (ferror(fp)) {
      fclose(fp);
      SetWindowTextA(g_hEdit, "Log file cannot be read.");
      return;
    }
  }

  /* Read content */
  long readSize = fileSize - skip;
  if (readSize < 0)
    readSize = 0;
  char *content = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)readSize + 1);
  if (content) {
    size_t read = fread(content, 1, (size_t)readSize, fp);
    content[read] = '\0';
    SetWindowTextA(g_hEdit, content);
    /* Scroll to end */
    SendMessage(g_hEdit, EM_SETSEL, (WPARAM)read, (LPARAM)read);
    SendMessage(g_hEdit, EM_SCROLLCARET, 0, 0);
    HeapFree(GetProcessHeap(), 0, content);
  }

  fclose(fp);
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

HWND tray_log_viewer_show(HINSTANCE hInstance, const char *logPath) {
  if (g_hLogViewer) {
    SetForegroundWindow(g_hLogViewer);
    return g_hLogViewer;
  }

  snprintf(g_logPath, sizeof(g_logPath), "%s", logPath ? logPath : "");

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
