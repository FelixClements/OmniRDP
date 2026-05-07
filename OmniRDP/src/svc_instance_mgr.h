/**
 * @file svc_instance_mgr.h
 * @brief Instance manager for OmniRDP service
 *
 * Manages lifecycle of child OmniRDP.exe processes, one per configured
 * backend instance.  Handles spawning, monitoring (heartbeat), graceful
 * shutdown, and automatic reconnection with exponential backoff.
 *
 * Windows-only; no FreeRDP dependency.
 */

#ifndef SVC_INSTANCE_MGR_H
#define SVC_INSTANCE_MGR_H

#include "pipe_protocol.h"
#include "svc_config.h"
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Instance state machine ────────────────────────────────────── */

typedef enum {
  INST_STOPPED = 0,
  INST_STARTING,
  INST_RUNNING,
  INST_RECONNECTING
} InstanceState;

/* ── Per-instance tracking ─────────────────────────────────────── */

typedef struct {
  char name[128];        /* Instance name from config */
  InstanceState state;   /* Current state */
  InstanceConfig config; /* Copy of instance config */

  /* Child process */
  HANDLE hProcess; /* Process handle (NULL if not running) */
  HANDLE hJob;     /* Job object handle */
  DWORD pid;       /* Process ID (0 if not running) */

  /* Heartbeat */
  HANDLE hHeartbeatPipe;     /* Named pipe for heartbeat
                                (\\.\\pipe\OmniRDP_Instance_<name>) */
  ULONGLONG lastHeartbeatMs; /* Timestamp of last heartbeat */

  /* Reconnect backoff */
  unsigned int reconnectAttempts;
  ULONGLONG
  nextReconnectMs; /* Timestamp when next reconnect attempt is allowed */

  /* Stats (updated by heartbeat or IPC) */
  DWORD viewerCount;

  /* Shutdown flag */
  BOOL stopRequested; /* Set to TRUE to request graceful stop */
} ManagedInstance;

/* ── Instance manager ──────────────────────────────────────────── */

typedef struct {
  ManagedInstance *instances; /* Dynamic array of managed instances */
  unsigned int instanceCount;
  SvcConfig *config; /* Current config (may be owned by manager after reload) */
  const char *configPath; /* Path to config.ini */
  const char *exePath;    /* Path to OmniRDP.exe */
  CRITICAL_SECTION lock;  /* Thread safety for state changes */
  BOOL shuttingDown;      /* Set to TRUE during service shutdown */
} InstanceManager;

/**
 * @brief Initialize the instance manager
 * @param mgr Pointer to manager (caller allocates)
 * @param config Loaded config
 * @param configPath Path to config.ini (for --config flag and DPAPI)
 * @param exePath Path to OmniRDP.exe binary
 * @return 0 on success, -1 on error
 */
int inst_mgr_init(InstanceManager *mgr, SvcConfig *config,
                  const char *configPath, const char *exePath);

/**
 * @brief Start all enabled instances
 * Spawns child processes for each instance with enabled=TRUE.
 * Respects instance_startup_delay_ms between spawns.
 */
void inst_mgr_start_all(InstanceManager *mgr);

/**
 * @brief Start a specific instance by name
 * @return 0 on success, -1 on error
 */
int inst_mgr_start(InstanceManager *mgr, const char *instanceName);

/**
 * @brief Stop a specific instance by name
 * Sends CTRL_BREAK_EVENT, waits graceful_shutdown_sec, then TerminateProcess.
 * @return 0 on success, -1 on error
 */
int inst_mgr_stop(InstanceManager *mgr, const char *instanceName);

/**
 * @brief Restart a specific instance (stop + start)
 * @return 0 on success, -1 on error
 */
int inst_mgr_restart(InstanceManager *mgr, const char *instanceName);

/**
 * @brief Stop all instances (for service shutdown)
 * Sends stop signal to all running instances, waits for them to exit.
 */
void inst_mgr_stop_all(InstanceManager *mgr);

/**
 * @brief Poll instance health and handle reconnection
 * Call this periodically from the service main loop.
 * - Checks heartbeat timeouts
 * - Attempts reconnection for instances in RECONNECTING state
 * - Updates instance states
 */
void inst_mgr_poll(InstanceManager *mgr);

/**
 * @brief Get instance info for IPC responses
 * @param mgr Instance manager
 * @param index Instance index
 * @param info Output: filled with instance info for pipe protocol
 * @return 0 on success, -1 if index out of range
 */
int inst_mgr_get_info(InstanceManager *mgr, unsigned int index,
                      PipeInstanceInfo *info);

/**
 * @brief Find an instance by name
 * @return Pointer to ManagedInstance, or NULL if not found
 */
ManagedInstance *inst_mgr_find(InstanceManager *mgr, const char *name);

/**
 * @brief Reload config and apply changes
 * Parses the config file, diffs old vs new, and:
 * - Starts new instances
 * - Stops removed instances
 * - Toggles enabled/disabled
 * - Applies safe changes inline
 * - Stops+restarts for breaking changes
 * @param configPath Path to config.ini
 * @return 0 on success, -1 on error (old config preserved)
 */
int inst_mgr_reload_config(InstanceManager *mgr, const char *configPath);

/**
 * @brief Clean up instance manager resources
 * Stops all instances, closes handles, frees memory.
 */
void inst_mgr_cleanup(InstanceManager *mgr);

#ifdef __cplusplus
}
#endif

#endif /* SVC_INSTANCE_MGR_H */
