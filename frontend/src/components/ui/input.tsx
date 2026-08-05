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
          "flex h-9 w-full rounded-sm border border-instrument-rule bg-instrument px-2.5 py-2 font-mono text-xs text-ink/80 placeholder:text-ink/25",
          "transition-colors duration-100",
          "focus:outline-none focus:border-brand/60 focus:bg-instrument",
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
