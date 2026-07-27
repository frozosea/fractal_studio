"use client";
import * as React from "react";
import { cn } from "@/lib/utils/cn";
import { usePathname, useRouter, Link } from "@/i18n/navigation";
import { useAuth } from "@/providers/auth-provider";
import {
  Wand2,
  Images,
  Store,
  ReceiptText,
  Landmark,
  List,
  Heart,
  ShieldCheck,
  Loader2,
  type LucideIcon,
} from "lucide-react";

interface NavItem {
  label: string;
  href: string;
  icon: LucideIcon;
  requiredRole?: string;
}

const navItems: NavItem[] = [
  { label: "Studio", href: "/studio", icon: Wand2 },
  { label: "Library", href: "/assets", icon: Images },
  { label: "Marketplace", href: "/explore", icon: Store },
  { label: "Favorites", href: "/favorites", icon: Heart },
  { label: "My listings", href: "/listings", icon: List },
  { label: "Purchases", href: "/purchases", icon: ReceiptText },
  { label: "Payouts", href: "/payouts", icon: Landmark },
  { label: "Finance", href: "/finance", icon: ShieldCheck, requiredRole: "finance_operator" },
];

export function Sidebar() {
  const pathname = usePathname();
  const router = useRouter();
  const { user } = useAuth();
  const [pendingHref, setPendingHref] = React.useState<string | null>(null);

  React.useEffect(() => {
    setPendingHref(null);
  }, [pathname]);

  const prefetch = (href: string) => {
    void router.prefetch(href);
  };

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
      <nav className="flex-1 space-y-0.5 p-3" aria-label="Workspace navigation">
        {navItems.filter((item) => !item.requiredRole || user?.roles.includes(item.requiredRole)).map((item) => {
          const isActive = pathname.startsWith(item.href);
          const Icon = item.icon;
          return (
            <Link
              key={item.href}
              href={item.href}
              prefetch
              onMouseEnter={() => prefetch(item.href)}
              onFocus={() => prefetch(item.href)}
              onClick={() => setPendingHref(item.href)}
              aria-current={isActive ? "page" : undefined}
              aria-busy={pendingHref === item.href}
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
              {pendingHref === item.href && <Loader2 className="ml-auto h-3.5 w-3.5 animate-spin text-primary" />}
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
