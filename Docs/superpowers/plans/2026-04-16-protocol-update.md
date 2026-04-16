# BugBuster Protocol Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Update `Firmware/BugBusterProtocol.md` to Version 1.6 (BBP v4) to synchronize documentation with firmware v1.2.1.

**Architecture:** Systematic documentation update. Each task corresponds to a logical section of the protocol specification.

**Tech Stack:** Markdown

---

### Task 1: Update Versioning and Transport Headers

**Files:**
- Modify: `Firmware/BugBusterProtocol.md:1-20`

- [ ] **Step 1: Update version number and status**
Change Version to 1.6 and BBP Protocol Version to 4.

- [ ] **Step 2: Commit**
```bash
git add Firmware/BugBusterProtocol.md
git commit -m "docs: update protocol version to 1.6 (BBP v4)"
```

### Task 2: Update Core Status Commands (0x01, 0xC5)

**Files:**
- Modify: `Firmware/BugBusterProtocol.md:200-400` (approximate lines for 6.2 and 6.13)

- [ ] **Step 1: Update 0x01 GET_STATUS payload**
Add MUX state bytes at the end of the response.
```markdown
Per diagnostic slot (4x, starting at offset 127, stride = 7 bytes):
...
MUX state (4 bytes, starting at offset 155):
+0-3    mux_state           u8[4]   Current state of 4 MUX devices (Bit 0 = S1)

Total response: 155 + 4 = 159 bytes
```

- [ ] **Step 2: Update 0xC5 HAT_GET_STATUS payload**
Reflect the expanded 34-byte response.
```markdown
0       detected        bool    HAT physically present (ADC)
1       connected       bool    UART communication established
2       type            u8      HAT type (0=none, 1=SWD/GPIO)
3       detect_voltage  f32     Raw ADC voltage on detect pin
7       fw_major        u8      HAT firmware version major
8       fw_minor        u8      HAT firmware version minor
9       confirmed       bool    Last config was acknowledged by HAT
10-13   ext_func        u8[4]   EXP_EXT_1 to EXP_EXT_4 functions
14      a_enabled       bool    Connector A power
15      a_current_ma    f32     Connector A current
19      a_fault         bool    Connector A fault
20      b_enabled       bool    Connector B power
21      b_current_ma    f32     Connector B current
25      b_fault         bool    Connector B fault
26      io_voltage_mv   u16     HVPAK IO voltage
28      hvpak_part      u8      HVPAK identity
29      hvpak_ready     bool    HVPAK mailbox ready
30      hvpak_last_err  u8      HVPAK error code
31      dap_connected   bool    CMSIS-DAP host connected
32      target_detect   bool    SWD target detected
33      target_dpidr    u32     Target DPIDR
```

- [ ] **Step 3: Commit**
```bash
git add Firmware/BugBusterProtocol.md
git commit -m "docs: expand GET_STATUS and HAT_GET_STATUS payloads"
```

### Task 3: Add New BBP Commands (0x74, 0xEB, 0xED, 0xEE)

**Files:**
- Modify: `Firmware/BugBusterProtocol.md` (various sections)

- [ ] **Step 1: Add 0x74 GET_ADMIN_TOKEN to System Commands**
```markdown
#### 0x74 GET_ADMIN_TOKEN
Retrieve the transient admin token for auth-protected REST endpoints.
**USB CDC #0 ONLY.**

**Request payload:** (empty)
**Response payload:**
0       len             u8      Token length
1..N    token           char[]  Admin token string
```

- [ ] **Step 2: Add HAT LA missing commands to 6.13c**
Add 0xEB (LOG_ENABLE), 0xED (USB_RESET), 0xEE (STREAM_START).

- [ ] **Step 3: Commit**
```bash
git add Firmware/BugBusterProtocol.md
git commit -m "docs: add GET_ADMIN_TOKEN and LA control commands"
```

### Task 4: Complete MUX Matrix Section (0x90–0x92)

**Files:**
- Modify: `Firmware/BugBusterProtocol.md:1800-1820` (approximate lines for 6.15)

- [ ] **Step 1: Document 0x90 MUX_SET_ALL**
Define the 4-byte payload and the 100ms safety dead-time.

- [ ] **Step 2: Document 0x91 MUX_GET_ALL and 0x92 MUX_SET_SWITCH**
Define payloads.

- [ ] **Step 3: Commit**
```bash
git add Firmware/BugBusterProtocol.md
git commit -m "docs: document MUX Matrix commands 0x90-0x92"
```

### Task 5: Update Enums, Errors, and Events

**Files:**
- Modify: `Firmware/BugBusterProtocol.md` (Tables and Events sections)

- [ ] **Step 1: Update HatPinFunction**
Mark 1–4 as RESERVED.

- [ ] **Step 2: Add 0xEC LA_LOG Event**
```markdown
#### 0xEC LA_LOG (Event)
Unsolicited log message from RP2040 HAT.
0       len             u8      Message length
1..N    msg             char[]  Log message
```

- [ ] **Step 3: Update Error Codes table**
Add 0x0B–0x10 (HVPAK) and 0x11 (TIMEOUT).

- [ ] **Step 4: Commit**
```bash
git add Firmware/BugBusterProtocol.md
git commit -m "docs: update enums, errors, and add LA_LOG event"
```

### Task 6: Update REST API Section

**Files:**
- Modify: `Firmware/BugBusterProtocol.md` (End of file)

- [ ] **Step 1: Add GET /api/debug**
Document the combined I2C status object.

- [ ] **Step 2: Document /api/mux endpoints**
Add `GET /api/mux` and `POST /api/mux/set`.

- [ ] **Step 3: Commit**
```bash
git add Firmware/BugBusterProtocol.md
git commit -m "docs: add /api/debug and /api/mux REST endpoints"
```
