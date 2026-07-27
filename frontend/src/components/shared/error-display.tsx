"use client";
import * as React from "react";
import { cn } from "@/lib/utils/cn";
import { AlertTriangle, RefreshCw } from "lucide-react";
import { Button } from "@/components/ui/button";

interface ErrorDisplayProps {
  message: string;
  onRetry?: () => void;
  className?: string;
  fullPage?: boolean;
}

export function ErrorDisplay({ message, onRetry, className, fullPage = false }: ErrorDisplayProps) {
  const content = (
    <div
      className={cn(
        "flex flex-col items-center justify-center gap-4 rounded-2xl border border-red-500/20 bg-red-500/5 p-8 text-center",
        className
      )}
    >
      <div className="flex h-12 w-12 items-center justify-center rounded-full bg-red-500/10">
        <AlertTriangle className="h-6 w-6 text-red-400" />
      </div>
      <div className="space-y-1">
        <h3 className="text-sm font-semibold text-red-400">Error</h3>
        <p className="max-w-md text-sm text-muted-foreground">{message}</p>
      </div>
      {onRetry && (
        <Button variant="outline" size="sm" onClick={onRetry} className="gap-2">
          <RefreshCw className="h-3 w-3" />
          Retry
        </Button>
      )}
    </div>
  );

  if (fullPage) {
    return (
      <div className="flex min-h-[400px] w-full items-center justify-center">
        {content}
      </div>
    );
  }

  return content;
}
