/**
 * Intrinsic card grid, shared by every gallery in the app.
 *
 * Expressed as an inline style because Tailwind cannot spell `min()` inside a
 * grid track. The `min(100%, 18rem)` floor is the load-bearing part: a plain
 * `minmax(18rem, 1fr)` overflows any container narrower than 288px, which is
 * every phone once padding is subtracted. Clamping the floor to the container
 * collapses cleanly to a single column instead, then grows to 2, 3 or 4 columns
 * on its own — no breakpoints, no resize listener.
 */
export const CARD_GRID_STYLE = {
  gridTemplateColumns: "repeat(auto-fill, minmax(min(100%, 18rem), 1fr))",
} as const;
