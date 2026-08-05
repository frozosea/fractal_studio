import { defineConfig, devices } from "@playwright/test";

export default defineConfig({
  testDir: "./tests/e2e",
  timeout: 45_000,
  fullyParallel: false,
  reporter: "list",
  use: {
    baseURL: process.env.E2E_BASE_URL ?? "http://localhost:3010",
    trace: "retain-on-failure",
  },
  projects: [
    // The desktop project owns the full journey; the mobile one only runs the
    // layout checks, which is where a small viewport actually tells you
    // something the desktop run cannot.
    { name: "chromium", use: { ...devices["Desktop Chrome"], channel: "chrome" }, testIgnore: /\.mobile\.spec\.ts$/ },
    { name: "mobile", use: { ...devices["Pixel 5"], channel: "chrome", locale: "zh-CN" }, testMatch: /\.mobile\.spec\.ts$/ },
  ],
});
