import { CompletionSource, snippetCompletion } from "@codemirror/autocomplete";

export const bugbusterCompletions: CompletionSource = (context) => {
  // Match the word being typed, including dots for module access
  let word = context.matchBefore(/[\w.]*/);
  if (!word || (word.from === word.to && !context.explicit)) return null;

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
