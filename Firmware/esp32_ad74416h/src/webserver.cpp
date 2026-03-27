// =============================================================================
// webserver.cpp - HTTP API server for AD74416H controller
// =============================================================================

#include "webserver.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

#include "tasks.h"
#include "config.h"
#include "ad74416h.h"
#include "ad74416h_spi.h"

// -----------------------------------------------------------------------------
// Helper: CORS headers for development
// -----------------------------------------------------------------------------

static void addCorsHeaders(AsyncWebServerResponse* response)
{
    response->addHeader("Access-Control-Allow-Origin",  "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type");
}

// -----------------------------------------------------------------------------
// Helper: send a JSON document as an HTTP response
// -----------------------------------------------------------------------------

static void sendJson(AsyncWebServerRequest* request,
                     JsonDocument&          doc,
                     int                    code = 200)
{
    String body;
    serializeJson(doc, body);
    AsyncWebServerResponse* resp =
        request->beginResponse(code, "application/json", body);
    addCorsHeaders(resp);
    resp->addHeader("Cache-Control", "no-cache");
    request->send(resp);
}

// -----------------------------------------------------------------------------
// Helper: validate channel number extracted from URL
// -----------------------------------------------------------------------------

static bool validChannel(AsyncWebServerRequest* request, int& ch)
{
    ch = request->pathArg(0).toInt();
    if (ch < 0 || ch >= AD74416H_NUM_CHANNELS) {
        JsonDocument err;
        err["error"] = "Channel must be 0-3";
        sendJson(request, err, 400);
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Helper: parse a JSON body from a request (ArduinoJson v7)
// -----------------------------------------------------------------------------

static bool parseBody(AsyncWebServerRequest* request,
                      uint8_t*               data,
                      size_t                 len,
                      JsonDocument&          doc)
{
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        JsonDocument errDoc;
        errDoc["error"] = "Invalid JSON";
        sendJson(request, errDoc, 400);
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// channelFunctionToString
// -----------------------------------------------------------------------------

String channelFunctionToString(ChannelFunction f)
{
    switch (f) {
        case CH_FUNC_HIGH_IMP:          return "HIGH_IMP";
        case CH_FUNC_VOUT:              return "VOUT";
        case CH_FUNC_IOUT:              return "IOUT";
        case CH_FUNC_VIN:               return "VIN";
        case CH_FUNC_IIN_EXT_PWR:       return "IIN_EXT_PWR";
        case CH_FUNC_IIN_LOOP_PWR:      return "IIN_LOOP_PWR";
        case CH_FUNC_RES_MEAS:          return "RES_MEAS";
        case CH_FUNC_DIN_LOGIC:         return "DIN_LOGIC";
        case CH_FUNC_DIN_LOOP:          return "DIN_LOOP";
        case CH_FUNC_IOUT_HART:         return "IOUT_HART";
        case CH_FUNC_IIN_EXT_PWR_HART:  return "IIN_EXT_PWR_HART";
        case CH_FUNC_IIN_LOOP_PWR_HART: return "IIN_LOOP_PWR_HART";
        default:                        return "UNKNOWN";
    }
}

// -----------------------------------------------------------------------------
// Route handler implementations
// -----------------------------------------------------------------------------

// GET /
static void handleRoot(AsyncWebServerRequest* request)
{
    if (SPIFFS.exists("/index.html")) {
        AsyncWebServerResponse* resp =
            request->beginResponse(SPIFFS, "/index.html", "text/html");
        addCorsHeaders(resp);
        request->send(resp);
    } else {
        request->send(404, "text/plain", "index.html not found in SPIFFS");
    }
}

// GET /api/status
static void handleGetStatus(AsyncWebServerRequest* request)
{
    JsonDocument doc;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        doc["spiOk"]            = g_deviceState.spiOk;
        doc["dieTemp"]          = g_deviceState.dieTemperature;
        doc["alertStatus"]      = g_deviceState.alertStatus;
        doc["alertMask"]        = g_deviceState.alertMask;
        doc["supplyAlertStatus"]= g_deviceState.supplyAlertStatus;
        doc["supplyAlertMask"]  = g_deviceState.supplyAlertMask;
        doc["liveStatus"]       = g_deviceState.liveStatus;

        JsonArray channels = doc["channels"].to<JsonArray>();
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            const ChannelState& cs = g_deviceState.channels[ch];
            JsonObject obj = channels.add<JsonObject>();
            obj["id"]               = ch;
            obj["function"]         = channelFunctionToString(cs.function);
            obj["adcRaw"]           = cs.adcRawCode;
            obj["adcValue"]         = cs.adcValue;
            obj["adcRange"]         = (int)cs.adcRange;
            obj["adcRate"]          = (int)cs.adcRate;
            obj["adcMux"]           = (int)cs.adcMux;
            obj["dacCode"]          = cs.dacCode;
            obj["dacValue"]         = cs.dacValue;
            obj["dinState"]         = cs.dinState;
            obj["dinCounter"]       = cs.dinCounter;
            obj["doState"]          = cs.doState;
            obj["channelAlert"]     = cs.channelAlertStatus;
            obj["channelAlertMask"] = cs.channelAlertMask;
        }

        xSemaphoreGive(g_stateMutex);
    } else {
        JsonDocument err;
        err["error"] = "State mutex timeout";
        sendJson(request, err, 503);
        return;
    }

    sendJson(request, doc);
}

// GET /api/channel/{n}/adc
static void handleGetChannelAdc(AsyncWebServerRequest* request)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        const ChannelState& cs = g_deviceState.channels[ch];
        doc["id"]       = ch;
        doc["adcRaw"]   = cs.adcRawCode;
        doc["adcValue"] = cs.adcValue;
        doc["adcRange"] = (int)cs.adcRange;
        doc["adcRate"]  = (int)cs.adcRate;
        doc["adcMux"]   = (int)cs.adcMux;
        xSemaphoreGive(g_stateMutex);
    } else {
        JsonDocument err;
        err["error"] = "State mutex timeout";
        sendJson(request, err, 503);
        return;
    }

    sendJson(request, doc);
}

// GET /api/faults
static void handleGetFaults(AsyncWebServerRequest* request)
{
    JsonDocument doc;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        doc["alertStatus"]       = g_deviceState.alertStatus;
        doc["alertMask"]         = g_deviceState.alertMask;
        doc["supplyAlertStatus"] = g_deviceState.supplyAlertStatus;
        doc["supplyAlertMask"]   = g_deviceState.supplyAlertMask;

        JsonArray channels = doc["channels"].to<JsonArray>();
        for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
            JsonObject obj = channels.add<JsonObject>();
            obj["id"]               = ch;
            obj["channelAlert"]     = g_deviceState.channels[ch].channelAlertStatus;
            obj["channelAlertMask"] = g_deviceState.channels[ch].channelAlertMask;
        }
        xSemaphoreGive(g_stateMutex);
    } else {
        JsonDocument err;
        err["error"] = "State mutex timeout";
        sendJson(request, err, 503);
        return;
    }
    sendJson(request, doc);
}

// POST /api/channel/{n}/function
// Body: {"function": <int>}
static void handlePostChannelFunction(AsyncWebServerRequest* request,
                                      uint8_t* data, size_t len,
                                      size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    if (!doc["function"].is<int>()) {
        JsonDocument err; err["error"] = "Missing 'function' field";
        sendJson(request, err, 400); return;
    }

    Command cmd{};
    cmd.type    = CMD_SET_CHANNEL_FUNC;
    cmd.channel = (uint8_t)ch;
    cmd.func    = (ChannelFunction)doc["function"].as<int>();
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]       = true;
    resp["channel"]  = ch;
    resp["function"] = (int)cmd.func;
    sendJson(request, resp);
}

// POST /api/channel/{n}/dac
// Body: {"code": N} OR {"voltage": V, "bipolar": bool} OR {"current_mA": I}
static void handlePostChannelDac(AsyncWebServerRequest* request,
                                  uint8_t* data, size_t len,
                                  size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    Command cmd{};
    cmd.channel = (uint8_t)ch;

    if (doc["code"].is<int>()) {
        cmd.type    = CMD_SET_DAC_CODE;
        cmd.dacCode = (uint16_t)doc["code"].as<int>();
    } else if (doc["voltage"].is<float>() || doc["voltage"].is<int>()) {
        cmd.type     = CMD_SET_DAC_VOLTAGE;
        bool bipolar = doc["bipolar"] | false;
        float v      = doc["voltage"].as<float>();
        // Encode bipolar flag by negating voltage if bipolar and voltage >= 0
        // Better: store as-is; taskCommandProcessor uses sign check only for
        // negative voltages. Instead pass a dedicated encoding:
        // We store floatVal = voltage, and encode bipolar in a separate union
        // member is not possible. Use the sign convention: if bipolar is true
        // and voltage is non-negative, negate to signal bipolar. The task
        // will interpret any negative value as bipolar.
        // HOWEVER, for bipolar positive voltages this is ambiguous.
        // A cleaner approach: embed bipolar in the float by using NaN payload
        // is unsafe. Instead we send two commands: CMD_SET_VOUT_RANGE then
        // CMD_SET_DAC_VOLTAGE. The range is set first, voltage second.
        if (bipolar) {
            Command rangeCmd{};
            rangeCmd.type     = CMD_SET_VOUT_RANGE;
            rangeCmd.channel  = (uint8_t)ch;
            rangeCmd.boolVal  = true;
            sendCommand(rangeCmd);
            // For bipolar, negate positive values so task can detect bipolar
            // This works for negative target voltages; for positive bipolar
            // voltages the task simply calls setDacVoltage(ch, v, false).
            // To reliably pass bipolar=true, encode as a sufficiently large
            // negative sentinel: -(v + 1000) would break magnitude.
            // Simplest safe approach: always treat negative floatVal as bipolar.
            // Positive bipolar voltages: caller must set range separately.
            cmd.floatVal = (v >= 0.0f) ? -0.0001f - v : v;
        } else {
            cmd.floatVal = v;
        }
    } else if (doc["current_mA"].is<float>() || doc["current_mA"].is<int>()) {
        cmd.type     = CMD_SET_DAC_CURRENT;
        cmd.floatVal = doc["current_mA"].as<float>();
    } else {
        JsonDocument err;
        err["error"] = "Body must have 'code', 'voltage', or 'current_mA'";
        sendJson(request, err, 400);
        return;
    }

    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["channel"] = ch;
    sendJson(request, resp);
}

// POST /api/channel/{n}/adc/config
// Body: {"mux": 0, "range": 0, "rate": 1}
static void handlePostAdcConfig(AsyncWebServerRequest* request,
                                 uint8_t* data, size_t len,
                                 size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    Command cmd{};
    cmd.type           = CMD_ADC_CONFIG;
    cmd.channel        = (uint8_t)ch;
    cmd.adcCfg.mux     = (AdcConvMux)doc["mux"].as<int>();
    cmd.adcCfg.range   = (AdcRange)  doc["range"].as<int>();
    cmd.adcCfg.rate    = (AdcRate)   doc["rate"].as<int>();
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["channel"] = ch;
    sendJson(request, resp);
}

// POST /api/channel/{n}/din/config
static void handlePostDinConfig(AsyncWebServerRequest* request,
                                 uint8_t* data, size_t len,
                                 size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    Command cmd{};
    cmd.type              = CMD_DIN_CONFIG;
    cmd.channel           = (uint8_t)ch;
    cmd.dinCfg.thresh     = (uint8_t)doc["thresh"].as<int>();
    cmd.dinCfg.threshMode = doc["threshMode"] | false;
    cmd.dinCfg.debounce   = (uint8_t)doc["debounce"].as<int>();
    cmd.dinCfg.sink       = (uint8_t)doc["sink"].as<int>();
    cmd.dinCfg.sinkRange  = doc["sinkRange"] | false;
    cmd.dinCfg.ocDet      = doc["ocDet"] | false;
    cmd.dinCfg.scDet      = doc["scDet"] | false;
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["channel"] = ch;
    sendJson(request, resp);
}

// POST /api/channel/{n}/do/config
static void handlePostDoConfig(AsyncWebServerRequest* request,
                                uint8_t* data, size_t len,
                                size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    Command cmd{};
    cmd.type              = CMD_DO_CONFIG;
    cmd.channel           = (uint8_t)ch;
    cmd.doCfg.mode        = (uint8_t)doc["mode"].as<int>();
    cmd.doCfg.srcSelGpio  = doc["srcSelGpio"] | false;
    cmd.doCfg.t1          = (uint8_t)doc["t1"].as<int>();
    cmd.doCfg.t2          = (uint8_t)doc["t2"].as<int>();
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["channel"] = ch;
    sendJson(request, resp);
}

// POST /api/channel/{n}/do/set
static void handlePostDoSet(AsyncWebServerRequest* request,
                             uint8_t* data, size_t len,
                             size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    Command cmd{};
    cmd.type     = CMD_DO_SET;
    cmd.channel  = (uint8_t)ch;
    cmd.boolVal  = doc["on"] | false;
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["channel"] = ch;
    resp["on"]      = cmd.boolVal;
    sendJson(request, resp);
}

// POST /api/channel/{n}/vout/range
static void handlePostVoutRange(AsyncWebServerRequest* request,
                                 uint8_t* data, size_t len,
                                 size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    Command cmd{};
    cmd.type     = CMD_SET_VOUT_RANGE;
    cmd.channel  = (uint8_t)ch;
    cmd.boolVal  = doc["bipolar"] | false;
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["channel"] = ch;
    resp["bipolar"] = cmd.boolVal;
    sendJson(request, resp);
}

// POST /api/channel/{n}/ilimit
static void handlePostCurrentLimit(AsyncWebServerRequest* request,
                                    uint8_t* data, size_t len,
                                    size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    Command cmd{};
    cmd.type     = CMD_SET_CURRENT_LIMIT;
    cmd.channel  = (uint8_t)ch;
    cmd.boolVal  = doc["limit8mA"] | false;
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]        = true;
    resp["channel"]   = ch;
    resp["limit8mA"]  = cmd.boolVal;
    sendJson(request, resp);
}

// POST /api/device/reset
static void handlePostDeviceReset(AsyncWebServerRequest* request,
                                   uint8_t* /*data*/, size_t /*len*/,
                                   size_t /*index*/, size_t /*total*/)
{
    // Reset via command queue: clear alerts + set all HIGH_IMP
    Command cmd{};
    cmd.type = CMD_CLEAR_ALERTS;
    sendCommand(cmd);

    // Reset all channel functions to HIGH_IMP
    for (uint8_t ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
        Command funcCmd{};
        funcCmd.type    = CMD_SET_CHANNEL_FUNC;
        funcCmd.channel = ch;
        funcCmd.func    = CH_FUNC_HIGH_IMP;
        sendCommand(funcCmd);
    }

    JsonDocument resp;
    resp["ok"] = true;
    sendJson(request, resp);
}

// POST /api/faults/clear
static void handlePostClearAllFaults(AsyncWebServerRequest* request,
                                      uint8_t* /*data*/, size_t /*len*/,
                                      size_t /*index*/, size_t /*total*/)
{
    Command cmd{};
    cmd.type = CMD_CLEAR_ALERTS;
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"] = true;
    sendJson(request, resp);
}

// POST /api/faults/clear/{n}
static void handlePostClearChannelFault(AsyncWebServerRequest* request,
                                         uint8_t* /*data*/, size_t /*len*/,
                                         size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    Command cmd{};
    cmd.type    = CMD_CLEAR_CHANNEL_ALERT;
    cmd.channel = (uint8_t)ch;
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["channel"] = ch;
    sendJson(request, resp);
}

// POST /api/faults/mask
// Body: {"alertMask": 0, "supplyMask": 0}
static void handlePostFaultsMask(AsyncWebServerRequest* request,
                                  uint8_t* data, size_t len,
                                  size_t /*index*/, size_t /*total*/)
{
    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    Command alertCmd{};
    alertCmd.type    = CMD_SET_ALERT_MASK;
    alertCmd.maskVal = (uint16_t)doc["alertMask"].as<int>();
    sendCommand(alertCmd);

    Command supplyCmd{};
    supplyCmd.type    = CMD_SET_SUPPLY_ALERT_MASK;
    supplyCmd.maskVal = (uint16_t)doc["supplyMask"].as<int>();
    sendCommand(supplyCmd);

    JsonDocument resp;
    resp["ok"] = true;
    sendJson(request, resp);
}

// POST /api/faults/mask/{n}
// Body: {"mask": 0}
static void handlePostChannelFaultMask(AsyncWebServerRequest* request,
                                        uint8_t* data, size_t len,
                                        size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    Command cmd{};
    cmd.type    = CMD_SET_CH_ALERT_MASK;
    cmd.channel = (uint8_t)ch;
    cmd.maskVal = (uint16_t)doc["mask"].as<int>();
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["channel"] = ch;
    sendJson(request, resp);
}

// -----------------------------------------------------------------------------
// GPIO mode name helper
// -----------------------------------------------------------------------------

static const char* gpioModeName(uint8_t mode) {
    static const char* names[] = {"HIGH_IMP","OUTPUT","INPUT","DIN_OUT","DO_EXT"};
    return mode < 5 ? names[mode] : "?";
}

// -----------------------------------------------------------------------------
// GPIO route handlers
// -----------------------------------------------------------------------------

// GET /api/gpio
static void handleGetGpio(AsyncWebServerRequest* request)
{
    JsonDocument doc;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        JsonArray gpios = doc["gpios"].to<JsonArray>();
        for (uint8_t g = 0; g < AD74416H_NUM_GPIOS; g++) {
            const GpioState& gs = g_deviceState.gpio[g];
            JsonObject obj = gpios.add<JsonObject>();
            obj["pin"]      = g;
            obj["name"]     = String((char)('A' + g));
            obj["mode"]     = gs.mode;
            obj["modeName"] = gpioModeName(gs.mode);
            obj["output"]   = gs.outputVal;
            obj["input"]    = gs.inputVal;
            obj["pulldown"] = gs.pulldown;
        }
        xSemaphoreGive(g_stateMutex);
    } else {
        JsonDocument err;
        err["error"] = "State mutex timeout";
        sendJson(request, err, 503);
        return;
    }

    sendJson(request, doc);
}

// POST /api/gpio/{n}/config
// Body: {"mode": 1, "pulldown": false}
static void handlePostGpioConfig(AsyncWebServerRequest* request,
                                  uint8_t* data, size_t len,
                                  size_t /*index*/, size_t /*total*/)
{
    int g = request->pathArg(0).toInt();
    if (g < 0 || g >= AD74416H_NUM_GPIOS) {
        JsonDocument err;
        err["error"] = "GPIO must be 0-5";
        sendJson(request, err, 400);
        return;
    }

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    if (!doc["mode"].is<int>()) {
        JsonDocument err; err["error"] = "Missing 'mode' field";
        sendJson(request, err, 400); return;
    }

    Command cmd{};
    cmd.type            = CMD_GPIO_CONFIG;
    cmd.gpioCfg.gpio    = (uint8_t)g;
    cmd.gpioCfg.mode    = (uint8_t)doc["mode"].as<int>();
    cmd.gpioCfg.pulldown = doc["pulldown"] | false;
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]       = true;
    resp["gpio"]     = g;
    resp["mode"]     = (int)cmd.gpioCfg.mode;
    resp["pulldown"] = cmd.gpioCfg.pulldown;
    sendJson(request, resp);
}

// POST /api/gpio/{n}/set
// Body: {"value": true}
static void handlePostGpioSet(AsyncWebServerRequest* request,
                               uint8_t* data, size_t len,
                               size_t /*index*/, size_t /*total*/)
{
    int g = request->pathArg(0).toInt();
    if (g < 0 || g >= AD74416H_NUM_GPIOS) {
        JsonDocument err;
        err["error"] = "GPIO must be 0-5";
        sendJson(request, err, 400);
        return;
    }

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    Command cmd{};
    cmd.type          = CMD_GPIO_SET;
    cmd.gpioSet.gpio  = (uint8_t)g;
    cmd.gpioSet.value = doc["value"] | false;
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]    = true;
    resp["gpio"]  = g;
    resp["value"] = cmd.gpioSet.value;
    sendJson(request, resp);
}

// -----------------------------------------------------------------------------
// Diagnostic source name / unit helpers
// -----------------------------------------------------------------------------

static const char* diagSourceName(uint8_t source)
{
    switch (source) {
        case 0:  return "AGND";
        case 1:  return "TEMP";
        case 2:  return "DVCC";
        case 3:  return "AVCC";
        case 4:  return "LDO1V8";
        case 5:  return "AVDD_HI";
        case 6:  return "AVDD_LO";
        case 7:  return "AVSS";
        case 8:  return "LVIN";
        case 9:  return "DO_VDD";
        case 10: return "VSENSEP";
        case 11: return "VSENSEN";
        case 12: return "DO_CURRENT";
        case 13: return "AVDD";
        default: return "UNKNOWN";
    }
}

static const char* diagSourceUnit(uint8_t source)
{
    switch (source) {
        case 1:  return "C";       // Temperature in degrees Celsius
        default: return "V";       // All other sources are voltages
    }
}

// -----------------------------------------------------------------------------
// GET /api/diagnostics
// -----------------------------------------------------------------------------

static void handleGetDiagnostics(AsyncWebServerRequest* request)
{
    JsonDocument doc;

    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        JsonArray slots = doc["slots"].to<JsonArray>();
        for (uint8_t i = 0; i < 4; i++) {
            const DiagState& ds = g_deviceState.diag[i];
            JsonObject obj = slots.add<JsonObject>();
            obj["slot"]       = i;
            obj["source"]     = ds.source;
            obj["sourceName"] = diagSourceName(ds.source);
            obj["raw"]        = ds.rawCode;
            obj["value"]      = ds.value;
            obj["unit"]       = diagSourceUnit(ds.source);
        }
        xSemaphoreGive(g_stateMutex);
    } else {
        JsonDocument err;
        err["error"] = "State mutex timeout";
        sendJson(request, err, 503);
        return;
    }

    sendJson(request, doc);
}

// -----------------------------------------------------------------------------
// POST /api/diagnostics/config
// Body: {"slot": 0, "source": 1}
// -----------------------------------------------------------------------------

static void handlePostDiagConfig(AsyncWebServerRequest* request,
                                  uint8_t* data, size_t len,
                                  size_t /*index*/, size_t /*total*/)
{
    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    if (!doc["slot"].is<int>() || !doc["source"].is<int>()) {
        JsonDocument err; err["error"] = "Missing 'slot' or 'source' field";
        sendJson(request, err, 400); return;
    }

    int slot   = doc["slot"].as<int>();
    int source = doc["source"].as<int>();

    if (slot < 0 || slot > 3) {
        JsonDocument err; err["error"] = "Slot must be 0-3";
        sendJson(request, err, 400); return;
    }
    if (source < 0 || source > 13) {
        JsonDocument err; err["error"] = "Source must be 0-13";
        sendJson(request, err, 400); return;
    }

    Command cmd{};
    cmd.type             = CMD_DIAG_CONFIG;
    cmd.diagCfg.slot     = (uint8_t)slot;
    cmd.diagCfg.source   = (uint8_t)source;
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]     = true;
    resp["slot"]   = slot;
    resp["source"] = source;
    sendJson(request, resp);
}

// -----------------------------------------------------------------------------
// GET /api/device/info
// -----------------------------------------------------------------------------

static void handleGetDeviceInfo(AsyncWebServerRequest* request)
{
    extern AD74416H_SPI spiDriver;

    uint16_t rev = 0, id0 = 0, id1 = 0;
    spiDriver.readRegister(REG_SILICON_REV, &rev);
    spiDriver.readRegister(REG_SILICON_ID0, &id0);
    spiDriver.readRegister(REG_SILICON_ID1, &id1);

    bool spiOk = false;
    if (xSemaphoreTake(g_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        spiOk = g_deviceState.spiOk;
        xSemaphoreGive(g_stateMutex);
    }

    char id0Str[8], id1Str[8];
    snprintf(id0Str, sizeof(id0Str), "0x%04X", id0);
    snprintf(id1Str, sizeof(id1Str), "0x%04X", id1);

    JsonDocument doc;
    doc["siliconRev"]  = (int)rev;
    doc["siliconId0"]  = id0Str;
    doc["siliconId1"]  = id1Str;
    doc["spiOk"]       = spiOk;
    sendJson(request, doc);
}

// -----------------------------------------------------------------------------
// POST /api/channel/{n}/avdd
// Body: {"select": 0}
// -----------------------------------------------------------------------------

static void handlePostAvddSelect(AsyncWebServerRequest* request,
                                   uint8_t* data, size_t len,
                                   size_t /*index*/, size_t /*total*/)
{
    int ch;
    if (!validChannel(request, ch)) return;

    JsonDocument doc;
    if (!parseBody(request, data, len, doc)) return;

    if (!doc["select"].is<int>()) {
        JsonDocument err; err["error"] = "Missing 'select' field";
        sendJson(request, err, 400); return;
    }

    int sel = doc["select"].as<int>();
    if (sel < 0 || sel > 3) {
        JsonDocument err; err["error"] = "Select must be 0-3";
        sendJson(request, err, 400); return;
    }

    Command cmd{};
    cmd.type    = CMD_SET_AVDD_SELECT;
    cmd.channel = (uint8_t)ch;
    cmd.avddSel = (uint8_t)sel;
    sendCommand(cmd);

    JsonDocument resp;
    resp["ok"]      = true;
    resp["channel"] = ch;
    resp["select"]  = sel;
    sendJson(request, resp);
}

// -----------------------------------------------------------------------------
// GET /api/channel/{n}/dac/readback
// -----------------------------------------------------------------------------

static void handleGetDacReadback(AsyncWebServerRequest* request)
{
    int ch;
    if (!validChannel(request, ch)) return;

    extern AD74416H_SPI spiDriver;

    uint16_t activeCode = 0;
    spiDriver.readRegister(AD74416H_REG_DAC_ACTIVE(ch), &activeCode);

    JsonDocument doc;
    doc["channel"]    = ch;
    doc["activeCode"] = (int)activeCode;
    sendJson(request, doc);
}

// -----------------------------------------------------------------------------
// OPTIONS preflight handler (CORS)
// -----------------------------------------------------------------------------

static void handleOptions(AsyncWebServerRequest* request)
{
    AsyncWebServerResponse* resp = request->beginResponse(204);
    addCorsHeaders(resp);
    request->send(resp);
}

// -----------------------------------------------------------------------------
// initWebServer
// -----------------------------------------------------------------------------

void initWebServer(AsyncWebServer& server)
{
    // Serve index.html
    server.on("/", HTTP_GET, handleRoot);

    // Full device status
    server.on("/api/status", HTTP_GET, handleGetStatus);

    // Fault information
    server.on("/api/faults", HTTP_GET, handleGetFaults);

    // Device reset
    server.on("/api/device/reset", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostDeviceReset);

    // Clear all faults
    server.on("/api/faults/clear", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostClearAllFaults);

    // Global fault masks
    server.on("/api/faults/mask", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostFaultsMask);

    // Per-channel fault clear: /api/faults/clear/{n}
    server.on("^\\/api\\/faults\\/clear\\/(\\d+)$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostClearChannelFault);

    // Per-channel fault mask: /api/faults/mask/{n}
    server.on("^\\/api\\/faults\\/mask\\/(\\d+)$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostChannelFaultMask);

    // Per-channel GET ADC: /api/channel/{n}/adc
    server.on("^\\/api\\/channel\\/(\\d+)\\/adc$", HTTP_GET,
        handleGetChannelAdc);

    // Per-channel POST function: /api/channel/{n}/function
    server.on("^\\/api\\/channel\\/(\\d+)\\/function$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostChannelFunction);

    // Per-channel POST DAC: /api/channel/{n}/dac
    server.on("^\\/api\\/channel\\/(\\d+)\\/dac$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostChannelDac);

    // Per-channel POST ADC config: /api/channel/{n}/adc/config
    server.on("^\\/api\\/channel\\/(\\d+)\\/adc\\/config$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostAdcConfig);

    // Per-channel POST DIN config: /api/channel/{n}/din/config
    server.on("^\\/api\\/channel\\/(\\d+)\\/din\\/config$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostDinConfig);

    // Per-channel POST DO config: /api/channel/{n}/do/config
    server.on("^\\/api\\/channel\\/(\\d+)\\/do\\/config$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostDoConfig);

    // Per-channel POST DO set: /api/channel/{n}/do/set
    server.on("^\\/api\\/channel\\/(\\d+)\\/do\\/set$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostDoSet);

    // Per-channel POST VOUT range: /api/channel/{n}/vout/range
    server.on("^\\/api\\/channel\\/(\\d+)\\/vout\\/range$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostVoutRange);

    // Per-channel POST current limit: /api/channel/{n}/ilimit
    server.on("^\\/api\\/channel\\/(\\d+)\\/ilimit$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostCurrentLimit);

    // Diagnostics: GET all slots
    server.on("/api/diagnostics", HTTP_GET, handleGetDiagnostics);

    // Diagnostics: POST config
    server.on("/api/diagnostics/config", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostDiagConfig);

    // Device info
    server.on("/api/device/info", HTTP_GET, handleGetDeviceInfo);

    // Per-channel POST AVDD select: /api/channel/{n}/avdd
    server.on("^\\/api\\/channel\\/(\\d+)\\/avdd$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostAvddSelect);

    // Per-channel GET DAC readback: /api/channel/{n}/dac/readback
    server.on("^\\/api\\/channel\\/(\\d+)\\/dac\\/readback$", HTTP_GET,
        handleGetDacReadback);

    // GPIO: GET all states
    server.on("/api/gpio", HTTP_GET, handleGetGpio);

    // GPIO: POST config /api/gpio/{n}/config
    server.on("^\\/api\\/gpio\\/([0-5])\\/config$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostGpioConfig);

    // GPIO: POST set /api/gpio/{n}/set
    server.on("^\\/api\\/gpio\\/([0-5])\\/set$", HTTP_POST,
        [](AsyncWebServerRequest* request) {},
        nullptr,
        handlePostGpioSet);

    // CORS preflight for all API routes
    server.on("^\\/api\\/.*$", HTTP_OPTIONS, handleOptions);

    // 404 handler
    server.onNotFound([](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* resp =
            request->beginResponse(404, "text/plain", "Not found");
        addCorsHeaders(resp);
        request->send(resp);
    });
}
