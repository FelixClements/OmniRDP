/**
 * @file svc_dpapi.h
 * @brief DPAPI helper for encrypting/decrypting passwords in config
 *
 * Windows-only. Uses CryptProtectData/CryptUnprotectData with
 * CRYPTPROTECT_LOCAL_MACHINE flag so that all processes running
 * on the machine (under the service account) can decrypt.
 *
 * No FreeRDP dependency.
 */

#ifndef SVC_DPAPI_H
#define SVC_DPAPI_H

#include <stddef.h>
#include <windows.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ── Constants ─────────────────────────────────────────────────── */

/**
 * DPAPI prefix that identifies encrypted values in the config file.
 * Values starting with this prefix are treated as DPAPI-encrypted blobs.
 */
#define DPAPI_PREFIX "dpapi:"

	/* ── Public API ────────────────────────────────────────────────── */

	/**
	 * @brief Check if a password value is DPAPI-encrypted
	 * @param password Password string from config
	 * @return 1 if the password starts with DPAPI_PREFIX, 0 otherwise
	 */
	int svc_dpapi_is_encrypted(const char* password);

	/**
	 * @brief Decrypt a DPAPI-encrypted password
	 *
	 * Expects a string in the format "dpapi:<base64_encoded_blob>".
	 * Decodes the base64, then calls CryptUnprotectData to get the plaintext.
	 *
	 * @param encrypted_password The dpapi:-prefixed string from config
	 * @param plaintext_out Buffer to receive the decrypted password
	 * @param plaintext_out_size Size of plaintext_out buffer
	 * @return 0 on success, -1 on error
	 *
	 * Note: The caller must SecureZeroMemory the plaintext_out buffer when done.
	 */
	int svc_dpapi_decrypt(const char* encrypted_password, char* plaintext_out,
	                      size_t plaintext_out_size);

	/**
	 * @brief Encrypt a plaintext password using DPAPI
	 *
	 * Calls CryptProtectData with CRYPTPROTECT_LOCAL_MACHINE flag,
	 * then base64-encodes the result.
	 *
	 * @param plaintext The plaintext password to encrypt
	 * @param encrypted_out Buffer to receive the "dpapi:<base64>" string
	 * @param encrypted_out_size Size of encrypted_out buffer
	 * @return 0 on success, -1 on error
	 */
	int svc_dpapi_encrypt(const char* plaintext, char* encrypted_out, size_t encrypted_out_size);

	/**
	 * @brief Encrypt a plaintext password in-place in the config file
	 *
	 * Reads the config file, finds the line containing the plaintext password
	 * for the given instance and key, encrypts it, and writes the dpapi: blob
	 * back to the file in-place using a temp-file + rename for atomicity.
	 *
	 * @param config_path Path to config.ini
	 * @param instance_name Instance name (e.g., "office-desktop")
	 * @param key Config key (e.g., "backend.password")
	 * @return 0 on success, -1 on error
	 *
	 * This function is used by the service on first read to replace plaintext
	 * passwords with DPAPI-encrypted blobs.
	 */
	int svc_dpapi_encrypt_in_file(const char* config_path, const char* instance_name,
	                              const char* key);

#ifdef __cplusplus
}
#endif

#endif /* SVC_DPAPI_H */
