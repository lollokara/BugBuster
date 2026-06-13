"""
HTTP route dispatcher for SimulatedDevice.

Routes GET/POST requests to the appropriate handler and returns dicts
that match what _normalize_http_* functions in client.py expect.
"""

def check_admin_auth(device, headers: dict) -> bool:
    """Checks for X-BugBuster-Admin-Token header (case-insensitive per HTTP spec)."""
    # HTTP headers are case-insensitive; WSGI normalises them to uppercase so we
    # do a lower-case scan rather than a direct dict.get().
    token = next(
        (v for k, v in headers.items() if k.lower() == "x-bugbuster-admin-token"),
        None,
    )
    # If device.admin_token is None, auth is disabled
    if getattr(device, 'admin_token', None) is None:
        return True
    return token == device.admin_token

def dispatch(device, method: str, path: str, params: dict, body: dict, headers: dict) -> dict:
    method = method.upper()
    # Normalize path: remove leading /api if present
    if path.startswith("/api"):
        path = path[4:]
    if not path.startswith("/"):
        path = "/" + path

    key = (method, path)

    # Auth enforcement for POST / mutating routes
    if method == "POST":
        if not check_admin_auth(device, headers):
            return {"error": "unauthorized", "code": 401}

    # Device version
    if key == ("GET", "/device/version"):
        return {
            "fwMajor": device.fw_version[0],
            "fwMinor": device.fw_version[1],
            "fwPatch": device.fw_version[2],
        }

    # Combined I2C diagnostics — mirrors firmware /api/debug.
    if key == ("GET", "/debug"):
        return {
            "i2cBusOk": True,
            "ds4424": {"present": True},
            "husb238": {"present": True, "attached": False, "voltageV": 0, "currentA": 0},
            "pca9535": {
                "present": True,
                "input0": 0, "input1": 0,
                "output0": 0, "output1": 0,
            },
        }

    # Device info
    if key == ("GET", "/device/info"):
        # Mirror the firmware: expose macAddress so HTTP pairing flows
        # (desktop + python client get_mac_address) can be exercised hardware-free.
        mac = getattr(device, "mac_address", "aa:bb:cc:00:11:22")
        return {
            "spi_ok": device.spi_ok,
            "silicon_rev": 1,
            "silicon_id0": 0xABCD,
            "silicon_id1": 0x1234,
            "macAddress": mac,
            "mac_address": mac,
        }

    # Full status snapshot
    if key == ("GET", "/status"):
        return _status_dict(device)

    # Faults
    if key == ("GET", "/faults"):
        return _faults_dict(device)

    if key == ("POST", "/faults/clear"):
        device.alert_status = 0
        device.supply_alert_status = 0
        for ch in device.channels:
            ch["channel_alert"] = 0
        return {"ok": True}

    if method == "POST" and path.startswith("/faults/clear/"):
        try:
            ch_idx = int(path.split("/")[-1])
            if 0 <= ch_idx < len(device.channels):
                device.channels[ch_idx]["channel_alert"] = 0
            return {"ok": True}
        except ValueError:
            return {"error": "invalid channel index"}

    # Device reset
    if key == ("POST", "/device/reset"):
        for ch in device.channels:
            ch["function"] = 0
            ch["dac_code"] = 0
            ch["dac_value"] = 0.0
            ch["channel_alert"] = 0
        device.alert_status = 0
        device.supply_alert_status = 0
        # Reset LA state if present
        if hasattr(device, 'la_state'):
            device.la_state = "IDLE"
        return {"ok": True}

    # Channel function
    if method == "POST" and path.startswith("/channel/") and path.endswith("/function"):
        if "function" not in body:
            return {"error": "missing field: function", "code": 400}
        parts = path.split("/")
        try:
            ch_idx = int(parts[2])
            if 0 <= ch_idx < len(device.channels):
                func = int(body.get("function", 0))
                ch = device.channels[ch_idx]
                ch["function"] = func
                if func == 0:  # HIGH_IMP resets DAC/ADC state
                    ch["dac_code"] = 0
                    ch["dac_value"] = 0.0
                    ch["adc_raw"] = 0
                    ch["adc_value"] = 0.0
                    ch["adc_range"] = 0
                    ch["adc_rate"] = 0
                    ch["adc_mux"] = 0
            return {"ok": True}
        except ValueError:
            return {"error": "invalid path or body"}

    # DAC readback
    if method == "GET" and path.startswith("/channel/") and path.endswith("/dac/readback"):
        parts = path.split("/")
        try:
            ch_idx = int(parts[2])
            code = 0
            if 0 <= ch_idx < len(device.channels):
                code = device.channels[ch_idx]["dac_code"]
            return {"code": code}
        except ValueError:
            return {"error": "invalid channel index"}

    # DAC set (voltage, current, or raw code)
    if method == "POST" and path.startswith("/channel/") and path.endswith("/dac"):
        parts = path.split("/")
        try:
            ch_idx = int(parts[2])
            if 0 <= ch_idx < len(device.channels):
                ch = device.channels[ch_idx]
                if "voltage" in body:
                    voltage = float(body["voltage"])
                    bipolar = bool(body.get("bipolar", False))
                    ch["dac_value"] = voltage
                    if bipolar:
                        code = int((voltage / 24.0 + 0.5) * 65535) & 0xFFFF
                    else:
                        code = int((voltage / 12.0) * 65535) & 0xFFFF
                    ch["dac_code"] = code
                elif "current_mA" in body:
                    current_ma = float(body["current_mA"])
                    ch["dac_value"] = current_ma
                    ch["dac_code"] = int((current_ma / 25.0) * 65535) & 0xFFFF
                elif "code" in body:
                    ch["dac_code"] = int(body["code"]) & 0xFFFF
                else:
                    return {"error": "missing field: voltage, current_mA or code", "code": 400}
            return {"ok": True}
        except (ValueError, TypeError):
            return {"error": "invalid value"}

    # Channel alert mask — matches firmware route `POST /api/faults/mask/<ch>`.
    # Must be checked BEFORE the global `POST /faults/mask` so the channel
    # variant wins on `/faults/mask/0` etc.
    if method == "POST" and path.startswith("/faults/mask/"):
        if "mask" not in body:
            return {"error": "missing field: mask", "code": 400}
        try:
            ch_idx = int(path[len("/faults/mask/"):])
            if 0 <= ch_idx < len(device.channels):
                device.channels[ch_idx]["channel_alert_mask"] = int(body.get("mask", 0xFFFF)) & 0xFFFF
            return {"ok": True}
        except ValueError:
            return {"error": "invalid channel index"}

    # Global alert mask
    if key == ("POST", "/faults/mask"):
        if "alert_mask" not in body and "supply_mask" not in body:
             return {"error": "missing field: alert_mask or supply_mask", "code": 400}
        if "alert_mask" in body:
            device.alert_mask = int(body.get("alert_mask", 0xFFFF)) & 0xFFFF
        if "supply_mask" in body:
            device.supply_alert_mask = int(body.get("supply_mask", 0xFFFF)) & 0xFFFF
        return {"ok": True}

    # Per-channel fault clear
    if method == "POST" and path.startswith("/faults/channel/") and path.endswith("/clear"):
        parts = path.split("/")
        try:
            ch_idx = int(parts[3])
            if 0 <= ch_idx < len(device.channels):
                device.channels[ch_idx]["channel_alert"] = 0
            return {"ok": True}
        except ValueError:
            return {"error": "invalid channel index"}

    # Selftest
    if key == ("GET", "/selftest"):
        return {
            "boot": {"ran": True, "passed": True,
                     "vadj1_v": 12.0, "vadj2_v": 5.0, "vlogic_v": 3.3},
            "cal": {"status": 0, "channel": 0, "points": 0, "error_mv": 0.0},
        }

    if method == "GET" and path.startswith("/selftest/supply/"):
        try:
            rail = int(path.split("/")[-1])
            voltages = [12.0, 5.0, 3.3]
            return {"voltage": voltages[rail] if rail < len(voltages) else -1.0}
        except ValueError:
            return {"error": "invalid supply index"}

    if key == ("GET", "/selftest/supplies/cached"):
        return {
            "available": True,
            "timestampMs": 0,
            "rails": [
                {"rail": 0, "name": "VADJ1", "voltageV": 12.0},
                {"rail": 1, "name": "VADJ2", "voltageV": 5.0},
                {"rail": 2, "name": "VLOGIC", "voltageV": 3.3},
            ],
        }

    if key == ("GET", "/selftest/efuse"):
        return {
            "available": True,
            "timestampMs": 0,
            "rails": [
                {"rail": 0, "name": "VADJ1", "voltageV": 12.0},
                {"rail": 1, "name": "VADJ2", "voltageV": 5.0},
                {"rail": 2, "name": "VLOGIC", "voltageV": 3.3},
            ],
        }

    if key == ("GET", "/selftest/supplies"):
        return {
            "valid": True, "supplies_ok": True,
            "avdd_hi_v": 21.5, "dvcc_v": 5.0, "avcc_v": 5.0,
            "avss_v": -16.0, "temp_c": 25.0,
        }

    # GPIO — GET /gpio → {"gpios": [...]}
    if key == ("GET", "/gpio"):
        return {
            "gpios": [
                {
                    "id": g.get("id", i),
                    "mode": g.get("mode", 0),
                    "output": g.get("output", False),
                    "input": g.get("input", False),
                    "pulldown": g.get("pulldown", False),
                }
                for i, g in enumerate(device.gpio)
            ]
        }

    # GPIO — POST /gpio/{pin}/config
    if method == "POST" and path.startswith("/gpio/") and path.endswith("/config"):
        if "mode" not in body:
            return {"error": "missing field: mode", "code": 400}
        parts = path.split("/")
        try:
            pin_id = int(parts[2])
            if 0 <= pin_id < len(device.gpio):
                device.gpio[pin_id]["mode"] = int(body.get("mode", 0))
                device.gpio[pin_id]["pulldown"] = bool(body.get("pulldown", False))
            return {"ok": True}
        except ValueError:
            return {"error": "invalid pin id"}

    # GPIO — POST /gpio/{pin}/set
    if method == "POST" and path.startswith("/gpio/") and path.endswith("/set"):
        if "value" not in body:
            return {"error": "missing field: value", "code": 400}
        parts = path.split("/")
        try:
            pin_id = int(parts[2])
            if 0 <= pin_id < len(device.gpio):
                device.gpio[pin_id]["output"] = bool(body.get("value", False))
            return {"ok": True}
        except ValueError:
            return {"error": "invalid pin id"}

    # DIO — GET /dio → {"ios": [...]}
    if key == ("GET", "/dio"):
        return {
            "ios": [
                {
                    "io": i + 1,
                    "gpio": 0,
                    "mode": d.get("mode", 0),
                    "output": d.get("output", False),
                    "input": d.get("input", False),
                }
                for i, d in enumerate(device.dio)
            ]
        }

    # DIO — GET /dio/{io}
    if method == "GET" and path.startswith("/dio/"):
        try:
            io_num = int(path.split("/")[-1])
            idx = io_num - 1
            if 0 <= idx < len(device.dio):
                d = device.dio[idx]
                return {
                    "io": io_num,
                    "mode": d.get("mode", 0),
                    "value": bool(d.get("output", False)),
                }
            return {"io": io_num, "mode": 0, "value": False}
        except ValueError:
            return {"error": "invalid io number"}

    # DIO — POST /dio/{io}/config
    if method == "POST" and path.startswith("/dio/") and path.endswith("/config"):
        if "mode" not in body:
            return {"error": "missing field: mode", "code": 400}
        parts = path.split("/")
        try:
            io_num = int(parts[2])
            idx = io_num - 1
            if 0 <= idx < len(device.dio):
                device.dio[idx]["mode"] = int(body.get("mode", 0))
            return {"ok": True}
        except ValueError:
            return {"error": "invalid io number"}

    # DIO — POST /dio/{io}/set
    if method == "POST" and path.startswith("/dio/") and path.endswith("/set"):
        if "value" not in body:
            return {"error": "missing field: value", "code": 400}
        parts = path.split("/")
        try:
            io_num = int(parts[2])
            idx = io_num - 1
            if 0 <= idx < len(device.dio):
                device.dio[idx]["output"] = bool(body.get("value", False))
            return {"ok": True}
        except ValueError:
            return {"error": "invalid io number"}

    # MUX — GET /mux → {"states": [b0, b1, b2, b3]}
    if key == ("GET", "/mux"):
        return {"states": list(device.mux_states[:4])}

    # MUX — POST /mux/all
    if key == ("POST", "/mux/all"):
        if "states" not in body:
            return {"error": "missing field: states", "code": 400}
        states = body.get("states", [0, 0, 0, 0])
        for i in range(4):
            if i < len(states):
                device.mux_states[i] = int(states[i]) & 0xFF
        return {"ok": True}

    # MUX — POST /mux/switch
    if key == ("POST", "/mux/switch"):
        if "device" not in body or "switch" not in body or "closed" not in body:
            return {"error": "missing fields: device, switch or closed", "code": 400}
        try:
            dev_idx  = int(body.get("device", 0))
            sw_idx   = int(body.get("switch", 0))
            closed   = bool(body.get("closed", False))
            if not hasattr(device, "adgs_active"):
                device.adgs_active = [None, None, None, None]
            if 0 <= dev_idx < 4 and 0 <= sw_idx < 8:
                if closed:
                    if sw_idx < 4:
                        group_mask = 0x0F
                    elif sw_idx < 6:
                        group_mask = 0x30
                    else:
                        group_mask = 0xC0
                    device.mux_states[dev_idx] &= ~group_mask & 0xFF
                    device.mux_states[dev_idx] |= (1 << sw_idx)
                    remaining = device.mux_states[dev_idx]
                    device.adgs_active[dev_idx] = (
                        (remaining & -remaining).bit_length() - 1 if remaining else None
                    )
                else:
                    device.mux_states[dev_idx] &= ~(1 << sw_idx)
                    device.mux_states[dev_idx] &= 0xFF
                    remaining = device.mux_states[dev_idx]
                    device.adgs_active[dev_idx] = (
                        (remaining & -remaining).bit_length() - 1 if remaining else None
                    )
            return {"ok": True}
        except ValueError:
            return {"error": "invalid value"}

    # HAT status
    if key == ("GET", "/hat"):
        pins = getattr(device, 'hat_pins', [0] * 8)
        detected = device.hat.present
        return {
            "detected": detected,
            "connected": detected,
            "type": 1 if detected else 0,
            "detect_voltage": 3.3 if detected else 0.0,
            "fw_version": "1.0",
            "config_confirmed": detected,
            "pin_config": list(pins[:4]),
        }

    # LA Status — Phase 0 schema
    if key == ("GET", "/hat/la/status"):
        state_name = getattr(device, "la_state", "IDLE")
        rate = device.la_config.get("sample_rate", 1000000)
        return {
            "stateName": state_name,
            "stopReasonName": "NONE",
            "active": state_name != "IDLE",
            "triggerArmed": False,
            "samplesCaptured": 0,
            "maxSamples": 1024,
            "actualRateHz": rate,
            "clockHz": rate,
            "channels": 8
        }

    # HAT detect
    if key == ("POST", "/hat/detect"):
        detected = device.hat.present
        return {
            "detected": detected,
            "type": 1 if detected else 0,
            "detect_voltage": 3.3 if detected else 0.0,
            "connected": detected,
        }

    # HAT reset
    if key == ("POST", "/hat/reset"):
        device.la_state = "IDLE"
        device.hat_pins = [0] * 8
        return {"ok": True}

    # HAT set pin / set all pins
    if key == ("POST", "/hat/config"):
        if "pin" in body:
            pin_id = int(body["pin"])
            func = int(body.get("function", 0))
            if 0 <= pin_id < len(device.hat_pins):
                device.hat_pins[pin_id] = func
            return {"ok": True}
        if "pins" in body:
            pins = body["pins"]
            for i, p in enumerate(pins):
                if i < len(device.hat_pins):
                    device.hat_pins[i] = int(p)
            return {"ok": True}
        return {"error": "missing field: pin or pins", "code": 400}

    # HAT SWD setup
    if key == ("POST", "/hat/setup_swd"):
        return {"ok": True}

    # HAT v2 capabilities
    if key == ("GET", "/hat/v2/caps"):
        flags = 0x37  # RAILS|LEDS|LA_LOW_SPEED|SHIFTED_IO|HVPAK_UNSUPPORTED
        return {
            "hwRevision": 2,
            "flags": flags,
            "railCount": 3,
            "ledCount": 8,
            "shiftedIoCount": 8,
            "laRouteCount": 2,
            "fwMajor": 2,
            "fwMinor": 1,
            "hvpakPresent": False,
        }

    # HAT v2 rails status
    if key == ("GET", "/hat/v2/rails"):
        return {
            "railCount": len(device.hat_rails),
            "rails": [
                {
                    "railId": r["rail_id"],
                    "enabled": r["enabled"],
                    "voltageMv": r["voltage_mv"],
                    "currentMa": r["current_ma"],
                    "status": r["status"]
                }
                for r in device.hat_rails
            ]
        }

    # HAT v2 rail enable
    if key == ("POST", "/hat/v2/rail/enable"):
        rail_id = int(body.get("railId", 0))
        enable = bool(body.get("enable", False))
        if 0 <= rail_id < len(device.hat_rails):
            device.hat_rails[rail_id]["enabled"] = enable
            if enable:
                if rail_id == 0:
                    device.hat_rails[rail_id]["voltage_mv"] = 3300
                    device.hat_rails[rail_id]["current_ma"] = 150
                elif rail_id == 1:
                    device.hat_rails[rail_id]["voltage_mv"] = 3300
                    device.hat_rails[rail_id]["current_ma"] = 100
                elif rail_id == 2:
                    device.hat_rails[rail_id]["voltage_mv"] = 5000
                    device.hat_rails[rail_id]["current_ma"] = 200
            else:
                device.hat_rails[rail_id]["voltage_mv"] = 0
                device.hat_rails[rail_id]["current_ma"] = 0
        r = device.hat_rails[rail_id]
        return {
            "ok": True,
            "railId": r["rail_id"],
            "enabled": r["enabled"],
            "voltageMv": r["voltage_mv"],
            "currentMa": r["current_ma"],
            "status": r["status"]
        }

    # HAT connector power GET
    if key == ("GET", "/hat/power"):
        return {
            "connA": {"enabled": device.hat_power[0], "currentMa": 0.0, "fault": False},
            "connB": {"enabled": device.hat_power[1], "currentMa": 0.0, "fault": False},
            "ioVoltageMv": device.hat_io_volt,
        }

    # HAT connector power SET
    if key == ("POST", "/hat/power"):
        connector = int(body.get("connector", 0))
        enable = bool(body.get("enable", False))
        if 0 <= connector <= 1:
            device.hat_power[connector] = enable
        conn_key = "connA" if connector == 0 else "connB"
        return {"ok": True, "connector": connector, "enabled": device.hat_power[connector], "currentMa": 0.0, "fault": False}

    # HAT v2 set LED state
    if key == ("POST", "/hat/v2/led"):
        led_id = int(body.get("ledId", 0))
        color_code = int(body.get("colorCode", 0))
        if 0 <= led_id < 8:
            device.hat_led_states[led_id] = [0, 0, 0, color_code]
        return {"ok": True}

    # HAT v2 set LA route
    if key == ("POST", "/hat/v2/la/route"):
        route = int(body.get("route", 0))
        if route == 1:
            return {"error": "unsupported route", "code": 400}
        device.hat_la_route = route
        return {"ok": True, "route": route}

    # HAT v2 calibration start
    if key == ("POST", "/hat/v2/calibrate/start"):
        rail_id = int(body.get("railId", 0))
        device.hat_cal_state = 2
        device.hat_cal_progress = 100
        device.hat_cal_rail_id = rail_id
        device.hat_cal_last_error = 0
        return {"ok": True, "status": device.hat_cal_state}

    # HAT v2 calibration status
    if key == ("GET", "/hat/v2/calibrate/status"):
        return {
            "state": device.hat_cal_state,
            "progress": device.hat_cal_progress,
            "railId": device.hat_cal_rail_id,
            "lastError": device.hat_cal_last_error,
            "persistState": getattr(device, "hat_cal_persist_state", 0),
            "stage": getattr(device, "hat_cal_stage", 5),
            "point": getattr(device, "hat_cal_point", 128),
            "code": getattr(device, "hat_cal_code", 0),
            "measuredMv": getattr(device, "hat_cal_measured_mv", 3300),
            "minMv": getattr(device, "hat_cal_min_mv", 0),
            "maxMv": getattr(device, "hat_cal_max_mv", 36000),
            "maxGapMv": getattr(device, "hat_cal_max_gap_mv", 500),
            "maxErrorMv": getattr(device, "hat_cal_max_error_mv", 0),
            "validationFlags": getattr(device, "hat_cal_validation_flags", 0),
        }

    # HAT v2 calibration import
    if key == ("POST", "/hat/v2/calibrate/import"):
        return {"ok": True}

    # HAT v2 set IO bank
    if key == ("POST", "/hat/v2/io_bank"):
        device.hat_io_bank["dirs"] = int(body.get("dirs", 0))
        device.hat_io_bank["ups"] = int(body.get("ups", 0))
        device.hat_io_bank["dns"] = int(body.get("dns", 0))
        return {"ok": True}

    # HAT v2 set level shift
    if key == ("POST", "/hat/v2/level_shift"):
        oe = bool(body.get("oe", False))
        ls_dir = bool(body.get("dir", False))
        device.hat_level_shift["oe"] = oe
        device.hat_level_shift["dir"] = ls_dir
        return {
            "ok": True,
            "oe": oe,
            "dir": ls_dir
        }


    # IDAC status — GET /idac
    if key == ("GET", "/idac"):
        idac = getattr(device, 'idac', [])
        channels = []
        for i, ch in enumerate(idac):
            channels.append({
                "channel": i,
                "code": ch.get("code", 0),
                "target_v": ch.get("target_v", 0.0),
                "actual_v": ch.get("actual_v", 0.0),
                "v_min": ch.get("v_min", 0.0),
                "v_max": ch.get("v_max", 15.0),
                "step_mv": ch.get("step_mv", 100.0),
                "calibrated": ch.get("calibrated", False),
            })
        return {"present": True, "channels": channels}

    # IDAC set voltage — POST /idac/voltage
    if key == ("POST", "/idac/voltage"):
        if "ch" not in body or "voltage" not in body:
            return {"error": "missing field: ch or voltage", "code": 400}
        try:
            ch_idx = int(body.get("ch", 0))
            voltage = float(body.get("voltage", 0.0))
            idac = getattr(device, 'idac', [])
            if 0 <= ch_idx < len(idac):
                idac[ch_idx]["target_v"] = voltage
                idac[ch_idx]["actual_v"] = voltage
            return {"ok": True}
        except ValueError:
            return {"error": "invalid value"}

    # IDAC set code — POST /idac/code
    if key == ("POST", "/idac/code"):
        if "ch" not in body or "code" not in body:
            return {"error": "missing field: ch or code", "code": 400}
        try:
            ch_idx = int(body.get("ch", 0))
            code = int(body.get("code", 0))
            idac = getattr(device, 'idac', [])
            if 0 <= ch_idx < len(idac):
                idac[ch_idx]["code"] = code
            return {"ok": True}
        except ValueError:
            return {"error": "invalid value"}

    # IDAC cal save — POST /idac/cal/save
    if key == ("POST", "/idac/cal/save"):
        return {"ok": True}

    # IO expander (PCA9535) power status — GET /ioexp
    if key == ("GET", "/ioexp"):
        return {
            "present": True,
            "logic_pg": True,
            "vadj1_pg": True,
            "vadj2_pg": True,
            "efuse_faults": [False, False, False, False],
            "enables": {
                "vadj1": True, "vadj2": True, "15v": False,
                "mux": True, "usb_hub": True,
                "efuse1": True, "efuse2": True, "efuse3": True, "efuse4": True,
            },
        }

    # IO expander control — POST /ioexp/control
    if key == ("POST", "/ioexp/control"):
        return {"ok": True}

    # IO expander fault log — GET /ioexp/faults
    if key == ("GET", "/ioexp/faults"):
        return {"faults": []}

    # IO expander fault config — POST /ioexp/fault_config
    if key == ("POST", "/ioexp/fault_config"):
        return {"ok": True}

    # USB PD status — GET /usbpd
    if key == ("GET", "/usbpd"):
        _CODE_TO_V = {1: 5.0, 2: 9.0, 3: 12.0, 4: 15.0, 5: 18.0, 6: 20.0}
        code = getattr(device, 'usbpd_voltage', 1)
        voltage_v = _CODE_TO_V.get(code, 5.0)
        source_pdos = []
        for v in [5.0, 9.0, 12.0, 15.0, 18.0, 20.0]:
            source_pdos.append({
                "voltage": f"{int(v)}V",
                "detected": True,
                "maxCurrentA": 3.0,
                "maxPowerW": v * 3.0
            })
        return {
            "present": True,
            "attached": True,
            "cc": "CC1",
            "voltageV": voltage_v,
            "currentA": 3.0,
            "powerW": voltage_v * 3.0,
            "pdResponse": 0,
            "sourcePdos": source_pdos,
            "selectedPdo": code - 1
        }

    # USB PD select voltage — POST /usbpd/select
    if key == ("POST", "/usbpd/select"):
        if "voltage" not in body:
            return {"error": "missing field: voltage", "code": 400}
        _V_TO_CODE = {5: 1, 9: 2, 12: 3, 15: 4, 18: 5, 20: 6}
        try:
            voltage = int(body.get("voltage", 5))
            device.usbpd_voltage = _V_TO_CODE.get(voltage, 1)
            return {"ok": True}
        except ValueError:
            return {"error": "invalid voltage"}

    # Quick setup — GET /quicksetup
    if key == ("GET", "/quicksetup"):
        slots = getattr(device, "qs_slots", [None] * 4)
        result = []
        for i, slot in enumerate(slots):
            entry = {"index": i, "occupied": slot is not None, "summary": None}
            if slot is not None:
                entry["summary"] = {
                    "name": slot["name"],
                    "ts": slot["ts"],
                    "size": len(slot.get("payload_json", "{}")),
                    "hash": slot["hash"],
                }
            result.append(entry)
        return {"slots": result}

    # Quick setup — GET /quicksetup/{slot}
    if method == "GET" and path.startswith("/quicksetup/") and len(path) == len("/quicksetup/X"):
        try:
            idx = int(path[-1])
            slots = getattr(device, "qs_slots", [None] * 4)
            if 0 <= idx < 4 and slots[idx] is not None:
                return slots[idx]["payload"]
            return {"error": "not found", "code": 404}
        except (ValueError, IndexError):
            return {"error": "invalid slot", "code": 400}

    # Quick setup — POST /quicksetup/{slot}[/apply|/delete]
    if method == "POST" and path.startswith("/quicksetup/"):
        import json as _json
        import time as _time
        parts = path.split("/")
        try:
            idx = int(parts[2])
            suffix = parts[3] if len(parts) > 3 else ""
        except (ValueError, IndexError):
            return {"error": "invalid slot", "code": 400}
        if not (0 <= idx < 4):
            return {"error": "invalid slot", "code": 400}
        if not hasattr(device, "qs_slots"):
            device.qs_slots = [None] * 4

        if suffix == "":
            snap = {"channels": [{"id": ch["id"], "function": ch["function"], "dac_code": ch["dac_code"], "dac_value": ch["dac_value"]} for ch in device.channels]}
            raw = _json.dumps(snap, sort_keys=True).encode()
            h = 0
            for b in raw:
                h = ((h << 5) + h + b) & 0xFF
            device.qs_slots[idx] = {"name": f"slot{idx}", "ts": int(_time.time()), "hash": h, "payload": snap}
            return snap

        if suffix == "apply":
            slot = device.qs_slots[idx]
            if slot is None:
                return {"ok": False, "applied": False, "error": "not found", "code": 404}
            for ch_snap in slot["payload"].get("channels", []):
                for ch in device.channels:
                    if ch["id"] == ch_snap.get("id"):
                        ch["function"] = ch_snap.get("function", ch["function"])
                        ch["dac_code"] = ch_snap.get("dac_code", ch["dac_code"])
                        ch["dac_value"] = ch_snap.get("dac_value", ch["dac_value"])
            return {"ok": True, "applied": True}

        if suffix == "delete":
            existed = device.qs_slots[idx] is not None
            device.qs_slots[idx] = None
            return {"ok": True, "deleted": existed}

        return {"error": "unknown quicksetup action", "code": 404}

    # WiFi status — GET /wifi
    if key == ("GET", "/wifi"):
        return {
            "connected": getattr(device, 'wifi_connected', False),
            "sta_ssid": "",
            "sta_ip": "",
            "rssi": 0,
            "ap_ssid": "BugBuster",
            "ap_ip": "192.168.4.1",
            "ap_mac": "AA:BB:CC:DD:EE:FF",
        }

    # WiFi scan — GET /wifi/scan
    if key == ("GET", "/wifi/scan"):
        return {"networks": []}

    # WiFi connect — POST /wifi/connect
    if key == ("POST", "/wifi/connect"):
        return {"ok": True}

    # Fallback
    return {"error": "not implemented", "path": path, "method": method, "code": 404}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _status_dict(device) -> dict:
    channels = []
    for ch in device.channels:
        channels.append({
            "id": ch["id"],
            "function": ch["function"],
            "adc_raw": ch["adc_raw"],
            "adc_value": ch["adc_value"],
            "adc_range": ch["adc_range"],
            "adc_rate": ch["adc_rate"],
            "adc_mux": ch["adc_mux"],
            "dac_code": ch["dac_code"],
            "dac_value": ch["dac_value"],
            "din_state": ch["din_state"],
            "din_counter": ch["din_counter"],
            "do_state": ch["do_state"],
            "channel_alert": ch["channel_alert"],
            "channel_alert_mask": ch["channel_alert_mask"],
            "rtd_excitation_ua": ch["rtd_excitation_ua"],
        })
    return {
        "spi_ok": device.spi_ok,
        "die_temp_c": device.die_temp_c,
        "alert_status": device.alert_status,
        "alert_mask": device.alert_mask,
        "supply_alert_status": device.supply_alert_status,
        "supply_alert_mask": device.supply_alert_mask,
        "live_status": device.live_status,
        "channels": channels,
        "diagnostics": [],
        "mux_states": list(getattr(device, "mux_states", [0, 0, 0, 0])[:4]),
    }


def _faults_dict(device) -> dict:
    channels = []
    for ch in device.channels:
        channels.append({
            "id": ch["id"],
            "alert": ch["channel_alert"],
            "mask": ch["channel_alert_mask"],
        })
    return {
        "alert_status": device.alert_status,
        "alert_mask": device.alert_mask,
        "supply_alert_status": device.supply_alert_status,
        "supply_alert_mask": device.supply_alert_mask,
        "channels": channels,
    }
