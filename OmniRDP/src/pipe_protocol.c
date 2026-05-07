/**
 * @file pipe_protocol.c
 * @brief Framing functions for OmniRDP named pipe IPC protocol
 *
 * Implements the length-prefixed frame send/receive functions declared
 * in pipe_protocol.h.  See that header for the wire format definition.
 *
 * This file is FreeRDP-independent and uses only the Windows API.
 */

#include "pipe_protocol.h"
#include "svc_log.h"
#include <windows.h>

/* ──────────────────────────────────────────────────────────────────
 * Helper: write exactly `total_bytes` from `buf` to the pipe,
 * looping until all bytes are transferred or an error occurs.
 * Returns TRUE on success, FALSE on failure (caller checks
 * GetLastError()).
 * ────────────────────────────────────────────────────────────────── */
static BOOL
write_all(HANDLE hPipe, const void *buf, DWORD total_bytes)
{
    DWORD offset = 0;
    DWORD written = 0;

    while (offset < total_bytes)
    {
        if (!WriteFile(hPipe,
                       (const BYTE *)buf + offset,
                       total_bytes - offset,
                       &written,
                       NULL))
        {
            return FALSE;
        }
        offset += written;
    }
    return TRUE;
}

/* ──────────────────────────────────────────────────────────────────
 * Helper: read exactly `total_bytes` from the pipe into `buf`,
 * looping until all bytes are transferred or an error occurs.
 * Returns TRUE on success, FALSE on failure.
 * ────────────────────────────────────────────────────────────────── */
static BOOL
read_all(HANDLE hPipe, void *buf, DWORD total_bytes)
{
    DWORD offset = 0;
    DWORD read = 0;

    while (offset < total_bytes)
    {
        if (!ReadFile(hPipe,
                      (BYTE *)buf + offset,
                      total_bytes - offset,
                      &read,
                      NULL))
        {
            return FALSE;
        }
        if (read == 0)
        {
            /* Graceful close — peer has closed the connection */
            SetLastError(ERROR_BROKEN_PIPE);
            return FALSE;
        }
        offset += read;
    }
    return TRUE;
}

/* ══════════════════════════════════════════════════════════════════ */
/*  Public API                                                       */
/* ══════════════════════════════════════════════════════════════════ */

BOOL
pipe_frame_send(HANDLE hPipe, const char *json_payload, DWORD payload_len)
{
    DWORD len_le;

    /* ── Validate ──────────────────────────────────────────────── */
    if (payload_len > PIPE_MAX_PAYLOAD_SIZE)
    {
        SetLastError(ERROR_BAD_LENGTH);
        return FALSE;
    }

    /* ── Write 4-byte length prefix (little-endian DWORD) ──────── */
    len_le = payload_len;  /* Windows is little-endian; no conversion needed */
    if (!write_all(hPipe, &len_le, PIPE_HEADER_SIZE))
    {
        return FALSE;
    }

    /* ── Write payload bytes ───────────────────────────────────── */
    if (!write_all(hPipe, json_payload, payload_len))
    {
        return FALSE;
    }

    return TRUE;
}

BOOL
pipe_frame_recv(HANDLE hPipe, char **payload_out, DWORD *payload_len_out)
{
    DWORD payload_len = 0;
    char *buf = NULL;

    /* ── Initialise outputs to failure state ───────────────────── */
    *payload_out = NULL;
    *payload_len_out = 0;

    /* ── Read 4-byte length prefix (little-endian DWORD) ───────── */
    if (!read_all(hPipe, &payload_len, PIPE_HEADER_SIZE))
    {
        LOG_D("pipe_protocol", "frame_recv: error reading header, lastError=%lu", GetLastError());
        return FALSE;
    }

    LOG_D("pipe_protocol", "frame_recv: read length prefix, payloadLen=%lu", payload_len);

    /* ── Validate payload length ───────────────────────────────── */
    if (payload_len > PIPE_MAX_PAYLOAD_SIZE)
    {
        LOG_D("pipe_protocol", "frame_recv: error, payloadLen=%lu exceeds max %lu", payload_len, (unsigned long)PIPE_MAX_PAYLOAD_SIZE);
        SetLastError(ERROR_BAD_LENGTH);
        return FALSE;
    }

    /* ── Allocate buffer (extra byte for null terminator) ──────── */
    if (payload_len > 0)
    {
        buf = (char *)HeapAlloc(GetProcessHeap(), 0, payload_len + 1);
        if (buf == NULL)
        {
            LOG_D("pipe_protocol", "frame_recv: error, out of memory (payloadLen=%lu)", payload_len);
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }

        /* ── Read payload bytes ────────────────────────────────── */
        if (!read_all(hPipe, buf, payload_len))
        {
            LOG_D("pipe_protocol", "frame_recv: error reading payload, lastError=%lu", GetLastError());
            HeapFree(GetProcessHeap(), 0, buf);
            return FALSE;
        }

        LOG_D("pipe_protocol", "frame_recv: read payload, bytesRead=%lu", payload_len);

        /* ── Null-terminate ────────────────────────────────────── */
        buf[payload_len] = '\0';
    }
    else
    {
        /* Zero-length payload — allocate a single byte for the
         * null terminator so the caller can always dereference
         * safely. */
        buf = (char *)HeapAlloc(GetProcessHeap(), 0, 1);
        if (buf == NULL)
        {
            LOG_D("pipe_protocol", "frame_recv: error, out of memory for empty payload");
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        buf[0] = '\0';
    }

    /* ── Set outputs ───────────────────────────────────────────── */
    *payload_out = buf;
    *payload_len_out = payload_len;

    return TRUE;
}
