"use client";
import * as React from "react";
import { cn } from "@/lib/utils/cn";

export interface InputProps
  extends React.InputHTMLAttributes<HTMLInputElement> {}

const Input = React.forwardRef<HTMLInputElement, InputProps>(
  ({ className, type, ...props }, ref) => {
    return (
      <input
        type={type}
        className={cn(
          "flex h-10 w-full rounded-lg border border-hairline/[0.06] bg-wash/[0.03] px-3 py-2 text-sm text-ink/75 placeholder:text-ink/20",
          "transition-all duration-200",
          "focus:outline-none focus:border-primary/30 focus:bg-wash/[0.05]",
          "disabled:cursor-not-allowed disabled:opacity-30",
          "file:border-0 file:bg-transparent file:text-sm file:font-medium",
          className
        )}
        ref={ref}
        {...props}
      />
    );
  }
);
Input.displayName = "Input";

export { Input };
