/**
 * @file tray_main.c
 * @brief Entry point for OmniRDP-tray.exe (System Tray Application)
 *
 * Runs in the user session. Connects to the OmniRDP service via named
 * pipe IPC, shows a system tray icon reflecting instance states, and
 * provides a context menu and status window for instance management.
 *
 * Invocation:
 *   OmniRDP-tray.exe              Run normally (auto-start or manual)
 *   OmniRDP-tray.exe --install    Register in HKLM Run key for auto-start
 *   OmniRDP-tray.exe --uninstall  Remove from HKLM Run key
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include "tray_icon.h"
#include "tray_status_dlg.h"
#include "tray_pipe_client.h"
#include "svc_log.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define IDM_OPEN_STATUS     1001
#define IDM_RELOAD_CONFIG   1002
#define IDM_VIEW_LOG        1003
#define IDM_ABOUT           1004
#define IDM_EXIT            1005
#define IDM_SERVICE_BASE    2000

/* ── Global context ──────────────────────────────────────────────── */

static TrayAppCtx g_ctx;

/* ── Registry helpers for auto-start ────────────────────────────── */

static const char *RUN_KEY_PATH =
    "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char *RUN_VALUE_NAME = "OmniRDP-Tray";

static int install_autostart(void) {
    HKEY hKey;
    LONG err = RegOpenKeyExA(HKEY_LOCAL_MACHINE, RUN_KEY_PATH, 0,
                              KEY_SET_VALUE, &hKey);
    if (err != ERROR_SUCCESS) {
        fprintf(stderr, "Failed to open Run key (error %ld)\n", err);
        return -1;
    }

    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    err = RegSetValueExA(hKey, RUN_VALUE_NAME, 0, REG_SZ,
                         (const BYTE *)exePath, (DWORD)strlen(exePath) + 1);
    RegCloseKey(hKey);

    if (err != ERROR_SUCCESS) {
        fprintf(stderr, "Failed to set Run value (error %ld)\n", err);
        return -1;
    }

    printf("Registered OmniRDP-Tray for auto-start.\n");
    return 0;
}

static int uninstall_autostart(void) {
    HKEY hKey;
    LONG err = RegOpenKeyExA(HKEY_LOCAL_MACHINE, RUN_KEY_PATH, 0,
                              KEY_SET_VALUE, &hKey);
    if (err != ERROR_SUCCESS) {
        fprintf(stderr, "Failed to open Run key (error %ld)\n", err);
        return -1;
    }

    err = RegDeleteValueA(hKey, RUN_VALUE_NAME);
    RegCloseKey(hKey);

    if (err != ERROR_SUCCESS && err != ERROR_FILE_NOT_FOUND) {
        fprintf(stderr, "Failed to delete Run value (error %ld)\n", err);
        return -1;
    }

    printf("Removed OmniRDP-Tray from auto-start.\n");
    return 0;
}

/* ── Usage ──────────────────────────────────────────────────────── */

static void print_usage(const char *program) {
    printf("OmniRDP Tray Application\n\n");
    printf("Usage:\n");
    printf("  %s              Run the tray application\n", program);
    printf("  %s --install    Register for auto-start on login\n", program);
    printf("  %s --uninstall  Remove from auto-start\n", program);
}

/* ── Main ────────────────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    /* Parse command line */
    if (lpCmdLine && strstr(lpCmdLine, "--install") != NULL) {
        return install_autostart();
    }
    if (lpCmdLine && strstr(lpCmdLine, "--uninstall") != NULL) {
        return uninstall_autostart();
    }
    if (lpCmdLine && (strstr(lpCmdLine, "--help") != NULL ||
                      strstr(lpCmdLine, "-h") != NULL)) {
        print_usage("OmniRDP-tray.exe");
        return 0;
    }

    /* Prevent multiple instances */
    HANDLE hMutex = CreateMutexA(NULL, TRUE, "OmniRDP-Tray-Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* Another instance is already running */
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    /* Initialize common controls (for ListView) */
    INITCOMMONCONTROLSEX icc = {
        .dwSize = sizeof(icc),
        .dwICC = ICC_LISTVIEW_CLASSES
    };
    InitCommonControlsEx(&icc);

    /* Initialize logging */
    svc_log_init("C:\\ProgramData\\OmniRDP\\logs", SVC_LOG_INFO, 10, 5);
    LOG_I("tray", "OmniRDP Tray starting");

    /* Initialize tray context */
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.running = TRUE;
    InitializeCriticalSection(&g_ctx.lock);

    /* Initialize tray icon */
    if (tray_icon_init(&g_ctx, hInstance) != 0) {
        LOG_E("tray", "Failed to initialize tray icon");
        DeleteCriticalSection(&g_ctx.lock);
        svc_log_shutdown();
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    /* Discover services and connect */
    tray_icon_discover_services(&g_ctx);
    tray_icon_refresh_instances(&g_ctx);
    tray_icon_update(&g_ctx);

    /* First-time installer prompt: if no services found, offer to install */
    if (g_ctx.serviceCount == 0) {
        int result = MessageBoxA(
            NULL,
            "OmniRDP service is not installed.\n\n"
            "Would you like to install it now?",
            "OmniRDP",
            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND
        );
        if (result == IDYES) {
            /* Find OmniRDP-svc.exe in the same directory */
            char exePath[MAX_PATH];
            GetModuleFileNameA(NULL, exePath, MAX_PATH);
            char *lastSlash = strrchr(exePath, '\\');
            if (lastSlash) {
                strcpy(lastSlash + 1, "OmniRDP-svc.exe");
            }

            SHELLEXECUTEINFOA sei = {0};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = "runas";
            sei.lpFile = exePath;
            sei.lpParameters = "--install";
            sei.nShow = SW_SHOWNORMAL;

            if (ShellExecuteExA(&sei)) {
                if (sei.hProcess) {
                    WaitForSingleObject(sei.hProcess, 30000);
                    CloseHandle(sei.hProcess);
                }
                /* Re-discover services after installation */
                tray_icon_discover_services(&g_ctx);
                tray_icon_refresh_instances(&g_ctx);
                tray_icon_update(&g_ctx);
            }
        }
    }

    /* If service is running but has no instances, offer to open config */
    if (g_ctx.serviceCount > 0) {
        BOOL anyInstances = FALSE;
        for (unsigned int i = 0; i < g_ctx.serviceCount; i++) {
            if (g_ctx.services[i].instanceCount > 0) {
                anyInstances = TRUE;
                break;
            }
        }
        if (!anyInstances) {
            int result = MessageBoxA(
                NULL,
                "OmniRDP service is running but has no configured instances.\n\n"
                "Would you like to open the config file to add one?",
                "OmniRDP",
                MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND
            );
            if (result == IDYES) {
                char configPath[MAX_PATH];
                snprintf(configPath, sizeof(configPath),
                         "C:\\ProgramData\\OmniRDP\\config.ini");
                SHELLEXECUTEINFOA sei = {0};
                sei.cbSize = sizeof(sei);
                sei.lpVerb = "open";
                sei.lpFile = configPath;
                sei.nShow = SW_SHOWNORMAL;
                if (!ShellExecuteExA(&sei)) {
                    sei.lpFile = "notepad.exe";
                    sei.lpParameters = configPath;
                    ShellExecuteExA(&sei);
                }
            }
        }
    }

    /* Message loop */
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        /* Let the dialog handle its own messages if it's visible */
        HWND hDlg = tray_status_dlg_get_hwnd();
        if (hDlg && IsDialogMessage(hDlg, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* Cleanup */
    LOG_I("tray", "OmniRDP Tray shutting down");
    g_ctx.running = FALSE;

    tray_status_dlg_close();
    tray_icon_cleanup(&g_ctx);
    svc_log_shutdown();

    if (hMutex) CloseHandle(hMutex);
    return 0;
}
