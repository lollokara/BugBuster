import { test, expect } from '../fixtures'

test.describe('App Launch', () => {
  test('renders the BugBuster logo in the header', async ({ page }) => {
    await page.goto('/')
    const logo = page.locator('span.logo-text')
    await logo.waitFor({ state: 'visible', timeout: 5000 })
    await expect(logo).toHaveText('BugBuster')
  })

  test('renders the subtitle in the header', async ({ page }) => {
    await page.goto('/')
    const subtitle = page.locator('span.subtitle')
    await subtitle.waitFor({ state: 'visible', timeout: 5000 })
    await expect(subtitle).toContainText('AD74416H')
  })

  test('shows the disconnected status indicator', async ({ page }) => {
    await page.goto('/')
    const dot = page.locator('span.status-dot.disconnected')
    await dot.waitFor({ state: 'visible', timeout: 5000 })
    await expect(dot).toBeVisible()
  })

  test('shows "Disconnected" status text', async ({ page }) => {
    await page.goto('/')
    const statusText = page.locator('span.status-text')
    await statusText.waitFor({ state: 'visible', timeout: 5000 })
    await expect(statusText).toHaveText('Disconnected')
  })

  test('does not show the tab bar when disconnected', async ({ page }) => {
    await page.goto('/')
    await expect(page.locator('nav.tab-bar')).toHaveCount(0)
  })
})
