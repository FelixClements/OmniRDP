/**
 * @file svc_config.h
 * @brief Service config structure declarations for OmniRDP
 *
 * FreeRDP-independent config layer.  Uses standard C types (int for
 * booleans, uint16_t for ports, uint32_t/unsigned int for counts) so
 * this header can be consumed by both OmniRDP.exe and OmniRDP-svc.exe.
 */

#ifndef SVC_CONFIG_H
#define SVC_CONFIG_H

#include "ini_parser.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Service-level settings (from [service] section) ────────── */

typedef struct {
  char log_level[64];           /* default: "info" */
  char log_dir[512];            /* default: "C:\ProgramData\OmniRDP\logs" */
  unsigned int log_max_size_mb; /* default: 10 */
  unsigned int log_max_files;   /* default: 5 */
  char pipe_name[256];          /* default: "OmniRDP_ServicePipe" */
  unsigned int heartbeat_timeout_sec;     /* default: 10 */
  unsigned int graceful_shutdown_sec;     /* default: 10 */
  unsigned int health_poll_interval_sec;  /* default: 2 */
  unsigned int instance_startup_delay_ms; /* default: 500 */
} SvcServiceConfig;

/* ── Instance-level settings (from [instance:<name>] section) ─ */

typedef struct {
  char name[128]; /* instance name (from section header) */
  int enabled;    /* default: 1 (true) */

  /* Backend connection */
  char backend_hostname[256];  /* REQUIRED */
  uint16_t backend_port;       /* default: 3389 */
  char backend_username[256];  /* REQUIRED */
  char backend_password[1024]; /* REQUIRED (may be dpapi:... or plaintext) */
  char backend_domain[256];    /* default: "" */
  unsigned int backend_connect_timeout_ms; /* default: 30000 */

  /* Reconnect policy */
  int reconnect_enabled;                   /* default: 1 */
  unsigned int reconnect_max_attempts;     /* default: 10 */
  unsigned int reconnect_initial_delay_ms; /* default: 1000 */
  unsigned int reconnect_max_delay_ms;     /* default: 60000 */
  double reconnect_backoff_multiplier;     /* default: 2.0 */

  /* Viewer listener */
  char viewer_bind_address[64];                      /* default: "127.0.0.1" */
  uint16_t viewer_port;                              /* REQUIRED */
  unsigned int viewer_max_viewers;                   /* default: 10 */
  char viewer_cert_path[512];                        /* default: "" */
  char viewer_key_path[512];                         /* default: "" */
  int viewer_slow_disconnect_enabled;                /* default: 1 */
  unsigned int viewer_slow_lag_interval_ms;          /* default: 5000 */
  unsigned int viewer_slow_disconnect_after_ms;      /* default: 30000 */
  unsigned int viewer_late_join_timeout_ms;          /* default: 15000 */
  unsigned int viewer_late_join_refresh_deadline_ms; /* default: 5000 */
  unsigned int viewer_late_join_replay_max_frames;   /* default: 4 */
  unsigned int viewer_throttle_max_updates_per_sec; /* default: 0 (unlimited) */

  /* Display */
  unsigned int display_monitor_count;  /* default: 1 */
  unsigned int display_monitor_width;  /* default: 1920 */
  unsigned int display_monitor_height; /* default: 1080 */
  unsigned int display_color_depth;    /* default: 32 */

  /* Codec */
  int codec_nscodec;                    /* default: 1 */
  int codec_remote_fx;                  /* default: 1 */
  int codec_graphics_pipeline;          /* default: 0 */
  int codec_h264;                       /* default: 0 */
  int codec_avc444;                     /* default: 0 */
  int codec_avc444v2;                   /* default: 0 */
  unsigned int codec_frame_acknowledge; /* default: 4 */

  /* Security */
  int security_tls_enabled;           /* default: 1 */
  int security_nla_enabled;           /* default: 1 */
  char security_tls_min_version[16];  /* default: "1.2" */
  int security_server_authentication; /* default: 1 */
  int security_ignore_certificate;    /* default: 0 */
} InstanceConfig;

/* ── Top-level config ────────────────────────────────────────── */

typedef struct {
  SvcServiceConfig service;
  InstanceConfig *instances;
  unsigned int instance_count;
  char **instance_names; /* array of instance name strings from
                            [instances].names */
  IniFile *ini;          /* internal – owned pointer for cleanup */
} SvcConfig;

/**
 * @brief Load config from an INI file
 * @param filename Path to config.ini
 * @return Populated SvcConfig, or NULL on error. Caller must call
 * svc_config_free().
 */
SvcConfig *svc_config_load(const char *filename);

/**
 * @brief Create a default config with zero instances
 *
 * Used when the config file is missing or unreadable.
 * The service runs idle (no instances) until the user adds one.
 *
 * @return Valid SvcConfig with service defaults and zero instances, or NULL on
 * error
 */
SvcConfig *svc_config_create_default(void);

/**
 * @brief Find an instance config by name
 * @param config Loaded config
 * @param name Instance name
 * @return Pointer to instance config, or NULL if not found
 */
const InstanceConfig *svc_config_find_instance(const SvcConfig *config,
                                               const char *name);

/**
 * @brief Free config resources
 * @param config Config to free (may be NULL)
 */
void svc_config_free(SvcConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* SVC_CONFIG_H */
