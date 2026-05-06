#include "platform_compat.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <time.h>
#include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>

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

void platform_signal_init(platform_shutdown_fn handler) {
  g_shutdown_handler = handler;
  SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
}

const char *platform_cert_path(void) {
  const char *env = getenv("MULTIPLEXER_CERT");
  if (env)
    return env;
  return "server.crt";
}

const char *platform_key_path(void) {
  const char *env = getenv("MULTIPLEXER_KEY");
  if (env)
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

void platform_signal_init(platform_shutdown_fn handler) {
  g_shutdown_handler = handler;
  signal(SIGINT, posix_signal_handler);
  signal(SIGTERM, posix_signal_handler);
}

const char *platform_cert_path(void) {
  const char *env = getenv("MULTIPLEXER_CERT");
  if (env)
    return env;
  return "/tmp/server.crt";
}

const char *platform_key_path(void) {
  const char *env = getenv("MULTIPLEXER_KEY");
  if (env)
    return env;
  return "/tmp/server.key";
}

#endif
