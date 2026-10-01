"""BUS-012 (firmware half): the BBP I2C handlers passed any u8 address to the
driver. The ArgSpec says 0..127 but nothing enforces ArgSpec ranges, so an
8-bit address (0xA0) went out on the bus. Every I2C entry point must reject
addr > 0x7F with BAD_ARG / OUT_OF_RANGE before touching the driver."""

from tests.firmware_host.fwhost import compile_and_run, extract_function

CMD = "Firmware/ESP32/src/bbp/cmds/cmd_ext_bus.cpp"
HANDLERS = ("handler_ext_i2c_write", "handler_ext_i2c_read",
            "handler_ext_i2c_write_read", "handler_ext_job_submit")

PRELUDE = r"""
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define BBP_MAX_PAYLOAD 1024
enum { CMD_OK = 0, CMD_ERR_BAD_ARG = 1, CMD_ERR_OUT_OF_RANGE = 2, CMD_ERR_INVALID_STATE = 5,
       CMD_ERR_TIMEOUT = 6 };
enum { EXT_BUS_JOB_I2C_READ = 1, EXT_BUS_JOB_I2C_WRITE_READ = 2, EXT_BUS_JOB_SPI_TRANSFER = 3 };
static int g_driver_calls = 0;
static uint8_t bbp_get_u8(const uint8_t *b, size_t *p) { return b[(*p)++]; }
static uint16_t bbp_get_u16(const uint8_t *b, size_t *p) { uint16_t v = b[*p] | (b[*p + 1] << 8); *p += 2; return v; }
static void bbp_put_u32(uint8_t *b, size_t *p, uint32_t v) { for (int i = 0; i < 4; i++) b[(*p)++] = v >> (8 * i); }
static bool ext_i2c_ready(void) { return true; }
static bool ext_i2c_write(uint8_t, const uint8_t *, size_t, uint16_t) { g_driver_calls++; return true; }
static bool ext_i2c_read(uint8_t, uint8_t *, size_t, uint16_t) { g_driver_calls++; return true; }
static bool ext_i2c_write_read(uint8_t, const uint8_t *, size_t, uint8_t *, size_t, uint16_t) { g_driver_calls++; return true; }
static bool ext_job_submit_i2c_read(uint8_t, uint8_t, uint16_t, uint32_t *id) { g_driver_calls++; *id = 1; return true; }
static bool ext_job_submit_i2c_write_read(uint8_t, const uint8_t *, uint8_t, uint8_t, uint16_t, uint32_t *id) { g_driver_calls++; *id = 1; return true; }
static bool ext_job_submit_spi_transfer(const uint8_t *, uint16_t, uint16_t, uint32_t *id) { *id = 1; return true; }
"""

MAIN = r"""
int main(void) {
    static uint8_t rsp[1100]; size_t rl;
    uint8_t wr[]   = { ADDR, 100, 0, 1, 0x55 };
    uint8_t rd[]   = { ADDR, 100, 0, 1 };
    uint8_t wrrd[] = { ADDR, 100, 0, 1, 1, 0x55 };
    uint8_t jr[]   = { 1, 100, 0, ADDR, 1 };
    uint8_t jwr[]  = { 2, 100, 0, ADDR, 1, 1, 0x55 };
    int r[5];
    r[0] = handler_ext_i2c_write(wr, sizeof wr, rsp, &rl);
    r[1] = handler_ext_i2c_read(rd, sizeof rd, rsp, &rl);
    r[2] = handler_ext_i2c_write_read(wrrd, sizeof wrrd, rsp, &rl);
    r[3] = handler_ext_job_submit(jr, sizeof jr, rsp, &rl);
    r[4] = handler_ext_job_submit(jwr, sizeof jwr, rsp, &rl);
    printf("%d %d %d %d %d calls=%d\n", r[0] < 0, r[1] < 0, r[2] < 0, r[3] < 0, r[4] < 0, g_driver_calls);
    return 0;
}
"""


def _run(addr: int) -> str:
    src = PRELUDE + "".join(extract_function(CMD, rf"^static int {h}\(") for h in HANDLERS)
    return compile_and_run(src + MAIN.replace("ADDR", str(addr)), cxx=True)


def test_seven_bit_address_reaches_the_driver():
    assert _run(0x50).strip() == "0 0 0 0 0 calls=5"


def test_eight_bit_address_is_rejected_everywhere():
    assert _run(0xA0).strip() == "1 1 1 1 1 calls=0"
