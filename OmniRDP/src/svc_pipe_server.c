/**
 * @file svc_pipe_server.c
 * @brief Named pipe IPC server implementation for the OmniRDP service
 *
 * Implements the server-side IPC endpoint that the tray application
 * connects to.  Handles command dispatch, connection lifecycle, and
 * unsolicited push messages (stats / events).
 *
 * Windows-only; no FreeRDP dependency.
 */

#include "svc_pipe_server.h"
#include "pipe_protocol.h"
#include "svc_log.h"

#include <share.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wtsapi32.h>

/* ── Internal constants ────────────────────────────────────────── */

/** Maximum number of concurrent named pipe instances (connected clients). */
#define PIPE_MAX_INSTANCES 4U

/** Named pipe buffer size for both in/out. */
#define PIPE_BUFFER_SIZE (64U * 1024U)

/** Maximum log lines to return in a get_logs response. */
#define MAX_LOG_LINES 1000U

/** Size of the read-backwards buffer when tailing the log file. */
#define TAIL_CHUNK_SIZE 4096U

/** Log file base name (matches svc_log.c). */
#define LOG_FILE_NAME "OmniRDP-svc.log"

#define PIPE_SERVER_CS_SPIN_COUNT 4000U

static PipeServer *g_initialized_pipe_server = NULL;

static BOOL pipe_server_is_initialized(const PipeServer *server) {
  return server && g_initialized_pipe_server == server;
}

/* ── Internal types ────────────────────────────────────────────── */

/**
 * @brief Represents a single connected client.
 *
 * Instances are kept in a singly-linked list owned by PipeServer and
 * protected by PipeServer::lock.
 */
typedef struct ClientConnection {
  struct ClientConnection *next;
  PipeServer *server; /* Owning server (back-reference) */
  HANDLE hPipe;
  HANDLE hThread;
} ClientConnection;

/* ── Forward declarations ──────────────────────────────────────── */

static DWORD WINAPI listener_thread(LPVOID arg);
static DWORD WINAPI client_handler(LPVOID arg);

static BOOL build_pipe_security_attributes(SECURITY_ATTRIBUTES *sa,
                                           SECURITY_DESCRIPTOR *sd,
                                           PACL *aclOut);
static HANDLE create_pipe_instance(const char *pipeName);
static void client_remove(PipeServer *server, HANDLE hPipe);
static int extract_command(const char *payload);
static int extract_instance_name(const char *payload, char *name,
                                 size_t nameSize);

/* Command handlers */
static void cmd_list_instances(PipeServer *server, char *response,
                               size_t respSize);
static void cmd_start_instance(PipeServer *server, const char *payload,
                               char *response, size_t respSize);
static void cmd_stop_instance(PipeServer *server, const char *payload,
                              char *response, size_t respSize);
static void cmd_restart_instance(PipeServer *server, const char *payload,
                                 char *response, size_t respSize);
static void cmd_reload_config(PipeServer *server, char *response,
                              size_t respSize);
static void cmd_get_logs(PipeServer *server, const char *payload,
                         char *response, size_t respSize);

/* Push helpers */
static BOOL push_write_frame(HANDLE hPipe, const char *json, DWORD jsonLen);

/* ════════════════════════════════════════════════════════════════ */
/*  Security Descriptor Helpers                                     */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Build a SECURITY_ATTRIBUTES that grants GENERIC_READ|GENERIC_WRITE
 *        to SYSTEM and the current interactive console user.
 *
 * The caller must free @p aclOut via LocalFree() when the attributes are
 * no longer needed.
 *
 * @param sa     [out] Filled SECURITY_ATTRIBUTES
 * @param sd     [out] Filled SECURITY_DESCRIPTOR
 * @param aclOut [out] Allocated ACL (caller frees with LocalFree)
 * @return TRUE on success, FALSE on failure.
 */
static BOOL build_pipe_security_attributes(SECURITY_ATTRIBUTES *sa,
                                           SECURITY_DESCRIPTOR *sd,
                                           PACL *aclOut) {
  /*
   * Calculate the ACL buffer size:
   *   sizeof(ACL)                      – header
   *   3 × (sizeof(ACCESS_ALLOWED_ACE)  – ace headers (minus the DWORD
   *       - sizeof(DWORD))               trailing SidStart member)
   *   3 × SECURITY_MAX_SID_SIZE        – worst-case SID storage
   */
  DWORD aclSize =
      (DWORD)(sizeof(ACL) + 3 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) +
              3 * SECURITY_MAX_SID_SIZE);

  PACL acl = (PACL)LocalAlloc(LPTR, aclSize);
  if (!acl)
    return FALSE;

  if (!InitializeAcl(acl, aclSize, ACL_REVISION))
    goto fail;

  SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;

  /* ── SYSTEM ──────────────────────────────────────────────── */
  {
    PSID sid = NULL;
    if (!AllocateAndInitializeSid(&ntAuth, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0,
                                  0, 0, 0, 0, 0, &sid))
      goto fail;
    AddAccessAllowedAceEx(acl, ACL_REVISION, 0, GENERIC_READ | GENERIC_WRITE,
                          sid);
    FreeSid(sid);
  }

  /* ── NetworkService ──────────────────────────────────────── */
  {
    PSID sid = NULL;
    if (!AllocateAndInitializeSid(&ntAuth, 1, SECURITY_NETWORK_SERVICE_RID, 0,
                                  0, 0, 0, 0, 0, 0, &sid))
      goto fail;
    AddAccessAllowedAceEx(acl, ACL_REVISION, 0, GENERIC_READ | GENERIC_WRITE,
                          sid);
    FreeSid(sid);
  }

  /* ── Current interactive user (console session) ───────────── */
  {
    HANDLE hToken = NULL;
    DWORD tokenInfoLen = 0;

    /* Try to get the token of the interactive user from the console session */
    if (WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &hToken)) {
      if (!GetTokenInformation(hToken, TokenUser, NULL, 0, &tokenInfoLen) &&
          GetLastError() == ERROR_INSUFFICIENT_BUFFER && tokenInfoLen > 0) {
        PTOKEN_USER pTokenUser =
            (PTOKEN_USER)HeapAlloc(GetProcessHeap(), 0, tokenInfoLen);
        if (pTokenUser && GetTokenInformation(hToken, TokenUser, pTokenUser,
                                              tokenInfoLen, &tokenInfoLen)) {
          AddAccessAllowedAceEx(acl, ACL_REVISION, 0,
                                GENERIC_READ | GENERIC_WRITE,
                                pTokenUser->User.Sid);
        }
        if (pTokenUser)
          HeapFree(GetProcessHeap(), 0, pTokenUser);
      }
      CloseHandle(hToken);
    } else {
      /* Fallback: if WTSQueryUserToken fails (e.g., no interactive session),
       * use INTERACTIVE SID as before */
      PSID sid = NULL;
      if (AllocateAndInitializeSid(&ntAuth, 1, SECURITY_INTERACTIVE_RID, 0, 0,
                                   0, 0, 0, 0, 0, &sid)) {
        AddAccessAllowedAceEx(acl, ACL_REVISION, 0,
                              GENERIC_READ | GENERIC_WRITE, sid);
        FreeSid(sid);
      }
    }
  }

  /* ── Build the descriptor ────────────────────────────────── */
  if (!InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION))
    goto fail;
  if (!SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE))
    goto fail;

  sa->nLength = sizeof(SECURITY_ATTRIBUTES);
  sa->lpSecurityDescriptor = sd;
  sa->bInheritHandle = FALSE;

  *aclOut = acl;
  return TRUE;

fail:
  LocalFree(acl);
  return FALSE;
}

/* ════════════════════════════════════════════════════════════════ */
/*  Pipe Instance Helpers                                           */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Create a single named-pipe instance.
 *
 * Uses the pre-built security attributes to restrict access to
 * SYSTEM and the current interactive user's SID.
 *
 * @param pipeName  Full pipe path (e.g. "\\\\.\\pipe\\OmniRDP_ServicePipe")
 * @return A handle to the new pipe instance, or INVALID_HANDLE_VALUE.
 */
static HANDLE create_pipe_instance(const char *pipeName) {
  SECURITY_ATTRIBUTES sa;
  SECURITY_DESCRIPTOR sd;
  PACL acl = NULL;
  BOOL hasSec = build_pipe_security_attributes(&sa, &sd, &acl);

  HANDLE hPipe =
      CreateNamedPipeA(pipeName, PIPE_ACCESS_DUPLEX,
                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                       PIPE_MAX_INSTANCES, PIPE_BUFFER_SIZE, /* output buffer */
                       PIPE_BUFFER_SIZE,                     /* input buffer */
                       0,                    /* default timeout (ms) */
                       hasSec ? &sa : NULL); /* security attributes */

  if (acl)
    LocalFree(acl);

  if (hPipe == INVALID_HANDLE_VALUE) {
    LOG_E("pipe_server", "CreateNamedPipeA failed: %lu", GetLastError());
  }
  return hPipe;
}

/* ════════════════════════════════════════════════════════════════ */
/*  Client List Helpers                                             */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Remove a client (identified by its pipe handle) from the list.
 *
 * Safe to call with or without the lock held; acquires the lock internally.
 * Closes the pipe handle, thread handle, and frees the node.
 */
static void client_remove(PipeServer *server, HANDLE hPipe) {
  EnterCriticalSection(&server->lock);

  ClientConnection **pp = (ClientConnection **)&server->clients;
  while (*pp) {
    if ((*pp)->hPipe == hPipe) {
      ClientConnection *victim = *pp;
      *pp = victim->next;
      LeaveCriticalSection(&server->lock);

      /* Close handles owned by this node */
      if (victim->hPipe) {
        FlushFileBuffers(victim->hPipe);
        DisconnectNamedPipe(victim->hPipe);
        CloseHandle(victim->hPipe);
      }
      if (victim->hThread)
        CloseHandle(victim->hThread);
      free(victim);
      return;
    }
    pp = &(*pp)->next;
  }

  LeaveCriticalSection(&server->lock);
}

/* ════════════════════════════════════════════════════════════════ */
/*  JSON Helpers                                                    */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Escape a raw string for inclusion as a JSON string value.
 *
 * Replaces: " → \", \ → \\, and strips control characters.
 * Writes at most outSize-1 characters plus NUL terminator.
 *
 * @param input    NUL-terminated raw string.
 * @param output   Destination buffer.
 * @param outSize  Size of destination buffer.
 * @return Number of bytes written (excluding NUL terminator), or 0 on error.
 */
static size_t json_escape_string(const char *input, char *output,
                                 size_t outSize) {
  if (!input || !output || outSize == 0)
    return 0;

  size_t j = 0;
  for (const char *p = input; *p && j < outSize - 1; p++) {
    unsigned char c = (unsigned char)*p;
    switch (c) {
    case '"':
      if (j + 2 < outSize) {
        output[j++] = '\\';
        output[j++] = '"';
      }
      break;
    case '\\':
      if (j + 2 < outSize) {
        output[j++] = '\\';
        output[j++] = '\\';
      }
      break;
    case '\n':
      if (j + 2 < outSize) {
        output[j++] = '\\';
        output[j++] = 'n';
      }
      break;
    case '\r':
      if (j + 2 < outSize) {
        output[j++] = '\\';
        output[j++] = 'r';
      }
      break;
    case '\t':
      if (j + 2 < outSize) {
        output[j++] = '\\';
        output[j++] = 't';
      }
      break;
    default:
      if (c >= 0x20) {
        if (j < outSize - 1)
          output[j++] = (char)c;
      }
      break;
    }
  }
  output[j] = '\0';
  return j;
}

/**
 * @brief Extract the PipeCommand integer from a JSON request payload.
 *
 * Scans for "cmd":<N>  where N is in 0..5.
 *
 * @return The command value, or -1 if not found or out of range.
 */
static int extract_command(const char *payload) {
  if (!payload)
    return -1;

  const char *p = strstr(payload, "\"cmd\":");
  if (!p)
    return -1;

  char *end = NULL;
  unsigned long cmd = strtoul(p + 6, &end, 10);
  if (end == p + 6)
    return -1;

  if (cmd >= PIPE_CMD_COUNT)
    return -1;

  return (int)cmd;
}

/**
 * @brief Extract the instance_name string from a JSON request payload.
 *
 * Expects:  "instance_name":"<value>"
 *
 * @param payload  Raw JSON payload (null-terminated).
 * @param name     Output buffer for the extracted name.
 * @param nameSize Size of the output buffer.
 * @return 0 on success, -1 if not found.
 */
static int extract_instance_name(const char *payload, char *name,
                                 size_t nameSize) {
  if (!payload || !name || nameSize == 0)
    return -1;

  const char *p = strstr(payload, "\"instance_name\":\"");
  if (!p)
    return -1;

  p += sizeof("\"instance_name\":\"") - 1;
  const char *end = strchr(p, '"');
  if (!end)
    return -1;

  size_t len = (size_t)(end - p);
  if (len >= nameSize)
    return -1;
  if (memcpy_s(name, nameSize, p, len) != 0)
    return -1;
  name[len] = '\0';

  return 0;
}

/* ════════════════════════════════════════════════════════════════ */
/*  Client Handler Thread                                           */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Thread function for a single connected client.
 *
 * Reads request frames in a loop, dispatches commands, and sends
 * responses.  Exits when the client disconnects or an error occurs.
 *
 * The thread argument is a ClientConnection pointer.  On exit the
 * function removes itself from the server's client list and cleans up.
 */
static DWORD WINAPI client_handler(LPVOID arg) {
  ClientConnection *client = (ClientConnection *)arg;
  PipeServer *server = client->server;
  HANDLE hPipe = client->hPipe;

  LOG_I("pipe_server", "Client handler thread started (pipe=%p, threadId=%lu)",
        (void *)hPipe, GetCurrentThreadId());

  BOOL recvResult = FALSE;

  while (server->running) {
    LOG_D("pipe_server", "Client handler: waiting for request from pipe %p",
          (void *)hPipe);
    char *payload = NULL;
    DWORD payloadLen = 0;
    recvResult = pipe_frame_recv(hPipe, &payload, &payloadLen);
    LOG_D("pipe_server",
          "Client handler: pipe_frame_recv returned %d, payloadLen=%lu, "
          "lastError=%lu",
          recvResult, payloadLen, GetLastError());

    if (!recvResult) {
      DWORD err = GetLastError();
      if (err != ERROR_BROKEN_PIPE && err != ERROR_PIPE_NOT_CONNECTED) {
        LOG_D("pipe_server", "pipe_frame_recv error %lu on pipe %p", err,
              (void *)hPipe);
      }
      break;
    }

    /* Parse the command */
    int cmd = extract_command(payload);
    char response[PIPE_STRUCT_JSON_MAX];
    response[0] = '\0';

    LOG_I("pipe_server", "Client handler: received command %d, payload='%s'",
          cmd, payload);

    switch (cmd) {
    case PIPE_CMD_LIST_INSTANCES:
      cmd_list_instances(server, response, sizeof(response));
      break;

    case PIPE_CMD_START_INSTANCE:
      cmd_start_instance(server, payload, response, sizeof(response));
      break;

    case PIPE_CMD_STOP_INSTANCE:
      cmd_stop_instance(server, payload, response, sizeof(response));
      break;

    case PIPE_CMD_RESTART_INSTANCE:
      cmd_restart_instance(server, payload, response, sizeof(response));
      break;

    case PIPE_CMD_RELOAD_CONFIG:
      cmd_reload_config(server, response, sizeof(response));
      break;

    case PIPE_CMD_GET_LOGS:
      cmd_get_logs(server, payload, response, sizeof(response));
      break;

    default:
      _snprintf(response, sizeof(response),
                "{\"type\":\"response\",\"success\":0,"
                "\"error_message\":\"Unknown command %d\","
                "\"json_payload\":\"\"}",
                cmd);
      break;
    }

    HeapFree(GetProcessHeap(), 0, payload);

    /* Send the response — synchronously (dedicated thread) */
    size_t responseLenSize = strnlen_s(response, sizeof(response));
    if (responseLenSize >= sizeof(response))
      responseLenSize = sizeof(response) - 1;
    DWORD responseLen = (DWORD)responseLenSize;
    if (responseLen > 0) {
      LOG_D("pipe_server", "Client handler: sending response (%lu bytes): %s",
            responseLen, response);
      BOOL sendResult = pipe_frame_send(hPipe, response, responseLen);
      LOG_D("pipe_server",
            "Client handler: pipe_frame_send returned %d, lastError=%lu",
            sendResult, GetLastError());
      if (!sendResult) {
        LOG_D("pipe_server", "pipe_frame_send error %lu on pipe %p",
              GetLastError(), (void *)hPipe);
        break;
      }
    }
  }

  LOG_I("pipe_server",
        "Client handler: exiting loop (recvResult=%d, lastError=%lu)",
        recvResult, GetLastError());
  LOG_I("pipe_server", "Client disconnected (pipe=%p)", (void *)hPipe);

  /* Remove from server list and clean up */
  client_remove(server, hPipe);
  return 0;
}

/* ════════════════════════════════════════════════════════════════ */
/*  Listener Thread                                                 */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Thread function that accepts incoming named-pipe connections.
 *
 * Creates a pipe instance and calls ConnectNamedPipe in blocking mode.
 * On connection a client-handler thread is spawned.  The blocking call
 * is aborted by pipe_server_stop via CancelSynchronousIo.
 */
static DWORD WINAPI listener_thread(LPVOID arg) {
  PipeServer *server = (PipeServer *)arg;

  LOG_I("pipe_server", "Listener thread started (pipe=%s)", server->pipeName);

  while (server->running) {
    /* ── Create a pipe instance for the next client ──────── */
    HANDLE hPipe = create_pipe_instance(server->pipeName);
    if (hPipe == INVALID_HANDLE_VALUE) {
      LOG_E("pipe_server", "create_pipe_instance failed in listener loop");
      Sleep(1000);
      continue;
    }

    /* ── Accept a connection (blocking) ──────────────────── */
    BOOL connected = ConnectNamedPipe(hPipe, NULL);
    if (!connected) {
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        /* Race: client connected between CreateNamedPipe and
         * ConnectNamedPipe — treat as connected. */
      } else if (err == ERROR_OPERATION_ABORTED) {
        /* Shutting down via CancelSynchronousIo */
        CloseHandle(hPipe);
        break;
      } else {
        LOG_W("pipe_server", "ConnectNamedPipe failed (error %lu)", err);
        CloseHandle(hPipe);
        continue;
      }
    }

    /* ── Client connected — spawn handler thread ────────── */
    ClientConnection *client =
        (ClientConnection *)calloc(1, sizeof(ClientConnection));
    if (!client) {
      LOG_E("pipe_server", "Out of memory for client connection");
      CloseHandle(hPipe);
      continue;
    }
    client->hPipe = hPipe;
    client->server = server;

    HANDLE hThread = CreateThread(NULL, 0, client_handler, client, 0, NULL);
    if (!hThread) {
      LOG_E("pipe_server", "CreateThread for client failed: %lu",
            GetLastError());
      CloseHandle(hPipe);
      free(client);
      continue;
    }
    client->hThread = hThread;

    /* Add to the server's client list */
    EnterCriticalSection(&server->lock);
    client->next = (ClientConnection *)server->clients;
    *(ClientConnection **)&server->clients = client;
    LeaveCriticalSection(&server->lock);
  }

  LOG_I("pipe_server", "Listener thread exiting");
  return 0;
}

/* ════════════════════════════════════════════════════════════════ */
/*  Command Handlers                                                */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Handle PIPE_CMD_LIST_INSTANCES.
 *
 * Response format:
 *   {"type":"response","success":1,"error_message":"",
 *    "json_payload":"[{\"name\":\"...\",\"state\":N,\"viewer_count\":N,
 *    \"backend_hostname\":\"...\",\"backend_port\":N},...]"}
 */
static void cmd_list_instances(PipeServer *server, char *response,
                               size_t respSize) {
  if (!server->mgr) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Instance manager unavailable\","
              "\"json_payload\":\"\"}");
    return;
  }

  /*
   * Build the JSON array by iterating all instances.
   * We accumulate into a temporary buffer first, then escape it
   * into the json_payload field.
   */
  char array[PIPE_STRUCT_JSON_MAX];
  size_t pos = 0;
  array[0] = '\0';

  for (unsigned int i = 0; i < server->mgr->instanceCount; i++) {
    PipeInstanceInfo info;
    memset(&info, 0, sizeof(info));

    if (inst_mgr_get_info(server->mgr, i, &info) != 0)
      continue;

    int written = _snprintf(
        array + pos, sizeof(array) - pos,
        "%s{\"name\":\"%s\",\"state\":%d,\"viewer_count\":%lu,"
        "\"backend_hostname\":\"%s\",\"backend_port\":%u,"
        "\"viewer_port\":%u}",
        (pos > 0) ? "," : "", info.name, (int)info.state, info.viewer_count,
        info.backend_hostname, (unsigned int)info.backend_port,
        (unsigned int)info.viewer_port);

    if (written > 0 && (size_t)written < sizeof(array) - pos)
      pos += (size_t)written;
    else
      break; /* buffer full — truncate */
  }

  /* Escape the array and wrap in a response with json_payload */
  char escaped[PIPE_STRUCT_JSON_MAX];
  json_escape_string(array, escaped, sizeof(escaped));

  _snprintf(response, respSize,
            "{\"type\":\"response\",\"success\":1,"
            "\"error_message\":\"\","
            "\"json_payload\":\"{\\\"instances\\\":[%s]}\"}",
            escaped);
}

/**
 * @brief Handle PIPE_CMD_START_INSTANCE.
 *
 * Response:
 * {"type":"response","success":1,"error_message":"","json_payload":""} or
 * {"type":"response","success":0,"error_message":"...","json_payload":""}
 */
static void cmd_start_instance(PipeServer *server, const char *payload,
                               char *response, size_t respSize) {
  if (!server->mgr) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Instance manager unavailable\","
              "\"json_payload\":\"\"}");
    return;
  }

  char name[128];
  if (extract_instance_name(payload, name, sizeof(name)) != 0) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Missing or invalid instance_name\","
              "\"json_payload\":\"\"}");
    return;
  }

  char escaped_name[256];
  json_escape_string(name, escaped_name, sizeof(escaped_name));

  int ret = inst_mgr_start(server->mgr, name);
  if (ret == 0) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":1,"
              "\"error_message\":\"\",\"json_payload\":\"\"}");
  } else {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Failed to start '%s'\","
              "\"json_payload\":\"\"}",
              escaped_name);
  }
}

/**
 * @brief Handle PIPE_CMD_STOP_INSTANCE.
 *
 * Response:
 * {"type":"response","success":1,"error_message":"","json_payload":""} or
 * {"type":"response","success":0,"error_message":"...","json_payload":""}
 */
static void cmd_stop_instance(PipeServer *server, const char *payload,
                              char *response, size_t respSize) {
  if (!server->mgr) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Instance manager unavailable\","
              "\"json_payload\":\"\"}");
    return;
  }

  char name[128];
  if (extract_instance_name(payload, name, sizeof(name)) != 0) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Missing or invalid instance_name\","
              "\"json_payload\":\"\"}");
    return;
  }

  char escaped_name[256];
  json_escape_string(name, escaped_name, sizeof(escaped_name));

  int ret = inst_mgr_stop(server->mgr, name);
  if (ret == 0) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":1,"
              "\"error_message\":\"\",\"json_payload\":\"\"}");
  } else {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Failed to stop '%s'\","
              "\"json_payload\":\"\"}",
              escaped_name);
  }
}

/**
 * @brief Handle PIPE_CMD_RESTART_INSTANCE.
 *
 * Response:
 * {"type":"response","success":1,"error_message":"","json_payload":""} or
 * {"type":"response","success":0,"error_message":"...","json_payload":""}
 */
static void cmd_restart_instance(PipeServer *server, const char *payload,
                                 char *response, size_t respSize) {
  if (!server->mgr) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Instance manager unavailable\","
              "\"json_payload\":\"\"}");
    return;
  }

  char name[128];
  if (extract_instance_name(payload, name, sizeof(name)) != 0) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Missing or invalid instance_name\","
              "\"json_payload\":\"\"}");
    return;
  }

  char escaped_name[256];
  json_escape_string(name, escaped_name, sizeof(escaped_name));

  int ret = inst_mgr_restart(server->mgr, name);
  if (ret == 0) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":1,"
              "\"error_message\":\"\",\"json_payload\":\"\"}");
  } else {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Failed to restart '%s'\","
              "\"json_payload\":\"\"}",
              escaped_name);
  }
}

/**
 * @brief Handle PIPE_CMD_RELOAD_CONFIG.
 *
 * Response:
 * {"type":"response","success":1,"error_message":"","json_payload":""} or
 * {"type":"response","success":0,"error_message":"...","json_payload":""}
 */
static void cmd_reload_config(PipeServer *server, char *response,
                              size_t respSize) {
  if (!server->mgr) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Instance manager unavailable\","
              "\"json_payload\":\"\"}");
    return;
  }

  const char *configPath = server->mgr->configPath
                               ? server->mgr->configPath
                               : "C:\\ProgramData\\OmniRDP\\config.ini";

  int ret = inst_mgr_reload_config(server->mgr, configPath);
  if (ret == 0) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":1,"
              "\"error_message\":\"\",\"json_payload\":\"\"}");
  } else {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Config reload failed\","
              "\"json_payload\":\"\"}");
  }
}

/**
 * @brief Handle PIPE_CMD_GET_LOGS.
 *
 * Reads the last N lines from the service log file and returns them
 * as a JSON array.  Optionally filters by instance_name.
 *
 * Response: {"type":"response","success":1,"error_message":"",
 *           "json_payload":"{\"logs\":[\"line1\",\"line2\",...]}"}
 */
static void cmd_get_logs(PipeServer *server, const char *payload,
                         char *response, size_t respSize) {
  /* ── Determine log file path ─────────────────────────────── */
  char logDirBuf[MAX_PATH];
  const char *logDir = "C:\\ProgramData\\OmniRDP\\logs";
  if (server->mgr && server->mgr->config &&
      server->mgr->config->service.log_dir[0] != '\0') {
    logDir = server->mgr->config->service.log_dir;
  }

  /* Append service name subdirectory to match svc_service.c log path */
  _snprintf(logDirBuf, sizeof(logDirBuf), "%s\\%s", logDir,
            server->serviceName);
  logDirBuf[sizeof(logDirBuf) - 1] = '\0';

  char logPath[MAX_PATH];
  _snprintf(logPath, sizeof(logPath), "%s\\%s", logDirBuf, LOG_FILE_NAME);
  logPath[sizeof(logPath) - 1] = '\0';

  /* ── Open the log file ───────────────────────────────────── */
  /* Use _fsopen with _SH_DENYNO to allow concurrent reads.
   * Both svc_log.c and this function use _SH_DENYNO so the file
   * can be read while the service is actively writing to it. */
  FILE *f = _fsopen(logPath, "r", _SH_DENYNO);
  if (!f) {
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Cannot open log file\","
              "\"json_payload\":\"\"}");
    return;
  }

  /* ── Seek to end, then scan backwards for newlines ───────── */
  /* Strategy: read chunks backwards from the end, counting
   * newlines until we have MAX_LOG_LINES or reach the start. */
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Cannot seek log file\","
              "\"json_payload\":\"\"}");
    return;
  }

  long fileSize = ftell(f);
  if (fileSize <= 0) {
    fclose(f);
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":1,"
              "\"error_message\":\"\","
              "\"json_payload\":\"{\\\"logs\\\":[]}\"}");
    return;
  }

  /* Read backwards in chunks counting newlines.  We re-read the
   * tail from the discovered offset to get the actual lines. */
  long tailStart = fileSize; /* byte offset to start reading from */
  unsigned int lineCount = 0;
  char chunk[TAIL_CHUNK_SIZE];
  long readPos = fileSize;

  while (readPos > 0 && lineCount < MAX_LOG_LINES) {
    long chunkSize =
        (readPos >= (long)TAIL_CHUNK_SIZE) ? (long)TAIL_CHUNK_SIZE : readPos;
    readPos -= chunkSize;
    if (fseek(f, readPos, SEEK_SET) != 0)
      break;

    size_t actual = fread(chunk, 1, (size_t)chunkSize, f);
    if (actual == 0)
      break;

    /* Count newlines in this chunk (skip the last char to avoid
     * double-counting boundaries on subsequent iterations). */
    for (size_t i = actual; i > 0; i--) {
      if (chunk[i - 1] == '\n') {
        lineCount++;
        if (lineCount >= MAX_LOG_LINES) {
          /* Adjust tailStart to the byte after this newline */
          tailStart = readPos + (long)i;
          break;
        }
      }
    }
    if (lineCount >= MAX_LOG_LINES)
      break;

    tailStart = readPos;
  }

  /* ── Read the tail lines ─────────────────────────────────── */
  if (tailStart < 0)
    tailStart = 0;

  if (fseek(f, tailStart, SEEK_SET) != 0) {
    fclose(f);
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Cannot seek in log file\","
              "\"json_payload\":\"\"}");
    return;
  }

  /* Read remaining content */
  size_t tailBytes = (size_t)(fileSize - tailStart);
  char *tailBuf = (char *)malloc(tailBytes + 1);
  if (!tailBuf) {
    fclose(f);
    _snprintf(response, respSize,
              "{\"type\":\"response\",\"success\":0,"
              "\"error_message\":\"Out of memory\","
              "\"json_payload\":\"\"}");
    return;
  }

  size_t bytesRead = fread(tailBuf, 1, tailBytes, f);
  fclose(f);
  tailBuf[bytesRead] = '\0';

  /* ── Build JSON array of lines ───────────────────────────── */
  /* Escape backslashes, quotes, and control characters. */
  char linesBuf[PIPE_STRUCT_JSON_MAX];
  size_t outPos = 0;
  linesBuf[0] = '\0';

  char *line = tailBuf;
  char *nextLine = NULL;
  BOOL first = TRUE;

  while (line && *line && outPos < sizeof(linesBuf) - 64) {
    /* Find end of line */
    nextLine = strchr(line, '\n');
    if (nextLine)
      *nextLine = '\0';

    /* Trim trailing \r */
    size_t maxLineLen = tailBytes - (size_t)(line - tailBuf);
    size_t lineLen = strnlen_s(line, maxLineLen + 1);
    while (lineLen > 0 && (line[lineLen - 1] == '\r'))
      line[--lineLen] = '\0';

    if (!first) {
      if (outPos < sizeof(linesBuf) - 1)
        linesBuf[outPos++] = ',';
    }
    first = FALSE;

    linesBuf[outPos++] = '"';

    /* Copy with minimal JSON escaping */
    for (const char *src = line; *src && outPos < sizeof(linesBuf) - 8; src++) {
      char c = *src;
      switch (c) {
      case '"':
        linesBuf[outPos++] = '\\';
        linesBuf[outPos++] = '"';
        break;
      case '\\':
        linesBuf[outPos++] = '\\';
        linesBuf[outPos++] = '\\';
        break;
      case '\t':
        linesBuf[outPos++] = '\\';
        linesBuf[outPos++] = 't';
        break;
      case '\r': /* skip */
        break;
      case '\n': /* skip */
        break;
      default:
        if ((unsigned char)c >= 0x20)
          linesBuf[outPos++] = c;
        else
          ; /* skip other control chars */
        break;
      }
    }

    linesBuf[outPos++] = '"';

    if (outPos >= sizeof(linesBuf) - 64)
      break;

    line = nextLine ? nextLine + 1 : NULL;
  }

  linesBuf[outPos] = '\0';
  free(tailBuf);

  /* Escape the log lines array for embedding in json_payload */
  char escaped[PIPE_STRUCT_JSON_MAX];
  json_escape_string(linesBuf, escaped, sizeof(escaped));

  _snprintf(response, respSize,
            "{\"type\":\"response\",\"success\":1,"
            "\"error_message\":\"\","
            "\"json_payload\":\"{\\\"logs\\\":[%s]}\"}",
            escaped);
}

/* ════════════════════════════════════════════════════════════════ */
/*  Push Helpers                                                    */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Send a length-prefixed frame to a single client.
 *
 * Synchronous write (the pipe handle is NOT overlapped).
 *
 * @param hPipe   Client pipe handle.
 * @param json    JSON payload to send.
 * @param jsonLen Length of the payload in bytes.
 * @return TRUE if the frame was sent successfully, FALSE otherwise.
 */
static BOOL push_write_frame(HANDLE hPipe, const char *json, DWORD jsonLen) {
  DWORD lenLe = jsonLen;
  DWORD written = 0;

  /* ── Send the 4-byte header ────────────────────────────────── */
  if (!WriteFile(hPipe, &lenLe, PIPE_HEADER_SIZE, &written, NULL))
    return FALSE;

  /* ── Send the payload (if any) ─────────────────────────────── */
  if (jsonLen > 0) {
    if (!WriteFile(hPipe, json, jsonLen, &written, NULL))
      return FALSE;
  }

  return TRUE;
}

/**
 * @brief Iterate over all connected clients and try to send a push
 *        frame to each one.
 *
 * Best-effort: if a client is unresponsive or disconnected it is
 * silently removed from the list.
 *
 * @param server  Pipe server context.
 * @param json    JSON payload to send.
 * @param jsonLen Length of the payload.
 */
static void push_to_all(PipeServer *server, const char *json, DWORD jsonLen) {
  if (!server || !json || jsonLen == 0)
    return;

  EnterCriticalSection(&server->lock);

  ClientConnection *prev = NULL;
  ClientConnection *curr = (ClientConnection *)server->clients;

  while (curr) {
    ClientConnection *next = curr->next;

    if (!push_write_frame(curr->hPipe, json, jsonLen)) {
      /* Client is dead — remove it from the list */
      LOG_D("pipe_server", "Removing dead client (%p) during push",
            (void *)curr->hPipe);

      if (prev)
        prev->next = next;
      else
        *(ClientConnection **)&server->clients = next;

      FlushFileBuffers(curr->hPipe);
      DisconnectNamedPipe(curr->hPipe);
      CloseHandle(curr->hPipe);
      if (curr->hThread)
        CloseHandle(curr->hThread);
      free(curr);
    } else {
      prev = curr;
    }

    curr = next;
  }

  LeaveCriticalSection(&server->lock);
}

/* ════════════════════════════════════════════════════════════════ */
/*  Public API                                                      */
/* ════════════════════════════════════════════════════════════════ */

int pipe_server_init(PipeServer *server, const char *pipeName,
                     InstanceManager *mgr, const char *serviceName) {
  if (!server || !pipeName || !mgr || !serviceName) {
    LOG_E("pipe_server", "pipe_server_init: invalid parameters");
    return -1;
  }

  memset(server, 0, sizeof(*server));
  server->mgr = mgr;
  server->running = FALSE;
  strncpy_s(server->serviceName, sizeof(server->serviceName), serviceName,
            _TRUNCATE);

  /* ── Build the full pipe path ──────────────────────────────── */
  int ret = _snprintf(server->pipeName, sizeof(server->pipeName),
                      "\\\\.\\pipe\\%s", pipeName);
  if (ret < 0 || (size_t)ret >= sizeof(server->pipeName)) {
    LOG_E("pipe_server", "Pipe name is too long");
    memset(server, 0, sizeof(*server));
    return -1;
  }

  /* ── Initialise the client-list critical section ───────────── */
  if (!InitializeCriticalSectionAndSpinCount(&server->lock,
                                             PIPE_SERVER_CS_SPIN_COUNT)) {
    LOG_E("pipe_server", "InitializeCriticalSectionAndSpinCount failed");
    memset(server, 0, sizeof(*server));
    return -1;
  }
  g_initialized_pipe_server = server;

  /* ── Pre-create the first pipe instance to verify security ─── */
  HANDLE hPipe = create_pipe_instance(server->pipeName);
  if (hPipe == INVALID_HANDLE_VALUE) {
    LOG_E("pipe_server", "Initial pipe creation failed — check ACLs");
    DeleteCriticalSection(&server->lock);
    if (g_initialized_pipe_server == server)
      g_initialized_pipe_server = NULL;
    memset(server, 0, sizeof(*server));
    return -1;
  }
  /* Close this test instance; the listener will create real ones. */
  CloseHandle(hPipe);

  /* ── Start the listener thread ─────────────────────────────── */
  server->running = TRUE;

  server->hListenerThread =
      CreateThread(NULL, 0, listener_thread, server, 0, NULL);
  if (!server->hListenerThread) {
    LOG_E("pipe_server", "CreateThread(listener) failed: %lu", GetLastError());
    server->running = FALSE;
    DeleteCriticalSection(&server->lock);
    if (g_initialized_pipe_server == server)
      g_initialized_pipe_server = NULL;
    memset(server, 0, sizeof(*server));
    return -1;
  }

  LOG_I("pipe_server", "Pipe server initialized on %s", server->pipeName);
  return 0;
}

void pipe_server_stop(PipeServer *server) {
  if (!pipe_server_is_initialized(server))
    return;

  LOG_I("pipe_server", "Stopping pipe server...");

  /* ── Signal shutdown ──────────────────────────────────────── */
  server->running = FALSE;

  /* Cancel any blocking ConnectNamedPipe in the listener thread */
  if (server->hListenerThread) {
    CancelSynchronousIo(server->hListenerThread);
  }

  /* ── Wait for the listener thread to exit ──────────────────── */
  if (server->hListenerThread) {
    if (WaitForSingleObject(server->hListenerThread, 5000) == WAIT_TIMEOUT) {
      LOG_W("pipe_server", "Listener thread did not exit within 5s");
    }
    CloseHandle(server->hListenerThread);
    server->hListenerThread = NULL;
  }

  /* ── Disconnect all clients and detach the list ────────────── */
  EnterCriticalSection(&server->lock);

  /* Detach the client list so client_remove (called from exiting
   * client handler threads) will find an empty list and no-op.
   * This avoids a deadlock: client_remove acquires the same lock
   * we release below. */
  ClientConnection *clients = (ClientConnection *)server->clients;
  server->clients = NULL;

  /* Cancel I/O and disconnect each pipe to force client handler
   * threads to wake up and exit. */
  ClientConnection *curr = clients;
  while (curr) {
    CancelIoEx(curr->hPipe, NULL);
    FlushFileBuffers(curr->hPipe);
    DisconnectNamedPipe(curr->hPipe);
    curr = curr->next;
  }

  /* Leave the lock BEFORE waiting for threads — this prevents
   * the deadlock where client_handler -> client_remove blocks
   * on the same lock we hold. */
  LeaveCriticalSection(&server->lock);

  /* ── Wait for each client thread and clean up ──────────────── */
  curr = clients;
  while (curr) {
    ClientConnection *next = curr->next;

    if (curr->hThread) {
      if (WaitForSingleObject(curr->hThread, 1000) == WAIT_TIMEOUT) {
        LOG_W("pipe_server", "Client thread %p did not exit within 1s",
              (void *)curr->hThread);
      }
      CloseHandle(curr->hThread);
      curr->hThread = NULL;
    }

    if (curr->hPipe) {
      CloseHandle(curr->hPipe);
      curr->hPipe = NULL;
    }

    free(curr);
    curr = next;
  }

  /* ── Clean up synchronisation objects ──────────────────────── */
  DeleteCriticalSection(&server->lock);
  if (g_initialized_pipe_server == server)
    g_initialized_pipe_server = NULL;
  memset(server, 0, sizeof(*server));

  LOG_I("pipe_server", "Pipe server stopped");
}

void pipe_server_push_stats(PipeServer *server) {
  if (!server || !server->running || !server->mgr)
    return;

  /* Build a compact stats JSON payload.
   * Format: {"type":"push","push_type":"stats","stats":{...}} */
  char json[PIPE_STRUCT_JSON_MAX];
  size_t pos = 0;

  int n = _snprintf(json + pos, sizeof(json) - pos,
                    "{\"type\":\"push\",\"push_type\":\"stats\",\"stats\":{");
  if (n > 0)
    pos += (size_t)n;

  BOOL first = TRUE;
  for (unsigned int i = 0; i < server->mgr->instanceCount; i++) {
    PipeInstanceInfo info;
    memset(&info, 0, sizeof(info));
    if (inst_mgr_get_info(server->mgr, i, &info) != 0)
      continue;

    n = _snprintf(json + pos, sizeof(json) - pos,
                  "%s\"%s\":{\"state\":%d,\"viewer_count\":%lu}",
                  first ? "" : ",", info.name, (int)info.state,
                  info.viewer_count);
    if (n > 0 && (size_t)n < sizeof(json) - pos)
      pos += (size_t)n;
    first = FALSE;

    if (pos >= sizeof(json) - 64)
      break;
  }

  n = _snprintf(json + pos, sizeof(json) - pos, "}}");
  if (n > 0)
    pos += (size_t)n;

  push_to_all(server, json, (DWORD)pos);
}

void pipe_server_push_event(PipeServer *server, const char *eventType,
                            const char *instanceName) {
  if (!server || !server->running || !eventType)
    return;

  char json[PIPE_STRUCT_JSON_MAX];
  int n;

  if (instanceName && instanceName[0] != '\0') {
    n = _snprintf(json, sizeof(json),
                  "{\"type\":\"push\",\"push_type\":\"event\","
                  "\"event\":\"%s\",\"instance_name\":\"%s\"}",
                  eventType, instanceName);
  } else {
    n = _snprintf(json, sizeof(json),
                  "{\"type\":\"push\",\"push_type\":\"event\","
                  "\"event\":\"%s\"}",
                  eventType);
  }

  if (n < 0 || (size_t)n >= sizeof(json))
    return;

  push_to_all(server, json, (DWORD)n);
}
