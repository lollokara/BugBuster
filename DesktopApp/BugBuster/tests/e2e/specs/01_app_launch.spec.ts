describe('App Launch', () => {
  it('opens a window with the correct title', async () => {
    const title = await browser.getTitle()
    expect(title).toBe('BugBuster')
  })

  it('renders the BugBuster logo in the header', async () => {
    const logo = await $('span.logo-text')
    await logo.waitForDisplayed({ timeout: 5000 })
    const text = await logo.getText()
    expect(text).toBe('BugBuster')
  })

  it('renders the subtitle in the header', async () => {
    const subtitle = await $('span.subtitle')
    await subtitle.waitForDisplayed({ timeout: 5000 })
    const text = await subtitle.getText()
    expect(text).toContain('AD74416H')
  })

  it('shows the disconnected status indicator', async () => {
    const dot = await $('span.status-dot.disconnected')
    await dot.waitForDisplayed({ timeout: 5000 })
    expect(await dot.isDisplayed()).toBe(true)
  })

  it('shows "Disconnected" status text', async () => {
    const statusText = await $('span.status-text')
    await statusText.waitForDisplayed({ timeout: 5000 })
    const text = await statusText.getText()
    expect(text).toBe('Disconnected')
  })

  it('does not show the tab bar when disconnected', async () => {
    const tabBar = await $('nav.tab-bar')
    // Tab bar should not be visible / not exist when disconnected
    const exists = await tabBar.isExisting()
    expect(exists).toBe(false)
  })
})
