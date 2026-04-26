#pragma once

// =============================================================================
// cli_cmds_sys.h - System command handlers (GPIO, WiFi, I2C peripherals,
// MUX, SPI clock). Bodies moved verbatim from cli.cpp during the phase-1 CLI
// refactor; the shell/dispatch layer above them is new.
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

void cli_cmd_gpio(const char* args);
void cli_cmd_wifi(const char* args);
void cli_cmd_usbpd(const char* args);
void cli_cmd_pca(const char* args);
void cli_cmd_supplies(const char* args);
void cli_cmd_selftest(const char* args);
void cli_cmd_i2c_scan(const char* args);
void cli_cmd_mux(const char* args);
void cli_cmd_mux_reset(const char* args);
void cli_cmd_cstest(const char* args);
void cli_cmd_muxtest(const char* args);
void cli_cmd_spiclock(const char* args);
void cli_cmd_token(const char* args);
void cli_cmd_rstinfo(const char* args);

#ifdef __cplusplus
}
#endif
