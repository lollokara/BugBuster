#pragma once

// =============================================================================
// cli_cmds_dev.h - Device-oriented command handlers (AD74416H, DAC, ADC, DIO,
// faults, IDAC). Bodies moved verbatim from cli.cpp during the phase-1 CLI
// refactor; the shell/dispatch layer above them is new.
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

void cli_cmd_menu(const char* args);
void cli_cmd_status(const char* args);
void cli_cmd_temp(const char* args);
void cli_cmd_reset(const char* args);
void cli_cmd_scratch(const char* args);
void cli_cmd_silicon(const char* args);
void cli_cmd_regs(const char* args);

void cli_cmd_read_reg(const char* args);
void cli_cmd_write_reg(const char* args);

void cli_cmd_func(const char* args);
void cli_cmd_ilimit(const char* args);
void cli_cmd_vrange(const char* args);
void cli_cmd_avdd(const char* args);

void cli_cmd_adc(const char* args);
void cli_cmd_adc_cont(const char* args);
void cli_cmd_adc_diag(const char* args);
void cli_cmd_diag_cfg(const char* args);
void cli_cmd_diag_read(const char* args);

void cli_cmd_dac(const char* args);
void cli_cmd_sweep(const char* args);

void cli_cmd_din(const char* args);
void cli_cmd_do_set(const char* args);

void cli_cmd_faults(const char* args);
void cli_cmd_clear_faults(const char* args);

void cli_cmd_idac(const char* args);
void cli_cmd_idac_cal(const char* args);
void cli_cmds_dev_tick(void);

#ifdef __cplusplus
}
#endif
