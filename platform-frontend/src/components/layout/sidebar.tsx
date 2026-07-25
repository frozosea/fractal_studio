"use client";
import * as React from "react";
import { cn } from "@/lib/utils/cn";
import { usePathname, Link } from "@/i18n/navigation";
import {
  Wand2,
  Crosshair,
  Box,
  Film,
  ListTodo,
  FlaskConical,
  Cpu,
  type LucideIcon,
} from "lucide-react";

interface NavItem {
  label: string;
  href: string;
  icon: LucideIcon;
}

const navItems: NavItem[] = [
  { label: "Studio", href: "/studio", icon: Wand2 },
  { label: "Points", href: "/studio/points", icon: Crosshair },
  { label: "3D", href: "/studio/3d", icon: Box },
  { label: "Video", href: "/studio/video", icon: Film },
  { label: "Runs", href: "/runs", icon: ListTodo },
  { label: "Variants", href: "/studio/variants", icon: FlaskConical },
  { label: "System", href: "/system", icon: Cpu },
];

export function Sidebar() {
  const pathname = usePathname();

  return (
    <aside
      className="fixed left-0 top-0 z-40 flex h-screen w-60 flex-col"
      style={{
        background: "linear-gradient(180deg, hsl(228 45% 10% / 0.92) 0%, hsl(228 45% 8% / 0.88) 100%)",
        backdropFilter: "blur(24px) saturate(140%)",
        WebkitBackdropFilter: "blur(24px) saturate(140%)",
        borderRight: "1px solid hsl(226 22% 18% / 0.5)",
        boxShadow: "1px 0 24px hsl(228 50% 4% / 0.3)",
      }}
    >
      {/* Logo — quiet glow */}
      <div
        className="flex h-14 shrink-0 items-center gap-2.5 px-5"
        style={{ borderBottom: "1px solid hsl(226 22% 18% / 0.4)" }}
      >
        <div
          className="flex h-7 w-7 items-center justify-center rounded-lg"
          style={{
            background: "linear-gradient(135deg, hsl(271 85% 50%) 0%, hsl(271 85% 35%) 100%)",
            boxShadow: "0 0 20px hsl(271 85% 50% / 0.2), 0 0 40px hsl(271 85% 50% / 0.08)",
          }}
        >
          <span className="text-xs font-bold text-white">F</span>
        </div>
        <span
          className="text-sm font-semibold tracking-wide"
          style={{
            color: "hsl(0 0% 100%)",
            textShadow: "0 0 12px hsl(271 85% 50% / 0.4)",
          }}
        >
          Fractal Studio
        </span>
      </div>

      {/* Navigation — soft hover, no harsh highlights */}
      <nav className="flex-1 space-y-0.5 p-3">
        {navItems.map((item) => {
          const isActive = pathname.startsWith(item.href);
          const Icon = item.icon;
          return (
            <Link
              key={item.href}
              href={item.href}
              className={cn(
                "flex items-center gap-3 rounded-lg px-3 py-2 text-sm transition-all duration-200",
                isActive
                  ? "bg-white/[0.06] text-white"
                  : "text-white/45 hover:bg-white/[0.03] hover:text-white/75"
              )}
              style={
                isActive
                  ? {
                      boxShadow:
                        "0 0 20px hsl(271 85% 50% / 0.06), inset 0 0 0 1px hsl(271 85% 50% / 0.1)",
                    }
                  : undefined
              }
            >
              <Icon
                className={cn(
                  "h-4 w-4 shrink-0 transition-colors duration-200",
                  isActive ? "text-primary" : "text-white/30"
                )}
              />
              <span className="font-normal tracking-wide">{item.label}</span>
            </Link>
          );
        })}
      </nav>

      {/* Footer — whisper the version */}
      <div
        className="border-t p-3"
        style={{ borderTopColor: "hsl(226 22% 18% / 0.4)" }}
      >
        <div
          className="rounded-lg px-3 py-2"
          style={{ background: "hsl(226 22% 14% / 0.4)" }}
        >
          <p
            className="text-[10px] uppercase tracking-[0.15em]"
            style={{ color: "hsl(220 16% 40%)" }}
          >
            Berkeley · Fractal
          </p>
          <p className="text-xs text-primary/60">v0.1.0</p>
        </div>
      </div>
    </aside>
  );
}
