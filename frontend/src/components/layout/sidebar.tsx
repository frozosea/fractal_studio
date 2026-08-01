"use client";
import * as React from "react";
import { cn } from "@/lib/utils/cn";
import { usePathname, useRouter, Link } from "@/i18n/navigation";
import { useAuth } from "@/providers/auth-provider";
import { useTranslations } from "next-intl";
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
  Crown,
  UserCog,
  type LucideIcon,
} from "lucide-react";

interface NavItem {
  label: string;
  href: string;
  icon: LucideIcon;
  requiredRole?: string;
}

const navItems: NavItem[] = [
  { label: "studio", href: "/studio", icon: Wand2 },
  { label: "library", href: "/assets", icon: Images },
  { label: "marketplace", href: "/explore", icon: Store },
  { label: "favorites", href: "/favorites", icon: Heart },
  // Listings are gated on the creator role, not on membership: anyone can apply
  // for a creator profile, so every signed-in account gets the entry.
  { label: "listings", href: "/listings", icon: List },
  { label: "purchases", href: "/purchases", icon: ReceiptText },
  { label: "payouts", href: "/payouts", icon: Landmark },
  { label: "finance", href: "/finance", icon: ShieldCheck, requiredRole: "finance_operator" },
  { label: "admin", href: "/admin", icon: UserCog, requiredRole: "admin" },
  { label: "membership", href: "/membership", icon: Crown },
];

interface SidebarProps {
  collapsed?: boolean;
  mobileOpen?: boolean;
  onNavigate?: () => void;
}

export function Sidebar({ collapsed = false, mobileOpen = false, onNavigate }: SidebarProps) {
  const t = useTranslations("workbench");
  const pathname = usePathname();
  const router = useRouter();
  const { user } = useAuth();
  const isAdmin = Boolean(user?.roles.includes("admin"));
  const [pendingHref, setPendingHref] = React.useState<string | null>(null);

  React.useEffect(() => {
    setPendingHref(null);
  }, [pathname]);

  const prefetch = (href: string) => {
    void router.prefetch(href);
  };

  return (
    <aside
      className={cn(
        "fixed left-0 top-0 z-40 flex h-[100dvh] w-60 flex-col transition-[width,transform] duration-200",
        "border-r border-instrument-rule bg-instrument-panel",
        mobileOpen ? "translate-x-0" : "-translate-x-full md:translate-x-0",
        collapsed ? "md:w-16" : "md:w-60",
      )}
      style={{
        // Fixed to the viewport, so the shell's insets do not reach it.
        paddingTop: "env(safe-area-inset-top)",
        paddingLeft: "env(safe-area-inset-left)",
        paddingBottom: "env(safe-area-inset-bottom)",
      }}
    >
      {/* Logo — quiet glow */}
      <div
        className={cn(
          "flex h-14 shrink-0 items-center gap-2.5 border-b border-instrument-rule/40",
          collapsed ? "md:justify-center md:px-2" : "px-5",
        )}
      >
        <div className="flex h-7 w-7 items-center justify-center bg-brand">
          <span className="text-xs font-bold text-brand-ink">F</span>
        </div>
        <span className={cn("text-sm font-semibold tracking-wide text-ink", collapsed && "md:hidden")}>
          Fractal Studio
        </span>
      </div>

      {/* Navigation — soft hover, no harsh highlights */}
      <nav className={cn("flex-1 space-y-0.5", collapsed ? "p-2 md:px-2" : "p-3")} aria-label={t("navigation")}>
        {navItems.filter((item) => {
          if (isAdmin) return item.requiredRole === "admin";
          if (item.requiredRole && !user?.roles.includes(item.requiredRole)) return false;
          return true;
        }).map((item) => {
          const isActive = pathname.startsWith(item.href);
          const Icon = item.icon;
          return (
            <Link
              key={item.href}
              href={item.href}
              prefetch
              onMouseEnter={() => prefetch(item.href)}
              onFocus={() => prefetch(item.href)}
              onClick={() => {
                setPendingHref(item.href);
                onNavigate?.();
              }}
              aria-current={isActive ? "page" : undefined}
              aria-busy={pendingHref === item.href}
              title={collapsed ? t(`nav.${item.label}`) : undefined}
              className={cn(
                "flex items-center gap-3 rounded-sm border-l-2 px-3 py-2 text-sm transition-colors duration-150",
                collapsed && "md:justify-center md:px-2",
                isActive
                  ? "border-amber-400 bg-instrument-raised text-ink"
                  : "border-transparent text-ink/60 hover:bg-wash/[0.03] hover:text-ink/75"
              )}
            >
              <Icon
                className={cn(
                  "h-4 w-4 shrink-0 transition-colors duration-200",
                  isActive ? "text-amber-400" : "text-ink/30"
                )}
              />
              <span className={cn("font-normal tracking-wide", collapsed && "md:hidden")}>{t(`nav.${item.label}`)}</span>
              {pendingHref === item.href && <Loader2 className={cn("ml-auto h-3.5 w-3.5 animate-spin text-amber-400", collapsed && "md:hidden")} />}
            </Link>
          );
        })}
      </nav>

      {/* Footer — whisper the version */}
      <div className={cn("border-t border-instrument-rule/40 p-3", collapsed && "md:px-2")}>
        <div
          className={cn(
            "rounded-lg bg-instrument-raised/40 px-3 py-2",
            collapsed && "md:px-1 md:text-center",
          )}
        >
          <p
            className={cn(
              "text-[10px] uppercase tracking-[0.15em] text-ink/60",
              collapsed && "md:hidden",
            )}
          >
            {t("footer")}
          </p>
          <p className="text-xs text-amber-400/60">{collapsed ? "v0.1" : "v0.1.0"}</p>
        </div>
      </div>
    </aside>
  );
}
