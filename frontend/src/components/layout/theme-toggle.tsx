"use client";

import * as React from "react";
import { useTranslations } from "next-intl";
import { Check, Monitor, Moon, Sun } from "lucide-react";
import { Button } from "@/components/ui/button";
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import { useTheme } from "@/providers/theme-provider";
import type { Theme } from "@/lib/theme";
import { cn } from "@/lib/utils/cn";

const options = [
  { value: "light", icon: Sun },
  { value: "dark", icon: Moon },
  { value: "system", icon: Monitor },
] as const satisfies ReadonlyArray<{ value: Theme; icon: typeof Sun }>;

export function ThemeToggle() {
  const t = useTranslations("common.theme");
  const { theme, resolvedTheme, setTheme, ready } = useTheme();

  const Icon = resolvedTheme === "dark" ? Moon : Sun;

  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button
          variant="outline"
          size="icon"
          className="h-8 w-8 border-instrument-rule bg-instrument-panel text-ink/60 hover:border-brand/45 hover:text-brand coarse:h-10 coarse:w-10"
          aria-label={t("label")}
          title={`${t("label")}: ${t(theme)}`}
        >
          {/* The stored preference is unknown until the client reads it, so the
              slot is held open rather than filled with a guess that would swap
              under the user a frame later. */}
          {ready ? <Icon className="h-3.5 w-3.5" /> : <span className="h-3.5 w-3.5" aria-hidden />}
        </Button>
      </DropdownMenuTrigger>
      {/* Geometry and the current-item colour match LocaleSwitcher deliberately
          — the two menus sit side by side in every shell. */}
      <DropdownMenuContent align="end" className="min-w-[132px]">
        {options.map((option) => {
          const OptionIcon = option.icon;
          return (
            <DropdownMenuItem
              key={option.value}
              onSelect={() => setTheme(option.value)}
              aria-current={theme === option.value ? "true" : undefined}
              className={cn(
                "gap-2 border-l border-transparent",
                theme === option.value && "border-brand bg-brand/[0.08] text-brand",
              )}
            >
              <OptionIcon className="h-3.5 w-3.5" />
              <span className="flex-1">{t(option.value)}</span>
              {theme === option.value && <Check className="h-3 w-3" aria-hidden />}
            </DropdownMenuItem>
          );
        })}
      </DropdownMenuContent>
    </DropdownMenu>
  );
}
