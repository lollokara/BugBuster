import { test, expect } from '../fixtures'

test.describe('Connection Screen', () => {
  test('shows the connection panel card', async ({ page }) => {
    await page.goto('/')
    const panel = page.locator('.connection-panel')
    await panel.waitFor({ state: 'visible', timeout: 5000 })
    await expect(panel).toBeVisible()
  })

  test('shows the "Connect to BugBuster" heading', async ({ page }) => {
    await page.goto('/')
    const heading = page.locator('.connection-panel h2')
    await heading.waitFor({ state: 'visible', timeout: 5000 })
    await expect(heading).toHaveText('Connect to BugBuster')
  })

  test('shows the scanning hint text', async ({ page }) => {
    await page.goto('/')
    const hint = page.locator('.connection-panel p.hint')
    await hint.waitFor({ state: 'visible', timeout: 5000 })
    await expect(hint).toContainText('USB')
  })

  test('shows the Scan for Devices button', async ({ page }) => {
    await page.goto('/')
    const scanBtn = page.locator('button.btn.btn-primary')
    await scanBtn.waitFor({ state: 'visible', timeout: 5000 })
    await expect(scanBtn).toHaveText('Scan for Devices')
  })

  test('Scan button is enabled initially', async ({ page }) => {
    await page.goto('/')
    const scanBtn = page.locator('button.btn.btn-primary')
    await scanBtn.waitFor({ state: 'visible', timeout: 5000 })
    await expect(scanBtn).toBeEnabled()
  })

  test('shows "Scanning..." while scan is in progress', async ({ page }) => {
    await page.goto('/')
    const scanBtn = page.locator('button.btn.btn-primary')
    await scanBtn.waitFor({ state: 'visible', timeout: 5000 })
    await scanBtn.click()

    // While scanning, the button text should change
    await expect(scanBtn).toHaveText(/Scanning\.\.\.|Scan for Devices/, { timeout: 5000 })
    // Wait for scan to complete
    await expect(scanBtn).toHaveText('Scan for Devices', { timeout: 15000 })
  })

  test('shows the device list container', async ({ page }) => {
    await page.goto('/')
    const list = page.locator('div.device-list')
    await list.waitFor({ state: 'attached', timeout: 5000 })
    await expect(list).toHaveCount(1)
  })
})
