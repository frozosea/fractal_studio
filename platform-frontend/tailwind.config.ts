import type { Config } from "tailwindcss";

const config: Config = {
  darkMode: ["class"],
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
        fractal: {
          50: "#f5f0ff",
          100: "#ede5ff",
          200: "#dccfff",
          300: "#c4a8ff",
          400: "#a878ff",
          500: "#9333ff",
          600: "#7c22e0",
          700: "#6516c2",
          800: "#52149e",
          900: "#3e127e",
          950: "#1f0750",
        },
        neon: {
          purple: "#9333FF",
          cyan: "#36F0E8",
        },
        deep: {
          indigo: "#0F172A",
          slate: "#1E293B",
          void: "#090E1A",
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
        glass:
          "0 8px 32px hsl(222 47% 6% / 0.3), 0 2px 8px hsl(222 47% 6% / 0.2)",
        "glass-sm": "0 4px 16px hsl(222 47% 6% / 0.2)",
        float: "0 12px 40px hsl(222 47% 6% / 0.5)",
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
  plugins: [require("tailwindcss-animate")],
};

export default config;
