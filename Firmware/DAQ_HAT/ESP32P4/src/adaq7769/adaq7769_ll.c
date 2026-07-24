// =============================================================================
// adaq7769_ll.c — Low-level SPI transport for the ADAQ7769-1
// =============================================================================

#include "adaq7769_ll.h"
#include <string.h>
#include "esp_log.h"
#include "hal/spi_ll.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/spi_periph.h"
#include "config.h"

static const char *TAG = "adaq_ll";

// SPI Mode 3: SCLK idles high, data clocked out on falling edge, sampled on
// rising edge (datasheet "Digital Interface").
#define ADAQ_SPI_MODE   3

// -----------------------------------------------------------------------------
// CRC-8: x^8 + x^2 + x + 1  (0x07), MSB-first.
// -----------------------------------------------------------------------------
uint8_t adaq_ll_crc8(const uint8_t *data, size_t len, uint8_t init)
{
    uint8_t crc = init;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80u) {
                crc = (uint8_t)((crc << 1) ^ ADAQ_CRC8_POLY);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

void adaq_ll_set_crc(adaq_ll_t *ll, bool enabled, bool xor_mode)
{
    ll->crc_enabled = enabled;
    ll->crc_xor     = xor_mode;
}

// -----------------------------------------------------------------------------
// Bus / device lifecycle
// -----------------------------------------------------------------------------
esp_err_t adaq_ll_bus_init(spi_host_device_t host,
                           gpio_num_t sclk, gpio_num_t mosi, gpio_num_t miso,
                           int max_xfer_bytes)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num     = mosi,
        .miso_io_num     = miso,
        .sclk_io_num     = sclk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = max_xfer_bytes,
    };
    // DMA DISABLED on purpose: every ADAQ transfer is tiny (register frames <=4
    // bytes, continuous-read samples 3..5 bytes), all well within the SPI FIFO.
    // With SPI_DMA_CH_AUTO the driver builds a DMA descriptor + does cache
    // writeback/invalidate per transfer, which measured ~30 us of fixed
    // overhead per read on the ESP32-P4 (capping capture at ~31k reads/s). The
    // CPU-FIFO path skips all of that for these small words.
    esp_err_t err = spi_bus_initialize(host, &buscfg, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize(host=%d) failed: %s", host, esp_err_to_name(err));
    }
    return err;
}

esp_err_t adaq_ll_add_device(adaq_ll_t *ll, spi_host_device_t host,
                             gpio_num_t cs_pin, uint32_t cfg_hz, uint32_t data_hz)
{
    memset(ll, 0, sizeof(*ll));
    ll->host    = host;
    ll->cs_pin  = cs_pin;
    ll->cfg_hz  = cfg_hz;
    ll->data_hz = data_hz;

    // Config handle: register access in FULL-DUPLEX (SPI Mode 3). The 8-bit
    // instruction goes out in the command phase; the data byte is exchanged in
    // the data phase. Half-duplex was previously used, but ESP-IDF half-duplex
    // reads need dummy clocks to place the MISO sample point, and NO_DUMMY (set
    // to preserve the ADAQ's tight instruction->data framing) left the final
    // read bit sampled too late -> the register LSB was dropped (reads came
    // back as value & 0xFE, e.g. CHIP_TYPE 0x07->0x06). Full-duplex inserts no
    // dummy clocks and matches the proven ADC-data readout path; reads clock a
    // trailing dummy byte so the real data byte is never the marginal last bit
    // before CS deasserts.
    spi_device_interface_config_t cfg = {
        .command_bits      = 8,
        .mode              = ADAQ_SPI_MODE,
        .clock_speed_hz    = (int)cfg_hz,
        .spics_io_num      = cs_pin,
        .queue_size        = 1,
        .input_delay_ns    = ADAQ_SPI_INPUT_DELAY_NS,
        // Hold CS low a few SCLK cycles after the last data bit. A normal
        // register read frames the data LSB (D0) on the final rising edge, and
        // the ADAQ holds D0 only until CS rises. Without this, HW CS deasserts
        // right at that edge and the ESP32-P4's (slightly late) final MISO
        // sample catches the reset instead of D0 -> LSB dropped (value & 0xFE).
        .cs_ena_posttrans  = ADAQ_CS_POSTTRANS_CYCLES,
        .flags             = 0,
    };
    esp_err_t err = spi_bus_add_device(host, &cfg, &ll->dev_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add cfg device (cs=%d) failed: %s", cs_pin, esp_err_to_name(err));
        return err;
    }

    // Data handle: fast clock, deeper queue for the streaming layer.
    // IMPORTANT: it must NOT own the hardware CS pin. Two devices sharing one
    // spics_io_num make the GPIO matrix route the physical CS to whichever
    // device was added last (here dev_data) — so dev_cfg's register-access CS
    // never reaches the pin and CS sits idle-high forever (device never
    // selected -> all-zero reads). Give CS to dev_cfg only; the streaming path
    // drives CS via 3-wire/manual control.
    spi_device_interface_config_t datcfg = {
        .mode           = ADAQ_SPI_MODE,
        .clock_speed_hz = (int)data_hz,
        .spics_io_num   = -1,          // no HW CS (dev_cfg owns the CS pin)
        .queue_size     = 8,
        .input_delay_ns = ADAQ_SPI_INPUT_DELAY_NS,
        .flags          = 0,
    };
    err = spi_bus_add_device(host, &datcfg, &ll->dev_data);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add data device (cs=%d) failed: %s", cs_pin, esp_err_to_name(err));
        spi_bus_remove_device(ll->dev_cfg);
        ll->dev_cfg = NULL;
        return err;
    }
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Runtime SPI-mode reconfiguration (bring-up aid). Removes and re-adds both
// device handles at the requested SPI mode (0..3). Lets the bench CLI sweep
// CPOL/CPHA without reflashing when the front-end is silent.
// -----------------------------------------------------------------------------
esp_err_t adaq_ll_set_mode(adaq_ll_t *ll, uint8_t mode)
{
    if (ll->dev_cfg)  { spi_bus_remove_device(ll->dev_cfg);  ll->dev_cfg  = NULL; }
    if (ll->dev_data) { spi_bus_remove_device(ll->dev_data); ll->dev_data = NULL; }

    spi_device_interface_config_t cfg = {
        .command_bits   = 8,
        .mode           = mode,
        .clock_speed_hz = (int)ll->cfg_hz,
        .spics_io_num   = ll->cs_pin,
        .queue_size     = 1,
        .input_delay_ns = ADAQ_SPI_INPUT_DELAY_NS,
        .flags          = 0,
    };
    esp_err_t err = spi_bus_add_device(ll->host, &cfg, &ll->dev_cfg);
    if (err != ESP_OK) return err;

    spi_device_interface_config_t datcfg = {
        .mode           = mode,
        .clock_speed_hz = (int)ll->data_hz,
        .spics_io_num   = -1,          // no HW CS (dev_cfg owns the CS pin)
        .queue_size     = 8,
        .input_delay_ns = ADAQ_SPI_INPUT_DELAY_NS,
        .flags          = 0,
    };
    return spi_bus_add_device(ll->host, &datcfg, &ll->dev_data);
}

// -----------------------------------------------------------------------------
// Runtime timing reconfiguration (bring-up aid). Re-creates both handles at the
// given SPI mode, register clock, and MISO input delay. Lets the bench CLI
// sweep input_delay_ns / cfg clock to place the sample point correctly without
// reflashing. cfg_hz/data_hz of 0 keep the current values.
// -----------------------------------------------------------------------------
esp_err_t adaq_ll_reconfig(adaq_ll_t *ll, uint8_t mode, uint32_t cfg_hz,
                           int input_delay_ns)
{
    if (cfg_hz) ll->cfg_hz = cfg_hz;
    if (ll->dev_cfg)  { spi_bus_remove_device(ll->dev_cfg);  ll->dev_cfg  = NULL; }
    if (ll->dev_data) { spi_bus_remove_device(ll->dev_data); ll->dev_data = NULL; }

    spi_device_interface_config_t cfg = {
        .command_bits      = 8,
        .mode              = mode,
        .clock_speed_hz    = (int)ll->cfg_hz,
        .spics_io_num      = ll->cs_pin,
        .queue_size        = 1,
        .input_delay_ns    = input_delay_ns,
        .cs_ena_posttrans  = ADAQ_CS_POSTTRANS_CYCLES,
        .flags             = 0,
    };
    esp_err_t err = spi_bus_add_device(ll->host, &cfg, &ll->dev_cfg);
    if (err != ESP_OK) return err;

    spi_device_interface_config_t datcfg = {
        .mode           = mode,
        .clock_speed_hz = (int)ll->data_hz,
        .spics_io_num   = -1,          // no HW CS (dev_cfg owns the CS pin)
        .queue_size     = 8,
        .input_delay_ns = input_delay_ns,
        .flags          = 0,
    };
    return spi_bus_add_device(ll->host, &datcfg, &ll->dev_data);
}

// -----------------------------------------------------------------------------
// Register access
// -----------------------------------------------------------------------------
esp_err_t adaq_ll_write_reg(adaq_ll_t *ll, uint8_t addr, uint8_t val)
{
    // Full-duplex: instruction in the command phase, data byte in the data
    // phase (CS stays low across both; MISO ignored).
    spi_transaction_t t = {0};
    t.flags   = SPI_TRANS_USE_TXDATA;
    t.cmd     = ADAQ_INSTR_WRITE(addr);
    t.length  = 8;
    t.tx_data[0] = val;
    if (ll->crc_enabled) {
        uint8_t frame[2] = { ADAQ_INSTR_WRITE(addr), val };
        t.tx_data[1] = adaq_ll_crc8(frame, 2, 0x00);
        t.length = 16;
    }
    return spi_device_polling_transmit(ll->dev_cfg, &t);
}

esp_err_t adaq_ll_read_reg(adaq_ll_t *ll, uint8_t addr, uint8_t *val)
{
    // Full-duplex, SEAMLESS byte buffer, clocking EXACTLY the frame the ADAQ
    // expects: 16 SCLK for a plain read (8 instruction + 8 data), 24 SCLK with
    // CRC (+8). The ADAQ resets DOUT at its expected frame length, so any
    // trailing clocks beyond the frame drive DOUT to 0 right as the data LSB
    // (D0, the 16th SCLK) is sampled — which dropped the LSB (value & 0xFE).
    // Clocking exactly to the frame end leaves D0 as the final bit, held until
    // CS rises, so it is captured. Instruction is tx[0]; data lands in rx[1].
    WORD_ALIGNED_ATTR uint8_t tx[4] = {0};
    WORD_ALIGNED_ATTR uint8_t rx[4] = {0};
    tx[0] = ADAQ_INSTR_READ(addr);
    size_t nbytes = ll->crc_enabled ? 3 : 2;   // instr + data [+ crc], no trailing

    spi_transaction_ext_t te = {0};
    te.base.flags     = SPI_TRANS_VARIABLE_CMD;   // command_bits = 0 (raw stream)
    te.command_bits   = 0;
    te.base.length    = nbytes * 8;
    te.base.rxlength  = nbytes * 8;
    te.base.tx_buffer = tx;
    te.base.rx_buffer = rx;
    esp_err_t err = spi_device_polling_transmit(ll->dev_cfg, (spi_transaction_t *)&te);
    if (err != ESP_OK) {
        return err;
    }
    if (ll->crc_enabled) {
        // instr, data(rx[1]), crc(rx[2]). The CRC byte is the final frame byte,
        // so the ESP32-P4 drops ITS lsb the same way a plain read dropped the
        // data lsb -> compare with bit0 masked (still a 7-bit integrity check).
        uint8_t crc_rx = rx[2];
        uint8_t frame[2] = { tx[0], rx[1] };
        uint8_t expect = ll->crc_xor
                             ? (uint8_t)(frame[0] ^ frame[1])
                             : adaq_ll_crc8(frame, 2, 0x00);
        if (((expect ^ crc_rx) & 0xFEu) != 0) {
            ESP_LOGW(TAG, "reg 0x%02X CRC mismatch (got 0x%02X want 0x%02X)",
                     addr, crc_rx, expect);
            return ESP_ERR_INVALID_CRC;
        }
    }
    *val = rx[1];
    return ESP_OK;
}

// Bring-up diagnostic: read instr + TWO data bytes so we can see where the
// register value (and its dropped LSB) actually lands across the byte boundary.
esp_err_t adaq_ll_read_reg2(adaq_ll_t *ll, uint8_t addr, uint8_t *b1, uint8_t *b2)
{
    WORD_ALIGNED_ATTR uint8_t tx[4] = {0};
    WORD_ALIGNED_ATTR uint8_t rx[4] = {0};
    tx[0] = ADAQ_INSTR_READ(addr);
    spi_transaction_ext_t te = {0};
    te.base.flags     = SPI_TRANS_VARIABLE_CMD;
    te.command_bits   = 0;
    te.base.length    = 4 * 8;   // instr + two data bytes + trailing dummy
    te.base.rxlength  = 4 * 8;
    te.base.tx_buffer = tx;
    te.base.rx_buffer = rx;
    esp_err_t err = spi_device_polling_transmit(ll->dev_cfg, (spi_transaction_t *)&te);
    if (err != ESP_OK) return err;
    *b1 = rx[1];
    *b2 = rx[2];
    return ESP_OK;
}

esp_err_t adaq_ll_read_adc24(adaq_ll_t *ll, int32_t *sample)
{
    // Word-aligned buffers keep the SPI DMA happy (unaligned stack buffers can
    // silently read back zeros on ESP32-P4).
    WORD_ALIGNED_ATTR uint8_t tx[8] = {0};
    WORD_ALIGNED_ATTR uint8_t rx[8] = {0};
    uint8_t n = 4;                 // instruction + 3 data bytes
    tx[0] = ADAQ_INSTR_READ(ADAQ_REG_ADC_DATA);
    if (ll->crc_enabled) {
        n = 5;
    }
    // Use the register-access handle (dev_cfg): it owns the hardware CS. The
    // streaming data handle (dev_data) has spics_io_num = -1, so a single-shot
    // read on it leaves CS deasserted and returns all-zeros. Send zero command
    // bits (raw byte stream) via the extended transaction, like read_reg. The
    // fast streaming path uses the FIFO route (with manual CS), not this.
    spi_transaction_ext_t te = {0};
    te.base.flags     = SPI_TRANS_VARIABLE_CMD;
    te.command_bits   = 0;
    te.base.length    = (size_t)n * 8;
    te.base.rxlength  = (size_t)n * 8;
    te.base.tx_buffer = tx;
    te.base.rx_buffer = rx;
    esp_err_t err = spi_device_polling_transmit(ll->dev_cfg, (spi_transaction_t *)&te);
    if (err != ESP_OK) {
        return err;
    }
    if (ll->crc_enabled) {
        uint8_t frame[4] = { tx[0], rx[1], rx[2], rx[3] };
        uint8_t expect = ll->crc_xor
                             ? (uint8_t)(frame[0] ^ frame[1] ^ frame[2] ^ frame[3])
                             : adaq_ll_crc8(frame, 4, 0x00);
        // CRC is the final frame byte, so the ESP32-P4 drops its lsb: mask bit0.
        if (((expect ^ rx[4]) & 0xFEu) != 0) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    uint32_t raw = ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | rx[3];
    *sample = adaq_sign_extend24(raw);
    return ESP_OK;
}

esp_err_t adaq_ll_write_raw(adaq_ll_t *ll, const uint8_t *tx, size_t len)
{
    // dev_cfg has command_bits=8; a raw byte stream must send ZERO command
    // bits, so use the extended transaction with SPI_TRANS_VARIABLE_CMD.
    spi_transaction_ext_t te = {0};
    te.base.flags   = SPI_TRANS_VARIABLE_CMD;
    te.command_bits = 0;
    te.base.length  = len * 8;
    te.base.tx_buffer = tx;
    return spi_device_polling_transmit(ll->dev_cfg, (spi_transaction_t *)&te);
}

esp_err_t adaq_ll_contread_word(adaq_ll_t *ll, uint8_t *rx, size_t len_bytes)
{
    spi_transaction_t t = {0};
    t.length    = len_bytes * 8;
    t.rxlength  = len_bytes * 8;
    t.tx_buffer = NULL;
    t.rx_buffer = rx;
    return spi_device_polling_transmit(ll->dev_data, &t);
}

// Hold the SPI bus across a whole streaming session so each contread skips the
// per-transaction bus-lock acquire (which otherwise costs ~100 us/read on the
// ESP32-P4). Only valid for a bus with a SINGLE active device — while held, no
// other device on the same host can transact.
esp_err_t adaq_ll_bus_acquire(adaq_ll_t *ll)
{
    return spi_device_acquire_bus(ll->dev_data, portMAX_DELAY);
}

void adaq_ll_bus_release(adaq_ll_t *ll)
{
    spi_device_release_bus(ll->dev_data);
}

// -----------------------------------------------------------------------------
// Direct SPI-FIFO fast read path.
//
// spi_device_polling_transmit() re-derives and re-applies the peripheral config
// on every call (~9 us of fixed overhead on the ESP32-P4 for a 4-byte word,
// dwarfing the ~1.6 us transfer). For continuous-read streaming the frame is a
// fixed-length MISO-only read, so we configure the peripheral ONCE and then
// just trigger + poll + drain the RX FIFO per sample. The bus must be held
// (adaq_ll_bus_acquire) and a normal transfer primed first so the clock/mode
// registers are already set — we only override the phase/bit-length here.
// -----------------------------------------------------------------------------
void adaq_ll_fifo_setup(adaq_ll_t *ll, size_t n_bytes)
{
    spi_dev_t *hw = SPI_LL_GET_HW(ll->host);
    spi_ll_set_command_bitlen(hw, 0);
    spi_ll_set_addr_bitlen(hw, 0);
    spi_ll_set_dummy(hw, 0);
    spi_ll_set_mosi_bitlen(hw, 0);
    spi_ll_set_miso_bitlen(hw, n_bytes * 8);
    spi_ll_enable_mosi(hw, 0);
    spi_ll_enable_miso(hw, 1);
    spi_ll_apply_config(hw);
}

esp_err_t adaq_ll_fifo_read(adaq_ll_t *ll, uint8_t *rx, size_t n_bytes)
{
    adaq_ll_fifo_start(ll);
    adaq_ll_fifo_wait(ll);
    adaq_ll_fifo_drain(ll, rx, n_bytes);
    return ESP_OK;
}

// Split-phase FIFO read for overlapping two SPI peripherals. Trigger both hosts
// (adaq_ll_fifo_start) so their SCLKs clock CONCURRENTLY, then wait+drain each.
// FINE lives on SPI3 and COARSE/VOLTAGE on SPI2, so a FINE+COARSE pair costs one
// ~1.2 us SCLK instead of two — the single capture core can then sustain
// 256 kSPS on both channels.
void adaq_ll_fifo_start(adaq_ll_t *ll)
{
    spi_ll_user_start(SPI_LL_GET_HW(ll->host));
}

void adaq_ll_fifo_wait(adaq_ll_t *ll)
{
    spi_dev_t *hw = SPI_LL_GET_HW(ll->host);
    while (spi_ll_get_running_cmd(hw)) { /* poll ~1.2 us (or ~0 if overlapped) */ }
}

void adaq_ll_fifo_drain(adaq_ll_t *ll, uint8_t *rx, size_t n_bytes)
{
    spi_ll_read_buffer(SPI_LL_GET_HW(ll->host), rx, n_bytes * 8);
}

// -----------------------------------------------------------------------------
// Manual chip-select for the streaming / continuous-read fast path.
//
// The streaming reads go through adaq_ll_fifo_read() (spi_ll_user_start), which
// drives NO chip-select, and the fast data handle (dev_data) owns no hardware CS
// — dev_cfg holds the CS pin for register access. So during a streaming session
// nothing pulls CS low: the ADAQ is never selected, DOUT stays high-Z, and every
// sample reads back 0x000000. To fix that we temporarily take the CS pin away
// from the SPI peripheral and drive it directly as a GPIO: idle high, asserted
// low around each FIFO read (adaq_ll_cs_assert/deassert). On a shared bus this
// also multiplexes the two devices — only the one being read is selected.
//
// adaq_ll_cs_manual_end() hands the pin back to the SPI peripheral's CS0 output
// so dev_cfg register access works again after the stream stops.
void adaq_ll_cs_manual_begin(adaq_ll_t *ll)
{
    gpio_set_direction(ll->cs_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(ll->cs_pin, 1);   // idle high before takeover
    esp_rom_gpio_connect_out_signal(ll->cs_pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_level(ll->cs_pin, 1);
}

// -----------------------------------------------------------------------------
// Hardware CS for the streaming fast path (bus A / FINE only). See ADAQ_HW_CS
// in config.h for full history/rationale. Unlike cs_manual_begin(), the CS pin
// is NEVER taken over as a GPIO -- it stays routed to the SPI peripheral's own
// CS output (already wired there by adaq_ll_add_device()'s dev_cfg
// spics_io_num=cs_pin). The peripheral then asserts/deasserts CS itself around
// each spi_ll_user_start() transaction, with an explicit CS-hold phase
// (ADAQ_HW_CS_HOLD_CYCLES SCLK cycles) keeping it low after the final data bit
// before it rises -- the same posttrans-hold fix already proven safe for
// register access (cs_ena_posttrans / ADAQ_CS_POSTTRANS_CYCLES above).
//
// select_cs(hw, 0) is safe specifically because bus A has exactly one CS-
// owning device (dev_cfg of the sole FINE ADAQ) registered on that host, so
// the SPI driver can only have assigned it hw CS id 0 -- there is no second
// device to contend for a different id. Do NOT reuse this on bus B (two CS-
// owning devices) without first confirming each device's actual hw CS id.
//
// BUG FOUND (baseline-cal investigation): spi_bus_remove_device() frees the
// device's CS GPIO via spicommon_cs_free_io() -> gpio_reset_pin(), which tears
// down the GPIO-matrix (or IOMUX) routing that connected cs_pin to this host's
// CS0 output signal. The comment above used to claim "the pin itself is left
// alone -- still routed to the SPI peripheral's CS output"; that is false --
// after the remove_device() call the pin is a bare reset GPIO (floating
// input), so every spi_ll_master_select_cs()/spi_ll_apply_config() below only
// toggles the peripheral's *internal* CS0 register -- nothing ever reaches the
// ADAQ. The chip then never sees a real CS transition, so its continuous-read
// shift register never advances past whatever it last latched: FINE reads
// back one frozen 24-bit code forever (proven live: fine_v was bit-identical
// across >6000 consecutive samples while coarse_v, on the same board, jittered
// normally sample to sample as a live ADC should).
//
// Fix: re-establish the CS-pin -> SPI-host GPIO-matrix routing ourselves right
// after remove_device() tears it down, mirroring what spicommon_cs_initialize()
// does for cs_num==0 (see esp_driver_spi/src/gpspi/spi_common.c). Force the
// GPIO-matrix path (not IOMUX) unconditionally -- simpler and correct either
// way, at the cost of the ~UI ns matrix-routing delay IOMUX would have saved.
#if ADAQ_HW_CS
void adaq_ll_hwcs_begin(adaq_ll_t *ll)
{
    // Release dev_cfg's grip on the peripheral (mirrors cs_manual_begin()) so
    // the raw spi_ll_* fifo path has sole use of it during streaming. This
    // also resets cs_pin to a bare GPIO (see BUG FOUND above) -- restore its
    // routing to this host's CS0 output signal before relying on it.
    if (ll->dev_cfg) {
        spi_bus_remove_device(ll->dev_cfg);
        ll->dev_cfg = NULL;
    }
    gpio_set_direction(ll->cs_pin, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(ll->cs_pin,
                                    spi_periph_signal[ll->host].spics_out[0],
                                    false, false);

    spi_dev_t *hw = SPI_LL_GET_HW(ll->host);
    spi_ll_master_select_cs(hw, 0);              // bus A: FINE is the sole CS on this host
    spi_ll_master_keep_cs(hw, 0);                // auto-deassert after each transaction
    spi_ll_master_set_cs_setup(hw, 1);           // minimal setup phase before first edge
    spi_ll_master_set_cs_hold(hw, ADAQ_HW_CS_HOLD_CYCLES);  // hold after last edge
    spi_ll_apply_config(hw);
    ll->hw_cs_streaming = true;
}

void adaq_ll_hwcs_end(adaq_ll_t *ll)
{
    ll->hw_cs_streaming = false;
    // Same rebuild as cs_manual_end(): re-create dev_cfg so normal register
    // access (spi_device_polling_transmit) works after the stream stops. No
    // gpio_set_level() needed -- the pin was never taken over as a GPIO.
    spi_device_interface_config_t cfg = {
        .command_bits      = 8,
        .mode              = ADAQ_SPI_MODE,
        .clock_speed_hz    = (int)ll->cfg_hz,
        .spics_io_num      = ll->cs_pin,
        .queue_size        = 1,
        .input_delay_ns    = ADAQ_SPI_INPUT_DELAY_NS,
        .cs_ena_posttrans  = ADAQ_CS_POSTTRANS_CYCLES,
        .flags             = 0,
    };
    esp_err_t err = spi_bus_add_device(ll->host, &cfg, &ll->dev_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hwcs_end: re-add dev_cfg (cs=%d) failed: %s",
                 ll->cs_pin, esp_err_to_name(err));
    }
}
#endif // ADAQ_HW_CS

void adaq_ll_cs_manual_end(adaq_ll_t *ll)
{
    gpio_set_level(ll->cs_pin, 1);   // leave deasserted
    // Rebuild the register-access (dev_cfg) handle so the SPI driver re-routes
    // the CS pin to this host's chip-select exactly as it did at init. This is
    // host-agnostic: the earlier esp_rom_gpio reconnect had to guess the CS
    // line's output-signal index, which differs per SPI host and left Bus A
    // (SPI3) mis-routed -> FINE register writes silently failed after streaming.
    if (ll->dev_cfg) {
        spi_bus_remove_device(ll->dev_cfg);
        ll->dev_cfg = NULL;
    }
    spi_device_interface_config_t cfg = {
        .command_bits      = 8,
        .mode              = ADAQ_SPI_MODE,
        .clock_speed_hz    = (int)ll->cfg_hz,
        .spics_io_num      = ll->cs_pin,
        .queue_size        = 1,
        .input_delay_ns    = ADAQ_SPI_INPUT_DELAY_NS,
        .cs_ena_posttrans  = ADAQ_CS_POSTTRANS_CYCLES,
        .flags             = 0,
    };
    esp_err_t err = spi_bus_add_device(ll->host, &cfg, &ll->dev_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cs_manual_end: re-add dev_cfg (cs=%d) failed: %s",
                 ll->cs_pin, esp_err_to_name(err));
    }
}
