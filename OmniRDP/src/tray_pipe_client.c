/**
 * @file tray_pipe_client.c
 * @brief Named pipe IPC client implementation for the OmniRDP tray app
 *
 * Connects to the OmniRDP service's named pipe server, sends
 * request-response commands, and receives unsolicited push messages.
 *
 * Wire format: length-prefixed UTF-8 JSON frames as defined in
 * pipe_protocol.h (4-byte LE length prefix + JSON payload).
 *
 * This file is FreeRDP-independent and Windows-only.
 */

#include "tray_pipe_client.h"
#include "svc_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ── Defaults ──────────────────────────────────────────────────── */

/** Default response timeout in milliseconds when caller passes 0. */
#define REQUEST_TIMEOUT_MS 5000U

/** Log source tag. */
#define LOG_TAG "pipe_client"

/* ══════════════════════════════════════════════════════════════════ */
/*  Public API                                                        */
/* ══════════════════════════════════════════════════════════════════ */

void pipe_client_init(PipeClient *client, const char *pipeName) {
  if (!client || !pipeName)
    return;

  client->hPipe = INVALID_HANDLE_VALUE;
  client->connected = FALSE;

  /* Build full pipe path: \\.\pipe\<pipeName> */
  _snprintf(client->pipeName, sizeof(client->pipeName), "\\\\.\\pipe\\%s",
            pipeName);
  client->pipeName[sizeof(client->pipeName) - 1] = '\0';

  InitializeCriticalSection(&client->lock);

  LOG_I(LOG_TAG, "PipeClient initialised (pipe: %s)", client->pipeName);
}

int pipe_client_connect(PipeClient *client) {
  if (!client)
    return -1;

  if (client->connected) {
    LOG_W(LOG_TAG, "Already connected to %s", client->pipeName);
    return 0;
  }

  HANDLE hPipe = CreateFileA(client->pipeName, GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, /* default security attributes */
                             OPEN_EXISTING,
                             SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                             NULL /* no template handle */
  );

  if (hPipe == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    LOG_W(LOG_TAG, "Failed to connect to %s (error %lu)", client->pipeName,
          err);
    return -1;
  }

  /*
   * Optional: switch to message-read mode so that PeekNamedPipe works
   * naturally with the framing.  We set PIPE_READMODE_BYTE so that
   * ReadFile reads raw bytes (the framing layer handles message
   * boundaries via the length prefix).
   *
   * The default pipe mode is byte-stream, which is what we want.
   * No need to call SetNamedPipeHandleState unless we need
   * non-blocking or message-mode.
   */
  client->hPipe = hPipe;
  client->connected = TRUE;

  LOG_I(LOG_TAG, "Connected to %s", client->pipeName);
  return 0;
}

void pipe_client_disconnect(PipeClient *client) {
  if (!client)
    return;

  if (client->hPipe != INVALID_HANDLE_VALUE) {
    /* Flush any remaining data before closing */
    FlushFileBuffers(client->hPipe);
    CloseHandle(client->hPipe);
    client->hPipe = INVALID_HANDLE_VALUE;
  }

  client->connected = FALSE;
  LOG_I(LOG_TAG, "Disconnected from pipe");
}

BOOL pipe_client_is_connected(PipeClient *client) {
  return (client && client->connected) ? TRUE : FALSE;
}

int pipe_client_send_request(PipeClient *client, const PipeRequest *request,
                             PipeResponse *response, DWORD timeoutMs) {
  char *jsonReq = NULL;
  char *reply = NULL;
  DWORD replyLen = 0;
  int ret = -1;

  if (!client || !request || !response)
    return -1;

  if (!client->connected) {
    LOG_E(LOG_TAG, "send_request: not connected");
    return -1;
  }

  if (timeoutMs == 0)
    timeoutMs = REQUEST_TIMEOUT_MS;

  EnterCriticalSection(&client->lock);

  /* ── Build the JSON request ─────────────────────────────────── */
  /*
   * Format: {"cmd":<int>,"instance_name":"<name>","payload":<json>}
   * The payload field is already a JSON fragment (object, array, or
   * empty string).
   */
  {
    /*
     * Calculate required buffer size:
     *   fixed overhead ~64 bytes + instance_name + json_payload
     */
    size_t needed =
        64 + strlen(request->instance_name) + strlen(request->json_payload);

    jsonReq = (char *)HeapAlloc(GetProcessHeap(), 0, needed);
    if (!jsonReq) {
      LOG_E(LOG_TAG, "send_request: out of memory");
      LeaveCriticalSection(&client->lock);
      return -1;
    }

    _snprintf(jsonReq, needed,
              "{\"cmd\":%d,\"instance_name\":\"%s\",\"payload\":%s}",
              (int)request->command, request->instance_name,
              request->json_payload[0] != '\0' ? request->json_payload : "{}");
    jsonReq[needed - 1] = '\0';
  }

  LOG_D("pipe_client", "send_request: sending %lu bytes: %s",
        (DWORD)strlen(jsonReq), jsonReq);

  /* ── Send the frame ─────────────────────────────────────────── */
  {
    DWORD jsonLen = (DWORD)strlen(jsonReq);
    BOOL sendResult = pipe_frame_send(client->hPipe, jsonReq, jsonLen);
    LOG_D("pipe_client",
          "send_request: pipe_frame_send returned %d, lastError=%lu",
          sendResult, GetLastError());
    if (!sendResult) {
      DWORD err = GetLastError();
      LOG_E("pipe_client", "send_request: pipe_frame_send failed (error %lu)",
            err);
      goto cleanup;
    }
  }

  /* ── Receive the response frame (with push-message filtering) ── */
  /*
   * Receive frames in a loop, discarding any unsolicited push
   * messages (type:"push") that may have been sent between our
   * request and the response.  Only a frame with type:"response"
   * is treated as the command reply.
   *
   * Uses PeekNamedPipe polling to implement a coarse timeout,
   * since the pipe handle is opened in synchronous (non-overlapped)
   * mode.
   */
  {
    DWORD startTime = GetTickCount();
    BOOL gotResp = FALSE;
    int attempt = 0;

    while (!gotResp) {
      attempt++;

      LOG_D("pipe_client", "send_request: waiting for response (attempt %d)",
            attempt);

      /* ── Check for timeout ──────────────────────────────── */
      if (GetTickCount() - startTime >= timeoutMs) {
        LOG_E("pipe_client",
              "send_request: timeout (%lu ms) waiting for response", timeoutMs);
        goto cleanup;
      }

      /* ── Poll for available data ────────────────────────── */
      DWORD avail = 0;
      if (!PeekNamedPipe(client->hPipe, NULL, 0, NULL, &avail, NULL)) {
        DWORD err = GetLastError();
        LOG_E("pipe_client", "send_request: PeekNamedPipe failed (error %lu)",
              err);
        goto cleanup;
      }

      if (avail == 0) {
        Sleep(50);
        continue;
      }

      /* ── Read a frame ───────────────────────────────────── */
      {
        BOOL recvResult = pipe_frame_recv(client->hPipe, &reply, &replyLen);
        LOG_D("pipe_client",
              "send_request: pipe_frame_recv returned %d, payloadLen=%lu, "
              "lastError=%lu",
              recvResult, replyLen, GetLastError());
        if (recvResult && replyLen > 0) {
          LOG_D("pipe_client", "send_request: received payload: %s", reply);
        }
        if (!recvResult) {
          DWORD err = GetLastError();
          LOG_E("pipe_client",
                "send_request: pipe_frame_recv failed (error %lu)", err);
          goto cleanup;
        }
      }

      /* ── Filter out unsolicited push messages ───────────── */
      if (strstr(reply, "\"type\":\"push\"") != NULL) {
        LOG_D("pipe_client", "send_request: received push message, discarding");
        HeapFree(GetProcessHeap(), 0, reply);
        reply = NULL;
        replyLen = 0;
        continue;
      }

      gotResp = TRUE;
    }
  }

  LOG_D(LOG_TAG, "send_request: received response (%lu bytes)", replyLen);

  /* ── Parse the response JSON into PipeResponse ──────────────── */
  {
    const char *p;
    size_t len;

    /* success flag */
    response->success = (strstr(reply, "\"success\":1") != NULL) ? 1 : 0;

    /* error_message */
    response->error_message[0] = '\0';
    p = strstr(reply, "\"error_message\":\"");
    if (p) {
      p += 17; /* skip past the key and opening quote */
      const char *end = strchr(p, '"');
      if (end) {
        len = (size_t)(end - p);
        if (len >= sizeof(response->error_message))
          len = sizeof(response->error_message) - 1;
        memcpy(response->error_message, p, len);
        response->error_message[len] = '\0';
      }
    }

    /* json_payload — copy everything after the key */
    response->json_payload[0] = '\0';
    p = strstr(reply, "\"json_payload\":");
    if (p) {
      p += 15; /* skip past the key */
      strncpy(response->json_payload, p, sizeof(response->json_payload) - 1);
      response->json_payload[sizeof(response->json_payload) - 1] = '\0';
    }
  }

  ret = 0;

cleanup:
  if (jsonReq)
    HeapFree(GetProcessHeap(), 0, jsonReq);
  if (reply)
    HeapFree(GetProcessHeap(), 0, reply);

  LeaveCriticalSection(&client->lock);
  return ret;
}

int pipe_client_recv_push(PipeClient *client, PipePushType *pushType,
                          char **payload, DWORD *payloadLen) {
  char *msg = NULL;
  DWORD msgLen = 0;
  DWORD avail = 0;
  int ret = -1;

  if (!client || !pushType || !payload || !payloadLen)
    return -1;

  if (!client->connected)
    return -1;

  /* Initialise outputs */
  *payload = NULL;
  *payloadLen = 0;

  EnterCriticalSection(&client->lock);

  /* ── Non-blocking peek to see if data is available ──────────── */
  if (!PeekNamedPipe(client->hPipe, NULL, 0, NULL, &avail, NULL) ||
      avail == 0) {
    LeaveCriticalSection(&client->lock);
    return -1; /* no data available */
  }

  /* ── Read a frame ───────────────────────────────────────────── */
  if (!pipe_frame_recv(client->hPipe, &msg, &msgLen)) {
    DWORD err = GetLastError();
    LOG_W(LOG_TAG, "recv_push: pipe_frame_recv failed (error %lu)", err);
    LeaveCriticalSection(&client->lock);
    return -1;
  }

  LeaveCriticalSection(&client->lock);

  /* ── Parse the push type from the JSON ──────────────────────── */
  /*
   * Expected JSON format:
   *   {"type":"push","push_type":"stats", ...}
   *   {"type":"push","push_type":"event", ...}
   */
  if (strstr(msg, "\"type\":\"push\"") == NULL) {
    /* Not a push message — unexpected */
    LOG_W(LOG_TAG, "recv_push: received non-push message, discarding");
    HeapFree(GetProcessHeap(), 0, msg);
    return -1;
  }

  if (strstr(msg, "\"push_type\":\"stats\"") != NULL)
    *pushType = PIPE_PUSH_STATS;
  else if (strstr(msg, "\"push_type\":\"event\"") != NULL)
    *pushType = PIPE_PUSH_EVENT;
  else {
    LOG_W(LOG_TAG, "recv_push: unknown push_type in message");
    HeapFree(GetProcessHeap(), 0, msg);
    return -1;
  }

  /* Transfer ownership of the heap-allocated buffer to the caller */
  *payload = msg;
  *payloadLen = msgLen;

  LOG_D(LOG_TAG, "recv_push: received push type %s (%lu bytes)",
        (*pushType == PIPE_PUSH_STATS) ? "stats" : "event", msgLen);

  return 0;
}

void pipe_client_cleanup(PipeClient *client) {
  if (!client)
    return;

  pipe_client_disconnect(client);
  DeleteCriticalSection(&client->lock);

  LOG_I(LOG_TAG, "PipeClient cleaned up");
}
