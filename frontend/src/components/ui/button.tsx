"use client";
import * as React from "react";
import { Slot } from "@radix-ui/react-slot";
import { cva, type VariantProps } from "class-variance-authority";
import { cn } from "@/lib/utils/cn";
import { Loader2 } from "lucide-react";

const buttonVariants = cva(
  "inline-flex items-center justify-center whitespace-nowrap rounded-lg text-sm font-normal tracking-wide ring-offset-background transition-all duration-200 focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-primary/30 focus-visible:ring-offset-1 disabled:pointer-events-none disabled:opacity-40 gap-2",
  {
    variants: {
      variant: {
        default:
          "bg-white/[0.08] text-white/80 hover:bg-white/[0.12] border border-white/[0.06]",
        destructive:
          "bg-red-500/10 text-red-400/80 hover:bg-red-500/15 border border-red-500/10",
        outline:
          "border border-white/[0.08] bg-transparent text-white/60 hover:bg-white/[0.04] hover:text-white/85",
        secondary:
          "bg-white/[0.04] text-white/55 hover:bg-white/[0.07] hover:text-white/75",
        ghost:
          "text-white/45 hover:bg-white/[0.04] hover:text-white/70",
        link: "text-primary/80 underline-offset-4 hover:underline",
        fractal:
          "bg-primary/85 text-white hover:bg-primary/75",
        neon:
          "bg-transparent border border-primary/30 text-primary hover:bg-primary/[0.06]",
      },
      size: {
        default: "h-10 px-4 py-2",
        sm: "h-8 rounded-md px-3 text-xs",
        lg: "h-11 rounded-lg px-8 text-sm",
        icon: "h-9 w-9",
      },
    },
    defaultVariants: { variant: "default", size: "default" },
  }
);

export interface ButtonProps
  extends React.ButtonHTMLAttributes<HTMLButtonElement>,
    VariantProps<typeof buttonVariants> {
  asChild?: boolean;
  loading?: boolean;
}
const Button = React.forwardRef<HTMLButtonElement, ButtonProps>(
  (
    { className, variant, size, asChild = false, loading, disabled, children, ...props },
    ref
  ) => {
    const Comp = asChild ? Slot : "button";
    return (
      <Comp
        className={cn(buttonVariants({ variant, size, className }))}
        ref={ref}
        disabled={disabled || loading}
        {...props}
      >
        {loading && <Loader2 className="h-4 w-4 animate-spin opacity-60" />}
        {children}
      </Comp>
    );
  }
);
Button.displayName = "Button";
export { Button, buttonVariants };
