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
          "flex h-10 w-full rounded-lg border border-white/[0.06] bg-white/[0.03] px-3 py-2 text-sm text-white/75 placeholder:text-white/20",
          "transition-all duration-200",
          "focus:outline-none focus:border-primary/30 focus:bg-white/[0.05]",
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
