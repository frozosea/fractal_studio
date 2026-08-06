import { expect, test, type Page } from "@playwright/test";

test.use({ channel: "chrome", colorScheme: "dark" });
test.describe.configure({ mode: "serial" });

const MASKS = (page: Page) => [page.locator("img"), page.locator("canvas")];
const VISUAL_FACETS = [
  { facet: "colorMap", value: "custom_gradient", count: 25 },
  { facet: "colorMap", value: "inferno", count: 6 },
  { facet: "colorMap", value: "twilight", count: 6 },
  { facet: "colorMap", value: "ember_blue", count: 3 },
  { facet: "colorMap", value: "classic_cos", count: 2 },
  { facet: "colorMap", value: "hs_rainbow", count: 1 },
  { facet: "colorMap", value: "hsv_wheel", count: 1 },
  { facet: "colorMap", value: "spectral1530", count: 1 },
  { facet: "colorMap", value: "tri765", count: 1 },
  { facet: "colorMap", value: "viridis", count: 1 },
  { facet: "depth", value: "le1024", count: 17 },
  { facet: "depth", value: "gt2048", count: 15 },
  { facet: "depth", value: "le512", count: 11 },
  { facet: "depth", value: "le2048", count: 6 },
  { facet: "resolution", value: "gt8mp", count: 31 },
  { facet: "resolution", value: "le8mp", count: 16 },
  { facet: "resolution", value: "le1mp", count: 2 },
  { facet: "variant", value: "mandelbrot", count: 24 },
  { facet: "variant", value: "custom", count: 8 },
  { facet: "variant", value: "burning_ship", count: 5 },
  { facet: "variant", value: "buffalo", count: 3 },
  { facet: "variant", value: "celtic", count: 2 },
  { facet: "variant", value: "celtic_ship", count: 1 },
  { facet: "variant", value: "cos_z", count: 1 },
  { facet: "variant", value: "cosh_z", count: 1 },
  { facet: "variant", value: "heart", count: 1 },
  { facet: "variant", value: "perp_buffalo", count: 1 },
  { facet: "variant", value: "perp_ship", count: 1 },
  { facet: "variant", value: "sin_z", count: 1 },
] as const;

test("public, auth, and workbench surfaces keep the instrument visual language", async ({ page }) => {
  await page.goto("/en");
  await expect(page.locator("main").getByRole("heading", { level: 1 })).toBeVisible();

  const rootStyles = await page.evaluate(() => {
    const root = getComputedStyle(document.documentElement);
    const header = getComputedStyle(document.querySelector("header")!);
    return {
      background: root.getPropertyValue("--instrument-bg").trim(),
      amber: root.getPropertyValue("--instrument-amber").trim(),
      headerBlur: header.backdropFilter,
    };
  });
  expect(rootStyles).toEqual({ background: "9 10 12", amber: "240 160 48", headerBlur: "none" });
  await expect(page).toHaveScreenshot("landing-dark.png", { animations: "disabled", mask: MASKS(page) });

  await page.getByRole("button", { name: "Language: English" }).click();
  await expect(page.getByRole("menuitem", { name: "English" })).toHaveAttribute("aria-current", "true");
  await page.getByRole("menuitem", { name: "中文" }).click();
  await page.waitForURL((url) => url.pathname === "/");
  await expect(page.getByRole("button", { name: "Language: 中文" })).toBeVisible();
  await page.goto("/en");

  await page.evaluate(() => localStorage.setItem("fractal-studio-theme", "light"));
  await page.reload();
  await expect(page.locator("html")).not.toHaveClass(/dark/);
  await expect(page).toHaveScreenshot("landing-light.png", { animations: "disabled", mask: MASKS(page) });

  await page.goto("/en/login");
  const input = page.getByPlaceholder("Email");
  await expect(input).toBeVisible();
  const primitives = await page.evaluate(() => {
    const field = getComputedStyle(document.querySelector("input")!);
    const action = getComputedStyle(document.querySelector('button[type="submit"]')!);
    return {
      inputRadius: field.borderRadius,
      buttonRadius: action.borderRadius,
      inputFont: field.fontFamily,
      buttonTransition: action.transitionDuration,
    };
  });
  expect(parseFloat(primitives.inputRadius)).toBeLessThanOrEqual(2);
  expect(parseFloat(primitives.buttonRadius)).toBeLessThanOrEqual(2);
  expect(primitives.inputFont.toLowerCase()).toContain("mono");
  expect(primitives.buttonTransition).toBe("0.1s");

  await page.route("**/platform/v1/me", (route) => route.fulfill({
    contentType: "application/json",
    body: JSON.stringify({
      data: { id: "visual-user", email: "visual@example.test", roles: [], status: "active", member: false },
    }),
  }));
  await page.route("**/platform/v1/me/assets?limit=48", (route) => route.fulfill({
    contentType: "application/json",
    body: JSON.stringify({ data: [], page: { nextCursor: null } }),
  }));
  await page.route("**/platform/v1/explore?**", (route) => route.fulfill({
    contentType: "application/json",
    body: JSON.stringify({ data: [], page: { nextCursor: null } }),
  }));
  await page.route("**/platform/v1/explore/facets", (route) => route.fulfill({
    contentType: "application/json",
    body: JSON.stringify({ data: VISUAL_FACETS }),
  }));
  await page.evaluate(() => localStorage.setItem("fractal-studio-theme", "dark"));
  await page.goto("/en/studio");
  const navigationRail = page.locator("aside.fixed");
  await expect(navigationRail).toBeVisible();
  await expect(navigationRail).toHaveCSS("width", "240px");
  await expect(page.locator("header").first()).toHaveCSS("height", "56px");
  await expect(page).toHaveScreenshot("studio-dark.png", { animations: "disabled", mask: MASKS(page) });

  await page.goto("/en/assets");
  await expect(page.locator("main").getByRole("heading", { level: 1 })).toBeVisible();
  await expect(page).toHaveScreenshot("assets-dark.png", { animations: "disabled", mask: MASKS(page) });

  await page.goto("/en/explore");
  await expect(page.locator("main").getByRole("heading", { level: 1 })).toBeVisible();
  await expect(page).toHaveScreenshot("explore-dark.png", { animations: "disabled", mask: MASKS(page) });

  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto("/en/studio");
  await expect.poll(async () => navigationRail.evaluate((element) => element.getBoundingClientRect().right)).toBeLessThanOrEqual(0);
  await expect(page).toHaveScreenshot("studio-mobile-dark.png", { animations: "disabled", mask: MASKS(page) });
});
