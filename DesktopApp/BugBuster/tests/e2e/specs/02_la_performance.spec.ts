import { test, expect } from '../fixtures'

// Fire connection-status only after the app has mounted and registered its Tauri event listeners.
// We wait for the connection panel (proof the WASM app is live) before dispatching the mock event.
// After clicking the LA tab we also wait for the canvas and allow time for the initial la_get_view
// async response + Leptos reactive updates to settle — otherwise Playwright can hit elements mid-re-render.
async function goToLaTab(page: any) {
  await page.goto('/')
  await page.locator('.connection-panel').waitFor({ state: 'visible', timeout: 10000 })
  await page.evaluate(() => {
    (window as any).__tauriMockFire('connection-status', { mode: 'USB', port_or_url: '/dev/mockUSB' })
  })
  await page.locator('nav.tab-bar').waitFor({ state: 'visible', timeout: 5000 })
  await page.locator('button.tab-item', { hasText: 'Logic Analyzer' }).click()
  // Wait for canvas (confirms LA component mounted) then settle reactive updates
  await page.locator('canvas').waitFor({ state: 'visible', timeout: 5000 })
  await page.waitForTimeout(400)
}

// Buttons with title= attributes don't resolve correctly via getByRole in Playwright's
// aria name computation. Use locator + filter({ hasText: regex }) for exact text matching.
function btn(page: any, text: RegExp) {
  return page.locator('button').filter({ hasText: text })
}

test.describe('LA Tab', () => {
  test('tab bar appears when connected', async ({ page }) => {
    await page.goto('/')
    await page.locator('.connection-panel').waitFor({ state: 'visible', timeout: 10000 })
    await page.evaluate(() => {
      (window as any).__tauriMockFire('connection-status', { mode: 'USB', port_or_url: '/dev/mockUSB' })
    })
    const tabBar = page.locator('nav.tab-bar')
    await tabBar.waitFor({ state: 'visible', timeout: 5000 })
    await expect(tabBar).toBeVisible()
  })

  test('Logic Analyzer tab button is present in the tab bar', async ({ page }) => {
    await page.goto('/')
    await page.locator('.connection-panel').waitFor({ state: 'visible', timeout: 10000 })
    await page.evaluate(() => {
      (window as any).__tauriMockFire('connection-status', { mode: 'USB', port_or_url: '/dev/mockUSB' })
    })
    await page.locator('nav.tab-bar').waitFor({ state: 'visible', timeout: 5000 })
    await expect(page.locator('button.tab-item', { hasText: 'Logic Analyzer' })).toBeVisible()
  })

  test('navigating to LA tab shows the tab content', async ({ page }) => {
    await goToLaTab(page)
    await expect(page.locator('.tab-content')).toBeVisible()
  })

  test('waveform canvas is rendered', async ({ page }) => {
    await goToLaTab(page)
    await expect(page.locator('canvas')).toBeVisible()
  })

  test('channel toggle buttons CH0–CH3 are all shown', async ({ page }) => {
    await goToLaTab(page)
    for (const ch of [/^CH0$/, /^CH1$/, /^CH2$/, /^CH3$/]) {
      await expect(btn(page, ch)).toBeVisible()
    }
  })

  test('sample rate pill buttons are rendered', async ({ page }) => {
    await goToLaTab(page)
    // lowercase k distinguishes rate pills from depth pills (uppercase K)
    for (const label of [/^100k$/, /^1M$/, /^10M$/, /^100M$/]) {
      await expect(btn(page, label)).toBeVisible()
    }
  })

  test('memory depth pill buttons are rendered', async ({ page }) => {
    await goToLaTab(page)
    // uppercase K distinguishes depth pills from rate pills (lowercase k)
    for (const label of [/^10K$/, /^50K$/, /^100K$/, /^500K$/]) {
      await expect(btn(page, label)).toBeVisible()
    }
  })

  test('capture mode buttons are rendered', async ({ page }) => {
    await goToLaTab(page)
    await expect(btn(page, /^Memory$/)).toBeVisible()
    await expect(btn(page, /^Stream$/)).toBeVisible()
    await expect(btn(page, /^RLE$/)).toBeVisible()
  })

  test('control buttons Arm / Force / Stop / Read / Clear are rendered', async ({ page }) => {
    await goToLaTab(page)
    for (const name of [/^Arm$/, /^Force$/, /^Stop$/, /^Read$/, /^Clear$/]) {
      await expect(btn(page, name)).toBeVisible()
    }
  })

  test('clicking a sample rate pill makes it active (blue style)', async ({ page }) => {
    await goToLaTab(page)
    // 10M is inactive by default (default rate = 1M) — clicking sets the inline style to blue
    const pill10M = btn(page, /^10M$/)
    await pill10M.click()
    // Check the inline style attribute directly to avoid CSS transition timing issues
    await expect(pill10M).toHaveAttribute('style', /background: #3b82f6/)
  })

  test('clicking a memory depth pill makes it active (green style)', async ({ page }) => {
    await goToLaTab(page)
    // 500K is inactive by default (default depth = 100K)
    const pill500K = btn(page, /^500K$/)
    await pill500K.click()
    await expect(pill500K).toHaveAttribute('style', /background: #10b981/)
  })

  test('switching to Stream mode removes high-rate sample options', async ({ page }) => {
    await goToLaTab(page)
    // 100M is present in Memory mode
    await expect(btn(page, /^100M$/)).toBeVisible()
    // Switching to Stream re-renders rate pills — DOM update is reactive (Leptos)
    await btn(page, /^Stream$/).click()
    // 100M is hidden in Stream mode (rate capped at 2M)
    await expect(btn(page, /^100M$/)).not.toBeVisible()
  })

  test('switching back to Memory mode restores high-rate options', async ({ page }) => {
    await goToLaTab(page)
    await btn(page, /^Stream$/).click()
    await expect(btn(page, /^100M$/)).not.toBeVisible()
    await btn(page, /^Memory$/).click()
    await expect(btn(page, /^100M$/)).toBeVisible()
  })

  test('decoder panel opens when Show is clicked', async ({ page }) => {
    await goToLaTab(page)
    const toggle = page.locator('button', { hasText: 'Show' })
    await toggle.waitFor({ state: 'visible', timeout: 3000 })
    await toggle.click()
    await expect(page.locator('button', { hasText: 'Hide' })).toBeVisible()
  })

  test('decoder panel closes when Hide is clicked', async ({ page }) => {
    await goToLaTab(page)
    await page.locator('button', { hasText: 'Show' }).click()
    await page.locator('button', { hasText: 'Hide' }).click()
    await expect(page.locator('button', { hasText: 'Show' })).toBeVisible()
  })
})
