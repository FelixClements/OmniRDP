/**
 * @file svc_log.c
 * @brief Logging shim implementation for the OmniRDP service
 *
 * Thread-safe logging to a rotating file (OmniRDP-svc.log) with
 * printf-style formatting.  Falls back to OutputDebugStringA when
 * the logging subsystem has not yet been initialised.
 *
 * Rotation policy:
 *   - When the current file exceeds max_size_mb megabytes the chain
 *     is shifted:  .N -> delete, .(N-1) -> .N, ..., .1 -> .2,
 *     current -> .1, then a new file is opened.
 */

#include "svc_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ── Constants ─────────────────────────────────────────────────── */

#define LOG_FILE_NAME "OmniRDP-svc.log"
#define LINE_BUF_SIZE 8192

/* ── Internal state ───────────────────────────────────────────── */

static CRITICAL_SECTION g_cs;
static FILE *g_logfile = NULL;
static int g_init = 0; /* non-zero after init */
static SvcLogLevel g_min_level = SVC_LOG_INFO;
static unsigned int g_max_size = 10; /* MB */
static unsigned int g_max_files = 5;
static char g_log_dir[MAX_PATH] = {0};
static char g_log_path[MAX_PATH] = {0};

/* ── Directory helpers ─────────────────────────────────────────── */

/**
 * @brief Create a directory hierarchy recursively.
 *
 * Thin wrapper around CreateDirectoryA that splits the path on
 * backslashes and creates each component in turn.
 *
 * @param path  Full directory path (backslash-separated).
 * @return 0 on success, -1 on error.
 */
static int svc_log_mkdir_recursive(const char *path) {
  if (!path || path[0] == '\0')
    return -1;

  /* Try to create the full path first -- quick path for existing dirs. */
  if (CreateDirectoryA(path, NULL))
    return 0;

  DWORD err = GetLastError();
  if (err == ERROR_ALREADY_EXISTS)
    return 0;

  if (err != ERROR_PATH_NOT_FOUND)
    return -1;

  /* One or more parent components are missing -- walk the path. */
  char tmp[MAX_PATH];
  strncpy(tmp, path, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';

  /* Skip the leading "X:\" (drive root) on absolute paths */
  char *p = tmp;
  if (p[0] != '\0' && p[1] == ':')
    p += 2;

  for (;;) {
    /* Find next backslash (or end of string) */
    char *slash = strchr(p, '\\');
    if (!slash)
      break;

    *slash = '\0';
    CreateDirectoryA(tmp, NULL);
    *slash = '\\';
    p = slash + 1;

    /* If we reach a component that already exists, skip ahead */
    while (*p == '\\')
      p++;
  }

  /* Final attempt to create the full path */
  if (CreateDirectoryA(path, NULL))
    return 0;

  return (GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
}

/* ── Level helpers ────────────────────────────────────────────── */

/** Map a SvcLogLevel to its printable name. */
static const char *svc_log_level_name(SvcLogLevel level) {
  switch (level) {
  case SVC_LOG_DEBUG:
    return "DEBUG";
  case SVC_LOG_INFO:
    return "INFO";
  case SVC_LOG_WARN:
    return "WARN";
  case SVC_LOG_ERROR:
    return "ERROR";
  default:
    return "?";
  }
}

/**
 * @brief Convert a case-insensitive level string to SvcLogLevel.
 *
 * Accepted values: "debug", "info", "warn", "error".
 *
 * @param str  Input string (may be NULL).
 * @param out  Receives the parsed level.
 * @return 0 on success, -1 on parse failure.
 */
int svc_log_level_from_string(const char *str, SvcLogLevel *out) {
  if (!str || !out)
    return -1;

  if (_stricmp(str, "debug") == 0) {
    *out = SVC_LOG_DEBUG;
    return 0;
  }
  if (_stricmp(str, "info") == 0) {
    *out = SVC_LOG_INFO;
    return 0;
  }
  if (_stricmp(str, "warn") == 0) {
    *out = SVC_LOG_WARN;
    return 0;
  }
  if (_stricmp(str, "error") == 0) {
    *out = SVC_LOG_ERROR;
    return 0;
  }

  return -1;
}

/* ── Rotation ─────────────────────────────────────────────────── */

/**
 * @brief Rotate log files when the current file exceeds the size limit.
 *
 * Assumes g_cs is held by the caller.  Closes the current file,
 * shifts the .1 .. .N chain, renames current -> .1, and opens a
 * fresh file.
 */
static void svc_log_rotate_internal(void) {
  if (!g_init || g_max_files == 0)
    return;

  char oldpath[MAX_PATH];
  char newpath[MAX_PATH];

  /* 1. Close the current file */
  if (g_logfile) {
    fclose(g_logfile);
    g_logfile = NULL;
  }

  /* 2. Delete the highest-numbered archive */
  _snprintf(oldpath, sizeof(oldpath), "%s\\%s.%u", g_log_dir, LOG_FILE_NAME,
            g_max_files);
  DeleteFileA(oldpath);

  /* 3. Shift the chain: (N-1) -> N, ..., 1 -> 2 */
  for (unsigned int i = g_max_files - 1; i >= 1; i--) {
    _snprintf(oldpath, sizeof(oldpath), "%s\\%s.%u", g_log_dir, LOG_FILE_NAME,
              i);
    _snprintf(newpath, sizeof(newpath), "%s\\%s.%u", g_log_dir, LOG_FILE_NAME,
              i + 1);
    MoveFileExA(oldpath, newpath, MOVEFILE_REPLACE_EXISTING);
  }

  /* 4. Rename current log -> .1 */
  _snprintf(newpath, sizeof(newpath), "%s\\%s.1", g_log_dir, LOG_FILE_NAME);
  MoveFileExA(g_log_path, newpath, MOVEFILE_REPLACE_EXISTING);

  /* 5. Open a fresh file */
  g_logfile = fopen(g_log_path, "a");
}

/**
 * @brief Check file size and trigger rotation if necessary.
 *
 * Assumes g_cs is held by the caller.
 */
static void svc_log_check_rotate(void) {
  if (!g_logfile || g_max_size == 0)
    return;

  long pos = ftell(g_logfile);
  if (pos < 0)
    return;

  unsigned long long max_bytes =
      (unsigned long long)g_max_size * 1024ULL * 1024ULL;
  if ((unsigned long long)pos >= max_bytes)
    svc_log_rotate_internal();
}

/* ── Timestamp ────────────────────────────────────────────────── */

/**
 * @brief Format a timestamp into the provided buffer.
 *
 * Format: "YYYY-MM-DD HH:MM:SS".  Uses GetLocalTime for the current
 * system time.
 *
 * @param buf   Output buffer (at least 20 chars recommended).
 * @param size  Size of the output buffer.
 */
static void svc_log_timestamp(char *buf, size_t size) {
  if (!buf || size == 0)
    return;

  SYSTEMTIME st;
  GetLocalTime(&st);
  _snprintf(buf, size, "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth,
            st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/* ── Public API ───────────────────────────────────────────────── */

int svc_log_init(const char *log_dir, SvcLogLevel log_level,
                 unsigned int max_size_mb, unsigned int max_files) {
  if (!log_dir)
    return -1;

  /* Clamp max_files to something sane (0 = no rotation by count). */
  if (max_files > 100)
    max_files = 100;

  /* Build the full log file path */
  int ret = _snprintf(g_log_path, sizeof(g_log_path), "%s\\%s", log_dir,
                      LOG_FILE_NAME);
  if (ret < 0 || (size_t)ret >= sizeof(g_log_path))
    return -1;

  /* Create log directory (recursively if needed) */
  svc_log_mkdir_recursive(log_dir);

  /* Open the log file for appending */
  FILE *f = fopen(g_log_path, "a");
  if (!f)
    return -1; /* directory creation may have failed */

  /* Store copies of configuration strings */
  strncpy(g_log_dir, log_dir, sizeof(g_log_dir) - 1);
  g_log_dir[sizeof(g_log_dir) - 1] = '\0';

  g_min_level = log_level;
  g_max_size = max_size_mb;
  g_max_files = max_files;

  /* Initialise synchronisation */
  InitializeCriticalSection(&g_cs);

  g_logfile = f;
  g_init = 1;

  return 0;
}

void svc_log_write(SvcLogLevel level, const char *source, const char *fmt,
                   ...) {
  /*
   * If not yet initialised, fall back to OutputDebugStringA so that
   * early-startup messages are not completely lost.
   */
  if (!g_init) {
    char debug_buf[LINE_BUF_SIZE];
    int n = _snprintf(debug_buf, sizeof(debug_buf), "[svc_log] [%s] [%s] ",
                      svc_log_level_name(level), source ? source : "?");

    if (n > 0 && (size_t)n < sizeof(debug_buf)) {
      va_list args;
      va_start(args, fmt);
      _vsnprintf(debug_buf + n, sizeof(debug_buf) - (size_t)n, fmt, args);
      va_end(args);
    }
    debug_buf[sizeof(debug_buf) - 1] = '\0';
    OutputDebugStringA(debug_buf);
    OutputDebugStringA("\n");
    return;
  }

  /* Level filtering */
  if (level < g_min_level)
    return;

  EnterCriticalSection(&g_cs);

  /* Check whether rotation is needed */
  svc_log_check_rotate();

  if (!g_logfile) {
    LeaveCriticalSection(&g_cs);
    return;
  }

  /* Timestamp */
  char ts[24];
  svc_log_timestamp(ts, sizeof(ts));

  /* Format the full line: [TIMESTAMP] [LEVEL] [source] message */
  char line[LINE_BUF_SIZE];
  int n = _snprintf(line, sizeof(line), "[%s] [%s] [%s] ", ts,
                    svc_log_level_name(level), source ? source : "?");

  if (n > 0 && (size_t)n < sizeof(line)) {
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, args);
    va_end(args);
  }
  line[sizeof(line) - 1] = '\0';

  /* Write to file */
  fprintf(g_logfile, "%s\n", line);
  fflush(g_logfile);

  LeaveCriticalSection(&g_cs);
}

void svc_log_shutdown(void) {
  if (!g_init)
    return;

  /*
   * We intentionally do NOT reset g_init here to guard against
   * recursive/incorrect usage patterns during shutdown.
   */

  EnterCriticalSection(&g_cs);

  if (g_logfile) {
    fflush(g_logfile);
    fclose(g_logfile);
    g_logfile = NULL;
  }

  LeaveCriticalSection(&g_cs);

  DeleteCriticalSection(&g_cs);
  g_init = 0;
}
