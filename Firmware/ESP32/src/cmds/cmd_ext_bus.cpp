// =============================================================================
// cmd_ext_bus.cpp — Registry handlers for external target bus commands
//
//   BBP_CMD_EXT_I2C_SETUP      (0xB8)
//   BBP_CMD_EXT_I2C_SCAN       (0xB9)
//   BBP_CMD_EXT_I2C_WRITE      (0xBA)
//   BBP_CMD_EXT_I2C_READ       (0xBB)
//   BBP_CMD_EXT_I2C_WRITE_READ (0xBC)
//   BBP_CMD_EXT_SPI_SETUP      (0xBD)
//   BBP_CMD_EXT_SPI_TRANSFER   (0xBE)
//   BBP_CMD_EXT_JOB_SUBMIT     (0x75)
//   BBP_CMD_EXT_JOB_GET        (0x76)
// =============================================================================
#include "../cmd_registry.h"
#include "../cmd_errors.h"
#include "../bbp_codec.h"
#include "../bbp.h"
#include "../ext_bus.h"

// ---------------------------------------------------------------------------
// EXT_I2C_SETUP  payload: u8 sda_gpio, u8 scl_gpio, u32 frequency_hz, bool pullups
// resp: u8 sda_gpio, u8 scl_gpio, u32 frequency_hz, bool pullups
// Wire format matches legacy handleExtI2cSetup (bbp.cpp:2138-2158).
// ---------------------------------------------------------------------------
static int handler_ext_i2c_setup(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 7) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  sda_gpio         = bbp_get_u8(payload, &rpos);
    uint8_t  scl_gpio         = bbp_get_u8(payload, &rpos);
    uint32_t frequency_hz     = bbp_get_u32(payload, &rpos);
    bool     internal_pullups = bbp_get_bool(payload, &rpos);

    if (!ext_i2c_setup(sda_gpio, scl_gpio, frequency_hz, internal_pullups))
        return -CMD_ERR_BAD_ARG;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, sda_gpio);
    bbp_put_u8(resp, &pos, scl_gpio);
    bbp_put_u32(resp, &pos, frequency_hz);
    bbp_put_bool(resp, &pos, internal_pullups);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// EXT_I2C_SCAN  payload: u8 start_addr, u8 stop_addr, bool skip_reserved, u16 timeout_ms
// resp: u8 count, then count u8 addresses
// Wire format matches legacy handleExtI2cScan (bbp.cpp:2161-2183).
// ---------------------------------------------------------------------------
static int handler_ext_i2c_scan(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    if (len < 5) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  start_addr    = bbp_get_u8(payload, &rpos);
    uint8_t  stop_addr     = bbp_get_u8(payload, &rpos);
    bool     skip_reserved = bbp_get_bool(payload, &rpos);
    uint16_t timeout_ms    = bbp_get_u16(payload, &rpos);

    uint8_t addrs[128] = {};
    size_t count = 0;
    if (!ext_i2c_scan(start_addr, stop_addr, skip_reserved, addrs, sizeof(addrs), &count, timeout_ms)) {
        return ext_i2c_ready() ? -CMD_ERR_BAD_ARG : -CMD_ERR_INVALID_STATE;
    }

    size_t pos = 0;
    bbp_put_u8(resp, &pos, (uint8_t)count);
    for (size_t i = 0; i < count; i++) {
        bbp_put_u8(resp, &pos, addrs[i]);
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// EXT_I2C_WRITE  payload: u8 addr, u16 timeout_ms, u8 wr_len, then wr_len bytes
// resp: u8 wr_len
// Wire format matches legacy handleExtI2cWrite (bbp.cpp:2186-2201).
// ---------------------------------------------------------------------------
static int handler_ext_i2c_write(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 4) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  addr       = bbp_get_u8(payload, &rpos);
    uint16_t timeout_ms = bbp_get_u16(payload, &rpos);
    uint8_t  wr_len     = bbp_get_u8(payload, &rpos);
    if (len < 4 + wr_len) return -CMD_ERR_BAD_ARG;

    if (!ext_i2c_write(addr, payload + rpos, wr_len, timeout_ms)) {
        return ext_i2c_ready() ? -CMD_ERR_TIMEOUT : -CMD_ERR_INVALID_STATE;
    }

    resp[0] = wr_len;
    *resp_len = 1;
    return 1;
}

// ---------------------------------------------------------------------------
// EXT_I2C_READ  payload: u8 addr, u16 timeout_ms, u8 rd_len
// resp: u8 rd_len, then rd_len bytes
// Wire format matches legacy handleExtI2cRead (bbp.cpp:2204-2222).
// Note: uses resp+1 for data bytes, matching legacy out[0] = rd_len pattern.
// ---------------------------------------------------------------------------
static int handler_ext_i2c_read(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    if (len < 4) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  addr       = bbp_get_u8(payload, &rpos);
    uint16_t timeout_ms = bbp_get_u16(payload, &rpos);
    uint8_t  rd_len     = bbp_get_u8(payload, &rpos);
    if (rd_len == 0 || rd_len > BBP_MAX_PAYLOAD - 1) return -CMD_ERR_BAD_ARG;

    if (!ext_i2c_read(addr, resp + 1, rd_len, timeout_ms)) {
        return ext_i2c_ready() ? -CMD_ERR_TIMEOUT : -CMD_ERR_INVALID_STATE;
    }

    resp[0] = rd_len;
    *resp_len = 1 + rd_len;
    return (int)(1 + rd_len);
}

// ---------------------------------------------------------------------------
// EXT_I2C_WRITE_READ  payload: u8 addr, u16 timeout_ms, u8 wr_len, u8 rd_len,
//                               then wr_len bytes
// resp: u8 rd_len, then rd_len bytes
// Wire format matches legacy handleExtI2cWriteRead (bbp.cpp:2225-2244).
// ---------------------------------------------------------------------------
static int handler_ext_i2c_write_read(const uint8_t *payload, size_t len,
                                      uint8_t *resp, size_t *resp_len)
{
    if (len < 5) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  addr       = bbp_get_u8(payload, &rpos);
    uint16_t timeout_ms = bbp_get_u16(payload, &rpos);
    uint8_t  wr_len     = bbp_get_u8(payload, &rpos);
    uint8_t  rd_len     = bbp_get_u8(payload, &rpos);
    if (wr_len == 0 || rd_len == 0 || rd_len > BBP_MAX_PAYLOAD - 1 || len < 5 + wr_len)
        return -CMD_ERR_BAD_ARG;

    if (!ext_i2c_write_read(addr, payload + rpos, wr_len, resp + 1, rd_len, timeout_ms)) {
        return ext_i2c_ready() ? -CMD_ERR_TIMEOUT : -CMD_ERR_INVALID_STATE;
    }

    resp[0] = rd_len;
    *resp_len = 1 + rd_len;
    return (int)(1 + rd_len);
}

// ---------------------------------------------------------------------------
// EXT_SPI_SETUP  payload: u8 sck, u8 mosi, u8 miso, u8 cs, u32 freq_hz, u8 mode
// resp: u8 sck, u8 mosi, u8 miso, u8 cs, u32 freq_hz, u8 mode
// Wire format matches legacy handleExtSpiSetup (bbp.cpp:2247-2271).
// ---------------------------------------------------------------------------
static int handler_ext_spi_setup(const uint8_t *payload, size_t len,
                                 uint8_t *resp, size_t *resp_len)
{
    if (len < 9) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  sck_gpio    = bbp_get_u8(payload, &rpos);
    uint8_t  mosi_gpio   = bbp_get_u8(payload, &rpos);
    uint8_t  miso_gpio   = bbp_get_u8(payload, &rpos);
    uint8_t  cs_gpio     = bbp_get_u8(payload, &rpos);
    uint32_t frequency_hz = bbp_get_u32(payload, &rpos);
    uint8_t  mode        = bbp_get_u8(payload, &rpos);

    if (!ext_spi_setup(sck_gpio, mosi_gpio, miso_gpio, cs_gpio, frequency_hz, mode))
        return -CMD_ERR_BAD_ARG;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, sck_gpio);
    bbp_put_u8(resp, &pos, mosi_gpio);
    bbp_put_u8(resp, &pos, miso_gpio);
    bbp_put_u8(resp, &pos, cs_gpio);
    bbp_put_u32(resp, &pos, frequency_hz);
    bbp_put_u8(resp, &pos, mode);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// EXT_SPI_TRANSFER  payload: u16 timeout_ms, u16 tx_len, then tx_len bytes
// resp: u16 rx_len, then rx_len bytes
// Wire format matches legacy handleExtSpiTransfer (bbp.cpp:2274-2293).
// Note: rx data written to resp+2, matching legacy out+2 pattern.
// ---------------------------------------------------------------------------
static int handler_ext_spi_transfer(const uint8_t *payload, size_t len,
                                    uint8_t *resp, size_t *resp_len)
{
    if (len < 4) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint16_t timeout_ms = bbp_get_u16(payload, &rpos);
    uint16_t tx_len     = bbp_get_u16(payload, &rpos);
    if (len < 4 + tx_len || tx_len > 512) return -CMD_ERR_BAD_ARG;

    size_t rx_len = tx_len;
    if (!ext_spi_transfer(payload + rpos, tx_len, resp + 2, &rx_len, timeout_ms)) {
        return ext_spi_ready() ? -CMD_ERR_TIMEOUT : -CMD_ERR_INVALID_STATE;
    }

    size_t pos = 0;
    bbp_put_u16(resp, &pos, (uint16_t)rx_len);
    *resp_len = 2 + rx_len;
    return (int)(2 + rx_len);
}

// ---------------------------------------------------------------------------
// EXT_JOB_SUBMIT  payload: u8 kind, u16 timeout_ms, then kind-specific args
// resp: u32 job_id
// Wire format matches legacy handleExtJobSubmit (bbp.cpp:2296-2334).
// ---------------------------------------------------------------------------
static int handler_ext_job_submit(const uint8_t *payload, size_t len,
                                  uint8_t *resp, size_t *resp_len)
{
    if (len < 3) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t  kind       = bbp_get_u8(payload, &rpos);
    uint16_t timeout_ms = bbp_get_u16(payload, &rpos);

    uint32_t job_id = 0;
    bool ok = false;

    if (kind == EXT_BUS_JOB_I2C_READ) {
        if (len < rpos + 2) return -CMD_ERR_BAD_ARG;
        uint8_t addr     = bbp_get_u8(payload, &rpos);
        uint8_t read_len = bbp_get_u8(payload, &rpos);
        ok = ext_job_submit_i2c_read(addr, read_len, timeout_ms, &job_id);
    } else if (kind == EXT_BUS_JOB_I2C_WRITE_READ) {
        if (len < rpos + 3) return -CMD_ERR_BAD_ARG;
        uint8_t addr     = bbp_get_u8(payload, &rpos);
        uint8_t wr_len   = bbp_get_u8(payload, &rpos);
        uint8_t read_len = bbp_get_u8(payload, &rpos);
        if (len < rpos + wr_len) return -CMD_ERR_BAD_ARG;
        ok = ext_job_submit_i2c_write_read(addr, payload + rpos, wr_len, read_len, timeout_ms, &job_id);
    } else if (kind == EXT_BUS_JOB_SPI_TRANSFER) {
        if (len < rpos + 2) return -CMD_ERR_BAD_ARG;
        uint16_t tx_len = bbp_get_u16(payload, &rpos);
        if (len < rpos + tx_len) return -CMD_ERR_BAD_ARG;
        ok = ext_job_submit_spi_transfer(payload + rpos, tx_len, timeout_ms, &job_id);
    } else {
        return -CMD_ERR_BAD_ARG;
    }

    if (!ok) return -CMD_ERR_INVALID_STATE;

    size_t pos = 0;
    bbp_put_u32(resp, &pos, job_id);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// EXT_JOB_GET  payload: u32 job_id
// resp: u32 job_id, u8 status, u8 kind, u16 result_len, then result bytes
// Wire format matches legacy handleExtJobGet (bbp.cpp:2337-2362).
// ---------------------------------------------------------------------------
static int handler_ext_job_get(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    if (len < 4) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint32_t job_id = bbp_get_u32(payload, &rpos);

    uint8_t status = EXT_BUS_JOB_EMPTY;
    uint8_t kind = 0;
    size_t result_len = 0;
    uint8_t result[512] = {};

    if (!ext_job_get(job_id, &status, &kind, result, sizeof(result), &result_len))
        return -CMD_ERR_BAD_ARG;

    size_t pos = 0;
    bbp_put_u32(resp, &pos, job_id);
    bbp_put_u8(resp, &pos, status);
    bbp_put_u8(resp, &pos, kind);
    bbp_put_u16(resp, &pos, (uint16_t)result_len);
    if (result_len > 0) {
        memcpy(resp + pos, result, result_len);
        pos += result_len;
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
// ext_i2c_write, ext_i2c_write_read, ext_spi_transfer: contain BLOB payload
// data — args=NULL (CLI adapter rejects BLOB-containing commands).
// ext_i2c_scan, ext_job_submit, ext_job_get: variable/complex responses.

static const ArgSpec s_ext_i2c_setup_args[] = {
    { "sda_gpio",       ARG_U8,   true, 0, 255 },
    { "scl_gpio",       ARG_U8,   true, 0, 255 },
    { "frequency_hz",   ARG_U32,  true, 100, 1000000 },
    { "internal_pullups", ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_ext_i2c_setup_rsp[] = {
    { "sda_gpio",       ARG_U8,   true, 0, 0 },
    { "scl_gpio",       ARG_U8,   true, 0, 0 },
    { "frequency_hz",   ARG_U32,  true, 0, 0 },
    { "internal_pullups", ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_ext_i2c_scan_args[] = {
    { "start_addr",    ARG_U8,   true, 0, 127 },
    { "stop_addr",     ARG_U8,   true, 0, 127 },
    { "skip_reserved", ARG_BOOL, true, 0, 0 },
    { "timeout_ms",    ARG_U16,  true, 0, 60000 },
};

static const ArgSpec s_ext_i2c_read_args[] = {
    { "addr",       ARG_U8,  true, 0, 127 },
    { "timeout_ms", ARG_U16, true, 0, 60000 },
    { "rd_len",     ARG_U8,  true, 0, 255 },
};

static const ArgSpec s_ext_spi_setup_args[] = {
    { "sck_gpio",    ARG_U8,  true, 0, 255 },
    { "mosi_gpio",   ARG_U8,  true, 0, 255 },
    { "miso_gpio",   ARG_U8,  true, 0, 255 },
    { "cs_gpio",     ARG_U8,  true, 0, 255 },
    { "frequency_hz", ARG_U32, true, 100, 40000000 },
    { "mode",        ARG_U8,  true, 0, 3 },
};
static const ArgSpec s_ext_spi_setup_rsp[] = {
    { "sck_gpio",    ARG_U8,  true, 0, 0 },
    { "mosi_gpio",   ARG_U8,  true, 0, 0 },
    { "miso_gpio",   ARG_U8,  true, 0, 0 },
    { "cs_gpio",     ARG_U8,  true, 0, 0 },
    { "frequency_hz", ARG_U32, true, 0, 0 },
    { "mode",        ARG_U8,  true, 0, 0 },
};

static const ArgSpec s_ext_job_get_args[] = {
    { "job_id", ARG_U32, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_ext_bus_cmds[] = {
    { BBP_CMD_EXT_I2C_SETUP,      "ext_i2c_setup",
      s_ext_i2c_setup_args,  4, s_ext_i2c_setup_rsp,  4, handler_ext_i2c_setup,      0                   },
    { BBP_CMD_EXT_I2C_SCAN,       "ext_i2c_scan",
      s_ext_i2c_scan_args,   4, NULL,                  0, handler_ext_i2c_scan,       0                   },
    { BBP_CMD_EXT_I2C_WRITE,      "ext_i2c_write",
      NULL,                  0, NULL,                  0, handler_ext_i2c_write,      0                   },
    { BBP_CMD_EXT_I2C_READ,       "ext_i2c_read",
      s_ext_i2c_read_args,   3, NULL,                  0, handler_ext_i2c_read,       0                   },
    { BBP_CMD_EXT_I2C_WRITE_READ, "ext_i2c_write_read",
      NULL,                  0, NULL,                  0, handler_ext_i2c_write_read, 0                   },
    { BBP_CMD_EXT_SPI_SETUP,      "ext_spi_setup",
      s_ext_spi_setup_args,  6, s_ext_spi_setup_rsp,  6, handler_ext_spi_setup,      0                   },
    { BBP_CMD_EXT_SPI_TRANSFER,   "ext_spi_transfer",
      NULL,                  0, NULL,                  0, handler_ext_spi_transfer,   0                   },
    { BBP_CMD_EXT_JOB_SUBMIT,     "ext_job_submit",
      NULL,                  0, NULL,                  0, handler_ext_job_submit,     0                   },
    { BBP_CMD_EXT_JOB_GET,        "ext_job_get",
      s_ext_job_get_args,    1, NULL,                  0, handler_ext_job_get,        CMD_FLAG_READS_STATE },
};

extern "C" void register_cmds_ext_bus(void)
{
    cmd_registry_register_block(s_ext_bus_cmds,
        sizeof(s_ext_bus_cmds) / sizeof(s_ext_bus_cmds[0]));
}
