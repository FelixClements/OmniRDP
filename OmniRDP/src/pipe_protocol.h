/**
 * @file pipe_protocol.h
 * @brief Shared IPC protocol for OmniRDP named pipe communication
 *
 * Defines the wire format, message types, and framing functions used
 * between OmniRDP-svc.exe (the service) and OmniRDP-tray.exe (the
 * tray application).  This header is FreeRDP-independent and Windows-only.
 *
 * ── Wire Format ──────────────────────────────────────────────────
 *
 * All messages use length-prefixed framing:
 *
 *   | 4 bytes LE: payload length N | N bytes: UTF-8 JSON payload |
 *
 * The 4-byte length prefix is a little-endian DWORD.  The payload is
 * a UTF-8 encoded JSON string (NOT null-terminated on the wire; the
 * receiving function null-terminates for convenience).
 *
 * ── Message Flow ─────────────────────────────────────────────────
 *
 * Tray → Service (Request-Response):
 *   The tray sends a PipeCommand as a JSON request.  The service
 *   processes it and replies with a JSON response (success/fail).
 *
 * Service → Tray (Unsolicited Push):
 *   The service may push stats/event messages at any time.  The tray
 *   distinguishes pushes from responses by a "type" field in the JSON.
 *
 * ── JSON Serialization ──────────────────────────────────────────
 *
 * This protocol deliberately avoids external JSON libraries.  Messages
 * are built with sprintf() / StringCbPrintfA() and parsed with sscanf()
 * or simple string comparison (strstr, etc.).  See the JSON helper note
 * at the bottom of this header for examples.
 */

#ifndef PIPE_PROTOCOL_H
#define PIPE_PROTOCOL_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════ */
/*  Wire Format Constants                                         */
/* ════════════════════════════════════════════════════════════════ */

/** Size of the length prefix in bytes (4-byte little-endian DWORD). */
#define PIPE_HEADER_SIZE 4U

/**
 * Maximum allowed payload size: 1 MB.
 * This prevents runaway allocations while allowing large responses
 * (e.g. log tails, instance lists with many entries).
 */
#define PIPE_MAX_PAYLOAD_SIZE (1024U * 1024U)

/**
 * Default maximum JSON size embedded in the helper structs.
 * Larger payloads should use the framing functions directly with
 * dynamically allocated buffers.
 */
#define PIPE_STRUCT_JSON_MAX 8192U

/* ════════════════════════════════════════════════════════════════ */
/*  Command Enums                                                  */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Commands sent from the tray app to the service.
 *
 * These follow a request-response pattern.  The tray sends a JSON
 * request with one of these commands; the service replies with a
 * JSON response containing success/failure status and optional data.
 */
typedef enum {
  /** Get all managed instances and their current status. */
  PIPE_CMD_LIST_INSTANCES = 0,

  /** Start a stopped instance.  Requires instance_name. */
  PIPE_CMD_START_INSTANCE,

  /** Gracefully stop a running instance.  Requires instance_name. */
  PIPE_CMD_STOP_INSTANCE,

  /** Stop then restart an instance.  Requires instance_name. */
  PIPE_CMD_RESTART_INSTANCE,

  /** Hot-reload the configuration file without restarting the service. */
  PIPE_CMD_RELOAD_CONFIG,

  /** Retrieve recent log lines (optional: instance_name to filter). */
  PIPE_CMD_GET_LOGS,

  /** Number of valid commands (used as a sentinel / array size). */
  PIPE_CMD_COUNT
} PipeCommand;

/**
 * @brief Push message types sent unsolicited from the service to the tray.
 *
 * These messages arrive asynchronously; the tray must distinguish them
 * from command responses by inspecting the JSON "type" field.
 */
typedef enum {
  /** Periodic per-instance statistics (viewer count, uptime, etc.). */
  PIPE_PUSH_STATS = 0,

  /** Event notification — instance crashed, reconnected, config error. */
  PIPE_PUSH_EVENT,

  /** Number of valid push types (sentinel). */
  PIPE_PUSH_COUNT
} PipePushType;

/* ════════════════════════════════════════════════════════════════ */
/*  Instance State Enum                                            */
/* ════════════════════════════════════════════════════════════════ */

/** @brief Current lifecycle state of a managed RDP instance. */
typedef enum {
  /** Instance is not running (initial state after stop or on startup). */
  INSTANCE_STOPPED = 0,

  /** Instance is being launched / started (transitional). */
  INSTANCE_STARTING,

  /** Instance is active, connected to the backend, and accepting viewers. */
  INSTANCE_RUNNING,

  /** Instance lost its backend connection and is attempting to reconnect. */
  INSTANCE_RECONNECTING
} PipeInstanceState;

/* ════════════════════════════════════════════════════════════════ */
/*  Message Structs                                                */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Logical representation of a request from tray to service.
 *
 * These fields are typically serialised to JSON for the wire;
 * the struct is provided for programmatic convenience when building
 * or parsing messages.  For payloads exceeding PIPE_STRUCT_JSON_MAX
 * bytes, use the framing functions directly with manually managed
 * JSON strings.
 */
typedef struct {
  /** The command to execute (see PipeCommand enum). */
  PipeCommand command;

  /**
   * Target instance name, or empty string if the command does not
   * apply to a specific instance (e.g. PIPE_CMD_LIST_INSTANCES).
   */
  char instance_name[128];

  /**
   * Additional JSON payload to merge into the request message.
   * Can be empty for simple commands.  For large payloads, bypass
   * this struct and pass the JSON directly to pipe_frame_send().
   */
  char json_payload[PIPE_STRUCT_JSON_MAX];
} PipeRequest;

/**
 * @brief Logical representation of a response from service to tray.
 */
typedef struct {
  /** Non-zero (TRUE) if the command succeeded, zero (FALSE) on failure. */
  int success;

  /** Human-readable error message, empty string when success is TRUE. */
  char error_message[512];

  /**
   * Response data as JSON.  The structure depends on the command:
   *   - PIPE_CMD_LIST_INSTANCES : array of PipeInstanceInfo objects
   *   - PIPE_CMD_GET_LOGS      : array of log line strings
   *   - other commands          : typically empty or a simple status object
   */
  char json_payload[PIPE_STRUCT_JSON_MAX];
} PipeResponse;

/**
 * @brief Describes a single managed RDP instance.
 *
 * Used primarily in the JSON response for PIPE_CMD_LIST_INSTANCES.
 */
typedef struct {
  /** Instance name (matches [instance:<name>] in config). */
  char name[128];

  /** Current lifecycle state (see PipeInstanceState enum). */
  PipeInstanceState state;

  /** Number of viewers currently connected to this instance. */
  DWORD viewer_count;

  /** Backend RDP server hostname or IP address. */
  char backend_hostname[256];

  /** Backend RDP server TCP port (typically 3389). */
  uint16_t backend_port;
} PipeInstanceInfo;

/* ════════════════════════════════════════════════════════════════ */
/*  Framing Functions                                              */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @brief Send a length-prefixed frame over a named pipe.
 *
 * Writes a 4-byte little-endian DWORD payload length followed
 * immediately by @p payload_len bytes from @p json_payload.
 * This is a blocking, synchronous write.
 *
 * @param hPipe        Handle to a connected named pipe.
 * @param json_payload Pointer to the UTF-8 JSON payload bytes to send.
 *                     Does not need to be null-terminated (exactly
 *                     @p payload_len bytes are transmitted).
 * @param payload_len  Number of bytes to send from json_payload.
 *                     Must not exceed PIPE_MAX_PAYLOAD_SIZE.
 * @return TRUE on success.  FALSE on failure — call GetLastError()
 *         for extended error information.
 */
BOOL pipe_frame_send(HANDLE hPipe, const char *json_payload, DWORD payload_len);

/**
 * @brief Receive a length-prefixed frame from a named pipe.
 *
 * Reads a 4-byte little-endian DWORD payload length, then reads
 * exactly that many payload bytes.  Allocates a buffer via
 * HeapAlloc(GetProcessHeap(), ...) to hold the payload, null-terminates
 * it, and returns it through @p payload_out.
 *
 * @param hPipe           Handle to a connected named pipe.
 * @param payload_out     Receives a pointer to the allocated buffer
 *                        containing the JSON payload (null-terminated).
 *                        The caller must free this buffer with
 *                        HeapFree(GetProcessHeap(), 0, *payload_out).
 *                        Set to NULL on failure.
 * @param payload_len_out Receives the number of bytes in the payload
 *                        (excluding the null terminator).  Set to 0
 *                        on failure.
 * @return TRUE on success.  FALSE on failure — call GetLastError()
 *         for extended error information.
 *
 * @note On success, the caller owns the buffer returned in
 *       *payload_out and must free it via HeapFree().
 */
BOOL pipe_frame_recv(HANDLE hPipe, char **payload_out, DWORD *payload_len_out);

/* ════════════════════════════════════════════════════════════════ */
/*  JSON Helper Note                                               */
/* ════════════════════════════════════════════════════════════════ */

/**
 * @note Minimal JSON Serialisation Strategy
 *
 * This protocol deliberately avoids external JSON library dependencies
 * (no cJSON, jsmn, nlohmann, etc.).  Instead, messages are built and
 * parsed using simple C string operations:
 *
 *   ── Building a request (sending) ──
 *   @code
 *     char buf[512];
 *     int n = sprintf(buf,
 *         "{\"cmd\":%d,\"instance_name\":\"%s\"}",
 *         PIPE_CMD_START_INSTANCE, "myinstance");
 *     pipe_frame_send(hPipe, buf, (DWORD)n);
 *   @endcode
 *
 *   ── Parsing a response (receiving) ──
 *   @code
 *     char *payload = NULL;
 *     DWORD len = 0;
 *     if (pipe_frame_recv(hPipe, &payload, &len)) {
 *         if (strstr(payload, "\"success\":1")) {
 *             // command succeeded
 *         } else {
 *             // parse error_message field
 *         }
 *         HeapFree(GetProcessHeap(), 0, payload);
 *     }
 *   @endcode
 *
 *   ── Distinguishing pushes from responses ──
 *   Pushes include a "type" field in the JSON root:
 *   @code
 *     // Incoming message from service
 *     char *msg = ...;
 *     if (strstr(msg, "\"type\":\"push\"")) {
 *         // This is an unsolicited push — check push_type
 *         if (strstr(msg, "\"push_type\":\"stats\"")) { ... }
 *         if (strstr(msg, "\"push_type\":\"event\"")) { ... }
 *     } else {
 *         // This is a response to our command — check success
 *     }
 *   @endcode
 *
 * If message complexity grows significantly (nested objects, arrays,
 * etc.), consider adopting a lightweight JSON library.  The wire
 * format (length-prefixed UTF-8 JSON) remains unchanged regardless
 * of the serialisation method.
 */

#ifdef __cplusplus
}
#endif

#endif /* PIPE_PROTOCOL_H */
