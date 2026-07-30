"use client";

import { useCallback, useSyncExternalStore } from "react";

/** Tailwind's `md` breakpoint is 768px, so "mobile" is everything below it. */
export const MOBILE_QUERY = "(max-width: 767px)";
/** Touch and stylus input, independent of how wide the screen happens to be. */
export const COARSE_POINTER_QUERY = "(pointer: coarse)";

/**
 * Reactive media query.
 *
 * `useSyncExternalStore` keeps the value stable within a render pass and
 * re-renders on rotation or resize, unlike a bare `matchMedia()` read inside an
 * event handler, which never notices a device turning sideways.
 *
 * The server snapshot is always `false`: markup is produced without a viewport,
 * so anything gated on this has to degrade to the wide layout or hydration will
 * mismatch. Prefer a CSS breakpoint when the choice is purely visual; reach for
 * this hook when behaviour, not styling, has to differ.
 */
export function useMediaQuery(query: string): boolean {
  const subscribe = useCallback(
    (onStoreChange: () => void) => {
      const list = window.matchMedia(query);
      list.addEventListener("change", onStoreChange);
      return () => list.removeEventListener("change", onStoreChange);
    },
    [query],
  );

  const getSnapshot = useCallback(() => window.matchMedia(query).matches, [query]);

  return useSyncExternalStore(subscribe, getSnapshot, () => false);
}

export function useIsMobile(): boolean {
  return useMediaQuery(MOBILE_QUERY);
}

export function useIsCoarsePointer(): boolean {
  return useMediaQuery(COARSE_POINTER_QUERY);
}
