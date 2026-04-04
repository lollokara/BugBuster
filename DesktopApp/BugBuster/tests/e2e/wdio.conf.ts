import { spawn, ChildProcess } from 'child_process'
import * as os from 'os'
import * as path from 'path'
import * as fs from 'fs'

// Path to the compiled app binary
function getAppBinary(): string {
  const p = process.platform
  const base = path.resolve(__dirname, '..', '..', 'target', 'debug')
  if (p === 'win32') return path.join(base, 'bugbuster.exe')
  if (p === 'darwin') return path.join(base, 'bugbuster')
  return path.join(base, 'bugbuster')
}

// Path to tauri-driver (installed via: cargo install tauri-driver)
function getTauriDriverBin(): string {
  return path.join(os.homedir(), '.cargo', 'bin', 'tauri-driver')
}

let tauriDriver: ChildProcess | null = null

export const config: WebdriverIO.Config = {
  hostname: '127.0.0.1',
  port: 4444,

  specs: ['./specs/**/*.spec.ts'],
  exclude: [],
  maxInstances: 1,

  capabilities: [{
    maxInstances: 1,
    browserName: 'wry',
    'tauri:options': {
      application: getAppBinary(),
    },
    acceptInsecureCerts: true,
  }] as unknown as WebdriverIO.Capabilities[],

  logLevel: 'warn',
  bail: 0,
  baseUrl: 'http://localhost',
  waitforTimeout: 10000,
  connectionRetryTimeout: 30000,
  connectionRetryCount: 3,

  framework: 'mocha',
  reporters: [['spec', { addConsoleLogs: true }]],
  mochaOpts: {
    ui: 'bdd',
    timeout: 60000,
  },

  onPrepare() {
    const driverBin = getTauriDriverBin()
    if (!fs.existsSync(driverBin)) {
      throw new Error(
        `tauri-driver not found at ${driverBin}.\n` +
        `Install it with: cargo install tauri-driver`
      )
    }
    const appBin = getAppBinary()
    if (!fs.existsSync(appBin)) {
      throw new Error(
        `App binary not found at ${appBin}.\n` +
        `Build it with: cd DesktopApp/BugBuster && cargo build`
      )
    }

    tauriDriver = spawn(driverBin, [], {
      stdio: [null, process.stdout, process.stderr],
    })
    // Give the driver a moment to start
    return new Promise((resolve) => setTimeout(resolve, 1000))
  },

  onComplete() {
    if (tauriDriver) {
      tauriDriver.kill()
      tauriDriver = null
    }
  },
}
