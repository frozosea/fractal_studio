"use client";
import * as React from "react";
import { cva, type VariantProps } from "class-variance-authority";
import { cn } from "@/lib/utils/cn";

const badgeVariants = cva(
  "inline-flex items-center rounded-sm border px-2 py-0.5 font-mono text-[10px] font-medium uppercase tracking-wider transition-colors focus:outline-none focus:ring-1 focus:ring-ring gap-1.5",
  {
    variants: {
      variant: {
        default: "border-transparent bg-primary text-primary-foreground hover:bg-primary/80",
        secondary: "border-transparent bg-secondary text-secondary-foreground hover:bg-secondary/80",
        destructive: "border-transparent bg-destructive text-destructive-foreground hover:bg-destructive/80",
        outline: "text-foreground border-border",
        fractal: "border-transparent text-ink/90 border-fractal-500/30",
        neon: "border-transparent text-neon-cyan border-neon-cyan/30",
        gradient: "border-transparent text-ink/85",
        "gradient-cyan": "border-transparent text-ink/85",
        success: "border-transparent bg-emerald-500/10 text-emerald-400 border-emerald-500/30",
        warning: "border-transparent bg-amber-500/10 text-amber-400 border-amber-500/30",
        error: "border-transparent bg-red-500/10 text-red-400 border-red-500/30",
        info: "border-transparent bg-fractal-500/10 text-fractal-400 border-fractal-500/30",
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
  return (
    <div
      className={cn(badgeVariants({ variant }), className)}
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
