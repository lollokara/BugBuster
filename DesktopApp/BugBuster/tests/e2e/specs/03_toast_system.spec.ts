describe('Toast System', () => {
  it('renders the toast container', async () => {
    const container = await $('div.toast-container')
    await container.waitForExisting({ timeout: 5000 })
    expect(await container.isExisting()).toBe(true)
  })

  it('has no toasts visible on startup', async () => {
    // Give any startup animations time to settle
    await browser.pause(500)
    const container = await $('div.toast-container')
    const toasts = await container.$$('div.toast')
    expect(toasts.length).toBe(0)
  })
})
