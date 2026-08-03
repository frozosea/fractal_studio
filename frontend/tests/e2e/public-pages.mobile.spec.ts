import { expect, test, type Page } from "@playwright/test";

/**
 * Layout guards for the pages an anonymous visitor can reach, run on a phone
 * viewport. These are the checks a desktop run cannot make: before this suite
 * existed, nothing stopped a hardcoded column count or a fixed-width track from
 * pushing the whole page sideways on a 360px screen.
 */

const PUBLIC_PATHS = ["/", "/tutorial", "/help"] as const;

/**
 * A page may scroll vertically, never horizontally. Individual regions — facet
 * chip rows, dense studio controls — are allowed their own overflow, which this
 * does not see because it measures the document, not its descendants.
 */
async function horizontalOverflow(page: Page): Promise<number> {
  return page.evaluate(() => {
    const root = document.scrollingElement ?? document.documentElement;
    return root.scrollWidth - root.clientWidth;
  });
}

test.describe("public pages on a phone", () => {
  for (const path of PUBLIC_PATHS) {
    test(`${path} renders for a signed-out visitor without sideways scroll`, async ({ page }) => {
      await page.goto(path);
      // Anonymous access is the point: none of these may bounce to sign-in.
      await expect(page).not.toHaveURL(/\/login/);
      await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
      expect(await horizontalOverflow(page)).toBeLessThanOrEqual(1);
    });
  }

  test("the header collapses its links into a menu", async ({ page }) => {
    await page.goto("/");
    // The inline nav is hidden below `md`; the menu button replaces it.
    await expect(page.locator("header nav").getByRole("link", { name: /Tutorial|教程/ })).toBeHidden();
    await page.getByRole("button", { name: /Site navigation|站点导航/ }).click();
    await expect(page.getByRole("menuitem", { name: /Tutorial|教程/ })).toBeVisible();
  });

  test("the landing page reaches the tutorial and the help page", async ({ page }) => {
    await page.goto("/");
    await page.getByRole("link", { name: /Read the tutorial|查看教程/ }).first().click();
    await expect(page).toHaveURL(/\/tutorial$/);
    await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
    expect(await horizontalOverflow(page)).toBeLessThanOrEqual(1);
  });

  test("a creator profile is public and its gallery fits the viewport", async ({ page, request }) => {
    const response = await request.get("/platform/v1/explore?limit=1");
    expect(response.ok()).toBeTruthy();
    const listings = (await response.json()).data as Array<{ creator: { handle: string } }>;
    test.skip(listings.length === 0, "needs a published catalogue");

    await page.goto(`/creator/${listings[0]!.creator.handle}`);
    await expect(page).not.toHaveURL(/\/login/);
    await expect(page.getByRole("heading", { level: 1 })).toBeVisible();
    expect(await horizontalOverflow(page)).toBeLessThanOrEqual(1);
  });

  test("a protected page still redirects to sign-in", async ({ page }) => {
    // The public surface must not have opened up the workbench by accident.
    await page.goto("/studio");
    await page.waitForURL(/\/login/, { timeout: 15_000 });
  });
});
