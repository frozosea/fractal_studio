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
  // Dark is the app default, so an untouched browser gets it whatever the OS
  // says. Only an explicit "system" hands the choice to the OS.
  { name: "nothing stored defaults to dark on a dark OS", stored: null, prefersDark: true, expected: "dark" },
  { name: "nothing stored defaults to dark on a light OS", stored: null, prefersDark: false, expected: "dark" },
  { name: "an explicit dark choice beats a light OS", stored: "dark", prefersDark: false, expected: "dark" },
  { name: "an explicit light choice beats a dark OS", stored: "light", prefersDark: true, expected: "light" },
  { name: '"system" follows a dark OS', stored: "system", prefersDark: true, expected: "dark" },
  { name: '"system" follows a light OS', stored: "system", prefersDark: false, expected: "light" },
  // A corrupt value is not a licence to pin light; it falls back to the default.
  { name: "a corrupt value falls back to the default", stored: "chartreuse", prefersDark: false, expected: "dark" },
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

test("a storage read that throws still lands on the default", () => {
  // Safari's private mode throws on getItem. Those users cannot have a stored
  // preference at all, so they must get the default rather than a blank page
  // or a half-applied theme.
  for (const prefersDark of [true, false]) {
    expect(runInitScript({ stored: null, prefersDark, storageThrows: true })).toEqual({
      dark: true,
      colorScheme: "dark",
    });
  }
});
