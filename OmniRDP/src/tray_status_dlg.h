#ifndef TRAY_STATUS_DLG_H
#define TRAY_STATUS_DLG_H

#include <windows.h>
#include "tray_icon.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create and show the status window
 *
 * Creates a modeless dialog with a ListView table showing all
 * instances across all discovered services.
 *
 * @param ctx Tray app context (for accessing service/instance info)
 * @param hInstance Application instance handle
 * @return HWND of the dialog, or NULL on error
 */
HWND tray_status_dlg_show(TrayAppCtx *ctx, HINSTANCE hInstance);

/**
 * @brief Refresh the status window data
 *
 * Updates the ListView with current instance info from all services.
 * Called when the user clicks "Refresh" or when push notifications arrive.
 *
 * @param ctx Tray app context
 */
void tray_status_dlg_refresh(TrayAppCtx *ctx);

/**
 * @brief Close the status window
 *
 * Destroys the dialog window.
 */
void tray_status_dlg_close(void);

/**
 * @brief Check if the status window is currently visible
 *
 * @return TRUE if the dialog is visible, FALSE otherwise
 */
BOOL tray_status_dlg_is_visible(void);

/**
 * @brief Get the HWND of the status dialog
 *
 * @return HWND of the dialog, or NULL if not created
 */
HWND tray_status_dlg_get_hwnd(void);

#ifdef __cplusplus
}
#endif

#endif /* TRAY_STATUS_DLG_H */
