// playwright.config.ts
import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './tests',
  use: {
    baseURL: 'http://localhost:8080',
    headless: false, // change to true if you don’t want the browser window
  },
  retries: 0,
  timeout: 30000,
});