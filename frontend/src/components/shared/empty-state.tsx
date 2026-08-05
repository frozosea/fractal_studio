"use client";
import * as React from "react";
import { cn } from "@/lib/utils/cn";
import { Inbox } from "lucide-react";
import { Button } from "@/components/ui/button";

interface EmptyStateProps {
  icon?: React.ReactNode;
  title: string;
  description?: string;
  action?: {
    label: string;
    onClick: () => void;
  };
  className?: string;
}

export function EmptyState({
  icon,
  title,
  description,
  action,
  className,
}: EmptyStateProps) {
  return (
    <div
      className={cn(
        "flex flex-col items-center justify-center gap-4 rounded-sm border border-dashed border-instrument-rule bg-instrument-panel p-10 text-center",
        className
      )}
    >
      <div className="flex h-14 w-14 items-center justify-center border border-brand/25 bg-brand/5">
        {icon ?? <Inbox className="h-8 w-8 text-fractal-400" />}
      </div>
      <div className="space-y-1">
        <h3 className="text-base font-semibold text-foreground">{title}</h3>
        {description && <p className="max-w-sm text-sm text-muted-foreground">{description}</p>}
      </div>
      {action && (
        <Button variant="fractal" size="sm" onClick={action.onClick}>
          {action.label}
        </Button>
      )}
    </div>
  );
}
