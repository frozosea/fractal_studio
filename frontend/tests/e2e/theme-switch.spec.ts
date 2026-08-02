import { expect, test } from "@playwright/test";

/**
 * Drives the real control in a real browser, which is what `theme.spec.ts`
 * cannot do — that one executes the pre-paint init script against a stub
 * document, so it proves the resolution rule and nothing about the UI.
 *
 * Uses the system Chrome rather than Playwright's bundled Chromium: the
 * download does not complete in this environment, but `google-chrome` is
 * installed, so `channel` is what makes these runnable here at all.
 *
 * Needs a server on `baseURL` serving a *current* build — a stale `next start`
 * will fail the first assertion with the toggle simply absent.
 */
test.use({ channel: "chrome" });

const readTheme = (page: import("@playwright/test").Page) =>
  page.evaluate(() => ({
    dark: document.documentElement.classList.contains("dark"),
    colorScheme: document.documentElement.style.colorScheme,
    bodyBackground: getComputedStyle(document.body).backgroundColor,
    stored: localStorage.getItem("fractal-studio-theme"),
  }));

async function choose(page: import("@playwright/test").Page, option: string) {
  await page.getByRole("button", { name: "主题" }).click();
  await page.getByRole("menuitem", { name: option }).click();
}

test.describe("theme switching", () => {
  test("with nothing stored the page follows the OS", async ({ browser }) => {
    for (const scheme of ["dark", "light"] as const) {
      const context = await browser.newContext({ colorScheme: scheme });
      const page = await context.newPage();
      await page.goto("/zh");

      const state = await readTheme(page);
      expect(state.dark).toBe(scheme === "dark");
      expect(state.colorScheme).toBe(scheme);
      await context.close();
    }
  });

  // Pinned dark, so "picking light" is a real change and the final "follow the
  // system" step has a known answer. A Playwright context is light by default.
  test("choosing a theme repaints, persists, and survives a reload", async ({ browser }) => {
    const context = await browser.newContext({ colorScheme: "dark" });
    const page = await context.newPage();
    await page.goto("/zh");
    expect((await readTheme(page)).dark).toBe(true);

    await choose(page, "浅色");
    const light = await readTheme(page);
    expect(light.dark).toBe(false);
    expect(light.stored).toBe("light");
    expect(light.colorScheme).toBe("light");

    // Assert the surface actually repainted rather than merely losing a class —
    // a token that failed to flip would keep the body dark with `.dark` gone.
    const [r, g, b] = light.bodyBackground.match(/\d+/g)!.map(Number);
    expect(r, light.bodyBackground).toBeGreaterThan(200);
    expect(g).toBeGreaterThan(200);
    expect(b).toBeGreaterThan(200);

    await page.reload();
    expect((await readTheme(page)).dark).toBe(false);

    await choose(page, "深色");
    expect(await readTheme(page)).toMatchObject({ dark: true, stored: "dark" });

    await choose(page, "跟随系统");
    expect(await readTheme(page)).toMatchObject({ stored: "system", dark: true });
    await context.close();
  });

  test("the choice is reachable from every shell", async ({ page }) => {
    // Public, auth and workbench each mount their own chrome; a toggle missing
    // from one of them strands the user in the wrong theme on that route.
    for (const path of ["/zh", "/zh/tutorial", "/zh/login"]) {
      await page.goto(path);
      await expect(page.getByRole("button", { name: "主题" }), path).toBeVisible();
    }
  });

  test("the light theme does not leave dark surfaces behind", async ({ browser }) => {
    const context = await browser.newContext({ colorScheme: "light" });
    const page = await context.newPage();
    await page.goto("/zh");

    // The migration's failure mode is a token that never flipped, which shows
    // up as a near-black panel sitting on paper.
    const dark = await page.evaluate(() =>
      [...document.querySelectorAll("header, footer, section, article, aside, main")]
        .filter((el) => {
          const bg = getComputedStyle(el).backgroundColor;
          const m = bg.match(/^rgba?\((\d+), (\d+), (\d+)(?:, ([\d.]+))?/);
          if (!m) return false;
          const [r, g, b] = [m[1], m[2], m[3]].map(Number);
          const alpha = m[4] === undefined ? 1 : Number(m[4]);
          return alpha > 0.5 && (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255 < 0.35;
        })
        .map((el) => `${el.tagName.toLowerCase()}.${el.className}`.slice(0, 120)),
    );

    expect(dark, `dark surfaces on the light theme: ${dark.join(" | ")}`).toEqual([]);
    await context.close();
  });
});
