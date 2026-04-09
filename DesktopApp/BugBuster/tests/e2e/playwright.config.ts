import { defineConfig, devices } from '@playwright/test'
import * as path from 'path'

export default defineConfig({
  testDir: './specs',
  fullyParallel: false,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  workers: 1,
  reporter: [['list']],

  use: {
    baseURL: 'http://localhost:1420',
    trace: 'on-first-retry',
    // Mocking Tauri APIs for hardware-independent E2E tests.
    // Use the fixture from ./fixtures.ts in your tests to automatically inject the mock.
  },

  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
  ],

  webServer: {
    command: `cd ${path.resolve(__dirname, '../..')} && trunk serve`,
    url: 'http://localhost:1420',
    reuseExistingServer: !process.env.CI,
    timeout: 60000,
  },
})
