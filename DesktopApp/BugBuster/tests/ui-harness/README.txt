BugBuster UI harness: mock Tauri IPC + screenshot/report tool (no hardware needed)

Prerequisites
  - Node 18+.
  - The frontend dev server running for this worktree, e.g. trunk serve --port 1431
    (serves http://localhost:1431). Do not start a second one.
  - One-time: npm install && npx playwright install chromium

Run (from this folder)
  node shoot.mjs --hat daq --theme dark
  node shoot.mjs --hat la --theme light
  node shoot.mjs --hat none --theme both --only overview,hat --out shots

Options
  --url    dev server URL (default http://localhost:1431)
  --hat    daq | la | none         which HAT the mock device reports (default daq)
  --theme  light | dark | both     sets the browser colorScheme (default dark)
  --only   comma separated view ids (connect, overview, board, voltages, faults, diag,
           adc, vdac, idac, iin, gpio, din, dout, hv_io, ioexp, scope, la, daq,
           wavegen, sigpath, hat, usbpd, uart)
  --out    output folder (default shots, git-ignored)

Output
  <out>/<hat>-<theme>/<NN>-<view>.png   1440x900 screenshots (00-connect is the disconnected screen)
  <out>/report.json                     per view: console errors/warnings, page errors,
                                        unhandled mock commands, empty-content flag
  A compact table is printed; the exit code is always 0.

Files
  mock_ipc.js   installs window.__TAURI__ (invoke + event.listen) before the app loads.
                Scenario comes from window.__BB_MOCK = { hat, connected, theme }.
                Debug handles: window.__mockLog, window.__mockUnhandled, window.__mock.
  shoot.mjs     Playwright driver (connects to the mock device, visits every view).

Notes
  - Navigation uses [data-nav-view="<id>"] when present, otherwise the .category-item and
    .tab-item markup.
  - Trunk watches the whole project, so PNGs are written in one burst at the end of each run and
    its livereload socket is disabled inside the page; otherwise the page would reload mid-run.
  - Scope is started and the LA capture is armed by the script so those views show data.
