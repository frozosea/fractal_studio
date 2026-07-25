"use client";
import * as React from "react";
import { cn } from "@/lib/utils/cn";
import { usePathname, useRouter } from "@/i18n/navigation";
import { useLocale } from "next-intl";
import { Globe } from "lucide-react";
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
  const pathname = usePathname();
  const router = useRouter();

  const currentLocale = locales.find((l) => l.code === locale) ?? locales[0];

  const handleSwitch = (newLocale: string) => {
    router.replace(pathname, { locale: newLocale });
  };

  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button variant="ghost" size="sm" className="gap-1.5 px-2 text-xs">
          <Globe className="h-3.5 w-3.5" />
          {currentLocale.label}
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end" className="min-w-[100px]">
        {locales.map((l) => (
          <DropdownMenuItem
            key={l.code}
            onClick={() => handleSwitch(l.code)}
            className={cn(locale === l.code && "text-fractal-400")}
          >
            {l.name}
          </DropdownMenuItem>
        ))}
      </DropdownMenuContent>
    </DropdownMenu>
  );
}
