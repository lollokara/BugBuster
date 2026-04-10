/**
 * LA 10M-sample rendering performance tests.
 *
 * Rendering pipeline under test:
 *   wheel event → set_view_start/set_view_end (Leptos signals)
 *     → Effect calls la_get_view (async, instant in mock)
 *       → view_data signal updated
 *         → canvas Effect redraws
 *
 * Canvas frame detection: every redraw begins with fillRect(0, 0, w, h) using
 * the background colour #060a14.  We intercept HTMLCanvasElement.prototype.getContext
 * in an addInitScript (runs before Leptos mounts) and patch fillRect on the returned
 * 2D context to count "frame starts" whenever x === 0 && y === 0.
 *
 * 10M-sample mock: la_get_capture_info returns totalSamples = 10_000_000.
 * la_get_view returns 500 dense transitions per channel to stress the renderer.
 */

import { test as base, expect } from '../fixtures'

// Extend the standard fixture to inject the canvas frame counter BEFORE Leptos
// creates the 2D context.  The parent fixture already injects tauriMock.
const test = base.extend<{ page: any }>({
  page: async ({ page }, use) => {
    await page.addInitScript(() => {
      ;(window as any).__la_frames = { count: 0, timestamps: [] as number[] }

      const origGetCtx = HTMLCanvasElement.prototype.getContext
      HTMLCanvasElement.prototype.getContext = function (type: string, ...rest: any[]) {
        const ctx = (origGetCtx as any).call(this, type, ...rest)
        if (type === '2d' && ctx && !(ctx as any).__la_patched) {
          ;(ctx as any).__la_patched = true
          const orig = ctx.fillRect.bind(ctx)
          ctx.fillRect = function (x: number, y: number, w: number, h: number) {
            // Every canvas redraw starts with fillRect(0, 0, fullW, fullH)
            if (x === 0 && y === 0) {
              const f = (window as any).__la_frames
              f.count++
              f.timestamps.push(performance.now())
            }
            return orig(x, y, w, h)
          }
        }
        return ctx
      }
    })
    await use(page)
  },
})

// ─── helpers ─────────────────────────────────────────────────────────────────

async function goToLaTab(page: any) {
  await page.goto('/')
  await page.locator('.connection-panel').waitFor({ state: 'visible', timeout: 10000 })
  await page.evaluate(() => {
    ;(window as any).__tauriMockFire('connection-status', { mode: 'USB', port_or_url: '/dev/mockUSB' })
  })
  await page.locator('nav.tab-bar').waitFor({ state: 'visible', timeout: 5000 })
  await page.locator('button.tab-item', { hasText: 'Logic Analyzer' }).click()
  await page.locator('canvas').waitFor({ state: 'visible', timeout: 5000 })
  await page.waitForTimeout(400)
}

/**
 * Override la_get_capture_info and la_get_view to serve a 10M-sample capture
 * with 500 dense transitions per channel per viewport.
 */
async function injectLargeCaptureMock(page: any) {
  await page.evaluate(() => {
    const TEN_M = 10_000_000
    const original = (window as any).__TAURI__.core.invoke

    ;(window as any).__TAURI__.core.invoke = async (cmd: string, args: any) => {
      if (cmd === 'la_get_capture_info') {
        return {
          channels: 4,
          sampleRateHz: 100_000_000,
          totalSamples: TEN_M,
          durationSec: 0.1,
          triggerSample: TEN_M / 2,
        }
      }

      if (cmd === 'la_get_view') {
        const { startSample, endSample } = args
        const span = Math.max(endSample - startSample, 1)
        const N = 500
        const step = Math.floor(span / N)

        const mkTrans = (phase: number): [number, number][] =>
          Array.from({ length: N }, (_, i) => [startSample + i * step + phase, i % 2])

        return {
          channels: 4,
          sampleRateHz: 100_000_000,
          totalSamples: TEN_M,
          viewStart: startSample,
          viewEnd: endSample,
          triggerSample: TEN_M / 2,
          channelTransitions: [mkTrans(0), mkTrans(1), mkTrans(2), mkTrans(3)],
          density: Array.from({ length: 200 }, (_, i) =>
            Math.floor(50 + 50 * Math.sin(i / 20))),
          decimated: true,
        }
      }

      return original(cmd, args)
    }
  })
}

/**
 * Fire la-done and wait up to 3 s for the first canvas frame to appear.
 * Returns elapsed ms from event dispatch to first canvas render.
 */
async function loadCapture(page: any): Promise<number> {
  await page.evaluate(() => {
    ;(window as any).__la_frames = { count: 0, timestamps: [] }
  })
  const t0 = Date.now()
  await page.evaluate(() => { ;(window as any).__tauriMockFire('la-done', {}) })

  for (let i = 0; i < 60; i++) {
    await page.waitForTimeout(50)
    const n: number = await page.evaluate(() => (window as any).__la_frames.count)
    if (n > 0) break
  }

  return Date.now() - t0
}

// ─── tests ───────────────────────────────────────────────────────────────────

test.describe('LA – 10 M sample rendering performance', () => {

  test('initial render of 10 M sample capture completes within 2 s', async ({ page }) => {
    await goToLaTab(page)
    await injectLargeCaptureMock(page)

    const renderMs = await loadCapture(page)
    console.log(`[perf] initial render: ${renderMs} ms`)

    // Tightened 2026-04-10. Best observed: 65ms.
    expect(renderMs, `Initial canvas render took ${renderMs} ms — limit is 1000 ms`).toBeLessThan(1000)
  })

  test('canvas re-renders after each zoom scroll event', async ({ page }) => {
    await goToLaTab(page)
    await injectLargeCaptureMock(page)
    await loadCapture(page)

    const box = await page.locator('canvas').boundingBox()
    const cx = box!.x + box!.width / 2
    const cy = box!.y + box!.height / 2

    await page.evaluate(() => { ;(window as any).__la_frames = { count: 0, timestamps: [] } })
    await page.mouse.move(cx, cy)

    for (let i = 0; i < 10; i++) {
      await page.mouse.wheel(0, -120) // zoom in
      await page.waitForTimeout(100)
    }

    const frames: number = await page.evaluate(() => (window as any).__la_frames.count)
    console.log(`[perf] frames for 10 scroll events: ${frames}`)

    // Each scroll triggers an async la_get_view → canvas redraw cycle;
    // with 100 ms between events most should complete.
    // Tightened 2026-04-10. Best observed: 33 frames.
    expect(frames, `Expected ≥10 renders for 10 scroll events, got ${frames}`).toBeGreaterThanOrEqual(10)
  })

  test('sustained scrolling over 2 s maintains ≥ 10 fps (browser timestamps)', async ({ page }) => {
    await goToLaTab(page)
    await injectLargeCaptureMock(page)
    await loadCapture(page)

    const box = await page.locator('canvas').boundingBox()
    await page.mouse.move(box!.x + box!.width / 2, box!.y + box!.height / 2)

    await page.evaluate(() => { ;(window as any).__la_frames = { count: 0, timestamps: [] } })

    // 40 scroll events at 50 ms intervals = ~2 s, alternating zoom direction
    for (let i = 0; i < 40; i++) {
      await page.mouse.wheel(0, i % 2 === 0 ? -120 : 120)
      await page.waitForTimeout(50)
    }
    await page.waitForTimeout(300) // flush in-flight renders

    const timestamps: number[] = await page.evaluate(() => (window as any).__la_frames.timestamps)
    const frames = timestamps.length

    // Use browser-side performance.now() timestamps to calculate fps, avoiding
    // Playwright CDP round-trip overhead that inflates wall-clock elapsed time.
    const renderDurationMs = timestamps.length >= 2
      ? timestamps[timestamps.length - 1] - timestamps[0]
      : 1
    const fps = frames >= 2 ? ((frames - 1) / renderDurationMs) * 1000 : 0

    console.log(`[perf] sustained: ${frames} frames over ${renderDurationMs.toFixed(0)} ms (browser) → ${fps.toFixed(1)} fps`)
    expect(fps, `Rendering fps ${fps.toFixed(1)} is below 10 fps minimum`).toBeGreaterThan(10)
  })

  test('burst of 20 rapid scroll events: last render within 1.5 s of burst start', async ({ page }) => {
    await goToLaTab(page)
    await injectLargeCaptureMock(page)
    await loadCapture(page)

    const box = await page.locator('canvas').boundingBox()
    await page.mouse.move(box!.x + box!.width / 2, box!.y + box!.height / 2)

    await page.evaluate(() => { ;(window as any).__la_frames = { count: 0, timestamps: [] } })

    // Record browser timestamp just before the burst (avoids CDP latency in measurement)
    const burstStartTs: number = await page.evaluate(() => performance.now())

    for (let i = 0; i < 20; i++) {
      await page.mouse.wheel(0, -120)
    }
    await page.waitForTimeout(800) // drain the async render pipeline

    const { frames, timestamps } = await page.evaluate(() => ({
      frames: (window as any).__la_frames.count as number,
      timestamps: (window as any).__la_frames.timestamps as number[],
    }))

    console.log(`[perf] burst: ${frames} frames rendered`)
    expect(frames, 'Canvas must render at least once after scroll burst').toBeGreaterThanOrEqual(1)

    // Time from burst start (browser clock) to last canvas render
    const lastRenderDelay = timestamps[timestamps.length - 1] - burstStartTs
    console.log(`[perf] burst: last render at +${lastRenderDelay.toFixed(0)} ms from burst start`)
    // Tightened 2026-04-10. Best observed: 340ms.
    expect(lastRenderDelay, `Last render took ${lastRenderDelay.toFixed(0)} ms — limit is 1200 ms`).toBeLessThan(1200)
  })
})
