#include "svc_config.h"
#include "svc_log.h"
#include "test_utils.h"

#include <stdio.h>

static int write_backend_port_config(const char *path) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf(fp, "[service]\n"
              "log_level = warn\n"
              "\n"
              "[instances]\n"
              "names = Test\n"
              "\n"
              "[instance:Test]\n"
              "backend.hostname = 127.0.0.1\n"
              "backend.port = 70000\n"
              "backend.username = alice\n"
              "backend.password = secret\n"
              "viewer.port = 3390\n");

  fclose(fp);
  return 1;
}

static int write_viewer_port_config(const char *path) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf(fp, "[instances]\n"
              "names = Test\n"
              "\n"
              "[instance:Test]\n"
              "backend.hostname = 127.0.0.1\n"
              "backend.port = 3389\n"
              "backend.username = alice\n"
              "backend.password = secret\n"
              "viewer.port = 70000\n");

  fclose(fp);
  return 1;
}

static int write_classic_latest_config(const char *path, int enabled) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf(fp,
          "[instances]\n"
          "names = Test\n"
          "\n"
          "[instance:Test]\n"
          "backend.hostname = 127.0.0.1\n"
          "backend.port = 3389\n"
          "backend.username = alice\n"
          "backend.password = secret\n"
          "viewer.port = 3390\n"
          "viewer.classic_latest_state_enabled = %s\n"
          "viewer.classic_latest_state_max_queue_depth = 7\n"
          "viewer.classic_latest_state_max_queue_bytes = 4096\n",
          enabled ? "true" : "false");

  fclose(fp);
  return 1;
}

int main(void) {
  const char *path = "test_svc_config.ini";
  const char *bad_viewer_path = "test_svc_config_bad_viewer.ini";
  const char *classic_latest_path = "test_svc_config_classic_latest.ini";
  SvcLogLevel level = SVC_LOG_INFO;
  SvcConfig *config = NULL;
  const InstanceConfig *inst = NULL;
  int ok = 1;

  test_suppress_crt_dialogs();

  if (!write_backend_port_config(path))
    return 1;

  config = svc_config_load(path);
  if (!config) {
    remove(path);
    return 1;
  }

  if (svc_log_level_from_string(config->service.log_level, &level) != 0 ||
      level != SVC_LOG_WARN)
    ok = 0;

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->backend_port != 3389 || inst->viewer_port != 3390)
    ok = 0;
  if (!inst || inst->viewer_classic_latest_state_enabled != 0 ||
      inst->viewer_classic_latest_state_max_queue_depth != 0 ||
      inst->viewer_classic_latest_state_max_queue_bytes != 0)
    ok = 0;

  svc_config_free(config);

  if (!write_classic_latest_config(classic_latest_path, 1)) {
    remove(path);
    return 1;
  }

  config = svc_config_load(classic_latest_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->viewer_classic_latest_state_enabled != 1 ||
      inst->viewer_classic_latest_state_max_queue_depth != 7 ||
      inst->viewer_classic_latest_state_max_queue_bytes != 4096)
    ok = 0;

  svc_config_free(config);

  if (!write_viewer_port_config(bad_viewer_path)) {
    remove(path);
    return 1;
  }

  config = svc_config_load(bad_viewer_path);
  if (config) {
    svc_config_free(config);
    ok = 0;
  }

  remove(path);
  remove(bad_viewer_path);
  remove(classic_latest_path);
  return ok ? 0 : 1;
}
