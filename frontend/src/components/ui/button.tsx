"use client";
import * as React from "react";
import { Slot, Slottable } from "@radix-ui/react-slot";
import { cva, type VariantProps } from "class-variance-authority";
import { cn } from "@/lib/utils/cn";
import { Loader2 } from "lucide-react";

const buttonVariants = cva(
  "inline-flex items-center justify-center whitespace-nowrap rounded-sm border text-xs font-medium uppercase tracking-[0.08em] ring-offset-background transition-colors duration-100 focus-visible:outline-none focus-visible:border-brand focus-visible:ring-1 focus-visible:ring-brand/20 disabled:pointer-events-none disabled:opacity-40 gap-2",
  {
    variants: {
      variant: {
        default:
          "border-instrument-rule bg-instrument-raised text-ink/75 hover:border-brand/50 hover:text-brand",
        destructive:
          "border-red-500/35 bg-red-500/5 text-red-400 hover:bg-red-500/10",
        outline:
          "border-instrument-rule bg-transparent text-ink/60 hover:border-brand/40 hover:text-ink/90",
        secondary:
          "border-instrument-rule bg-instrument text-ink/55 hover:bg-instrument-raised hover:text-ink/80",
        ghost:
          "border-transparent text-ink/60 hover:border-instrument-rule hover:bg-instrument-raised hover:text-ink/85",
        link: "border-transparent text-brand underline-offset-4 hover:underline",
        fractal:
          "border-brand/60 bg-brand/10 text-brand hover:bg-brand/16",
        neon:
          "border-brand/45 bg-transparent text-brand hover:bg-brand/[0.08]",
      },
      size: {
        default: "h-9 px-3 py-2",
        sm: "h-8 px-2.5 text-[11px]",
        lg: "h-10 px-5 text-xs",
        icon: "h-8 w-8",
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
        <Slottable>{children}</Slottable>
      </Comp>
    );
  }
);
Button.displayName = "Button";
export { Button, buttonVariants };
