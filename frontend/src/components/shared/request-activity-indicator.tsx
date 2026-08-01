"use client";

import { Loader2 } from "lucide-react";
import { useEffect, useState } from "react";
import { useTranslations } from "next-intl";
import { PLATFORM_REQUEST_ACTIVITY_EVENT } from "@/lib/api/platform";

export function RequestActivityIndicator() {
  const t = useTranslations("common");
  const [activeRequests, setActiveRequests] = useState(0);

  useEffect(() => {
    const onRequestActivity = (event: Event) => {
      setActiveRequests((event as CustomEvent<{ active: number }>).detail.active);
    };
    window.addEventListener(PLATFORM_REQUEST_ACTIVITY_EVENT, onRequestActivity);
    return () => window.removeEventListener(PLATFORM_REQUEST_ACTIVITY_EVENT, onRequestActivity);
  }, []);

  if (activeRequests === 0) return null;

  return (
    <div
      className="pointer-events-none fixed right-4 top-4 z-[110] flex items-center gap-2 rounded-sm border border-amber-300/25 bg-instrument-panel/95 px-3 py-2 font-mono text-[11px] text-amber-100/80 shadow-xl"
      role="status"
      aria-live="polite"
    >
      <Loader2 className="h-3.5 w-3.5 animate-spin" />
      {t("loading")}
    </div>
  );
}
