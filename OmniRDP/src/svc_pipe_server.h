/**
 * @file svc_pipe_server.h
 * @brief Named pipe IPC server for the OmniRDP service
 *
 * Implements the server-side of the named pipe IPC protocol defined in
 * pipe_protocol.h.  Listens for tray-app connections, dispatches
 * commands to the instance manager, and pushes unsolicited stats/event
 * messages to connected clients.
 *
 * Windows-only; no FreeRDP dependency.
 */

#ifndef SVC_PIPE_SERVER_H
#define SVC_PIPE_SERVER_H

#include "svc_instance_mgr.h"
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Named pipe IPC server context
 *
 * Manages the named pipe listener and connected clients.
 * Runs in the service process (Session 0).
 */
typedef struct {
  char pipeName[256]; /* Full pipe path, e.g. "\\.\pipe\OmniRDP_ServicePipe" */
  InstanceManager *mgr;  /* Pointer to instance manager (not owned) */
  CRITICAL_SECTION lock; /* Protects client list */

  HANDLE hListenerThread; /* Thread that accepts connections */
  volatile BOOL running;  /* Server running flag */

  /* Internal: linked list of connected clients (ClientConnection*) */
  void *clients;
} PipeServer;

/**
 * @brief Initialize and start the pipe server
 *
 * Creates the named pipe with appropriate security, starts the listener
 * thread, and begins accepting connections.
 *
 * @param server Pointer to PipeServer (caller allocates)
 * @param pipeName Pipe name suffix (e.g., "OmniRDP_ServicePipe").
 *                 Full path is constructed as "\\.\pipe\<pipeName>"
 * @param mgr Pointer to instance manager
 * @return 0 on success, -1 on error
 */
int pipe_server_init(PipeServer *server, const char *pipeName,
                     InstanceManager *mgr);

/**
 * @brief Stop the pipe server and disconnect all clients
 *
 * Signals the listener thread to stop, waits for it to exit,
 * and closes all client connections.
 */
void pipe_server_stop(PipeServer *server);

/**
 * @brief Push a stats update to all connected clients
 *
 * Sends an unsolicited stats message to every connected client.
 * Called periodically by the service main loop.
 *
 * @param server Pipe server context
 */
void pipe_server_push_stats(PipeServer *server);

/**
 * @brief Push an event notification to all connected clients
 *
 * @param server Pipe server context
 * @param eventType Event type string (e.g., "crash", "reconnect",
 * "config_error")
 * @param instanceName Instance name the event relates to
 */
void pipe_server_push_event(PipeServer *server, const char *eventType,
                            const char *instanceName);

#ifdef __cplusplus
}
#endif

#endif /* SVC_PIPE_SERVER_H */
