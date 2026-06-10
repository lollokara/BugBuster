import { snippetCompletion } from "@codemirror/autocomplete";
export const bugbusterCompletions = (context) => {
    // Match the word being typed, including dots for module access
    let word = context.matchBefore(/[\w.]*/);
    if (!word || (word.from === word.to && !context.explicit))
        return null;
    return {
        from: word.from,
        options: [
            // Modules
            { label: "bugbuster", type: "namespace" },
            { label: "bb_helpers", type: "namespace" },
            { label: "bb_devices", type: "namespace" },
            { label: "bb_logging", type: "namespace" },
            // bugbuster functions
            snippetCompletion("bugbuster.sleep(${ms})", { label: "bugbuster.sleep", detail: "cooperative sleep", type: "function" }),
            snippetCompletion("bugbuster.Channel(${id})", { label: "bugbuster.Channel", detail: "analog channel (0-3)", type: "class" }),
            snippetCompletion("bugbuster.I2C(sda_io=${sda}, scl_io=${scl}, freq=${400000})", { label: "bugbuster.I2C", detail: "I2C bus setup", type: "class" }),
            snippetCompletion("bugbuster.SPI(sck_io=${sck}, mosi_io=${mosi}, miso_io=${miso}, cs_io=${cs})", { label: "bugbuster.SPI", detail: "SPI bus setup", type: "class" }),
            snippetCompletion("bugbuster.http_get('${url}')", { label: "bugbuster.http_get", type: "function" }),
            snippetCompletion("bugbuster.http_post('${url}', body=${body})", { label: "bugbuster.http_post", type: "function" }),
            snippetCompletion("bugbuster.mqtt_publish(topic='${topic}', payload=${payload}, host='${host}')", { label: "bugbuster.mqtt_publish", type: "function" }),
            snippetCompletion("bugbuster.hat_status()", { label: "bugbuster.hat_status", detail: "HAT presence and health", type: "function" }),
            snippetCompletion("bugbuster.hat_caps()", { label: "bugbuster.hat_caps", detail: "HAT v2 capabilities", type: "function" }),
            snippetCompletion("bugbuster.hat_rails()", { label: "bugbuster.hat_rails", detail: "HAT rail status", type: "function" }),
            snippetCompletion("bugbuster.hat_set_rail_enable(${rail_id}, ${enable})", { label: "bugbuster.hat_set_rail_enable", type: "function" }),
            snippetCompletion("bugbuster.hat_set_rail_voltage(${rail_id}, ${voltage_mv})", { label: "bugbuster.hat_set_rail_voltage", type: "function" }),
            snippetCompletion("bugbuster.hat_led(${led_id}, ${color_code})", { label: "bugbuster.hat_led", type: "function" }),
            snippetCompletion("bugbuster.hat_io_bank(${dirs}, ups=${0}, dns=${0}, vals=${0})", { label: "bugbuster.hat_io_bank", type: "function" }),
            snippetCompletion("bugbuster.hat_level_shift(${oe}, ${dir})", { label: "bugbuster.hat_level_shift", type: "function" }),
            snippetCompletion("bugbuster.hat_calibrate_status()", { label: "bugbuster.hat_calibrate_status", type: "function" }),
            snippetCompletion("bugbuster.hat_calibrate_start(${rail_id})", { label: "bugbuster.hat_calibrate_start", type: "function" }),
            snippetCompletion("bugbuster.hat_calibrate_import(${rail_id}, ${points})", { label: "bugbuster.hat_calibrate_import", type: "function" }),
            snippetCompletion("bugbuster.hat_setup_swd(${3300}, connector=${0})", { label: "bugbuster.hat_setup_swd", type: "function" }),
            // Constants
            { label: "bugbuster.FUNC_HIGH_IMP", type: "constant" },
            { label: "bugbuster.FUNC_VOUT", type: "constant" },
            { label: "bugbuster.FUNC_IOUT", type: "constant" },
            { label: "bugbuster.FUNC_VIN", type: "constant" },
            { label: "bugbuster.FUNC_IIN_EXT_PWR", type: "constant" },
            { label: "bugbuster.FUNC_IIN_LOOP_PWR", type: "constant" },
            { label: "bugbuster.FUNC_RES_MEAS", type: "constant" },
            { label: "bugbuster.FUNC_DIN_LOGIC", type: "constant" },
            { label: "bugbuster.FUNC_DIN_LOOP", type: "constant" },
            { label: "bugbuster.FUNC_IOUT_HART", type: "constant" },
            { label: "bugbuster.FUNC_IIN_EXT_PWR_HART", type: "constant" },
            { label: "bugbuster.FUNC_IIN_LOOP_PWR_HART", type: "constant" },
            { label: "bugbuster.HAT_RAIL_3V3_ADJ", type: "constant" },
            { label: "bugbuster.HAT_RAIL_VADJ3", type: "constant" },
            { label: "bugbuster.HAT_RAIL_VADJ4", type: "constant" },
            { label: "bugbuster.HAT_LED_OFF", type: "constant" },
            { label: "bugbuster.HAT_LED_RED", type: "constant" },
            { label: "bugbuster.HAT_LED_GREEN", type: "constant" },
            { label: "bugbuster.HAT_LED_BLUE", type: "constant" },
            { label: "bugbuster.HAT_LED_YELLOW", type: "constant" },
            { label: "bugbuster.HAT_LED_CYAN", type: "constant" },
            { label: "bugbuster.HAT_LED_MAGENTA", type: "constant" },
            { label: "bugbuster.HAT_LED_WHITE", type: "constant" },
            // Methods (generic, will match after 'ch.' etc)
            snippetCompletion("set_function(${func})", { label: "set_function", type: "method" }),
            snippetCompletion("set_voltage(${v})", { label: "set_voltage", type: "method" }),
            snippetCompletion("read_voltage()", { label: "read_voltage", type: "method" }),
            snippetCompletion("set_do(${value})", { label: "set_do", type: "method" }),
            snippetCompletion("scan()", { label: "scan", type: "method" }),
            snippetCompletion("writeto(${addr}, ${data})", { label: "writeto", type: "method" }),
            snippetCompletion("readfrom(${addr}, ${n})", { label: "readfrom", type: "method" }),
            snippetCompletion("writeto_then_readfrom(${addr}, ${wr_buf}, ${rd_n})", { label: "writeto_then_readfrom", type: "method" }),
            snippetCompletion("transfer(${data})", { label: "transfer", type: "method" }),
            // bb_helpers
            snippetCompletion("bb_helpers.settle(${ms})", { label: "bb_helpers.settle", type: "function" }),
            snippetCompletion("bb_helpers.dac_ramp(channel=${ch}, lo=${0.0}, hi=${5.0}, step=${1.0})", { label: "bb_helpers.dac_ramp", type: "function" }),
            // bb_logging
            snippetCompletion("bb_logging.info('${msg}')", { label: "bb_logging.info", type: "function" }),
            snippetCompletion("bb_logging.warn('${msg}')", { label: "bb_logging.warn", type: "function" }),
            snippetCompletion("bb_logging.error('${msg}')", { label: "bb_logging.error", type: "function" }),
            // bb_devices
            snippetCompletion("bb_devices.TMP102(${i2c}, addr=${0x48})", { label: "bb_devices.TMP102", type: "class" }),
            snippetCompletion("bb_devices.BMP280(${i2c}, addr=${0x76})", { label: "bb_devices.BMP280", type: "class" }),
            snippetCompletion("bb_devices.MCP3008(${spi})", { label: "bb_devices.MCP3008", type: "class" }),
        ],
        validFor: /^[\w.]*$/,
    };
};
