import type { Config } from "tailwindcss";
import plugin from "tailwindcss/plugin";

const RAMP_STEPS = [50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 950] as const;

/** A full 50–950 scale wired to `--<name>-<step>` custom properties. */
function rampVars(name: string): Record<string, string> {
  return Object.fromEntries(
    RAMP_STEPS.map((step) => [step, `rgb(var(--${name}-${step}) / <alpha-value>)`]),
  );
}

const config: Config = {
  darkMode: ["class"],
  future: {
    // Wrap `hover:` in `@media (hover: hover)`. Without it a tap on a touch
    // device leaves the hover state stuck until something else is tapped.
    hoverOnlyWhenSupported: true,
  },
  content: ["./src/**/*.{ts,tsx}"],
  prefix: "",
  theme: {
    container: {
      center: true,
      padding: "2rem",
      screens: { "2xl": "1400px" },
    },
    extend: {
      fontFamily: {
        sans: ["Inter", "system-ui", "-apple-system", "sans-serif"],
      },
      colors: {
        border: "hsl(var(--border))",
        input: "hsl(var(--input))",
        ring: "hsl(var(--ring))",
        background: "hsl(var(--background))",
        foreground: "hsl(var(--foreground))",

        // Direction, not colour. The app was written against a dark surface
        // with `bg-white/5`, `border-white/10` and `text-white/60`, none of
        // which mean "white" — they mean "step away from the surface". These
        // three flip with the theme so a single class works in both. Keep the
        // alpha you would have used; only the direction changes.
        wash: "rgb(var(--wash) / <alpha-value>)",
        hairline: "rgb(var(--hairline) / <alpha-value>)",
        ink: "rgb(var(--ink) / <alpha-value>)",

        // Amber is this project's accent, red and emerald its failure and
        // success states. In all three the shade number encodes contrast
        // against the surface rather than lightness, so each ramp mirrors
        // about 500 in the light theme. See the note in globals.css.
        //
        // Every step is listed even where the app uses only a few: `extend`
        // deep-merges with Tailwind's defaults, so any step left out keeps its
        // stock literal and the scale reverses direction partway up.
        amber: rampVars("amber"),
        red: rampVars("red"),
        emerald: rampVars("emerald"),
        // A solid accent fill has to stay amber in both themes — inverting it
        // would turn the logo mark into a brown square. Only its ink flips.
        brand: {
          DEFAULT: "rgb(var(--brand) / <alpha-value>)",
          ink: "rgb(var(--brand-ink) / <alpha-value>)",
        },

        // Instrument chrome, previously repeated as raw hex in the shells.
        instrument: {
          DEFAULT: "rgb(var(--instrument-bg) / <alpha-value>)",
          panel: "rgb(var(--instrument-panel) / <alpha-value>)",
          raised: "rgb(var(--instrument-panel-raised) / <alpha-value>)",
          rule: "rgb(var(--instrument-rule) / <alpha-value>)",
          ink: "rgb(var(--instrument-ink) / <alpha-value>)",
        },
        primary: {
          DEFAULT: "hsl(var(--primary))",
          foreground: "hsl(var(--primary-foreground))",
        },
        secondary: {
          DEFAULT: "hsl(var(--secondary))",
          foreground: "hsl(var(--secondary-foreground))",
        },
        destructive: {
          DEFAULT: "hsl(var(--destructive))",
          foreground: "hsl(var(--destructive-foreground))",
        },
        muted: {
          DEFAULT: "hsl(var(--muted))",
          foreground: "hsl(var(--muted-foreground))",
        },
        accent: {
          DEFAULT: "hsl(var(--accent))",
          foreground: "hsl(var(--accent-foreground))",
        },
        popover: {
          DEFAULT: "hsl(var(--popover))",
          foreground: "hsl(var(--popover-foreground))",
        },
        card: {
          DEFAULT: "hsl(var(--card))",
          foreground: "hsl(var(--card-foreground))",
        },
        fractal: rampVars("fractal"),
        neon: {
          purple: "rgb(var(--neon-purple) / <alpha-value>)",
          cyan: "rgb(var(--neon-cyan) / <alpha-value>)",
        },
        // Surface roles, not literal colours: `void` is the app shell behind
        // everything, `slate` the raised sheet popovers and inputs sit on.
        deep: {
          indigo: "rgb(var(--surface-indigo) / <alpha-value>)",
          slate: "rgb(var(--surface-raised) / <alpha-value>)",
          void: "rgb(var(--surface-sunken) / <alpha-value>)",
        },
      },
      borderRadius: {
        lg: "var(--radius)",
        md: "calc(var(--radius) - 2px)",
        sm: "calc(var(--radius) - 4px)",
        xl: "1rem",
        "2xl": "1.25rem",
      },
      boxShadow: {
        "glow-purple": "0 0 30px hsl(271 91% 65% / 0.2), 0 0 60px hsl(271 91% 65% / 0.08)",
        "glow-cyan": "0 0 30px hsl(178 84% 58% / 0.15), 0 0 60px hsl(178 84% 58% / 0.06)",
        "glow-canvas":
          "0 0 40px hsl(271 91% 65% / 0.1), 0 0 80px hsl(178 84% 58% / 0.06), inset 0 0 60px hsl(222 47% 11% / 0.3)",
        // Cast against `--glass-shadow`, which is near-black on the dark theme
        // and a soft grey-violet on paper — a 30%-black drop shadow that reads
        // as depth on black reads as grime on white.
        glass:
          "0 8px 32px rgb(var(--glass-shadow) / var(--glass-shadow-alpha)), 0 2px 8px rgb(var(--glass-shadow) / var(--glass-shadow-alpha-soft))",
        "glass-sm": "0 4px 16px rgb(var(--glass-shadow) / var(--glass-shadow-alpha-soft))",
        float: "0 12px 40px rgb(var(--glass-shadow) / calc(var(--glass-shadow-alpha) * 1.25))",
      },
      keyframes: {
        "accordion-down": {
          from: { height: "0" },
          to: { height: "var(--radix-accordion-content-height)" },
        },
        "accordion-up": {
          from: { height: "var(--radix-accordion-content-height)" },
          to: { height: "0" },
        },
        "pulse-dot": {
          "0%, 100%": { opacity: "1" },
          "50%": { opacity: "0.4" },
        },
        "progress-indeterminate": {
          "0%": { transform: "translateX(-100%)" },
          "100%": { transform: "translateX(400%)" },
        },
        "gradient-flow": {
          "0%, 100%": { backgroundPosition: "0% center" },
          "50%": { backgroundPosition: "200% center" },
        },
        "glow-pulse": {
          "0%, 100%": { opacity: "0.6" },
          "50%": { opacity: "1" },
        },
        "ambient-drift": {
          "0%": { transform: "translate(0, 0) scale(1)" },
          "25%": { transform: "translate(-10px, 5px) scale(1.02)" },
          "50%": { transform: "translate(5px, -8px) scale(1)" },
          "75%": { transform: "translate(8px, 3px) scale(1.01)" },
          "100%": { transform: "translate(0, 0) scale(1)" },
        },
        float: {
          "0%, 100%": { transform: "translateY(0px)" },
          "50%": { transform: "translateY(-6px)" },
        },
      },
      animation: {
        "accordion-down": "accordion-down 0.2s ease-out",
        "accordion-up": "accordion-up 0.2s ease-out",
        "pulse-dot": "pulse-dot 1.5s ease-in-out infinite",
        "progress-indeterminate":
          "progress-indeterminate 2s ease-in-out infinite",
        "gradient-flow": "gradient-flow 6s ease-in-out infinite",
        "glow-pulse": "glow-pulse 4s ease-in-out infinite",
        "ambient-drift": "ambient-drift 30s linear infinite",
        float: "float 6s ease-in-out infinite",
      },
    },
  },
  plugins: [
    require("tailwindcss-animate"),
    // `coarse:` targets touch input rather than a screen width, so a tablet at
    // desktop width still gets finger-sized controls. Used to raise hit targets
    // without inflating the studio's dense instrument rows on a mouse.
    plugin(({ addVariant }) => {
      addVariant("coarse", "@media (pointer: coarse)");
    }),
  ],
};

export default config;
