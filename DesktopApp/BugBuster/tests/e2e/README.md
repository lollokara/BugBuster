# BugBuster Desktop App — E2E Tests

End-to-end tests for the BugBuster Tauri desktop app using [WebDriverIO](https://webdriver.io/) and [tauri-driver](https://crates.io/crates/tauri-driver).

## Prerequisites

### 1. Build the app

```bash
cd DesktopApp/BugBuster
cargo build
```

### 2. Install tauri-driver

```bash
cargo install tauri-driver
```

### 3. Install test dependencies

```bash
cd DesktopApp/BugBuster/tests/e2e
npm install
```

### Platform requirements

| Platform | WebDriver backend | Additional setup |
|----------|------------------|-----------------|
| **macOS** | WebKitWebDriver (bundled) | None |
| **Linux** | WebKitWebDriver | `sudo apt-get install webkit2gtk-driver` |
| **Windows** | Microsoft Edge WebDriver | Must match installed Edge version |

## Running Tests

```bash
cd DesktopApp/BugBuster/tests/e2e

# Run all tests
npm test

# Run a single spec
npx wdio run wdio.conf.ts --spec specs/01_app_launch.spec.ts
```

## Test Categories

| Spec File | What it Tests |
|-----------|--------------|
| `01_app_launch.spec.ts` | Window title, header logo, connection status indicator |
| `02_connection_screen.spec.ts` | Connection panel, scan button, device list |
| `03_toast_system.spec.ts` | Toast notification container |

## Architecture

- **`wdio.conf.ts`** — WebDriverIO configuration; starts/stops `tauri-driver` subprocess
- **`specs/`** — Test specs using Mocha BDD syntax (`describe`/`it`)
- Tests run against the **real compiled app binary** (no mocks at the WebDriver level)
- Tests currently cover the **disconnected state** only (no device required)

## Adding Tests for Connected State

To test the connected UI (tabs, channel controls, etc.), you need a BugBuster device connected. Add a `before` hook in `wdio.conf.ts` or per-spec setup that:

1. Injects a mock serial endpoint, OR
2. Uses a real device and sets `BUGBUSTER_USB_PORT` / `BUGBUSTER_HTTP_IP` env vars

Example mock approach (future work):
```typescript
// In spec beforeEach:
await browser.execute(() => {
  // Override __TAURI__.core.invoke to return mock data
  window.__TAURI__.core.invoke = async (cmd, args) => { ... }
})
```
