"use client";
import { ShieldCheck } from "lucide-react";
import { useTranslations } from "next-intl";
import { useAuth } from "@/providers/auth-provider";

export function StatusRail() {
  const t = useTranslations("workbench");
  const { user } = useAuth();
  const isAdmin = Boolean(user?.roles.includes("admin"));
  return (
    // Hidden on phones: a permanent 32px band of provenance text is not worth
    // the vertical space when the viewport is already short.
    <div className="hidden h-8 shrink-0 items-center gap-3 border-b border-hairline/5 bg-deep-void/60 px-4 sm:flex">
      <div className="flex items-center gap-1.5">
        <ShieldCheck className="h-3 w-3 text-emerald-400" />
        <span className="font-mono text-[11px] text-muted-foreground">
          {t(isAdmin ? "adminArchitecture" : "architecture")}
        </span>
      </div>

      <div className="flex-1" />

      {/* Version */}
      <span className="font-mono text-[10px] text-muted-foreground/50">v1.0.0</span>
    </div>
  );
}
