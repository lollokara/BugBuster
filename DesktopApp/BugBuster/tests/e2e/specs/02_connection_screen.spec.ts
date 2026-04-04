describe('Connection Screen', () => {
  it('shows the connection panel card', async () => {
    const panel = await $('.connection-panel')
    await panel.waitForDisplayed({ timeout: 5000 })
    expect(await panel.isDisplayed()).toBe(true)
  })

  it('shows the "Connect to BugBuster" heading', async () => {
    const heading = await $('.connection-panel h2')
    await heading.waitForDisplayed({ timeout: 5000 })
    const text = await heading.getText()
    expect(text).toBe('Connect to BugBuster')
  })

  it('shows the scanning hint text', async () => {
    const hint = await $('.connection-panel p.hint')
    await hint.waitForDisplayed({ timeout: 5000 })
    const text = await hint.getText()
    expect(text).toContain('USB')
  })

  it('shows the Scan for Devices button', async () => {
    const scanBtn = await $('button.btn.btn-primary')
    await scanBtn.waitForDisplayed({ timeout: 5000 })
    const text = await scanBtn.getText()
    expect(text).toBe('Scan for Devices')
  })

  it('Scan button is enabled initially', async () => {
    const scanBtn = await $('button.btn.btn-primary')
    await scanBtn.waitForEnabled({ timeout: 5000 })
    expect(await scanBtn.isEnabled()).toBe(true)
  })

  it('shows "Scanning..." while scan is in progress', async () => {
    const scanBtn = await $('button.btn.btn-primary')
    await scanBtn.click()

    // While scanning, the button text should change
    await browser.waitUntil(
      async () => {
        const text = await scanBtn.getText()
        return text === 'Scanning...' || text === 'Scan for Devices'
      },
      { timeout: 5000, interval: 200 },
    )
    // Wait for scan to complete
    await browser.waitUntil(
      async () => (await scanBtn.getText()) === 'Scan for Devices',
      { timeout: 15000, interval: 500 },
    )
    expect(await scanBtn.getText()).toBe('Scan for Devices')
  })

  it('shows the device list container', async () => {
    const list = await $('div.device-list')
    await list.waitForExisting({ timeout: 5000 })
    expect(await list.isExisting()).toBe(true)
  })
})
