
import { test as base } from '@playwright/test';
import { tauriMock } from './tauri_mock';

export const test = base.extend({
  page: async ({ page }, use) => {
    // Inject Tauri mock before every test
    await page.addInitScript(tauriMock);
    await use(page);
  },
});

export { expect } from '@playwright/test';
