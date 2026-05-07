/**
 * @file svc_log.h
 * @brief Logging shim for the OmniRDP service
 *
 * Thread-safe logging with log-level filtering, file rotation (by size
 * and count), and printf-style formatting.  Windows-only; no FreeRDP
 * dependency.
 */

#ifndef SVC_LOG_H
#define SVC_LOG_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C"
{
#endif

	/* ── Log levels ───────────────────────────────────────────────── */

	typedef enum
	{
		SVC_LOG_DEBUG = 0,
		SVC_LOG_INFO = 1,
		SVC_LOG_WARN = 2,
		SVC_LOG_ERROR = 3
	} SvcLogLevel;

	/* ── Public API ───────────────────────────────────────────────── */

	/**
	 * @brief Initialise the logging subsystem.
	 *
	 * Creates the log directory (recursively) if it does not exist and
	 * opens the log file for appending.  Must be called before any
	 * svc_log_write / LOG_* calls.
	 *
	 * @param log_dir    Directory for log files
	 * @param log_level  Minimum log level to write
	 * @param max_size_mb  Maximum log file size in MB before rotation
	 * @param max_files  Maximum number of old log files to retain
	 * @return 0 on success, -1 on error
	 */
	int svc_log_init(const char* log_dir, SvcLogLevel log_level, unsigned int max_size_mb,
	                 unsigned int max_files);

	/**
	 * @brief Write a log message.
	 *
	 * Thread-safe.  Messages below the configured log level are silently
	 * dropped.  If svc_log_init has not been called the message is emitted
	 * via OutputDebugStringA so that early-startup output is not lost.
	 *
	 * @param level   Severity of the message
	 * @param source  Function / module name (printed in square brackets)
	 * @param fmt     Printf-style format string
	 * @param ...     Arguments for the format string
	 */
	void svc_log_write(SvcLogLevel level, const char* source, const char* fmt, ...);

/* ── Convenience macros ───────────────────────────────────────── */

/** Write a DEBUG-level message. */
#define LOG_D(source, ...) svc_log_write(SVC_LOG_DEBUG, source, __VA_ARGS__)

/** Write an INFO-level message. */
#define LOG_I(source, ...) svc_log_write(SVC_LOG_INFO, source, __VA_ARGS__)

/** Write a WARN-level message. */
#define LOG_W(source, ...) svc_log_write(SVC_LOG_WARN, source, __VA_ARGS__)

/** Write an ERROR-level message. */
#define LOG_E(source, ...) svc_log_write(SVC_LOG_ERROR, source, __VA_ARGS__)

	/**
	 * @brief Shut down the logging subsystem.
	 *
	 * Flushes and closes the log file.  Safe to call multiple times.
	 */
	void svc_log_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SVC_LOG_H */
