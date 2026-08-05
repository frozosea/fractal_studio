"use client";
import { cn } from "@/lib/utils/cn";
import { useSearchParams } from "next/navigation";
import { useLocale } from "next-intl";
import { Check, Globe } from "lucide-react";
import { Button } from "@/components/ui/button";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";

const locales = [
  { code: "en", label: "EN", name: "English" },
  { code: "zh", label: "ZH", name: "中文" },
] as const;

export function LocaleSwitcher() {
  const locale = useLocale();
  const searchParams = useSearchParams();

  const currentLocale = locales.find((l) => l.code === locale) ?? locales[0];

  const handleSwitch = (newLocale: (typeof locales)[number]["code"]) => {
    if (newLocale === locale) return;
    // Keep next-intl's locale detection from redirecting the unprefixed
    // default-locale URL back to the previously selected locale.
    document.cookie = `NEXT_LOCALE=${newLocale}; Path=/; Max-Age=31536000; SameSite=Lax`;
    const query = searchParams.toString();
    const hash = typeof window === "undefined" ? "" : window.location.hash;
    const strippedPath = window.location.pathname.replace(/^\/(?:zh|en)(?=\/|$)/, "") || "/";
    const localizedPath = newLocale === "zh"
      ? strippedPath
      : `/en${strippedPath === "/" ? "" : strippedPath}`;
    window.location.assign(`${localizedPath}${query ? `?${query}` : ""}${hash}`);
  };

  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button
          variant="outline"
          size="sm"
          className="h-8 gap-1.5 border-instrument-rule bg-instrument-panel px-2 font-mono text-[11px] text-ink/60 hover:border-brand/45 hover:text-brand coarse:h-10"
          aria-label={`Language: ${currentLocale.name}`}
          title={`Language: ${currentLocale.name}`}
        >
          <Globe className="h-3.5 w-3.5" />
          {currentLocale.label}
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end" className="min-w-[132px]">
        {locales.map((l) => (
          <DropdownMenuItem
            key={l.code}
            onSelect={() => handleSwitch(l.code)}
            aria-current={locale === l.code ? "true" : undefined}
            className={cn(
              "gap-2 border-l border-transparent",
              locale === l.code && "border-brand bg-brand/[0.08] text-brand",
            )}
          >
            <span className="flex-1">{l.name}</span>
            {locale === l.code && <Check className="h-3 w-3" aria-hidden />}
          </DropdownMenuItem>
        ))}
      </DropdownMenuContent>
    </DropdownMenu>
  );
}
