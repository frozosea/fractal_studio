"use client";

import * as React from "react";
import { Menu } from "lucide-react";
import { useTranslations } from "next-intl";
import { Link, usePathname } from "@/i18n/navigation";
import { Button } from "@/components/ui/button";
import { LocaleSwitcher } from "@/components/layout/locale-switcher";
import { ThemeToggle } from "@/components/layout/theme-toggle";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import { useAuth } from "@/providers/auth-provider";
import { cn } from "@/lib/utils/cn";

const links = [
  { href: "/", key: "home" },
  { href: "/tutorial", key: "tutorial" },
  { href: "/help", key: "help" },
] as const;

export function PublicHeader() {
  const t = useTranslations("landing");
  const pathname = usePathname();
  const { isAuthenticated } = useAuth();

  const isCurrent = (href: string) => (href === "/" ? pathname === "/" : pathname.startsWith(href));

  return (
    <header
      className="sticky top-0 z-40 border-b border-instrument-rule bg-instrument"
      style={{ paddingTop: "env(safe-area-inset-top)" }}
    >
      <div className="mx-auto flex h-14 max-w-6xl items-center gap-3 px-4 sm:px-8">
        <Link href="/" className="flex shrink-0 items-center gap-2.5" aria-label={t("nav.home")}>
          <span className="flex h-7 w-7 items-center justify-center bg-brand text-xs font-bold text-brand-ink">
            F
          </span>
          <span className="text-sm font-semibold tracking-wide text-ink">Fractal Studio</span>
        </Link>

        {/* Desktop links */}
        <nav className="ml-6 hidden items-center gap-1 md:flex" aria-label={t("nav.label")}>
          {links.map((link) => (
            <Link
              key={link.href}
              href={link.href}
              aria-current={isCurrent(link.href) ? "page" : undefined}
              className={cn(
                "px-3 py-1.5 text-sm transition-colors duration-150",
                isCurrent(link.href) ? "text-amber-300" : "text-ink/50 hover:text-ink/80",
              )}
            >
              {t(`nav.${link.key}`)}
            </Link>
          ))}
        </nav>

        <div className="ml-auto flex items-center gap-2">
          <ThemeToggle />
          <LocaleSwitcher />
          {isAuthenticated ? (
            <Button asChild size="sm" className="coarse:h-10">
              <Link href="/studio">{t("nav.workbench")}</Link>
            </Button>
          ) : (
            <>
              <Button asChild size="sm" variant="ghost" className="hidden coarse:h-10 sm:inline-flex">
                <Link href="/login">{t("nav.signIn")}</Link>
              </Button>
              <Button asChild size="sm" className="coarse:h-10">
                <Link href="/register">{t("nav.signUp")}</Link>
              </Button>
            </>
          )}

          {/* Mobile links collapse into a menu rather than a second drawer. */}
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <Button variant="ghost" size="icon" className="coarse:h-10 coarse:w-10 md:hidden" aria-label={t("nav.label")}>
                <Menu className="h-4 w-4" />
              </Button>
            </DropdownMenuTrigger>
            <DropdownMenuContent align="end" className="min-w-[10rem]">
              {links.map((link) => (
                <DropdownMenuItem key={link.href} asChild>
                  <Link href={link.href} className={cn(isCurrent(link.href) && "text-amber-300")}>
                    {t(`nav.${link.key}`)}
                  </Link>
                </DropdownMenuItem>
              ))}
              {!isAuthenticated && (
                <DropdownMenuItem asChild>
                  <Link href="/login">{t("nav.signIn")}</Link>
                </DropdownMenuItem>
              )}
            </DropdownMenuContent>
          </DropdownMenu>
        </div>
      </div>
    </header>
  );
}
