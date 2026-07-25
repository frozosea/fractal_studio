"use client";
import * as React from "react";
import { cn } from "@/lib/utils/cn";
import { Loader2 } from "lucide-react";

interface LoadingSpinnerProps {
  size?: "sm" | "md" | "lg";
  fullPage?: boolean;
  label?: string;
  className?: string;
}

const sizeMap = {
  sm: "h-4 w-4",
  md: "h-8 w-8",
  lg: "h-12 w-12",
};

export function LoadingSpinner({ size = "md", fullPage = false, label, className }: LoadingSpinnerProps) {
  const spinner = (
    <div className={cn("flex flex-col items-center justify-center gap-3", fullPage && "h-full w-full", className)}>
      <Loader2
        className={cn(
          "animate-spin text-fractal-500",
          "drop-shadow-[0_0_8px_hsl(271_91%_65%_/_0.4)]",
          sizeMap[size]
        )}
      />
      {label && <p className="text-sm text-muted-foreground animate-pulse">{label}</p>}
    </div>
  );

  if (fullPage) {
    return (
      <div className="flex min-h-[400px] w-full items-center justify-center">
        {spinner}
      </div>
    );
  }

  return spinner;
}
