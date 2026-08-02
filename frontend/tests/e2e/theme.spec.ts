import { expect, test } from "@playwright/test";
import { THEME_INIT_SCRIPT, THEME_STORAGE_KEY, resolveTheme } from "../../src/lib/theme";

/**
 * No browser fixture is requested, so this runs anywhere — same arrangement as
 * preview-transform.spec.ts.
 *
 * The init script is the one piece of the theme that cannot be imported and
 * called: it ships as a string inlined into <head>, and it runs before React
 * exists. So it is executed here against a stub document rather than
 * reimplemented, which is the only way a test can catch it drifting from
 * `resolveTheme`.
 */

interface InitResult {
  dark: boolean;
  colorScheme: string;
}

function runInitScript(options: {
  stored: string | null;
  prefersDark: boolean;
  storageThrows?: boolean;
}): InitResult {
  const classes = new Set<string>();
  const documentElement = {
    classList: {
      toggle(name: string, force: boolean) {
        if (force) classes.add(name);
        else classes.delete(name);
      },
    },
    style: { colorScheme: "" },
  };

  const localStorage = {
    getItem(key: string) {
      if (options.storageThrows) throw new Error("storage disabled");
      expect(key).toBe(THEME_STORAGE_KEY);
      return options.stored;
    },
  };

  const window = { matchMedia: () => ({ matches: options.prefersDark }) };

  // Passing the globals as parameters shadows them inside the script body.
  // eslint-disable-next-line no-new-func
  new Function("localStorage", "window", "document", THEME_INIT_SCRIPT)(localStorage, window, {
    documentElement,
  });

  return { dark: classes.has("dark"), colorScheme: documentElement.style.colorScheme };
}

const cases: Array<{
  name: string;
  stored: string | null;
  prefersDark: boolean;
  expected: "light" | "dark";
}> = [
  { name: "nothing stored follows a dark OS", stored: null, prefersDark: true, expected: "dark" },
  { name: "nothing stored follows a light OS", stored: null, prefersDark: false, expected: "light" },
  { name: "an explicit dark choice beats a light OS", stored: "dark", prefersDark: false, expected: "dark" },
  { name: "an explicit light choice beats a dark OS", stored: "light", prefersDark: true, expected: "light" },
  { name: '"system" follows a dark OS', stored: "system", prefersDark: true, expected: "dark" },
  { name: '"system" follows a light OS', stored: "system", prefersDark: false, expected: "light" },
  // Anything unrecognised must behave like "system", not pin one theme.
  { name: "a corrupt value falls back to the OS", stored: "chartreuse", prefersDark: true, expected: "dark" },
];

for (const testCase of cases) {
  test(`init script: ${testCase.name}`, () => {
    const result = runInitScript(testCase);
    expect(result.dark).toBe(testCase.expected === "dark");
    expect(result.colorScheme).toBe(testCase.expected);
  });

  test(`resolveTheme agrees: ${testCase.name}`, () => {
    expect(resolveTheme(testCase.stored, testCase.prefersDark)).toBe(testCase.expected);
  });
}

test("a storage read that throws still honours the OS preference", () => {
  // Safari's private mode throws on getItem. Catching too widely here would
  // skip the matchMedia branch entirely and hand those users a light page on a
  // dark desktop.
  expect(runInitScript({ stored: null, prefersDark: true, storageThrows: true })).toEqual({
    dark: true,
    colorScheme: "dark",
  });
  expect(runInitScript({ stored: null, prefersDark: false, storageThrows: true })).toEqual({
    dark: false,
    colorScheme: "light",
  });
});
