#include "svc_config.h"
#include "svc_log.h"
#include "test_utils.h"

#include <stdio.h>
#include <string.h>

static int write_backend_port_config(const char *path) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp, "[service]\n"
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

  fprintf_s(fp, "[instances]\n"
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

  fprintf_s(fp,
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

static int write_viewer_gfx_config(const char *path, int enabled) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp,
            "[instances]\n"
            "names = Test\n"
            "\n"
            "[instance:Test]\n"
            "backend.hostname = 127.0.0.1\n"
            "backend.port = 3389\n"
            "backend.username = alice\n"
            "backend.password = secret\n"
            "viewer.port = 3390\n"
            "viewer.gfx.enabled = %s\n",
            enabled ? "true" : "false");

  fclose(fp);
  return 1;
}

static int write_viewer_gfx_full_frame_dirty_config(const char *path,
                                                    int enabled) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp,
            "[instances]\n"
            "names = Test\n"
            "\n"
            "[instance:Test]\n"
            "backend.hostname = 127.0.0.1\n"
            "backend.port = 3389\n"
            "backend.username = alice\n"
            "backend.password = secret\n"
            "viewer.port = 3390\n"
            "viewer.gfx.diagnostic.full_frame_dirty = %s\n",
            enabled ? "true" : "false");

  fclose(fp);
  return 1;
}

static int write_viewer_gfx_codec_config(const char *path,
                                         const char *codec_value) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp,
            "[instances]\n"
            "names = Test\n"
            "\n"
            "[instance:Test]\n"
            "backend.hostname = 127.0.0.1\n"
            "backend.port = 3389\n"
            "backend.username = alice\n"
            "backend.password = secret\n"
            "viewer.port = 3390\n"
            "viewer.gfx.codec = %s\n",
            codec_value);

  fclose(fp);
  return 1;
}

static int write_viewer_gfx_dirty_limits_config(const char *path,
                                                unsigned int frames,
                                                unsigned int bytes) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp,
            "[instances]\n"
            "names = Test\n"
            "\n"
            "[instance:Test]\n"
            "backend.hostname = 127.0.0.1\n"
            "backend.port = 3389\n"
            "backend.username = alice\n"
            "backend.password = secret\n"
            "viewer.port = 3390\n"
            "viewer.gfx.dirty_max_in_flight_frames = %u\n"
            "viewer.gfx.dirty_max_in_flight_bytes = %u\n",
            frames, bytes);

  fclose(fp);
  return 1;
}

static int write_backend_rdp_file_options_config(const char *path) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp, "[instances]\n"
                "names = Test\n"
                "\n"
                "[instance:Test]\n"
                "backend.hostname = rds-broker.domain.com\n"
                "backend.port = 3389\n"
                "backend.username = alice\n"
                "backend.password = secret\n"
                "viewer.port = 3390\n"
                "backend.rdp_file.workspace_id = rds-broker.domain.com\n"
                "backend.rdp_file.use_redirection_server_name = true\n"
                "backend.rdp_file.loadbalanceinfo = "
                "tsv://MS Terminal Services Plugin.1.Marketing_Pool\n"
                "backend.rdp_file.alternate_full_address = "
                "rds-broker.domain.com\n");

  fclose(fp);
  return 1;
}

static int write_backend_rdp_file_options_default_config(const char *path) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp, "[instances]\n"
                "names = Test\n"
                "\n"
                "[instance:Test]\n"
                "backend.hostname = rds-broker.domain.com\n"
                "backend.port = 3389\n"
                "backend.username = alice\n"
                "backend.password = secret\n"
                "viewer.port = 3390\n");

  fclose(fp);
  return 1;
}
static int write_backend_credentials_without_nla_config(const char *path) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp, "[instances]\n"
                "names = Test\n"
                "\n"
                "[instance:Test]\n"
                "backend.hostname = 127.0.0.1\n"
                "backend.port = 3389\n"
                "backend.username = alice\n"
                "backend.password = secret\n"
                "viewer.port = 3390\n"
                "viewer.security.nla_enabled = false\n"
                "viewer.security.tls_enabled = true\n"
                "viewer.auth.mode = backend_credentials\n");

  fclose(fp);
  return 1;
}

static int write_backend_gfx_config(const char *path, int backend_gfx,
                                    int codec_gfx) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp,
            "[instances]\n"
            "names = Test\n"
            "\n"
            "[instance:Test]\n"
            "backend.hostname = 127.0.0.1\n"
            "backend.port = 3389\n"
            "backend.username = alice\n"
            "backend.password = secret\n"
            "viewer.port = 3390\n"
            "backend.gfx.decode_only_enabled = %s\n"
            "codec.graphics_pipeline = %s\n",
            backend_gfx ? "true" : "false", codec_gfx ? "true" : "false");

  fclose(fp);
  return 1;
}

static int write_backend_rfx_config(const char *path, int backend_rfx,
                                    int legacy_remote_fx) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp,
            "[instances]\n"
            "names = Test\n"
            "\n"
            "[instance:Test]\n"
            "backend.hostname = 127.0.0.1\n"
            "backend.port = 3389\n"
            "backend.username = alice\n"
            "backend.password = secret\n"
            "viewer.port = 3390\n"
            "backend.gfx.rfx_enabled = %s\n"
            "codec.remote_fx = %s\n",
            backend_rfx ? "true" : "false",
            legacy_remote_fx ? "true" : "false");

  fclose(fp);
  return 1;
}

static int write_viewer_gfx_rfx_threading_config(const char *path,
                                                 int enabled) {
  FILE *fp = NULL;
  if (fopen_s(&fp, path, "w") != 0 || !fp)
    return 0;

  fprintf_s(fp,
            "[instances]\n"
            "names = Test\n"
            "\n"
            "[instance:Test]\n"
            "backend.hostname = 127.0.0.1\n"
            "backend.port = 3389\n"
            "backend.username = alice\n"
            "backend.password = secret\n"
            "viewer.port = 3390\n"
            "viewer.gfx.rfx_threading_enabled = %s\n",
            enabled ? "true" : "false");

  fclose(fp);
  return 1;
}

int main(void) {
  const char *path = "test_svc_config.ini";
  const char *bad_viewer_path = "test_svc_config_bad_viewer.ini";
  const char *classic_latest_path = "test_svc_config_classic_latest.ini";
  const char *viewer_gfx_path = "test_svc_config_viewer_gfx.ini";
  const char *viewer_gfx_rfx_threading_path =
      "test_svc_config_viewer_gfx_rfx_threading.ini";
  const char *viewer_gfx_dirty_limits_path =
      "test_svc_config_viewer_gfx_dirty_limits.ini";
  const char *viewer_gfx_full_frame_dirty_path =
      "test_svc_config_viewer_gfx_full_frame_dirty.ini";
  const char *backend_gfx_path = "test_svc_config_backend_gfx.ini";
  const char *backend_rfx_path = "test_svc_config_backend_rfx.ini";
  const char *legacy_remote_fx_path = "test_svc_config_legacy_remote_fx.ini";
  const char *codec_gfx_path = "test_svc_config_codec_gfx.ini";
  const char *viewer_gfx_codec_rfx_path = "test_svc_config_gfx_codec_rfx.ini";
  const char *viewer_gfx_codec_remote_fx_path =
      "test_svc_config_gfx_codec_remote_fx.ini";
  const char *viewer_gfx_codec_invalid_path =
      "test_svc_config_gfx_codec_invalid.ini";
  const char *backend_credentials_without_nla_path =
      "test_svc_config_backend_credentials_without_nla.ini";
  const char *backend_rdp_file_options_path =
      "test_svc_config_backend_rdp_file_options.ini";
  const char *backend_rdp_file_options_default_path =
      "test_svc_config_backend_rdp_file_options_default.ini";
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
      inst->viewer_classic_latest_state_max_queue_bytes != 0 ||
      inst->viewer_gfx_enabled != 0 ||
      inst->viewer_gfx_codec != SVC_VIEWER_GFX_CODEC_UNCOMPRESSED ||
      inst->viewer_gfx_rfx_threading_enabled != 0 ||
      inst->viewer_gfx_dirty_max_in_flight_frames != 1 ||
      inst->viewer_gfx_dirty_max_in_flight_bytes != 4U * 1024U * 1024U ||
      inst->viewer_gfx_diagnostic_full_frame_dirty != 0 ||
      inst->backend_gfx_decode_only_enabled != 0 ||
      inst->backend_gfx_rfx_enabled != 0)
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

  if (!write_viewer_gfx_config(viewer_gfx_path, 1)) {
    remove(path);
    remove(classic_latest_path);
    return 1;
  }

  config = svc_config_load(viewer_gfx_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->viewer_gfx_enabled != 1)
    ok = 0;

  svc_config_free(config);

  if (!write_viewer_gfx_rfx_threading_config(viewer_gfx_rfx_threading_path,
                                             1)) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    return 1;
  }

  config = svc_config_load(viewer_gfx_rfx_threading_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_rfx_threading_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->viewer_gfx_rfx_threading_enabled != 1)
    ok = 0;

  svc_config_free(config);

  if (!write_viewer_gfx_dirty_limits_config(viewer_gfx_dirty_limits_path, 2,
                                            8U * 1024U * 1024U)) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_rfx_threading_path);
    return 1;
  }

  config = svc_config_load(viewer_gfx_dirty_limits_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_rfx_threading_path);
    remove(viewer_gfx_dirty_limits_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->viewer_gfx_dirty_max_in_flight_frames != 2 ||
      inst->viewer_gfx_dirty_max_in_flight_bytes != 8U * 1024U * 1024U)
    ok = 0;

  svc_config_free(config);

  if (!write_viewer_gfx_full_frame_dirty_config(
          viewer_gfx_full_frame_dirty_path, 1)) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_rfx_threading_path);
    return 1;
  }

  config = svc_config_load(viewer_gfx_full_frame_dirty_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_rfx_threading_path);
    remove(viewer_gfx_full_frame_dirty_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->viewer_gfx_diagnostic_full_frame_dirty != 1)
    ok = 0;

  svc_config_free(config);

  if (!write_backend_gfx_config(backend_gfx_path, 1, 0)) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    return 1;
  }

  config = svc_config_load(backend_gfx_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->backend_gfx_decode_only_enabled != 1 ||
      inst->codec_graphics_pipeline != 0)
    ok = 0;

  svc_config_free(config);

  if (!write_backend_rfx_config(backend_rfx_path, 1, 0)) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    return 1;
  }

  config = svc_config_load(backend_rfx_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(backend_rfx_path);
    remove(codec_gfx_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->backend_gfx_rfx_enabled != 1 || inst->codec_remote_fx != 0)
    ok = 0;

  svc_config_free(config);

  if (!write_backend_rfx_config(legacy_remote_fx_path, 0, 1)) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(backend_rfx_path);
    remove(codec_gfx_path);
    return 1;
  }

  config = svc_config_load(legacy_remote_fx_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(backend_rfx_path);
    remove(legacy_remote_fx_path);
    remove(codec_gfx_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->backend_gfx_rfx_enabled != 0 || inst->codec_remote_fx != 1)
    ok = 0;

  svc_config_free(config);

  if (!write_backend_gfx_config(codec_gfx_path, 0, 1)) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(backend_rfx_path);
    remove(legacy_remote_fx_path);
    return 1;
  }

  config = svc_config_load(codec_gfx_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(backend_rfx_path);
    remove(legacy_remote_fx_path);
    remove(codec_gfx_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->backend_gfx_decode_only_enabled != 0 ||
      inst->codec_graphics_pipeline != 1)
    ok = 0;

  svc_config_free(config);

  if (!write_viewer_gfx_codec_config(viewer_gfx_codec_rfx_path, "rfx")) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    return 1;
  }

  config = svc_config_load(viewer_gfx_codec_rfx_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->viewer_gfx_codec != SVC_VIEWER_GFX_CODEC_RFX)
    ok = 0;
  svc_config_free(config);

  if (!write_viewer_gfx_codec_config(viewer_gfx_codec_remote_fx_path,
                                     "remote_fx")) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    return 1;
  }

  config = svc_config_load(viewer_gfx_codec_remote_fx_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    remove(viewer_gfx_codec_remote_fx_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->viewer_gfx_codec != SVC_VIEWER_GFX_CODEC_RFX)
    ok = 0;
  svc_config_free(config);

  if (!write_viewer_gfx_codec_config(viewer_gfx_codec_invalid_path,
                                     "not_a_codec")) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    remove(viewer_gfx_codec_remote_fx_path);
    return 1;
  }

  config = svc_config_load(viewer_gfx_codec_invalid_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    remove(viewer_gfx_codec_remote_fx_path);
    remove(viewer_gfx_codec_invalid_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->viewer_gfx_codec != SVC_VIEWER_GFX_CODEC_UNCOMPRESSED)
    ok = 0;
  svc_config_free(config);

  if (!write_backend_rdp_file_options_config(backend_rdp_file_options_path)) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    remove(viewer_gfx_codec_remote_fx_path);
    remove(viewer_gfx_codec_invalid_path);
    return 1;
  }

  config = svc_config_load(backend_rdp_file_options_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    remove(viewer_gfx_codec_remote_fx_path);
    remove(viewer_gfx_codec_invalid_path);
    remove(backend_rdp_file_options_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst ||
      strcmp(inst->backend_rdp_workspace_id, "rds-broker.domain.com") != 0 ||
      inst->backend_rdp_use_redirection_server_name != 1 ||
      strcmp(inst->backend_rdp_loadbalanceinfo,
             "tsv://MS Terminal Services Plugin.1.Marketing_Pool") != 0 ||
      strcmp(inst->backend_rdp_alternate_full_address,
             "rds-broker.domain.com") != 0)
    ok = 0;
  svc_config_free(config);

  if (!write_backend_rdp_file_options_default_config(
          backend_rdp_file_options_default_path)) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    remove(viewer_gfx_codec_remote_fx_path);
    remove(viewer_gfx_codec_invalid_path);
    remove(backend_rdp_file_options_path);
    return 1;
  }

  config = svc_config_load(backend_rdp_file_options_default_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    remove(viewer_gfx_codec_remote_fx_path);
    remove(viewer_gfx_codec_invalid_path);
    remove(backend_rdp_file_options_path);
    remove(backend_rdp_file_options_default_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->backend_rdp_workspace_id[0] != '\0' ||
      inst->backend_rdp_use_redirection_server_name != 0 ||
      inst->backend_rdp_loadbalanceinfo[0] != '\0' ||
      inst->backend_rdp_alternate_full_address[0] != '\0')
    ok = 0;
  svc_config_free(config);

  if (!write_backend_credentials_without_nla_config(
          backend_credentials_without_nla_path)) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    remove(viewer_gfx_codec_remote_fx_path);
    remove(viewer_gfx_codec_invalid_path);
    return 1;
  }

  config = svc_config_load(backend_credentials_without_nla_path);
  if (!config) {
    remove(path);
    remove(classic_latest_path);
    remove(viewer_gfx_path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_gfx_path);
    remove(codec_gfx_path);
    remove(viewer_gfx_codec_rfx_path);
    remove(viewer_gfx_codec_remote_fx_path);
    remove(viewer_gfx_codec_invalid_path);
    remove(backend_credentials_without_nla_path);
    remove(backend_rdp_file_options_path);
    remove(backend_rdp_file_options_default_path);
    return 1;
  }

  inst = svc_config_find_instance(config, "Test");
  if (!inst || inst->viewer_security_nla_enabled != 0 ||
      inst->viewer_security_tls_enabled != 1 ||
      strcmp(inst->viewer_auth_mode, "backend_credentials") != 0)
    ok = 0;
  svc_config_free(config);

  if (!write_viewer_port_config(bad_viewer_path)) {
    remove(path);
    remove(viewer_gfx_full_frame_dirty_path);
    remove(backend_credentials_without_nla_path);
    remove(backend_rdp_file_options_path);
    remove(backend_rdp_file_options_default_path);
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
  remove(viewer_gfx_path);
  remove(viewer_gfx_rfx_threading_path);
  remove(viewer_gfx_dirty_limits_path);
  remove(viewer_gfx_full_frame_dirty_path);
  remove(backend_gfx_path);
  remove(backend_rfx_path);
  remove(legacy_remote_fx_path);
  remove(codec_gfx_path);
  remove(viewer_gfx_codec_rfx_path);
  remove(viewer_gfx_codec_remote_fx_path);
  remove(viewer_gfx_codec_invalid_path);
  remove(backend_credentials_without_nla_path);
  remove(backend_rdp_file_options_path);
  remove(backend_rdp_file_options_default_path);
  return ok ? 0 : 1;
}
