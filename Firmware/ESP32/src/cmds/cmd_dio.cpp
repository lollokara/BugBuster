// =============================================================================
// cmd_dio.cpp — Registry handlers for Digital IO commands
//   BBP_CMD_DIO_GET_ALL  (0x43)
//   BBP_CMD_DIO_CONFIG   (0x44)
//   BBP_CMD_DIO_WRITE    (0x45)
//   BBP_CMD_DIO_READ     (0x46)
// =============================================================================
#include "../cmd_registry.h"
#include "../cmd_errors.h"
#include "../bbp_codec.h"
#include "../bbp.h"
#include "../dio.h"

// ---------------------------------------------------------------------------
// DIO_GET_ALL  payload: (none)
//   resp: u8 count, then for each IO: u8 ioNum(1-12), u8 gpioPin,
//         u8 mode, bool outputLevel, bool inputLevel
// Wire format matches legacy handleDioGetAll (bbp.cpp:964-980).
// Calls dio_poll_inputs() then dio_get_all() exactly as legacy does.
// ---------------------------------------------------------------------------
static int handler_dio_get_all(const uint8_t *payload, size_t len,
                                uint8_t *resp, size_t *resp_len)
{
    (void)payload; (void)len;

    const DioState *all = dio_get_all();
    dio_poll_inputs();  // refresh input levels before responding

    size_t pos = 0;
    bbp_put_u8(resp, &pos, DIO_NUM_IOS);
    for (int i = 0; i < DIO_NUM_IOS; i++) {
        bbp_put_u8(resp, &pos, (uint8_t)(i + 1));         // io number (1-12)
        bbp_put_u8(resp, &pos, all[i].gpio_num);           // ESP32 GPIO pin
        bbp_put_u8(resp, &pos, all[i].mode);               // 0=disabled, 1=input, 2=output
        bbp_put_bool(resp, &pos, all[i].output_level);     // last written output
        bbp_put_bool(resp, &pos, all[i].input_level);      // last read input
    }
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// DIO_CONFIG  payload: u8 io(1-12), u8 mode(0=dis/1=in/2=out)
//   resp: u8 io, u8 mode
// Wire format matches legacy handleDioConfig (bbp.cpp:982-1000).
// dio_configure() returns false for invalid io or mode.
// ---------------------------------------------------------------------------
static int handler_dio_config(const uint8_t *payload, size_t len,
                               uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t io   = bbp_get_u8(payload, &rpos);
    uint8_t mode = bbp_get_u8(payload, &rpos);

    if (!dio_configure(io, mode)) return -CMD_ERR_OUT_OF_RANGE;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, io);
    bbp_put_u8(resp, &pos, mode);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// DIO_WRITE  payload: u8 io(1-12), bool level
//   resp: u8 io, bool level
// Wire format matches legacy handleDioWrite (bbp.cpp:1002-1020).
// dio_write() returns false if io is not configured as output.
// ---------------------------------------------------------------------------
static int handler_dio_write(const uint8_t *payload, size_t len,
                              uint8_t *resp, size_t *resp_len)
{
    if (len < 2) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t io = bbp_get_u8(payload, &rpos);
    bool level = bbp_get_bool(payload, &rpos);

    if (!dio_write(io, level)) return -CMD_ERR_OUT_OF_RANGE;

    size_t pos = 0;
    bbp_put_u8(resp, &pos, io);
    bbp_put_bool(resp, &pos, level);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// DIO_READ  payload: u8 io(1-12)
//   resp: u8 io, u8 mode, bool level
// Wire format matches legacy handleDioRead (bbp.cpp:1022-1049).
// Reads live pin for inputs, echoes output_level for outputs.
// ---------------------------------------------------------------------------
static int handler_dio_read(const uint8_t *payload, size_t len,
                             uint8_t *resp, size_t *resp_len)
{
    if (len < 1) return -CMD_ERR_BAD_ARG;
    size_t rpos = 0;
    uint8_t io = bbp_get_u8(payload, &rpos);

    DioState st;
    if (!dio_get_state(io, &st)) return -CMD_ERR_OUT_OF_RANGE;

    bool level = false;
    if (st.mode == DIO_MODE_INPUT) {
        level = dio_read(io);
    } else if (st.mode == DIO_MODE_OUTPUT) {
        level = st.output_level;
    }

    size_t pos = 0;
    bbp_put_u8(resp, &pos, io);
    bbp_put_u8(resp, &pos, st.mode);
    bbp_put_bool(resp, &pos, level);
    *resp_len = pos;
    return (int)pos;
}

// ---------------------------------------------------------------------------
// ArgSpec tables
// ---------------------------------------------------------------------------
// dio_get_all: no args, variable-length response — rsp=NULL (not CLI-decodable)
static const ArgSpec s_dio_config_args[] = {
    { "io",   ARG_U8, true, 1, 12 },
    { "mode", ARG_U8, true, 0, 2 },
};
static const ArgSpec s_dio_config_rsp[] = {
    { "io",   ARG_U8, true, 0, 0 },
    { "mode", ARG_U8, true, 0, 0 },
};

static const ArgSpec s_dio_write_args[] = {
    { "io",    ARG_U8,   true, 1, 12 },
    { "level", ARG_BOOL, true, 0, 0 },
};
static const ArgSpec s_dio_write_rsp[] = {
    { "io",    ARG_U8,   true, 0, 0 },
    { "level", ARG_BOOL, true, 0, 0 },
};

static const ArgSpec s_dio_read_args[] = {
    { "io", ARG_U8, true, 1, 12 },
};
static const ArgSpec s_dio_read_rsp[] = {
    { "io",    ARG_U8,   true, 0, 0 },
    { "mode",  ARG_U8,   true, 0, 0 },
    { "level", ARG_BOOL, true, 0, 0 },
};

// ---------------------------------------------------------------------------
// Descriptor table
// ---------------------------------------------------------------------------
static const CmdDescriptor s_dio_cmds[] = {
    { BBP_CMD_DIO_GET_ALL, "dio_get_all",
      NULL,               0, NULL,             0, handler_dio_get_all, CMD_FLAG_READS_STATE },
    { BBP_CMD_DIO_CONFIG,  "dio_config",
      s_dio_config_args,  2, s_dio_config_rsp,  2, handler_dio_config,  0                   },
    { BBP_CMD_DIO_WRITE,   "dio_write",
      s_dio_write_args,   2, s_dio_write_rsp,   2, handler_dio_write,   0                   },
    { BBP_CMD_DIO_READ,    "dio_read",
      s_dio_read_args,    1, s_dio_read_rsp,    3, handler_dio_read,    CMD_FLAG_READS_STATE },
};

extern "C" void register_cmds_dio(void)
{
    cmd_registry_register_block(s_dio_cmds,
        sizeof(s_dio_cmds) / sizeof(s_dio_cmds[0]));
}
