/**
 * @file tray_pipe_client.h
 * @brief Named pipe IPC client for the OmniRDP tray application
 *
 * Provides a client-side interface to the OmniRDP service's named pipe
 * IPC server.  Supports both request-response commands and receiving
 * unsolicited push messages (stats / events).
 *
 * This file is FreeRDP-independent and Windows-only.
 */

#ifndef TRAY_PIPE_CLIENT_H
#define TRAY_PIPE_CLIENT_H

#include <windows.h>
#include "pipe_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Named pipe IPC client context
 *
 * Connects to the OmniRDP service pipe server from the tray app.
 * Supports both request-response commands and receiving push messages.
 */
typedef struct {
    HANDLE hPipe;                  /* Connected pipe handle, or INVALID_HANDLE_VALUE */
    char pipeName[256];            /* Full pipe path */
    BOOL connected;                /* TRUE if currently connected */
    CRITICAL_SECTION lock;         /* Thread safety for send/receive */
} PipeClient;

/**
 * @brief Initialize the pipe client
 *
 * Does NOT connect yet. Call pipe_client_connect() to establish a connection.
 *
 * @param client Pointer to PipeClient (caller allocates)
 * @param pipeName Pipe name suffix (e.g., "OmniRDP_ServicePipe").
 *                 Full path is constructed as "\\.\pipe\<pipeName>"
 */
void pipe_client_init(PipeClient *client, const char *pipeName);

/**
 * @brief Connect to the service pipe server
 *
 * Attempts to connect to the named pipe. If the server is not available,
 * returns -1. The client can retry later.
 *
 * @param client Pipe client context
 * @return 0 on success, -1 on error
 */
int pipe_client_connect(PipeClient *client);

/**
 * @brief Disconnect from the service pipe server
 *
 * Closes the pipe handle. Safe to call if not connected.
 */
void pipe_client_disconnect(PipeClient *client);

/**
 * @brief Check if the client is currently connected
 */
BOOL pipe_client_is_connected(PipeClient *client);

/**
 * @brief Send a command and receive a response
 *
 * Sends a request frame and waits for a response frame.
 * This is a blocking call with a timeout.
 *
 * @param client Pipe client context (must be connected)
 * @param request The request to send
 * @param response The response to fill (caller allocates)
 * @param timeoutMs Timeout in milliseconds (0 = default 5000ms)
 * @return 0 on success, -1 on error or timeout
 */
int pipe_client_send_request(PipeClient *client, const PipeRequest *request,
                             PipeResponse *response, DWORD timeoutMs);

/**
 * @brief Receive a push message from the server
 *
 * Non-blocking check for an incoming push message. If no message
 * is available, returns immediately with -1.
 *
 * @param client Pipe client context (must be connected)
 * @param pushType Out: the push type (stats or event)
 * @param payload Out: allocated buffer with JSON payload (caller must HeapFree)
 * @param payloadLen Out: length of payload
 * @return 0 if a push message was received, -1 if no message available
 */
int pipe_client_recv_push(PipeClient *client, PipePushType *pushType,
                          char **payload, DWORD *payloadLen);

/**
 * @brief Clean up pipe client resources
 *
 * Disconnects if connected and destroys the critical section.
 */
void pipe_client_cleanup(PipeClient *client);

#ifdef __cplusplus
}
#endif

#endif /* TRAY_PIPE_CLIENT_H */
