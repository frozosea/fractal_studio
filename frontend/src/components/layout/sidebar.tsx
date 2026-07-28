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
  type LucideIcon,
} from "lucide-react";

interface NavItem {
  label: string;
  href: string;
  icon: LucideIcon;
  requiredRole?: string;
  hideForMember?: boolean;
  requireMember?: boolean;
}

const navItems: NavItem[] = [
  { label: "studio", href: "/studio", icon: Wand2 },
  { label: "library", href: "/assets", icon: Images },
  { label: "marketplace", href: "/explore", icon: Store },
  { label: "favorites", href: "/favorites", icon: Heart },
  { label: "listings", href: "/listings", icon: List, requireMember: true },
  { label: "purchases", href: "/purchases", icon: ReceiptText },
  { label: "payouts", href: "/payouts", icon: Landmark },
  { label: "finance", href: "/finance", icon: ShieldCheck, requiredRole: "finance_operator" },
  { label: "membership", href: "/membership", icon: Crown, hideForMember: true },
];

export function Sidebar() {
  const t = useTranslations("workbench");
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
        background: "#0d0f12",
        borderRight: "1px solid #2b2f36",
      }}
    >
      {/* Logo — quiet glow */}
      <div
        className="flex h-14 shrink-0 items-center gap-2.5 px-5"
        style={{ borderBottom: "1px solid hsl(226 22% 18% / 0.4)" }}
      >
        <div
          className="flex h-7 w-7 items-center justify-center"
          style={{
            background: "#f0a030",
          }}
        >
          <span className="text-xs font-bold text-black">F</span>
        </div>
        <span
          className="text-sm font-semibold tracking-wide"
          style={{
            color: "hsl(0 0% 100%)",
          }}
        >
          Fractal Studio
        </span>
      </div>

      {/* Navigation — soft hover, no harsh highlights */}
      <nav className="flex-1 space-y-0.5 p-3" aria-label={t("navigation")}>
        {navItems.filter((item) => {
          if (item.requiredRole && !user?.roles.includes(item.requiredRole)) return false;
          if (item.hideForMember && user?.member) return false;
          if (item.requireMember && !user?.member) return false;
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
              onClick={() => setPendingHref(item.href)}
              aria-current={isActive ? "page" : undefined}
              aria-busy={pendingHref === item.href}
              className={cn(
                "flex items-center gap-3 rounded-sm border-l-2 px-3 py-2 text-sm transition-colors duration-150",
                isActive
                  ? "border-amber-400 bg-[#15181e] text-white"
                  : "border-transparent text-white/45 hover:bg-white/[0.03] hover:text-white/75"
              )}
            >
              <Icon
                className={cn(
                  "h-4 w-4 shrink-0 transition-colors duration-200",
                  isActive ? "text-amber-400" : "text-white/30"
                )}
              />
              <span className="font-normal tracking-wide">{t(`nav.${item.label}`)}</span>
              {pendingHref === item.href && <Loader2 className="ml-auto h-3.5 w-3.5 animate-spin text-amber-400" />}
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
            {t("footer")}
          </p>
          <p className="text-xs text-amber-400/60">v0.1.0</p>
        </div>
      </div>
    </aside>
  );
}
