"use client";

import * as React from "react";
import { useMediaQuery } from "@/lib/hooks/use-media-query";
import {
  DARK_MEDIA_QUERY,
  THEME_STORAGE_KEY,
  isTheme,
  resolveTheme,
  type ResolvedTheme,
  type Theme,
} from "@/lib/theme";

interface ThemeContextValue {
  /** What the user picked — may be "system". */
  theme: Theme;
  /** What that resolves to right now. */
  resolvedTheme: ResolvedTheme;
  setTheme: (theme: Theme) => void;
  /**
   * False until the stored preference has been read on the client. Controls
   * that render differently per theme must wait for it, or the server markup
   * (which cannot know the preference) will not match on hydration.
   */
  ready: boolean;
}

const ThemeContext = React.createContext<ThemeContextValue | null>(null);

function readStoredTheme(): Theme {
  try {
    const stored = window.localStorage.getItem(THEME_STORAGE_KEY);
    if (isTheme(stored)) return stored;
  } catch {
    // Private-mode Safari throws rather than returning null.
  }
  return "dark";
}

function applyTheme(resolved: ResolvedTheme) {
  const root = document.documentElement;
  root.classList.toggle("dark", resolved === "dark");
  // Keeps native form controls, scrollbars and the caret in step; CSS alone
  // cannot restyle them.
  root.style.colorScheme = resolved;
}

export function ThemeProvider({ children }: { children: React.ReactNode }) {
  const [theme, setThemeState] = React.useState<Theme>("system");
  const [ready, setReady] = React.useState(false);

  // Subscribing through the shared hook rather than a local matchMedia
  // listener: it is tearing-safe, and it means the OS flipping to dark at
  // sunset re-renders us for free while the preference is "system".
  const systemPrefersDark = useMediaQuery(DARK_MEDIA_QUERY);

  // Derived, not state. Holding it separately would mean writing it on mount,
  // on every OS change and in setTheme, with three chances to drift from
  // `theme`.
  const resolvedTheme = resolveTheme(theme, systemPrefersDark);

  React.useEffect(() => {
    setThemeState(readStoredTheme());
    setReady(true);
  }, []);

  // THEME_INIT_SCRIPT already applied the right class before first paint, so
  // nothing is touched until the stored preference has been read — otherwise
  // the pre-hydration render, which cannot know it, would clobber the class.
  // After that this one effect covers both a user pick and an OS change.
  React.useEffect(() => {
    if (!ready) return;
    applyTheme(resolvedTheme);
  }, [ready, resolvedTheme]);

  const setTheme = React.useCallback((next: Theme) => {
    setThemeState(next);
    try {
      window.localStorage.setItem(THEME_STORAGE_KEY, next);
    } catch {
      // A rejected write costs persistence, not the switch itself.
    }
  }, []);

  const value = React.useMemo<ThemeContextValue>(
    () => ({ theme, resolvedTheme, setTheme, ready }),
    [theme, resolvedTheme, setTheme, ready],
  );

  return <ThemeContext.Provider value={value}>{children}</ThemeContext.Provider>;
}

export function useTheme(): ThemeContextValue {
  const context = React.useContext(ThemeContext);
  if (!context) throw new Error("useTheme must be used within a ThemeProvider");
  return context;
}
