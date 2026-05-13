/**
 * @file svc_dpapi.c
 * @brief DPAPI helper implementation for OmniRDP
 *
 * Provides base64 encoding/decoding, DPAPI encryption (CryptProtectData),
 * DPAPI decryption (CryptUnprotectData), and in-place config file
 * encryption.  Windows-only; no FreeRDP dependency.
 *
 * Links against: crypt32.lib
 */

#include "svc_dpapi.h"

#include <ctype.h>
#include <dpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Internal: Base64 helpers
 * ═══════════════════════════════════════════════════════════════════ */

/** Standard base64 alphabet */
static const char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                   "abcdefghijklmnopqrstuvwxyz"
                                   "0123456789+/";

static size_t dpapi_prefix_len(void) { return strnlen_s(DPAPI_PREFIX, 64); }

/**
 * @brief Encode a byte buffer to base64 (null-terminated).
 *
 * @param in       Input bytes
 * @param in_len   Number of input bytes
 * @param out      Output buffer (receives null-terminated base64 string)
 * @param out_size Size of output buffer (must be >= 4*((in_len+2)/3) + 1)
 * @return Number of characters written (excluding null terminator),
 *         or -1 on error (buffer too small).
 */
static int b64_encode(const unsigned char *in, size_t in_len, char *out,
                      size_t out_size) {
  size_t out_len = 4 * ((in_len + 2) / 3);
  if (out_len == 0) {
    if (out_size < 1)
      return -1;
    out[0] = '\0';
    return 0;
  }
  if (out_size < out_len + 1)
    return -1;

  size_t i, j;
  for (i = 0, j = 0; i < in_len;) {
    unsigned int octet_a = (unsigned int)in[i++];
    unsigned int octet_b = (i < in_len) ? (unsigned int)in[i++] : 0;
    unsigned int octet_c = (i < in_len) ? (unsigned int)in[i++] : 0;

    unsigned int triple = (octet_a << 16) | (octet_b << 8) | octet_c;

    out[j++] = b64_alphabet[(triple >> 18) & 0x3F];
    out[j++] = b64_alphabet[(triple >> 12) & 0x3F];
    out[j++] = b64_alphabet[(triple >> 6) & 0x3F];
    out[j++] = b64_alphabet[triple & 0x3F];
  }

  /* Replace padding characters */
  {
    int padding = (3 - (int)(in_len % 3)) % 3;
    for (int p = 1; p <= padding; p++)
      out[out_len - p] = '=';
  }

  out[out_len] = '\0';
  return (int)out_len;
}

/**
 * @brief Get the base64 value of a single character.
 * @param c Base64 character
 * @return Numeric value (0-63), or -1 if invalid.
 */
static int b64_char_val(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

/**
 * @brief Decode a base64 string into raw bytes.
 *
 * @param in       Base64 input string (may include trailing padding '=')
 * @param in_len   Length of base64 string (must be multiple of 4)
 * @param out      Output buffer for decoded bytes
 * @param out_size Size of output buffer
 * @return Number of decoded bytes, or -1 on error (invalid input or
 *         buffer too small).
 */
static int b64_decode(const char *in, size_t in_len, unsigned char *out,
                      size_t out_size) {
  if (in_len % 4 != 0 || in_len == 0)
    return -1;

  /* Count padding characters */
  size_t padding = 0;
  if (in_len >= 1 && in[in_len - 1] == '=')
    padding++;
  if (in_len >= 2 && in[in_len - 2] == '=')
    padding++;

  size_t decoded_len = 3 * (in_len / 4) - padding;
  if (out_size < decoded_len)
    return -1;

  size_t out_idx = 0;
  for (size_t i = 0; i < in_len; i += 4) {
    int a = b64_char_val(in[i]);
    int b = b64_char_val(in[i + 1]);

    /* The first two chars are always required and must be valid */
    if (a < 0 || b < 0)
      return -1;

    int c = (in[i + 2] != '=') ? b64_char_val(in[i + 2]) : 0;
    int d = (in[i + 3] != '=') ? b64_char_val(in[i + 3]) : 0;

    if ((in[i + 2] != '=' && c < 0) || (in[i + 3] != '=' && d < 0))
      return -1;

    unsigned int triple = ((unsigned int)a << 18) | ((unsigned int)b << 12) |
                          ((unsigned int)c << 6) | (unsigned int)d;

    if (out_idx < decoded_len)
      out[out_idx++] = (unsigned char)((triple >> 16) & 0xFF);
    if (out_idx < decoded_len)
      out[out_idx++] = (unsigned char)((triple >> 8) & 0xFF);
    if (out_idx < decoded_len)
      out[out_idx++] = (unsigned char)(triple & 0xFF);
  }

  return (int)decoded_len;
}

/* ═══════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════ */

int svc_dpapi_is_encrypted(const char *password) {
  if (!password)
    return 0;
  return (strncmp(password, DPAPI_PREFIX, dpapi_prefix_len()) == 0) ? 1 : 0;
}

int svc_dpapi_decrypt(const char *encrypted_password, char *plaintext_out,
                      size_t plaintext_out_size) {
  if (!encrypted_password || !plaintext_out || plaintext_out_size == 0)
    return -1;

  /* 1. Verify the dpapi: prefix */
  size_t prefix_len = dpapi_prefix_len();
  if (strncmp(encrypted_password, DPAPI_PREFIX, prefix_len) != 0)
    return -1;

  /* 2. Skip past the prefix to get the base64 string */
  const char *b64_str = encrypted_password + prefix_len;
  size_t b64_len = strnlen_s(b64_str, 16384);
  if (b64_len == 0)
    return -1;
  if (b64_len >= 16384)
    return -1;

  /* 3. Base64-decode to get the encrypted blob */
  size_t max_decoded = 3 * (b64_len / 4); /* upper bound */
  unsigned char *decoded = (unsigned char *)malloc(max_decoded);
  if (!decoded)
    return -1;

  int decoded_len = b64_decode(b64_str, b64_len, decoded, max_decoded);
  if (decoded_len < 0) {
    free(decoded);
    return -1;
  }

  DATA_BLOB encrypted_blob;
  encrypted_blob.pbData = decoded;
  encrypted_blob.cbData = (DWORD)decoded_len;

  /* 4. Call CryptUnprotectData */
  DATA_BLOB decrypted_blob;
  SecureZeroMemory(&decrypted_blob, sizeof(decrypted_blob));

  BOOL ok =
      CryptUnprotectData(&encrypted_blob, NULL, /* ppszDataDescr – not needed */
                         NULL,                  /* pOptionalEntropy */
                         NULL,                  /* pvReserved */
                         NULL,                  /* pPromptStruct */
                         CRYPTPROTECT_LOCAL_MACHINE, &decrypted_blob);

  /* Free the base64-decoded buffer regardless of outcome */
  free(decoded);
  encrypted_blob.pbData = NULL;

  if (!ok)
    return -1;

  /* 5. Copy decrypted data to plaintext_out (null-terminated) */
  size_t copy_len = (size_t)decrypted_blob.cbData;
  if (copy_len >= plaintext_out_size)
    copy_len = plaintext_out_size - 1;

  if (memcpy_s(plaintext_out, plaintext_out_size, decrypted_blob.pbData,
               copy_len) != 0) {
    LocalFree(decrypted_blob.pbData);
    return -1;
  }
  plaintext_out[copy_len] = '\0';

  /* 6. Free DPAPI-allocated memory */
  LocalFree(decrypted_blob.pbData);

  return 0;
}

int svc_dpapi_encrypt(const char *plaintext, char *encrypted_out,
                      size_t encrypted_out_size) {
  if (!plaintext || !encrypted_out || encrypted_out_size == 0)
    return -1;

  size_t plaintext_len = strnlen_s(plaintext, 4096);
  if (plaintext_len >= 4096)
    return -1;

  /* 1. Create a DATA_BLOB from the plaintext password */
  DATA_BLOB plaintext_blob;
  plaintext_blob.pbData = (BYTE *)plaintext;
  plaintext_blob.cbData = (DWORD)plaintext_len;

  /* 2. Call CryptProtectData */
  DATA_BLOB encrypted_blob;
  SecureZeroMemory(&encrypted_blob, sizeof(encrypted_blob));

  BOOL ok = CryptProtectData(&plaintext_blob, NULL, /* szDataDescr */
                             NULL,                  /* pOptionalEntropy */
                             NULL,                  /* pvReserved */
                             NULL,                  /* pPromptStruct */
                             CRYPTPROTECT_LOCAL_MACHINE, &encrypted_blob);

  if (!ok)
    return -1;

  /* plaintext_blob.pbData points to the input buffer; do NOT free it.
   * Only encrypted_blob.pbData was allocated (by CryptProtectData via
   * LocalAlloc) and must be freed. */

  /* 3. Base64-encode the encrypted blob */
  size_t b64_buf_size = 4 * (((size_t)encrypted_blob.cbData + 2) / 3) + 1;
  char *b64_buf = (char *)malloc(b64_buf_size);
  if (!b64_buf) {
    LocalFree(encrypted_blob.pbData);
    return -1;
  }

  int b64_len = b64_encode(encrypted_blob.pbData, (size_t)encrypted_blob.cbData,
                           b64_buf, b64_buf_size);

  /* Free the DPAPI-allocated encrypted blob */
  LocalFree(encrypted_blob.pbData);

  if (b64_len < 0) {
    free(b64_buf);
    return -1;
  }

  /* 4. Format as "dpapi:<base64>" into encrypted_out */
  size_t prefix_len = dpapi_prefix_len();
  if (encrypted_out_size < prefix_len + (size_t)b64_len + 1) {
    free(b64_buf);
    return -1;
  }

  if (memcpy_s(encrypted_out, encrypted_out_size, DPAPI_PREFIX, prefix_len) !=
          0 ||
      memcpy_s(encrypted_out + prefix_len, encrypted_out_size - prefix_len,
               b64_buf, (size_t)b64_len) != 0) {
    free(b64_buf);
    return -1;
  }
  encrypted_out[prefix_len + (size_t)b64_len] = '\0';

  free(b64_buf);
  return 0;
}

int svc_dpapi_encrypt_in_file(const char *config_path,
                              const char *instance_name, const char *key) {
  if (!config_path || !instance_name || !key)
    return -1;

  /* Build the section name: "instance:<name>" */
  char section[256];
  int ret = _snprintf(section, sizeof(section), "instance:%s", instance_name);
  if (ret < 0 || (size_t)ret >= sizeof(section))
    return -1;

  /* ── Open original file for reading ───────────────────────── */
  FILE *orig = NULL;
  fopen_s(&orig, config_path, "r");
  if (!orig)
    return -1;

  /* ── Build temp file path ─────────────────────────────────── */
  char tmp_path[MAX_PATH];
  ret = _snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", config_path);
  if (ret < 0 || (size_t)ret >= sizeof(tmp_path)) {
    fclose(orig);
    return -1;
  }

  FILE *tmp = NULL;
  fopen_s(&tmp, tmp_path, "w");
  if (!tmp) {
    fclose(orig);
    return -1;
  }

  /* ── Line-by-line processing ──────────────────────────────── */
  char line[8192];
  char current_section[256] = "";
  int in_target_section = 0;
  int target_found = 0;
  int target_modified = 0;
  int encryption_failed = 0;

  while (fgets(line, sizeof(line), orig)) {
    /* ── Detect section header ──────────────────────────── */
    /* Trim leading whitespace to check for '[' */
    const char *p = line;
    while (*p && isspace((unsigned char)*p))
      p++;

    if (*p == '[') {
      /* Find the closing ']' */
      const char *end_bracket = strchr(p, ']');
      if (end_bracket) {
        /* Extract section name */
        char sec[256];
        size_t sec_len = (size_t)(end_bracket - p - 1);
        if (sec_len >= sizeof(sec))
          sec_len = sizeof(sec) - 1;
        if (memcpy_s(sec, sizeof(sec), p + 1, sec_len) != 0) {
          fclose(orig);
          fclose(tmp);
          remove(tmp_path);
          return -1;
        }
        sec[sec_len] = '\0';

        /* Trim whitespace from section name */
        char *sp = sec;
        while (*sp && isspace((unsigned char)*sp))
          sp++;
        size_t sp_len = strnlen_s(sp, sizeof(sec));
        if (sp_len > 0) {
          char *se = sp + sp_len - 1;
          while (se > sp && isspace((unsigned char)*se))
            *se-- = '\0';
        }

        if (_snprintf(current_section, sizeof(current_section), "%s", sp) <
            0) {
          fclose(orig);
          fclose(tmp);
          remove(tmp_path);
          return -1;
        }
        current_section[sizeof(current_section) - 1] = '\0';
        in_target_section = (strcmp(current_section, section) == 0);
      } else {
        in_target_section = 0;
      }

      fputs(line, tmp);
      continue;
    }

    /* ── Inside target section: look for key = value ────── */
    if (in_target_section) {
      /* Find '=' in the line */
      char *eq = strchr(line, '=');
      if (eq) {
        /* Extract the key part (before '=') */
        size_t key_len = (size_t)(eq - line);
        if (key_len > 0) {
          /* Trim trailing whitespace from key */
          char *key_start = line;
          while (*key_start && isspace((unsigned char)*key_start))
            key_start++;
          char *key_end = line + key_len - 1;
          while (key_end >= key_start && isspace((unsigned char)*key_end))
            key_end--;

          size_t trimmed_key_len = (size_t)(key_end - key_start + 1);
          if (trimmed_key_len > 0) {
            /* Temporarily null-terminate for comparison */
            char saved = key_end[1];
            key_end[1] = '\0';

            int keys_match = (strcmp(key_start, key) == 0);

            key_end[1] = saved; /* restore */

            if (keys_match) {
              target_found = 1;

              /* Extract value after '=' */
              const char *val_start = eq + 1;
              while (*val_start && isspace((unsigned char)*val_start))
                val_start++;

              /* Copy the value and trim trailing ws/newlines */
              char value[8192];
              if (_snprintf(value, sizeof(value), "%s", val_start) < 0) {
                fclose(orig);
                fclose(tmp);
                remove(tmp_path);
                return -1;
              }
              value[sizeof(value) - 1] = '\0';

              size_t vlen = strnlen_s(value, sizeof(value));
              while (vlen > 0 && isspace((unsigned char)value[vlen - 1]))
                value[--vlen] = '\0';

              /* Check if already encrypted */
              if (strncmp(value, DPAPI_PREFIX, dpapi_prefix_len()) != 0) {
                /* Encrypt the value */
                char encrypted[4096];
                if (svc_dpapi_encrypt(value, encrypted, sizeof(encrypted)) ==
                    0) {
                  /* Write modified line */
                  fprintf(tmp, "%s = %s\n", key, encrypted);
                  target_modified = 1;
                } else {
                  /* Encryption failed – write original */
                  fputs(line, tmp);
                  encryption_failed = 1;
                }
              } else {
                /* Already encrypted – write original */
                fputs(line, tmp);
              }

              continue;
            }
          }
        }
      }
    }

    /* ── Default: write the original line ────────────────── */
    fputs(line, tmp);
  }

  /* ── Cleanup ──────────────────────────────────────────────── */
  fclose(orig);
  fclose(tmp);

  if (target_modified) {
    /* Atomically replace original with temp file */
    if (!MoveFileExA(tmp_path, config_path, MOVEFILE_REPLACE_EXISTING)) {
      /* Attempt to clean up the temp file */
      remove(tmp_path);
      return -1;
    }
    return 0;
  }

  /* No modification was made: clean up temp file */
  remove(tmp_path);

  if (encryption_failed)
    return -1; /* encryption was attempted and failed */

  if (!target_found)
    return -1; /* key was not found in the specified section */

  return 0; /* key found but already encrypted */
}
