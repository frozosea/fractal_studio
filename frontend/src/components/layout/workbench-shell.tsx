"use client";
import * as React from "react";
import { Sidebar } from "@/components/layout/sidebar";
import { Navbar } from "@/components/layout/navbar";
import { StatusRail } from "@/components/layout/status-rail";

interface WorkbenchShellProps {
  children: React.ReactNode;
  title?: string;
}

export function WorkbenchShell({ children, title }: WorkbenchShellProps) {
  return (
    <div className="flex h-screen overflow-hidden bg-deep-void">
      <Sidebar />
      <div className="flex flex-1 flex-col overflow-hidden pl-60">
        <Navbar title={title} />
        <StatusRail />
        <main className="flex-1 overflow-auto p-6">
          {children}
        </main>
      </div>
    </div>
  );
}
