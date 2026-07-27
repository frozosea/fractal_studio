import { expect, test } from "@playwright/test";

test("browser registers through Platform and explores an interactive real Compute Studio", async ({ page }) => {
  const forbiddenApiRequests: string[] = [];
  const favoriteRequests: string[] = [];
  const previewScales: number[] = [];
  const previewSpecs: Array<Record<string, unknown>> = [];
  page.on("request", (request) => {
    const { pathname } = new URL(request.url());
    if (pathname.startsWith("/api/")) forbiddenApiRequests.push(request.url());
    if (pathname === "/platform/v1/me/favorites") favoriteRequests.push(request.url());
  });
  page.on("response", async (response) => {
    if (new URL(response.url()).pathname === "/platform/v1/studio/preview" && response.request().method() === "POST") {
      const spec = (await response.request().postDataJSON()).canonicalSpec as Record<string, unknown>;
      previewSpecs.push(spec);
      previewScales.push(Number(spec.scale));
    }
  });

  await page.goto("/en/register");
  await page.getByPlaceholder("Email").fill(`browser-${Date.now()}@example.test`);
  await page.getByPlaceholder("Password", { exact: true }).fill("browser-test-password");
  await page.getByPlaceholder("Confirm password").fill("browser-test-password");
  await page.getByRole("button", { name: "Create account" }).click();
  await page.waitForURL(/\/studio$/, { timeout: 30_000 });
  await page.goto("/payouts");
  const creatorHandle = `e2e${Date.now()}`;
  await page.getByPlaceholder("handle (lowercase, e.g. fractal_artist)").fill(creatorHandle);
  await page.getByPlaceholder("display name").fill("E2E Creator");
  await page.getByRole("button", { name: "Save creator profile" }).click();
  await expect(page.getByText("Available to withdraw")).toBeVisible({ timeout: 30_000 });
  await expect(page.getByText("0.00 CNY")).toBeVisible();
  await expect(page.getByRole("heading", { name: "Payout history" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Clear filters" })).toBeVisible();
  await expect(page.getByRole("button", { name: "Request payout" })).toBeDisabled();
  await page.getByRole("link", { name: "Studio" }).click();
  await page.waitForURL(/\/studio$/, { timeout: 30_000 });
  await expect(page.locator("main").getByRole("heading", { name: "Fractal Studio" })).toBeVisible();
  await expect(page.getByRole("link", { name: "Finance" })).toHaveCount(0);
  await page.goto("/finance");
  await page.waitForURL(/\/studio$/, { timeout: 30_000 });
  await expect(page.getByText("drag: pan · wheel/double-click: zoom")).toBeVisible();
  await expect(page.getByAltText("Fractal map preview")).toBeVisible({ timeout: 30_000 });

  await expect.poll(() => previewScales.length).toBeGreaterThan(0);
  const initialScale = previewScales.at(-1)!;
  await page.getByRole("button", { name: "Zoom in", exact: true }).click();
  await expect.poll(() => previewScales.some((scale) => scale < initialScale)).toBe(true);

  await page.getByRole("button", { name: "Julia", exact: true }).click();
  await expect.poll(() => previewSpecs.some((spec) => spec.julia === true)).toBe(true);
  await page.getByRole("button", { name: "Pair transition", exact: true }).click();
  await expect.poll(() => previewSpecs.some((spec) => spec.transitionMode === "pair")).toBe(true);
  await page.getByRole("button", { name: "Formula", exact: true }).click();
  await page.getByLabel("Orbit formula").fill("z*z*z+c");
  await expect.poll(() => previewSpecs.some((spec) => (spec.orbitProgram as { type?: string } | undefined)?.type === "formula")).toBe(true);
  await page.getByRole("button", { name: "Map", exact: true }).click();
  await page.getByLabel("Dynamic coloring").selectOption("eq_full");
  await expect.poll(() => previewSpecs.some((spec) => spec.colorMode === "eq_full")).toBe(true);
  await page.getByLabel("Custom gradient").check();
  await expect(page.locator('input[type="color"]')).toHaveCount(5);

  await page.goto("/zh/studio");
  await expect(page.locator("main").getByRole("heading", { name: "分形工作室" })).toBeVisible();
  await expect(page.getByRole("link", { name: "图谱工作室" })).toBeVisible();
  await expect(page.getByRole("button", { name: "双公式过渡" })).toBeVisible();
  await page.goto("/en/studio");

  await page.route("**/platform/v1/me/assets?limit=48", async (route) => {
    await new Promise((resolve) => setTimeout(resolve, 300));
    await route.continue();
  });
  await page.getByRole("link", { name: "Library" }).click();
  await page.waitForURL(/\/assets$/, { timeout: 30_000 });
  await expect(page.getByRole("status")).toHaveText("Loading data…");

  await page.getByRole("link", { name: "Favorites" }).click();
  await page.waitForURL(/\/favorites$/, { timeout: 30_000 });
  await expect.poll(() => favoriteRequests).toHaveLength(1);
  await page.getByRole("link", { name: "Studio" }).click();
  await page.waitForURL(/\/studio$/, { timeout: 30_000 });
  await page.getByRole("link", { name: "Favorites" }).click();
  await page.waitForURL(/\/favorites$/, { timeout: 30_000 });
  await expect(page.getByRole("heading", { name: "Favorites" })).toBeVisible();
  expect(favoriteRequests).toHaveLength(1);

  expect(forbiddenApiRequests).toEqual([]);
});
