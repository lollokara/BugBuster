import { test, expect } from '@playwright/test'

test.describe('Toast System', () => {
  test('renders the toast container', async ({ page }) => {
    await page.goto('/')
    const container = page.locator('div.toast-container')
    await container.waitFor({ state: 'attached', timeout: 5000 })
    await expect(container).toHaveCount(1)
  })

  test('has no toasts visible on startup', async ({ page }) => {
    await page.goto('/')
    // Give any startup animations time to settle
    await page.waitForTimeout(500)
    const toasts = page.locator('div.toast-container div.toast')
    await expect(toasts).toHaveCount(0)
  })
})
