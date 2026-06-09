#include "platform_compat.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
extern char **environ;
#endif

static platform_shutdown_fn g_shutdown_handler = NULL;

#ifdef _WIN32

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
  (void)ctrl_type;
  if (g_shutdown_handler)
    g_shutdown_handler();
  return TRUE;
}

void platform_sleep_ms(uint32_t ms) { Sleep((DWORD)ms); }

uint64_t platform_get_timestamp_ms(void) { return GetTickCount64(); }

uint64_t platform_get_timestamp_us(void) {
  static LARGE_INTEGER frequency = {0};
  LARGE_INTEGER counter = {0};

  if (frequency.QuadPart == 0) {
    if (!QueryPerformanceFrequency(&frequency) || (frequency.QuadPart <= 0))
      return platform_get_timestamp_ms() * 1000ULL;
  }

  if (!QueryPerformanceCounter(&counter))
    return platform_get_timestamp_ms() * 1000ULL;

  return (uint64_t)((counter.QuadPart * 1000000ULL) / frequency.QuadPart);
}

static uint64_t platform_filetime_to_100ns(FILETIME time) {
  ULARGE_INTEGER value = {0};
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return value.QuadPart;
}

bool platform_get_process_cpu_sample(PlatformProcessCpuSample *sample) {
  FILETIME creation_time = {0};
  FILETIME exit_time = {0};
  FILETIME kernel_time = {0};
  FILETIME user_time = {0};

  if (!sample)
    return false;

  memset(sample, 0, sizeof(*sample));
  if (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time,
                       &kernel_time, &user_time))
    return false;

  sample->wall_time_ms = platform_get_timestamp_ms();
  sample->kernel_time_100ns = platform_filetime_to_100ns(kernel_time);
  sample->user_time_100ns = platform_filetime_to_100ns(user_time);
  return true;
}

void platform_signal_init(platform_shutdown_fn handler) {
  g_shutdown_handler = handler;
  SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}

const char *platform_cert_path(void) {
  static char env[MAX_PATH];
  DWORD len = GetEnvironmentVariableA("MULTIPLEXER_CERT", env, ARRAYSIZE(env));
  if ((len > 0) && (len < ARRAYSIZE(env)))
    return env;
  return "server.crt";
}

const char *platform_key_path(void) {
  static char env[MAX_PATH];
  DWORD len = GetEnvironmentVariableA("MULTIPLEXER_KEY", env, ARRAYSIZE(env));
  if ((len > 0) && (len < ARRAYSIZE(env)))
    return env;
  return "server.key";
}

#else /* POSIX */

static void posix_signal_handler(int sig) {
  (void)sig;
  if (g_shutdown_handler)
    g_shutdown_handler();
}

void platform_sleep_ms(uint32_t ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

uint64_t platform_get_timestamp_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint64_t platform_get_timestamp_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t platform_timeval_to_100ns(const struct timeval *time) {
  if (!time)
    return 0;

  return ((uint64_t)time->tv_sec * 10000000ULL) +
         ((uint64_t)time->tv_usec * 10ULL);
}

bool platform_get_process_cpu_sample(PlatformProcessCpuSample *sample) {
  struct rusage usage;

  if (!sample)
    return false;

  memset(sample, 0, sizeof(*sample));
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return false;

  sample->wall_time_ms = platform_get_timestamp_ms();
  sample->kernel_time_100ns = platform_timeval_to_100ns(&usage.ru_stime);
  sample->user_time_100ns = platform_timeval_to_100ns(&usage.ru_utime);
  return true;
}

void platform_signal_init(platform_shutdown_fn handler) {
  g_shutdown_handler = handler;
  signal(SIGINT, posix_signal_handler);
  signal(SIGTERM, posix_signal_handler);
}

const char *platform_cert_path(void) {
  const char prefix[] = "MULTIPLEXER_CERT=";
  const size_t prefix_len = sizeof(prefix) - 1;
  if (environ) {
    for (char **entry = environ; *entry; entry++) {
      if (strncmp(*entry, prefix, prefix_len) == 0)
        return *entry + prefix_len;
    }
  }
  return "/tmp/server.crt";
}

const char *platform_key_path(void) {
  const char prefix[] = "MULTIPLEXER_KEY=";
  const size_t prefix_len = sizeof(prefix) - 1;
  if (environ) {
    for (char **entry = environ; *entry; entry++) {
      if (strncmp(*entry, prefix, prefix_len) == 0)
        return *entry + prefix_len;
    }
  }
  return "/tmp/server.key";
}

#endif

double platform_process_cpu_percent(const PlatformProcessCpuSample *older,
                                    const PlatformProcessCpuSample *newer) {
  uint64_t old_cpu = 0;
  uint64_t new_cpu = 0;
  uint64_t cpu_delta_100ns = 0;
  uint64_t wall_delta_100ns = 0;

  if (!older || !newer || (newer->wall_time_ms <= older->wall_time_ms))
    return 0.0;

  old_cpu = older->kernel_time_100ns + older->user_time_100ns;
  new_cpu = newer->kernel_time_100ns + newer->user_time_100ns;
  if (new_cpu < old_cpu)
    return 0.0;

  cpu_delta_100ns = new_cpu - old_cpu;
  wall_delta_100ns = (newer->wall_time_ms - older->wall_time_ms) * 10000ULL;
  if (wall_delta_100ns == 0)
    return 0.0;

  return ((double)cpu_delta_100ns * 100.0) / (double)wall_delta_100ns;
}
