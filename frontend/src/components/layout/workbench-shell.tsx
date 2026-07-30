"use client";
import * as React from "react";
import { Sidebar } from "@/components/layout/sidebar";
import { Navbar } from "@/components/layout/navbar";
import { StatusRail } from "@/components/layout/status-rail";
import { cn } from "@/lib/utils/cn";

interface WorkbenchShellProps {
  children: React.ReactNode;
  title?: string;
}

export function WorkbenchShell({ children, title }: WorkbenchShellProps) {
  const [sidebarCollapsed, setSidebarCollapsed] = React.useState(false);
  const [mobileSidebarOpen, setMobileSidebarOpen] = React.useState(false);

  React.useEffect(() => {
    setSidebarCollapsed(window.localStorage.getItem("fs-sidebar-collapsed") === "true");
  }, []);

  const toggleSidebar = () => {
    if (window.matchMedia("(max-width: 767px)").matches) {
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
    <div className="flex h-screen overflow-hidden bg-deep-void">
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
        <main className="flex-1 overflow-auto p-3 sm:p-4 lg:p-6">
          {children}
        </main>
      </div>
    </div>
  );
}
