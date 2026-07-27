"use client";
import { ShieldCheck } from "lucide-react";

export function StatusRail() {
  return (
    <div className="flex h-8 shrink-0 items-center gap-3 border-b border-white/5 bg-deep-void/60 px-4">
      <div className="flex items-center gap-1.5">
        <ShieldCheck className="h-3 w-3 text-emerald-400" />
        <span className="font-mono text-[11px] text-muted-foreground">
          Browser → Platform → private Compute
        </span>
      </div>

      <div className="flex-1" />

      {/* Version */}
      <span className="font-mono text-[10px] text-muted-foreground/50">v1.0.0</span>
    </div>
  );
}
