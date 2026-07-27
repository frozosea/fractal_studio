"use client";
import * as React from "react";
import { cn } from "@/lib/utils/cn";
import { PanelLeft, LogOut, User } from "lucide-react";
import { Button } from "@/components/ui/button";
import { LocaleSwitcher } from "@/components/layout/locale-switcher";
import { useAuth } from "@/providers/auth-provider";

interface NavbarProps {
  title?: string;
  onToggleSidebar?: () => void;
}

export function Navbar({ title = "Fractal Studio", onToggleSidebar }: NavbarProps) {
  const { user, logout } = useAuth();

  return (
    <header className="flex h-14 shrink-0 items-center gap-4 border-b border-white/5 bg-deep-void/80 backdrop-blur-xl px-4">
      {/* Left: Sidebar toggle */}
      <Button
        variant="ghost"
        size="icon"
        onClick={onToggleSidebar}
        className="shrink-0"
        aria-label="Toggle sidebar"
      >
        <PanelLeft className="h-4 w-4" />
      </Button>

      {/* Center: Page title */}
      <div className="flex flex-1 items-center justify-center">
        <h1 className="text-sm font-medium text-foreground truncate">{title}</h1>
      </div>

      {/* Right: Status + Locale */}
      <div className="flex items-center gap-3 shrink-0">
        {/* Backend status indicator */}
        <div className="flex items-center gap-2 rounded-lg border border-white/5 bg-deep-slate/30 px-3 py-1.5">
          <span
            className={cn(
              "relative flex h-2 w-2",
              "text-emerald-400"
            )}
          >
            <span
              className={cn(
                "absolute inline-flex h-full w-full rounded-full opacity-75",
                "animate-ping bg-emerald-400"
              )}
            />
            <span
              className={cn(
                "relative inline-flex h-2 w-2 rounded-full",
                "bg-emerald-400"
              )}
            />
          </span>
          <span className="text-xs text-muted-foreground hidden sm:inline">
            Platform API
          </span>
        </div>

        {/* User info + logout */}
        {user && (
          <div className="flex items-center gap-2 rounded-lg border border-white/5 bg-deep-slate/30 px-3 py-1.5">
            <User className="h-3 w-3 text-fractal-400" />
            <span className="text-xs text-muted-foreground hidden sm:inline">
              {user.creatorProfile?.displayName ?? user.email}
            </span>
            <Button
              variant="ghost"
              size="icon"
              className="h-6 w-6"
              onClick={() => logout()}
              title="Logout"
            >
              <LogOut className="h-3 w-3" />
            </Button>
          </div>
        )}

        <LocaleSwitcher />
      </div>
    </header>
  );
}
