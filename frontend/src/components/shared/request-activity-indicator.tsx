"use client";

import { Circle } from "lucide-react";
import { useEffect, useRef, useState } from "react";
import { useTranslations } from "next-intl";
import { PLATFORM_REQUEST_ACTIVITY_EVENT } from "@/lib/api/platform";

export function RequestActivityIndicator() {
  const t = useTranslations("common");
  const [activeRequests, setActiveRequests] = useState(0);
  const [visible, setVisible] = useState(false);
  const showTimer = useRef<number | null>(null);
  const hideTimer = useRef<number | null>(null);

  useEffect(() => {
    const onRequestActivity = (event: Event) => {
      setActiveRequests((event as CustomEvent<{ active: number }>).detail.active);
    };
    window.addEventListener(PLATFORM_REQUEST_ACTIVITY_EVENT, onRequestActivity);
    return () => {
      window.removeEventListener(PLATFORM_REQUEST_ACTIVITY_EVENT, onRequestActivity);
      if (showTimer.current) window.clearTimeout(showTimer.current);
      if (hideTimer.current) window.clearTimeout(hideTimer.current);
    };
  }, []);

  useEffect(() => {
    if (activeRequests > 0) {
      if (hideTimer.current) {
        window.clearTimeout(hideTimer.current);
        hideTimer.current = null;
      }
      if (!visible && !showTimer.current) {
        showTimer.current = window.setTimeout(() => {
          showTimer.current = null;
          setVisible(true);
        }, 200);
      }
      return;
    }
    if (showTimer.current) {
      window.clearTimeout(showTimer.current);
      showTimer.current = null;
    }
    if (visible && !hideTimer.current) {
      hideTimer.current = window.setTimeout(() => {
        hideTimer.current = null;
        setVisible(false);
      }, 900);
    }
  }, [activeRequests, visible]);

  if (!visible) return null;

  return (
    <div
      className="pointer-events-none fixed right-4 top-4 z-[110] flex items-center gap-2 rounded-sm border border-amber-300/25 bg-instrument-panel px-3 py-2 font-mono text-[11px] text-amber-100/80"
      role="status"
      aria-live="polite"
    >
      <Circle className="h-3.5 w-3.5 fill-amber-300/70 text-amber-300/70" />
      {t("loading")}
    </div>
  );
}
