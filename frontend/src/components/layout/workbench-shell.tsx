"use client";
import * as React from "react";
import { Sidebar } from "@/components/layout/sidebar";
import { Navbar } from "@/components/layout/navbar";
import { StatusRail } from "@/components/layout/status-rail";
import { useIsMobile } from "@/lib/hooks/use-media-query";
import { cn } from "@/lib/utils/cn";

interface WorkbenchShellProps {
  children: React.ReactNode;
  title?: string;
}

export function WorkbenchShell({ children, title }: WorkbenchShellProps) {
  const [sidebarCollapsed, setSidebarCollapsed] = React.useState(false);
  const [mobileSidebarOpen, setMobileSidebarOpen] = React.useState(false);
  // Same threshold as the `md:` classes below, read from one place rather than
  // duplicated as a magic number in a click handler.
  const isMobile = useIsMobile();

  React.useEffect(() => {
    setSidebarCollapsed(window.localStorage.getItem("fs-sidebar-collapsed") === "true");
  }, []);

  const toggleSidebar = () => {
    if (isMobile) {
      setMobileSidebarOpen((open) => !open);
      return;
    }
    setSidebarCollapsed((collapsed) => {
      const next = !collapsed;
      window.localStorage.setItem("fs-sidebar-collapsed", String(next));
      return next;
    });
  };

  return (
    // `viewport-fit=cover` lets the page run under the notch and the home
    // indicator, so the shell has to pad itself back out. The sidebar is fixed
    // to the viewport and therefore needs its own insets.
    <div
      className="flex h-[100dvh] overflow-hidden bg-deep-void"
      style={{ paddingTop: "env(safe-area-inset-top)" }}
    >
      <Sidebar
        collapsed={sidebarCollapsed}
        mobileOpen={mobileSidebarOpen}
        onNavigate={() => setMobileSidebarOpen(false)}
      />
      {mobileSidebarOpen && (
        <button
          type="button"
          className="fixed inset-0 z-30 bg-black/60 backdrop-blur-sm md:hidden"
          aria-label="Close navigation"
          onClick={() => setMobileSidebarOpen(false)}
        />
      )}
      <div className={cn(
        "flex min-w-0 flex-1 flex-col overflow-hidden transition-[padding] duration-200",
        sidebarCollapsed ? "md:pl-16" : "md:pl-60",
      )}>
        <Navbar title={title} onToggleSidebar={toggleSidebar} />
        <StatusRail />
        <main
          className="flex-1 overflow-auto p-3 sm:p-4 lg:p-6"
          style={{ paddingBottom: "max(env(safe-area-inset-bottom), 0.75rem)" }}
        >
          {children}
        </main>
      </div>
    </div>
  );
}
