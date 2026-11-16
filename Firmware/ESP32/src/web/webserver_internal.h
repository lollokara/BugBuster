#pragma once

// =============================================================================
// webserver_internal.h - shared by webserver.cpp and the webserver_*.cpp handler files.
// Not an API: nothing outside web/ includes this.
// =============================================================================

#include "freertos/idf_additions.h"  // vTaskDeleteWithCaps
#include "webserver.h"
#include "cors.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_mac.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

#include "tasks.h"
#include "config.h"
#include "ad74416h.h"
#include "ad74416h_spi.h"
#include "ad74416h_regs.h"
#include "uart_bridge.h"
#include "i2c_bus.h"
#include "ds4424.h"
#include "husb238.h"
#include "pca9535.h"
#include "power/rail_power.h"
#include "cmd_errors.h"
#include "hat.h"
#include "adgs2414d.h"
#include "dio.h"
#include "bus_planner.h"
#include "selftest.h"
#include "bbp.h"
#include "auth.h"
#include "board_profile.h"
#include "wifi_manager.h"
#include "mdns_responder.h"
#include "ws_stream.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "quicksetup.h"
#include "ext_bus.h"
#include "http_adapter.h"
#include "io_owner.h"
#include "scripting.h"
#include "script_storage.h"
#include "autorun.h"
#include "diag/crash_report.h"
#include "repl_ws.h"
#include "serial_io.h"
#include "cli/cli_term.h"
#include "pd_vadj_guard.h"
#include "update_manager.h"
#include "api_core.h"
#include "api_standby.h"
#include "standby_api.h"
#include "standby_hw.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "bbp.h"

extern AD74416H_SPI spiDriver;

#define BUGBUSTER_WEB_BUILD_MARKER "scripts-fix-20260427-1150"

#define VALIDATE_JSON_FIELD(obj, key, type, error_msg) \
    do { \
        cJSON *item = cJSON_GetObjectItem(obj, key); \
        if (!item || !cJSON_Is##type(item)) { \
            cJSON_Delete(obj); \
            return send_error(req, 400, error_msg); \
        } \
    } while(0)

// webserver.cpp
void add_bool_alias(cJSON *obj, const char *camel, const char * /*retired*/, bool value);
void add_number_alias(cJSON *obj, const char *camel, const char * /*retired*/, double value);
esp_err_t check_admin_auth(httpd_req_t *req);
const char* diagSourceName(uint8_t source);
const char* diagSourceUnit(uint8_t source);
int extract_channel(const char* uri);
int extract_fault_channel(const char* uri, const char* prefix);
const char* gpioModeName(uint8_t mode);
cJSON* recv_json_body(httpd_req_t *req);
cJSON* recv_json_body_cap(httpd_req_t *req, int cap);
esp_err_t send_error(httpd_req_t *req, int code, const char *msg);
esp_err_t send_json(httpd_req_t *req, cJSON *root, int code = 200);
esp_err_t send_raw_json(httpd_req_t *req, const char *body, int code = 200);
void set_cors_headers(httpd_req_t *req, char *origin_buf, size_t origin_buf_size);
int upload_recv(httpd_req_t *req, char *buf, size_t len);

// webserver_core.cpp
esp_err_t handle_browser_fallback(httpd_req_t *req);
esp_err_t handle_channel_get_dispatch(httpd_req_t *req);
esp_err_t handle_channel_post_dispatch(httpd_req_t *req);
esp_err_t handle_get_device_info(httpd_req_t *req);
esp_err_t handle_get_diagnostics(httpd_req_t *req);
esp_err_t handle_get_faults(httpd_req_t *req);
esp_err_t handle_get_gpio(httpd_req_t *req);
esp_err_t handle_get_scope(httpd_req_t *req);
esp_err_t handle_get_scope_stream(httpd_req_t *req);
esp_err_t handle_get_status(httpd_req_t *req);
esp_err_t handle_get_system_crash(httpd_req_t *req);
esp_err_t handle_get_system_memory(httpd_req_t *req);
esp_err_t handle_gpio_post_dispatch(httpd_req_t *req);
esp_err_t handle_post_adc_dsp_start(httpd_req_t *req);
esp_err_t handle_post_adc_dsp_stop(httpd_req_t *req);
esp_err_t handle_post_channel_fault_mask(httpd_req_t *req);
esp_err_t handle_post_clear_all_faults(httpd_req_t *req);
esp_err_t handle_post_clear_channel_fault(httpd_req_t *req);
esp_err_t handle_post_device_reset(httpd_req_t *req);
esp_err_t handle_post_diag_config(httpd_req_t *req);
esp_err_t handle_post_faults_mask(httpd_req_t *req);
esp_err_t handle_post_system_crash_clear(httpd_req_t *req);
esp_err_t handle_root(httpd_req_t *req);
esp_err_t handle_static_asset(httpd_req_t *req);

// webserver_hat.cpp
esp_err_t handle_get_daq(httpd_req_t *req);
esp_err_t handle_get_daq_vdut_status(httpd_req_t *req);
esp_err_t handle_get_daq_wifi_stream_status(httpd_req_t *req);
esp_err_t handle_get_debug(httpd_req_t *req);
esp_err_t handle_get_hat(httpd_req_t *req);
esp_err_t handle_get_hat_calibration(httpd_req_t *req);
esp_err_t handle_get_hat_power(httpd_req_t *req);
esp_err_t handle_get_hat_v2_calibrate_status(httpd_req_t *req);
esp_err_t handle_get_hat_v2_caps(httpd_req_t *req);
esp_err_t handle_get_hat_v2_la_log(httpd_req_t *req);
esp_err_t handle_get_hat_v2_rails(httpd_req_t *req);
esp_err_t handle_get_hub_config(httpd_req_t *req);
esp_err_t handle_get_hub_status(httpd_req_t *req);
esp_err_t handle_post_daq_acq_config(httpd_req_t *req);
esp_err_t handle_post_daq_bs(httpd_req_t *req);
esp_err_t handle_post_daq_bs_read(httpd_req_t *req);
esp_err_t handle_post_daq_vdut_enable(httpd_req_t *req);
esp_err_t handle_post_daq_vdut_setpoint(httpd_req_t *req);
esp_err_t handle_post_daq_wifi_stream_recycle(httpd_req_t *req);
esp_err_t handle_post_daq_wifi_stream_start(httpd_req_t *req);
esp_err_t handle_post_daq_wifi_stream_stop(httpd_req_t *req);
esp_err_t handle_post_hat_power(httpd_req_t *req);
esp_err_t handle_post_hat_v2_calibrate_import(httpd_req_t *req);
esp_err_t handle_post_hat_v2_calibrate_start(httpd_req_t *req);
esp_err_t handle_post_hat_v2_io_bank(httpd_req_t *req);
esp_err_t handle_post_hat_v2_io_voltage(httpd_req_t *req);
esp_err_t handle_post_hat_v2_la_log_enable(httpd_req_t *req);
esp_err_t handle_post_hat_v2_la_route(httpd_req_t *req);
esp_err_t handle_post_hat_v2_led(httpd_req_t *req);
esp_err_t handle_post_hat_v2_level_shift(httpd_req_t *req);
esp_err_t handle_post_hat_v2_rail_enable(httpd_req_t *req);
esp_err_t handle_post_hat_v2_rail_voltage(httpd_req_t *req);
esp_err_t handle_post_hat_v2_swd_setup(httpd_req_t *req);
esp_err_t handle_post_hub_config(httpd_req_t *req);
esp_err_t handle_post_hub_resync(httpd_req_t *req);
esp_err_t send_api_core_result(httpd_req_t *req, char *resp, const char *fail_msg);

// webserver_io.cpp
esp_err_t handle_bus_post_dispatch(httpd_req_t *req);
esp_err_t handle_delete_io_owner(httpd_req_t *req);
esp_err_t handle_dio_post_dispatch(httpd_req_t *req);
esp_err_t handle_get_adgs_routes(httpd_req_t *req);
esp_err_t handle_get_bus_status(httpd_req_t *req);
esp_err_t handle_get_dio(httpd_req_t *req);
esp_err_t handle_get_dio_single(httpd_req_t *req);
esp_err_t handle_get_efuse_imon(httpd_req_t *req);
esp_err_t handle_get_io_owner(httpd_req_t *req);
esp_err_t handle_get_mux(httpd_req_t *req);
esp_err_t handle_get_overview(httpd_req_t *req);
esp_err_t handle_get_quicksetup_list(httpd_req_t *req);
esp_err_t handle_get_quicksetup_slot(httpd_req_t *req);
esp_err_t handle_get_selftest(httpd_req_t *req);
esp_err_t handle_get_selftest_supplies(httpd_req_t *req);
esp_err_t handle_get_selftest_supplies_cached(httpd_req_t *req);
esp_err_t handle_get_selftest_supply(httpd_req_t *req);
esp_err_t handle_get_uart_config(httpd_req_t *req);
esp_err_t handle_get_uart_pins(httpd_req_t *req);
esp_err_t handle_mux_post_dispatch(httpd_req_t *req);
esp_err_t handle_post_efuse_imon(httpd_req_t *req);
esp_err_t handle_post_io_owner(httpd_req_t *req);
esp_err_t handle_post_io_owner_force(httpd_req_t *req);
esp_err_t handle_post_lshift_oe(httpd_req_t *req);
esp_err_t handle_post_selftest_calibrate(httpd_req_t *req);
esp_err_t handle_post_selftest_worker(httpd_req_t *req);
esp_err_t handle_quicksetup_post_dispatch(httpd_req_t *req);
esp_err_t handle_uart_post_dispatch(httpd_req_t *req);
esp_err_t handle_wavegen_post_dispatch(httpd_req_t *req);

// webserver_power.cpp
esp_err_t handle_get_idac(httpd_req_t *req);
esp_err_t handle_get_idac_cal_points(httpd_req_t *req);
esp_err_t handle_get_ioexp(httpd_req_t *req);
esp_err_t handle_get_ioexp_faults(httpd_req_t *req);
esp_err_t handle_get_usbpd(httpd_req_t *req);
esp_err_t handle_idac_post_dispatch(httpd_req_t *req);
esp_err_t handle_ioexp_post_dispatch(httpd_req_t *req);
esp_err_t handle_usbpd_post_dispatch(httpd_req_t *req);

// webserver_scripts.cpp
esp_err_t handle_delete_scripts_file(httpd_req_t *req);
esp_err_t handle_get_autorun_status(httpd_req_t *req);
esp_err_t handle_get_scripts_file(httpd_req_t *req);
esp_err_t handle_get_scripts_files(httpd_req_t *req);
esp_err_t handle_get_scripts_logs(httpd_req_t *req);
esp_err_t handle_get_scripts_status(httpd_req_t *req);
esp_err_t handle_get_scripts_storage(httpd_req_t *req);
esp_err_t handle_post_autorun_disable(httpd_req_t *req);
esp_err_t handle_post_autorun_enable(httpd_req_t *req);
esp_err_t handle_post_scripts_eval(httpd_req_t *req);
esp_err_t handle_post_scripts_files(httpd_req_t *req);
esp_err_t handle_post_scripts_files_chunk(httpd_req_t *req);
esp_err_t handle_post_scripts_files_delete(httpd_req_t *req);
esp_err_t handle_post_scripts_lint(httpd_req_t *req);
esp_err_t handle_post_scripts_reset(httpd_req_t *req);
esp_err_t handle_post_scripts_run_file(httpd_req_t *req);
esp_err_t handle_post_scripts_stop(httpd_req_t *req);

// webserver_system.cpp
esp_err_t handle_get_board(httpd_req_t *req);
esp_err_t handle_get_hat_la_status(httpd_req_t *req);
esp_err_t handle_get_pairing_info(httpd_req_t *req);
esp_err_t handle_get_standby_status(httpd_req_t *req);
esp_err_t handle_get_version(httpd_req_t *req);
esp_err_t handle_get_wifi(httpd_req_t *req);
esp_err_t handle_get_wifi_hostname(httpd_req_t *req);
esp_err_t handle_get_wifi_scan(httpd_req_t *req);
esp_err_t handle_hat_post_dispatch(httpd_req_t *req);
esp_err_t handle_ota_upload(httpd_req_t *req);
esp_err_t handle_post_board_select(httpd_req_t *req);
esp_err_t handle_post_daq_config(httpd_req_t *req);
esp_err_t handle_post_pairing_rotate(httpd_req_t *req);
esp_err_t handle_post_pairing_verify(httpd_req_t *req);
esp_err_t handle_post_standby_policy(httpd_req_t *req);
esp_err_t handle_post_standby_presence(httpd_req_t *req);
esp_err_t handle_post_standby_sleep(httpd_req_t *req);
esp_err_t handle_post_standby_wake(httpd_req_t *req);
esp_err_t handle_post_wifi_ap_password(httpd_req_t *req);
esp_err_t handle_post_wifi_connect(httpd_req_t *req);
esp_err_t handle_post_wifi_hostname(httpd_req_t *req);
esp_err_t handle_rp2040_upload(httpd_req_t *req);
bool parse_sha256_query(httpd_req_t *req, uint8_t out[32], bool *out_present);

// webserver_update.cpp
esp_err_t handle_c6_upload(httpd_req_t *req);
esp_err_t handle_get_ota_info(httpd_req_t *req);
esp_err_t handle_get_update_check(httpd_req_t *req);
esp_err_t handle_get_update_status(httpd_req_t *req);
esp_err_t handle_p4_upload(httpd_req_t *req);
esp_err_t handle_post_ota_rollback(httpd_req_t *req);
esp_err_t handle_post_update_apply(httpd_req_t *req);
esp_err_t handle_uploadfs(httpd_req_t *req);
