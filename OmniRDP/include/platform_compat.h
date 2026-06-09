#ifndef PLATFORM_COMPAT_H
#define PLATFORM_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sleep for specified milliseconds */
void platform_sleep_ms(uint32_t ms);

/* Get monotonic timestamp in milliseconds */
uint64_t platform_get_timestamp_ms(void);

/* Get high-resolution monotonic timestamp in microseconds */
uint64_t platform_get_timestamp_us(void);

typedef struct PlatformProcessCpuSample {
  uint64_t wall_time_ms;
  uint64_t kernel_time_100ns;
  uint64_t user_time_100ns;
} PlatformProcessCpuSample;

bool platform_get_process_cpu_sample(PlatformProcessCpuSample *sample);
double platform_process_cpu_percent(const PlatformProcessCpuSample *older,
                                    const PlatformProcessCpuSample *newer);

/* Initialize shutdown signal handling (SIGINT on POSIX, console handler on
 *
 * Windows) */
typedef void (*platform_shutdown_fn)(void);
void platform_signal_init(platform_shutdown_fn handler);

/* Get default certificate file path */
const char *platform_cert_path(void);

/* Get default private key file path */
const char *platform_key_path(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_COMPAT_H */
