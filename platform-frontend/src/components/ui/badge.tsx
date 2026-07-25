"use client";
import * as React from "react";
import { cva, type VariantProps } from "class-variance-authority";
import { cn } from "@/lib/utils/cn";

const badgeVariants = cva(
  "inline-flex items-center rounded-full border px-2.5 py-0.5 text-xs font-semibold transition-colors focus:outline-none focus:ring-2 focus:ring-ring focus:ring-offset-2 gap-1.5",
  {
    variants: {
      variant: {
        default: "border-transparent bg-primary text-primary-foreground hover:bg-primary/80",
        secondary: "border-transparent bg-secondary text-secondary-foreground hover:bg-secondary/80",
        destructive: "border-transparent bg-destructive text-destructive-foreground hover:bg-destructive/80",
        outline: "text-foreground border-border",
        fractal: "border-transparent text-white/90 border-fractal-500/30",
        neon: "border-transparent text-neon-cyan border-neon-cyan/30",
        gradient: "border-transparent text-white/85",
        "gradient-cyan": "border-transparent text-white/85",
        success: "border-transparent bg-emerald-500/10 text-emerald-400 border-emerald-500/30",
        warning: "border-transparent bg-amber-500/10 text-amber-400 border-amber-500/30",
        error: "border-transparent bg-red-500/10 text-red-400 border-red-500/30",
        info: "border-transparent bg-blue-500/10 text-blue-400 border-blue-500/30",
        running: "border-transparent bg-fractal-500/15 text-fractal-300 border-fractal-500/30",
      },
    },
    defaultVariants: { variant: "default" },
  }
);

export interface BadgeProps
  extends React.HTMLAttributes<HTMLDivElement>,
    VariantProps<typeof badgeVariants> {}

function Badge({ className, variant, ...props }: BadgeProps) {
  const gradientStyle =
    variant === "fractal" || variant === "gradient"
      ? {
          background:
            "linear-gradient(135deg, hsl(271 85% 45% / 0.25) 0%, hsl(271 60% 30% / 0.15) 100%)",
          boxShadow: "0 0 12px hsl(271 85% 50% / 0.08)",
        }
      : variant === "neon" || variant === "gradient-cyan"
        ? {
            background:
              "linear-gradient(135deg, hsl(178 75% 40% / 0.20) 0%, hsl(178 60% 25% / 0.12) 100%)",
            boxShadow: "0 0 12px hsl(178 75% 50% / 0.06)",
          }
        : undefined;

  return (
    <div
      className={cn(badgeVariants({ variant }), className)}
      style={gradientStyle}
      {...props}
    >
      {variant === "running" && (
        <span className="relative flex h-2 w-2">
          <span className="absolute inline-flex h-full w-full animate-ping rounded-full bg-fractal-400 opacity-75" />
          <span className="relative inline-flex h-2 w-2 rounded-full bg-fractal-400" />
        </span>
      )}
      {props.children}
    </div>
  );
}

export { Badge, badgeVariants };
