#ifndef TRAY_LOG_VIEWER_H
#define TRAY_LOG_VIEWER_H

#include <windows.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief Create and show the log viewer window
	 *
	 * Creates a modeless dialog with a read-only edit control
	 * that displays the service log file. Auto-refreshes every 2 seconds.
	 *
	 * @param hInstance Application instance handle
	 * @param logPath Path to the log file to display
	 * @return HWND of the dialog, or NULL on error
	 */
	HWND tray_log_viewer_show(HINSTANCE hInstance, const char* logPath);

	/**
	 * @brief Close the log viewer window
	 */
	void tray_log_viewer_close(void);

	/**
	 * @brief Check if the log viewer is visible
	 */
	BOOL tray_log_viewer_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* TRAY_LOG_VIEWER_H */
