"use client";

import * as React from "react";
import { useTranslations } from "next-intl";
import { Monitor, Moon, Sun } from "lucide-react";
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
        <Button variant="ghost" size="sm" className="gap-1.5 px-2 text-xs" aria-label={t("label")}>
          {/* The stored preference is unknown until the client reads it, so the
              slot is held open rather than filled with a guess that would swap
              under the user a frame later. */}
          {ready ? <Icon className="h-3.5 w-3.5" /> : <span className="h-3.5 w-3.5" aria-hidden />}
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end" className="min-w-[8rem]">
        {options.map((option) => {
          const OptionIcon = option.icon;
          return (
            <DropdownMenuItem
              key={option.value}
              onClick={() => setTheme(option.value)}
              className={cn("gap-2", ready && theme === option.value && "text-amber-300")}
            >
              <OptionIcon className="h-3.5 w-3.5" />
              {t(option.value)}
            </DropdownMenuItem>
          );
        })}
      </DropdownMenuContent>
    </DropdownMenu>
  );
}
