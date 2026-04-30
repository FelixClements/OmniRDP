#ifndef PLATFORM_COMPAT_H
#define PLATFORM_COMPAT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sleep for specified milliseconds */
void platform_sleep_ms(uint32_t ms);

/* Get monotonic timestamp in milliseconds */
uint64_t platform_get_timestamp_ms(void);

/* Initialize shutdown signal handling (SIGINT on POSIX, console handler on Windows) */
typedef void (*platform_shutdown_fn)(void);
void platform_signal_init(platform_shutdown_fn handler);

/* Get default certificate file path */
const char* platform_cert_path(void);

/* Get default private key file path */
const char* platform_key_path(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_COMPAT_H */
