# Task 4: Complete MUX Matrix Section Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the documentation for MUX Matrix commands 0x90–0x92 in `Firmware/BugBusterProtocol.md`.

**Architecture:** Update the existing placeholder section 6.15 with detailed command payloads and behavioral notes (safety dead-time).

**Tech Stack:** Markdown

---

### Task 1: Document 0x90 MUX_SET_ALL

**Files:**
- Modify: `Firmware/BugBusterProtocol.md:1652-1655`

- [ ] **Step 1: Replace placeholder with 0x90 documentation**

Replace:
```markdown
### 6.15 MUX Switch Matrix (0x90-0x92)

See existing documentation for 0x90 MUX_SET_ALL, 0x91 MUX_GET_ALL, 0x92 MUX_SET_SWITCH.
```

With:
```markdown
### 6.15 MUX Switch Matrix (0x90-0x92)

The MUX matrix consists of 4x ADGS2414D octal SPST switches daisy-chained on the SPI bus.

#### 0x90 MUX_SET_ALL
Set the state of all 32 switches in the matrix. To prevent signal contention and protect the level shifters, the firmware enforces a **100 ms safety dead-time** (all switches open) before applying the new state if any switch is being closed.

**Request payload:**
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | mux0 | u8 | State of MUX 0 (U10). Bit 0 = S1, Bit 7 = S8. |
| 1 | mux1 | u8 | State of MUX 1 (U11). Bit 0 = S1, Bit 7 = S8. |
| 2 | mux2 | u8 | State of MUX 2 (U16). Bit 0 = S1, Bit 7 = S8. |
| 3 | mux3 | u8 | State of MUX 3 (U17). Bit 0 = S1, Bit 7 = S8. |

**Response payload:** (empty)

**Web API equivalent:** `POST /api/mux/all` with body `{"states": [0, 0, 0, 0]}`
```

- [ ] **Step 2: Commit**

```bash
git add Firmware/BugBusterProtocol.md
git commit -m "docs: document MUX_SET_ALL command (0x90)"
```

### Task 2: Document 0x91 MUX_GET_ALL and 0x92 MUX_SET_SWITCH

**Files:**
- Modify: `Firmware/BugBusterProtocol.md` (append after 0x90)

- [ ] **Step 1: Add 0x91 and 0x92 documentation**

Append after 0x90:
```markdown

#### 0x91 MUX_GET_ALL
Read the current cached state of all 32 switches.

**Request payload:** (empty)

**Response payload:**
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | mux0 | u8 | State of MUX 0 (U10). |
| 1 | mux1 | u8 | State of MUX 1 (U11). |
| 2 | mux2 | u8 | State of MUX 2 (U16). |
| 3 | mux3 | u8 | State of MUX 3 (U17). |

**Web API equivalent:** `GET /api/mux/all`

#### 0x92 MUX_SET_SWITCH
Set the state of a single switch in the matrix. Enforces safety dead-time for the affected switch group.

**Request payload:**
| Offset | Field | Type | Description |
|--------|-------|------|-------------|
| 0 | device | u8 | ADGS2414D index (0-3). |
| 1 | switch | u8 | Switch index (0-7). |
| 2 | state | u8 | 0 = OPEN (Off), 1 = CLOSED (On). |

**Response payload:** (empty)

**Web API equivalent:** `POST /api/mux/switch` with body `{"device": 0, "switch": 2, "state": true}`
```

- [ ] **Step 2: Commit**

```bash
git add Firmware/BugBusterProtocol.md
git commit -m "docs: document MUX_GET_ALL and MUX_SET_SWITCH commands (0x91-0x92)"
```
