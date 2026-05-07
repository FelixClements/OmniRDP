/**
 * @file svc_config.c
 * @brief Config loading implementation for OmniRDP
 *
 * Parses an INI file (via ini_parser.h) into strongly-typed config
 * structs (SvcServiceConfig / InstanceConfig / SvcConfig).
 */

#include "svc_config.h"
#include "ini_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Default value helpers ───────────────────────────────────── */

static void svc_config_default_service(SvcServiceConfig *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  strncpy(cfg->log_level, "info", sizeof(cfg->log_level) - 1);
  cfg->log_level[sizeof(cfg->log_level) - 1] = '\0';
  strncpy(cfg->log_dir, "C:\\ProgramData\\OmniRDP\\logs",
          sizeof(cfg->log_dir) - 1);
  cfg->log_dir[sizeof(cfg->log_dir) - 1] = '\0';
  cfg->log_max_size_mb = 10;
  cfg->log_max_files = 5;
  strncpy(cfg->pipe_name, "OmniRDP_ServicePipe", sizeof(cfg->pipe_name) - 1);
  cfg->pipe_name[sizeof(cfg->pipe_name) - 1] = '\0';
  cfg->heartbeat_timeout_sec = 10;
  cfg->graceful_shutdown_sec = 10;
  cfg->health_poll_interval_sec = 2;
  cfg->instance_startup_delay_ms = 500;
}

static void svc_config_default_instance(InstanceConfig *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->enabled = 1;
  cfg->backend_port = 3389;
  cfg->backend_connect_timeout_ms = 30000;
  cfg->reconnect_enabled = 1;
  cfg->reconnect_max_attempts = 10;
  cfg->reconnect_initial_delay_ms = 1000;
  cfg->reconnect_max_delay_ms = 60000;
  cfg->reconnect_backoff_multiplier = 2.0;
  strncpy(cfg->viewer_bind_address, "127.0.0.1",
          sizeof(cfg->viewer_bind_address) - 1);
  cfg->viewer_bind_address[sizeof(cfg->viewer_bind_address) - 1] = '\0';
  cfg->viewer_max_viewers = 10;
  cfg->viewer_slow_disconnect_enabled = 1;
  cfg->viewer_slow_lag_interval_ms = 5000;
  cfg->viewer_slow_disconnect_after_ms = 30000;
  cfg->viewer_late_join_timeout_ms = 15000;
  cfg->viewer_late_join_refresh_deadline_ms = 5000;
  cfg->viewer_late_join_replay_max_frames = 4;
  cfg->viewer_throttle_max_updates_per_sec = 0;
  cfg->display_monitor_count = 1;
  cfg->display_monitor_width = 1920;
  cfg->display_monitor_height = 1080;
  cfg->display_color_depth = 32;
  cfg->codec_nscodec = 1;
  cfg->codec_remote_fx = 1;
  cfg->codec_graphics_pipeline = 0;
  cfg->codec_h264 = 0;
  cfg->codec_avc444 = 0;
  cfg->codec_avc444v2 = 0;
  cfg->codec_frame_acknowledge = 4;
  cfg->security_tls_enabled = 1;
  cfg->security_nla_enabled = 1;
  strncpy(cfg->security_tls_min_version, "1.2",
          sizeof(cfg->security_tls_min_version) - 1);
  cfg->security_tls_min_version[sizeof(cfg->security_tls_min_version) - 1] =
      '\0';
  cfg->security_server_authentication = 1;
  cfg->security_ignore_certificate = 0;
}

/* ── Helper: safe string copy with null termination ──────────── */

static int strcpy_safe(char *dest, size_t dest_size, const char *src) {
  if (!dest || dest_size == 0 || !src)
    return 0;
  strncpy(dest, src, dest_size - 1);
  dest[dest_size - 1] = '\0';
  if (strlen(src) >= dest_size) {
    fprintf(stderr,
            "Warning: config value truncated: '%s' -> '%s' (max %zu chars)\n",
            src, dest, dest_size - 1);
    return -1;
  }
  return 0;
}

/* ── Helper: parse a single instance from an [instance:<name>] section ─ */

static int parse_one_instance(const IniFile *ini, const char *name,
                              InstanceConfig *inst) {
  char section[256];
  int ret = snprintf(section, sizeof(section), "instance:%s", name);
  if (ret < 0 || (size_t)ret >= sizeof(section))
    return -1;

  svc_config_default_instance(inst);
  strcpy_safe(inst->name, sizeof(inst->name), name);

  inst->enabled = ini_get_bool(ini, section, "enabled", inst->enabled);

  /* Backend connection */
  strcpy_safe(inst->backend_hostname, sizeof(inst->backend_hostname),
              ini_get(ini, section, "backend.hostname", ""));
  inst->backend_port =
      (uint16_t)ini_get_uint(ini, section, "backend.port", inst->backend_port);
  strcpy_safe(inst->backend_username, sizeof(inst->backend_username),
              ini_get(ini, section, "backend.username", ""));
  strcpy_safe(inst->backend_password, sizeof(inst->backend_password),
              ini_get(ini, section, "backend.password", ""));
  strcpy_safe(inst->backend_domain, sizeof(inst->backend_domain),
              ini_get(ini, section, "backend.domain", ""));
  inst->backend_connect_timeout_ms =
      ini_get_uint(ini, section, "backend.connect_timeout_ms",
                   inst->backend_connect_timeout_ms);

  /* Reconnect policy */
  inst->reconnect_enabled =
      ini_get_bool(ini, section, "reconnect.enabled", inst->reconnect_enabled);
  inst->reconnect_max_attempts = ini_get_uint(
      ini, section, "reconnect.max_attempts", inst->reconnect_max_attempts);
  inst->reconnect_initial_delay_ms =
      ini_get_uint(ini, section, "reconnect.initial_delay_ms",
                   inst->reconnect_initial_delay_ms);
  inst->reconnect_max_delay_ms = ini_get_uint(
      ini, section, "reconnect.max_delay_ms", inst->reconnect_max_delay_ms);
  {
    const char *bm =
        ini_get(ini, section, "reconnect.backoff_multiplier", NULL);
    if (bm)
      inst->reconnect_backoff_multiplier = strtod(bm, NULL);
  }

  /* Viewer listener */
  strcpy_safe(
      inst->viewer_bind_address, sizeof(inst->viewer_bind_address),
      ini_get(ini, section, "viewer.bind_address", inst->viewer_bind_address));
  inst->viewer_port =
      (uint16_t)ini_get_uint(ini, section, "viewer.port", inst->viewer_port);
  inst->viewer_max_viewers = ini_get_uint(ini, section, "viewer.max_viewers",
                                          inst->viewer_max_viewers);
  strcpy_safe(inst->viewer_cert_path, sizeof(inst->viewer_cert_path),
              ini_get(ini, section, "viewer.cert_path", ""));
  strcpy_safe(inst->viewer_key_path, sizeof(inst->viewer_key_path),
              ini_get(ini, section, "viewer.key_path", ""));
  inst->viewer_slow_disconnect_enabled =
      ini_get_bool(ini, section, "viewer.slow_disconnect_enabled",
                   inst->viewer_slow_disconnect_enabled);
  inst->viewer_slow_lag_interval_ms =
      ini_get_uint(ini, section, "viewer.slow_lag_interval_ms",
                   inst->viewer_slow_lag_interval_ms);
  inst->viewer_slow_disconnect_after_ms =
      ini_get_uint(ini, section, "viewer.slow_disconnect_after_ms",
                   inst->viewer_slow_disconnect_after_ms);
  inst->viewer_late_join_timeout_ms =
      ini_get_uint(ini, section, "viewer.late_join_timeout_ms",
                   inst->viewer_late_join_timeout_ms);
  inst->viewer_late_join_refresh_deadline_ms =
      ini_get_uint(ini, section, "viewer.late_join_refresh_deadline_ms",
                   inst->viewer_late_join_refresh_deadline_ms);
  inst->viewer_late_join_replay_max_frames =
      ini_get_uint(ini, section, "viewer.late_join_replay_max_frames",
                   inst->viewer_late_join_replay_max_frames);
  inst->viewer_throttle_max_updates_per_sec =
      ini_get_uint(ini, section, "viewer.throttle_max_updates_per_sec",
                   inst->viewer_throttle_max_updates_per_sec);

  /* Display */
  inst->display_monitor_count = ini_get_uint(
      ini, section, "display.monitor_count", inst->display_monitor_count);
  inst->display_monitor_width = ini_get_uint(
      ini, section, "display.monitor_width", inst->display_monitor_width);
  inst->display_monitor_height = ini_get_uint(
      ini, section, "display.monitor_height", inst->display_monitor_height);
  inst->display_color_depth = ini_get_uint(ini, section, "display.color_depth",
                                           inst->display_color_depth);

  /* Codec */
  inst->codec_nscodec =
      ini_get_bool(ini, section, "codec.nscodec", inst->codec_nscodec);
  inst->codec_remote_fx =
      ini_get_bool(ini, section, "codec.remote_fx", inst->codec_remote_fx);
  inst->codec_graphics_pipeline = ini_get_bool(
      ini, section, "codec.graphics_pipeline", inst->codec_graphics_pipeline);
  inst->codec_h264 = ini_get_bool(ini, section, "codec.h264", inst->codec_h264);
  inst->codec_avc444 =
      ini_get_bool(ini, section, "codec.avc444", inst->codec_avc444);
  inst->codec_avc444v2 =
      ini_get_bool(ini, section, "codec.avc444v2", inst->codec_avc444v2);
  inst->codec_frame_acknowledge = ini_get_uint(
      ini, section, "codec.frame_acknowledge", inst->codec_frame_acknowledge);

  /* Security */
  inst->security_tls_enabled = ini_get_bool(
      ini, section, "security.tls_enabled", inst->security_tls_enabled);
  inst->security_nla_enabled = ini_get_bool(
      ini, section, "security.nla_enabled", inst->security_nla_enabled);
  strcpy_safe(inst->security_tls_min_version,
              sizeof(inst->security_tls_min_version),
              ini_get(ini, section, "security.tls_min_version",
                      inst->security_tls_min_version));
  inst->security_server_authentication =
      ini_get_bool(ini, section, "security.server_authentication",
                   inst->security_server_authentication);
  inst->security_ignore_certificate =
      ini_get_bool(ini, section, "security.ignore_certificate",
                   inst->security_ignore_certificate);

  /* Validate required fields */
  if (inst->backend_hostname[0] == '\0') {
    fprintf(stderr,
            "Instance '%s': missing required field 'backend.hostname'\n", name);
    return -1;
  }
  if (inst->backend_username[0] == '\0') {
    fprintf(stderr,
            "Instance '%s': missing required field 'backend.username'\n", name);
    return -1;
  }
  if (inst->backend_port == 0) {
    fprintf(stderr, "Instance '%s': missing required field 'backend.port'\n",
            name);
    return -1;
  }
  if (inst->viewer_port == 0) {
    fprintf(stderr, "Instance '%s': missing required field 'viewer.port'\n",
            name);
    return -1;
  }

  return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

SvcConfig *svc_config_create_default(void) {
  SvcConfig *config = (SvcConfig *)calloc(1, sizeof(SvcConfig));
  if (!config)
    return NULL;
  svc_config_default_service(&config->service);
  config->instance_count = 0;
  config->instances = NULL;
  config->instance_names = NULL;
  config->ini = NULL;
  return config;
}

SvcConfig *svc_config_load(const char *filename) {
  if (!filename)
    return NULL;

  IniFile *ini = ini_parse(filename);
  if (!ini)
    return NULL;

  SvcConfig *config = (SvcConfig *)calloc(1, sizeof(SvcConfig));
  if (!config) {
    ini_free(ini);
    return NULL;
  }
  config->ini = ini;

  /* ── Parse [service] section ───────────────────────────── */
  {
    SvcServiceConfig *svc = &config->service;
    svc_config_default_service(svc);

    strcpy_safe(svc->log_level, sizeof(svc->log_level),
                ini_get(ini, "service", "log_level", svc->log_level));
    strcpy_safe(svc->log_dir, sizeof(svc->log_dir),
                ini_get(ini, "service", "log_dir", svc->log_dir));
    svc->log_max_size_mb =
        ini_get_uint(ini, "service", "log_max_size_mb", svc->log_max_size_mb);
    svc->log_max_files =
        ini_get_uint(ini, "service", "log_max_files", svc->log_max_files);
    strcpy_safe(svc->pipe_name, sizeof(svc->pipe_name),
                ini_get(ini, "service", "pipe_name", svc->pipe_name));
    svc->heartbeat_timeout_sec = ini_get_uint(
        ini, "service", "heartbeat_timeout_sec", svc->heartbeat_timeout_sec);
    svc->graceful_shutdown_sec = ini_get_uint(
        ini, "service", "graceful_shutdown_sec", svc->graceful_shutdown_sec);
    svc->health_poll_interval_sec =
        ini_get_uint(ini, "service", "health_poll_interval_sec",
                     svc->health_poll_interval_sec);
    svc->instance_startup_delay_ms =
        ini_get_uint(ini, "service", "instance_startup_delay_ms",
                     svc->instance_startup_delay_ms);
  }

  /* ── Parse [instances] section ─────────────────────────── */
  const char *names_str = ini_get(ini, "instances", "names", NULL);
  if (names_str && names_str[0] != '\0') {
    /* Duplicate so we can tokenize */
    char *names_copy = _strdup(names_str);
    if (!names_copy) {
      svc_config_free(config);
      return NULL;
    }

    /* Count comma-separated tokens */
    unsigned int count = 1;
    for (const char *p = names_copy; *p; p++) {
      if (*p == ',')
        count++;
    }

    config->instance_count = count;
    config->instance_names = (char **)calloc(count, sizeof(char *));
    if (!config->instance_names) {
      free(names_copy);
      svc_config_free(config);
      return NULL;
    }
    config->instances = (InstanceConfig *)calloc(count, sizeof(InstanceConfig));
    if (!config->instances) {
      free(names_copy);
      svc_config_free(config);
      return NULL;
    }

    /* Tokenize on commas */
    char *ctx;
    char *token = strtok_s(names_copy, ",", &ctx);
    unsigned int idx = 0;
    while (token && idx < count) {
      /* Trim leading whitespace */
      while (*token && isspace((unsigned char)*token))
        token++;
      /* Trim trailing whitespace in-place */
      char *end = token + strlen(token) - 1;
      while (end > token && isspace((unsigned char)*end))
        *end-- = '\0';

      if (token[0] != '\0') {
        config->instance_names[idx] = _strdup(token);
        if (!config->instance_names[idx]) {
          free(names_copy);
          svc_config_free(config);
          return NULL;
        }

        if (parse_one_instance(ini, token, &config->instances[idx]) != 0) {
          free(names_copy);
          svc_config_free(config);
          return NULL;
        }
        idx++;
      }
      token = strtok_s(NULL, ",", &ctx);
    }
    config->instance_count = idx;

    free(names_copy);
  }

  return config;
}

const InstanceConfig *svc_config_find_instance(const SvcConfig *config,
                                               const char *name) {
  if (!config || !name)
    return NULL;
  for (unsigned int i = 0; i < config->instance_count; i++) {
    if (strcmp(config->instances[i].name, name) == 0)
      return &config->instances[i];
  }
  return NULL;
}

void svc_config_free(SvcConfig *config) {
  if (!config)
    return;

  /* Free instance names */
  if (config->instance_names) {
    for (unsigned int i = 0; i < config->instance_count; i++)
      free(config->instance_names[i]);
    free(config->instance_names);
  }

  /* Free instance configs */
  free(config->instances);

  /* Free the parsed INI file */
  ini_free(config->ini);

  /* Free the config itself */
  free(config);
}
